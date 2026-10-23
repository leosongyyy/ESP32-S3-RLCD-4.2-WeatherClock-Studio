// GB 游戏音频会话：APU 渲染 → interleaved stereo 直写 I2S（ES8311，32000Hz）。
//
// 架构决策记录（team-lead 阶段 1 规格修正，逐条对应）：
//
// 1. 拉取（pull）模型：音频任务自己渲染、自己写 codec，不跟随模拟器
//    帧率。推屏（RLCD_Sendbuffera，SPI 5MHz ≈ 24ms）阻塞的是 core 1 上
//    的模拟器任务；音频任务 pin 在 core 0 且优先级 8（提示音 4 / 模拟器
//    1 之上），推屏期间 APU 寄存器状态被游戏冻结、输出延续，不出静音洞，
//    音调也与跳帧完全解耦。
//
// 2. 节拍：不设定时器、不设账本。i2s_channel_write（esp_codec_dev_write，
//    timeout 1000ms）阻塞到 DMA 描述符有空间才返回，「渲染一帧 → 写一帧」
//    的循环天然被 I2S 消费速率钳制在 32000 样本/s，长期零漂移。
//
// 3. 环形缓冲（8 帧 × 2140B ≈ 17KB，静态 PSRAM）：作用是渲染侧批量——
//    任务一次把 ring 灌满再连续写出，抖动回来时本地有存货可立即搬入
//    I2S。注意它**不能**把播出容忍度从 I2S DMA 深度（3 描述符 × 64 帧
//    ≈ 6ms @32k stereo）提高：ring 里的数据不经 i2s_channel_write 就
//    不会被播出，播出存货上限由 codec_init.c 的 DMA 配置决定。
//
// 4. 立体声直写：minigb_apu 回调产出 AUDIO_SAMPLES_TOTAL 个 interleaved
//    stereo int16（samples[i]=L, samples[i+1]=R），CodecPort_SetInfo 按
//    channel=2 打开（_i2s_data_set_fmt 对 channel==2 补全双 slot），
//    GB 的 L/R panning 完整保留，零转换零混音。
//
// 5. APU 数据竞争：audio_write（模拟器任务）与渲染回调（音频任务）经
//    peanut_gb_impl.c 内的文件级 mutex 互斥；audio_read 只读 audio_mem
//    （仅模拟器任务写），无需持锁。APU 上下文本体是 peanut_gb_impl.c 的
//    文件作用域静态对象（内部 DRAM），不存在任何 APU 指针跨模块传递，
//    pgb_apu_render_frame() 只收输出缓冲区。mutex 取锁一律带超时：见
//    决策记录 8。
//
// 6. 互斥：复用 audio_services 现成的播放门（audio_try_mark_playing），
//    游戏会话期间持有它，所有提示音入口自然失败——即用户已接受的
//    「游戏期间提示音静音」。不新建第二套门。
//
// 7. 生命周期走 audio_services 既有通道：audio_prepare_codec_for_playback()
//    （acquire PM 锁 + 惰性构造 CodecPort）起，audio_finish_playback()
//    （CloseSpeaker → GPIO46 拉低 → park 六只脚 → 释放 PM 锁 → 清门）止。
//    GPIO46 是 ES8311 的 PA 使能脚，由 codec 驱动自己管（es8311.c 的
//    es8311_pa_power），不手动 init，codec 开着也绝不调
//    park_unused_audio_peripherals()。
//
// 8. APU mutex 必须带超时取锁：本文件的停机有「2s 内没退出就 vTaskDelete
//    强杀音频任务」的兜底，若被杀的任务正攥着 APU mutex，那把锁就永久
//    held，下次开游戏时模拟器任务的第一次 audio_write() 会永久挂起 →
//    整机卡死。带超时后最坏退化成丢掉一帧声音，不会死锁。
#include "game_audio.h"

#include "audio_services_internal.h"
#include "codec_bsp.h"
#include "game_audio_config.h"
#include "game_config.h"
#include "game_emu.h"
#include "peanut_gb_impl.h"

// minigb_apu.h 是 vendored 第三方源码，没有 extern "C" 守卫（不改动它），
// 从 C++ 包含必须包一层，否则函数签名被 C++ name mangling，链接不到
// C 编译出来的 minigb_apu.c。
extern "C" {
#include "minigb_apu.h"
}

#include <atomic>

#include <esp_attr.h>
#include <esp_codec_dev_types.h>
#include <esp_log.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define GA_LOG_TAG "GameBoyAudio"

