/*******************************************************************************
 * 广播二层桥接（用户态 AF_PACKET，网口 CAP_IFACE / eth0）
 *
 * 为什么需要它（根因，2026-09-17 板卡实测 + 内核源码确认）：
 *   192.168.1.255 恰好是 eth0 自身网段（192.168.1.0/24）的定向广播，内核在配
 *   `ip addr add 192.168.1.60/24` 时会在 **local 表**（ip rule pref 0，先于 main 表 32766）
 *   自动生成一条 `broadcast 192.168.1.255 dev eth0 proto kernel scope link`。
 *   于是 user_gateway.c 里加在 main 表的 `192.168.1.255/32 dev rf0` 被架空，双向都不通：
 *
 *   上行（PC -> 板卡 -> 射频）：
 *     入站 FIB 命中 local 表那条 RTN_BROADCAST；v5.4 net/ipv4/route.c 的
 *     ip_route_input_slow() 中，只有 `IN_DEV_BFORWARD(in_dev)`（收包网口的 bc_forwarding，
 *     默认 0）为真才 `goto make_route` 走转发，否则 `goto brd_input` -> `local_input:`，
 *     路由缓存带 RTCF_LOCAL 且 `rth->dst.output = ip_rt_bug` —— 只本地投递，
 *     既不转发也不出设备。3408 上没有本地监听 socket，于是包被静默丢弃，永远进不了 rf0。
 *     注意：光开 bc_forwarding 也不行，因为 goto make_route 用的 FIB 结果仍是 local 表那条
 *     （dev eth0 == 入接口）。
 *
 *   下行（射频 -> 板卡 -> 本地 PC）：
 *     应用 write(tun_fd) 注入时收包设备是 rf0，同样命中 local 表那条 RTN_BROADCAST，
 *     同样走 brd_input，结果只本地投递、**永远不会从 eth0 发给本地 PC**。
 *
 *   而**不能**用"删掉 local 表项"来修：删掉后下行的输入 FIB 会落到 main 表
 *   `/32 dev rf0`（out_dev == in_dev）→ 自环风暴；且重启/重跑 setup_gateway_rules() 后
 *   内核会重新生成该表项，属于"每次开机都要重做"的脆弱方案。
 *
 * 因此本模块用用户态 AF_PACKET 在 eth0 上直接做二层，彻底绕开内核路由与转发策略：
 *   上行：抓到目的为 192.168.1.255 的入向帧 -> 按 IP 头 tot_len 裁剪 -> 直接送 AMP（node 255）
 *   下行：从 AMP 收到目的为 192.168.1.255 的包时，在 eth0 上补发一份二层广播给本地 PC
 *   （自建二层头 dst=ff:ff:ff:ff:ff:ff，不查路由、不需要 ARP）
 *
 * 与内置路由路径的互斥关系：
 *   本模块生效期间，广播**不经过** rf0（pkt_sniff rf0 看不到广播是正常的，判据要用
 *   驱动打印的 `TX: ip=192.168.1.255 ...`）。若日后改成让内核转发广播，必须把
 *   BCAST_BRIDGE_ENABLE 置 0，否则同一个包会被送上射频两次。
 *
 * 已知残余风险（多板卡挂在同一广播域时，例如台架环境）：
 *   A 板把射频收到的广播中继到 LAN 上后，B 板会把这条中继帧当成普通广播再送一次射频，
 *   形成"二跳回环"。这里用**中继指纹环**抑制：本机刚中继出去的帧（内容完全相同）在
 *   1s 内再次被抓到就不再上射频。代价是每个节点最多多发一轮，之后收敛。
 *   注：指纹含 IP-ID 与载荷，PC 连续发的多包若 IP-ID 不同则不会被误抑制。
 ******************************************************************************/
#include <stdio.h>              /* fprintf / printf / perror */
#include <string.h>             /* memset / memcpy / strncpy */
#include <unistd.h>             /* close / sleep */
#include <errno.h>              /* errno / ENETDOWN */
#include <time.h>               /* time()：指纹窗口与日志限频 */
#include <sys/socket.h>         /* socket / bind / sendto / recvfrom */
#include <sys/ioctl.h>          /* ioctl / SIOCGIFINDEX */
#include <net/if.h>             /* struct ifreq / IFNAMSIZ */
#include <arpa/inet.h>          /* htons / ntohs */
#include <linux/if_packet.h>    /* sockaddr_ll / PACKET_OUTGOING */
#include <linux/if_ether.h>     /* ETH_P_ALL / ETH_P_IP */
#include <netinet/ip.h>         /* struct iphdr */

#include "user_declaration.h"   /* 全局量、宏、以及本模块对外接口声明 */

/* ========= 模块状态 ========= */
static int bcast_fd = -1;               /* AF_PACKET 套接字：上行收 + 下行发共用 */
static int bcast_ifindex = -1;          /* eth0 的 ifindex */
static volatile int bcast_ready = 0;    /* 1 = 已就绪，可收发 */

