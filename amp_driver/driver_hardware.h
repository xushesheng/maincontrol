/*************************/
/* 本文件存放定义的变量 */
/*************************/
#ifndef DRIVER_AMP_HW_H
#define DRIVER_AMP_HW_H

#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/io.h>

#include "driver_struct.h"

/* TX: 主控 -> 组网 (0x3800xxxx) */
#define TX_BASE_ADDR        0x38000000              // TX基地址
#define TX_NET_IP_ADDR      (TX_BASE_ADDR + 0x00)   // 发送数据IP地址
#define TX_NET_NODE_ID      (TX_BASE_ADDR + 0x04)   // 发送数据节点号
#define TX_NET_IP_LEN       (TX_BASE_ADDR + 0x08)   // 发送长度
#define TX_TEST_FREQ_ADDR   (TX_BASE_ADDR + 0x0C)   // 测试信号频率
#define TX_TEST_ENABLE_ADDR (TX_BASE_ADDR + 0x10)   // 测试信号使能
#define TX_FIXED_FREQ_ADDR  (TX_BASE_ADDR + 0x14)   // 定频频率
#define TX_NET_TEST_ADDR    (TX_BASE_ADDR + 0x18)   // 组网数据发送测试
#define TX_LOOPBACK_ADDR    (TX_BASE_ADDR + 0x1C)   // 数据自回环
#define TX_IQ_SWAP_ADDR     (TX_BASE_ADDR + 0x20)   // 接收基带IQ对调
#define TX_ATTEN_ADDR       (TX_BASE_ADDR + 0x24)   // 发射衰减系数
#define IP_TX_RAM_ADDR      0x38001000              // 数据首地址 (4K)
/* 控制寄存器地址 - 用于区分UDP数据和控制数据 */
#define CTRL_REG_ADDR       0x38005000              // 控制寄存器地址

/* RX: 组网 -> 主控 (0x3900xxxx) */
#define RX_NET_IP_ADDR      0x39000000  // 接收数据IP地址
#define RX_NET_NODE_ID      0x39000004  // 接收数据节点号
#define RX_NET_IP_LEN       0x39000008  // 接收长度
#define IP_RX_RAM_ADDR      0x39001000  // 数据首地址 (4K)
/* 监测数据地址 */
#define CH_TEMP_ADDR        0x3900000C  // 信道模块温度

#define AMP_SGI_TX          15
#define AMP_SGI_RX          14

/* RX缓存：CPU1(组网) -> CPU0(主控) 的数据先落在这里，用户态再read()取走 */
extern struct amp_net_msg rx_msg;
extern size_t rx_msg_bytes;
extern atomic_t rx_pending;  // 防重入/丢包保护：1表示有包未读
extern wait_queue_head_t rx_wq;

/* 共享内存虚拟地址映射 */
extern void __iomem *tx_ip_addr;          // 发送IP地址
extern void __iomem *tx_node_id;          // 发送节点号
extern void __iomem *tx_len;              // 发送长度
extern void __iomem *tx_data_addr;        // 发送数据区

/* 控制参数寄存器映射 */
extern void __iomem *tx_test_freq;          // 测试信号频率
extern void __iomem *tx_test_enable;        // 测试信号使能
extern void __iomem *tx_fixed_freq;         // 定频频率
extern void __iomem *tx_net_test;           // 组网数据发送测试
extern void __iomem *tx_loopback;           // 数据自回环
extern void __iomem *tx_iq_swap;            // 接收基带IQ对调
extern void __iomem *tx_atten;              // 发射衰减系数
extern void __iomem *ctrl_reg;              // 控制寄存器
extern void __iomem *rx_ip_addr;            // 接收IP地址
extern void __iomem *rx_node_id;            // 接收节点号
extern void __iomem *rx_len;                // 接收长度
extern void __iomem *rx_data_addr;          // 接收数据区
/******************/
/* 监测数据地址映射 */
/******************/
extern void __iomem *ch_temp_addr;          // 信道温度

extern struct miscdevice amp_miscdev;       //设备结构体声明

u32 ip_to_nodeid(__be32 ip_be);             //IP与节点映射函数声明
u32 nodeid_to_ip(u32 node_id);

int process_udp_data(struct amp_net_msg *msg);      /* 处理UDP数据 */
int process_control_data(struct amp_net_msg *msg);  /* 处理控制数据 */
void cpu1_to_cpu0_handler(int ipinr, void *dev_id); /* 软中断处理函数：CPU1通知CPU0 */

ssize_t amp_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos);     /* 用户态写入接口 */
ssize_t amp_read(struct file *file, char __user *buf, size_t len, loff_t *ppos);            /* 用户态读取接口：读取CPU1回来的数据包 */

int driver_amp_map_resources(void);         /*共享内存映射函数*/
void driver_amp_unmap_resources(void);      /*释放所有共享内存映射函数*/
bool driver_amp_resources_ready(void);      /*映射验证函数*/

#endif
