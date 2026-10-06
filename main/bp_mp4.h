// main/bp_mp4.h —— 流式 M4A(MP4 容器内的 AAC)头部解析,纯逻辑,主机可测。
//
// 只顺序读取一遍:解析 moov 中第一条 AAC 音轨的解码参数(AudioSpecificConfig)、
// 时长和首个数据块偏移,跳过体积巨大的 stsz/stco 表(不存入内存),停在第一帧
// 音频数据处。之后由 AAC 解码器以“裸数据块”方式顺序解码 mdat,不需要逐帧大小表。
// 要求 moov 位于 mdat 之前(常见的 faststart 布局)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 顺序读取接口:read 读满 n 字节返回 0,skip 向后跳过 n 字节返回 0。
typedef struct {
    int (*read)(void *ctx, uint8_t *dst, size_t n);
    int (*skip)(void *ctx, uint64_t n);
    void *ctx;
    uint64_t pos;   // 已消费字节(由解析器维护)
} bp_reader_t;

typedef struct {
    uint8_t object_type;     // 2 = AAC-LC(HE-AAC 已折算为其核心层)
    bool sbr;                // 显式标记了 SBR/PS(无 SBR 解码时按核心层输出)
    uint32_t samplerate;     // 核心层采样率
    uint8_t channels;
    uint32_t timescale;
    uint64_t duration;       // 以 timescale 为单位
    uint64_t data_start;     // 首个音频数据字节的文件偏移
    uint64_t data_end;       // mdat 结束偏移(不含)
} bp_m4a_info_t;

enum {
    BP_M4A_OK = 0,
    BP_M4A_IO = -1,          // 读取失败或数据提前结束
    BP_M4A_FORMAT = -2,      // 不是 MP4,或结构损坏
    BP_M4A_NO_AAC = -3,      // 没有可解码的 AAC 音轨(含加密、Main/LTP 等)
    BP_M4A_MOOV_LAST = -4,   // mdat 在 moov 之前,无法流式播放
};

// 从文件开头解析;成功时 reader 恰好停在 info->data_start。file_size 为 0 表示未知。
int bp_m4a_parse(bp_reader_t *r, uint64_t file_size, bp_m4a_info_t *info);

// 解析 AudioSpecificConfig。成功返回 true。
bool bp_aac_parse_asc(const uint8_t *asc, size_t len, bp_m4a_info_t *info);

// 总时长(毫秒),未知返回 0。
uint32_t bp_m4a_duration_ms(const bp_m4a_info_t *info);
