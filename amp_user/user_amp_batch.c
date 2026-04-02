/*************************************/
/*     一批次组帧初始化与发送函数      */
/*************************************/
#include <string.h>

#include "user_amp_runtime.h"

/************
*初始化聚合帧
************/
void batch_reset(batch_state_t *b)	            //传入一个批次帧结构体
{
    uint32_t seq = b->seq;	                    //定义赋值批帧序号
    amp_batch_hdr_t hdr;	                    //定义帧头结构体

    memset(b, 0, sizeof(*b));	                //初始化帧结构体
    b->seq = seq;

    memset(&hdr, 0, sizeof(hdr));	            //初始化帧头结构体
    memcpy(hdr.magic, AMP_BATCH_MAGIC, 4);      //初始化批次限定AMPB
    hdr.version = AMP_BATCH_VERSION;            //初始化版本
    hdr.flags = 0;	                            //初始化标识为0
    hdr.count_be = htons(0);	                //初始化子包数量为0
    hdr.seq_be = htonl(b->seq);	                //初始化头序号为帧序号

    memcpy(b->buf, &hdr, sizeof(hdr));	        //把帧头放入帧结构体首部buf中
    b->len = sizeof(hdr);	                    //帧长度为帧头长度
    b->count = 0;	                            //初始化写入子包数
    b->dst_ip = 0;	                            //初始化当前批次目的地址
}

/***********************
*把新数据帧pkt添加到当前批次聚合帧
***********************/
int batch_append(batch_state_t *b, const uint8_t *pkt, size_t pkt_len, uint32_t dst_ip)
{
    if (pkt_len > 0xFFFF)   //如果长度超长返回-1
        return -1;
    if (b->len + 2 + pkt_len > AMP_BATCH_MAX_BYTES)
        return -2;  //如果核算长度超出640B返回-2

    if (b->count == 0)  //如果经过以上筛选，且核算长度不为零
        b->dst_ip = dst_ip; //将聚合帧结构体的目的地址赋值为传入的目的地址

    write_be16_unaligned(b->buf + b->len, (uint16_t)pkt_len);   //将传入的长度写入buf的末尾位置
    b->len += 2;    //聚合帧结构体的长度+2
    memcpy(b->buf + b->len, pkt, pkt_len);  //将传入的数据写到聚合帧buf的末尾
    b->len += pkt_len;  //聚合帧长度加上pkt的长度

    b->count++; //聚合帧子包数量+1
    write_be16_unaligned(b->buf + offsetof(amp_batch_hdr_t, count_be), b->count);   //buf的地址加上coungt_be的偏移量，改写帧头的子包数量
    write_be32_unaligned(b->buf + offsetof(amp_batch_hdr_t, seq_be), b->seq);       //改写帧头的序号
    return 0;
}

/**********************
 *发送当前批次的所有数据
 ********************/
int amp_flush_batch_if_any(batch_state_t *b)
{
    if (b->count == 0)
        return 0;

    /* 如果批次里只有 1 个子包，为减少头开销，直接发原始 IP 包 */
    if (b->count == 1) {
        size_t off = sizeof(amp_batch_hdr_t);   //帧头大小赋给off
        uint16_t l;

        if (b->len < off + 2) {                 //如果帧长度小于帧头结构体大小+2字节子包长度字段，表示为空包或异常
            batch_reset(b);                     //初始化帧
            return -1;
        }

        l = read_be16_unaligned(b->buf + off);  //读取帧头末尾的2字节唯一子包长度字段赋给l
        if (off + 2 + l > b->len) {             //如果帧长度<帧头结构体大小+子包长度，即长度不对应
            batch_reset(b);                     //直接放弃当前批次,初始化一个新批次
            return -1;
        }

        (void)amp_send_msg(b->dst_ip, b->buf + off + 2, l); //跳过帧头和长度字节，直接把子包发出去
        b->seq++;   //批次序号+1
        batch_reset(b);
        return 0;
    }

    (void)amp_send_msg(b->dst_ip, b->buf, b->len);  //把头和所有子包（即整个buf部分）一起原样发出
    b->seq++;
    batch_reset(b);
    return 0;
}