// codec_bsp.cpp 里 kCodecNameEs8311 是文件级私有常量，这里用字面量。
#define GA_CODEC_NAME_ES8311 "es8311"

namespace {
constexpr int kSampleRate = AUDIO_SAMPLE_RATE;    // 32000（ES8311 系数表内值）
constexpr int kChannels = AUDIO_CHANNELS;         // 2：APU interleaved stereo 直写
constexpr int kBitsPerSample = 16;
// 音量档位：0 = 静音，1..5 每档 20%。默认 4 档 = 80%，与原先硬编码一致。
constexpr int kVolumeStepCount = 5;
constexpr int kPercentPerStep = 100 / kVolumeStepCount;   // 20
constexpr int kDefaultVolumeStep = 4;                      // 80%
constexpr uint32_t kTaskStackBytes = 6144;        // APU 回调 + I2S 写入路径
// 优先级刻度：network=4 / ui=3 / button=2 / 模拟器=1，提示音播放任务=4
// （audio_chime.cpp）。音频任务取 8：高于一切既有任务，又不到会惹麻烦的高度。
constexpr int kTaskPriority = 8;
// 模拟器任务在 core 1（game_config.h kTaskCoreId）。音频任务 pin 到
// core 0：推屏阻塞只发生在 core 1 的模拟器任务上，异核放置把它隔离掉。
constexpr int kTaskCore = 0;
// ring 容量（帧）。8 × 2140B ≈ 17KB 静态 PSRAM。见文件头决策记录 3：
// 它提供渲染侧批量与本地存货，不改变 I2S DMA 的 6ms 播出存货上限。
constexpr uint32_t kRingFrames = 8;
constexpr uint32_t kFrameBytes = AUDIO_SAMPLES_TOTAL * sizeof(int16_t);
constexpr uint32_t kStopWaitStepMs = 10;
// 停机最多等 2s（与 gameboy_runtime 等自己任务的节奏一致）。
// i2s_channel_write 的超时是 1000ms（audio_codec_data_i2s.c 的
// DEFAULT_WAIT_TIMEOUT），病态情况下一次写最多堵 1s，2s 预算留足了一次
// 超时 + 收尾。
constexpr uint32_t kStopWaitSteps = 200;
// 开门预热：先写静音再调音量，防 PA 使能瞬间的噗声。
// 提示音路径用 90ms（codec_bsp.cpp play_pcm_to_slot0），游戏会话取 30ms。
constexpr uint32_t kWarmupFrames = 2;             // 2 × 16.7ms ≈ 33ms
// 收尾静音：排空 DMA 残余，防 PA 关闭时把半截旧声音截断出噗声。
// 提示音路径的 tail silence 是 40ms，这里取 3 帧 ≈ 50ms。
constexpr uint32_t kTailFrames = 3;
constexpr const char *kTaskName = "gb_audio";

std::atomic<bool> s_active{false};
std::atomic<bool> s_stop_requested{false};
std::atomic<bool> s_paused{false};
std::atomic<TaskHandle_t> s_task{nullptr};
CodecPort *s_codec = nullptr;

// 音量档位（0..5）。初值取默认档，真正的 NVS 恢复推迟到第一次有人问音量
// （ensure_volume_loaded），避免在启动路径上抢 flash。
std::atomic<int> s_volume_step{kDefaultVolumeStep};
std::atomic<bool> s_volume_loaded{false};

// 每帧静音模板（内部 RAM）：APU 回调产出 interleaved stereo
// AUDIO_SAMPLES_TOTAL 个 int16 直送 I2S，这里只写不读内容（恒零）。
int16_t s_silence[AUDIO_SAMPLES_TOTAL] = {};

// SPSC 帧环形缓冲：生产者与消费者都是音频任务自己（见文件头决策 3），
// 单线程访问，无需原子/锁。静态 PSRAM，无分配失败路径。
EXT_RAM_BSS_ATTR int16_t s_ring_storage[kRingFrames * AUDIO_SAMPLES_TOTAL];
uint32_t s_ring_head = 0;   // 下一帧待写出的位置（帧索引）
uint32_t s_ring_count = 0;  // 存货帧数

bool ring_full()
{
    return s_ring_count == kRingFrames;
}

bool ring_empty()
{
    return s_ring_count == 0;
}

int16_t *ring_slot(uint32_t index)
{
    return &s_ring_storage[(index % kRingFrames) * AUDIO_SAMPLES_TOTAL];
}

const int16_t *ring_peek_frame()
{
    return ring_slot(s_ring_head);
}

void ring_pop_frame()
{
    s_ring_head = (s_ring_head + 1U) % kRingFrames;
    --s_ring_count;
}

void ring_reset()
{
    s_ring_head = 0;
    s_ring_count = 0;
}

bool s_write_fault_logged = false;

int step_to_percent(int step)
{
    return step <= 0 ? 0 : step * kPercentPerStep;
}

// NVS 恢复只做一次，且是幂等读：就算两个任务同时进来，读出来的值一样，
// 用 atomic 做闸门足够（不需要 mutex）。
void ensure_volume_loaded()
{
    if (s_volume_loaded.load(std::memory_order_acquire)) {
        return;
    }
    int step = kDefaultVolumeStep;
    nvs_handle_t handle = 0;
    if (nvs_open(game_config::kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        int32_t stored = 0;
        if (nvs_get_i32(handle, game_config::kNvsVolumeStepKey, &stored) == ESP_OK &&
            stored >= 0 && stored <= kVolumeStepCount) {
            step = (int)stored;
        }
        nvs_close(handle);
    }
    s_volume_step.store(step, std::memory_order_release);
    s_volume_loaded.store(true, std::memory_order_release);
}

// 会话进行中就地改音量。会话没开（s_active == false）时 codec 是关着的，
// 绝对不能碰 —— 调 SetSpeakerVol 会打到已关闭的设备上。
void apply_volume_now(int step)
{
    if (!s_active.load(std::memory_order_acquire) || s_codec == nullptr) {
        return;
    }
    s_codec->CodecPort_SetSpeakerVol(step_to_percent(step));
}

bool persist_volume_step(int step)
{
    nvs_handle_t handle = 0;
    if (nvs_open(game_config::kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(GA_LOG_TAG, "volume persist failed: nvs open");
        return false;
    }
    const esp_err_t err = nvs_set_i32(handle, game_config::kNvsVolumeStepKey, (int32_t)step);
    const esp_err_t commit_err = (err == ESP_OK) ? nvs_commit(handle) : err;
    nvs_close(handle);
    if (commit_err != ESP_OK) {
        ESP_LOGW(GA_LOG_TAG, "volume persist failed: %d", (int)commit_err);
        return false;
    }
    return true;
}

// 写失败只记第一次：否则 60Hz 的告警日志会把串口刷爆。
int write_frame(const int16_t *frame)
{
    const int ret = s_codec->CodecPort_PlayWrite((void *)frame,
                                                 (int)kFrameBytes);
    if (ret == ESP_CODEC_DEV_OK) {
        s_write_fault_logged = false;
        return ret;
    }
    if (!s_write_fault_logged) {
        s_write_fault_logged = true;
        ESP_LOGW(GA_LOG_TAG, "i2s write failed: %d", ret);
    }
    return ret;
}

void audio_game_task(void *)
{
    while (!s_stop_requested.load(std::memory_order_acquire)) {
        if (s_paused.load(std::memory_order_acquire)) {
            // 暂停：不渲染新帧，先把存货写完，随后只灌静音保住 DMA
            // （功放保持使能，恢复时从头放音，没有开关噗声）。
            if (!ring_empty()) {
                write_frame(ring_peek_frame());
                ring_pop_frame();
                continue;
            }
            write_frame(s_silence);   // 阻塞约 16.7ms，天然限速
            continue;
        }

        // 批量渲染：一次把 ring 灌满（每次回调亚毫秒级，8 帧约几毫秒
        // CPU），抖动回来时本地立即有存货可搬入 I2S。直接渲染进 ring
        // 尾槽，省一层拷贝。
        while (!ring_full() && !s_stop_requested.load(std::memory_order_acquire) &&
               !s_paused.load(std::memory_order_acquire)) {
            pgb_apu_render_frame(ring_slot(s_ring_head + s_ring_count));
            ++s_ring_count;
        }

        // 连续写出：存货搬进 I2S，阻塞到 DMA 有空间，平均速率被钳制在
        // 32000 样本/s，与模拟器帧率、跳帧比例完全无关。
        while (!ring_empty() && !s_stop_requested.load(std::memory_order_acquire)) {
            write_frame(ring_peek_frame());
            ring_pop_frame();
        }

        if (ring_empty() && !s_paused.load(std::memory_order_acquire)) {
            // ring 刚被写空且未暂停：下一轮立即重新灌满，无需让出 CPU。
            continue;
        }
        // 只有暂停/停机路径会走到这里，让出 CPU 等状态变化。
        vTaskDelay(1);
    }

    s_task.store(nullptr, std::memory_order_release);
    vTaskDelete(nullptr);
}

static_assert(AUDIO_SAMPLE_RATE == 32000,
              "GB audio rate must be 32000: ES8311 coeff table has no 32768");
static_assert(AUDIO_SAMPLES == 535,
              "frame buffers assume 535 samples per channel at 32000Hz");
static_assert(AUDIO_SAMPLES_TOTAL == 1070,
              "frame buffers must hold one full APU callback (stereo)");
static_assert(AUDIO_CHANNELS == 2,
              "APU output is interleaved stereo; codec must be opened with 2 channels");
static_assert(kChannels == AUDIO_CHANNELS,
              "codec channel count must match the APU output layout");
static_assert(kTaskPriority > game_config::kTaskPriority,
              "audio task must outrank the emulator task (priority 1)");
static_assert(kTaskPriority > 4,
              "audio task must outrank the chime playback task (priority 4)");
static_assert(kTaskCore != game_config::kTaskCoreId,
              "audio task must not share the core with the emulator task");
static_assert(kTaskCore >= 0 && kTaskCore < portNUM_PROCESSORS,
              "audio task core must exist on the target");
static_assert(kRingFrames > 0 && kStopWaitSteps > 0,
              "ring capacity and stop bounds must be positive");
static_assert(kWarmupFrames > 0 && kTailFrames > 0,
              "warmup and tail silence must be positive");
static_assert(100 % kVolumeStepCount == 0,
              "step count must divide 100 evenly, otherwise top step is not 100%");
static_assert(kPercentPerStep * kVolumeStepCount == 100,
              "top volume step must map to exactly 100%");
static_assert(kDefaultVolumeStep >= 0 && kDefaultVolumeStep <= kVolumeStepCount,
              "default volume step must be within [0, step count]");
} // namespace

bool game_audio_start(void)
{
    if (s_active.load(std::memory_order_acquire)) {
        return true;
    }
    // 与提示音（整点/番茄钟完成/闹铃/设置试听）互斥：门被占说明正在放
    // 提示音，本次游戏静音运行；反之游戏期间提示音也会被门挡掉。
    // （team-lead 已确认接受该副作用。）
    if (!audio_try_mark_playing()) {
        ESP_LOGW(GA_LOG_TAG, "game audio skipped: another playback holds the gate");
        return false;
    }
    if (!game_emu_running()) {
        // 模拟器没在跑就没有 APU 状态可渲染，直接归还互斥门（否则提示音
        // 会被永久挡掉）。
        audio_clear_playing();
        ESP_LOGE(GA_LOG_TAG, "game audio unavailable: emulator not running");
        return false;
    }
    // 既有播放通道：acquire PM 锁 + 惰性构造 CodecPort（内部 RAM）。
    CodecPort *codec = audio_prepare_codec_for_playback();
    if (!codec) {
        // prepare 失败时内部已 park GPIO 并归还 PM 锁，但互斥门还在我们
        // 手里，必须在这里显式归还。
        audio_clear_playing();
        return false;
    }
    // 32000 而不是 GB 原生的 32768：ES8311 系数表按 rate×256 精确查表，
    // 没有 32768，用了会 open 失败 → 完全无声。channel=2：APU 的
    // interleaved stereo 零转换直写，GB 的 L/R panning 完整保留。
    if (!codec->CodecPort_SetInfo(GA_CODEC_NAME_ES8311,
                                  1,
                                  kSampleRate,
                                  kChannels,
                                  kBitsPerSample)) {
        ESP_LOGE(GA_LOG_TAG,
                 "codec open failed: rate=%d channels=%d bits=%d",
                 kSampleRate,
                 kChannels,
                 kBitsPerSample);
        audio_finish_playback();
        return false;
    }
    s_codec = codec;

    // 预热：先静音写 ~30ms 再调音量（chime 路径同款 90ms 的理由——
    // 新建的 DMA 描述符内容不定，不预热可能把上电 garbage 当声音放出来）。
    codec->CodecPort_SetSpeakerVol(0);
    for (uint32_t i = 0; i < kWarmupFrames; ++i) {
        (void)write_frame(s_silence);
    }
    // 取一次就用到底：日志里的 vol= 与实际下发的必须是同一个值。
    const int start_volume = game_audio_volume_percent();
    codec->CodecPort_SetSpeakerVol(start_volume);

    ring_reset();
    s_stop_requested.store(false, std::memory_order_release);
    s_paused.store(false, std::memory_order_release);

    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(audio_game_task,
                                kTaskName,
                                kTaskStackBytes,
                                nullptr,
                                (UBaseType_t)kTaskPriority,
                                &task,
                                (BaseType_t)kTaskCore) != pdPASS) {
        s_codec = nullptr;
        // 收掉预热写的 DMA 残余，再走统一的 finish（内含 CloseSpeaker）。
        for (uint32_t i = 0; i < kTailFrames; ++i) {
            (void)write_frame(s_silence);
        }
        audio_finish_playback();
        ESP_LOGE(GA_LOG_TAG, "audio task create failed");
        return false;
    }
    s_task.store(task, std::memory_order_release);
    s_active.store(true, std::memory_order_release);
    ESP_LOGI(GA_LOG_TAG,
             "game audio started: rate=%d channels=%d bits=%d vol=%d%% "
             "ring=%uframes task=%s/prio%d/core%d",
             kSampleRate,
             kChannels,
             kBitsPerSample,
             start_volume,
             (unsigned)kRingFrames,
             kTaskName,
             kTaskPriority,
             kTaskCore);
    return true;
}

void game_audio_stop(void)
{
    if (!s_active.load(std::memory_order_acquire)) {
        return;
    }
    s_stop_requested.store(true, std::memory_order_release);

    // 任务最长阻塞在一个 I2S 写周期（i2s_channel_write timeout 1000ms）
    //或一次 vTaskDelay(1)，正常几十毫秒内必退出；等 2s 是病态余量。
    for (uint32_t step = 0; step < kStopWaitSteps; ++step) {
        if (s_task.load(std::memory_order_acquire) == nullptr) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(kStopWaitStepMs));
    }
    TaskHandle_t stale = s_task.exchange(nullptr, std::memory_order_acq_rel);
    if (stale != nullptr) {
        // 与 gameboy_runtime 停机同款兜底：只可能是 I2S 写超时未返回，
        // 留 ERROR 日志作为"这里还得继续查"的信号。
        ESP_LOGE(GA_LOG_TAG, "audio task did not exit in time, killing");
        vTaskDelete(stale);
    }

