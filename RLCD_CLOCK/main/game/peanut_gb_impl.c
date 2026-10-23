// 唯一包含 Peanut-GB 实现的编译单元。
//
// 关键约束（架构师 P1/P3/P6）：
//   1. 全工程只有这里 include "peanut_gb.h"，并且绝不能定义
//      PEANUT_GB_HEADER_ONLY（定义了会出现 multiple definition of gb_run_frame）。
//   2. 单色屏不需要 GBC 双速/HDMA 精确时序，关掉 12 色模式与高精度 LCD，
//      每帧省几毫秒。
//   3. gb_init() 之后必须 gb_set_rtc()，否则 MBC3+RTC（宝可梦金/银）卡在启动画面。
//   4. 音频：ENABLE_SOUND 必须在 include peanut_gb.h 之前定义；
//      minigb_apu.h 依赖的 AUDIO_SAMPLE_RATE 由 game_audio_config.h 统一锁定
//      为 32000（ES8311 系数表没有 32768，用了会完全无声，见该头注释）；
//      audio_read/audio_write 的定义必须出现在 include peanut_gb.h 之前——
//      peanut_gb.h 的函数体直接调用它们，且头文件末尾不提供原型。
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define PEANUT_GB_12_COLOUR 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 0

#include "game_audio_config.h"
#include "minigb_apu.h"

// 签名按 deltabeard/Peanut-GB master 的调用点取（peanut_gb.h:863/1076
// 直接调用）：读是 uint16_t addr；写是 uint16_t addr + uint8_t val。
// 别处分支（如 CrankBoy）签名不同，不可照抄。
// 前置声明必须是非 static 的：头文件末尾不提供原型，声明要先于
// include peanut_gb.h 出现。
uint8_t audio_read(uint16_t addr);
void audio_write(uint16_t addr, uint8_t val);

// APU 上下文本体：文件作用域静态对象，不随模拟器 context 缓冲区分配/释放。
// 312 字节放内部 DRAM，音频任务 60Hz 访问比 PSRAM 更快。
static struct minigb_apu_ctx s_apu_obj;
static struct minigb_apu_ctx *s_apu_ctx = NULL;

// APU 互斥量：audio_write（模拟器任务，每帧几次）与 audio_callback
// （音频任务，每 16.7ms 一次）并发读写同一个 ctx 的 chans[]。上游 SDL
// 示例单线程裸奔，我们不学。callback 亚毫秒级，争用成本接近零。
// 静态分配：无堆分配失败路径，也永不删除——跨多个游戏会话复用同一把锁。
static SemaphoreHandle_t s_apu_mutex = NULL;
static StaticSemaphore_t s_apu_mutex_storage;

// 取锁超时（ms）。绝不用 portMAX_DELAY：game_audio_stop() 有「2s 内没退出就
// vTaskDelete 强杀」的兜底，若被杀的任务正攥着这把锁，锁就永久 held；下次
// 开游戏时模拟器任务的第一次 audio_write() 会永久挂起，整机卡死。带超时后
// 最坏情况退化成「丢掉这一帧的寄存器写入 / 这一帧渲染静音」，不会死锁。
#define kApuMutexTimeoutMs 20

uint8_t audio_read(uint16_t addr)
{
    // audio_read 只读 audio_mem，而 audio_mem 只被 audio_write 写，两者
    // 都在模拟器任务内调用，不参与跨线程竞争，无需持锁。
    if (!s_apu_ctx) {
        return 0xFF;
    }
    return minigb_apu_audio_read(s_apu_ctx, addr);
}

void audio_write(uint16_t addr, uint8_t val)
{
    if (!s_apu_ctx) {
        return;
    }
    // 带超时（理由见 kApuMutexTimeoutMs 定义处）：拿不到锁就放弃本次写入，
    // 丢一次寄存器写的代价远小于把模拟器任务挂死。
    if (s_apu_mutex) {
        if (xSemaphoreTake(s_apu_mutex, pdMS_TO_TICKS(kApuMutexTimeoutMs)) != pdTRUE) {
            return;
        }
    }
    minigb_apu_audio_write(s_apu_ctx, addr, val);
    if (s_apu_mutex) {
        xSemaphoreGive(s_apu_mutex);
    }
}

void pgb_apu_render_frame(int16_t *stream);

#define ENABLE_SOUND 1
#include "peanut_gb.h"

#include "esp_log.h"

#include "gameboy_fb.h"
#include "peanut_gb_impl.h"

#define PGB_LOG_TAG "GameBoyEmu"
#define PGB_INIT_FAILED_FMT "gb_init failed: err=%d rom_bytes=%u"
#define PGB_EMU_ERROR_FMT "emulation error=%d addr=0x%04x"
#define PGB_INVALID_CONTEXT_LOG "invalid emulator context"

// struct gb_s 之后的私有状态；指针全部指向 PSRAM 中的缓冲，
// 回调里只做指针读，绝不 fread / mmap（P2）。
struct pgb_priv {
    const uint8_t *rom;
    uint32_t rom_size;
    uint8_t *cart_ram;
    uint32_t cart_ram_capacity;
    volatile uint8_t *dirty;
};

