/*****************************/
/*   本文件存放数据流帧结构体  */
/*****************************/
#ifndef USER_AMP_PROTO_H
#define USER_AMP_PROTO_H

#include <stddef.h>        /* offsetof / size_t */
#include <stdint.h>        /* uint8_t / uint16_t / uint32_t */
#include <string.h>        /* memcpy */
#include <arpa/inet.h>     /* ntohs / ntohl / htons / htonl */

#include "user_config.h"   /* 宏定义：端口、MTU、批次大小等 */

/* 业务数据传输结构（与驱动侧 struct amp_net_msg 字段一致，保持已有字段语义） */
struct amp_net_msg {
    uint32_t ip;                        /* 目标 IP 地址（网络字节序） */
    uint32_t node_id;                   /* 目标节点号 */
    uint32_t len;                       /* 载荷长度 */
    uint8_t data_type;                  /* 数据类型：1=业务, 2=语音 */
    uint8_t data[MAX_PAYLOAD_SIZE];     /* 载荷缓冲区 */
};

/* 控制数据传输结构（与驱动侧 struct amp_ctrl_msg 字段一致） */
struct amp_ctrl_msg {
    uint32_t len;                       /* 控制帧载荷长度 */
    uint8_t data_type;                  /* 控制数据类型 */
    uint8_t data[MAX_PAYLOAD_SIZE];     /* 控制帧载荷缓冲区 */
};

#pragma pack(push, 1)                   /* 按 1 字节对齐：确保结构体紧凑，无填充字节 */
/* 一批次帧头结构体（AMPB 协议头，8 字节） */
typedef struct {
    uint8_t magic[4];                   /* 魔数："AMPB"，用于识别 AMP 批帧 */
    uint8_t version;                    /* 版本号：当前为 1 */
    uint8_t flags;                      /* 标志位：当前为 0 */
    uint16_t count_be;                  /* 子包数量（网络字节序，大端） */
    uint32_t seq_be;                    /* 批帧序号（网络字节序，大端） */
} amp_batch_hdr_t;
#pragma pack(pop)                       /* 恢复默认对齐 */

/* 一批次组合帧信息结构体（用于聚合小包成批帧） */
typedef struct {
    uint8_t buf[AMP_BATCH_MAX_BYTES];   /* 640 字节缓冲区：存放帧头 + 各个子包的 [2B长度 | 数据] */
    size_t len;                         /* 当前已写入的字节数 */
    uint16_t count;                     /* 当前已写入的子包数量 */
    uint32_t seq;                       /* 批帧序号（递增），用于接收侧去重/排序 */
    uint32_t dst_ip;                    /* 当前批次的目的 IP 地址（同一批次只能发给同一目标） */
} batch_state_t;

/* 从内存中安全地读取一个 16 位大端整数，并转换为主机字节序返回 */
/* 使用 memcpy 而非直接指针强转，避免未对齐访问导致的总线错误 */
static inline uint16_t read_be16_unaligned(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, sizeof(v));           /* 安全拷贝（避免未对齐访问） */
    return ntohs(v);                    /* 大端 -> 主机字节序 */
}

/* 将 16 位主机字节序整数转换为大端格式，并安全地写入内存 */
static inline void write_be16_unaligned(uint8_t *p, uint16_t v)
{
    uint16_t t = htons(v);              /* 主机序 -> 大端 */
    memcpy(p, &t, sizeof(t));           /* 安全写入（避免未对齐访问） */
}

/* 将 32 位主机字节序整数转换为大端格式，并安全地写入内存 */
static inline void write_be32_unaligned(uint8_t *p, uint32_t v)
{
    uint32_t t = htonl(v);              /* 主机序 -> 大端 */
    memcpy(p, &t, sizeof(t));           /* 安全写入 */
}

#endif
