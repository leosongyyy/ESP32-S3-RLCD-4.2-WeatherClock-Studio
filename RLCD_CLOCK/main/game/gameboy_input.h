// 声明 GB 按键位图：本机 BOOT/KEY 与（Phase 3 的）蓝牙手柄统一入口。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gameboy_pad.h"

#ifdef __cplusplus
extern "C" {
#endif

// 按下/松开一个 GB 按键（本机按键走这里；手柄在 Phase 3 也走这里）。
void gameboy_pad_set(gb_button_t button, bool pressed);

// 手柄来源的按键状态。与本机按键分别记在两张位图里再取并集，
// 这样手柄松开不会误清掉正在按住的本机按键。
void gameboy_pad_set_remote(gb_button_t button, bool pressed);
void gameboy_pad_clear_remote(void);

// 手柄是否已连接。Phase 2 恒为 false（BLE 在 Phase 3 接入）。
bool gameboy_pad_connected(void);

// 释放全部按键，退出游戏或切换 ROM 时调用，避免按键卡在按下状态。
void gameboy_pad_release_all(void);

// 当前按下的按键位图（1 = 按下），仅用于诊断与 UI 显示。
uint8_t gameboy_pad_pressed_mask(void);

// Peanut-GB 需要的 gb->direct.joypad 寄存器值。
// 注意：Peanut-GB 的 JOYP 寄存器是低电平有效——gb_reset() 把它置为 0xFF，
// 读取时按位 OR 进 JOYP，所以「0 = 按下」。本函数返回的就是该取反后的值。
uint8_t gameboy_pad_joypad_register(void);

#ifdef __cplusplus
}
#endif
