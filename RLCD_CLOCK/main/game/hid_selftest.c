// hid_selftest.c —— gameboy_hid.c 的离线自检（宿主机 gcc，不进固件构建）
//
// 为什么要有这一层：HID 描述符解析器的 bug 在设备上极难定位 —— 一旦位算错，
// 现场症状就是「按 A 触发 B」「没按键角色一直动」，而日志里只有一堆十六进制。
// 这里把手写的样例描述符（1..6，覆盖 Push/Pop、填充位、有无 Report ID 等通用
// 规则）与一份真机抓下来的描述符（7 = Q36 for Android，122 字节原文）一起喂进
// 去，与**人工预先算好**的字段表逐项比对，任何一项不符就直接 FAIL。
//
// 编译（不进 CMakeLists，main/CMakeLists.txt 用的是显式源文件列表）：
//   gcc -std=c11 -Wall -Wextra -Werror -O1 -DHID_SELFTEST -I. \
//       hid_selftest.c gameboy_hid.c -o /tmp/hid_selftest && /tmp/hid_selftest

#include "gameboy_hid.h"

#ifdef HID_SELFTEST

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- 测试框架

static int g_fail = 0;

static const char *kind_name(uint8_t kind)
{
    switch (kind) {
    case HID_FIELD_CONST:  return "CONST";
    case HID_FIELD_BUTTON: return "BUTTON";
    case HID_FIELD_HAT:    return "HAT";
    case HID_FIELD_AXIS:   return "AXIS";
    default:               return "OTHER";
    }
}

typedef struct {
    uint8_t  rid;
    uint8_t  kind;
    uint16_t off;
    uint16_t size;
    uint16_t cnt;
    uint16_t page;
    uint16_t umin;
    uint16_t umax;
    int32_t  lmin;
    int32_t  lmax;
    uint8_t  flags;    // Input item 的原始 data（0x42 = Null-state Hat，0x01 = 填充）
} expect_field_t;

typedef struct {
    const char        *name;
    const uint8_t     *map;
    uint16_t           map_len;
    bool               has_id;
    uint16_t           field_count;
    const expect_field_t *fields;
} expect_map_t;

static void print_fields(const hid_map_info_t *info)
{
    for (uint16_t k = 0; k < info->field_count; ++k) {
        const hid_field_t *f = &info->fields[k];
        printf("    [%u] rid=%u kind=%-6s off=%3u size=%2u cnt=%2u "
               "page=0x%02X usage=%u..%u lmin=%d lmax=%d flags=0x%02X\n",
               k, (unsigned)f->report_id, kind_name(f->kind),
               (unsigned)f->bit_offset, (unsigned)f->bit_size,
               (unsigned)f->count, (unsigned)f->usage_page,
               (unsigned)f->usage_min, (unsigned)f->usage_max,
               (int)f->logical_min, (int)f->logical_max,
               (unsigned)f->main_flags);
    }
}

