/**************************/
/*    业务数据收发线程     */
/**************************/
#include <stdio.h>                 /* fprintf / perror */
#include <string.h>                /* memset / memcpy / memcmp */
#include <unistd.h>                /* read / write / usleep / close */
#include <fcntl.h>                 /* fcntl / O_NONBLOCK */
#include <errno.h>                 /* errno / EAGAIN / EINTR */
#include <poll.h>                  /* poll() 多路复用 */
#include <stddef.h>                /* offsetof / size_t */
#include <sys/ioctl.h>             /* ioctl() */
#include <sys/socket.h>            /* socket 地址结构 */
#include <linux/if.h>              /* IFNAMSIZ / struct ifreq */
#include <linux/if_tun.h>          /* TUN 设备 ioctl（TUNSETIFF / IFF_TUN） */
#include <netinet/ip.h>            /* struct iphdr（IP 头结构） */
#include <netinet/ip_icmp.h>       /* struct icmphdr（ICMP 头结构） */
#include <netinet/udp.h>           /* struct udphdr（UDP 头结构） */

#include "user_declaration.h"      /* 全局变量、类型、函数声明 */

amp_tx_runtime_t amp_tx_runtime = {
    .lock = PTHREAD_MUTEX_INITIALIZER,          /* 静态初始化互斥锁 */
    .not_empty = PTHREAD_COND_INITIALIZER,      /* 静态初始化"非空"条件变量 */
    .data_not_full = PTHREAD_COND_INITIALIZER,  /* 静态初始化"非满"条件变量 */
};

/***********************/
/*  TUN虚拟设备相关函数 */
/***********************/
int tun_alloc(const char *devname)
{
    struct ifreq ifr;                           //创建ifr设备标识符
    int fd = open("/dev/net/tun", O_RDWR);      //Linux 中创建虚拟网卡的标准入口

    if (fd < 0) {
        perror("open /dev/net/tun");            //创建失败直接返回-1
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));               //初始化标识符
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;        //创建 TUN 设备（IP 层），而不是 TAP 设备;不添加额外的包头信息（Packet Information）
    strncpy(ifr.ifr_name, devname, IFNAMSIZ - 1);       //指定虚拟网卡的名称（如"rf0"）

    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {       //通过ioctl 系统调用创建虚拟网络设备
        perror("ioctl(TUNSETIFF)");
        close(fd);
        return -1;
    }
    return fd;
}

/**************************
*向TUN虚拟网络设备写入数据包
**************************/
int tun_write_packet(int fd, const uint8_t *pkt, size_t len)
{
    while (1) {
        ssize_t w = write(fd, pkt, len);
        if (w == (ssize_t)len)      //成功写入则立即返回
            return 0;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {       //设备输出缓冲区满时返回此错误,poll等10ms，直到设备可写然后重新尝试写入
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            (void)poll(&p, 1, 10);
            continue;
        }
        if (w < 0 && errno == EINTR)          // 被信号中断，重试
            continue;
        return -1;
    }
}

/* 把一个文件描述符 fd 设置成非阻塞模式 */
static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);                  /* 获取当前文件状态标志 */
    if (flags < 0)
        return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)    /* 在原有标志上，额外打开 O_NONBLOCK 位设置为非阻塞 */
        return -1;
    return 0;
}

