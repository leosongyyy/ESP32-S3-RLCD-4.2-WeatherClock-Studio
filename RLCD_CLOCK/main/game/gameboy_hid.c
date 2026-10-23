// gameboy_hid.c —— HID 报告描述符（Report Map）解析器实现
//
// 规格依据：HID 1.11 §6.2.2（Report Descriptor / Short Items）。
//
// 三条最容易写错、写错后症状最难查的规则，改之前先读：
//
//  1. 短条目的位布局是 bTag[7:4] / bType[3:2] / bSize[1:0]，**数据紧跟在前缀
//     字节之后，没有独立的 tag 字节**。bSize 的编码是 0/1/2/3 → 0/1/2/4 字节
//     （注意 2 代表 4 字节，不是 2 字节）。按「第二字节是 tag」去写的话整个
//     数据流会错位一个字节，解析出来全是垃圾，而且很难从日志上看出来。
//  2. Local 项（Usage / Usage Min / Usage Max）的作用域**只到下一个 Main 项**。
//     每个 Main 项之后必须清空。漏了这一步，前一个字段的 usage 会串到后一个
//     字段上 —— 现场表现是「明明是填充位却被解析成了按钮」，也就是幽灵按键
//     的另一条成因。
//  3. Logical Min / Logical Max 必须按**有符号**解读。大量手柄把轴的范围写成
//     -127..127（0x81 / 0x7F），按无符号读会得到 129 / 127，中点与死区全错，
//     于是摇杆在中位就被判成「往一个方向推到底」。
//
// 另外 Push / Pop 必须实现：手柄描述符里用 Push 临时改 Report Size/Count 再
// Pop 恢复的写法非常普遍，不实现的话 Pop 之后的全局状态是脏的，后面所有字段
// 的位偏移全错。
//
// 纯 C、无动态分配、不依赖 ESP-IDF（要能在宿主机上用 gcc 跑 hid_selftest.c）。

#include "gameboy_hid.h"

#include <string.h>

// Push / Pop 的栈深。HID 1.11 没规定上限，实际描述符嵌套很少超过 2 层，8 足够。
#define HID_PUSH_DEPTH 8

// 一份描述符里最多可能出现的 report ID 个数。字段数被 HID_MAX_FIELDS 卡住，
// 所以不同 report ID 的个数不可能超过它。
#define HID_MAX_REPORTS HID_MAX_FIELDS

// 通知报文尾部允许多出来的字节数（厂商字节 / 时间戳 / 对齐填充）。
//
// ⚠ 为什么不能要求严格相等：真实设备经常在尾部多挂 1~2 字节。原先
//   `expected == len` 的严格判据会让这类设备两个候选一起落空、直接掉进"猜"的
//   兜底分支 —— 那是"描述符优先却变成完全没反应"的成因之一。
// ⚠ 上限取 2 而不是更大：容差越大，越可能把"另一个 report"误当成"本 report 加
//   了填充"。2 字节足以覆盖实测见到的厂商字节与时间戳，又不至于失去区分度。
#define HID_PICK_EXTRA_TOLERANCE 2

// ---------------------------------------------------------------- 内部工具

// 全局项（Global）的状态。Push / Pop 保存与恢复的就是这一组。
//
// ⚠ Usage Page 是 Global 项而不是 Local 项：描述符里 0x05,0x09 之后的所有字段
//   都属于 Button 页，直到下一个 Usage Page 出现。把它当成 Local 每个 Main 项
//   清一次的话，按钮字段会被误判成轴或 hat。
typedef struct {
    uint16_t usage_page;
    uint16_t report_size;
    uint16_t report_count;
    uint8_t  report_id;
    int32_t  logical_min;
    int32_t  logical_max;
} hid_globals_t;

// 按 size 编码（0/1/2/3 → 0/1/2/4 字节）小端读取无符号值。
static uint32_t hid_item_u32(const uint8_t *p, uint8_t size_code)
{
    switch (size_code) {
    case 0:
        return 0u;                      // 无数据项，隐含值 0
    case 1:
        return (uint32_t)p[0];
    case 2:
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    case 3:
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    default:
        return 0u;
    }
}

