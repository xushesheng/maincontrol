/**************************/
/*   控制面收发与透传模块   */
/**************************/
#include <stdio.h>                 /* fprintf / perror */
#include <string.h>                /* memset / memcpy */
#include <unistd.h>                /* close / sleep */
#include <time.h>                   /* time / localtime_r（开机时钟下发用） */
#include <errno.h>                 /* errno / EINTR */
#include <stddef.h>                /* offsetof / size_t */
#include <stdbool.h>               /* bool / true / false */
#include <pthread.h>               /* POSIX 线程与互斥锁 */
#include <sys/socket.h>            /* socket / bind / sendto / recvfrom */
#include <sys/ioctl.h>             /* ioctl */
#include <arpa/inet.h>             /* inet_ntoa / htons */
#include <linux/if.h>              /* IFNAMSIZ / struct ifreq */

#include "user_declaration.h"      /* 全局变量声明与函数声明 */

/* ========= 控制帧类型常量（协议定义） ========= */
#define CTRL_FRAME_TYPE_ACK          0x30    /* 应答帧类型 */
#define CTRL_FRAME_TYPE_NACK         0x40    /* 否定应答帧类型 */
#define CTRL_FRAME_TYPE_VERSION      0x10    /* 版本上报帧类型 */
#define CTRL_FRAME_TYPE_WORK_PARAM   0x20    /* 工作参数上报帧类型 */

/* 注：新协议已删除头区域第二字节的"计数"字段，改为"保留(0x00)"。
 * 因此不再定义 CTRL_FRAME_CNT_* 计数值；帧类型仅凭 pkt[10] 的"类型"字节区分。 */

/* 控制帧类型枚举：用于 classify_control_frame 的返回值 */
typedef enum {
    CTRL_FRAME_UNKNOWN = 0,         /* 未知/非法帧 */
    CTRL_FRAME_ACK,                 /* 应答帧 */
    CTRL_FRAME_NACK,                /* 否定应答帧 */
    CTRL_FRAME_VERSION_REPORT,      /* 版本上报帧 */
    CTRL_FRAME_WORK_PARAM_REPORT,   /* 工作参数上报帧 */
} ctrl_frame_kind_t;

#pragma pack(push, 1)
typedef struct {
    uint16_t frameHead;
    uint16_t frameRetain;
    uint16_t GoalId;
    uint16_t SourceID;
    uint16_t synchronizing;
    uint8_t frameType;
    uint8_t frameReserve;               /* 头区域第二字节：新协议为"保留"，原"计数"字段已删除 */
    uint8_t WEBVersion;
    uint8_t MCVersion;
    uint8_t NETVersion;
    uint8_t SPCLVersion;
    uint8_t JDCLVersion;
    uint8_t frameEnd;
} ctrl_version_report_t;

