/************************************/
/* 本文件存放驱动主函数（不可轻易修改） */
/************************************/
#include <linux/module.h>
#include <linux/platform_device.h>
#include <asm/smp.h>

#include "driver_amp_hw.h"

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
