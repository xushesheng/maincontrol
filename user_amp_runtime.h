#ifndef USER_AMP_RUNTIME_H
#define USER_AMP_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

#include "user_amp_proto.h"

extern int amp_fd;
extern int tun_fd;
extern uint32_t remote_pc_addr;

int tun_alloc(const char *devname);
int tun_write_packet(int fd, const uint8_t *pkt, size_t len);
void setup_gateway_rules(void);

void batch_reset(batch_state_t *b);
int batch_append(batch_state_t *b, const uint8_t *pkt, size_t pkt_len, uint32_t dst_ip);
int amp_flush_batch_if_any(batch_state_t *b);

int amp_send_msg(uint32_t dst_ip, const uint8_t *payload, size_t len);

void *tun_to_amp_thread(void *arg);
void *amp_to_tun_thread(void *arg);
void *pcap_control_thread(void *arg);

#endif
