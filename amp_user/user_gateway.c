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

#include "user_declaration.h"

/* 创建一个节点的结构体 */
typedef struct {
    uint8_t node_id;
    const char *board_ip;
    const char *pc_ip;
    const char *rf_ip_cidr;
    uint32_t board_ip_be;
    uint32_t pc_ip_be;
} gateway_node_t;

/*
 * board_ip 设置为本地板卡IP on eth1.
 * pc_ip 必须匹配当前驱动侧的映射规则:
 * 192.168.1.(10 + node_id) <-> node_id.
 */
static gateway_node_t gateway_nodes[] = {
    { 13, "192.168.1.53", "192.168.1.13", "10.255.0.13/24", 0, 0 },
    { 12, "192.168.1.52", "192.168.1.12", "10.255.0.12/24", 0, 0 },
    { 14, "192.168.1.54", "192.168.1.14", "10.255.0.14/24", 0, 0 },
};//分别代表：节点号，板卡IP，电脑IP，虚拟网卡IP

#define GATEWAY_NODE_COUNT (sizeof(gateway_nodes) / sizeof(gateway_nodes[0]))

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

/* 根据 node_id 推导出这个节点理论上应该对应的 pc_ip */
static int expected_pc_ip_be(uint8_t node_id, uint32_t *ip_be)
{
    char ip_str[32];

    if (node_id > 32 || node_id == 0)       //节点0不使用
        return -1;

    snprintf(ip_str, sizeof(ip_str), "192.168.1.%u", (unsigned int)(10 + node_id));
    return parse_ipv4_be(ip_str, ip_be);        //转换成整数返回
}

/* 对静态节点表做一次预处理和合法性检查 */
static int prepare_gateway_nodes(void)
{
    size_t i;
    size_t j;
    if (gateway_nodes_ready)
        return 0;
    for (i = 0; i < GATEWAY_NODE_COUNT; i++) {
        uint32_t expected_pc_be;        //定义变量 expected_pc_be，保存理论 pc_ip
        /* 把当前节点的 board_ip 从字符串解析到 board_ip_be；失败就打印错误并返回 */
        if (parse_ipv4_be(gateway_nodes[i].board_ip, &gateway_nodes[i].board_ip_be) != 0) {
            fprintf(stderr, "[ERROR] bad board_ip in node table: %s\n", gateway_nodes[i].board_ip);
            return -1;
        }
        /* 把当前节点的 pc_ip 从字符串解析到 pc_ip_be；失败就打印错误并返回 */
        if (parse_ipv4_be(gateway_nodes[i].pc_ip, &gateway_nodes[i].pc_ip_be) != 0) {
            fprintf(stderr, "[ERROR] bad pc_ip in node table: %s\n", gateway_nodes[i].pc_ip);
            return -1;
        }
        /* 校验 pc_ip 是否真的符合 node_id -> pc_ip 的驱动映射规则；不符合就报错退出 */
        if (expected_pc_ip_be(gateway_nodes[i].node_id, &expected_pc_be) != 0 ||
            expected_pc_be != gateway_nodes[i].pc_ip_be) {
            fprintf(stderr,
                    "[ERROR] node table pc_ip=%s does not match node_id=%u driver mapping\n",
                    gateway_nodes[i].pc_ip,
                    (unsigned int)gateway_nodes[i].node_id);
            return -1;
        }
        /* 开始和前面的节点逐个比对，检查重复项 */
        for (j = 0; j < i; j++) {
            if (gateway_nodes[j].node_id == gateway_nodes[i].node_id) {             //如果 node_id 重复，报错并返回
                fprintf(stderr, "[ERROR] duplicate node_id in node table: %u\n",
                        (unsigned int)gateway_nodes[i].node_id);
                return -1;
            }
            if (gateway_nodes[j].board_ip_be == gateway_nodes[i].board_ip_be) {     //如果 board_ip 重复，报错并返回
                fprintf(stderr, "[ERROR] duplicate board_ip in node table: %s\n",
                        gateway_nodes[i].board_ip);
                return -1;
            }
            if (gateway_nodes[j].pc_ip_be == gateway_nodes[i].pc_ip_be) {           //如果 pc_ip 重复，报错并返回
                fprintf(stderr, "[ERROR] duplicate pc_ip in node table: %s\n",
                        gateway_nodes[i].pc_ip);
                return -1;
            }
        }
    }
    gateway_nodes_ready = 1;        //所有节点都校验通过后，把 gateway_nodes_ready 置 1
    return 0;
}

/* 根据本机 eth1 的实际 IP，找到自己在节点表中的那一行 */
static gateway_node_t *find_self_node(uint32_t local_ip_be)
{
    size_t i;

    for (i = 0; i < GATEWAY_NODE_COUNT; i++) {
        if (gateway_nodes[i].board_ip_be == local_ip_be)
            return &gateway_nodes[i];
    }

    return NULL;
}

/* 读取指定网络接口(eth1)的 IPv4 地址 */
static int get_iface_ipv4(const char *ifname, struct in_addr *addr)
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

    printf("[INFO] gateway enabled on %s=%s, self node=%u, peer_count=%zu\n",
           CAP_IFACE, inet_ntoa(local_ip),
           (unsigned int)self_node->node_id, peer_count);
}
