// Peanut-GB 编译单元的内部桥接接口（只被 game_emu.cpp 使用）。
//
// 全工程只允许 peanut_gb_impl.c 一个 TU include "peanut_gb.h"，
// 其它文件一律通过这里的 void* 接口访问，避免重复定义 gb_run_frame 等符号。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// struct gb_s 与内部 priv 结构的合计大小（含对齐）。
size_t pgb_context_size(void);

// 卡带 RAM 缓冲容量（16 个 8KB bank）。
size_t pgb_cart_ram_capacity(void);

// 初始化模拟器上下文。context 必须是 pgb_context_size() 字节的可写缓冲。
// rtc_now 为 NULL 时跳过 gb_set_rtc()（MBC3+RTC 卡带会卡在启动画面）。
// out_init_error 返回 Peanut-GB 的原始 gb_init_error_e 值。
bool pgb_context_init(void *context,
                      const uint8_t *rom,
                      size_t rom_size,
                      uint8_t *cart_ram,
                      size_t cart_ram_capacity,
                      volatile uint8_t *dirty_flag,
                      const struct tm *rtc_now,
                      int *out_init_error);

// 跑一帧（vblank 为止）。
void pgb_context_run_frame(void *context);

// 写入 gb->direct.joypad（低电平有效，见 gameboy_input.h）。
void pgb_context_set_joypad(void *context, uint8_t joypad_register);

// 当前卡带实际使用的存档字节数（num_ram_banks * 8KB，已按容量裁剪）。
size_t pgb_context_cart_ram_bytes(const void *context);

// 断开 audio_read/audio_write 包装对 APU 上下文的静态引用。
// APU 上下文本体是文件作用域静态对象，不随 context 缓冲释放，因此这里
// 不是防悬空；它的作用是让会话结束后的 audio_read/audio_write 安全降级
// （返回 0xFF / 空操作），与 ENABLE_SOUND=0 的行为保持一致。
void pgb_context_audio_shutdown(void);

// 渲染一帧 APU 音频：产出 AUDIO_SAMPLES_TOTAL 个 int16 的 interleaved
// stereo（小端）写入 stream。stream 由调用方保证容量 AUDIO_SAMPLES_TOTAL
// 个 int16；APU 未初始化时写静音。与 audio_write 互斥（同一把文件级
// mutex，带超时），可安全地从音频任务调用。
void pgb_apu_render_frame(int16_t *stream);

#ifdef __cplusplus
}
#endif
