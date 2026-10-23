// 游戏工作页：idle 态显示 LVGL ROM 列表，运行态把整屏交给模拟器任务。
//
// 线程模型：
//   - 本文件里的 LVGL 操作全部发生在 ui_task 内（build/update 由 ui_views 调用）。
//   - game_page_next_rom()/game_page_start_selected() 只改状态，供按键任务调用。
#include "ui_game.h"

#include "app_display_config.h"
#include "app_metadata.h"
#include "battery_runtime_state.h"
#include "game_audio.h"
#include "game_config.h"
#include "gameboy_pad.h"
#include "gameboy_rom_store.h"
#include "gameboy_runtime.h"
#include "ota_runtime_state.h"
#include "ui_battery.h"
#include "ui_fonts.h"
#include "ui_game_bind.h"
#include "ui_page_state.h"
#include "ui_widgets.h"
#include "ui_work_page_layout.h"
#include "ui_work_status.h"
#include "wifi_portal_state.h"
#include "work_page_ids.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr const char *kGameLogTag = "GamePage";
constexpr int kListX = 10;
constexpr int kListWidth = kDisplayWidth - 2 * kListX;
constexpr int kListTopY = 62;
constexpr int kRowHeight = 28;
constexpr int kRowPitch = 30;
// 4 行 ROM + 1 行音量条 = 5 个可选项。ROM 从原来的 5 行缩到 4 行，
// 腾出来的高度正好放音量行，屏幕总高度不变。
constexpr int kVisibleRows = 4;
constexpr int kHintPrimaryY = 216;
constexpr int kHintSecondaryY = 238;
constexpr int kHintStatusY = 260;
constexpr int kHintTertiaryY = 276;   // 276+20=296，留 4px 底边，别贴着屏幕边缘
constexpr int kHintHeight = 20;
constexpr int kHintX = 6;
constexpr int kHintWidth = kDisplayWidth - 2 * kHintX;
constexpr int kEmptyTitleY = 108;
constexpr int kEmptyTitleHeight = 40;
constexpr int kEmptyHintY = 158;
constexpr int kEmptyHintHeight = 26;
constexpr int kEmptyHintSecondY = 190;

// 音量行：紧跟 4 行 ROM（62..180）之后，赶在提示行（216）之前。
constexpr int kVolumeRowY = 186;
constexpr int kVolumeRowHeight = 24;
constexpr int kVolumeLabelWidth = 44;                 // "音量" 两字 32px + 余量
constexpr int kVolumeCellCount = 5;
constexpr int kVolumeCellWidth = 40;
constexpr int kVolumeCellGap = 4;
constexpr int kVolumeCellsX = kListX + kVolumeLabelWidth + 6;   // 60
constexpr int kVolumeValueWidth = 92;
constexpr int kVolumeValueX = kListX + kListWidth - kVolumeValueWidth;  // 298

constexpr const char *kHintPrimaryText = "BOOT 换项/长按翻页 游戏中A(+START)";
constexpr const char *kHintSecondaryText = "KEY 开始/音量加档/长按设置 游戏中B(+SELECT)";
// 暂停和退出是同一个手势的两个时长，必须写出来——否则用户永远不知道能暂停，
// 更不知道"要退出得再按住不放"。
constexpr const char *kHintTertiaryText = "游戏中 双键600ms暂停 按住2s退出";
// 空状态文案按 TF 卡语义写：ROM 只从 TF 卡的 rom/gb 目录读，不再走配网上传引导。
// 屏幕上这行下面还有一条状态行（rom_store_diagnostic()）给出具体原因
// （未检测到 TF 卡 / rom/gb 不可读 / rom/gb 无 ROM），所以这里只写"该做什么"。
constexpr const char *kEmptyTitleText = "请插入 TF 卡";
constexpr const char *kEmptyHintText = "把 .gb / .gbc 文件拷进 TF 卡";
constexpr const char *kEmptyHintSecondText = "根目录下的 rom/gb 文件夹";
constexpr const char *kRowLabelWarning = "game rom row label create failed";
constexpr const char *kHintLabelWarning = "game hint label create failed";
// 行文本宽度预算。create_row_objects() 里行标签宽度是 kListWidth - 16 = 364px，
// 这里再留 6px 余量（覆盖标签内边距与亚像素偏差），取 358px。
constexpr int kRowTextWidthPx = kListWidth - 16 - 6;
// 截断标记。刻意用三个 ASCII 点而不是 U+2026「…」：zh_font_16 的码点范围是
// 0x20-0x7E + 0x2103 + 0x3000-0x303F + 0x4E00-0x9FFF + 0xFF00-0xFFEF，
// 不含 0x2026，用了会静默 fallback 到 montserrat_14，宽度预算就对不上了。
constexpr const char *kRowTruncationMarker = "...";
// UI 数据流诊断日志用的 tag。和 RomStore（rom_store 模块）、GamePage（本文件
// 原有日志）分开，方便按 tag grep 定位是哪一层出的数据。
constexpr const char *kGameUiLogTag = "GameUI";
// 诊断日志打前几行的最终文本（交给 label 的字符串，format_row_text() 之后）。
constexpr int kGameDiagRows = 3;
static_assert(kGameDiagRows > 0 && kGameDiagRows <= kVisibleRows,
              "diagnostic rows must be a subset of the visible rows");

lv_obj_t *s_row_bars[kVisibleRows] = {};
lv_obj_t *s_row_labels[kVisibleRows] = {};
lv_obj_t *s_hint_primary = nullptr;
lv_obj_t *s_hint_secondary = nullptr;
lv_obj_t *s_hint_status = nullptr;
lv_obj_t *s_hint_tertiary = nullptr;
lv_obj_t *s_empty_title = nullptr;
lv_obj_t *s_empty_hint = nullptr;
lv_obj_t *s_empty_hint_second = nullptr;