typedef struct {
    uint16_t frameHead;
    uint16_t frameRetain;
    uint16_t GoalId;
    uint16_t SourceID;
    uint16_t synchronizing;
    uint8_t frameType;
    uint8_t frameReserve;               /* 头区域第二字节：新协议为"保留"，原"计数"字段已删除 */
    uint8_t SiteAttribute;
    uint8_t NodeName;
    uint8_t NodeID;
    uint8_t IPB1datecode;
    uint8_t IPB2datecode;
    uint8_t IPB3datecode;
    uint8_t IPB4datecode;
    uint8_t IPYMB1datecode;
    uint8_t IPYMB2datecode;
    uint8_t IPYMB3datecode;
    uint8_t IPYMB4datecode;
    uint8_t WGDZB1datecode;
    uint8_t WGDZB2datecode;
    uint8_t WGDZB3datecode;
    uint8_t WGDZB4datecode;
    uint16_t HopRate;
    uint8_t SynSignal;
    uint8_t LinkQuality;
    uint8_t FaultSignal;
    uint8_t Silent;
    uint8_t AllSlient;
    uint8_t ChannelInfo;
    uint16_t ChannelTemp;
    uint8_t ChannelVolt;
    uint8_t ChannelElect;
    uint8_t RFInfo;
    uint16_t RFTemp;
    uint8_t RFVolt;
    uint8_t RFElect;
    uint8_t BasedInfo;
    uint16_t BasedTemp;
    uint8_t BasedVolt;
    uint8_t BasedElect;
    uint8_t Power1Info;
    uint16_t Power1Temp;
    uint8_t Power1Volt;
    uint8_t Power1Elect;
    uint8_t Power2Info;
    uint16_t Power2Temp;
    uint8_t Power2Volt;
    uint8_t Power2Elect;
    uint8_t PowerModuleInfo;       /* 电源模块状态信息（BCD，0x01 正常/0x02 故障超温） */
    uint16_t PowerModuleTemp;      /* 电源模块运行温度信息（2 字节，同上温度格式） */
    uint8_t PowerModuleVolt;       /* 电源模块运行电压信息（BCD） */
    uint8_t PowerModuleElect;      /* 电源模块运行电流信息（BCD，单位 mA） */
    uint8_t BandwidthSet;
    uint8_t PowerSet;
    uint8_t Encryption;
    uint8_t WorkMode;
    uint32_t FixedFrequency;
    uint32_t AdaHopMinFre;
    uint32_t AdaHopMaxFre;
    uint8_t NotAdaHopFre;
    uint8_t ComNetName;
    uint32_t MinFreThreshold;
    uint32_t MaxFreThreshold;
    uint8_t Modulation;
    uint8_t OnlineNodeSum;
    uint8_t ComDataSum;
    uint8_t ComDataBER;
    uint8_t ComDataPLP;
    uint8_t NodeConnect[128];   /* 节点连接关系：32 节点 × 4B/节点，每行 32bit 表示该节点与 1~32 号节点的连接（含自连接）；bit=1 连接、bit=0 断开，字节内 MSB 在前 */
    uint8_t frameEnd;
} ctrl_work_param_report_t;
#pragma pack(pop)

typedef struct {
    int valid;
    size_t len;
    ctrl_version_report_t frame;
    uint8_t raw[sizeof(ctrl_version_report_t)];
} ctrl_version_cache_t;

typedef struct {
    int valid;
    size_t len;
    ctrl_work_param_report_t frame;
    uint8_t raw[sizeof(ctrl_work_param_report_t)];
} ctrl_work_param_cache_t;

/* ========= 控制面全局状态 ========= */
static int control_sockfd = -1;                                 /* UDP socket 文件描述符 */
static pthread_mutex_t control_cache_lock = PTHREAD_MUTEX_INITIALIZER;  /* 保护缓存的互斥锁（静态初始化） */
static struct sockaddr_in control_report_peer;                  /* 控制上报目标地址（本机 IP：3419） */
static ctrl_version_cache_t last_version_report;                /* 最近一次版本上报帧缓存 */
static ctrl_work_param_cache_t last_work_param_report;          /* 最近一次工作参数上报帧缓存 */

/***********************************
 * 从控制帧字节流里读 16 位小端字段 *
 * 注：控制帧头区域统一小端（与 TX 侧 ctrl_write_le16 一致）
 **********************************/
static uint16_t ctrl_read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/***********************************
 * 从控制帧字节流里读 32 位小端字段 *
 * 注：控制帧头区域统一小端（频率等 32 位字段同理）
 **********************************/
static uint32_t ctrl_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/***********************************
 *          异或校验计算函数         *
 * 协议：校验和 = 头区域(类型+保留) 与 数据域 每个字节按位异或。
 * 本帧布局：pkt[10]=类型、pkt[11]=保留、pkt[12..] = 数据域、pkt[len-1]=校验和。
 * 故从 pkt[10] 异或至 pkt[len-2]，结果应与 pkt[len-1] 相等。
 **********************************/
static unsigned char ctrl_xor_checksum(const uint8_t *pkt, size_t len)
{
    size_t i;
    unsigned char checksum;

    checksum = pkt[10];             //先取第十个字节
    for (i = 11; i < len - 1; i++)  //再从第十个字节一直异或到倒数第二个字节
        checksum ^= pkt[i];
    return checksum;
}


/***********************************
 *   校验和与数据帧长度是否合理     *
 **********************************/
static bool ctrl_frame_is_valid(const uint8_t *pkt, size_t len)
{
    if (!pkt || len < 13 || len > MAX_PAYLOAD_SIZE)
        return false;
    return ctrl_xor_checksum(pkt, len) == pkt[len - 1];
}

