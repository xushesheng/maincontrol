#ifndef DRIVER_AMP_HW_H
#define DRIVER_AMP_HW_H

#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/io.h>

#include "driver_amp_proto.h"

#define TX_BASE_ADDR 0x38000000
#define TX_NET_IP_ADDR (TX_BASE_ADDR + 0x00)
#define TX_NET_NODE_ID (TX_BASE_ADDR + 0x04)
#define TX_NET_IP_LEN (TX_BASE_ADDR + 0x08)
#define TX_TEST_FREQ_ADDR (TX_BASE_ADDR + 0x0C)
#define TX_TEST_ENABLE_ADDR (TX_BASE_ADDR + 0x10)
#define TX_FIXED_FREQ_ADDR (TX_BASE_ADDR + 0x14)
#define TX_NET_TEST_ADDR (TX_BASE_ADDR + 0x18)
#define TX_LOOPBACK_ADDR (TX_BASE_ADDR + 0x1C)
#define TX_IQ_SWAP_ADDR (TX_BASE_ADDR + 0x20)
#define TX_ATTEN_ADDR (TX_BASE_ADDR + 0x24)
#define IP_TX_RAM_ADDR 0x38001000
#define CTRL_REG_ADDR 0x38005000

#define RX_NET_IP_ADDR 0x39000000
#define RX_NET_NODE_ID 0x39000004
#define RX_NET_IP_LEN 0x39000008
#define IP_RX_RAM_ADDR 0x39001000
#define CH_TEMP_ADDR 0x3900000C

#define AMP_SGI_TX 15
#define AMP_SGI_RX 14

extern struct amp_net_msg rx_msg;
extern size_t rx_msg_bytes;
extern atomic_t rx_pending;
extern wait_queue_head_t rx_wq;

extern void __iomem *tx_ip_addr;
extern void __iomem *tx_node_id;
extern void __iomem *tx_len;
extern void __iomem *tx_data_addr;
extern void __iomem *tx_test_freq;
extern void __iomem *tx_test_enable;
extern void __iomem *tx_fixed_freq;
extern void __iomem *tx_net_test;
extern void __iomem *tx_loopback;
extern void __iomem *tx_iq_swap;
extern void __iomem *tx_atten;
extern void __iomem *ctrl_reg;
extern void __iomem *rx_ip_addr;
extern void __iomem *rx_node_id;
extern void __iomem *rx_len;
extern void __iomem *rx_data_addr;
extern void __iomem *ch_temp_addr;

extern struct miscdevice amp_miscdev;

u32 ip_to_nodeid(__be32 ip_be);
u32 nodeid_to_ip(u32 node_id);

int process_udp_data(struct amp_net_msg *msg);
int process_control_data(struct amp_net_msg *msg);
void cpu1_to_cpu0_handler(int ipinr, void *dev_id);

ssize_t amp_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos);
ssize_t amp_read(struct file *file, char __user *buf, size_t len, loff_t *ppos);

int driver_amp_map_resources(void);
void driver_amp_unmap_resources(void);
bool driver_amp_resources_ready(void);

#endif
