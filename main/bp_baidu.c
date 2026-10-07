// main/bp_baidu.c —— 百度网盘接入:设备码授权 + 目录/音频列表 + 下载直链。
//
// 授权: GET oauth/2.0/device/code → 屏显二维码(verification_url + user_code) →
//       轮询 oauth/2.0/token(grant_type=device_token) → NVS 存 access/refresh。
// 列表: xpan/file?method=list(目录)或 xpan/multimedia?method=categorylist
//       (category=2 音频,ext=mp3,wav,递归)。
// 直链: xpan/multimedia?method=filemetas&dlink=1,下载时追加 access_token 并带
//       User-Agent: pan.baidu.com(由播放器完成)。
//
// 令牌:refresh_token 单次有效。多个工作任务可能同时遇到 errno=111,所以刷新在
// 互斥锁内进行,并先比较调用方看到的旧令牌,已被别人刷新过就直接复用。
#include "bp_app.h"
// 应用凭据在构建时编码(tools/obfuscate_keys.py),此处只在需要时解码到栈上并立即清零;
// 本文件不包含 bp_baidu_keys.h,固件中没有明文凭据。
#include "bp_keys.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md5.h"
#include "nvs.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "bp_bd"

#define BD_NVS_NS "bp_bd"
#define BD_LIST_RESP_MAX (14 * 1024)
#define BD_META_RESP_MAX 3072
#define BD_AUTH_STACK 8192
#define BD_LIST_STACK 8192
// 播放中再开一条 TLS 连接需要的最小连续内存(16 KiB 入站记录 + 余量)。
#define BD_MIN_BLOCK_FOR_TLS (20 * 1024)

static SemaphoreHandle_t s_tok_lock;
static char s_access[160];
static char s_refresh[96];
static volatile bp_bd_state_t s_state = BP_BD_NO_AUTH;

static volatile bool s_auth_running;
static volatile bool s_auth_cancel;
static char s_user_code[16];
static char s_verify_url[96];

static portMUX_TYPE s_list_mux = portMUX_INITIALIZER_UNLOCKED;
static bp_list_t s_list;            // 异步列表结果
static bool s_list_running;
static bp_list_req_t s_list_pending_req;

// ---- 令牌持久化 ---------------------------------------------------------------
// 缓存的授权与应用凭据绑定:换了 AppKey/Secret(或旧缓存无指纹)需重新扫码。
static bool credential_fingerprint(unsigned char out[16]) {
    char appkey[BP_KEY_BUF], secret[BP_KEY_BUF];
    size_t al = bp_key_get(BP_KEY_APPKEY, appkey, sizeof(appkey));
    size_t sl = bp_key_get(BP_KEY_SECRET, secret, sizeof(secret));
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    // 长度含结尾 NUL:与旧版 sizeof(字符串宏) 的指纹一致,升级后已有授权不失效。
    bool ok = al && sl && mbedtls_md5_starts(&ctx) == 0 &&
              mbedtls_md5_update(&ctx, (const unsigned char *)appkey, al + 1) == 0 &&
              mbedtls_md5_update(&ctx, (const unsigned char *)secret, sl + 1) == 0 &&
              mbedtls_md5_finish(&ctx, out) == 0;
    mbedtls_md5_free(&ctx);
    bp_key_wipe(appkey, sizeof(appkey));
    bp_key_wipe(secret, sizeof(secret));
    return ok;
}

// 拼接带 client_id(及可选 client_secret)的 OAuth URL;用完须 bp_key_wipe(url)。
static void oauth_url(char *url, size_t cap, const char *prefix, bool with_secret) {
    char appkey[BP_KEY_BUF], secret[BP_KEY_BUF] = "";
    bp_key_get(BP_KEY_APPKEY, appkey, sizeof(appkey));
    if (with_secret) bp_key_get(BP_KEY_SECRET, secret, sizeof(secret));
    snprintf(url, cap, "%s&client_id=%s%s%s", prefix, appkey,
             with_secret ? "&client_secret=" : "", secret);
    bp_key_wipe(appkey, sizeof(appkey));
    bp_key_wipe(secret, sizeof(secret));
}

