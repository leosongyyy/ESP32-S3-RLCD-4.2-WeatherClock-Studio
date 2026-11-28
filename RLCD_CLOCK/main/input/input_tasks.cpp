// 处理 BOOT 和 KEY 按键输入、页面切换和设置页操作请求。
#include "input_tasks.h"

#include "active_work_page_state_internal.h"
#include "alarm_services.h"
#include "app_metadata.h"
#include "app_task_readiness.h"
#include "battery_runtime_state.h"
#include "engine_api.h"   // 引擎层唯一 API 面
#include "input_button_config.h"
#include "input_button_wait_policy.h"
#include "network_diagnostics_state.h"
#include "network_sync_requests.h"
#include "ota_services.h"
#include "pomodoro_services.h"
#include "pomodoro/pomodoro_classic.h"
#include "runtime_health.h"
#include "task_notification_target.h"
#include "ui_game.h"
#include "ui_game_bind.h"
#include "ui_info_page_state.h"
#include "ui_main_menu.h"
#include "ui_pomodoro.h"
#include "ui_reading.h"
#include "ui_settings_activity_state.h"
#include "ui_settings_feedback.h"
#include "ui_settings_navigation.h"
#include "ui_time_calendar.h"
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
// 阅读弹层里「按住 BOOT = 加/删书签」的门槛。必须明显长于双击窗口
// （350ms），否则双击的第二下还没落下就被判成长按；又短于全局长按
// （1200ms），因为这里按住不放等 1.2 秒太难受，800ms 已经很清楚。
constexpr int kReadingBookmarkHoldMs = 800;
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
constexpr TickType_t kReadingBookmarkHoldTicks = pdMS_TO_TICKS(kReadingBookmarkHoldMs);
constexpr const char *kSettingsBusyFeedbackText = "请等待操作完成";
TaskNotificationTarget s_button_task_target;

// 主菜单选「设置」时复用现有入口；其定义在本文件后段。
void enter_settings_primary_menu(TickType_t now);

// 按键时长判据，定义在本文件后段；手柄全局导航会提前用到。
bool button_press_is_short(TickType_t held);
bool button_press_is_long(TickType_t held);

// 设置页返回动作，定义在本文件后段；手柄全局导航的 B 键会提前用到。
void handle_settings_key_long_or_busy();

// 从任何子页面回到主菜单主页。必须清掉辅助页请求，否则退出主页那一帧
// 会被残留的 settings/info/network_diag/bind 状态重新抢回对应页面。
// 当前工作页保持不变，主页只是覆盖在它上面；下次从主页选模块会显式重设。
void return_to_main_menu()
{
    if (pomodoro_shown()) {
        pomodoro_hide();   // 计时数据由 classic 核心后台继续维护
    }
    if (reading_shown()) {
        reading_hide();
    }
    if (game_bind_page_active()) {
        game_bind_page_close();
    }
    if (settings_page_requested()) {
        settings_page_clear();
    }
    if (info_page_requested()) {
        info_page_clear();
    }
    if (network_diag_page_requested()) {
        network_diag_page_clear();
    }
    main_menu_show();
    notify_ui_task();
    ESP_LOGI(TAG, "return to main menu");
}

// 执行主菜单当前选中项。返回 true 表示进入了模块（主页应收起）。
// 未实现的空模块返回 false，留在主页，避免按一下就掉回工作页。
bool activate_main_menu_selection(TickType_t now)
{
    const MainMenuAction action = main_menu_current_action();
    if (action == MainMenuAction::kWeatherClock) {
        active_work_page_store(kWorkPageWeatherClock);
    } else if (action == MainMenuAction::kGameBoy) {
        active_work_page_store(kWorkPageGameBoy);
    } else if (action == MainMenuAction::kSettings) {
        enter_settings_primary_menu(now);
    } else if (action == MainMenuAction::kPomodoro) {
        classic_init();
        pomodoro_show();
    } else if (action == MainMenuAction::kBookshelf) {
        reading_show();
    } else {
        return false;  // kNone，空模块
    }
    main_menu_hide();
    notify_ui_task();
    return true;
}

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
// 阅读里长按加书签必须比双击窗口更晚触发，不然双击会被吃成长按。
static_assert(kReadingBookmarkHoldMs > kBootDoubleClickMs &&
                  kReadingBookmarkHoldMs <= kButtonLongPressMs,
              "reading bookmark hold must sit after double-click window");

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
    static bool        shoulder_home_was = false;

    // LB+RB 同按 → 回主菜单（边沿），与全局导航页一致。
    const bool shoulder_home = pad_shoulders() == 0x03;
    if (shoulder_home && !shoulder_home_was) {
        ESP_LOGI(TAG, "gamepad LB+RB: return to main menu");
        return_to_main_menu();
        shoulder_home_was = true;
        return;
    }
    shoulder_home_was = shoulder_home;
    if (shoulder_home) return;

    const bool up = pad_button(GB_BTN_UP);
    const bool down = pad_button(GB_BTN_DOWN);
    const bool launch = pad_button(GB_BTN_A);   // A = 确认/启动
    const bool back = pad_button(GB_BTN_B);     // B = 返回主菜单
    const bool left = pad_button(GB_BTN_LEFT);
    const bool right = pad_button(GB_BTN_RIGHT);

    // B = 返回主菜单（边沿触发）
    static bool back_was = false;
    if (back && !back_was) {
        return_to_main_menu();
        back_was = true;
        // 下一帧主菜单会接手，本帧不再往下喂方向键。
        dir_was = 0;
        left_was = left;
        right_was = right;
        return;
    }
    back_was = back;

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

