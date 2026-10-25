// 实现 Game Boy 模拟器任务：生命周期、60Hz 节拍、自适应跳帧推屏与性能快照。
#include "gameboy_runtime.h"

#include "game_audio.h"
#include "game_config.h"
#include "game_emu.h"
#include "gameboy_chrome.h"
#include "gameboy_fb.h"
#include "gameboy_input.h"
#include "gameboy_rom_store.h"

#include "ui_display_flush.h"

#include <atomic>

#include <esp_log.h>
#include <esp_pm.h>
#include <esp_rom_sys.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {
constexpr const char *kRuntimeLogTag = "GameBoy";
constexpr uint32_t kStopWaitSteps = 200;
constexpr uint32_t kStopWaitStepMs = 10;
constexpr uint32_t kForceYieldFrameInterval = 16;
constexpr uint32_t kEmuAverageWeight = 4;
constexpr const char *kTaskName = "gameboy";
// 暂停期间任务照常醒来喂狗，20ms 一次足够（TWDT 默认 5s）。
constexpr uint32_t kPausedPollMs = 20;

std::atomic<bool> s_display_owner{false};
std::atomic<bool> s_run_request{false};
std::atomic<bool> s_running{false};
std::atomic<bool> s_paused{false};
// 推屏（SPI 阻塞传输）进行中的标记。强杀任务前必须确认它为 false，
// 否则 SPI 驱动会停在半途，之后整块屏的刷新全乱。
std::atomic<bool> s_in_present{false};
// 停机重入闸。stop() 现在有两个跨任务调用方（ui_task 的逃生守卫 game_page_guard_runtime_stop()
// 与 button 任务的双键长按），而函数体本身不可重入：
//   - game_emu_stop() 会被调两次；
//   - esp_pm_lock_release(s_pm_lock) 会被释放两次（s_pm_lock 不清空，计数会跑负）；
//   - vTaskDelete() 可能删到已经被删掉的句柄。
// 用 CAS 让第一个进来的执行、后来的等它做完再返回（调用方返回时 running 必为 false）。
// 不用互斥量是为了避免在 stop 路径上做堆分配。
std::atomic<bool> s_stopping{false};
TaskHandle_t s_task_handle = nullptr;
esp_pm_lock_handle_t s_pm_lock = nullptr;
int s_rom_index = -1;
char s_last_error[96] = "";

portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
GameboyRuntimeStats s_stats = {};

void set_last_error(const char *text)
{
    snprintf(s_last_error, sizeof(s_last_error), "%s", text ? text : "");
}

// 单帧模拟耗时与单次推屏耗时决定了「每 N 帧推一次屏」才能守住 60Hz 实时。
// N 至少为 1；模拟本身超预算时退到上限，让游戏以自然降速继续跑。
uint32_t recompute_present_stride(uint32_t emu_us_avg, uint32_t present_us_avg)
{
    const uint32_t frame_budget_us = (uint32_t)game_config::kFramePeriodUs;
    if (emu_us_avg >= frame_budget_us) {
        return game_config::kMaxPresentStride;
    }
    const uint32_t remaining_us = frame_budget_us - emu_us_avg;
    uint32_t stride = (present_us_avg + remaining_us - 1U) / remaining_us;
    if (stride < game_config::kMinPresentStride) {
        stride = game_config::kMinPresentStride;
    }
    if (stride > game_config::kMaxPresentStride) {
        stride = game_config::kMaxPresentStride;
    }
    return stride;
}

uint32_t update_average(uint32_t average, uint32_t sample)
{
    if (average == 0) {
        return sample;
    }
    return (average * (kEmuAverageWeight - 1U) + sample) / kEmuAverageWeight;
}

