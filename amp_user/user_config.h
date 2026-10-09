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
#define BUSINESS_PORT 3408                  /* 业务 UDP 端口：数据走此端口进入 AMP 通道（默认值，配置文件缺失或非法时回落至此） */
#define CONTROL_PORT 3409                   /* 控制 UDP 端口：网管指令走此端口 */
#define CONTROL_REPORT_PORT 3419            /* 控制回执端口：CPU1 上报转发至此端口 */

/* ========= 业务端口运行时配置（网管下发，主控本地消费，不透传 CPU1） ========= */
#define CTRL_FRAME_TYPE_BUSINESS_PORT 0x21  /* 网管下发业务端口配置帧的类型码 */
#define BUSINESS_PORT_FRAME_LEN 16          /* 端口配置帧总长：12 字节帧头 + 3 字节 BCD + 1 字节校验和 */
#define BUSINESS_PORT_BCD_BYTES 3           /* 端口 BCD 编码字节数：3 字节 = 6 位十进制 */
#define AMP_PORT_CONF_FILE "/etc/amp_business_port.conf"  /* 业务端口持久化配置文件路径 */
#define BUSINESS_PORT_MIN 1024              /* 合法业务端口下限：避开特权端口与常见服务端口 */
#define BUSINESS_PORT_MAX 65535             /* 合法业务端口上限 */

/* 业务数据统一串行下发到驱动，避免并发踩写 TX 单槽 */
#define  AMP_TX_QUEUE_DEPTH 64

/* 每次写完 /dev/amp_ipi 后，留一个很小的保护间隔，降低 TX 单槽覆盖概率 */
#define AMP_TX_GUARD_US 200

/* rf0 MTU：按标准以太网设置，保证业务 IP 包可完整注入 TUN */
#define RF0_MTU 1500

/* 广播地址 192.168.1.255：用于与 struct iphdr.daddr（__be32）直接比较。
 * 注意：不能写成 0xC0A801FF —— 那只是"书写顺序"上的大端数值，在小端主机上与
 * ip->daddr 的 uint32 表示（0xFF01A8C0）并不相等，会导致广播包在
 * user_datapath.c 的准入判断里被静默丢弃（实测：驱动收不到任何 TX 请求）。
 * htonl() 可保证该值与 ip->daddr 在大小端主机上都一致。 */
#define BROADCAST_IP_BE ((uint32_t)htonl(0xC0A801FFu))

/* ========= 开机自启动时钟下发（协议类型 0x19） ========= */
#define CTRL_FRAME_TYPE_CLOCK   0x19    /* 主控自主下发时钟信息 */
#define SRIO_ID_MASTER          0x0A00  /* 主控源 ID（协议 ID 表） */
#define SRIO_ID_ROUTER          0x0C00  /* 路由目的 ID（协议 ID 表） */
#define CLOCK_SYNC_MAGIC        0xFFF5  /* 同步序列 */
#define CLOCK_BOOT_DELAY_SEC    3       /* 启动后延迟若干秒再发，等路由固件就绪 */
#define CLOCK_BOOT_RETRY        10      /* 发送失败（TX 单槽未释放等）最多重试次数 */
#define CLOCK_BOOT_RETRY_GAP_SEC 1      /* 重试间隔（秒） */