// 音量行。s_volume_bar 是整行的焦点高亮底条，最先创建（在最下层），
// 格子和文字后创建，叠在它上面。
lv_obj_t *s_volume_bar = nullptr;
lv_obj_t *s_volume_label = nullptr;
lv_obj_t *s_volume_cells[kVolumeCellCount] = {};
lv_obj_t *s_volume_value = nullptr;
// 焦点是否在音量行上。焦点只有一处：为真时 ROM 行一律不高亮。
bool s_volume_focused = false;

int s_scroll_offset = 0;
bool s_was_running = false;

// 按键任务写入的一次性提示（启动失败原因等），UI 任务读取后显示。
portMUX_TYPE s_notice_mux = portMUX_INITIALIZER_UNLOCKED;
char s_notice[64] = "";

int s_rendered_selected = -1;
int s_rendered_count = -1;
char s_rendered_names[kVisibleRows][game_config::kRomDisplayNameBytes] = {};
char s_rendered_notice[64] = "";
// 音量行是纯样式变化（填充几格 / 整行反色），set_label_text_if_changed()
// 发现不了，只能自己缓存上一帧的档位与焦点来判 diff。
int s_rendered_volume_step = -1;
bool s_rendered_volume_focused = false;

static_assert(kVisibleRows > 0, "game page must render at least one ROM row");
static_assert(kListTopY + kVisibleRows * kRowPitch <= kHintPrimaryY,
              "ROM list must not overlap the hint lines");
static_assert(kVolumeCellCount >= 1, "volume row must render at least one cell");
static_assert(kVolumeRowY + kVolumeRowHeight <= kHintPrimaryY,
              "volume row must not overlap the hint lines");
static_assert(kVolumeCellsX + kVolumeCellCount * kVolumeCellWidth +
                      (kVolumeCellCount - 1) * kVolumeCellGap <=
                  kVolumeValueX,
              "volume cells must not overlap the value label");
static_assert(kVolumeValueX + kVolumeValueWidth <= kListX + kListWidth,
              "volume value label must stay inside the list width");
static_assert(kHintStatusY + kHintHeight <= kDisplayHeight,
              "game hint lines must stay inside the display");
static_assert(kEmptyHintSecondY + kEmptyHintHeight <= kDisplayHeight,
              "empty-state hints must stay inside the display");
static_assert(kHintPrimaryText[0] != '\0' && kHintSecondaryText[0] != '\0',
              "game hint texts must not be empty");
static_assert(kHintTertiaryText[0] != '\0',
              "game play-control hint must not be empty");
static_assert(kHintTertiaryY + kHintHeight <= kDisplayHeight,
              "game pause hint must stay inside the display");

void set_notice(const char *text)
{
    portENTER_CRITICAL(&s_notice_mux);
    snprintf(s_notice, sizeof(s_notice), "%s", text ? text : "");
    portEXIT_CRITICAL(&s_notice_mux);
}

void copy_notice(char *out, size_t out_bytes)
{
    if (!out || out_bytes == 0) {
        return;
    }
    portENTER_CRITICAL(&s_notice_mux);
    snprintf(out, out_bytes, "%s", s_notice);
    portEXIT_CRITICAL(&s_notice_mux);
}

// 行文本 = "N. " + 名字，超宽时按 UTF-8 字符边界截断并保留扩展名。
//
// FATFS 配成 CONFIG_FATFS_API_ENCODING_UTF_8 之后 rom_store_name() 直接给 UTF-8，
// LVGL 的中文字体也是按 UTF-8 索引的，所以直接拼，不需要任何转码。
// （曾经这里要调 gbk_to_utf8()，因为 API 编码是 ANSI/OEM/GBK；那套模块已删。）
//
// 为什么要截断：用户真实 ROM 名很长，比如
// 「口袋妖怪黄+(繁)+(修正版)+(yishiluo)(8Mb).gb」= 34 字符 52 字节，
// 在 zh_font_16 下实测渲染 362px，加上 "64. " 前缀 31px 一共 393px，
// 而行标签可用宽度只有 kListWidth-16 = 364px。行标签是
// LV_LABEL_LONG_CLIP（make_label_with_font 里设的），不主动截就会被硬切，
// 三字节汉字切一半会在屏幕上画出乱码方块。
//
// 宽度必须用 lv_txt_get_width() 实测，不能拍固定值：这个字体的 ASCII 实际
// 4~17px/字（'i' 4px、'M' 16px、'+' 11px），中文才是整齐的 16px/字。
// 拍一个「ASCII 8px」会把最长的名字算成 300px 出头，结果照样溢出。
size_t utf8_char_bytes(const char *p)
{
    const unsigned char c = (unsigned char)p[0];
    if (c < 0x80) return 1;              // 0xxxxxxx
    if ((c & 0xE0) == 0xC0) return 2;    // 110xxxxx
    if ((c & 0xF0) == 0xE0) return 3;    // 1110xxxx  ← 常用汉字在这
    if ((c & 0xF8) == 0xF0) return 4;    // 11110xxx
    return 1;  // 非法首字节：按 1 字节走，保证循环一定前进、不会死循环
}

// 测一段文本在 zh_font_16 下的渲染像素宽。用 LVGL 自己的度量函数，
// 保证和实际绘制用的是同一套字形数据。
int row_text_width_px(const char *text, size_t bytes)
{
    if (!text || bytes == 0) {
        return 0;
    }
    return lv_txt_get_width(text, (uint32_t)bytes, &zh_font_16, 0,
                            LV_TEXT_FLAG_NONE);
}