void publish_stats(uint32_t emulated_frames,
                   uint32_t presented_frames,
                   uint32_t fps_x10,
                   uint32_t emu_us_avg,
                   uint32_t present_us_avg,
                   uint32_t present_stride,
                   uint32_t loop_us_avg,
                   uint32_t loop_us_max,
                   bool running)
{
    portENTER_CRITICAL(&s_stats_mux);
    s_stats.running = running;
    s_stats.emulated_frames = emulated_frames;
    s_stats.presented_frames = presented_frames;
    s_stats.fps_x10 = fps_x10;
    s_stats.emu_us_avg = emu_us_avg;
    s_stats.present_us_avg = present_us_avg;
    s_stats.present_stride = present_stride;
    s_stats.loop_us_avg = loop_us_avg;
    s_stats.loop_us_max = loop_us_max;
    portEXIT_CRITICAL(&s_stats_mux);
}

// 精确定时到某个绝对微秒时刻。
//
// tick 只有 4ms（CONFIG_FREERTOS_HZ=250），单靠 vTaskDelay 会把 16743us 的帧长
// 量化成 16ms 或 20ms——62.5fps / 50fps，都是永久跑偏而不是抖动。所以分两步：
//   1. vTaskDelay 睡掉「整 tick」的部分，把 CPU 让给同/低优先级任务；
//   2. 剩下不足一个 tick 的零头（0~3999us）用 esp_rom_delay_us 精确定时。
// 零头忙等平均约 2ms/帧 ≈ 12% CPU，而且只发生在优先级 1 上：
// ui_task(3)/network_task(4) 依旧随时抢占我们，只有 idle 会少睡一会儿。
// 换来的是帧周期误差从 ±4000us 降到 ±50us 量级。
void sleep_until_us(int64_t deadline_us)
{
    constexpr int64_t kTickUs = (int64_t)portTICK_PERIOD_MS * 1000;
    const int64_t remain_us = deadline_us - esp_timer_get_time();
    if (remain_us <= 0) {
        return;
    }
    const int64_t whole_ticks = remain_us / kTickUs;
    if (whole_ticks > 0) {
        vTaskDelay((TickType_t)whole_ticks);
    }
    const int64_t leftover_us = deadline_us - esp_timer_get_time();
    if (leftover_us > 0 && leftover_us < kTickUs) {
        esp_rom_delay_us((uint32_t)leftover_us);
    }
}

// 抢显示所有者的握手：把门闸置 true 之后必须先确认 LVGL 那边已经走到 flush
// 回调（也就是它上一次 SPI 传输已经返回），再让模拟器推首帧。
// 不握手的话，首帧推屏可能和 LVGL 最后一次传输交错写同一块面板缓冲，
// 结果不是闪一帧，而是半屏旧画面。
// LVGL 此刻可能本来就没东西要刷，所以必须带超时兜底，不能死等。
void wait_for_display_handover()
{
    constexpr int64_t kHandoverTimeoutUs = 100 * 1000;
    const uint32_t before = ui_display_flush_entry_count();
    gameboy_display_owner_store(true);

    const int64_t deadline_us = esp_timer_get_time() + kHandoverTimeoutUs;
    while (esp_timer_get_time() < deadline_us) {
        if (ui_display_flush_entry_count() != before) {
            return;
        }
        vTaskDelay(1);
    }
    ESP_LOGW(kRuntimeLogTag,
             "display handover timeout, LVGL may have been idle");
}

void save_cart_ram_if_dirty()
{
    if (!game_emu_cart_ram_dirty() || s_rom_index < 0) {
        return;
    }
    size_t bytes = 0;
    const uint8_t *ram = game_emu_cart_ram(&bytes);
    if (!ram || bytes == 0) {
        game_emu_cart_ram_clear_dirty();
        return;
    }
    if (rom_store_save_cart_ram(s_rom_index, ram, bytes)) {
        ESP_LOGI(kRuntimeLogTag, "cart ram saved bytes=%u", (unsigned)bytes);
    } else {
        ESP_LOGW(kRuntimeLogTag, "cart ram save failed");
    }
    game_emu_cart_ram_clear_dirty();
}

