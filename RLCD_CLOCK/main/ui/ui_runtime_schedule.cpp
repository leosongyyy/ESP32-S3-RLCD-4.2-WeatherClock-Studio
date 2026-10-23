// 编排 UI 运行期超时、辅助页门控和唤醒间隔。
#include "ui_runtime_schedule.h"

#include "active_work_page_state_internal.h"
#include "app_metadata.h"
#include "app_runtime_timing.h"
#include "app_tick_time.h"
#include "battery_runtime_state.h"
#include "network_diagnostics_state.h"
#include "sensor_time.h"
#include "ui_info_page_state.h"
#include "ui_loop_schedule.h"
#include "ui_settings_activity_state.h"
#include "ui_work_page_catalog.h"
#include "wifi_portal_state.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <sys/time.h>

namespace {

constexpr int64_t kUiRuntimeUsPerSecond = 1000000LL;

int64_t current_wall_clock_us(time_t &sampled_wall_second)
{
    struct timeval now = {};
    int64_t wall_clock_us = esp_timer_get_time();
    if (gettimeofday(&now, nullptr) == 0) {
        wall_clock_us = static_cast<int64_t>(now.tv_sec) * kUiRuntimeUsPerSecond +
                        static_cast<int64_t>(now.tv_usec);
    } else {
        sampled_wall_second = static_cast<time_t>(
            wall_clock_us / kUiRuntimeUsPerSecond);
    }
    return wall_clock_us;
}

TickType_t next_second_delay_ticks(time_t sampled_wall_second)
{
    int64_t wall_clock_us = current_wall_clock_us(sampled_wall_second);
    return pdMS_TO_TICKS(ui_next_second_delay_ms(
        static_cast<int64_t>(sampled_wall_second),
        wall_clock_us));
}

TickType_t next_minute_delay_ticks(time_t sampled_wall_second)
{
    int64_t wall_clock_us = current_wall_clock_us(sampled_wall_second);
    return pdMS_TO_TICKS(ui_next_minute_delay_ms(
        static_cast<int64_t>(sampled_wall_second),
        wall_clock_us));
}

static_assert(sizeof(TickType_t) == sizeof(uint32_t),
              "UI delay candidates require 32-bit FreeRTOS ticks");
static_assert(kUiRuntimeUsPerSecond > 0,
              "UI wall-clock conversion factor must be positive");
} // namespace

bool ui_runtime_settings_timeout_elapsed(TickType_t last_activity)
{
    if (last_activity == 0) {
        return false;
    }
    TickType_t now = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(kSettingsTimeoutMs);
    return app_tick_interval_elapsed(now, last_activity, timeout_ticks);
}

UiRuntimeSurfaceSnapshot ui_runtime_surface_snapshot_load()
{
    return {
        setup_portal_active_load(),
        settings_page_requested(),
        info_page_requested(),
        network_diag_page_requested(),
    };
}

TickType_t ui_runtime_next_loop_delay_ticks(time_t sampled_wall_second,
                                            const BatteryRuntimeSnapshot &battery,
                                            bool battery_blink_visible,
                                            int active_page,
                                            const UiRuntimeSurfaceSnapshot &surfaces)
{
    bool low_idle = ui_low_battery_minute_idle(
        battery.low_battery_mode,
        surfaces.auxiliary_page_requested());
    bool low_refresh_page_idle = ui_low_refresh_page_minute_idle(
        work_page_uses_low_refresh_idle(active_page),
        battery.low_battery_mode,
        surfaces.setup_portal_active,
        surfaces.auxiliary_page_requested());
    const bool minute_level_wait = low_idle || low_refresh_page_idle;
    const TickType_t second_delay_ticks =
        (!minute_level_wait || battery_blink_visible)
            ? next_second_delay_ticks(sampled_wall_second)
            : 0;
    uint32_t delay_candidates[2] = {};
    delay_candidates[0] = minute_level_wait
                              ? next_minute_delay_ticks(sampled_wall_second)
                              : second_delay_ticks;
    if (battery_blink_visible) {
        delay_candidates[1] = second_delay_ticks;
    }
    return static_cast<TickType_t>(ui_shortest_delay_ticks(
        delay_candidates,
        sizeof(delay_candidates) / sizeof(delay_candidates[0])));
}