/* ========= 开机自启动频表下发（协议 表5.11，类型 0x09，见 user_freqtable.c） ========= */
#define CTRL_FRAME_TYPE_FREQTABLE   0x09    /* 主控开机下发非自适应跳频频表设置帧 */
#define FREQTABLE_LAST_SENT_FILE "/data/lastSentFreqTable.json"  /* 网管 jar 数据目录：最近下发频表号（占位路径，部署时按实际 data 目录修改） */
#define FREQTABLE_STORE_FILE     "/data/freqtable.json"         /* 网管 jar 数据目录：频表库（占位路径，部署时按实际 data 目录修改） */
#define FREQTABLE_BOOT_DELAY_SEC    3       /* 组帧成功后延迟若干秒再发，等路由固件就绪（与 0x19 同款） */
#define FREQTABLE_BOOT_RETRY        10      /* 写 /dev/amp_ctrl 失败最多重试次数 */
#define FREQTABLE_BOOT_RETRY_GAP_SEC 1      /* 重试间隔（秒） */
#define FREQTABLE_TABLE_ID_MAX      99      /* 频表号上限（1 字节 BCD：0~99） */
#define FREQTABLE_POINT_ID_MAX      511     /* 频点编号上限（2 字节 BCD：0~511） */
#define FREQTABLE_FREQ_MIN          470     /* 频点频率下限（MHz，2 字节 BCD） */
#define FREQTABLE_FREQ_MAX          790     /* 频点频率上限（MHz） */
#define FREQTABLE_POINT_MAX         321     /* 协议单帧频点总数上限（网管库可存 512，超出整表拒发） */
#define FREQTABLE_FRAME_MAX         (16 + 4 * FREQTABLE_POINT_MAX)  /* 最大整帧：12 头 + 3 载荷头 + 4×321 + 1 校验 = 1300 字节 */
#define FREQTABLE_JSON_MAX_BYTES    (8 << 20)  /* 两个 JSON 文件大小上限（8MiB）：全库 100 表×512 点 pretty-print 约 2.6MB，
                                                    * 1MiB 会误拒极端场景；cJSON 解析 DOM 峰值内存约为文件大小的数倍（瞬时，
                                                    * 仅启动期、业务线程未创建时），解析完即释放，板卡内存可承受 */

/* ========= 广播二层桥接（用户态 AF_PACKET，见 user_bcast.c） ========= */
/* 背景：192.168.1.255 是 eth0 自身网段的定向广播，内核在 local 表里自动生成
 * `broadcast 192.168.1.255 dev eth0`（pref 0 先于 main 表 32766），导致：
 *   上行：PC 发来的广播被当"本机广播"本地投递（3408 无 socket 即丢），永远进不了 rf0；
 *   下行：应用 write(tun_fd) 注入的广播也只本地投递（RTN_BROADCAST 走 brd_input →
 *         RTCF_LOCAL + dst.output=ip_rt_bug），永远不从 eth0 发给本地 PC。
 * 因此广播收发改由用户态用 AF_PACKET 在 eth0 上直接做二层，绕开内核路由/转发策略。 */
#define BCAST_BRIDGE_ENABLE 1       /* 1=启用广播桥接；**若日后改由内核路由转发广播，必须置 0**，否则同一包会重复上射频 */
#define BCAST_BUF_MAX 2048          /* 抓包缓冲区（业务包不超过 rf0 MTU 1600，2048 足够） */
#define BCAST_RELAY_MAX_BYTES 1500  /* 下行中继允许的最大 IP 包长（eth0 MTU；AF_PACKET 发送超 MTU 会 EMSGSIZE） */
#define BCAST_RELAY_DEDUP_MS 1000   /* 中继指纹去重窗口（抑制多板卡同一广播域造成的二跳回环） */
#define BCAST_RELAY_RING 8          /* 中继指纹环大小 */

/* ========= 组播链路维护（通信设备 <-> 指挥协同计算机） ========= */
/* 协议依据：《通信设备与指挥协同计算机通信协议20260912》
 * 收发使用不同的组播组：
 *   收：加入 LINK_MCAST_SELF_ADDR，绑定 LINK_MCAST_SELF_PORT
 *   发：sendto 到 LINK_MCAST_PEER_ADDR : LINK_MCAST_PEER_PORT
 * 注：8600 / 6200 与现有 3408 / 3409 / 3419 均不冲突。
 * 字节序：全文小端（协议 1.2 明确"先传低位字节，后传高位字节"）。 */

