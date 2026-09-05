/***************************/
/* 本文件存放定义的全局变量 */
/***************************/
#ifndef DRIVER_AMP_HW_H
#define DRIVER_AMP_HW_H

#include <linux/miscdevice.h>           /* misc 设备注册 / 注销 */
#include <linux/platform_device.h>      /* 平台设备驱动模型（probe/remove） */
#include <linux/wait.h>                 /* 等待队列（wait_event_interruptible） */
#include <linux/fs.h>                   /* 文件操作结构体 file_operations */
#include <linux/types.h>                /* 基础类型定义 */
#include <linux/atomic.h>               /* 原子变量（atomic_t） */
#include <linux/io.h>                   /* IO 内存读写（readb/writeb/writel/ioremap_nocache） */
#include <linux/spinlock.h>             /* 自旋锁（spinlock_t） */
#include <linux/bitops.h>               /* 位操作辅助宏 */

#include "driver_struct.h"              /* 驱动侧业务/控制消息结构体 */
#include <linux/printk.h>              /* pr_info_ratelimited / pr_warn_ratelimited */

/* ============================================================
 * 运行时打印开关
 *   通过 /sys/module/driver_amp/parameters/amp_verbose 控制：
 *     echo 0 > .../amp_verbose  关闭所有串口打印
 *     echo 1 > .../amp_verbose  开启（默认）
 *   关闭后 amp_pr_info / amp_pr_warn 不输出到串口，不影响业务逻辑。
 * ============================================================ */
extern bool amp_verbose;

