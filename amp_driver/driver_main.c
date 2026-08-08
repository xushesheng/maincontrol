/************************************/
/* 本文件存放驱动主函数（不可轻易修改） */
/************************************/
/*************************************/
/*   本文件存放给用户的写入和读取接口   */
/************************************/
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/irqchip/arm-gic.h>
#include <asm/smp.h>
#include <linux/io.h>

#include "driver_hardware.h"

/* 发送路径共用同一把锁，避免业务/控制并发置位共享TX控制寄存器时发生竞争 */
static DEFINE_MUTEX(amp_tx_lock);
static DEFINE_MUTEX(amp_read_lock);
static DEFINE_MUTEX(amp_ctrl_read_lock);

/*************************/
/* 用户态业务数据写入接口 */
/*************************/
ssize_t amp_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
    struct amp_net_msg *msg;
    int ret;

    msg = kzalloc(sizeof(*msg), GFP_KERNEL);
    if (!msg)
        return -ENOMEM;

    /* 检查最小长度 */
    if (len < offsetof(struct amp_net_msg, data)) {
        ret = -EINVAL;
        goto out_free;
    }

    /* 复制消息头部 */
    if (copy_from_user(msg, buf, offsetof(struct amp_net_msg, data))) {
        ret = -EFAULT;
        goto out_free;
    }

    /* 检查数据长度是否超出 */
    if (msg->len > MAX_PAYLOAD_SIZE) {
        ret = -EINVAL;
        goto out_free;
    }

    /* 严格等长校验：用户传入长度必须精确等于 header + 载荷，避免短写导致协议错位 */
    if (len != offsetof(struct amp_net_msg, data) + msg->len) {
        ret = -EINVAL;
        goto out_free;
    }

    /* 复制数据部分 */
    if (msg->len > 0) {
        if (copy_from_user(msg->data, buf + offsetof(struct amp_net_msg, data), msg->len)) {
            ret = -EFAULT;
            goto out_free;
        }
    }

    /* 检查共享内存映射 */
    if (!driver_amp_resources_ready() || !ctrl_reg) {
        ret = -ENODEV;
        goto out_free;
    }

    /* mutex_lock_interruptible是 Linux 内核 互斥锁（mutex） API 中的一种加锁 */
    if (mutex_lock_interruptible(&amp_tx_lock)) {
        ret = -ERESTARTSYS;
        goto out_free;
    }

    if (msg->data_type == 0)
        msg->data_type = 1;

    ret = process_udp_data(msg);
    if (ret)
        goto out_unlock;    //写入之后解锁

    pr_info_ratelimited("TX: ip=%pI4 len=%u target=%u sgi=%u\n",
            &msg->ip, msg->len, 1, AMP_SGI_TX);

    /* 触发中断通知CPU1 */
    smp_kick_ipi(cpumask_of(1), AMP_SGI_TX);
    ret = offsetof(struct amp_net_msg, data) + msg->len;

out_unlock:     //解锁写入互斥锁
    mutex_unlock(&amp_tx_lock);
out_free:
    kfree(msg);
    return ret;
}

/*************************/
/* 用户态控制数据写入接口 */
/*************************/
ssize_t amp_ctrl_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
    struct amp_ctrl_msg *msg;
    int ret;

    msg = kzalloc(sizeof(*msg), GFP_KERNEL);
    if (!msg)
        return -ENOMEM;

    /* 检查最小长度 */
    if (len < offsetof(struct amp_ctrl_msg, data)) {
        ret = -EINVAL;
        goto out_free;
    }

    /* 复制消息头部 */
    if (copy_from_user(msg, buf, offsetof(struct amp_ctrl_msg, data))) {
        ret = -EFAULT;
        goto out_free;
    }

    /* 检查数据长度是否超出 */
    if (msg->len > MAX_PAYLOAD_SIZE) {
        ret = -EINVAL;
        goto out_free;
    }
  
    /* 严格等长校验：用户传入长度必须精确等于 header + 载荷，避免短写导致协议错位 */
    if (len != offsetof(struct amp_ctrl_msg, data) + msg->len) {
        ret = -EINVAL;
        goto out_free;
    }
    
    /* 复制数据部分 */
    if (msg->len > 0) {
        if (copy_from_user(msg->data, buf + offsetof(struct amp_ctrl_msg, data), msg->len)) {
            ret = -EFAULT;
            goto out_free;
        }
    }

    /* 检查共享内存映射 */
    if (!driver_amp_resources_ready() || !ctrl_reg) {
        ret = -ENODEV;
        goto out_free;
    }

    /* mutex_lock_interruptible是 Linux 内核 互斥锁（mutex） API 中的一种加锁 */
    if (mutex_lock_interruptible(&amp_tx_lock)) {
        ret = -ERESTARTSYS;
        goto out_free;
    }

    if (msg->data_type == 0)
        msg->data_type = 1;

    ret = process_ctrl_data(msg);
    if (ret)
        goto out_unlock;    //写入成功之后解锁

    pr_info_ratelimited("CTRL TX: len=%u target=%u sgi=%u\n",
            msg->len, 1, AMP_SGI_TX);

    smp_kick_ipi(cpumask_of(1), AMP_SGI_TX);
    ret = offsetof(struct amp_ctrl_msg, data) + msg->len;