/***********************************
 *   固定上报目标为本机IP + UDP 3419  *
 **********************************/
static int init_control_report_peer(void)
{
    struct in_addr local_ip;

    if (get_iface_ipv4(CAP_IFACE, &local_ip) != 0) {
        fprintf(stderr, "[ERROR] cannot get %s IPv4 for control report target\n", CAP_IFACE);
        return -1;
    }

    memset(&control_report_peer, 0, sizeof(control_report_peer));
    control_report_peer.sin_family = AF_INET;
    control_report_peer.sin_addr = local_ip;
    control_report_peer.sin_port = htons(CONTROL_REPORT_PORT);
    return 0;
}

/***********************************
 *       判断CPU1上报控制帧类型      *
 **********************************/
static ctrl_frame_kind_t classify_control_frame(const uint8_t *pkt, size_t len)
{
    if (!pkt || len < 13)
        return CTRL_FRAME_UNKNOWN;

    /* 新协议头区域第二字节为"保留"，不再携带计数值，故仅按"类型"字节(pkt[10])区分帧类型 */
    if (pkt[10] == CTRL_FRAME_TYPE_ACK)
        return CTRL_FRAME_ACK;
    if (pkt[10] == CTRL_FRAME_TYPE_NACK)
        return CTRL_FRAME_NACK;
    if (pkt[10] == CTRL_FRAME_TYPE_VERSION)
        return CTRL_FRAME_VERSION_REPORT;
    if (pkt[10] == CTRL_FRAME_TYPE_WORK_PARAM)
        return CTRL_FRAME_WORK_PARAM_REPORT;

    return CTRL_FRAME_UNKNOWN;
}

/***********************************
 *        解析CPU1版本上报帧         *
  **********************************/
static void parse_version_report(ctrl_version_report_t *frame, const uint8_t *pkt)
{
    size_t offset = 0;

#define CTRL_PARSE_U8(field) \
    do {                     \
        frame->field = pkt[offset++]; \
    } while (0)
#define CTRL_PARSE_LE16(field)                \
    do {                                      \
        frame->field = ctrl_read_le16(pkt + offset); \
        offset += 2;                          \
    } while (0)

    CTRL_PARSE_LE16(frameHead);
    CTRL_PARSE_LE16(frameRetain);
    CTRL_PARSE_LE16(GoalId);
    CTRL_PARSE_LE16(SourceID);
    CTRL_PARSE_LE16(synchronizing);
    CTRL_PARSE_U8(frameType);
    CTRL_PARSE_U8(frameReserve);
    CTRL_PARSE_U8(WEBVersion);
    CTRL_PARSE_U8(MCVersion);
    CTRL_PARSE_U8(NETVersion);
    CTRL_PARSE_U8(SPCLVersion);
    CTRL_PARSE_U8(JDCLVersion);
    CTRL_PARSE_U8(frameEnd);

#undef CTRL_PARSE_LE16
#undef CTRL_PARSE_U8
}

/***********************************
 *      解析CPU1工作参数上报帧       *
 **********************************/
