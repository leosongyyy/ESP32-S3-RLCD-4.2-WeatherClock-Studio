// gameboy_pad.h —— 蓝牙手柄（BLE HID）输入接口
//
// 供游戏页与模拟器输入合并使用：按键兜底优先级低于手柄。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GB_BTN_A = 0,
    GB_BTN_B,
    GB_BTN_START,
    GB_BTN_SELECT,
    GB_BTN_UP,
    GB_BTN_DOWN,
    GB_BTN_LEFT,
    GB_BTN_RIGHT,
    GB_BTN_COUNT
} gb_button_t;

typedef enum {
    PAD_STATE_OFF = 0,      // 未启动
    PAD_STATE_STARTING,     // NimBLE host 初始化中
    PAD_STATE_SCANNING,     // 正在扫描 HID 设备
    PAD_STATE_CONNECTING,   // 正在建立连接
    PAD_STATE_DISCOVERING,  // 服务/特征/描述符发现中
    PAD_STATE_READY,        // 已订阅，可以收报文
    PAD_STATE_RETRY_WAIT,   // 断开后等待重连
    PAD_STATE_ERROR         // 栈初始化失败，本次上电不再尝试
} pad_state_t;

// 启动 BLE host 并开始扫描。重复调用幂等。返回 false 表示栈初始化失败。
bool        pad_start(void);

// 停止扫描/断开连接（不反初始化 host）。离开游戏页时调用可省电。
void        pad_stop(void);

pad_state_t pad_state(void);

// 状态的人类可读文本（ASCII，给 LVGL label 用）。
const char *pad_state_text(void);

// 已连接手柄名（未连接返回空串）。
const char *pad_name(void);

// 某个 GB 按键当前是否按下。
bool        pad_button(gb_button_t button);

// 收到过多少条 HID 报文，用于判断链路是否真的在工作。
uint32_t    pad_report_count(void);

// 解析器模式（调试用）：0=未知/未锁定，1=Xbox BLE 固定布局，2=通用 Hat 布局，
// 3=轴在前布局（b[0..3] 四轴居中 0x80，b[4]=hat，b[5]=按键低字节）。
int         pad_report_mode(void);

// 由游戏任务/UI 任务周期调用，驱动断线重连（不占独立任务栈）。
void        pad_poll(void);

// 手柄服务任务：懒创建，按「游戏页是否可见」自动开关扫描与连接，
// 并把报文状态同步进 gameboy_input 的 remote 位图。
void        pad_service_ensure(void);
// 游戏页每次刷新时调用；超过 1.5s 没有调用就自动停止扫描以省电。
void        pad_service_notify_visible(void);

#ifdef __cplusplus
}
#endif