    // 顺序不能反：先确认音频任务已死，再关 codec / 还互斥门。反过来的话
    // 音频任务会在已销毁的 CodecPort 上写 I2S（use-after-free）。
    // （APU 上下文本体是 peanut_gb_impl.c 里的文件作用域静态对象，不随
    // 模拟器 context 缓冲释放，所以这里不存在 APU 侧的 use-after-free；
    // 真正的约束只来自 CodecPort。）
    // 收尾静音：把 DMA 里残余的旧声音排空，PA 关闭时不会截断出噗声。
    for (uint32_t i = 0; i < kTailFrames; ++i) {
        (void)write_frame(s_silence);
    }
    // 内含 CloseSpeaker（功放 GPIO46 随之拉低）+ park 六只脚省电
    // + 释放 PM 锁 + 清互斥门。
    audio_finish_playback();
    ring_reset();
    s_codec = nullptr;
    s_paused.store(false, std::memory_order_release);
    s_stop_requested.store(false, std::memory_order_release);
    s_active.store(false, std::memory_order_release);
    ESP_LOGI(GA_LOG_TAG, "game audio stopped");
}

void game_audio_set_paused(bool paused)
{
    s_paused.store(paused, std::memory_order_release);
}

int game_audio_volume_step(void)
{
    ensure_volume_loaded();
    return s_volume_step.load(std::memory_order_acquire);
}

int game_audio_volume_percent(void)
{
    return step_to_percent(game_audio_volume_step());
}

bool game_audio_volume_set_step(int step)
{
    if (step < 0 || step > kVolumeStepCount) {
        return false;
    }
    ensure_volume_loaded();
    const int before = s_volume_step.exchange(step, std::memory_order_acq_rel);
    if (before == step) {
        return true;   // 值没变：不落盘、不打日志，避免重复写 flash
    }
    (void)persist_volume_step(step);
    apply_volume_now(step);
    ESP_LOGI(GA_LOG_TAG, "volume step %d -> %d (%d%%)", before, step, step_to_percent(step));
    return true;
}

bool game_audio_volume_step_up(void)
{
    const int cur = game_audio_volume_step();
    return game_audio_volume_set_step(cur >= kVolumeStepCount ? 0 : cur + 1);
}

bool game_audio_volume_step_down(void)
{
    const int cur = game_audio_volume_step();
    return game_audio_volume_set_step(cur <= 0 ? kVolumeStepCount : cur - 1);
}

bool game_audio_active(void)
{
    return s_active.load(std::memory_order_acquire);
}
