/******************************************/
/*业务数据发送处理、RX 中断处理、IP 与节点映射*/
/******************************************/
#include <linux/module.h>
#include <linux/byteorder/generic.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <asm/barrier.h>
#include <linux/fs.h>

#include "driver_hardware.h"

#define NODEID_INVALID U32_MAX
/************************
*******IP-节点对应表*******
************************/
/* IP -> 节点号映射（协议A：上位机 IP = 192.168.1.(10 + node_id)，node 0~31）
 * 组播 239.0.0.1 -> 节点 254，广播 192.168.1.255 -> 节点 255，其余视为非法 */
u32 ip_to_nodeid(__be32 ip_be)
{
    u32 ip = ntohl(ip_be);          // 转成主机字节序，便于拆字节
    u8 b0 = (ip >> 24) & 0xff;
    u8 b1 = (ip >> 16) & 0xff;
    u8 b2 = (ip >> 8) & 0xff;
    u8 b3 = ip & 0xff;

    /* 组播：239.0.0.1 -> 254 */
    if (b0 == 239 && b1 == 0 && b2 == 0 && b3 == 1)
        return 254;
    /* 广播：192.168.1.255 -> 255 */
    if (b0 == 192 && b1 == 168 && b2 == 1 && b3 == 255)
        return 255;
    /* 单播：192.168.1.10 ~ 192.168.1.41 -> 0 ~ 31 */
    if (b0 == 192 && b1 == 168 && b2 == 1 && b3 >= 10 && b3 <= 41)
        return (u32)(b3 - 10);  // 对应 node_id <= 31

    return NODEID_INVALID;
}
/************************
* 把节点号转成业务目标IP *
************************/
u32 nodeid_to_ip(u32 node_id)
{
    u8 ip[4] = {192, 168, 1, 0};

    if (node_id <= 31) {
        ip[3] = 10 + node_id;
        return htonl(*(u32 *)ip);
    }

    if (node_id == 254) {
        ip[0] = 239;
        ip[1] = 0;
        ip[2] = 0;
        ip[3] = 1;
        return htonl(*(u32 *)ip);
    }

    if (node_id == 255) {
        ip[3] = 255;
        return htonl(*(u32 *)ip);
    }

    return 0;
}


/*************************************/
/* 数据模式寄存器操作：写入业务数据置位 */
/*************************************/
static void set_udp_data_enable(void)
{
    u8 reg_val = readb(ctrl_reg);

    (void)reg_val;
    /* bit0=UDP, bit1=CTRL: 设置UDP时要清掉CTRL位，避免两位同时为1 */
    reg_val = (reg_val & ~0x02) | 0x01;
    writeb(reg_val, ctrl_reg);
    wmb();
}

/*************************************/
/* 数据模式寄存器操作：写入控制数据置位 */
/*************************************/
static void set_ctrl_data_enable(void)
{
    u8 reg_val = readb(ctrl_reg);

    (void)reg_val;
    /* bit0=UDP, bit1=CTRL: 设置CTRL时要清掉UDP位 */
    reg_val = (reg_val & ~0x01) | 0x02;
    writeb(reg_val, ctrl_reg);
    wmb();
}

/*******************************************/
/* 数据模式寄存器操作：读取业务/控制数据清零 */
/******************************************/
static void clear_rx_enable_bit(void)
{
    writeb(0x00, rx_ctrl_reg);
    wmb();
}

/****************************************/
/* 把CPU1来的业务数据放入驱动侧环形缓冲区 */
/****************************************/
static int enqueue_business_rx_msg(u32 ip, u32 node_id, u32 data_type, u32 len)
{
    unsigned long flags;
    struct amp_rx_slot *slot;

    spin_lock_irqsave(&rx_ring_lock, flags);    //获取自旋锁的同时保存当前CPU的中断状态
    if (atomic_read(&rx_ring_count) >= RX_RING_SIZE) {
        atomic_inc(&rx_drop_full);
        spin_unlock_irqrestore(&rx_ring_lock, flags);
        pr_warn_ratelimited("RX ring full, dropping packet len=%u node=%u\n", len, node_id);
        return -ENOSPC;
    }

    slot = &rx_ring[rx_ring_tail];
    memset(slot, 0, sizeof(*slot));
    slot->msg.data_type = (u8)data_type;
    slot->msg.ip = ip;
    slot->msg.node_id = node_id;
    slot->msg.len = len;
    if (len > 0)
        memcpy_fromio(slot->msg.data, rx_data_addr, len);
    slot->msg_bytes = offsetof(struct amp_net_msg, data) + len;

    rx_ring_tail = (rx_ring_tail + 1U) % RX_RING_SIZE;
    atomic_inc(&rx_ring_count);
    atomic_inc(&rx_enqueued);
    spin_unlock_irqrestore(&rx_ring_lock, flags);
    wake_up_interruptible(&rx_wq);
    return 0; 
}

