// main/main.c —— 云盘随身听入口:初始化、按键分发、页面状态机、空闲熄屏、心跳。
//
// 按键(三键:上 / OK / 下):
//   首页      上/下=选择  OK=进入  长按OK=回到播放页  长按下=熄屏
//   列表      上/下=选择(长按 ±5)  OK=打开文件夹/播放/翻页  长按OK=上一级/首页
//   播放页    OK=暂停/继续(已停止时重播本曲)  双击OK=停止  上/下=音量
//             长按上/下=上一首/下一首  长按OK=返回
//   设置/信息 上/下=选择  OK=进入/执行  长按OK=返回
//   无线网络  OK=开启配网热点(手机连热点后网页配网)  长按OK=返回
//   配网热点  长按OK=关闭热点并返回(网页配网完成后自动关闭)
//   熄屏时任意键唤醒(该次按键不触发操作)
#include "bp_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "main";

#define IDLE_TIMEOUT_MS 30000

bp_state_t g_bp;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static uint32_t s_last_activity_ms;
static int s_wake_button = -1;
static bp_page_t s_player_back = BP_PAGE_HOME;
static bool s_logout_armed;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void screen_off(bool off) {
    if (g_bp.screen_off == off) return;
    g_bp.screen_off = off;
    bsp_display_backlight(off ? 0 : 100);
}

static void on_wifi_event(int evt, const char *data) {
    if (evt == 2) {
        ESP_LOGI(TAG, "Wi-Fi connected: %s", data);
        g_bp.wifi_up = true;
        bp_baidu_on_wifi(true);
    } else if (evt == 3) {
        g_bp.wifi_up = false;
        bp_baidu_on_wifi(false);
    }
}

// ---- 页面动作(输入任务内调用,LVGL 操作都加锁) --------------------------------
static void go(bp_page_t page) {
    if (bsp_lvgl_lock(500)) {
        bp_ui_goto(page);
        bsp_lvgl_unlock();
    }
}

static void ui_move(int delta) {
    if (bsp_lvgl_lock(500)) {
        bp_ui_move(delta);
        bsp_lvgl_unlock();
    }
}

static void start_web_config(void) {
    bp_player_stop();   // 热点 + HTTP 服务需要内存,且 radio 切到 AP+STA
    if (bp_wifi_config_start() == 0) go(BP_PAGE_WIFI_AP);
    else bp_ui_toast("无法开启热点");
}

static bool require_auth(void) {
    if (bp_baidu_state() == BP_BD_READY) return true;
    if (!g_bp.wifi_up) {
        bp_ui_toast("请先连接 Wi-Fi");
        go(BP_PAGE_WIFI);
        return false;
    }
    bp_baidu_auth_start();
    go(BP_PAGE_AUTH);
    return false;
}

// 每页在网盘接口里的起始偏移(过滤后页码不能直接换算),用于“上一页”。
static uint32_t s_page_start[BP_PAGE_HISTORY];

static void open_list_at(bp_source_t source, const char *dir, int page, uint32_t start) {
    // 浏览网盘时先自动暂停播放(挂起并释放播放占用的内存),在播放页按 OK 继续。
    if (bp_player_suspend()) bp_ui_toast("已暂停播放");
    bp_list_req_t req = {.source = source, .page = page, .start = start};
    strlcpy(req.dir, dir ? dir : "/", sizeof(req.dir));
    if (page >= 0 && page < BP_PAGE_HISTORY) s_page_start[page] = start;
    int rc = bp_baidu_list_request(&req);
    if (rc == -3) {
        bp_ui_toast("请稍候");
        return;
    }
    if (rc != 0) bp_ui_toast("网盘暂不可用");
    if (bsp_lvgl_lock(500)) {
        bp_ui_list_reset_sel();
        bp_ui_goto(BP_PAGE_LIST);
        bsp_lvgl_unlock();
    }
}

static void open_list(bp_source_t source, const char *dir) {
    open_list_at(source, dir, 0, 0);
}

static void list_activate(void) {
    bp_list_t *list = malloc(sizeof(bp_list_t));
    if (!list) return;
    bp_baidu_list_snapshot(list);
    int fi = -1;
    bp_row_kind_t kind = BP_ROW_NONE;
    if (bsp_lvgl_lock(500)) {
        kind = bp_ui_list_row(&fi);
        bsp_lvgl_unlock();
    }
    switch (kind) {
        case BP_ROW_RETRY:
            open_list_at(list->req.source, list->req.dir, list->req.page, list->req.start);
            break;
        case BP_ROW_NEXT:
            open_list_at(list->req.source, list->req.dir, list->req.page + 1, list->next_start);
            break;
        case BP_ROW_PREV: {
            int prev = list->req.page - 1;
            // 超出历史记录(极深的翻页)时回到第一页。
            if (prev >= BP_PAGE_HISTORY) open_list(list->req.source, list->req.dir);
            else open_list_at(list->req.source, list->req.dir, prev, s_page_start[prev]);
            break;
        }
        case BP_ROW_FILE: {
            const bp_file_t *f = &list->files[fi];
            if (f->is_dir) {
                char child[BP_PATH_MAX];
                if (bp_path_child(list->req.dir, f->name, child, sizeof(child)))
                    open_list(BP_SRC_DIR, child);
                else
                    bp_ui_toast("路径太长");
            } else if (f->format == BP_FMT_UNKNOWN) {
                bp_ui_toast("暂不支持此格式");
            } else if (bp_player_play_list(list, fi) == 0) {
                s_player_back = BP_PAGE_LIST;
                go(BP_PAGE_PLAYER);
            }
            break;
        }
        default:
            break;
    }
    free(list);
}