void format_row_text(char *out, size_t out_bytes, int index, const char *name)
{
    if (!out || out_bytes == 0) {
        return;
    }
    const char *safe = name ? name : "";

    char full[game_config::kRomDisplayNameBytes] = {};
    snprintf(full, sizeof(full), "%d. %s", index + 1, safe);

    const size_t full_len = strlen(full);
    if (row_text_width_px(full, full_len) <= kRowTextWidthPx) {
        snprintf(out, out_bytes, "%s", full);
        return;
    }

    // 超宽。保留扩展名（.gb/.gbc）让用户还认得出这是什么，
    // 中间部分按 UTF-8 字符边界截断，尾部加省略号。
    char prefix[16] = {};
    snprintf(prefix, sizeof(prefix), "%d. ", index + 1);

    // 取最后一个 '.' 之后的部分作扩展名；名字以 '.' 开头时不当扩展名处理。
    const char *dot = strrchr(safe, '.');
    const char *suffix = (dot && dot != safe) ? dot : "";
    const size_t body_len = (size_t)(suffix - safe);

    const int prefix_w = row_text_width_px(prefix, strlen(prefix));
    const int marker_w =
        row_text_width_px(kRowTruncationMarker, strlen(kRowTruncationMarker));
    const int suffix_w = row_text_width_px(suffix, strlen(suffix));

    const int avail = kRowTextWidthPx - prefix_w - marker_w - suffix_w;
    if (avail <= 0) {
        // 极端退化：连「前缀 + 省略号 + 后缀」都放不下（正常不会发生，
        // kRowTextWidthPx 有 358px）。此时只保前缀与后缀，至少不画乱码。
        snprintf(out, out_bytes, "%s%s", prefix, suffix);
        return;
    }

    // 逐字符累加，只在 UTF-8 字符边界停——utf8_char_bytes() 保证了这一点。
    size_t keep = 0;
    int used = 0;
    size_t i = 0;
    while (i < body_len) {
        const size_t clen = utf8_char_bytes(safe + i);
        const int cw = row_text_width_px(safe + i, clen);
        if (used + cw > avail) {
            break;
        }
        used += cw;
        i += clen;
        keep = i;
    }
    snprintf(out, out_bytes, "%s%.*s%s%s", prefix, (int)keep, safe,
             kRowTruncationMarker, suffix);
}

// 让选中项始终落在可见窗口内。
void ensure_scroll_window(int count, int selected)
{
    if (count <= kVisibleRows) {
        s_scroll_offset = 0;
        return;
    }
    if (selected < s_scroll_offset) {
        s_scroll_offset = selected;
    }
    if (selected >= s_scroll_offset + kVisibleRows) {
        s_scroll_offset = selected - kVisibleRows + 1;
    }
    const int max_offset = count - kVisibleRows;
    if (s_scroll_offset > max_offset) {
        s_scroll_offset = max_offset;
    }
    if (s_scroll_offset < 0) {
        s_scroll_offset = 0;
    }
}

void apply_row_visual(int row, bool visible, bool selected)
{
    lv_obj_t *bar = s_row_bars[row];
    lv_obj_t *label = s_row_labels[row];
    if (bar) {
        set_obj_visible(bar, visible);
        set_obj_black(bar, visible && selected);
    }
    if (label) {
        set_obj_visible(label, visible);
        lv_obj_set_style_text_color(label,
                                   (visible && selected) ? lv_color_white()
                                                         : lv_color_black(),
                                   LV_PART_MAIN);
    }
}

void create_row_objects(lv_obj_t *screen)
{
    for (int row = 0; row < kVisibleRows; ++row) {
        const int y = kListTopY + row * kRowPitch;
        s_row_bars[row] = make_bar(screen, kListX, y, kListWidth, kRowHeight);
        s_row_labels[row] = make_label_with_font(screen,
                                                 kListX + 8,
                                                 y + 4,
                                                 kListWidth - 16,
                                                 kRowHeight - 6,
                                                 "",
                                                 &zh_font_16);
        if (!s_row_labels[row]) {
            ESP_LOGW(TAG, "%s", kRowLabelWarning);
        }
        apply_row_visual(row, false, false);
    }
}

