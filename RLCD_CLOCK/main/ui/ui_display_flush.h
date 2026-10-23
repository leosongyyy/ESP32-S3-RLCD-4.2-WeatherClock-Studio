// 声明 LVGL 到 RLCD 的底层显示刷新回调。
#pragma once

#include "lvgl.h"

void flush_callback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);

// LVGL 侧已经进入 flush_callback() 的累计次数（每次进入 +1，含被门闸丢弃的那些）。
//
// 用途：模拟器任务抢显示所有者时必须确认「LVGL 上一次 SPI 传输已经返回」。
// flush_callback 只在 ui_task 里串行执行，所以「计数在 owner 置位之后又变了」
// 就等价于「ui_task 已经走到门闸这一步」，背后不可能还有在途的 SPI 写入。
// 不这样做的话，首帧推屏可能和 LVGL 最后一次传输交错写同一块面板缓冲，
// 结果是半屏旧画面（比闪一帧严重得多）。
uint32_t ui_display_flush_entry_count(void);