static void list_back(void) {
    bp_list_t *list = malloc(sizeof(bp_list_t));
    if (!list) return;
    bp_baidu_list_snapshot(list);
    char parent[BP_PATH_MAX];
    if (list->req.source == BP_SRC_DIR && bp_path_parent(list->req.dir, parent, sizeof(parent)))
        open_list(BP_SRC_DIR, parent);
    else
        go(BP_PAGE_HOME);
    free(list);
}

static void handle_input(const input_event_t *in) {
    s_last_activity_ms = now_ms();

    // 唤醒手势整体吞掉:物理按键会先 PRESS,再 CLICK/LONG。
    if (s_wake_button == (int)in->btn) {
        if (in->event != BSP_BTN_PRESS) s_wake_button = -1;
        return;
    }
    if (g_bp.screen_off) {
        screen_off(false);
        if (in->event == BSP_BTN_PRESS) s_wake_button = (int)in->btn;
        return;
    }
    if (in->event == BSP_BTN_PRESS) return;
    bool dbl = in->event == BSP_BTN_DOUBLE;

    bool click = in->event == BSP_BTN_CLICK;
    bool lng = in->event == BSP_BTN_LONG;
    bool up = in->btn == BSP_BTN_UP, down = in->btn == BSP_BTN_DOWN, ok = in->btn == BSP_BTN_OK;
    bp_page_t page = bp_ui_page();

    switch (page) {
        case BP_PAGE_HOME:
            if ((up || down) && click) ui_move(up ? -1 : 1);
            else if (down && lng) screen_off(true);
            else if (ok && lng) {
                bp_player_info_t pi;
                bp_player_get_info(&pi);
                if (pi.name[0]) {
                    s_player_back = BP_PAGE_HOME;
                    go(BP_PAGE_PLAYER);
                } else {
                    bp_ui_toast("还没有播放的歌曲");
                }
            } else if (ok && click) {
                int sel = bp_ui_selected();
                if (sel == 2) go(BP_PAGE_SETTINGS);
                else if (require_auth()) open_list(sel == 0 ? BP_SRC_ALL_AUDIO : BP_SRC_DIR, "/");
            }
            break;

        case BP_PAGE_LIST:
            if ((up || down) && click) ui_move(up ? -1 : 1);
            else if ((up || down) && lng) ui_move(up ? -5 : 5);
            else if (ok && click) list_activate();
            else if (ok && lng) list_back();
            break;

        case BP_PAGE_PLAYER:
            if (ok && dbl) {
                if (bp_player_active()) {
                    bp_player_stop();
                    bp_ui_toast("已停止播放");
                }
            } else if (ok && click) bp_player_toggle_pause();
            else if (ok && lng) go(s_player_back);
            else if ((up || down) && click)
                bp_player_set_volume((uint8_t)bp_volume_step(g_bp.volume, up ? 10 : -10));
            else if (up && lng) bp_player_prev();
            else if (down && lng) bp_player_next();
            break;

        case BP_PAGE_AUTH:
            if (ok && lng) {
                bp_baidu_auth_cancel();
                go(BP_PAGE_HOME);
            }
            break;

        case BP_PAGE_SETTINGS:
            if ((up || down) && click) ui_move(up ? -1 : 1);
            else if (ok && lng) go(BP_PAGE_HOME);
            else if (ok && click) {
                static const bp_page_t targets[] = {BP_PAGE_WIFI, BP_PAGE_ACCOUNT, BP_PAGE_ABOUT};
                int sel = bp_ui_selected();
                if (sel >= 0 && sel < 3) {
                    s_logout_armed = false;
                    go(targets[sel]);
                }
            }
            break;

        case BP_PAGE_ACCOUNT:
            if (ok && lng) go(BP_PAGE_SETTINGS);
            else if (ok && click) {
                if (bp_baidu_state() != BP_BD_READY) {
                    require_auth();
                } else if (!s_logout_armed) {
                    s_logout_armed = true;
                    if (bsp_lvgl_lock(500)) { bp_ui_account_arm(true); bsp_lvgl_unlock(); }
                } else {
                    s_logout_armed = false;
                    bp_player_stop();
                    bp_ui_toast(bp_baidu_logout() == 0 ? "已退出网盘账号" : "退出失败");
                    if (bsp_lvgl_lock(500)) { bp_ui_account_arm(false); bsp_lvgl_unlock(); }
                }
            } else if ((up || down) && click && s_logout_armed) {
                s_logout_armed = false;
                if (bsp_lvgl_lock(500)) { bp_ui_account_arm(false); bsp_lvgl_unlock(); }
            }
            break;

        case BP_PAGE_WIFI:
            if (ok && lng) go(BP_PAGE_SETTINGS);
            else if (ok && click) start_web_config();
            break;

        case BP_PAGE_WIFI_AP:
            if (ok && lng) {
                bp_wifi_config_stop();
                go(BP_PAGE_WIFI);
            }
            break;

        case BP_PAGE_ABOUT:
            if (ok && lng) go(BP_PAGE_SETTINGS);
            break;

        default:
            break;
    }
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t in;
    for (;;)
        if (xQueueReceive(s_input_queue, &in, portMAX_DELAY) == pdTRUE) handle_input(&in);
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready) return;
    const input_event_t in = {.btn = btn, .event = ev};
    (void)xQueueSend(s_input_queue, &in, 0);
}

