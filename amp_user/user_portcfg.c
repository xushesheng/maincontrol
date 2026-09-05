/********************************************************/
/*   业务 UDP 端口配置的持久化（配置文件读写）            */
/*   本模块只负责文件 I/O 与取值校验，不涉及 socket、      */
/*   控制帧解析与 ACK 回执（那些在 user_control.c 中）    */
/********************************************************/
#include <stdio.h>                 /* fopen / fclose / fscanf / fprintf / snprintf / rename */
#include <unistd.h>                /* unlink（删除临时文件） */
#include <stdint.h>                /* uint16_t */

#include "user_config.h"           /* 端口范围、配置文件路径等宏定义 */
#include "user_declaration.h"      /* g_business_port 声明 */

/* 当前生效的业务 UDP 端口。
 * 初值为编译期默认 BUSINESS_PORT，启动时由 portcfg_load() 用配置文件覆盖，
 * 运行中由 portcfg_apply_and_save() 在网管下发配置时改写。 */
volatile uint16_t g_business_port = BUSINESS_PORT;

/************************************************
 * 判断一个端口号是否可以作为业务端口使用        *
 * 返回 1 表示合法，0 表示非法                   *
 ***********************************************/
static int port_is_valid(uint16_t port)
{
    /* 1) 必须在约定的端口区间内：下限避开特权端口与常见服务端口 */
    if (port < BUSINESS_PORT_MIN || port > BUSINESS_PORT_MAX)
        return 0;

    /* 2) 不可与本机控制通道端口冲突：
     *    3409 是网管指令入口，3419 是控制回执出口，
     *    若业务端口取这两个值，控制报文会被业务面过滤逻辑误判 */
    if (port == CONTROL_PORT || port == CONTROL_REPORT_PORT)
        return 0;

    return 1;
}

/************************************************
 * 启动时从配置文件载入业务端口                  *
 * 文件不存在或内容非法时保持默认值并返回 -1，    *
 * 不视为致命错误，程序照常启动                  *
 ***********************************************/
int portcfg_load(void)
{
    FILE *fp;                   /* 配置文件句柄 */
    unsigned int value;         /* 从文件读到的端口数值 */

    fp = fopen(AMP_PORT_CONF_FILE, "r");
    if (!fp) {
        /* 首次启动或文件被删除：直接用默认值 3408 */
        fprintf(stderr, "[WARN] port config %s not readable, use default %d\n",
                AMP_PORT_CONF_FILE, BUSINESS_PORT);
        return -1;
    }

    if (fscanf(fp, "%u", &value) != 1) {
        fclose(fp);
        fprintf(stderr, "[WARN] port config %s has no valid number, use default %d\n",
                AMP_PORT_CONF_FILE, BUSINESS_PORT);
        return -1;
    }
    fclose(fp);

    if (!port_is_valid((uint16_t)value)) {
        fprintf(stderr, "[WARN] port config %s value %u out of range, use default %d\n",
                AMP_PORT_CONF_FILE, value, BUSINESS_PORT);
        return -1;
    }

    g_business_port = (uint16_t)value;      /* 载入成功，覆盖默认值 */
    fprintf(stderr, "[INFO] business port loaded from config: %u\n",
            (unsigned)g_business_port);
    return 0;
}

/************************************************
 * 校验端口、写入配置文件并切换到新端口          *
 * 成功返回 0，失败返回 -1（当前生效值不变）     *
 ***********************************************/
int portcfg_apply_and_save(uint16_t port)
{
    FILE *fp;                                       /* 临时文件句柄 */
    char tmp_path[sizeof(AMP_PORT_CONF_FILE) + 8];   /* 配置文件路径 + ".tmp" 后缀 */

    if (!port_is_valid(port)) {
        fprintf(stderr, "[WARN] reject invalid business port: %u\n", (unsigned)port);
        return -1;
    }

    /* 先写临时文件再 rename：rename 在同目录下是原子操作，
     * 避免写一半断电导致配置文件损坏、板卡起不来业务面 */
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", AMP_PORT_CONF_FILE);

    fp = fopen(tmp_path, "w");
    if (!fp) {
        fprintf(stderr, "[ERROR] cannot write port config temp file %s\n", tmp_path);
        return -1;
    }

    if (fprintf(fp, "%u\n", (unsigned)port) < 0) {
        fclose(fp);
        unlink(tmp_path);       /* 清理半成品临时文件 */
        fprintf(stderr, "[ERROR] write port config temp file failed\n");
        return -1;
    }

    /* fclose 失败意味着数据可能仍在缓冲区未落盘，同样按失败处理 */
    if (fclose(fp) != 0) {
        unlink(tmp_path);
        fprintf(stderr, "[ERROR] flush port config temp file failed\n");
        return -1;
    }

    if (rename(tmp_path, AMP_PORT_CONF_FILE) != 0) {
        unlink(tmp_path);
        fprintf(stderr, "[ERROR] rename port config to %s failed\n", AMP_PORT_CONF_FILE);
        return -1;
    }

    /* 只有持久化成功之后才切换生效值，保证「配置里的值」与「内存里的值」一致 */
    g_business_port = port;
    fprintf(stderr, "[INFO] business port applied and saved: %u\n", (unsigned)port);
    return 0;
}
