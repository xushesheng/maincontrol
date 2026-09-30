/************************************************************/
/*   组播链路维护模块（通信设备 <-> 指挥协同计算机）           */
/*   协议依据：《通信设备与指挥协同计算机通信协议20260912》     */
/*                                                            */
/*   角色：本机为「通信设备」（编号 0x35），对端为「指挥协同    */
/*   计算机」（编号 0x03）。收发使用不同的组播组：              */
/*     收：加入 224.5.1.13，绑定端口 8600                      */
/*     发：sendto 224.1.1.5 : 6200                             */
/*   字节序：全文小端（协议 1.2 明确"先传低位字节"）            */
/*                                                            */
/*   按协议已实现的报文：                                      */
/*     上行 0xA1 通信状态检测报文   每 1s，持续发送             */
/*     上行 0xA2 BIT 检测报文       每 5s（自检完成后）         */
/*     上行 0xA4 工作状态报文       每 1s（建链后）             */
/*     上行 0xA5 工作模式反馈报文   每 1s（建链后）             */
/*     下行 0x01 检测反馈报文       用于建链/断链判定           */
/*                                                            */
/*   【待后续确认后补充】                                       */
/*     上行 0xA3 战车身份信息反馈报文（需先收下行 0x03）        */
/*     下行 0x02 时间信息报文                                  */
/*     下行 0x03 战车身份信息报文                              */
/*     下行 0x04 宽带组网静默/辐射控制报文                      */
/*     下行 0x05 工作模式控制报文                              */
/*   当前这些报文被接收后会被校验过滤丢弃，不产生副作用。       */
/************************************************************/
#include <stdio.h>                 /* fprintf / perror / snprintf */
#include <string.h>                /* memset / memcpy */
#include <unistd.h>                /* close */
#include <errno.h>                 /* errno / EINTR / EAGAIN */
#include <fcntl.h>                 /* fcntl / O_NONBLOCK */
#include <poll.h>                  /* poll */
#include <time.h>                  /* clock_gettime / localtime_r */

#include <sys/socket.h>            /* socket / bind / sendto / recvfrom / setsockopt */
#include <net/if.h>                /* if_nametoindex */
#include <netinet/in.h>            /* sockaddr_in / ip_mreqn */
#include <arpa/inet.h>             /* inet_addr / htons */

#include "user_declaration.h"      /* 全局变量声明与函数声明 */

/* ========= 组播链路模块内部状态 ========= */
static int link_sockfd = -1;                    /* 组播 socket 文件描述符 */
static struct sockaddr_in link_peer_addr;       /* 发送目标：指挥协同计算机所属组播组 */
static uint32_t link_seq_detect = 1;            /* 0xA1 检测报文序号（从 1 开始，独立一套） */
static uint32_t link_seq_bit = 1;               /* 0xA2 BIT 检测报文序号 */
static uint32_t link_seq_work = 1;              /* 0xA4 工作状态报文序号 */
static uint32_t link_seq_mode_ack = 1;          /* 0xA5 工作模式反馈报文序号 */
static int link_up = 0;                         /* 建链标志：0=未建链，1=已建链 */
static uint64_t link_last_feedback_ms = 0;      /* 上次收到合法反馈报文的时刻（单调时钟） */

/***********************************
 *  取单调时钟毫秒数（不受校时影响） *
 **********************************/
static uint64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 序号自增：从 1 开始递增，溢出回绕后重新从 1 计数（协议"从1开始，满了重"） */
static uint32_t seq_inc(uint32_t seq)
{
    seq++;
    if (seq == 0)
        seq = 1;
    return seq;
}

/***********************************
 *      以小端写入 16 位字段         *
 **********************************/
static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

/***********************************
 *      以小端写入 32 位字段         *
 **********************************/
static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/***********************************
 *      以小端读取 16 位字段         *
 **********************************/
static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/******************************************************
 *  组 18 字节「通信状态检测报文」0xA1                   *
 *  布局：起始2 | 长度2 | 目的1 | 源1 | 类型1 | 序号4 |   *
 *        时1 | 分1 | 秒1 | 毫秒2 | 结束标志2            *
 *  时间戳取本机实时钟，毫秒由 tv_nsec 换算              *
 ******************************************************/