int bp_test_key(int button, int kind) {
    if (!s_input_ready || button < 0 || button > 2 || kind < 0 || kind > 2) return -1;
    static const bsp_btn_t buttons[] = {BSP_BTN_UP, BSP_BTN_OK, BSP_BTN_DOWN};
    static const bsp_btn_ev_t kinds[] = {BSP_BTN_CLICK, BSP_BTN_LONG, BSP_BTN_DOUBLE};
    const input_event_t in = {.btn = buttons[button], .event = kinds[kind]};
    return xQueueSend(s_input_queue, &in, pdMS_TO_TICKS(100)) == pdTRUE ? 0 : -1;
}

int bp_test_page(void) { return bp_ui_page(); }

// ---- 定时器 -----------------------------------------------------------------------
static void idle_timer_cb(void *arg) {
    (void)arg;
    // 播放中也熄屏省电,音乐继续;授权页与配网页需要扫码,保持常亮。
    bp_page_t page = bp_ui_page();
    if (!g_bp.screen_off && page != BP_PAGE_AUTH && page != BP_PAGE_WIFI_AP &&
        now_ms() - s_last_activity_ms > IDLE_TIMEOUT_MS) {
        screen_off(true);
    }
}

static void heartbeat_cb(void *arg) {
    (void)arg;
    bp_player_info_t pi;
    bp_player_get_info(&pi);
    ESP_LOGI(TAG, "[HB] free=%u min=%u largest=%u wifi=%d bd=%d play=%d pos=%lu buf=%u%%",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), g_bp.wifi_up,
             bp_baidu_state(), pi.state, (unsigned long)pi.pos_ms, pi.buffer_pct);
}

void app_main(void) {
    ESP_LOGI(TAG, "Baidupan Pocket Player v" BP_APP_VERSION);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed");
        return;
    }
    bsp_battery_init();
    bp_baidu_init();
    bp_player_init();

    if (bsp_lvgl_lock(1000)) {
        bp_ui_init();
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(100);
    s_last_activity_ms = now_ms();

    s_input_queue = xQueueCreate(8, sizeof(input_event_t));
    if (s_input_queue && xTaskCreate(input_task, "bp_input", 6144, NULL, 5, NULL) == pdPASS &&
        bsp_button_init(on_key, NULL) == ESP_OK) {
        s_input_ready = true;
    } else {
        ESP_LOGE(TAG, "button init failed");
    }

    const esp_timer_create_args_t idle_args = {.callback = idle_timer_cb, .name = "bp_idle"};
    esp_timer_handle_t idle_timer;
    if (esp_timer_create(&idle_args, &idle_timer) == ESP_OK)
        esp_timer_start_periodic(idle_timer, 1000 * 1000);
    const esp_timer_create_args_t hb_args = {.callback = heartbeat_cb, .name = "bp_hb"};
    esp_timer_handle_t hb_timer;
    if (esp_timer_create(&hb_args, &hb_timer) == ESP_OK)
        esp_timer_start_periodic(hb_timer, 30 * 1000 * 1000);

    bp_wifi_init(on_wifi_event);
    if (bp_wifi_saved_count() == 0) {
        // 首次使用:直接开启配网热点,手机扫屏幕二维码即可配网。
        ESP_LOGI(TAG, "no saved Wi-Fi; starting web provisioning hotspot");
        bp_wifi_config_start();
        go(BP_PAGE_WIFI_AP);
    } else {
        bp_wifi_start_station();
    }
    bp_console_start();
}
