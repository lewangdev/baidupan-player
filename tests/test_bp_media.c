// Host tests for the Baidu Netdisk player's pure media and path logic.
#include "bp_media.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static size_t make_wav(uint8_t *buf, uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bits,
                       bool list_chunk) {
    size_t pos = 0;
    memcpy(buf, "RIFF", 4); put32(buf + 4, 0); memcpy(buf + 8, "WAVE", 4);
    pos = 12;
    memcpy(buf + pos, "fmt ", 4); put32(buf + pos + 4, 16);
    put16(buf + pos + 8, tag); put16(buf + pos + 10, ch); put32(buf + pos + 12, rate);
    put32(buf + pos + 16, rate * ch * bits / 8); put16(buf + pos + 20, (uint16_t)(ch * bits / 8));
    put16(buf + pos + 22, bits);
    pos += 24;
    if (list_chunk) {
        memcpy(buf + pos, "LIST", 4); put32(buf + pos + 4, 5);   // odd size → pad byte
        memset(buf + pos + 8, 'x', 6);
        pos += 14;
    }
    memcpy(buf + pos, "data", 4); put32(buf + pos + 4, 4000);
    return pos + 8;
}

static void test_format(void) {
    assert(bp_media_format("a.mp3") == BP_FMT_MP3);
    assert(bp_media_format("周杰伦 - 晴天.MP3") == BP_FMT_MP3);
    assert(bp_media_format("x.Wav") == BP_FMT_WAV);
    assert(bp_media_format("x.flac") == BP_FMT_UNKNOWN);
    assert(bp_media_format("故事.M4A") == BP_FMT_M4A);
    assert(bp_media_format("x.aac") == BP_FMT_AAC);
    assert(bp_media_format(".mp3") == BP_FMT_UNKNOWN);
    assert(bp_media_format("mp3") == BP_FMT_UNKNOWN);
    assert(bp_media_format(NULL) == BP_FMT_UNKNOWN);
    assert(bp_media_listable(true, "照片"));
    assert(bp_media_listable(true, "a.jpg"));      // 文件夹名带扩展名也显示
    assert(bp_media_listable(false, "歌.MP3"));
    assert(bp_media_listable(false, "x.wav"));
    assert(!bp_media_listable(false, "封面.jpg"));
    assert(!bp_media_listable(false, "歌.flac"));
    assert(bp_media_listable(false, "故事.m4a"));
    assert(!bp_media_listable(false, "README"));
}

static void test_id3(void) {
    uint8_t h[10] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0x02, 0x01};
    assert(bp_id3v2_size(h, sizeof(h)) == 10 + 257);
    h[5] = 0x10;
    assert(bp_id3v2_size(h, sizeof(h)) == 20 + 257);
    h[6] = 0x80;
    assert(bp_id3v2_size(h, sizeof(h)) == 0);
    uint8_t big[10] = {'I', 'D', '3', 3, 0, 0, 0x00, 0x0f, 0x7f, 0x7f};
    assert(bp_id3v2_size(big, sizeof(big)) == 10 + 0x3ffff);
    uint8_t mp3[10] = {0xff, 0xfb, 0x90, 0x64};
    assert(bp_id3v2_size(mp3, sizeof(mp3)) == 0);
    assert(bp_id3v2_size(h, 9) == 0);
}

static void test_wav(void) {
    uint8_t buf[128];
    bp_wav_info_t info;
    size_t n = make_wav(buf, 1, 2, 44100, 16, false);
    assert(bp_wav_parse(buf, n, &info) == 0);
    assert(info.rate == 44100 && info.channels == 2 && info.bits == 16);
    assert(info.data_offset == 44 && info.data_bytes == 4000);
    for (size_t cut = 0; cut < n; cut++) assert(bp_wav_parse(buf, cut, &info) == 1 || cut >= 12);
    assert(bp_wav_parse(buf, 20, &info) == 1);

    n = make_wav(buf, 1, 1, 16000, 16, true);
    assert(bp_wav_parse(buf, n, &info) == 0);
    assert(info.data_offset == 12 + 24 + 14 + 8);

    n = make_wav(buf, 3, 2, 44100, 32, false);   // IEEE float
    assert(bp_wav_parse(buf, n, &info) == -2);
    n = make_wav(buf, 1, 2, 44100, 24, false);
    assert(bp_wav_parse(buf, n, &info) == -2);
    n = make_wav(buf, 1, 6, 44100, 16, false);
    assert(bp_wav_parse(buf, n, &info) == -2);
    memcpy(buf, "RIFX", 4);
    assert(bp_wav_parse(buf, n, &info) == -1);
}

