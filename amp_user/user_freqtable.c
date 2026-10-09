/********************************************************/
/*      开机频表下发模块（类型 0x09）                     */
/*   本模块只负责：读取网管 jar 维护的两个 JSON 频表文件、
    构造非自适应跳频频表设置帧并写入 /dev/amp_ctrl；不涉及
    socket收发与 ACK 回执（那些在 user_control.c）      */
/********************************************************/
#include <stdio.h>                 /* fopen / fread / fseek / ftell / fprintf */
#include <stdlib.h>                /* malloc / free */
#include <string.h>                /* memset / memcpy */
#include <stdint.h>                /* uint8_t / uint16_t */
#include <stddef.h>                /* size_t / offsetof */
#include <unistd.h>                /* sleep / write */
#include <errno.h>                 /* errno / EINTR */

#include "cjson/cJSON.h"           /* 第三方 JSON 解析库（vendored，MIT，v1.7.19，见 cjson/README.md） */

#include "user_declaration.h"      /* ctrl_fd / struct amp_ctrl_msg / freqtable_send_on_boot 声明 */

/* ========= 本地编码辅助 ========= */
/* 以下三个函数与 user_control.c 内的 ctrl_write_le16 / dec_to_bcd /
 * ctrl_xor_checksum 逻辑一致；因其均为该文件内的 static、无法跨文件
 * 复用（且按约定不改 user_control.c），故在此本地复刻等价实现，
 * 统一加 ft_ 前缀以便溯源。 */

/* 把 16 位字段以小端写入缓冲区（协议头字段按小端发送，与路由固件对齐） */
static void ft_write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

/* 十进制数值编成 bytes 字节 BCD（多字节高位字节在前）：
 * 例如 321/2B -> 03 21；470/2B -> 04 70；1/1B -> 01。
 * 数值为负或超出 bytes 字节 BCD 容量（bytes×2 位十进制）返回 -1。 */
static int ft_encode_bcd(uint8_t *out, size_t bytes, long v)
{
    size_t i;

    if (v < 0)
        return -1;
    for (i = bytes; i > 0; i--) {
        long two = v % 100;        /* 每字节承载两位十进制数 */

        out[i - 1] = (uint8_t)(((two / 10) << 4) | (two % 10));
        v /= 100;
    }
    return (v != 0) ? -1 : 0;      /* 仍有剩余高位 → 超出容量 */
}

/* 校验和：头区域(类型+保留)与数据域逐字节异或（与 ctrl_xor_checksum 同逻辑） */
static uint8_t ft_xor_checksum(const uint8_t *pkt, size_t len)
{
    size_t i;
    uint8_t checksum;

    checksum = pkt[10];            /* 先取类型字节 */
    for (i = 11; i < len - 1; i++) /* 一直异或到倒数第二个字节 */
        checksum ^= pkt[i];
    return checksum;
}

/* ========= 文件读取 ========= */

/* 整读一个小文件（上限 FREQTABLE_JSON_MAX_BYTES，防异常巨文件耗尽内存）。
 * 成功返回缓冲区（额外多分配 1 字节并置 NUL，cJSON 解析需要 NUL 结尾兜底）；
 * 失败返回 NULL。 */
static char *ft_read_file(const char *path, size_t *out_len)
{
    FILE *fp;
    long fsize;
    char *buf;

    fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0)
        goto fail;
    fsize = ftell(fp);
    if (fsize <= 0 || (size_t)fsize > FREQTABLE_JSON_MAX_BYTES)
        goto fail;
    rewind(fp);

    buf = malloc((size_t)fsize + 1);
    if (!buf)
        goto fail;
    if (fread(buf, 1, (size_t)fsize, fp) != (size_t)fsize) {
        free(buf);
        goto fail;
    }
    fclose(fp);
    buf[fsize] = 0;
    *out_len = (size_t)fsize;
    return buf;

fail:
    fclose(fp);
    return NULL;
}

/* ========= cJSON 取值辅助 ========= */

/* 从 cJSON Number 取 [0, max] 内的精确非负整数。
 * cJSON 把数字存成 double，valueint 有 int 钳位，直接用会把 470.5
 * 静默截成 470——这里必须用 valuedouble 做整数性 + 范围双重校验。 */