out_unlock:
    mutex_unlock(&amp_tx_lock);
out_free:
    kfree(msg);
    return ret;
}


/******************************************/
/* 用户态读取接口：读取CPU1回来的业务数据包 */
/******************************************/
ssize_t amp_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
    ssize_t ret;
    size_t msg_bytes;
    unsigned int head;
    unsigned long flags;

    if (mutex_lock_interruptible(&amp_read_lock))       //给读取互斥锁上锁（可被信号量中断的）
        return -ERESTARTSYS;

retry_wait:
    /* 阻塞等待：直到有数据包到来 */
    if (!(file->f_flags & O_NONBLOCK)) {
        ret = wait_event_interruptible(rx_wq, atomic_read(&rx_ring_count) != 0);    //等待条件变为真，即环内待接收数量不为0
        if (ret)
            goto out_unlock;
    } else if (atomic_read(&rx_ring_count) == 0) {
        ret = -EAGAIN;
        goto out_unlock;
    }

    /* 设备正在卸载 */
    if (atomic_read(&rx_ring_count) < 0) {
        ret = -ENODEV;
        goto out_unlock;
    }

    spin_lock_irqsave(&rx_ring_lock, flags);
    if (atomic_read(&rx_ring_count) == 0) {
        spin_unlock_irqrestore(&rx_ring_lock, flags);
        if (!(file->f_flags & O_NONBLOCK))
            goto retry_wait;
        ret = -EAGAIN;
        goto out_unlock;
    }
    head = rx_ring_head;
    msg_bytes = rx_ring[head].msg_bytes;
    spin_unlock_irqrestore(&rx_ring_lock, flags);

    if (len < msg_bytes) {
        /* 返回实际所需字节数，不丢包。调用方可用更大的 buf 重试 */
        ret = (ssize_t)msg_bytes;
        goto out_unlock;
    }

    if (copy_to_user(buf, &rx_ring[head].msg, msg_bytes)) {
        ret = -EFAULT;
        goto out_unlock;
    }

    spin_lock_irqsave(&rx_ring_lock, flags);
    if (atomic_read(&rx_ring_count) == 0 || rx_ring_head != head) {
        spin_unlock_irqrestore(&rx_ring_lock, flags);
        ret = -EIO;
        goto out_unlock;
    }
    rx_ring_head = (rx_ring_head + 1U) % RX_RING_SIZE;
    atomic_dec(&rx_ring_count);
    atomic_inc(&rx_dequeued);
    spin_unlock_irqrestore(&rx_ring_lock, flags);

    ret = msg_bytes;

out_unlock:
    mutex_unlock(&amp_read_lock);
    return ret;
}

/******************************************/
/* 用户态读取接口：读取CPU1回来的控制数据包 */
/******************************************/
ssize_t amp_ctrl_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
    ssize_t ret;
    size_t msg_bytes;
    unsigned int head;
    unsigned long flags;

    if (mutex_lock_interruptible(&amp_ctrl_read_lock))
        return -ERESTARTSYS;

retry_wait:
    if (!(file->f_flags & O_NONBLOCK)) {
        ret = wait_event_interruptible(ctrl_rx_wq, atomic_read(&ctrl_rx_ring_count) != 0);
        if (ret)
            goto out_unlock;
    } else if (atomic_read(&ctrl_rx_ring_count) == 0) {
        ret = -EAGAIN;
        goto out_unlock;
    }

    /* 设备正在卸载 */
    if (atomic_read(&ctrl_rx_ring_count) < 0) {
        ret = -ENODEV;
        goto out_unlock;
    }

    spin_lock_irqsave(&ctrl_rx_ring_lock, flags);
    if (atomic_read(&ctrl_rx_ring_count) == 0) {
        spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);
        if (!(file->f_flags & O_NONBLOCK))
            goto retry_wait;
        ret = -EAGAIN;
        goto out_unlock;
    }
    head = ctrl_rx_ring_head;
    msg_bytes = ctrl_rx_ring[head].msg_bytes;
    spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);

    if (len < msg_bytes) {
        /* 返回实际所需字节数，不丢包。调用方可用更大的 buf 重试 */
        ret = (ssize_t)msg_bytes;
        goto out_unlock;
    }

    if (copy_to_user(buf, &ctrl_rx_ring[head].msg, msg_bytes)) {
        ret = -EFAULT;
        goto out_unlock;
    }

    spin_lock_irqsave(&ctrl_rx_ring_lock, flags);
    if (atomic_read(&ctrl_rx_ring_count) == 0 || ctrl_rx_ring_head != head) {
        spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);
        ret = -EIO;
        goto out_unlock;
    }
    ctrl_rx_ring_head = (ctrl_rx_ring_head + 1U) % RX_RING_SIZE;
    atomic_dec(&ctrl_rx_ring_count);
    atomic_inc(&ctrl_rx_dequeued);
    spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);

    ret = msg_bytes;