static void check_map(const expect_map_t *e)
{
    hid_map_info_t info;
    printf("\n=== %s (map %u bytes) ===\n", e->name, (unsigned)e->map_len);
    const bool ok = hid_parse_report_map(e->map, e->map_len, &info);
    if (!ok) {
        printf("  FAIL: hid_parse_report_map returned false\n");
        ++g_fail;
        return;
    }
    printf("  has_report_id=%d field_count=%u\n",
           (int)info.has_report_id, (unsigned)info.field_count);
    print_fields(&info);

    if (info.has_report_id != e->has_id) {
        printf("  FAIL: has_report_id expected %d got %d\n",
               (int)e->has_id, (int)info.has_report_id);
        ++g_fail;
    }
    if (info.field_count != e->field_count) {
        printf("  FAIL: field_count expected %u got %u\n",
               (unsigned)e->field_count, (unsigned)info.field_count);
        ++g_fail;
        return;
    }
    for (uint16_t k = 0; k < e->field_count; ++k) {
        const hid_field_t *f = &info.fields[k];
        const expect_field_t *x = &e->fields[k];
        int bad = 0;
        if (f->report_id != x->rid)    { bad = 1; }
        if (f->kind != x->kind)        { bad = 2; }
        if (f->bit_offset != x->off)   { bad = 3; }
        if (f->bit_size != x->size)    { bad = 4; }
        if (f->count != x->cnt)        { bad = 5; }
        if (f->usage_page != x->page)  { bad = 6; }
        if (f->usage_min != x->umin)   { bad = 7; }
        if (f->usage_max != x->umax)   { bad = 8; }
        if (f->logical_min != x->lmin) { bad = 9; }
        if (f->logical_max != x->lmax) { bad = 10; }
        if (f->main_flags != x->flags) { bad = 11; }
        if (bad != 0) {
            printf("  FAIL: field %u mismatch (check #%d)\n"
                   "        got    rid=%u kind=%s off=%u size=%u cnt=%u "
                   "page=0x%02X usage=%u..%u lmin=%d lmax=%d flags=0x%02X\n"
                   "        expect rid=%u kind=%s off=%u size=%u cnt=%u "
                   "page=0x%02X usage=%u..%u lmin=%d lmax=%d flags=0x%02X\n",
                   k, bad,
                   (unsigned)f->report_id, kind_name(f->kind),
                   (unsigned)f->bit_offset, (unsigned)f->bit_size,
                   (unsigned)f->count, (unsigned)f->usage_page,
                   (unsigned)f->usage_min, (unsigned)f->usage_max,
                   (int)f->logical_min, (int)f->logical_max,
                   (unsigned)f->main_flags,
                   (unsigned)x->rid, kind_name(x->kind),
                   (unsigned)x->off, (unsigned)x->size, (unsigned)x->cnt,
                   (unsigned)x->page, (unsigned)x->umin, (unsigned)x->umax,
                   (int)x->lmin, (int)x->lmax, (unsigned)x->flags);
            ++g_fail;
        }
    }
    // 与固件里 pad_apply_descriptor() 同样的推导：按钮位总数 / 有没有 hat /
    // 有没有 X、Y 轴（没有时记 -1）。这三样决定了上层能不能免绑定直接玩。
    uint32_t buttons = 0;
    int      ax_x = -1;
    int      ax_y = -1;
    bool     has_hat = false;
    for (uint16_t k = 0; k < info.field_count; ++k) {
        const hid_field_t *f = &info.fields[k];
        if (f->kind == (uint8_t)HID_FIELD_BUTTON) {
            buttons += (uint32_t)f->bit_size * (uint32_t)f->count;
        } else if (f->kind == (uint8_t)HID_FIELD_HAT) {
            has_hat = true;
        } else if (f->kind == (uint8_t)HID_FIELD_AXIS) {
            // 与固件一致：按 usage **范围**判定（X/Y 常常被合成一个
            // usage 0x30..0x31 的字段，只看 usage_min 会漏掉 Y）。
            const uint16_t lo = f->usage_min;
            const uint16_t hi = (f->usage_max >= lo) ? f->usage_max : lo;
            if (lo <= 0x30u && 0x30u <= hi) { ax_x = 0x30; }
            if (lo <= 0x31u && 0x31u <= hi) { ax_y = 0x31; }
        }
    }
    printf("  derived: buttons=%u hat=%s axX=%d axY=%d total_bits=%u\n",
           (unsigned)buttons, has_hat ? "yes" : "no", ax_x, ax_y,
           (unsigned)((info.fields[info.field_count - 1].bit_offset +
                       (uint32_t)info.fields[info.field_count - 1].bit_size *
                       (uint32_t)info.fields[info.field_count - 1].count)));
}

// ---------------------------------------------------------------- 描述符 1
// 典型「16 按钮 + 4 位 Hat + 4 轴」通用手柄，带 Report ID 1。
// 人工核对：轴 32 位(0..31) + hat 4 位(32..35) + 按钮 16 位(36..51) = 52 位
//           → 52/8 向上取整 = 7 字节（不含 ID）。
static const uint8_t kMap1[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x09, 0x32,       //   Usage (Z)
    0x09, 0x35,       //   Usage (Rz)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x04,       //   Report Count (4)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x09, 0x39,       //   Usage (Hat switch)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x07,       //   Logical Maximum (7)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null State)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x10,       //   Usage Maximum (16)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x10,       //   Report Count (16)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xC0              // End Collection
};

static const expect_field_t kExpect1[] = {
    { 1, HID_FIELD_AXIS,   0,  8,  4, 0x01, 0x30, 0x35, 0, 255, 0x02 },
    { 1, HID_FIELD_HAT,   32,  4,  1, 0x01, 0x39, 0x39, 0, 7,   0x42 },
    { 1, HID_FIELD_BUTTON, 36, 1, 16, 0x09, 0x01, 0x10, 0, 1,   0x02 },
};

// ---------------------------------------------------------------- 描述符 2
// 不带 Report ID 的精简描述符：2 轴 + 4 位 hat + 8 按钮。
// 人工核对：16 + 4 + 8 = 28 位 → 4 字节。
static const uint8_t kMap2[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x02,       //   Report Count (2)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x09, 0x39,       //   Usage (Hat switch)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x07,       //   Logical Maximum (7)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null State)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x08,       //   Usage Maximum (8)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xC0              // End Collection
};

static const expect_field_t kExpect2[] = {
    { 0, HID_FIELD_AXIS,   0,  8,  2, 0x01, 0x30, 0x31, 0, 255, 0x02 },
    { 0, HID_FIELD_HAT,   16,  4,  1, 0x01, 0x39, 0x39, 0, 7,   0x42 },
    { 0, HID_FIELD_BUTTON, 20, 1, 8, 0x09, 0x01, 0x08, 0, 1,    0x02 },
};