// 按 size 编码读取**有符号**值。只有 Logical Min / Logical Max 需要走这条路径。
static int32_t hid_item_s32(const uint8_t *p, uint8_t size_code)
{
    const uint32_t u = hid_item_u32(p, size_code);
    switch (size_code) {
    case 1:
        return (int32_t)(int8_t)(uint8_t)u;     // 0x81 → -127，不是 129
    case 2:
        return (int32_t)(int16_t)(uint16_t)u;
    case 3:
        return (int32_t)u;
    default:
        return 0;
    }
}

// 判定字段种类。判定顺序不能改：Constant 位优先，否则填充位会被当成按钮读，
// 而填充位的值通常恰好是 0 或 1 —— 那就是「没按键却一直触发」的直接来源。
static uint8_t hid_classify(uint16_t usage_page, uint16_t usage_min,
                            uint8_t main_flags)
{
    if ((main_flags & 0x01u) != 0u) {
        return (uint8_t)HID_FIELD_CONST;
    }
    if (usage_page == 0x09u) {
        return (uint8_t)HID_FIELD_BUTTON;
    }
    if (usage_page == 0x01u) {
        if (usage_min == 0x39u) {
            return (uint8_t)HID_FIELD_HAT;
        }
        if (usage_min >= 0x30u && usage_min <= 0x35u) {
            return (uint8_t)HID_FIELD_AXIS;     // X/Y/Z/Rx/Ry/Rz
        }
    }
    return (uint8_t)HID_FIELD_OTHER;
}

// ---------------------------------------------------------------- 描述符解析

