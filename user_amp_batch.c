#include <string.h>

#include "user_amp_runtime.h"

void batch_reset(batch_state_t *b)
{
    uint32_t seq = b->seq;
    amp_batch_hdr_t hdr;

    memset(b, 0, sizeof(*b));
    b->seq = seq;

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, AMP_BATCH_MAGIC, 4);
    hdr.version = AMP_BATCH_VERSION;
    hdr.flags = 0;
    hdr.count_be = htons(0);
    hdr.seq_be = htonl(b->seq);

    memcpy(b->buf, &hdr, sizeof(hdr));
    b->len = sizeof(hdr);
    b->count = 0;
    b->dst_ip = 0;
}

int batch_append(batch_state_t *b, const uint8_t *pkt, size_t pkt_len, uint32_t dst_ip)
{
    if (pkt_len > 0xFFFF)
        return -1;
    if (b->len + 2 + pkt_len > AMP_BATCH_MAX_BYTES)
        return -2;

    if (b->count == 0)
        b->dst_ip = dst_ip;

    write_be16_unaligned(b->buf + b->len, (uint16_t)pkt_len);
    b->len += 2;
    memcpy(b->buf + b->len, pkt, pkt_len);
    b->len += pkt_len;

    b->count++;
    write_be16_unaligned(b->buf + offsetof(amp_batch_hdr_t, count_be), b->count);
    write_be32_unaligned(b->buf + offsetof(amp_batch_hdr_t, seq_be), b->seq);
    return 0;
}

int amp_flush_batch_if_any(batch_state_t *b)
{
    if (b->count == 0)
        return 0;

    if (b->count == 1) {
        size_t off = sizeof(amp_batch_hdr_t);
        uint16_t l;

        if (b->len < off + 2) {
            batch_reset(b);
            return -1;
        }

        l = read_be16_unaligned(b->buf + off);
        if (off + 2 + l > b->len) {
            batch_reset(b);
            return -1;
        }

        (void)amp_send_msg(b->dst_ip, b->buf + off + 2, l);
        b->seq++;
        batch_reset(b);
        return 0;
    }

    (void)amp_send_msg(b->dst_ip, b->buf, b->len);
    b->seq++;
    batch_reset(b);
    return 0;
}
