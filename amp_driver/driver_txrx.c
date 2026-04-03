/****************************************************/
/*业务数据/控制数据发送处理、RX 中断处理、IP 与节点映射*/
/****************************************************/
#include <linux/module.h>
#include <linux/byteorder/generic.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/string.h>
#include <asm/cacheflush.h>
#include <asm/barrier.h>
#include <linux/fs.h>

#include "driver_hardware.h"

/************************
*******IP-节点对应表*******
************************/
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
    /* 单播：192.168.1.10 ~ 192.168.1.42 -> 0 ~ 33 */
    if (b0 == 192 && b1 == 168 && b2 == 1 && b3 >= 10 && b3 <= 42)
        return (u32)(b3 - 10);  // 对应 node_id <= 33

    return 0;
}
u32 nodeid_to_ip(u32 node_id)
{
    u8 ip[4] = {192, 168, 1, 0};

    if (node_id <= 32) {
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


/*****************************************/
/* 控制寄存器操作-按照协议：写入共享内存后置1 */
/*****************************************/
static void set_udp_data_enable(void)
{
    u8 reg_val = readb(ctrl_reg);
    /* bit0=UDP, bit1=CTRL: 设置UDP时要清掉CTRL位，避免两位同时为1 */
    reg_val = (reg_val & ~0x02) | 0x01;
    writeb(reg_val, ctrl_reg);
    wmb();
}
static void set_control_data_enable(void)
{
    u8 reg_val = readb(ctrl_reg);
    /* bit0=UDP, bit1=CTRL: 设置CTRL时要清掉UDP位 */
    reg_val = (reg_val & ~0x01) | 0x02;
    writeb(reg_val, ctrl_reg);
    wmb();
}


/**************/
/* 处理控制数据 */
/**************/
int process_control_data(struct amp_net_msg *msg)
{
    struct control_frame *ctrl_frame;
    u16 frame_header;
    u16 frame_tail;

    if (msg->len != sizeof(struct control_frame))
        return -EINVAL;
    ctrl_frame = (struct control_frame *)msg->data;
    /* 检查帧头和帧尾 */
    frame_header = be16_to_cpu(ctrl_frame->frame_header);
    frame_tail = be16_to_cpu(ctrl_frame->frame_tail);
    if (frame_header != 0xF00F || frame_tail != 0xE00E)
        return -EINVAL;

    /* 提取目的地址作为节点号 */
    msg->node_id = ctrl_frame->dst_addr;
    /* 设置控制寄存器为控制数据模式 */
    set_control_data_enable();

    /* 写入控制参数到对应寄存器（注意字节序转换） */
    writel(be32_to_cpu(ctrl_frame->dst_addr), tx_node_id);          // 测试信号频率
    writel(be32_to_cpu(ctrl_frame->test_freq), tx_test_freq);       // 测试信号频率
    writel(be32_to_cpu(ctrl_frame->test_enable), tx_test_enable);   // 测试信号使能
    writel(be32_to_cpu(ctrl_frame->fixed_freq), tx_fixed_freq);     // 定频频率
    writel(be32_to_cpu(ctrl_frame->net_test), tx_net_test);         // 组网数据发送测试
    writel(be32_to_cpu(ctrl_frame->loopback), tx_loopback);         // 数据自回环
    writel(be32_to_cpu(ctrl_frame->iq_swap), tx_iq_swap);           // 接收基带IQ对调
    writel(be32_to_cpu(ctrl_frame->attenuation), tx_atten);         // 发射衰减系数

    wmb();  // 写内存屏障
    // 关键：写入完成后刷新缓存
    // 方法1：使用dsb指令确保写操作完成
    dsb(sy);
    // 方法2：刷新特定的缓存行
    __cpuc_flush_dcache_area(tx_test_freq, sizeof(struct control_frame));

    pr_info("Control data sent: seq=%u, tFreq=%u, tEn=%u, freq=%u, netT=%u, loopback=%u, iq_swap=%u, atten=%u\n",
            be32_to_cpu(ctrl_frame->frame_seq),
            be32_to_cpu(ctrl_frame->test_freq),
            be32_to_cpu(ctrl_frame->test_enable),
            be32_to_cpu(ctrl_frame->fixed_freq),
            be32_to_cpu(ctrl_frame->net_test),
            be32_to_cpu(ctrl_frame->loopback),
            be32_to_cpu(ctrl_frame->iq_swap),
            be32_to_cpu(ctrl_frame->attenuation));
    return 0;
}


/**************/
/* 处理UDP数据 */
/**************/
int process_udp_data(struct amp_net_msg *msg)
{
    u32 nodeid;

    /* 设置控制寄存器为UDP数据模式 */
    set_udp_data_enable();
    mb();  // 内存屏障，确保之前的写操作完成
    /* 根据数据类型处理 */
    /* 写入UDP数据到数据区 */

    if (msg->len > 0)
        memcpy_toio(tx_data_addr, msg->data, msg->len);

    // 关键：写入完成后刷新缓存
    if (tx_data_addr) {
        // 方法1：使用dsb指令确保写操作完成
        dsb(sy);
        // 方法2：刷新特定的缓存行
        __cpuc_flush_dcache_area(tx_data_addr, msg->len);
    }

    // 再次添加内存屏障
    wmb();  // 写内存屏障
    /* 写入元数据 */
    writel(msg->ip, tx_ip_addr);
    nodeid = ip_to_nodeid(msg->ip);
    writel(nodeid, tx_node_id);
    wmb();
    writel(msg->len, tx_len);
    wmb();
    return 0;
}



/*****************************/
/* 软中断处理函数：CPU1通知CPU0 */
/*****************************/
void cpu1_to_cpu0_handler(int ipinr, void *dev_id)
{
    u32 len;
    u32 node_id;
    u32 ip;

    /* 防重入保护 */
    if (atomic_cmpxchg(&rx_pending, 0, 1) != 0) {
        pr_warn("RX already pending, skipping\n");
        return;
    }

    if (!rx_len || !rx_node_id || !rx_ip_addr || !rx_data_addr) {
        atomic_set(&rx_pending, 0);
        return;
    }

    /* 读取元信息 */
    len = readl(rx_len);
    node_id = readl(rx_node_id);
    ip = readl(rx_ip_addr);
    rmb();

    if (len > MAX_PAYLOAD_SIZE) {
        atomic_set(&rx_pending, 0);
        return;
    }

    /* 组装一条发给用户态的消息（read()取走后再清 pending） */
    memset(&rx_msg, 0, sizeof(rx_msg));
    rx_msg.data_type = 0; /* 目前RX侧只回传数据类（业务/隧道IP包） */
    rx_msg.ip = ip;
    rx_msg.node_id = node_id;
    rx_msg.len = len;
    if (len > 0)
        memcpy_fromio(rx_msg.data, rx_data_addr, len);

    rx_msg_bytes = offsetof(struct amp_net_msg, data) + len;
    wake_up_interruptible(&rx_wq);
}
