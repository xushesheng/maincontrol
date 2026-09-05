/************************************/
/*eth0/rf0路由、proxy ARP、sysctl配置*/
/************************************/
#include <stdio.h>                 /* printf / fprintf / snprintf */
#include <stdlib.h>                /* system / exit */
#include <string.h>                /* memset / strncpy */
#include <unistd.h>                /* close */
#include <arpa/inet.h>             /* inet_addr / inet_ntoa */
#include <sys/ioctl.h>             /* ioctl / SIOCGIFADDR */
#include <sys/socket.h>            /* socket / struct sockaddr_in */
#include <linux/if.h>              /* IFNAMSIZ / struct ifreq */

#include "user_declaration.h"      /* 全局变量声明 */

/* 网关节点表条目：每个对端节点对应一条记录 */
typedef struct {
    uint8_t node_id;            /* 节点编号（0~31） */
    char board_ip[16];          /* 本地板卡 IP 字符串（192.168.1.(50+node_id)） */
    char pc_ip[16];             /* 对端 PC IP 字符串（192.168.1.(10+node_id)） */
    char rf_ip_cidr[16];        /* rf0 网口的 CIDR 地址（10.255.0.(10+node_id)/24） */
    uint32_t board_ip_be;       /* board_ip 的网络字节序形式（用于快速比较） */
    uint32_t pc_ip_be;          /* pc_ip 的网络字节序形式（用于快速比较） */
} gateway_node_t;

/*
 * 节点表按规则在 prepare_gateway_nodes() 中运行时生成，不在此硬编码：
 *   board_ip  = 192.168.1.(50 + node_id)   // 本地板卡IP on eth0
 *   pc_ip     = 192.168.1.(10 + node_id)   // 与驱动侧 ip_to_nodeid 映射一致
 *   rf_ip     = 10.255.0.(10 + node_id)/24
 */
/* ========= 节点表全局状态 ========= */
#define GATEWAY_NODE_COUNT 32                          /* 最多支持 32 个节点 */
static gateway_node_t gateway_nodes[GATEWAY_NODE_COUNT];  /* 节点表数组 */

static gateway_node_t *self_node = NULL;              /* 指向节点表中本机对应的条目（通过 eth0 IP 匹配） */
static int gateway_nodes_ready = 0;                   /* 节点表是否已初始化（避免重复生成） */


/* 把字符串形式 ip_str 转换成 uint32_t 的网络字节序地址 ip_be */
static int parse_ipv4_be(const char *ip_str, uint32_t *ip_be)
{
    uint32_t ip = inet_addr(ip_str);    /* 字符串 IP -> 网络字节序 uint32_t */

    if (ip == INADDR_NONE)              /* 转换失败（非法 IP 格式） */
        return -1;

    *ip_be = ip;                        /* 输出网络字节序地址 */
    return 0;
}

/* 按规则生成 32 节点表，并解析出网络字节序地址 */
static int prepare_gateway_nodes(void)
{
    size_t i;   /* 节点索引 */

    if (gateway_nodes_ready)
        return 0;   /* 已经生成过，直接返回 */

    for (i = 0; i < GATEWAY_NODE_COUNT; i++) {
        gateway_nodes[i].node_id = (uint8_t)i;      /* 节点编号 = 索引 */

        /* board_ip = 192.168.1.(50 + node_id)：eth0 上的板卡 IP */
        snprintf(gateway_nodes[i].board_ip, sizeof(gateway_nodes[i].board_ip),
                 "192.168.1.%u", 50u + (unsigned)i);

        /* pc_ip = 192.168.1.(10 + node_id)：对端 PC 的 IP（与驱动侧 ip_to_nodeid 映射一致） */
        snprintf(gateway_nodes[i].pc_ip, sizeof(gateway_nodes[i].pc_ip),
                 "192.168.1.%u", 10u + (unsigned)i);

        /* rf_ip = 10.255.0.(10 + node_id)/24：rf0 上的 IP */
        snprintf(gateway_nodes[i].rf_ip_cidr, sizeof(gateway_nodes[i].rf_ip_cidr),
                 "10.255.0.%u/24", 10u + (unsigned)i);

        /* 解析为网络字节序 uint32_t，用于快速 IP 比较 */
        if (parse_ipv4_be(gateway_nodes[i].board_ip, &gateway_nodes[i].board_ip_be) != 0 ||
            parse_ipv4_be(gateway_nodes[i].pc_ip, &gateway_nodes[i].pc_ip_be) != 0) {
            fprintf(stderr, "[ERROR] bad generated ip in node %zu\n", i);
            return -1;
        }
    }

    gateway_nodes_ready = 1;    /* 标记已初始化 */
    return 0;
}

/* 根据本机 eth0 的实际 IP，找到自己在节点表中的那一行 */
static gateway_node_t *find_self_node(uint32_t local_ip_be)
{
    size_t i;

    for (i = 0; i < GATEWAY_NODE_COUNT; i++) {
        if (gateway_nodes[i].board_ip_be == local_ip_be)   /* 比较网络字节序 IP */
            return &gateway_nodes[i];
    }

    return NULL;    /* 未匹配（本机 eth0 IP 不在节点表中） */
}

