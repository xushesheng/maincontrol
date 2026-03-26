#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/irqchip/arm-gic.h>

#include "driver_amp_hw.h"

ssize_t amp_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
    struct amp_net_msg msg;
    int ret;

    if (len < offsetof(struct amp_net_msg, data))
        return -EINVAL;

    if (copy_from_user(&msg, buf, offsetof(struct amp_net_msg, data)))
        return -EFAULT;

    if (msg.len > MAX_PAYLOAD_SIZE)
        return -EINVAL;

    if (msg.len > 0) {
        if (copy_from_user(msg.data, buf + offsetof(struct amp_net_msg, data), msg.len))
            return -EFAULT;
    }

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

    gic_raise_softirq_fmsh(1, AMP_SGI_TX);
    return offsetof(struct amp_net_msg, data) + msg.len;
}

ssize_t amp_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
    ssize_t ret;

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

static const struct file_operations amp_fops = {
    .owner = THIS_MODULE,
    .write = amp_write,
    .read = amp_read,
};

struct miscdevice amp_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "amp_ipi",
    .fops = &amp_fops,
};
