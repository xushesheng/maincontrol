#ifndef USER_AMP_CONFIG_H
#define USER_AMP_CONFIG_H

#define AMP_DEV "/dev/amp_ipi"
#define CAP_IFACE "eth1"
#define CTRL_UDP_PORT 3409

#define MAX_PAYLOAD_SIZE 4096

#define AMP_BATCH_MAX_BYTES 640
#define AMP_BATCH_MAGIC "AMPB"
#define AMP_BATCH_VERSION 1
#define AMP_BATCH_TIMEOUT_MS 96
#define AMP_ICMP_FASTPATH 1

#define RF0_MTU 1600

#endif
