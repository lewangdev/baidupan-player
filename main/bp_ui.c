// main/bp_ui.c —— 云盘随身听界面(240x320 竖屏,圆角屏)。
//
// 所有页面在 bp_ui_init 一次建好,切页只切换 HIDDEN,不反复创建/删除对象,
// 避免堆碎片。状态栏与提示条放在 lv_layer_top(),各页共用。
// 动态内容由 200 ms 的 lv_timer 刷新(运行在 LVGL 任务内,已持有锁)。
// 中文用 bp_font_16(GB2312 全集,显示任意网盘文件名)与 bp_font_24(界面固定文字子集);
// 图标用 Montserrat 内置符号,中文字体里没有这些码位。
#include "bp_app.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(bp_font_16);
LV_FONT_DECLARE(bp_font_24);

#define COL_BG       0x0B1220
#define COL_SURFACE  0x16213A
#define COL_SURFACE2 0x22314F
#define COL_ACCENT   0xFFB547
#define COL_ON_ACC   0x1A1205
#define COL_TEXT     0xEEF2F8
#define COL_MUTED    0x8A97B0
#define COL_DANGER   0xFF6B6B
#define COL_OK       0x5DD39E

#define LIST_ROWS 7
#define LIST_ROW_H 32
#define LIST_TOP 70
#define HOME_ITEMS 3
#define SETTINGS_ITEMS 3
#define TOAST_MS 2200

static lv_obj_t *s_pages[BP_PAGE_COUNT];
static bp_page_t s_page = BP_PAGE_HOME;
static int s_sel[BP_PAGE_COUNT];

// 顶层
static lv_obj_t *s_status, *s_toast, *s_toast_label;
static portMUX_TYPE s_toast_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_toast_text[64];
static bool s_toast_pending;
static int64_t s_toast_until;

// 首页
static lv_obj_t *s_home_card_title, *s_home_card_name, *s_home_card_bar, *s_home_card_icon;
static lv_obj_t *s_home_items[HOME_ITEMS], *s_home_icons[HOME_ITEMS], *s_home_texts[HOME_ITEMS];

// 列表页
static lv_obj_t *s_list_title, *s_list_sub, *s_list_msg;
static lv_obj_t *s_rows[LIST_ROWS], *s_row_icons[LIST_ROWS], *s_row_texts[LIST_ROWS];
static bp_list_t s_list;     // 最近一次快照
static int s_list_rows;      // 虚拟行数(文件 + 上/下一页)

// 播放页
static lv_obj_t *s_arc, *s_pl_time, *s_pl_total, *s_pl_state, *s_pl_name, *s_pl_meta;
static lv_obj_t *s_pl_vol_bar, *s_pl_icon;

// 授权页
static lv_obj_t *s_auth_qr_box, *s_auth_qr, *s_auth_code, *s_auth_hint;
static char s_auth_shown[96];

// 设置/信息页
static lv_obj_t *s_set_items[SETTINGS_ITEMS], *s_set_icons[SETTINGS_ITEMS], *s_set_texts[SETTINGS_ITEMS];
static lv_obj_t *s_wifi_info, *s_wifi_action, *s_acct_state, *s_acct_action, *s_acct_hint;
static bool s_acct_armed;

// 配网热点页
static lv_obj_t *s_ap_qr, *s_ap_ssid, *s_ap_steps;
static char s_ap_shown[33];

// ---- 小工具 ---------------------------------------------------------------------
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void set_text_if(lv_obj_t *l, const char *text) {
    const char *old = lv_label_get_text(l);
    if (!old || strcmp(old, text)) lv_label_set_text(l, text);
}

