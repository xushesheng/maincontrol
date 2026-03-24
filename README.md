# zhukong(new)

这是一个基于 Zynq AMP 场景的跨核/跨板卡通信示例项目，代码分成两部分：

- `driver_amp.c`：Linux 内核侧 AMP IPC 驱动，负责共享内存映射、SGI 中断通知、用户态设备接口 `/dev/amp_ipi`
- `user_amp.c`：Linux 用户态转发程序，负责把 `eth1` 上需要跨链路转发的 IPv4/控制数据收集后，通过 `/dev/amp_ipi` 发给对端，同时把对端回来的数据重新注入到 `rf0` TUN 设备

仓库当前更像“核心源码快照”，不是完整可直接发布的工程。用户态程序可按 `Makefile.txt` 编译；内核驱动源码已在仓库中，但没有配套的 Kbuild/设备树/部署脚本。

## 项目用途

从代码看，这个项目要解决的是：

- CPU0 侧运行 Linux
- CPU1 侧连接射频/组网逻辑
- CPU0 与 CPU1 通过共享内存和 SGI 中断通信
- Linux 用户态把本地网络流量导入 AMP 链路，再把对端回来的流量回灌到内核网络栈
- 控制 UDP 报文单独识别，并按控制协议写入寄存器区域

可以把它理解为一个“射频链路前后的网络桥接程序”：

- 数据面：转发 IPv4 业务流量
- 控制面：捕获 `eth1` 上目标端口 `3409` 的 UDP 控制帧

## 代码分工

### 1. 内核驱动 `driver_amp.c`

主要职责：

- 将固定物理地址映射为内核虚拟地址
- 暴露 misc 设备 `/dev/amp_ipi`
- 处理用户态写入的数据包或控制帧
- 通过 `gic_raise_softirq_fmsh(1, AMP_SGI_TX)` 通知 CPU1
- 接收 CPU1 回来的 SGI 中断，把 RX 共享内存中的内容缓存后提供给用户态 `read()`

核心逻辑：

- `amp_write()`：接收用户态 `amp_net_msg`
- `process_udp_data()`：写共享内存中的 TX 数据区，并写入 IP、node_id、长度
- `process_control_data()`：校验控制帧头尾后，把控制参数写入专用寄存器
- `cpu1_to_cpu0_handler()`：收到 CPU1 通知后，从 RX 区取回数据并唤醒阻塞中的用户态读
- `zynq_amp_probe()`：做寄存器/共享内存映射、注册 IPI handler、注册 misc 设备

协议与地址特征：

- TX 共享内存基址：`0x38000000`
- RX 共享内存基址：`0x39000000`
- 最大有效负载：`4096` 字节
- 通过 `data_type` 区分数据面和控制面
- IP 与 node_id 做了固定映射：
  - `192.168.1.10` 到 `192.168.1.25` 映射到 `0` 到 `15`
  - `239.0.0.1` 映射到 `254`
  - `192.168.1.255` 映射到 `255`

### 2. 用户态程序 `user_amp.c`

主要职责：

- 打开 `/dev/amp_ipi`
- 创建 TUN 设备 `rf0`
- 配置 Linux 转发、proxy ARP、路由
- 从 `rf0` 读出需要跨链路发送的 IPv4 包，写入驱动
- 从驱动读取对端返回的数据包，重新写回 `rf0`
- 用 `pcap` 监听 `eth1` 上的控制 UDP 报文并转成控制帧发送

程序内有三个线程：

- `tun_to_amp_thread()`：`rf0 -> /dev/amp_ipi`
- `amp_to_tun_thread()`：`/dev/amp_ipi -> rf0`
- `pcap_control_thread()`：抓取 `eth1` 上 `udp dst port 3409`

#### 数据面优化

代码里做了几项比较明确的优化：

- 小包聚合：多个小 IP 包打成一个 `AMPB` 批帧后再发送
- 大包直发：大于 `640` 字节的包不参与聚合，直接单包发送
- ICMP 快通道：`ping` 报文优先快速发送，减少等待聚合窗口带来的 RTT 增大

相关参数：

- 聚合帧最大长度：`640` 字节
- 聚合超时：`96 ms`
- `rf0` MTU：`1600`

#### 网络假设

`setup_gateway_rules()` 写死了一个双板卡场景：

- 如果本机 `eth1` 是 `192.168.1.13`
  - 认为对端 PC 是 `192.168.1.15`
  - 本地 `rf0` 使用 `10.255.0.1/30`
