/*************************************/
/*          用户态配置宏定义         */
/************************************/
#ifndef USER_AMP_CONFIG_H      /* 头文件保护：防止重复包含 */
#define USER_AMP_CONFIG_H

/* ========= 设备名 ========= */
#define AMP_DATA_DEV "/dev/amp_ipi"     /* 业务数据设备节点 */
#define AMP_CTRL_DEV "/dev/amp_ctrl"    /* 控制数据设备节点 */
#define CAP_IFACE "eth0"                /* 本机承载网口名称：用于读取 IP、配置路由/proxy ARP */

#define MAX_PAYLOAD_SIZE 4096              /* 最大载荷大小（与驱动侧一致） */

/* ========= UDP 端口号定义 ========= */
#define BUSINESS_PORT 3408                  /* 业务 UDP 端口：数据走此端口进入 AMP 通道 */
#define CONTROL_PORT 3409                   /* 控制 UDP 端口：网管指令走此端口 */
#define CONTROL_REPORT_PORT 3419            /* 控制回执端口：CPU1 上报转发至此端口 */

/* 聚合帧最大长度（只对“批帧 AMPB”限制 640，单包直发不受此限制） */
#define AMP_BATCH_MAX_BYTES 640
#define AMP_BATCH_MAGIC "AMPB"
#define AMP_BATCH_VERSION 1

/* 聚合窗口：第一个包进入批次后，最多再等这么多 ms 看能不能凑更多包。
 * 调大：吞吐更好但交互/ ping RTT 更大；调小：时延更好但 SGI 次数更多。 */
#define AMP_BATCH_TIMEOUT_MS 60

/* 业务数据统一串行下发到驱动，避免并发踩写 TX 单槽 */
#define AMP_TX_QUEUE_DEPTH 64

/* 每次写完 /dev/amp_ipi 后，留一个很小的保护间隔，降低 TX 单槽覆盖概率 */
#define AMP_TX_GUARD_US 200

/* ICMP/ping 快速通道开关（1为开启，0为关闭） */
#define AMP_ICMP_FASTPATH 1

/* rf0 MTU：为了允许 >640 的 IP 包“单包直发” */
#define RF0_MTU 1600

/* 广播地址 192.168.1.255 网络字节序 */
#define BROADCAST_IP_BE ((uint32_t)0xC0A801FF)

/* ========= 开机自启动时钟下发（协议类型 0x19） ========= */
#define CTRL_FRAME_TYPE_CLOCK   0x19    /* 主控自主下发时钟信息 */
#define SRIO_ID_MASTER          0x0A00  /* 主控源 ID（协议 ID 表） */
#define SRIO_ID_ROUTER          0x0C00  /* 路由目的 ID（协议 ID 表） */
#define CLOCK_SYNC_MAGIC        0xFFF5  /* 同步序列 */
#define CLOCK_BOOT_DELAY_SEC    3       /* 启动后延迟若干秒再发，等路由固件就绪 */
#define CLOCK_BOOT_RETRY        10      /* 发送失败（TX 单槽未释放等）最多重试次数 */
#define CLOCK_BOOT_RETRY_GAP_SEC 1      /* 重试间隔（秒） */

#endif
