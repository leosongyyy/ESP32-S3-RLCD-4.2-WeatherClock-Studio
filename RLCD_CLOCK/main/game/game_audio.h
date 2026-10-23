// GB 游戏音频会话的公共接口（实现见 game_audio.cpp）。
//
// 生命周期由 gameboy_runtime 驱动：
//   - start：gameboy_runtime_start() 里 s_running.store(true) 之后；
//   - stop：gameboy_runtime_stop() 里**模拟器任务确认退出之后**、
//     game_emu_stop() 之前。这里唯一的真实约束是音频任务必须先于 codec
//     关闭停止（game_audio_stop() 内部已保证：先等任务退出，再关 codec），
//     否则音频任务会在已销毁的 CodecPort 上写 I2S（use-after-free）。
//     APU 上下文本身是 peanut_gb_impl.c 的文件作用域静态对象（内部 DRAM），
//     不随 game_emu_stop() 的模拟器缓冲区释放，不存在 APU use-after-free。
#pragma once

#include <stdbool.h>

// 开启音频会话。返回 false 时游戏继续运行、只是没有声音：典型原因是
// 提示音（整点/番茄钟/闹铃）正持有播放互斥门，属预期互斥
// （用户已接受「游戏期间提示音静音」这个副作用）。
bool game_audio_start(void);

// 关闭音频会话：先等音频任务退出，再写一小段尾部静音排空 DMA，
// 最后关闭 codec（功放 GPIO46 由 ES8311 驱动自动拉低）并归还互斥门
// 与电源锁。
void game_audio_stop(void);

// 暂停/恢复。暂停期间音频任务排空存货后只灌静音保住 DMA（功放保持
// 使能，恢复时无开关噗声）。
void game_audio_set_paused(bool paused);

// 会话是否处于活动状态（已 start 未 stop）。
bool game_audio_active(void);

// 模拟器音量：6 档，0 = 静音，1..5 = 20%/40%/60%/80%/100%。
//
// 档位存在 NVS（game_config::kNvsVolumeStepKey），开机恢复，改档时立即落盘。
// 为什么用「档」而不是百分比对外：UI 上一行只能画 5 个格子，回绕（5→0→1）
// 和边界判定都在 0..5 的整数域里做，百分比只是给 codec 和显示用的派生值。

// 当前档位（0..5）。首次调用时从 NVS 恢复（内部只恢复一次）。
int game_audio_volume_step(void);

// 当前档位对应的百分比（0/20/40/60/80/100），供 codec 与 UI 显示用。
int game_audio_volume_percent(void);

// 直接设档。越界返回 false（状态不变）；成功返回 true，值未变也算成功
// （此时不落盘、不打日志，避免重复擦写 flash）。
bool game_audio_volume_set_step(int step);

// +1 档。越过 5 回绕到 0（静音），供按键循环调节。
bool game_audio_volume_step_up(void);

// -1 档。越过 0 回绕到 5，供按键循环调节。
bool game_audio_volume_step_down(void);
