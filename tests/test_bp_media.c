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
    assert(bp_media_format(".mp3") == BP_FMT_UNKNOWN);
    assert(bp_media_format("mp3") == BP_FMT_UNKNOWN);
    assert(bp_media_format(NULL) == BP_FMT_UNKNOWN);
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
}

int main(void) {
    test_format();
    test_id3();
    test_wav();
    test_time_and_size();
    test_paths();
    test_misc();
    puts("test_bp_media: PASS");
    return 0;
}
