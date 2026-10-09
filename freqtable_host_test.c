/***********************************************/
/*  freqtable_host_test.c —— user_freqtable.c 主机端单测  */
/*  仿 bcast_test.c / pkt_sniff.c 的根级调试工具先例，     */
/*  不参与 amp_user 主构建（Makefile 不含此文件）。         */
/*                                                        */
/*  编译（Git Bash，MinGW gcc + .workbuddy 桩头目录）：    */
/*    gcc -std=gnu99 -Wall -Wextra \                      */
/*        -I.workbuddy/freqtable-test \                   */
/*        -I.workbuddy/freqtable-test/inc \               */
/*        -Iamp_user freqtable_host_test.c \              */
/*        amp_user/cjson/cJSON.c \                        */
/*        -o .workbuddy/freqtable-test/freqtable_host_test.exe */
/*  运行：.workbuddy/freqtable-test/freqtable_host_test.exe */
/*                                                        */
/*  通过 #include 直接触达 user_freqtable.c 的 static 函数；*/
/*  write/sleep/ctrl_fd 提供桩实现（单测不真正发设备）。    */
/***********************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amp_user/user_freqtable.c"   /* 触达 static 函数 */

/* ==== 桩：满足链接（单测不走到真实设备路径） ==== */
int ctrl_fd = -1;

ssize_t write(int fd, const void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    return (ssize_t)count;             /* 假装全量写成功 */
}

unsigned int sleep(unsigned int seconds)
{
    (void)seconds;
    return 0;                          /* 立即返回，避免单测变慢 */
}

/* ==== 断言辅助 ==== */
static int g_failed = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] line %d: %s\n", __LINE__, #cond); \
        g_failed++; \
    } \
} while (0)

/* ========= 1. BCD 编码 ========= */
static void test_bcd(void)
{
    uint8_t b[4];

    fprintf(stderr, "--- test_bcd ---\n");

    CHECK(ft_encode_bcd(b, 1, 1) == 0 && b[0] == 0x01);          /* 1/1B -> 01 */
    CHECK(ft_encode_bcd(b, 2, 321) == 0 && b[0] == 0x03 && b[1] == 0x21);
    CHECK(ft_encode_bcd(b, 2, 5) == 0 && b[0] == 0x00 && b[1] == 0x05);
    CHECK(ft_encode_bcd(b, 2, 470) == 0 && b[0] == 0x04 && b[1] == 0x70);
    CHECK(ft_encode_bcd(b, 1, 99) == 0 && b[0] == 0x99);         /* 1B 容量上界 */
    CHECK(ft_encode_bcd(b, 1, 100) == -1);                       /* 超 1B 容量 */
    CHECK(ft_encode_bcd(b, 2, -1) == -1);                        /* 负数 */
    CHECK(ft_encode_bcd(b, 2, 10000) == -1);                     /* 超 2B 容量 */
}

/* ========= 2. lastSentFreqTable.json 解析 ========= */
/* 借助 strlen 传长度，避免手数字节数出错（长度算错属于测试自身 bug） */
#define LS_OK(s, want) do { \
    long v_ = -1; \
    CHECK(ft_parse_last_sent((s), strlen(s), &v_) == 1 && v_ == (want)); \
} while (0)

#define LS_BAD(s) do { \
    long v_ = -1; \
    CHECK(ft_parse_last_sent((s), strlen(s), &v_) == 0); \
} while (0)

static void test_last_sent(void)
{
    long v = -1;

    fprintf(stderr, "--- test_last_sent ---\n");

    LS_OK("{\n  \"lastSentTableId\" : 7\n}", 7);                     /* Jackson 缩进风格 */
    LS_OK("{\"lastSentTableId\":42}", 42);                          /* 紧凑风格 */
    LS_OK("{\"other\":[1,2,{\"x\":true}],\"lastSentTableId\":0,\"t\":null}", 0); /* 额外字段+嵌套跳过 */
    {
        /* BOM 前缀变体 */
        static const char bom_json[] = "\xEF\xBB\xBF{\"lastSentTableId\":9}";
        CHECK(ft_parse_last_sent(bom_json, sizeof(bom_json) - 1, &v) == 1 && v == 9);
    }
    /* 失败路径 */
    LS_BAD("{}");                                       /* 空对象 */
    LS_BAD("{\"lastSentTableId\":\"5\"}");              /* 字符串值 */
    LS_BAD("{\"lastSentTableId\":4.5}");                /* 浮点 */
    LS_BAD("{\"a\":1}");                                /* 缺键 */
    LS_BAD("garbage");                                  /* 非 JSON */
}

