/************************************/
/* 本文件存放驱动主函数（不可轻易修改） */
/************************************/
/*************************************/
/*   本文件存放给用户的写入和读取函数   */
/************************************/
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/irqchip/arm-gic.h>
#include <asm/smp.h>

#include "driver_hardware.h"

/* 定义两个互斥锁（发送和接收） */
static DEFINE_MUTEX(amp_tx_lock);
static DEFINE_MUTEX(amp_read_lock);

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

    /* mutex_lock_interruptible是 Linux 内核 互斥锁（mutex） API 中的一种加锁 */
    if (mutex_lock_interruptible(&amp_tx_lock))
        return -ERESTARTSYS;

    if (msg.data_type == 0) {
        ret = process_udp_data(&msg);
    } else if (msg.data_type == 1) {
        ret = process_control_data(&msg);
    } else {
        ret = -EINVAL;
        goto out_unlock;
    }

    if (ret)
        goto out_unlock;

    pr_info("TX: ip=%pI4 len=%u ctrl=%u target=%u sgi=%u\n",
            &msg.ip, msg.len, readl(ctrl_reg), 1, AMP_SGI_TX);

    /* 触发中断通知CPU1 */
    gic_raise_softirq_fmsh(1, AMP_SGI_TX);
    ret = offsetof(struct amp_net_msg, data) + msg.len;

out_unlock:     //解锁写入互斥锁
    mutex_unlock(&amp_tx_lock);
    return ret;
}


/***********************************/
/* 用户态读取接口：读取CPU1回来的数据包 */
/***********************************/
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
        ret = -EINVAL;
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
    /* 清理软中断 */
    clear_ipi_handler(AMP_SGI_RX);
    /* 注销misc设备 */
    misc_deregister(&amp_miscdev);
    //调用释放所有映射函数
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
