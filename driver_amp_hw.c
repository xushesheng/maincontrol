#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/wait.h>
#include <linux/atomic.h>

#include "driver_amp_hw.h"

struct amp_net_msg rx_msg;
size_t rx_msg_bytes;
atomic_t rx_pending = ATOMIC_INIT(0);
wait_queue_head_t rx_wq;

void __iomem *tx_ip_addr;
void __iomem *tx_node_id;
void __iomem *tx_len;
void __iomem *tx_data_addr;
void __iomem *tx_test_freq;
void __iomem *tx_test_enable;
void __iomem *tx_fixed_freq;
void __iomem *tx_net_test;
void __iomem *tx_loopback;
void __iomem *tx_iq_swap;
void __iomem *tx_atten;
void __iomem *ctrl_reg;
void __iomem *rx_ip_addr;
void __iomem *rx_node_id;
void __iomem *rx_len;
void __iomem *rx_data_addr;
void __iomem *ch_temp_addr;

bool driver_amp_resources_ready(void)
{
    return tx_ip_addr && tx_node_id && tx_len && tx_data_addr &&
           tx_test_freq && tx_test_enable && tx_fixed_freq &&
           tx_net_test && tx_loopback && tx_iq_swap && tx_atten &&
           ctrl_reg && rx_ip_addr && rx_node_id && rx_len && rx_data_addr;
}

int driver_amp_map_resources(void)
{
    tx_ip_addr = ioremap_nocache(TX_NET_IP_ADDR, 4);
    tx_node_id = ioremap_nocache(TX_NET_NODE_ID, 4);
    tx_len = ioremap_nocache(TX_NET_IP_LEN, 4);
    tx_data_addr = ioremap_nocache(IP_TX_RAM_ADDR, MAX_PAYLOAD_SIZE);

    tx_test_freq = ioremap_nocache(TX_TEST_FREQ_ADDR, 4);
    tx_test_enable = ioremap_nocache(TX_TEST_ENABLE_ADDR, 4);
    tx_fixed_freq = ioremap_nocache(TX_FIXED_FREQ_ADDR, 4);
    tx_net_test = ioremap_nocache(TX_NET_TEST_ADDR, 4);
    tx_loopback = ioremap_nocache(TX_LOOPBACK_ADDR, 4);
    tx_iq_swap = ioremap_nocache(TX_IQ_SWAP_ADDR, 4);
    tx_atten = ioremap_nocache(TX_ATTEN_ADDR, 4);
    ctrl_reg = ioremap_nocache(CTRL_REG_ADDR, 4);

    rx_ip_addr = ioremap_nocache(RX_NET_IP_ADDR, 4);
    rx_node_id = ioremap_nocache(RX_NET_NODE_ID, 4);
    rx_len = ioremap_nocache(RX_NET_IP_LEN, 4);
    rx_data_addr = ioremap_nocache(IP_RX_RAM_ADDR, MAX_PAYLOAD_SIZE);
    ch_temp_addr = ioremap_nocache(CH_TEMP_ADDR, 4);

    if (!driver_amp_resources_ready())
        return -ENOMEM;

    writeb(0x00, ctrl_reg);
    wmb();

    init_waitqueue_head(&rx_wq);
    atomic_set(&rx_pending, 0);
    return 0;
}

void driver_amp_unmap_resources(void)
{
    if (tx_ip_addr) iounmap(tx_ip_addr);
    if (tx_node_id) iounmap(tx_node_id);
    if (tx_len) iounmap(tx_len);
    if (tx_data_addr) iounmap(tx_data_addr);
    if (ctrl_reg) iounmap(ctrl_reg);
    if (tx_test_freq) iounmap(tx_test_freq);
    if (tx_test_enable) iounmap(tx_test_enable);
    if (tx_fixed_freq) iounmap(tx_fixed_freq);
    if (tx_net_test) iounmap(tx_net_test);
    if (tx_loopback) iounmap(tx_loopback);
    if (tx_iq_swap) iounmap(tx_iq_swap);
    if (tx_atten) iounmap(tx_atten);
    if (rx_ip_addr) iounmap(rx_ip_addr);
    if (rx_node_id) iounmap(rx_node_id);
    if (rx_len) iounmap(rx_len);
    if (rx_data_addr) iounmap(rx_data_addr);
    if (ch_temp_addr) iounmap(ch_temp_addr);

    tx_ip_addr = NULL;
    tx_node_id = NULL;
    tx_len = NULL;
    tx_data_addr = NULL;
    tx_test_freq = NULL;
    tx_test_enable = NULL;
    tx_fixed_freq = NULL;
    tx_net_test = NULL;
    tx_loopback = NULL;
    tx_iq_swap = NULL;
    tx_atten = NULL;
    ctrl_reg = NULL;
    rx_ip_addr = NULL;
    rx_node_id = NULL;
    rx_len = NULL;
    rx_data_addr = NULL;
    ch_temp_addr = NULL;
}
