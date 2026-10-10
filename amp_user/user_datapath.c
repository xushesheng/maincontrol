/**************************/
/*    业务数据收发线程     */
/**************************/
#include <stdio.h>                 /* fprintf / perror */
#include <string.h>                /* memset / memcpy / memcmp */
#include <unistd.h>                /* read / write / usleep / close */
#include <fcntl.h>                 /* fcntl / O_NONBLOCK */
#include <errno.h>                 /* errno / EAGAIN / EINTR */
#include <poll.h>                  /* poll() 多路复用 */
#include <time.h>                  /* time()：诊断日志按秒限频用 */
#include <stddef.h>                /* offsetof / size_t */
#include <sys/ioctl.h>             /* ioctl() */
#include <sys/socket.h>            /* socket 地址结构 */
#include <linux/if.h>              /* IFNAMSIZ / struct ifreq */
#include <linux/if_tun.h>          /* TUN 设备 ioctl（TUNSETIFF / IFF_TUN） */
#include <netinet/ip.h>            /* struct iphdr（IP 头结构） */
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

/* 对UDP业务做端口分流：
 * UDP 源/目的端口任一落在业务端口区间 [下限, 上限] 内即进入业务面
 * （区间默认 3380~3480，可由网管下发 0x22 配置帧修改并持久化）；
 * 3409/3419 保留给独立控制面（指令入口/回执出口），无论业务区间
 * 怎么配置都在此处"挖洞"优先排除，不进入TUN业务通道；
 * 其他UDP流量当前不进入AMP业务面。
 * 注意：非 static —— 广播桥接入口（user_bcast.c 抓到的 eth0 二层广播）复用同一份策略，
 * 保证"TUN 入口"与"广播桥接入口"的准入规则不会分叉。 */
int classify_udp_business_port(const uint8_t *pkt, size_t len)
{
    const struct iphdr *ip;
    size_t ihl;
    const struct udphdr *udp;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t range_snapshot;    /* 业务区间打包值的一次快照 */
    uint16_t biz_min;
    uint16_t biz_max;

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

    /* "挖洞"：控制端口优先于业务区间排除。
     * 3409 是网管指令入口，3419 是控制回执出口——即便业务区间把这两个
     * 端口覆盖在内（如默认区间 3380~3480），控制面流量也不会被吸进业务面 */
    if (src_port == CONTROL_PORT || dst_port == CONTROL_PORT ||
        src_port == CONTROL_REPORT_PORT || dst_port == CONTROL_REPORT_PORT)
        return -1;

    /* 先取一次快照再比较：配置线程可能在判断过程中改写业务区间。
     * g_business_range 是 32 位打包值（下限<<16|上限），对齐读写在
     * ARM64 上天然原子，一次快照即可同时取到配套的下限/上限，
     * 不会出现"新下限+旧上限"的撕裂区间 */
    range_snapshot = g_business_range;
    biz_min = biz_range_min(range_snapshot);
    biz_max = biz_range_max(range_snapshot);

    if ((src_port >= biz_min && src_port <= biz_max) ||
        (dst_port >= biz_min && dst_port <= biz_max))
        return 1;
    return -1;
}

/************************************************************* * 
* 线程1：从TUN读取需要“跨射频”的IP包，写入驱动（-> CPU1 -> 对端）*
* 注：不做小包聚合，每个 IP 包单独下发（一包一次 write(/dev/amp_ipi) + 一次 SGI15）*
 ***************************************************************/
void *tun_to_amp_thread(void *arg)
{
    uint8_t buf[MAX_PAYLOAD_SIZE];

    (void)arg;
    (void)set_nonblock(tun_fd);

    while (g_running) {
        struct pollfd pfd = { .fd = tun_fd, .events = POLLIN };   //初始化poll阻塞，设置标识符为tun_fd,events为pollin可读
        int prc = poll(&pfd, 1, -1);                              //无限阻塞：只在 TUN 可读时唤醒

        if (prc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll(tun)");
            break;
        }

        /* 尽可能把当前可读的数据读空（non-blocking） */
        while (1) {
            ssize_t n = read(tun_fd, buf, sizeof(buf));         //从tun中取一个IP包
            struct iphdr *ip;
            size_t pkt_len;
            uint32_t dst_ip;

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
            if (ip->version != 4) {
                continue;
            }
            if (ip->daddr != BROADCAST_IP_BE && !is_peer_pc_addr(ip->daddr)) {     //广播+对端节点IP才走AMP通道
                continue;
            }

            pkt_len = (size_t)n;      //定义pkt_len设置为本条IP包长度
            dst_ip = ip->daddr;       //定义dst_ip赋值为当前IP包目的地址

            if (classify_udp_business_port(buf, pkt_len) < 0) {
                continue;
            }

            /* 不做聚合：直接单包下发 */
            (void)amp_send_msg(dst_ip, buf, pkt_len);
        }
    }

out:
    return NULL;
}

/**************************************************************************** 
* 线程2：从驱动read()取出对端发来的IP包，写回TUN，让内核继续路由到eth0发给本地PC *
* 注：线上只跑裸 IP 包（已取消 AMPB 批帧），读到即整包写回 TUN                    *
*****************************************************************************/
void *amp_to_tun_thread(void *arg)
{
    struct amp_net_msg msg;

    (void)arg;

    while (g_running) {
        ssize_t n = read(amp_fd, &msg, sizeof(msg));
		/*********读到数据后过滤一遍下列条件**********/

        if (n < 0) {
            if (errno == EINTR)                     /* 被信号打断：立即重试，不算错误 */
                continue;
            if (errno == ENODEV)                    /* 驱动已卸载（remove 时 count 置 -1）：无法恢复，退出线程 */
                break;
            /* 其余瞬时错误（EIO/EFAULT 等）：绝不能直接退出线程 —— 否则用户态不再消费下行包，
             * 驱动侧仍持续入队而本地 PC 永久收不到数据。这里限频打印（每秒最多一条）+ 退避后重试。 */
            {
                static time_t last_err_log = 0;
                time_t now = time(NULL);
                if (now != last_err_log) {
                    last_err_log = now;
                    perror("read(amp)");
                }
            }
            usleep(1000);
            continue;
        }
        if ((size_t)n < offsetof(struct amp_net_msg, data))
            continue;
        if (msg.len == 0 || msg.len > MAX_PAYLOAD_SIZE){
            fprintf(stderr, "[WARN] len invalid\n");
            continue;
        }
        /* 当前 /dev/amp_ipi RX 路径只回传业务数据 */

        /* 线上只跑裸 IP 包（已取消 AMPB 批帧）：直接整个写回 TUN
         * 广播包：内核对此只会做本地投递、不会从 eth0 发出（详见 user_bcast.c 顶部说明），
         * 因此由用户态在 eth0 上补发一份二层广播，本地 PC 才收得到 */
        if (tun_write_packet(tun_fd, msg.data, msg.len) != 0)
            perror("write(tun)");
        bcast_maybe_relay(msg.data, msg.len);
    }

    return NULL;
}
