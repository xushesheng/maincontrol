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
/*     下行 0x02 时间信息报文       系统校时 + 0x19 下发路由     */
/*                                                            */
/*   0x02 时间信息报文的处理（22 字节，字段为二进制小端）：      */
/*     年u16@11 | 月@13 | 日@14 | 时@15 | 分@16 | 秒@17 |       */
/*     毫秒u16@18 | 结束标志@20                                */
/*     口径：报文携带 BDT（北斗时）= UTC + 4s，故 UTC = BDT - 4  */
/*     动作：settimeofday 校时 → hwclock --systohc 写 RTC →     */
/*           调 user_control.c 的 clock_send_now() 发 0x19      */
/*     节流：首次立即；之后每 LINK_TIME_SYNC_PERIOD_MS(60s)      */
/*           最多一次（用单调时钟计时，免疫自身校时跳变）        */
/*     年份按约定不校验，仅校验月/日/时/分/秒/毫秒取值范围。     */
/*                                                            */
/*   【待后续确认后补充】                                       */
/*     上行 0xA3 战车身份信息反馈报文（需先收下行 0x03）        */
/*     下行 0x03 战车身份信息报文                              */
/*     下行 0x04 宽带组网静默/辐射控制报文                      */
/*     下行 0x05 工作模式控制报文                              */
/*   当前这些报文被接收后会被校验过滤丢弃，不产生副作用。       */
/************************************************************/
#include <stdio.h>                 /* fprintf / perror / snprintf */
#include <stdlib.h>                /* system（校时后写硬件 RTC） */
#include <string.h>                /* memset / memcpy */
#include <unistd.h>                /* close */
#include <errno.h>                 /* errno / EINTR / EAGAIN */
#include <fcntl.h>                 /* fcntl / O_NONBLOCK */
#include <poll.h>                  /* poll */
#include <time.h>                  /* clock_gettime / localtime_r / time_t */

#include <sys/time.h>              /* gettimeofday / settimeofday / struct timeval */

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
static int link_time_synced = 0;                /* 是否已用 0x02 校时过：0=尚未（收到即立即同步） */
static uint64_t link_last_timesync_ms = 0;      /* 上次真正执行校时的时刻（单调时钟，用于节流） */

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

/******************************************************
 *  取某年某月的天数（1~31）；月份非法返回 0            *
 *  闰年规则：能被 4 整除且不被 100 整除，或能被 400 整除 *
 ******************************************************/
static int days_in_month(int y, int m)
{
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (m < 1 || m > 12)
        return 0;
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return dim[m - 1];
}

/******************************************************
 *  公历日期 -> 距 1970-01-01 的天数（Howard Hinnant）  *
 *  纯整数运算，不依赖 TZ 环境变量与 tzdata。            *
 *  不用 mktime / timegm 的原因（三者都会踩坑）：         *
 *    ① mktime 按本地时区解释，受 TZ 影响；              *
 *    ② timegm 是 GNU 扩展，在 uclibc/musl 与不同        *
 *       _DEFAULT_SOURCE 下可用性不稳；                  *
 *    ③ 最致命：两者都会**静默归一化越界字段**            *
 *       （mon=13 会被折成次年 1 月），恰好把本该拦下的   *
 *       非法报文放行。                                  *
 ******************************************************/
static long days_from_civil(int y, int m, int d)
{
    int era;                                    /* 400 年一个"纪元" */
    int yoe;                                    /* 纪元内的年序号 [0, 399] */
    int doy;                                    /* 年内天序号 [0, 365]，以 3 月为年首 */
    int doe;                                    /* 纪元内天序号 [0, 146096] */

    y -= (m <= 2);                              /* 把 1、2 月算作上一年的第 13、14 月 */
    era = (y >= 0 ? y : y - 399) / 400;         /* 保证被除数非负，规避不同实现截断方向差异 */
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return (long)era * 146097L + (long)doe - 719468L;   /* 719468 = 0000-03-01 到 1970-01-01 的天数 */
}

/******************************************************
 *  校验收到的 22 字节「时间信息报文」0x02（仅帧结构）    *
 *  字段取值范围另在 handle_time_frame() 里校验。        *
 *  返回 1 合法，0 非法                                 *
 ******************************************************/
static int is_valid_time(const uint8_t *pkt, size_t len)
{
    if (!pkt)
        return 0;
    if (len != LINK_FRAME_LEN_TIME)             /* 协议 1.1.2：长度不符即判出错 */
        return 0;
    if (get_le16(pkt + 0) != LINK_FRAME_HEAD)
        return 0;
    if (get_le16(pkt + 2) != LINK_FRAME_LEN_TIME)
        return 0;
    if (pkt[4] != LINK_DEV_ID_SELF)             /* 目的必须是本机（通信设备 0x35） */
        return 0;
    if (pkt[5] != LINK_DEV_ID_PEER)             /* 源必须是指挥协同计算机 0x03 */
        return 0;
    if (pkt[6] != LINK_TYPE_TIME)               /* 报文类型必须是 0x02 */
        return 0;
    if (get_le16(pkt + 20) != LINK_FRAME_TAIL)
        return 0;

    return 1;
}

