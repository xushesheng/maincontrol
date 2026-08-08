/*****************************/
/* 本文件存放共享消息结构体 */
/*****************************/
#ifndef DRIVER_AMP_PROTO_H
#define DRIVER_AMP_PROTO_H

#include <linux/types.h>

#define MAX_PAYLOAD_SIZE 4096        // 4K 数据区大小

/* 用户态传输的业务数据结构 */
struct amp_net_msg {
    u32 ip;
    u32 node_id;
    u32 len;
    u8 data_type;
    u8 data[MAX_PAYLOAD_SIZE];
};

/* 控制通道传输的数据结构 */
struct amp_ctrl_msg {
    u32 len;
    u8 data_type;
    u8 data[MAX_PAYLOAD_SIZE];
};

#endif
