/********************************************************/
/*   业务 UDP 端口区间的持久化（配置文件读写）            */
/*   本模块只负责文件 I/O 与取值校验，不涉及 socket、      */
/*   控制帧解析与 ACK 回执（那些在 user_control.c 中）    */
/********************************************************/
#include <stdio.h>                 /* fopen / fclose / fscanf / fprintf / snprintf / rename */
#include <unistd.h>                /* unlink（删除临时文件） */
#include <stdint.h>                /* uint16_t / uint32_t */

#include "user_config.h"           /* 端口范围、配置文件路径等宏定义 */
#include "user_declaration.h"      /* g_business_range 声明 */

/* 当前生效的业务 UDP 端口区间，打包为 (下限 << 16) | 上限。
 * 初值为编译期默认区间 [BUSINESS_PORT_DEFAULT_MIN, BUSINESS_PORT_DEFAULT_MAX]，
 * 启动时由 portcfg_load() 用配置文件覆盖，运行中由 portcfg_apply_and_save()
 * 在网管下发 0x22 配置帧（协议表 5.26）时改写。 */
volatile uint32_t g_business_range =
    ((uint32_t)BUSINESS_PORT_DEFAULT_MIN << 16) | BUSINESS_PORT_DEFAULT_MAX;

/************************************************
 * 判断一个端口区间是否可以作为业务端口区间使用  *
 * 返回 1 表示合法，0 表示非法                   *
 * 注：参数为 uint16_t，天然 <= 65535，故上限    *
 * （BUSINESS_PORT_RANGE_MAX）无需比较；下限与   *
 * 顺序约束在此把关。                            *
 ***********************************************/
static int range_is_valid(uint16_t port_min, uint16_t port_max)
{
    /* 1) 区间两端都必须不小于约定下限：避开特权端口与常见服务端口。
     *    上限 BUSINESS_PORT_RANGE_MAX=65535 与 uint16_t 表示范围一致，
     *    对 uint16_t 参数的上限比较恒假（-Wtype-limits），无需书写。 */
    if (port_min < BUSINESS_PORT_RANGE_MIN)
        return 0;
    if (port_max < BUSINESS_PORT_RANGE_MIN)
        return 0;

    /* 2) 下限不得大于上限（允许相等，即退化为单端口的区间） */
    if (port_min > port_max)
        return 0;

    /* 注意：这里不再拦截区间覆盖 3409/3419 的情况——准入侧
     * （user_datapath.c 的 classify_udp_business_port）对控制端口
     * 3409 与控制回执端口 3419 做了"挖洞"式优先排除，无论业务区间
     * 怎么配置，控制面流量都不会被吸进业务面。 */
    return 1;
}

/************************************************
 * 启动时从配置文件载入业务端口区间                *
 * 文件不存在或内容非法时保持默认区间并返回 -1，    *
 * 不视为致命错误，程序照常启动                  *
 * 文件格式：一行两个十进制数"下限 上限"（如      *
 * "3380 3480"）；兼容旧版单值文件（只有一个数   *
 * 时退化为"下限=上限=该值"的区间，不丢配置）     *
 ***********************************************/
int portcfg_load(void)
{
    FILE *fp;                   /* 配置文件句柄 */
    unsigned int value_min;     /* 从文件读到的区间下限 */
    unsigned int value_max;     /* 从文件读到的区间上限 */
    int fields;                 /* fscanf 实际读到的字段数 */

    fp = fopen(AMP_PORT_CONF_FILE, "r");
    if (!fp) {
        /* 首次启动或文件被删除：直接用默认区间 */
        fprintf(stderr, "[WARN] port config %s not readable, use default [%d, %d]\n",
                AMP_PORT_CONF_FILE, BUSINESS_PORT_DEFAULT_MIN, BUSINESS_PORT_DEFAULT_MAX);
        return -1;
    }

    fields = fscanf(fp, "%u %u", &value_min, &value_max);
    fclose(fp);

    if (fields < 1) {
        /* 文件里连一个数都没有 */
        fprintf(stderr, "[WARN] port config %s has no valid number, use default [%d, %d]\n",
                AMP_PORT_CONF_FILE, BUSINESS_PORT_DEFAULT_MIN, BUSINESS_PORT_DEFAULT_MAX);
        return -1;
    }

    if (fields == 1) {
        /* 旧版单值配置文件：退化为下限=上限的单点区间 */
        value_max = value_min;
        fprintf(stderr, "[INFO] port config %s carries a single value, degenerate to range [%u, %u]\n",
                AMP_PORT_CONF_FILE, value_min, value_max);
    }

    /* 在截断到 uint16_t 之前先拦掉超出 16 位端口范围的数值，
     * 避免 70000 之类被静默截断成 4464 而误判为合法 */
    if (value_min > 65535u || value_max > 65535u) {
        fprintf(stderr, "[WARN] port config %s value [%u, %u] exceeds 16-bit port range, use default [%d, %d]\n",
                AMP_PORT_CONF_FILE, value_min, value_max,
                BUSINESS_PORT_DEFAULT_MIN, BUSINESS_PORT_DEFAULT_MAX);
        return -1;
    }

    if (!range_is_valid((uint16_t)value_min, (uint16_t)value_max)) {
        fprintf(stderr, "[WARN] port config %s value [%u, %u] invalid, use default [%d, %d]\n",
                AMP_PORT_CONF_FILE, value_min, value_max,
                BUSINESS_PORT_DEFAULT_MIN, BUSINESS_PORT_DEFAULT_MAX);
        return -1;
    }

    g_business_range = ((uint32_t)value_min << 16) | (uint32_t)value_max;      /* 载入成功，覆盖默认值 */
    fprintf(stderr, "[INFO] business port range loaded from config: [%u, %u]\n",
            value_min, value_max);
    return 0;
}

/************************************************
 * 校验区间、写入配置文件并切换到新区间          *
 * 成功返回 0，失败返回 -1（当前生效值不变）     *
 ***********************************************/
int portcfg_apply_and_save(uint16_t port_min, uint16_t port_max)
{
    FILE *fp;                                       /* 临时文件句柄 */
    char tmp_path[sizeof(AMP_PORT_CONF_FILE) + 8];   /* 配置文件路径 + ".tmp" 后缀 */

    if (!range_is_valid(port_min, port_max)) {
        fprintf(stderr, "[WARN] reject invalid business port range: [%u, %u]\n",
                (unsigned)port_min, (unsigned)port_max);
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

    if (fprintf(fp, "%u %u\n", (unsigned)port_min, (unsigned)port_max) < 0) {
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

    /* 只有持久化成功之后才切换生效值，保证「配置里的值」与「内存里的值」一致。
     * 32 位打包值一次写入，读取方不会看到"新下限+旧上限"的撕裂区间。 */
    g_business_range = ((uint32_t)port_min << 16) | (uint32_t)port_max;
    fprintf(stderr, "[INFO] business port range applied and saved: [%u, %u]\n",
            (unsigned)port_min, (unsigned)port_max);
    return 0;
}
