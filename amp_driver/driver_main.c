/************************************/
/* 本文件存放驱动主函数（不可轻易修改） */
/************************************/
/*************************************/
/*   本文件存放给用户的写入和读取接口   */
/************************************/
#include <linux/module.h>                /* 内核模块框架（MODULE_LICENSE 等） */
#include <linux/platform_device.h>       /* 平台设备驱动模型 */
#include <linux/mutex.h>                 /* 互斥锁（mutex_lock/mutex_unlock） */
#include <linux/slab.h>                  /* 内存分配（kzalloc/kfree） */
#include <linux/uaccess.h>               /* 用户态内存访问（copy_from_user/copy_to_user） */
#include <linux/irqchip/arm-gic.h>       /* ARM GIC 中断控制器相关 */
#include <linux/io.h>                    /* IO 内存操作 */
#include <linux/err.h>                   /* 错误指针（ERR_PTR/PTR_ERR/IS_ERR） */
#include <asm/io.h>                      /* SMP 相关（smp_kick_ipi 等） */

#include "driver_hardware.h"             /* 硬件地址、全局变量与函数声明 */

/* 发送路径共用同一把锁，避免业务/控制并发置位共享TX控制寄存器时发生竞争 */
static DEFINE_MUTEX(amp_tx_lock);        /* TX 发送锁：保护 process_udp_data / process_ctrl_data */
static DEFINE_MUTEX(amp_read_lock);      /* 业务 RX 读取锁：保护 amp_read() 串行化 */
static DEFINE_MUTEX(amp_ctrl_read_lock); /* 控制 RX 读取锁：保护 amp_ctrl_read() 串行化 */

/* ========= 运行时打印开关 ========= */
/* amp_verbose 默认 true，运行时通过 /sys/module/driver_amp/parameters/amp_verbose 修改 */
bool amp_verbose = true;
module_param(amp_verbose, bool, 0644);
MODULE_PARM_DESC(amp_verbose, "Enable/disable driver printk (1=on, 0=off, default=1)");

/**************************************/
/* 公共：从用户态拷贝一条 AMP 消息并校验 */
/* hdr_size = offsetof(struct X, data) */
/* len_off / dtype_off 为对应字段偏移   */
/**************************************/
static void *amp_msg_from_user(const char __user *buf, size_t len,
                               size_t hdr_size, size_t len_off, size_t dtype_off)
{
    u32 plen;       /* 从消息头中解析出的载荷长度 */
    void *msg;      /* 内核态分配的消息缓冲区 */

    /* 用户传入长度连头部都不够，直接拒绝 */
    if (len < hdr_size)
        return ERR_PTR(-EINVAL);

    /* 分配内核缓冲区：头部大小 + 最大载荷（4K），确保不会溢出 */
    msg = kzalloc(hdr_size + MAX_PAYLOAD_SIZE, GFP_KERNEL);
    if (!msg)
        return ERR_PTR(-ENOMEM);

    /* 第一步：从用户态拷贝消息头部 */
    if (copy_from_user(msg, buf, hdr_size)) {
        kfree(msg);
        return ERR_PTR(-EFAULT);
    }

    /* 从消息头中偏移 len_off 处读取载荷长度字段 */
    plen = *(u32 *)((char *)msg + len_off);
    if (plen > MAX_PAYLOAD_SIZE) {
        kfree(msg);
        return ERR_PTR(-EINVAL);
    }

    /* 严格等长校验：用户传入长度必须精确等于 header + 载荷，避免短写导致协议错位 */
    if (len != hdr_size + plen) {
        kfree(msg);
        return ERR_PTR(-EINVAL);
    }

    /* 第二步：从用户态拷贝载荷数据（如果有） */
    if (plen > 0 &&
        copy_from_user((char *)msg + hdr_size, buf + hdr_size, plen)) {
        kfree(msg);
        return ERR_PTR(-EFAULT);
    }

    /* 若 data_type 为 0，默认修正为 1（避免无效类型） */
    if (*(u8 *)((char *)msg + dtype_off) == 0)
        *(u8 *)((char *)msg + dtype_off) = 1;

    return msg;
}