// ---------------------------------------------------------------- 描述符 3
// 带 Push / Pop 的描述符（Report ID 5）。
//
// ⚠ 这份的区分度：Pop 之后必须恢复到 Push 时的 size=8 / count=2 / lmin=0 /
//   lmax=255。若解析器没实现 Push/Pop，第三个字段会变成 size=4 / count=1 /
//   lmin=1 / lmax=8 —— 位偏移也会从 20 变成 20 但 size 错，测试必然 FAIL。
// 人工核对：16 + 4 + 16 = 36 位 → 5 字节。
static const uint8_t kMap3[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x05,       //   Report ID (5)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x02,       //   Report Count (2)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xA4,             //   Push          ← 保存 size=8/count=2/lmin=0/lmax=255
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x15, 0x01,       //   Logical Minimum (1)
    0x25, 0x08,       //   Logical Maximum (8)
    0x09, 0x39,       //   Usage (Hat switch)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null State)
    0xB4,             //   Pop           ← 恢复 size=8/count=2/lmin=0/lmax=255
    0x09, 0x32,       //   Usage (Z)
    0x09, 0x35,       //   Usage (Rz)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xC0              // End Collection
};

static const expect_field_t kExpect3[] = {
    { 5, HID_FIELD_AXIS,  0,  8, 2, 0x01, 0x30, 0x31, 0, 255, 0x02 },
    { 5, HID_FIELD_HAT,  16,  4, 1, 0x01, 0x39, 0x39, 1, 8,   0x42 },
    { 5, HID_FIELD_AXIS, 20,  8, 2, 0x01, 0x32, 0x35, 0, 255, 0x02 },
};

// ---------------------------------------------------------------- 描述符 4
// 含 Constant 填充位的描述符（Report ID 3）。
//
// ⚠ 关键判据：两个 0x81,0x01（Input(Const)）字段的 kind 必须是 HID_FIELD_CONST，
//   绝不能被当成按钮。填充位通常取值 0 或 1，一旦误判成按钮就是常按/常断的
//   幽灵输入 —— 这正是本次改造要消灭的那类故障。
// 人工核对：16 + 4 + 2(const) + 10 + 6(const) = 38 位 → 5 字节。
static const uint8_t kMap4[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x03,       //   Report ID (3)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x02,       //   Report Count (2)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x09, 0x39,       //   Usage (Hat switch)
    0x15, 0x01,       //   Logical Minimum (1)
    0x25, 0x08,       //   Logical Maximum (8)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null State)
    0x75, 0x02,       //   Report Size (2)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x01,       //   Input (Const,Array,Abs)   ← 2 位填充
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x0A,       //   Usage Maximum (10)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x0A,       //   Report Count (10)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x75, 0x06,       //   Report Size (6)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x01,       //   Input (Const,Array,Abs)   ← 6 位填充，补齐整字节
    0xC0              // End Collection
};

static const expect_field_t kExpect4[] = {
    { 3, HID_FIELD_AXIS,   0,  8,  2, 0x01, 0x30, 0x31, 0, 255, 0x02 },
    { 3, HID_FIELD_HAT,   16,  4,  1, 0x01, 0x39, 0x39, 1, 8,   0x42 },
    { 3, HID_FIELD_CONST, 20,  2,  1, 0x01, 0x00, 0x00, 1, 8,   0x01 },
    { 3, HID_FIELD_BUTTON, 22, 1, 10, 0x09, 0x01, 0x0A, 0, 1,   0x02 },
    { 3, HID_FIELD_CONST, 32,  6,  1, 0x09, 0x00, 0x00, 0, 1,   0x01 },
};

// ---------------------------------------------------------------- 描述符 5
// 只有 X/Y 轴、没有按钮也没有 hat（Report ID 7）。
// 用途：证明"可用字段计数"不会**过度**触发降级 —— 只有轴但轴能驱动方向键时，
//       描述符路径应当继续接管（方向键兜底会用 X/Y），不能退回启发式。
// 人工核对：16 位 → 2 字节。usable 应为 1。
static const uint8_t kMap5[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x07,       //   Report ID (7)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x02,       //   Report Count (2)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xC0              // End Collection
};

// ---------------------------------------------------------------- 描述符 6
// 只有 Z 轴、没有按钮也没有 hat（Report ID 8）。
//
// ⚠ 关键判据：Z(0x32) 无法映射成方向键（方向键兜底只认 X/Y 或 Rx/Ry），所以
//   这个 report 解析出来必然是全 0 键。hid_report_usable_fields() 必须对它返回
//   0，上层才能退回启发式 —— 这正是"描述符接管却永远吐 0 键"的死局样本。
// 人工核对：usage_min = usage_max = 0x32 → 不覆盖 0x30/0x31/0x33/0x34。
//           16 位 → 2 字节。usable 应为 0。
static const uint8_t kMap6[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x08,       //   Report ID (8)
    0x09, 0x32,       //   Usage (Z)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x02,       //   Report Count (2)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xC0              // End Collection
};

