// 声明游戏工作页：idle 态 ROM 列表（LVGL）与运行态交给模拟器任务。
#pragma once

#include <stdbool.h>
#include <time.h>

// 构建游戏页对象树（幂等）。
void build_game_page();

// 周期刷新：仅在游戏未运行时重绘列表。
bool update_game_page(const struct tm &local);

// 逃生守卫：低电量 / OTA 升级 / 配网页激活 / 等待重启时强制停止模拟器。
//
// 必须由 ui_task 主循环**每轮无条件**调用（在拿 LVGL 锁之前），不能挂在
// update_game_page() 里：低电量与配网页激活时 ui_views 会把工作页强切到天气钟，
// 且 active_work_page_state_for_mode() 会把 state.game 置 false，
// update_game_page() 根本不会被调用 —— 那样三种场景里有两种永远退不出来。
// 本函数只读状态 + 写一次日志 + 置一条待展示文案，不碰 LVGL。
void game_page_guard_runtime_stop();

// 展示逃生守卫攒下的退出理由（全局横幅 + 游戏页提示行）。
// 必须在持有 LVGL 锁时调用，且每轮都要调（负责横幅自动消失）。
// 返回 true 表示本次改动了屏幕内容，调用方需要触发一次刷新。
bool game_page_guard_tick_ui();

// 以下操作只改状态、不碰 LVGL 对象，可从按键任务安全调用。
bool game_page_next_rom();
bool game_page_prev_rom();
bool game_page_start_selected();
int game_page_rom_count();
int game_page_selected_index();

// 焦点在 ROM 行与音量行之间循环（音量行是列表的第 5 个可选项）。
// next_rom()/prev_rom() 保留给纯 rom_store 导航，不带焦点概念。
bool game_page_volume_focused();
bool game_page_focus_next();
bool game_page_focus_prev();
// 调音量时会把焦点拉到音量行，让档位变化在屏幕上看得见。
bool game_page_volume_step_up();
bool game_page_volume_step_down();
