// gameboy_pad_bind.h —— 蓝牙手柄按键绑定表
//
// 为什么需要这一层：现有 pad_apply_*() 靠"报文看起来像什么"去猜布局，
// 猜错一次整场会话的按键就全废（现场已经踩过：Q36 for Android 的报文被判成
// 带 report ID，偏移错 1 字节，A/B/方向键恒为 0）。绑定表换一条路走：让用户
// 在屏幕上指着"A 键"按下手柄上一个物理键，把**哪一字节的哪些位变成了什么**
// 直接记下来。之后判定就变成纯粹的位运算，不再依赖任何布局假设。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- HID Report Map 的取值接口（实现在 gameboy_pad_ble.c） ----
//
// ⚠ 这里**刻意只做前置声明，不 include gameboy_hid.h，更不 include
//   gameboy_pad_ble 的任何东西**：gameboy_pad_ble.c 要 include 本头文件，
//   本头文件再反向 include 它就成环了。
//
// 之所以能这么写：gameboy_hid.h 里的 hid_map_info_t 带结构体 tag
// （`typedef struct hid_map_info { ... } hid_map_info_t;`），所以这里可以用
// `struct hid_map_info;` 前置声明 + 同类型 typedef 重声明（C11 6.7p3 允许把
// typedef 名重新定义为它已经代表的同一个类型；C++ 同样允许）。
//
// 代价：只包含本头文件的翻译单元拿到的是**不完整类型**，只能传指针。
// 需要访问字段的 .c（gameboy_pad_bind.c）自己 include gameboy_hid.h —— 那个
// 头文件没有任何依赖，不会引入环。
struct hid_map_info;
typedef struct hid_map_info hid_map_info_t;

// 本次连接解析出来的 HID 描述符；还没解析好 / 读不到时返回 NULL。
const hid_map_info_t *pad_hid_map(void);

// 可绑定的动作。顺序就是绑定页的遍历顺序（同时也是 NVS blob 里的条目下标）。
//
// ⚠ 调整顺序会改变 NVS 里已有记录的含义，必须同步 PAD_BIND_BLOB_VERSION 让
//   老绑定作废，否则用户升级后会发现"按 A 实际触发了 B"。
//
// GB 只有 A / B / START / SELECT / 方向这 8 个键，所以 X / Y 是**映射到已有 GB
// 键**上的第二组位置（X -> GB 的 A，Y -> GB 的 B，映射表见 gameboy_pad_ble.c
// 的 kBindMap）。用意是让用户选择趁手的那个按键：有些人习惯用外侧那两个键，
// 手上这只手柄凑巧又把它们印成了 X / Y。多个绑定指向同一个 GB 键时判定按
// 「任一被按下」处理。
typedef enum {
    PAD_BIND_A = 0,
    PAD_BIND_B,
    PAD_BIND_X,
    PAD_BIND_Y,
    PAD_BIND_UP,
    PAD_BIND_DOWN,
    PAD_BIND_LEFT,
    PAD_BIND_RIGHT,
    PAD_BIND_START,
    PAD_BIND_SELECT,
    PAD_BIND_COUNT
} pad_bind_action_t;

// 一条绑定记录。
//
// 判定规则统一成位运算：(report[byte_index] & mask) == expect
//   - 普通按钮位：mask = 变化的那 1 位，expect = 该位（按下时为 1）
//   - Hat / 枚举型字段（一次变化涉及多个位）：mask = 全部变化的位，
//     expect = 按下时的位值。这样"上=0x00、下=0x04"这类编码也能正确区分。
typedef struct {
    uint8_t  valid;        // 0 = 未绑定
    uint16_t report_len;   // 绑定时捕获到的报文长度（layout 变更即失效）
    uint8_t  byte_index;   // 原始报文字节下标
    uint8_t  mask;         // 关心的位
    uint8_t  expect;       // (byte & mask) == expect 视为按下
} pad_bind_entry_t;

// 启动时调一次（懒加载 NVS，可重复调用）。
void pad_bind_init(void);

// 读一条绑定。未绑定/未初始化时返回 valid=0 的零值。
pad_bind_entry_t pad_bind_get(pad_bind_action_t action);

// 覆盖写入一条绑定并落盘（内部用）。
void pad_bind_set(pad_bind_action_t action, const pad_bind_entry_t *entry);

// 清掉一条 / 全部，并落盘。
void pad_bind_clear(pad_bind_action_t action);
void pad_bind_clear_all(void);

// 全部 9 个动作是否都已绑定。
bool pad_bind_all_set(void);

