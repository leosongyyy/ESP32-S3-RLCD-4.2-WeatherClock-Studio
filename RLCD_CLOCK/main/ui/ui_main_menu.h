// 主菜单主页：状态栏 + 标题 + 4x2 模块 + 按键提示栏。
// 用取模 Fusion 像素字体 + Lucide 图标点阵，直接画进整屏 canvas。
#pragma once

#include "lvgl.h"

// 8 个模块对应的页面动作（未实现的模块保持占位，不接功能）。
enum class MainMenuAction {
    kNone,          // 空模块 / 无效
    kWeatherClock,  // 天气时钟工作页
    kGameBoy,       // 游戏工作页
    kSettings,      // 设置辅助页
    kPomodoro,      // 番茄钟页
};

void build_main_menu_ui();
lv_obj_t *main_menu_root();

// 主页显示状态（供视图循环与输入任务共享）。
bool main_menu_shown();
void main_menu_show();
void main_menu_hide();

// 选中项导航。
// main_menu_move 供手柄方向键用，走到边界停住。
// main_menu_step 供物理 BOOT 用，按 4x2 行优先顺序在 8 个模块间循环，
// delta=+1 右移（第一排走到头进第二排，第二排走到头回第一排第一个），
// delta=-1 反向循环。
int main_menu_selection();
void main_menu_move(int dx, int dy);   // dx:-1左/+1右, dy:-1上/+1下
void main_menu_step(int delta);        // +1 下一格 / -1 上一格，越过边界回卷
void main_menu_set_selection(int index);

// 当前选中模块对应的动作。
MainMenuAction main_menu_current_action();

// 数据变化时重画状态栏（时间/电量）。返回是否真的重画了。
bool update_main_menu(const struct tm *local, int battery_percent);
