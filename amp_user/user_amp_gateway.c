/************************************/
/*eth1/rf0路由、proxy ARP、sysctl配置*/
/************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>

#include "user_amp_runtime.h"

/* 获取指定网络接口 ifname 的 IPv4 地址写到 *addr 里。*/
static int get_iface_ipv4(const char *ifname, struct in_addr *addr)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);        //创建了一个套接字，返回文件描述符fd用于ioctl
    struct ifreq ifr;                               //ifreq 是网络接口控制里常用的结构体，用于和内核交换网卡信息

    if (fd < 0)
        return -1;

    memset(&ifr, 0, sizeof(ifr));                   //初始化ifr结构体
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);    //把传进来的网卡名复制到 ifr.ifr_name
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {         //通过ioctl 获取接口IPv4地址到ifr.ifr_addr
        close(fd);
        return -1;
    }

    *addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;        //取出 IPv4 地址写到*addr 
    close(fd);
    return 0;
}

/*调用系统命令函数*/
static void run_cmd(const char *cmd)
{
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "[WARN] cmd failed (%d): %s\n", rc, cmd);
}

/*
 * 根据本板卡eth1 IP（192.168.1.13 / 192.168.1.12）推导：
 * - 要代理ARP的“远端PC IP”
 * - rf0地址
 */
void setup_gateway_rules(void)
{
    struct in_addr local_ip;
    const char *remote_pc = NULL;
    const char *rf_ip = NULL;
    uint8_t *p;
    uint8_t last;

    if (get_iface_ipv4(CAP_IFACE, &local_ip) != 0) {
        fprintf(stderr, "[ERROR] cannot get %s IPv4 address\n", CAP_IFACE);
        return;
    }

    p = (uint8_t *)&local_ip.s_addr; /* network order */
    /* local_ip.s_addr是网络序，逐字节拿出来没问题 */
    last = p[3];

    if (last == 13) {
        /* 板卡A：192.168.1.13 连接 PC A(192.168.1.23)，要把发往PC B(192.168.1.20)的流量引到自己 */
        remote_pc = "192.168.1.20"; /* PC B */
        rf_ip = "10.255.0.1/30";
    } else if (last == 12) {
        /* 板卡B：192.168.1.12 连接 PC B(192.168.1.20)，要把发往PC A(192.168.1.23)的流量引到自己 */
        remote_pc = "192.168.1.23";
        rf_ip = "10.255.0.2/30";
    } else {
        fprintf(stderr,
                "[WARN] unexpected %s IP=%s, default remote_pc=192.168.1.15, rf_ip=10.255.0.1/30\n",
                CAP_IFACE, inet_ntoa(local_ip));
        remote_pc = "192.168.1.20";
        rf_ip = "10.255.0.1/30";
    }

    remote_pc_addr = inet_addr(remote_pc);
    if (remote_pc_addr == INADDR_NONE) {
        fprintf(stderr, "[ERROR] bad remote_pc ip: %s\n", remote_pc);
        exit(1);
    }

    /* 让内核允许转发 */
    run_cmd("sysctl -w net.ipv4.ip_forward=1 >/dev/null");

    /* 避免反向路径过滤把转发包丢掉 */
    run_cmd("sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null");
    run_cmd("sysctl -w net.ipv4.conf.eth1.rp_filter=0 >/dev/null");

    /* proxy ARP：让PC把对端PC的ARP解析到本板卡MAC */
    run_cmd("sysctl -w net.ipv4.conf.eth1.proxy_arp=1 >/dev/null");
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip neigh add proxy %s dev %s 2>/dev/null || true", remote_pc, CAP_IFACE);
        run_cmd(cmd);
    }

    /* TUN口配置 */
    run_cmd("ip link set rf0 up 2>/dev/null || true");
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip link set rf0 mtu %d 2>/dev/null || true", RF0_MTU);
        run_cmd(cmd);
    }

    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip addr add %s dev rf0 2>/dev/null || true", rf_ip);
        run_cmd(cmd);
    }

    {
        /* 把远端PC的/32路由导向rf0，使内核把这类包吐给TUN */
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip route add %s/32 dev rf0 2>/dev/null || true", remote_pc);
        run_cmd(cmd);
    }

    printf("[INFO] gateway enabled on %s=%s, proxy_arp for %s, route %s/32 -> rf0\n",
           CAP_IFACE, inet_ntoa(local_ip), remote_pc, remote_pc);
}