// 是否有任何一条已绑定。
bool pad_bind_any_set(void);

// 用一帧原始报文判定某个动作是否按下。
// 报文长度与绑定时不一致时视为未按下（换了手柄/换了模式不会误触发）。
bool pad_bind_pressed(pad_bind_action_t action, const uint8_t *data, uint16_t len);

// ---- 实时按下状态（给绑定页 UI 做按键闪烁用） ----
//
// 为什么放在这一层而不是让 UI 自己去查 pad_bind_pressed()：UI 任务拿不到原始
// 报文，报文只在 BLE 回调里出现一次。这里按最新一包统一算好，UI 只读结果。
// 每次进新报文都会整体重算，所以"松开"自动体现，不需要额外的清理时机。
void pad_bind_update_live(const uint8_t *data, uint16_t len);

// 某个动作此刻是否被手柄按着。未绑定恒为 false。
//
// ⚠ 这是**两个来源的或**：绑定表推导的那份（pad_bind_update_live()）+ 描述符
//   推导的那份（pad_bind_update_live_desc()）。描述符生效后用户不绑任何键也能
//   玩，若这里只看绑定表，绑定页上的圈会永远不亮 —— 用户会以为手柄没连上。
bool pad_bind_action_live(pad_bind_action_t action);

// ---- 实时按下状态（第二来源：HID Report Map 描述符推导） ----
//
// 为什么要有第二个来源：第一份（pad_bind_update_live()）完全依赖用户手动绑定
// 的条目，没绑过就是全 false。而描述符（GATT 0x2A4B）是权威的字段布局，解析出
// 来的按下状态不需要任何绑定。两者互相独立，UI 侧取或 —— 谁有结果谁点亮。
//
// ⚠ 调用方必须传 PAD_BIND_COUNT 长度的数组；传 NULL 表示"描述符路径当前没有
//   结论"，会把这份状态清零（手柄断线时就是靠这个把圈熄灭的）。
void pad_bind_update_live_desc(const bool live[PAD_BIND_COUNT]);

// ---- 捕获流程（非阻塞，由 UI 每帧轮询进度） ----
//
// BLE 层每收到一包原始报文就要调 pad_bind_on_raw_report()，捕获状态机靠它推进。
// 捕获期间 BLE 层应当跳过正常解析（否则半成品绑定会把乱码按键喂给模拟器）。
typedef enum {
    PAD_BIND_CAP_IDLE = 0,     // 没在捕获
    PAD_BIND_CAP_WAIT = 1,     // 已开始，等用户按下一个物理键
    PAD_BIND_CAP_RELEASE = 2,  // 抓到了，等用户松手（松手后才写基线，防止连击）
    PAD_BIND_CAP_DONE = 3,     // 成功并已落盘
    PAD_BIND_CAP_FAIL = 4,     // 失败（超时 / 无有效变化 / 被取消）
} pad_bind_capture_stage_t;

// 开始捕获。会先要求"所有键都处于松开状态"作为基线：基线报文的采集也可能
// 需要用户松手，所以返回后要轮询 pad_bind_capture_stage()。
void pad_bind_capture_start(pad_bind_action_t action);

// 取消捕获（退出页面时务必调用，否则 BLE 层会一直跳过正常解析）。
void pad_bind_capture_cancel(void);

pad_bind_capture_stage_t pad_bind_capture_stage(void);

// 已经确认了几次「按下-松开」（0 .. PAD_BIND_REQUIRED_CYCLES）。
// UI 拿它显示"第 2/3 次"，否则用户不知道为什么按了一次还不算完。
uint8_t pad_bind_capture_cycles(void);

// 确认一次绑定要求的完整「按下-松开」次数。见 gameboy_pad_bind.c 里的说明：
// 串键（误把别的变化当成本次按键）是最常见的绑定事故，要求连续三次得到完全
// 相同的样本才能把偶发串键挡在外面。
#define PAD_BIND_REQUIRED_CYCLES 3

// 捕获超时（毫秒）。UI 侧到点后调 cancel 并提示。
// 要比单次按键留足余量 —— 现在一次绑定要连按三次，窗口不能太紧。
#define PAD_BIND_CAPTURE_TIMEOUT_MS 30000

// 由 BLE 层每收到一包原始报文调用（未剥 report ID 的原始 data/len）。
void pad_bind_on_raw_report(const uint8_t *data, uint16_t len);

// 给 UI 用的中文动作名（UTF-8）。越界返回 "?"。
const char *pad_bind_action_name(pad_bind_action_t action);

#ifdef __cplusplus
}
#endif
