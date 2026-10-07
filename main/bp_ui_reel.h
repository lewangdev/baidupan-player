// main/bp_ui_reel.h —— 磁带风格播放界面(由 bp_ui.c 调用,均在 LVGL 任务内/持有锁时执行)。
#pragma once

#include "bp_app.h"
#include "lvgl.h"

// 在 parent(240x320 的播放页容器)中创建全部对象。
void bp_reel_build(lv_obj_t *parent);
// 文字与状态(约 200 ms 一次)。
void bp_reel_refresh(const bp_player_info_t *pi);
// 动画(转轮、走带线与鱼的声波),按经过的毫秒推进;静止后不再重绘。
void bp_reel_tick(uint32_t dt_ms, const bp_player_info_t *pi);
// 长按切歌:转轮高速快进(+1)或倒带(-1)约 0.7 秒。
void bp_reel_kick(int dir);
// 显示音量浮层约 1.4 秒。
void bp_reel_volume(void);
