/**************************/
/*   控制面收发与透传模块   */
/**************************/
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <linux/if.h>

#include "user_declaration.h"

#define CTRL_FRAME_TYPE_ACK          0x30
#define CTRL_FRAME_TYPE_NACK         0x40
#define CTRL_FRAME_TYPE_VERSION      0x10
#define CTRL_FRAME_TYPE_WORK_PARAM   0x20

#define CTRL_FRAME_CNT_ACK           0x00
#define CTRL_FRAME_CNT_NACK          0x00
#define CTRL_FRAME_CNT_VERSION       0x05
#define CTRL_FRAME_CNT_WORK_PARAM    0x8C

typedef enum {
    CTRL_FRAME_UNKNOWN = 0,
    CTRL_FRAME_ACK,
    CTRL_FRAME_NACK,
    CTRL_FRAME_VERSION_REPORT,
    CTRL_FRAME_WORK_PARAM_REPORT,
} ctrl_frame_kind_t;

#pragma pack(push, 1)
typedef struct {
    uint16_t frameHead;
    uint16_t frameRetain;
    uint16_t GoalId;
    uint16_t SourceID;
    uint16_t synchronizing;
    uint8_t frameType;
    uint8_t frameCnt;
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
    uint8_t frameCnt;
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
    uint8_t NodeConnect1;
    uint8_t NodeConnect2;
    uint8_t NodeConnect3;
    uint8_t NodeConnect4;
    uint8_t NodeConnect5;
    uint8_t NodeConnect6;
    uint8_t NodeConnect7;
    uint8_t NodeConnect8;
    uint8_t NodeConnect9;
    uint8_t NodeConnect10;
    uint8_t NodeConnect11;
    uint8_t NodeConnect12;
    uint8_t NodeConnect13;
    uint8_t NodeConnect14;
    uint8_t NodeConnect15;
    uint8_t NodeConnect16;
    uint8_t NodeConnect17;
    uint8_t NodeConnect18;
    uint8_t NodeConnect19;
    uint8_t NodeConnect20;
    uint8_t NodeConnect21;
    uint8_t NodeConnect22;
    uint8_t NodeConnect23;
    uint8_t NodeConnect24;
    uint8_t NodeConnect25;
    uint8_t NodeConnect26;
    uint8_t NodeConnect27;
    uint8_t NodeConnect28;
    uint8_t NodeConnect29;
    uint8_t NodeConnect30;
    uint8_t NodeConnect31;
    uint8_t NodeConnect32;
    uint8_t NodeConnect33;
    uint8_t NodeConnect34;
    uint8_t NodeConnect35;
    uint8_t NodeConnect36;
    uint8_t NodeConnect37;
    uint8_t NodeConnect38;
    uint8_t NodeConnect39;
    uint8_t NodeConnect40;
    uint8_t NodeConnect41;
    uint8_t NodeConnect42;
    uint8_t NodeConnect43;
    uint8_t NodeConnect44;
    uint8_t NodeConnect45;
    uint8_t NodeConnect46;
    uint8_t NodeConnect47;
    uint8_t NodeConnect48;
    uint8_t NodeConnect49;
    uint8_t NodeConnect50;
    uint8_t NodeConnect51;
    uint8_t NodeConnect52;
    uint8_t NodeConnect53;
    uint8_t NodeConnect54;
    uint8_t NodeConnect55;
    uint8_t NodeConnect56;
    uint8_t NodeConnect57;
    uint8_t NodeConnect58;
    uint8_t NodeConnect59;
    uint8_t NodeConnect60;
    uint8_t NodeConnect61;
    uint8_t NodeConnect62;
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

static int control_sockfd = -1;
static pthread_mutex_t control_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sockaddr_in control_report_peer;
static ctrl_version_cache_t last_version_report;
static ctrl_work_param_cache_t last_work_param_report;

/***********************************
 * 从控制帧字节流里读 16 位大端字段 *
 **********************************/
static uint16_t ctrl_read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/***********************************
 * 从控制帧字节流里读 32 位大端字段 *
 **********************************/
static uint32_t ctrl_read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

/***********************************
 *          异或校验计算函数         *
 * 协议A 第67行：校验和 = 头区域(类型+计数) 与 数据域 每个字节按位异或。
 * 本帧布局：pkt[10]=类型、pkt[11]=计数、pkt[12..] = 数据域、pkt[len-1]=校验和。
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
 * 计数校验结果与数据帧长度是否合理 *
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

    if (pkt[10] == CTRL_FRAME_TYPE_ACK && pkt[11] == CTRL_FRAME_CNT_ACK)
        return CTRL_FRAME_ACK;
    if (pkt[10] == CTRL_FRAME_TYPE_NACK && pkt[11] == CTRL_FRAME_CNT_NACK)
        return CTRL_FRAME_NACK;
    if (pkt[10] == CTRL_FRAME_TYPE_VERSION && pkt[11] == CTRL_FRAME_CNT_VERSION)
        return CTRL_FRAME_VERSION_REPORT;
    if (pkt[10] == CTRL_FRAME_TYPE_WORK_PARAM && pkt[11] == CTRL_FRAME_CNT_WORK_PARAM)
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
#define CTRL_PARSE_BE16(field)                \
    do {                                      \
        frame->field = ctrl_read_be16(pkt + offset); \
        offset += 2;                          \
    } while (0)

    CTRL_PARSE_BE16(frameHead);
    CTRL_PARSE_BE16(frameRetain);
    CTRL_PARSE_BE16(GoalId);
    CTRL_PARSE_BE16(SourceID);
    CTRL_PARSE_BE16(synchronizing);
    CTRL_PARSE_U8(frameType);
    CTRL_PARSE_U8(frameCnt);
    CTRL_PARSE_U8(WEBVersion);
    CTRL_PARSE_U8(MCVersion);
    CTRL_PARSE_U8(NETVersion);
    CTRL_PARSE_U8(SPCLVersion);
    CTRL_PARSE_U8(JDCLVersion);
    CTRL_PARSE_U8(frameEnd);

#undef CTRL_PARSE_BE16
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
#define CTRL_PARSE_BE16(field)                \
    do {                                      \
        frame->field = ctrl_read_be16(pkt + offset); \
        offset += 2;                          \
    } while (0)
#define CTRL_PARSE_BE32(field)                \
    do {                                      \
        frame->field = ctrl_read_be32(pkt + offset); \
        offset += 4;                          \
    } while (0)

    CTRL_PARSE_BE16(frameHead);
    CTRL_PARSE_BE16(frameRetain);
    CTRL_PARSE_BE16(GoalId);
    CTRL_PARSE_BE16(SourceID);
    CTRL_PARSE_BE16(synchronizing);
    CTRL_PARSE_U8(frameType);
    CTRL_PARSE_U8(frameCnt);
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
    CTRL_PARSE_BE16(HopRate);
    CTRL_PARSE_U8(SynSignal);
    CTRL_PARSE_U8(LinkQuality);
    CTRL_PARSE_U8(FaultSignal);
    CTRL_PARSE_U8(Silent);
    CTRL_PARSE_U8(AllSlient);
    CTRL_PARSE_U8(ChannelInfo);
    CTRL_PARSE_BE16(ChannelTemp);
    CTRL_PARSE_U8(ChannelVolt);
    CTRL_PARSE_U8(ChannelElect);
    CTRL_PARSE_U8(RFInfo);
    CTRL_PARSE_BE16(RFTemp);
    CTRL_PARSE_U8(RFVolt);
    CTRL_PARSE_U8(RFElect);
    CTRL_PARSE_U8(BasedInfo);
    CTRL_PARSE_BE16(BasedTemp);
    CTRL_PARSE_U8(BasedVolt);
    CTRL_PARSE_U8(BasedElect);
    CTRL_PARSE_U8(Power1Info);
    CTRL_PARSE_BE16(Power1Temp);
    CTRL_PARSE_U8(Power1Volt);
    CTRL_PARSE_U8(Power1Elect);
    CTRL_PARSE_U8(Power2Info);
    CTRL_PARSE_BE16(Power2Temp);
    CTRL_PARSE_U8(Power2Volt);
    CTRL_PARSE_U8(Power2Elect);
    CTRL_PARSE_U8(BandwidthSet);
    CTRL_PARSE_U8(PowerSet);
    CTRL_PARSE_U8(Encryption);
    CTRL_PARSE_U8(WorkMode);
    CTRL_PARSE_BE32(FixedFrequency);
    CTRL_PARSE_BE32(AdaHopMinFre);
    CTRL_PARSE_BE32(AdaHopMaxFre);
    CTRL_PARSE_U8(NotAdaHopFre);
    CTRL_PARSE_U8(ComNetName);
    CTRL_PARSE_BE32(MinFreThreshold);
    CTRL_PARSE_BE32(MaxFreThreshold);
    CTRL_PARSE_U8(Modulation);
    CTRL_PARSE_U8(OnlineNodeSum);
    CTRL_PARSE_U8(ComDataSum);
    CTRL_PARSE_U8(ComDataBER);
    CTRL_PARSE_U8(ComDataPLP);
    CTRL_PARSE_U8(NodeConnect1);
    CTRL_PARSE_U8(NodeConnect2);
    CTRL_PARSE_U8(NodeConnect3);
    CTRL_PARSE_U8(NodeConnect4);
    CTRL_PARSE_U8(NodeConnect5);
    CTRL_PARSE_U8(NodeConnect6);
    CTRL_PARSE_U8(NodeConnect7);
    CTRL_PARSE_U8(NodeConnect8);
    CTRL_PARSE_U8(NodeConnect9);
    CTRL_PARSE_U8(NodeConnect10);
    CTRL_PARSE_U8(NodeConnect11);
    CTRL_PARSE_U8(NodeConnect12);
    CTRL_PARSE_U8(NodeConnect13);
    CTRL_PARSE_U8(NodeConnect14);
    CTRL_PARSE_U8(NodeConnect15);
    CTRL_PARSE_U8(NodeConnect16);
    CTRL_PARSE_U8(NodeConnect17);
    CTRL_PARSE_U8(NodeConnect18);
    CTRL_PARSE_U8(NodeConnect19);
    CTRL_PARSE_U8(NodeConnect20);
    CTRL_PARSE_U8(NodeConnect21);
    CTRL_PARSE_U8(NodeConnect22);
    CTRL_PARSE_U8(NodeConnect23);
    CTRL_PARSE_U8(NodeConnect24);
    CTRL_PARSE_U8(NodeConnect25);
    CTRL_PARSE_U8(NodeConnect26);
    CTRL_PARSE_U8(NodeConnect27);
    CTRL_PARSE_U8(NodeConnect28);
    CTRL_PARSE_U8(NodeConnect29);
    CTRL_PARSE_U8(NodeConnect30);
    CTRL_PARSE_U8(NodeConnect31);
    CTRL_PARSE_U8(NodeConnect32);
    CTRL_PARSE_U8(NodeConnect33);
    CTRL_PARSE_U8(NodeConnect34);
    CTRL_PARSE_U8(NodeConnect35);
    CTRL_PARSE_U8(NodeConnect36);
    CTRL_PARSE_U8(NodeConnect37);
    CTRL_PARSE_U8(NodeConnect38);
    CTRL_PARSE_U8(NodeConnect39);
    CTRL_PARSE_U8(NodeConnect40);
    CTRL_PARSE_U8(NodeConnect41);
    CTRL_PARSE_U8(NodeConnect42);
    CTRL_PARSE_U8(NodeConnect43);
    CTRL_PARSE_U8(NodeConnect44);
    CTRL_PARSE_U8(NodeConnect45);
    CTRL_PARSE_U8(NodeConnect46);
    CTRL_PARSE_U8(NodeConnect47);
    CTRL_PARSE_U8(NodeConnect48);
    CTRL_PARSE_U8(NodeConnect49);
    CTRL_PARSE_U8(NodeConnect50);
    CTRL_PARSE_U8(NodeConnect51);
    CTRL_PARSE_U8(NodeConnect52);
    CTRL_PARSE_U8(NodeConnect53);
    CTRL_PARSE_U8(NodeConnect54);
    CTRL_PARSE_U8(NodeConnect55);
    CTRL_PARSE_U8(NodeConnect56);
    CTRL_PARSE_U8(NodeConnect57);
    CTRL_PARSE_U8(NodeConnect58);
    CTRL_PARSE_U8(NodeConnect59);
    CTRL_PARSE_U8(NodeConnect60);
    CTRL_PARSE_U8(NodeConnect61);
    CTRL_PARSE_U8(NodeConnect62);
    CTRL_PARSE_U8(frameEnd);

#undef CTRL_PARSE_BE32
#undef CTRL_PARSE_BE16
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
 *      创建socket并绑定本地 UDP 3409     *
 ****************************************/
int control_socket_init(void)
{
    int on = 1;
    struct sockaddr_in local_addr;

    if (control_sockfd >= 0)
        return 0;

    control_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (control_sockfd < 0) {
        perror("socket(control)");
        return -1;
    }

    if (setsockopt(control_sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
        perror("setsockopt(SO_REUSEADDR)");

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	//INADDR_ANY表示服务器可以接收来自任何网络接口的连接请求也就是说，服务器不绑定到特定的 IP 地址，而是监听所有可用的本地 IP 地址。
    local_addr.sin_port = htons(CONTROL_PORT);

    if (bind(control_sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind(control)");
        close(control_sockfd);
        control_sockfd = -1;
        return -1;
    }

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
