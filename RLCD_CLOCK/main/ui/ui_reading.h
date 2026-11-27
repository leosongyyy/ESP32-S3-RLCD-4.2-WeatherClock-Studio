// 阅读模块：书库页（BOOKS）与阅读页（READING）共用一张整屏 canvas。
// 版式按 400x300 反射屏 1bit 排定，中文用 zh_font_16（全 CJK），
// 西文小字用 fusion_pixel_12 / fusion_pixel_8，封面是 tools/gen_book_covers.py
// 生成的 packed 点阵。
//
// 数据来源：book_store 扫描 TF 卡 /sdcard/book 下的 .txt（UTF-8/UTF-16/GB2312
// 自动识别），在数据层完成排版分页，本模块只负责绘制。
// .epub 本轮只列进书库，打开时提示暂不支持。
#pragma once

#include "lvgl.h"

#include <time.h>

// 当前视图：书库列表还是正文。
enum class ReadingView {
    kShelf,
    kReader,
};

void build_reading_ui();
lv_obj_t *reading_root();

bool reading_shown();
void reading_show();
void reading_hide();

ReadingView reading_view();
// 视图/选中/页码每变一次就 +1。ui_task 靠它判断要不要重画，
// 输入任务改状态后只需 notify_ui_task()，不必共享静态变量。
uint32_t reading_state_version();
// 打开当前选中的书。失败时不切视图，把原因显示在书库题头右侧。
void reading_open_selected();
// 从正文退回书库；已在书库时返回 false，由调用方回主菜单。
bool reading_back_to_shelf();

// 书库选中项移动，dx/dy 为方向。左右越界时自动翻一页书架。
void reading_move_selection(int dx, int dy);
int reading_selection();

// 正文翻页：delta 为 +1/-1，越界不动作。
void reading_turn_page(int delta);

// ---------- 书签 / 目录 ----------
// 目录与书签共用一个弹层，同一时刻只开一个。
bool reading_panel_shown();
// 打开弹层，tab 0=目录 1=书签。默认落到当前阅读位置附近。
void reading_panel_open(int tab);
void reading_panel_close();
// 弹层内上下移动选中项，越界不动作。
void reading_panel_move(int delta);
// 弹层内左右切换 tab（-1 上一个 / +1 下一个）。
void reading_panel_switch_tab(int dir);
// 跳到选中项并关闭弹层；没有可跳条目时不动。
void reading_panel_activate();

// 当前页加/减书签。返回 1 新增、0 移除、-1 已满或不可用。
int reading_toggle_bookmark();

// 弹层里长按 BOOT：目录页签给当前阅读页加/删书签，书签页签删掉选中那条。
// 弹层没开时什么都不做。
void reading_panel_bookmark();

// ---------- 全屏阅读 ----------
// 全屏时不画状态栏、题头、右栏和底栏，正文重新分页铺满整屏。
// 只对正文视图有效，书库里调用会被忽略。
bool reading_fullscreen();
void reading_toggle_fullscreen();

// 整页重画：书库或正文，取决于当前视图。首次进入会挂卡并扫描书库。
void reading_redraw_all(const struct tm *local, int battery_percent);
// 状态栏单独局刷（分钟或电量变化时）。
void reading_refresh_status_bar(const struct tm *local, int battery_percent);
// 书库选中项变化：只重画新旧两格。
void reading_refresh_selection(int previous_index);