bool hid_parse_report_map(const uint8_t *map, uint16_t len, hid_map_info_t *out)
{
    if (map == NULL || out == NULL || len == 0u) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    hid_globals_t g;
    memset(&g, 0, sizeof(g));

    hid_globals_t stack[HID_PUSH_DEPTH];
    int sp = 0;

    // 局部项（Local）：Usage / Usage Min / Usage Max。作用域只到下一个 Main 项。
    uint16_t l_min = 0;
    uint16_t l_max = 0;
    bool     l_seen = false;
    // 逐个列出的 Usage（`Usage(X) Usage(Y) Usage(Z) Usage(Rz)` 这种写法）。
    // 与 l_min/l_max 是两套互补的记录：范围用法只有 min/max，逐个用法才有这张表。
    uint16_t l_usage[HID_MAX_USAGE_LIST];
    uint8_t  l_usage_n = 0;

    // 当前 report 的位偏移。⚠ 这里是相对**数据体**的偏移，不含 report ID 那
    // 8 位 —— 上层拿到的 body 也是剥掉 ID 之后的，两边必须对齐，否则整场会话
    // 偏移都错 1 字节。
    uint32_t bit_offset = 0;

    uint16_t i = 0;
    while (i < len) {
        const uint8_t b0 = map[i++];

        // 长条目：0xFE, data_size, tag, data...。手柄里几乎见不到，直接跳过，
        // 但长度必须算对，否则后面的短条目会从数据中间开始解析。
        if (b0 == 0xFEu) {
            if ((uint32_t)i + 2u > (uint32_t)len) {
                break;
            }
            const uint32_t data_size = (uint32_t)map[i];
            i = (uint16_t)(i + 2u);             // data_size 字节 + tag 字节
            if ((uint32_t)i + data_size > (uint32_t)len) {
                break;
            }
            i = (uint16_t)(i + data_size);
            continue;
        }

        const uint8_t size_code  = (uint8_t)(b0 & 0x03u);
        const uint8_t item_type  = (uint8_t)((b0 >> 2) & 0x03u);
        const uint8_t item_tag   = (uint8_t)((b0 >> 4) & 0x0Fu);
        const uint8_t data_bytes = (size_code == 3u) ? 4u : size_code;

        if ((uint32_t)i + (uint32_t)data_bytes > (uint32_t)len) {
            break;                              // 描述符被截断了，剩下的不要解析
        }
        const uint8_t *p = &map[i];
        i = (uint16_t)(i + data_bytes);

        if (item_type == 1u) {
            // ---- Global ----
            switch (item_tag) {
            case 0u:    // Usage Page
                g.usage_page = (uint16_t)hid_item_u32(p, size_code);
                break;
            case 1u:    // Logical Minimum（有符号）
                g.logical_min = hid_item_s32(p, size_code);
                break;
            case 2u:    // Logical Maximum（有符号）
                g.logical_max = hid_item_s32(p, size_code);
                break;
            case 7u:    // Report Size
                g.report_size = (uint16_t)hid_item_u32(p, size_code);
                break;
            case 8u:    // Report ID
                g.report_id = (uint8_t)hid_item_u32(p, size_code);
                out->has_report_id = true;
                // 每个 report ID 的位偏移独立从 0 开始。不清零的话第二个
                // report 的字段偏移会累加上一个 report 的总长，全错位。
                bit_offset = 0;
                break;
            case 9u:    // Report Count
                g.report_count = (uint16_t)hid_item_u32(p, size_code);
                break;
            case 10u:   // Push
                if (sp < HID_PUSH_DEPTH) {
                    stack[sp++] = g;
                }
                break;
            case 11u:   // Pop
                if (sp > 0) {
                    g = stack[--sp];
                }
                break;
            default:
                // 3/4（Physical Min/Max）、5（Unit Exponent）、6（Unit）：
                // 接收但忽略，它们不影响位布局。
                break;
            }
            continue;
        }

        if (item_type == 2u) {
            // ---- Local ----
            if (item_tag == 0u) {
                // Usage。size==4 时是 32 位扩展用法（高 16 位是 page），
                // 取低 16 位即可 —— 我们只关心用法号本身。
                //
                // 连续多个 Usage（如 X/Y/Z/Rz）出现在同一个 Main 项之前时，
                // 记成「第一个..最后一个」：这样 count=4 的四轴字段能同时体现
                // 0x30..0x35，上层按 usage_min 归类仍然正确。
                const uint16_t u = (uint16_t)(hid_item_u32(p, size_code) & 0xFFFFu);
                // ⚠ 逐个 Usage 必须**按出现顺序**记进 usage_list：四轴字段的
                //   第 0 个是 X、第 1 个是 Y。上层靠下标区分"第几个字节是 Y"。
                if (l_usage_n < (uint8_t)HID_MAX_USAGE_LIST) {
                    l_usage[l_usage_n++] = u;
                }
                if (!l_seen) {
                    l_min = u;
                    l_max = u;
                    l_seen = true;
                } else {
                    l_max = u;
                }
            } else if (item_tag == 1u) {
                const uint16_t v = (uint16_t)(hid_item_u32(p, size_code) & 0xFFFFu);
                l_min = v;
                if (!l_seen) {
                    l_max = v;
                }
                l_seen = true;
            } else if (item_tag == 2u) {
                const uint16_t v = (uint16_t)(hid_item_u32(p, size_code) & 0xFFFFu);
                l_max = v;
                if (!l_seen) {
                    l_min = v;
                }
                l_seen = true;
            }
            continue;
        }

        if (item_type == 0u) {
            // ---- Main ----
            if (item_tag == 8u) {
                // Input。只有它声明 input report 的字段布局。
                const uint8_t  main_flags = (uint8_t)hid_item_u32(p, size_code);
                const uint16_t bit_size   = g.report_size;
                const uint16_t count      = g.report_count;

                // ⚠ Constant 字段（bit0=1）**仍然要记录**：它占的位必须被算进
                // 偏移，上层才能正确跳过填充位。只是 kind 标成 CONST，让上层
                // 明确知道「这些位不是按键」。
                if (bit_size != 0u && count != 0u &&
                    out->field_count < (uint16_t)HID_MAX_FIELDS) {
                    hid_field_t *f = &out->fields[out->field_count++];
                    f->report_id   = out->has_report_id ? g.report_id : (uint8_t)0;
                    f->kind        = hid_classify(g.usage_page, l_min, main_flags);
                    f->main_flags  = main_flags;
                    f->bit_offset  = (bit_offset > 0xFFFFu) ? (uint16_t)0xFFFFu
                                                            : (uint16_t)bit_offset;
                    f->bit_size    = bit_size;
                    f->count       = count;
                    f->usage_page  = g.usage_page;
                    f->usage_min   = l_min;
                    f->usage_max   = l_max;
                    // 逐个 Usage 的完整列表（可能比 usage_min..usage_max 精确：
                    // 0x30..0x35 的范围看不出中间有没有 0x33/0x34）。
                    {
                        const uint8_t n =
                            (l_usage_n > (uint8_t)HID_MAX_USAGE_LIST)
                                ? (uint8_t)HID_MAX_USAGE_LIST : l_usage_n;
                        for (uint8_t q = 0; q < n; ++q) {
                            f->usage_list[q] = l_usage[q];
                        }
                        f->usage_count = n;
                    }
                    f->logical_min = g.logical_min;
                    f->logical_max = g.logical_max;
                }
                bit_offset += (uint32_t)bit_size * (uint32_t)count;
            }
            // ⚠ Local 项在每个 Main 项之后必须清空 —— 包括 Output / Feature /
            // Collection / End Collection。只清 Input 是不够的：Collection 与
            // End Collection 同样会终结 Local 作用域。
            l_min  = 0;
            l_max  = 0;
            l_seen = false;
            l_usage_n = 0;
            continue;
        }

        // type == 3（Reserved）：忽略。
    }

    out->valid = (out->field_count > 0u);
    return out->valid;
}

