# cjson/ —— vendored 第三方库（cJSON）

本目录是 **[cJSON](https://github.com/DaveGamble/cJSON) v1.7.19** 的未修改拷贝，
供 `user_freqtable.c` 解析网管下发的 JSON 频表文件使用。

- 来源：https://github.com/DaveGamble/cJSON/tree/v1.7.19
- 版本：1.7.19（`cJSON.h` 中 `CJSON_VERSION_MAJOR/MINOR/PATCH` = 1/7/19）
- 拷贝日期：2026-10-08
- 文件：`cJSON.c`（约 80KB）、`cJSON.h`、`LICENSE`（MIT 原文）
- 修改：**无**（原样 vendor；升级时整体替换并同步本说明）
- 集成方式：单源文件静态编入 `user_amp`（见 `amp_user/Makefile` 的 SRCS），无需安装系统库
- 许可：MIT（见本目录 `LICENSE`）
- 用途说明：本工程只用解析 API（`cJSON_ParseWithLengthOpts` / `cJSON_GetObjectItemCaseSensitive` /
  `cJSON_ArrayForEach` / `cJSON_IsNumber` 等），未使用 `cJSON_Utils`