/****************************************/
/* 把CPU1来的控制数据放入驱动侧环形缓冲区 */
/****************************************/
static int enqueue_ctrl_rx_msg(u32 data_type, u32 len)
{
    unsigned long flags;
    struct amp_ctrl_rx_slot *slot;

    spin_lock_irqsave(&ctrl_rx_ring_lock, flags);
    if (atomic_read(&ctrl_rx_ring_count) >= RX_RING_SIZE) {
        atomic_inc(&ctrl_rx_drop_full);
        spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);
        pr_warn_ratelimited("CTRL RX ring full, dropping packet len=%u type=%u\n", len, data_type);
        return -ENOSPC;
    }

    slot = &ctrl_rx_ring[ctrl_rx_ring_tail];
    memset(slot, 0, sizeof(*slot));
    slot->msg.data_type = (u8)data_type;
    slot->msg.len = len;
    if (len > 0)
        memcpy_fromio(slot->msg.data, rx_ctrl_data_addr, len);
    slot->msg_bytes = offsetof(struct amp_ctrl_msg, data) + len;

    ctrl_rx_ring_tail = (ctrl_rx_ring_tail + 1U) % RX_RING_SIZE;
    atomic_inc(&ctrl_rx_ring_count);
    atomic_inc(&ctrl_rx_enqueued);
    spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);
    wake_up_interruptible(&ctrl_rx_wq);
    return 0;
}


/*************************************/
/* 上位机下发的UDP业务数据写入共享内存 */
/*************************************/

/*
 * 方案 A：等待 TX 单槽被 CPU1 释放。
 * CPU1 读走 TX 槽数据后会清掉 ctrl_reg 的 bit0/bit1；CPU0 在写入新帧前
 * 必须确认这两位都为 0，否则会发生单槽覆盖。
 * 该函数在 amp_write() 的进程上下文调用，允许短延时轮询。
 */
static int wait_for_tx_slot_idle(void)
{
    unsigned int waited = 0;

    while (readb(ctrl_reg) & CTRL_REG_TX_MASK) {
        /* 若当前进程收到信号，立即退出，避免长时间阻塞 */
        if (signal_pending(current))
            return -ERESTARTSYS;

        if (waited >= AMP_TX_POLL_TIMEOUT_US) {
            atomic_inc(&tx_busy_timeout);
            pr_warn_ratelimited("AMP TX slot busy timeout (ctrl_reg=0x%02x)\n",
                                readb(ctrl_reg));
            return -EBUSY;
        }

        udelay(AMP_TX_POLL_STEP_US);
        waited += AMP_TX_POLL_STEP_US;
    }

    return 0;
}


/****************************/
/* 语音或其他业务数据写入进程 */
/****************************/
int process_udp_data(struct amp_net_msg *msg)
{
    u32 nodeid;
    int ret;

    if (msg->data_type != 1 && msg->data_type != 2) {
        pr_warn_ratelimited("unsupported business data_type=%u\n", msg->data_type);
        return -EINVAL;
    }

    /*
     * 方案 A：写 TX 单槽前，等待 CPU1 把上一帧读走并清掉 ctrl_reg。
     * 若超时说明 CPU1 侧处理过慢，本次写入失败，避免覆盖上一帧。
     */
    ret = wait_for_tx_slot_idle();
    if (ret)
        return ret;

    if (msg->len > 0)
        memcpy_toio(tx_data_addr, msg->data, msg->len);

    /*
     * tx_data_addr 由 ioremap_nocache() 映射，已是 non-cacheable，
     * 不需要 dsb + __cpuc_flush_dcache_area()。wmb() 保证后续 writel
     * 元数据在数据 memcpy 完成之后、置位 ctrl_reg 之前下发。
     */
    wmb();

    nodeid = ip_to_nodeid(msg->ip);
    if (nodeid == NODEID_INVALID) {
        pr_warn_ratelimited("unsupported dst ip=%pI4\n", &msg->ip);//nodeid没转换成功就返回错误
        return -EINVAL;
    }

    /* 写入元数据 */
    writel(msg->ip, tx_ip_addr);
    wmb();
    writel(nodeid, tx_node_id);
    wmb();
    writel(msg->len, tx_len);
    wmb();
    writel(msg->data_type, tx_type);
    wmb();

    /* 数据写完后再置位bit0，通知CPU1读取业务区 */
    set_udp_data_enable();
    mb();
    return 0;
}