// ---------------------------------------------------------------- 位提取

uint32_t hid_extract_bits(const uint8_t *body, uint16_t body_len,
                          uint16_t bit_offset, uint16_t bit_size)
{
    if (body == NULL || bit_size == 0u || bit_size > 32u) {
        return 0u;
    }
    if ((uint32_t)bit_offset + (uint32_t)bit_size > (uint32_t)body_len * 8u) {
        return 0u;                      // 越界：宁可返回 0 也不要读越界内存
    }

    uint32_t v = 0u;
    for (uint16_t k = 0; k < bit_size; ++k) {
        const uint32_t bit_index = (uint32_t)bit_offset + (uint32_t)k;
        const uint32_t byte_index = bit_index >> 3;
        const uint32_t bit_in_byte = bit_index & 7u;
        const uint32_t bit = (uint32_t)((body[byte_index] >> bit_in_byte) & 1u);
        v |= (bit << k);                // 字段自身的 bit0 = LSB，即 LSB 在前
    }
    return v;
}

// ---------------------------------------------------------------- 报表选择

// 内部用的「每个 report 的汇总」。用紧凑表而不是 256 项数组：这是在 BLE host
// 任务栈上调用的，1KB 的栈数组太奢侈。
typedef struct {
    uint8_t  id;
    bool     used;
    uint32_t total_bits;
    uint32_t button_bits;
} hid_report_sum_t;

static hid_report_sum_t *hid_sum_find(hid_report_sum_t *list, uint16_t *n,
                                      uint8_t id)
{
    for (uint16_t k = 0; k < *n; ++k) {
        if (list[k].used && list[k].id == id) {
            return &list[k];
        }
    }
    if (*n >= (uint16_t)HID_MAX_REPORTS) {
        return NULL;                    // 理论上到不了：字段数已被 HID_MAX_FIELDS 卡住
    }
    hid_report_sum_t *s = &list[*n];
    s->id = id;
    s->used = true;
    s->total_bits = 0;
    s->button_bits = 0;
    ++(*n);
    return s;
}

