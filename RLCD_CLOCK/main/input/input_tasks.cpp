// 处理 BOOT 和 KEY 按键输入、页面切换和设置页操作请求。
#include "input_tasks.h"

#include "active_work_page_state_internal.h"
#include "alarm_services.h"
#include "app_metadata.h"
#include "app_task_readiness.h"
#include "battery_runtime_state.h"
#include "gameboy_input.h"
#include "gameboy_pad.h"
#include "gameboy_pad_bind.h"
#include "gameboy_runtime.h"
#include "input_button_config.h"
#include "input_button_wait_policy.h"
#include "network_diagnostics_state.h"
#include "network_sync_requests.h"
#include "ota_services.h"
#include "pomodoro_services.h"
#include "runtime_health.h"
#include "task_notification_target.h"
#include "ui_game.h"
#include "ui_game_bind.h"
#include "ui_info_page_state.h"
#include "ui_settings_activity_state.h"
#include "ui_settings_feedback.h"
#include "ui_settings_navigation.h"
#include "ui_runtime_schedule.h"
#include "ui_task_notify.h"
#include "ui_work_page_catalog.h"
#include "wifi_portal_state.h"
#include "work_page_ids.h"

#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/task.h"

#define BUTTON_GPIO_CONFIG_FAILED_LOG_FORMAT "button gpio config failed: %s"
#define BUTTON_GPIO_CONFIG_RETRY_LOG_FORMAT \
    "button gpio config failed: %s; retrying attempt=%u/%u"
#define BUTTON_ISR_SERVICE_FAILED_LOG_FORMAT "button gpio isr service failed: %s; using polling fallback"
#define BUTTON_ISR_HANDLER_FAILED_LOG_FORMAT "button gpio %d isr handler failed: %s; using polling fallback"
#define BUTTON_WAKEUP_FAILED_LOG_FORMAT "button light sleep wakeup failed: %s; using polling fallback"
#define BUTTON_EDGE_WAKEUP_READY_LOG_FORMAT "button edge wakeup ready"
#define BUTTON_SWITCH_WORK_PAGE_LOG_FORMAT "switch work page: %d"
#define BUTTON_SHOW_SETTINGS_LOG_FORMAT "key button clicked, showing settings page"
#define BUTTON_GAME_NEXT_ROM_LOG "game page: select next rom"
#define BUTTON_GAME_PREV_ROM_LOG "game page: select prev rom"
#define BUTTON_GAME_START_LOG "game page: start selected rom"
#define BUTTON_GAME_VOLUME_UP_LOG "game page: volume step up"
#define BUTTON_GAME_VOLUME_DOWN_LOG "game page: volume step down"
#define BUTTON_GAME_SETTINGS_LOG "game page: long press opens settings"
#define BUTTON_GAME_BIND_OPEN_LOG "game bind page opened"
#define BUTTON_GAME_BIND_CLOSE_LOG "game bind page closed"
#define BUTTON_GAME_BIND_CLEAR_LOG "game bind page: long press cleared all bindings"
#define BUTTON_GAME_EXIT_LOG "game page: both buttons held, exiting game"
#define BUTTON_GAME_PAUSE_LOG "game page: both buttons held, toggling pause"

