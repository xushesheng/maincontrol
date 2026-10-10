/*************************************/
/*       跨文件全局变量和函数声明      */
/************************************/
#ifndef USER_AMP_RUNTIME_H
#define USER_AMP_RUNTIME_H

#include <pthread.h>            /* POSIX 线程（pthread_mutex_t / pthread_cond_t） */
#include <stdint.h>             /* 固定宽度整数类型 */
#include <stddef.h>             /* size_t */

#include "user_struct.h"        /* amp_net_msg / amp_ctrl_msg */

/* ========= 全局文件描述符 ========= */
extern int amp_fd;              /* /dev/amp_ipi 文件描述符（业务数据） */
extern int ctrl_fd;             /* /dev/amp_ctrl 文件描述符（控制数据） */
extern int tun_fd;              /* TUN 虚拟网卡 rf0 文件描述符 */
extern volatile int g_running;  /* 全局运行标志：0 表示退出，所有线程检查此标志 */

/* ========= 业务端口运行时配置 ========= */
/* 当前生效的业务 UDP 端口区间，打包为单个 32 位值：(下限 << 16) | 上限。
 * 默认取 BUSINESS_PORT_DEFAULT_MIN/MAX（3380/3480）。
 * 写方：control_rx_to_amp_thread 收到网管 0x22 配置帧（协议表 5.26）后改写；
 * 读方：tun_to_amp_thread 与广播桥接入口共用的端口过滤判断。
 * 打包成 32 位的目的：对齐的 32 位读写在 ARM64 上天然原子，读取方一次快照即可
 * 同时取到下限/上限，不会出现"读到新下限+旧上限"的撕裂状态；
 * 单写多读，故仅用 volatile，不加锁。 */
extern volatile uint32_t g_business_range;

/* 从打包的区间值中解出下限/上限 */
static inline uint16_t biz_range_min(uint32_t range)
{
    return (uint16_t)(range >> 16);
}

static inline uint16_t biz_range_max(uint32_t range)
{
    return (uint16_t)(range & 0xFFFFu);
}

int portcfg_load(void);                     /* 启动时从配置文件载入端口区间；文件缺失或非法时保持默认值并返回 -1 */
int portcfg_apply_and_save(uint16_t port_min, uint16_t port_max);  /* 校验并持久化端口区间；成功返回 0，失败返回 -1 且不改写当前生效值 */

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
int clock_send_now(void);                       /* 立即下发一帧 0x19 时钟帧（无延时、无重试），供 0x02 时间报文回调 */
int freqtable_send_on_boot(void);               /* 开机一次性下发 0x09 频表帧（读网管 JSON，失败不致命） */
int ctrl_get_work_status(uint8_t *dev_status, uint8_t *rf_state);  /* 取缓存中的通信设备状态与静默/辐射状态 */

/* ========= 组播链路相关函数（通信设备 <-> 指挥协同计算机） ========= */
int link_socket_init(void);                     /* 创建组播 socket 并加入本机组播组 */
void link_socket_close(void);                   /* 关闭组播 socket */

int amp_send_msg(uint32_t dst_ip, const uint8_t *payload, size_t len);  /* 构造并发送一条业务消息 */

/* ========= 业务面准入与诊断（定义见 user_datapath.c，user_bcast.c 复用） ========= */
/* TUN 入口与广播桥接入口共用同一份端口准入策略，避免两条入口的过滤规则分叉 */
int classify_udp_business_port(const uint8_t *pkt, size_t len);


/* ========= 广播二层桥接（用户态 AF_PACKET，定义见 user_bcast.c） ========= */
int bcast_socket_init(void);                    /* 创建并绑定 eth0 的 AF_PACKET 套接字，失败返回 -1（非致命） */
void bcast_socket_close(void);                  /* 关闭广播桥接套接字 */
void *bcast_uplink_thread(void *arg);           /* 广播上行线程：抓 eth0 二层广播 -> /dev/amp_ipi（node 255） */
void bcast_maybe_relay(const uint8_t *pkt, size_t len);  /* 下行中继：daddr=255 的包在 eth0 上补发一份二层广播给本地 PC */

/* ========= 7 个工作线程入口 ========= */
void *amp_tx_thread(void *arg);                 /* 统一业务发送线程：从队列取数据写 /dev/amp_ipi */
void *tun_to_amp_thread(void *arg);             /* TUN -> AMP 线程：从 rf0 读 IP 包，聚合发送 */
void *amp_to_tun_thread(void *arg);             /* AMP -> TUN 线程：从 /dev/amp_ipi 读，写回 rf0 */
void *control_rx_to_amp_thread(void *arg);      /* 控制上行线程：UDP 3409 -> /dev/amp_ctrl */
void *control_amp_to_udp_thread(void *arg);     /* 控制下行线程：/dev/amp_ctrl -> UDP 3419 */
void *link_mcast_thread(void *arg);             /* 组播链路线程：检测/状态报文发送与反馈接收 */

#endif