int hid_pick_report(const hid_map_info_t *info,
                    const uint8_t *data, uint16_t len,
                    uint8_t *out_report_id,
                    const uint8_t **out_body, uint16_t *out_body_len)
{
    if (info == NULL || !info->valid || data == NULL || len == 0u) {
        return 0;
    }
    if (out_report_id == NULL || out_body == NULL || out_body_len == NULL) {
        return 0;
    }

    hid_report_sum_t list[HID_MAX_REPORTS];
    memset(list, 0, sizeof(list));
    uint16_t n = 0;

    // 1) 统计每个 report 的 input 字段总位数与按钮位数。
    //    ⚠ 包含 CONST 字段：填充位也实实在在占空间，漏算会让 expected_bytes
    //    偏小，长度匹配永远对不上。
    for (uint16_t k = 0; k < info->field_count && k < (uint16_t)HID_MAX_FIELDS;
         ++k) {
        const hid_field_t *f = &info->fields[k];
        hid_report_sum_t *s = hid_sum_find(list, &n, f->report_id);
        if (s == NULL) {
            break;
        }
        const uint32_t bits = (uint32_t)f->bit_size * (uint32_t)f->count;
        s->total_bits += bits;
        if (f->kind == (uint8_t)HID_FIELD_BUTTON) {
            s->button_bits += bits;
        }
    }
    if (n == 0u) {
        return 0;                       // 描述符里一个 input 字段都没有
    }

    // 2) 不带 report ID：整包就是数据体。
    if (!info->has_report_id) {
        *out_report_id = 0u;
        *out_body = data;
        *out_body_len = len;
        const uint32_t need = (list[0].total_bits + 7u) / 8u;
        if ((uint32_t)len < need) {
            return 0;                   // 比描述符声明的还短，没法解析
        }
        return ((uint32_t)len == need) ? 1 : 2;
    }

    // 3a) 候选 A「payload 里带 ID」。
    //
    // ⚠ 允许尾部多余 HID_PICK_EXTRA_TOLERANCE 字节，不再要求严格相等。
    // ⚠ A 优先于 B 在这里**同时**等价于"优先更紧的那个"：extra_A = extra_B - 1，
    //   所以只要 A 能命中，它一定比同 report 的 B 更紧（差值更小）。不会出现
    //   "A 宽松却压掉了 B 的精确"这种情况 —— 那要求 extra_B == 0 且
    //   extra_A == -1，而 -1 意味着 A 根本不命中。
    if (len >= 1u) {
        for (uint16_t k = 0; k < n; ++k) {
            if (!list[k].used || list[k].id != data[0]) {
                continue;
            }
            const uint32_t need = (list[k].total_bits + 7u) / 8u;
            const uint32_t avail = (uint32_t)(len - 1u);
            if (need > avail) {
                continue;
            }
            const uint32_t extra = avail - need;
            if (extra > (uint32_t)HID_PICK_EXTRA_TOLERANCE) {
                continue;               // 多得太离谱，按没命中处理
            }
            *out_report_id = list[k].id;
            *out_body = data + 1;
            *out_body_len = (uint16_t)(len - 1u);
            return (extra == 0u) ? 1 : 2;
        }
    }

    // 3b) 候选 B「payload 不带 ID」：HOGP 规范的做法 —— 通知里不带 ID，ID 由
    //     Report Reference 描述符说明。
    //
    // 多个 report 都落在容差内时取**差值最小**的那个（差值相同取 ID 小的），
    // 保证结果确定 —— 原先要求"唯一吻合"会因为两个 report 长度接近而直接判
    // 失败，白扔掉一个明明能用的匹配。
    {
        int      best = -1;
        uint32_t best_extra = 0;
        for (uint16_t k = 0; k < n; ++k) {
            if (!list[k].used) {
                continue;
            }
            const uint32_t need = (list[k].total_bits + 7u) / 8u;
            if (need > (uint32_t)len) {
                continue;
            }
            const uint32_t extra = (uint32_t)len - need;
            if (extra > (uint32_t)HID_PICK_EXTRA_TOLERANCE) {
                continue;
            }
            if (best < 0 || extra < best_extra ||
                (extra == best_extra && list[k].id < list[best].id)) {
                best = k;
                best_extra = extra;
            }
        }
        if (best >= 0) {
            *out_report_id = list[best].id;
            *out_body = data;
            *out_body_len = len;
            return (best_extra == 0u) ? 1 : 2;
        }
    }

    // 4) 兜底：选按钮位数最多的那个 report（并列取 ID 小的）。
    //    为什么还要兜底而不是直接返回 0：描述符与报文长度对不上的情况很常见
    //    （厂商多塞了几个字节、或把时间戳挂在后面），此时整场不解析的后果是
    //    手柄完全不能用，而解析得糙一点至少 A/B/方向键是准的。
    //    ⚠ 返回值标成 2（宽松）：这一帧是"猜"的，上层可以据此降级处理。
    {
        uint16_t best = 0;
        for (uint16_t k = 1; k < n; ++k) {
            if (!list[k].used) {
                continue;
            }
            if (list[k].button_bits > list[best].button_bits ||
                (list[k].button_bits == list[best].button_bits &&
                 list[k].id < list[best].id)) {
                best = k;
            }
        }
        const uint32_t need = (list[best].total_bits + 7u) / 8u;
        if ((uint32_t)len >= need + 1u) {
            *out_report_id = list[best].id;
            *out_body = data + 1;
            *out_body_len = (uint16_t)(len - 1u);
        } else {
            *out_report_id = list[best].id;
            *out_body = data;
            *out_body_len = len;
        }
        return 2;
    }
}