namespace {
constexpr int kButtonDebounceMs = 18;
constexpr int kButtonLongPressMs = 1200;
constexpr int kButtonBusyFeedbackMs = 2000;
constexpr int kGamepadExitHoldMs = 2000;
constexpr int kGamepadPollMs = 8;
// ROM 列表页的手柄轮询周期。这一页 idle 时 UI 走的是墨水屏的低刷节奏
// （几百毫秒一跳），照那个频率扫手柄会有明显的迟滞感，所以另开一个档。
constexpr int kGamepadUiPollMs = 20;
// 十字键按住不放时的连发节奏：先等一会儿再开始连发，连发起来之后快一些。
// ROM 列表一般只有几个到几十个条目，太快就冲过头了。
constexpr int kGamepadRepeatFirstMs = 450;
constexpr int kGamepadRepeatNextMs = 220;
// 进绑定页：**双击 BOOT**。
//
// 之前用的是「BOOT + KEY 同按 600ms」，现场反馈很难按出来：两个键要同时按住、
// 还得掐准时长，墨水屏没反馈的时候根本不知道按够了没有。双击 BOOT 是单键
// 操作，和 ROM 列表页"BOOT 短按换 ROM"的手感一致，只是快按两下。
//
// 代价：单次短按必须等这个窗口过去才能执行（不然没法区分单击/双击）。
// 350ms 的延迟在墨水屏上基本感知不到——这一页本来就是几百毫秒一跳的节奏。
constexpr int kBootDoubleClickMs = 350;
// 按住多久算「长按 = 带上 START/SELECT」。
// 250ms 足够躲开抖动（kButtonDebounceMs=18）又远小于退出阈值（2000ms），
// 中间这段用户可以正常做出「按住 A 连跳」「按住 B 加速」这类 GB 常见操作。
constexpr int kGamepadModifierHoldMs = 250;
// 双键同时按住多久算「暂停/继续」。见 poll_gamepad_while_running() 的说明：
// 暂停不能挂在 KEY 上（短按是 B，长按是 B+SELECT，B 是高频键）。
constexpr int kGamepadPauseHoldMs = 600;
constexpr uint64_t kBootButtonPinMask = 1ULL << kBootButtonGpio;
constexpr uint64_t kKeyButtonPinMask = 1ULL << kKeyButtonGpio;
constexpr uint64_t kButtonInputPinMask = kBootButtonPinMask | kKeyButtonPinMask;
constexpr TickType_t kButtonDebounceTicks = pdMS_TO_TICKS(kButtonDebounceMs);
constexpr TickType_t kButtonLongPressTicks = pdMS_TO_TICKS(kButtonLongPressMs);
constexpr TickType_t kGamepadExitHoldTicks = pdMS_TO_TICKS(kGamepadExitHoldMs);
constexpr TickType_t kGamepadModifierHoldTicks = pdMS_TO_TICKS(kGamepadModifierHoldMs);
constexpr TickType_t kGamepadPauseHoldTicks = pdMS_TO_TICKS(kGamepadPauseHoldMs);
constexpr TickType_t kGamepadUiPollTicks = pdMS_TO_TICKS(kGamepadUiPollMs);
constexpr TickType_t kGamepadRepeatFirstTicks = pdMS_TO_TICKS(kGamepadRepeatFirstMs);
constexpr TickType_t kGamepadRepeatNextTicks = pdMS_TO_TICKS(kGamepadRepeatNextMs);
constexpr TickType_t kBootDoubleClickTicks = pdMS_TO_TICKS(kBootDoubleClickMs);
constexpr const char *kSettingsBusyFeedbackText = "请等待操作完成";
TaskNotificationTarget s_button_task_target;
// 退出游戏后吞掉「这次松开」：退出那一刻用户两个键都按着，手指还没抬起来。
// 不吞的话，主循环会在同一个按键周期里把 boot_pressed_since 重新赋成 now，
// 松手时 held 只有几百毫秒 → 判成短按 → BOOT 立刻换 ROM、KEY 立刻重启游戏，
// 用户看到的现象就是"按两个键退不出去，一松手又进去了"。
bool s_swallow_until_release = false;

static_assert(kButtonLongPressMs > kButtonDebounceMs,
              "button long-press duration must be longer than debounce duration");
static_assert(kBootButtonPinMask != 0, "BOOT button pin mask must not be empty");
static_assert(kKeyButtonPinMask != 0, "KEY button pin mask must not be empty");
static_assert(kButtonInputPinMask == (kBootButtonPinMask | kKeyButtonPinMask),
              "button input pin mask must include BOOT and KEY");
static_assert(kButtonLongPressTicks > kButtonDebounceTicks,
              "button long-press tick duration must be longer than debounce duration");
static_assert(kButtonGpioConfigMaxAttempts > 1,
              "button GPIO configuration must retain a retry opportunity");
static_assert(kButtonGpioConfigRetryDelayMs > 0,
              "button GPIO configuration retry delay must be positive");
static_assert(kButtonIdlePollMs <= kButtonLowRefreshIdlePollMs,
              "low-refresh fallback polling must not wake more often than idle polling");
static_assert(kButtonActivePollMs <= kButtonIdlePollMs,
              "active button polling must stay at least as responsive as idle polling");
static_assert(kButtonPressedPollMs <= kButtonActivePollMs,
              "pressed button polling must stay at least as responsive as active polling");
static_assert(kGamepadExitHoldMs > kButtonLongPressMs,
              "game exit hold must be longer than a normal long press");
static_assert(kGamepadModifierHoldMs > kButtonDebounceMs &&
                  kGamepadModifierHoldMs < kGamepadExitHoldMs,
              "gamepad modifier hold must sit between debounce and exit hold");
static_assert(kGamepadModifierHoldTicks > 0,
              "gamepad modifier hold must survive tick quantisation");
static_assert(kGamepadPauseHoldMs > kGamepadModifierHoldMs &&
                  kGamepadPauseHoldMs < kGamepadExitHoldMs,
              "pause hold must sit between modifier hold and exit hold");
static_assert(kGamepadPollMs > 0 && kGamepadPollMs <= kButtonPressedPollMs,
              "gamepad polling must stay as responsive as pressed-button polling");
static_assert(kGamepadUiPollMs > 0,
              "gamepad UI polling interval must be positive");
static_assert(kGamepadUiPollTicks > 0,
              "gamepad UI polling must survive tick quantisation");
static_assert(kGamepadRepeatFirstMs > kGamepadUiPollMs,
              "repeat delay must exceed one poll interval or it never elapses");
static_assert(kGamepadRepeatNextMs > 0 &&
                  kGamepadRepeatNextMs <= kGamepadRepeatFirstMs,
              "repeat interval must be positive and no slower than the first delay");
static_assert(kGamepadRepeatFirstTicks > 0 && kGamepadRepeatNextTicks > 0,
              "gamepad repeat timing must survive tick quantisation");
// 双击窗口必须长于去抖（否则一次按键的抖动会被当成两击），
// 又必须短于长按门槛（否则长按翻页会被先判成一次单击，再拖到窗口里）。
static_assert(kBootDoubleClickMs > kButtonDebounceMs &&
                  kBootDoubleClickMs < kButtonLongPressMs,
              "double-click window must sit between debounce and long press");
static_assert(kBootDoubleClickTicks > 0,
              "double-click window must survive tick quantisation");

// 运行期按键语义（只有两个物理键，靠「按住时长」把 8 个 GB 按键压出来）：
//   BOOT 短按            = A
//   BOOT 按住 ≥250ms     = A + START
//   KEY  短按            = B
//   KEY  按住 ≥250ms     = B + SELECT
//   双键同按 ≥600ms      = 暂停 / 继续
//   双键同按 ≥2000ms     = 退出并保存 .sav
//
// 为什么暂停挂在「双键 600ms」而不是 team-lead 最初说的「KEY 短按」：
// 短按 KEY 已经是 B。B 是高频键（马里奥按住跑、宝可梦取消），把它挪去暂停，
// 游戏直接没法玩；把它挪到「KEY 长按」也不行——按住 B 跑动超过 800ms 就会误暂停。
// A+B 同时按住 600ms 不是任何 GB 游戏的常规操作，误触率最低，而且和退出共用
// 同一个手势，用户只需要记住「同时按两个键，短按暂停、按住就退出」。
// 双键按住期间一个按键都不喂给 GB，避免暂停时游戏卡在「A 按住」状态。
void poll_gamepad_while_running(bool boot_pressed, bool key_pressed, TickType_t now)
{
    static TickType_t boot_pressed_since = 0;
    static TickType_t key_pressed_since = 0;
    static TickType_t both_pressed_since = 0;
    static bool exit_handled = false;
    static bool pause_handled = false;

    // 记录每个键各自的按下时刻（0 表示当前没按下）。
    if (boot_pressed) {
        if (boot_pressed_since == 0) {
            boot_pressed_since = now;
        }
    } else {
        boot_pressed_since = 0;
    }
    if (key_pressed) {
        if (key_pressed_since == 0) {
            key_pressed_since = now;
        }
    } else {
        key_pressed_since = 0;
    }

    // 手柄的 START+SELECT 与「本机双键同按」等价。退出不能挂在手柄的单键上：
    // START 在宝可梦里是菜单、SELECT 在不少游戏里是切换道具，单独长按都会
    // 被当成正常操作；而两个一起按在 GB 游戏里没有任何常规含义。
    const bool pad_both = pad_button(GB_BTN_START) && pad_button(GB_BTN_SELECT);
    const bool both = (boot_pressed && key_pressed) || pad_both;
    if (!both) {
        both_pressed_since = 0;
        exit_handled = false;
        pause_handled = false;
    } else if (both_pressed_since == 0) {
        both_pressed_since = now;
    }

    if (both) {
        const TickType_t held = now - both_pressed_since;
        if (!exit_handled && held >= kGamepadExitHoldTicks) {
            exit_handled = true;
            ESP_LOGI(TAG, BUTTON_GAME_EXIT_LOG);
            gameboy_pad_release_all();
            gameboy_runtime_stop();
            s_swallow_until_release = true;
            notify_ui_task();
            return;
        }
        if (!pause_handled && !exit_handled && held >= kGamepadPauseHoldTicks) {
            pause_handled = true;
            // 先松开所有 GB 按键再暂停，否则游戏会记住「A 还按着」，
            // 恢复瞬间主角自己往前跑。
            gameboy_pad_release_all();
            gameboy_runtime_toggle_pause();
            ESP_LOGI(TAG, BUTTON_GAME_PAUSE_LOG);
        }
        // 双键按住期间不产生任何 GB 输入。
        gameboy_pad_set(GB_BTN_A, false);
        gameboy_pad_set(GB_BTN_B, false);
        gameboy_pad_set(GB_BTN_START, false);
        gameboy_pad_set(GB_BTN_SELECT, false);
        return;
    }

    const bool boot_held_long =
        boot_pressed && (now - boot_pressed_since) >= kGamepadModifierHoldTicks;
    const bool key_held_long =
        key_pressed && (now - key_pressed_since) >= kGamepadModifierHoldTicks;

    gameboy_pad_set(GB_BTN_A, boot_pressed);
    gameboy_pad_set(GB_BTN_B, key_pressed);
    gameboy_pad_set(GB_BTN_START, boot_held_long);
    gameboy_pad_set(GB_BTN_SELECT, key_held_long);
}

// ROM 列表页的手柄导航：十字键上/下换焦点，A 或 B 执行当前焦点项，
// 左/右直接调音量。
//
// 关键点是 pad_button() 返回的是**当前电平**，不是「按下事件」。直接按电平
// 响应的话，按住不放会以轮询频率疯狂翻页（20ms 一次 = 每秒 50 项）。所以这里
// 自己做边沿检测 + 长按连发：刚按下走一步，按住超过 kGamepadRepeatFirstMs
// 之后按 kGamepadRepeatNextMs 的节奏继续走。
//
// A/B 只认「刚按下」那一下：列表页没有 A/B 的连发语义，连发会把 ROM 连续
// 启动好几次（第一次成功后模拟器就接管了，后面几次只是白打）。
//
// 左/右是音量的全局快捷键，同样只认边沿，**且不参与连发**：音量一共 6 档，
// 连发会让手指一抖就绕好几圈，用户根本停不到想要的档位上。
// 执行「当前焦点项」：焦点在音量行上时是音量 +1 档，否则启动当前 ROM。
//
// ⚠ 音量档位动不了时**必须退回启动路径**，不能把这一下吞掉。
//   s_volume_focused 是被左右键（音量快捷键）**粘住**的 ——
//   game_page_volume_step_up() / step_down() 每次都会把它置 true，而只有上下
//   换焦点（game_page_focus_next/prev）才会清掉。所以用户只要碰过一次左右，
//   焦点就一直停在音量行上。此时再按 A：档位已经到顶的话
//   game_audio_volume_step_up() 返回 false，屏幕上一点变化都没有，连日志都
//   不打 —— 用户看到的就是「A 键时灵时不灵」，而它其实从来没坏过，只是焦点
//   被粘在了一行上。
//
//   音量行是快捷键带出来的临时焦点，不是用户主动上下选中的一行。档位动得了
//   就调音量（有可见变化），动不了就回到主路径启动 ROM，保证按下去一定有反应。
static bool game_page_activate_focused_item()
{
    if (game_page_volume_focused()) {
        if (game_page_volume_step_up()) {
            ESP_LOGI(TAG, BUTTON_GAME_VOLUME_UP_LOG);
            return true;
        }
        ESP_LOGW(TAG, "volume at max while focused, fall through to start");
    }
    if (game_page_start_selected()) {
        ESP_LOGI(TAG, BUTTON_GAME_START_LOG);
        return true;
    }
    return false;
}

void poll_gamepad_in_game_list(TickType_t now)
{
    static int         dir_was = 0;      // -1 上 / 0 无 / +1 下
    static bool        launch_was = false;
    static bool        repeating = false;   // 已进入连发（用更短的间隔）
    static TickType_t  repeat_at = 0;
    static bool        left_was = false;
    static bool        right_was = false;

    const bool up = pad_button(GB_BTN_UP);
    const bool down = pad_button(GB_BTN_DOWN);
    const bool launch = pad_button(GB_BTN_A) || pad_button(GB_BTN_B);
    const bool left = pad_button(GB_BTN_LEFT);
    const bool right = pad_button(GB_BTN_RIGHT);

    if (launch && !launch_was) {
        launch_was = true;
        game_page_activate_focused_item();
        notify_ui_task();
        // 启动成功的话下一轮就进模拟器分支了，别把这一帧的方向键继续往下喂。
        dir_was = up ? -1 : (down ? 1 : 0);
        left_was = left;
        right_was = right;
        return;
    }
    launch_was = launch;

    // 左右只走边沿，且刻意不碰 dir_was/repeat_at：它们和上/下共用同一批
    // 状态变量，被左右改到的话上下键的连发节奏会莫名其妙地断掉或加速。
    if ((left && !left_was) || (right && !right_was)) {
        const bool acted = (left && !left_was) ? game_page_volume_step_down()
                                              : game_page_volume_step_up();
        if (acted) {
            if (left && !left_was) {
                ESP_LOGI(TAG, BUTTON_GAME_VOLUME_DOWN_LOG);
            } else {
                ESP_LOGI(TAG, BUTTON_GAME_VOLUME_UP_LOG);
            }
            notify_ui_task();
        }
    }
    left_was = left;
    right_was = right;

    const int dir = up ? -1 : (down ? 1 : 0);
    bool acted = false;
    if (dir == 0) {
        // 松手：取消连发，下次按下重新走「先等一会儿」的节奏。
        repeat_at = 0;
        repeating = false;
    } else if (dir != dir_was) {
        // 刚按下（或从上切到下）：立刻走一步，然后排第一次连发。
        acted = (dir < 0) ? game_page_focus_prev() : game_page_focus_next();
        repeat_at = (TickType_t)(now + kGamepadRepeatFirstTicks);
        repeating = false;
    } else if (!repeating &&
               (TickType_t)(now - repeat_at) >= kGamepadRepeatFirstTicks) {
        acted = (dir < 0) ? game_page_focus_prev() : game_page_focus_next();
        repeat_at = (TickType_t)(now + kGamepadRepeatNextTicks);
        repeating = true;
    } else if (repeating &&
               (TickType_t)(now - repeat_at) >= kGamepadRepeatNextTicks) {
        acted = (dir < 0) ? game_page_focus_prev() : game_page_focus_next();
        repeat_at = (TickType_t)(now + kGamepadRepeatNextTicks);
    }
    dir_was = dir;
    if (acted) {
        // 不能用三元表达式挑格式串：ESP_LOGI 是宏，条件运算符会让实参
        // 拆分出错（实测报 "expression cannot be used as a function"）。
        if (dir < 0) {
            ESP_LOGI(TAG, BUTTON_GAME_PREV_ROM_LOG);
        } else {
            ESP_LOGI(TAG, BUTTON_GAME_NEXT_ROM_LOG);
        }
        notify_ui_task();
    }
}

bool button_press_is_short(TickType_t held)
{
    return held >= kButtonDebounceTicks &&
           held < kButtonLongPressTicks;
}

bool button_press_is_long(TickType_t held)
{
    return held >= kButtonLongPressTicks;
}

// 绑定页的按键语义，只在这一页生效：
//   BOOT 短按 = 选中下一个动作
//   KEY  短按 = 对当前动作发起捕获（再按一次取消）
//   KEY  长按 = 退出绑定页
//
// 这三条必须独吞这一轮按键处理（调用方直接 continue），否则 BOOT 短按会去换
// ROM、KEY 长按会进设置页，绑定页根本没法用。
//
// 长按为什么不走 button_press_is_long()：那个判据是"松开时才算"，而退出动作
// 要在**按住到 1200ms 的那一刻**就发生，不能等松手 —— 用户一直按着页面不关，
// 会以为卡死了。所以这里在按住途中就处理，并用 key_handled 保证只触发一次。
void handle_bind_page_buttons(bool boot_pressed, bool key_pressed, TickType_t now)
{
    static TickType_t boot_since = 0;
    static TickType_t key_since = 0;
    static bool       key_exit_handled = false;

    if (boot_pressed) {
        if (boot_since == 0) {
            boot_since = now;
        } else if (button_press_is_long(now - boot_since)) {
            // 长按 BOOT = 清空全部绑定。
            //
            // 这是救命口子：一旦某条绑定锁死在一个会自抖动的字节上（典型是
            // 模拟摇杆轴），表现就是「没碰手柄，按键却一直在触发」，模拟器
            // 完全没法玩，而绑定页上根本看不出是哪一条坏了。没有这个口子，
            // 用户唯一的出路是重新烧固件。
            boot_since = 0;
            pad_bind_clear_all();
            pad_bind_capture_cancel();
            ESP_LOGW(TAG, BUTTON_GAME_BIND_CLEAR_LOG);
            notify_ui_task();
            return;
        }
    } else {
        if (boot_since != 0 && button_press_is_short(now - boot_since)) {
            game_bind_page_next_action();
            notify_ui_task();
        }
        boot_since = 0;
    }

    if (key_pressed) {
        if (key_since == 0) {
            key_since = now;
            key_exit_handled = false;
        } else if (!key_exit_handled &&
                   button_press_is_long(now - key_since)) {
            key_exit_handled = true;
            game_bind_page_close();
            ESP_LOGI(TAG, BUTTON_GAME_BIND_CLOSE_LOG);
            notify_ui_task();
        }
    } else {
        // 松手才算短按：按住的时候不能判定，否则每次长按都会先误触发一次绑定。
        if (key_since != 0 && !key_exit_handled &&
            button_press_is_short(now - key_since)) {
            game_bind_page_bind_current();
            notify_ui_task();
        }
        key_since = 0;
        key_exit_handled = false;
    }
}

void return_to_system_settings_item(int selection, TickType_t now)
{
    settings_page_request();
    enter_settings_system_item_navigation(selection);
    settings_activity_record(now);
}

void enter_settings_primary_menu(TickType_t now)
{
    info_page_clear();
    settings_page_request();
    enter_settings_primary_navigation();
    settings_activity_record(now);
}

void handle_settings_key_long_or_busy()
{
    if (!is_settings_sync_busy() && !ota_flow_active()) {
        handle_settings_key_long();
        return;
    }
    set_settings_feedback(kSettingsBusyFeedbackText, kButtonBusyFeedbackMs);
}

void IRAM_ATTR notify_button_edge(void *)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (!s_button_task_target.notify_from_isr(&higher_priority_task_woken)) {
        return;
    }
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void disable_button_interrupts()
{
    (void)gpio_set_intr_type(kBootButtonGpio, GPIO_INTR_DISABLE);
    (void)gpio_set_intr_type(kKeyButtonGpio, GPIO_INTR_DISABLE);
}

void remove_button_isr_handlers(bool boot_registered, bool key_registered)
{
    if (boot_registered) {
        (void)gpio_isr_handler_remove(kBootButtonGpio);
    }
    if (key_registered) {
        (void)gpio_isr_handler_remove(kKeyButtonGpio);
    }
    disable_button_interrupts();
}

bool configure_button_gpio_with_retry(const gpio_config_t &config)
{
    for (unsigned attempt = 1;
         attempt <= kButtonGpioConfigMaxAttempts;
         ++attempt) {
        const esp_err_t err = gpio_config(&config);
        if (err == ESP_OK) {
            return true;
        }
        if (!button_gpio_config_retry_due(attempt, err == ESP_OK)) {
            ESP_LOGE(TAG,
                     BUTTON_GPIO_CONFIG_FAILED_LOG_FORMAT,
                     esp_err_to_name(err));
            return false;
        }
        ESP_LOGW(TAG,
                 BUTTON_GPIO_CONFIG_RETRY_LOG_FORMAT,
                 esp_err_to_name(err),
                 static_cast<unsigned>(attempt + 1U),
                 kButtonGpioConfigMaxAttempts);
        vTaskDelay(pdMS_TO_TICKS(kButtonGpioConfigRetryDelayMs));
    }
    return false;
}

bool setup_button_edge_wakeup()
{
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, BUTTON_ISR_SERVICE_FAILED_LOG_FORMAT, esp_err_to_name(err));
        disable_button_interrupts();
        return false;
    }

    bool boot_registered = false;
    bool key_registered = false;
    err = gpio_isr_handler_add(kBootButtonGpio, notify_button_edge, nullptr);
    if (err == ESP_OK) {
        boot_registered = true;
    } else {
        ESP_LOGW(TAG,
                 BUTTON_ISR_HANDLER_FAILED_LOG_FORMAT,
                 static_cast<int>(kBootButtonGpio),
                 esp_err_to_name(err));
        remove_button_isr_handlers(boot_registered, key_registered);
        return false;
    }

    err = gpio_isr_handler_add(kKeyButtonGpio, notify_button_edge, nullptr);
    if (err == ESP_OK) {
        key_registered = true;
    } else {
        ESP_LOGW(TAG,
                 BUTTON_ISR_HANDLER_FAILED_LOG_FORMAT,
                 static_cast<int>(kKeyButtonGpio),
                 esp_err_to_name(err));
        remove_button_isr_handlers(boot_registered, key_registered);
        return false;
    }

    err = esp_sleep_enable_ext1_wakeup_io(kButtonInputPinMask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, BUTTON_WAKEUP_FAILED_LOG_FORMAT, esp_err_to_name(err));
        remove_button_isr_handlers(boot_registered, key_registered);
        return false;
    }

    ESP_LOGI(TAG, BUTTON_EDGE_WAKEUP_READY_LOG_FORMAT);
    // 调试期临时：把页面启用掩码打在这里。启动早期的日志会被串口打开前
    // 那一秒多的时间差吃掉，这里已经到了启动后段，稳定读得到。
    // 迁移有没有生效，看 game_bit 是不是 1。验证完必须删掉。
    {
        const uint8_t m = work_page_enabled_mask_load();
        ESP_LOGI(TAG, "page_mask=0x%02x game_bit=%d first=%d",
                 m,
                 (m & (1U << kWorkPageGameBoy)) != 0,
                 first_enabled_work_page());
    }
    return true;
}
} // namespace