// ---------------------------------------------------------------- 描述符 7
// Q36「for Android」实机描述符（122 字节，GATT 0x2A4B 读回来的原文，逐字节照抄）。
//
// ⚠ 这份是**真机抓下来的**，不是手写的样例，优先级高于上面 6 份：
//   上面那些用来验证解析器的通用规则（Push/Pop、填充位、有无 ID…），
//   这一份用来验证"这台设备到底被解析成什么样" —— 固件里的字段表、绑定表
//   清洗、捕获字节选择全部以它为准。
//
// 人工解码（与固件 "hid field" 日志逐条对照）：
//   rid 3（Consumer Control）: 16 位 → 2 字节
//   rid 4（Game Pad）       : 4×8 + 4(hat) + 4(填充) + 16(按钮) + 2×8(油门刹车)
//                             + 8(填充) = 80 位 → 10 字节
//   换算成字节：byte0..3 = 四轴，byte4 低 4 位 = Hat、高 4 位 = 填充，
//               byte5 = 按钮 1..8，byte6 = 按钮 9..16，
//               byte7/8 = 油门/刹车，byte9 = 填充。
//
// ⚠ 两个易错点：
//   1. hat 的 flags 是 0x42（Variable + Null state），紧接着那个 4 位填充是
//      0x01 —— 只看 kind 分得出来，但只有 flags 能证明"描述符确实这么写"。
//   2. 四轴是 4 个**独立 Usage**（X/Y/Z/Rz）而不是 Usage Min/Max，所以
//      usage_list 必须按出现顺序是 {0x30, 0x31, 0x32, 0x35}。
static const uint8_t kMap7[] = {
    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage (Consumer Control)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x03,       //   Report ID (3)
    0x75, 0x10,       //   Report Size (16)
    0x95, 0x01,       //   Report Count (1)
    0x15, 0x01,       //   Logical Minimum (1)
    0x26, 0x9C, 0x02, //   Logical Maximum (668)
    0x19, 0x01,       //   Usage Minimum (1)
    0x2A, 0x9C, 0x02, //   Usage Maximum (668)
    0x81, 0x00,       //   Input (Data,Array,Abs)
    0xC0,             // End Collection
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x04,       //   Report ID (4)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x09, 0x32,       //     Usage (Z)
    0x09, 0x35,       //     Usage (Rz)
    0x15, 0x00,       //     Logical Minimum (0)
    0x26, 0xFF, 0x00, //     Logical Maximum (255)
    0x75, 0x08,       //     Report Size (8)
    0x95, 0x04,       //     Report Count (4)
    0x81, 0x02,       //     Input (Data,Var,Abs)
    0xC0,             //   End Collection
    0x09, 0x39,       //   Usage (Hat switch)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x07,       //   Logical Maximum (7)
    0x35, 0x00,       //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14,       //   Unit (Eng Rot: Degree)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null State)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x01,       //   Input (Const)          ← 4 位填充
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x10,       //   Usage Maximum (16)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x10,       //   Report Count (16)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x05, 0x02,       //   Usage Page (Simulation)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x09, 0xC4,       //   Usage (Accelerator)
    0x09, 0xC5,       //   Usage (Brake)
    0x95, 0x02,       //   Report Count (2)
    0x75, 0x08,       //   Report Size (8)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x01,       //   Input (Const)          ← 8 位填充
    0xC0              // End Collection
};

static const expect_field_t kExpect7[] = {
    { 3, HID_FIELD_OTHER,  0, 16, 1, 0x0C, 0x0001, 0x029C, 1, 668, 0x00 },
    { 4, HID_FIELD_AXIS,   0,  8, 4, 0x01, 0x0030, 0x0035, 0, 255, 0x02 },
    { 4, HID_FIELD_HAT,   32,  4, 1, 0x01, 0x0039, 0x0039, 0, 7,   0x42 },
    { 4, HID_FIELD_CONST, 36,  4, 1, 0x01, 0x0000, 0x0000, 0, 7,   0x01 },
    { 4, HID_FIELD_BUTTON,40,  1, 16, 0x09, 0x0001, 0x0010, 0, 1,  0x02 },
    { 4, HID_FIELD_OTHER, 56,  8, 2, 0x02, 0x00C4, 0x00C5, 0, 255, 0x02 },
    { 4, HID_FIELD_CONST, 72,  8, 1, 0x02, 0x0000, 0x0000, 0, 255, 0x01 },
};

// ---------------------------------------------------------------- pick_report

