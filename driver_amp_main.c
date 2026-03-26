#include <linux/module.h>
#include <linux/platform_device.h>
#include <asm/smp.h>

#include "driver_amp_hw.h"

static int zynq_amp_probe(struct platform_device *pdev)
{
    int ret;

    ret = driver_amp_map_resources();
    if (ret)
        goto error;

    ret = set_ipi_handler(AMP_SGI_RX, cpu1_to_cpu0_handler, NULL);
    if (ret)
        goto error;

    ret = misc_register(&amp_miscdev);
    if (ret) {
        clear_ipi_handler(AMP_SGI_RX);
        goto error;
    }

    return 0;

error:
    driver_amp_unmap_resources();
    return ret;
}

static int zynq_amp_remove(struct platform_device *pdev)
{
    clear_ipi_handler(AMP_SGI_RX);
    misc_deregister(&amp_miscdev);
    driver_amp_unmap_resources();
    return 0;
}

static const struct of_device_id amp_of_match[] = {
    { .compatible = "xlnx,zynq-amp" },
    { }
};
MODULE_DEVICE_TABLE(of, amp_of_match);

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
