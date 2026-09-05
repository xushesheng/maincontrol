/******************************************/
/*业务数据发送处理、RX 中断处理、IP 与节点映射*/
/******************************************/
#include <linux/module.h>             /* 内核模块框架 */
#include <linux/byteorder/generic.h>   /* 字节序转换（ntohl/htonl） */
#include <linux/uaccess.h>             /* 用户态内存访问 */
#include <linux/io.h>                  /* IO 内存读写（readb/writeb/writel/memcpy_fromio/memcpy_toio） */
#include <linux/string.h>              /* memset/memcpy */
#include <linux/delay.h>               /* udelay 微秒延时 */
#include <linux/sched.h>               /* 进程调度（signal_pending） */
#include <linux/sched/signal.h>        /* 信号相关 */
#include <asm/barrier.h>               /* 内存屏障（wmb/mb/rmb） */
#include <linux/fs.h>                  /* 文件系统相关 */

#include "driver_hardware.h"           /* 硬件地址、全局变量与函数声明 */

#define NODEID_INVALID U32_MAX         /* 无效节点号标记（= 0xFFFFFFFF），表示 IP 映射失败 */
/************************
*******IP-节点对应表*******
************************/
/* IP -> 节点号映射（协议A：上位机 IP = 192.168.1.(10 + node_id)，node 0~31）
 * 组播 239.0.0.1 -> 节点 254，广播 192.168.1.255 -> 节点 255，其余视为非法 */
u32 ip_to_nodeid(__be32 ip_be)
{
    u32 ip = ntohl(ip_be);          /* 转成主机字节序，便于拆字节（大端网络序 -> 小端主机序） */
    u8 b0 = (ip >> 24) & 0xff;      /* 提取 IP 第1字节（如 192） */
    u8 b1 = (ip >> 16) & 0xff;      /* 提取 IP 第2字节（如 168） */
    u8 b2 = (ip >> 8) & 0xff;       /* 提取 IP 第3字节（如 1） */
    u8 b3 = ip & 0xff;              /* 提取 IP 第4字节（如 10~41 / 255） */

    /* 组播：239.0.0.1 -> 254 */
    if (b0 == 239 && b1 == 0 && b2 == 0 && b3 == 1)
        return 254;
    /* 广播：192.168.1.255 -> 255 */
    if (b0 == 192 && b1 == 168 && b2 == 1 && b3 == 255)
        return 255;
    /* 单播：192.168.1.10 ~ 192.168.1.41 -> 0 ~ 31 */
    if (b0 == 192 && b1 == 168 && b2 == 1 && b3 >= 10 && b3 <= 41)
        return (u32)(b3 - 10);      /* 对应 node_id <= 31 */

    return NODEID_INVALID;          /* 无法映射的 IP，返回 U32_MAX 表示非法 */
}
/************************
* 把节点号转成业务目标IP *
* 与 ip_to_nodeid 互逆   *
************************/
u32 nodeid_to_ip(u32 node_id)
{
    u8 ip[4] = {192, 168, 1, 0};    /* 初始化为基础网段 192.168.1.0 */

    if (node_id <= 31) {
        ip[3] = 10 + node_id;       /* 节点 0~31 -> 192.168.1.10 ~ 192.168.1.41 */
        return htonl(*(u32 *)ip);   /* 转成网络字节序返回 */
    }

    if (node_id == 254) {
        ip[0] = 239;                /* 组播节点 -> 239.0.0.1 */
        ip[1] = 0;
        ip[2] = 0;
        ip[3] = 1;
        return htonl(*(u32 *)ip);
    }

    if (node_id == 255) {
        ip[3] = 255;                /* 广播节点 -> 192.168.1.255 */
        return htonl(*(u32 *)ip);
    }

    return 0;   /* 未知节点号，返回 0 */
}


/*************************************/
/* 数据模式寄存器操作：写入业务数据置位 */
/* 操作 ctrl_reg (0x38005000)：bit0=业务待读, bit1=控制待读 */
/*************************************/
static void set_udp_data_enable(void)
{
    u8 reg_val = readb(ctrl_reg);       /* 读取当前寄存器值 */

    (void)reg_val;                      /* 抑制"未使用"编译警告（readb 有 volatile 副作用） */
    /* bit0=UDP, bit1=CTRL: 设置 UDP 位时要清掉 CTRL 位，避免两位同时为 1 */
    reg_val = (reg_val & ~0x02) | 0x01; /* 清除 bit1，置位 bit0 */
    writeb(reg_val, ctrl_reg);          /* 写回寄存器 */
    wmb();                              /* 写内存屏障：确保 writeb 在后续操作之前完成 */
}