void gameboy_task_entry(void *)
{
    int64_t next_deadline_us = esp_timer_get_time() + game_config::kFramePeriodUs;
    int64_t last_stats_us = esp_timer_get_time();
    int64_t loop_started_us = esp_timer_get_time();
    uint32_t present_stride = game_config::kMinPresentStride;
    uint32_t frames_since_present = 0;
    uint32_t frames_since_yield = 0;
    uint32_t emulated_frames = 0;
    uint32_t presented_frames = 0;
    uint32_t window_emulated = 0;
    uint32_t emu_us_avg = 0;
    uint32_t present_us_avg = 0;
    uint32_t fps_x10 = 0;
    uint32_t loop_us_avg = 0;
    uint32_t loop_us_max = 0;
    uint32_t lag_frames = 0;
    bool pause_painted = false;
    // 音频会话的暂停镜像：只在暂停状态变化时调用 sink，避免每帧都跨模块。
    bool sink_paused = false;
    // 侧栏（时间 / 电量 / 手柄 / 音量）的检查时刻。每秒看一次签名，变了才重画。
    int64_t last_chrome_check_us = esp_timer_get_time();

    // 把本任务挂到任务看门狗上：本工程 CONFIG_ESP_TASK_WDT_PANIC=y，
    // 一旦超过 TWDT 超时就会 panic 重启（不是打日志），订阅后每帧补喂。
    // 若 TWDT 被配置成不初始化，这里是 ESP_ERR_INVALID_STATE，忽略即可。
    const bool watchdog_subscribed = (esp_task_wdt_add(nullptr) == ESP_OK);

    while (s_run_request.load()) {
        // 暂停：模拟与推屏全停，画面冻结在最后一帧。
        // 注意三件事——① 必须继续喂看门狗（PANIC 模式下不喂就是 5 秒后重启）；
        // ② 角标只能直接写面板缓冲，绝不能在 game 任务里调 LVGL（跨任务必崩）；
        // ③ 暂停瞬间落一次 .sav，用户拔电池也不丢档。恢复时清掉角标标记，
        //    下一帧会把 144 行全部重绘，角标自然被覆盖。
        if (s_paused.load()) {
            if (!sink_paused) {
                // 进暂停：音频任务改为排空存货后只灌静音（功放保持使能）。
                sink_paused = true;
                game_audio_set_paused(true);
            }
            if (!pause_painted) {
                if (game_fb_draw_pause_badge()) {
                    s_in_present.store(true);
                    (void)game_fb_present();
                    s_in_present.store(false);
                }
                save_cart_ram_if_dirty();
                pause_painted = true;
                ESP_LOGI(kRuntimeLogTag, "paused, cart ram flushed");
            }
            if (watchdog_subscribed) {
                (void)esp_task_wdt_reset();
            }
            vTaskDelay(pdMS_TO_TICKS(kPausedPollMs));
            // 恢复时不要补帧：把节拍基准直接推到"下一帧"。
            next_deadline_us = esp_timer_get_time() + game_config::kFramePeriodUs;
            continue;
        }
        if (pause_painted) {
            pause_painted = false;
            ESP_LOGI(kRuntimeLogTag, "resumed");
        }
        if (sink_paused) {
            // 出暂停：音频从当前 APU 状态继续渲染。
            sink_paused = false;
            game_audio_set_paused(false);
        }

        // 侧栏状态栏每秒检查一次：时间跳分钟、电量跨档、手柄连上/断开、
        // 音量变动，才重画并推那两条 40px 宽的竖条。没变化就一个字节都不发。
        // ⚠ 放在模拟之前而不是推屏那一段：一次侧栏推送约 5ms，塞进推屏段会
        //   直接吃掉本帧的时间预算，把帧节拍打乱。放在这里顶多是这一帧慢一点。
        const int64_t chrome_now_us = esp_timer_get_time();
        if ((chrome_now_us - last_chrome_check_us) >= 1000000) {
            last_chrome_check_us = chrome_now_us;
            s_in_present.store(true);
            (void)game_chrome_refresh(false);
            s_in_present.store(false);
        }

        game_emu_set_joypad_register(gameboy_pad_joypad_register());

        const int64_t emu_started_us = esp_timer_get_time();
        game_emu_run_frame();
        const uint32_t emu_us =
            (uint32_t)(esp_timer_get_time() - emu_started_us);
        emu_us_avg = update_average(emu_us_avg, emu_us);
        ++emulated_frames;
        ++window_emulated;
        ++frames_since_present;

        if (frames_since_present >= present_stride) {
            const int64_t present_started_us = esp_timer_get_time();
            s_in_present.store(true);
            const bool presented = game_fb_present();
            s_in_present.store(false);
            const uint32_t present_us =
                (uint32_t)(esp_timer_get_time() - present_started_us);
            if (presented) {
                present_us_avg = update_average(present_us_avg, present_us);
                ++presented_frames;
            }
            frames_since_present = 0;
            present_stride = recompute_present_stride(emu_us_avg, present_us_avg);
        }

        // ---- 帧节拍 ----
        // 为什么不是 vTaskDelayUntil：本工程 CONFIG_FREERTOS_HZ=250 → 1 tick = 4ms，
        // 而 GB 一帧是 16743us = 4.1857 tick。vTaskDelayUntil 只能按整数 tick 递增，
        // 4 tick=16ms（62.5fps，游戏快 4.4%）、5 tick=20ms（50fps，游戏慢 16%），
        // 而且 vTaskDelay 族的实际唤醒还会被对齐到 tick 边界，误差只会往"更慢"偏，
        // 累积起来就是永久跑偏（不是抖动）。所以这里用绝对微秒时基 + 分级睡眠。
        const int64_t now_us = esp_timer_get_time();
        const int64_t delta_us = next_deadline_us - now_us;
        if (delta_us > 0) {
            sleep_until_us(next_deadline_us);
            frames_since_yield = 0;
        } else {
            ++frames_since_yield;
            if (-delta_us > game_config::kPaceResyncThresholdUs) {
                // 落后太多（例如刚被 LVGL 抢占了 150ms）：重置时间基准。
                // 不重置的话接下来会连续不睡，一路把屏幕"快进"过去。
                next_deadline_us = now_us;
            }
            // 推屏本身会阻塞几十毫秒，能给低优先级任务让出 CPU；
            // 若本帧既没睡也没推屏，则周期性强制让出，避免饿死其它任务。
            if (frames_since_yield >= kForceYieldFrameInterval) {
                vTaskDelay(1);
                frames_since_yield = 0;
            }
        }
        next_deadline_us += game_config::kFramePeriodUs;

        // 看门狗：CONFIG_ESP_TASK_WDT_PANIC=y，超时直接 panic 重启而不是打日志。
        // 本任务优先级只有 1，一帧 16.7ms，理论上不会超时；但推屏走 SPI 阻塞
        // 期间若被更高优先级任务长时间占用，WDT 仍可能在此期间到期，所以每帧补喂。
        if (watchdog_subscribed) {
            (void)esp_task_wdt_reset();
        }

        // 实测循环周期（含模拟 + 推屏 + vTaskDelay 睡眠）：用来验证优先级 1 下
        // 本任务有没有被 ui_task 的 LVGL 渲染整段饿死。
        // 正常情况下它应该贴着 16743us；出现 2 倍以上就说明被抢占过。
        const int64_t loop_finished_us = esp_timer_get_time();
        const uint32_t loop_us = (uint32_t)(loop_finished_us - loop_started_us);
        loop_started_us = loop_finished_us;
        loop_us_avg = update_average(loop_us_avg, loop_us);
        if (loop_us > loop_us_max) {
            loop_us_max = loop_us;
        }
        if ((int64_t)loop_us * 2 > game_config::kFramePeriodUs * 3) {
            ++lag_frames;
        }

        const int64_t stats_now_us = esp_timer_get_time();
        const int64_t stats_window_us = stats_now_us - last_stats_us;
        if (stats_window_us >= (int64_t)game_config::kStatsLogIntervalMs * 1000) {
            fps_x10 = (uint32_t)(((uint64_t)window_emulated * 10ULL * 1000000ULL) /
                                 (uint64_t)stats_window_us);
            publish_stats(emulated_frames,
                          presented_frames,
                          fps_x10,
                          emu_us_avg,
                          present_us_avg,
                          present_stride,
                          loop_us_avg,
                          loop_us_max,
                          true);
            ESP_LOGI(kRuntimeLogTag,
                     "fps=%u.%u emu_us=%u present_us=%u stride=%u "
                     "loop_us=%u loop_max_us=%u lag=%u frames=%u",
                     (unsigned)(fps_x10 / 10U),
                     (unsigned)(fps_x10 % 10U),
                     (unsigned)emu_us_avg,
                     (unsigned)present_us_avg,
                     (unsigned)present_stride,
                     (unsigned)loop_us_avg,
                     (unsigned)loop_us_max,
                     (unsigned)lag_frames,
                     (unsigned)emulated_frames);
            last_stats_us = stats_now_us;
            window_emulated = 0;
            loop_us_max = 0;
            lag_frames = 0;
        }
    }

    publish_stats(emulated_frames,
                  presented_frames,
                  fps_x10,
                  emu_us_avg,
                  present_us_avg,
                  present_stride,
                  loop_us_avg,
                  loop_us_max,
                  false);
    if (watchdog_subscribed) {
        (void)esp_task_wdt_delete(nullptr);
    }
    s_task_handle = nullptr;
    s_running.store(false);
    vTaskDelete(nullptr);
}
} // namespace

