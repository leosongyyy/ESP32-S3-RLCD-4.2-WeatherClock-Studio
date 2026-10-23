// ui_game_bind.h —— 游戏页的「手柄按键绑定」子模式
//
// 刻意做成游戏页的子模式而不是独立的辅助页：辅助页那套机制要动
// AuxiliaryPage 枚举、ui_views 的显示/恢复分支、runtime_surfaces 快照、
// 以及 input_tasks 里的页面优先级判断，牵一发动全身；而绑定页的生命周期
// 和 ROM 列表完全一样（都在游戏工作页里），复用同一块屏幕和同一条刷新路径
// 最省事，也不会影响别的页面。
//
// 线程约定与 ui_game.h 一致：
//   - open/close/active/next_action/bind_current 只改状态，可在按键任务里调
//   - build/update 必须在持有 LVGL 锁的 ui_task 里调
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 进入/退出绑定页（只改状态）。open 会隐式取消上一次未完成的捕获。
void game_bind_page_open(void);
void game_bind_page_close(void);
bool game_bind_page_active(void);

// 建立对象树，挂在 parent 下面（幂等：重复调用只更新已有对象）。
// 每帧都可以调，内部自己判断是否需要重建。
void game_bind_page_build(lv_obj_t *parent);

// 每帧刷新。返回 true 表示画面有变化、需要触发一次重绘。
// 负责推进捕获状态机显示（等待按键 / 请松手 / 成功 / 失败）。
bool game_bind_page_update(void);

// 按键任务调用：把选中项移到下一个可绑定动作（循环）。
bool game_bind_page_next_action(void);

// 按键任务调用：对当前选中项发起捕获。
bool game_bind_page_bind_current(void);

#ifdef __cplusplus
}
#endif