/*************************************/
/* 数据模式寄存器操作：写入控制数据置位 */
/*************************************/
static void set_ctrl_data_enable(void)
{
    u8 reg_val = readb(ctrl_reg);       /* 读取当前寄存器值 */

    (void)reg_val;
    /* bit0=UDP, bit1=CTRL: 设置 CTRL 位时要清掉 UDP 位 */
    reg_val = (reg_val & ~0x01) | 0x02; /* 清除 bit0，置位 bit1 */
    writeb(reg_val, ctrl_reg);          /* 写回寄存器 */
    wmb();                              /* 写屏障：确保寄存器写入完成 */
}

/*******************************************/
/* 数据模式寄存器操作：读取业务/控制数据清零 */
/* 操作 rx_ctrl_reg (0x39005000)：CPU0 读完 CPU1 的数据后，清零通知 CPU1 槽已释放 */
/******************************************/
static void clear_rx_enable_bit(void)
{
    writeb(0x00, rx_ctrl_reg);          /* 将 RX 数据模式寄存器清零（bit0 和 bit1 都清） */
    wmb();                              /* 写屏障：确保清零在后续操作前完成 */
}

/****************************************/
/* 把CPU1来的业务数据放入驱动侧环形缓冲区 */
/* 在 SGI14 软中断上下文中调用              */
/****************************************/
static int enqueue_business_rx_msg(u32 ip, u32 node_id, u32 data_type, u32 len)
{
    unsigned long flags;                    /* 保存中断状态的 flags */
    struct amp_rx_slot *slot;               /* 指向当前队尾槽位 */

    spin_lock_irqsave(&rx_ring_lock, flags);    /* 获取自旋锁的同时保存当前 CPU 的中断状态 */
    if (atomic_read(&rx_ring_count) >= RX_RING_SIZE) {     /* 队列已满 */
        atomic_inc(&rx_drop_full);          /* 满丢包计数加1 */
        spin_unlock_irqrestore(&rx_ring_lock, flags);
        amp_pr_warn("RX ring full, dropping packet len=%u node=%u\n", len, node_id);
        return -ENOSPC;                     /* 返回"无空间"错误 */
    }

    slot = &rx_ring[rx_ring_tail];          /* 取队尾槽位指针 */
    memset(slot, 0, sizeof(*slot));         /* 清空槽位 */
    slot->msg.data_type = (u8)data_type;    /* 填充数据类型 */
    slot->msg.ip = ip;                      /* 填充来源 IP */
    slot->msg.node_id = node_id;            /* 填充来源节点号 */
    slot->msg.len = len;                    /* 填充载荷长度 */
    if (len > 0)
        memcpy_fromio(slot->msg.data, rx_data_addr, len);   /* 从 IO 内存拷贝载荷数据 */
    slot->msg_bytes = offsetof(struct amp_net_msg, data) + len;  /* 计算完整消息字节数 */

    rx_ring_tail = (rx_ring_tail + 1U) % RX_RING_SIZE;   /* 环形推进队尾索引 */
    atomic_inc(&rx_ring_count);             /* 待读数加1 */
    atomic_inc(&rx_enqueued);               /* 入队计数加1 */
    spin_unlock_irqrestore(&rx_ring_lock, flags);     /* 释放自旋锁（恢复中断状态） */
    wake_up_interruptible(&rx_wq);          /* 唤醒阻塞在 rx_wq 上的 amp_read() */
    return 0; 
}

