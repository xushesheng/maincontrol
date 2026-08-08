# zhukong 项目长期记忆

## 目标板构建环境（已通过板子 dmesg/uname 实测确认）
- 开发板内核为 **64 位 aarch64**（buildroot，内核 `5.4.52`，`uname -m = aarch64`）。
- 因此：
  - 内核模块必须按 `ARCH=arm64` + `aarch64-linux-gnu-` 工具链编译（`ARCH=arm` 编出的 32 位 .ko 会 `invalid module format`）。
  - 用户态程序目标应为 aarch64 原生；若编成 32 位 armhf，板子根文件系统缺 `/lib/ld-linux-armhf.so.3` 等 32 位 libc，会报 `-sh: ./xxx: not found`。
- 模块必须用「板子内核同源的那份 `linux-5.4.52-fmsh` 源码 + 对应 `.config` + `Module.symvers`」编译，否则 vermagic 仍可能对不上。
- 注意：板子内核构建时间 `Thu Aug 6 08:09:40 UTC 2026`，编译模块时要对上这份内核树。

## 内核模块 arm64 编译的坑（实测踩过）
- 模块编 arm64 时报 `error: unknown type name 'atomic64_t'`，根因是内核源码树的 `include/generated/autoconf.h` 仍是**32 位 ARM 配置**（无 `CONFIG_64BIT`，而 `atomic64_t` 仅在 `CONFIG_64BIT=y` 时定义）。编译器因 `ARCH=arm64` 展开 arm64 原子头 `atomic_ll_sc.h` 引用 `atomic64_t` 导致未定义。
- 解法：在 `KERNEL_SRC` 用板子同源的 arm64 defconfig 重新生成 `.config`，再 `make ARCH=arm64 ... modules_prepare`（最好完整 `make Image modules` 以生成匹配板子内核的 `Module.symvers`）。**不用改驱动源码**。
- 该 FMQL arm64 内核已自带 `smp_kick_ipi`/`set_ipi_handler`/`clear_ipi_handler`（`arch/arm64/include/asm/smp.h`），跨核 IPI 代码无需为 arm64 改写。
- 用户态 arm64 工具链后缀是 `aarch64-linux-gnu`（无 `hf`）；`aarch64-linux-gnueabihf` 不存在。
- 板子 arm64 内核 defconfig 为 `fmsh_fmqlmp_defconfig`（在 `arch/arm64/configs/`；另一个是 `fmsh_fmqlmp_docker_defconfig`，容器用，板子不用）。准备内核树：`make ARCH=arm64 mrproper` → `fmsh_fmqlmp_defconfig` → `modules_prepare`（最好完整 `make Image modules` 以生成匹配板子的 `Module.symvers`）。
- arm32→arm64 移植常见坑：靠「传递包含」带进来的声明在 arm64 下会暴露为 implicit-function-declaration。已修：`driver_txrx.c` 的 `signal_pending()` 需显式 `#include <linux/sched/signal.h>`；`driver_txrx.c` 里老的 `__cpuc_flush_dcache_area()`（ARM32 专属）已删除（`tx_data_addr` 是 `ioremap_nocache` 不可缓存，`wmb()` 已保证顺序）。这类错误会逐个冒出，按编译器提示补头文件或换 arm64 等价接口即可。

## 已知坑
- `invalid module format` 先 `dmesg | tail` 看内核打印的具体 vermagic/符号原因，再决定改 ARCH 还是换内核树。
- `./xxx: not found` 在 busybox 下通常是 ELF 解释器缺失（动态链接器不存在），不是文件真的丢失。
