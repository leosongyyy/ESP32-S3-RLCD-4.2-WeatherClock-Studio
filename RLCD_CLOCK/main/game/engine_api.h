// engine_api.h — 引擎层唯一对外 API 面（facade）
//
// 分层约定（由 tools/check_deps.py 在构建期强制校验）
//
//   上层（ui/ input/ network/ core/main.cpp）
//       —— 只允许 include "engine_api.h"，不得直连 game/ 下的任何子头。
//   引擎（game/）
//       —— 跨出 game/ 的「上层动作」一律走本文件提供的回调注入，
//          禁止反向 include ui/、input/ 的头（会造成层次倒置与循环依赖）。
//
// 为什么 ESP-IDF 的 REQUIRES 挡不住：所有组件 public 头默认全局可见，
// 写错方向照样编得过。必须靠静态扫描兜底，见 tools/check_deps.py。
//
// 本头聚合的是「上层确实需要的引擎 API」，不含 fb/chrome/emu 这些引擎内部件——
// 它们只被 game/ 自己使用，收进门面只会扩大上层可见面。

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "game_config.h"             // 几何 / 存储 / 调度常量（UI 排版要用）
#include "game_audio.h"              // 音量档位（ROM 列表页音量行）
#include "gameboy_input.h"           // 本机/手柄按键合并入口
#include "gameboy_pad.h"             // BLE 手柄状态与按键
#include "gameboy_pad_bind.h"        // 绑定表（绑定页）
#include "gameboy_rom_store.h"       // ROM 枚举 / 选择 / .sav
#include "gameboy_runtime.h"         // 模拟器任务启停 / 显示所有者

#ifdef __cplusplus
extern "C" {
#endif

// ---- 上层动作注入 ----
//
// LVGL flush 入口累计次数。引擎抢显示所有者时要确认「LVGL 上一次 SPI 传输
// 已经返回」才能推首帧，否则会和 flush_callback 交错写同一块面板缓冲，
// 出现半屏旧画面。这是引擎对 UI 层唯一的需求，用回调注入代替 #include。
//
// 由 core/main.cpp 在 app_main 里注册；未注册时返回 0（握手超时后会打 WARN，
// 行为退化成原来的「LVGL 空闲就直接推」，不会崩，但首帧有交错风险）。
typedef uint32_t (*engine_display_barrier_cb_t)(void);
void     engine_api_set_display_barrier_cb(engine_display_barrier_cb_t cb);
uint32_t engine_api_display_barrier_count(void);

#ifdef __cplusplus
}
#endif