static size_t pgb_context_aligned_size(void)
{
    return (sizeof(struct gb_s) + 7U) & ~(size_t)7U;
}

// 上下文缓冲只容纳 struct gb_s + priv。APU 上下文本体是文件作用域静态对象，
// 不排在这里，也就不再有「context + offset」这套指针语义。
size_t pgb_context_size(void)
{
    return pgb_context_aligned_size() + sizeof(struct pgb_priv);
}

size_t pgb_cart_ram_capacity(void)
{
    return (size_t)16U * 8192U;
}

// 断开 audio_read/audio_write 包装对 APU 上下文的静态引用。
// APU 对象是静态的，释放 context 缓冲不会造成 use-after-free；这里的作用
// 是让会话结束后的 audio_read/audio_write 安全降级（返回 0xFF / 空操作），
// 与 ENABLE_SOUND=0 的行为保持一致。
void pgb_context_audio_shutdown(void)
{
    s_apu_ctx = NULL;
}

void pgb_apu_render_frame(int16_t *stream)
{
    if (!stream) {
        return;
    }
    if (!s_apu_ctx) {
        // APU 未初始化：必须把缓冲区写静音，不能留未初始化数据——调用方
        // 直接把它当一帧送进 I2S。
        memset(stream, 0, AUDIO_SAMPLES_TOTAL * sizeof(int16_t));
        return;
    }
    // 与 audio_write 互斥：整个 callback 持锁。持锁者是音频任务
    // （prio 8）时模拟器任务（prio 1）最多等一个 callback（亚毫秒）。
    // 带超时（理由见 kApuMutexTimeoutMs 定义处）：拿不到锁就写静音返回，
    // 绝不带着未初始化的缓冲区返回。
    if (s_apu_mutex) {
        if (xSemaphoreTake(s_apu_mutex, pdMS_TO_TICKS(kApuMutexTimeoutMs)) != pdTRUE) {
            memset(stream, 0, AUDIO_SAMPLES_TOTAL * sizeof(int16_t));
            return;
        }
    } else {
        memset(stream, 0, AUDIO_SAMPLES_TOTAL * sizeof(int16_t));
        return;
    }
    minigb_apu_audio_callback(s_apu_ctx, stream);
    xSemaphoreGive(s_apu_mutex);
}

static struct pgb_priv *pgb_priv_of(struct gb_s *gb)
{
    return (struct pgb_priv *)gb->direct.priv;
}

static uint8_t pgb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    const struct pgb_priv *priv = pgb_priv_of(gb);
    if (!priv || !priv->rom || addr >= priv->rom_size) {
        return 0xFF;
    }
    return priv->rom[addr];
}

static uint8_t pgb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    const struct pgb_priv *priv = pgb_priv_of(gb);
    if (!priv || !priv->cart_ram || addr >= priv->cart_ram_capacity) {
        return 0xFF;
    }
    return priv->cart_ram[addr];
}

static void pgb_cart_ram_write(struct gb_s *gb,
                               const uint_fast32_t addr,
                               const uint8_t val)
{
    struct pgb_priv *priv = pgb_priv_of(gb);
    if (!priv || !priv->cart_ram || addr >= priv->cart_ram_capacity) {
        return;
    }
    if (priv->cart_ram[addr] == val) {
        return;
    }
    priv->cart_ram[addr] = val;
    if (priv->dirty) {
        *priv->dirty = 1U;
    }
}

static void pgb_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr)
{
    (void)gb;
    ESP_LOGW(PGB_LOG_TAG, PGB_EMU_ERROR_FMT, (int)err, (unsigned)addr);
}

static void pgb_lcd_draw_line(struct gb_s *gb,
                              const uint8_t *pixels,
                              const uint_fast8_t line)
{
    (void)gb;
    game_fb_push_line((uint32_t)line, pixels);
}

// 修复 ROM 头部校验和（0x014D）。
//
// gb_init() 会把 0x0134..0x014C 按 `x = x - rom[i] - 1` 累加后与 0x014D 比对，
// 不等就直接返回 GB_INIT_INVALID_CHECKSUM，整个 ROM 启动不了。
// 但现存的汉化版、ROM hack、被工具 trim/merge/patch 过的 dump，头部校验和
// 大面积没重算——内容本身是好的。别的模拟器大多不看这个字节，用户就会觉得
// "为什么电脑上能跑、你这设备说校验失败"。
//
// 这里只改 PSRAM 里的副本（game_emu 用 heap_caps_malloc 分配的可写缓冲），
// 不动 flash 上的原文件，所以是纯粹的本地容错。
static bool pgb_repair_header_checksum(uint8_t *rom, size_t rom_size)
{
    if (!rom || rom_size <= ROM_HEADER_CHECKSUM_LOC) {
        return false;
    }
    uint8_t sum = 0;
    for (size_t i = 0x0134U; i <= 0x014CU; ++i) {
        sum = (uint8_t)(sum - rom[i] - 1U);
    }
    const uint8_t stored = rom[ROM_HEADER_CHECKSUM_LOC];
    if (stored == sum) {
        return false;
    }
    ESP_LOGW(PGB_LOG_TAG,
             "rom header checksum fixed: 0x%02x -> 0x%02x",
             (unsigned)stored,
             (unsigned)sum);
    rom[ROM_HEADER_CHECKSUM_LOC] = sum;
    return true;
}

