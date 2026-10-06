#include "bp_media.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

bp_format_t bp_media_format(const char *name) {
    if (!name) return BP_FMT_UNKNOWN;
    const char *ext = strrchr(name, '.');
    if (!ext || ext == name) return BP_FMT_UNKNOWN;
    if (!strcasecmp(ext, ".mp3")) return BP_FMT_MP3;
    if (!strcasecmp(ext, ".wav")) return BP_FMT_WAV;
    if (!strcasecmp(ext, ".m4a")) return BP_FMT_M4A;
    if (!strcasecmp(ext, ".aac")) return BP_FMT_AAC;
    return BP_FMT_UNKNOWN;
}

bool bp_media_listable(bool is_dir, const char *name) {
    return is_dir || bp_media_format(name) != BP_FMT_UNKNOWN;
}

bp_sniff_t bp_media_sniff(const uint8_t *buf, size_t len) {
    if (!buf || len < 4) return BP_SNIFF_UNKNOWN;
    if (!memcmp(buf, "RIFF", 4)) return BP_SNIFF_WAV;
    if (!memcmp(buf, "ID3", 3)) return BP_SNIFF_MP3;
    if (!memcmp(buf, "fLaC", 4)) return BP_SNIFF_FLAC;
    if (!memcmp(buf, "OggS", 4)) return BP_SNIFF_OGG;
    if (len >= 8 && !memcmp(buf + 4, "ftyp", 4)) return BP_SNIFF_MP4;
    bp_mp3_hdr_t h;
    if (bp_mp3_parse_header(buf, &h)) return BP_SNIFF_MP3;
    // ADTS:同步字 12 位全 1,layer 字段为 0(MP3 的 layer 字段非 0)。
    if (buf[0] == 0xff && (buf[1] & 0xf6) == 0xf0) return BP_SNIFF_AAC;
    return BP_SNIFF_UNKNOWN;
}

bool bp_mp3_parse_header(const uint8_t *h, bp_mp3_hdr_t *out) {
    static const uint16_t br_v1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint16_t br_v2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const uint32_t sr_v1[3] = {44100, 48000, 32000};
    if (!h || h[0] != 0xff || (h[1] & 0xe0) != 0xe0) return false;
    int ver_bits = (h[1] >> 3) & 3;        // 0:2.5 1:保留 2:MPEG-2 3:MPEG-1
    int layer_bits = (h[1] >> 1) & 3;      // 1 = Layer III
    int br_idx = h[2] >> 4, sr_idx = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
    if (ver_bits == 1 || layer_bits != 1 || br_idx == 0 || br_idx == 15 || sr_idx == 3 ||
        (h[3] & 3) == 2)
        return false;
    bp_mp3_hdr_t r;
    r.version = ver_bits == 3 ? 1 : ver_bits == 2 ? 2 : 25;
    int div = r.version == 1 ? 1 : r.version == 2 ? 2 : 4;
    r.samplerate = sr_v1[sr_idx] / (uint32_t)div;
    r.bitrate = (uint32_t)(r.version == 1 ? br_v1[br_idx] : br_v2[br_idx]) * 1000;
    r.channels = (h[3] >> 6) == 3 ? 1 : 2;
    r.frame_len = (r.version == 1 ? 144 : 72) * r.bitrate / r.samplerate + (uint32_t)pad;
    if (out) *out = r;
    return true;
}

static bool hdr_compatible(const bp_mp3_hdr_t *a, const bp_mp3_hdr_t *b) {
    return a->version == b->version && a->samplerate == b->samplerate;
}

