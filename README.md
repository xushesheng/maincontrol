# zhukong

这是一个面向 Zynq AMP 场景的主控侧跨核通信工程（驱动以 `xlnx,zynq-amp` 兼容字符串匹配，平台相关配置见目标 BSP）。当前仓库聚焦 Linux 主控侧实现，按驱动侧与用户态侧拆分为两个目录：

- `amp_driver/`：Linux 内核侧 AMP IPC 驱动，负责 `/dev/amp_ipi` 业务接口、`/dev/amp_ctrl` 控制接口、共享内存与寄存器映射、SGI 中断收发，以及 CPU0 与 CPU1 之间的数据搬运。
- `amp_user/`：Linux 用户态主控程序，负责 `rf0` TUN 设备、静态节点表路由、业务数据聚合转发、控制 UDP 报文与驱动控制通道之间的透传；近期新增了业务端口运行时配置（0x21 帧）、开机时钟下发（0x19 帧）、组播链路维护（通信设备↔指挥协同计算机）与广播二层桥接（绕开被内核 local 表架空的广播路由）等模块。

当前仓库不包含 CPU1 配套程序、完整设备树节点和目标 BSP 工程，因此它描述的是主控侧实现，而不是一套可单独脱离目标板直接运行的完整系统。

## 根目录说明

- `AGENTS.md`：仓库内代理协作约束。
- `README.md`：项目说明与当前实现梳理。
- `amp_driver/`：驱动侧源码与构建文件。
- `amp_user/`：用户态源码与构建文件。
- `bcast_test.c`：广播下发测试工具（调试用，不参与主程序构建）。从本机向 `192.168.1.255:3408` 发 UDP 广播，用于验证“本机输出路由 → rf0 → 用户态准入 → /dev/amp_ipi → node 255”链路。
- `pkt_sniff.c`：基于 AF_PACKET 的极简抓包工具（调试用，需 root）。用于板载 busybox 无 `tcpdump` 时观察某网口（eth0/rf0）的入向/出向 IPv4 包，辅助广播与路由问题定位。

> 注：仓库当前**不含** SNMP / AMP-MIB 代理源码（早期设计中的 `amp_user/AMPs_mib.c`、`AMPs_mib.h` 与 `AMP-MIB.txt` 未纳入本仓库），仅保留主控侧 C 实现。协议文档（`宽带无线管理软件模块交换协议`、`网管与主控通信协议` 等）亦不在仓库内，相关帧结构以代码中的注释与结构体为准。

## 整体数据路径

### 业务数据路径

主控侧业务数据的典型路径如下：

1. 本地 PC 发往远端 PC 的 IPv4 包进入板卡 `eth0`。
2. 用户态程序通过 `proxy_arp`、`/32` 路由和 `rf0` TUN，把目标为对端 PC 的报文导入 `rf0`。
3. 用户态从 `rf0` 读取报文。所有发往对端 PC 的 IPv4 包（含 TCP/ICMP 等非 UDP 协议）均进入业务通道；UDP 层面按业务端口放行（**默认 `3408`**，可由网管 0x21 帧动态调整），排除 `3409` 控制端口并过滤其余 UDP 流量。对小包进行 `AMPB` 聚合，对大包直接发送，并通过 `/dev/amp_ipi` 写给驱动。
4. 驱动把数据写入 TX 共享区，填写目标 IP、节点号、长度等元数据，然后触发 CPU0 → CPU1 的 SGI（SGI15）。裸机 CPU1 固件实际运行在 **core 3**，驱动通过 `smp_kick_ipi(cpumask_of(3), AMP_SGI_TX)` 通知。
5. CPU1 返回的数据写入 RX 共享区后，通过 SGI 通知 CPU0（SGI14）。
6. 驱动把 RX 共享区数据搬入软件环形队列，用户态再从 `/dev/amp_ipi` 读出。
7. 用户态把解出的业务包重新写回 `rf0`，由内核继续路由到 `eth0` 发给本地 PC。

### 控制数据路径

主控侧控制数据的典型路径如下：

