/*****************************/
/* 本文件存放共享消息结构体 */
/*****************************/
#ifndef DRIVER_AMP_PROTO_H
#define DRIVER_AMP_PROTO_H

#include <linux/types.h>                        /* 内核基础类型：u32/u8 等 */

#define MAX_PAYLOAD_SIZE 4096                   /* 4K 数据区大小，即共享内存中每个业务/控制数据块的最大容量 */

/* 用户态传输的业务数据结构（驱动侧与用户态侧保持一致） */
struct amp_net_msg {
    u32 ip;                                     /* 目标 IP 地址（网络字节序） */
    u32 node_id;                                /* 目标节点号（0~31 单播 / 254 组播 / 255 广播） */
    u32 len;                                    /* 载荷长度（data[] 中的有效字节数） */
    u8 data_type;                               /* 数据类型：1=业务, 2=语音 */
    u8 data[MAX_PAYLOAD_SIZE];                  /* 载荷缓冲区，最大 4096 字节 */
};

/* 控制通道传输的数据结构（驱动侧与用户态侧保持一致） */
struct amp_ctrl_msg {
    u32 len;                                    /* 控制帧载荷长度 */
    u8 data_type;                               /* 控制数据类型：1=控制帧 */
    u8 data[MAX_PAYLOAD_SIZE];                  /* 控制帧载荷缓冲区 */
};

#endif
