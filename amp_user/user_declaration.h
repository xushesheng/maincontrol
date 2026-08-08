/*************************************/
/*       跨文件全局变量和函数声明      */
/************************************/
#ifndef USER_AMP_RUNTIME_H
#define USER_AMP_RUNTIME_H

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

#include "user_struct.h"

extern int amp_fd;
extern int ctrl_fd;
extern int tun_fd;
extern volatile int g_running;

typedef struct {
    struct amp_net_msg msg;
    size_t msg_bytes;
} amp_tx_slot_t;

typedef struct {
    amp_tx_slot_t data_q[AMP_TX_QUEUE_DEPTH];
    unsigned int data_head;
    unsigned int data_tail;
    unsigned int data_count;
    unsigned long data_waits;
    unsigned long tx_write_fail;
    unsigned long tx_short_write;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t data_not_full;
} amp_tx_runtime_t;

extern amp_tx_runtime_t amp_tx_runtime;

int tun_alloc(const char *devname);
int tun_write_packet(int fd, const uint8_t *pkt, size_t len);
void setup_gateway_rules(void);
int is_peer_pc_addr(uint32_t ip_be);

int control_socket_init(void);
void control_socket_close(void);

void batch_reset(batch_state_t *b);
int batch_append(batch_state_t *b, const uint8_t *pkt, size_t pkt_len, uint32_t dst_ip);
int amp_flush_batch_if_any(batch_state_t *b);

int amp_send_msg(uint32_t dst_ip, const uint8_t *payload, size_t len);

void *amp_tx_thread(void *arg);
void *tun_to_amp_thread(void *arg);
void *amp_to_tun_thread(void *arg);
void *control_rx_to_amp_thread(void *arg);
void *control_amp_to_udp_thread(void *arg);

#endif