static int ft_get_uint(const cJSON *item, long max, long *out)
{
    double d;

    if (!cJSON_IsNumber(item))
        return 0;
    d = item->valuedouble;
    if (d < 0.0 || d > (double)max)
        return 0;
    if ((double)(long long)d != d)
        return 0;                  /* 非整数（如 470.5）→ 拒绝 */
    *out = (long)d;
    return 1;
}

/* ========= lastSentFreqTable.json 解析 ========= */

/* 解析 {"lastSentTableId": N}；容忍额外成员（cJSON 原生支持）。
 * 成功返回 1 并写出 N；失败返回 0（结构非法/缺键/值非非负整数）。 */
static int ft_parse_last_sent(const char *buf, size_t len, long *table_id)
{
    cJSON *root;
    const cJSON *item;
    long v;
    int ok = 0;

    /* 防 UTF-8 BOM：cJSON 不接受 BOM 前缀（Jackson 不写，但手工编辑可能带入） */
    if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        buf += 3;
        len -= 3;
    }

    root = cJSON_ParseWithLengthOpts(buf, len, NULL, 0);
    if (!root)
        return 0;

    item = cJSON_GetObjectItemCaseSensitive(root, "lastSentTableId");
    if (ft_get_uint(item, (long)FREQTABLE_TABLE_ID_MAX, &v)) {
        *table_id = v;
        ok = 1;
    }

    cJSON_Delete(root);
    return ok;
}

/* ========= freqtable.json 定位与取点 ========= */

/* 在频表库中定位 tableId 等于 target_id 的元素并提取 points 编码进帧。
 * 同 tableId 重复时取首个命中（语义与原实现一致）。
 * 帧布局：12B 帧头 + 1B 表ID + 2B 总数，频点数据自 frame[15] 起每点 4B
 * （编号 2B BCD + 频率 2B BCD）——帧头与载荷头由本函数最后回填。
 * 成功返回 0：*frame_len 为整帧长度、*out_count 为实际频点数；
 * 失败返回 -1（各类失败原因均已打印 WARN，整表拒发防半张表下发）。 */
