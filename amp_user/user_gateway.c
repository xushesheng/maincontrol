/************************************/
/*eth0/rf0路由、proxy ARP、sysctl配置*/
/************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>

#include "user_declaration.h"

/* 创建一个节点的结构体 */
typedef struct {
    uint8_t node_id;
    char board_ip[16];
    char pc_ip[16];
    char rf_ip_cidr[16];
    uint32_t board_ip_be;
    uint32_t pc_ip_be;
} gateway_node_t;

/*
 * 节点表按规则在 prepare_gateway_nodes() 中运行时生成，不在此硬编码：
 *   board_ip  = 192.168.1.(50 + node_id)   // 本地板卡IP on eth0
 *   pc_ip     = 192.168.1.(10 + node_id)   // 与驱动侧 ip_to_nodeid 映射一致
 *   rf_ip     = 10.255.0.(10 + node_id)/24
 */
#define GATEWAY_NODE_COUNT 32
static gateway_node_t gateway_nodes[GATEWAY_NODE_COUNT];

static gateway_node_t *self_node = NULL;
static int gateway_nodes_ready = 0;


/* 把字符串形式 ip_str 转换成uint32_t 的网络字节序地址 ip_be */
static int parse_ipv4_be(const char *ip_str, uint32_t *ip_be)
{
    uint32_t ip = inet_addr(ip_str);

    if (ip == INADDR_NONE)
        return -1;

    *ip_be = ip;
    return 0;
}

/* 按规则生成 32 节点表，并解析出网络字节序地址 */
static int prepare_gateway_nodes(void)
{
    size_t i;

    if (gateway_nodes_ready)
        return 0;

    for (i = 0; i < GATEWAY_NODE_COUNT; i++) {
        gateway_nodes[i].node_id = (uint8_t)i;
        snprintf(gateway_nodes[i].board_ip, sizeof(gateway_nodes[i].board_ip),
                 "192.168.1.%u", 50u + (unsigned)i);
        snprintf(gateway_nodes[i].pc_ip, sizeof(gateway_nodes[i].pc_ip),
                 "192.168.1.%u", 10u + (unsigned)i);
        snprintf(gateway_nodes[i].rf_ip_cidr, sizeof(gateway_nodes[i].rf_ip_cidr),
                 "10.255.0.%u/24", 10u + (unsigned)i);

        if (parse_ipv4_be(gateway_nodes[i].board_ip, &gateway_nodes[i].board_ip_be) != 0 ||
            parse_ipv4_be(gateway_nodes[i].pc_ip, &gateway_nodes[i].pc_ip_be) != 0) {
            fprintf(stderr, "[ERROR] bad generated ip in node %zu\n", i);
            return -1;
        }
    }

    gateway_nodes_ready = 1;
    return 0;
}

/* 根据本机 eth0 的实际 IP，找到自己在节点表中的那一行 */
static gateway_node_t *find_self_node(uint32_t local_ip_be)
{
    size_t i;

    for (i = 0; i < GATEWAY_NODE_COUNT; i++) {
        if (gateway_nodes[i].board_ip_be == local_ip_be)
            return &gateway_nodes[i];
    }

    return NULL;
}

/* 读取指定网络接口(eth0)的 IPv4 地址 */
int get_iface_ipv4(const char *ifname, struct in_addr *addr)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr;

    if (fd < 0)
        return -1;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        close(fd);
        return -1;
    }

    *addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
    close(fd);
    return 0;
}

/* 调用系统命令函数 */
static void run_cmd(const char *cmd)
{
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "[WARN] cmd failed (%d): %s\n", rc, cmd);
}

/* 判断一个目的 IP 是否在节点表里 */
int is_peer_pc_addr(uint32_t ip_be)
{
    size_t i;

    if (!self_node)
        return 0;

    for (i = 0; i < GATEWAY_NODE_COUNT; i++) {
        if (&gateway_nodes[i] == self_node)
            continue;
        if (gateway_nodes[i].pc_ip_be == ip_be)
            return 1;
    }

    return 0;
}


/* 网管规则设定，配置转发 */
void setup_gateway_rules(void)
{
    struct in_addr local_ip;
    size_t i;
    size_t peer_count = 0;

    if (get_iface_ipv4(CAP_IFACE, &local_ip) != 0) {
        fprintf(stderr, "[ERROR] cannot get %s IPv4 address\n", CAP_IFACE);
        return;
    }

    if (prepare_gateway_nodes() != 0)
        exit(1);

    self_node = find_self_node(local_ip.s_addr);
    if (!self_node) {
        fprintf(stderr, "[ERROR] %s IP=%s is not present in the node table\n",
                CAP_IFACE, inet_ntoa(local_ip));
        exit(1);
    }

    /* 让内核允许转发 */
    run_cmd("sysctl -w net.ipv4.ip_forward=1 >/dev/null");

    /* 避免反向路径过滤把转发包丢掉 */
    run_cmd("sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null");
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "sysctl -w net.ipv4.conf.%s.rp_filter=0 >/dev/null", CAP_IFACE);
        run_cmd(cmd);
    }

    /* proxy ARP：让PC把对端PC的ARP解析到本板卡MAC */
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "sysctl -w net.ipv4.conf.%s.proxy_arp=1 >/dev/null", CAP_IFACE);
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
        snprintf(cmd, sizeof(cmd), "ip addr add %s dev rf0 2>/dev/null || true", self_node->rf_ip_cidr);
        run_cmd(cmd);
    }

    for (i = 0; i < GATEWAY_NODE_COUNT; i++) {
        char cmd[256];

        if (&gateway_nodes[i] == self_node)
            continue;

        peer_count++;

        snprintf(cmd, sizeof(cmd), "ip neigh add proxy %s dev %s 2>/dev/null || true",
                 gateway_nodes[i].pc_ip, CAP_IFACE);
        run_cmd(cmd);

        /* 把远端PC的/32路由导向rf0，使内核把这类包吐给TUN */
        snprintf(cmd, sizeof(cmd), "ip route add %s/32 dev rf0 2>/dev/null || true",
                 gateway_nodes[i].pc_ip);
        run_cmd(cmd);
    }

    /* 广播 192.168.1.255 也导向 rf0，使广播包能走 AMP 通道 */
    run_cmd("ip route add 192.168.1.255/32 dev rf0 2>/dev/null || true");

    printf("[INFO] gateway enabled on %s=%s, self node=%u, peer_count=%zu\n",
           CAP_IFACE, inet_ntoa(local_ip),
           (unsigned int)self_node->node_id, peer_count);
}
