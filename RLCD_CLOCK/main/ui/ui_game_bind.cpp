// ui_game_bind.cpp —— 游戏页的「手柄按键绑定」子模式
//
// 分工严格照抄 ui_game_bind.h 的约定：
//   - open/close/active/next_action/bind_current 只改状态，按键任务里随便调，
//     一个 LVGL 调用都不能有（按键任务和 ui_task 抢不到同一把锁）；
//   - build/update 碰 LVGL，只能在 ui_task 持锁时调。
//
// 画面构成：一整块铺满 400x300 的白底容器 s_root，里面从上到下是
//   标题行（左边标题 / 右边手柄状态）→ 虚拟手柄机身（十字键、A/B、三颗胶囊）
//   → 三行状态文本。
// 单色屏只有黑白两色，所以状态全靠「反色」和「线宽」表达：
//   已绑定 = 描边加粗，当前选中 = 整块反色，捕获中 = 反色 + 描边加粗。
#include "ui_game_bind.h"

#include "app_display_config.h"
#include "app_metadata.h"
#include "gameboy_pad.h"
#include "gameboy_pad_bind.h"
#include "ui_fonts.h"
#include "ui_widgets.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include <string.h>

namespace {
// ---------------------------------------------------------------- 文案
constexpr const char *kTitleText = "手柄按键绑定";
constexpr const char *kHintText =
    "BOOT 下一个　KEY 绑定(连按3次)　KEY 长按 退出　BOOT 长按 清空";
constexpr const char *kBoundText = "已绑定";
constexpr const char *kUnboundText = "未绑定";
constexpr const char *kWaitText = "请按下手柄上的键";
constexpr const char *kReleaseText = "请松开按键";
constexpr const char *kCycleText = "已确认";
constexpr const char *kFailText = "绑定失败，请重试";
constexpr const char *kTimeoutText = "超时，请重试";
constexpr const char *kPadReadyText = "手柄已就绪";
constexpr const char *kPadMissingText = "手柄未连接，请先连接";

// 截断标记：三个 ASCII 点，和 ui_game.cpp 里保持一致（U+2026「…」不在
// zh_font_16 的码点范围里，用了会静默掉进 fallback 字体）。
constexpr const char *kTruncationMarker = "...";

// 行内分隔符。刻意躲开两个坑：
//   1. 冒号「：」——本项目 UI 文案一律不用；
//   2. U+00B7「·」——它不在 zh_font_16 的码点范围里（0x20-0x7E + 0x2103 +
//      0x3000-0x303F + 0x4E00-0x9FFF + 0xFF00-0xFFEF），会静默 fallback。
//      zh_font_16 的 fallback 是 montserrat_14，其范围含 U+2022「•」，
//      渲染出来就是一个居中的小点，视觉等价但一定画得出来。
constexpr const char *kSeparator = "\xE2\x80\xA2";

constexpr const char *kCreateFailedLog = "game bind object create failed";
constexpr const char *kLabelCreateFailedLog = "game bind label create failed";

// ---------------------------------------------------------------- 布局
//
// 全部写成相对 s_root 的绝对坐标：屏幕只有 400x300，每块位置都是数出来的，
// 用相对布局（FLEX/GRID）反而要在心里再算一遍，代码里也看不出在哪。
constexpr int kTitleX = 12;
constexpr int kTitleY = 4;
constexpr int kTitleWidth = 176;
constexpr int kTitleHeight = 22;

constexpr int kPadStatusX = 200;
constexpr int kPadStatusWidth = 188;
// 右侧留 2px：贴着屏幕边缘容易被外壳压住一点。
constexpr int kPadStatusMaxPx = kPadStatusWidth - 2;

constexpr int kPanelX = 14;
constexpr int kPanelY = 30;
constexpr int kPanelWidth = 372;
constexpr int kPanelHeight = 202;
constexpr int kPanelRadius = 72;

// 十字键的两条臂（臂宽 24 / 半臂长 34）。两个矩形叠起来就是十字。
constexpr int kDpadBarX = 52;
constexpr int kDpadBarY = 119;
constexpr int kDpadBarWidth = 68;
constexpr int kDpadBarHeight = 24;
constexpr int kDpadColumnX = 74;
constexpr int kDpadColumnY = 97;
constexpr int kDpadColumnWidth = 24;
constexpr int kDpadColumnHeight = 68;
constexpr int kDpadCenterX = 86;
constexpr int kDpadCenterY = 131;

constexpr int kStatusRowX = 0;
constexpr int kStatusRowWidth = kDisplayWidth;
constexpr int kStatusRowHeight = 20;
constexpr int kActionRowY = 236;
constexpr int kHintRowY = 256;
constexpr int kProgressRowY = 276;

constexpr lv_coord_t kBorderThin = 2;   // 未绑定
constexpr lv_coord_t kBorderThick = 4;  // 已绑定 / 捕获中

static_assert(kTitleX + kTitleWidth <= kPadStatusX,
              "bind page title must not overlap the pad status area");
static_assert(kPadStatusX + kPadStatusWidth <= kDisplayWidth,
              "pad status line must stay inside the display");
static_assert(kPanelX + kPanelWidth <= kDisplayWidth &&
                  kPanelY + kPanelHeight <= kDisplayHeight,
              "virtual pad body must stay inside the display");
static_assert(kProgressRowY + kStatusRowHeight <= kDisplayHeight,
              "bind status rows must stay inside the display");
static_assert(kDpadBarX + kDpadBarWidth / 2 == kDpadCenterX &&
                  kDpadColumnX + kDpadColumnWidth / 2 == kDpadCenterX,
              "dpad arms must share one center column");
static_assert(kDpadBarY + kDpadBarHeight / 2 == kDpadCenterY &&
                  kDpadColumnY + kDpadColumnHeight / 2 == kDpadCenterY,
              "dpad arms must share one center row");

// ------------------------------------------------- 9 个动作的命中图形
//
// 顺序必须与 pad_bind_action_t 一致：这张表的下标直接当 pad_bind_action_t 用，
// 枚举顺序一变整页就串位（NVS 里存的顺序也跟着错）。
struct ZoneGeometry {
    int x;
    int y;
    int w;
    int h;
    int radius;
};

// 右侧四颗面键排成菱形：B 左上、A 右下是原有的两颗，新加的 X / Y 补在另外
// 两个角上。四个 40x40 的圆互不重叠（相邻两颗的 x 或 y 都刚好错开 48px，
// 圆的直径 40），下面用 static_assert 把这件事钉死，免得以后挪动数值时
// 悄悄叠在一起。
constexpr ZoneGeometry kZoneGeometry[PAD_BIND_COUNT] = {
    /* PAD_BIND_A      */ {278, 130, 40, 40, LV_RADIUS_CIRCLE},
    /* PAD_BIND_B      */ {230, 90, 40, 40, LV_RADIUS_CIRCLE},
    /* PAD_BIND_X      */ {230, 130, 40, 40, LV_RADIUS_CIRCLE},
    /* PAD_BIND_Y      */ {278, 90, 40, 40, LV_RADIUS_CIRCLE},
    /* PAD_BIND_UP     */ {74, 97, 24, 24, 6},
    /* PAD_BIND_DOWN   */ {74, 141, 24, 24, 6},
    /* PAD_BIND_LEFT   */ {52, 119, 24, 24, 6},
    /* PAD_BIND_RIGHT  */ {96, 119, 24, 24, 6},
    /* PAD_BIND_START  */ {204, 200, 52, 16, 8},
    /* PAD_BIND_SELECT */ {140, 200, 52, 16, 8},
};

constexpr bool zones_overlap(const ZoneGeometry &a, const ZoneGeometry &b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

static_assert(!zones_overlap(kZoneGeometry[PAD_BIND_A], kZoneGeometry[PAD_BIND_X]),
              "bind page A and X zones must not overlap");
static_assert(!zones_overlap(kZoneGeometry[PAD_BIND_A], kZoneGeometry[PAD_BIND_Y]),
              "bind page A and Y zones must not overlap");
static_assert(!zones_overlap(kZoneGeometry[PAD_BIND_B], kZoneGeometry[PAD_BIND_X]),
              "bind page B and X zones must not overlap");
static_assert(!zones_overlap(kZoneGeometry[PAD_BIND_B], kZoneGeometry[PAD_BIND_Y]),
              "bind page B and Y zones must not overlap");

// 每个区域上的文字。只有区内的文字需要跟着反色，区外的（START / SELECT）必须
// 保持黑色 —— 它们画在胶囊上方的白底上，跟着反色就整块消失了。
struct ZoneLabelGeometry {
    int x;
    int y;
    int w;
    int h;
    const char *text;
    bool inside_zone;
};

// 为什么 START / SELECT 的文字画在胶囊外面：这两个英文在 zh_font_16 下实测
// 59.5px / 67.7px，而胶囊只有 52px 宽，塞进去会被 LV_LABEL_LONG_CLIP 切掉
// 尾巴（长什么样取决于最右边的字符，"SELECT" 会变成一个认不出来的词）。
// 所以放到胶囊正上方的空白处，几何一点没动，画面还是干净的一排按钮。
constexpr ZoneLabelGeometry kZoneLabelGeometry[PAD_BIND_COUNT] = {
    /* PAD_BIND_A      */ {278, 130, 40, 40, "A", true},
    /* PAD_BIND_B      */ {230, 90, 40, 40, "B", true},
    /* PAD_BIND_X      */ {230, 130, 40, 40, "X", true},
    /* PAD_BIND_Y      */ {278, 90, 40, 40, "Y", true},
    /* PAD_BIND_UP     */ {0, 0, 0, 0, nullptr, false},
    /* PAD_BIND_DOWN   */ {0, 0, 0, 0, nullptr, false},
    /* PAD_BIND_LEFT   */ {0, 0, 0, 0, nullptr, false},
    /* PAD_BIND_RIGHT  */ {0, 0, 0, 0, nullptr, false},
    /* PAD_BIND_START  */ {202, 182, 60, 16, "START", false},
    /* PAD_BIND_SELECT */ {130, 182, 68, 16, "SELECT", false},
};

// ---------------------------------------------------------------- 状态
lv_obj_t *s_root = nullptr;
lv_obj_t *s_pad_status_label = nullptr;
lv_obj_t *s_action_label = nullptr;
lv_obj_t *s_progress_label = nullptr;
lv_obj_t *s_zones[PAD_BIND_COUNT] = {};
lv_obj_t *s_zone_labels[PAD_BIND_COUNT] = {};

bool s_active = false;
int s_current = 0;

// 超时计时起点：0 = 没在计时。按键任务（bind_current）写、ui 任务（update）
// 读，两边不做握手，用自旋锁保证 ESP32 上 int64 不会被读成半个值。
portMUX_TYPE s_capture_start_mux = portMUX_INITIALIZER_UNLOCKED;
int64_t s_capture_start_us = 0;

// LVGL 属性缓存：只在状态和上次不同时才下写样式。
// -1 = 还没画过（配合 s_visual_dirty 用），避免每帧给 LVGL 灌一堆相同样式。
int s_rendered_border[PAD_BIND_COUNT] = {};
int s_rendered_invert[PAD_BIND_COUNT] = {};
bool s_visual_dirty = true;

// ------------------------------------------------------------ 计时读写
void write_capture_start(int64_t value_us)
{
    portENTER_CRITICAL(&s_capture_start_mux);
    s_capture_start_us = value_us;
    portEXIT_CRITICAL(&s_capture_start_mux);
}

int64_t read_capture_start()
{
    int64_t value_us = 0;
    portENTER_CRITICAL(&s_capture_start_mux);
    value_us = s_capture_start_us;
    portEXIT_CRITICAL(&s_capture_start_mux);
    return value_us;
}

bool take_visual_dirty()
{
    const bool dirty = s_visual_dirty;
    s_visual_dirty = false;
    return dirty;
}

// ------------------------------------------------------------ 文本宽度
// 宽度一律用 lv_txt_get_width() 实测：这个字体的 ASCII 实际宽 5~14px/字
// （汉字才是整齐的 16px/字），按 8px 拍脑袋一定会溢出。
size_t utf8_char_bytes(const char *p)
{
    const unsigned char c = (unsigned char)p[0];
    if (c < 0x80) return 1;              // 0xxxxxxx
    if ((c & 0xE0) == 0xC0) return 2;    // 110xxxxx
    if ((c & 0xF0) == 0xE0) return 3;    // 1110xxxx ← 常用汉字在这
    if ((c & 0xF8) == 0xF0) return 4;    // 11110xxx
    return 1;  // 非法首字节：按 1 字节走，保证循环一定前进、不会死循环
}

int text_width_px(const char *text, size_t bytes)
{
    if (!text || bytes == 0) {
        return 0;
    }
    return lv_txt_get_width(text, (uint32_t)bytes, &zh_font_16, 0,
                            LV_TEXT_FLAG_NONE);
}

// 超宽就按 UTF-8 字符边界截断并加省略号，保证屏幕上不出现半个汉字。
void fit_text_width(const char *text, int max_px, char *out, size_t out_bytes)
{
    if (!out || out_bytes == 0) {
        return;
    }
    out[0] = '\0';
    if (!text || text[0] == '\0' || max_px <= 0) {
        return;
    }

    const size_t len = strlen(text);
    if (text_width_px(text, len) <= max_px) {
        snprintf(out, out_bytes, "%s", text);
        return;
    }

    const int marker_w =
        text_width_px(kTruncationMarker, strlen(kTruncationMarker));
    const int avail = max_px - marker_w;
    if (avail <= 0) {
        snprintf(out, out_bytes, "%s", kTruncationMarker);
        return;
    }

    size_t keep = 0;
    int used = 0;
    size_t i = 0;
    while (i < len) {
        const size_t clen = utf8_char_bytes(text + i);
        const int cw = text_width_px(text + i, clen);
        if (used + cw > avail) {
            break;
        }
        used += cw;
        i += clen;
        keep = i;
    }
    snprintf(out, out_bytes, "%.*s%s", (int)keep, text, kTruncationMarker);
}

// 标题行右侧的手柄状态 = pad_state_text() + 手柄名，超宽截断。
//
// 为什么这里用 pad_state_text() 而不是 ui_game.cpp 那份中文映射：本页上半部分
// 只剩 188px，中文状态词（「连接中」等）加上设备名很容易超宽；而且这一行是给
// 用户核对"连的是不是我要的那个手柄"用的，直接显示 BLE 报文里的原始名字最有
// 辨识度，翻译反而添乱。
void format_pad_status(char *out, size_t out_bytes)
{
    const char *state = pad_state_text();
    const char *name = (pad_state() == PAD_STATE_READY) ? pad_name() : "";
    char joined[80] = {};
    if (name && name[0] != '\0') {
        snprintf(joined, sizeof(joined), "%s %s", state, name);
    } else {
        snprintf(joined, sizeof(joined), "%s", state);
    }
    fit_text_width(joined, kPadStatusMaxPx, out, out_bytes);
}

// ------------------------------------------------------------ 对象创建
lv_obj_t *make_frame_object(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) {
        ESP_LOGW(TAG, "%s", kCreateFailedLog);
        return nullptr;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    set_obj_box(obj, x, y, w, h);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    return obj;
}

void create_bind_objects(lv_obj_t *parent)
{
    // s_root：铺满整页的白底不透明容器。必须不透明：墨水屏不整幕擦，底下
    // 的 ROM 列表会透过半透明层糊出来。
    s_root = make_frame_object(parent, 0, 0, kDisplayWidth, kDisplayHeight);
    if (!s_root) {
        return;
    }
    lv_obj_set_style_radius(s_root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_root, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_root, 0, LV_PART_MAIN);

    // 虚拟手柄机身
    lv_obj_t *panel = make_frame_object(s_root, kPanelX, kPanelY, kPanelWidth,
                                        kPanelHeight);
    if (!panel) {
        // 半棵树没画出来也没用了，删干净，下一帧从头再建一次。
        lv_obj_del(s_root);
        s_root = nullptr;
        return;
    }
    lv_obj_set_style_radius(panel, kPanelRadius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, kBorderThin, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_black(), LV_PART_MAIN);

    // 十字键的两条臂：空心（白底黑描边），四个方向的命中框盖在它上面。
    lv_obj_t *bar = make_frame_object(s_root, kDpadBarX, kDpadBarY,
                                      kDpadBarWidth, kDpadBarHeight);
    if (bar) {
        lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, kBorderThin, LV_PART_MAIN);
        lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
    }
    lv_obj_t *column = make_frame_object(s_root, kDpadColumnX, kDpadColumnY,
                                         kDpadColumnWidth, kDpadColumnHeight);
    if (column) {
        lv_obj_set_style_radius(column, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(column, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(column, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(column, kBorderThin, LV_PART_MAIN);
        lv_obj_set_style_border_color(column, lv_color_black(), LV_PART_MAIN);
    }

    // 9 个命中框 + 各自的文字。文字创建在框之后，画在上层。
    for (int i = 0; i < (int)PAD_BIND_COUNT; ++i) {
        const ZoneGeometry &geo = kZoneGeometry[i];
        s_zones[i] = make_frame_object(s_root, geo.x, geo.y, geo.w, geo.h);
        if (!s_zones[i]) {
            continue;
        }
        lv_obj_set_style_radius(s_zones[i], geo.radius, LV_PART_MAIN);
        // 底色必须不透明：这 9 个框压在十字键的臂上，透明的话臂的描边会
        // 从框里透出来，分不清哪一个方向被选中。
        lv_obj_set_style_bg_color(s_zones[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_zones[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_zones[i], kBorderThin, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_zones[i], lv_color_black(), LV_PART_MAIN);
    }
    for (int i = 0; i < (int)PAD_BIND_COUNT; ++i) {
        const ZoneLabelGeometry &geo = kZoneLabelGeometry[i];
        if (!geo.text) {
            continue;
        }
        s_zone_labels[i] = make_centered_label_with_font(s_root,
                                                        geo.x,
                                                        geo.y,
                                                        geo.w,
                                                        geo.h,
                                                        geo.text,
                                                        &zh_font_16,
                                                        kLabelCreateFailedLog);
        if (s_zone_labels[i]) {
            lv_obj_set_style_text_color(s_zone_labels[i], lv_color_black(),
                                        LV_PART_MAIN);
        }
    }

    // 标题 + 右侧手柄状态。标题是一次性静态文本，之后再也不改，所以不用留
    // 静态句柄（要改的文本一律只在 onCreate 时用局部变量，避免留一堆死变量）。
    if (!make_label_with_font(s_root,
                              kTitleX,
                              kTitleY,
                              kTitleWidth,
                              kTitleHeight,
                              kTitleText,
                              &zh_font_16)) {
        ESP_LOGW(TAG, "%s", kLabelCreateFailedLog);
    }

    s_pad_status_label = make_label_with_font(s_root,
                                              kPadStatusX,
                                              kTitleY,
                                              kPadStatusWidth,
                                              kTitleHeight,
                                              "",
                                              &zh_font_16);
    if (!s_pad_status_label) {
        ESP_LOGW(TAG, "%s", kLabelCreateFailedLog);
    } else {
        // 靠右：这一行是"连的是哪个手柄"，名字左对齐会离标题太近。
        lv_obj_set_style_text_align(s_pad_status_label, LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN);
    }

    s_action_label = make_centered_label_with_font(s_root,
                                                   kStatusRowX,
                                                   kActionRowY,
                                                   kStatusRowWidth,
                                                   kStatusRowHeight,
                                                   "",
                                                   &zh_font_16,
                                                   kLabelCreateFailedLog);
    (void)make_centered_label_with_font(s_root,
                                        kStatusRowX,
                                        kHintRowY,
                                        kStatusRowWidth,
                                        kStatusRowHeight,
                                        kHintText,
                                        &zh_font_16,
                                        kLabelCreateFailedLog);
    s_progress_label = make_centered_label_with_font(s_root,
                                                     kStatusRowX,
                                                     kProgressRowY,
                                                     kStatusRowWidth,
                                                     kStatusRowHeight,
                                                     "",
                                                     &zh_font_16,
                                                     kLabelCreateFailedLog);

    // 绑定表从 NVS 懒加载一次（内部幂等）。不调的话：pad_start() 里虽然也会
    // init，但那要等 BLE host 起来；在此之前进绑定页会看到全部"未绑定"，
    // 用户的老绑定像丢了一样。
    pad_bind_init();
}

bool is_bound(pad_bind_action_t action)
{
    return pad_bind_get(action).valid != 0;
}

// 单个元素的样式。只在状态和缓存不同时被调用（见 apply_all_zone_styles）。
void apply_zone_style(int index, bool invert, lv_coord_t border_width)
{
    lv_obj_t *zone = s_zones[index];
    if (!zone) {
        return;
    }
    lv_obj_set_style_bg_color(zone,
                              invert ? lv_color_black() : lv_color_white(),
                              LV_PART_MAIN);
    // bg_opa 不能省：LVGL 默认是 LV_OPA_TRANSP，不设就一个黑底都看不见。
    lv_obj_set_style_bg_opa(zone, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(zone, border_width, LV_PART_MAIN);
    // 反色时描边跟着反成白色：黑描边压在黑底上等于没有，加粗到 4px 也看不
    // 出来（捕获态和已绑定态就没法区分了）。
    lv_obj_set_style_border_color(zone,
                                  invert ? lv_color_white() : lv_color_black(),
                                  LV_PART_MAIN);

    lv_obj_t *label = s_zone_labels[index];
    if (label && kZoneLabelGeometry[index].inside_zone) {
        // 只有压在黑底上的文字才需要反成白色。挂到外面白底上的
        // （START / SELECT）跟着反色就会整个消失。
        lv_obj_set_style_text_color(label,
                                    invert ? lv_color_white() : lv_color_black(),
                                    LV_PART_MAIN);
    }
}

// 返回 true 表示至少一个元素的画法变了（墨水屏据此决定要不要重绘）。
//
// force 由 open() 置位：绑定页可能昨天用过、今天重进，页面上残留的是上一次
// 的画法，而 LVGL 对象本身没错，逐项比较不出来，所以无条件重刷一遍。
// 手柄按下时对应的虚拟键整块反色 —— 这就是用户要的「按键闪烁」。
//
// 为什么用反色而不是真的做周期闪烁：这块屏是 400x300 的单色电子纸，一次整幕
// 刷新要几百毫秒到一秒多，做不出「一亮一灭」的节奏；硬做只会把每次按键变成
// 一次卡住的重绘。反色是这块屏上反馈最快、也最清楚的表达：按下即黑，松开即
// 白，一帧就到位。
bool apply_all_zone_styles(bool capturing)
{
    const bool force = take_visual_dirty();
    bool changed = force;
    for (int i = 0; i < (int)PAD_BIND_COUNT; ++i) {
        const bool selected = (i == s_current);
        // 捕获只可能发生在当前选中项上（bind_current 就是对 s_current 开的）。
        const bool thick = is_bound((pad_bind_action_t)i) ||
                           (capturing && selected);
        const lv_coord_t border = thick ? kBorderThick : kBorderThin;
        // 反色条件从"仅选中"扩成"选中 或 此刻被手柄按着"。
        // pad_bind_action_live() 由绑定表逐包判定，未绑定的动作恒为 false，
        // 所以捕获期间（动作还没绑上）不会跟着乱闪。
        const bool live = pad_bind_action_live((pad_bind_action_t)i);
        const int invert = (selected || live) ? 1 : 0;
        if (!force && s_rendered_border[i] == (int)border &&
            s_rendered_invert[i] == invert) {
            continue;
        }
        apply_zone_style(i, invert != 0, border);
        s_rendered_border[i] = (int)border;
        s_rendered_invert[i] = invert;
        changed = true;
    }
    return changed;
}

// 第 1 行：当前选中的动作 + 它有没有绑过。
// 最长的情况是「当前 返回主页 • 未绑定」（约 166px），离 400px 还很远，
// 所以不用截断。
void format_action_line(char *out, size_t out_bytes)
{
    const pad_bind_action_t action = (pad_bind_action_t)s_current;
    snprintf(out,
             out_bytes,
             "当前 %s %s %s",
             pad_bind_action_name(action),
             kSeparator,
             is_bound(action) ? kBoundText : kUnboundText);
}
} // namespace

void game_bind_page_open()
{
    s_active = true;
    // 选中项复位到第一个：绑定是一次性流程，从 A 开始最顺手，也让上一次
    // 停在半途的页码不会莫名其妙留在那里。
    s_current = 0;
    // 开页前先把上一次可能残留的捕获清掉。不调的话 BLE 层会一直停在
    // "跳过正常解析"的分支里（pad_bind_on_raw_report 只认 WAIT/RELEASE），
    // 表现就是摇杆和按键全部失效，而且日志里没有任何线索。
    pad_bind_capture_cancel();
    write_capture_start(0);
    // 让下一帧无条件重画 9 个元素的样式：退出再进，画布上的缓存已过期。
    s_visual_dirty = true;
}

void game_bind_page_close()
{
    s_active = false;
    pad_bind_capture_cancel();
    write_capture_start(0);
    s_visual_dirty = true;
}

bool game_bind_page_active()
{
    return s_active;
}

void game_bind_page_build(lv_obj_t *parent)
{
    // 每次调用都必须喂一次：手柄服务超过 1.5s 没被喂就停扫描，
    // 绑定页上就永远等不到按键。
    pad_service_notify_visible();

    if (!s_active) {
        // 契约里没有 hide 入口，这里就是唯一的收起来时机：绑定页退出后必须
        // 把整块 s_root 藏好，否则它会一直盖在游戏页上面。
        if (s_root) {
            lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (!parent) {
        return;
    }
    if (!s_root) {
        create_bind_objects(parent);
    }
    if (!s_root) {
        return;
    }
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

bool game_bind_page_update()
{
    pad_service_notify_visible();
    if (!s_root) {
        // 对象还没建出来（父页面没搭好），本帧没什么可刷。
        return false;
    }

    // ---- 推进捕获状态机 ----
    //
    // pad_bind_capture_stage() 内部也会查超时，这里再查一次是为了给出
    // 「超时，请重试」这条人话提示（模块内部只会把状态推到 FAIL）。
    const pad_bind_capture_stage_t stage = pad_bind_capture_stage();
    bool capturing = (stage == PAD_BIND_CAP_WAIT ||
                      stage == PAD_BIND_CAP_RELEASE);
    pad_bind_capture_stage_t effective_stage = stage;
    bool timed_out = false;

    if (capturing) {
        const int64_t start_us = read_capture_start();
        if (start_us != 0 &&
            esp_timer_get_time() - start_us >
                (int64_t)PAD_BIND_CAPTURE_TIMEOUT_MS * 1000) {
            pad_bind_capture_cancel();
            write_capture_start(0);
            effective_stage = PAD_BIND_CAP_FAIL;
            timed_out = true;
            // 已经取消了，这一帧不能再按"还在捕获"去给元素加粗边框。
            capturing = false;
        }
    }

    bool changed = false;

    // 三次取样的进度写进同一行：不显示的话，用户按了一次发现没反应，无从
    // 判断是"还差两次"还是"这一次没被接受"。
    char cycles_line[40] = {};
    if (capturing) {
        const int done = (int)pad_bind_capture_cycles();
        snprintf(cycles_line, sizeof(cycles_line), "%s %d/%d 次",
                 kCycleText, done, (int)PAD_BIND_REQUIRED_CYCLES);
    }

    const char *progress = "";
    switch (effective_stage) {
    case PAD_BIND_CAP_WAIT:
        // 还在等用户按物理键，这时候 cancel 等于把用户的操作毁掉，不调。
        progress = (cycles_line[0] != '\0') ? cycles_line : kWaitText;
        break;
    case PAD_BIND_CAP_RELEASE:
        // 同上：已经在等松手了，cancel 会把这次捕获作废。
        progress = (cycles_line[0] != '\0') ? cycles_line : kReleaseText;
        break;
    case PAD_BIND_CAP_DONE:
        // 终态：必须 cancel 把状态机拉回 IDLE，否则它会永远停在 DONE，
        // BLE 层每次收到报文都会走进"跳过正常解析"的分支，整个手柄失效。
        progress = kBoundText;
        pad_bind_capture_cancel();
        write_capture_start(0);
        break;
    case PAD_BIND_CAP_FAIL:
        // 终态，同理必须 cancel。
        progress = timed_out ? kTimeoutText : kFailText;
        pad_bind_capture_cancel();
        write_capture_start(0);
        break;
    case PAD_BIND_CAP_IDLE:
    default:
        // 没在捕获：这一行改成用手柄连接状态兜底，免得用户对着空白一行
        // 怀疑页面卡了。IDLE 本来就没有东西要取消。
        progress = (pad_state() == PAD_STATE_READY) ? kPadReadyText
                                                    : kPadMissingText;
        break;
    }
    if (set_label_text_if_changed(s_progress_label, progress)) {
        changed = true;
    }

    char action_line[48] = {};
    format_action_line(action_line, sizeof(action_line));
    if (set_label_text_if_changed(s_action_label, action_line)) {
        changed = true;
    }

    char pad_status[80] = {};
    format_pad_status(pad_status, sizeof(pad_status));
    if (set_label_text_if_changed(s_pad_status_label, pad_status)) {
        changed = true;
    }

    // 元素的边框粗细 / 反色放在最后：它依赖上面算出来的 capturing。
    if (apply_all_zone_styles(capturing)) {
        changed = true;
    }
    return changed;
}

bool game_bind_page_next_action()
{
    s_current = (s_current + 1) % (int)PAD_BIND_COUNT;
    return true;
}

bool game_bind_page_bind_current()
{
    const pad_bind_capture_stage_t stage = pad_bind_capture_stage();
    if (stage == PAD_BIND_CAP_WAIT || stage == PAD_BIND_CAP_RELEASE) {
        // 已经在等某个键了，再按一次 = 放弃这次捕获。不做这一步的话，用户
        // 在等待时手一抖按到 KEY，就会把状态机重启一遍、永远卡在第一步。
        pad_bind_capture_cancel();
        write_capture_start(0);
        return true;
    }
    pad_bind_capture_start((pad_bind_action_t)s_current);
    write_capture_start(esp_timer_get_time());
    return true;
}
