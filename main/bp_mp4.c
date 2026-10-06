#include "bp_mp4.h"

#include <string.h>

#define MAX_DEPTH 8
#define ESDS_MAX 128

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

static int rd(bp_reader_t *r, void *dst, size_t n) {
    if (n && r->read(r->ctx, dst, n)) return -1;
    r->pos += n;
    return 0;
}

static int sk(bp_reader_t *r, uint64_t n) {
    if (n && r->skip(r->ctx, n)) return -1;
    r->pos += n;
    return 0;
}

// 跳到 end(不能回退)。
static int seek_to(bp_reader_t *r, uint64_t end) {
    if (end < r->pos) return BP_M4A_FORMAT;
    return sk(r, end - r->pos) ? BP_M4A_IO : BP_M4A_OK;
}

typedef struct {
    uint64_t start, end;
    char type[5];
} box_t;

// 读盒子头;limit 为父容器结束位置。
static int box_header(bp_reader_t *r, uint64_t limit, box_t *b) {
    uint8_t h[8];
    b->start = r->pos;
    if (rd(r, h, 8)) return BP_M4A_IO;
    uint64_t size = be32(h);
    memcpy(b->type, h + 4, 4);
    b->type[4] = 0;
    if (size == 1) {
        uint8_t e[8];
        if (rd(r, e, 8)) return BP_M4A_IO;
        size = be64(e);
    } else if (size == 0) {
        if (limit == UINT64_MAX) return BP_M4A_FORMAT;
        size = limit - b->start;
    }
    if (size < r->pos - b->start || b->start + size > limit) return BP_M4A_FORMAT;
    b->end = b->start + size;
    return BP_M4A_OK;
}

static const uint32_t SAMPLE_RATES[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                          22050, 16000, 12000, 11025, 8000, 7350};

typedef struct {
    const uint8_t *p;
    size_t len, bit;
} bits_t;

static bool getbits(bits_t *b, int n, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++, b->bit++) {
        if (b->bit / 8 >= b->len) return false;
        v = (v << 1) | ((b->p[b->bit / 8] >> (7 - b->bit % 8)) & 1);
    }
    *out = v;
    return true;
}

static bool get_aot(bits_t *b, uint32_t *aot) {
    if (!getbits(b, 5, aot)) return false;
    if (*aot == 31) {
        uint32_t ext;
        if (!getbits(b, 6, &ext)) return false;
        *aot = 32 + ext;
    }
    return true;
}

static bool get_rate(bits_t *b, uint32_t *rate) {
    uint32_t idx;
    if (!getbits(b, 4, &idx)) return false;
    if (idx == 15) return getbits(b, 24, rate);
    if (idx >= 13) return false;
    *rate = SAMPLE_RATES[idx];
    return true;
}

bool bp_aac_parse_asc(const uint8_t *asc, size_t len, bp_m4a_info_t *info) {
    bits_t b = {asc, len, 0};
    uint32_t aot, rate, ch;
    if (!get_aot(&b, &aot) || !get_rate(&b, &rate) || !getbits(&b, 4, &ch)) return false;
    bool sbr = false;
    if (aot == 5 || aot == 29) {   // 显式 HE-AAC / HE-AACv2:跳过扩展采样率,取核心层类型
        uint32_t ext_rate;
        sbr = true;
        if (!get_rate(&b, &ext_rate) || !get_aot(&b, &aot)) return false;
    }
    if (aot != 2) return false;    // 只支持 LC(Main/SSR/LTP 不支持)
    info->object_type = (uint8_t)aot;
    info->sbr = sbr;
    info->samplerate = rate;
    info->channels = (uint8_t)(ch == 0 || ch > 2 ? 2 : ch);   // 0=PCE,多声道下混
    return true;
}

// MPEG-4 描述符长度:1~4 字节,高位为续读标志。
static bool desc_len(const uint8_t *p, size_t n, size_t *pos, uint32_t *len) {
    *len = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= n) return false;
        uint8_t c = p[(*pos)++];
        *len = (*len << 7) | (c & 0x7f);
        if (!(c & 0x80)) return true;
    }
    return true;
}

// 从 esds 负载(已去掉 4 字节 FullBox 头)中取出 AudioSpecificConfig。
static bool parse_esds(const uint8_t *p, size_t n, bp_m4a_info_t *info) {
    size_t pos = 0;
    uint32_t len;
    if (pos >= n || p[pos++] != 0x03 || !desc_len(p, n, &pos, &len)) return false;
    if (pos + 3 > n) return false;
    pos += 2;   // ES_ID
    uint8_t flags = p[pos++];
    if (flags & 0x80) pos += 2;
    if (flags & 0x40) {
        if (pos >= n) return false;
        pos += 1 + p[pos];
    }
    if (flags & 0x20) pos += 2;
    if (pos >= n || p[pos++] != 0x04 || !desc_len(p, n, &pos, &len)) return false;
    if (pos + 13 > n) return false;
    uint8_t oti = p[pos];
    if (oti != 0x40 && oti != 0x66 && oti != 0x67 && oti != 0x68) return false;   // MPEG-4/2 AAC
    pos += 13;
    if (pos >= n || p[pos++] != 0x05 || !desc_len(p, n, &pos, &len)) return false;
    if (pos + len > n || len < 2) return false;
    return bp_aac_parse_asc(p + pos, len, info);
}

typedef struct {
    bool has_aac;
    bool has_media;
    bp_m4a_info_t aac;
    uint32_t timescale;
    uint64_t duration;
    uint64_t first_chunk;
} trak_t;

static int parse_container(bp_reader_t *r, uint64_t end, int depth, trak_t *trak,
                           bp_m4a_info_t *out, bool *found);

