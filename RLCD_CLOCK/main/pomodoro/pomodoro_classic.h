// 经典番茄钟核心：25/05/15 阶段流转、时间戳计时、NVS 统计持久化。
// 旧的 pomodoro_services 是小智语音的单调倒计时，两者互不干扰。
// 本模块只维护数据，不碰 UI、不碰输入；UI 层从 snapshot 填 PomodoroView。
#pragma once

#include <stdint.h>
#include <time.h>

enum class ClassicPhase {
    kFocus = 0,       // 专注 25:00
    kShortBreak = 1,  // 短休 05:00
    kLongBreak = 2,   // 长休 15:00
};

enum class ClassicRunState {
    kIdle = 0,
    kRunning,
    kPaused,
};

struct ClassicSnapshot {
    ClassicPhase phase;
    ClassicRunState run_state;
    uint32_t remaining_sec;      // 剩余秒（向上取整，显示用）
    uint32_t phase_duration_sec; // 本阶段总秒数
    int focus_round;             // 本轮已完成专注数 0..4
    int session;                 // 当前第几节（每次阶段切换 +1，跨天归 1）
    int today_count;             // 今日完成番茄数（UI 最多显示 4）
    uint32_t total_focus_sec;    // 累计专注秒数
    uint32_t interrupt_count;    // 中断次数
    int selected_mode;           // MODE 选中行，等于当前 phase

    const char *phase_en;        // FOCUS / SHORT BREAK / LONG BREAK
    const char *phase_zh;        // 专注中 / 短休中 / 长休中
};

// 变化事件位，tick 返回给 UI 决定刷哪一块。
constexpr uint32_t kClassicEvProgress = 1u << 0;  // 倒计时/圆环变化（每秒）
constexpr uint32_t kClassicEvToday = 1u << 1;      // 今日完成数变化
constexpr uint32_t kClassicEvMode = 1u << 2;       // 阶段/选中变化
constexpr uint32_t kClassicEvStats = 1u << 3;      // 累计/中断变化
constexpr uint32_t kClassicEvNext = 1u << 4;       // 下一阶段预告变化

// 启动时调用一次：读 NVS，做跨天清零。
void classic_init();

// 取当前快照（含实时计算的剩余秒，不推进流转）。
void classic_get_snapshot(ClassicSnapshot *out);

// 推进计时与阶段流转。now_us 用 esp_timer_get_time()，now_s 用 time(NULL)。
// 返回变化事件位（可能多位）；UI 拿到后按需局刷。
uint32_t classic_tick(int64_t now_us, time_t now_s);

// A 键：空闲/暂停 -> 开始，运行 -> 暂停（记一次中断）。返回是否有变化。
bool classic_toggle_start_pause();

// 上下键选 MODE：切换到对应阶段并重置为本阶段满时长、空闲态。
// 若原先在运行，记一次中断。
void classic_select_mode(int mode);

// 重置番茄钟：回到专注第 1 节、空闲态、25:00。今日番茄数/累计/中断保留。
void classic_reset();

// 当前阶段时长（秒）。
uint32_t classic_phase_duration(ClassicPhase phase);