// 番茄钟页手柄语义（独吞这一轮输入）：
// 阅读模块的手柄操作：
//   书库：方向(边沿) = 移动选中格；A = 打开这本书；B = 回主页
//   正文：左/上 = 上一页，右/下 = 下一页；B = 回书库
//         双击 A(350ms 内两次) = 切换全屏阅读（全屏里同样操作退出）
//         START(边沿)  = 当前页加/删书签
//         SELECT(边沿) = 打开目录/书签弹层
//   弹层：上/下 = 选择条目；左/右 = 切换目录、书签页签；
//         A = 跳到选中条目并关闭；B/SELECT = 关闭；START = 加/删书签
//   LB+RB(边沿) = 回主页
// 返回 true 表示本帧处理了输入（调用方据此 notify UI）。
bool poll_gamepad_reading(TickType_t now)
{
    if (pad_state() != PAD_STATE_READY) return false;
    bool acted = false;

    static bool shoulder_home_was = false;
    const bool shoulder_home = pad_shoulders() == 0x03;
    if (shoulder_home && !shoulder_home_was) {
        ESP_LOGI(TAG, "reading LB+RB: return to main menu");
        return_to_main_menu();
        shoulder_home_was = true;
        return true;
    }
    shoulder_home_was = shoulder_home;
    if (shoulder_home) return false;

    // 四个方向的边沿锁存，两套视图共用（视图切换时不会残留按住状态）。
    static bool dir_was[4] = {false, false, false, false};
    const bool dir_now[4] = {pad_button(GB_BTN_LEFT), pad_button(GB_BTN_RIGHT),
                             pad_button(GB_BTN_UP), pad_button(GB_BTN_DOWN)};

    static bool a_was = false;
    static bool b_was = false;
    static bool start_was = false;
    static bool select_was = false;
    // 双击 A 切全屏用：记住上一次 A 按下的时刻。
    static TickType_t a_tap_tick = 0;
    static bool a_tap_pending = false;
    const bool a_now = pad_button(GB_BTN_A);
    const bool b_now = pad_button(GB_BTN_B);
    const bool start_now = pad_button(GB_BTN_START);
    const bool select_now = pad_button(GB_BTN_SELECT);

    const bool left_edge = dir_now[0] && !dir_was[0];
    const bool right_edge = dir_now[1] && !dir_was[1];
    const bool up_edge = dir_now[2] && !dir_was[2];
    const bool down_edge = dir_now[3] && !dir_was[3];
    const bool a_edge = a_now && !a_was;
    const bool b_edge = b_now && !b_was;
    const bool start_edge = start_now && !start_was;
    const bool select_edge = select_now && !select_was;

    if (reading_view() == ReadingView::kShelf) {
        if (left_edge) { reading_move_selection(-1, 0); acted = true; }
        if (right_edge) { reading_move_selection(1, 0); acted = true; }
        if (up_edge) { reading_move_selection(0, -1); acted = true; }
        if (down_edge) { reading_move_selection(0, 1); acted = true; }
        if (a_edge) {
            reading_open_selected();
            ESP_LOGI(TAG, "reading: open book index=%d", reading_selection());
            acted = true;
        }
        if (b_edge) {
            return_to_main_menu();
            acted = true;
        }
    } else if (reading_panel_shown()) {
        if (up_edge) { reading_panel_move(-1); acted = true; }
        if (down_edge) { reading_panel_move(1); acted = true; }
        if (left_edge) { reading_panel_switch_tab(-1); acted = true; }
        if (right_edge) { reading_panel_switch_tab(1); acted = true; }
        if (a_edge) {
            reading_panel_activate();
            ESP_LOGI(TAG, "reading: jump from panel");
            acted = true;
        }
        if (b_edge || select_edge) {
            reading_panel_close();
            acted = true;
        }
        if (start_edge) {
            const int r = reading_toggle_bookmark();
            ESP_LOGI(TAG, "reading: toggle bookmark -> %d", r);
            acted = true;
        }
    } else {
        if (left_edge || up_edge) {
            reading_turn_page(-1);
            acted = true;
        }
        if (right_edge || down_edge) {
            reading_turn_page(1);
            acted = true;
        }
        if (b_edge) {
            reading_back_to_shelf();
            acted = true;
        }
        if (a_edge) {
            if (a_tap_pending &&
                (TickType_t)(now - a_tap_tick) <= kBootDoubleClickTicks) {
                a_tap_pending = false;
                reading_toggle_fullscreen();
                ESP_LOGI(TAG, "reading: fullscreen -> %d",
                         reading_fullscreen() ? 1 : 0);
            } else {
                a_tap_pending = true;
                a_tap_tick = now;
            }
            acted = true;
        }
        if (start_edge) {
            const int r = reading_toggle_bookmark();
            ESP_LOGI(TAG, "reading: toggle bookmark -> %d", r);
            acted = true;
        }
        if (select_edge) {
            reading_panel_open(0);
            ESP_LOGI(TAG, "reading: open toc panel");
            acted = true;
        }
    }

    for (int i = 0; i < 4; ++i) dir_was[i] = dir_now[i];
    a_was = a_now;
    b_was = b_now;
    start_was = start_now;
    select_was = select_now;
    return acted;
}

