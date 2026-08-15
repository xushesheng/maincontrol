/*************************************/
/*       跨文件全局变量和函数声明      */
/************************************/
#ifndef USER_AMP_RUNTIME_H
#define USER_AMP_RUNTIME_H

#include <pthread.h>            /* POSIX 线程（pthread_mutex_t / pthread_cond_t） */
#include <stdint.h>             /* 固定宽度整数类型 */
#include <stddef.h>             /* size_t */

#include "user_struct.h"        /* amp_net_msg / amp_ctrl_msg / batch_state_t */

/* ========= 全局文件描述符 ========= */
extern int amp_fd;              /* /dev/amp_ipi 文件描述符（业务数据） */
extern int ctrl_fd;             /* /dev/amp_ctrl 文件描述符（控制数据） */
extern int tun_fd;              /* TUN 虚拟网卡 rf0 文件描述符 */
extern volatile int g_running;  /* 全局运行标志：0 表示退出，所有线程检查此标志 */

/* ========= 发送队列相关类型 ========= */
typedef struct {
    struct amp_net_msg msg;     /* 待发送的业务消息 */
    size_t msg_bytes;           /* 消息实际字节数 */
} amp_tx_slot_t;

/* 业务数据统一发送队列运行时结构 */
typedef struct {
    amp_tx_slot_t data_q[AMP_TX_QUEUE_DEPTH];   /* 环形发送队列，容量 64 */
    unsigned int data_head;                     /* 队列头索引（消费侧） */
    unsigned int data_tail;                     /* 队列尾索引（生产侧） */
    unsigned int data_count;                    /* 队列中待发送消息数 */
    unsigned long data_waits;                   /* 队列满等待次数（调试/监控） */
    unsigned long tx_write_fail;                /* write(amp) 失败计数 */
    unsigned long tx_short_write;               /* write(amp) 短写计数 */
    pthread_mutex_t lock;                       /* 保护队列的互斥锁 */
    pthread_cond_t not_empty;                   /* 条件变量：队列非空（唤醒发送线程） */
    pthread_cond_t data_not_full;               /* 条件变量：队列非满（唤醒生产线程） */
} amp_tx_runtime_t;

extern amp_tx_runtime_t amp_tx_runtime;         /* 全局发送队列运行时实例 */

/* ========= 网关/网络相关函数 ========= */
/* 读取指定网络接口的 IPv4 地址（定义见 user_gateway.c，user_control.c 复用） */
int get_iface_ipv4(const char *ifname, struct in_addr *addr);

int tun_alloc(const char *devname);             /* 创建 TUN 虚拟网卡设备 */
int tun_write_packet(int fd, const uint8_t *pkt, size_t len);  /* 向 TUN 设备写数据包 */
void setup_gateway_rules(void);                 /* 配置路由、proxy ARP、sysctl */
int is_peer_pc_addr(uint32_t ip_be);            /* 判断一个 IP 是否属于节点表中的对端 PC */

/* ========= 控制面相关函数 ========= */
int control_socket_init(void);                  /* 创建并绑定 UDP 3409 socket */
void control_socket_close(void);                /* 关闭控制 socket */
int clock_send_on_boot(void);                   /* 开机一次性下发 0x19 时钟帧 */

/* ========= 批次聚合相关函数 ========= */
void batch_reset(batch_state_t *b);             /* 初始化/重置批次帧 */
int batch_append(batch_state_t *b, const uint8_t *pkt, size_t pkt_len, uint32_t dst_ip);  /* 添加子包到批次 */
int amp_flush_batch_if_any(batch_state_t *b);   /* 发送当前批次（如有） */

int amp_send_msg(uint32_t dst_ip, const uint8_t *payload, size_t len);  /* 构造并发送一条业务消息 */

/* ========= 5 个工作线程入口 ========= */
void *amp_tx_thread(void *arg);                 /* 统一业务发送线程：从队列取数据写 /dev/amp_ipi */
void *tun_to_amp_thread(void *arg);             /* TUN -> AMP 线程：从 rf0 读 IP 包，聚合发送 */
void *amp_to_tun_thread(void *arg);             /* AMP -> TUN 线程：从 /dev/amp_ipi 读，写回 rf0 */
void *control_rx_to_amp_thread(void *arg);      /* 控制上行线程：UDP 3409 -> /dev/amp_ctrl */
void *control_amp_to_udp_thread(void *arg);     /* 控制下行线程：/dev/amp_ctrl -> UDP 3419 */

#endif
