// main/bp_player.c —— 流式播放引擎。
//
// 资源归属(每首歌一条流水线,同一时刻只有一条):
//   ctrl 任务(常驻)  串行处理播放/切歌/停止命令,获取下载直链,启动/回收流水线
//   fetch 任务       HTTPS 下载 → StreamBuffer;跟随 302,断线按 Range 续传
//   decode 任务      StreamBuffer → MP3(Helix)/WAV 解码 → 单声道 PCM → I2S
// decode 任务是流水线的持有者:它启动 fetch、等待 fetch 退出、释放缓冲,最后把
// 结束/失败事件(带代号)投回 ctrl,ctrl 忽略过期代号的事件。
// 按键回调只投递命令或改暂停标志,不做任何网络/音频操作。
#include "bp_app.h"

#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mp3dec.h"
#include "nvs.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG "bp_play"

#define CTRL_STACK 7168
#define FETCH_STACK 7168
#define DECODE_STACK 6144
#define FETCH_CHUNK 2048
#define FETCH_MAX_FAILURES 6
#define IN_BUF_SIZE 4096
#define PCM_MAX_SAMPLES (1152 * 2)
// 环形缓冲与解码缓冲静态分配:若从堆里取,会切走唯一一块能容纳 TLS 16.7 KB
// 记录缓冲的连续内存(实测 CDN 读数据时 alloc 失败)。20 KB ≈ 128 kbps 下 1.25 秒。
#define STREAM_SIZE (20 * 1024)
#define MAX_DECODE_ERRORS 300

typedef enum {
    CMD_PLAY,
    CMD_NEXT,
    CMD_PREV,
    CMD_STOP,
    CMD_TRACK_END,
    CMD_TRACK_FAIL,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint32_t gen;
    const char *error;
} cmd_t;

typedef struct {
    char *url;
    uint64_t size;
    uint32_t gen;
} track_t;

static QueueHandle_t s_cmd;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// 播放列表(只含可播放文件)与其来源,用于末尾自动加载下一页。
static bp_file_t s_playlist[BP_PLAYLIST_MAX];
static int s_pl_count;
static int s_pl_index;
static bp_list_req_t s_pl_req;
static bool s_pl_has_more;
static bp_file_t s_pending_list[BP_PLAYLIST_MAX];   // CMD_PLAY 携带的数据
static int s_pending_count, s_pending_index;
static bp_list_req_t s_pending_req;
static bool s_pending_has_more;

static bp_player_info_t s_info;
static volatile bool s_paused;
static volatile bool s_abort;
static volatile bool s_pipeline_running;
static volatile bool s_fetch_running;
static volatile bool s_fetch_eof;
static volatile bool s_fetch_failed;
static uint32_t s_gen;
static StreamBufferHandle_t s_stream;
static const size_t s_stream_size = STREAM_SIZE;
static StaticStreamBuffer_t s_stream_struct;
static uint8_t s_stream_storage[STREAM_SIZE + 1];
static uint8_t s_in_storage[IN_BUF_SIZE] __attribute__((aligned(4)));
static int16_t s_pcm_storage[PCM_MAX_SAMPLES];

static void log_heap(const char *stage) {
    ESP_LOGI(TAG, "MEM %s: free=%u largest=%u min=%u", stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)esp_get_minimum_free_heap_size());
}

static void set_state(bp_play_state_t st, const char *error) {
    portENTER_CRITICAL(&s_mux);
    s_info.state = st;
    s_info.error = error;
    portEXIT_CRITICAL(&s_mux);
}

static void post(cmd_type_t type, uint32_t gen, const char *error) {
    cmd_t c = {.type = type, .gen = gen, .error = error};
    if (s_cmd) xQueueSend(s_cmd, &c, pdMS_TO_TICKS(100));
}

// ---- fetch 任务 -----------------------------------------------------------------
typedef struct {
    char *location;
} fetch_ctx_t;

