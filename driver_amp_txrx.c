#include <linux/module.h>
#include <linux/byteorder/generic.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/string.h>
#include <asm/cacheflush.h>
#include <asm/barrier.h>
#include <linux/fs.h>

#include "driver_amp_hw.h"

u32 ip_to_nodeid(__be32 ip_be)
{
    u32 ip = ntohl(ip_be);
    u8 b0 = (ip >> 24) & 0xff;
    u8 b1 = (ip >> 16) & 0xff;
    u8 b2 = (ip >> 8) & 0xff;
    u8 b3 = ip & 0xff;

    if (b0 == 239 && b1 == 0 && b2 == 0 && b3 == 1)
        return 254;
    if (b0 == 192 && b1 == 168 && b2 == 1 && b3 == 255)
        return 255;
    if (b0 == 192 && b1 == 168 && b2 == 1 && b3 >= 10 && b3 <= 25)
        return (u32)(b3 - 10);

    return 0;
}

u32 nodeid_to_ip(u32 node_id)
{
    u8 ip[4] = {192, 168, 1, 0};

    if (node_id <= 15) {
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

static void set_udp_data_enable(void)
{
    u8 reg_val = readb(ctrl_reg);

    reg_val = (reg_val & ~0x02) | 0x01;
    writeb(reg_val, ctrl_reg);
    wmb();
}

static void set_control_data_enable(void)
{
    u8 reg_val = readb(ctrl_reg);

    reg_val = (reg_val & ~0x01) | 0x02;
    writeb(reg_val, ctrl_reg);
    wmb();
}

int process_control_data(struct amp_net_msg *msg)
{
    struct control_frame *ctrl_frame;
    u16 frame_header;
    u16 frame_tail;

    if (msg->len != sizeof(struct control_frame))
        return -EINVAL;

    ctrl_frame = (struct control_frame *)msg->data;
    frame_header = be16_to_cpu(ctrl_frame->frame_header);
    frame_tail = be16_to_cpu(ctrl_frame->frame_tail);
    if (frame_header != 0xF00F || frame_tail != 0xE00E)
        return -EINVAL;

    msg->node_id = ctrl_frame->dst_addr;
    set_control_data_enable();

    writel(be32_to_cpu(ctrl_frame->test_freq), tx_test_freq);
    writel(be32_to_cpu(ctrl_frame->test_enable), tx_test_enable);
    writel(be32_to_cpu(ctrl_frame->fixed_freq), tx_fixed_freq);
    writel(be32_to_cpu(ctrl_frame->net_test), tx_net_test);
    writel(be32_to_cpu(ctrl_frame->loopback), tx_loopback);
    writel(be32_to_cpu(ctrl_frame->iq_swap), tx_iq_swap);
    writel(be32_to_cpu(ctrl_frame->attenuation), tx_atten);

    wmb();
    dsb(sy);
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

int process_udp_data(struct amp_net_msg *msg)
{
    u32 nodeid;

    set_udp_data_enable();
    mb();

    if (msg->len > 0)
        memcpy_toio(tx_data_addr, msg->data, msg->len);

    if (tx_data_addr) {
        dsb(sy);
        __cpuc_flush_dcache_area(tx_data_addr, msg->len);
    }

    wmb();
    writel(msg->ip, tx_ip_addr);
    nodeid = ip_to_nodeid(msg->ip);
    writel(nodeid, tx_node_id);
    wmb();
    writel(msg->len, tx_len);
    wmb();
    return 0;
}

void cpu1_to_cpu0_handler(int ipinr, void *dev_id)
{
    u32 len;
    u32 node_id;
    u32 ip;

    if (atomic_cmpxchg(&rx_pending, 0, 1) != 0) {
        pr_warn("RX already pending, skipping\n");
        return;
    }

    if (!rx_len || !rx_node_id || !rx_ip_addr || !rx_data_addr) {
        atomic_set(&rx_pending, 0);
        return;
    }

    len = readl(rx_len);
    node_id = readl(rx_node_id);
    ip = readl(rx_ip_addr);
    rmb();

    if (len > MAX_PAYLOAD_SIZE) {
        atomic_set(&rx_pending, 0);
        return;
    }

    memset(&rx_msg, 0, sizeof(rx_msg));
    rx_msg.data_type = 0;
    rx_msg.ip = ip;
    rx_msg.node_id = node_id;
    rx_msg.len = len;
    if (len > 0)
        memcpy_fromio(rx_msg.data, rx_data_addr, len);

    rx_msg_bytes = offsetof(struct amp_net_msg, data) + len;
    wake_up_interruptible(&rx_wq);
}
