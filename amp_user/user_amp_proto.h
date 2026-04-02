/*****************************/
/* 本文件存放控制数据流帧结构体 */
/*****************************/
#ifndef USER_AMP_PROTO_H
#define USER_AMP_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>

#include "user_amp_config.h"

/* 与驱动一致（保持已有字段语义） */
struct amp_net_msg {
    uint32_t ip;
    uint32_t node_id;
    uint32_t len;
    uint8_t data_type;
    uint8_t data[MAX_PAYLOAD_SIZE];
};

/* 控制帧结构（保持原协议） */
#pragma pack(push, 1)
typedef struct {
    uint16_t frame_header;
    uint8_t  frame_type;
    uint8_t  dst_addr;
    uint32_t frame_seq;
    uint32_t test_freq;
    uint32_t test_enable;
    uint32_t fixed_freq;
    uint32_t net_test;
    uint32_t loopback;
    uint32_t iq_swap;
    uint32_t attenuation;
    uint16_t frame_tail;
} control_frame_t;
#pragma pack(pop)

#pragma pack(push, 1)
/*一批次帧头结构体*/
typedef struct {
    uint8_t magic[4];       //AMPB
    uint8_t version;    	//1
    uint8_t flags;      	//0
    uint16_t count_be;		//子包数量
    uint32_t seq_be;	    //序号
} amp_batch_hdr_t;
#pragma pack(pop)

/*一批次组合帧信息结构体*/
typedef struct {
    uint8_t buf[AMP_BATCH_MAX_BYTES];	//640字节缓冲区
    size_t len;	                        //当前已写入长度
    uint16_t count;	                    //当前已写入子包数量
    uint32_t seq;	                    //批帧序号
    uint32_t dst_ip;	                //当前批次的目的IP地址
} batch_state_t;

/*从内存中安全地读取一个16位大端整数，并转换为主机字节序返回*/
static inline uint16_t read_be16_unaligned(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return ntohs(v);
}

/*将16位主机字节序整数转换为大端格式，并安全地写入内存*/
static inline void write_be16_unaligned(uint8_t *p, uint16_t v)
{
    uint16_t t = htons(v);
    memcpy(p, &t, sizeof(t));
}

/*将32位主机字节序整数转换为大端格式，并安全地写入内存*/
static inline void write_be32_unaligned(uint8_t *p, uint32_t v)
{
    uint32_t t = htonl(v);
    memcpy(p, &t, sizeof(t));
}

#endif