#define LINK_MCAST_SELF_ADDR    "224.5.1.13"    /* 通信设备（本机）所属组播组，用于接收 */
#define LINK_MCAST_SELF_PORT    8600            /* 通信设备接收端口 */
#define LINK_MCAST_PEER_ADDR    "224.1.1.5"     /* 指挥协同计算机所属组播组，用于发送 */
#define LINK_MCAST_PEER_PORT    6200            /* 指挥协同计算机接收端口 */

#define LINK_MCAST_IFNAME       CAP_IFACE       /* 组播收发绑定的网口（eth0） */

#define LINK_DEV_ID_SELF        0x35            /* 设备编号：通信设备（本机） */
#define LINK_DEV_ID_PEER        0x03            /* 设备编号：指挥协同计算机 */

#define LINK_FRAME_HEAD         0xF00F          /* 报文起始标志 */
#define LINK_FRAME_TAIL         0x0EE0          /* 报文结束标志 */

/* 报文类型：上行（通信设备 -> 指挥）用 0xA1~0xA5，
 *           下行（指挥 -> 通信设备）用 0x01~0x05 */
#define LINK_TYPE_DETECT        0xA1            /* 上行：通信状态检测报文 */
#define LINK_TYPE_BIT           0xA2            /* 上行：BIT 检测报文 */
#define LINK_TYPE_IDENTITY_ACK  0xA3            /* 上行：战车身份信息反馈报文 */
#define LINK_TYPE_WORK          0xA4            /* 上行：工作状态报文 */
#define LINK_TYPE_MODE_ACK      0xA5            /* 上行：工作模式反馈报文 */
#define LINK_TYPE_FEEDBACK      0x01            /* 下行：通信状态检测反馈报文 */
#define LINK_TYPE_TIME          0x02            /* 下行：时间信息报文 */
#define LINK_TYPE_IDENTITY      0x03            /* 下行：战车身份信息报文 */
#define LINK_TYPE_RF_CTRL       0x04            /* 下行：宽带组网静默/辐射控制报文 */
#define LINK_TYPE_MODE_CTRL     0x05            /* 下行：工作模式控制报文 */

/* 报文长度（协议 20260912 已修正，与各字段字节数之和一致） */
#define LINK_FRAME_LEN_DETECT   18              /* 通信状态检测报文 0xA1 */
#define LINK_FRAME_LEN_BIT      14              /* BIT 检测报文 0xA2 */
#define LINK_FRAME_LEN_IDENTITY_ACK 14          /* 战车身份信息反馈报文 0xA3 */
#define LINK_FRAME_LEN_WORK     14              /* 工作状态报文 0xA4 */
#define LINK_FRAME_LEN_MODE_ACK 14              /* 工作模式反馈报文 0xA5 */
#define LINK_FRAME_LEN_FEEDBACK 13              /* 通信状态检测反馈报文 0x01 */

/* 发送周期（协议 20260912 已将原"1ms"统一修正为 1s） */
#define LINK_PERIOD_MS          1000            /* 基础节拍：1s */
#define LINK_BIT_PERIOD_TICKS   5               /* BIT 检测报文 5s = 5 个基础节拍 */
#define LINK_LOST_TIMEOUT_MS    3000            /* 连续 3s 未收到反馈报文判定断链 */

/* 报文数据字段取值（均为 1 字节 unsigned char） */
#define LINK_DEV_STATUS_NORMAL  0x33            /* 通信设备状态（BIT 检测）：正常 */
#define LINK_DEV_STATUS_FAULT   0xAA            /* 通信设备状态（BIT 检测）：故障 */
#define LINK_RF_SILENT          0x00            /* 静默/辐射状态（工作状态）：静默 */
#define LINK_RF_RADIATE         0x01            /* 静默/辐射状态（工作状态）：辐射 */
#define LINK_DEV_ENABLED        0xAA            /* 通信设备启用状态（工作模式反馈）：启用 */

#endif
