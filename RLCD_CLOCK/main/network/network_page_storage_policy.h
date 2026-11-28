// 定义工作页 NVS 版本迁移使用的纯计算规则。
//
// 历史版本布局（按旧编号）：
//   v4: 0..5（天气时钟/图片时钟/天气看板/温湿时钟/日历/温湿历史）
//   v5/v6: 0..7（v4 + 游戏(6) + 聚合时钟(7)）
// 现行布局只有 2 页：0=天气时钟，1=游戏（旧 6）。
#pragma once

#include "work_page_ids.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace network_page_storage {
// 旧 v4 时代共有 6 个工作页（编号 0..5）。
inline constexpr size_t kLegacyV4WorkPageCount = 6;
inline constexpr int kLegacyGamePageId = 6;
inline constexpr uint8_t kLegacyV4KnownPageMask =
    static_cast<uint8_t>((1U << kLegacyV4WorkPageCount) - 1U);
inline constexpr uint8_t kLegacyV5KnownPageMask =
    static_cast<uint8_t>(kLegacyV4KnownPageMask |
                         (1U << kLegacyGamePageId) |
                         (1U << 7));
inline constexpr uint8_t kCurrentKnownPageMask =
    static_cast<uint8_t>((1U << kWorkPageCount) - 1U);

// 把旧 8 位布局的掩码映射到现行 2 页布局：保留天气时钟（旧 0）和游戏
// （旧 6 -> 新 1），其余已删除页面的开关位一律丢弃。
constexpr uint8_t remap_legacy_page_mask(uint8_t legacy_mask)
{
    uint8_t current_mask = static_cast<uint8_t>(legacy_mask & 0x01U);
    if ((legacy_mask & (1U << kLegacyGamePageId)) != 0) {
        current_mask = static_cast<uint8_t>(current_mask |
                                            (1U << kWorkPageGameBoy));
    }
    return current_mask;
}

constexpr uint8_t migrate_v4_page_mask(uint8_t legacy_mask)
{
    // v4 掩码不含游戏页（旧 6），迁移时视作启用。
    return remap_legacy_page_mask(static_cast<uint8_t>(
        legacy_mask | (1U << kLegacyGamePageId)));
}

inline bool migrate_v4_page_order(const uint8_t *legacy_order,
                                  size_t legacy_order_size,
                                  uint8_t *current_order,
                                  size_t current_order_size)
{
    if (!legacy_order || legacy_order_size != kLegacyV4WorkPageCount ||
        !current_order || current_order_size != kWorkPageCount) {
        return false;
    }
    // 现行顺序只有天气时钟和游戏两页；游戏页在旧顺序里排在最后。
    current_order[0] = kWorkPageWeatherClock;
    current_order[1] = kWorkPageGameBoy;
    return true;
}

static_assert(kWorkPageGameBoy == 1,
              "current layout expects the game page right after the weather clock");
static_assert(kCurrentKnownPageMask == 0x03,
              "current page mask must cover weather clock and game pages");
static_assert((kLegacyV4KnownPageMask & (1U << kLegacyGamePageId)) == 0,
              "v4 mask must not contain the legacy game page");
} // namespace network_page_storage
