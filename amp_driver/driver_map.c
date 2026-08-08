/*************************/
/* 本文件存放映射相关函数 */
/*************************/
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/string.h>

#include "driver_hardware.h"

struct amp_rx_slot rx_ring[RX_RING_SIZE];
unsigned int rx_ring_head;
unsigned int rx_ring_tail;
atomic_t rx_ring_count = ATOMIC_INIT(0);
spinlock_t rx_ring_lock;
atomic_t rx_drop_full = ATOMIC_INIT(0);
atomic_t rx_enqueued = ATOMIC_INIT(0);
atomic_t rx_dequeued = ATOMIC_INIT(0);
atomic_t tx_busy_timeout = ATOMIC_INIT(0);      // TX 单槽被占用导致超时丢包计数
wait_queue_head_t rx_wq;

struct amp_ctrl_rx_slot ctrl_rx_ring[RX_RING_SIZE];
unsigned int ctrl_rx_ring_head;
unsigned int ctrl_rx_ring_tail;
atomic_t ctrl_rx_ring_count = ATOMIC_INIT(0);
spinlock_t ctrl_rx_ring_lock;
atomic_t ctrl_rx_drop_full = ATOMIC_INIT(0);
atomic_t ctrl_rx_enqueued = ATOMIC_INIT(0);
atomic_t ctrl_rx_dequeued = ATOMIC_INIT(0);
wait_queue_head_t ctrl_rx_wq;

void __iomem *tx_ip_addr;
void __iomem *tx_node_id;
void __iomem *tx_len;
void __iomem *tx_type;
void __iomem *tx_data_addr;
void __iomem *tx_ctrl_len;
void __iomem *tx_ctrl_type;
void __iomem *tx_ctrl_data_addr;
void __iomem *ctrl_reg;
void __iomem *rx_ip_addr;
void __iomem *rx_node_id;
void __iomem *rx_len;
void __iomem *rx_type;
void __iomem *rx_data_addr;
void __iomem *rx_ctrl_len;
void __iomem *rx_ctrl_type;
void __iomem *rx_ctrl_data_addr;
void __iomem *rx_ctrl_reg;

/*******************/
/*    映射验证函数  检查业务/控制两套共享内存和寄存器是否都映射成功*/
/*******************/
bool driver_amp_resources_ready(void)
{
    return tx_ip_addr && tx_node_id && tx_len && tx_type && tx_data_addr &&
           tx_ctrl_len && tx_ctrl_type && tx_ctrl_data_addr &&
           ctrl_reg && rx_ip_addr && rx_node_id && rx_len && rx_type &&
           rx_data_addr && rx_ctrl_len && rx_ctrl_type &&
           rx_ctrl_data_addr && rx_ctrl_reg;
}