// 1bpp 屏上「已填充」= 实心黑块，未填充 = 白底 + 1px 黑边。
// 焦点在音量行时整行反色：底条变黑，格子与文字跟着反色，
// 这样「选中」这件事在墨水屏上一眼能看出来，不用靠闪烁。
//
// 不复用 set_obj_black()：它会把 radius 强制设成 1，格子要方角，
// 而且它只管「黑/白」两态，没法表达「白底黑边」的空档。
void apply_volume_cell(int index, bool filled, bool focused)
{
    lv_obj_t *cell = s_volume_cells[index];
    if (!cell) {
        return;
    }
    const lv_color_t ink = focused ? lv_color_white() : lv_color_black();
    const lv_color_t paper = focused ? lv_color_black() : lv_color_white();
    lv_obj_set_style_radius(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(cell, ink, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cell, filled ? ink : paper, LV_PART_MAIN);
}

// 创建顺序即叠放顺序（LVGL 后创建的在上层）：底条 → "音量" → 格子 → 数值。
void create_volume_objects(lv_obj_t *screen)
{
    s_volume_bar = make_bar(screen, kListX, kVolumeRowY, kListWidth, kVolumeRowHeight);
    if (!s_volume_bar) {
        ESP_LOGW(TAG, "%s", kRowLabelWarning);
    }

    s_volume_label = make_label_with_font(screen,
                                          kListX,
                                          kVolumeRowY + 3,
                                          kVolumeLabelWidth,
                                          kVolumeRowHeight - 6,
                                          "音量",
                                          &zh_font_16);
    if (!s_volume_label) {
        ESP_LOGW(TAG, "%s", kRowLabelWarning);
    }

    for (int i = 0; i < kVolumeCellCount; ++i) {
        const int x = kVolumeCellsX + i * (kVolumeCellWidth + kVolumeCellGap);
        s_volume_cells[i] = make_bar(screen,
                                     x,
                                     kVolumeRowY + 2,
                                     kVolumeCellWidth,
                                     kVolumeRowHeight - 4);
        if (!s_volume_cells[i]) {
            ESP_LOGW(TAG, "%s", kRowLabelWarning);
        }
        apply_volume_cell(i, false, false);
    }

    s_volume_value = make_label_with_font(screen,
                                          kVolumeValueX,
                                          kVolumeRowY + 3,
                                          kVolumeValueWidth,
                                          kVolumeRowHeight - 6,
                                          "",
                                          &zh_font_16);
    if (!s_volume_value) {
        ESP_LOGW(TAG, "%s", kRowLabelWarning);
        return;
    }
    // 档位文本右对齐贴着列表右边：格子是左对齐的定宽块，数值再左对齐就会
    // 在不同档位下左右晃（"静音" 与 "100%" 差一个字宽）。
    lv_obj_set_style_text_align(s_volume_value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
}

void create_hint_objects(lv_obj_t *screen)
{
    s_hint_primary = make_centered_label_with_font(screen,
                                                   kHintX,
                                                   kHintPrimaryY,
                                                   kHintWidth,
                                                   kHintHeight,
                                                   kHintPrimaryText,
                                                   &zh_font_16,
                                                   kHintLabelWarning);
    s_hint_secondary = make_centered_label_with_font(screen,
                                                     kHintX,
                                                     kHintSecondaryY,
                                                     kHintWidth,
                                                     kHintHeight,
                                                     kHintSecondaryText,
                                                     &zh_font_16,
                                                     kHintLabelWarning);
    s_hint_status = make_centered_label_with_font(screen,
                                                  kHintX,
                                                  kHintStatusY,
                                                  kHintWidth,
                                                  kHintHeight,
                                                  "",
                                                  &zh_font_16,
                                                  kHintLabelWarning);
    s_hint_tertiary = make_centered_label_with_font(screen,
                                                    kHintX,
                                                    kHintTertiaryY,
                                                    kHintWidth,
                                                    kHintHeight,
                                                    kHintTertiaryText,
                                                    &zh_font_16,
                                                    kHintLabelWarning);
}

void create_empty_objects(lv_obj_t *screen)
{
    s_empty_title = make_centered_label_with_font(screen,
                                                  0,
                                                  kEmptyTitleY,
                                                  kDisplayWidth,
                                                  kEmptyTitleHeight,
                                                  kEmptyTitleText,
                                                  // zh_pomodoro_title_24 是"番茄钟"三字的子集字体，
                                                  // 落到它这里只会显示 ASCII 部分、汉字全丢；
                                                  // 标题要靠汉字说清"插 TF 卡"，所以用全字集的 zh_font_16。
                                                  &zh_font_16,
                                                  kHintLabelWarning);
    s_empty_hint = make_centered_label_with_font(screen,
                                                 0,
                                                 kEmptyHintY,
                                                 kDisplayWidth,
                                                 kEmptyHintHeight,
                                                 kEmptyHintText,
                                                 &zh_font_16,
                                                 kHintLabelWarning);
    s_empty_hint_second = make_centered_label_with_font(screen,
                                                        0,
                                                        kEmptyHintSecondY,
                                                        kDisplayWidth,
                                                        kEmptyHintHeight,
                                                        kEmptyHintSecondText,
                                                        &zh_font_16,
                                                        kHintLabelWarning);
}

// 蓝牙手柄状态文案。
//
// 为什么不直接用 gameboy_pad.h 的 pad_state_text()：它返回的是 "BT SCANNING"
// 这类 ASCII 串（给串口日志用很合适），但本页状态行的其它文案全是中文，
// 混一排英文很突兀。这里按同一个 pad_state_t 做一份中文映射。
// 映射表要跟着 gameboy_pad.h 的枚举一起改——枚举新增状态时会漏，
// 所以 default 分支兜底成"未知"，至少不会显示空白让人以为坏了。
void pad_status_text(char *out, size_t out_bytes)
{
    const char *state = "未知";
    switch (pad_state()) {
    case PAD_STATE_OFF:         state = "已关闭"; break;
    case PAD_STATE_STARTING:    state = "启动中"; break;
    case PAD_STATE_SCANNING:    state = "扫描中"; break;
    case PAD_STATE_CONNECTING:  state = "连接中"; break;
    case PAD_STATE_DISCOVERING: state = "配对中"; break;
    case PAD_STATE_READY:       state = "已连接"; break;
    case PAD_STATE_RETRY_WAIT:  state = "待重连"; break;
    case PAD_STATE_ERROR:       state = "异常";   break;
    default:                    break;
    }

    // READY 时把手柄名带上。pad_name() 未连接返回空串，BLE 广播名是 ASCII，
    // 落在 zh_font_16 的 0x20-0x7E 范围内，不会触发字体 fallback。
    //
    // 宽度预算：状态行标签宽 kHintWidth=388px。左侧最坏情况是
    // 「共 N 个 ROM  上次 59.6fps」约 168px，加上本函数自己的「  手柄 已连接 」
    // 约 92px，留给手柄名只剩约 128px。zh_font_16 的 ASCII 约 8px/字符，
    // 所以名字最多 14 个字符——超过就会把整行撑出标签，LVGL 会换行后被
    // 20px 的行高裁掉半行。这里直接截断（BLE 广播名上限 PAD_NAME_LEN=24）。
    constexpr size_t kPadNameMaxChars = 14;
    const char *name = (pad_state() == PAD_STATE_READY) ? pad_name() : "";
    if (name && name[0] != '\0') {
        char safe[kPadNameMaxChars + 1] = {};
        snprintf(safe, sizeof(safe), "%s", name);
        snprintf(out, out_bytes, "手柄 %s %s", state, safe);
    } else {
        snprintf(out, out_bytes, "手柄 %s", state);
    }
}

bool update_status_line(int count, const char *notice)
{
    if (!s_hint_status) {
        return false;
    }
    const GameboyRuntimeStats stats = gameboy_runtime_stats();
    char text[96] = {};
    if (notice && notice[0] != '\0') {
        snprintf(text, sizeof(text), "%s", notice);
    } else if (count > 0) {
        if (stats.presented_frames > 0 || stats.emulated_frames > 0) {
            snprintf(text,
                     sizeof(text),
                     "共 %d 个 ROM  上次 %u.%ufps",
                     count,
                     (unsigned)(stats.fps_x10 / 10U),
                     (unsigned)(stats.fps_x10 % 10U));
        } else {
            snprintf(text, sizeof(text), "共 %d 个 ROM", count);
        }
    } else {
        snprintf(text, sizeof(text), "%s", rom_store_diagnostic());
    }

    // 蓝牙状态接在后面。有 notice 时不追加：notice 是启动失败原因这类
    // 一次性提示，比蓝牙状态重要得多，不能让它半句话被挤掉。
    if (!(notice && notice[0] != '\0')) {
        char pad[40] = {};
        pad_status_text(pad, sizeof(pad));
        // 两个空格做视觉分隔，与上面"共 N 个 ROM  上次 xx fps"的内部间距一致。
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "  %s", pad);
    }

    return set_label_text_if_changed(s_hint_status, text);
}

// 上一帧是不是还在绑定页。退出绑定页那一帧要靠它补一次强制重绘，原因见
// refresh_game_page_content() 里的注释。
bool s_bind_was_active = false;

// 把 ROM 列表这一整层对象藏起来。没有对应的 show_()：恢复显隐不用单独做，
// 下面正常路径每帧都会按 has_rows 调 set_obj_visible()，而 set_obj_visible()
// 自己会比对 flag，重复调用不会产生多余的重绘。
//
// 为什么要藏：绑定页是盖在游戏页之上的整屏不透明容器，这些对象其实已经被完全
// 遮住，藏掉只是让 LVGL 不用再遍历它们，也免得将来调整层级时两套对象打架。
void hide_list_layer()
{
    for (int row = 0; row < kVisibleRows; ++row) {
        (void)set_obj_visible(s_row_bars[row], false);
        (void)set_obj_visible(s_row_labels[row], false);
    }
    (void)set_obj_visible(s_hint_primary, false);
    (void)set_obj_visible(s_hint_secondary, false);
    (void)set_obj_visible(s_hint_status, false);
    (void)set_obj_visible(s_hint_tertiary, false);
    (void)set_obj_visible(s_empty_title, false);
    (void)set_obj_visible(s_empty_hint, false);
    (void)set_obj_visible(s_empty_hint_second, false);
    (void)set_obj_visible(s_volume_bar, false);
    (void)set_obj_visible(s_volume_label, false);
    (void)set_obj_visible(s_volume_value, false);
    for (int i = 0; i < kVolumeCellCount; ++i) {
        (void)set_obj_visible(s_volume_cells[i], false);
    }
}

// 返回本次是否改变了屏幕内容（用于通知 ui_task 是否需要重绘）。
bool refresh_game_page_content()
{
    lv_obj_t *const bind_root = work_page_root(kWorkPageGameBoy);
    const bool bind_active = game_bind_page_active();

    // ---- 手柄绑定页：整块接管本屏幕 ----
    //
    // 绑定页是游戏页的一个子模式（见 ui_game_bind.h 开头），优先级高于 ROM
    // 列表：命中就直接返回，一行列表都不能再画，否则两套对象叠在一起。
    if (bind_active) {
        s_bind_was_active = true;
        hide_list_layer();
        if (!bind_root) {
            return false;
        }
        game_bind_page_build(bind_root);
        return game_bind_page_update();
    }
    // 退出绑定页的第一帧必须主动置一次 changed：下面正常路径会把列表对象重新
    // 放出来，但它们的**文本可能一个字都没变**，而本函数的 changed 是靠文本
    // diff 累加的，于是这一帧会被判成"画面没变"，墨水屏停在绑定页上不动。
    const bool bind_just_closed = s_bind_was_active && !bind_active;
    s_bind_was_active = bind_active;
    // 不在绑定页也要调一次 build()：契约里没有 hide 入口，这是让绑定页把自己
    // 收回去的唯一时机，之后下面的正常路径会把列表重新显示出来。
    if (bind_root) {
        game_bind_page_build(bind_root);
    }

    const bool mounted = rom_store_mount();
    const int count = rom_store_count();
    const int selected = rom_store_selected();

    char notice[64] = {};
    copy_notice(notice, sizeof(notice));
    if (notice[0] == '\0' && !mounted && count == 0) {
        snprintf(notice, sizeof(notice), "%s", rom_store_diagnostic());
    }

    // bind_just_closed：见上面绑定页分支的注释，退出绑定页这一帧必须重绘。
    bool changed = bind_just_closed;
    if (count != s_rendered_count || selected != s_rendered_selected ||
        strcmp(notice, s_rendered_notice) != 0) {
        changed = true;
    }

    // 焦点判定必须赶在画 ROM 行之前定下来：焦点在音量行时 ROM 行一个都不
    // 许高亮，否则墨水屏上同时出现两个"选中项"，用户不知道 KEY 会作用于谁。
    const bool has_rows = count > 0;
    const bool volume_focused = has_rows && s_volume_focused;

    ensure_scroll_window(count, selected);

    for (int row = 0; row < kVisibleRows; ++row) {
        const int index = s_scroll_offset + row;
        const bool visible = index < count;
        const char *name = visible ? rom_store_name(index) : "";
        char text[game_config::kRomDisplayNameBytes] = {};
        if (visible) {
            format_row_text(text, sizeof(text), index, name);
        }
        const bool row_changed =
            strcmp(text, s_rendered_names[row]) != 0;
        if (row_changed) {
            snprintf(s_rendered_names[row], sizeof(s_rendered_names[row]), "%s", text);
            changed = true;
        }
        apply_row_visual(row, visible, visible && index == selected && !volume_focused);
        if (s_row_labels[row] && row_changed) {
            (void)set_label_text_if_changed(s_row_labels[row], text);
        }
    }

    set_obj_visible(s_empty_title, !has_rows);
    set_obj_visible(s_empty_hint, !has_rows);
    set_obj_visible(s_empty_hint_second, !has_rows);
    set_obj_visible(s_hint_primary, has_rows);
    set_obj_visible(s_hint_secondary, has_rows);
    set_obj_visible(s_hint_tertiary, has_rows);

    if (!has_rows) {
        (void)set_label_text_if_changed(s_empty_title, kEmptyTitleText);
    }

    // 诊断日志只盯列表/notice 那一档变化（见下面日志段），音量行不参与，
    // 所以快照要赶在音量块之前取。
    const bool list_changed = changed;

    // ---- 音量行 ----
    //
    // 没有 ROM（TF 卡没插）时整个列表层是空的，音量行跟着藏起来：这一页此时
    // 显示的是"请插入 TF 卡"，塞一行音量条进去只会让用户以为能调。
    const int volume_step = game_audio_volume_step();
    changed = set_obj_visible(s_volume_bar, has_rows) || changed;
    (void)set_obj_visible(s_volume_label, has_rows);
    (void)set_obj_visible(s_volume_value, has_rows);
    for (int i = 0; i < kVolumeCellCount; ++i) {
        (void)set_obj_visible(s_volume_cells[i], has_rows);
    }
    set_obj_black(s_volume_bar, volume_focused);
    if (s_volume_label) {
        lv_obj_set_style_text_color(s_volume_label,
                                    volume_focused ? lv_color_white()
                                                   : lv_color_black(),
                                    LV_PART_MAIN);
    }
    if (s_volume_value) {
        lv_obj_set_style_text_color(s_volume_value,
                                    volume_focused ? lv_color_white()
                                                   : lv_color_black(),
                                    LV_PART_MAIN);
        char volume_text[16] = {};
        if (volume_step <= 0) {
            snprintf(volume_text, sizeof(volume_text), "%s", "静音");
        } else {
            snprintf(volume_text, sizeof(volume_text), "%d%%",
                     game_audio_volume_percent());
        }
        changed = set_label_text_if_changed(s_volume_value, volume_text) || changed;
    }
    for (int i = 0; i < kVolumeCellCount; ++i) {
        apply_volume_cell(i, i < volume_step, volume_focused);
    }
    // 格子填充和整行反色都是纯样式变化，LVGL 会 invalidate 但本函数的 changed
    // 是给墨水屏门闸看的，必须由这里自己判：漏了就出现"音量其实变了，屏幕
    // 却停在旧档位"。
    if (volume_step != s_rendered_volume_step ||
        volume_focused != s_rendered_volume_focused) {
        changed = true;
    }

    // 状态行里的蓝牙状态会自己变（扫描中→已连接），此时 ROM 列表没动，
    // 若不把它的变化并进 changed，墨水屏就不会重绘，用户永远看不到状态更新。
    changed = changed || update_status_line(count, notice);

    // ---- 诊断日志（tag: GameUI）----
    //
    // 为什么要有这条：自检阶段 log_rom_inventory() 的 rom count 是它**自己
    // opendir 数出来**的，那条路不走 name_chars_allowed()；而 RomCatalog 的
    // refresh_locked() 走。两套口径一旦不一致，现象就是"自检日志 rom count=8、
    // UI 却显示请插入 TF 卡"，且没有任何一方报错——这个坑已经真实发生过一次
    // （用户实机反馈 ROM 不显示，日志 count=8，根因是白名单漏了 '+' '(' ')'，
    // 8 个 ROM 全被静默过滤）。
    //
    // 所以这里把这条链路真正的出口数据打出来：RomCatalog 的 count（注意不是
    // 自检那个独立计数）、挂载结果、选中项、has_rows 的最终取值，以及**最终交给
    // label 的文本**（format_row_text() 之后，也就是屏幕上真正显示的东西）。
    // 三者对不上时，这条日志能直接指出分歧出在哪一环。
    //
    // 限流：本函数每轮 ui_task 都会调，不能每轮都打。用函数末尾的 changed 做
    // 门闸——列表/选中项/提示任一变化时打一条，稳定之后不打，不刷屏。
    if (list_changed) {
        char diag[kGameDiagRows][game_config::kRomDisplayNameBytes] = {};
        for (int row = 0; row < kGameDiagRows; ++row) {
            const int index = s_scroll_offset + row;
            if (index < count) {
                format_row_text(diag[row], sizeof(diag[row]), index,
                                rom_store_name(index));
            }
        }
        char pad[40] = {};
        pad_status_text(pad, sizeof(pad));
        ESP_LOGI(kGameUiLogTag,
                 "refresh: mounted=%d count=%d selected=%d has_rows=%d "
                 "notice=%s pad=%s rows=[%s | %s | %s]",
                 mounted ? 1 : 0,
                 count,
                 selected,
                 has_rows ? 1 : 0,
                 notice,
                 pad,
                 diag[0],
                 diag[1],
                 diag[2]);
    }

    if (changed) {
        s_rendered_count = count;
        s_rendered_selected = selected;
        snprintf(s_rendered_notice, sizeof(s_rendered_notice), "%s", notice);
        s_rendered_volume_step = volume_step;
        s_rendered_volume_focused = volume_focused;
    }
    return changed;
}
} // namespace