/* 业务发送队列的底层入队函数（调用者必须持有 amp_tx_runtime.lock） */
static int amp_tx_enqueue_locked(amp_tx_slot_t *queue,
                                 unsigned int depth,         /* 队列深度 */
                                 unsigned int *tail,         /* 队尾索引指针 */
                                 unsigned int *count,        /* 当前计数指针 */
                                 pthread_cond_t *not_full,   /* "队列非满"条件变量 */
                                 unsigned long *waits,       /* 等待次数计数器 */
                                 const char *queue_name,     /* 队列名称（日志用） */
                                 const struct amp_net_msg *msg,
                                 size_t msg_bytes)
{
    /* 队列满：在条件变量上等待，直到发送线程取走数据 */
    while (*count == depth) {
        int rc;

        (*waits)++;                                 /* 等待计数加1 */
        if ((*waits % 64UL) == 1UL)                 /* 每 64 次打印一次告警（避免刷屏） */
            fprintf(stderr, "[WARN] %s queue full, waiting...\n", queue_name);

        rc = pthread_cond_wait(not_full, &amp_tx_runtime.lock);  /* 释放锁并等待，被唤醒时自动重新获取锁 */
        if (rc != 0)
            return -1;
    }

    /* 队列有空位：将消息写入队尾 */
    queue[*tail].msg = *msg;                        /* 拷贝消息内容 */
    queue[*tail].msg_bytes = msg_bytes;             /* 记录实际字节数 */
    *tail = (*tail + 1U) % depth;                   /* 环形推进队尾索引 */
    (*count)++;                                     /* 计数加1 */
    pthread_cond_signal(&amp_tx_runtime.not_empty);    /* 唤醒发送线程（队列非空） */
    return 0;
}

/* 业务数据发送队列外层封装 */
static int amp_tx_enqueue(const struct amp_net_msg *msg, size_t msg_bytes)
{
    int rc;

    rc = pthread_mutex_lock(&amp_tx_runtime.lock);
    if (rc != 0)
        return -1;

    rc = amp_tx_enqueue_locked(amp_tx_runtime.data_q,
                               AMP_TX_QUEUE_DEPTH,
                               &amp_tx_runtime.data_tail,
                               &amp_tx_runtime.data_count,
                               &amp_tx_runtime.data_not_full,
                               &amp_tx_runtime.data_waits,
                               "amp data tx",
                               msg,
                               msg_bytes);

    (void)pthread_mutex_unlock(&amp_tx_runtime.lock);
    return rc;
}

/* 调用 write(amp_fd, ...) 写 /dev/amp_ipi */
static int amp_write_msg_direct(const struct amp_net_msg *msg, size_t msg_bytes)
{
    while (1) {
        ssize_t w = write(amp_fd, msg, msg_bytes);

        if (w == (ssize_t)msg_bytes)
            return 0;
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0) {
            amp_tx_runtime.tx_write_fail++;
            perror("write(amp)");
            return -1;
        }

        amp_tx_runtime.tx_short_write++;
        fprintf(stderr, "[ERROR] short write(amp): %zd/%zu\n", w, msg_bytes);
        return -1;
    }
}

/* 统一业务发送线程，避免多个线程同时写业务设备 */
/* 统一业务发送线程：串行化 write(/dev/amp_ipi)，避免多个线程并发踩 TX 单槽 */
void *amp_tx_thread(void *arg)
{
    amp_tx_slot_t slot;     /* 从队列中取出的发送槽位 */

    (void)arg;

    while (g_running) {
        /* 获取队列锁 */
        int rc = pthread_mutex_lock(&amp_tx_runtime.lock);

        if (rc != 0) {
            fprintf(stderr, "[ERROR] pthread_mutex_lock(amp_tx_runtime) failed\n");
            break;
        }

        /* 队列为空：在条件变量上等待（释放锁，被唤醒时重获锁） */
        while (amp_tx_runtime.data_count == 0) {
            if (!g_running) {                           /* 退出标志已设置 */
                (void)pthread_mutex_unlock(&amp_tx_runtime.lock);
                return NULL;
            }
            rc = pthread_cond_wait(&amp_tx_runtime.not_empty, &amp_tx_runtime.lock);
            if (rc != 0) {
                (void)pthread_mutex_unlock(&amp_tx_runtime.lock);
                fprintf(stderr, "[ERROR] pthread_cond_wait(amp_tx_runtime) failed\n");
                return NULL;
            }
        }

        /* 从队头取出一条消息 */
        slot = amp_tx_runtime.data_q[amp_tx_runtime.data_head];
        amp_tx_runtime.data_head = (amp_tx_runtime.data_head + 1U) % AMP_TX_QUEUE_DEPTH;  /* 推进队头 */
        amp_tx_runtime.data_count--;                    /* 计数减1 */
        pthread_cond_signal(&amp_tx_runtime.data_not_full);   /* 唤醒可能等待的生产者 */

        (void)pthread_mutex_unlock(&amp_tx_runtime.lock);     /* 释放锁：write 是耗时操作，不在锁内进行 */

        /* 实际写入 /dev/amp_ipi */
        (void)amp_write_msg_direct(&slot.msg, slot.msg_bytes);

        /* 写完后留一个保护间隔，降低连续两次 write 之间 TX 单槽被覆盖的概率 */
        if (AMP_TX_GUARD_US > 0)
            usleep(AMP_TX_GUARD_US);
    }

    return NULL;
}