//   A(边沿)           = 开始 / 暂停
//   上/下(边沿+连发)  = 选 MODE（专注/短休/长休，选中即切换并重置）
//   左/右(边沿)       = 选 MODE（同上，便于单向操作）
//   B(边沿)           = 返回主页（计时后台继续）
//   LB+RB(边沿)       = 回主页（由调用前的全局肩键处理完成）
// 返回 true 表示本帧确实处理了输入（调用方据此 notify UI）。
bool poll_gamepad_pomodoro(TickType_t now)
{
    if (pad_state() != PAD_STATE_READY) return false;
    bool acted = false;

    // LB+RB 同按 → 回主菜单（边沿，与全局导航一致）。
    static bool shoulder_home_was = false;
    const bool shoulder_home = pad_shoulders() == 0x03;
    if (shoulder_home && !shoulder_home_was) {
        ESP_LOGI(TAG, "pomodoro LB+RB: return to main menu");
        return_to_main_menu();
        shoulder_home_was = true;
        return true;
    }
    shoulder_home_was = shoulder_home;
    if (shoulder_home) return false;

    const bool pad_up = pad_button(GB_BTN_UP);
    const bool pad_down = pad_button(GB_BTN_DOWN);
    const bool pad_left = pad_button(GB_BTN_LEFT);
    const bool pad_right = pad_button(GB_BTN_RIGHT);
    const bool pad_a = pad_button(GB_BTN_A);
    const bool pad_b = pad_button(GB_BTN_B);

    // A：单击 = 开始 / 暂停；双击 = 重置番茄钟。
    // 单击必须延迟到双击窗口结束才执行，否则双击会先误触发一次开始/暂停。
    static bool a_was = false;
    static TickType_t a_pending = 0;
    const bool a_now = pad_a;
    if (a_now && !a_was) {
        if (a_pending != 0 &&
            (TickType_t)(now - a_pending) <= kBootDoubleClickTicks) {
            // 双击成立：重置到专注第 1 节。
            a_pending = 0;
            classic_reset();
            ESP_LOGI(TAG, "pomodoro pad A double-click: reset");
            acted = true;
        } else {
            a_pending = now;   // 第一击，等窗口确认是否双击
        }
    }
    a_was = a_now;
    // 双击窗口过去仍无第二击 → 单击，执行开始/暂停。
    if (a_pending != 0 &&
        (TickType_t)(now - a_pending) > kBootDoubleClickTicks) {
        a_pending = 0;
        classic_toggle_start_pause();
        acted = true;
    }

    // B：返回主页（边沿，计时后台继续）。
    static bool b_was = false;
    const bool b_now = pad_b;
    if (b_now && !b_was) {
        return_to_main_menu();
        acted = true;
    }
    b_was = b_now;

    // 上下：选 MODE，边沿 + 连发。当前阶段即选中行，循环 0..2。
    static int ud_dir = 0;
    static TickType_t ud_repeat_at = 0;
    static bool ud_repeating = false;
    const int ud = pad_down ? 1 : (pad_up ? -1 : 0);
    bool mode_step = false;
    if (ud == 0) {
        ud_repeat_at = 0;
        ud_repeating = false;
    } else if (ud != ud_dir) {
        mode_step = true;
        ud_repeat_at = now;
        ud_repeating = false;
    } else if (!ud_repeating &&
               (TickType_t)(now - ud_repeat_at) >= kGamepadRepeatFirstTicks) {
        mode_step = true;
        ud_repeat_at = now + kGamepadRepeatNextTicks;
        ud_repeating = true;
    } else if (ud_repeating &&
               (TickType_t)(now - ud_repeat_at) >= kGamepadRepeatNextTicks) {
        mode_step = true;
        ud_repeat_at = now + kGamepadRepeatNextTicks;
    }
    ud_dir = ud;
    if (mode_step) {
        ClassicSnapshot snap = {};
        classic_get_snapshot(&snap);
        int mode = snap.selected_mode + (ud > 0 ? 1 : -1);
        if (mode < 0) mode = 2;
        if (mode > 2) mode = 0;
        classic_select_mode(mode);
        acted = true;
    }

    // 左右：选 MODE，短按边沿（长按不切页，番茄钟无工作页概念）。
    static int lr_dir = 0;
    const int lr = pad_right ? 1 : (pad_left ? -1 : 0);
    if (lr != lr_dir && lr != 0) {
        ClassicSnapshot snap = {};
        classic_get_snapshot(&snap);
        int mode = snap.selected_mode + lr;
        if (mode < 0) mode = 2;
        if (mode > 2) mode = 0;
        classic_select_mode(mode);
        acted = true;
    }
    lr_dir = lr;

    return acted;
}