1. 网管软件通过 UDP `3409` 向主控发送控制帧。
2. 用户态控制模块校验 XOR，并通过 `/dev/amp_ctrl` 把完整控制帧写给驱动。
3. 驱动把控制数据写入控制 TX 区，填写长度、类型，并通过 `0x38005000` 的 `bit1` 通知 CPU1 读取控制区。
4. CPU1 上报控制数据时，驱动中断处理按 `0x39005000` 的 `bit1` 分支识别，调用 `enqueue_ctrl_rx_msg()` 把控制帧放入控制 RX 环形队列，再通过 `/dev/amp_ctrl` 送到用户态（控制面回传路径已恢复）。前提是 CPU1 上报控制帧时已置位 `bit1` 并将帧写入 `0x39003000` 控制 RX 区。
5. 用户态控制模块从 `/dev/amp_ctrl` 读出 CPU1 上报的控制帧，不改载荷内容，通过 UDP 发送回报。当前回报目标地址固定为本机 `eth0` 地址，端口为 `3419`（见 `user_config.h` 中 `CONTROL_REPORT_PORT`）。

**控制帧的两个特殊类型（主控本地消费，不经驱动下发 CPU1）：**

- **业务端口配置帧 `0x21`**：网管下发新的业务 UDP 端口，主控本地解码 BCD 端口、校验范围 `[1024, 65535]` 且不冲突 `3409`/`3419`，合法则持久化到 `/etc/amp_business_port.conf` 并切换生效端口，回 `ACK(0x30)`/`NACK(0x40)`。详见「用户态关键机制 §6」。
- **开机时钟下发帧 `0x19`**：主控启动后延迟 `CLOCK_BOOT_DELAY_SEC`(3s) 下发一次给路由（目的 ID `0x0C00` / 源 `0x0A00`，同步序列 `0xFFF5`，BCD 时间字段），带 `CLOCK_BOOT_RETRY`(10) 次重试、间隔 1s，失败不致命。

### 广播数据路径（user_bcast.c，绕开内核路由）

`192.168.1.255` 恰好是 `eth0` 自身网段（`192.168.1.0/24`）的定向广播，内核在配置 `eth0` 地址时会在 **local 表**（`ip rule` pref 0，先于 main 表 32766）自动生成 `broadcast 192.168.1.255 dev eth0`；用户态加在 main 表的 `192.168.1.255/32 dev rf0` 被架空，导致该广播**双向都不走 rf0**（上行被本地投递丢弃、下行只本地投递不从 eth0 发出）。用“删 local 表项”修不可行（重启/重跑网关规则会重建，且删后下行会自环风暴）。

因此广播改由用户态 `user_bcast.c` 用 AF_PACKET 在 `eth0` 上直接做二层，绕开内核路由与转发策略：

- **上行（PC → 板卡 → 射频）**：`bcast_uplink_thread` 用 `AF_PACKET(ETH_P_ALL)` 在 `eth0` 抓二层广播帧，按 IP 头 `tot_len` 裁剪填充字节后，直接 `amp_send_msg(BROADCAST_IP_BE=192.168.1.255)` → `/dev/amp_ipi`（驱动 `ip_to_nodeid` 映射为 node 255）。
- **下行（射频 → 板卡 → 本地 PC）**：`amp_to_tun_thread` 把广播包写回 `rf0` 的同时调用 `bcast_maybe_relay()`，在 `eth0` 上补发一份二层广播（目的 MAC `ff:ff:ff:ff:ff:ff`），本地 PC 才能收到（内核路由只对广播做本地投递、不会出 eth0）。
- **回环抑制**：多板卡挂在同一广播域时，本机刚中继出去的帧（FNV-1a 指纹，1s 窗口）再次被抓到则不再上射频，避免“二跳回环”。
- **开关**：`user_config.h` 的 `BCAST_BRIDGE_ENABLE`（默认 1）。若改为由内核路由转发广播，必须把该宏置 0，否则同一包会被送上射频两次。

## 驱动侧目录 `amp_driver/`

### 文件说明

- `driver_main.c`
  - 提供 `/dev/amp_ipi` 业务接口和 `/dev/amp_ctrl` 控制接口的 `read()` / `write()`。
  - 完成平台驱动的 `probe/remove`、misc 设备注册和 SGI 处理函数（`set_ipi_handler(AMP_SGI_RX=14, ...)`）注册。
  - TX 写路径共用一把互斥锁 `amp_tx_lock`，避免业务/控制并发置位共享 TX 控制寄存器时发生竞争；另有 `amp_read_lock` / `amp_ctrl_read_lock` 分别串行化业务/控制读。
  - `remove()` 实现有序卸载：注销 SGI → 唤醒阻塞读线程 → 注销 misc 设备 → 互斥锁屏障等待读写返回 → 释放 IO 映射。