/****************************/
/* 网管软件下发的控制数据写入 */
/****************************/
int process_ctrl_data(struct amp_ctrl_msg *msg)
{
    int ret;

    if (msg->data_type != 1) {
        pr_warn_ratelimited("unsupported control data_type=%u\n", msg->data_type);
        return -EINVAL;
    }

    /* 控制面同样复用 TX 单槽，写前也要等 CPU1 释放 */
    ret = wait_for_tx_slot_idle();
    if (ret)
        return ret;

    if (msg->len > 0)
        memcpy_toio(tx_ctrl_data_addr, msg->data, msg->len);

    /* 控制区同样由 ioremap_nocache() 映射，non-cacheable，
     * 只需 wmb() 保证 memcpy/writel 顺序。 */
    wmb();

    writel(msg->len, tx_ctrl_len);
    wmb();
    writel(msg->data_type, tx_ctrl_type);
    wmb();

    /* 控制区准备完成后再置位bit1 */
    set_ctrl_data_enable();
    mb();
    return 0;
}



/*****************************/
/* 软中断处理函数：CPU1通知CPU0 */
/*****************************/
void cpu1_to_cpu0_handler(int ipinr, void *dev_id)
{
    u8 rx_mode8;
    u32 rx_mode32;
        u32 len;
        u32 node_id;
        u32 ip;
        u32 data_type;

    (void)ipinr;
    (void)dev_id;

    if (!rx_len || !rx_node_id || !rx_ip_addr || !rx_data_addr ||
        !rx_type || !rx_ctrl_len || !rx_ctrl_type ||
        !rx_ctrl_data_addr || !rx_ctrl_reg) {
        return;
    }

    rx_mode8 = readb(rx_ctrl_reg);       //先读 0x39005000，如果 =0x01 就搬业务上报；如果 =0x02 就搬控制上报；读不出来默认业务上报
    rmb();
    rx_mode32 = readl(rx_ctrl_reg); 
    rmb();
    pr_info_ratelimited("AMP RX: sgi=%d rx_ctrl_reg readb=0x%02x readl=0x%08x\n",ipinr, rx_mode8, rx_mode32);

    if (rx_mode8 & 0x01) {
        /* 业务 RX：CPU1 把业务数据放在 0x39001000 业务 RX 区 */
        len = readl(rx_len);
        rmb();
        data_type = readl(rx_type);
        rmb();
        node_id = readl(rx_node_id);
        rmb();
        ip = readl(rx_ip_addr);
        rmb();

        if (len > MAX_PAYLOAD_SIZE) {
            pr_warn_ratelimited("RX len too large, dropping: %u\n", len);
        } else {
            if (data_type == 0)
                data_type = 1;
            enqueue_business_rx_msg(ip, node_id, data_type, len);
        }

        clear_rx_enable_bit();
    } else if (rx_mode8 & 0x02) {
        /* 控制 RX：CPU1 把控制数据放在 0x39003000 控制 RX 区 */
        len = readl(rx_ctrl_len);
        rmb();
        data_type = readl(rx_ctrl_type);
        rmb();

        if (len > MAX_PAYLOAD_SIZE) {
            pr_warn_ratelimited("CTRL RX len too large, dropping: %u\n", len);
        } else {
            if (data_type == 0)
                data_type = 1;
            enqueue_ctrl_rx_msg(data_type, len);
        }

        clear_rx_enable_bit();
    } else if (!(rx_mode8 & 0x03)) {
        /* 兜底：rx_ctrl_reg 两位都未置位时按业务 RX 处理，兼容 CPU1 未置位的场景 */
        len = readl(rx_len);
        rmb();
        data_type = readl(rx_type);
        rmb();
        node_id = readl(rx_node_id);
        rmb();
        ip = readl(rx_ip_addr);
        rmb();

        if (len > MAX_PAYLOAD_SIZE) {
            pr_warn_ratelimited("RX len too large, dropping: %u\n", len);
        } else {
            if (data_type == 0)
                data_type = 1;
            enqueue_business_rx_msg(ip, node_id, data_type, len);
        }

        clear_rx_enable_bit();
    }
}
