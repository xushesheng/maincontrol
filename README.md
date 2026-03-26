# zhukong(new)

这是一个 Zynq AMP 场景下的跨核通信项目，现已按驱动侧与用户态侧拆分为目录结构，便于维护和局部修改。

## 项目结构

### 根目录

- `AGENTS.md`：代理协作规则
- `README.md`：项目说明
- `amp_driver/`：驱动侧源码
- `amp_user/`：用户态源码与构建文件

### 驱动侧 `amp_driver/`

- `driver_amp_main.c`：平台驱动入口、probe/remove、模块注册
- `driver_amp_dev.c`：`/dev/amp_ipi` 读写接口
- `driver_amp_txrx.c`：业务数据/控制数据发送处理、RX 中断处理、IP 与节点映射
- `driver_amp_hw.c`：共享内存和寄存器映射、资源初始化与释放
- `driver_amp_proto.h`：驱动侧公共协议结构
- `driver_amp_hw.h`：硬件地址、共享状态、跨文件声明

### 用户态 `amp_user/`

- `Makefile`：用户态程序构建文件
- `user_amp_main.c`：主程序入口与线程创建
- `user_amp_tun.c`：TUN 设备创建与写包
- `user_amp_gateway.c`：`eth1`/`rf0` 路由、proxy ARP、sysctl 配置
- `user_amp_batch.c`：小包聚合与批帧刷出
- `user_amp_datapath.c`：业务数据收发线程
- `user_amp_control.c`：控制 UDP 抓取与控制帧发送
- `user_amp_proto.h`：用户态公共协议结构与批帧定义
- `user_amp_config.h`：用户态配置宏
- `user_amp_runtime.h`：跨文件运行时状态与函数声明

## 功能概览

驱动侧负责：

- 映射共享内存和寄存器
- 暴露 `/dev/amp_ipi`
- 接收用户态写入的数据或控制帧
- 触发 CPU0 到 CPU1 的 SGI
- 接收 CPU1 返回数据并提供给用户态 `read()`

用户态负责：

- 创建 `rf0` TUN 设备
- 自动配置路由与 proxy ARP
- 从 `rf0` 读取需要跨链路发送的 IPv4 包
- 对小包做 `AMPB` 聚合，对大包直发
- 将对端回包重新写回 `rf0`
- 抓取 `eth1` 上的控制 UDP(`3409`)并转发给驱动

## 构建

当前仓库提供用户态程序构建，`Makefile` 位于 `amp_user/` 目录下。

进入用户态目录后执行：

```bash
cd amp_user
make
```

生成目标：

- `user_amp`

驱动侧源码已拆分，但仓库仍未包含完整 Kbuild、设备树节点和 CPU1 配套程序，因此驱动部分仍需要接入目标 BSP/内核工程中构建。