- `driver_txrx.c`
  - 负责业务数据和控制数据的发送处理。
  - 维护 IP 与节点号映射关系（`ip_to_nodeid` / `nodeid_to_ip`）。
  - 处理 CPU1 → CPU0 的 RX 中断（`cpu1_to_cpu0_handler`，SGI14）。按 `0x39005000` 的 `bit0`/`bit1` 分支分别将业务帧、控制帧入队到各自的 RX 环形队列；`bit0` 与 `bit1` 均未置位时按业务兜底处理。
  - 实现 TX 忙等待机制：写共享内存前轮询 `0x38005000` 等待 CPU1 消费上一帧，超时 5ms 后返回 `-EBUSY` 拒绝覆盖。
  - 通过 `smp_kick_ipi(cpumask_of(3), AMP_SGI_TX)` 通知运行在 core 3 的裸机 CPU1。
- `driver_map.c`
  - 负责业务/控制共享内存、数据模式寄存器的 `ioremap_nocache`、初始化和释放（18 个物理地址统一映射表）。
  - 定义并初始化业务 RX 队列、控制 RX 队列及其统计变量（入队/出队/满丢包/TX 超时计数、等待队列、自旋锁）。
- `driver_hardware.h`
  - 定义业务/控制 TX/RX 共享区地址、数据模式寄存器地址、SGI 编号、跨文件共享变量和函数声明。
  - 定义驱动侧业务 RX 队列和控制 RX 队列结构及深度（`RX_RING_SIZE=64`）。
  - 定义 TX 忙等待轮询参数（5ms 超时 / 100µs 步进）及 `tx_busy_timeout` 等统计变量声明。
  - 定义受 `amp_verbose` 控制的打印宏 `amp_pr_info` / `amp_pr_warn`（运行时通过 `/sys/module/driver_amp/parameters/amp_verbose` 开关）。
- `driver_struct.h`
  - 定义 `amp_net_msg` 业务结构和 `amp_ctrl_msg` 控制结构。
- `Makefile`
  - 驱动侧模块构建文件：`ARCH=arm64` + `aarch64-linux-gnu-` 前缀的交叉编译器，依赖外部内核源码树（`KERNEL_SRC`）与目标设备树（compatible `xlnx,zynq-amp`）。

### 当前驱动侧关键机制

- `/dev/amp_ipi` 负责业务面，`/dev/amp_ctrl` 负责控制面。
- TX 共享区位于 `0x3800xxxx` 地址段，RX 共享区位于 `0x3900xxxx` 地址段。
- `0x38005000` 的 `bit0` 表示业务 TX 使能，`bit1` 表示控制 TX 使能。
- `0x39005000` 的 `bit0` 表示业务 RX 有效，`bit1` 表示控制 RX 有效。中断处理按 `bit0`/`bit1` 分支分别走业务/控制 RX 路径，`bit0` 与 `bit1` 均未置位时兜底按业务处理。
- 当前 IP 到节点号映射规则为：
  - `192.168.1.10 ~ 192.168.1.41 -> node_id 0 ~ 31`
  - `239.0.0.1 -> 254`
  - `192.168.1.255 -> 255`
- 当前驱动 RX 路径已不是单包缓存，而是业务/控制各自 `64` 深度的软件环形队列，用于降低用户态读取稍慢时的丢包风险。
- 裸机 CPU1 固件预期运行在 **core 3**，主控侧 SGI 一律 `cpumask_of(3)` 定向触发。

### 当前驱动侧 TX 忙等待机制

为解决 TX 共享内存单槽覆盖风险，驱动在写入新帧前会轮询等待 CPU1 消费上一帧：

- `wait_for_tx_slot_idle()` 轮询 `0x38005000` 的 `bit0` 和 `bit1`，等待两者均为零（表示 CPU1 已读走上一帧）。
- 每次轮询间隔 `100 µs`，超时阈值 `5 ms`（最多 50 次）。
- 轮询期间响应进程信号（`signal_pending`），收到信号返回 `-ERESTARTSYS`。
- 超时返回 `-EBUSY`，调用方 `amp_write()` / `amp_ctrl_write()` 将错误传播到用户态，同时递增 `tx_busy_timeout` 统计计数。
- TX 共享区由 `ioremap_nocache()` 映射为 non-cacheable 内存，写入顺序由 `wmb()`/`mb()` 保证，不再使用多余的 cache flush 操作。