static void nvs_save_tokens(void) {
    nvs_handle_t h;
    if (nvs_open(BD_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "at", s_access);
    nvs_set_str(h, "rt", s_refresh);
    unsigned char fp[16];
    if (credential_fingerprint(fp)) nvs_set_blob(h, "client_fp", fp, sizeof(fp));
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_tokens(void) {
    s_access[0] = s_refresh[0] = 0;
    nvs_handle_t h;
    if (nvs_open(BD_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    unsigned char saved[16], current[16];
    size_t len = sizeof(saved);
    if (nvs_get_blob(h, "client_fp", saved, &len) != ESP_OK || len != sizeof(saved) ||
        !credential_fingerprint(current) || memcmp(saved, current, sizeof(saved))) {
        nvs_close(h);
        ESP_LOGI(TAG, "no authorization for this app key; scan to authorize");
        return;
    }
    size_t l1 = sizeof(s_access), l2 = sizeof(s_refresh);
    nvs_get_str(h, "at", s_access, &l1);
    nvs_get_str(h, "rt", s_refresh, &l2);
    nvs_close(h);
}

static void copy_access(char *out, size_t cap) {
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    strlcpy(out, s_access, cap);
    xSemaphoreGive(s_tok_lock);
}

static int64_t boot_sec(void) { return esp_timer_get_time() / 1000000; }

static void sntp_start_once(void) {
    if (esp_sntp_enabled()) return;
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
}

// URL 组件百分号编码(UTF-8 安全)。
static size_t url_encode(const char *in, char *out, size_t cap) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = HEX[*p >> 4];
            out[o++] = HEX[*p & 15];
        }
    }
    out[o] = 0;
    return o;
}

// ---- HTTPS GET ----------------------------------------------------------------
// 返回 0=HTTP 2xx 且 resp 完整;-1 传输失败;-2 响应超出 cap;>0 HTTP 状态码。
static int http_get(const char *url, char *resp, size_t cap, int timeout_ms) {
    resp[0] = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "User-Agent", "pan.baidu.com");
    int rc = -1;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int total = 0;
        while (total < (int)cap - 1) {
            int n = esp_http_client_read(c, resp + total, (int)cap - 1 - total);
            if (n <= 0) break;
            total += n;
        }
        resp[total] = 0;
        int status = esp_http_client_get_status_code(c);
        if (total >= (int)cap - 1 && !esp_http_client_is_complete_data_received(c)) rc = -2;
        else rc = (status >= 200 && status < 300) ? 0 : status;
    } else {
        ESP_LOGW(TAG, "http open: %s", esp_err_to_name(err));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (rc != 0) {
        // URL 含 access_token / client_secret,只记录主机名。
        const char *host = strstr(url, "://");
        host = host ? host + 3 : url;
        const char *end = strchr(host, '/');
        ESP_LOGW(TAG, "http failed host=%.*s rc=%d", end ? (int)(end - host) : (int)strlen(host),
                 host, rc);
    }
    return rc;
}

// ---- 令牌 -----------------------------------------------------------------------
// 调用方持有 s_tok_lock。成功写入令牌并持久化返回 0;业务错误返回 -2。
static int token_exchange_locked(const char *url, char *resp, size_t cap) {
    int rc = http_get(url, resp, cap, 12000);
    // OAuth 失败时 HTTP 400 也带 JSON 错误体,照常解析。
    if (rc < 0) return -1;
    cJSON *root = cJSON_Parse(resp);
    if (!root) return -1;
    const cJSON *at = cJSON_GetObjectItem(root, "access_token");
    const cJSON *rt = cJSON_GetObjectItem(root, "refresh_token");
    const cJSON *err = cJSON_GetObjectItem(root, "error");
    int result = -2;
    if (cJSON_IsString(at) && at->valuestring[0] &&
        strlen(at->valuestring) < sizeof(s_access)) {
        strlcpy(s_access, at->valuestring, sizeof(s_access));
        if (cJSON_IsString(rt) && rt->valuestring[0])
            strlcpy(s_refresh, rt->valuestring, sizeof(s_refresh));
        nvs_save_tokens();
        result = 0;
    } else if (cJSON_IsString(err) && strcmp(err->valuestring, "authorization_pending")) {
        ESP_LOGW(TAG, "oauth error: %.40s", err->valuestring);
    }
    cJSON_Delete(root);
    return result;
}

