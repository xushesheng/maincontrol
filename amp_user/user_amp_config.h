/*************************************/
/*       本文件存放定义的不可变量      */
/************************************/
#ifndef USER_AMP_CONFIG_H
#define USER_AMP_CONFIG_H

#define AMP_DEV "/dev/amp_ipi"
#define CAP_IFACE "eth1"
#define CTRL_UDP_PORT 3409

#define MAX_PAYLOAD_SIZE 4096

/* 聚合帧最大长度（只对“批帧 AMPB”限制 640，单包直发不受此限制） */
#define AMP_BATCH_MAX_BYTES 640
#define AMP_BATCH_MAGIC "AMPB"
#define AMP_BATCH_VERSION 1

/* 聚合窗口：第一个包进入批次后，最多再等这么多 ms 看能不能凑更多包。
 * 调大：吞吐更好但交互/ ping RTT 更大；调小：时延更好但 SGI 次数更多。 */
#define AMP_BATCH_TIMEOUT_MS 96

/* ICMP/ping 快速通道开关 */
#define AMP_ICMP_FASTPATH 1

/* rf0 MTU：为了允许 >640 的 IP 包“单包直发” */
#define RF0_MTU 1600

#endif
