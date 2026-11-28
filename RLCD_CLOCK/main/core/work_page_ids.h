// 统一定义跨 UI、输入、网络和存储使用的稳定工作页编号。
#pragma once

inline constexpr int kWorkPageWeatherClock = 0;
inline constexpr int kWorkPageGameBoy = 1;
inline constexpr int kWorkPageCount = 2;

// 显示设置中的页面开关与工作页编号保持一一对应。
inline constexpr int kDisplaySettingsPageItemCount = kWorkPageCount;

constexpr bool is_valid_work_page_id(int page)
{
    return page >= kWorkPageWeatherClock && page < kWorkPageCount;
}

static_assert(kWorkPageWeatherClock == 0,
              "work page ids must start at weather clock zero");
static_assert(kWorkPageCount == kWorkPageGameBoy + 1,
              "work page count must match the last work page id");
static_assert(kDisplaySettingsPageItemCount == kWorkPageCount,
              "display page setting count must match work page count");
static_assert(is_valid_work_page_id(kWorkPageWeatherClock),
              "first work page id must be valid");
static_assert(is_valid_work_page_id(kWorkPageGameBoy),
              "game page id must be valid");
static_assert(!is_valid_work_page_id(-1),
              "negative work page ids must be rejected");
static_assert(!is_valid_work_page_id(kWorkPageCount),
              "work page count is outside the valid id range");