/****************************************/
/* 把CPU1来的控制数据放入驱动侧环形缓冲区 */
/* 流程与 enqueue_business_rx_msg 一致    */
/****************************************/
static int enqueue_ctrl_rx_msg(u32 data_type, u32 len)
{
    unsigned long flags;                    /* 中断状态 flags */
    struct amp_ctrl_rx_slot *slot;          /* 队尾槽位 */

    spin_lock_irqsave(&ctrl_rx_ring_lock, flags);   /* 获取控制队列自旋锁 */
    if (atomic_read(&ctrl_rx_ring_count) >= RX_RING_SIZE) {   /* 控制队列已满 */
        atomic_inc(&ctrl_rx_drop_full);     /* 控制满丢包计数加1 */
        spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);
        amp_pr_warn("CTRL RX ring full, dropping packet len=%u type=%u\n", len, data_type);
        return -ENOSPC;
    }

    slot = &ctrl_rx_ring[ctrl_rx_ring_tail];    /* 取队尾槽位 */
    memset(slot, 0, sizeof(*slot));             /* 清空 */
    slot->msg.data_type = (u8)data_type;        /* 数据类型 */
    slot->msg.len = len;                        /* 载荷长度 */
    if (len > 0)
        memcpy_fromio(slot->msg.data, rx_ctrl_data_addr, len);   /* 从控制 RX IO 内存拷贝 */
    slot->msg_bytes = offsetof(struct amp_ctrl_msg, data) + len;  /* 完整字节数 */

    ctrl_rx_ring_tail = (ctrl_rx_ring_tail + 1U) % RX_RING_SIZE; /* 推进队尾 */
    atomic_inc(&ctrl_rx_ring_count);            /* 待读数加1 */
    atomic_inc(&ctrl_rx_enqueued);              /* 入队计数加1 */
    spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);
    wake_up_interruptible(&ctrl_rx_wq);         /* 唤醒阻塞在 ctrl_rx_wq 上的 amp_ctrl_read() */
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
    unsigned int waited = 0;    /* 累计等待时间（微秒） */

    /* 循环轮询 ctrl_reg，直到 bit0 和 bit1 都被 CPU1 清零 */
    while (readb(ctrl_reg) & CTRL_REG_TX_MASK) {
        /* 若当前进程收到信号，立即退出，避免长时间阻塞 */
        if (signal_pending(current))
            return -ERESTARTSYS;

        if (waited >= AMP_TX_POLL_TIMEOUT_US) {     /* 超过 5ms 仍未释放 */
            atomic_inc(&tx_busy_timeout);            /* 超时计数加1 */
            amp_pr_warn("AMP TX slot busy timeout (ctrl_reg=0x%02x)\n",
                                readb(ctrl_reg));
            return -EBUSY;
        }

        udelay(AMP_TX_POLL_STEP_US);                /* 等待 100us */
        waited += AMP_TX_POLL_STEP_US;              /* 累加已等待时间 */
    }

    return 0;   /* TX 槽空闲 */
}


/****************************/
/* 语音或其他业务数据写入进程 */
/* 在 amp_write() 的进程上下文中调用 */
/****************************/
int process_udp_data(struct amp_net_msg *msg)
{
    u32 nodeid;     /* IP 映射出的节点号 */
    int ret;

    /* 只允许 data_type=1（业务）或 2（语音） */
    if (msg->data_type != 1 && msg->data_type != 2) {
        amp_pr_warn("unsupported business data_type=%u\n", msg->data_type);
        return -EINVAL;
    }

    /*
     * 方案 A：写 TX 单槽前，等待 CPU1 把上一帧读走并清掉 ctrl_reg。
     * 若超时说明 CPU1 侧处理过慢，本次写入失败，避免覆盖上一帧。
     */
    ret = wait_for_tx_slot_idle();
    if (ret)
        return ret;

    /* 将载荷数据写入 TX 业务数据共享内存区（0x38001000） */
    if (msg->len > 0)
        memcpy_toio(tx_data_addr, msg->data, msg->len);

    /*
     * tx_data_addr 由 ioremap_nocache() 映射，已是 non-cacheable，
     * 不需要 dsb + __cpuc_flush_dcache_area()。wmb() 保证后续 writel
     * 元数据在数据 memcpy 完成之后、置位 ctrl_reg 之前下发。
     */
    wmb();

    /* IP -> 节点号映射 */
    nodeid = ip_to_nodeid(msg->ip);
    if (nodeid == NODEID_INVALID) {
        amp_pr_warn("unsupported dst ip=%pI4\n", &msg->ip);    /* nodeid 没转换成功就返回错误 */
        return -EINVAL;
    }

    /* 写入元数据到 TX 寄存器区（0x38000000 ~ 0x3800000C） */
    writel(msg->ip, tx_ip_addr);        /* 目标 IP */
    wmb();                              /* 写屏障：确保 IP 先于后续字段到达 */
    writel(nodeid, tx_node_id);         /* 目标节点号 */
    wmb();
    writel(msg->len, tx_len);           /* 载荷长度 */
    wmb();
    writel(msg->data_type, tx_type);    /* 数据类型 */
    wmb();

    /* 数据写完后再置位 bit0，通知 CPU1 读取业务区 */
    set_udp_data_enable();
    mb();   /* 全内存屏障：确保所有写操作在置位 ctrl_reg 前完成 */
    return 0;
}

