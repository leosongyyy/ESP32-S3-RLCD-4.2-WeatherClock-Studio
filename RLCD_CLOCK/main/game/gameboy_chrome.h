// 模拟器运行时的两侧 Game Boy 风格装饰栏（"机身"）。
//
// 背景：GB 画面是 160x144 放大 2 倍 = 320x288，居中放在 400x300 面板上，
// 左右各剩 40px、下方剩 12px，原来整片留黑，看起来就是两条黑边。本模块把
// 左右这两条 40x300 画成 DMG 机身：左边放电池 + 十字键 + 日期，右边放时间
// + A/B 键 + 手柄连接 + 喇叭栅格 + 音量，再点阵纹理填满剩余空间。
//
// ⚠ 不能用 LVGL：模拟器运行时整屏归门闸独占，从 game 任务跨任务调 LVGL 必崩
//   （见 gameboy_fb.cpp 里 game_fb_draw_pause_badge() 的说明）。所以这里全部
//   是写面板位，自带一套 5x7 点阵字库和圆/斜线图元。
//
// ⚠ 推屏策略：侧栏内容只在「分钟变了 / 电量变了 / 手柄状态变了 / 音量变了」
//   时才重画并重推，平时每帧只推中间的 GB 窗口（见 game_fb_present）。
//   一次侧栏推送 = 2 段 x 20 列 x 75 字节 = 3000 字节，约 5ms，不能每帧做。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 只把两条侧栏画进面板帧缓冲，**不推屏**。调用方负责 present。
// 适合紧跟整屏推屏（game_fb_prepare_surface）的场景，省一次局部刷新。
void game_chrome_draw_all(void);

// 只把两条侧栏所在的列区间推到面板（x 0..39 与 360..399）。
void game_chrome_present(void);

// 内容签名变了才重画 + 重推，返回是否真的推过。force=true 无条件重画。
// 由模拟器主循环每秒调一次；没变化就什么都不做，不占 SPI。
bool game_chrome_refresh(bool force);

#ifdef __cplusplus
}
#endif
