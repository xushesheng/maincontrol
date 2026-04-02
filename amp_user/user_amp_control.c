/*************************************/
/*     控制 UDP 抓取与控制帧发送      */
/*************************************/
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <pcap.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include "user_amp_runtime.h"

/*控制数据发送函数*/
static int process_control_frame(uint8_t *udp_payload, int payload_len)
{
    control_frame_t ctrl_frame;                             //创建一个数据帧ctrl_frame
    struct amp_net_msg msg;                                 //创建一个网络数据发送帧msg
    ssize_t w;

    if (payload_len != (int)sizeof(control_frame_t))        //传入数据长度不符合返回-1
        return -1;

    memcpy(&ctrl_frame, udp_payload, sizeof(ctrl_frame));   //传入数据长度符合复制到ctrl_frame
    if (ntohs(ctrl_frame.frame_header) != 0xF00F)           //验证帧头帧尾，不对返回-1
        return -1;
    if (ntohs(ctrl_frame.frame_tail) != 0xE00E)
        return -1;

    memset(&msg, 0, sizeof(msg));                           //初始化发送结构体msg
    msg.data_type = 1;
    msg.node_id = ctrl_frame.dst_addr;                      //目的IP赋为控制数据的目的IP
    msg.ip = 0;
    msg.len = sizeof(ctrl_frame);
    memcpy(msg.data, &ctrl_frame, sizeof(ctrl_frame));      //将整个ctrl_frame放到msg.data里面发送

    w = write(amp_fd, &msg, offsetof(struct amp_net_msg, data) + msg.len);  //把msg写入amp_fd文件描述符，长度为msg头部加上数据部分
    if (w < 0)
        perror("write(amp ctrl)");
    return 0;
}

/*pcap_dispatch的回调函数，发送截获的控制数据*/
static void got_packet(u_char *args, const struct pcap_pkthdr *header, const u_char *packet)
{
    const struct iphdr *ip;
    size_t ihl;
    const struct udphdr *udp;
    uint16_t dst_port;
    const uint8_t *payload;
    int payload_len;

    (void)args;

    if (header->caplen < 14 + sizeof(struct iphdr))                 //以太网帧头14字节，IP头最小20字节，如果捕获长度小于34字节，返回
        return;

    ip = (const struct iphdr *)(packet + 14);                       //以太网帧头14字节，IP头从第15字节开始
    if (ip->version != 4)                                           //只处理IPv4数据包，其他返回
        return;

    ihl = (size_t)ip->ihl * 4;                                      //IP头长度字段单位是4字节，计算出ihl(IP Hearder Length)
    if (header->caplen < 14 + ihl + sizeof(struct udphdr))          //以太网帧头14字节，IP头ihl字节，UDP头8字节，如果捕获长度小于14+ihl+8字节，返回
        return;
    if (ip->protocol != IPPROTO_UDP)                                //只处理UDP数据包，其他返回
        return;

    udp = (const struct udphdr *)((const uint8_t *)ip + ihl);       //UDP头从以太网帧头14字节和IP头ihl字节之后开始
    dst_port = ntohs(udp->dest);                                    //UDP目的端口转换为主机字节序
    if (dst_port != CTRL_UDP_PORT)                                  //3409端口
        return;

    payload = (const uint8_t *)(udp + 1);                           //UDP 头后面紧跟着就是 UDP payload，udp + 1 就是负载起点
    payload_len = (int)ntohs(udp->len) - (int)sizeof(struct udphdr);//udp->len 是“UDP 头 + UDP 负载”的总长度减掉 sizeof(struct udphdr)是纯 payload 长度
    if (payload_len <= 0)
        return;

    (void)process_control_frame((uint8_t *)payload, payload_len);   //控制数据写入
}

/* 控制帧用pcap方式截获线程 */
void *pcap_control_thread(void *arg)
{
    char errbuf[PCAP_ERRBUF_SIZE];  //pcap 出错时存放错误信息
    pcap_t *handle;                 //抓包句柄
    struct bpf_program fp;          //编译后的 BPF 过滤器
    char filter_exp[128];           //过滤表达式字符串

    (void)arg;

    handle = pcap_open_live(CAP_IFACE,              4096,                       0,          10,         errbuf);
                            //eth1;     每次最多抓取的数据长度4096;  不启用混杂模式;    10ms读取超时;   超时错误信息
    if (!handle) {          //打开截获句柄失败报错
        fprintf(stderr, "[ERROR] pcap_open_live(%s) failed: %s\n", CAP_IFACE, errbuf);
        return NULL;
    }

    snprintf(filter_exp, sizeof(filter_exp), "udp and dst port %d", CTRL_UDP_PORT);     //构造 BPF 过滤表达式
    if (pcap_compile(handle, &fp, filter_exp, 1, PCAP_NETMASK_UNKNOWN) < 0 ||           //把文本过滤表达式编译成 BPF 程序
        pcap_setfilter(handle, &fp) < 0) {                                              //把编译好的过滤器绑定到当前抓包句柄
        fprintf(stderr, "[ERROR] pcap filter failed: %s\n", pcap_geterr(handle));       //过滤器编译或安装失败
        pcap_close(handle);                                                             //关闭抓包句柄
        return NULL;
    }

    while (1) {
        int rc = pcap_dispatch(handle, 1, got_packet, NULL);        //循环调用pcap_dispatch处理一个数据包，回调函数是got_packet
        if (rc < 0) {
            fprintf(stderr, "[ERROR] pcap_dispatch: %s\n", pcap_geterr(handle));
            break;
        }
    }

    pcap_close(handle);
    return NULL;
}