/* 构造一个业务 amp_net_msg 并入发送队列 */
int amp_send_msg(uint32_t dst_ip, const uint8_t *payload, size_t len)
{
    struct amp_net_msg msg;         //创建发送结构体

    if (len > MAX_PAYLOAD_SIZE) {   //判断传入长度不超过最大容纳值
        fprintf(stderr, "[ERROR] payload too large: %zu > %d\n", len, MAX_PAYLOAD_SIZE);
        return -1;
    }

    memset(&msg, 0, sizeof(msg));
    msg.data_type = 1;
    msg.ip = dst_ip;
    msg.node_id = 0;    //
    msg.len = (uint32_t)len;
    memcpy(msg.data, payload, len);

    if (amp_tx_enqueue(&msg, offsetof(struct amp_net_msg, data) + msg.len) != 0) {
        fprintf(stderr, "[ERROR] amp data enqueue failed\n");
        return -1;
    }
    return 0;
}

/************
*初始化聚合帧
************/
void batch_reset(batch_state_t *b)               //传入一个批次帧结构体
{
    uint32_t seq = b->seq;                       //定义赋值批帧序号
    amp_batch_hdr_t hdr;                         //定义帧头结构体
    memset(b, 0, sizeof(*b));                    //初始化帧结构体
    b->seq = seq;

    memset(&hdr, 0, sizeof(hdr));                //初始化帧头结构体
    memcpy(hdr.magic, AMP_BATCH_MAGIC, 4);       //初始化批次魔数AMPB
    hdr.version = AMP_BATCH_VERSION;             //初始化版本
    hdr.flags = 0;                               //初始化标识为0
    hdr.count_be = htons(0);                     //初始化子包数量为0
    hdr.seq_be = htonl(b->seq);                  //初始化头序号为帧序号

    memcpy(b->buf, &hdr, sizeof(hdr));           //把帧头放入帧结构体首部buf中
    b->len = sizeof(hdr);                        //帧长度为帧头长度
    b->count = 0;                                //初始化已写入子包数
    b->dst_ip = 0;                               //初始化当前批次目的地地址
}

/***********************
*把新数据帧pkt添加到当前批次聚合帧
***********************/
int batch_append(batch_state_t *b, const uint8_t *pkt, size_t pkt_len, uint32_t dst_ip)
{
    if (pkt_len > 0xFFFF)   //如果长度超长返回-1
        return -1;
    if (b->len + 2 + pkt_len > AMP_BATCH_MAX_BYTES)
        return -2;  //如果核算长度超出640B返回-2

    if (b->count == 0)  //如果经过以上筛选，且核算长度不为零
        b->dst_ip = dst_ip; //将聚合帧结构体的目的地地址赋值为传入的目的地地址

    write_be16_unaligned(b->buf + b->len, (uint16_t)pkt_len);   //将传入的长度写入buf的末尾位置
    b->len += 2;    //聚合帧结构体的长度+2
    memcpy(b->buf + b->len, pkt, pkt_len);  //将传入的数据写到聚合帧buf的末尾
    b->len += pkt_len;  //聚合帧长度加上pkt的长度
    b->count++; //聚合帧子包数量+1
    write_be16_unaligned(b->buf + offsetof(amp_batch_hdr_t, count_be), b->count);   //buf的地址加上count_be的偏移量，改写帧头的子包数量
    write_be32_unaligned(b->buf + offsetof(amp_batch_hdr_t, seq_be), b->seq);       //改写帧头的序号
    return 0;
}

