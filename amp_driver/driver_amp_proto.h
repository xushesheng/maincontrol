/*****************************/
/* 本文件存放控制数据流帧结构体 */
/*****************************/
#ifndef DRIVER_AMP_PROTO_H
#define DRIVER_AMP_PROTO_H

#include <linux/types.h>

#define MAX_PAYLOAD_SIZE 4096        // 4K 数据区大小

/* 控制帧结构（与用户程序一致） */
#pragma pack(push, 1)
struct control_frame {
    uint16_t frame_header;
    uint8_t frame_type;
    uint8_t dst_addr;
    uint32_t frame_seq;
    uint32_t test_freq;
    uint32_t test_enable;
    uint32_t fixed_freq;
    uint32_t net_test;
    uint32_t loopback;
    uint32_t iq_swap;
    uint32_t attenuation;
    uint16_t frame_tail;
};
#pragma pack(pop)

/* 用户态传输的数据结构 */
struct amp_net_msg {
    u32 ip;
    u32 node_id;
    u32 len;
    u8 data_type;
    u8 data[MAX_PAYLOAD_SIZE];
};

#endif