/*************************/
/* 用户态业务数据写入接口 */
/*************************/
ssize_t amp_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
    struct amp_net_msg *msg;    /* 从用户态拷贝并校验后的消息 */
    int ret;

    /* 从用户态拷贝消息并校验：hdr_size = 到 data 字段的偏移，即头部大小 */
    msg = amp_msg_from_user(buf, len,
                            offsetof(struct amp_net_msg, data),       /* hdr_size：头部到 data 的偏移 */
                            offsetof(struct amp_net_msg, len),        /* len_off：len 字段偏移 */
                            offsetof(struct amp_net_msg, data_type)); /* dtype_off：data_type 字段偏移 */
    if (IS_ERR(msg))
        return PTR_ERR(msg);    /* 返回具体错误码（-EINVAL / -ENOMEM / -EFAULT） */

    /* 检查共享内存是否已映射完成 */
    if (!driver_amp_resources_ready() || !ctrl_reg) {
        ret = -ENODEV;
        goto out_free;
    }

    /* 获取 TX 发送锁（可被信号中断的阻塞锁） */
    if (mutex_lock_interruptible(&amp_tx_lock)) {
        ret = -ERESTARTSYS;
        goto out_free;
    }

    /* 将业务数据写入 TX 共享内存并置位 ctrl_reg bit0 */
    ret = process_udp_data(msg);
    if (ret)
        goto out_unlock;

    /* 打印发送日志（频率限制避免刷屏） */
    amp_pr_info("TX: ip=%pI4 len=%u target=%u sgi=%u\n",
            &msg->ip, msg->len, 3, AMP_SGI_TX);

    /* 向 CPU1（CPU 3）发送 SGI15 中断，通知其读取业务数据 */
    smp_kick_ipi(cpumask_of(3), AMP_SGI_TX);
    amp_pr_info("IPI sent to CPU3 for business data\n");
    
    /* 返回实际写入字节数 = header 大小 + 载荷长度 */
    ret = offsetof(struct amp_net_msg, data) + msg->len;

out_unlock:
    mutex_unlock(&amp_tx_lock);     /* 释放 TX 锁 */
out_free:
    kfree(msg);                     /* 释放内核缓冲区 */
    return ret;
}

/*************************/
/* 用户态控制数据写入接口 */
/* 流程与 amp_write 类似，但使用 ctrl_msg 结构体和控制数据 TX 区域 */
/*************************/
ssize_t amp_ctrl_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
    struct amp_ctrl_msg *msg;   /* 从用户态拷贝并校验后的控制消息 */
    int ret;

    /* 从用户态拷贝控制消息：参数与业务消息类似，但使用 amp_ctrl_msg 结构体 */
    msg = amp_msg_from_user(buf, len,
                            offsetof(struct amp_ctrl_msg, data),       /* hdr_size */
                            offsetof(struct amp_ctrl_msg, len),        /* len_off */
                            offsetof(struct amp_ctrl_msg, data_type)); /* dtype_off */
    if (IS_ERR(msg))
        return PTR_ERR(msg);

    /* 检查共享内存是否已映射 */
    if (!driver_amp_resources_ready() || !ctrl_reg) {
        ret = -ENODEV;
        goto out_free;
    }

    /* 获取 TX 发送锁 */
    if (mutex_lock_interruptible(&amp_tx_lock)) {
        ret = -ERESTARTSYS;
        goto out_free;
    }

    /* 将控制数据写入 TX 共享内存并置位 ctrl_reg bit1 */
    ret = process_ctrl_data(msg);
    if (ret)
        goto out_unlock;

    /* 打印控制发送日志 */
    amp_pr_info("CTRL TX: len=%u target=%u sgi=%u\n",
            msg->len, 3, AMP_SGI_TX);

    /* 向 CPU1 发送 SGI15 中断，通知其读取控制数据 */
    smp_kick_ipi(cpumask_of(3), AMP_SGI_TX);

    /* 返回实际写入字节数 */
    ret = offsetof(struct amp_ctrl_msg, data) + msg->len;

out_unlock:
    mutex_unlock(&amp_tx_lock);     /* 释放 TX 锁 */
out_free:
    kfree(msg);                     /* 释放内核缓冲区 */
    return ret;
}


/******************************************/
/* 用户态读取接口：读取CPU1回来的业务数据包 */
/******************************************/
ssize_t amp_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
    ssize_t ret;                    /* 返回值：正数=读取字节数，负数=错误码 */
    size_t msg_bytes;               /* 当前消息的完整字节数 */
    unsigned int head;              /* 当前队列头索引的快照 */
    unsigned long flags;            /* 自旋锁 irqsave 的 flags */

    if (mutex_lock_interruptible(&amp_read_lock))       /* 给读取互斥锁上锁（可被信号中断） */
        return -ERESTARTSYS;