/**********************
 *发送当前批次的所有数据
 ********************/
int amp_flush_batch_if_any(batch_state_t *b)
{
    int rc = 0;

    if (b->count == 0)
        return 0;

    /* 如果批次里只有 1 个子包，为减少头开销，直接发原始 IP 包 */
    if (b->count == 1) {
        size_t off = sizeof(amp_batch_hdr_t);   //帧头大小赋值给off
        uint16_t l;

        if (b->len < off + 2) {                 //如果帧长度小于帧头结构体大小+2字节子包长度字段，表示为空包或异常
            batch_reset(b);                     //初始化帧
            return -1;
        }

        l = read_be16_unaligned(b->buf + off);  //读取帧头末尾的2字节唯一子包长度字段赋值给l
        if (off + 2 + l > b->len) {             //如果帧长度<帧头结构体大小+子包长度，即长度不对应
            batch_reset(b);                     //直接放弃当前批次,初始化一个新批次
            return -1;
        }

        rc = amp_send_msg(b->dst_ip, b->buf + off + 2, l); //跳过帧头和长度字节，直接把子包发出去
        b->seq++;   //批次序号+1
        batch_reset(b);
        return rc;
    }

    rc = amp_send_msg(b->dst_ip, b->buf, b->len);  //把头和所有子包（即整个buf部分）一起原样发出
    b->seq++;
    batch_reset(b);
    return rc;
}

/**********************
*   ping包判断函数    *
**********************/
static int is_ping_or(const uint8_t *pkt, size_t len)
{
    const struct iphdr *ip;
    size_t ihl;
    const struct icmphdr *ic;

    if (len < sizeof(struct iphdr))
        return 0;

    ip = (const struct iphdr *)pkt;
    if (ip->version != 4)
        return 0;

    ihl = (size_t)ip->ihl * 4;
    if (ihl < sizeof(struct iphdr) || len < ihl + sizeof(struct icmphdr))
        return 0;
    if (ip->protocol != IPPROTO_ICMP)
        return 0;

    ic = (const struct icmphdr *)(pkt + ihl);
    return ic->type == ICMP_ECHO || ic->type == ICMP_ECHOREPLY;    //包类型为ICMP请求或应答返回1，否则返回0
}

/* 对UDP业务做端口分流：
 * g_business_port（默认 3408，可由网管下发配置帧修改）进入业务面；
 * 3409 保留给独立控制面，不进入TUN业务通道；
 * 其他UDP流量当前不进入AMP业务面。 */
static int classify_udp_business_port(const uint8_t *pkt, size_t len)
{
    const struct iphdr *ip;
    size_t ihl;
    const struct udphdr *udp;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t biz_port;

    if (len < sizeof(struct iphdr))
        return 0;

    ip = (const struct iphdr *)pkt;
    if (ip->version != 4 || ip->protocol != IPPROTO_UDP)
        return 0;

    ihl = (size_t)ip->ihl * 4;
    if (ihl < sizeof(struct iphdr) || len < ihl + sizeof(struct udphdr))
        return -1;

    udp = (const struct udphdr *)(pkt + ihl);
    src_port = ntohs(udp->source);
    dst_port = ntohs(udp->dest);

    if (src_port == CONTROL_PORT || dst_port == CONTROL_PORT)
        return -1;

    /* 先取一次快照再比较：配置线程可能在两次比较之间改写业务端口，
     * 取快照可保证同一个包的前后判断基于同一个端口值，不会自相矛盾 */
    biz_port = g_business_port;
    if (src_port == biz_port || dst_port == biz_port)
        return 1;
    return -1;
}

/************************************************************* * 
* 线程1：从TUN读取需要“跨射频”的IP包，写入驱动（-> CPU1 -> 对端）*
 ***************************************************************/