static void parse_work_param_report(ctrl_work_param_report_t *frame, const uint8_t *pkt)
{
    size_t offset = 0;

#define CTRL_PARSE_U8(field) \
    do {                     \
        frame->field = pkt[offset++]; \
    } while (0)
#define CTRL_PARSE_LE16(field)                \
    do {                                      \
        frame->field = ctrl_read_le16(pkt + offset); \
        offset += 2;                          \
    } while (0)
#define CTRL_PARSE_LE32(field)                \
    do {                                      \
        frame->field = ctrl_read_le32(pkt + offset); \
        offset += 4;                          \
    } while (0)

    CTRL_PARSE_LE16(frameHead);
    CTRL_PARSE_LE16(frameRetain);
    CTRL_PARSE_LE16(GoalId);
    CTRL_PARSE_LE16(SourceID);
    CTRL_PARSE_LE16(synchronizing);
    CTRL_PARSE_U8(frameType);
    CTRL_PARSE_U8(frameReserve);
    CTRL_PARSE_U8(SiteAttribute);
    CTRL_PARSE_U8(NodeName);
    CTRL_PARSE_U8(NodeID);
    CTRL_PARSE_U8(IPB1datecode);
    CTRL_PARSE_U8(IPB2datecode);
    CTRL_PARSE_U8(IPB3datecode);
    CTRL_PARSE_U8(IPB4datecode);
    CTRL_PARSE_U8(IPYMB1datecode);
    CTRL_PARSE_U8(IPYMB2datecode);
    CTRL_PARSE_U8(IPYMB3datecode);
    CTRL_PARSE_U8(IPYMB4datecode);
    CTRL_PARSE_U8(WGDZB1datecode);
    CTRL_PARSE_U8(WGDZB2datecode);
    CTRL_PARSE_U8(WGDZB3datecode);
    CTRL_PARSE_U8(WGDZB4datecode);
    CTRL_PARSE_LE16(HopRate);
    CTRL_PARSE_U8(SynSignal);
    CTRL_PARSE_U8(LinkQuality);
    CTRL_PARSE_U8(FaultSignal);
    CTRL_PARSE_U8(Silent);
    CTRL_PARSE_U8(AllSlient);
    CTRL_PARSE_U8(ChannelInfo);
    CTRL_PARSE_LE16(ChannelTemp);
    CTRL_PARSE_U8(ChannelVolt);
    CTRL_PARSE_U8(ChannelElect);
    CTRL_PARSE_U8(RFInfo);
    CTRL_PARSE_LE16(RFTemp);
    CTRL_PARSE_U8(RFVolt);
    CTRL_PARSE_U8(RFElect);
    CTRL_PARSE_U8(BasedInfo);
    CTRL_PARSE_LE16(BasedTemp);
    CTRL_PARSE_U8(BasedVolt);
    CTRL_PARSE_U8(BasedElect);
    CTRL_PARSE_U8(Power1Info);
    CTRL_PARSE_LE16(Power1Temp);
    CTRL_PARSE_U8(Power1Volt);
    CTRL_PARSE_U8(Power1Elect);
    CTRL_PARSE_U8(Power2Info);
    CTRL_PARSE_LE16(Power2Temp);
    CTRL_PARSE_U8(Power2Volt);
    CTRL_PARSE_U8(Power2Elect);
    CTRL_PARSE_U8(PowerModuleInfo);
    CTRL_PARSE_LE16(PowerModuleTemp);
    CTRL_PARSE_U8(PowerModuleVolt);
    CTRL_PARSE_U8(PowerModuleElect);
    CTRL_PARSE_U8(BandwidthSet);
    CTRL_PARSE_U8(PowerSet);
    CTRL_PARSE_U8(Encryption);
    CTRL_PARSE_U8(WorkMode);
    CTRL_PARSE_LE32(FixedFrequency);
    CTRL_PARSE_LE32(AdaHopMinFre);
    CTRL_PARSE_LE32(AdaHopMaxFre);
    CTRL_PARSE_U8(NotAdaHopFre);
    CTRL_PARSE_U8(ComNetName);
    CTRL_PARSE_LE32(MinFreThreshold);
    CTRL_PARSE_LE32(MaxFreThreshold);
    CTRL_PARSE_U8(Modulation);
    CTRL_PARSE_U8(OnlineNodeSum);
    CTRL_PARSE_U8(ComDataSum);
    CTRL_PARSE_U8(ComDataBER);
    CTRL_PARSE_U8(ComDataPLP);
    /* 节点连接关系段：128 字节（32 节点 × 4B/节点，含自连接） */
    for (size_t i = 0; i < 128; i++)
        CTRL_PARSE_U8(NodeConnect[i]);
    CTRL_PARSE_U8(frameEnd);

#undef CTRL_PARSE_LE32
#undef CTRL_PARSE_LE16
#undef CTRL_PARSE_U8
}

/***********************************
 *         缓存CPU1版本上报          *
 **********************************/
static void cache_version_report(const uint8_t *pkt, size_t len)
{
    pthread_mutex_lock(&control_cache_lock);
    memset(&last_version_report, 0, sizeof(last_version_report));
    last_version_report.valid = 1;
    last_version_report.len = len;
    parse_version_report(&last_version_report.frame, pkt);
    memcpy(last_version_report.raw, pkt, len);
    pthread_mutex_unlock(&control_cache_lock);
}

/***********************************
 *       缓存CPU1工作参数上报        *
 **********************************/