int bp_mp3_find_frame(const uint8_t *buf, size_t len, bool at_eof, const bp_mp3_hdr_t *want,
                      bp_mp3_hdr_t *out, size_t *partial) {
    if (!buf) return -1;
    for (size_t i = 0; i + 4 <= len; i++) {
        bp_mp3_hdr_t h;
        if (!bp_mp3_parse_header(buf + i, &h)) continue;
        if (want && !hdr_compatible(&h, want)) continue;
        size_t next = i + h.frame_len;
        if (next + 4 > len) {
            if (at_eof) {
                if (out) *out = h;
                return (int)i;
            }
            if (partial) *partial = i;
            return -2;
        }
        bp_mp3_hdr_t n;
        if (!bp_mp3_parse_header(buf + next, &n) || !hdr_compatible(&h, &n)) continue;
        if (out) *out = h;
        return (int)i;
    }
    return -1;
}

static size_t utf8_floor(const char *s, size_t n) {
    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) n--;
    return n;
}

void bp_name_shorten(const char *src, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!src) return;
    size_t len = strlen(src);
    if (len < cap) {
        memcpy(out, src, len + 1);
        return;
    }
    static const char ELLIPSIS[] = "\xe2\x80\xa6";   // …
    const char *dot = strrchr(src, '.');
    size_t ext = dot && dot != src && (size_t)(src + len - dot) <= 10 ? (size_t)(src + len - dot) : 0;
    size_t room = cap - 1;
    if (room < ext + 3 + 1) ext = 0;            // 太短放不下扩展名时只截断
    size_t keep = room - ext - (ext ? 3 : 0);
    keep = utf8_floor(src, keep);
    memcpy(out, src, keep);
    size_t o = keep;
    if (ext) {
        memcpy(out + o, ELLIPSIS, 3);
        o += 3;
        memcpy(out + o, src + len - ext, ext);
        o += ext;
    }
    out[o] = 0;
}

size_t bp_id3v2_size(const uint8_t *buf, size_t len) {
    if (!buf || len < 10 || memcmp(buf, "ID3", 3) != 0) return 0;
    if (buf[3] == 0xff || buf[4] == 0xff) return 0;
    for (int i = 6; i < 10; i++)
        if (buf[i] & 0x80) return 0;   // syncsafe 整数最高位必须为 0
    size_t size = ((size_t)buf[6] << 21) | ((size_t)buf[7] << 14) |
                  ((size_t)buf[8] << 7) | (size_t)buf[9];
    size += 10;
    if (buf[5] & 0x10) size += 10;     // footer present
    return size;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int bp_wav_parse(const uint8_t *buf, size_t len, bp_wav_info_t *out) {
    if (!buf || !out) return -1;
    if (len < 12) return 1;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) return -1;
    bool have_fmt = false;
    bp_wav_info_t info = {0};
    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *chunk = buf + pos;
        uint32_t size = le32(chunk + 4);
        if (!memcmp(chunk, "fmt ", 4)) {
            if (size < 16) return -2;
            if (pos + 8 + 16 > len) return 1;
            uint16_t tag = le16(chunk + 8);
            info.channels = le16(chunk + 10);
            info.rate = le32(chunk + 12);
            info.bits = le16(chunk + 22);
            // WAVE_FORMAT_EXTENSIBLE: 子格式 GUID 前两字节即格式码。
            if (tag == 0xfffe && size >= 40) {
                if (pos + 8 + 26 > len) return 1;
                tag = le16(chunk + 8 + 24);
            }
            if (tag != 1 || info.bits != 16 || info.channels < 1 ||
                info.channels > 2 || info.rate < 8000 || info.rate > 48000)
                return -2;
            have_fmt = true;
        } else if (!memcmp(chunk, "data", 4)) {
            if (!have_fmt) return -2;
            info.data_offset = (uint32_t)(pos + 8);
            info.data_bytes = size;
            *out = info;
            return 0;
        }
        // 块按偶数对齐;超长块(如 LIST 里的大封面)可能需要更多数据。
        uint64_t next = (uint64_t)pos + 8 + size + (size & 1);
        if (next > 64 * 1024) return -2;   // 头部过大,不在流式缓冲内处理
        pos = (size_t)next;
    }
    return 1;
}

