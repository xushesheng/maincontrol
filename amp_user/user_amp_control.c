#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <pcap.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include "user_amp_runtime.h"

static int process_control_frame(uint8_t *udp_payload, int payload_len)
{
    control_frame_t ctrl_frame;
    struct amp_net_msg msg;
    ssize_t w;

    if (payload_len != (int)sizeof(control_frame_t))
        return -1;

    memcpy(&ctrl_frame, udp_payload, sizeof(ctrl_frame));
    if (ntohs(ctrl_frame.frame_header) != 0xF00F)
        return -1;
    if (ntohs(ctrl_frame.frame_tail) != 0xE00E)
        return -1;

    memset(&msg, 0, sizeof(msg));
    msg.data_type = 1;
    msg.node_id = ctrl_frame.dst_addr;
    msg.ip = 0;
    msg.len = sizeof(ctrl_frame);
    memcpy(msg.data, &ctrl_frame, sizeof(ctrl_frame));

    w = write(amp_fd, &msg, offsetof(struct amp_net_msg, data) + msg.len);
    if (w < 0)
        perror("write(amp ctrl)");
    return 0;
}

static void got_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet)
{
    const struct iphdr *ip;
    size_t ihl;
    const struct udphdr *udp;
    uint16_t dst_port;
    const uint8_t *payload;
    int payload_len;

    (void)args;

    if (header->caplen < 14 + sizeof(struct iphdr))
        return;

    ip = (const struct iphdr *)(packet + 14);
    if (ip->version != 4)
        return;

    ihl = (size_t)ip->ihl * 4;
    if (header->caplen < 14 + ihl + sizeof(struct udphdr))
        return;
    if (ip->protocol != IPPROTO_UDP)
        return;

    udp = (const struct udphdr *)((const uint8_t *)ip + ihl);
    dst_port = ntohs(udp->dest);
    if (dst_port != CTRL_UDP_PORT)
        return;

    payload = (const uint8_t *)(udp + 1);
    payload_len = (int)ntohs(udp->len) - (int)sizeof(struct udphdr);
    if (payload_len <= 0)
        return;

    (void)process_control_frame((uint8_t *)payload, payload_len);
}

void *pcap_control_thread(void *arg)
{
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t *handle;
    struct bpf_program fp;
    char filter_exp[128];

    (void)arg;

    handle = pcap_open_live(CAP_IFACE, 4096, 0, 10, errbuf);
    if (!handle) {
        fprintf(stderr, "[ERROR] pcap_open_live(%s) failed: %s\n", CAP_IFACE, errbuf);
        return NULL;
    }

    snprintf(filter_exp, sizeof(filter_exp), "udp and dst port %d", CTRL_UDP_PORT);
    if (pcap_compile(handle, &fp, filter_exp, 1, PCAP_NETMASK_UNKNOWN) < 0 ||
        pcap_setfilter(handle, &fp) < 0) {
        fprintf(stderr, "[ERROR] pcap filter failed: %s\n", pcap_geterr(handle));
        pcap_close(handle);
        return NULL;
    }

    while (1) {
        int rc = pcap_dispatch(handle, 1, got_packet, NULL);
        if (rc < 0) {
            fprintf(stderr, "[ERROR] pcap_dispatch: %s\n", pcap_geterr(handle));
            break;
        }
    }

    pcap_close(handle);
    return NULL;
}
