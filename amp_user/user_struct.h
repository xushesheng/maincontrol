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

#endif
