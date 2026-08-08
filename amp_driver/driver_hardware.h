/***************************/
/* 本文件存放定义的全局变量 */
/***************************/
#ifndef DRIVER_AMP_HW_H
#define DRIVER_AMP_HW_H

#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/bitops.h>

#include "driver_struct.h"

/* TX: 主控 -> 组网 (0x3800xxxx) */
#define TX_NET_IP_ADDR      0x38000000              // 发送业务数据IP地址
#define TX_NET_NODE_ID      0x38000004              // 发送业务数据节点号
#define TX_NET_IP_LEN       0x38000008              // 发送长度
#define TX_NET_TYPE         0x3800000C              // 发送业务数据类型
#define IP_TX_RAM_ADDR      0x38001000              // 数据首地址 (4K)
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
#define RX_CTRL_LEN         0x39002008  // 接收控制数据长度
#define RX_CTRL_TYPE        0x3900200C  // 接收控制数据类型
#define CTRL_RX_RAM_ADDR    0x39003000  // 控制数据首地址 (4K)
#define RX_CTRL_REG_ADDR    0x39005000  // CPU1->CPU0 数据模式寄存器地址


#define AMP_SGI_TX          15          //写入后触发中断号
#define AMP_SGI_RX          14          //读取响应中断号

#define RX_RING_SIZE        64

/* RX缓存：CPU1(组网) -> CPU0(主控) 的数据先进入驱动侧环形队列，用户态再read()取走 */
struct amp_rx_slot {
    struct amp_net_msg msg;
    size_t msg_bytes;
};

/* 控制RX缓存：CPU1(组网) -> CPU0(主控) 的控制数据先进入驱动侧环形队列 */
struct amp_ctrl_rx_slot {
    struct amp_ctrl_msg msg;
    size_t msg_bytes;
};

extern struct amp_rx_slot rx_ring[RX_RING_SIZE];        //建立一个amp_rx_slot类型的接收环
extern unsigned int rx_ring_head;
extern unsigned int rx_ring_tail;
extern atomic_t rx_ring_count;      //定义原子级变量：接收环计数
extern spinlock_t rx_ring_lock;     //定义不可抢占的自旋锁
extern atomic_t rx_drop_full;
extern atomic_t rx_enqueued;
extern atomic_t rx_dequeued;
extern atomic_t tx_busy_timeout;    // TX 单槽被占用导致超时丢包计数
extern wait_queue_head_t rx_wq;

extern struct amp_ctrl_rx_slot ctrl_rx_ring[RX_RING_SIZE];
extern unsigned int ctrl_rx_ring_head;
extern unsigned int ctrl_rx_ring_tail;
extern atomic_t ctrl_rx_ring_count;
extern spinlock_t ctrl_rx_ring_lock;
extern atomic_t ctrl_rx_drop_full;
extern atomic_t ctrl_rx_enqueued;
extern atomic_t ctrl_rx_dequeued;
extern wait_queue_head_t ctrl_rx_wq;

/* 共享内存虚拟地址映射 */
extern void __iomem *tx_ip_addr;          // 发送IP地址
extern void __iomem *tx_node_id;          // 发送节点号
extern void __iomem *tx_len;              // 发送长度
extern void __iomem *tx_type;             // 发送业务类型
extern void __iomem *tx_data_addr;        // 发送数据区
extern void __iomem *tx_ctrl_len;         // 发送控制长度
extern void __iomem *tx_ctrl_type;        // 发送控制类型
extern void __iomem *tx_ctrl_data_addr;   // 发送控制数据区

extern void __iomem *ctrl_reg;              // 数据模式寄存器
extern void __iomem *rx_ip_addr;            // 接收IP地址
extern void __iomem *rx_node_id;            // 接收节点号
extern void __iomem *rx_len;                // 接收长度
extern void __iomem *rx_type;               // 接收业务类型
extern void __iomem *rx_data_addr;          // 接收数据区
extern void __iomem *rx_ctrl_len;           // 接收控制长度
extern void __iomem *rx_ctrl_type;          // 接收控制类型
extern void __iomem *rx_ctrl_data_addr;     // 接收控制数据区
extern void __iomem *rx_ctrl_reg;           // CPU1->CPU0 数据模式寄存器

extern struct miscdevice amp_miscdev;       //设备结构体声明
extern struct miscdevice amp_ctrl_miscdev;  //控制设备结构体声明

u32 ip_to_nodeid(__be32 ip_be);             //IP与节点映射函数声明
u32 nodeid_to_ip(u32 node_id);

int process_udp_data(struct amp_net_msg *msg);      /* 处理UDP数据 */
int process_ctrl_data(struct amp_ctrl_msg *msg);    /* 处理控制数据 */
void cpu1_to_cpu0_handler(int ipinr, void *dev_id); /* 软中断处理函数：CPU1通知CPU0 */

ssize_t amp_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos);     /* 用户态写入业务数据接口 */
ssize_t amp_read(struct file *file, char __user *buf, size_t len, loff_t *ppos);            /* 用户态读取业务数据接口：读取CPU1回来的数据包 */
ssize_t amp_ctrl_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos);/* 用户态写入控制数据接口 */
ssize_t amp_ctrl_read(struct file *file, char __user *buf, size_t len, loff_t *ppos);       /* 用户态读取控制数据接口：读取CPU1回来的数据包 */

int driver_amp_map_resources(void);         /*共享内存映射函数*/
void driver_amp_unmap_resources(void);      /*释放所有共享内存映射函数*/
bool driver_amp_resources_ready(void);      /*映射验证函数*/

#endif