/* ========= 3. 组帧：正常路径 ========= */
static const uint8_t expect2[24] = {
    0x18, 0x00,                          /* 总帧长 24（含长度字段自身） */
    0x00, 0x00,                          /* 保留 */
    0x00, 0x0C,                          /* 目的 ID 0x0C00（路由，小端） */
    0x00, 0x0A,                          /* 源 ID 0x0A00（主控，小端） */
    0xF5, 0xFF,                          /* 同步序列 0xFFF5（小端） */
    0x09, 0x00,                          /* 类型 0x09 / 保留 */
    0x01,                                /* 频表 ID = 1（BCD） */
    0x00, 0x02,                          /* 频点总数 = 2（BCD） */
    0x00, 0x01, 0x04, 0x70,              /* 点1：编号 1，频率 470 */
    0x00, 0x02, 0x04, 0x80,              /* 点2：编号 2，频率 480 */
    0xF9                                 /* 校验和 = 09^00^01^00^02^00^01^04^70^00^02^04^80 */
};

/* Jackson pretty-print 风格（与网管实际写盘格式一致的两表样例） */
static const char *json_pretty =
    "[ {\n"
    "  \"tableId\" : 1,\n"
    "  \"pointCount\" : 2,\n"
    "  \"points\" : [ {\n"
    "    \"pointId\" : 1,\n"
    "    \"frequency\" : 470\n"
    "  }, {\n"
    "    \"pointId\" : 2,\n"
    "    \"frequency\" : 480\n"
    "  } ]\n"
    "}, {\n"
    "  \"tableId\" : 2,\n"
    "  \"pointCount\" : 1,\n"
    "  \"points\" : [ {\n"
    "    \"pointId\" : 1,\n"
    "    \"frequency\" : 500\n"
    "  } ]\n"
    "} ]";

/* key 乱序 + 未知字段（含转义字符串/true/null/负数/浮点）+ 紧凑风格 */
static const char *json_shuffled =
    "[\n"
    "  {\n"
    "    \"points\" : [ { \"frequency\" : 470, \"pointId\" : 1 },\n"
    "                   { \"pointId\" : 2, \"frequency\" : 480 } ],\n"
    "    \"tableId\" : 1,\n"
    "    \"extra\" : { \"note\" : \"a\\\"b\", \"flag\" : true, \"n\" : null, \"arr\" : [1, 2.5, -3] },\n"
    "    \"pointCount\" : 2\n"
    "  }\n"
    "]";

/* 重算校验和（独立实现，防 ft_xor_checksum 自证） */
static uint8_t recompute_xor(const uint8_t *f, size_t len)
{
    uint8_t c = f[10];
    size_t i;

    for (i = 11; i < len - 1; i++)
        c ^= f[i];
    return c;
}

/* 生成 n 个频点的大表 JSON（点号 i、频率 470 + i%321，保证全部合法） */
static char *make_big_json(int n_points, int table_id)
{
    size_t cap = (size_t)n_points * 48 + 128;
    char *s = malloc(cap);
    size_t off = 0;
    int i;

    if (!s)
        return NULL;
    off += (size_t)snprintf(s + off, cap - off,
                            "[ { \"tableId\" : %d, \"pointCount\" : %d, \"points\" : [ ",
                            table_id, n_points);
    for (i = 0; i < n_points; i++) {
        off += (size_t)snprintf(s + off, cap - off,
                                "%s{ \"pointId\" : %d, \"frequency\" : %d }",
                                i ? ", " : "", i, 470 + i % 321);
    }
    off += (size_t)snprintf(s + off, cap - off, " ] } ]");
    return s;
}