static void test_time_and_size(void) {
    char s[24];
    bp_format_time(0, s, sizeof(s)); assert(!strcmp(s, "0:00"));
    bp_format_time(61999, s, sizeof(s)); assert(!strcmp(s, "1:01"));
    bp_format_time(3723000, s, sizeof(s)); assert(!strcmp(s, "1:02:03"));
    bp_format_size(512, s, sizeof(s)); assert(!strcmp(s, "512 B"));
    bp_format_size(1536, s, sizeof(s)); assert(!strcmp(s, "1.5 KB"));
    bp_format_size(5 * 1024 * 1024 + 300 * 1024, s, sizeof(s)); assert(!strcmp(s, "5.2 MB"));
    bp_format_size(3ULL << 30, s, sizeof(s)); assert(!strcmp(s, "3.0 GB"));
    assert(bp_estimate_ms(16000, 128000) == 1000);
    assert(bp_estimate_ms(4000000, 320000) == 100000);
    assert(bp_estimate_ms(100, 0) == 0);
    assert(bp_pcm_ms(44100, 44100) == 1000);
    assert(bp_pcm_ms(1, 0) == 0);
}

static void test_paths(void) {
    char out[BP_PATH_MAX];
    assert(bp_path_child("/", "音乐", out, sizeof(out)) && !strcmp(out, "/音乐"));
    assert(bp_path_child("/音乐", "周杰伦", out, sizeof(out)) && !strcmp(out, "/音乐/周杰伦"));
    assert(!bp_path_child("/a", "..", out, sizeof(out)) && !out[0]);
    assert(!bp_path_child("/a", "b/c", out, sizeof(out)));
    assert(!bp_path_child("a", "b", out, sizeof(out)));
    assert(!bp_path_child("/a/", "b", out, sizeof(out)));
    assert(!bp_path_child("/a", "b", out, 4));
    assert(bp_path_parent("/音乐/周杰伦", out, sizeof(out)) && !strcmp(out, "/音乐"));
    assert(bp_path_parent("/音乐", out, sizeof(out)) && !strcmp(out, "/"));
    assert(!bp_path_parent("/", out, sizeof(out)));
    assert(!bp_path_parent("//x", out, sizeof(out)));
    assert(!strcmp(bp_path_basename("/a/歌.mp3"), "歌.mp3"));
    assert(!strcmp(bp_path_basename("/"), "/"));
}

static void test_misc(void) {
    int16_t pcm[6] = {100, 300, -200, -400, 32767, 32767};
    assert(bp_downmix_s16(pcm, 3) == 3);
    assert(pcm[0] == 200 && pcm[1] == -300 && pcm[2] == 32767);
    assert(bp_volume_step(95, 10) == 100);
    assert(bp_volume_step(5, -10) == 0);
    assert(bp_volume_step(50, 10) == 60);
    assert(bp_playlist_move(0, 3, 1, false) == 1);
    assert(bp_playlist_move(2, 3, 1, false) == -1);
    assert(bp_playlist_move(2, 3, 1, true) == 0);
    assert(bp_playlist_move(0, 3, -1, true) == 2);
    assert(bp_playlist_move(0, 0, 1, true) == -1);
    assert(bp_brightness_percent(1) == 10 && bp_brightness_percent(5) == 100);
    for (int l = 1; l < BP_BRIGHTNESS_LEVELS; l++)
        assert(bp_brightness_percent(l) < bp_brightness_percent(l + 1));   // 单调递增
    assert(bp_brightness_percent(0) == 10 && bp_brightness_percent(9) == 100);
    assert(bp_brightness_step(5, 1) == 5 && bp_brightness_step(1, -1) == 1);
    assert(bp_brightness_step(3, 1) == 4 && bp_brightness_step(3, -1) == 2);
}

// 128 kbps / 44.1 kHz / 立体声 MPEG-1 Layer III 帧头,无填充:帧长 417。
static const uint8_t MP3_HDR[4] = {0xff, 0xfb, 0x90, 0x04};