uint32_t bp_estimate_ms(uint64_t bytes, uint32_t bitrate_bps) {
    if (!bitrate_bps) return 0;
    uint64_t ms = bytes * 8000ULL / bitrate_bps;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

uint32_t bp_pcm_ms(uint64_t frames, uint32_t rate) {
    if (!rate) return 0;
    uint64_t ms = frames * 1000ULL / rate;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

void bp_format_time(uint32_t ms, char *out, size_t cap) {
    if (!out || !cap) return;
    uint32_t s = ms / 1000;
    uint32_t h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    if (h) snprintf(out, cap, "%lu:%02lu:%02lu", (unsigned long)h,
                    (unsigned long)m, (unsigned long)sec);
    else snprintf(out, cap, "%lu:%02lu", (unsigned long)m, (unsigned long)sec);
}

void bp_format_size(uint64_t bytes, char *out, size_t cap) {
    if (!out || !cap) return;
    static const char *units[] = {"KB", "MB", "GB", "TB"};
    if (bytes < 1024) {
        snprintf(out, cap, "%u B", (unsigned)bytes);
        return;
    }
    // 定点一位小数,避免依赖浮点格式化。
    uint64_t scaled = bytes * 10 / 1024;
    int unit = 0;
    while (scaled >= 10240 && unit < 3) {
        scaled /= 1024;
        unit++;
    }
    snprintf(out, cap, "%u.%u %s", (unsigned)(scaled / 10), (unsigned)(scaled % 10),
             units[unit]);
}

static bool name_valid(const char *name) {
    if (!name || !name[0] || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32 || *p == 127 || *p == '/') return false;
    return true;
}

static bool dir_valid(const char *dir) {
    if (!dir || dir[0] != '/') return false;
    size_t len = strlen(dir);
    if (len >= BP_PATH_MAX) return false;
    if (len > 1 && dir[len - 1] == '/') return false;
    return strstr(dir, "//") == NULL;
}

bool bp_path_child(const char *dir, const char *name, char *out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    if (!dir_valid(dir) || !name_valid(name)) return false;
    int n = !strcmp(dir, "/") ? snprintf(out, cap, "/%s", name)
                              : snprintf(out, cap, "%s/%s", dir, name);
    if (n > 0 && (size_t)n < cap && n < BP_PATH_MAX) return true;
    out[0] = 0;
    return false;
}

bool bp_path_parent(const char *dir, char *out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    if (!dir_valid(dir) || !strcmp(dir, "/")) return false;
    const char *slash = strrchr(dir, '/');
    size_t len = (size_t)(slash - dir);
    if (len == 0) len = 1;             // 父目录是根
    if (len >= cap) return false;
    memcpy(out, dir, len);
    out[len] = 0;
    return true;
}

const char *bp_path_basename(const char *path) {
    if (!path) return "";
    if (!strcmp(path, "/")) return path;
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

size_t bp_downmix_s16(int16_t *pcm, size_t frames) {
    if (!pcm) return 0;
    for (size_t i = 0; i < frames; i++) {
        int32_t mixed = ((int32_t)pcm[2 * i] + (int32_t)pcm[2 * i + 1]) / 2;
        pcm[i] = (int16_t)mixed;
    }
    return frames;
}

int bp_volume_step(int volume, int delta) {
    int v = volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

uint8_t bp_brightness_percent(int level) {
    // 低档间隔小、高档间隔大,人眼对暗处亮度变化更敏感。
    static const uint8_t PERCENT[BP_BRIGHTNESS_LEVELS] = {10, 25, 45, 70, 100};
    return PERCENT[bp_brightness_step(level, 0) - 1];
}

int bp_brightness_step(int level, int delta) {
    int v = level + delta;
    if (v < 1) v = 1;
    if (v > BP_BRIGHTNESS_LEVELS) v = BP_BRIGHTNESS_LEVELS;
    return v;
}

int bp_playlist_move(int current, int count, int delta, bool wrap) {
    if (count <= 0) return -1;
    int next = current + delta;
    if (next >= 0 && next < count) return next;
    if (!wrap) return -1;
    next %= count;
    if (next < 0) next += count;
    return next;
}