- 如果本机 `eth1` 是 `192.168.1.12`
  - 认为对端 PC 是 `192.168.1.20`
  - 本地 `rf0` 使用 `10.255.0.2/30`

程序会自动执行这些系统配置：

- 开启 `net.ipv4.ip_forward=1`
- 关闭 `rp_filter`
- 开启 `eth1` 的 `proxy_arp`
- 给 `rf0` 配地址和 MTU
- 加一条到远端 PC 的 `/32` 路由，经 `rf0`

这意味着它不是通用网络程序，而是为当前实验网络拓扑定制的。

## 数据流说明

### 业务数据

1. Linux 内核把发往远端 PC 的流量导向 `rf0`
2. `user_amp` 从 `rf0` 读出 IPv4 包
3. 视包长与类型决定聚合或直发
4. 通过 `/dev/amp_ipi` 写给内核驱动
5. 驱动写共享内存并触发 CPU1 SGI
6. CPU1 处理后，把回包写入 RX 区并通知 CPU0
7. 驱动在 `read()` 路径把包交给用户态
8. `user_amp` 再写回 `rf0`
9. Linux 内核继续从普通网卡发给本地 PC

### 控制数据

1. `pcap` 抓到 `eth1` 上目标端口为 `3409` 的 UDP 报文
2. 用户态提取 UDP payload，按 `control_frame_t` 校验
3. 写入 `/dev/amp_ipi`
4. 驱动解析控制帧并写入测试频率、定频、环回、IQ 交换、衰减等寄存器

## 构建

仓库里当前只提供了用户态程序的简单构建脚本。

### 编译用户态程序

`[Makefile.txt](C:\Users\xushengqiaoya\Desktop\zhukong(new)\Makefile.txt)` 使用的是交叉编译器：

```make
CC = arm-linux-gnueabihf-gcc
LDFLAGS = -lpthread -lpcap
```

在目标环境具备交叉编译工具链和 `libpcap` 后，可执行：

```bash
make -f Makefile.txt
```

生成：

- `user_amp`

### 内核驱动说明

`[driver_amp.c](C:\Users\xushengqiaoya\Desktop\zhukong(new)\driver_amp.c)` 是平台相关驱动源码，依赖：

- Zynq AMP 环境
- 内核中可用的 IPI/SGI 接口
- 对应设备树兼容项：`xlnx,zynq-amp`
- 固定共享内存/寄存器物理地址布局

当前仓库未提供：

- 驱动模块 Makefile/Kbuild
- 设备树节点
- CPU1 固件/程序
- 板级启动与加载脚本

所以驱动部分更适合拿来阅读、移植或并入现有 BSP 工程，而不是在当前仓库里直接单独编译。

## 运行前提

至少需要满足以下条件：

- 目标板运行 Linux，且支持 TUN/TAP
- 系统存在 `/dev/net/tun`
- 内核已加载或集成 `driver_amp.c` 对应驱动，生成 `/dev/amp_ipi`
- 有 `eth1` 接口，且网络拓扑与代码中的假设一致或已按需修改
- 安装 `libpcap`
- 具备 root 权限运行程序，因为会创建 TUN 并修改路由/sysctl

## 运行方式

用户态程序启动逻辑非常直接：

```bash
./user_amp
```

启动后它会：

- 打开 `/dev/amp_ipi`
- 创建 `rf0`
- 自动写入 sysctl / route / proxy ARP 配置
- 启动 3 个线程分别处理数据面与控制面

## 当前仓库的局限

从接手维护的角度，当前代码有几个明显特点：

- 注释存在部分乱码，应该是编码不一致导致
- 网络拓扑和 IP 映射是硬编码的
- 用户态程序的构建脚本较简陋，缺少安装/清理/依赖检查
- 驱动源码没有配套工程文件，移植时需要自行接入内核构建系统

## 建议的后续整理方向

- 把硬编码 IP、接口名、端口号改成配置项
- 给驱动补充 Kbuild、设备树示例和加载说明
- 给用户态程序补充命令行参数
- 统一源码文件编码为 UTF-8，修复乱码注释
- 增加链路时序图和部署拓扑图

## 文件说明

- `[driver_amp.c](C:\Users\xushengqiaoya\Desktop\zhukong(new)\driver_amp.c)`：AMP 驱动源码
- `[user_amp.c](C:\Users\xushengqiaoya\Desktop\zhukong(new)\user_amp.c)`：用户态网关/转发程序
- `[Makefile.txt](C:\Users\xushengqiaoya\Desktop\zhukong(new)\Makefile.txt)`：用户态程序构建脚本