static void cache_work_param_report(const uint8_t *pkt, size_t len)
{
    pthread_mutex_lock(&control_cache_lock);
    memset(&last_work_param_report, 0, sizeof(last_work_param_report));
    last_work_param_report.valid = 1;
    last_work_param_report.len = len;
    parse_work_param_report(&last_work_param_report.frame, pkt);
    memcpy(last_work_param_report.raw, pkt, len);
    pthread_mutex_unlock(&control_cache_lock);
}

/***********************************
 *    仅缓存CPU1上报的版本/工作参数   *
 **********************************/
static void cache_cpu1_report_frame(const uint8_t *pkt, size_t len)
{
    ctrl_frame_kind_t kind;

    if (!ctrl_frame_is_valid(pkt, len)) {
        fprintf(stderr, "[WARN] CPU1 report control frame checksum invalid, skip cache\n");
        return;
    }

    kind = classify_control_frame(pkt, len);
    if (kind == CTRL_FRAME_VERSION_REPORT) {
        if (len != sizeof(ctrl_version_report_t)) {
            fprintf(stderr, "[WARN] version report len mismatch: %zu/%zu\n",
                    len, sizeof(ctrl_version_report_t));
            return;
        }
        cache_version_report(pkt, len);
        return;
    }

    if (kind == CTRL_FRAME_WORK_PARAM_REPORT) {
        if (len != sizeof(ctrl_work_param_report_t)) {
            fprintf(stderr, "[WARN] work-param report len mismatch: %zu/%zu\n",
                    len, sizeof(ctrl_work_param_report_t));
            return;
        }
        cache_work_param_report(pkt, len);
    }
}

/***********************************
 *      把控制数据写到amp_ctrl       *
 **********************************/
static int write_ctrl_msg(const struct amp_ctrl_msg *msg, size_t msg_bytes)
{
    while (1) {
        ssize_t written = write(ctrl_fd, msg, msg_bytes);

        if (written == (ssize_t)msg_bytes)
            return 0;
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0) {
            perror("write(ctrl_fd)");
            return -1;
        }
        fprintf(stderr, "[ERROR] short write(ctrl_fd): %zd/%zu\n", written, msg_bytes);
        return -1;
    }
}

/******************************************
 *      开机自启动时钟下发（协议类型 0x19） *
 ****************************************/

/* 十进制转 BCD：例如 59 -> 0x59（协议时间字段均为 BCD 码） */
static uint8_t dec_to_bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10) << 4) | (dec % 10));
}

/* 把 16 位字段以小端写入缓冲区（协议头字段按小端发送，与路由固件对齐） */
static void ctrl_write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

/***********************************
 *   组 0x19 时钟下发帧（主控->路由）  *
 * 帧布局（大端 16 位头字段）：         *
 *   长度2(含长度字段自身=20)           *
 *   保留2(0x0000)                      *
 *   目的ID 0x0C00 / 源ID 0x0A00        *
 *   同步序列 0xFFF5                    *
 *   类型 0x19 / 保留 0x00              *
 *   年/月/星期/日/时/分/秒（BCD 各1B）  *
 *   校验和1（头区域+数据域逐字节异或）   *
 * 返回 0 成功，<0 失败。               *
 **********************************/