static int parse_stsd(bp_reader_t *r, uint64_t end, trak_t *trak) {
    uint8_t h[8];
    if (rd(r, h, 8)) return BP_M4A_IO;   // FullBox + entry_count
    if (be32(h + 4) == 0 || r->pos + 8 > end) return BP_M4A_OK;
    box_t entry;
    int rc = box_header(r, end, &entry);
    if (rc) return rc;
    if (strcmp(entry.type, "mp4a")) return seek_to(r, end);   // enca 等不支持
    uint8_t fields[28];
    if (rd(r, fields, sizeof(fields))) return BP_M4A_IO;
    uint16_t qt_version = (uint16_t)((fields[8] << 8) | fields[9]);
    if (qt_version == 1 && sk(r, 16)) return BP_M4A_IO;
    if (qt_version == 2 && sk(r, 36)) return BP_M4A_IO;
    while (r->pos + 8 <= entry.end) {
        box_t child;
        if ((rc = box_header(r, entry.end, &child))) return rc;
        if (!strcmp(child.type, "esds") && child.end - r->pos <= ESDS_MAX + 4) {
            uint8_t buf[ESDS_MAX + 4];
            size_t n = (size_t)(child.end - r->pos);
            if (rd(r, buf, n)) return BP_M4A_IO;
            if (n > 4 && parse_esds(buf + 4, n - 4, &trak->aac)) trak->has_aac = true;
        }
        if ((rc = seek_to(r, child.end))) return rc;
    }
    return seek_to(r, end);
}

static int parse_box(bp_reader_t *r, const box_t *b, int depth, trak_t *trak,
                     bp_m4a_info_t *out, bool *found) {
    static const char *CONTAINERS[] = {"moov", "trak", "mdia", "minf", "stbl"};
    for (size_t i = 0; i < sizeof(CONTAINERS) / sizeof(CONTAINERS[0]); i++) {
        if (strcmp(b->type, CONTAINERS[i])) continue;
        if (!strcmp(b->type, "trak")) {
            trak_t t = {0};
            int rc = parse_container(r, b->end, depth + 1, &t, out, found);
            if (!rc && t.has_aac && !*found) {
                *out = t.aac;
                out->timescale = t.timescale;
                out->duration = t.duration;
                out->data_start = t.first_chunk;
                *found = true;
            }
            return rc;
        }
        return parse_container(r, b->end, depth + 1, trak, out, found);
    }
    if (trak && !strcmp(b->type, "mdhd")) {
        uint8_t v[4];
        if (rd(r, v, 4)) return BP_M4A_IO;
        uint8_t f[28];
        size_t n = v[0] == 1 ? 28 : 16;   // version 1: 64 位时间字段
        if (rd(r, f, n)) return BP_M4A_IO;
        trak->timescale = be32(f + (v[0] == 1 ? 16 : 8));
        trak->duration = v[0] == 1 ? be64(f + 20) : be32(f + 12);
    } else if (trak && !strcmp(b->type, "stsd")) {
        return parse_stsd(r, b->end, trak);
    } else if (trak && (!strcmp(b->type, "stco") || !strcmp(b->type, "co64"))) {
        uint8_t h[8];
        if (rd(r, h, 8)) return BP_M4A_IO;   // FullBox + entry_count
        if (be32(h + 4) > 0) {
            uint8_t e[8];
            bool wide = b->type[0] == 'c';
            if (rd(r, e, wide ? 8 : 4)) return BP_M4A_IO;
            trak->first_chunk = wide ? be64(e) : be32(e);
        }
    }
    return seek_to(r, b->end);
}

static int parse_container(bp_reader_t *r, uint64_t end, int depth, trak_t *trak,
                           bp_m4a_info_t *out, bool *found) {
    if (depth > MAX_DEPTH) return BP_M4A_FORMAT;
    while (r->pos + 8 <= end) {
        box_t b;
        int rc = box_header(r, end, &b);
        if (rc) return rc;
        if ((rc = parse_box(r, &b, depth, trak, out, found))) return rc;
    }
    return seek_to(r, end);
}

int bp_m4a_parse(bp_reader_t *r, uint64_t file_size, bp_m4a_info_t *info) {
    memset(info, 0, sizeof(*info));
    uint64_t limit = file_size ? file_size : UINT64_MAX;
    bool moov = false, found = false, first = true;
    while (limit == UINT64_MAX || r->pos + 8 <= limit) {
        box_t b;
        int rc = box_header(r, limit, &b);
        if (rc) return rc;
        if (first && strcmp(b.type, "ftyp")) return BP_M4A_FORMAT;
        first = false;
        if (!strcmp(b.type, "moov")) {
            moov = true;
            if ((rc = parse_container(r, b.end, 1, NULL, info, &found))) return rc;
            if (!found) return BP_M4A_NO_AAC;
        } else if (!strcmp(b.type, "mdat")) {
            if (!moov) return BP_M4A_MOOV_LAST;
            uint64_t data = r->pos;
            info->data_end = b.end;
            // stco 首项给出首个数据块的绝对偏移;缺失或越界时从 mdat 数据区开头算。
            if (info->data_start < data || info->data_start >= b.end) info->data_start = data;
            return seek_to(r, info->data_start);
        } else if ((rc = seek_to(r, b.end))) {
            return rc;
        }
    }
    return moov ? BP_M4A_IO : BP_M4A_FORMAT;
}

uint32_t bp_m4a_duration_ms(const bp_m4a_info_t *info) {
    if (!info->timescale) return 0;
    uint64_t ms = info->duration * 1000 / info->timescale;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}
