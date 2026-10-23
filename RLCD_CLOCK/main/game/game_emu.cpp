// 管理模拟器上下文与卡带 RAM 的 PSRAM 分配、初始化与释放。
#include "game_emu.h"

#include "game_config.h"
#include "peanut_gb_impl.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <string.h>

namespace {
constexpr const char *kEmuLogTag = "GameBoyEmu";

uint8_t *s_context = nullptr;
size_t s_context_bytes = 0;
uint8_t *s_cart_ram = nullptr;
size_t s_cart_ram_capacity = 0;
const uint8_t *s_rom = nullptr;
size_t s_rom_size = 0;
volatile uint8_t s_cart_ram_dirty = 0;
bool s_running = false;
game_emu_error_t s_last_error = GAME_EMU_OK;

void release_buffers()
{
    if (s_cart_ram) {
        heap_caps_free(s_cart_ram);
        s_cart_ram = nullptr;
    }
    // APU 上下文本体是 peanut_gb_impl.c 的文件作用域静态对象，不随 context
    // 释放，这里不存在悬空风险；调用它只是为了让 audio_read/audio_write
    // 在会话结束后降级（返回 0xFF / 空操作）。
    pgb_context_audio_shutdown();
    if (s_context) {
        heap_caps_free(s_context);
        s_context = nullptr;
    }
    s_context_bytes = 0;
    s_cart_ram_capacity = 0;
    s_rom = nullptr;
    s_rom_size = 0;
    s_cart_ram_dirty = 0;
    s_running = false;
}
} // namespace

bool game_emu_start(const uint8_t *rom, size_t rom_size, const struct tm *rtc_now)
{
    if (s_running) {
        game_emu_stop();
    }
    if (!rom || rom_size < 0x0150U) {
        s_last_error = GAME_EMU_ERR_BAD_ROM;
        return false;
    }

    s_context_bytes = pgb_context_size();
    s_context = static_cast<uint8_t *>(
        heap_caps_malloc(s_context_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_context) {
        s_last_error = GAME_EMU_ERR_NO_MEMORY;
        ESP_LOGE(kEmuLogTag, "context allocation failed bytes=%u",
                 (unsigned)s_context_bytes);
        release_buffers();
        return false;
    }

    s_cart_ram_capacity = pgb_cart_ram_capacity();
    s_cart_ram = static_cast<uint8_t *>(
        heap_caps_malloc(s_cart_ram_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_cart_ram) {
        s_last_error = GAME_EMU_ERR_NO_MEMORY;
        ESP_LOGE(kEmuLogTag, "cart ram allocation failed bytes=%u",
                 (unsigned)s_cart_ram_capacity);
        release_buffers();
        return false;
    }
    memset(s_cart_ram, 0, s_cart_ram_capacity);

    int init_error = 0;
    if (!pgb_context_init(s_context,
                          rom,
                          rom_size,
                          s_cart_ram,
                          s_cart_ram_capacity,
                          &s_cart_ram_dirty,
                          rtc_now,
                          &init_error)) {
        s_last_error = (init_error == 1) ? GAME_EMU_ERR_UNSUPPORTED_CART
                                         : (init_error == 2) ? GAME_EMU_ERR_INVALID_CHECKSUM
                                                              : GAME_EMU_ERR_BAD_ROM;
        ESP_LOGE(kEmuLogTag, "emulator init rejected rom err=%d", init_error);
        release_buffers();
        return false;
    }

    s_rom = rom;
    s_rom_size = rom_size;
    s_cart_ram_dirty = 0;
    s_running = true;
    s_last_error = GAME_EMU_OK;
    return true;
}

void game_emu_stop(void)
{
    release_buffers();
}

bool game_emu_running(void)
{
    return s_running;
}

void game_emu_run_frame(void)
{
    if (!s_running || !s_context) {
        return;
    }
    pgb_context_run_frame(s_context);
}

void game_emu_set_joypad_register(uint8_t joypad_register)
{
    if (!s_running || !s_context) {
        return;
    }
    pgb_context_set_joypad(s_context, joypad_register);
}

bool game_emu_cart_ram_dirty(void)
{
    return s_cart_ram_dirty != 0;
}

void game_emu_cart_ram_clear_dirty(void)
{
    s_cart_ram_dirty = 0;
}

const uint8_t *game_emu_cart_ram(size_t *out_size)
{
    if (!s_running || !s_context) {
        if (out_size) {
            *out_size = 0;
        }
        return nullptr;
    }
    const size_t bytes = pgb_context_cart_ram_bytes(s_context);
    if (out_size) {
        *out_size = bytes;
    }
    return s_cart_ram;
}

uint8_t *game_emu_cart_ram_writable(size_t *out_capacity)
{
    if (!s_running || !s_cart_ram) {
        if (out_capacity) {
            *out_capacity = 0;
        }
        return nullptr;
    }
    if (out_capacity) {
        *out_capacity = s_cart_ram_capacity;
    }
    return s_cart_ram;
}

size_t game_emu_cart_ram_capacity(void)
{
    return pgb_cart_ram_capacity();
}

game_emu_error_t game_emu_last_error(void)
{
    return s_last_error;
}

const char *game_emu_error_text(game_emu_error_t error)
{
    switch (error) {
    case GAME_EMU_OK:
        return "ok";
    case GAME_EMU_ERR_NO_MEMORY:
        return "内存不足";
    case GAME_EMU_ERR_BAD_ROM:
        return "ROM 无效";
    case GAME_EMU_ERR_UNSUPPORTED_CART:
        return "不支持的卡带类型";
    case GAME_EMU_ERR_INVALID_CHECKSUM:
        return "ROM 头部校验失败";
    case GAME_EMU_ERR_NOT_RUNNING:
        return "模拟器未运行";
    default:
        return "未知错误";
    }
}