static esp_err_t fetch_event(esp_http_client_event_t *evt) {
    fetch_ctx_t *ctx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && ctx && evt->header_key &&
        evt->header_value && !strcasecmp(evt->header_key, "Location") &&
        strlen(evt->header_value) < 4096) {
        free(ctx->location);
        ctx->location = strdup(evt->header_value);
    }
    return ESP_OK;
}

static bool stream_send_all(const uint8_t *data, size_t len) {
    size_t off = 0;
    while (off < len && !s_abort) {
        off += xStreamBufferSend(s_stream, data + off, len - off, pdMS_TO_TICKS(100));
    }
    return off == len;
}

// 返回 1=完整下载结束,0=被中止,-1=可重试错误,-2=不可重试(4xx 等)。
static int fetch_once(const char *start_url, uint64_t *offset, uint8_t *buf) {
    char *url = strdup(start_url);
    int result = -1;
    for (int hop = 0; url && hop < 5 && !s_abort; hop++) {
        fetch_ctx_t ctx = {0};
        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .method = HTTP_METHOD_GET,
            .timeout_ms = 10000,
            .disable_auto_redirect = true,
            .event_handler = fetch_event,
            .user_data = &ctx,
            .buffer_size = 2048,
            .buffer_size_tx = 2048,   // dlink 带签名参数,请求行较长
            .keep_alive_enable = true,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) break;
        esp_http_client_set_header(c, "User-Agent", "pan.baidu.com");
        char range[40];
        if (*offset) {
            snprintf(range, sizeof(range), "bytes=%" PRIu64 "-", *offset);
            esp_http_client_set_header(c, "Range", range);
        }
        esp_err_t err = esp_http_client_open(c, 0);
        int status = 0;
        if (err == ESP_OK) {
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
        }
        if (err == ESP_OK && status >= 300 && status < 400 && ctx.location &&
            (!strncmp(ctx.location, "https://", 8) || !strncmp(ctx.location, "http://", 7))) {
            free(url);
            url = ctx.location;
            ctx.location = NULL;
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            continue;
        }
        free(ctx.location);
        if (err == ESP_OK && (status == 200 || status == 206)) {
            log_heap(hop ? "cdn-connected" : "connected");
            // 服务器忽略 Range 时从头读,丢弃已送出的部分。
            uint64_t skip = (status == 200) ? *offset : 0;
            int n;
            while (!s_abort && (n = esp_http_client_read(c, (char *)buf, FETCH_CHUNK)) > 0) {
                size_t start = 0;
                if (skip) {
                    size_t drop = skip > (uint64_t)n ? (size_t)n : (size_t)skip;
                    skip -= drop;
                    start = drop;
                }
                if (start < (size_t)n && !stream_send_all(buf + start, n - start)) break;
                *offset += n - start;
            }
            if (s_abort) result = 0;
            else if (esp_http_client_is_complete_data_received(c)) result = 1;
            else result = -1;
        } else if (err == ESP_OK && status == 416) {
            result = 1;   // 续传偏移已到文件尾
        } else if (err == ESP_OK && status >= 400 && status < 500) {
            ESP_LOGW(TAG, "download http=%d", status);
            result = -2;
        } else {
            ESP_LOGW(TAG, "download open=%s http=%d", esp_err_to_name(err), status);
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        break;
    }
    free(url);
    return result;
}