/******************************************************
 *  处理下行 0x02「时间信息报文」：                      *
 *    解析 → 字段校验 → 节流 → settimeofday →           *
 *    hwclock 写 RTC → 发 0x19 给路由                   *
 *  报文携带 BDT（北斗时）= UTC + 4s，故先减 4 秒再校时。 *
 *  任何一步失败都只告警、不重试，等下一次 0x02 再补。    *
 ******************************************************/
static void handle_time_frame(const uint8_t *pkt)
{
    int year;                                   /* 年（二进制，如 2026） */
    int mon, mday, hour, min, sec, ms;          /* 月日时分秒毫秒 */
    long days;                                  /* 距 1970-01-01 的天数 */
    time_t epoch;                               /* 待写入的 UTC 秒级时间戳 */
    struct timeval tv;                          /* settimeofday 入参 */
    struct timeval old;                         /* 校时前的系统时间（仅用于打印跳变量） */
    uint64_t now;                               /* 当前单调时钟毫秒数 */

    year = (int)get_le16(pkt + 11);
    mon  = pkt[13];
    mday = pkt[14];
    hour = pkt[15];
    min  = pkt[16];
    sec  = pkt[17];
    ms   = (int)get_le16(pkt + 18);

    /* 字段取值范围校验（年份按约定不校验，只拦明显的月/日/时/分/秒/毫秒越界） */
    if (mon < 1 || mon > 12 ||
        mday < 1 || mday > days_in_month(year, mon) ||
        hour > 23 || min > 59 || sec > 59 || ms > 999) {
        fprintf(stderr, "[WARN] 0x02 time frame rejected: bad field "
                        "y=%d m=%d d=%d %02d:%02d:%02d.%03d\n",
                year, mon, mday, hour, min, sec, ms);
        return;
    }

    /* 节流：首次（link_time_synced==0）立即同步；之后每 60s 最多一次。
     * 用单调时钟计时——若用 REALTIME，前跳会让窗口瞬时分到期（变成每包都校时），
     * 后跳会让窗口永远不到期（彻底停摆），而 REALTIME 恰恰是本函数自己改的。 */
    now = now_ms();
    if (link_time_synced &&
        (now - link_last_timesync_ms) < (uint64_t)LINK_TIME_SYNC_PERIOD_MS)
        return;

    days  = days_from_civil(year, mon, mday);
    epoch = (time_t)days * 86400 + (time_t)hour * 3600 + (time_t)min * 60 + (time_t)sec;
    epoch -= LINK_TIME_BDT_OFFSET_SEC;          /* BDT -> UTC：减 4 秒 */

    if (gettimeofday(&old, NULL) == 0 && (long)(epoch - old.tv_sec) != 0)
        fprintf(stderr, "[INFO] system time will jump %ld s by 0x02\n",
                (long)(epoch - old.tv_sec));

    tv.tv_sec  = epoch;
    tv.tv_usec = ms * 1000;                     /* ms<=999 ⇒ tv_usec<=999000，合法 */
    if (settimeofday(&tv, NULL) != 0) {         /* 第二参数必须 NULL；需 CAP_SYS_TIME（root） */
        perror("settimeofday");
        return;                                 /* 校时失败则不写 RTC、也不发 0x19 */
    }

    link_time_synced = 1;
    link_last_timesync_ms = now;
    fprintf(stderr, "[INFO] system time synced by 0x02: %04d-%02d-%02d %02d:%02d:%02d.%03d "
                    "(BDT, -%ds => UTC)\n",
            year, mon, mday, hour, min, sec, ms, LINK_TIME_BDT_OFFSET_SEC);

#if LINK_TIME_HWCLK_SYNC
    /* 同步到硬件 RTC，使重启后时间保持。失败仅告警。
     * 注意：system() 会 fork 并可能阻塞百毫秒级，每 60s 才一次，可接受；
     * 若不能接受把 LINK_TIME_HWCLK_SYNC 置 0 即可关闭。 */
    if (system(LINK_TIME_HWCLK_CMD " >/dev/null 2>&1") != 0)
        fprintf(stderr, "[WARN] %s failed\n", LINK_TIME_HWCLK_CMD);
#endif

    /* 必须在 settimeofday 之后调用：build_clock_frame() 取的是调用瞬间的 time(NULL)，
     * 早于校时则会把旧时间发给路由。 */
    if (clock_send_now() != 0)
        fprintf(stderr, "[WARN] clock frame(0x19) not sent after time sync\n");
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
                } else if (is_valid_time(rxbuf, (size_t)n)) {
                    /* 0x02 处理不依赖 link_up：即使尚未建链也接受校时 */
                    handle_time_frame(rxbuf);
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