// seen = 调用方失败时使用的 access_token。若已被其他任务刷新,直接返回成功。
static int token_refresh(const char *seen) {
    int rc = -1;
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    if (seen && strcmp(seen, s_access) != 0 && s_access[0]) {
        rc = 0;
    } else if (s_refresh[0]) {
        char prefix[256], url[512], resp[1024];
        snprintf(prefix, sizeof(prefix),
                 "https://openapi.baidu.com/oauth/2.0/token?grant_type=refresh_token"
                 "&refresh_token=%s", s_refresh);
        oauth_url(url, sizeof(url), prefix, true);
        rc = token_exchange_locked(url, resp, sizeof(resp));
        bp_key_wipe(url, sizeof(url));
        // 刷新令牌被拒(过期/已吊销)才需要重新扫码;网络失败保留旧令牌下次再试。
        if (rc == -2) {
            s_access[0] = s_refresh[0] = 0;
            s_state = BP_BD_NO_AUTH;
            ESP_LOGW(TAG, "refresh rejected; re-authorization required");
        }
    }
    xSemaphoreGive(s_tok_lock);
    return rc;
}

static bool token_invalid(int code) { return code == 111 || code == -6; }

// ---- 授权任务 ---------------------------------------------------------------------
static void auth_task(void *arg) {
    (void)arg;
    int rc = -1;
    char url[320];
    char *resp = malloc(1024);
    while (resp && rc != 0 && !s_auth_cancel) {
        s_user_code[0] = s_verify_url[0] = 0;
        bool got_code = false;
        oauth_url(url, sizeof(url),
                  "https://openapi.baidu.com/oauth/2.0/device/code"
                  "?response_type=device_code&scope=basic,netdisk", false);
        bool fetched = g_bp.wifi_up && http_get(url, resp, 1024, 12000) == 0;
        bp_key_wipe(url, sizeof(url));
        if (fetched) {
            cJSON *root = cJSON_Parse(resp);
            const cJSON *dc = cJSON_GetObjectItem(root, "device_code");
            const cJSON *uc = cJSON_GetObjectItem(root, "user_code");
            const cJSON *ex = cJSON_GetObjectItem(root, "expires_in");
            const cJSON *iv = cJSON_GetObjectItem(root, "interval");
            if (cJSON_IsString(dc) && cJSON_IsString(uc) &&
                strlen(uc->valuestring) < sizeof(s_user_code)) {
                char device_code[96];
                strlcpy(device_code, dc->valuestring, sizeof(device_code));
                strlcpy(s_user_code, uc->valuestring, sizeof(s_user_code));
                // 二维码直接带上用户码,扫码即到确认页,无需手动输入。
                snprintf(s_verify_url, sizeof(s_verify_url),
                         "https://openapi.baidu.com/device?display=page&code=%s", s_user_code);
                int interval = cJSON_IsNumber(iv) && iv->valueint > 0 ? iv->valueint : 5;
                int expires = cJSON_IsNumber(ex) && ex->valueint > 0 ? ex->valueint : 300;
                got_code = true;
                cJSON_Delete(root);
                root = NULL;
                ESP_LOGI(TAG, "AUTH: user code valid for %ds", expires);
                int64_t deadline = boot_sec() + expires;
                rc = -2;
                while (boot_sec() < deadline && !s_auth_cancel) {
                    for (int s = 0; s < interval && !s_auth_cancel; s++)
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    if (s_auth_cancel || boot_sec() >= deadline) break;
                    if (!g_bp.wifi_up) continue;
                    char prefix[192];
                    snprintf(prefix, sizeof(prefix),
                             "https://openapi.baidu.com/oauth/2.0/token?grant_type=device_token"
                             "&code=%s", device_code);
                    oauth_url(url, sizeof(url), prefix, true);
                    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
                    rc = token_exchange_locked(url, resp, 1024);
                    xSemaphoreGive(s_tok_lock);
                    bp_key_wipe(url, sizeof(url));
                    if (rc == 0) break;
                }
            }
            cJSON_Delete(root);
        }
        if (!got_code)
            for (int s = 0; s < 10 && !s_auth_cancel; s++) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    free(resp);
    s_user_code[0] = s_verify_url[0] = 0;
    if (rc == 0 && !s_auth_cancel) {
        s_state = BP_BD_READY;
        sntp_start_once();
        ESP_LOGI(TAG, "AUTH: authorized");
        bp_app_on_authorized();
    } else if (s_state == BP_BD_WAIT_CODE) {
        s_state = BP_BD_NO_AUTH;
    }
    s_auth_running = false;
    vTaskDelete(NULL);
}

int bp_baidu_auth_start(void) {
    if (s_state == BP_BD_READY) return 1;
    if (!g_bp.wifi_up) return -2;
    if (s_auth_running) return s_auth_cancel ? -3 : 0;
    s_auth_cancel = false;
    s_auth_running = true;
    s_user_code[0] = s_verify_url[0] = 0;
    s_state = BP_BD_WAIT_CODE;
    if (xTaskCreate(auth_task, "bp_bd_auth", BD_AUTH_STACK, NULL, 4, NULL) != pdPASS) {
        s_auth_running = false;
        s_state = BP_BD_NO_AUTH;
        return -3;
    }
    return 0;
}

void bp_baidu_auth_cancel(void) {
    if (s_auth_running) s_auth_cancel = true;
}

void bp_baidu_get_auth(char *url, size_t ucap, char *code, size_t ccap) {
    strlcpy(url, s_verify_url, ucap);
    strlcpy(code, s_user_code, ccap);
}

bp_bd_state_t bp_baidu_state(void) { return s_state; }

int bp_baidu_logout(void) {
    if (s_auth_running) return -3;
    nvs_handle_t h;
    esp_err_t err = nvs_open(BD_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) return -1;
    xSemaphoreTake(s_tok_lock, portMAX_DELAY);
    memset(s_access, 0, sizeof(s_access));
    memset(s_refresh, 0, sizeof(s_refresh));
    s_state = BP_BD_NO_AUTH;
    xSemaphoreGive(s_tok_lock);
    portENTER_CRITICAL(&s_list_mux);
    memset(&s_list, 0, sizeof(s_list));
    portEXIT_CRITICAL(&s_list_mux);
    ESP_LOGI(TAG, "logged out locally (cloud files untouched)");
    return 0;
}

// ---- 列表 -------------------------------------------------------------------------
static bool memory_allows_request(void) {
    // 播放时下载连接已占一份 TLS;再开一份前确认最大连续块够用。
    return !bp_player_active() ||
           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= BD_MIN_BLOCK_FOR_TLS;
}

// 把一批原始条目中可显示的追加到 out。返回本批实际消费的原始条目数:
// out 已满时停在第一个放不下的可显示条目上,下一页从这里继续。
// 取 path 的上一级目录名(“/音乐/儿歌/a.mp3” → “儿歌”),根目录记作“我的网盘”。
static void parent_folder(const char *path, char *out, size_t cap) {
    const char *last = path ? strrchr(path, '/') : NULL;
    if (!last || last == path) {
        bp_name_shorten("我的网盘", out, cap);
        return;
    }
    const char *start = last - 1;
    while (start > path && *start != '/') start--;
    if (*start == '/') start++;
    char seg[BP_PATH_MAX];
    size_t n = (size_t)(last - start);
    if (n >= sizeof(seg)) n = sizeof(seg) - 1;
    memcpy(seg, start, n);
    seg[n] = 0;
    bp_name_shorten(seg, out, cap);
}

static int append_entries(const cJSON *list, bp_list_t *out, bool *stopped, const char *folder) {
    int n = cJSON_GetArraySize(list);
    *stopped = false;
    for (int i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(list, i);
        const cJSON *id = cJSON_GetObjectItem(e, "fs_id");
        const cJSON *name = cJSON_GetObjectItem(e, "server_filename");
        const cJSON *size = cJSON_GetObjectItem(e, "size");
        const cJSON *isdir = cJSON_GetObjectItem(e, "isdir");
        if (!cJSON_IsNumber(id) || !cJSON_IsString(name) || id->valuedouble < 1 ||
            id->valuedouble > 9007199254740991.0)
            continue;
        bool is_dir = cJSON_IsNumber(isdir) && isdir->valueint != 0;
        if (!bp_media_listable(is_dir, name->valuestring)) continue;   // 不支持的格式不显示
        if (out->count >= BP_PAGE_SIZE) {
            *stopped = true;
            return i;
        }
        bp_file_t *f = &out->files[out->count++];
        f->fs_id = (uint64_t)id->valuedouble;
        f->size = cJSON_IsNumber(size) ? (uint64_t)size->valuedouble : 0;
        f->is_dir = is_dir;
        // 格式按完整文件名判断;显示名超长时缩短为“前半…扩展名”,不切断中文字符。
        f->format = is_dir ? BP_FMT_UNKNOWN : (uint8_t)bp_media_format(name->valuestring);
        bp_name_shorten(name->valuestring, f->name, sizeof(f->name));
        // “全部音频”跨目录:按各自 path 取文件夹名;浏览目录时就是当前目录。
        const cJSON *path = cJSON_GetObjectItem(e, "path");
        if (folder) bp_name_shorten(folder, f->folder, sizeof(f->folder));
        else parent_folder(cJSON_IsString(path) ? path->valuestring : NULL, f->folder,
                           sizeof(f->folder));
    }
    return n;
}

// 请求一批原始条目(从 cursor 起 BP_PAGE_SIZE 条)并追加。
// 返回 0 成功;*consumed 为消费的原始条目数,*api_more 表示接口在本批之后还有数据。
static int fetch_chunk(const bp_list_req_t *req, uint32_t cursor, bp_list_t *out,
                       char *resp, char *url, uint32_t *consumed, bool *api_more) {
    char access[sizeof(s_access)];
    int result = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        copy_access(access, sizeof(access));
        if (req->source == BP_SRC_ALL_AUDIO) {
            snprintf(url, 1280,
                     "https://pan.baidu.com/rest/2.0/xpan/multimedia?method=categorylist"
                     "&access_token=%s&category=2&parent_path=%%2F&recursion=1"
                     "&ext=mp3%%2Cwav%%2Cm4a%%2Caac&order=name&desc=0&start=%lu&limit=%d",
                     access, (unsigned long)cursor, BP_PAGE_SIZE);
        } else {
            char dir_enc[3 * BP_PATH_MAX];
            url_encode(req->dir, dir_enc, sizeof(dir_enc));
            snprintf(url, 1280,
                     "https://pan.baidu.com/rest/2.0/xpan/file?method=list"
                     "&access_token=%s&dir=%s&order=name&desc=0&start=%lu&limit=%d",
                     access, dir_enc, (unsigned long)cursor, BP_PAGE_SIZE);
        }
        int rc = http_get(url, resp, BD_LIST_RESP_MAX, 15000);
        if (rc == -2) { result = -2; break; }
        if (rc != 0) break;
        cJSON *root = cJSON_Parse(resp);
        if (!root) break;
        const cJSON *errno_j = cJSON_GetObjectItem(root, "errno");
        int code = cJSON_IsNumber(errno_j) ? errno_j->valueint : -100;
        if (token_invalid(code) && attempt == 0) {
            cJSON_Delete(root);
            if (token_refresh(access) == 0) continue;
            break;
        }
        if (code == 0) {
            const cJSON *list = cJSON_GetObjectItem(root, "list");
            int raw = cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0;
            bool stopped = false;
            const char *folder = req->source == BP_SRC_DIR ?
                (strcmp(req->dir, "/") ? bp_path_basename(req->dir) : "我的网盘") : NULL;
            *consumed = raw ? (uint32_t)append_entries(list, out, &stopped, folder) : 0;
            const cJSON *more = cJSON_GetObjectItem(root, "has_more");
            bool chunk_more = cJSON_IsNumber(more) ? more->valueint != 0 : raw >= BP_PAGE_SIZE;
            *api_more = stopped || chunk_more;
            result = 0;
        } else if (code == -9) {   // 目录不存在
            *consumed = 0;
            *api_more = false;
            result = 0;
        } else {
            ESP_LOGW(TAG, "list errno=%d", code);
        }
        cJSON_Delete(root);
        break;
    }
    return result;
}