bool gameboy_display_owner(void)
{
    return s_display_owner.load();
}

void gameboy_display_owner_store(bool owned)
{
    s_display_owner.store(owned);
}

bool gameboy_runtime_running(void)
{
    return s_running.load();
}

bool gameboy_runtime_paused(void)
{
    return s_paused.load();
}

void gameboy_runtime_toggle_pause(void)
{
    if (!s_running.load()) {
        return;
    }
    const bool next = !s_paused.load();
    s_paused.store(next);
    // 真正的画面冻结、角标绘制与 .sav 落盘都在 game 任务里做（单一所有者，
    // 不用和推屏抢 SPI）。这里只翻标志位，所以可以从按键任务安全调用。
    ESP_LOGI(kRuntimeLogTag, "%s", next ? "pause requested" : "resume requested");
}

bool gameboy_runtime_start(void)
{
    if (s_running.load()) {
        return true;
    }
    set_last_error("");

    if (!rom_store_mount()) {
        set_last_error(rom_store_diagnostic());
        return false;
    }
    if (rom_store_count() <= 0) {
        (void)rom_store_refresh();
    }
    if (rom_store_count() <= 0) {
        set_last_error("未安装 ROM");
        return false;
    }

    const int index = rom_store_selected();
    size_t rom_bytes = 0;
    const uint8_t *rom = rom_store_load(index, &rom_bytes);
    if (!rom || rom_bytes == 0) {
        set_last_error(rom_store_diagnostic());
        return false;
    }

    struct tm rtc_now = {};
    const time_t now_seconds = time(nullptr);
    if (now_seconds > 0) {
        localtime_r(&now_seconds, &rtc_now);
    }

    if (!game_emu_start(rom, rom_bytes, now_seconds > 0 ? &rtc_now : nullptr)) {
        set_last_error(game_emu_error_text(game_emu_last_error()));
        rom_store_unload();
        return false;
    }

    // 把上次的电池存档灌回卡带 RAM（直接写缓冲，不触发脏标记）。
    size_t cart_capacity = 0;
    uint8_t *cart_ram = game_emu_cart_ram_writable(&cart_capacity);
    if (cart_ram && cart_capacity > 0) {
        const size_t loaded =
            rom_store_load_cart_ram(index, cart_ram, cart_capacity);
        if (loaded > 0) {
            ESP_LOGI(kRuntimeLogTag, "cart ram restored bytes=%u", (unsigned)loaded);
        }
    }

    // 先抢显示所有权（含握手），再准备整屏，避免 LVGL 的局部刷新盖掉游戏画面。
    wait_for_display_handover();
    if (!game_fb_prepare_surface()) {
        gameboy_display_owner_store(false);
        game_emu_stop();
        rom_store_unload();
        set_last_error("显示缓冲不可用");
        return false;
    }

    if (!s_pm_lock) {
        if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, kTaskName, &s_pm_lock) !=
            ESP_OK) {
            s_pm_lock = nullptr;
        }
    }
    if (s_pm_lock) {
        (void)esp_pm_lock_acquire(s_pm_lock);
    }

    gameboy_pad_release_all();
    s_rom_index = index;
    s_paused.store(false);
    s_run_request.store(true);
    publish_stats(0,
                  0,
                  0,
                  0,
                  0,
                  game_config::kMinPresentStride,
                  0,
                  0,
                  true);

    // 音频会话必须在模拟器任务**之前**起：模拟器第一帧就会往
    // 0xFF10-0xFF3F 写声卡寄存器，那时若 APU 还没 minigb_apu_audio_init()，
    // audio_write 会因 s_apu_ctx 为空而静默丢掉这批初始化写入——游戏可能
    // 一直没声音，直到它下一次重新配置声卡。
    // 失败（例如整点提示音正占着门）不阻塞游戏：这一局静音运行，
    // 原因已在 game_audio 里打日志。
    (void)game_audio_start();

    if (xTaskCreatePinnedToCore(gameboy_task_entry,
                                kTaskName,
                                game_config::kTaskStackBytes,
                                nullptr,
                                (UBaseType_t)game_config::kTaskPriority,
                                &s_task_handle,
                                (BaseType_t)game_config::kTaskCoreId) != pdPASS) {
        s_task_handle = nullptr;
        s_run_request.store(false);
        gameboy_display_owner_store(false);
        if (s_pm_lock) {
            (void)esp_pm_lock_release(s_pm_lock);
        }
        // 音频会话已经在上面起了，失败分支必须一并收掉：泄漏的 codec 会
        // 一直占着提示音互斥门，此后所有整点提示音都被永久挡住。
        game_audio_stop();
        game_emu_stop();
        rom_store_unload();
        set_last_error("模拟器任务创建失败");
        return false;
    }

    s_running.store(true);

    ESP_LOGI(kRuntimeLogTag,
             "started rom=%s bytes=%u",
             rom_store_name(index),
             (unsigned)rom_bytes);
    return true;
}

