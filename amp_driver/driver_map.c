/*************************/
/* 本文件存放映射相关函数 */
/*************************/
#include <linux/module.h>             /* 内核模块框架 */
#include <linux/platform_device.h>    /* 平台设备驱动 */
#include <linux/io.h>                 /* IO 内存映射（memremap/memunmap） */
#include <linux/wait.h>               /* 等待队列初始化 */
#include <linux/atomic.h>             /* 原子变量初始化 */
#include <linux/string.h>             /* memset */

#include "driver_hardware.h"          /* 硬件地址宏与全局变量声明 */

/* ========= 业务 RX 环形队列的全局变量定义 ========= */
struct amp_rx_slot rx_ring[RX_RING_SIZE];       /* 业务 RX 环形队列数组（64 槽） */
unsigned int rx_ring_head;                       /* 队列头索引：初始为 0 */
unsigned int rx_ring_tail;                       /* 队列尾索引：初始为 0 */
atomic_t rx_ring_count = ATOMIC_INIT(0);        /* 待读取消息数初始化为 0 */
spinlock_t rx_ring_lock;                         /* 队列自旋锁（在 map 函数中 init） */
atomic_t rx_drop_full = ATOMIC_INIT(0);         /* 满丢包计数初始 0 */
atomic_t rx_enqueued = ATOMIC_INIT(0);          /* 入队累计计数初始 0 */
atomic_t rx_dequeued = ATOMIC_INIT(0);          /* 出队累计计数初始 0 */
atomic_t tx_busy_timeout = ATOMIC_INIT(0);      /* TX 单槽被占用导致超时丢包计数 */
wait_queue_head_t rx_wq;                         /* 业务等待队列头（在 map 函数中 init） */

/* ========= 控制 RX 环形队列的全局变量定义 ========= */
struct amp_ctrl_rx_slot ctrl_rx_ring[RX_RING_SIZE]; /* 控制 RX 环形队列数组 */
unsigned int ctrl_rx_ring_head;                      /* 控制队列头索引 */
unsigned int ctrl_rx_ring_tail;                      /* 控制队列尾索引 */
atomic_t ctrl_rx_ring_count = ATOMIC_INIT(0);       /* 控制队列待读数初始 0 */
spinlock_t ctrl_rx_ring_lock;                        /* 控制队列自旋锁 */
atomic_t ctrl_rx_drop_full = ATOMIC_INIT(0);        /* 控制满丢包计数 */
atomic_t ctrl_rx_enqueued = ATOMIC_INIT(0);         /* 控制入队计数 */
atomic_t ctrl_rx_dequeued = ATOMIC_INIT(0);         /* 控制出队计数 */
wait_queue_head_t ctrl_rx_wq;                        /* 控制等待队列头 */

/* ========= TX 方向 IO 内存指针（CPU0 -> CPU1） ========= */
void __iomem *tx_ip_addr;           /* 发送业务：目标 IP 地址寄存器 */
void __iomem *tx_node_id;           /* 发送业务：目标节点号寄存器 */
void __iomem *tx_len;               /* 发送业务：载荷长度寄存器 */
void __iomem *tx_type;              /* 发送业务：数据类型寄存器 */
void __iomem *tx_data_addr;         /* 发送业务：数据区首地址 */
void __iomem *tx_ctrl_len;          /* 发送控制：载荷长度寄存器 */
void __iomem *tx_ctrl_type;         /* 发送控制：数据类型寄存器 */
void __iomem *tx_ctrl_data_addr;    /* 发送控制：数据区首地址 */
void __iomem *ctrl_reg;             /* CPU0->CPU1 数据模式寄存器 */

/* ========= RX 方向 IO 内存指针（CPU1 -> CPU0） ========= */
void __iomem *rx_ip_addr;           /* 接收业务：来源 IP 地址寄存器 */
void __iomem *rx_node_id;           /* 接收业务：来源节点号寄存器 */
void __iomem *rx_len;               /* 接收业务：载荷长度寄存器 */
void __iomem *rx_type;              /* 接收业务：数据类型寄存器 */
void __iomem *rx_data_addr;         /* 接收业务：数据区首地址 */
void __iomem *rx_ctrl_len;          /* 接收控制：载荷长度寄存器 */
void __iomem *rx_ctrl_type;         /* 接收控制：数据类型寄存器 */
void __iomem *rx_ctrl_data_addr;    /* 接收控制：数据区首地址 */
void __iomem *rx_ctrl_reg;          /* CPU1->CPU0 数据模式寄存器 */