static void fetch_task(void *arg) {
    track_t *t = arg;
    uint8_t *buf = malloc(FETCH_CHUNK);
    uint64_t offset = 0;
    int failures = 0, rc = -1;
    while (buf && !s_abort) {
        uint64_t before = offset;
        rc = fetch_once(t->url, &offset, buf);
        if (rc >= 0 || rc == -2) break;
        if (offset > before) failures = 0;   // 有进展就重新计数(如暂停后被服务器断开)
        if (++failures >= FETCH_MAX_FAILURES) break;
        ESP_LOGW(TAG, "download interrupted at %" PRIu64 ", retry %d", offset, failures);
        for (int i = 0; i < failures * 10 && !s_abort; i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    free(buf);
    s_fetch_failed = rc < 0 && !s_abort;
    s_fetch_eof = true;
    s_fetch_running = false;
    vTaskDelete(NULL);
}

// ---- decode 任务 ----------------------------------------------------------------
typedef struct {
    uint8_t *in;
    size_t len;      // in 中有效字节(从 in[0] 开始)
    size_t pos;      // 已消费字节
} inbuf_t;

static void update_buffer_pct(void) {
    size_t avail = xStreamBufferBytesAvailable(s_stream);
    portENTER_CRITICAL(&s_mux);
    s_info.buffer_pct = (uint8_t)(avail * 100 / s_stream_size);
    portEXIT_CRITICAL(&s_mux);
}

// 等待缓冲到目标比例(或下载结束);期间状态为“缓冲中”。返回 false 表示中止。
static bool wait_buffered(int pct) {
    size_t want = s_stream_size * pct / 100;
    bool announced = false;
    while (!s_abort && !s_fetch_eof && xStreamBufferBytesAvailable(s_stream) < want) {
        if (!announced && !s_paused) {
            set_state(BP_PLAY_BUFFERING, NULL);
            announced = true;
        }
        update_buffer_pct();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return !s_abort;
}

static bool stream_done(void) {
    return s_fetch_eof && xStreamBufferIsEmpty(s_stream);
}

// 填充输入缓冲到满或流暂时为空(欠载时会先重新缓冲)。返回新增字节数;
// 返回 0 且 stream_done() 为真表示数据已全部读完。
static size_t refill(inbuf_t *b) {
    if (b->pos) {
        memmove(b->in, b->in + b->pos, b->len - b->pos);
        b->len -= b->pos;
        b->pos = 0;
    }
    size_t added = 0;
    while (b->len < IN_BUF_SIZE && !s_abort) {
        size_t got = xStreamBufferReceive(s_stream, b->in + b->len, IN_BUF_SIZE - b->len,
                                          pdMS_TO_TICKS(20));
        b->len += got;
        added += got;
        if (got) continue;
        if (stream_done() || b->len >= IN_BUF_SIZE / 2) break;
        // 欠载:重新缓冲 25% 再继续,避免断断续续。
        if (!wait_buffered(25)) break;
    }
    return added;
}

// 丢弃 n 字节(用于跳过 ID3 标签)。
static bool discard(inbuf_t *b, size_t n) {
    while (n && !s_abort) {
        size_t have = b->len - b->pos;
        if (!have) {
            if (!refill(b) && stream_done()) return false;
            continue;
        }
        size_t d = have < n ? have : n;
        b->pos += d;
        n -= d;
    }
    return !s_abort;
}

static void wait_while_paused(void) {
    if (!s_paused) return;
    set_state(BP_PLAY_PAUSED, NULL);
    while (s_paused && !s_abort) {
        update_buffer_pct();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_abort) set_state(BP_PLAY_PLAYING, NULL);
}

static bool open_codec(uint32_t rate) {
    if (bsp_audio_set_format(rate, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed at %lu Hz", (unsigned long)rate);
        return false;
    }
    bsp_audio_set_volume(g_bp.volume);
    return true;
}

static void publish_progress(uint64_t frames, uint32_t rate, uint32_t total_ms) {
    portENTER_CRITICAL(&s_mux);
    if (s_info.state == BP_PLAY_BUFFERING) s_info.state = BP_PLAY_PLAYING;
    s_info.pos_ms = bp_pcm_ms(frames, rate);
    if (total_ms) s_info.total_ms = total_ms;
    portEXIT_CRITICAL(&s_mux);
}

static const char *play_mp3(inbuf_t *b, uint64_t file_size, size_t id3) {
    HMP3Decoder dec = MP3InitDecoder();
    log_heap("decoder-ready");
    int16_t *pcm = s_pcm_storage;
    const char *error = NULL;
    if (!dec) {
        error = "内存不足";
        goto out;
    }
    uint32_t rate = 0;
    uint64_t frames_out = 0, bitrate_sum = 0, bitrate_frames = 0;
    int errors = 0;
    while (!s_abort) {
        wait_while_paused();
        if (s_abort) break;
        if (b->len - b->pos < MAINBUF_SIZE && !stream_done()) refill(b);
        int left = (int)(b->len - b->pos);
        if (left <= 0) {
            if (stream_done()) break;
            continue;
        }
        int sync = MP3FindSyncWord(b->in + b->pos, left);
        if (sync < 0) {
            // 保留末尾 3 字节,同步字可能跨越缓冲边界。
            b->pos = left > 3 ? b->len - 3 : b->len;
            if (++errors > MAX_DECODE_ERRORS) { error = "不是有效的 MP3"; break; }
            if (!refill(b) && stream_done()) break;
            continue;
        }
        b->pos += sync;
        left -= sync;
        unsigned char *ptr = b->in + b->pos;
        int err = MP3Decode(dec, &ptr, &left, pcm, 0);
        b->pos = (size_t)(ptr - b->in);
        if (err == ERR_MP3_INDATA_UNDERFLOW) {
            if (!refill(b) && stream_done()) break;   // 末尾残缺帧
            continue;
        }
        if (err == ERR_MP3_MAINDATA_UNDERFLOW) continue;   // 比特池未满,正常
        if (err) {
            if (b->pos < b->len) b->pos++;
            if (++errors > MAX_DECODE_ERRORS) { error = "解码失败"; break; }
            continue;
        }
        errors = 0;
        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        if (fi.nChans < 1 || fi.nChans > 2 || fi.samprate <= 0) continue;
        if ((uint32_t)fi.samprate != rate) {
            if (!open_codec((uint32_t)fi.samprate)) { error = "音频设备异常"; break; }
            rate = (uint32_t)fi.samprate;
            portENTER_CRITICAL(&s_mux);
            s_info.rate = rate;
            s_info.channels = (uint16_t)fi.nChans;
            portEXIT_CRITICAL(&s_mux);
        }
        size_t frames = (size_t)fi.outputSamps / (size_t)fi.nChans;
        if (fi.nChans == 2) bp_downmix_s16(pcm, frames);
        if (bsp_audio_write(pcm, frames * sizeof(int16_t)) != ESP_OK) {
            error = "音频设备异常";
            break;
        }
        frames_out += frames;
        uint32_t total = 0;
        if (fi.bitrate > 0) {
            bitrate_sum += (uint64_t)fi.bitrate;
            bitrate_frames++;
            uint32_t avg = (uint32_t)(bitrate_sum / bitrate_frames);
            if (file_size > id3) total = bp_estimate_ms(file_size - id3, avg);
            portENTER_CRITICAL(&s_mux);
            s_info.kbps = avg / 1000;
            portEXIT_CRITICAL(&s_mux);
        }
        publish_progress(frames_out, rate, total);
        if ((frames_out & 0x3fff) < frames) update_buffer_pct();
    }
    if (!error && !s_abort && frames_out == 0) error = "不是有效的 MP3";
out:
    if (dec) MP3FreeDecoder(dec);
    return error;
}

static const char *play_wav(inbuf_t *b, uint64_t file_size) {
    bp_wav_info_t wav;
    int rc;
    while ((rc = bp_wav_parse(b->in, b->len, &wav)) == 1) {
        if (b->len >= IN_BUF_SIZE) return "WAV 头过大";
        if (!refill(b) && stream_done()) return "WAV 文件不完整";
    }
    if (rc != 0) return rc == -2 ? "仅支持 16 位 PCM WAV" : "不是有效的 WAV";
    if (!open_codec(wav.rate)) return "音频设备异常";
    uint32_t align = (uint32_t)wav.channels * 2;
    // 流式录制的 WAV 常把 data 长度写成 0 或 0xFFFFFFFF,以文件大小为准。
    uint64_t avail = file_size > wav.data_offset ? file_size - wav.data_offset : 0;
    uint64_t data_bytes = wav.data_bytes;
    if (!data_bytes || data_bytes == 0xffffffffu || (avail && data_bytes > avail))
        data_bytes = avail ? avail : UINT64_MAX;
    uint32_t total_ms = data_bytes == UINT64_MAX ? 0 : bp_pcm_ms(data_bytes / align, wav.rate);
    portENTER_CRITICAL(&s_mux);
    s_info.rate = wav.rate;
    s_info.channels = wav.channels;
    s_info.kbps = wav.rate * align * 8 / 1000;
    s_info.total_ms = total_ms;
    portEXIT_CRITICAL(&s_mux);
    set_state(BP_PLAY_PLAYING, NULL);
    b->pos = wav.data_offset;
    uint64_t played = 0, frames_out = 0;
    while (!s_abort && played < data_bytes) {
        wait_while_paused();
        size_t have = b->len - b->pos;
        if (have < align) {
            if (!refill(b) && stream_done()) break;
            continue;
        }
        size_t usable = have - have % align;
        if (usable > data_bytes - played) usable = (size_t)(data_bytes - played);
        int16_t *pcm = (int16_t *)(b->in + b->pos);
        size_t frames = usable / align;
        // in 缓冲起点 4 字节对齐,pos 按 align 递进;data_offset 为偶数,满足 int16 对齐。
        if (wav.channels == 2) bp_downmix_s16(pcm, frames);
        if (bsp_audio_write(pcm, frames * sizeof(int16_t)) != ESP_OK) return "音频设备异常";
        b->pos += usable;
        played += usable;
        frames_out += frames;
        publish_progress(frames_out, wav.rate, 0);
        if ((frames_out & 0x3fff) < frames) update_buffer_pct();
    }
    return NULL;
}

static void decode_task(void *arg) {
    track_t *t = arg;
    const char *error = NULL;
    inbuf_t b = {.in = s_in_storage};   // 4 字节对齐(WAV 按 int16 读)
    s_fetch_eof = s_fetch_failed = false;
    s_fetch_running = true;
    if (xTaskCreate(fetch_task, "bp_fetch", FETCH_STACK, t, 5, NULL) != pdPASS) {
        s_fetch_running = false;
        error = "内存不足";
        goto done;
    }
    if (!wait_buffered(50)) goto done;
    refill(&b);
    if (b.len < 4) {
        error = s_fetch_failed ? "网络错误" : "文件为空";
        goto done;
    }
    if (!memcmp(b.in, "RIFF", 4)) {
        error = play_wav(&b, t->size);
    } else {
        size_t id3 = bp_id3v2_size(b.in, b.len);
        if (id3 && !discard(&b, id3)) goto done;
        if (!s_abort) error = play_mp3(&b, t->size, id3);
    }
    if (!error && s_fetch_failed && !s_abort) error = "网络中断";
done:
    s_abort = true;   // 通知 fetch 退出(若尚在运行)
    while (s_fetch_running) vTaskDelay(pdMS_TO_TICKS(20));
    log_heap("pipeline-end");
    uint32_t gen = t->gen;
    bool aborted_by_user;
    portENTER_CRITICAL(&s_mux);
    aborted_by_user = s_gen != gen;
    portEXIT_CRITICAL(&s_mux);
    free(t->url);
    free(t);
    s_pipeline_running = false;
    if (!aborted_by_user) post(error ? CMD_TRACK_FAIL : CMD_TRACK_END, gen, error);
    vTaskDelete(NULL);
}

// ---- ctrl 任务 ------------------------------------------------------------------
static void stop_pipeline(void) {
    if (!s_pipeline_running) return;
    portENTER_CRITICAL(&s_mux);
    s_gen++;              // 使流水线结束事件过期
    portEXIT_CRITICAL(&s_mux);
    s_abort = true;
    while (s_pipeline_running) vTaskDelay(pdMS_TO_TICKS(20));
}

static void start_track(int index) {
    stop_pipeline();
    if (index < 0 || index >= s_pl_count) {
        set_state(BP_PLAY_IDLE, NULL);
        return;
    }
    const bp_file_t *f = &s_playlist[index];
    uint32_t gen;
    portENTER_CRITICAL(&s_mux);
    gen = ++s_gen;
    s_pl_index = index;
    memset(&s_info, 0, sizeof(s_info));
    strlcpy(s_info.name, f->name, sizeof(s_info.name));
    s_info.index = index;
    s_info.count = s_pl_count;
    s_info.state = BP_PLAY_RESOLVING;
    portEXIT_CRITICAL(&s_mux);
    s_paused = false;

    char *url = NULL;
    log_heap("before-dlink");
    if (bp_baidu_dlink(f->fs_id, &url) != 0) {
        set_state(BP_PLAY_ERROR, g_bp.wifi_up ? "获取下载地址失败" : "无网络");
        post(CMD_TRACK_FAIL, gen, NULL);   // 让 ctrl 统一处理跳过逻辑
        return;
    }
    track_t *t = calloc(1, sizeof(track_t));
    xStreamBufferReset(s_stream);   // 上一条流水线已退出,缓冲可安全清空
    if (!t) {
        free(url);
        set_state(BP_PLAY_ERROR, "内存不足");
        post(CMD_TRACK_FAIL, gen, NULL);
        return;
    }
    t->url = url;
    t->size = f->size;
    t->gen = gen;
    s_abort = false;
    s_pipeline_running = true;
    ESP_LOGI(TAG, "play #%d gen=%lu stream=%uKB free=%u largest=%u", index,
             (unsigned long)gen, (unsigned)(s_stream_size / 1024),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    esp_wifi_set_ps(WIFI_PS_NONE);
    set_state(BP_PLAY_BUFFERING, NULL);
    if (xTaskCreate(decode_task, "bp_decode", DECODE_STACK, t, 6, NULL) != pdPASS) {
        s_pipeline_running = false;
        free(t->url);
        free(t);
        set_state(BP_PLAY_ERROR, "内存不足");
        post(CMD_TRACK_FAIL, gen, NULL);
    }
}

// 列表播完:有下一页就加载并继续,否则停止。返回 true 表示已开始新曲目。
static bool continue_next_page(void) {
    if (!s_pl_has_more) return false;
    bp_list_t *page = malloc(sizeof(bp_list_t));
    if (!page) return false;
    bp_list_req_t req = s_pl_req;
    req.page++;
    bool started = false;
    set_state(BP_PLAY_RESOLVING, NULL);
    if (bp_baidu_list_fetch(&req, page) == 0) {
        int n = 0;
        for (int i = 0; i < page->count; i++)
            if (!page->files[i].is_dir && bp_media_format(page->files[i].name) != BP_FMT_UNKNOWN)
                s_playlist[n++] = page->files[i];
        s_pl_req = req;
        s_pl_has_more = page->has_more;
        if (n) {
            s_pl_count = n;
            start_track(0);
            started = true;
        } else if (s_pl_has_more) {
            started = continue_next_page();   // 整页无可播放文件,继续翻
        }
    }
    free(page);
    return started;
}

static void finish_idle(void) {
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    set_state(BP_PLAY_IDLE, NULL);
}

static void ctrl_task(void *arg) {
    (void)arg;
    int consecutive_failures = 0;
    cmd_t c;
    for (;;) {
        if (xQueueReceive(s_cmd, &c, portMAX_DELAY) != pdTRUE) continue;
        switch (c.type) {
            case CMD_PLAY: {
                portENTER_CRITICAL(&s_mux);
                memcpy(s_playlist, s_pending_list, sizeof(s_playlist));
                s_pl_count = s_pending_count;
                s_pl_req = s_pending_req;
                s_pl_has_more = s_pending_has_more;
                int index = s_pending_index;
                portEXIT_CRITICAL(&s_mux);
                consecutive_failures = 0;
                start_track(index);
                break;
            }
            case CMD_NEXT: {
                int next = bp_playlist_move(s_pl_index, s_pl_count, 1, false);
                consecutive_failures = 0;
                if (next >= 0) {
                    start_track(next);
                    break;
                }
                stop_pipeline();
                if (!continue_next_page()) start_track(0);   // 没有更多页:回到列表开头
                break;
            }
            case CMD_PREV: {
                bp_player_info_t info;
                bp_player_get_info(&info);
                consecutive_failures = 0;
                if (info.pos_ms > 3000 && info.state != BP_PLAY_ERROR) start_track(s_pl_index);
                else start_track(bp_playlist_move(s_pl_index, s_pl_count, -1, true));
                break;
            }
            case CMD_STOP:
                stop_pipeline();
                finish_idle();
                break;
            case CMD_TRACK_END:
            case CMD_TRACK_FAIL: {
                if (c.gen != s_gen) break;   // 已被新命令取代
                if (c.type == CMD_TRACK_FAIL) {
                    if (c.error) set_state(BP_PLAY_ERROR, c.error);
                    ESP_LOGW(TAG, "track failed: %s", s_info.error ? s_info.error : "?");
                    // 停留 2 秒让用户看到原因;连续失败 3 首就停下,避免空转。
                    if (++consecutive_failures >= 3) {
                        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    if (c.gen != s_gen || uxQueueMessagesWaiting(s_cmd)) break;
                } else {
                    consecutive_failures = 0;
                }
                int next = bp_playlist_move(s_pl_index, s_pl_count, 1, false);
                if (next >= 0) start_track(next);
                else if (!continue_next_page()) finish_idle();
                break;
            }
        }
    }
}

// ---- 公共接口 -------------------------------------------------------------------
int bp_player_play_list(const bp_list_t *list, int index) {
    if (!list || index < 0 || index >= list->count) return -1;
    int n = 0, start = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < list->count && n < BP_PLAYLIST_MAX; i++) {
        const bp_file_t *f = &list->files[i];
        if (f->is_dir || bp_media_format(f->name) == BP_FMT_UNKNOWN) continue;
        if (i == index) start = n;
        s_pending_list[n++] = *f;
    }
    s_pending_count = n;
    s_pending_index = start;
    s_pending_req = list->req;
    s_pending_has_more = list->has_more;
    portEXIT_CRITICAL(&s_mux);
    if (start < 0) return -2;
    post(CMD_PLAY, 0, NULL);
    return 0;
}

void bp_player_toggle_pause(void) {
    bp_play_state_t st = s_info.state;
    if (st == BP_PLAY_PLAYING || st == BP_PLAY_BUFFERING || st == BP_PLAY_PAUSED)
        s_paused = !s_paused;
}

void bp_player_next(void) { post(CMD_NEXT, 0, NULL); }
void bp_player_prev(void) { post(CMD_PREV, 0, NULL); }
void bp_player_stop(void) { post(CMD_STOP, 0, NULL); }

void bp_player_set_volume(uint8_t volume) {
    g_bp.volume = volume > 100 ? 100 : volume;
    bsp_audio_set_volume(g_bp.volume);
    nvs_handle_t h;
    if (nvs_open("bp_player", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vol", g_bp.volume);
        nvs_commit(h);
        nvs_close(h);
    }
}

void bp_player_get_info(bp_player_info_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_mux);
}

bool bp_player_active(void) {
    bp_play_state_t st = s_info.state;
    return st == BP_PLAY_RESOLVING || st == BP_PLAY_BUFFERING || st == BP_PLAY_PLAYING ||
           st == BP_PLAY_PAUSED;
}

void bp_player_init(void) {
    uint8_t vol = 60;
    nvs_handle_t h;
    if (nvs_open("bp_player", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "vol", &vol);
        nvs_close(h);
    }
    g_bp.volume = vol > 100 ? 60 : vol;
    if (bsp_audio_init() != ESP_OK) ESP_LOGE(TAG, "audio init failed");
    bsp_audio_set_volume(g_bp.volume);
    s_stream = xStreamBufferCreateStatic(STREAM_SIZE, 1, s_stream_storage, &s_stream_struct);
    s_cmd = xQueueCreate(6, sizeof(cmd_t));
    if (!s_cmd || xTaskCreate(ctrl_task, "bp_ctrl", CTRL_STACK, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "player task create failed");
}