// 从 req->start 起向后扫描,直到凑满一页可显示条目、接口没有更多,或达到扫描上限
// (避免满是图片/文档的目录一次读太久;没凑满时仍可“下一页”继续)。
#define BD_MAX_SCAN_CHUNKS 6

int bp_baidu_list_fetch(const bp_list_req_t *req, bp_list_t *out) {
    memset(out, 0, sizeof(*out));
    out->req = *req;
    out->status = -1;
    out->next_start = req->start;
    if (!g_bp.wifi_up || s_state != BP_BD_READY) return -1;
    if (!memory_allows_request()) return -5;
    char *resp = malloc(BD_LIST_RESP_MAX);
    char *url = malloc(1280);
    if (!resp || !url) {
        free(resp);
        free(url);
        return -4;
    }
    uint32_t cursor = req->start;
    bool api_more = true;
    int result = 0;
    for (int chunk = 0; chunk < BD_MAX_SCAN_CHUNKS && api_more && out->count < BP_PAGE_SIZE;
         chunk++) {
        uint32_t consumed = 0;
        int rc = fetch_chunk(req, cursor, out, resp, url, &consumed, &api_more);
        if (rc != 0) {
            // 第一批就失败才算出错;后续批次失败则返回已读到的部分,下一页可重试。
            if (chunk == 0) result = rc;
            else api_more = true;
            break;
        }
        cursor += consumed;
    }
    free(url);
    free(resp);
    if (result != 0) return result;
    out->next_start = cursor;
    out->has_more = api_more;
    out->status = 2;
    return 0;
}