void gameboy_runtime_stop(void)
{
    if (!s_running.load() && !s_run_request.load()) {
        return;
    }
    bool expected = false;
    if (!s_stopping.compare_exchange_strong(expected, true)) {
        // 另一个任务正在停机：等它做完再返回，保证「本函数返回时 running==false」
        // 这个约定对两个调用方都成立。最坏等 kStopWaitSteps*kStopWaitStepMs（2s）。
        for (uint32_t step = 0; step < kStopWaitSteps && s_stopping.load(); ++step) {
            vTaskDelay(pdMS_TO_TICKS(kStopWaitStepMs));
        }
        return;
    }
    s_run_request.store(false);

    // 任务自己收尾后会置 s_task_handle = nullptr；最多等 2 秒。
    for (uint32_t step = 0; step < kStopWaitSteps; ++step) {
        if (!s_task_handle) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(kStopWaitStepMs));
    }
    if (s_task_handle) {
        // 任务没在 2 秒内收尾，说明它卡在某个阻塞点上。绝对不能在 SPI 传输
        // 中间强杀：驱动内部状态不会复位，之后 LVGL 所有刷屏都会异常，只能重启。
        // 所以先等推屏结束，实在等不到才强杀，并且用 ESP_LOGE 留痕——出现这条
        // 日志本身就是"这里还得继续查"的信号。
        for (uint32_t step = 0; step < kStopWaitSteps && s_in_present.load();
             ++step) {
            vTaskDelay(pdMS_TO_TICKS(kStopWaitStepMs));
        }
        if (s_task_handle) {
            if (s_in_present.load()) {
                ESP_LOGE(kRuntimeLogTag,
                         "task stuck inside present, killing anyway");
            } else {
                ESP_LOGE(kRuntimeLogTag, "task did not exit in time, killing");
            }
            TaskHandle_t stale = s_task_handle;
            s_task_handle = nullptr;
            vTaskDelete(stale);
        }
    }

    // 音频会话必须先于 save_cart_ram_if_dirty() / game_emu_stop() 关掉。
    // 承重理由不是防 use-after-free——APU 上下文本体是 peanut_gb_impl.c
    // 里的文件作用域静态对象，不随 context 缓冲释放——而是「codec 先关，
    // 再去做 SD 存档 I/O」：SD 写入会阻塞几十毫秒，音频任务留在那儿就是
    // 空转白耗电，还可能把最后一段声音拖出咔哒声。
    // 此刻模拟器任务已确认退出（上面含强杀兜底），audio_read/audio_write
    // 不会再被调用，APU 状态稳定，停渲染安全。
    game_audio_stop();

    save_cart_ram_if_dirty();
    game_emu_stop();
    // ⚠ 这三行的次序不能动，它是「ROM 缓冲不会变成悬空指针」的全部依据：
    //   1. 必须在 game_emu_stop() 之后 —— 释放时 gb->rom 已经不会再被解引用；
    //   2. 必须在 s_running.store(false) 之前 —— free 期间 running 仍为 true，
    //      任何并发的 game_emu_start() 都会在自己的 `if (s_running) return` 处早退，
    //      不可能有人在这时候去 rom_store_load() 换掉缓冲。
    // 详见 gameboy_rom_store.cpp 里 clear_rom_buffer() 上方的说明。
    rom_store_unload();
    s_rom_index = -1;

    if (s_pm_lock) {
        (void)esp_pm_lock_release(s_pm_lock);
    }

    // 顺序很关键：先交还显示所有权，再宣布 running=false。
    // 反过来的话，ui_task 会读到 running=false、owner=true，于是调
    // lv_obj_invalidate() 去重绘，而那次刷新正好被门闸丢弃（但仍 flush_ready），
    // LVGL 认为已经刷完、dirty 清空 → 退出游戏后屏幕永远停在最后一帧 GB 画面。
    gameboy_display_owner_store(false);
    s_paused.store(false);
    s_running.store(false);
    gameboy_pad_release_all();

    GameboyRuntimeStats snapshot = {};
    portENTER_CRITICAL(&s_stats_mux);
    s_stats.running = false;
    snapshot = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);

    ESP_LOGI(kRuntimeLogTag,
             "stopped frames=%u presented=%u fps=%u.%u emu_us=%u present_us=%u "
             "loop_us=%u loop_max_us=%u",
             (unsigned)snapshot.emulated_frames,
             (unsigned)snapshot.presented_frames,
             (unsigned)(snapshot.fps_x10 / 10U),
             (unsigned)(snapshot.fps_x10 % 10U),
             (unsigned)snapshot.emu_us_avg,
             (unsigned)snapshot.present_us_avg,
             (unsigned)snapshot.loop_us_avg,
             (unsigned)snapshot.loop_us_max);

    // 函数体里没有中途 return，所以在这里统一放闸即可。
    s_stopping.store(false);
}

GameboyRuntimeStats gameboy_runtime_stats(void)
{
    GameboyRuntimeStats snapshot = {};
    portENTER_CRITICAL(&s_stats_mux);
    snapshot = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);
    return snapshot;
}

const char *gameboy_runtime_last_error(void)
{
    return s_last_error;
}