static void test_build_ok(void)
{
    uint8_t frame[FREQTABLE_FRAME_MAX];
    size_t flen = 0;
    long points = 0;
    char *big;
    uint8_t frame2[FREQTABLE_FRAME_MAX];
    size_t flen2 = 0;
    long points2 = 0;

    fprintf(stderr, "--- test_build_ok ---\n");

    /* Jackson 缩进样例：逐字节比对样例帧 */
    CHECK(ft_build_frame(json_pretty, strlen(json_pretty), 1,
                         frame, &flen, &points) == 0);
    CHECK(flen == 24 && points == 2);
    CHECK(memcmp(frame, expect2, 24) == 0);

    /* key 乱序 + 未知字段变体：产出同一帧 */
    CHECK(ft_build_frame(json_shuffled, strlen(json_shuffled), 1,
                         frame2, &flen2, &points2) == 0);
    CHECK(flen2 == 24 && points2 == 2);
    CHECK(memcmp(frame2, expect2, 24) == 0);

    /* BOM 前缀变体 */
    {
        static const char bom_head[] = "\xEF\xBB\xBF";
        char *s = malloc(sizeof(bom_head) - 1 + strlen(json_pretty) + 1);
        uint8_t frame3[FREQTABLE_FRAME_MAX];
        size_t flen3 = 0;
        long points3 = 0;

        if (s) {
            memcpy(s, bom_head, sizeof(bom_head) - 1);
            memcpy(s + sizeof(bom_head) - 1, json_pretty, strlen(json_pretty) + 1);
            CHECK(ft_build_frame(s, strlen(s), 1, frame3, &flen3, &points3) == 0);
            CHECK(flen3 == 24 && memcmp(frame3, expect2, 24) == 0);
            free(s);
        }
    }

    /* 321 点极限表：帧长 1300、总数 BCD 03 21、首尾点字段、校验和自洽 */
    big = make_big_json(321, 9);
    if (big) {
        uint8_t frame4[FREQTABLE_FRAME_MAX];
        size_t flen4 = 0;
        long points4 = 0;

        CHECK(ft_build_frame(big, strlen(big), 9,
                             frame4, &flen4, &points4) == 0);
        CHECK(flen4 == 16 + 4 * 321 && points4 == 321);
        CHECK(frame4[12] == 0x09);                    /* 表 ID 9 */
        CHECK(frame4[13] == 0x03 && frame4[14] == 0x21);  /* 总数 321 */
        CHECK(frame4[15] == 0x00 && frame4[16] == 0x00 && /* 点0：编号 0 */
              frame4[17] == 0x04 && frame4[18] == 0x70);  /*       频率 470 */
        CHECK(frame4[15 + 4 * 320] == 0x03 && frame4[16 + 4 * 320] == 0x20 && /* 点320：编号 320 */
              frame4[17 + 4 * 320] == 0x07 && frame4[18 + 4 * 320] == 0x90); /*       频率 790 */
        CHECK(recompute_xor(frame4, flen4) == frame4[flen4 - 1]); /* 校验和自洽 */
        free(big);
    }

    /* 重复 tableId：首个命中即用（第二条同号表被忽略） */
    {
        static const char *dup_json =
            "[ { \"tableId\" : 1, \"pointCount\" : 2, \"points\" : "
            "[ { \"pointId\" : 1, \"frequency\" : 470 }, { \"pointId\" : 2, \"frequency\" : 480 } ] },"
            "  { \"tableId\" : 1, \"pointCount\" : 1, \"points\" : "
            "[ { \"pointId\" : 9, \"frequency\" : 790 } ] } ]";
        uint8_t frame6[FREQTABLE_FRAME_MAX];
        size_t flen6 = 0;
        long points6 = 0;

        CHECK(ft_build_frame(dup_json, strlen(dup_json), 1,
                             frame6, &flen6, &points6) == 0);
        CHECK(flen6 == 24 && points6 == 2);
        CHECK(memcmp(frame6, expect2, 24) == 0);
    }

    /* cJSON 语义：470.0 是整数值的浮点形式，整数性校验通过（470.5 才拒发） */
    {
        static const char *float_int_json =
            "[ { \"tableId\" : 1, \"points\" : [ { \"pointId\" : 1, \"frequency\" : 470.0 } ] } ]";
        uint8_t frame7[FREQTABLE_FRAME_MAX];
        size_t flen7 = 0;
        long points7 = 0;

        CHECK(ft_build_frame(float_int_json, strlen(float_int_json), 1,
                             frame7, &flen7, &points7) == 0);
        CHECK(flen7 == 20 && points7 == 1);
        CHECK(frame7[15] == 0x00 && frame7[16] == 0x01 &&
              frame7[17] == 0x04 && frame7[18] == 0x70);
        CHECK(recompute_xor(frame7, flen7) == frame7[flen7 - 1]);
    }

    /* pointCount 声明值 != 实际：仍成功，帧内写实际数 */
    {
        static const char *mismatch =
            "[ { \"tableId\" : 1, \"pointCount\" : 5, \"points\" : "
            "[ { \"pointId\" : 1, \"frequency\" : 470 }, { \"pointId\" : 2, \"frequency\" : 480 } ] } ]";
        uint8_t frame5[FREQTABLE_FRAME_MAX];
        size_t flen5 = 0;
        long points5 = 0;

        CHECK(ft_build_frame(mismatch, strlen(mismatch), 1,
                             frame5, &flen5, &points5) == 0);
        CHECK(flen5 == 24 && points5 == 2);
        CHECK(memcmp(frame5, expect2, 24) == 0);
    }
}