retry_wait:
    /* 阻塞等待：直到有数据包到来 */
    if (!(file->f_flags & O_NONBLOCK)) {                /* 阻塞模式：在等待队列上睡眠 */
        ret = wait_event_interruptible(rx_wq, atomic_read(&rx_ring_count) != 0);    /* 等待条件变为真，即环内待接收数量不为0 */
        if (ret)
            goto out_unlock;
    } else if (atomic_read(&rx_ring_count) == 0) {      /* 非阻塞模式且队列为空：立即返回 EAGAIN */
        ret = -EAGAIN;
        goto out_unlock;
    }

    /* 设备正在卸载（remove 时 count 被置为 -1） */
    if (atomic_read(&rx_ring_count) < 0) {
        ret = -ENODEV;
        goto out_unlock;
    }

    /* 加自旋锁（关本地中断），安全读取队列头 */
    spin_lock_irqsave(&rx_ring_lock, flags);
    if (atomic_read(&rx_ring_count) == 0) {             /* 二次检查：加锁后队列可能已被清空 */
        spin_unlock_irqrestore(&rx_ring_lock, flags);
        if (!(file->f_flags & O_NONBLOCK))
            goto retry_wait;                            /* 阻塞模式：回去重新等待 */
        ret = -EAGAIN;
        goto out_unlock;
    }
    head = rx_ring_head;                                /* 快照队列头索引 */
    msg_bytes = rx_ring[head].msg_bytes;                /* 读取本条消息的字节数 */
    spin_unlock_irqrestore(&rx_ring_lock, flags);       /* 释放自旋锁（恢复中断） */

    if (len < msg_bytes) {
        /* 用户缓冲区不够大：返回实际所需字节数，不丢包。调用方可用更大的 buf 重试 */
        ret = (ssize_t)msg_bytes;
        goto out_unlock;
    }

    /* 将消息内容拷贝到用户态缓冲区 */
    if (copy_to_user(buf, &rx_ring[head].msg, msg_bytes)) {
        ret = -EFAULT;
        goto out_unlock;
    }

    /* 再次加锁，安全地推进队列头 */
    spin_lock_irqsave(&rx_ring_lock, flags);
    if (atomic_read(&rx_ring_count) == 0 || rx_ring_head != head) {
        /* 这期间队列被篡改（可能是 remove 清空），返回 EIO */
        spin_unlock_irqrestore(&rx_ring_lock, flags);
        ret = -EIO;
        goto out_unlock;
    }
    rx_ring_head = (rx_ring_head + 1U) % RX_RING_SIZE;  /* 环形推进头索引 */
    atomic_dec(&rx_ring_count);                          /* 待读计数减1 */
    atomic_inc(&rx_dequeued);                            /* 出队计数加1 */
    spin_unlock_irqrestore(&rx_ring_lock, flags);

    ret = msg_bytes;    /* 成功：返回本次读取的字节数 */

out_unlock:
    mutex_unlock(&amp_read_lock);    /* 释放读取锁 */
    return ret;
}

/******************************************/
/* 用户态读取接口：读取CPU1回来的控制数据包 */
/******************************************/
ssize_t amp_ctrl_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
    ssize_t ret;                    /* 返回值 */
    size_t msg_bytes;               /* 当前控制消息的字节数 */
    unsigned int head;              /* 队列头索引快照 */
    unsigned long flags;            /* 自旋锁 flags */

    if (mutex_lock_interruptible(&amp_ctrl_read_lock))   /* 获取控制读锁 */
        return -ERESTARTSYS;

