#ifndef USER_AMP_PROTO_H
#define USER_AMP_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>

#include "user_amp_config.h"

struct amp_net_msg {
    uint32_t ip;
    uint32_t node_id;
    uint32_t len;
    uint8_t data_type;
    uint8_t data[MAX_PAYLOAD_SIZE];
};

#pragma pack(push, 1)
typedef struct {
    uint16_t frame_header;
    uint8_t frame_type;
    uint8_t dst_addr;
    uint32_t frame_seq;
    uint32_t test_freq;
    uint32_t test_enable;
    uint32_t fixed_freq;
    uint32_t net_test;
    uint32_t loopback;
    uint32_t iq_swap;
    uint32_t attenuation;
    uint16_t frame_tail;
} control_frame_t;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    uint8_t magic[4];
    uint8_t version;
    uint8_t flags;
    uint16_t count_be;
    uint32_t seq_be;
} amp_batch_hdr_t;
#pragma pack(pop)

typedef struct {
    uint8_t buf[AMP_BATCH_MAX_BYTES];
    size_t len;
    uint16_t count;
    uint32_t seq;
    uint32_t dst_ip;
} batch_state_t;

static inline uint16_t read_be16_unaligned(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return ntohs(v);
}

static inline void write_be16_unaligned(uint8_t *p, uint16_t v)
{
    uint16_t t = htons(v);
    memcpy(p, &t, sizeof(t));
}

static inline void write_be32_unaligned(uint8_t *p, uint32_t v)
{
    uint32_t t = htonl(v);
    memcpy(p, &t, sizeof(t));
}

#endif