/* 读取指定网络接口（如 eth0）的 IPv4 地址 */
int get_iface_ipv4(const char *ifname, struct in_addr *addr)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);    /* 创建临时 UDP socket（仅用于 ioctl） */
    struct ifreq ifr;                            /* 接口请求结构体 */

    if (fd < 0)
        return -1;

    memset(&ifr, 0, sizeof(ifr));               /* 清零 */
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);/* 设置要查询的接口名 */
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {     /* SIOCGIFADDR = 获取接口 IPv4 地址 */
        close(fd);
        return -1;
    }

    *addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;   /* 提取 IP 地址 */
    close(fd);
    return 0;
}

/* 调用系统命令函数（用于 sysctl / ip 等网络配置命令） */
static void run_cmd(const char *cmd)
{
    int rc = system(cmd);   /* 同步执行 shell 命令 */
    if (rc != 0)
        fprintf(stderr, "[WARN] cmd failed (%d): %s\n", rc, cmd);
}

/* 判断一个目的 IP（网络字节序）是否在节点表里（排除自己） */
int is_peer_pc_addr(uint32_t ip_be)
{
    size_t i;

    if (!self_node)             /* 本机节点尚未确定 */
        return 0;

    for (i = 0; i < GATEWAY_NODE_COUNT; i++) {
        if (&gateway_nodes[i] == self_node)
            continue;           /* 跳过自己 */
        if (gateway_nodes[i].pc_ip_be == ip_be)
            return 1;           /* 找到匹配的对端 PC */
    }

    return 0;   /* 不是任何对端 PC 的 IP */
}


/* 网关规则设定：配置 ip_forward、rp_filter、proxy_arp、TUN 参数及对端路由 */
void setup_gateway_rules(void)
{
    struct in_addr local_ip;            /* eth0 的 IPv4 地址 */
    size_t i;                           /* 循环索引 */
    size_t peer_count = 0;              /* 对端节点计数（排除自己） */

    /* 获取本机 eth0 的 IP 地址 */
    if (get_iface_ipv4(CAP_IFACE, &local_ip) != 0) {
        fprintf(stderr, "[ERROR] cannot get %s IPv4 address\n", CAP_IFACE);
        return;
    }

    /* 生成 32 节点表 */
    if (prepare_gateway_nodes() != 0)
        exit(1);    /* 节点表生成失败，直接退出 */

    /* 根据 eth0 IP 在节点表中定位自己 */
    self_node = find_self_node(local_ip.s_addr);
    if (!self_node) {
        fprintf(stderr, "[ERROR] %s IP=%s is not present in the node table\n",
                CAP_IFACE, inet_ntoa(local_ip));
        exit(1);    /* 本机 IP 不在预期的节点 IP 范围内 */
    }

    /* 让内核允许 IP 转发（本机作为网关转发包到 rf0） */
    run_cmd("sysctl -w net.ipv4.ip_forward=1 >/dev/null");

    /* 关闭反向路径过滤：避免内核因非对称路由丢弃转发包 */
    run_cmd("sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null");
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "sysctl -w net.ipv4.conf.%s.rp_filter=0 >/dev/null", CAP_IFACE);
        run_cmd(cmd);   /* 同时关闭 eth0 上的 rp_filter */
    }

    /* 开启 proxy ARP：让本机代为响应 PC 对远端 PC 的 ARP 请求 */
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "sysctl -w net.ipv4.conf.%s.proxy_arp=1 >/dev/null", CAP_IFACE);
        run_cmd(cmd);
    }

    /* TUN 口配置：启用 rf0 并设置 MTU */
    run_cmd("ip link set rf0 up 2>/dev/null || true");     /* 启动 rf0 设备 */
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip link set rf0 mtu %d 2>/dev/null || true", RF0_MTU);
        run_cmd(cmd);   /* 设置 MTU = 1600，允许 >640 的单包直发 */
    }

    /* 给 rf0 配置本机对应的 IP */
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip addr add %s dev rf0 2>/dev/null || true", self_node->rf_ip_cidr);
        run_cmd(cmd);   /* 如 ip addr add 10.255.0.10/24 dev rf0 */
    }

    /* 为每个对端节点配置 proxy ARP 和 /32 路由 */
    for (i = 0; i < GATEWAY_NODE_COUNT; i++) {
        char cmd[256];

        if (&gateway_nodes[i] == self_node)
            continue;   /* 跳过自己 */

        peer_count++;   /* 统计对端数量 */

        /* 配置 proxy ARP 条目：让本机代答对端 PC 的 ARP */
        snprintf(cmd, sizeof(cmd), "ip neigh add proxy %s dev %s 2>/dev/null || true",
                 gateway_nodes[i].pc_ip, CAP_IFACE);
        run_cmd(cmd);

        /* 把远端 PC 的 /32 路由导向 rf0，使内核把目的为对端 PC 的包吐给 TUN */
        snprintf(cmd, sizeof(cmd), "ip route add %s/32 dev rf0 2>/dev/null || true",
                 gateway_nodes[i].pc_ip);
        run_cmd(cmd);
    }

    /* 广播 192.168.1.255 也导向 rf0，使广播包能走 AMP 通道到达所有节点 */
    run_cmd("ip route add 192.168.1.255/32 dev rf0 2>/dev/null || true");

    /* 打印配置完成摘要 */
    printf("[INFO] gateway enabled on %s=%s, self node=%u, peer_count=%zu\n",
           CAP_IFACE, inet_ntoa(local_ip),
           (unsigned int)self_node->node_id, peer_count);
}
