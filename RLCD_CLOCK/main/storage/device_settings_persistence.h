// 声明声音、工作页和图库周期设置的持久化入口。
#pragma once

#include <stddef.h>
#include <stdint.h>

struct ChimeRuntimeSnapshot;

bool save_hourly_chime_setting();
bool set_chime_setting(const ChimeRuntimeSnapshot &settings);
bool set_work_page_enabled_mask_setting(uint8_t page_mask);
bool set_work_page_order_setting(const uint8_t *page_order,
                                 size_t page_order_size);
bool set_gallery_rotation_period_setting(uint8_t period);

// 把「游戏页」一次性补进用户的页面启用配置。
//
// 背景：第 6 页在老固件里是小智 AI 那一页，这次被改成了 Game Boy。
// 用户机器里存下来的掩码是老固件写的（实测 0x38，只有第 3/4/5 位），
// 第 6 位是 0，所以无论怎么按 BOOT 都翻不到游戏页。
// V4 那套迁移会补第 6 位，V5/V6 不会 —— 这是迁移规则的缺口。
//
// 只做一次，做完在 NVS 里记一个标记。这个标记不能省：
// 没有它的话，用户以后在设置里关掉游戏页，下次开机又会被重新打开，
// 设置等于失效。
bool ensure_game_page_enabled_once();
