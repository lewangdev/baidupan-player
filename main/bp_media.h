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
    BP_FMT_M4A,    // MP4 容器中的 AAC
    BP_FMT_AAC,    // ADTS 裸流
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

// 按文件内容识别容器/编码(网盘里常见扩展名与内容不符,如 M4A 被命名为 .mp3)。
typedef enum {
    BP_SNIFF_UNKNOWN = 0,
    BP_SNIFF_MP3,        // ID3 标签或有效的 MPEG Layer III 帧头
    BP_SNIFF_WAV,
    BP_SNIFF_MP4,        // M4A / MP4(ftyp)
    BP_SNIFF_AAC,        // ADTS AAC
    BP_SNIFF_FLAC,
    BP_SNIFF_OGG,
} bp_sniff_t;
bp_sniff_t bp_media_sniff(const uint8_t *buf, size_t len);

// MPEG-1/2/2.5 Layer III 帧头。
typedef struct {
    uint8_t version;       // 1 = MPEG-1, 2 = MPEG-2, 25 = MPEG-2.5
    uint32_t samplerate;
    uint32_t bitrate;      // bit/s
    uint8_t channels;
    uint32_t frame_len;    // 含帧头的整帧字节数
} bp_mp3_hdr_t;
bool bp_mp3_parse_header(const uint8_t *h, bp_mp3_hdr_t *out);

// 在 buf 中查找“可信”的帧起点:帧头有效,且紧随其后的下一帧帧头也有效、参数一致
// (至少差一帧数据时,at_eof 为真则只校验当前帧)。want 非空时还要求版本/采样率与之一致。
// 返回偏移;-1 表示 buf 中没有(调用方可丢弃除末尾 3 字节外的数据);
// -2 表示 *partial 处有候选但需要更多数据才能确认。
int bp_mp3_find_frame(const uint8_t *buf, size_t len, bool at_eof, const bp_mp3_hdr_t *want,
                      bp_mp3_hdr_t *out, size_t *partial);

// 把文件名缩短到 cap(含 NUL)以内:保留扩展名,中间用“…”,不切断 UTF-8 字符。
void bp_name_shorten(const char *src, char *out, size_t cap);

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

// 屏幕亮度:共 BP_BRIGHTNESS_LEVELS 档(1..5),返回背光百分比;越界按最近一档处理。
#define BP_BRIGHTNESS_LEVELS 5
uint8_t bp_brightness_percent(int level);
// 档位步进并夹到 [1, BP_BRIGHTNESS_LEVELS]。
int bp_brightness_step(int level, int delta);

// 播放列表索引移动。wrap=false 时越界返回 -1。
int bp_playlist_move(int current, int count, int delta, bool wrap);
