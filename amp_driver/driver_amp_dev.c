/*************************************/
/*   本文件存放给用户的写入和读取函数   */
/************************************/
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/irqchip/arm-gic.h>

#include "driver_amp_hw.h"

/****************/
/* 用户态写入接口 */
/****************/
ssize_t amp_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
    struct amp_net_msg msg;
    int ret;

    /* 检查最小长度 */
    if (len < offsetof(struct amp_net_msg, data))
        return -EINVAL;

    /* 复制消息头部 */
    if (copy_from_user(&msg, buf, offsetof(struct amp_net_msg, data)))
        return -EFAULT;

    /* 检查数据长度是否超出 */
    if (msg.len > MAX_PAYLOAD_SIZE)
        return -EINVAL;

    /* 复制数据部分 */
    if (msg.len > 0) {
        if (copy_from_user(msg.data, buf + offsetof(struct amp_net_msg, data), msg.len))
            return -EFAULT;
    }

    /* 检查共享内存映射 */
    if (!driver_amp_resources_ready() || !ctrl_reg)
        return -ENODEV;

    if (msg.data_type == 0) {
        ret = process_udp_data(&msg);
    } else if (msg.data_type == 1) {
        ret = process_control_data(&msg);
    } else {
        return -EINVAL;
    }

    if (ret)
        return ret;

    pr_info("TX: ip=%pI4 len=%u ctrl=%u target=%u sgi=%u\n",
            &msg.ip, msg.len, readl(ctrl_reg), 1, AMP_SGI_TX);

    /* 触发中断通知CPU1 */
    gic_raise_softirq_fmsh(1, AMP_SGI_TX);
    return offsetof(struct amp_net_msg, data) + msg.len;
}


/***********************************/
/* 用户态读取接口：读取CPU1回来的数据包 */
/***********************************/
ssize_t amp_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
    ssize_t ret;

    /* 阻塞等待：直到有数据包到来 */
    if (!(file->f_flags & O_NONBLOCK)) {
        ret = wait_event_interruptible(rx_wq, atomic_read(&rx_pending) != 0);
        if (ret)
            return ret;
    } else if (atomic_read(&rx_pending) == 0) {
        return -EAGAIN;
    }

    if (len < rx_msg_bytes)
        return -EINVAL;

    if (copy_to_user(buf, &rx_msg, rx_msg_bytes))
        return -EFAULT;

    atomic_set(&rx_pending, 0);
    return rx_msg_bytes;
}

/****************/
/* 文件操作结构体 */
/****************/
static const struct file_operations amp_fops = {
    .owner = THIS_MODULE,
    .write = amp_write,
    .read = amp_read,
};


/*****************/
/* Misc设备结构体 */
/****************/
struct miscdevice amp_miscdev = { //在主函数里注册和注销
    .minor = MISC_DYNAMIC_MINOR,
    .name = "amp_ipi",
    .fops = &amp_fops,
};
