// 经典番茄钟核心实现。详见 pomodoro_classic.h。
#include "pomodoro_classic.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <ctime>

namespace {
constexpr char TAG[] = "pomodoro_classic";
constexpr const char *kNvsNamespace = "pomodoro";
constexpr uint32_t kFocusSec = 25 * 60;
constexpr uint32_t kShortBreakSec = 5 * 60;
constexpr uint32_t kLongBreakSec = 15 * 60;
constexpr int kFocusPerRound = 4;
constexpr int64_t kUsPerSec = 1000000;

struct ClassicState {
    ClassicPhase phase;
    ClassicRunState run_state;
    int64_t deadline_us;            // 运行时的结束时刻（单调时钟）
    uint32_t paused_remaining_sec;  // 暂停时冻结的剩余秒
    int focus_round;                // 本轮已完成专注数 0..4
    int session;                    // 当前第几节（每次阶段切换 +1，跨天归 1）
    int today_count;
    uint32_t total_focus_sec;
    uint32_t interrupt_count;
    uint32_t date_stamp;            // YYYYMMDD
};

ClassicState s_state = {
    ClassicPhase::kFocus,
    ClassicRunState::kIdle,
    0,
    kFocusSec,
    0,
    1,   // session 从第 1 节开始
    0,
    0,
    0,
    0,
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
bool s_initialized = false;

// 上一次 tick 对外报告的剩余秒，用于判定倒计时数字是否需要局刷。
// 不能用 classic_tick 里的 before/after 比较：那两个快照基于同一个 now_us
// 计算，中间没有推进时间，remaining_sec 恒等，事件几乎永不置位。
// 初值设成哨兵，保证第一次 tick 必然置位一次。
uint32_t s_last_reported_remaining_sec = 0xFFFFFFFFu;

uint32_t phase_duration(ClassicPhase phase)
{
    switch (phase) {
    case ClassicPhase::kFocus:      return kFocusSec;
    case ClassicPhase::kShortBreak: return kShortBreakSec;
    case ClassicPhase::kLongBreak:  return kLongBreakSec;
    }
    return kFocusSec;
}

const char *phase_en(ClassicPhase phase)
{
    switch (phase) {
    case ClassicPhase::kFocus:      return "FOCUS";
    case ClassicPhase::kShortBreak: return "SHORT BREAK";
    case ClassicPhase::kLongBreak:  return "LONG BREAK";
    }
    return "FOCUS";
}

const char *phase_zh(ClassicPhase phase)
{
    switch (phase) {
    case ClassicPhase::kFocus:      return "专注中";
    case ClassicPhase::kShortBreak: return "短休中";
    case ClassicPhase::kLongBreak:  return "长休中";
    }
    return "专注中";
}

uint32_t day_key(time_t now_s)
{
    struct tm local = {};
    if (!localtime_r(&now_s, &local)) return 0;
    return static_cast<uint32_t>(local.tm_year + 1900) * 10000u +
           static_cast<uint32_t>(local.tm_mon + 1) * 100u +
           static_cast<uint32_t>(local.tm_mday);
}

// 必须在持锁时调用。
uint32_t remaining_sec_locked(int64_t now_us)
{
    if (s_state.run_state == ClassicRunState::kRunning) {
        int64_t rem_us = s_state.deadline_us - now_us;
        if (rem_us < 0) rem_us = 0;
        return static_cast<uint32_t>((rem_us + kUsPerSec - 1) / kUsPerSec);
    }
    if (s_state.run_state == ClassicRunState::kPaused) {
        return s_state.paused_remaining_sec;
    }
    return phase_duration(s_state.phase);
}

void fill_snapshot_locked(ClassicSnapshot *out, int64_t now_us)
{
    out->phase = s_state.phase;
    out->run_state = s_state.run_state;
    out->remaining_sec = remaining_sec_locked(now_us);
    out->phase_duration_sec = phase_duration(s_state.phase);
    out->focus_round = s_state.focus_round;
    out->session = s_state.session;
    out->today_count = s_state.today_count;
    out->total_focus_sec = s_state.total_focus_sec;
    out->interrupt_count = s_state.interrupt_count;
    out->selected_mode = static_cast<int>(s_state.phase);
    out->phase_en = phase_en(s_state.phase);
    out->phase_zh = phase_zh(s_state.phase);
}

void save_stats()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open for save failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_u32(handle, "today", s_state.today_count);
    nvs_set_u32(handle, "session", static_cast<uint32_t>(s_state.session));
    nvs_set_u32(handle, "total_focus", s_state.total_focus_sec);
    nvs_set_u32(handle, "interrupts", s_state.interrupt_count);
    nvs_set_u32(handle, "date", s_state.date_stamp);
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs commit failed: %s", esp_err_to_name(err));
    }
}

