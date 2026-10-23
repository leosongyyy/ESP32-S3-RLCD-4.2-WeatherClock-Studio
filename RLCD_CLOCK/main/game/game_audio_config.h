// minigb_apu / GB 音频的编译期配置唯一出处。
//
// 任何编译单元在 include "minigb_apu.h" 之前必须先包含本头（组件
// components/minigb_apu 的 CMakeLists 也以 PUBLIC 编译定义的形式注入同样的
// 值，两边靠下面的锁保持一致，漂移会直接 #error）。
//
// 1) 采样率为什么是 32000 而不是 GB 的"原生" 32768：
//    ES8311 的系数表按 rate×256 精确查表（components/ExternLib/esp_codec_dev/
//    device/es8311/es8311.c 的 get_coeff()），表里没有 32768。用 32768 时
//    es8311_config_sample() 返回 ESP_CODEC_DEV_NOT_SUPPORT →
//    esp_codec_dev_open() 失败 → 现象是「完全无声 + 一行错误日志」，
//    极难排查。表内可用的接近值只有 32000 / 44100 / 48000。
//    取 32000 还因为 minigb_apu 的音高与采样率解耦：
//    FREQ_INC_REF = AUDIO_SAMPLE_RATE × 105，方波 8 步占空比按 8× 时钟系数
//    补偿，任意采样率下音准都正确，不存在「采样率不对音调就不对」的问题。
//    32000Hz 下 AUDIO_SAMPLES = (unsigned)(32000/59.7275) = 535。
//
// 2) 样本格式 S16SYS = int16 本机端序。ES8311 走 16bit PCM；APU 四通道
//    求和峰值约 28672（见 audio_game_sink.cpp 的核算），int16 不会削顶。
#pragma once

#define GB_AUDIO_SAMPLE_RATE 32000

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE GB_AUDIO_SAMPLE_RATE
#elif AUDIO_SAMPLE_RATE != GB_AUDIO_SAMPLE_RATE
#error "AUDIO_SAMPLE_RATE must be 32000: ES8311 coeff table has no 32768 (open fails -> total silence)"
#endif

#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#endif
