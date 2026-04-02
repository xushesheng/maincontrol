/***********************/
/*  TUN虚拟设备相关函数 */
/***********************/
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>

#include "user_amp_runtime.h"

/*******************
*TUN虚拟设备创建函数*
*******************/
int tun_alloc(const char *devname)
{
    struct ifreq ifr;                           //创建ifr设备标识符
    int fd = open("/dev/net/tun", O_RDWR);      //Linux 中创建虚拟网卡的标准入口

    if (fd < 0) {
        perror("open /dev/net/tun");            //创建失败直接返回-1
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));               //初始化标识符
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;        //创建 TUN 设备（IP 层），而不是 TAP 设备;不添加额外的包头信息（Packet Information）
    strncpy(ifr.ifr_name, devname, IFNAMSIZ - 1);       //指定虚拟网卡的名称（如 "rf0"）   

    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {       //通过ioctl 系统调用创建虚拟网络设备
        perror("ioctl(TUNSETIFF)");
        close(fd);
        return -1;
    }
    return fd;
}

/**************************
*向TUN虚拟网络设备写入数据包*
**************************/
int tun_write_packet(int fd, const uint8_t *pkt, size_t len)
{
    while (1) {
        ssize_t w = write(fd, pkt, len);
        if (w == (ssize_t)len)      //成功写入则立即返回
            return 0;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {       //设备输出缓冲区满时返回此错误,poll等10ms，直到设备可写然后重新尝试写入
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            (void)poll(&p, 1, 10);
            continue;
        }
        if (w < 0 && errno == EINTR)          // 被信号中断，重试
            continue;
        return -1;
    }
}
