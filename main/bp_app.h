// main/bp_app.h —— 云盘随身听(Baidu Netdisk Player)应用层公共定义。
//
// 模块划分:
//   main.c        初始化、按键分发、页面状态机、心跳
//   bp_ui.c       LVGL 界面(全部页面,只在持有 LVGL 锁时调用)
//   bp_baidu.c    百度网盘:设备码授权、令牌刷新、目录/音频列表、下载直链
//   bp_player.c   流式播放:HTTP 下载任务 → 环形缓冲 → MP3/WAV 解码 → I2S
//   bp_media.c    纯逻辑(主机单测)
//   bp_console.c  USB 串口命令(配网、状态、模拟按键)
//   bp_wifi.cc    esp-wifi-connect 的 C 桥接
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bp_media.h"

#define BP_APP_VERSION "0.1.0"
#define BP_NAME_MAX 96
#define BP_PAGE_SIZE 12       // 每次向网盘请求的条目数(受 JSON 内存预算约束)
#define BP_PLAYLIST_MAX BP_PAGE_SIZE

typedef struct {
    uint64_t fs_id;
    uint64_t size;
    bool is_dir;
    char name[BP_NAME_MAX];
} bp_file_t;

// 列表来源:目录浏览,或网盘内全部 MP3/WAV(按修改时间倒序)。
typedef enum {
    BP_SRC_DIR = 0,
    BP_SRC_ALL_AUDIO,
} bp_source_t;

// 游标翻页:浏览目录时会过滤掉不支持的文件,所以页码不能换算成接口偏移;
// start 为本页在网盘接口里的起始偏移,next_start 为下一页的起始偏移。
#define BP_PAGE_HISTORY 64

typedef struct {
    bp_source_t source;
    int page;              // 显示用页码(0 起)
    uint32_t start;        // 接口偏移
    char dir[BP_PATH_MAX];
} bp_list_req_t;

// 列表状态:0 未加载,1 加载中,2 就绪,-1 出错。
typedef struct {
    bp_list_req_t req;
    int status;
    int count;
    bool has_more;
    uint32_t next_start;
    bp_file_t files[BP_PAGE_SIZE];
} bp_list_t;

typedef enum {
    BP_PAGE_HOME = 0,
    BP_PAGE_LIST,
    BP_PAGE_PLAYER,
    BP_PAGE_AUTH,
    BP_PAGE_SETTINGS,
    BP_PAGE_WIFI,
    BP_PAGE_ACCOUNT,
    BP_PAGE_ABOUT,
    BP_PAGE_WIFI_AP,      // SoftAP 网页配网
    BP_PAGE_COUNT,
} bp_page_t;

// 网盘授权状态。
typedef enum {
    BP_BD_NO_AUTH = 0,
    BP_BD_WAIT_CODE,
    BP_BD_READY,
} bp_bd_state_t;

typedef enum {
    BP_PLAY_IDLE = 0,
    BP_PLAY_RESOLVING,    // 获取下载直链
    BP_PLAY_BUFFERING,
    BP_PLAY_PLAYING,
    BP_PLAY_PAUSED,
    BP_PLAY_ERROR,
} bp_play_state_t;

typedef struct {
    bp_play_state_t state;
    char name[BP_NAME_MAX];
    uint32_t pos_ms;
    uint32_t total_ms;     // 0 = 未知
    int index;             // 播放列表下标
    int count;             // 播放列表长度
    uint32_t rate;
    uint16_t channels;
    uint32_t kbps;
    uint8_t buffer_pct;    // 环形缓冲填充度
    const char *error;     // BP_PLAY_ERROR 时的中文原因
} bp_player_info_t;

typedef struct {
    volatile bool wifi_up;
    volatile bool screen_off;
    volatile uint8_t volume;   // 0..100
} bp_state_t;

extern bp_state_t g_bp;

// ---- UI(除 bp_ui_toast 外,调用方必须持有 bsp_lvgl_lock) -----------------------
typedef enum {
    BP_ROW_NONE = 0,
    BP_ROW_FILE,      // *file_index 为列表下标
    BP_ROW_PREV,
    BP_ROW_NEXT,
    BP_ROW_RETRY,     // 列表加载失败或为空时的重试
} bp_row_kind_t;

void bp_ui_init(void);
void bp_ui_goto(bp_page_t page);
bp_page_t bp_ui_page(void);
void bp_ui_toast(const char *text);             // 任意任务可调用(内部只拷贝)
void bp_ui_move(int delta);                     // 当前页选中项移动
int  bp_ui_selected(void);                      // 首页/设置页选中项
bp_row_kind_t bp_ui_list_row(int *file_index);  // 列表页选中行
void bp_ui_list_reset_sel(void);
void bp_ui_account_arm(bool armed);             // 账号页“再按一次确认退出”
// ---- 百度网盘 ----------------------------------------------------------------
void bp_baidu_init(void);
bp_bd_state_t bp_baidu_state(void);
int  bp_baidu_auth_start(void);     // 0=已开始 1=已授权 -2=无网络 -3=忙
void bp_baidu_auth_cancel(void);
void bp_baidu_get_auth(char *url, size_t ucap, char *code, size_t ccap);
int  bp_baidu_logout(void);         // 清除本地令牌;0 成功
void bp_baidu_on_wifi(bool up);

// 异步列表(UI 用):结果写入内部 bp_list_t,用 bp_baidu_list_snapshot 读取。
int  bp_baidu_list_request(const bp_list_req_t *req);   // 0=已派发 -1 未就绪 -3 忙
void bp_baidu_list_snapshot(bp_list_t *out);
// 同步列表(播放器自动续播用,调用方须在工作任务中)。0 成功。
int  bp_baidu_list_fetch(const bp_list_req_t *req, bp_list_t *out);
// 取文件下载直链(含 access_token)。0 成功;out 为 malloc 字符串,调用方 free。
int  bp_baidu_dlink(uint64_t fs_id, char **out);

// ---- 播放器 ------------------------------------------------------------------
void bp_player_init(void);
// 以列表快照为播放列表,从 index 开始播放(只取可播放文件)。0=已派发。
int  bp_player_play_list(const bp_list_t *list, int index);
void bp_player_toggle_pause(void);
void bp_player_next(void);
void bp_player_prev(void);
void bp_player_stop(void);
void bp_player_set_volume(uint8_t volume);
void bp_player_get_info(bp_player_info_t *out);
bool bp_player_active(void);        // 有曲目在缓冲/播放/暂停

// ---- 串口与测试钩子 ----------------------------------------------------------
void bp_console_start(void);
int  bp_test_key(int button, bool long_press);   // 0 UP, 1 OK, 2 DOWN
int  bp_test_page(void);

// ---- Wi-Fi 桥接 --------------------------------------------------------------
#include "bp_wifi.h"