void *tun_to_amp_thread(void *arg)
{
    uint8_t buf[MAX_PAYLOAD_SIZE];
    batch_state_t batch;	            //准备一个空批次帧

    (void)arg;
    memset(&batch, 0, sizeof(batch));   //清零当前批次帧数据
    batch_reset(&batch);                //初始化批次帧
    (void)set_nonblock(tun_fd);

    while (g_running) {
        int timeout_ms = (batch.count == 0) ? -1 : AMP_BATCH_TIMEOUT_MS;    //batch 为空：timeout=-1;batch非空：timeout=60
        struct pollfd pfd = { .fd = tun_fd, .events = POLLIN };             //初始化poll阻塞，设置标识符为tun_fd,events为pollin可读
        int prc = poll(&pfd, 1, timeout_ms);        //阻塞pfd标识timeout_ms时间

        if (prc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll(tun)");
            break;
        }

        if (prc == 0) {                      //表示pfd中tun_fd没有准备好读写或出错，当poll阻塞超时timeout
            /* 聚合窗口超时：发掉当前批次 */
            (void)amp_flush_batch_if_any(&batch);
            continue;
        }

        /* 尽可能把当前可读的数据读空（non-blocking），提高聚合命中率 */
        while (1) {
            ssize_t n = read(tun_fd, buf, sizeof(buf));         //从tun中取一个IP包
            struct iphdr *ip;
            size_t pkt_len;
            uint32_t dst_ip;
            int udp_class;
            int is_ping;
            size_t agg_overhead;
            int rc;

            if (n < 0) {                            //取出失败报错
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                    continue;
                perror("read(tun)");
                goto out;
            }
            if (n == 0)
                break;
            if ((size_t)n < sizeof(struct iphdr))   //如果取出包长度小于iphdr结构体长度
                continue;

            ip = (struct iphdr *)buf;
            if (ip->version != 4)
                continue;
            if (ip->daddr != BROADCAST_IP_BE && !is_peer_pc_addr(ip->daddr))       //广播+对端节点IP才走AMP通道
                continue;

            pkt_len = (size_t)n;      //定义pkt_len设置为本条IP包长度
            dst_ip = ip->daddr;       //定义dst_ip赋值为当前IP包目的地址
            udp_class = classify_udp_business_port(buf, pkt_len);

            if (udp_class < 0)
                continue;

#if AMP_ICMP_FASTPATH   //是否开启ICMP包快速通道
            is_ping = is_ping_or(buf, pkt_len); //判断是否为ping包，是的话置is_ping为1
#else
            is_ping = 0;
#endif

            /* ====== 解除单包 640B 限制：>640 直接单包直发 ====== */
            if (pkt_len > AMP_BATCH_MAX_BYTES) {            //如果当前包超过640B
                (void)amp_flush_batch_if_any(&batch);       //则先将之前存的batch发出
                (void)amp_send_msg(dst_ip, buf, pkt_len);   //再单独将本批次的IP包写入
                continue;
            }

            /* 对于 <=640 的包，仍要考虑批帧头/长度字段的开销：塞不进批帧就单包直发 */
            agg_overhead = sizeof(amp_batch_hdr_t) + 2;             /* 若批帧为空，只装一个子包的最小开销（包长+2） */
            if (pkt_len + agg_overhead > AMP_BATCH_MAX_BYTES) {     //如果当前IP包长度加上另一最小帧大于640依旧
                (void)amp_flush_batch_if_any(&batch);               //则先将之前存的batch发出
                (void)amp_send_msg(dst_ip, buf, pkt_len);           //再单独将本批次的IP包写入
                continue;
            }

            /* ping 快速通道：不等待聚合窗口。
             * 优先尝试把 ping 塞进当前批次，然后立刻 flush（尽量不额外增加 SGI 次数）。 */
            if (is_ping) {
                int appended = 0;       //定义添加标识
                if (batch.count == 0 || batch.dst_ip == dst_ip) {           //如果当前序列为空或者batch的目的IP与ping包一致
                    if (batch_append(&batch, buf, pkt_len, dst_ip) == 0)    //如果添加成功
                        appended = 1;   //添加标识置为1
                }

                if (!appended) {        //如果添加标识还是0代表上一个if中添加ping包失败
                    (void)amp_flush_batch_if_any(&batch);
                    (void)amp_send_msg(dst_ip, buf, pkt_len);
                } else {
                    (void)amp_flush_batch_if_any(&batch);   //如果前面添加成功，则直接发出整个batch
                }
                continue;
            }

            /* 普通小包：按目的IP聚合。
             * 若目的IP改变，先 flush 再开始新批次。 */
            if (batch.count > 0 && batch.dst_ip != dst_ip)  //当前IP包目的IP如果与存的batch不同
                (void)amp_flush_batch_if_any(&batch);       //则先发出存的batch

            rc = batch_append(&batch, buf, pkt_len, dst_ip);    //赋值rc为添加IP包进入batch函数的返回值
            if (rc == -2) {
                /* 空间不足：先 flush 再试一次；若还是不行就单包直发 */
                (void)amp_flush_batch_if_any(&batch);
                rc = batch_append(&batch, buf, pkt_len, dst_ip);
                if (rc != 0)
                    (void)amp_send_msg(dst_ip, buf, pkt_len);    //再次添加失败，则单独发送一次当前IP包
            } else if (rc != 0) {
                (void)amp_flush_batch_if_any(&batch);
                (void)amp_send_msg(dst_ip, buf, pkt_len);
            }
        }
    }

out:
    (void)amp_flush_batch_if_any(&batch);       //退出循环时发出当前批次帧
    return NULL;
}