static int build_clock_frame(uint8_t *buf, size_t *len)
{
    struct tm tmv;
    time_t now;
    int wday_iso;       /* ISO 星期：周一=1 .. 周日=7 */

    if (!buf || !len)
        return -1;

    now = time(NULL);
    if (localtime_r(&now, &tmv) == NULL)
        return -1;

    /* struct tm 的 tm_wday：0=周日..6=周六；转 ISO：1=周一..7=周日 */
    wday_iso = (tmv.tm_wday == 0) ? 7 : tmv.tm_wday;

    memset(buf, 0, 20);

    ctrl_write_le16(buf + 0, 20);                  /* 长度（含自身 = 20），小端 */
    ctrl_write_le16(buf + 2, 0x0000);             /* 保留，小端 */
    ctrl_write_le16(buf + 4, SRIO_ID_ROUTER);     /* 目的 ID = 路由，小端 */
    ctrl_write_le16(buf + 6, SRIO_ID_MASTER);     /* 源 ID = 主控，小端 */
    ctrl_write_le16(buf + 8, CLOCK_SYNC_MAGIC);   /* 同步序列 0xFFF5，小端 */
    buf[10] = CTRL_FRAME_TYPE_CLOCK;              /* 类型 0x19 */
    buf[11] = 0x00;                               /* 头区域第二字节：保留 */

    /* 时间字段：均为 BCD（年取两位，如 2026 -> 0x26） */
    buf[12] = dec_to_bcd((uint8_t)((tmv.tm_year + 1900) % 100));  /* 年 */
    buf[13] = dec_to_bcd((uint8_t)(tmv.tm_mon + 1));              /* 月 1~12 */
    buf[14] = dec_to_bcd((uint8_t)wday_iso);                     /* 星期 1~7 */
    buf[15] = dec_to_bcd((uint8_t)(tmv.tm_mday));                /* 日 1~31 */
    buf[16] = dec_to_bcd((uint8_t)(tmv.tm_hour));                /* 时 0~23 */
    buf[17] = dec_to_bcd((uint8_t)(tmv.tm_min));                 /* 分 0~59 */
    buf[18] = dec_to_bcd((uint8_t)(tmv.tm_sec));                 /* 秒 0~59 */

    /* 校验和 = 头区域(类型+保留) 与 数据域 逐字节异或，复用现有校验函数 */
    buf[19] = ctrl_xor_checksum(buf, 20);

    *len = 20;
    return 0;
}

/***********************************
 *  开机一次性下发 0x19 时钟帧给路由   *
 *  带初始延时 + 有限重试；失败不致命  *
 **********************************/
int clock_send_on_boot(void)
{
    int attempt;
    uint8_t frame[20];
    size_t flen;
    struct amp_ctrl_msg msg;
    size_t msg_bytes;

    if (build_clock_frame(frame, &flen) != 0) {
        fprintf(stderr, "[ERROR] build clock frame(0x19) failed\n");
        return -1;
    }

    /* 初始延时：给路由固件留出启动时间，避免 TX 单槽尚未释放导致 -EBUSY */
    sleep(CLOCK_BOOT_DELAY_SEC);

    for (attempt = 0; attempt < CLOCK_BOOT_RETRY; attempt++) {
        memset(&msg, 0, sizeof(msg));
        msg.len = (uint32_t)flen;
        msg.data_type = 1;
        memcpy(msg.data, frame, flen);
        msg_bytes = offsetof(struct amp_ctrl_msg, data) + flen;

        if (write_ctrl_msg(&msg, msg_bytes) == 0) {
            fprintf(stderr, "[INFO] clock frame(0x19) sent to router on boot\n");
            return 0;
        }

        fprintf(stderr, "[WARN] clock frame(0x19) send failed, retry %d/%d\n",
                attempt + 1, CLOCK_BOOT_RETRY);
        sleep(CLOCK_BOOT_RETRY_GAP_SEC);
    }

    fprintf(stderr, "[ERROR] clock frame(0x19) send gave up after %d retries\n",
            CLOCK_BOOT_RETRY);
    return -1;
}

/******************************************
 *      创建socket并绑定本地 UDP 3409     *
 ****************************************/