static size_t build_detect_frame(uint8_t *buf)
{
    struct timespec ts;                         /* 实时钟：含秒与纳秒 */
    struct tm tmv;                              /* 本地日历时间 */
    time_t now_sec;                             /* 秒级时间戳 */
    uint16_t ms;                                /* 毫秒部分 */

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        memset(&ts, 0, sizeof(ts));

    now_sec = ts.tv_sec;
    if (localtime_r(&now_sec, &tmv) == NULL)
        memset(&tmv, 0, sizeof(tmv));

    ms = (uint16_t)(ts.tv_nsec / 1000000);

    put_le16(buf + 0, LINK_FRAME_HEAD);             /* 起始标志 0xF00F */
    put_le16(buf + 2, LINK_FRAME_LEN_DETECT);       /* 报文长度 0x0012 = 18 */
    buf[4] = LINK_DEV_ID_PEER;                      /* 目的设备编号：指挥协同计算机 0x03 */
    buf[5] = LINK_DEV_ID_SELF;                      /* 源设备编号：通信设备 0x35 */
    buf[6] = LINK_TYPE_DETECT;                      /* 报文类型 0xA1 */
    put_le32(buf + 7, link_seq_detect);             /* 报文序号 */
    buf[11] = (uint8_t)tmv.tm_hour;                 /* 时 [0-23] */
    buf[12] = (uint8_t)tmv.tm_min;                  /* 分 [0-59] */
    buf[13] = (uint8_t)tmv.tm_sec;                  /* 秒 [0-59] */
    put_le16(buf + 14, ms);                         /* 毫秒 [0-999] */
    put_le16(buf + 16, LINK_FRAME_TAIL);            /* 结束标志 0x0EE0 */

    return LINK_FRAME_LEN_DETECT;
}

/******************************************************
 *  组 14 字节的上行报文（0xA2 / 0xA3 / 0xA4 / 0xA5）    *
 *  这几种报文结构一致，仅类型与末尾 1 字节数据不同：     *
 *    起始2 | 长度2 | 目的1 | 源1 | 类型1 | 序号4 |       *
 *    数据1 | 结束标志2                                  *
 ******************************************************/
static size_t build_uplink_data_frame(uint8_t *buf, uint8_t type, uint32_t seq, uint8_t data)
{
    put_le16(buf + 0, LINK_FRAME_HEAD);             /* 起始标志 0xF00F */
    put_le16(buf + 2, LINK_FRAME_LEN_BIT);          /* 报文长度 0x000E = 14（四者相同） */
    buf[4] = LINK_DEV_ID_PEER;                      /* 目的设备编号：指挥协同计算机 0x03 */
    buf[5] = LINK_DEV_ID_SELF;                      /* 源设备编号：通信设备 0x35 */
    buf[6] = type;                                  /* 报文类型 */
    put_le32(buf + 7, seq);                         /* 报文序号 */
    buf[11] = data;                                 /* 报文数据（1 字节） */
    put_le16(buf + 12, LINK_FRAME_TAIL);            /* 结束标志 0x0EE0 */

    return LINK_FRAME_LEN_BIT;
}

/******************************************************
 *  校验收到的 13 字节「通信状态检测反馈报文」0x01        *
 *  下行各报文靠「长度 + 类型 + 目的/源编号」区分，        *
 *  本函数严格匹配 0x01，其余下行报文会被过滤丢弃。        *
 *  返回 1 合法，0 非法                                 *
 ******************************************************/
static int is_valid_feedback(const uint8_t *pkt, size_t len)
{
    if (!pkt)
        return 0;
    if (len != LINK_FRAME_LEN_FEEDBACK)             /* 协议 1.1.2：长度不符即判出错 */
        return 0;
    if (get_le16(pkt + 0) != LINK_FRAME_HEAD)
        return 0;
    if (get_le16(pkt + 2) != LINK_FRAME_LEN_FEEDBACK)
        return 0;
    if (pkt[4] != LINK_DEV_ID_SELF)                 /* 目的必须是本机（通信设备 0x35） */
        return 0;
    if (pkt[5] != LINK_DEV_ID_PEER)                 /* 源必须是指挥协同计算机 0x03 */
        return 0;
    if (pkt[6] != LINK_TYPE_FEEDBACK)               /* 报文类型必须是 0x01 */
        return 0;
    if (get_le16(pkt + 11) != LINK_FRAME_TAIL)
        return 0;

    return 1;
}

/***********************************
 *  向指挥协同计算机组播组发送一帧   *
 **********************************/