/**************************************************************************** 
* 线程2：从驱动read()取出对端发来的IP包，写回TUN，让内核继续路由到eth0发给本地PC *
*****************************************************************************/
void *amp_to_tun_thread(void *arg)
{
    struct amp_net_msg msg;

    (void)arg;

    while (g_running) {
        ssize_t n = read(amp_fd, &msg, sizeof(msg));
		/*********读到数据后过滤一遍下列条件**********/

        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read(amp)");
            break;
        }
        if ((size_t)n < offsetof(struct amp_net_msg, data))
            continue;
        if (msg.len == 0 || msg.len > MAX_PAYLOAD_SIZE)
            continue;
        /* 当前 /dev/amp_ipi RX 路径只回传业务数据 */

        /* 兼容：
         * - AMPB：拆包写回 TUN
         * - 否则：按单个原始 IP 包写回 TUN */
		//判断如果是 AMPB 批帧
        if (msg.len >= sizeof(amp_batch_hdr_t) && memcmp(msg.data, AMP_BATCH_MAGIC, 4) == 0) {
            const amp_batch_hdr_t *hdr = (const amp_batch_hdr_t *)msg.data;     //把数据头指向批次帧数据部分准备解析
            uint16_t count;
            size_t off;
            uint16_t i;

            if (hdr->version != AMP_BATCH_VERSION)
                continue;

            count = ntohs(hdr->count_be);       //网络序子包数量转换成主机序子包数量count
            off = sizeof(amp_batch_hdr_t);      //off赋值为聚合帧头长度
            for (i = 0; i < count; i++) {
                uint16_t zl;       //2字节子帧长度位

                if (off + 2 > msg.len)          //如果聚合帧头长度off加上2字节长度超过了msg长度则退出循环(防越界)
                    break;
                zl = read_be16_unaligned(msg.data + off);    //（从msg.data + off地址往后读2个字节）赋值给zl表示这一子帧长度
                off += 2;               //把读指针往后挪 2 个字节：跳过刚才读掉的长度字段，指向真正的包内容起始
                if (off + zl > msg.len) //检查：缓冲区里剩下的字节是否足够放下一个完整子包
                    break;
                if (tun_write_packet(tun_fd, msg.data + off, zl) != 0)
                    perror("write(tun)");
                off += zl;
            }
        } else {        //否则就是单包IP包，直接整个写回TUN
            if (tun_write_packet(tun_fd, msg.data, msg.len) != 0)
                perror("write(tun)");
        }
    }

    return NULL;
}
