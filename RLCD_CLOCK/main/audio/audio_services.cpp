// 管理共享 Codec、音频外设和提示音播放的音频资源生命周期。
#include "audio_services.h"

#include "app_hardware.h"
#include "app_metadata.h"
#include "app_tick_time.h"
#include "chime_runtime_state.h"
#include "audio_power_lock_ownership.h"
#include "audio_services_internal.h"
#include "atomic_ownership_gate.h"
#include "checked_size.h"
#include "codec_bsp.h"
#include "power_services_internal.h"

#include <atomic>
#include <cstddef>
#include <new>

#include <esp_codec_dev_types.h>
#include <esp_err.h>
#include <esp_log.h>
#include "driver/gpio.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define AUDIO_IDLE_GPIO_CONFIG_FAILED_LOG_FORMAT "audio idle gpio config failed pin=%d err=%s"
#define AUDIO_IDLE_GPIO_LEVEL_FAILED_LOG_FORMAT "audio idle gpio level failed pin=%d err=%s"

namespace {
alignas(CodecPort) unsigned char s_audio_codec_storage[sizeof(CodecPort)] = {};
CodecPort *s_audio_codec = nullptr;
std::atomic<bool> s_audio_codec_present{false};
AtomicOwnershipGate s_audio_playing_gate;
constexpr gpio_num_t kAudioMclkGpio = GPIO_NUM_16;
constexpr gpio_num_t kAudioBclkGpio = GPIO_NUM_9;
constexpr gpio_num_t kAudioWsGpio = GPIO_NUM_45;
constexpr gpio_num_t kAudioDinGpio = GPIO_NUM_10;
constexpr gpio_num_t kAudioDoutGpio = GPIO_NUM_8;
constexpr gpio_num_t kAudioPaGpio = GPIO_NUM_46;
constexpr const char *kAudioCodecBoardName = "S3_RLCD_4_2";

static_assert(kAudioMclkGpio >= GPIO_NUM_0, "audio MCLK GPIO must be valid");
static_assert(kAudioBclkGpio >= GPIO_NUM_0, "audio BCLK GPIO must be valid");
static_assert(kAudioWsGpio >= GPIO_NUM_0, "audio WS GPIO must be valid");
static_assert(kAudioDinGpio >= GPIO_NUM_0, "audio DIN GPIO must be valid");
static_assert(kAudioDoutGpio >= GPIO_NUM_0, "audio DOUT GPIO must be valid");
static_assert(kAudioPaGpio >= GPIO_NUM_0, "audio PA GPIO must be valid");
} // namespace

static AudioPowerLockOwnership s_audio_power_lock;

static void configure_audio_idle_gpio(gpio_num_t pin, gpio_mode_t mode, gpio_pulldown_t pull_down)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = mode;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = pull_down;
    cfg.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, AUDIO_IDLE_GPIO_CONFIG_FAILED_LOG_FORMAT, (int)pin, esp_err_to_name(err));
        return;
    }
    if (mode == GPIO_MODE_OUTPUT) {
        err = gpio_set_level(pin, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, AUDIO_IDLE_GPIO_LEVEL_FAILED_LOG_FORMAT, (int)pin, esp_err_to_name(err));
        }
    }
}

static void configure_audio_idle_output(gpio_num_t pin)
{
    configure_audio_idle_gpio(pin, GPIO_MODE_OUTPUT, GPIO_PULLDOWN_DISABLE);
}

static void configure_audio_idle_input(gpio_num_t pin)
{
    configure_audio_idle_gpio(pin, GPIO_MODE_INPUT, GPIO_PULLDOWN_ENABLE);
}

void park_unused_audio_peripherals()
{
    configure_audio_idle_output(kAudioPaGpio);
    configure_audio_idle_output(kAudioMclkGpio);
    configure_audio_idle_output(kAudioBclkGpio);
    configure_audio_idle_output(kAudioWsGpio);
    configure_audio_idle_output(kAudioDoutGpio);
    configure_audio_idle_input(kAudioDinGpio);
}

bool audio_try_mark_playing()
{
    return s_audio_playing_gate.try_acquire();
}

void audio_clear_playing()
{
    s_audio_playing_gate.release();
}

bool is_audio_playing()
{
    return s_audio_playing_gate.active();
}

bool wait_for_audio_playback_idle(uint32_t timeout_ms,
                                  uint32_t poll_interval_ms,
                                  AudioStopRequestedCallback stop_requested)
{
    if (!is_audio_playing()) {
        return true;
    }
    if (timeout_ms == 0 || poll_interval_ms == 0) {
        return false;
    }
    for (uint32_t waited_ms = 0;
         is_audio_playing() && waited_ms < timeout_ms &&
             (!stop_requested || !stop_requested());) {
        const uint32_t remaining_ms = timeout_ms - waited_ms;
        const uint32_t delay_ms =
            poll_interval_ms < remaining_ms ? poll_interval_ms : remaining_ms;
        const TickType_t delay_ticks =
            app_tick_nonzero_delay(pdMS_TO_TICKS(delay_ms));
        vTaskDelay(delay_ticks);
        waited_ms += delay_ms;
    }
    return !is_audio_playing();
}

bool audio_codec_active()
{
    return s_audio_codec_present.load(std::memory_order_acquire);
}

static CodecPort *ensure_audio_codec(bool microphone_enabled)
{
    if (!s_audio_codec) {
        CodecPort *codec = new (s_audio_codec_storage) CodecPort(app_i2c(),
                                                                 kAudioCodecBoardName,
                                                                 microphone_enabled);
        if (!codec->CodecPort_IsReady()) {
            ESP_LOGW(TAG, "audio codec playback handle unavailable");
            codec->~CodecPort();
        } else {
            s_audio_codec = codec;
            s_audio_codec_present.store(true, std::memory_order_release);
        }
    }
    return s_audio_codec;
}

static void release_audio_codec()
{
    if (s_audio_codec) {
        s_audio_codec_present.store(false, std::memory_order_release);
        s_audio_codec->~CodecPort();
        s_audio_codec = nullptr;
    }
}

void audio_finish_playback()
{
    release_audio_codec();
    park_unused_audio_peripherals();
    s_audio_power_lock.release();
    audio_clear_playing();
}

static CodecPort *prepare_audio_codec(bool microphone_enabled)
{
    if (!s_audio_power_lock.acquire()) {
        ESP_LOGW(TAG, "audio PM lock unavailable");
        return nullptr;
    }
    CodecPort *codec = ensure_audio_codec(microphone_enabled);
    if (!codec) {
        // 不把失败构造后的 GPIO 和三类 PM 锁留给调用方兜底；播放门仍由
        // 发起方通过统一 audio_finish_playback() 按原所有权顺序归还。
        park_unused_audio_peripherals();
        s_audio_power_lock.release();
    }
    return codec;
}

CodecPort *audio_prepare_codec_for_playback()
{
    return prepare_audio_codec(false);
}