static void link_sendto(const uint8_t *buf, size_t len)
{
    ssize_t n;

    n = sendto(link_sockfd, buf, len, 0,
               (struct sockaddr *)&link_peer_addr, sizeof(link_peer_addr));
    if (n < 0) {
        perror("sendto(link mcast)");
        return;
    }
    if (n != (ssize_t)len)
        fprintf(stderr, "[WARN] short sendto(link mcast): %zd/%zu\n", n, len);
}

/******************************************************
 *  创建组播 socket：加入本机组播组并绑定接收端口        *
 *  必设选项缺一不可：                                   *
 *    SO_REUSEADDR      允许同端口多 socket 绑定         *
 *    IP_ADD_MEMBERSHIP 不加则收不到任何组播             *
 *    IP_MULTICAST_IF   指定出口网卡，否则走默认路由      *
 *    IP_MULTICAST_TTL  限制在本地网段                   *
 *    IP_MULTICAST_LOOP 关回环，否则自发自收造成自激      *
 ******************************************************/
int link_socket_init(void)
{
    int on = 1;                                 /* setsockopt 开关值 */
    unsigned char ttl = 1;                      /* 组播 TTL：本地网段 */
    unsigned char loop = 0;                     /* 关闭组播回环 */
    int flags;                                  /* 文件状态标志（设非阻塞用） */
    struct sockaddr_in local_addr;              /* 本地绑定地址 */
    struct ip_mreqn mreq;                       /* 组播组成员关系 */
    struct in_addr local_ip;                    /* 本机网口 IPv4 */

    if (link_sockfd >= 0)
        return 0;                               /* 已经初始化过 */

    /* 取本机网口 IP：加入组播组与指定出口网卡都依赖它 */
    if (get_iface_ipv4(LINK_MCAST_IFNAME, &local_ip) != 0) {
        fprintf(stderr, "[ERROR] cannot get %s IPv4 for multicast link\n", LINK_MCAST_IFNAME);
        return -1;
    }

    link_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (link_sockfd < 0) {
        perror("socket(link mcast)");
        return -1;
    }

    if (setsockopt(link_sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
        perror("setsockopt(SO_REUSEADDR)");

    /* 绑定 INADDR_ANY + 本机组播端口：Linux 下如此可收到发往该端口的组播 */
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(LINK_MCAST_SELF_PORT);
    if (bind(link_sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind(link mcast)");
        close(link_sockfd);
        link_sockfd = -1;
        return -1;
    }

    /* 加入本机组播组，并用 imr_ifindex 显式指定网口，避免内核按路由表误选 */
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(LINK_MCAST_SELF_ADDR);
    mreq.imr_address.s_addr = local_ip.s_addr;
    mreq.imr_ifindex = (int)if_nametoindex(LINK_MCAST_IFNAME);
    if (setsockopt(link_sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt(IP_ADD_MEMBERSHIP)");
        close(link_sockfd);
        link_sockfd = -1;
        return -1;
    }

    if (setsockopt(link_sockfd, IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof(mreq)) < 0)
        perror("setsockopt(IP_MULTICAST_IF)");

    if (setsockopt(link_sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0)
        perror("setsockopt(IP_MULTICAST_TTL)");

    if (setsockopt(link_sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0)
        perror("setsockopt(IP_MULTICAST_LOOP)");

    /* 设为非阻塞：poll 返回可读后可把队列一次读空 */
    flags = fcntl(link_sockfd, F_GETFL, 0);
    if (flags >= 0)
        (void)fcntl(link_sockfd, F_SETFL, flags | O_NONBLOCK);

    /* 发送目标：指挥协同计算机所属组播组 */
    memset(&link_peer_addr, 0, sizeof(link_peer_addr));
    link_peer_addr.sin_family = AF_INET;
    link_peer_addr.sin_addr.s_addr = inet_addr(LINK_MCAST_PEER_ADDR);
    link_peer_addr.sin_port = htons(LINK_MCAST_PEER_PORT);

    fprintf(stderr, "[INFO] multicast link: recv %s:%d, send %s:%d, iface %s, devid 0x%02X\n",
            LINK_MCAST_SELF_ADDR, LINK_MCAST_SELF_PORT,
            LINK_MCAST_PEER_ADDR, LINK_MCAST_PEER_PORT,
            LINK_MCAST_IFNAME, LINK_DEV_ID_SELF);
    return 0;
}

/***********************************
 *         关闭组播 socket          *
 **********************************/
void link_socket_close(void)
{
    if (link_sockfd >= 0) {
        close(link_sockfd);
        link_sockfd = -1;
    }
}

/******************************************************
 *           组播链路维护线程                           *
 *  单线程 + poll(1000ms)：1s 超时既等报文又当发送节拍； *
 *  5s 周期的 BIT 检测报文用节拍计数取模实现。           *
 ******************************************************/
void *link_mcast_thread(void *arg)
{
    uint8_t rxbuf[128];                         /* 接收缓冲：最长报文 22 字节，余量充足 */
    uint8_t frame[LINK_FRAME_LEN_DETECT];       /* 发送缓冲：按最长的检测报文（18 字节）开 */
    uint8_t dev_status;                         /* 通信设备状态：0x33 / 0xAA */
    uint8_t rf_state;                           /* 静默辐射状态：0x00 / 0x01 */
    size_t flen;                                /* 待发送帧长度 */
    unsigned int tick;                          /* 1s 节拍计数（用于 5s 周期换算） */
    int cached;                                 /* 工作参数缓存是否有效 */
    struct pollfd pfd;                          /* poll 事件结构 */
    int rc;                                     /* poll 返回值 */

    (void)arg;
    tick = 0;

    while (g_running) {
        pfd.fd = link_sockfd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        rc = poll(&pfd, 1, LINK_PERIOD_MS);     /* 1s 超时既等报文又当发送节拍 */
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll(link mcast)");
            break;
        }

        /* ---- 接收：把当前可读数据一次读空 ---- */
        if (rc > 0 && (pfd.revents & POLLIN)) {
            while (1) {
                ssize_t n = recvfrom(link_sockfd, rxbuf, sizeof(rxbuf), 0, NULL, NULL);

                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;                  /* 已读空 */
                    if (errno == EINTR)
                        continue;
                    perror("recvfrom(link mcast)");
                    break;
                }

                if (is_valid_feedback(rxbuf, (size_t)n)) {
                    if (!link_up) {
                        link_up = 1;
                        fprintf(stderr, "[INFO] link established with command computer\n");
                    }
                    link_last_feedback_ms = now_ms();
                }
            }
        }

        tick++;

        /* 取一次缓存供本轮多个报文复用；失败表示 CPU1 尚未上报（自检未完成），
         * 此时 dev_status / rf_state 保持下面预置的默认值，避免使用未初始化值。 */
        dev_status = LINK_DEV_STATUS_NORMAL;
        rf_state = LINK_RF_SILENT;
        cached = (ctrl_get_work_status(&dev_status, &rf_state) == 0);

        /* ---- 0xA1 通信状态检测报文：持续每 1s 发送，与建链与否无关（协议 1.1.1） ---- */
        flen = build_detect_frame(frame);
        link_sendto(frame, flen);
        link_seq_detect = seq_inc(link_seq_detect);

        /* ---- 建链后追加 0xA4 与 0xA5 ---- */
        if (link_up) {
            /* 0xA4 工作状态报文：静默/辐射状态 */
            flen = build_uplink_data_frame(frame, LINK_TYPE_WORK, link_seq_work,
                                           cached ? rf_state : LINK_RF_SILENT);
            link_sendto(frame, flen);
            link_seq_work = seq_inc(link_seq_work);

            /* 0xA5 工作模式反馈报文：通信设备启用状态。
             * 启用状态的数据来源尚未确认，当前固定上报 0xAA（启用）。 */
            flen = build_uplink_data_frame(frame, LINK_TYPE_MODE_ACK, link_seq_mode_ack,
                                           LINK_DEV_ENABLED);
            link_sendto(frame, flen);
            link_seq_mode_ack = seq_inc(link_seq_mode_ack);
        }

        /* ---- 0xA2 BIT 检测报文：自检完成后每 5s（协议 1.2.1.2） ----
         * 以「是否已收到 CPU1 工作参数上报」作为自检完成的代理判据。 */
        if (cached && (tick % LINK_BIT_PERIOD_TICKS) == 0) {
            flen = build_uplink_data_frame(frame, LINK_TYPE_BIT, link_seq_bit, dev_status);
            link_sendto(frame, flen);
            link_seq_bit = seq_inc(link_seq_bit);
        }

        /* ---- 断链判定：连续 3s 未收到反馈（协议 1.1.1） ---- */
        if (link_up && (now_ms() - link_last_feedback_ms) > (uint64_t)LINK_LOST_TIMEOUT_MS) {
            link_up = 0;
            fprintf(stderr, "[WARN] link lost: no feedback for %d ms\n", LINK_LOST_TIMEOUT_MS);
        }
    }

    return NULL;
}