static void check_pick(const char *name, const uint8_t *map, uint16_t map_len,
                       const uint8_t *pkt, uint16_t pkt_len,
                       int want_ret, uint8_t want_rid,
                       uint16_t want_body_len, const uint8_t *want_body)
{
    hid_map_info_t info;
    if (!hid_parse_report_map(map, map_len, &info)) {
        printf("  FAIL: %s: parse failed\n", name);
        ++g_fail;
        return;
    }
    uint8_t rid = 0xFF;
    const uint8_t *body = NULL;
    uint16_t body_len = 0;
    const int ret = hid_pick_report(&info, pkt, pkt_len, &rid, &body, &body_len);
    printf("  pick %-30s -> ret=%d rid=%u body_len=%u (want ret=%d rid=%u "
           "body_len=%u)\n",
           name, ret, (unsigned)rid, (unsigned)body_len,
           want_ret, (unsigned)want_rid, (unsigned)want_body_len);
    if (ret != want_ret) {
        printf("  FAIL: %s: ret mismatch\n", name);
        ++g_fail;
        return;
    }
    if (want_ret == 0) {
        return;
    }
    if (rid != want_rid) { printf("  FAIL: %s: rid mismatch\n", name); ++g_fail; return; }
    if (body_len != want_body_len) {
        printf("  FAIL: %s: body_len mismatch\n", name);
        ++g_fail;
        return;
    }
    if (want_body != NULL && body != NULL &&
        memcmp(body, want_body, want_body_len) != 0) {
        printf("  FAIL: %s: body content mismatch\n", name);
        ++g_fail;
    }
    if (want_body != NULL && body == NULL) {
        printf("  FAIL: %s: body is NULL\n", name);
        ++g_fail;
    }
}

// 校验"可用字段计数"。这是上层决定要不要退回启发式的依据。
static void check_usable(const char *name, const uint8_t *map, uint16_t map_len,
                         uint8_t rid, uint16_t want)
{
    hid_map_info_t info;
    if (!hid_parse_report_map(map, map_len, &info)) {
        printf("  FAIL: %s: parse failed\n", name);
        ++g_fail;
        return;
    }
    const uint16_t got = hid_report_usable_fields(&info, rid);
    printf("  usable %-27s rid=%u -> %u (want %u)\n", name, (unsigned)rid,
           (unsigned)got, (unsigned)want);
    if (got != want) {
        printf("  FAIL: %s: usable mismatch\n", name);
        ++g_fail;
    }
}

static void check_bits(const char *name, const uint8_t *body, uint16_t body_len,
                       uint16_t off, uint16_t size, uint32_t want)
{
    const uint32_t got = hid_extract_bits(body, body_len, off, size);
    printf("  bits %-24s off=%2u size=%2u -> 0x%08X (want 0x%08X)\n",
           name, (unsigned)off, (unsigned)size, (unsigned)got, (unsigned)want);
    if (got != want) {
        printf("  FAIL: %s: extract mismatch\n", name);
        ++g_fail;
    }
}

// 校验 hid_field_at_byte()：某个字节落在什么字段上。want_kind < 0 表示期望 NULL。
static void check_field_at_byte(const char *name, const uint8_t *map,
                                uint16_t map_len, uint8_t rid,
                                uint16_t byte_index, int want_kind)
{
    hid_map_info_t info;
    if (!hid_parse_report_map(map, map_len, &info)) {
        printf("  FAIL: %s: parse failed\n", name);
        ++g_fail;
        return;
    }
    const hid_field_t *f = hid_field_at_byte(&info, rid, byte_index);
    if (want_kind < 0) {
        if (f != NULL) {
            printf("  FAIL: %s: expected NULL, got kind=%s\n", name,
                   kind_name(f->kind));
            ++g_fail;
        } else {
            printf("  at_byte %-28s -> NULL (ok)\n", name);
        }
        return;
    }
    if (f == NULL) {
        printf("  FAIL: %s: got NULL, want %s\n", name,
               kind_name((uint8_t)want_kind));
        ++g_fail;
        return;
    }
    printf("  at_byte %-28s -> kind=%s (want %s)\n", name,
           kind_name(f->kind), kind_name((uint8_t)want_kind));
    if (f->kind != (uint8_t)want_kind) {
        printf("  FAIL: %s: kind mismatch\n", name);
        ++g_fail;
    }
}

// 校验 hid_field_at_raw_byte()：入参是**原始报文**下标，且不指定 report ID。
static void check_raw_byte(const char *name, const uint8_t *map,
                           uint16_t map_len, uint16_t raw_index, int want_kind)
{
    hid_map_info_t info;
    if (!hid_parse_report_map(map, map_len, &info)) {
        printf("  FAIL: %s: parse failed\n", name);
        ++g_fail;
        return;
    }
    const hid_field_t *f = hid_field_at_raw_byte(&info, raw_index);
    if (f == NULL) {
        if (want_kind < 0) {
            printf("  raw_byte %-27s -> NULL (ok)\n", name);
        } else {
            printf("  FAIL: %s: got NULL, want %s\n", name,
                   kind_name((uint8_t)want_kind));
            ++g_fail;
        }
        return;
    }
    printf("  raw_byte %-27s -> kind=%s (want %s)\n", name,
           kind_name(f->kind),
           (want_kind < 0) ? "NULL" : kind_name((uint8_t)want_kind));
    if (want_kind < 0 || f->kind != (uint8_t)want_kind) {
        printf("  FAIL: %s: kind mismatch\n", name);
        ++g_fail;
    }
}