static void list_task(void *arg) {
    (void)arg;
    bp_list_t *result = malloc(sizeof(bp_list_t));
    bp_list_req_t req;
    portENTER_CRITICAL(&s_list_mux);
    req = s_list_pending_req;
    portEXIT_CRITICAL(&s_list_mux);
    // 浏览前已自动挂起播放;等流水线拆除,把内存让给这次列表请求。
    if (bp_player_active()) bp_player_wait_released(3000);
    int rc = result ? bp_baidu_list_fetch(&req, result) : -4;
    portENTER_CRITICAL(&s_list_mux);
    if (result) {
        s_list = *result;
    } else {
        s_list.status = -1;
    }
    s_list_running = false;
    portEXIT_CRITICAL(&s_list_mux);
    free(result);
    if (rc != 0)
        ESP_LOGW(TAG, "list failed rc=%d free=%u largest=%u", rc,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (rc == -5) bp_ui_toast("播放中内存不足，暂不能读取列表");
    else if (rc == -2) bp_ui_toast("目录过大，无法读取");
    else if (rc != 0) bp_ui_toast("读取网盘失败");
    vTaskDelete(NULL);
}

int bp_baidu_list_request(const bp_list_req_t *req) {
    if (!req || req->page < 0 || req->page > 10000) return -1;
    if (!g_bp.wifi_up || s_state != BP_BD_READY) return -1;
    portENTER_CRITICAL(&s_list_mux);
    if (s_list_running) {
        portEXIT_CRITICAL(&s_list_mux);
        return -3;
    }
    s_list_running = true;
    s_list_pending_req = *req;
    s_list.req = *req;
    s_list.status = 1;
    s_list.count = 0;
    s_list.has_more = false;
    portEXIT_CRITICAL(&s_list_mux);
    BaseType_t created = xTaskCreate(list_task, "bp_bd_list", BD_LIST_STACK, NULL, 4, NULL);
    if (created != pdPASS && bp_player_active()) {
        // 刚自动挂起时流水线还没拆除,任务栈分配不到;等释放后再试一次。
        bp_player_wait_released(3000);
        created = xTaskCreate(list_task, "bp_bd_list", BD_LIST_STACK, NULL, 4, NULL);
    }
    if (created != pdPASS) {
        ESP_LOGW(TAG, "list task create failed free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        portENTER_CRITICAL(&s_list_mux);
        s_list_running = false;
        s_list.status = -1;
        portEXIT_CRITICAL(&s_list_mux);
        return -4;
    }
    return 0;
}

void bp_baidu_list_snapshot(bp_list_t *out) {
    portENTER_CRITICAL(&s_list_mux);
    *out = s_list;
    portEXIT_CRITICAL(&s_list_mux);
}

// ---- 下载直链 -----------------------------------------------------------------------
int bp_baidu_dlink(uint64_t fs_id, char **out) {
    *out = NULL;
    if (!g_bp.wifi_up || s_state != BP_BD_READY) return -1;
    char *resp = malloc(BD_META_RESP_MAX);
    if (!resp) return -4;
    char access[sizeof(s_access)];
    int result = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        copy_access(access, sizeof(access));
        char url[400];
        snprintf(url, sizeof(url),
                 "https://pan.baidu.com/rest/2.0/xpan/multimedia?method=filemetas"
                 "&access_token=%s&fsids=%%5B%" PRIu64 "%%5D&dlink=1",
                 access, fs_id);
        if (http_get(url, resp, BD_META_RESP_MAX, 15000) != 0) break;
        cJSON *root = cJSON_Parse(resp);
        if (!root) break;
        const cJSON *errno_j = cJSON_GetObjectItem(root, "errno");
        int code = cJSON_IsNumber(errno_j) ? errno_j->valueint : -100;
        if (token_invalid(code) && attempt == 0) {
            cJSON_Delete(root);
            if (token_refresh(access) == 0) continue;
            break;
        }
        const cJSON *file = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "list"), 0);
        const cJSON *dlink = cJSON_GetObjectItem(file, "dlink");
        if (code == 0 && cJSON_IsString(dlink) && !strncmp(dlink->valuestring, "https://", 8)) {
            size_t len = strlen(dlink->valuestring) + strlen(access) + 24;
            char *full = malloc(len);
            if (full) {
                snprintf(full, len, "%s%saccess_token=%s", dlink->valuestring,
                         strchr(dlink->valuestring, '?') ? "&" : "?", access);
                *out = full;
                result = 0;
            }
        } else {
            ESP_LOGW(TAG, "filemetas errno=%d", code);
        }
        cJSON_Delete(root);
        break;
    }
    free(resp);
    return result;
}

void bp_baidu_on_wifi(bool up) {
    if (up) sntp_start_once();
}

void bp_baidu_init(void) {
    s_tok_lock = xSemaphoreCreateMutex();
    if (bp_keys_placeholder())
        ESP_LOGW(TAG, "built with placeholder Baidu credentials; authorization will fail");
    nvs_load_tokens();
    s_state = s_access[0] ? BP_BD_READY : BP_BD_NO_AUTH;
    if (s_state == BP_BD_READY) ESP_LOGI(TAG, "stored authorization found");
}