void button_task(void *)
{
    gpio_config_t button = {};
    button.intr_type = GPIO_INTR_ANYEDGE;
    button.mode = GPIO_MODE_INPUT;
    button.pin_bit_mask = kButtonInputPinMask;
    button.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    if (!configure_button_gpio_with_retry(button)) {
        vTaskDelete(nullptr);
        return;
    }

    s_button_task_target.publish(xTaskGetCurrentTaskHandle());
    const bool edge_wakeup_ready = setup_button_edge_wakeup();
    regular_app_task_mark_ready(RegularAppTaskId::kButton);

    TickType_t boot_pressed_since = 0;
    TickType_t key_pressed_since = 0;
    bool key_press_opened_settings = false;
    bool key_long_handled = false;
    bool boot_press_stopped_alert = false;
    bool key_press_stopped_alert = false;
    // BOOT 单击的延迟执行时刻（0 = 没有待执行的单击）。
    // 双击 BOOT 要进绑定页，所以一次短按必须等过双击窗口才能确定它是单击。
    TickType_t boot_pending_since = 0;

    for (;;) {
        runtime_health_record_current_task_stack(RegularAppTaskId::kButton);
        TickType_t now = xTaskGetTickCount();
        bool boot_pressed = gpio_get_level(kBootButtonGpio) == 0;
        bool key_pressed = gpio_get_level(kKeyButtonGpio) == 0;

        // 游戏运行中：整块按键语义归模拟器，本任务只做手柄轮询。
        if (gameboy_runtime_running()) {
            poll_gamepad_while_running(boot_pressed, key_pressed, now);
            vTaskDelay(pdMS_TO_TICKS(kGamepadPollMs));
            continue;
        }

        // 刚退出游戏：等用户把两个键都松开再恢复正常语义（见文件上方说明）。
        if (!boot_pressed && !key_pressed) {
            s_swallow_until_release = false;
        }
        if (s_swallow_until_release) {
            boot_pressed_since = 0;
            key_pressed_since = 0;
            vTaskDelay(pdMS_TO_TICKS(kButtonActivePollMs));
            continue;
        }

        // 游戏页 idle 态：BOOT 短按换 ROM / 长按翻页，KEY 短按开始 / 长按进设置。
        const bool game_idle_page =
            active_work_page_load() == kWorkPageGameBoy &&
            !settings_page_requested() && !info_page_requested() &&
            !network_diag_page_requested() && !setup_portal_active_load() &&
            !battery_low_mode_load();

        // 双击窗口过去了还没等到第二击 —— 这一下是单击，执行「换 ROM」。
        // 放在绑定页分支之前：进了绑定页就不会有遗留的单击排队。
        if (boot_pending_since != 0 &&
            (TickType_t)(now - boot_pending_since) > kBootDoubleClickTicks) {
            boot_pending_since = 0;
            // 焦点在 4 行 ROM + 音量行之间循环，走到最后一个 ROM 之后是音量行。
            if (game_idle_page && game_page_focus_next()) {
                ESP_LOGI(TAG, BUTTON_GAME_NEXT_ROM_LOG);
                notify_ui_task();
            }
        }

        if (game_bind_page_active()) {
            // 绑定页独吞这一轮：见 handle_bind_page_buttons() 的说明。
            handle_bind_page_buttons(boot_pressed, key_pressed, now);
            vTaskDelay(pdMS_TO_TICKS(kButtonActivePollMs));
            continue;
        }

        // 停在 ROM 列表页且手柄已就绪时，本任务必须保持短周期轮询。
        // 下面有两处会把任务挂进长眠分支，都得靠这个标志放开：
        //   1) button_task_can_wait_for_edge() 之后是 ulTaskNotifyTake(portMAX_DELAY)，
        //      只会被物理按键的 GPIO 边沿唤醒，手柄按到天亮也扫不到；
        //   2) 末尾的 button_task_poll_delay_ms() 在墨水屏 idle 下是几百毫秒。
        const bool gamepad_needs_poll =
            game_idle_page && pad_state() == PAD_STATE_READY;
        if (gamepad_needs_poll) {
            poll_gamepad_in_game_list(now);
        }

        if (boot_pressed) {
            if (boot_pressed_since == 0) {
                boot_pressed_since = now;
                boot_press_stopped_alert = alarm_stop_ringing_from_button() ||
                                           pomodoro_stop_alert_from_button();
                if (settings_page_requested()) {
                    settings_activity_record(now);
                }
            }
        } else {
            if (boot_pressed_since != 0 && boot_press_stopped_alert) {
                // 提醒音播放期间任意按键只负责停止音频，不继续执行原按键动作。
            } else if (boot_pressed_since != 0 && settings_page_requested()) {
                TickType_t held = now - boot_pressed_since;
                if (button_press_is_short(held)) {
                    settings_activity_record_action(now);
                    notify_ui_task();
                } else {
                    settings_activity_record(now);
                }
            } else if (boot_pressed_since != 0 && game_idle_page) {
                TickType_t held = now - boot_pressed_since;
                if (button_press_is_long(held)) {
                    // 长按翻页立即执行，并且要把待执行的单击取消掉 ——
                    // 否则翻页之后还会再补一次"换 ROM"。
                    boot_pending_since = 0;
                    int next_page = next_enabled_work_page(active_work_page_load());
                    active_work_page_store(next_page);
                    ESP_LOGI(TAG, BUTTON_SWITCH_WORK_PAGE_LOG_FORMAT, next_page + 1);
                    notify_ui_task();
                } else if (button_press_is_short(held)) {
                    // 双击 BOOT 进绑定页。单击不在这里执行：要等 kBootDoubleClickMs
                    // 过去才能确定没有第二击，见循环里"双击窗口过去"那一段。
                    if (boot_pending_since != 0 &&
                        (TickType_t)(now - boot_pending_since) <= kBootDoubleClickTicks) {
                        boot_pending_since = 0;
                        game_bind_page_open();
                        ESP_LOGI(TAG, BUTTON_GAME_BIND_OPEN_LOG);
                        notify_ui_task();
                    } else {
                        boot_pending_since = now;
                    }
                }
            } else if (boot_pressed_since != 0 &&
                       !info_page_requested() &&
                       !network_diag_page_requested() &&
                       !setup_portal_active_load() &&
                       !battery_low_mode_load()) {
                TickType_t held = now - boot_pressed_since;
                if (button_press_is_short(held)) {
                    int next_page = next_enabled_work_page(active_work_page_load());
                    active_work_page_store(next_page);
                    ESP_LOGI(TAG, BUTTON_SWITCH_WORK_PAGE_LOG_FORMAT, next_page + 1);
                    notify_ui_task();
                }
            }
            boot_pressed_since = 0;
            boot_press_stopped_alert = false;
        }

        if (key_pressed) {
            if (key_pressed_since == 0) {
                key_pressed_since = now;
                key_press_opened_settings = false;
                key_long_handled = false;
                key_press_stopped_alert = alarm_stop_ringing_from_button() ||
                                          pomodoro_stop_alert_from_button();
                if (settings_page_requested()) {
                    settings_activity_record(now);
                }
                if (!key_press_stopped_alert && !game_idle_page &&
                    !settings_page_requested() && !info_page_requested() && !network_diag_page_requested()) {
                    ESP_LOGI(TAG, BUTTON_SHOW_SETTINGS_LOG_FORMAT);
                    enter_settings_primary_menu(now);
                    key_press_opened_settings = true;
                    notify_ui_task();
                }
            } else if (!key_press_stopped_alert &&
                       !key_press_opened_settings &&
                       !key_long_handled &&
                       settings_page_requested() &&
                       button_press_is_long(now - key_pressed_since)) {
                settings_activity_record(now);
                handle_settings_key_long_or_busy();
                key_long_handled = true;
                notify_ui_task();
            } else if (!key_long_handled &&
                       game_idle_page &&
                       button_press_is_long(now - key_pressed_since)) {
                // 游戏页 idle：长按 KEY 进设置（短按留给“开始游戏”）。
                ESP_LOGI(TAG, BUTTON_GAME_SETTINGS_LOG);
                enter_settings_primary_menu(now);
                key_long_handled = true;
                notify_ui_task();
            } else if (!key_long_handled &&
                       info_page_requested() &&
                       !settings_page_requested() &&
                       button_press_is_long(now - key_pressed_since)) {
                info_page_clear();
                return_to_system_settings_item(kSystemSettingsInfoItem, now);
                key_long_handled = true;
                notify_ui_task();
            } else if (!key_long_handled &&
                       network_diag_page_requested() &&
                       !settings_page_requested() &&
                       button_press_is_long(now - key_pressed_since)) {
                cancel_network_diagnostics_sync();
                network_diag_page_clear();
                return_to_system_settings_item(kSystemSettingsNetworkDiagItem, now);
                key_long_handled = true;
                notify_ui_task();
            }
        } else {
            if (key_pressed_since != 0 && !key_press_stopped_alert &&
                !key_long_handled && game_idle_page) {
                // 游戏页 idle：KEY 短按 = 执行当前焦点项。
                // 焦点在音量行时是「音量 +1 档」（越过最大档回绕到静音），
                // 否则是「开始/继续游戏」。
                if (button_press_is_short(now - key_pressed_since)) {
                    game_page_activate_focused_item();
                    // 调音量也要刷新：档位变化是格子填充数的变化，不 notify
                    // 的话墨水屏会停在旧档位上。
                    notify_ui_task();
                }
            }
            if (key_pressed_since != 0 &&
                !key_press_stopped_alert &&
                !key_press_opened_settings && !key_long_handled && settings_page_requested()) {
                TickType_t held = now - key_pressed_since;
                if (button_press_is_long(held)) {
                    settings_activity_record(now);
                    handle_settings_key_long_or_busy();
                    notify_ui_task();
                } else if (button_press_is_short(held)) {
                    settings_activity_record(now);
                    if (!is_settings_sync_busy() && !ota_flow_active()) {
                        handle_settings_key_short();
                    } else {
                        set_settings_feedback(kSettingsBusyFeedbackText, kButtonBusyFeedbackMs);
                        notify_ui_task();
                    }
                }
            }
            if (key_pressed_since != 0 && settings_page_requested()) {
                settings_activity_record(now);
            }
            key_pressed_since = 0;
            key_press_opened_settings = false;
            key_long_handled = false;
            key_press_stopped_alert = false;
        }
        // 待执行的「单击 BOOT」也算有事在身：它要在 kBootDoubleClickMs 窗口
        // 过去之后由本循环补一次执行。不算的话，任务会在这里挂进
        // ulTaskNotifyTake(portMAX_DELAY)，只等 GPIO 边沿唤醒，那一声单击就
        // 要等到下一次按键才被补上（表现为按一下没反应，再按一下连换两次 ROM）。
        const bool press_tracking_active =
            boot_pressed_since != 0 || key_pressed_since != 0 ||
            boot_pending_since != 0;
        if (!gamepad_needs_poll &&
            button_task_can_wait_for_edge(edge_wakeup_ready,
                                          boot_pressed,
                                          key_pressed,
                                          press_tracking_active)) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const bool any_button_pressed = boot_pressed || key_pressed;
        bool interactive_surface = false;
        bool low_refresh_surface = false;
        if (!any_button_pressed) {
            const int active_page = active_work_page_load();
            const bool low_battery_mode = battery_low_mode_load();
            const UiRuntimeSurfaceSnapshot interactive_surfaces =
                ui_runtime_surface_snapshot_load();
            interactive_surface =
                interactive_surfaces.interactive_surface_requested();
            low_refresh_surface =
                low_battery_mode ||
                (is_work_page_enabled(active_page) &&
                 work_page_uses_low_refresh_idle(active_page));
        }
        int delay_ms = button_task_poll_delay_ms(any_button_pressed,
                                                 interactive_surface,
                                                 low_refresh_surface);
        if (gamepad_needs_poll && !any_button_pressed &&
            delay_ms > kGamepadUiPollMs) {
            delay_ms = kGamepadUiPollMs;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
