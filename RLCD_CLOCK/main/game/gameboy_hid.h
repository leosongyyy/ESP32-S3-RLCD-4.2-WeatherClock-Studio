// gameboy_hid.h —— HID 报告描述符（Report Map）解析器
//
// 为什么需要这一层：蓝牙手柄的 input report 布局千差万别，靠「报文看起来像
// 什么」去猜（见 gameboy_pad_ble.c 的 mode 1 / 2 / 3 启发式）迟早会在某台设备
// 上猜错。现场已经踩过一次：把左摇杆 X 轴（静置 0x80、带 ±1 LSB 抖动）当成
// 按钮字节，于是 bit0 反复翻转 = A 键被持续误触发，用户看到的就是「没按任何
// 键，角色却一直在动」的幽灵按键。
//
// HID Report Map（GATT 特征 0x2A4B）是唯一的权威依据：它逐字段写明了哪些位
// 是按钮、哪些位是 hat、哪些位是轴、各自的取值范围与极性。有了它就不需要猜，
// 手柄插上即可用，也不必让用户手动绑定按键。
//
// 实现范围：HID 1.11 §6.2.2 的短条目（覆盖所有 HOGP 手柄），外加 Push/Pop。
// 纯 C、无动态分配、不依赖 ESP-IDF —— 这样同一份代码能在宿主机上用 gcc 编译
// 跑离线自检（见 hid_selftest.c）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 字段表上限。真实手柄的 report map 通常 20~40 个字段，64 留了充足余量。
// 超限时解析器停止记录新字段（已解析的部分仍然可用），绝不越界写。
#define HID_MAX_FIELDS 64

// 单个字段最多记录多少个「逐个列出的 Usage」。
//
// 为什么需要：描述符里 `Usage(X) Usage(Y) Usage(Z) Usage(Rz) Report Count(4)`
// 这种写法非常普遍 —— 四个轴合成**一个**字段，只有 Usage Min/Max（0x30..0x35）
// 的话上层分不清第 2 个字节到底是 Y 还是 Z。逐个记下来才能按下标查。
// 超过 8 个的写法要么是 Usage Min/Max 范围（不进这张表），要么罕见，截断即可。
#define HID_MAX_USAGE_LIST 8

typedef enum {
    HID_FIELD_CONST  = 0,   // Constant：填充位/保留位，绝不能当按键读
    HID_FIELD_BUTTON = 1,   // Usage Page 0x09（Button）
    HID_FIELD_HAT    = 2,   // Usage Page 0x01（Generic Desktop），Usage 0x39（Hat switch）
    HID_FIELD_AXIS   = 3,   // Usage Page 0x01，Usage 0x30..0x35（X/Y/Z/Rx/Ry/Rz）
    HID_FIELD_OTHER  = 4
} hid_field_kind_t;

typedef struct {
    uint8_t  report_id;     // 所属 report ID（描述符没声明 ID 时为 0）
    uint8_t  kind;          // hid_field_kind_t
    uint8_t  main_flags;    // Input item 的原始 8-bit data（bit0=Constant, bit1=Variable, bit2=Relative）
    uint16_t bit_offset;    // 相对该 report **数据体**（不含 report ID 字节）的起始位
    uint16_t bit_size;      // Report Size
    uint16_t count;         // Report Count
    uint16_t usage_page;
    uint16_t usage_min;     // 范围用法的最小值（非范围时等于 usage_max）
    uint16_t usage_max;
    // 逐个列出的 Usage（Usage(X) Usage(Y) ... 写法）。usage_count == 0 表示这
    // 个字段用的是 Usage Min/Max 范围，此时只能看 usage_min / usage_max。
    uint16_t usage_list[HID_MAX_USAGE_LIST];
    uint8_t  usage_count;
    int32_t  logical_min;
    int32_t  logical_max;
} hid_field_t;

// ⚠ 这个结构体带 tag（struct hid_map_info）是有意的：gameboy_pad_bind.h 需要
//   在**不** include 本头文件的前提下声明 `const hid_map_info_t *pad_hid_map()`
//   （实现在 gameboy_pad_ble.c，反向 include 会造成头文件循环）。有了 tag 就可以
//   只写前置声明 `struct hid_map_info;`。
typedef struct hid_map_info {
    bool     valid;
    bool     has_report_id;
    uint16_t field_count;
    hid_field_t fields[HID_MAX_FIELDS];
} hid_map_info_t;

// 解析一份 HID Report Map。
//
// 返回 true 表示至少解析出一个字段（此时 out->valid 已置位）；返回 false 表示
// 参数非法或这段描述符里一个 input 字段都没有，调用方应回退到启发式路径。
bool hid_parse_report_map(const uint8_t *map, uint16_t len, hid_map_info_t *out);