/*******************/
/* 共享内存映射函数 */
/*******************/
int driver_amp_map_resources(void)
{
    /* 映射TX共享内存 */
    tx_ip_addr = ioremap_nocache(TX_NET_IP_ADDR, 4);
    tx_node_id = ioremap_nocache(TX_NET_NODE_ID, 4);
    tx_len = ioremap_nocache(TX_NET_IP_LEN, 4);
    tx_type = ioremap_nocache(TX_NET_TYPE, 4);
    tx_data_addr = ioremap_nocache(IP_TX_RAM_ADDR, MAX_PAYLOAD_SIZE);
    tx_ctrl_len = ioremap_nocache(TX_CTRL_LEN, 4);
    tx_ctrl_type = ioremap_nocache(TX_CTRL_TYPE, 4);
    tx_ctrl_data_addr = ioremap_nocache(CTRL_TX_RAM_ADDR, MAX_PAYLOAD_SIZE);

    /* 映射数据模式寄存器 */
    ctrl_reg = ioremap_nocache(CTRL_REG_ADDR, 4);

    /* 映射RX共享内存 */
    rx_ip_addr = ioremap_nocache(RX_NET_IP_ADDR, 4);
    rx_node_id = ioremap_nocache(RX_NET_NODE_ID, 4);
    rx_len = ioremap_nocache(RX_NET_IP_LEN, 4);
    rx_type = ioremap_nocache(RX_NET_TYPE, 4);
    rx_data_addr = ioremap_nocache(IP_RX_RAM_ADDR, MAX_PAYLOAD_SIZE);
    rx_ctrl_len = ioremap_nocache(RX_CTRL_LEN, 4);
    rx_ctrl_type = ioremap_nocache(RX_CTRL_TYPE, 4);
    rx_ctrl_data_addr = ioremap_nocache(CTRL_RX_RAM_ADDR, MAX_PAYLOAD_SIZE);
    rx_ctrl_reg = ioremap_nocache(RX_CTRL_REG_ADDR, 4);

    /* 检查映射是否成功 */
    if (!driver_amp_resources_ready()){
        //pr_err("Failed to ioremap shared memory\n");
        return -ENOMEM;
    }

    /* 初始化CPU0->CPU1与CPU1->CPU0数据模式寄存器为0x00 */
    writeb(0x00, ctrl_reg);
    writeb(0x00, rx_ctrl_reg);
    wmb();

    /* 初始化RX等待队列（用户态read()阻塞等待CPU1->CPU0数据） */
    init_waitqueue_head(&rx_wq);
    spin_lock_init(&rx_ring_lock);
    memset(rx_ring, 0, sizeof(rx_ring));
    rx_ring_head = 0;
    rx_ring_tail = 0;
    atomic_set(&rx_ring_count, 0);
    atomic_set(&rx_drop_full, 0);
    atomic_set(&rx_enqueued, 0);
    atomic_set(&rx_dequeued, 0);

    /* 初始化控制RX等待队列 */
    init_waitqueue_head(&ctrl_rx_wq);
    spin_lock_init(&ctrl_rx_ring_lock);
    memset(ctrl_rx_ring, 0, sizeof(ctrl_rx_ring));
    ctrl_rx_ring_head = 0;
    ctrl_rx_ring_tail = 0;
    atomic_set(&ctrl_rx_ring_count, 0);
    atomic_set(&ctrl_rx_drop_full, 0);
    atomic_set(&ctrl_rx_enqueued, 0);
    atomic_set(&ctrl_rx_dequeued, 0);
    return 0;
}

/********************************/
/* 驱动卸载时释放所有共享内存映射 */
/********************************/
void driver_amp_unmap_resources(void)
{
    if (tx_ip_addr) iounmap(tx_ip_addr);
    if (tx_node_id) iounmap(tx_node_id);
    if (tx_len) iounmap(tx_len);
    if (tx_type) iounmap(tx_type);
    if (tx_data_addr) iounmap(tx_data_addr);
    if (tx_ctrl_len) iounmap(tx_ctrl_len);
    if (tx_ctrl_type) iounmap(tx_ctrl_type);
    if (tx_ctrl_data_addr) iounmap(tx_ctrl_data_addr);
    if (ctrl_reg) iounmap(ctrl_reg);
    if (rx_ip_addr) iounmap(rx_ip_addr);
    if (rx_node_id) iounmap(rx_node_id);
    if (rx_len) iounmap(rx_len);
    if (rx_type) iounmap(rx_type);
    if (rx_data_addr) iounmap(rx_data_addr);
    if (rx_ctrl_len) iounmap(rx_ctrl_len);
    if (rx_ctrl_type) iounmap(rx_ctrl_type);
    if (rx_ctrl_data_addr) iounmap(rx_ctrl_data_addr);
    if (rx_ctrl_reg) iounmap(rx_ctrl_reg);

    tx_ip_addr = NULL;
    tx_node_id = NULL;
    tx_len = NULL;
    tx_type = NULL;
    tx_data_addr = NULL;
    tx_ctrl_len = NULL;
    tx_ctrl_type = NULL;
    tx_ctrl_data_addr = NULL;
    ctrl_reg = NULL;
    rx_ip_addr = NULL;
    rx_node_id = NULL;
    rx_len = NULL;
    rx_type = NULL;
    rx_data_addr = NULL;
    rx_ctrl_len = NULL;
    rx_ctrl_type = NULL;
    rx_ctrl_data_addr = NULL;
    rx_ctrl_reg = NULL;
}