/* ========= 4. 组帧：错误路径（全部拒发；长度一律 strlen 传参） ========= */
#define BF_BAD(json, id) \
    CHECK(ft_build_frame((json), strlen(json), (id), frame, &flen, &points) == -1)

static void test_build_fail(void)
{
    uint8_t frame[FREQTABLE_FRAME_MAX];
    size_t flen = 0;
    long points = 0;
    char *big;

    fprintf(stderr, "--- test_build_fail（以下 WARN 为预期输出） ---\n");

    /* 空库 / 表不存在 / 结构非法 */
    BF_BAD("[]", 1);
    BF_BAD(json_pretty, 99);
    BF_BAD("[{\"tableId\":1,", 1);
    BF_BAD("{}", 1);                                    /* 非顶层数组 */

    /* 空 points / 缺 points */
    BF_BAD("[ { \"tableId\" : 1, \"points\" : [ ] } ]", 1);
    BF_BAD("[ { \"tableId\" : 1 } ]", 1);

    /* 频点字段超限 / 浮点频率 / 缺字段 */
    BF_BAD("[ { \"tableId\" : 1, \"points\" : [ { \"pointId\" : 600, \"frequency\" : 470 } ] } ]", 1);
    BF_BAD("[ { \"tableId\" : 1, \"points\" : [ { \"pointId\" : 1, \"frequency\" : 800 } ] } ]", 1);
    BF_BAD("[ { \"tableId\" : 1, \"points\" : [ { \"pointId\" : 1, \"frequency\" : 469 } ] } ]", 1);
    BF_BAD("[ { \"tableId\" : 1, \"points\" : [ { \"pointId\" : 1, \"frequency\" : 470.5 } ] } ]", 1);
    BF_BAD("[ { \"tableId\" : 1, \"points\" : [ { \"pointId\" : 1 } ] } ]", 1);

    /* 322 个合法频点：超出协议单帧上限 321 */
    big = make_big_json(322, 1);
    if (big) {
        CHECK(ft_build_frame(big, strlen(big), 1,
                             frame, &flen, &points) == -1);
        free(big);
    }
}

/* ========= 5. 入口冒烟：板上路径在 Windows 不存在，应优雅返回 -1 ========= */
static void test_entry_smoke(void)
{
    fprintf(stderr, "--- test_entry_smoke ---\n");
    CHECK(freqtable_send_on_boot() == -1);   /* 文件缺失路径：WARN + 跳过 */
}

int main(void)
{
    test_bcd();
    test_last_sent();
    test_build_ok();
    test_build_fail();
    test_entry_smoke();

    if (g_failed == 0) {
        fprintf(stderr, "\nALL TESTS PASSED\n");
        return 0;
    }
    fprintf(stderr, "\n%d CHECK(S) FAILED\n", g_failed);
    return 1;
}