static void test_sniff_and_headers(void) {
    uint8_t m4a[16] = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'M', '4', 'A', ' '};
    assert(bp_media_sniff(m4a, sizeof(m4a)) == BP_SNIFF_MP4);
    assert(bp_media_sniff((const uint8_t *)"ID3\x04\0\0\0\0\0\0", 10) == BP_SNIFF_MP3);
    assert(bp_media_sniff((const uint8_t *)"RIFF....WAVE", 12) == BP_SNIFF_WAV);
    assert(bp_media_sniff((const uint8_t *)"fLaC\0\0", 6) == BP_SNIFF_FLAC);
    assert(bp_media_sniff((const uint8_t *)"OggS\0\0", 6) == BP_SNIFF_OGG);
    uint8_t adts[7] = {0xff, 0xf1, 0x50, 0x80, 0x02, 0x1f, 0xfc};
    assert(bp_media_sniff(adts, sizeof(adts)) == BP_SNIFF_AAC);
    assert(bp_media_sniff(MP3_HDR, 4) == BP_SNIFF_MP3);
    assert(bp_media_sniff((const uint8_t *)"\0\0\0\0", 4) == BP_SNIFF_UNKNOWN);

    bp_mp3_hdr_t h;
    assert(bp_mp3_parse_header(MP3_HDR, &h));
    assert(h.version == 1 && h.samplerate == 44100 && h.bitrate == 128000 && h.channels == 2);
    assert(h.frame_len == 417);
    uint8_t v2[4] = {0xff, 0xf3, 0x80, 0xc4};   // MPEG-2, 64 kbps, 22.05 kHz, 单声道
    assert(bp_mp3_parse_header(v2, &h) && h.version == 2 && h.samplerate == 22050 &&
           h.bitrate == 64000 && h.channels == 1 && h.frame_len == 72 * 64000 / 22050);
    uint8_t bad[4] = {0xff, 0xfb, 0xf0, 0x04};  // 码率索引 15
    assert(!bp_mp3_parse_header(bad, &h));
    uint8_t l2[4] = {0xff, 0xfd, 0x90, 0x04};   // Layer II
    assert(!bp_mp3_parse_header(l2, &h));
}

static void test_find_frame(void) {
    static uint8_t buf[2000];
    memset(buf, 0x11, sizeof(buf));
    // 伪同步:位置 10 处像帧头,但 417 字节后不是帧头(M4A 数据里的典型误判)。
    memcpy(buf + 10, MP3_HDR, 4);
    // 真帧:位置 500 与 917 连续两帧。
    memcpy(buf + 500, MP3_HDR, 4);
    memcpy(buf + 917, MP3_HDR, 4);
    bp_mp3_hdr_t h;
    size_t partial = 0;
    assert(bp_mp3_find_frame(buf, sizeof(buf), false, NULL, &h, &partial) == 500);
    // 采样率不符的候选被跳过。
    bp_mp3_hdr_t want = {.version = 1, .samplerate = 48000};
    assert(bp_mp3_find_frame(buf, sizeof(buf), false, &want, &h, &partial) == -1);
    // 下一帧还没读到:需要更多数据;流已结束则接受。
    assert(bp_mp3_find_frame(buf + 500, 300, false, NULL, &h, &partial) == -2 && partial == 0);
    assert(bp_mp3_find_frame(buf + 500, 300, true, NULL, &h, &partial) == 0);
    memset(buf, 0, sizeof(buf));
    assert(bp_mp3_find_frame(buf, sizeof(buf), false, NULL, &h, &partial) == -1);
}

static void test_shorten(void) {
    char out[32];
    bp_name_shorten("短名.mp3", out, sizeof(out));
    assert(!strcmp(out, "短名.mp3"));
    // 中文 3 字节/字:截断时保留 .mp3,且不切断字符。
    const char *longname = "【钱儿爸】一百万只猫美国史上第一本真正的绘本.mp3";
    bp_name_shorten(longname, out, sizeof(out));
    assert(strlen(out) < sizeof(out));
    assert(!strcmp(out + strlen(out) - 4, ".mp3"));
    assert(strstr(out, "\xe2\x80\xa6"));
    for (size_t i = 0; out[i]; ) {           // 合法 UTF-8 序列
        unsigned char c = (unsigned char)out[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        for (size_t k = 1; k < n; k++) assert(((unsigned char)out[i + k] & 0xc0) == 0x80);
        i += n;
    }
    char tiny[6];
    bp_name_shorten("abcdefghij.mp3", tiny, sizeof(tiny));
    assert(!strcmp(tiny, "abcde"));
}

int main(void) {
    test_sniff_and_headers();
    test_find_frame();
    test_shorten();
    test_format();
    test_id3();
    test_wav();
    test_time_and_size();
    test_paths();
    test_misc();
    puts("test_bp_media: PASS");
    return 0;
}