/* 受 amp_verbose 控制的打印宏：amp_verbose=false 时直接短路不打印 */
#define amp_pr_info(fmt, ...)  \
    do { if (amp_verbose) pr_info_ratelimited(fmt, ##__VA_ARGS__); } while (0)
#define amp_pr_warn(fmt, ...)  \
    do { if (amp_verbose) pr_warn_ratelimited(fmt, ##__VA_ARGS__); } while (0)

/* ============================================================
 * 协议B 共享内存地址总览（主控 CPU0 <-> 组网 CPU1）
 *   TX 区 0x3800xxxx：CPU0 写入、CPU1 读取（主控 -> 组网）
 *   RX 区 0x3900xxxx：CPU1 写入、CPU0 读取（组网 -> 主控）
 *   每个方向都有「业务区(0x...0000/0x...1000)」与「控制区(0x...2000/0x...3000)」
 *   数据模式寄存器：0x38005000(CPU0写,bit0=业务待读/bit1=控制待读)、0x39005000(CPU1写)
 * 详见《主控与路由通信协议》共享内存布局章节。
 * ============================================================ */

/* TX: 主控 -> 组网 (0x3800xxxx) */
#define TX_NET_IP_ADDR      0x38000000              // 发送业务数据IP地址
#define TX_NET_NODE_ID      0x38000004              // 发送业务数据节点号
#define TX_NET_IP_LEN       0x38000008              // 发送长度
#define TX_NET_TYPE         0x3800000C              // 发送业务数据类型
#define IP_TX_RAM_ADDR      0x38001000              // 数据首地址 (4K)
//以下为控制
#define TX_CTRL_LEN         0x38002008              // 发送控制数据长度
#define TX_CTRL_TYPE        0x3800200C              // 发送控制数据类型
#define CTRL_TX_RAM_ADDR    0x38003000              // 控制数据首地址 (4K)
/* 数据模式寄存器：bit0=业务，bit1=控制 */
#define CTRL_REG_ADDR       0x38005000              // 数据模式寄存器地址
#define CTRL_REG_UDP_BIT    0x01                    // 业务数据待读取
#define CTRL_REG_CTRL_BIT   0x02                    // 控制数据待读取
#define CTRL_REG_TX_MASK    (CTRL_REG_UDP_BIT | CTRL_REG_CTRL_BIT)

/* TX 单槽轮询等待参数：CPU0 写新帧前等待 CPU1 清掉 ctrl_reg 位的最大时间 */
#define AMP_TX_POLL_TIMEOUT_US  5000                // 最大等待 5ms
#define AMP_TX_POLL_STEP_US     100                 // 每次轮询间隔 100us

/* RX: 组网 -> 主控 (0x3900xxxx) */
#define RX_NET_IP_ADDR      0x39000000  // 接收业务数据IP地址
#define RX_NET_NODE_ID      0x39000004  // 接收业务数据节点号
#define RX_NET_IP_LEN       0x39000008  // 接收长度
#define RX_NET_TYPE         0x3900000C  // 接收数据类型
#define IP_RX_RAM_ADDR      0x39001000  // 数据首地址 (4K)
//以下为控制
#define RX_CTRL_LEN         0x39002008  // 接收控制数据长度
#define RX_CTRL_TYPE        0x3900200C  // 接收控制数据类型
#define CTRL_RX_RAM_ADDR    0x39003000  // 控制数据首地址 (4K)
#define RX_CTRL_REG_ADDR    0x39005000  // CPU1->CPU0 数据模式寄存器地址


#define AMP_SGI_TX          15          /* 写入后触发中断号：CPU0 写完后通过 SGI15 通知 CPU1 读取 */
#define AMP_SGI_RX          14          /* 读取响应中断号：CPU1 回传数据后通过 SGI14 通知 CPU0 读取 */

#define RX_RING_SIZE        64          /* RX 环形队列深度，最多缓存 64 条待读消息 */

/* RX缓存：CPU1(组网) -> CPU0(主控) 的业务数据先进入驱动侧环形队列，用户态再read()取走 */
struct amp_rx_slot {
    struct amp_net_msg msg;             /* 缓存的完整业务消息 */
    size_t msg_bytes;                   /* 本消息的实际字节数（header + payload） */
};

/* 控制RX缓存：CPU1(组网) -> CPU0(主控) 的控制数据先进入驱动侧环形队列 */
struct amp_ctrl_rx_slot {
    struct amp_ctrl_msg msg;            /* 缓存的完整控制消息 */
    size_t msg_bytes;                   /* 本消息的实际字节数 */
};

/* ========= 业务 RX 环形队列全局变量 ========= */
extern struct amp_rx_slot rx_ring[RX_RING_SIZE];        /* 业务 RX 环形队列本体（64 槽） */
extern unsigned int rx_ring_head;                       /* 队列头索引：用户态 read() 从这里取走 */
extern unsigned int rx_ring_tail;                       /* 队列尾索引：中断处理函数从这里写入 */
extern atomic_t rx_ring_count;                          /* 当前队列中待读取的消息数（原子变量，中断与进程上下文共享） */
extern spinlock_t rx_ring_lock;                         /* 保护队列头/尾/计数的自旋锁 */
extern atomic_t rx_drop_full;                           /* 队列满导致的丢包计数 */
extern atomic_t rx_enqueued;                            /* 累计入队成功计数 */
extern atomic_t rx_dequeued;                            /* 累计出队成功计数 */
extern atomic_t tx_busy_timeout;                        /* TX 单槽被占用导致超时丢包计数 */
extern wait_queue_head_t rx_wq;                         /* 业务 RX 等待队列：用户态阻塞等待数据到来 */

/* ========= 控制 RX 环形队列全局变量 ========= */
extern struct amp_ctrl_rx_slot ctrl_rx_ring[RX_RING_SIZE];  /* 控制 RX 环形队列本体 */
extern unsigned int ctrl_rx_ring_head;                      /* 控制队列头索引 */
extern unsigned int ctrl_rx_ring_tail;                      /* 控制队列尾索引 */
extern atomic_t ctrl_rx_ring_count;                         /* 控制队列待读取消息数 */
extern spinlock_t ctrl_rx_ring_lock;                        /* 控制队列自旋锁 */
extern atomic_t ctrl_rx_drop_full;                          /* 控制队列满丢包计数 */
extern atomic_t ctrl_rx_enqueued;                           /* 控制队列入队计数 */
extern atomic_t ctrl_rx_dequeued;                           /* 控制队列出队计数 */
extern wait_queue_head_t ctrl_rx_wq;                        /* 控制 RX 等待队列 */

/* ========= 共享内存虚拟地址映射（TX: CPU0 -> CPU1，0x3800xxxx 区域） ========= */
extern void __iomem *tx_ip_addr;          /* 发送业务数据：目标 IP 地址寄存器 */
extern void __iomem *tx_node_id;          /* 发送业务数据：目标节点号寄存器 */
extern void __iomem *tx_len;              /* 发送业务数据：载荷长度寄存器 */
extern void __iomem *tx_type;             /* 发送业务数据：数据类型寄存器 */
extern void __iomem *tx_data_addr;        /* 发送业务数据区首地址（4K 数据块） */
extern void __iomem *tx_ctrl_len;         /* 发送控制数据：载荷长度寄存器 */
extern void __iomem *tx_ctrl_type;        /* 发送控制数据：数据类型寄存器 */
extern void __iomem *tx_ctrl_data_addr;   /* 发送控制数据区首地址（4K 数据块） */

/* ========= 共享内存虚拟地址映射（RX: CPU1 -> CPU0，0x3900xxxx 区域） ========= */
extern void __iomem *ctrl_reg;              /* CPU0->CPU1 数据模式寄存器（bit0=业务待读, bit1=控制待读） */
extern void __iomem *rx_ip_addr;            /* 接收业务数据：来源 IP 地址寄存器 */
extern void __iomem *rx_node_id;            /* 接收业务数据：来源节点号寄存器 */
extern void __iomem *rx_len;                /* 接收业务数据：载荷长度寄存器 */
extern void __iomem *rx_type;               /* 接收业务数据：数据类型寄存器 */
extern void __iomem *rx_data_addr;          /* 接收业务数据区首地址（4K 数据块） */
extern void __iomem *rx_ctrl_len;           /* 接收控制数据：载荷长度寄存器 */
extern void __iomem *rx_ctrl_type;          /* 接收控制数据：数据类型寄存器 */
extern void __iomem *rx_ctrl_data_addr;     /* 接收控制数据区首地址（4K 数据块） */
extern void __iomem *rx_ctrl_reg;           /* CPU1->CPU0 数据模式寄存器 */

/* ========= misc 设备结构体声明 ========= */
extern struct miscdevice amp_miscdev;       /* 业务 misc 设备：/dev/amp_ipi */
extern struct miscdevice amp_ctrl_miscdev;  /* 控制 misc 设备：/dev/amp_ctrl */

/* ========= 驱动侧核心函数声明 ========= */
u32 ip_to_nodeid(__be32 ip_be);             /* IP 地址 -> 节点号映射（协议A 规则） */
u32 nodeid_to_ip(u32 node_id);              /* 节点号 -> IP 地址逆映射 */

int process_udp_data(struct amp_net_msg *msg);      /* 处理业务数据：写入 TX 共享内存并通知 CPU1 */
int process_ctrl_data(struct amp_ctrl_msg *msg);    /* 处理控制数据：写入 TX 共享内存并通知 CPU1 */
void cpu1_to_cpu0_handler(int ipinr, void *dev_id); /* SGI14 软中断处理函数：CPU1 通知 CPU0 有数据回传 */

ssize_t amp_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos);     /* 用户态写入业务数据接口：copy_from_user -> process_udp_data */
ssize_t amp_read(struct file *file, char __user *buf, size_t len, loff_t *ppos);            /* 用户态读取业务数据接口：从 RX 环形队列 copy_to_user */
ssize_t amp_ctrl_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos);/* 用户态写入控制数据接口：copy_from_user -> process_ctrl_data */
ssize_t amp_ctrl_read(struct file *file, char __user *buf, size_t len, loff_t *ppos);       /* 用户态读取控制数据接口：从控制 RX 环形队列 copy_to_user */

int driver_amp_map_resources(void);         /* 共享内存映射函数：ioremap_nocache 所有物理地址 -> 虚拟地址 */
void driver_amp_unmap_resources(void);      /* 释放所有共享内存映射函数：iounmap 所有地址 */
bool driver_amp_resources_ready(void);      /* 映射验证函数：检查所有地址是否都映射成功 */

#endif
