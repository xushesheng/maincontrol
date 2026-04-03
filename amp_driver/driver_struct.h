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
    uint16_t frame_header;  //帧头
    uint8_t  frame_type;    //帧类型
    uint8_t  dst_addr;      //目的地址
    uint32_t frame_seq;     //帧序列号
    uint32_t test_freq;     //测试信号频率
    uint32_t test_enable;   //测试信号使能
    uint32_t fixed_freq;    //定频频率
    uint32_t net_test;      //组网数据发送测试
    uint32_t loopback;      //数据自回环
    uint32_t iq_swap;       //接收基带IQ对调
    uint32_t attenuation;   //发射衰减系数
    uint16_t frame_tail;    //帧尾
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