retry_wait:
    if (!(file->f_flags & O_NONBLOCK)) {                /* 阻塞模式 */
        ret = wait_event_interruptible(ctrl_rx_wq, atomic_read(&ctrl_rx_ring_count) != 0);
        if (ret)
            goto out_unlock;
    } else if (atomic_read(&ctrl_rx_ring_count) == 0) { /* 非阻塞但队列为空 */
        ret = -EAGAIN;
        goto out_unlock;
    }

    /* 设备正在卸载（remove 时 count 被置为 -1） */
    if (atomic_read(&ctrl_rx_ring_count) < 0) {
        ret = -ENODEV;
        goto out_unlock;
    }

    spin_lock_irqsave(&ctrl_rx_ring_lock, flags);       /* 加锁读取队列头 */
    if (atomic_read(&ctrl_rx_ring_count) == 0) {         /* 二次检查 */
        spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);
        if (!(file->f_flags & O_NONBLOCK))
            goto retry_wait;                            /* 阻塞模式回去重新等 */
        ret = -EAGAIN;
        goto out_unlock;
    }
    head = ctrl_rx_ring_head;                           /* 快照头索引 */
    msg_bytes = ctrl_rx_ring[head].msg_bytes;           /* 获取消息字节数 */
    spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);

    if (len < msg_bytes) {
        /* 返回实际所需字节数，不丢包。调用方可用更大的 buf 重试 */
        ret = (ssize_t)msg_bytes;
        goto out_unlock;
    }

    /* 拷贝到用户态 */
    if (copy_to_user(buf, &ctrl_rx_ring[head].msg, msg_bytes)) {
        ret = -EFAULT;
        goto out_unlock;
    }

    spin_lock_irqsave(&ctrl_rx_ring_lock, flags);       /* 再次加锁推进队列 */
    if (atomic_read(&ctrl_rx_ring_count) == 0 || ctrl_rx_ring_head != head) {
        /* 期间队列被篡改 */
        spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);
        ret = -EIO;
        goto out_unlock;
    }
    ctrl_rx_ring_head = (ctrl_rx_ring_head + 1U) % RX_RING_SIZE;  /* 推进头索引 */
    atomic_dec(&ctrl_rx_ring_count);                    /* 计数减1 */
    atomic_inc(&ctrl_rx_dequeued);                      /* 出队计数加1 */
    spin_unlock_irqrestore(&ctrl_rx_ring_lock, flags);

    ret = msg_bytes;    /* 成功返回 */

out_unlock:
    mutex_unlock(&amp_ctrl_read_lock);   /* 释放控制读锁 */
    return ret;
}

/****************/
/* 文件操作结构体 */
/****************/
/* 业务设备 /dev/amp_ipi 的文件操作：支持 read/write */
static const struct file_operations amp_fops = {
    .owner = THIS_MODULE,       /* 模块所有者，防止模块在使用中被卸载 */
    .write = amp_write,         /* 用户态 write() -> 业务数据下行到 CPU1 */
    .read = amp_read,           /* 用户态 read() -> 从 CPU1 读取业务数据 */
};

/* 控制设备 /dev/amp_ctrl 的文件操作：支持 read/write */
static const struct file_operations amp_ctrl_fops = {
    .owner = THIS_MODULE,       /* 模块所有者 */
    .write = amp_ctrl_write,    /* 用户态 write() -> 控制指令下行到 CPU1 */
    .read = amp_ctrl_read,      /* 用户态 read() -> 从 CPU1 读取控制回执 */
};


/*****************/
/* Misc设备结构体 */
/****************/
struct miscdevice amp_miscdev = {           /* 在 probe 中注册，remove 中注销 */
    .minor = MISC_DYNAMIC_MINOR,            /* 动态分配次设备号 */
    .name = "amp_ipi",                      /* 设备名：用户态 open("/dev/amp_ipi") */
    .fops = &amp_fops,                      /* 指向业务文件操作结构体 */
};

struct miscdevice amp_ctrl_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,            /* 动态分配次设备号 */
    .name = "amp_ctrl",                     /* 设备名：用户态 open("/dev/amp_ctrl") */
    .fops = &amp_ctrl_fops,                 /* 指向控制文件操作结构体 */
};

