// 声明模拟器上下文的生命周期与逐帧驱动接口（不依赖 Peanut-GB 的类型）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GAME_EMU_OK = 0,
    GAME_EMU_ERR_NO_MEMORY,
    GAME_EMU_ERR_BAD_ROM,
    GAME_EMU_ERR_UNSUPPORTED_CART,
    GAME_EMU_ERR_INVALID_CHECKSUM,
    GAME_EMU_ERR_NOT_RUNNING,
} game_emu_error_t;

// 载入 ROM 并初始化模拟器（ROM 必须已经整份驻留在 PSRAM）。
// rtc_now 用于 MBC3+RTC 卡带（宝可梦金/银），可为 NULL。
bool game_emu_start(const uint8_t *rom, size_t rom_size, const struct tm *rtc_now);

// 释放上下文与卡带 RAM（不负责 ROM 缓冲，ROM 归 rom_store）。
void game_emu_stop(void);

bool game_emu_running(void);

// 跑一帧，画面经 lcd_draw_line 回调直接写进面板帧缓冲。
void game_emu_run_frame(void);

// 写入 Peanut-GB 的 gb->direct.joypad（低电平有效）。
void game_emu_set_joypad_register(uint8_t joypad_register);

// 卡带 RAM 是否被改写（退出/暂停时据此写 .sav）。
bool game_emu_cart_ram_dirty(void);
void game_emu_cart_ram_clear_dirty(void);

// 卡带 RAM 缓冲指针与当前实际使用的字节数。
const uint8_t *game_emu_cart_ram(size_t *out_size);
// 可写视图：仅供在启动前把 .sav 内容灌回卡带 RAM（不触发脏标记）。
uint8_t *game_emu_cart_ram_writable(size_t *out_capacity);
size_t game_emu_cart_ram_capacity(void);

game_emu_error_t game_emu_last_error(void);
const char *game_emu_error_text(game_emu_error_t error);

#ifdef __cplusplus
}
#endif