// 手柄全局导航（工作页 / 设置页 / 信息页通用，游戏 ROM 列表页除外）。
// 语义对齐用户要求：
//   肩键 LB+RB 同时按(边沿) = 回主菜单（最高优先级，所有非游戏运行页生效）
//   上/下(边沿+连发)        = 选单移动（设置页选焦点，其它页切工作页）
//   左/右 短按(边沿)        = 选单移动（同上）
//   左/右 长按(按住到阈值)  = 切工作页（next/prev）
//   A(边沿)                = 确认
//   B(边沿)                = 返回
//
// 只在 pad READY、且不在游戏运行/游戏绑定页/主菜单时由调用方调用。
// 返回 true 表示本帧确实处理了手柄输入（调用方据此 notify UI）。
bool poll_gamepad_global_nav(TickType_t now)
{
    if (pad_state() != PAD_STATE_READY) return false;
    bool acted = false;

    // LB+RB 同按 → 回主菜单。只在边沿触发，避免长按连发。
    static bool shoulder_home_was = false;
    const bool shoulder_home = pad_shoulders() == 0x03;
    if (shoulder_home && !shoulder_home_was) {
        ESP_LOGI(TAG, "gamepad LB+RB: return to main menu");
        return_to_main_menu();
        shoulder_home_was = true;
        return true;
    }
    shoulder_home_was = shoulder_home;
    if (shoulder_home) return false;   // 按住肩键期间吞掉其它方向，避免误触

    const bool pad_up = pad_button(GB_BTN_UP);
    const bool pad_down = pad_button(GB_BTN_DOWN);
    const bool pad_left = pad_button(GB_BTN_LEFT);
    const bool pad_right = pad_button(GB_BTN_RIGHT);
    const bool pad_a = pad_button(GB_BTN_A);
    const bool pad_b = pad_button(GB_BTN_B);

    const bool in_settings = settings_page_requested();

    // ---- 左右：短按移选单，长按切页 ----
    static int  lr_dir = 0;              // -1 左 / +1 右 / 0 松开
    static TickType_t lr_since = 0;      // 本方向按下的时刻
    static bool lr_long_done = false;    // 本方向长按是否已触发
    static bool lr_short_played = false; // 本方向短按是否已触发
    const int lr = pad_right ? 1 : (pad_left ? -1 : 0);
    if (lr == 0) {
        lr_dir = 0;
    } else if (lr != lr_dir) {
        lr_dir = lr;
        lr_since = now;
        lr_long_done = false;
        lr_short_played = false;
    } else {
        const TickType_t held = now - lr_since;
        if (!lr_long_done && button_press_is_long(held)) {
            // 长按：切工作页（右=下一页，左=上一页）
            const int target = lr > 0 ? next_enabled_work_page(active_work_page_load())
                                      : prev_enabled_work_page(active_work_page_load());
            active_work_page_store(target);
            ESP_LOGI(TAG, BUTTON_SWITCH_WORK_PAGE_LOG_FORMAT, target + 1);
            lr_long_done = true;
            acted = true;
        } else if (!lr_long_done && !lr_short_played &&
                   button_press_is_short(held)) {
            // 短按：设置页移选单；其它页等同切一页
            if (in_settings) {
                handle_settings_key_short();
            } else if (active_work_page_load() == kWorkPageWeatherClock &&
                       !setup_portal_active_load() && !battery_low_mode_load()) {
                // 天气时钟短按左右 = 在「时间与日历 / 天气时钟」两个子页之间切换；
                // 长按左右仍然切工作页，保留原来的全局导航能力。
                time_calendar_toggle_page();
                ESP_LOGI(TAG, "weather clock subpage: %d", time_calendar_page());
            } else {
                const int target = lr > 0 ? next_enabled_work_page(active_work_page_load())
                                          : prev_enabled_work_page(active_work_page_load());
                active_work_page_store(target);
                ESP_LOGI(TAG, BUTTON_SWITCH_WORK_PAGE_LOG_FORMAT, target + 1);
            }
            lr_short_played = true;
            acted = true;
        }
    }
    // 松开时若既没短按也没长按触发过（极短一帧），补一次短按，防止漏键。
    if (lr == 0 && lr_dir != 0 && !lr_long_done && !lr_short_played) {
        if (in_settings) {
            handle_settings_key_short();
        }
        acted = acted || in_settings;
    }
    if (lr == 0) {
        lr_long_done = false;
        lr_short_played = false;
    }

    // ---- 上下：边沿触发选单移动，带连发 ----
    static int  ud_dir = 0;
    static TickType_t ud_repeat_at = 0;
    static bool ud_repeating = false;
    const int ud = pad_down ? 1 : (pad_up ? -1 : 0);
    bool ud_act = false;
    if (ud == 0) {
        ud_repeat_at = 0;
        ud_repeating = false;
    } else if (ud != ud_dir) {
        // 刚按下：立即走一步，第一次连发排在 kGamepadRepeatFirstMs 之后。
        ud_act = true;
        ud_repeat_at = now;
        ud_repeating = false;
    } else if (!ud_repeating && (TickType_t)(now - ud_repeat_at) >= kGamepadRepeatFirstTicks) {
        ud_act = true;
        ud_repeat_at = now + kGamepadRepeatNextTicks;
        ud_repeating = true;
    } else if (ud_repeating && (TickType_t)(now - ud_repeat_at) >= kGamepadRepeatNextTicks) {
        ud_act = true;
        ud_repeat_at = now + kGamepadRepeatNextTicks;
    }
    ud_dir = ud;
    if (ud_act) {
        if (in_settings) {
            // 设置页只有一维选单，上/下都走 KEY 短按（下移一格）；
            // 向上额外补一次，形成"上=倒数第二格"的近似（一维列表的取舍）。
            handle_settings_key_short();
            if (ud < 0) handle_settings_key_short();
        } else {
            // 普通工作页：上=上一页，下=下一页
            const int target = ud > 0 ? next_enabled_work_page(active_work_page_load())
                                      : prev_enabled_work_page(active_work_page_load());
            active_work_page_store(target);
            ESP_LOGI(TAG, BUTTON_SWITCH_WORK_PAGE_LOG_FORMAT, target + 1);
        }
        acted = true;
    }

    // ---- A = 确认 ----
    static bool a_was = false;
    const bool a_now = pad_a;
    if (a_now && !a_was) {
        if (in_settings) {
            // 等同 BOOT 短按：触发当前项动作
            settings_activity_record_action(now);
            acted = true;
        }
        // 普通工作页 A 键不再进设置：设置只能从主菜单第 8 个模块进入。
    }
    a_was = a_now;

    // ---- B = 返回 ----
    static bool b_was = false;
    const bool b_now = pad_b;
    if (b_now && !b_was) {
        if (in_settings) {
            handle_settings_key_long_or_busy();   // 设置页：返回上一级/退出
        } else {
            return_to_main_menu();                // 工作页：返回主页
        }
        acted = true;
    }
    b_was = b_now;

    return acted;
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

// 主菜单主页的按键语义（独吞这一轮，类似绑定页）：
//   物理 BOOT 单击   = 右移一格，行优先循环，第一排末尾进第二排，末尾回第一格
//   物理 BOOT 双击   = 打开手柄绑定页（保留原入口）
//   物理 KEY  单击   = 左移一格，反向循环
//   物理 KEY  双击   = 进入当前选中模块（空模块留在主页）
//   物理 BOOT/KEY 长按 = 预留，当前无动作（回主页用 BOOT+KEY 双键）
//   手柄 方向键      = 上下左右移动焦点（边沿触发，不连发）
//   手柄 A/B         = 进入当前选中模块（边沿触发）
// 两个物理键的单击都要等双击窗口过去才能结算，避免和双击语义抢。
// 进入模块后由这里切到对应工作页 / 设置页，并 main_menu_hide() 唤醒 UI 任务。
void handle_main_menu_buttons(bool boot_pressed, bool key_pressed, TickType_t now)
{
    static TickType_t boot_since = 0;
    static TickType_t boot_pending_since = 0;   // BOOT 单击待结算时刻
    static TickType_t boot_last_release = 0;
    static bool boot_long_handled = false;
    static TickType_t key_since = 0;
    static TickType_t key_pending_since = 0;    // KEY 单击待结算时刻
    static TickType_t key_last_release = 0;
    static bool key_long_handled = false;
    static bool pad_dir_was = false;
    static bool pad_action_was = false;

    // 双击窗口过去还没等到第二击 —— 单击结算。
    // 按住期间不结算，避免「短按后又按下变长按」先移再误触。
    if (boot_pending_since != 0 && !boot_pressed &&
        (TickType_t)(now - boot_pending_since) > kBootDoubleClickTicks) {
        boot_pending_since = 0;
        main_menu_step(1);   // BOOT 单击右移
    }
    if (key_pending_since != 0 && !key_pressed &&
        (TickType_t)(now - key_pending_since) > kBootDoubleClickTicks) {
        key_pending_since = 0;
        main_menu_step(-1);  // KEY 单击左移
    }

    // 手柄方向键：边沿触发一次，4 方向。
    const bool pad_up = pad_state() == PAD_STATE_READY && pad_button(GB_BTN_UP);
    const bool pad_down = pad_state() == PAD_STATE_READY && pad_button(GB_BTN_DOWN);
    const bool pad_left = pad_state() == PAD_STATE_READY && pad_button(GB_BTN_LEFT);
    const bool pad_right = pad_state() == PAD_STATE_READY && pad_button(GB_BTN_RIGHT);
    const bool pad_dir = pad_up || pad_down || pad_left || pad_right;
    if (pad_dir && !pad_dir_was) {
        if (pad_up) main_menu_move(0, -1);
        else if (pad_down) main_menu_move(0, 1);
        else if (pad_left) main_menu_move(-1, 0);
        else if (pad_right) main_menu_move(1, 0);
    }
    pad_dir_was = pad_dir;

    const bool pad_action = pad_state() == PAD_STATE_READY &&
                            (pad_button(GB_BTN_A) || pad_button(GB_BTN_B));
    const bool action_edge = (pad_action && !pad_action_was);
    pad_action_was = pad_action;

    // 物理 BOOT：单击右移，双击进绑定页。长按预留无动作。
    if (boot_pressed) {
        if (boot_since == 0) {
            boot_since = now;
            boot_long_handled = false;
            boot_pending_since = 0;
        }
    } else {
        if (boot_since != 0 && !boot_long_handled &&
            button_press_is_short(now - boot_since)) {
            if (boot_last_release != 0 &&
                (TickType_t)(now - boot_last_release) <= kBootDoubleClickTicks) {
                boot_last_release = 0;
                boot_pending_since = 0;
                boot_since = 0;
                // 绑定页是游戏工作页的子模式，先切到游戏页再开绑定页，
                // 否则主菜单独占帧会把绑定页挡在后面。
                active_work_page_store(kWorkPageGameBoy);
                game_bind_page_open();
                main_menu_hide();
                notify_ui_task();
                ESP_LOGI(TAG, BUTTON_GAME_BIND_OPEN_LOG);
                return;
            }
            boot_last_release = now;
            boot_pending_since = now;
        }
        boot_since = 0;
        boot_long_handled = false;
    }

    // 物理 KEY：单击左移，双击确认进入当前模块。长按预留无动作。
    if (key_pressed) {
        if (key_since == 0) {
            key_since = now;
            key_long_handled = false;
            key_pending_since = 0;
        }
    } else {
        if (key_since != 0 && !key_long_handled &&
            button_press_is_short(now - key_since)) {
            if (key_last_release != 0 &&
                (TickType_t)(now - key_last_release) <= kBootDoubleClickTicks) {
                // 双击 KEY = 确认进入。
                key_last_release = 0;
                key_pending_since = 0;
                key_since = 0;
                // 空模块由 activate_main_menu_selection 内部留住主页，不 hide。
                (void)activate_main_menu_selection(now);
                return;
            }
            key_last_release = now;
            key_pending_since = now;   // 单击先挂起，超时结算左移
        }
        key_since = 0;
        key_long_handled = false;
    }

    if (action_edge) {
        (void)activate_main_menu_selection(now);
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

    // 全局回主页手势：BOOT + KEY 同时短按后一起松开 → 回到主菜单主页。
    // 放在游戏运行分支之后：游戏中双键是暂停/退出，语义归模拟器。
    // 放在主菜单与其它页面分支之前：双键手势优先，且按住期间吞掉单键语义，
    // 避免同一轮又被当成 BOOT 换页 / KEY 确认。
    static bool     home_chord_active = false;
    static TickType_t home_chord_since = 0;

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

        // 双键同按状态机。只在非游戏运行态生效。
        if (boot_pressed && key_pressed) {
            if (!home_chord_active) {
                home_chord_active = true;
                home_chord_since = now;
            }
            // 双键按住期间吞掉所有单键语义。
            vTaskDelay(pdMS_TO_TICKS(kButtonActivePollMs));
            continue;
        }
        if (home_chord_active) {
            home_chord_active = false;
            const bool all_released = !boot_pressed && !key_pressed;
            const bool was_short = button_press_is_short(now - home_chord_since);
            vTaskDelay(pdMS_TO_TICKS(kButtonActivePollMs));
            if (all_released && was_short) {
                return_to_main_menu();
            }
            // 刚松开的这一帧也吞掉，不让残留的键触发单键动作。
            continue;
        }

        // 主菜单主页：独吞这一轮按键（方向/确认/进入模块）。
        if (main_menu_shown()) {
            handle_main_menu_buttons(boot_pressed, key_pressed, now);
            vTaskDelay(pdMS_TO_TICKS(kGamepadPollMs));
            continue;
        }

        // 番茄钟页：独吞这一轮输入。手柄 A=开始/暂停、上下左右=选 MODE、
        // B/LB+RB=回主页；物理 BOOT 单击=A、双击=从上到下循环切 MODE、KEY=B。
        if (pomodoro_shown()) {
            bool acted = poll_gamepad_pomodoro(now);
            static bool boot_was = false, key_was = false;
            // 物理 BOOT：单击要延迟到双击窗口结束才执行，否则双击会先误触发
            // 一次开始/暂停。这里用番茄钟页独立的静态变量，不与游戏页的
            // boot_pending_since 共用（两条分支都提前 continue，互不干扰）。
            static TickType_t pom_boot_pending = 0;
            if (boot_pressed && !boot_was) {
                if (pom_boot_pending != 0 &&
                    (TickType_t)(now - pom_boot_pending) <= kBootDoubleClickTicks) {
                    // 双击成立：从上到下循环切 MODE（专注→短休→长休→专注）。
                    pom_boot_pending = 0;
                    ClassicSnapshot snap = {};
                    classic_get_snapshot(&snap);
                    int mode = snap.selected_mode + 1;
                    if (mode > 2) mode = 0;
                    classic_select_mode(mode);
                    ESP_LOGI(TAG, "pomodoro BOOT double-click: mode=%d", mode);
                    acted = true;
                } else {
                    pom_boot_pending = now;   // 第一击，等窗口确认是否双击
                }
            }
            boot_was = boot_pressed;
            // 双击窗口过去仍无第二击 → 单击，执行开始/暂停。
            if (pom_boot_pending != 0 &&
                (TickType_t)(now - pom_boot_pending) > kBootDoubleClickTicks) {
                pom_boot_pending = 0;
                classic_toggle_start_pause();
                acted = true;
            }
            // 物理 KEY：单击 = 返回主页；双击 = 重置番茄钟。
            // 单击延迟到双击窗口结束，避免双击先误触发返回主页。
            static TickType_t pom_key_pending = 0;
            if (key_pressed && !key_was) {
                if (pom_key_pending != 0 &&
                    (TickType_t)(now - pom_key_pending) <= kBootDoubleClickTicks) {
                    // 双击成立：重置到专注第 1 节。
                    pom_key_pending = 0;
                    classic_reset();
                    ESP_LOGI(TAG, "pomodoro KEY double-click: reset");
                    acted = true;
                } else {
                    pom_key_pending = now;   // 第一击，等窗口确认是否双击
                }
            }
            key_was = key_pressed;
            if (pom_key_pending != 0 &&
                (TickType_t)(now - pom_key_pending) > kBootDoubleClickTicks) {
                pom_key_pending = 0;
                return_to_main_menu();
                acted = true;
            }
            if (acted) notify_ui_task();
            vTaskDelay(pdMS_TO_TICKS(kGamepadPollMs));
            continue;
        }

        // 阅读模块：书库与正文都独吞这一轮输入。手柄见 poll_gamepad_reading，
        // 物理键映射：BOOT 当 A、KEY 当 B。
        //   书库：BOOT 单击 = 打开；KEY = 回主菜单
        //   正文（弹层关着）：BOOT 单击 = 下一页、双击 = 上一页、长按 800ms = 全屏开关
        //                    KEY  单击 = 回书库、双击 = 目录弹层
        //   正文（弹层开着）：BOOT 单击 = 关弹层、长按 = 加/删书签；KEY 单击 = 关弹层
        // 单击一律等松手后才执行，否则按住不放会被判成单击，双击和长按就做不出来。
        if (reading_shown()) {
            bool acted = poll_gamepad_reading(now);
            static bool boot_was = false, key_was = false;
            static TickType_t rd_boot_since = 0;    // 本次按下的时刻，0 = 没按
            static TickType_t rd_boot_pending = 0;  // 候选单击：记按下时刻
            static bool rd_boot_long_done = false;  // 本次按住已触发长按
            static TickType_t rd_key_pending = 0;

            const bool boot_edge = boot_pressed && !boot_was;

            if (boot_edge) {
                rd_boot_since = now;
                rd_boot_long_done = false;
                if (reading_view() == ReadingView::kShelf) {
                    rd_boot_pending = 0;
                    reading_open_selected();
                    acted = true;
                } else if (rd_boot_pending != 0 &&
                           (TickType_t)(now - rd_boot_pending) <= kBootDoubleClickTicks) {
                    // 第二击落在窗口内：算双击，把第一次排队的单击吃掉。
                    rd_boot_pending = 0;
                    rd_boot_long_done = true;   // 这一下不再判长按
                    if (reading_panel_shown()) {
                        reading_panel_close();
                    } else {
                        reading_turn_page(-1);
                        ESP_LOGI(TAG, "reading BOOT double-click: prev page");
                    }
                    acted = true;
                } else {
                    rd_boot_pending = now;      // 先当候选单击，等松手再定
                }
            }

            // 按住不放：弹层里是加/删书签，正文里是全屏开关；一次按住只触发一次。
            if (boot_pressed && !rd_boot_long_done && rd_boot_pending != 0 &&
                (TickType_t)(now - rd_boot_since) >= kReadingBookmarkHoldTicks) {
                rd_boot_long_done = true;
                rd_boot_pending = 0;
                if (reading_panel_shown()) {
                    reading_panel_bookmark();
                    ESP_LOGI(TAG, "reading BOOT long-press: toggle bookmark");
                } else {
                    reading_toggle_fullscreen();
                    ESP_LOGI(TAG, "reading BOOT long-press: fullscreen=%d",
                             reading_fullscreen() ? 1 : 0);
                }
                acted = true;
            }

            // 已松手、双击窗口也过了还没等到第二击 → 这一下是单击。
            if (rd_boot_pending != 0 && !boot_pressed &&
                (TickType_t)(now - rd_boot_pending) > kBootDoubleClickTicks) {
                rd_boot_pending = 0;
                if (reading_panel_shown()) reading_panel_close();
                else reading_turn_page(1);
                acted = true;
            }

            if (key_pressed && !key_was) {
                if (reading_view() == ReadingView::kShelf) {
                    rd_key_pending = 0;
                    return_to_main_menu();
                    acted = true;
                } else if (rd_key_pending != 0 &&
                           (TickType_t)(now - rd_key_pending) <= kBootDoubleClickTicks) {
                    rd_key_pending = 0;
                    if (reading_panel_shown()) reading_panel_close();
                    else reading_panel_open(0);
                    ESP_LOGI(TAG, "reading KEY double-click: toggle toc panel");
                    acted = true;
                } else {
                    rd_key_pending = now;
                }
            }
            if (rd_key_pending != 0 &&
                (TickType_t)(now - rd_key_pending) > kBootDoubleClickTicks) {
                rd_key_pending = 0;
                if (reading_panel_shown()) reading_panel_close();
                else reading_back_to_shelf();
                acted = true;
            }

            boot_was = boot_pressed;
            key_was = key_pressed;
            if (acted) notify_ui_task();
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

        // 手柄就绪时本任务必须保持短周期轮询，否则下面两处长眠会把手柄按键
        // 吞掉：1) ulTaskNotifyTake(portMAX_DELAY) 只等物理 GPIO 边沿；
        //      2) 末尾 button_task_poll_delay_ms() 在墨水屏 idle 下几百毫秒一跳。
        // 游戏 ROM 列表页走专用逻辑（上下选 ROM / 左右音量 / A 启动），
        // 其它可交互页面走全局导航（LB+RB 回主页 / 上下移选单 / 左右长短按 / A 确认 / B 返回）。
        const bool pad_ready = pad_state() == PAD_STATE_READY;
        const int  active_page_now = active_work_page_load();
        // 设置页、信息页、诊断页、普通工作页都接受手柄全局导航；
        // 配网门户和低电量模式仍排除（避免误改关键设置）。
        const bool pad_nav_page =
            !setup_portal_active_load() && !battery_low_mode_load() &&
            (settings_page_requested() || info_page_requested() ||
             network_diag_page_requested() ||
             is_work_page_enabled(active_page_now));
        const bool gamepad_needs_poll = pad_ready && (game_idle_page || pad_nav_page);
        if (gamepad_needs_poll) {
            if (game_idle_page) {
                poll_gamepad_in_game_list(now);
            } else {
                // 全局导航返回 acted 表示本帧确实处理了手柄输入。必须在这里
                // notify UI，否则设置页手柄 A 确认只靠 UI 任务自己的 seq 轮询，
                // 墨水屏 idle 下几百毫秒才跳一次，按 A 像没反应；物理 BOOT 确认
                // 有 notify（见下方 boot 分支），两边体验必须一致。B 返回同理。
                if (poll_gamepad_global_nav(now)) notify_ui_task();
            }
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
                    if (active_work_page_load() == kWorkPageWeatherClock) {
                        // 天气时钟只在此页两个子页间来回切，不切到其它工作页。
                        time_calendar_toggle_page();
                        ESP_LOGI(TAG, "weather clock BOOT: subpage=%d",
                                 time_calendar_page());
                    } else {
                        int next_page = next_enabled_work_page(active_work_page_load());
                        active_work_page_store(next_page);
                        ESP_LOGI(TAG, BUTTON_SWITCH_WORK_PAGE_LOG_FORMAT, next_page + 1);
                    }
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
                // 普通工作页 KEY 短按不再进设置：设置只能从主菜单第 8 个模块进入。
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
            if (key_pressed_since != 0 && !key_press_stopped_alert &&
                !key_long_handled && !settings_page_requested() &&
                active_work_page_load() == kWorkPageWeatherClock &&
                !setup_portal_active_load() && !battery_low_mode_load() &&
                button_press_is_short(now - key_pressed_since)) {
                // 天气时钟：KEY 短按回主菜单（BOOT 短按只切两个子页）。
                return_to_main_menu();
                ESP_LOGI(TAG, "weather clock KEY: back to main menu");
                notify_ui_task();
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