// ---------------------------------------------------------------- main

int main(void)
{
    printf("hid selftest start\n");

    const expect_map_t maps[] = {
        { "1) 16btn+hat+4axis, with Report ID 1", kMap1,
          (uint16_t)sizeof(kMap1), true, 3, kExpect1 },
        { "2) slim descriptor, no Report ID", kMap2,
          (uint16_t)sizeof(kMap2), false, 3, kExpect2 },
        { "3) with Push/Pop", kMap3,
          (uint16_t)sizeof(kMap3), true, 3, kExpect3 },
        { "4) with Constant padding bits", kMap4,
          (uint16_t)sizeof(kMap4), true, 5, kExpect4 },
        { "7) Q36 for Android (real device)", kMap7,
          (uint16_t)sizeof(kMap7), true, 7, kExpect7 },
    };

    for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); ++i) {
        check_map(&maps[i]);
    }

    // ---- hid_extract_bits ----
    printf("\n=== hid_extract_bits ===\n");
    static const uint8_t kBody[2] = { 0xAB, 0xCD };
    check_bits("byte0", kBody, 2, 0, 8, 0xABu);
    check_bits("byte1", kBody, 2, 8, 8, 0xCDu);
    check_bits("both little-endian", kBody, 2, 0, 16, 0xCDABu);
    check_bits("low nibble", kBody, 2, 0, 4, 0x0Bu);
    check_bits("high nibble", kBody, 2, 4, 4, 0x0Au);
    // bits 6..9 跨字节：byte0 bit6=0 / bit7=1，byte1 bit0=1 / bit1=0 → 0b0110
    check_bits("cross-byte 4 bits", kBody, 2, 6, 4, 0x6u);
    check_bits("out of range", kBody, 2, 12, 8, 0x0u);

    // ---- hid_pick_report ----
    printf("\n=== hid_pick_report ===\n");
    // 描述符 1：expected_bytes[1] = 7
    static const uint8_t kPkt1A[8] = { 0x01, 0x80, 0x80, 0x80, 0x80,
                                       0x08, 0x00, 0x00 };
    check_pick("map1 len8 id=1 exact (A)", kMap1, (uint16_t)sizeof(kMap1),
               kPkt1A, 8, 1, 1, 7, &kPkt1A[1]);
    // 【新增用例 1】尾部多 2 字节（厂商字节/时间戳）：期望宽松命中 ret=2。
    // expected_bytes[1] = 7，报文 9 字节：候选 A 剥掉 ID 后剩 8，多出 1 字节
    // → 落在 2 字节容差内 → ret=2，body_len=8。
    static const uint8_t kPkt1C[9] = { 0x01, 0x80, 0x80, 0x80, 0x80,
                                       0x08, 0x00, 0x00, 0x5A };
    check_pick("map1 len9 +2 trailing (A)", kMap1, (uint16_t)sizeof(kMap1),
               kPkt1C, 9, 2, 1, 8, &kPkt1C[1]);
    // 描述符 1，HOGP 规范做法：payload 不带 ID，长度正好 7
    static const uint8_t kPkt1B[7] = { 0x80, 0x80, 0x80, 0x80, 0x08,
                                       0x00, 0x00 };
    check_pick("map1 len7 no id (B)", kMap1, (uint16_t)sizeof(kMap1),
               kPkt1B, 7, 1, 1, 7, kPkt1B);
    // 描述符 2：不带 ID，expected_bytes[0] = 4
    static const uint8_t kPkt2A[4] = { 0x80, 0x80, 0x08, 0x00 };
    check_pick("map2 len4 no id", kMap2, (uint16_t)sizeof(kMap2),
               kPkt2A, 4, 1, 0, 4, kPkt2A);
    static const uint8_t kPkt2B[2] = { 0x80, 0x80 };
    check_pick("map2 len2 too short", kMap2, (uint16_t)sizeof(kMap2),
               kPkt2B, 2, 0, 0, 0, NULL);
    // 描述符 3：expected_bytes[5] = 5
    static const uint8_t kPkt3[6] = { 0x05, 0x80, 0x80, 0xF8, 0x80, 0x80 };
    check_pick("map3 len6 id=5 (A)", kMap3, (uint16_t)sizeof(kMap3),
               kPkt3, 6, 1, 5, 5, &kPkt3[1]);
    // 描述符 4：expected_bytes[3] = 5
    static const uint8_t kPkt4[6] = { 0x03, 0x80, 0x80, 0xF8, 0x00, 0x00 };
    check_pick("map4 len6 id=3 (A)", kMap4, (uint16_t)sizeof(kMap4),
               kPkt4, 6, 1, 3, 5, &kPkt4[1]);

    // 【新增用例 2】只有轴、没有按钮和 hat 的 report。
    //   - 描述符 5（X/Y 轴）：能选中，但轴**能**驱动方向键 → usable=1，
    //     描述符路径应当继续接管，不能降级。
    //   - 描述符 6（只有 Z 轴）：能选中，但 Z 无法映射成方向键 → usable=0，
    //     上层必须退回启发式，否则就是"永远吐 0 键"的死局。
    static const uint8_t kPkt5[3] = { 0x07, 0x80, 0x80 };
    check_pick("map5 X/Y axis only", kMap5, (uint16_t)sizeof(kMap5),
               kPkt5, 3, 1, 7, 2, &kPkt5[1]);
    static const uint8_t kPkt6[3] = { 0x08, 0x80, 0x80 };
    check_pick("map6 Z axis only", kMap6, (uint16_t)sizeof(kMap6),
               kPkt6, 3, 1, 8, 2, &kPkt6[1]);

    printf("\n=== hid_report_usable_fields ===\n");
    check_usable("map1 (16btn+hat+axis)", kMap1, (uint16_t)sizeof(kMap1), 1, 3);
    check_usable("map2 (no id)", kMap2, (uint16_t)sizeof(kMap2), 0, 3);
    check_usable("map4 (2 const padding)", kMap4, (uint16_t)sizeof(kMap4), 3, 3);
    check_usable("map5 X/Y -> dpad works", kMap5, (uint16_t)sizeof(kMap5), 7, 1);
    check_usable("map6 Z only -> no dpad", kMap6, (uint16_t)sizeof(kMap6), 8, 0);
    check_usable("map1 wrong rid", kMap1, (uint16_t)sizeof(kMap1), 99, 0);

    // 空指针 / 无效输入
    {
        hid_map_info_t info;
        memset(&info, 0, sizeof(info));
        uint8_t rid = 0; const uint8_t *body = NULL; uint16_t bl = 0;
        if (hid_pick_report(&info, kPkt4, 6, &rid, &body, &bl) != 0) {
            printf("  FAIL: invalid info should return 0\n");
            ++g_fail;
        } else {
            printf("  pick invalid info                 -> 0 (ok)\n");
        }
    }

    // ---- 参数校验 ----
    printf("\n=== argument guards ===\n");
    {
        hid_map_info_t info;
        if (hid_parse_report_map(NULL, 10, &info)) {
            printf("  FAIL: NULL map should return false\n");
            ++g_fail;
        } else {
            printf("  NULL map                        -> false (ok)\n");
        }
        if (hid_parse_report_map(kMap1, 0, &info)) {
            printf("  FAIL: zero len should return false\n");
            ++g_fail;
        } else {
            printf("  zero len                        -> false (ok)\n");
        }
    }

    // ---- 第 7 组：Q36 实机描述符的专项断言 ----
    //
    // 这一组是"真机解码结果"的验收标准，不是通用规则测试：字段表必须与我
    // 人工解码的那 7 个字段完全一致，任何一处不符都要**显式指出**。
    printf("\n=== map7 (Q36 for Android) hid_field_at_byte ===\n");
    // byte0..3 = 四轴（X/Y/Z/Rz）
    check_field_at_byte("map7 rid4 byte0", kMap7, (uint16_t)sizeof(kMap7),
                        4, 0, HID_FIELD_AXIS);
    check_field_at_byte("map7 rid4 byte3", kMap7, (uint16_t)sizeof(kMap7),
                        4, 3, HID_FIELD_AXIS);
    // byte4 低 4 位 = Hat、高 4 位 = 填充 → Hat 必须赢（填充位不能赢，否则
    // 方向键那半个字节就废了）
    check_field_at_byte("map7 rid4 byte4", kMap7, (uint16_t)sizeof(kMap7),
                        4, 4, HID_FIELD_HAT);
    // byte5 = 按钮 1..8，byte6 = 按钮 9..16
    check_field_at_byte("map7 rid4 byte5", kMap7, (uint16_t)sizeof(kMap7),
                        4, 5, HID_FIELD_BUTTON);
    check_field_at_byte("map7 rid4 byte6", kMap7, (uint16_t)sizeof(kMap7),
                        4, 6, HID_FIELD_BUTTON);
    // byte7/8 = 油门/刹车（Simulation 页）
    check_field_at_byte("map7 rid4 byte7", kMap7, (uint16_t)sizeof(kMap7),
                        4, 7, HID_FIELD_OTHER);
    // byte9 = 8 位填充
    check_field_at_byte("map7 rid4 byte9", kMap7, (uint16_t)sizeof(kMap7),
                        4, 9, HID_FIELD_CONST);
    // 越界 → NULL
    check_field_at_byte("map7 rid4 byte99", kMap7, (uint16_t)sizeof(kMap7),
                        4, 99, -1);
    // rid=3 的 Consumer Control 只有一个字段，byte2 起就没有了
    check_field_at_byte("map7 rid3 byte0", kMap7, (uint16_t)sizeof(kMap7),
                        3, 0, HID_FIELD_OTHER);
    check_field_at_byte("map7 rid3 byte9", kMap7, (uint16_t)sizeof(kMap7),
                        3, 9, -1);

    printf("\n=== map7 hid_field_at_raw_byte (原始报文下标) ===\n");
    // 绑定表记的是原始报文下标，通知带不带 ID 未知 → 两种解释都试。
    // 这台设备实际不带 ID，所以 raw==body。
    check_raw_byte("map7 raw0 -> axis", kMap7, (uint16_t)sizeof(kMap7),
                   0, HID_FIELD_AXIS);
    check_raw_byte("map7 raw5 -> button", kMap7, (uint16_t)sizeof(kMap7),
                   5, HID_FIELD_BUTTON);
    check_raw_byte("map7 raw6 -> button", kMap7, (uint16_t)sizeof(kMap7),
                   6, HID_FIELD_BUTTON);
    check_raw_byte("map7 raw99 -> NULL", kMap7, (uint16_t)sizeof(kMap7),
                   99, -1);

    printf("\n=== map7 hid_pick_report (10 / 11 字节) ===\n");
    // 10 字节且首字节不是 3/4 → 候选 A 落空，候选 B 精确命中 rid=4，ret=1。
    static const uint8_t kPkt7B[10] = { 0x80, 0x80, 0x80, 0x80, 0x08,
                                        0x00, 0x00, 0x00, 0x00, 0x00 };
    check_pick("map7 len10 no id (B)", kMap7, (uint16_t)sizeof(kMap7),
               kPkt7B, 10, 1, 4, 10, kPkt7B);
    // 11 字节且 data[0] == 4 → 候选 A 精确命中 rid=4，body_len=10。
    static const uint8_t kPkt7A[11] = { 0x04, 0x80, 0x80, 0x80, 0x80, 0x08,
                                        0x00, 0x00, 0x00, 0x00, 0x00 };
    check_pick("map7 len11 id=4 (A)", kMap7, (uint16_t)sizeof(kMap7),
               kPkt7A, 11, 1, 4, 10, &kPkt7A[1]);

    printf("\n=== map7 axis usage_list ===\n");
    {
        hid_map_info_t info;
        if (!hid_parse_report_map(kMap7, (uint16_t)sizeof(kMap7), &info)) {
            printf("  FAIL: map7 parse failed\n");
            ++g_fail;
        } else {
            // fields[1] 是 rid=4 的第一个字段（四轴），见 kExpect7。
            const hid_field_t *ax = &info.fields[1];
            static const uint16_t want[4] = { 0x30, 0x31, 0x32, 0x35 };
            printf("  axis field usage_count=%u (want 4)\n",
                   (unsigned)ax->usage_count);
            if (ax->usage_count != 4u) {
                printf("  FAIL: usage_count mismatch\n");
                ++g_fail;
            } else {
                for (uint8_t q = 0; q < 4u; ++q) {
                    printf("    usage_list[%u] = 0x%02X (want 0x%02X)\n",
                           (unsigned)q, (unsigned)ax->usage_list[q],
                           (unsigned)want[q]);
                    if (ax->usage_list[q] != want[q]) {
                        printf("  FAIL: usage_list[%u] mismatch\n", (unsigned)q);
                        ++g_fail;
                    }
                }
                // 反查：Y(0x31) 必须是下标 1，Rz(0x35) 必须是下标 3。
                // 上层靠这个下标决定"第几个字节是 Y"。
                int iy = -1;
                int irz = -1;
                for (uint8_t q = 0; q < ax->usage_count; ++q) {
                    if (ax->usage_list[q] == 0x31u) { iy = (int)q; }
                    if (ax->usage_list[q] == 0x35u) { irz = (int)q; }
                }
                printf("  index of Y=0x31 -> %d (want 1), "
                       "index of Rz=0x35 -> %d (want 3)\n", iy, irz);
                if (iy != 1 || irz != 3) {
                    printf("  FAIL: usage index lookup mismatch\n");
                    ++g_fail;
                }
            }
        }
    }

    printf("\nhid selftest %s (failures=%d)\n",
           (g_fail == 0) ? "PASS" : "FAIL", g_fail);
    return (g_fail == 0) ? 0 : 1;
}

#endif  // HID_SELFTEST
