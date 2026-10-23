// 维护设置页 KEY 导航、返回状态和二次确认清理逻辑。
#include "ui_settings_navigation.h"

#include "active_work_page_state_internal.h"
#include "app_tick_time.h"
#include "ui_settings_activity_state.h"
#include "ui_settings_confirmation_state_internal.h"
#include "ui_settings_feedback.h"
#include "ui_settings_navigation_state_internal.h"
#include "ui_task_notify.h"
#include "ui_work_page_catalog.h"
#include "work_page_ids.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>

namespace {
std::atomic<TickType_t> s_settings_primary_exit_block_until{0};

constexpr uint32_t kSettingsPrimaryExitBlockMs = 800;
constexpr uint32_t kSettingsOrderExitFeedbackMs = 2500;
constexpr const char *kSettingsOrderExitSavedFeedback = "页面顺序已保存";

constexpr int clamp_selection_to_count(int selected, int count)
{
    return count > 0 && selected >= 0 && selected < count ? selected : 0;
}

static_assert(kSettingsPrimaryExitBlockMs > 0, "settings primary exit block duration must be positive");
static_assert(kSettingsOrderExitFeedbackMs > 0, "settings order exit feedback duration must be positive");
static_assert(kSettingsOrderExitSavedFeedback[0] != '\0', "settings order saved feedback must not be empty");
static_assert(clamp_selection_to_count(kWorkPageCalendar, kWorkPageCount) == kWorkPageCalendar &&
                  clamp_selection_to_count(kWorkPageHistory, kWorkPageCount) == kWorkPageHistory &&
                  clamp_selection_to_count(kWorkPageGameBoy, kWorkPageCount) == kWorkPageGameBoy,
              "high work page indices must remain selectable in page toggle mode");
static_assert(kSettingsPrimaryCount <= 256 && kSettingsSecondaryMaxCount <= 256 &&
                  kWorkPageCount <= 256,
              "settings navigation selections must fit in the atomic snapshot fields");
} // namespace

int settings_secondary_count(int primary)
{
    switch (primary) {
    case kSettingsPrimaryNetwork:
        return kNetworkSettingsSecondaryCount;
    case kSettingsPrimarySound:
        return kSoundSettingsSecondaryCount;
    case kSettingsPrimaryDisplay:
        return kDisplaySettingsSecondaryCount;
    case kSettingsPrimarySystem:
        return kSystemSettingsSecondaryCount;
    default:
        return 0;
    }
}

void reset_settings_confirmation()
{
    settings_confirmation_clear_all();
}

void reset_settings_navigation_state()
{
    SettingsNavigationSnapshot navigation = settings_navigation_snapshot();
    navigation.focus_secondary = false;
    navigation.page_toggle_mode = false;
    navigation.page_order_mode = false;
    settings_navigation_store(navigation);
    s_settings_primary_exit_block_until.store(0, std::memory_order_relaxed);
    reset_settings_confirmation();
}

void enter_settings_primary_navigation()
{
    SettingsNavigationSnapshot navigation;
    navigation.primary_selection = kSettingsPrimaryNetwork;
    settings_navigation_store(navigation);
    s_settings_primary_exit_block_until.store(0, std::memory_order_relaxed);
}

void enter_settings_system_item_navigation(int selection)
{
    const int selected = clamp_settings_secondary(kSettingsPrimarySystem, selection);
    SettingsNavigationSnapshot navigation;
    navigation.focus_secondary = true;
    navigation.primary_selection = kSettingsPrimarySystem;
    navigation.selection = selected;
    settings_navigation_store(navigation);
}

int clamp_settings_primary(int primary)
{
    if (primary < 0 || primary >= kSettingsPrimaryCount) {
        return kSettingsPrimaryNetwork;
    }
    return primary;
}

int clamp_settings_secondary(int primary, int selected)
{
    int count = settings_secondary_count(primary);
    return clamp_selection_to_count(selected, count);
}

int clamp_settings_selection_for_mode(int primary, int selected, bool page_toggle_mode)
{
    if (!page_toggle_mode) {
        return clamp_settings_secondary(primary, selected);
    }
    return clamp_selection_to_count(selected, kWorkPageCount);
}

void handle_settings_key_short()
{
    settings_activity_record(xTaskGetTickCount());
    SettingsNavigationSnapshot navigation = settings_navigation_snapshot();
    int primary = clamp_settings_primary(navigation.primary_selection);
    navigation.primary_selection = primary;
    if (navigation.page_order_mode) {
        navigation.page_order_selection =
            next_enabled_work_page_order_index(navigation.page_order_selection);
    } else if (navigation.page_toggle_mode) {
        navigation.selection = (navigation.selection + 1) % kWorkPageCount;
    } else if (navigation.focus_secondary) {
        int count = settings_secondary_count(primary);
        if (count > 0) {
            navigation.selection =
                (clamp_settings_secondary(primary, navigation.selection) + 1) % count;
        }
    } else {
        navigation.primary_selection = (primary + 1) % kSettingsPrimaryCount;
        navigation.selection = 0;
    }
    settings_navigation_store(navigation);
    reset_settings_confirmation();
    clear_settings_feedback();
    notify_ui_task();
}

void handle_settings_key_long()
{
    settings_activity_record(xTaskGetTickCount());
    SettingsNavigationSnapshot navigation = settings_navigation_snapshot();
    if (navigation.page_order_mode) {
        navigation.page_order_mode = false;
        navigation.focus_secondary = true;
        navigation.primary_selection = kSettingsPrimaryDisplay;
        navigation.selection = kDisplaySettingsOrderItem;
        settings_navigation_store(navigation);
        active_work_page_store(first_enabled_work_page());
        set_settings_feedback(kSettingsOrderExitSavedFeedback, kSettingsOrderExitFeedbackMs);
        reset_settings_confirmation();
        notify_ui_task();
        return;
    } else if (navigation.page_toggle_mode) {
        navigation.page_toggle_mode = false;
        navigation.focus_secondary = true;
        navigation.primary_selection = kSettingsPrimaryDisplay;
        navigation.selection = kDisplaySettingsPageSwitchItem;
        settings_navigation_store(navigation);
        reset_settings_confirmation();
        clear_settings_feedback();
        notify_ui_task();
        return;
    } else if (navigation.focus_secondary) {
        navigation.focus_secondary = false;
        navigation.selection = 0;
        settings_navigation_store(navigation);
        s_settings_primary_exit_block_until.store(
            xTaskGetTickCount() + pdMS_TO_TICKS(kSettingsPrimaryExitBlockMs),
            std::memory_order_relaxed);
    } else {
        TickType_t now = xTaskGetTickCount();
        const TickType_t exit_block_until =
            s_settings_primary_exit_block_until.load(std::memory_order_relaxed);
        if (exit_block_until != 0 &&
            app_tick_deadline_pending(now, exit_block_until)) {
            settings_activity_record(now);
            notify_ui_task();
            return;
        }
        s_settings_primary_exit_block_until.store(0, std::memory_order_relaxed);
        settings_page_clear();
        reset_settings_navigation_state();
        clear_settings_feedback();
        notify_ui_task();
        return;
    }
    reset_settings_confirmation();
    clear_settings_feedback();
    notify_ui_task();
}