bool pgb_context_init(void *context,
                      const uint8_t *rom,
                      size_t rom_size,
                      uint8_t *cart_ram,
                      size_t cart_ram_capacity,
                      volatile uint8_t *dirty_flag,
                      const struct tm *rtc_now,
                      int *out_init_error)
{
    if (out_init_error) {
        *out_init_error = 0;
    }
    if (!context || !rom || !cart_ram || rom_size < 0x0150U) {
        return false;
    }

    // P6：每次初始化都把上下文整体清零，避免上一份 ROM 的残留状态。
    memset(context, 0, pgb_context_size());

    // APU：静态对象清零后按 DMG 上电默认值初始化（含 wave RAM 默认波形），
    // 再把静态指针指向它供 audio_read/audio_write 包装使用。之后即使音频
    // 会话没起来（例如提示音正占着 codec），寄存器读写语义也保持正确，
    // 只是没有声音输出。
    memset(&s_apu_obj, 0, sizeof(s_apu_obj));
    s_apu_ctx = &s_apu_obj;
    minigb_apu_audio_init(s_apu_ctx);
    // APU 互斥量惰性创建（静态存储，跨会话复用，永不删除）。
    if (!s_apu_mutex) {
        s_apu_mutex = xSemaphoreCreateMutexStatic(&s_apu_mutex_storage);
    }

    struct gb_s *gb = (struct gb_s *)context;
    struct pgb_priv *priv =
        (struct pgb_priv *)((uint8_t *)context + pgb_context_aligned_size());
    memset(priv, 0, sizeof(*priv));
    priv->rom = rom;
    priv->rom_size = (uint32_t)rom_size;
    priv->cart_ram = cart_ram;
    priv->cart_ram_capacity = (uint32_t)cart_ram_capacity;
    priv->dirty = dirty_flag;

    // 必须在 gb_init() 之前修校验和，否则 gb_init() 会直接拒收。
    (void)pgb_repair_header_checksum((uint8_t *)(uintptr_t)rom, rom_size);

    const enum gb_init_error_e err = gb_init(gb,
                                             pgb_rom_read,
                                             pgb_cart_ram_read,
                                             pgb_cart_ram_write,
                                             pgb_error,
                                             priv);
    if (out_init_error) {
        *out_init_error = (int)err;
    }
    if (err != GB_INIT_NO_ERROR) {
        ESP_LOGE(PGB_LOG_TAG, PGB_INIT_FAILED_FMT, (int)err, (unsigned)rom_size);
        return false;
    }

    gb_init_lcd(gb, pgb_lcd_draw_line);
    // frame_skip 必须关掉：它只省 __gb_draw_line() 里的像素生成，
    // gb_run_frame() 的 CPU 成本基本不变（CPU 指令一条不少），省不了几毫秒；
    // 但它的「整帧跳过」会和 gameboy_runtime 的 present_stride 形成两套独立跳帧：
    // present_stride=2 时若 game_fb_present() 恰好落在被 frame_skip 丢掉的那一帧，
    // 帧缓冲没有更新 → 这一帧白推，屏幕停在上上帧，最坏 60/2/8 ≈ 3.75fps 且抖动。
    // 跳帧只在推屏那一侧做（gameboy_runtime 的 present_stride），这里保持全帧绘制。
    gb->direct.frame_skip = false;
    gb->direct.interlace = false;

    if (rtc_now) {
        gb_set_rtc(gb, rtc_now);
    }
    return true;
}

void pgb_context_run_frame(void *context)
{
    struct gb_s *gb = (struct gb_s *)context;
    if (!gb) {
        ESP_LOGE(PGB_LOG_TAG, "%s", PGB_INVALID_CONTEXT_LOG);
        return;
    }
    game_fb_frame_begin();
    gb_run_frame(gb);
}

void pgb_context_set_joypad(void *context, uint8_t joypad_register)
{
    struct gb_s *gb = (struct gb_s *)context;
    if (!gb) {
        return;
    }
    gb->direct.joypad = joypad_register;
}

size_t pgb_context_cart_ram_bytes(const void *context)
{
    if (!context) {
        return 0;
    }
    const struct gb_s *gb = (const struct gb_s *)context;
    const struct pgb_priv *priv = (const struct pgb_priv *)gb->direct.priv;
    if (!priv) {
        return 0;
    }
    // MBC2 只有 512 个半字节，num_ram_banks 对它无意义。
    size_t bytes = (size_t)gb->num_ram_banks * 8192U;
    if (gb->mbc == 2 && bytes == 0) {
        bytes = 512U;
    }
    if (bytes > priv->cart_ram_capacity) {
        bytes = priv->cart_ram_capacity;
    }
    return bytes;
}