// 从报文中按小端位序提取 bit_size（1..32）位。
//
// ⚠ HID 的位序是「字节内 LSB 在前」：bit_offset 0 是 body[0] 的 bit0，
//   而不是 bit7。写成 MSB 在前的话，一个字节里的 8 个按钮位会整体镜像，
//   表现为「按 A 触发了 H」这类完全对不上号的现象。
// 越界 / 参数非法一律返回 0（宁可读到 0 也不要读越界内存）。
uint32_t hid_extract_bits(const uint8_t *body, uint16_t body_len,
                          uint16_t bit_offset, uint16_t bit_size);

// 在若干 input report 中挑出与当前通知报文长度吻合的那个，并给出数据体指针与
// 长度。
//
// 为什么要专门挑：一份 report map 里往往同时声明好几个 input report（手柄本体、
// 6 轴传感器、触摸板……），而我们订阅到的只有其中一个。选错 report 的表现和
// 「report ID 判错」一模一样 —— 偏移整体错位，按钮全乱。
//
// 返回值（**不是** bool）：
//   0 = 没命中，调用方应回退到启发式路径；
//   1 = 精确命中（长度与描述符算出来的完全吻合）；
//   2 = 宽松命中（尾部多了 1..2 字节，或走到了第 4 步兜底）。
//
// ⚠ 为什么允许尾部多出字节：真实设备经常在通知尾部挂厂商字节、时间戳或对齐
//   填充。原先要求 `expected == len` 严格相等，这类设备两个候选全部落空、直接
//   掉进"猜"的兜底分支，正是"描述符优先却变成完全没反应"的成因之一。
//   上层拿到 2 就知道这一帧是"大致对得上但不敢打包票"，可以据此降级或加日志。
int hid_pick_report(const hid_map_info_t *info,
                    const uint8_t *data, uint16_t len,
                    uint8_t *out_report_id,
                    const uint8_t **out_body, uint16_t *out_body_len);

// 统计某个 report 里「上层真正用得上的字段」数量。
//
// 用得上的定义：非常量的 BUTTON 字段 + HAT 字段 + **能驱动方向键**的 AXIS 字段
// （X/Y = 0x30/0x31，或退而求其次的 Rx/Ry = 0x33/0x34）。其余（填充位、厂商
// 自定义、Z/Rz 这类无法映射成方向键的轴）都不算。
//
// ⚠ 为什么要有这个函数：它是「描述符优先」这道架构的安全网。描述符路径一旦
//   接管，启发式就被整段跳过；若挑中的 report 里一个用得上的字段都没有，那条
//   路径只会永远吐 0 键，用户看到的就是"手柄完全没反应"——比改造前（启发式
//   至少还能动）更糟。上层在解析前先查一次这个计数，为 0 就主动 return false
//   让位给启发式，保证任何情况下都不会比改造前更差。
uint16_t hid_report_usable_fields(const hid_map_info_t *info, uint8_t report_id);

// 查询某个字节（相对 report **数据体**，即 hid_pick_report() 给出的 body）落在
// 哪个字段上。越界或找不到返回 NULL。
//
// 为什么需要：绑定表记的是「第 n 个字节的哪几位」，而只有描述符才知道第 n 个
// 字节到底是按钮、Hat、摇杆轴还是填充位。现场踩过的坑就是反过来做 —— 捕获时
// 按升序取第一个变化的字节，结果取到了最靠前的摇杆轴（静置 0x80、带 ±1 LSB
// 抖动），绑定判定式落在一个自己会跳的字节上，屏幕上的按键就一直跳。
//
// ⚠ 一个字节可能同时落在两个字段里（典型：低 4 位是 Hat、高 4 位是填充）。
//   此时按「可信度」取：BUTTON > HAT > AXIS > OTHER > CONST —— 填充位（CONST）
//   永远不能赢，否则"半个字节是方向键"会被判成"整字节是填充"而失效；
//   反过来按钮/Hat 必须赢过填充。交集更大者次之，都相同取靠前的那个。
const hid_field_t *hid_field_at_byte(const hid_map_info_t *info,
                                     uint8_t report_id, uint16_t byte_index);

// 同上，但入参是**原始通知报文**的字节下标，且**不指定** report ID。
//
// 为什么还要这一层：通知里到底带不带 report ID 字节，设备各不相同，而且只有
// 拿到真实报文才能确定（描述符只写"我定义了 ID"，不写"通知里塞不塞"）。所以
// 两种解释都试：
//   - 通知不带 ID → body_idx = raw_byte_index
//   - 通知带 ID   → body_idx = raw_byte_index - 1（raw_byte_index == 0 时跳过，
//     别算成 0xFFFF 那种下溢）
// 取"可信度最高"的那个（BUTTON > HAT > 其余），都不命中返回 NULL。
//
// 调用方（绑定表清洗 / 捕获时的字节选择）拿到 NULL 应当按"未知"保守处理，
// 不要退回"猜" —— 猜正是这次要消灭的故障来源。
const hid_field_t *hid_field_at_raw_byte(const hid_map_info_t *info,
                                         uint16_t raw_byte_index);

#ifdef __cplusplus
}
#endif