/****************************/
/* 网管软件下发的控制数据写入 */
/* 在 amp_ctrl_write() 的进程上下文中调用 */
/****************************/
int process_ctrl_data(struct amp_ctrl_msg *msg)
{
    int ret;

    /* 控制数据目前只支持 data_type=1 */
    if (msg->data_type != 1) {
        amp_pr_warn("unsupported control data_type=%u\n", msg->data_type);
        return -EINVAL;
    }

    /* 控制面同样复用 TX 单槽，写前也要等 CPU1 释放 */
    ret = wait_for_tx_slot_idle();
    if (ret)
        return ret;

    /* 将控制帧写入 TX 控制数据共享内存区（0x38003000） */
    if (msg->len > 0)
        memcpy_toio(tx_ctrl_data_addr, msg->data, msg->len);

    /* 控制区同样由 ioremap_nocache() 映射，non-cacheable，
     * 只需 wmb() 保证 memcpy/writel 顺序。 */
    wmb();

    /* 写入控制元数据到 TX 控制寄存器区（0x38002008 ~ 0x3800200C） */
    writel(msg->len, tx_ctrl_len);          /* 控制帧载荷长度 */
    wmb();
    writel(msg->data_type, tx_ctrl_type);   /* 控制数据类型 */
    wmb();

    /* 控制区准备完成后再置位 bit1，通知 CPU1 读取控制数据 */
    set_ctrl_data_enable();
    mb();   /* 全内存屏障 */
    return 0;
}



/*****************************/
/* 软中断处理函数：CPU1通知CPU0 */
/* 触发条件：CPU1 通过 SGI14 中断通知 CPU0，表示 RX 共享内存中有数据待读取 */
/*****************************/
void cpu1_to_cpu0_handler(int ipinr, void *dev_id)
{
    u8 rx_mode8;        /* rx_ctrl_reg 按字节读取的值 */
    u32 rx_mode32;      /* rx_ctrl_reg 按 32 位读取的值（调试用） */
    u32 len;            /* 载荷长度 */
    u32 node_id;        /* 来源节点号（仅业务） */
    u32 ip;             /* 来源 IP 地址（仅业务） */
    u32 data_type;      /* 数据类型 */

    (void)ipinr;        /* 抑制未使用警告 */
    (void)dev_id;
    
    /* 中断接收检查：验证驱动程序能否响应到对方中断 */
    amp_pr_info(" AMP RX: sgi=%d \n",AMP_SGI_RX); //接收到的IPI中断14

    /* 安全检查：所有 RX 相关 IO 指针必须已映射 */
    if (!rx_len || !rx_node_id || !rx_ip_addr || !rx_data_addr ||
        !rx_type || !rx_ctrl_len || !rx_ctrl_type ||
        !rx_ctrl_data_addr || !rx_ctrl_reg) {
        return;    /* 映射未完成，直接返回（可能是 probe 早期触发了残留中断） */
    }

    rx_mode8 = readb(rx_ctrl_reg);       /* 先读 0x39005000：如果 =0x01 就搬业务上报；=0x02 就搬控制上报；=0 兜底按业务处理 */
    rmb();                               /* 读屏障：确保 readb 完成后才能信赖 rx_mode8 */
    amp_pr_info("AMP RX: rx_ctrl_reg readb=0x%02x\n", rx_mode8);

    if (rx_mode8 & 0x01) {
        /* 业务 RX：CPU1 把业务数据放在 0x39001000 业务 RX 区，bit0=1 表示业务数据待读 */
        len = readl(rx_len);            /* 读取载荷长度 */
        rmb();                          /* 读屏障 */
        data_type = readl(rx_type);     /* 读取数据类型 */
        rmb();
        node_id = readl(rx_node_id);    /* 读取来源节点号 */
        rmb();
        ip = readl(rx_ip_addr);         /* 读取来源 IP */
        rmb();

        if (len > MAX_PAYLOAD_SIZE) {
            amp_pr_warn("RX len too large, dropping: %u\n", len);
        } else {
            if (data_type == 0)
                data_type = 1;          /* 修正非法 data_type */
            enqueue_business_rx_msg(ip, node_id, data_type, len);  /* 入队业务消息 */
        }

        clear_rx_enable_bit();          /* 读完清零，通知 CPU1 释放 RX 槽 */
    } else if (rx_mode8 & 0x02) {
        /* 控制 RX：CPU1 把控制数据放在 0x39003000 控制 RX 区，bit1=1 表示控制数据待读 */
        len = readl(rx_ctrl_len);       /* 读取控制载荷长度 */
        rmb();
        data_type = readl(rx_ctrl_type);/* 读取控制数据类型 */
        rmb();

        if (len > MAX_PAYLOAD_SIZE) {
            amp_pr_warn("CTRL RX len too large, dropping: %u\n", len);
        } else {
            if (data_type == 0)
                data_type = 1;
            enqueue_ctrl_rx_msg(data_type, len);    /* 入队控制消息 */
        }

        clear_rx_enable_bit();          /* 读完清零 */
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
            amp_pr_warn("RX len too large, dropping: %u\n", len);
        } else {
            if (data_type == 0)
                data_type = 1;
            enqueue_business_rx_msg(ip, node_id, data_type, len);
        }

        clear_rx_enable_bit();          /* 读完清零 */
    }
}
