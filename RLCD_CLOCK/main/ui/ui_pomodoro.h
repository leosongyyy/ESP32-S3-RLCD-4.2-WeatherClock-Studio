// 番茄钟工作页：状态栏 + 标题 + 圆环倒计时 + 番茄统计 + MODE/STATS/NEXT + 按键栏。
// 版式逐点复刻 tests/pomodoro_preview.py，写进一张 400x300 LVGL canvas。
// 本文件只负责绘制与分区局刷，计时/输入在后续阶段接入。
#pragma once

#include "lvgl.h"
#include "pomodoro/pomodoro_classic.h"

// 一帧要画的番茄钟数据。UI 层不持有计时逻辑，由外部填好后传给绘制接口。
struct PomodoroView {
    char remaining[16];    // 倒计时文本，如 "18:42"
    char today[40];        // "完成 3 个番茄"
    char stats[64];        // "累计 1h 15m · 中断 0 次"
    char next[80];         // "下一次  短休 05:00 后自动开始"
    int progress_done;     // 圆环已点亮的格数 0..30
    int selected_mode;     // MODE 选中行 0=FOCUS 1=SHORT 2=LONG
    int session;           // 当前第几节（阶段切换 +1）
    int today_count;       // 今日已完成番茄数（实心小番茄个数 0..4）
    bool running;          // 运行中=true 显示 phase_zh，否则显示 "已暂停"
    const char *phase_en;  // FOCUS / SHORT BREAK / LONG BREAK
    const char *phase_zh;  // 专注中 / 短休中 / 长休中
};

// 用一组默认占位数据填充 view（与预览首帧一致），供未接计时时显示。
void pomodoro_view_defaults(PomodoroView *view);

// 从计时核心快照填充 view（接真实计时后使用）。
void pomodoro_view_from_snapshot(PomodoroView *view, const ClassicSnapshot *snap);

// 建立 canvas 对象树（幂等）。必须在持有 LVGL 锁的 ui_task 里调用。
void build_pomodoro_ui();
lv_obj_t *pomodoro_root();

// 显示状态（供视图循环与输入任务共享，线程安全）。
bool pomodoro_shown();
void pomodoro_show();
void pomodoro_hide();

// 全量重画（首次构建/从主页进入时用），标脏整屏。
void pomodoro_redraw_all(const PomodoroView *view, const struct tm *local,
                         int battery_percent);

// 分区局刷（均须在持 LVGL 锁时调用，内部只标脏自己那一小块）：
// 倒计时区每秒刷一次；状态区仅在数据变化时刷。
// 状态栏（时间/电量/图标）单独局刷，分钟变化或电量变化时调用。
void pomodoro_refresh_status_bar(const struct tm *local, int battery_percent);

// 标题区（SESSION 第 N 节）：仅在节数变化时局刷。
void pomodoro_refresh_title(const PomodoroView *view);

void pomodoro_refresh_countdown(const PomodoroView *view, int battery_percent = -999);
void pomodoro_refresh_today(const PomodoroView *view);
void pomodoro_refresh_mode(const PomodoroView *view);
void pomodoro_refresh_stats(const PomodoroView *view);
void pomodoro_refresh_next(const PomodoroView *view);
