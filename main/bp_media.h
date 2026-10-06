// main/bp_media.h —— 与 ESP-IDF/LVGL 无关的纯逻辑：格式识别、容器头解析、
// 时长/大小格式化、网盘路径计算、PCM 下混。全部可在主机上单测。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BP_PATH_MAX 256

typedef enum {
    BP_FMT_UNKNOWN = 0,
    BP_FMT_MP3,
    BP_FMT_WAV,
} bp_format_t;

typedef struct {
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t data_offset;   // 第一个 PCM 字节在文件中的偏移
    uint32_t data_bytes;    // data 块声明长度(流式 WAV 可能为 0 或 0xFFFFFFFF)
} bp_wav_info_t;

// 按扩展名(不区分大小写)判断可播放格式。
bp_format_t bp_media_format(const char *name);

// 浏览网盘时是否显示该条目:文件夹或可播放的音频。
bool bp_media_listable(bool is_dir, const char *name);

// ID3v2 标签总长度(含 10 字节头和可选尾);无标签或数据不足 10 字节返回 0。
size_t bp_id3v2_size(const uint8_t *buf, size_t len);

// 解析 RIFF/WAVE 头。返回 0 成功;1 需要更多数据;-1 非 WAV;-2 不支持的编码
// (仅支持 PCM 16 bit、1/2 声道、8k..48k)。
int bp_wav_parse(const uint8_t *buf, size_t len, bp_wav_info_t *out);

// 按码率估算时长(毫秒)。bitrate 为 0 返回 0。
uint32_t bp_estimate_ms(uint64_t bytes, uint32_t bitrate_bps);

// PCM 字节数换算毫秒。
uint32_t bp_pcm_ms(uint64_t frames, uint32_t rate);

// "m:ss" 或 "h:mm:ss"。
void bp_format_time(uint32_t ms, char *out, size_t cap);

// "512 B" / "3.4 KB" / "12.0 MB" / "1.2 GB"。
void bp_format_size(uint64_t bytes, char *out, size_t cap);

// 网盘绝对路径:根为 "/"。名称不能为空、"."、".." 或含 '/' 与控制字符。
bool bp_path_child(const char *dir, const char *name, char *out, size_t cap);
// "/a/b" → "/a";"/a" → "/";"/" 返回 false。
bool bp_path_parent(const char *dir, char *out, size_t cap);
// "/a/b" → "b";"/" → "/"。
const char *bp_path_basename(const char *path);

// 16 bit 交错双声道就地下混为单声道,返回单声道采样数。
size_t bp_downmix_s16(int16_t *pcm, size_t frames);

// 音量步进并夹到 [0, 100]。
int bp_volume_step(int volume, int delta);

// 播放列表索引移动。wrap=false 时越界返回 -1。
int bp_playlist_move(int current, int count, int delta, bool wrap);