/* 共享内存/寄存器映射表：物理地址 -> 全局指针，统一 memremap/memunmap */
struct amp_io_entry {
    void __iomem **slot;        /* 指向全局 void __iomem * 指针的二级指针（用于赋值） */
    resource_size_t phys;       /* 物理地址 */
    unsigned long size;         /* 映射大小（字节） */
};

/* 映射表：列出所有需要映射的物理地址，在 map/unmap 中批量处理 */
static struct amp_io_entry amp_io_map[] = {
    { &tx_ip_addr,        TX_NET_IP_ADDR,        4 },
    { &tx_node_id,        TX_NET_NODE_ID,        4 },
    { &tx_len,            TX_NET_IP_LEN,         4 },
    { &tx_type,           TX_NET_TYPE,           4 },
    { &tx_data_addr,      IP_TX_RAM_ADDR,        MAX_PAYLOAD_SIZE },
    { &tx_ctrl_len,       TX_CTRL_LEN,           4 },
    { &tx_ctrl_type,      TX_CTRL_TYPE,          4 },
    { &tx_ctrl_data_addr, CTRL_TX_RAM_ADDR,      MAX_PAYLOAD_SIZE },
    { &ctrl_reg,          CTRL_REG_ADDR,         4 },
    { &rx_ip_addr,        RX_NET_IP_ADDR,        4 },
    { &rx_node_id,        RX_NET_NODE_ID,        4 },
    { &rx_len,            RX_NET_IP_LEN,         4 },
    { &rx_type,           RX_NET_TYPE,           4 },
    { &rx_data_addr,      IP_RX_RAM_ADDR,        MAX_PAYLOAD_SIZE },
    { &rx_ctrl_len,       RX_CTRL_LEN,           4 },
    { &rx_ctrl_type,      RX_CTRL_TYPE,          4 },
    { &rx_ctrl_data_addr, CTRL_RX_RAM_ADDR,      MAX_PAYLOAD_SIZE },
    { &rx_ctrl_reg,       RX_CTRL_REG_ADDR,      4 },
};

/*******************/
/*    映射验证函数  检查业务/控制两套共享内存和寄存器是否都映射成功*/
/*******************/
/* 映射验证函数：检查业务/控制两套共享内存和寄存器是否都映射成功 */
bool driver_amp_resources_ready(void)
{
    /* 逐一检查所有 18 个 IO 指针是否非空（非 NULL 即表示 memremap 成功） */
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
    size_t i;                                       /* 循环索引 */
    size_t n = sizeof(amp_io_map) / sizeof(amp_io_map[0]);   /* 计算映射表条目数 */

    /* 批量 memremap：禁用 CPU 缓存，保证每次读写直达硬件 */
    for (i = 0; i < n; i++)
        *amp_io_map[i].slot = memremap(amp_io_map[i].phys, amp_io_map[i].size,MEMREMAP_WB);    /* 映射为可写缓存模式 */

    /* 验证所有指针都映射成功 */
    if (!driver_amp_resources_ready())
        return -ENOMEM;

    /* 初始化CPU0->CPU1与CPU1->CPU0数据模式寄存器为0x00 */
    writeb(0x00, ctrl_reg);
    writeb(0x00, rx_ctrl_reg);
    wmb();

    /* 初始化业务RX等待队列（用户态read()阻塞等待CPU1->CPU0数据） */
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
    size_t i;
    size_t n = sizeof(amp_io_map) / sizeof(amp_io_map[0]);   /* 条目数 */

    /* 批量 memunmap：只释放非空的映射，释放后置 NULL 防止野指针 */
    for (i = 0; i < n; i++) {
        if (*amp_io_map[i].slot) {                  /* 指针非空才释放 */
            memunmap(*amp_io_map[i].slot);           /* 取消 IO 内存映射 */
            *amp_io_map[i].slot = NULL;             /* 置空，防止后续误用 */
        }
    }
}
