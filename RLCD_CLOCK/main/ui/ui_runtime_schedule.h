// 声明 UI 运行期超时、辅助页门控和唤醒调度入口。
#pragma once

#include <freertos/FreeRTOS.h>

#include <stdint.h>
#include <time.h>

struct BatteryRuntimeSnapshot;

struct UiRuntimeSurfaceSnapshot {
    bool setup_portal_active = false;
    bool settings_requested = false;
    bool info_requested = false;
    bool network_diag_requested = false;

    bool auxiliary_page_requested() const
    {
        return settings_requested || info_requested || network_diag_requested;
    }

    bool interactive_surface_requested() const
    {
        return setup_portal_active || auxiliary_page_requested();
    }
};

bool ui_runtime_settings_timeout_elapsed(TickType_t last_activity);
UiRuntimeSurfaceSnapshot ui_runtime_surface_snapshot_load();
TickType_t ui_runtime_next_loop_delay_ticks(time_t sampled_wall_second,
                                            const BatteryRuntimeSnapshot &battery,
                                            bool battery_blink_visible,
                                            int active_page,
                                            const UiRuntimeSurfaceSnapshot &surfaces);