static int ft_build_frame(const char *buf, size_t len, long target_id,
                          uint8_t *frame, size_t *frame_len, long *out_count)
{
    cJSON *root;
    const cJSON *elem;
    const cJSON *points;
    const cJSON *pt;
    long declared_count = -1;
    long dcl;
    size_t total;
    int count = 0;
    int found = 0;

    /* 防 UTF-8 BOM：cJSON 不接受 BOM 前缀（Jackson 不写，但手工编辑可能带入） */
    if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        buf += 3;
        len -= 3;
    }

    root = cJSON_ParseWithLengthOpts(buf, len, NULL, 0);
    if (!root || !cJSON_IsArray(root)) {
        fprintf(stderr, "[WARN] freqtable: parse %s failed\n", FREQTABLE_STORE_FILE);
        cJSON_Delete(root);        /* NULL 入参是安全空操作 */
        return -1;
    }

    /* 第一遍：找 tableId 命中的元素（首个命中即用） */
    cJSON_ArrayForEach(elem, root) {
        long v;

        if (ft_get_uint(cJSON_GetObjectItemCaseSensitive(elem, "tableId"),
                        (long)FREQTABLE_TABLE_ID_MAX, &v) && v == target_id) {
            found = 1;
            break;
        }
    }

    if (!found) {
        cJSON_Delete(root);
        fprintf(stderr, "[WARN] freqtable: tableId %ld not found in %s\n",
                target_id, FREQTABLE_STORE_FILE);
        return -1;
    }

    /* 声明的 pointCount 仅作交叉校验，帧内一律写实际数 */
    if (ft_get_uint(cJSON_GetObjectItemCaseSensitive(elem, "pointCount"),
                    1000000L, &dcl))
        declared_count = dcl;

    points = cJSON_GetObjectItemCaseSensitive(elem, "points");
    if (!cJSON_IsArray(points)) {
        cJSON_Delete(root);
        fprintf(stderr, "[WARN] freqtable: table %ld has no points\n", target_id);
        return -1;
    }

    memset(frame, 0, FREQTABLE_FRAME_MAX);

    /* 第二遍：逐点校验并直接编码进帧 */
    cJSON_ArrayForEach(pt, points) {
        const cJSON *pid_item = cJSON_GetObjectItemCaseSensitive(pt, "pointId");
        const cJSON *freq_item = cJSON_GetObjectItemCaseSensitive(pt, "frequency");
        long pid = -1;
        long freq = -1;
        int pid_ok = ft_get_uint(pid_item, (long)FREQTABLE_POINT_ID_MAX, &pid);
        int freq_ok = ft_get_uint(freq_item, (long)FREQTABLE_FREQ_MAX, &freq);

        /* 逐点校验：缺字段或超限一律整表拒发
         * （不静默丢点——半张表会让路由跳错频） */
        if (!pid_ok || !freq_ok || freq < FREQTABLE_FREQ_MIN) {
            fprintf(stderr, "[WARN] freqtable: table %ld has invalid point %d (id=%ld freq=%ld)\n",
                    target_id, count + 1, pid_ok ? pid : -1L, freq_ok ? freq : -1L);
            cJSON_Delete(root);
            return -1;
        }
        if (count >= FREQTABLE_POINT_MAX) {
            fprintf(stderr, "[WARN] freqtable: table %ld points exceed limit %d\n",
                    target_id, FREQTABLE_POINT_MAX);
            cJSON_Delete(root);
            return -1;
        }

        /* 编码：频点编号(2B BCD) + 频点频率(2B BCD)。
         * BCD 容量必够（编号≤511、频率≤790，2B BCD 容量 9999），
         * 故返回值按理论不可达处理。 */
        (void)ft_encode_bcd(frame + 15 + 4 * count, 2, pid);
        (void)ft_encode_bcd(frame + 17 + 4 * count, 2, freq);
        count++;
    }

    if (count == 0) {
        cJSON_Delete(root);
        fprintf(stderr, "[WARN] freqtable: table %ld has no points\n", target_id);
        return -1;
    }

    /* 回填帧头（小端 16 位字段，与 0x19 时钟帧同构：目的=路由、源=主控） */
    total = 16 + 4 * (size_t)count;             /* 12 头 + 3 载荷头 + 4×count + 1 校验和 */
    ft_write_le16(frame + 0, (uint16_t)total);  /* 总帧长（含长度字段自身） */
    ft_write_le16(frame + 2, 0x0000);           /* 保留 */
    ft_write_le16(frame + 4, SRIO_ID_ROUTER);   /* 目的 ID = 路由 */
    ft_write_le16(frame + 6, SRIO_ID_MASTER);   /* 源 ID = 主控 */
    ft_write_le16(frame + 8, CLOCK_SYNC_MAGIC); /* 同步序列 0xFFF5 */
    frame[10] = CTRL_FRAME_TYPE_FREQTABLE;      /* 类型 0x09 */
    frame[11] = 0x00;                           /* 头区域第二字节：保留 */

    /* 载荷头：频表 ID(1B BCD) + 频点总数(2B BCD)。
     * tableId 已限 0~99（1B BCD 容量 99）、count 已限 321（2B BCD 容量 9999），
     * 编码失败按理论不可达处理。帧内总数一律写实际数，保证帧自洽。 */
    (void)ft_encode_bcd(frame + 12, 1, target_id);
    (void)ft_encode_bcd(frame + 13, 2, (long)count);

    frame[total - 1] = ft_xor_checksum(frame, total);

    /* 声明 pointCount 与实际不符：仅告警（网管保存时强制一致，出现即数据异常） */
    if (declared_count >= 0 && declared_count != (long)count)
        fprintf(stderr, "[WARN] freqtable: table %ld declared pointCount %ld != actual %d, use actual\n",
                target_id, declared_count, count);

    *frame_len = total;
    *out_count = count;
    cJSON_Delete(root);
    return 0;
}

/* ========= 写设备 ========= */

/* 与 user_control.c 的 write_ctrl_msg 同逻辑（该函数为 static 无法跨文件
 * 复用，且按约定不改 user_control.c，故本地复刻）：
 * 全量写成功返回 0；EINTR 重试；其余错误返回 -1。 */
static int ft_write_ctrl(const struct amp_ctrl_msg *msg, size_t msg_bytes)
{
    while (1) {
        ssize_t written = write(ctrl_fd, msg, msg_bytes);

        if (written == (ssize_t)msg_bytes)
            return 0;
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0) {
            perror("write(ctrl_fd)");
            return -1;
        }
        fprintf(stderr, "[ERROR] short write(ctrl_fd): %zd/%zu\n", written, msg_bytes);
        return -1;
    }
}

