#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

#include "user_amp_runtime.h"

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

int amp_send_msg(uint32_t dst_ip, const uint8_t *payload, size_t len)
{
    struct amp_net_msg msg;
    ssize_t w;

    if (len > MAX_PAYLOAD_SIZE) {
        fprintf(stderr, "[ERROR] payload too large: %zu > %d\n", len, MAX_PAYLOAD_SIZE);
        return -1;
    }

    memset(&msg, 0, sizeof(msg));
    msg.data_type = 0;
    msg.ip = dst_ip;
    msg.node_id = 255;
    msg.len = (uint32_t)len;
    memcpy(msg.data, payload, len);

    w = write(amp_fd, &msg, offsetof(struct amp_net_msg, data) + msg.len);
    if (w < 0) {
        perror("write(amp)");
        return -1;
    }
    return 0;
}

static int is_icmp_ping_fastpath(const uint8_t *pkt, size_t len)
{
    const struct iphdr *ip;
    size_t ihl;
    const struct icmphdr *ic;

    if (len < sizeof(struct iphdr))
        return 0;

    ip = (const struct iphdr *)pkt;
    if (ip->version != 4)
        return 0;

    ihl = (size_t)ip->ihl * 4;
    if (ihl < sizeof(struct iphdr) || len < ihl + sizeof(struct icmphdr))
        return 0;
    if (ip->protocol != IPPROTO_ICMP)
        return 0;

    ic = (const struct icmphdr *)(pkt + ihl);
    return ic->type == ICMP_ECHO || ic->type == ICMP_ECHOREPLY;
}

void *tun_to_amp_thread(void *arg)
{
    uint8_t buf[MAX_PAYLOAD_SIZE];
    batch_state_t batch;

    (void)arg;
    memset(&batch, 0, sizeof(batch));
    batch_reset(&batch);
    (void)set_nonblock(tun_fd);

    while (1) {
        int timeout_ms = (batch.count == 0) ? -1 : AMP_BATCH_TIMEOUT_MS;
        struct pollfd pfd = { .fd = tun_fd, .events = POLLIN };
        int prc = poll(&pfd, 1, timeout_ms);

        if (prc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll(tun)");
            break;
        }

        if (prc == 0) {
            (void)amp_flush_batch_if_any(&batch);
            continue;
        }

        while (1) {
            ssize_t n = read(tun_fd, buf, sizeof(buf));
            struct iphdr *ip;
            size_t pkt_len;
            uint32_t dst_ip;
            int is_ping;
            size_t agg_overhead;
            int rc;

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                    continue;
                perror("read(tun)");
                goto out;
            }
            if (n == 0)
                break;
            if ((size_t)n < sizeof(struct iphdr))
                continue;

            ip = (struct iphdr *)buf;
            if (ip->version != 4)
                continue;
            if (ip->daddr != remote_pc_addr)
                continue;

            pkt_len = (size_t)n;
            dst_ip = ip->daddr;

#if AMP_ICMP_FASTPATH
            is_ping = is_icmp_ping_fastpath(buf, pkt_len);
#else
            is_ping = 0;
#endif

            if (pkt_len > AMP_BATCH_MAX_BYTES) {
                (void)amp_flush_batch_if_any(&batch);
                (void)amp_send_msg(dst_ip, buf, pkt_len);
                continue;
            }

            agg_overhead = sizeof(amp_batch_hdr_t) + 2;
            if (pkt_len + agg_overhead > AMP_BATCH_MAX_BYTES) {
                (void)amp_flush_batch_if_any(&batch);
                (void)amp_send_msg(dst_ip, buf, pkt_len);
                continue;
            }

            if (is_ping) {
                int appended = 0;

                if (batch.count == 0 || batch.dst_ip == dst_ip) {
                    if (batch_append(&batch, buf, pkt_len, dst_ip) == 0)
                        appended = 1;
                }

                if (!appended) {
                    (void)amp_flush_batch_if_any(&batch);
                    (void)amp_send_msg(dst_ip, buf, pkt_len);
                } else {
                    (void)amp_flush_batch_if_any(&batch);
                }
                continue;
            }

            if (batch.count > 0 && batch.dst_ip != dst_ip)
                (void)amp_flush_batch_if_any(&batch);

            rc = batch_append(&batch, buf, pkt_len, dst_ip);
            if (rc == -2) {
                (void)amp_flush_batch_if_any(&batch);
                rc = batch_append(&batch, buf, pkt_len, dst_ip);
                if (rc != 0)
                    (void)amp_send_msg(dst_ip, buf, pkt_len);
            } else if (rc != 0) {
                (void)amp_flush_batch_if_any(&batch);
                (void)amp_send_msg(dst_ip, buf, pkt_len);
            }
        }
    }

out:
    (void)amp_flush_batch_if_any(&batch);
    return NULL;
}

void *amp_to_tun_thread(void *arg)
{
    struct amp_net_msg msg;

    (void)arg;

    while (1) {
        ssize_t n = read(amp_fd, &msg, sizeof(msg));

        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read(amp)");
            break;
        }
        if ((size_t)n < offsetof(struct amp_net_msg, data))
            continue;
        if (msg.len == 0 || msg.len > MAX_PAYLOAD_SIZE)
            continue;

        if (msg.len >= sizeof(amp_batch_hdr_t) && memcmp(msg.data, AMP_BATCH_MAGIC, 4) == 0) {
            const amp_batch_hdr_t *hdr = (const amp_batch_hdr_t *)msg.data;
            uint16_t count;
            size_t off;
            uint16_t i;

            if (hdr->version != AMP_BATCH_VERSION)
                continue;

            count = ntohs(hdr->count_be);
            off = sizeof(amp_batch_hdr_t);
            for (i = 0; i < count; i++) {
                uint16_t l;

                if (off + 2 > msg.len)
                    break;
                l = read_be16_unaligned(msg.data + off);
                off += 2;
                if (off + l > msg.len)
                    break;
                (void)tun_write_packet(tun_fd, msg.data + off, l);
                off += l;
            }
        } else {
            (void)tun_write_packet(tun_fd, msg.data, msg.len);
        }
    }

    return NULL;
}