## 用户态目录 `amp_user/`

### 文件说明

- `user_main.c`
  - 用户态主入口。打开 `/dev/amp_ipi` 与 `/dev/amp_ctrl`，创建 `rf0`，载入业务端口配置，配置网关规则，初始化控制/组播/广播 socket，开机下发 0x19 时钟帧，并启动 **7 个工作线程**。
- `user_datapath.c`
  - 实现 `rf0` TUN 创建与读写。
  - 实现业务数据发送、`AMPB` 聚合、单包直发和从驱动回读数据后的 TUN 回写。
  - 实现统一发送线程 `amp_tx_thread`（串行写 `/dev/amp_ipi`，避免并发踩 TX 单槽）。
  - 实现 TUN 入口的 UDP 业务端口准入 `classify_udp_business_port()`（与广播桥接入口共用，避免两条入口规则分叉）及准入丢弃/广播接受诊断日志。
- `user_control.c`
  - 负责控制 UDP `3409` 的收发、控制帧 XOR 校验、版本/工作参数上报帧解析与缓存。
  - 通过 `/dev/amp_ctrl` 与驱动交换完整控制帧，并把 CPU1 上报的控制帧通过 UDP 回发（目标本机 `eth0` 地址，端口 `3419`）。
  - 识别并**本地消费**网管下发的业务端口配置帧（`0x21`：解码 BCD → 校验 → 持久化 → 回 ACK/NACK）。
  - 实现开机时钟下发帧（`0x19`）的构造与带重试发送。
  - 对外提供 `ctrl_get_work_status()`，从工作参数上报缓存取出通信设备状态与静默/辐射状态，供组播链路模块使用。
- `user_gateway.c`
  - 负责根据本机 `eth0` 地址在 32 节点静态表中定位自身。
  - 配置 `rf0` 地址、`proxy_arp`、路由（含 `192.168.1.255/32 dev rf0`）、和相关 `sysctl`。
- `user_portcfg.c`
  - 业务 UDP 端口的持久化模块。从 `/etc/amp_business_port.conf` 载入端口（缺失/非法则回落默认 3408）；校验端口范围与冲突；以“临时文件 + rename”原子写方式持久化网管下发的新端口。
- `user_link.c`
  - 组播链路维护模块（通信设备 ↔ 指挥协同计算机）。按协议 `20260912` 周期发送 `0xA1`/`0xA2`/`0xA4`/`0xA5` 上行报文并接收 `0x01` 反馈维持建链/断链判定。
- `user_bcast.c`
  - 广播二层桥接模块。用 AF_PACKET 在 `eth0` 上收发 `192.168.1.255` 二层广播，绕开内核路由（local 表架空广播路由）：上行送 `/dev/amp_ipi`（node 255），下行在 `eth0` 补发二层广播给本地 PC，并用指纹环抑制二跳回环。
- `user_struct.h`
  - 定义用户态 `amp_net_msg`、`amp_ctrl_msg`、`AMPB` 帧头和批次状态结构，以及大端字段的安全读写辅助。
- `user_config.h`
  - 定义业务/控制设备名、业务/控制端口、业务端口运行时配置、批帧大小、聚合超时、发送队列深度、发送保护间隔、`rf0` MTU、广播桥接与组播链路相关配置宏。
- `user_declaration.h`
  - 汇总全局变量、业务发送队列结构、控制/组播/广播线程入口和跨文件函数声明。
- `Makefile`
  - 用户态程序构建文件，输出目标为 `user_amp`（7 个源文件，链接 `pthread`）。

### 当前用户态关键机制

#### 1. 静态 32 节点表

当前用户态内置了 32 节点（node_id 0 ~ 31）的静态节点表：

| 字段 | 映射规则 |
|------|---------|
| `node_id` | 0 ~ 31 |
| `board_ip`（eth0） | `192.168.1.(50 + node_id)` |
| `pc_ip`（本地 PC） | `192.168.1.(10 + node_id)` |
| `rf0` 地址 | `10.255.0.(10 + node_id)/24` |

用户态启动时会校验：