uint16_t hid_report_usable_fields(const hid_map_info_t *info, uint8_t report_id)
{
    if (info == NULL || !info->valid) {
        return 0u;
    }

    // ⚠ 轴只数"能驱动方向键"的那几个（X/Y = 0x30/0x31，或退而求其次的
    //   Rx/Ry = 0x33/0x34）。像 Z/Rz 这种轴，pad_apply_descriptor() 的方向键
    //   兜底根本用不上它 —— 只挂 Z/Rz 的 report 解析出来必然是全 0 键，
    //   那正是"描述符接管却永远吐 0"的死局，必须让位给启发式。
    //
    // ⚠ 按 usage **范围**判定：X/Y 常被合成一个 usage 0x30..0x31 的字段，
    //   只看 usage_min 会漏掉 Y。
    uint16_t n = 0;
    for (uint16_t k = 0; k < info->field_count && k < (uint16_t)HID_MAX_FIELDS;
         ++k) {
        const hid_field_t *f = &info->fields[k];
        if (f->report_id != report_id) {
            continue;
        }
        if ((f->main_flags & 0x01u) != 0u) {
            continue;                   // Constant 填充位，用不上
        }
        if (f->kind == (uint8_t)HID_FIELD_BUTTON ||
            f->kind == (uint8_t)HID_FIELD_HAT) {
            ++n;
            continue;
        }
        if (f->kind == (uint8_t)HID_FIELD_AXIS) {
            const uint16_t lo = f->usage_min;
            const uint16_t hi = (f->usage_max >= lo) ? f->usage_max : lo;
            if ((lo <= 0x30u && 0x30u <= hi) || (lo <= 0x31u && 0x31u <= hi) ||
                (lo <= 0x33u && 0x33u <= hi) || (lo <= 0x34u && 0x34u <= hi)) {
                ++n;
            }
        }
    }
    return n;
}

// ---------------------------------------------------------------- 字节 → 字段

// 字段种类的「可信度」排序，数字越小越可信（见 hid_field_at_byte 的说明）。
//
// CONST 排最后是有意的：一个字节常常是"低 4 位 Hat + 高 4 位填充"，若让填充
// 位赢，那半个字节的方向键就废了。反过来 BUTTON / HAT 必须赢过填充。
static int hid_kind_priority(uint8_t kind)
{
    switch (kind) {
    case (uint8_t)HID_FIELD_BUTTON: return 0;
    case (uint8_t)HID_FIELD_HAT:    return 1;
    case (uint8_t)HID_FIELD_AXIS:   return 2;
    case (uint8_t)HID_FIELD_OTHER:  return 3;
    default:                        return 4;   // HID_FIELD_CONST
    }
}