/***********************************
 *  开机一次性下发 0x09 频表帧给路由   *
 *  读网管 JSON -> 组帧 -> 写设备；   *
 *  带初始延时 + 有限重试；失败不致命  *
 *  （任何文件/数据问题只跳过不发送，  *
 *   程序照常启动，与 0x19 同样待遇）  *
 **********************************/
int freqtable_send_on_boot(void)
{
    char *ls_buf;                  /* lastSentFreqTable.json 内容 */
    char *st_buf;                  /* freqtable.json 内容 */
    size_t ls_len = 0, st_len = 0;
    long table_id;                 /* 最近下发的频表号 */
    long points;                   /* 实际编入帧的频点数 */
    uint8_t frame[FREQTABLE_FRAME_MAX];
    size_t flen = 0;
    struct amp_ctrl_msg msg;
    size_t msg_bytes;
    int attempt;

    /* ---------- 第一步：读最近下发的频表号（失败路径零额外延时） ---------- */
    ls_buf = ft_read_file(FREQTABLE_LAST_SENT_FILE, &ls_len);
    if (!ls_buf) {
        fprintf(stderr, "[WARN] freqtable: cannot read %s, skip 0x09 send on boot\n",
                FREQTABLE_LAST_SENT_FILE);
        return -1;
    }
    if (!ft_parse_last_sent(ls_buf, ls_len, &table_id)) {
        free(ls_buf);
        fprintf(stderr, "[WARN] freqtable: parse %s failed, skip 0x09 send on boot\n",
                FREQTABLE_LAST_SENT_FILE);
        return -1;
    }
    free(ls_buf);                  /* 只需要这一个整数，立刻释放 */
    if (table_id < 0 || table_id > FREQTABLE_TABLE_ID_MAX) {
        fprintf(stderr, "[WARN] freqtable: lastSentTableId %ld out of range 0-%d, skip 0x09 send on boot\n",
                table_id, FREQTABLE_TABLE_ID_MAX);
        return -1;
    }

    /* ---------- 第二步：读频表库并组 0x09 帧（失败原因已在 ft_* 内打印） ---------- */
    st_buf = ft_read_file(FREQTABLE_STORE_FILE, &st_len);
    if (!st_buf) {
        fprintf(stderr, "[WARN] freqtable: cannot read %s, skip 0x09 send on boot\n",
                FREQTABLE_STORE_FILE);
        return -1;
    }
    if (ft_build_frame(st_buf, st_len, table_id, frame, &flen, &points) != 0) {
        free(st_buf);
        return -1;
    }
    free(st_buf);

    fprintf(stderr, "[INFO] freqtable: 0x09 frame built (tableId=%ld, points=%ld, len=%lu)\n",
            table_id, points, (unsigned long)flen);

    /* ---------- 第三步：初始延时（与 0x19 同款，给路由固件留出启动时间） ---------- */
    sleep(FREQTABLE_BOOT_DELAY_SEC);

    /* ---------- 第四步：组控制消息并写入 /dev/amp_ctrl ----------
     * 调用时机在 main 线程、任何工作线程创建之前（user_main.c 步骤 5.5.1），
     * 与 clock_send_on_boot 同路径，不存在并发写 ctrl_fd 的竞争。 */
    memset(&msg, 0, sizeof(msg));
    msg.len = (uint32_t)flen;
    msg.data_type = 1;             /* 控制数据类型：1=控制帧（与透传路径一致） */
    memcpy(msg.data, frame, flen);
    msg_bytes = offsetof(struct amp_ctrl_msg, data) + flen;

    for (attempt = 0; attempt < FREQTABLE_BOOT_RETRY; attempt++) {
        if (ft_write_ctrl(&msg, msg_bytes) == 0) {
            fprintf(stderr, "[INFO] freqtable frame(0x09) sent to router on boot (tableId=%ld, points=%ld, len=%lu)\n",
                    table_id, points, (unsigned long)flen);
            return 0;
        }

        fprintf(stderr, "[WARN] freqtable frame(0x09) send failed, retry %d/%d\n",
                attempt + 1, FREQTABLE_BOOT_RETRY);
        sleep(FREQTABLE_BOOT_RETRY_GAP_SEC);
    }

    fprintf(stderr, "[ERROR] freqtable frame(0x09) send gave up after %d retries\n",
            FREQTABLE_BOOT_RETRY);
    return -1;
}