- 当前 `eth0` 地址是否在节点表中找到自身（匹配 `board_ip`）。
- 每个 `pc_ip` 是否满足 `192.168.1.(10 + node_id)` 的驱动映射规则。
- `node_id`、`board_ip`、`pc_ip` 是否存在重复。
- 节点表为空或配置错误时程序直接退出。

#### 2. 业务数据聚合与直发

- 业务 UDP 端口默认 `3408`（见 §6，可由网管 0x21 帧动态修改），控制 UDP 端口固定 `3409`。
- 默认最大聚合帧长度为 `640` 字节。
- 聚合魔数为 `AMPB`，版本号为 `1`。
- 第一个小包进入批次后，最多等待 `60 ms` 再看能否聚合更多小包。
- 若单个包长度超过 `640` 字节，则不参与聚合，直接单包发送。
- `ICMP Echo/Echo Reply` 默认走快速通道，尽量降低 `ping` 时延（可通过 `user_config.h` 中的 `AMP_ICMP_FASTPATH` 宏关闭）。

#### 3. 用户态发送串行化

当前用户态发送侧包含以下机制：

- 单独的发送线程 `amp_tx_thread` 负责真正调用 `write(amp_fd, ...)`，所有业务发送都经 `amp_tx_runtime` 队列，避免多个线程并发踩写 TX 单槽。
- 每次写完 `/dev/amp_ipi` 后增加一个小保护间隔（`AMP_TX_GUARD_US=200`），降低 TX 单槽被连续覆盖的概率。

#### 4. 网关与转发行为

用户态会在启动时做以下配置：

- 打开 `net.ipv4.ip_forward`
- 关闭 `rp_filter`（all 与 eth0）
- 打开 `eth0` 上的 `proxy_arp`
- 拉起 `rf0`
- 设置 `rf0` MTU 为 `1600`（大于标准 1500，以允许 >640 字节的单包直发而不触发 IP 分片）
- 给 `rf0` 配置当前节点对应的 `10.255.0.x/24` 地址
- 为其他节点对应的 PC 地址配置 `proxy arp`
- 为其他节点对应的 PC 地址配置 `/32 -> rf0` 路由
- 为广播 `192.168.1.255/32` 配置 `-> rf0` 路由（实际广播收发由 `user_bcast.c` 的二层桥接完成，见「广播数据路径」）

#### 5. 控制面透传行为

- 控制面固定监听 UDP `3409`。
- 控制回报目标地址固定为本机 `eth0` 地址，端口为 `3419`（`CONTROL_REPORT_PORT`）。
- CPU1 上报控制数据时，主控不改控制载荷内容，直接通过 UDP 发送回报。
- 主控会解析并缓存控制帧头字段（版本上报、工作参数上报），但当前阶段不重新组 `back_jobmode_set/version_report` 等控制回执帧。
- 工作参数上报帧（0x20）的节点连接关系段为 **128 字节（1024 bit）**：32 个节点各占 4 字节（32 bit）一行，表示该节点与 1~32 号节点的连接情况（含自身节点号的自连接位）；bit=1 表示连接、bit=0 表示断开，字节内 MSB 在前。对应 `ctrl_work_param_report_t::NodeConnect[128]`，整帧长度 224 字节。
- 驱动侧控制 RX 入队已恢复，CPU1 上报的控制帧经 `0x39005000` 的 `bit1` 分支入控制 RX 队列后可通过 `/dev/amp_ctrl` 送到用户态，控制面回传路径已可用（需 CPU1 配合置位 `bit1`）。

#### 6. 业务端口运行时配置（0x21 帧）

业务 UDP 端口可在运行中由网管通过控制帧动态修改，主控本地消费、不转发给 CPU1：

- 帧格式：16 字节 = 12 字节帧头（类型 `0x21`）+ 3 字节 BCD 端口（高位在前，如 `3408 -> 00 34 08`）+ 1 字节 XOR 校验和。
- `control_rx_to_amp_thread` 收到 `0x21` 后：BCD 解码 → 校验范围 `[BUSINESS_PORT_MIN=1024, BUSINESS_PORT_MAX=65535]` 且不与 `3409`/`3419` 冲突 → 合法则 `portcfg_apply_and_save()` 原子写 `/etc/amp_business_port.conf` 并切换 `g_business_port`，回 `ACK(0x30)`；否则回 `NACK(0x40)`。
- 启动时 `portcfg_load()` 从配置文件载入端口；文件缺失或内容非法则回落默认 `3408`，不致命。
- `g_business_port` 由 TUN 入口（`tun_to_amp_thread`）与广播桥接入口（`user_bcast.c`）共用同一份准入策略（`classify_udp_business_port`），单写单读用 `volatile`。

