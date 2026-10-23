// 声明 GB 帧到 RLCD 打包帧缓冲的零中间缓冲写入接口。
// 只有 peanut_gb_impl.c（lcd_draw_line 回调）与本模块自身会调用这些函数。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 每帧开始时调用：清掉上一帧残留的行配对状态与脏标记。
void game_fb_frame_begin(void);

// 接收 GB 的一条扫描线（160 个 2bit 灰度）。攒满一对行就写入一个面板瓦片行。
// 不建立 160x144 中间帧缓冲（见 PHASE2_SPEC.md P4）。
void game_fb_push_line(uint32_t line, const uint8_t *pixels);

// 运行前一次性准备整屏：全黑底 + 居中窗口黑边。
bool game_fb_prepare_surface(void);

// 若本帧写过像素则整屏推屏，返回是否真的下发过。
bool game_fb_present(void);

// 在面板坐标系（400x300，含黑边区域）里画实心矩形，white=true 置白。
// 逐像素写位，供暂停角标一类的静态叠加使用；不参与行配对状态机。
void game_fb_fill_rect(int x0, int y0, int x1, int y1, bool white);

// 在最后一帧上盖一个居中的「暂停」角标（黑底白框 + 两条竖杠），并把帧标脏。
// 调用方自己决定什么时候 game_fb_present()。不用字体：此刻整屏归门闸独占，
// 不能跨任务调 LVGL。返回 false 表示面板缓冲不可用。
bool game_fb_draw_pause_badge(void);

#ifdef __cplusplus
}
#endif