static lv_obj_t *page(void) {
    lv_obj_t *p = box(lv_screen_active(), 0, 0, 240, 320, COL_BG, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static lv_obj_t *page_title(lv_obj_t *p, const char *text) {
    lv_obj_t *t = label(p, &bp_font_24, COL_ACCENT, text);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 32);
    return t;
}

// 胶囊菜单项:选中时琥珀底深色字。
static void pill(lv_obj_t *parent, int y, const char *icon, const char *text,
                 lv_obj_t **item, lv_obj_t **icon_l, lv_obj_t **text_l) {
    *item = box(parent, 20, y, 200, 40, COL_SURFACE, 20);
    *icon_l = label(*item, &lv_font_montserrat_14, COL_ACCENT, icon);
    lv_obj_align(*icon_l, LV_ALIGN_LEFT_MID, 18, 0);
    *text_l = label(*item, &bp_font_16, COL_TEXT, text);
    lv_obj_align(*text_l, LV_ALIGN_LEFT_MID, 46, 0);
}

static void pill_select(lv_obj_t *item, lv_obj_t *icon, lv_obj_t *text, bool on) {
    lv_obj_set_style_bg_color(item, lv_color_hex(on ? COL_ACCENT : COL_SURFACE), 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(on ? COL_ON_ACC : COL_ACCENT), 0);
    lv_obj_set_style_text_color(text, lv_color_hex(on ? COL_ON_ACC : COL_TEXT), 0);
}

static const char *state_text(const bp_player_info_t *pi, char *buf, size_t cap) {
    switch (pi->state) {
        case BP_PLAY_RESOLVING: return "正在连接网盘…";
        case BP_PLAY_BUFFERING: snprintf(buf, cap, "缓冲中 %u%%", pi->buffer_pct); return buf;
        case BP_PLAY_PLAYING:   return "播放中";
        case BP_PLAY_PAUSED:    return "已暂停";
        case BP_PLAY_ERROR:     return pi->error ? pi->error : "播放失败";
        default:                return "未在播放";
    }
}

// ---- 构建页面 -------------------------------------------------------------------
static void build_top_layer(void) {
    s_status = label(lv_layer_top(), &lv_font_montserrat_14, COL_MUTED, "");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 8);

    s_toast = box(lv_layer_top(), 24, 262, 192, 36, COL_SURFACE2, 18);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    s_toast_label = label(s_toast, &bp_font_16, COL_TEXT, "");
    lv_obj_set_width(s_toast_label, 176);
    lv_obj_set_style_text_align(s_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_toast_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(s_toast_label);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

static void build_home(void) {
    lv_obj_t *p = s_pages[BP_PAGE_HOME] = page();
    page_title(p, "云盘随身听");

    lv_obj_t *card = box(p, 20, 74, 200, 82, COL_SURFACE, 16);
    s_home_card_icon = label(card, &lv_font_montserrat_20, COL_ACCENT, LV_SYMBOL_AUDIO);
    lv_obj_set_pos(s_home_card_icon, 14, 14);
    s_home_card_title = label(card, &bp_font_16, COL_MUTED, "未在播放");
    lv_obj_set_pos(s_home_card_title, 44, 12);
    s_home_card_name = label(card, &bp_font_16, COL_TEXT, "选一首网盘里的歌吧");
    lv_obj_set_width(s_home_card_name, 172);
    lv_label_set_long_mode(s_home_card_name, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_pos(s_home_card_name, 14, 40);
    s_home_card_bar = lv_bar_create(card);
    lv_obj_set_size(s_home_card_bar, 172, 4);
    lv_obj_set_pos(s_home_card_bar, 14, 66);
    lv_bar_set_range(s_home_card_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_home_card_bar, lv_color_hex(COL_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_home_card_bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

    static const char *icons[HOME_ITEMS] = {LV_SYMBOL_AUDIO, LV_SYMBOL_DIRECTORY, LV_SYMBOL_SETTINGS};
    static const char *texts[HOME_ITEMS] = {"全部音频", "浏览网盘", "设置"};
    for (int i = 0; i < HOME_ITEMS; i++)
        pill(p, 168 + i * 46, icons[i], texts[i], &s_home_items[i], &s_home_icons[i], &s_home_texts[i]);
}

static void build_list(void) {
    lv_obj_t *p = s_pages[BP_PAGE_LIST] = page();
    s_list_title = label(p, &bp_font_16, COL_ACCENT, "");
    lv_obj_set_width(s_list_title, 180);
    lv_obj_set_style_text_align(s_list_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_list_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_list_title, LV_ALIGN_TOP_MID, 0, 30);
    s_list_sub = label(p, &bp_font_16, COL_MUTED, "");
    lv_obj_align(s_list_sub, LV_ALIGN_TOP_MID, 0, 48);
    for (int i = 0; i < LIST_ROWS; i++) {
        s_rows[i] = box(p, 12, LIST_TOP + i * LIST_ROW_H, 216, LIST_ROW_H - 4, COL_BG, 10);
        s_row_icons[i] = label(s_rows[i], &lv_font_montserrat_14, COL_MUTED, "");
        lv_obj_align(s_row_icons[i], LV_ALIGN_LEFT_MID, 10, 0);
        s_row_texts[i] = label(s_rows[i], &bp_font_16, COL_TEXT, "");
        lv_obj_set_width(s_row_texts[i], 176);
        lv_label_set_long_mode(s_row_texts[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(s_row_texts[i], LV_ALIGN_LEFT_MID, 32, 0);
    }
    s_list_msg = label(p, &bp_font_16, COL_MUTED, "");
    lv_obj_set_width(s_list_msg, 200);
    lv_obj_set_style_text_align(s_list_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_list_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_list_msg, LV_ALIGN_CENTER, 0, 0);
}

static void build_player(void) {
    lv_obj_t *p = s_pages[BP_PAGE_PLAYER] = page();
    s_arc = lv_arc_create(p);
    lv_obj_set_size(s_arc, 176, 176);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, 34);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 1000);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

    s_pl_icon = label(s_arc, &lv_font_montserrat_20, COL_ACCENT, LV_SYMBOL_PLAY);
    lv_obj_align(s_pl_icon, LV_ALIGN_CENTER, 0, -46);
    s_pl_time = label(s_arc, &lv_font_montserrat_28, COL_TEXT, "0:00");
    lv_obj_align(s_pl_time, LV_ALIGN_CENTER, 0, -10);
    s_pl_total = label(s_arc, &lv_font_montserrat_14, COL_MUTED, "/ --:--");
    lv_obj_align(s_pl_total, LV_ALIGN_CENTER, 0, 16);
    s_pl_state = label(s_arc, &bp_font_16, COL_MUTED, "");
    lv_obj_set_width(s_pl_state, 130);
    lv_obj_set_style_text_align(s_pl_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_pl_state, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_pl_state, LV_ALIGN_CENTER, 0, 42);

    s_pl_name = label(p, &bp_font_16, COL_TEXT, "");
    lv_obj_set_width(s_pl_name, 200);
    lv_obj_set_style_text_align(s_pl_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_pl_name, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_align(s_pl_name, LV_ALIGN_TOP_MID, 0, 220);
    s_pl_meta = label(p, &bp_font_16, COL_MUTED, "");
    lv_obj_align(s_pl_meta, LV_ALIGN_TOP_MID, 0, 244);

    lv_obj_t *vol = label(p, &lv_font_montserrat_14, COL_MUTED, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_pos(vol, 46, 276);
    s_pl_vol_bar = lv_bar_create(p);
    lv_obj_set_size(s_pl_vol_bar, 120, 6);
    lv_obj_set_pos(s_pl_vol_bar, 74, 282);
    lv_bar_set_range(s_pl_vol_bar, 0, 100);
    lv_obj_set_style_bg_color(s_pl_vol_bar, lv_color_hex(COL_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_pl_vol_bar, lv_color_hex(COL_MUTED), LV_PART_INDICATOR);
}

static void build_auth(void) {
    lv_obj_t *p = s_pages[BP_PAGE_AUTH] = page();
    page_title(p, "连接百度网盘");
    s_auth_qr_box = box(p, 44, 72, 152, 152, 0xFFFFFF, 12);
    s_auth_qr = lv_qrcode_create(s_auth_qr_box);
    lv_qrcode_set_size(s_auth_qr, 136);
    lv_qrcode_set_dark_color(s_auth_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_auth_qr, lv_color_hex(0xFFFFFF));
    lv_obj_center(s_auth_qr);
    s_auth_code = label(p, &lv_font_montserrat_20, COL_TEXT, "");
    lv_obj_align(s_auth_code, LV_ALIGN_TOP_MID, 0, 232);
    s_auth_hint = label(p, &bp_font_16, COL_MUTED, "");
    lv_obj_set_width(s_auth_hint, 196);
    lv_obj_set_style_text_align(s_auth_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_auth_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_auth_hint, LV_ALIGN_TOP_MID, 0, 262);
}

static void build_settings(void) {
    lv_obj_t *p = s_pages[BP_PAGE_SETTINGS] = page();
    page_title(p, "设置");
    static const char *icons[SETTINGS_ITEMS] = {LV_SYMBOL_WIFI, LV_SYMBOL_DRIVE, LV_SYMBOL_LIST};
    static const char *texts[SETTINGS_ITEMS] = {"无线网络", "网盘账号", "关于与按键"};
    for (int i = 0; i < SETTINGS_ITEMS; i++)
        pill(p, 92 + i * 52, icons[i], texts[i], &s_set_items[i], &s_set_icons[i], &s_set_texts[i]);
}

static lv_obj_t *info_label(lv_obj_t *p, int y) {
    lv_obj_t *l = label(p, &bp_font_16, COL_TEXT, "");
    lv_obj_set_width(l, 196);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(l, 6, 0);
    lv_obj_set_pos(l, 22, y);
    return l;
}

static void build_info_pages(void) {
    lv_obj_t *p = s_pages[BP_PAGE_WIFI] = page();
    page_title(p, "无线网络");
    s_wifi_info = info_label(p, 76);
    lv_obj_t *item, *icon;
    pill(p, 240, LV_SYMBOL_WIFI, "开启配网热点", &item, &icon, &s_wifi_action);
    pill_select(item, icon, s_wifi_action, true);

    p = s_pages[BP_PAGE_WIFI_AP] = page();
    page_title(p, "网页配网");
    lv_obj_t *qr_box = box(p, 60, 70, 120, 120, 0xFFFFFF, 10);
    s_ap_qr = lv_qrcode_create(qr_box);
    lv_qrcode_set_size(s_ap_qr, 108);
    lv_qrcode_set_dark_color(s_ap_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_ap_qr, lv_color_hex(0xFFFFFF));
    lv_obj_center(s_ap_qr);
    s_ap_ssid = label(p, &lv_font_montserrat_14, COL_ACCENT, "");
    lv_obj_align(s_ap_ssid, LV_ALIGN_TOP_MID, 0, 196);
    s_ap_steps = info_label(p, 218);
    lv_obj_set_style_text_line_space(s_ap_steps, 2, 0);
    lv_obj_set_style_text_color(s_ap_steps, lv_color_hex(COL_MUTED), 0);
    lv_label_set_text(s_ap_steps, "1. 手机扫码连接此热点\n"
                                  "2. 在弹出的网页选网络、填密码\n"
                                  "未弹出请访问 192.168.4.1");

    p = s_pages[BP_PAGE_ACCOUNT] = page();
    page_title(p, "网盘账号");
    s_acct_state = info_label(p, 80);
    pill(p, 172, LV_SYMBOL_REFRESH, "", &item, &icon, &s_acct_action);
    pill_select(item, icon, s_acct_action, true);
    s_acct_hint = info_label(p, 224);
    lv_obj_set_style_text_color(s_acct_hint, lv_color_hex(COL_MUTED), 0);

    p = s_pages[BP_PAGE_ABOUT] = page();
    page_title(p, "关于与按键");
    lv_obj_t *about = info_label(p, 74);
    lv_label_set_text(about,
        "云盘随身听 v" BP_APP_VERSION "\n"
        "上/下：选择 · 调音量\n"
        "OK：确认 · 暂停/继续\n"
        "播放页双击 OK：停止\n"
        "长按 OK：返回上一级\n"
        "长按上/下：上一首/下一首\n"
        "首页长按 OK：回到播放页\n"
        "支持 MP3、WAV(16 位)");
}

// ---- 刷新 -------------------------------------------------------------------------
static void refresh_status(void) {
    char buf[48];
    int soc = bsp_battery_soc();
    const char *batt = soc < 0 ? "" : soc > 80 ? LV_SYMBOL_BATTERY_FULL : soc > 55 ? LV_SYMBOL_BATTERY_3
                     : soc > 30 ? LV_SYMBOL_BATTERY_2 : soc > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    bp_player_info_t pi;
    bp_player_get_info(&pi);
    const char *play = pi.state == BP_PLAY_PLAYING ? LV_SYMBOL_PLAY " " :
                       pi.state == BP_PLAY_PAUSED ? LV_SYMBOL_PAUSE " " : "";
    if (soc >= 0) snprintf(buf, sizeof(buf), "%s%s  %s %d%%", play, g_bp.wifi_up ? LV_SYMBOL_WIFI : "", batt, soc);
    else snprintf(buf, sizeof(buf), "%s%s", play, g_bp.wifi_up ? LV_SYMBOL_WIFI : "");
    set_text_if(s_status, buf);
    lv_obj_set_style_text_color(s_status, lv_color_hex(g_bp.wifi_up ? COL_MUTED : COL_DANGER), 0);
}

static void refresh_toast(void) {
    int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_toast_mux);
    bool pending = s_toast_pending;
    char text[sizeof(s_toast_text)];
    if (pending) {
        memcpy(text, s_toast_text, sizeof(text));
        s_toast_pending = false;
    }
    portEXIT_CRITICAL(&s_toast_mux);
    if (pending) {
        lv_label_set_text(s_toast_label, text);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = now + TOAST_MS;
    } else if (s_toast_until && now > s_toast_until) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = 0;
    }
}

static void refresh_home(void) {
    bp_player_info_t pi;
    bp_player_get_info(&pi);
    char buf[32];
    bool has_track = pi.state != BP_PLAY_IDLE && pi.name[0];
    set_text_if(s_home_card_title, has_track ? state_text(&pi, buf, sizeof(buf)) : "未在播放");
    set_text_if(s_home_card_name, has_track ? pi.name :
                bp_baidu_state() == BP_BD_READY ? "选一首网盘里的歌吧" : "先在设置里授权网盘");
    lv_bar_set_value(s_home_card_bar, pi.total_ms ? (int32_t)((uint64_t)pi.pos_ms * 1000 / pi.total_ms) : 0,
                     LV_ANIM_OFF);
    set_text_if(s_home_card_icon, pi.state == BP_PLAY_PAUSED ? LV_SYMBOL_PAUSE : LV_SYMBOL_AUDIO);
    for (int i = 0; i < HOME_ITEMS; i++)
        pill_select(s_home_items[i], s_home_icons[i], s_home_texts[i], i == s_sel[BP_PAGE_HOME]);
}

static bool list_changed(const bp_list_t *a, const bp_list_t *b) {
    return a->status != b->status || a->count != b->count || a->has_more != b->has_more ||
           a->req.page != b->req.page || a->req.start != b->req.start ||
           a->req.source != b->req.source ||
           strcmp(a->req.dir, b->req.dir) ||
           (a->count && a->files[0].fs_id != b->files[0].fs_id);
}

// 虚拟行:[上一页] 文件... [下一页]
static bp_row_kind_t row_kind(int row, int *file_index) {
    int r = row;
    bool prev = s_list.req.page > 0;
    if (prev) {
        if (r == 0) return BP_ROW_PREV;
        r--;
    }
    if (r < s_list.count) {
        if (file_index) *file_index = r;
        return BP_ROW_FILE;
    }
    if (r == s_list.count && s_list.has_more) return BP_ROW_NEXT;
    return BP_ROW_NONE;
}

static void refresh_list(void) {
    static bp_list_t snap;   // 约 1.7 KiB,不放在 LVGL 任务栈上
    bp_baidu_list_snapshot(&snap);
    if (list_changed(&snap, &s_list)) {
        s_list = snap;
        s_list_rows = s_list.count + (s_list.req.page > 0) + (s_list.has_more ? 1 : 0);
        if (s_sel[BP_PAGE_LIST] >= s_list_rows) s_sel[BP_PAGE_LIST] = 0;
    }
    char buf[64];
    if (s_list.req.source == BP_SRC_ALL_AUDIO) set_text_if(s_list_title, "全部音频");
    else set_text_if(s_list_title, !strcmp(s_list.req.dir, "/") ? "我的网盘" : bp_path_basename(s_list.req.dir));
    snprintf(buf, sizeof(buf), "第 %d 页", s_list.req.page + 1);
    set_text_if(s_list_sub, buf);

    const char *msg = NULL;
    if (s_list.status == 1 || s_list.status == 0) msg = "正在读取网盘…";
    else if (s_list.status < 0) msg = "读取失败\n按 OK 重试";
    else if (s_list_rows == 0) msg = s_list.req.source == BP_SRC_ALL_AUDIO ?
                                     "网盘里还没有 MP3/WAV\n按 OK 刷新" : "这里没有可播放的音频";
    if (msg) {
        set_text_if(s_list_msg, msg);
        lv_obj_remove_flag(s_list_msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_list_msg, LV_OBJ_FLAG_HIDDEN);
    }

    int sel = s_sel[BP_PAGE_LIST];
    int first = sel - LIST_ROWS / 2;
    if (first > s_list_rows - LIST_ROWS) first = s_list_rows - LIST_ROWS;
    if (first < 0) first = 0;
    for (int i = 0; i < LIST_ROWS; i++) {
        int row = first + i;
        int fi = -1;
        bp_row_kind_t kind = msg ? BP_ROW_NONE : row_kind(row, &fi);
        if (kind == BP_ROW_NONE) {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
        bool on = row == sel;
        const char *icon = LV_SYMBOL_FILE, *text = "";
        uint32_t icon_col = COL_MUTED, text_col = COL_TEXT;
        if (kind == BP_ROW_PREV) { icon = LV_SYMBOL_LEFT; text = "上一页"; text_col = COL_MUTED; }
        else if (kind == BP_ROW_NEXT) { icon = LV_SYMBOL_RIGHT; text = "下一页"; text_col = COL_MUTED; }
        else {
            const bp_file_t *f = &s_list.files[fi];
            text = f->name;
            if (f->is_dir) { icon = LV_SYMBOL_DIRECTORY; icon_col = COL_ACCENT; }
            else if (bp_media_format(f->name) != BP_FMT_UNKNOWN) { icon = LV_SYMBOL_AUDIO; icon_col = COL_OK; }
            else text_col = COL_MUTED;   // 不支持的格式置灰
        }
        set_text_if(s_row_icons[i], icon);
        set_text_if(s_row_texts[i], text);
        lv_obj_set_style_bg_color(s_rows[i], lv_color_hex(on ? COL_SURFACE2 : COL_BG), 0);
        lv_obj_set_style_border_color(s_rows[i], lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_border_width(s_rows[i], on ? 1 : 0, 0);
        lv_obj_set_style_text_color(s_row_icons[i], lv_color_hex(icon_col), 0);
        lv_obj_set_style_text_color(s_row_texts[i], lv_color_hex(text_col), 0);
        // 选中行文字过长时滚动显示,其余行省略号。
        lv_label_long_mode_t mode = on ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR : LV_LABEL_LONG_MODE_DOTS;
        if (lv_label_get_long_mode(s_row_texts[i]) != mode) lv_label_set_long_mode(s_row_texts[i], mode);
    }
}

static void refresh_player(void) {
    bp_player_info_t pi;
    bp_player_get_info(&pi);
    char buf[48], t[16];
    bp_format_time(pi.pos_ms, t, sizeof(t));
    set_text_if(s_pl_time, t);
    if (pi.total_ms) {
        bp_format_time(pi.total_ms, t, sizeof(t));
        snprintf(buf, sizeof(buf), "/ %s", t);
    } else {
        strlcpy(buf, "/ --:--", sizeof(buf));
    }
    set_text_if(s_pl_total, buf);
    uint32_t prog = pi.total_ms ? (uint32_t)((uint64_t)pi.pos_ms * 1000 / pi.total_ms) : 0;
    lv_arc_set_value(s_arc, (int32_t)(prog > 1000 ? 1000 : prog));
    set_text_if(s_pl_state, state_text(&pi, buf, sizeof(buf)));
    lv_obj_set_style_text_color(s_pl_state, lv_color_hex(pi.state == BP_PLAY_ERROR ? COL_DANGER : COL_MUTED), 0);
    set_text_if(s_pl_icon, pi.state == BP_PLAY_PAUSED ? LV_SYMBOL_PAUSE :
                           pi.state == BP_PLAY_ERROR ? LV_SYMBOL_WARNING : LV_SYMBOL_PLAY);
    set_text_if(s_pl_name, pi.name[0] ? pi.name : "未在播放");
    if (pi.count && pi.kbps) snprintf(buf, sizeof(buf), "%d / %d · %lu kbps", pi.index + 1, pi.count,
                                      (unsigned long)pi.kbps);
    else if (pi.count) snprintf(buf, sizeof(buf), "%d / %d", pi.index + 1, pi.count);
    else buf[0] = 0;
    set_text_if(s_pl_meta, buf);
    lv_bar_set_value(s_pl_vol_bar, g_bp.volume, LV_ANIM_OFF);
}

static void refresh_auth(void) {
    char url[96], code[16], buf[48];
    bp_baidu_get_auth(url, sizeof(url), code, sizeof(code));
    bool ready = url[0] && code[0];
    if (ready && strcmp(url, s_auth_shown)) {
        lv_qrcode_update(s_auth_qr, url, strlen(url));
        strlcpy(s_auth_shown, url, sizeof(s_auth_shown));
    }
    if (ready) lv_obj_remove_flag(s_auth_qr_box, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_auth_qr_box, LV_OBJ_FLAG_HIDDEN);
    snprintf(buf, sizeof(buf), "%s", ready ? code : "");
    set_text_if(s_auth_code, buf);
    const char *hint;
    if (bp_baidu_state() == BP_BD_READY) hint = "授权成功";
    else if (!g_bp.wifi_up) hint = "请先连接 Wi-Fi\n设置 → 无线网络";
    else if (ready) hint = "手机扫码，登录百度账号并确认授权";
    else hint = "正在获取授权码…";
    set_text_if(s_auth_hint, hint);
    lv_obj_set_y(s_auth_hint, ready ? 262 : 140);
}

static void refresh_settings(void) {
    for (int i = 0; i < SETTINGS_ITEMS; i++)
        pill_select(s_set_items[i], s_set_icons[i], s_set_texts[i], i == s_sel[BP_PAGE_SETTINGS]);
}

static void refresh_wifi(void) {
    char ssid[33] = "", ip[20] = "", buf[256];
    int saved = bp_wifi_saved_count();
    if (g_bp.wifi_up) {
        bp_wifi_get_ssid(ssid, sizeof(ssid));
        bp_wifi_get_ip(ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "已连接 %s\nIP %s\n信号 %d dBm\n已保存 %d 个网络",
                 ssid, ip, bp_wifi_get_rssi(), saved);
    } else {
        snprintf(buf, sizeof(buf), "%s\n已保存 %d 个网络\n\n按 OK 开启热点，用手机网页配网",
                 saved ? "正在连接…" : "未配置网络", saved);
    }
    set_text_if(s_wifi_info, buf);
}

static void refresh_wifi_ap(void) {
    char ssid[33], payload[64];
    bp_wifi_config_ssid(ssid, sizeof(ssid));
    if (ssid[0] && strcmp(ssid, s_ap_shown)) {
        // 标准 Wi-Fi 二维码:手机相机扫描即可加入开放热点。
        int n = snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", ssid);
        lv_qrcode_update(s_ap_qr, payload, (uint32_t)n);
        strlcpy(s_ap_shown, ssid, sizeof(s_ap_shown));
    }
    set_text_if(s_ap_ssid, ssid);
}

static void refresh_account(void) {
    bp_bd_state_t st = bp_baidu_state();
    set_text_if(s_acct_state, st == BP_BD_READY ? "状态：已授权\n可以浏览和播放网盘音频" :
                              "状态：未授权\n需要扫码授权后才能使用");
    set_text_if(s_acct_action, st == BP_BD_READY ? (s_acct_armed ? "确认退出" : "退出登录") : "扫码授权");
    set_text_if(s_acct_hint, st == BP_BD_READY ?
                (s_acct_armed ? "再按一次 OK 退出\n网盘文件不受影响" : "退出只清除本机授权") :
                "按 OK 显示授权二维码");
}

static void ui_timer_cb(lv_timer_t *t) {
    (void)t;
    refresh_status();
    refresh_toast();
    // 授权完成后自动回到首页。
    if (s_page == BP_PAGE_AUTH && bp_baidu_state() == BP_BD_READY) {
        bp_ui_goto(BP_PAGE_HOME);
        bp_ui_toast("网盘授权成功");
    }
    // 网页配网完成(完成页请求 /exit 后热点关闭)后回到网络页看连接结果。
    if (s_page == BP_PAGE_WIFI_AP && !bp_wifi_config_active()) {
        bp_ui_goto(BP_PAGE_WIFI);
        bp_ui_toast("配网完成，正在连接");
    }
    switch (s_page) {
        case BP_PAGE_HOME: refresh_home(); break;
        case BP_PAGE_LIST: refresh_list(); break;
        case BP_PAGE_PLAYER: refresh_player(); break;
        case BP_PAGE_AUTH: refresh_auth(); break;
        case BP_PAGE_SETTINGS: refresh_settings(); break;
        case BP_PAGE_WIFI: refresh_wifi(); break;
        case BP_PAGE_ACCOUNT: refresh_account(); break;
        case BP_PAGE_WIFI_AP: refresh_wifi_ap(); break;
        default: break;
    }
}

// ---- 公共接口 -------------------------------------------------------------------
void bp_ui_init(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    build_home();
    build_list();
    build_player();
    build_auth();
    build_settings();
    build_info_pages();
    build_top_layer();
    lv_obj_remove_flag(s_pages[BP_PAGE_HOME], LV_OBJ_FLAG_HIDDEN);
    ui_timer_cb(NULL);
    lv_timer_create(ui_timer_cb, 200, NULL);
}

void bp_ui_goto(bp_page_t page) {
    if (page >= BP_PAGE_COUNT) return;
    if (page != s_page) {
        lv_obj_add_flag(s_pages[s_page], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_pages[page], LV_OBJ_FLAG_HIDDEN);
        s_page = page;
    }
    if (page == BP_PAGE_ACCOUNT) s_acct_armed = false;
    if (page == BP_PAGE_AUTH) s_auth_shown[0] = 0;
    if (page == BP_PAGE_WIFI_AP) s_ap_shown[0] = 0;
    switch (page) {   // 立即刷新,不等定时器
        case BP_PAGE_HOME: refresh_home(); break;
        case BP_PAGE_LIST: refresh_list(); break;
        case BP_PAGE_PLAYER: refresh_player(); break;
        case BP_PAGE_AUTH: refresh_auth(); break;
        case BP_PAGE_SETTINGS: refresh_settings(); break;
        case BP_PAGE_WIFI: refresh_wifi(); break;
        case BP_PAGE_ACCOUNT: refresh_account(); break;
        case BP_PAGE_WIFI_AP: refresh_wifi_ap(); break;
        default: break;
    }
}

bp_page_t bp_ui_page(void) { return s_page; }

void bp_ui_toast(const char *text) {
    portENTER_CRITICAL(&s_toast_mux);
    strlcpy(s_toast_text, text ? text : "", sizeof(s_toast_text));
    s_toast_pending = true;
    portEXIT_CRITICAL(&s_toast_mux);
}

void bp_ui_move(int delta) {
    int count = s_page == BP_PAGE_HOME ? HOME_ITEMS :
                s_page == BP_PAGE_SETTINGS ? SETTINGS_ITEMS :
                s_page == BP_PAGE_LIST ? s_list_rows : 0;
    if (count <= 0) return;
    int sel = s_sel[s_page] + delta;
    // 单步循环;大步(长按)夹到两端。
    if (delta == 1 || delta == -1) sel = bp_playlist_move(s_sel[s_page], count, delta, true);
    else if (sel < 0) sel = 0;
    else if (sel >= count) sel = count - 1;
    s_sel[s_page] = sel;
    bp_ui_goto(s_page);
}

int bp_ui_selected(void) { return s_sel[s_page]; }

bp_row_kind_t bp_ui_list_row(int *file_index) {
    if (s_list.status != 2 && s_list.status != 1) return BP_ROW_RETRY;
    if (s_list.status == 1) return BP_ROW_NONE;
    if (s_list_rows == 0) return BP_ROW_RETRY;
    return row_kind(s_sel[BP_PAGE_LIST], file_index);
}

void bp_ui_list_reset_sel(void) { s_sel[BP_PAGE_LIST] = 0; }

void bp_ui_account_arm(bool armed) {
    s_acct_armed = armed;
    if (s_page == BP_PAGE_ACCOUNT) refresh_account();
}