void load_stats()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // 首次运行没有命名空间，属正常情况。
        return;
    }
    uint32_t today = 0, session = 0, total = 0, interrupts = 0, date = 0;
    nvs_get_u32(handle, "today", &today);
    nvs_get_u32(handle, "session", &session);
    nvs_get_u32(handle, "total_focus", &total);
    nvs_get_u32(handle, "interrupts", &interrupts);
    nvs_get_u32(handle, "date", &date);
    nvs_close(handle);
    s_state.today_count = static_cast<int>(today);
    // 历史固件没有 session 键，读到 0 时按第 1 节处理。
    s_state.session = session == 0 ? 1 : static_cast<int>(session);
    s_state.total_focus_sec = total;
    s_state.interrupt_count = interrupts;
    s_state.date_stamp = date;
}

// 阶段到点：完成当前阶段并自动进入下一阶段（立即开始）。
// 必须在持锁时调用。注意临界区内禁止任何日志/锁操作，日志文案通过 log_round
// 带出，由调用方出临界区后再打印（-1 表示本次无需打印）。
void advance_phase_locked(int64_t now_us, int *log_round)
{
    *log_round = -1;
    // 每次阶段切换（专注→休息、休息→专注）节数 +1。
    s_state.session++;
    if (s_state.phase == ClassicPhase::kFocus) {
        s_state.today_count++;
        s_state.total_focus_sec += kFocusSec;
        s_state.focus_round++;
        if (s_state.focus_round >= kFocusPerRound) {
            s_state.focus_round = 0;
            s_state.phase = ClassicPhase::kLongBreak;
            *log_round = -2;   // 长休
        } else {
            s_state.phase = ClassicPhase::kShortBreak;
            *log_round = s_state.focus_round;
        }
    } else {
        // 短休/长休结束都回到专注。
        s_state.phase = ClassicPhase::kFocus;
    }
    s_state.run_state = ClassicRunState::kRunning;
    s_state.deadline_us = now_us + static_cast<int64_t>(phase_duration(s_state.phase)) * kUsPerSec;
    s_state.paused_remaining_sec = phase_duration(s_state.phase);
}

bool snapshot_changed(const ClassicSnapshot &a, const ClassicSnapshot &b)
{
    return a.phase != b.phase ||
           a.run_state != b.run_state ||
           a.today_count != b.today_count ||
           a.total_focus_sec != b.total_focus_sec ||
           a.interrupt_count != b.interrupt_count;
}

} // namespace

uint32_t classic_phase_duration(ClassicPhase phase)
{
    return phase_duration(phase);
}

void classic_init()
{
    // 临界区里只允许纯内存操作。load_stats 的 NVS 读写、day_key 内部的
    // localtime_r（首次调用会拿 newlib 环境锁）都必须在加锁前完成，
    // 否则 lock_acquire 在关中断状态下直接 abort() 导致软重启。
    load_stats();
    time_t now = time(nullptr);
    uint32_t today = day_key(now);

    portENTER_CRITICAL(&s_mux);
    if (s_initialized) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    s_state.phase = ClassicPhase::kFocus;
    s_state.run_state = ClassicRunState::kIdle;
    s_state.focus_round = 0;
    s_state.paused_remaining_sec = kFocusSec;
    s_state.deadline_us = 0;

    bool need_save = false;
    if (today != 0 && s_state.date_stamp != today) {
        // 跨天：今日完成数清零，节数回到第 1 节，累计时长与中断保留。
        s_state.today_count = 0;
        s_state.session = 1;
        s_state.date_stamp = today;
        need_save = true;
    }
    s_initialized = true;
    portEXIT_CRITICAL(&s_mux);

    if (need_save) {
        save_stats();
        ESP_LOGI(TAG, "new day, today_count reset to 0");
    }
}

void classic_get_snapshot(ClassicSnapshot *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    fill_snapshot_locked(out, esp_timer_get_time());
    portEXIT_CRITICAL(&s_mux);
}