#### 7. 开机时钟下发（0x19 帧）

- 主控启动后在 `CLOCK_BOOT_DELAY_SEC`(3s) 延迟后，向路由固件下发一次时钟信息帧（类型 `0x19`）。
- 帧结构 20 字节：长度(小端) / 保留 / 目的 ID `0x0C00` / 源 ID `0x0A00` / 同步序列 `0xFFF5` / 类型 `0x19` / 保留 / 年(2位BCD)月日时分秒(各 1B BCD) / XOR 校验和；头区域字段按小端发送。
- 带 `CLOCK_BOOT_RETRY`(10) 次重试、间隔 `CLOCK_BOOT_RETRY_GAP_SEC`(1s)；失败仅告警，不致命（避免 TX 单槽未释放时阻塞启动）。

#### 8. 组播链路维护（通信设备 ↔ 指挥协同计算机）

协议依据《通信设备与指挥协同计算机通信协议20260912》，全文小端：

- 角色：本机为「通信设备」（设备编号 `0x35`），对端为「指挥协同计算机」（编号 `0x03`）。
- 收发使用不同组播组：收加入 `LINK_MCAST_SELF_ADDR=224.5.1.13` 绑定 `LINK_MCAST_SELF_PORT=8600`；发 `sendto` 到 `LINK_MCAST_PEER_ADDR=224.1.1.5` : `LINK_MCAST_PEER_PORT=6200`。与现有 3408/3409/3419 均不冲突。
- `link_mcast_thread` 以 1s 为节拍（`LINK_PERIOD_MS=1000`）：
  - `0xA1` 通信状态检测报文：每 1s 持续发送。
  - `0xA2` BIT 检测报文：每 5s（`LINK_BIT_PERIOD_TICKS=5`）发送，以“已收到 CPU1 工作参数上报”作为自检完成代理判据。
  - `0xA4` 工作状态报文：建链后每 1s，携带静默/辐射状态（取自 CPU1 工作参数上报缓存 `Silent` 字段）。
  - `0xA5` 工作模式反馈报文：建链后每 1s，当前固定上报 `0xAA`（启用）。
  - 收到 `0x01` 反馈报文即标记建链；连续 `LINK_LOST_TIMEOUT_MS`(3s) 未收到则判断链。
- **尚未实现**（接收后按类型过滤丢弃，无副作用）：`0xA3` 战车身份信息反馈、`0x02` 时间信息、`0x03` 战车身份信息、`0x04` 宽带组网静默/辐射控制、`0x05` 工作模式控制。

#### 9. 广播二层桥接

详见「广播数据路径」。核心要点：广播 `192.168.1.255` 不走内核路由，而是由 `user_bcast.c` 在 `eth0` 上用 AF_PACKET 直接做二层收发；`BCAST_BRIDGE_ENABLE`(默认 1) 控制开关。上行入队 `amp_send_msg(node 255)`，下行经 `bcast_maybe_relay()` 补发二层广播，并用指纹环抑制多板卡同广播域的二跳回环。

#### 线程模型（7 个工作线程）

| 线程 | 职责 |
|------|------|
| `amp_tx_thread` | 统一写 `/dev/amp_ipi`（业务），串行化避免并发踩 TX 单槽 |
| `tun_to_amp_thread` | `rf0` 读 IP 包 → 端口准入 → 聚合/单发 → 发送队列 |
| `amp_to_tun_thread` | `/dev/amp_ipi` 读 → 拆 `AMPB`/原始 IP → 写回 `rf0`（含广播中继） |
| `control_rx_to_amp_thread` | UDP 3409 → 0x21 本地消费 / 其余透传 `/dev/amp_ctrl` |
| `control_amp_to_udp_thread` | `/dev/amp_ctrl` → 缓存上报帧 → UDP 3419 |
| `link_mcast_thread` | 组播链路维护（检测/状态报文发送与反馈接收） |
| `bcast_uplink_thread` | `eth0` 二层广播 → `/dev/amp_ipi`（node 255） |

## 协议与数据结构

### `amp_net_msg`

驱动和用户态共享同一套基础消息结构：