/****************/
/* 驱动probe函数 */
/* 设备匹配后按顺序：映射共享内存 -> 注册中断 -> 注册 misc 设备 */
/****************/
static int zynq_amp_probe(struct platform_device *pdev)
{
    int ret;    /* 各步骤返回值，0 表示成功 */

    /* 步骤1：ioremap_nocache 所有共享内存和寄存器 */
    ret = driver_amp_map_resources();
    if (ret)
        goto error;

    /* 步骤2：注册软中断处理函数（SGI14 = CPU1 通知 CPU0） */
    ret = set_ipi_handler(AMP_SGI_RX, cpu1_to_cpu0_handler, NULL);
    if (ret)
        goto error;    /* 失败则跳到 error 释放已映射的资源 */

    /* 步骤3：注册业务 misc 设备 /dev/amp_ipi */
    ret = misc_register(&amp_miscdev);
    if (ret) {
        clear_ipi_handler(AMP_SGI_RX);    /* 回滚：清除中断处理 */
        goto error;
    }

    /* 步骤4：注册控制 misc 设备 /dev/amp_ctrl */
    ret = misc_register(&amp_ctrl_miscdev);
    if (ret) {
        misc_deregister(&amp_miscdev);      /* 回滚：注销业务设备 */
        clear_ipi_handler(AMP_SGI_RX);     /* 回滚：清除中断处理 */
        goto error;
    }

    return 0;   /* probe 成功 */

error:
    /* 回滚：释放所有映射 */
    driver_amp_unmap_resources();
    return ret;
}


/*****************/
/* 驱动remove函数 */
/* 按相反顺序安全卸载：注销中断 -> 唤醒阻塞线程 -> 注销设备 -> 等待临界区 -> 释放映射 */
/*****************/
static int zynq_amp_remove(struct platform_device *pdev)
{
    /* 1. 先注销中断——阻止新的 RX 数据进入（之后的中断不再被处理） */
    clear_ipi_handler(AMP_SGI_RX);

    /* 2. 将计数置为 -1 标记"设备正在卸载"，然后唤醒所有阻塞在 read() 上的线程 */
    atomic_set(&rx_ring_count, -1);         /* 业务队列标记卸载 */
    atomic_set(&ctrl_rx_ring_count, -1);    /* 控制队列标记卸载 */
    wake_up_interruptible(&rx_wq);          /* 唤醒业务 read() 阻塞者（将看到 count=-1 -> ENODEV） */
    wake_up_interruptible(&ctrl_rx_wq);     /* 唤醒控制 read() 阻塞者 */

    /* 3. 注销 misc 设备——阻止新的 open() 调用 */
    misc_deregister(&amp_ctrl_miscdev);      /* 先注销控制设备 */
    misc_deregister(&amp_miscdev);           /* 再注销业务设备 */

    /* 4. 等待正在执行的 read/write 返回（用互斥锁做屏障：获取后立即释放 = 确保没人持锁） */
    mutex_lock(&amp_tx_lock);        /* 等待最后一次 write 完成 */
    mutex_unlock(&amp_tx_lock);
    mutex_lock(&amp_read_lock);      /* 等待最后一次业务 read 完成 */
    mutex_unlock(&amp_read_lock);
    mutex_lock(&amp_ctrl_read_lock); /* 等待最后一次控制 read 完成 */
    mutex_unlock(&amp_ctrl_read_lock);

    /* 5. 最后释放所有 ioremap_nocache 映射 */
    driver_amp_unmap_resources();
    return 0;
}

/***************************/
/* 设备树匹配表（不可修改） */
/* 当设备树中存在 compatible="xlnx,zynq-amp" 节点时，触发本驱动的 probe */
/**************************/
static const struct of_device_id amp_of_match[] = {
    { .compatible = "xlnx,zynq-amp" },  /* 匹配字符串，与设备树 .dts 中一致 */
    { }                                   /* 空项表示结束 */
};
MODULE_DEVICE_TABLE(of, amp_of_match);    /* 导出到模块信息，供用户态 modprobe 匹配 */


/*****************************/
/* 平台驱动结构体（不可修改） */
/* 将 probe/remove 与设备树匹配表绑定，构成完整的平台驱动 */
/****************************/
static struct platform_driver zynq_amp_driver = {
    .driver = {
        .name = "zynq_amp",                     /* 驱动名称（出现在 /sys/bus/platform/drivers/） */
        .of_match_table = amp_of_match,         /* 设备树匹配表 */
        .owner = THIS_MODULE,                   /* 模块所有者 */
    },
    .probe = zynq_amp_probe,                    /* 设备匹配时调用 */
    .remove = zynq_amp_remove,                  /* 设备移除时调用 */
};

module_platform_driver(zynq_amp_driver);        /* 注册平台驱动的便捷宏（自动处理 init/exit） */

MODULE_LICENSE("GPL");                                           /* 许可证类型 */
MODULE_AUTHOR("XuShengQiao");                                    /* 作者 */
MODULE_DESCRIPTION("AMP IPC Driver with Updated Protocol Support"); /* 模块描述 */