uint32_t classic_tick(int64_t now_us, time_t now_s)
{
    uint32_t events = 0;
    bool need_save = false;

    // day_key 内部调用 localtime_r 会拿 newlib 环境锁，必须在临界区外算好。
    uint32_t today = day_key(now_s);

    portENTER_CRITICAL(&s_mux);
    ClassicSnapshot before = {};
    fill_snapshot_locked(&before, now_us);

    // 跨天检查：今日数清零、节数回第 1 节，其余统计保留。
    if (today != 0 && s_state.date_stamp != today) {
        s_state.date_stamp = today;
        s_state.today_count = 0;
        s_state.session = 1;
        need_save = true;
    }

    bool phase_completed = false;
    int log_round = -1;
    if (s_state.run_state == ClassicRunState::kRunning && now_us >= s_state.deadline_us) {
        advance_phase_locked(now_us, &log_round);
        phase_completed = true;
        need_save = true;
    }

    ClassicSnapshot after = {};
    fill_snapshot_locked(&after, now_us);
    portEXIT_CRITICAL(&s_mux);

    // 日志必须在出临界区后打印（vprintf 会拿 newlib 锁）。
    if (log_round == -2) {
        ESP_LOGI(TAG, "focus round complete -> long break");
    } else if (log_round >= 0) {
        ESP_LOGI(TAG, "focus complete -> short break (%d/%d)", log_round, kFocusPerRound);
    } else if (phase_completed) {
        ESP_LOGI(TAG, "break complete -> focus");
    }

    // 倒计时局刷判定：和上一次对外报告的剩余秒比，而不是同一时刻的
    // before/after（那两个恒等）。每秒 remaining 减 1 时会正确置位。
    if (after.remaining_sec != s_last_reported_remaining_sec || phase_completed) {
        events |= kClassicEvProgress;
    }
    s_last_reported_remaining_sec = after.remaining_sec;
    if (snapshot_changed(before, after)) {
        if (before.today_count != after.today_count) events |= kClassicEvToday;
        if (before.phase != after.phase || before.run_state != after.run_state) events |= kClassicEvMode;
        if (before.total_focus_sec != after.total_focus_sec ||
            before.interrupt_count != after.interrupt_count) {
            events |= kClassicEvStats;
        }
        events |= kClassicEvNext;
    }

    if (need_save) save_stats();
    return events;
}

bool classic_toggle_start_pause()
{
    bool need_save = false;
    // 日志信息先在临界区内存到局部变量，出临界区后再打印。
    bool paused = false;
    unsigned log_interrupt = 0;
    int log_phase = 0;
    uint32_t log_remaining = 0;
    portENTER_CRITICAL(&s_mux);
    int64_t now_us = esp_timer_get_time();
    if (s_state.run_state == ClassicRunState::kRunning) {
        uint32_t rem = remaining_sec_locked(now_us);
        s_state.paused_remaining_sec = rem;
        s_state.run_state = ClassicRunState::kPaused;
        s_state.interrupt_count++;
        need_save = true;
        paused = true;
        log_interrupt = static_cast<unsigned>(s_state.interrupt_count);
    } else {
        if (s_state.run_state == ClassicRunState::kIdle) {
            s_state.paused_remaining_sec = phase_duration(s_state.phase);
        }
        s_state.run_state = ClassicRunState::kRunning;
        s_state.deadline_us = now_us +
            static_cast<int64_t>(s_state.paused_remaining_sec) * kUsPerSec;
        log_phase = static_cast<int>(s_state.phase);
        log_remaining = s_state.paused_remaining_sec;
    }
    portEXIT_CRITICAL(&s_mux);
    if (paused) {
        ESP_LOGI(TAG, "paused, interrupt count=%u", log_interrupt);
    } else {
        ESP_LOGI(TAG, "started/resumed phase=%d remaining=%u", log_phase, log_remaining);
    }
    if (need_save) save_stats();
    return true;
}

void classic_select_mode(int mode)
{
    if (mode < 0 || mode > 2) return;
    bool need_save = false;
    portENTER_CRITICAL(&s_mux);
    if (s_state.run_state == ClassicRunState::kRunning) {
        s_state.interrupt_count++;
        need_save = true;
    }
    // 手动切 MODE 且确实换了阶段时，节数 +1（与自动流转一致）。
    if (s_state.phase != static_cast<ClassicPhase>(mode)) {
        s_state.session++;
    }
    s_state.phase = static_cast<ClassicPhase>(mode);
    s_state.run_state = ClassicRunState::kIdle;
    s_state.paused_remaining_sec = phase_duration(s_state.phase);
    s_state.deadline_us = 0;
    if (mode == static_cast<int>(ClassicPhase::kFocus)) {
        s_state.focus_round = 0;
    }
    portEXIT_CRITICAL(&s_mux);
    if (need_save) save_stats();
    ESP_LOGI(TAG, "select mode=%d", mode);
}

void classic_reset()
{
    portENTER_CRITICAL(&s_mux);
    s_state.phase = ClassicPhase::kFocus;
    s_state.run_state = ClassicRunState::kIdle;
    s_state.focus_round = 0;
    s_state.session = 1;
    s_state.paused_remaining_sec = kFocusSec;
    s_state.deadline_us = 0;
    // 今日番茄数、累计专注、中断次数属于统计，重置计时不清这些。
    portEXIT_CRITICAL(&s_mux);
    save_stats();
    ESP_LOGI(TAG, "reset to focus session 1");
}