- `ip`：目标 IP 或返回数据的来源 IP（网络字节序）
- `node_id`：目标节点号或来源节点号（0~31 单播 / 254 组播 / 255 广播）
- `len`：数据长度
- `data_type`：业务类型字段；当前统一写 `1`（业务），`2` 表示语音
- `data[]`：实际载荷，最大 `4096` 字节

### `amp_ctrl_msg`

控制面使用独立消息结构：

- `len`：控制帧长度
- `data_type`：控制类型字段；当前阶段固定写 `1`
- `data[]`：完整控制帧载荷，最大 `4096` 字节

### `AMPB` 批帧

用户态业务聚合使用 `AMPB` 批帧格式：

- 魔数：`AMPB`
- 版本：`1`
- 标志位：当前固定为 `0`
- 子包数量：大端格式（`count_be`，`#pragma pack(1)` 12 字节帧头）
- 批帧序号：大端格式（`seq_be`）
- 每个子包前带 `2` 字节大端长度字段

## 构建说明

### 用户态构建

进入 `amp_user/` 后执行：

```bash
cd amp_user
make
```

生成目标：

- `user_amp`

用户态 `Makefile` 当前使用 **`aarch64-linux-gnu-gcc`**（FMQLMP-Linux-SDK-Prj-20250408 工具链，ARM64），源文件为 7 个 `.c`（`user_main` / `user_gateway` / `user_datapath` / `user_bcast` / `user_portcfg` / `user_control` / `user_link`），链接 `pthread`。

### 驱动侧构建

驱动侧也提供了 `Makefile`，但它依赖外部环境：

- 目标内核源码树（外部 `KERNEL_SRC`，如 `linux-5.4.52-fmsh`）
- 交叉编译器：`aarch64-linux-gnu-` 前缀（`ARCH=arm64`）
- 目标板匹配的内核配置与设备树（compatible `xlnx,zynq-amp`）

进入 `amp_driver/` 后可按 `Makefile` 中给定的 `KERNEL_SRC` 和 `CROSS_COMPILE` 进行模块构建，这部分路径当前是针对特定 BSP 环境写死的，使用前需要根据实际环境调整。

## 运行前提

当前程序要在目标板上正常工作，至少需要满足以下条件：

- Linux 内核中启用了 TUN/TAP 支持。
- 系统中存在 `/dev/net/tun`。
- `eth0` 为有效数据接口，且配置了节点表中的板卡 IP（`192.168.1.(50 + node_id)`）。
- 驱动已正确加载并暴露 `/dev/amp_ipi` 和 `/dev/amp_ctrl`。
- CPU1 侧程序（裸机固件，预期运行在 core 3）与当前共享内存地址、寄存器语义和 SGI 编号保持一致。
- 运行用户态程序时具备足够权限，以便配置 `sysctl`、创建 TUN、调整路由和邻居项。
- 广播功能依赖 `user_bcast.c` 的 AF_PACKET 能力（需 `CAP_NET_RAW`）；若 `BCAST_BRIDGE_ENABLE=0` 且未另行处理 local 表，广播将不通。

## 当前限制与说明

- 仓库仅包含主控侧实现，不包含 CPU1 配套程序。
- 仓库不包含完整设备树节点与 BSP 集成内容。
- 当前节点表是静态 32 节点表，不是动态配置。
- 当前用户态默认只处理 IPv4 业务流。
- 业务类型字段保留了扩展空间（1=业务，2=语音），这一版语音路径尚未与对端完整联调。
- 业务 UDP 端口已支持运行时配置（0x21 帧 + 持久化），但仅本机主控消费，不下发 CPU1。
- 组播链路模块已发送 `0xA1/0xA2/0xA4/0xA5` 并接收 `0x01` 反馈；`0xA3/0x02~0x05` 等帧预留但未实现。
- 广播依赖用户态二层桥接（`user_bcast.c`）绕开内核路由；若关闭桥接（`BCAST_BRIDGE_ENABLE=0`）且未清理 local 表，广播双向均不通。
- 驱动侧已实现 TX 忙等待机制（写入前轮询等待 CPU1 消费，5ms 超时拒绝覆盖），配合用户态发送串行化和互斥锁，可在 CPU1 正常响应时避免单槽覆盖。极端高负载下（CPU1 处理超过 5ms）仍可能触发 `-EBUSY` 丢帧。
