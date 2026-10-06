// Host test: stream-parse an M4A header, then decode its mdat as consecutive raw AAC
// blocks with Helix (no per-frame size table), exactly as the firmware does.
#include "aacdec.h"
#include "bp_mp4.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t len, pos;
} mem_t;

static int mem_read(void *ctx, uint8_t *dst, size_t n) {
    mem_t *m = ctx;
    if (m->pos + n > m->len) return -1;
    memcpy(dst, m->data + m->pos, n);
    m->pos += n;
    return 0;
}

static int mem_skip(void *ctx, uint64_t n) {
    mem_t *m = ctx;
    if (m->pos + n > m->len) return -1;
    m->pos += (size_t)n;
    return 0;
}

static uint8_t *load(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*len);
    assert(buf && fread(buf, 1, *len, f) == *len);
    fclose(f);
    return buf;
}

static long long energy(const short *pcm, int n) {
    long long e = 0;
    for (int i = 0; i < n; i++) e += (long long)pcm[i] * pcm[i] / 1024;
    return e;
}

static void test_m4a_stream_decode(const char *path) {
    size_t len;
    uint8_t *file = load(path, &len);
    mem_t m = {file, len, 0};
    bp_reader_t r = {mem_read, mem_skip, &m, 0};
    bp_m4a_info_t info;
    assert(bp_m4a_parse(&r, len, &info) == BP_M4A_OK);
    assert(r.pos == info.data_start && m.pos == info.data_start);
    assert(info.object_type == 2 && info.samplerate == 22050 && info.channels == 2);
    assert(info.data_end == len);
    uint32_t ms = bp_m4a_duration_ms(&info);
    assert(ms >= 1000 && ms <= 1200);  // 1 秒测试音 + 编码器的前置帧

    HAACDecoder dec = AACInitDecoder();
    assert(dec);
    AACFrameInfo fi = {.nChans = info.channels, .sampRateCore = (int)info.samplerate,
                       .profile = AAC_PROFILE_LC};
    assert(AACSetRawBlockParams(dec, 0, &fi) == 0);
    static short pcm[AAC_MAX_NCHANS * AAC_MAX_NSAMPS];
    unsigned char *ptr = file + info.data_start;
    int left = (int)(info.data_end - info.data_start);
    int frames = 0, samples = 0;
    long long e = 0;
    while (left > 0) {
        int err = AACDecode(dec, &ptr, &left, pcm);
        assert(err == 0);
        AACGetLastFrameInfo(dec, &fi);
        assert(fi.nChans == 2 && fi.sampRateOut == 22050 && fi.outputSamps == 2048);
        frames++;
        samples += fi.outputSamps / fi.nChans;
        e += energy(pcm, fi.outputSamps);
    }
    assert(left == 0);                 // 每个裸数据块都被精确消费,无残留
    assert(frames >= 21 && frames <= 24);
    assert(samples >= 22050);
    assert(e > 0);                     // 解出的是有声内容而非静音
    AACFreeDecoder(dec);
    free(file);
}

static void test_adts_decode(const char *path) {
    size_t len;
    uint8_t *file = load(path, &len);
    HAACDecoder dec = AACInitDecoder();
    static short pcm[AAC_MAX_NCHANS * AAC_MAX_NSAMPS];
    unsigned char *ptr = file;
    int left = (int)len, frames = 0;
    while (left > 0) {
        int sync = AACFindSyncWord(ptr, left);
        if (sync < 0) break;
        ptr += sync;
        left -= sync;
        int err = AACDecode(dec, &ptr, &left, pcm);
        if (err) break;
        frames++;
    }
    assert(frames >= 21);
    AACFreeDecoder(dec);
    free(file);
}

// mdat 在 moov 之前:无法流式播放,必须明确报错。
static void test_moov_last(const char *path) {
    size_t len;
    uint8_t *file = load(path, &len);
    uint8_t *re = malloc(len);
    // fixture 布局:ftyp(28) moov(934) free(3126) mdat(余下)。重排为 ftyp mdat moov。
    size_t ftyp = 28, moov = 934, free_sz = 3126, mdat = len - ftyp - moov - free_sz;
    memcpy(re, file, ftyp);
    memcpy(re + ftyp, file + ftyp + moov + free_sz, mdat);
    memcpy(re + ftyp + mdat, file + ftyp, moov);
    mem_t m = {re, ftyp + mdat + moov, 0};
    bp_reader_t r = {mem_read, mem_skip, &m, 0};
    bp_m4a_info_t info;
    assert(bp_m4a_parse(&r, m.len, &info) == BP_M4A_MOOV_LAST);
    // 非 MP4 数据
    uint8_t junk[64] = {0xff, 0xfb, 0x90};
    mem_t j = {junk, sizeof(junk), 0};
    bp_reader_t rj = {mem_read, mem_skip, &j, 0};
    assert(bp_m4a_parse(&rj, sizeof(junk), &info) == BP_M4A_FORMAT);
    // 截断的文件
    mem_t t = {file, 500, 0};
    bp_reader_t rt = {mem_read, mem_skip, &t, 0};
    assert(bp_m4a_parse(&rt, 0, &info) == BP_M4A_IO);
    free(re);
    free(file);
}

static void test_asc(void) {
    bp_m4a_info_t i;
    const uint8_t lc_44k_stereo[2] = {0x12, 0x10};      // AOT 2, 44.1 kHz, 2 ch
    assert(bp_aac_parse_asc(lc_44k_stereo, 2, &i) && i.samplerate == 44100 && i.channels == 2 && !i.sbr);
    const uint8_t lc_22k_mono[2] = {0x13, 0x88};        // AOT 2, 22.05 kHz, 1 ch
    assert(bp_aac_parse_asc(lc_22k_mono, 2, &i) && i.samplerate == 22050 && i.channels == 1);
    // 显式 HE-AAC:AOT 5, 核心 22.05 kHz, 扩展 44.1 kHz, 核心类型 LC
    const uint8_t he[4] = {0x2b, 0x92, 0x08, 0x00};
    assert(bp_aac_parse_asc(he, 4, &i) && i.sbr && i.samplerate == 22050 && i.object_type == 2);
    const uint8_t main_profile[2] = {0x0a, 0x10};       // AOT 1 (Main) 不支持
    assert(!bp_aac_parse_asc(main_profile, 2, &i));
    assert(!bp_aac_parse_asc(lc_44k_stereo, 1, &i));    // 截断
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/fixtures";
    char m4a[512], aac[512];
    snprintf(m4a, sizeof(m4a), "%s/tone_lc.m4a", dir);
    snprintf(aac, sizeof(aac), "%s/tone_lc.aac", dir);
    test_asc();
    test_m4a_stream_decode(m4a);
    test_adts_decode(aac);
    test_moov_last(m4a);
    puts("test_bp_mp4: PASS");
    return 0;
}
