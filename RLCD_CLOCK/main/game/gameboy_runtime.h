// 声明游戏任务的创建/销毁、LCD 门闸所有权与实测性能快照。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GameboyRuntimeStats {
    bool running;
    uint32_t emulated_frames;   // 累计模拟帧数
    uint32_t presented_frames;  // 累计推屏次数
    uint32_t fps_x10;           // 最近窗口的显示帧率 x10
    uint32_t emu_us_avg;        // 单帧模拟耗时（us）
    uint32_t present_us_avg;    // 单次推屏耗时（us）
    uint32_t present_stride;    // 当前每 N 帧推一次屏
    uint32_t loop_us_avg;       // 实测循环周期（us，含模拟+推屏+睡眠）
    uint32_t loop_us_max;       // 最近窗口内最差循环周期（us，用于判断是否被抢占）
};

// 载入当前选中的 ROM 并启动模拟器任务。失败返回 false，原因见 gameboy_runtime_last_error()。
bool gameboy_runtime_start(void);
// 停止模拟器任务、保存 .sav 并释放 PSRAM。可从任意任务调用。
void gameboy_runtime_stop(void);
bool gameboy_runtime_running(void);

// 暂停/继续。暂停时模拟与推屏都停（画面冻结在最后一帧），任务仍然活着并
// 继续喂任务看门狗（CONFIG_ESP_TASK_WDT_PANIC=y，不喂就是 5 秒后 panic 重启），
// 同时落一次 .sav。屏幕上会出现居中的暂停角标（直接写面板缓冲，不经 LVGL）。
bool gameboy_runtime_paused(void);
void gameboy_runtime_toggle_pause(void);

// L0 帧通部门闸：为真时 ui_display_flush 丢弃 LVGL 像素并立即 flush_ready。
bool gameboy_display_owner(void);
void gameboy_display_owner_store(bool owned);

GameboyRuntimeStats gameboy_runtime_stats(void);
const char *gameboy_runtime_last_error(void);

#ifdef __cplusplus
}
#endif