out_unlock:
    mutex_unlock(&amp_ctrl_read_lock);
    return ret;
}

/****************/
/* 文件操作结构体 */
/****************/
static const struct file_operations amp_fops = {
    .owner = THIS_MODULE,
    .write = amp_write,
    .read = amp_read,
};

static const struct file_operations amp_ctrl_fops = {
    .owner = THIS_MODULE,
    .write = amp_ctrl_write,
    .read = amp_ctrl_read,
};


/*****************/
/* Misc设备结构体 */
/****************/
struct miscdevice amp_miscdev = { //在主函数里注册和注销
    .minor = MISC_DYNAMIC_MINOR,
    .name = "amp_ipi",
    .fops = &amp_fops,
};

struct miscdevice amp_ctrl_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "amp_ctrl",
    .fops = &amp_ctrl_fops,
};

/****************/
/* 驱动probe函数 */
/****************/
static int zynq_amp_probe(struct platform_device *pdev)
{
    int ret;
    //调用共享内存映射函数
    ret = driver_amp_map_resources();
    if (ret)
        goto error;

    /* 注册软中断处理函数 */
    ret = set_ipi_handler(AMP_SGI_RX, cpu1_to_cpu0_handler, NULL);
    if (ret)
        goto error;

    /* 注册misc设备 */
    ret = misc_register(&amp_miscdev);
    if (ret) {
        clear_ipi_handler(AMP_SGI_RX);
        goto error;
    }

    ret = misc_register(&amp_ctrl_miscdev);
    if (ret) {
        misc_deregister(&amp_miscdev);
        clear_ipi_handler(AMP_SGI_RX);
        goto error;
    }

    return 0;

error:
    //调用释放所有映射函数
    driver_amp_unmap_resources();
    return ret;
}


/*****************/
/* 驱动remove函数 */
/*****************/
static int zynq_amp_remove(struct platform_device *pdev)
{
    /* 1. 先注销中断——阻止新的 RX 数据进入 */
    clear_ipi_handler(AMP_SGI_RX);

    /* 2. 唤醒所有阻塞在 read() 上的线程 */
    atomic_set(&rx_ring_count, -1);
    atomic_set(&ctrl_rx_ring_count, -1);
    wake_up_interruptible(&rx_wq);
    wake_up_interruptible(&ctrl_rx_wq);

    /* 3. 注销 misc 设备——阻止新的 open() */
    misc_deregister(&amp_ctrl_miscdev);
    misc_deregister(&amp_miscdev);

    /* 4. 等待正在执行的 read/write 返回（用互斥锁做屏障） */
    mutex_lock(&amp_tx_lock);
    mutex_unlock(&amp_tx_lock);
    mutex_lock(&amp_read_lock);
    mutex_unlock(&amp_read_lock);
    mutex_lock(&amp_ctrl_read_lock);
    mutex_unlock(&amp_ctrl_read_lock);

    /* 5. 最后释放映射 */
    driver_amp_unmap_resources();
    return 0;
}

/***************************/
/* 设备树匹配表（不可修改） */
/**************************/
static const struct of_device_id amp_of_match[] = {
    { .compatible = "xlnx,zynq-amp" },
    { }
};
MODULE_DEVICE_TABLE(of, amp_of_match);


/*****************************/
/* 平台驱动结构体（不可修改） */
/****************************/
static struct platform_driver zynq_amp_driver = {
    .driver = {
        .name = "zynq_amp",
        .of_match_table = amp_of_match,
        .owner = THIS_MODULE,
    },
    .probe = zynq_amp_probe,
    .remove = zynq_amp_remove,
};

module_platform_driver(zynq_amp_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XuShengQiao");
MODULE_DESCRIPTION("AMP IPC Driver with Updated Protocol Support");