void build_game_page()
{
    lv_obj_t *existing_root = work_page_root(kWorkPageGameBoy);
    if (existing_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    set_work_page_root(kWorkPageGameBoy, screen);

    build_work_page_battery_icon(screen, kWorkPageGameBoy);
    build_work_page_status_bar(screen,
                               kWorkPageGameBoy,
                               true,
                               true);
    make_black_bar(screen,
                   ui_work_page_layout::kTopSeparatorX,
                   ui_work_page_layout::kTopSeparatorY,
                   ui_work_page_layout::kTopSeparatorWidth,
                   ui_work_page_layout::kTopSeparatorHeight);

    create_row_objects(screen);
    create_volume_objects(screen);
    create_hint_objects(screen);
    create_empty_objects(screen);

    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    update_work_page_battery_icon(kWorkPageGameBoy, battery_percent_load());
    s_rendered_count = -1;
    s_rendered_selected = -1;
    s_rendered_notice[0] = '\0';
    // 焦点默认落在第一个 ROM 上，音量缓存置成"从未画过"逼首帧画出来。
    s_volume_focused = false;
    s_rendered_volume_step = -1;
    s_rendered_volume_focused = false;
    for (int row = 0; row < kVisibleRows; ++row) {
        s_rendered_names[row][0] = '\0';
    }
    (void)refresh_game_page_content();
}

// 游戏运行中必须主动退出的三个场景。三条都不是"体验问题"：
//   1. 低电量：模拟器把 CPU 钉在 240MHz 满负荷，会把"还能撑 20 分钟"变成"5 分钟关机"，
//      用户只会归因成"这机器电池不行"；
//   2. OTA：ui_display_flush.cpp 的 OTA 分支在门闸**之后**，游戏运行中进度页根本
//      画不出来，用户会以为升级卡死然后拔电 —— 那可能直接变砖；
//   3. 配网页：WiFi AP + BLE + 模拟器三路重负载叠加，最容易出玄学问题。
// 返回非空字符串 = 该退出，字符串就是给用户看的原因。
static const char *game_stop_reason()
{
    if (battery_low_mode_load()) {
        return "电量过低，已退出游戏";
    }
    const OtaRuntimeFlagsSnapshot ota = ota_runtime_flags_load();
    if (ota.state == kOtaUpdating) {
        return "正在升级固件，已退出游戏";
    }
    if (ota.reboot_pending) {
        return "等待重启，已退出游戏";
    }
    if (setup_portal_active_load()) {
        return "配网页已开启，已退出游戏";
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 逃生守卫：把游戏停下来，并保证用户看得见「为什么」。
//
// 横幅画在 lv_layer_top() 上而不是游戏页里：低电量模式会把工作页强切到天气钟、
// OTA 会切到设置/进度页，画在页面里的提示用户根本看不到。lv_layer_top() 在所有
// screen 之上，页面怎么切都不影响它。
// ---------------------------------------------------------------------------
constexpr uint32_t kExitToastVisibleMs = 4000;
constexpr int kExitToastX = 16;
constexpr int kExitToastY = 130;
constexpr int kExitToastBorder = 2;
constexpr int kExitToastWidth = kDisplayWidth - 2 * kExitToastX;
constexpr int kExitToastHeight = 36;
constexpr const char *kExitToastCreateWarning = "exit toast create failed";
constexpr const char *kExitToastLabelWarning = "exit toast label create failed";

lv_obj_t *s_exit_toast = nullptr;
lv_obj_t *s_exit_toast_label = nullptr;
int64_t s_exit_toast_hide_us = 0;

// 守卫在 ui_task 拿锁之前就停了游戏，此时不能碰 LVGL。
// 理由先落在这里，等下一次拿到锁再画。
portMUX_TYPE s_exit_reason_mux = portMUX_INITIALIZER_UNLOCKED;
char s_exit_reason[64] = "";
bool s_exit_reason_pending = false;

void post_exit_reason(const char *reason)
{
    portENTER_CRITICAL(&s_exit_reason_mux);
    snprintf(s_exit_reason, sizeof(s_exit_reason), "%s", reason ? reason : "");
    s_exit_reason_pending = true;
    portEXIT_CRITICAL(&s_exit_reason_mux);
}

bool take_exit_reason(char *out, size_t out_bytes)
{
    if (!out || out_bytes == 0) {
        return false;
    }
    out[0] = '\0';
    bool pending = false;
    portENTER_CRITICAL(&s_exit_reason_mux);
    if (s_exit_reason_pending) {
        snprintf(out, out_bytes, "%s", s_exit_reason);
        s_exit_reason_pending = false;
        pending = true;
    }
    portEXIT_CRITICAL(&s_exit_reason_mux);
    return pending;
}

void show_exit_toast(const char *text)
{
    if (!text || text[0] == '\0') {
        return;
    }
    if (!s_exit_toast) {
        s_exit_toast = lv_obj_create(lv_layer_top());
        if (!s_exit_toast) {
            ESP_LOGW(kGameLogTag, "%s", kExitToastCreateWarning);
            return;
        }
        lv_obj_clear_flag(s_exit_toast, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_exit_toast, kExitToastWidth, kExitToastHeight);
        lv_obj_set_pos(s_exit_toast, kExitToastX, kExitToastY);
        lv_obj_set_style_radius(s_exit_toast, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_exit_toast, 0, LV_PART_MAIN);
        // 面板是 1bit 黑白屏：必须不透明，否则会叠在半屏旧画面上糊成一团。
        lv_obj_set_style_bg_color(s_exit_toast, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_exit_toast, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_exit_toast, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_exit_toast, kExitToastBorder, LV_PART_MAIN);
        s_exit_toast_label = make_centered_label_with_font(s_exit_toast,
                                                          0,
                                                          0,
                                                          kExitToastWidth,
                                                          kExitToastHeight,
                                                          "",
                                                          &zh_font_16,
                                                          kExitToastLabelWarning);
        if (!s_exit_toast_label) {
            // 标签没建出来就整块拆掉，避免留下一个只有黑框的空洞。
            lv_obj_del(s_exit_toast);
            s_exit_toast = nullptr;
            return;
        }
        lv_obj_set_style_text_color(s_exit_toast_label,
                                    lv_color_white(),
                                    LV_PART_MAIN);
    }
    (void)set_label_text_if_changed(s_exit_toast_label, text);
    lv_obj_clear_flag(s_exit_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_exit_toast);
    s_exit_toast_hide_us = esp_timer_get_time() + (int64_t)kExitToastVisibleMs * 1000;
    lv_obj_invalidate(lv_layer_top());
}

bool hide_exit_toast_if_expired()
{
    if (!s_exit_toast || s_exit_toast_hide_us == 0) {
        return false;
    }
    if (esp_timer_get_time() < s_exit_toast_hide_us) {
        return false;
    }
    s_exit_toast_hide_us = 0;
    lv_obj_add_flag(s_exit_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_layer_top());
    return true;
}

void game_page_guard_runtime_stop()
{
    if (!gameboy_runtime_running()) {
        return;
    }
    const char *reason = game_stop_reason();
    if (!reason) {
        return;
    }
    // 先落文案再停：stop() 会关掉显示门闸，之后画面归 LVGL，
    // 顺序反了就会出现「已经停了但没有任何解释」的黑屏窗口。
    post_exit_reason(reason);
    ESP_LOGW(kGameLogTag, "auto stop game: %s", reason);
    gameboy_runtime_stop();
}

bool game_page_guard_tick_ui()
{
    bool changed = false;
    char reason[64] = {};
    if (take_exit_reason(reason, sizeof(reason))) {
        // 两条路都走：游戏页若还在前台，提示行直接显示；否则靠顶层横幅
        // 越过天气钟 / OTA 进度页 / 配网面板。
        set_notice(reason);
        show_exit_toast(reason);
        changed = true;
    }
    if (hide_exit_toast_if_expired()) {
        changed = true;
    }
    return changed;
}

bool update_game_page(const struct tm &local)
{
    // 手柄服务：进入本页拉起 BLE 扫描，离开本页 1.5s 后自动停扫省电。
    // 必须放在 running 的提前 return 之前 —— 游戏运行期间本页不再重绘，
    // 但手柄输入正是这时候要活着。
    pad_service_ensure();
    pad_service_notify_visible();

    bool changed = false;
    // 运行中不允许在本函数里判「该不该停」：低电量/配网页模式下 ui_views 会把
    // state.game 置 false、甚至根本不调本函数，判在这里等于没判。
    // 唯一的停机入口是 game_page_guard_runtime_stop()（ui_task 循环顶部无条件调用）。
    const bool running = gameboy_runtime_running();

    if (s_was_running && !running) {
        // 退出游戏：门闸已关，整屏重绘一次清掉帧缓冲里的 GB 残留。
        lv_obj_invalidate(lv_scr_act());
        s_rendered_count = -1;
        changed = true;
    }
    s_was_running = running;

    if (running && !game_bind_page_active()) {
        // 门闸打开期间不做任何游戏页渲染，也不刷状态栏时间（那次渲染会被
        // 门闸丢弃，纯粹白占 core 1 的 CPU，和优先级 1 的模拟器抢核）。
        // 绑定页是 idle 态页面（模拟器跑着的时候没人能进绑定页），显式放行
        // 是为了防御两边状态偶发不同步时绑定页还能自己刷出来。
        return changed;
    }
    changed |= update_work_page_status_time(kWorkPageGameBoy, local);
    changed |= refresh_game_page_content();
    return changed;
}

bool game_page_next_rom()
{
    if (gameboy_runtime_running()) {
        return false;
    }
    (void)rom_store_mount();
    const int count = rom_store_count();
    if (count <= 0) {
        return false;
    }
    int next = rom_store_selected() + 1;
    if (next >= count) {
        next = 0;
    }
    return rom_store_select(next);
}

// 手柄十字键「上」用：与 next 对称地回绕，手柄导航要是只能单向循环会很别扭。
bool game_page_prev_rom()
{
    if (gameboy_runtime_running()) {
        return false;
    }
    (void)rom_store_mount();
    const int count = rom_store_count();
    if (count <= 0) {
        return false;
    }
    int prev = rom_store_selected() - 1;
    if (prev < 0) {
        prev = count - 1;
    }
    return rom_store_select(prev);
}

bool game_page_start_selected()
{
    if (gameboy_runtime_running()) {
        return true;
    }
    if (!gameboy_runtime_start()) {
        set_notice(gameboy_runtime_last_error());
        ESP_LOGW(TAG, "game start failed: %s", gameboy_runtime_last_error());
        return false;
    }
    set_notice("");
    s_rendered_count = -1;
    return true;
}

int game_page_rom_count()
{
    return rom_store_count();
}

int game_page_selected_index()
{
    return rom_store_selected();
}

bool game_page_volume_focused()
{
    return s_volume_focused;
}

// BOOT 短按 / 手柄下：ROM 之间循环，走到最后一个 ROM 之后进音量行，
// 再走一次回到第一个 ROM。count == 0（没插 TF 卡）时没有音量行，直接返回 false。
bool game_page_focus_next()
{
    if (gameboy_runtime_running()) {
        return false;
    }
    (void)rom_store_mount();
    const int count = rom_store_count();
    if (count <= 0) {
        return false;
    }
    if (s_volume_focused) {
        s_volume_focused = false;
        return rom_store_select(0);
    }
    const int selected = rom_store_selected();
    if (selected >= count - 1) {
        s_volume_focused = true;
        return true;
    }
    return rom_store_select(selected + 1);
}

// 手柄上：与 focus_next 严格对称（第一个 ROM 再往前进音量行）。
bool game_page_focus_prev()
{
    if (gameboy_runtime_running()) {
        return false;
    }
    (void)rom_store_mount();
    const int count = rom_store_count();
    if (count <= 0) {
        return false;
    }
    if (s_volume_focused) {
        s_volume_focused = false;
        return rom_store_select(count - 1);
    }
    const int selected = rom_store_selected();
    if (selected <= 0) {
        s_volume_focused = true;
        return true;
    }
    return rom_store_select(selected - 1);
}

// 调音量前必须先把焦点拉到音量行：手柄左/右是全局快捷键，用户按的时候焦点
// 可能还在某个 ROM 上，不拉过去的话档位悄悄变了而屏幕上看不出是哪一行在变。
// 档位本身由 game_audio 持有，这里只负责「让变化可见」。
bool game_page_volume_step_up()
{
    s_volume_focused = true;
    return game_audio_volume_step_up();
}

bool game_page_volume_step_down()
{
    s_volume_focused = true;
    return game_audio_volume_step_down();
}