int control_socket_init(void)
{
    int on = 1;                                     /* setsockopt 的开关值 */
    struct sockaddr_in local_addr;                  /* 本地绑定地址 */

    if (control_sockfd >= 0)
        return 0;                                   /* 已经初始化过 */

    /* 创建 UDP socket */
    control_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (control_sockfd < 0) {
        perror("socket(control)");
        return -1;
    }

    /* 允许地址复用：防止重启时端口仍处于 TIME_WAIT 导致 bind 失败 */
    if (setsockopt(control_sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
        perror("setsockopt(SO_REUSEADDR)");

    /* 绑定到所有本地 IP 的 3409 端口 */
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);     /* INADDR_ANY = 监听所有网口 */
    local_addr.sin_port = htons(CONTROL_PORT);           /* 3409 */

    if (bind(control_sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind(control)");
        close(control_sockfd);
        control_sockfd = -1;
        return -1;
    }

    /* 初始化上报目标地址（本机 IP + 3419 端口） */
    if (init_control_report_peer() != 0) {
        close(control_sockfd);
        control_sockfd = -1;
        return -1;
    }

    return 0;
}

/***********************************
 *           关闭控制 socket        *
 **********************************/
void control_socket_close(void)
{
    if (control_sockfd >= 0) {
        close(control_sockfd);
        control_sockfd = -1;
    }
}

/***************************************
 *   BCD 字节解码：一个字节拆成两位十进制 *
 *   非法 BCD（任一半字节 > 9）返回 0xFFFF *
 ***************************************/
static uint16_t bcd_dec_byte(uint8_t b)
{
    uint8_t hi;     /* 高 4 位：十位 */
    uint8_t lo;     /* 低 4 位：个位 */

    hi = (uint8_t)((b >> 4) & 0x0F);
    lo = (uint8_t)(b & 0x0F);
    if (hi > 9 || lo > 9)
        return 0xFFFF;      /* 非法 BCD：交给调用方判错 */

    return (uint16_t)(hi * 10 + lo);
}

/***************************************
 *  给网管回一条 13 字节的应答帧         *
 *  mode: CTRL_FRAME_TYPE_ACK (0x30)    *
 *        或 CTRL_FRAME_TYPE_NACK(0x40) *
 *                                      *
 *  帧格式与 CPU1 上报的应答帧一致：      *
 *    [0..1]  长度 = 13（小端）          *
 *    [2..3]  保留 0x0000               *
 *    [4..5]  目的 ID                   *
 *    [6..7]  源 ID                     *
 *    [8..9]  同步序列 0xFFF5           *
 *    [10]    类型 mode                 *
 *    [11]    保留 cnt，必须为 0x00     *
 *    [12]    校验和                    *
 *                                      *
 *  按协议校验和 = pkt[10] ^ pkt[11]，   *
 *  取 cnt = 0x00 时结果恰等于 mode，    *
 *  正好与网管侧判定所用的 FrameEnd 重合 *
 *  （网管读 buffer[12] 判 0x30/0x40）。 *
 *                                      *
 *  注意：这里直接 sendto 本机 3419，     *
 *  不能走 /dev/amp_ctrl —— 那个方向是   *
 *  发给 CPU1 路由固件的。               *
 ***************************************/
static void portcfg_send_reply(uint8_t mode)
{
    uint8_t frame[13];
    ssize_t sent;

    memset(frame, 0, sizeof(frame));
    ctrl_write_le16(frame + 0, 13);                 /* 长度字段（含长度字段自身） */
    ctrl_write_le16(frame + 2, 0x0000);             /* 保留 */
    ctrl_write_le16(frame + 4, SRIO_ID_ROUTER);     /* 目的 ID */
    ctrl_write_le16(frame + 6, SRIO_ID_MASTER);     /* 源 ID */
    ctrl_write_le16(frame + 8, CLOCK_SYNC_MAGIC);   /* 同步序列 0xFFF5 */
    frame[10] = mode;                               /* 类型：0x30 ACK / 0x40 NACK */
    frame[11] = 0x00;                               /* 保留 cnt，网管侧要求必须为 0 */
    frame[12] = mode;                               /* 校验和 = mode ^ 0x00 = mode */

    sent = sendto(control_sockfd, frame, sizeof(frame), 0,
                  (struct sockaddr *)&control_report_peer,
                  sizeof(control_report_peer));
    if (sent < 0)
        perror("sendto(port reply)");
    else if (sent != (ssize_t)sizeof(frame))
        fprintf(stderr, "[WARN] short sendto(port reply): %zd/%zu\n", sent, sizeof(frame));
}

/***********************************
 *           控制数据写入线程       *
 **********************************/
void *control_rx_to_amp_thread(void *arg)
{
    uint8_t buffer[MAX_PAYLOAD_SIZE];
    struct sockaddr_in peer_addr;
    socklen_t peer_len;

    (void)arg;

    while (g_running) {
        ssize_t rx_len;
        struct amp_ctrl_msg msg;
        size_t msg_bytes;

        peer_len = sizeof(peer_addr);
        rx_len = recvfrom(control_sockfd,buffer,sizeof(buffer),0,(struct sockaddr *)&peer_addr,&peer_len);//socket接收到数据
        if (rx_len < 0) {
            if (errno == EINTR)
                continue;
            perror("recvfrom(control)");
            break;
        }

        //检验接收数据的校验和和长度
        if (!ctrl_frame_is_valid(buffer, (size_t)rx_len)) {
            fprintf(stderr, "[WARN] invalid control frame dropped, len=%zd\n", rx_len);
            continue;
        }

        /* 业务端口配置帧（0x21）：主控本地消费，不透传给路由固件。
         * 其余所有类型一律保持原有行为，原样写入 /dev/amp_ctrl 转发给 CPU1。 */
        if (buffer[10] == CTRL_FRAME_TYPE_BUSINESS_PORT) {
            uint16_t new_port;              /* 从 BCD 解码出来的新业务端口 */
            uint16_t digit;                 /* 单个 BCD 字节解码出的两位十进制数 */
            uint32_t acc;                   /* BCD 逐字节累加结果 */
            int i;

            /* 帧长必须是 16 字节：12 字节帧头 + 3 字节 BCD + 1 字节校验和。
             * 校验和已由 ctrl_frame_is_valid 验过，这里再判长度是为了
             * 防止短帧越界读到 buffer[13] / buffer[14]。 */
            if ((size_t)rx_len < BUSINESS_PORT_FRAME_LEN) {
                fprintf(stderr, "[WARN] business-port frame too short: %zd < %d\n",
                        rx_len, BUSINESS_PORT_FRAME_LEN);
                portcfg_send_reply(CTRL_FRAME_TYPE_NACK);
                continue;
            }

            /* 3 字节 BCD，高位在前：3408 -> 00 34 08 */
            acc = 0;
            for (i = 0; i < BUSINESS_PORT_BCD_BYTES; i++) {
                digit = bcd_dec_byte(buffer[12 + i]);
                if (digit == 0xFFFF) {          /* 非法 BCD：标记为失败并跳出 */
                    acc = 0xFFFFFFFFu;
                    break;
                }
                acc = acc * 100u + digit;
            }

            if (acc == 0xFFFFFFFFu || acc > BUSINESS_PORT_MAX) {
                fprintf(stderr, "[WARN] business-port frame carries invalid BCD port\n");
                portcfg_send_reply(CTRL_FRAME_TYPE_NACK);
                continue;
            }
            new_port = (uint16_t)acc;

            /* 应用并持久化：越界或撞 3409/3419 都会失败，回 NACK */
            if (portcfg_apply_and_save(new_port) != 0) {
                portcfg_send_reply(CTRL_FRAME_TYPE_NACK);
                continue;
            }

            fprintf(stderr, "[INFO] business port updated to %u by network manager\n",
                    (unsigned)new_port);
            portcfg_send_reply(CTRL_FRAME_TYPE_ACK);
            continue;                           /* 关键：不 write(ctrl_fd)，到此为止 */
        }

        memset(&msg, 0, sizeof(msg));
        msg.len = (uint32_t)rx_len;
        msg.data_type = 1;
        memcpy(msg.data, buffer, (size_t)rx_len);
        msg_bytes = offsetof(struct amp_ctrl_msg, data) + (size_t)rx_len;

        if (write_ctrl_msg(&msg, msg_bytes) != 0)
            break;
    }

    return NULL;
}

/***********************************
 *       控制数据读取上报线程       *
 **********************************/
void *control_amp_to_udp_thread(void *arg)
{
    struct amp_ctrl_msg msg;

    (void)arg;

    while (g_running) {
        ssize_t rx_len = read(ctrl_fd, &msg, sizeof(msg));

        if (rx_len < 0) {
            if (errno == EINTR)
                continue;
            perror("read(ctrl_fd)");
            break;
        }

        if ((size_t)rx_len < offsetof(struct amp_ctrl_msg, data))
            continue;
        if (msg.len == 0 || msg.len > MAX_PAYLOAD_SIZE)
            continue;
        if (msg.data_type == 0)
            msg.data_type = 1;

        cache_cpu1_report_frame(msg.data, msg.len);

        {
            ssize_t tx_len;

            tx_len = sendto(control_sockfd, msg.data, msg.len, 0,
                            (struct sockaddr *)&control_report_peer,
                            sizeof(control_report_peer));
            if (tx_len < 0) {
                perror("sendto(control)");
                continue;
            }
            if (tx_len != (ssize_t)msg.len)
                fprintf(stderr, "[WARN] short sendto(control): %zd/%u\n", tx_len, msg.len);
        }
    }

    return NULL;
}