/* 二层广播目的 MAC（中继帧用；ff:ff:ff:ff:ff:ff 即以太网广播地址） */
static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ========= 中继指纹环（抑制多板卡同广播域的二跳回环） ========= */
typedef struct {
    uint32_t hash;      /* IP 包内容哈希（FNV-1a） */
    uint16_t len;       /* 包长 */
    time_t   ts;        /* 记录时间 */
} bcast_fp_t;

static bcast_fp_t bcast_fp_ring[BCAST_RELAY_RING];
static unsigned int bcast_fp_next = 0;                       /* 环形写指针 */
static pthread_mutex_t bcast_fp_lock = PTHREAD_MUTEX_INITIALIZER;

/* 本文件内的日志限频（每秒最多一条），避免风暴时刷屏 */
static int bcast_log_ok(time_t *last)
{
    time_t now = time(NULL);

    if (now == *last)
        return 0;
    *last = now;
    return 1;
}

/* FNV-1a 32 位哈希：内容完全相同（含 IP-ID 与载荷）才判为同一帧 */
static uint32_t bcast_hash(const uint8_t *p, size_t len)
{
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* 记下"本机刚中继出去的这一帧" */
static void bcast_fp_remember(const uint8_t *pkt, size_t len)
{
    bcast_fp_t *e;

    (void)pthread_mutex_lock(&bcast_fp_lock);
    e = &bcast_fp_ring[bcast_fp_next];
    e->hash = bcast_hash(pkt, len);
    e->len = (uint16_t)len;
    e->ts = time(NULL);
    bcast_fp_next = (bcast_fp_next + 1U) % BCAST_RELAY_RING;
    (void)pthread_mutex_unlock(&bcast_fp_lock);
}

/* 最近 1s 内有没有中继过内容完全相同的帧 */
static int bcast_fp_recent(const uint8_t *pkt, size_t len)
{
    uint32_t h = bcast_hash(pkt, len);
    time_t now = time(NULL);
    unsigned int i;
    int hit = 0;

    (void)pthread_mutex_lock(&bcast_fp_lock);
    for (i = 0; i < BCAST_RELAY_RING; i++) {
        const bcast_fp_t *e = &bcast_fp_ring[i];

        if (e->len != (uint16_t)len)
            continue;
        if (!e->ts || now - e->ts > (BCAST_RELAY_DEDUP_MS / 1000))   /* 超出窗口，忽略（time() 为秒级精度） */
            continue;
        if (e->hash == h) {
            hit = 1;
            break;
        }
    }
    (void)pthread_mutex_unlock(&bcast_fp_lock);
    return hit;
}

/* ========= 初始化 / 释放 ========= */
int bcast_socket_init(void)
{
    struct ifreq ifr;           /* 取 ifindex 用 */
    struct sockaddr_ll sll;     /* 绑定地址 */
    int on = 1;                 /* SO_BROADCAST 开关值 */

    if (!BCAST_BRIDGE_ENABLE) {
        printf("[INFO] broadcast bridge disabled by config\n");
        return -1;
    }

    /* 协议号必须用 ETH_P_ALL，不能用 ETH_P_IP：
     * 内核发送方向 xmit_one() -> dev_queue_xmit_nit() **只遍历 ptype_all 链表**，
     * 而只有协议号为 ETH_P_ALL 的 AF_PACKET 套接字才会被 dev_add_pack() 挂到该链表；
     * 用 ETH_P_IP 只能收到"入向"包（正是 pkt_sniff v1 踩过的坑）。
     * 代价是 IPv4 过滤要自己在用户态做（下面 ip->version != 4 处已处理）。 */
    bcast_fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
    if (bcast_fd < 0) {
        perror("socket(AF_PACKET)");
        return -1;
    }

    /* 中继帧目的 MAC 是广播地址，显式打开 SO_BROADCAST，避免发送被拒 */
    (void)setsockopt(bcast_fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, CAP_IFACE, IFNAMSIZ - 1);
    if (ioctl(bcast_fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("ioctl(SIOCGIFINDEX)");
        close(bcast_fd);
        bcast_fd = -1;
        return -1;
    }
    bcast_ifindex = ifr.ifr_ifindex;

    /* 绑定到 eth0：既收该网卡的二层帧，也用它发中继帧 */
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = bcast_ifindex;
    if (bind(bcast_fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("bind(AF_PACKET)");
        close(bcast_fd);
        bcast_fd = -1;
        return -1;
    }

    bcast_ready = 1;
    printf("[INFO] broadcast bridge enabled on %s (ifindex=%d)\n", CAP_IFACE, bcast_ifindex);
    return 0;
}

void bcast_socket_close(void)
{
    bcast_ready = 0;            /* 先置 0，避免中继路径继续用已关闭的 fd */
    if (bcast_fd >= 0) {
        close(bcast_fd);
        bcast_fd = -1;
    }
}

/* ========= 上行：eth0 二层广播 -> /dev/amp_ipi（node 255） ========= */
void *bcast_uplink_thread(void *arg)
{
    uint8_t buf[BCAST_BUF_MAX];
    time_t last_log = 0;        /* 本线程内所有打印共用限频 */

    (void)arg;

    if (!bcast_ready) {
        fprintf(stderr, "[WARN] bcast uplink thread not started (bridge not ready)\n");
        return NULL;
    }

    while (g_running) {
        struct sockaddr_ll from;            /* 记录包的来向与类型 */
        socklen_t from_len = sizeof(from);
        const struct iphdr *ip;
        ssize_t n;
        size_t tot;
        int udp_class;

        n = recvfrom(bcast_fd, buf, sizeof(buf), 0,
                     (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ENETDOWN) {        /* 网口临时 down：等一会儿重试，不退出线程 */
                sleep(1);
                continue;
            }
            perror("recvfrom(AF_PACKET)");
            break;
        }

        /* 只处理"别人发来的"帧：OUTGOING 是本机自己发出的（含本模块发出的中继帧），
         * 不排除就会自己喂自己形成回环 */
        if (from.sll_pkttype == PACKET_OUTGOING)
            continue;

        if ((size_t)n < sizeof(struct iphdr))
            continue;

        ip = (const struct iphdr *)buf;
        if (ip->version != 4)
            continue;                       /* ARP / IPv6 等一律忽略 */

        /* 只接管广播；单播继续走内核路由 -> rf0（那条路已实测正常） */
        if (ip->daddr != BROADCAST_IP_BE)
            continue;

        /* 以太网最小帧 60 字节：短帧会被 NIC 补 0，必须按 IP 头的 tot_len 裁剪，
         * 否则填充字节会被一起送进 AMP（实测 39 字节 IP 包会以 len=46 出现） */
        tot = (size_t)ntohs(ip->tot_len);
        if (tot < sizeof(struct iphdr) || tot > (size_t)n)
            continue;
        if (tot > MAX_PAYLOAD_SIZE)
            continue;

        /* 与 TUN 入口共用同一份端口准入策略，避免两条入口规则分叉 */
        udp_class = classify_udp_business_port(buf, tot);
        if (udp_class < 0) {
            continue;
        }

        /* 回环抑制：本机刚中继出去、现在又回来的同一帧，不再上射频 */
        if (bcast_fp_recent(buf, tot)) {
            if (bcast_log_ok(&last_log))
                fprintf(stderr, "[BRIDGE] drop looped frame len=%zu\n", tot);
            continue;
        }

        (void)amp_send_msg(BROADCAST_IP_BE, buf, tot);
    }

    return NULL;
}

/* ========= 下行：AMP 收到的广播 -> 在 eth0 上补发二层广播给本地 PC ========= */
void bcast_maybe_relay(const uint8_t *pkt, size_t len)
{
    const struct iphdr *ip;
    struct sockaddr_ll to;
    size_t tot;
    ssize_t w;
    static time_t last_log = 0;

    if (!bcast_ready || len < sizeof(struct iphdr))
        return;

    ip = (const struct iphdr *)pkt;
    if (ip->version != 4)
        return;
    if (ip->daddr != BROADCAST_IP_BE)
        return;                             /* 只中继广播，单播不经过这里 */

    tot = (size_t)ntohs(ip->tot_len);
    if (tot < sizeof(struct iphdr) || tot > len)
        return;

    /* 超过 eth0 MTU 时 AF_PACKET 发送会 EMSGSIZE，这里直接放弃并留痕 */
    if (tot > BCAST_RELAY_MAX_BYTES) {
        if (bcast_log_ok(&last_log))
            fprintf(stderr, "[BRIDGE] relay skipped: len=%zu > %d\n",
                    tot, BCAST_RELAY_MAX_BYTES);
        return;
    }

    /* SOCK_DGRAM + 显式目的 MAC：内核按 sll_protocol 填以太网类型、按 sll_addr 填目的 MAC，
     * 源 MAC 取 eth0 自身地址 —— 全程不查路由、不发 ARP */
    memset(&to, 0, sizeof(to));
    to.sll_family = AF_PACKET;
    to.sll_protocol = htons(ETH_P_IP);
    to.sll_ifindex = bcast_ifindex;
    to.sll_halen = 6;
    memcpy(to.sll_addr, bcast_mac, 6);

    w = sendto(bcast_fd, pkt, tot, 0, (struct sockaddr *)&to, sizeof(to));
    if (w != (ssize_t)tot) {
        if (bcast_log_ok(&last_log))
            perror("sendto(AF_PACKET relay)");
        return;
    }

    /* 记指纹：这一帧若又经射频回到本机，上行线程可据此识别并抑制 */
    bcast_fp_remember(pkt, tot);

    if (bcast_log_ok(&last_log))
        fprintf(stderr, "[BRIDGE] relay bcast to %s len=%zu\n", CAP_IFACE, tot);
}