// 收集描述符里出现过的所有 report ID（去重，按出现顺序）。
static uint8_t hid_collect_rids(const hid_map_info_t *info,
                                uint8_t *out_ids, uint8_t max_ids)
{
    uint8_t n = 0;
    for (uint16_t k = 0; k < info->field_count && k < (uint16_t)HID_MAX_FIELDS;
         ++k) {
        const uint8_t rid = info->fields[k].report_id;
        bool seen = false;
        for (uint8_t j = 0; j < n; ++j) {
            if (out_ids[j] == rid) {
                seen = true;
                break;
            }
        }
        if (!seen && n < max_ids) {
            out_ids[n++] = rid;
        }
    }
    return n;
}

const hid_field_t *hid_field_at_byte(const hid_map_info_t *info,
                                     uint8_t report_id, uint16_t byte_index)
{
    if (info == NULL || !info->valid) {
        return NULL;
    }

    // 目标字节覆盖的位区间：[lo, hi]（闭区间，LSB 在前的位序不影响区间本身）。
    const uint32_t lo = (uint32_t)byte_index * 8u;
    const uint32_t hi = lo + 7u;

    const hid_field_t *best = NULL;
    int       best_prio = 99;
    uint32_t  best_overlap = 0;

    for (uint16_t k = 0; k < info->field_count && k < (uint16_t)HID_MAX_FIELDS;
         ++k) {
        const hid_field_t *f = &info->fields[k];
        if (f->report_id != report_id) {
            continue;
        }
        const uint32_t bits = (uint32_t)f->bit_size * (uint32_t)f->count;
        if (bits == 0u) {
            continue;
        }
        const uint32_t f_lo = (uint32_t)f->bit_offset;
        const uint32_t f_hi = f_lo + bits - 1u;
        if (f_hi < lo || f_lo > hi) {
            continue;                   // 与该字节没有交集
        }
        const uint32_t ovl = ((f_hi < hi) ? f_hi : hi) -
                             ((f_lo > lo) ? f_lo : lo) + 1u;
        const int prio = hid_kind_priority(f->kind);
        // 严格更优才替换：同类且交集相同时保留**靠前**的那个（字段表本身按
        // bit_offset 升序，于是等价于"取下标最小"）。
        if (best == NULL || prio < best_prio ||
            (prio == best_prio && ovl > best_overlap)) {
            best = f;
            best_prio = prio;
            best_overlap = ovl;
        }
    }
    return best;
}

const hid_field_t *hid_field_at_raw_byte(const hid_map_info_t *info,
                                         uint16_t raw_byte_index)
{
    if (info == NULL || !info->valid) {
        return NULL;
    }

    uint8_t rids[HID_MAX_FIELDS];
    const uint8_t nrid = hid_collect_rids(info, rids, (uint8_t)HID_MAX_FIELDS);

    const hid_field_t *best = NULL;
    int best_prio = 99;

    // 两种解释都试：t==0 通知不带 report ID，t==1 通知带 report ID。
    for (int t = 0; t < 2; ++t) {
        uint16_t body_idx;
        if (t == 0) {
            body_idx = raw_byte_index;
        } else {
            if (raw_byte_index == 0u) {
                // ⚠ 下溢防护：带 ID 的设备上，raw 的第 0 字节是 ID 本身，
                //   不存在对应的 body 下标。不挡住的话会变成 0xFFFF，
                //   等于"查最后一个字节"，结论完全不可信。
                continue;
            }
            body_idx = (uint16_t)(raw_byte_index - 1u);
        }
        for (uint8_t j = 0; j < nrid; ++j) {
            const hid_field_t *f = hid_field_at_byte(info, rids[j], body_idx);
            if (f == NULL) {
                continue;
            }
            const int prio = hid_kind_priority(f->kind);
            if (best == NULL || prio < best_prio) {
                best = f;
                best_prio = prio;
            }
        }
    }
    return best;
}
