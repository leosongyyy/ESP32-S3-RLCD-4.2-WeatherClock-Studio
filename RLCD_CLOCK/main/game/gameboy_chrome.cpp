// 模拟器两侧的 Game Boy 机身装饰栏。设计说明见 gameboy_chrome.h。
#include "gameboy_chrome.h"

#include "app_display_config.h"
#include "app_hardware.h"
#include "game_config.h"
#include "game_audio.h"
#include "gameboy_fb.h"
#include "gameboy_input.h"
#include "battery_runtime_state.h"

#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

constexpr const char *kTag = "GameBoyChrome";

// 侧栏几何：面板 400 宽，GB 窗口 320 宽居中，左右各 40。
constexpr int kBarWidth = 40;
constexpr int kLeftX0 = 0;
constexpr int kRightX0 = game_config::kWindowX + game_config::kWindowWidth; // 360

// ---------------------------------------------------------------- 图元
//
// 位映射与 display_bsp.cpp 的 RLCD_SetPixel() / game_fb_fill_rect() 同一套：
//   index = (x >> 1) * 75 + ((299 - y) >> 2)
//   bit   = 7 - ((((299 - y) & 3) << 1) | (x & 1))
// bit=1 为白。逐像素版本供字库、圆、斜线这些非矩形图元使用。

inline void set_px(int x, int y, bool white)
{
    if (x < 0 || x >= kDisplayWidth || y < 0 || y >= kDisplayHeight) {
        return;
    }
    uint8_t *fb = app_display().RLCD_Framebuffer();
    if (!fb) {
        return;
    }
    const int inv_y = kDisplayHeight - 1 - y;
    const int index =
        (x >> 1) * game_config::kPanelBytesPerColumn + (inv_y >> 2);
    const int bit = 7 - (((inv_y & 3) << 1) | (x & 1));
    if (white) {
        fb[index] = (uint8_t)(fb[index] | (uint8_t)(1u << bit));
    } else {
        fb[index] = (uint8_t)(fb[index] & (uint8_t)~(1u << bit));
    }
}

inline void box(int x0, int y0, int x1, int y1, bool white)
{
    game_fb_fill_rect(x0, y0, x1, y1, white);
}

// 实心圆。r 很小（<=10），直接按行求跨度，不用浮点也不用 sqrt。
void disc(int cx, int cy, int r, bool white)
{
    if (r <= 0) {
        return;
    }
    for (int dy = -r; dy <= r; ++dy) {
        const int rem = r * r - dy * dy;
        if (rem < 0) {
            continue;
        }
        int dx = 0;
        while ((dx + 1) * (dx + 1) <= rem) {
            ++dx;
        }
        box(cx - dx, cy + dy, cx + dx, cy + dy, white);
    }
}

// 圆环：先画白实心，再用背景色挖掉内圈。背景是纯黑，所以挖黑即可。
void ring(int cx, int cy, int r, int thickness, bool white)
{
    disc(cx, cy, r, white);
    disc(cx, cy, r - thickness, false);
}

// Bresenham 直线，给喇叭栅格的斜条用。
void line(int x0, int y0, int x1, int y1, bool white)
{
    int dx = x1 - x0;
    const int dy = y1 - y0;
    const int step_x = dx < 0 ? -1 : 1;
    const int step_y = dy < 0 ? -1 : 1;
    dx = dx < 0 ? -dx : dx;
    const int ady = dy < 0 ? -dy : dy;
    int err = dx - ady;
    int x = x0;
    int y = y0;
    for (;;) {
        set_px(x, y, white);
        if (x == x1 && y == y1) {
            break;
        }
        const int e2 = err * 2;
        if (e2 > -ady) {
            err -= ady;
            x += step_x;
        }
        if (e2 < dx) {
            err += dx;
            y += step_y;
        }
    }
}

// ---------------------------------------------------------------- 5x7 点阵字库
//
// 每个字形 7 行，每行低 5 位，bit4 = 最左列。只有 5x7 才塞得进 40px 宽的
// 侧栏（字距 6px，一行最多 6 个字符）。时间/电量用 scale=2 放大以保证可读，
// 其余标签用 scale=1。
struct Glyph {
    char     ch;
    uint8_t  rows[7];
};

// clang-format off
static const Glyph kFont[] = {
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F}},
    {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x13,0x0F}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {':', {0x00,0x04,0x04,0x00,0x04,0x04,0x00}},
    {'-', {0x00,0x00,0x00,0x0E,0x00,0x00,0x00}},
    {'/', {0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
    {'%', {0x19,0x1A,0x02,0x04,0x08,0x0B,0x13}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
};
// clang-format on

const Glyph *glyph_of(char c)
{
    // 小写按大写处理：字库里只有一套大写。
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    for (size_t i = 0; i < sizeof(kFont) / sizeof(kFont[0]); ++i) {
        if (kFont[i].ch == c) {
            return &kFont[i];
        }
    }
    return NULL;
}

constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;
constexpr int kPitch = 6;      // 5 列 + 1 列间隔

// 返回画完后的总宽度（不含末尾间隔）。
int text(int x, int y, const char *s, bool white, int scale)
{
    int cx = x;
    for (const char *p = s; *p != '\0'; ++p) {
        const Glyph *g = glyph_of(*p);
        if (g != NULL) {
            for (int r = 0; r < kGlyphH; ++r) {
                const uint8_t bits = g->rows[r];
                for (int c = 0; c < kGlyphW; ++c) {
                    if ((bits & (uint8_t)(1u << (4 - c))) == 0u) {
                        continue;
                    }
                    if (scale <= 1) {
                        set_px(cx + c, y + r, white);
                    } else {
                        box(cx + c * scale, y + r * scale,
                            cx + c * scale + scale - 1,
                            y + r * scale + scale - 1, white);
                    }
                }
            }
        }
        cx += kPitch * scale;
    }
    return cx - x - (scale > 1 ? 0 : 1);
}

// 实际占位宽度：n 个字符之间只有 (n-1) 个间隔，末字符只算字形本身那 5 列。
// 写成 n*kPitch 会让居中偏右半个字（scale=2 时偏 1px）。
int text_width(const char *s, int scale)
{
    const int n = (int)strlen(s);
    return n <= 0 ? 0 : scale * ((n - 1) * kPitch + kGlyphW);
}

// 在侧栏内居中画一行。x0 是侧栏左边界。
void text_row(int bar_x0, int y, const char *s, bool white, int scale)
{
    int w = text_width(s, scale);
    // ⚠ 兜底：字符串超出侧栏宽度时先降回 1x，还不能放下就从左边界起画。
    //   绝不能让它画到侧栏外面去 —— GB 窗口就在紧邻的 40px 处，越界等于
    //   把字符糊在游戏画面上，而且每帧刷新时会被 GB 内容盖掉一半，看着
    //   像随机噪点，极难排查。
    if (w > kBarWidth - 4 && scale > 1) {
        scale = 1;
        w = text_width(s, scale);
    }
    int x = bar_x0 + (kBarWidth - w) / 2;
    if (x < bar_x0 + 2) {
        x = bar_x0 + 2;
    }
    text(x, y, s, white, scale);
}

// ---------------------------------------------------------------- 装饰件

// DMG 的「点阵」底纹：2x2 白点，横竖都留间距，把剩余竖向空间填满而不显乱。
void dot_field(int bar_x0, int y0, int y1)
{
    for (int y = y0; y <= y1 - 1; y += 8) {
        for (int x = bar_x0 + 6; x <= bar_x0 + 33; x += 6) {
            box(x, y, x + 1, y + 1, true);
        }
    }
}

// 电池：外壳 + 右侧极头 + 5 段电量格。
void battery_icon(int bar_x0, int y0, int percent)
{
    const int x0 = bar_x0 + 8;
    const int x1 = bar_x0 + 30;
    const int y1 = y0 + 12;                       // 外壳 14 高
    box(x0, y0, x1, y1, true);                    // 白壳
    box(x1 + 1, y0 + 4, x1 + 4, y1 - 4, true);    // 极头
    box(x0 + 2, y0 + 2, x1 - 2, y1 - 2, false);   // 挖空

    int level = 0;
    if (percent > 0) {
        level = (percent + 19) / 20;              // 1..5
        if (level > 5) {
            level = 5;
        }
    }
    // 内腔是 x0+2 .. x1-2 共 19px，5 格每格 3px 夹 1px 缝刚好铺满。
    // 起点必须是 x0+2（内腔左壁），写成 +3 的话最后一格会压到右壳壁上。
    for (int i = 0; i < level; ++i) {
        const int sx = x0 + 2 + i * 4;
        box(sx, y0 + 3, sx + 2, y1 - 3, true);
    }
}

// GB 十字方向键：白外框 + 黑内芯 + 中心定位点。
void dpad(int bar_x0, int y0)
{
    const int cx = bar_x0 + 20;
    box(bar_x0 + 4, y0 + 10, bar_x0 + 35, y0 + 22, true);   // 横臂
    box(cx - 6, y0, cx + 6, y0 + 32, true);                 // 竖臂
    box(bar_x0 + 6, y0 + 12, bar_x0 + 33, y0 + 20, false);
    box(cx - 4, y0 + 2, cx + 4, y0 + 30, false);
    box(cx - 2, y0 + 14, cx + 1, y0 + 17, true);            // 中心点
}

// A/B 键：DMG 上 B 在左下、A 在右上，两个圆钮中间刻字母。
void ab_buttons(int bar_x0, int y0)
{
    const int bx = bar_x0 + 13;
    const int by = y0 + 18;
    const int ax = bar_x0 + 29;
    const int ay = y0 + 6;
    // r=7：两个圆钮各占 15px，放在 40px 宽的栏里刚好留出两侧余量。
    // r=8 时 B 的右缘和 A 的左缘会在 x=381 相切，两个圈糊成一个。
    ring(bx, by, 7, 2, true);
    ring(ax, ay, 7, 2, true);
    // 字母用 1x 字库：圆钮内径只有 12px，2x 会溢出。
    text(bx - 2, by - 3, "B", true, 1);
    text(ax - 2, ay - 3, "A", true, 1);
}

// 手柄小图标：外壳 + 两颗按键 + 中间方向键。
void pad_icon(int bar_x0, int y0, bool connected)
{
    const int x0 = bar_x0 + 7;
    const int x1 = bar_x0 + 32;
    const int y1 = y0 + 13;
    box(x0, y0, x1, y1, true);
    box(x0 + 2, y0 + 2, x1 - 2, y1 - 2, false);
    // 左十字 + 右两键，手柄的通用剪影。
    box(x0 + 4, y0 + 5, x0 + 10, y0 + 7, true);
    box(x0 + 6, y0 + 3, x0 + 8, y0 + 9, true);
    box(x1 - 12, y0 + 4, x1 - 9, y0 + 7, true);
    box(x1 - 7, y0 + 6, x1 - 4, y0 + 9, true);
    if (!connected) {
        // 未连接：盖一道斜杠，比换个空壳更容易一眼看出状态。
        line(x0 + 1, y1 - 1, x1 - 1, y0 + 1, true);
    }
}

// DMG 右下角的喇叭栅格：6 条平行斜线。
void speaker_grille(int bar_x0, int y_top, int y_bottom)
{
    for (int i = 0; i < 6; ++i) {
        const int sx = bar_x0 + 6 + i * 4;
        line(sx, y_bottom, sx + 8, y_top, true);
    }
}

// 音量格：5 格，step 决定点亮几格（step 0 = 静音，全灭）。
void volume_bars(int bar_x0, int y0, int step)
{
    for (int i = 0; i < 5; ++i) {
        const int sx = bar_x0 + 5 + i * 6;
        box(sx, y0, sx + 4, y0 + 8, i < step);
        if (i >= step) {
            // 熄灭的格子留个 1px 白边，否则黑底上完全看不见有 5 档。
            box(sx, y0, sx + 4, y0, true);
            box(sx, y0 + 8, sx + 4, y0 + 8, true);
            box(sx, y0, sx, y0 + 8, true);
            box(sx + 4, y0, sx + 4, y0 + 8, true);
        }
    }
}

// ---------------------------------------------------------------- 数据

struct Snapshot {
    int      battery;      // -1 表示未知
    bool     pad_connected;
    int      volume_step;
    int      hour;
    int      minute;
    int      month;
    int      day;
    int      wday;
    bool     time_valid;
};

Snapshot take_snapshot(void)
{
    Snapshot s{};
    s.battery = battery_percent_load();
    s.pad_connected = gameboy_pad_connected();
    s.volume_step = game_audio_volume_step();

    const time_t now = time(NULL);
    struct tm tm_buf;
    if (now > 0 && localtime_r(&now, &tm_buf) != NULL &&
        tm_buf.tm_year + 1900 >= 2020) {
        s.hour = tm_buf.tm_hour;
        s.minute = tm_buf.tm_min;
        s.month = tm_buf.tm_mon + 1;
        s.day = tm_buf.tm_mday;
        s.wday = tm_buf.tm_wday;
        s.time_valid = true;
    }
    return s;
}

// 只有这些字段变了才需要重画 + 重推。电池按 5% 一档量化，避免 ADC 抖动
// 让侧栏每分钟重推好几次（一次 5ms 的 SPI 对模拟器帧率不是免费的）。
uint32_t signature_of(const Snapshot &s)
{
    const uint32_t bat = (s.battery < 0) ? 0u : (uint32_t)(s.battery / 5);
    return ((uint32_t)(s.hour & 0x1F) << 27) |
           ((uint32_t)(s.minute & 0x3F) << 21) |
           ((uint32_t)(s.month & 0x0F) << 17) |
           ((uint32_t)(s.day & 0x1F) << 12) |
           (bat << 5) |
           ((uint32_t)(s.volume_step & 0x0F) << 1) |
           (s.pad_connected ? 1u : 0u);
}

const char *weekday_name(int wday)
{
    static const char *kNames[] = {"SUN", "MON", "TUE", "WED",
                                   "THU", "FRI", "SAT"};
    if (wday < 0 || wday > 6) {
        return "---";
    }
    return kNames[wday];
}

void draw_left_bar(const Snapshot &s)
{
    const int x0 = kLeftX0;

    // ---- 电量 ----
    battery_icon(x0, 8, s.battery);
    char buf[8];
    if (s.battery >= 100) {
        // ⚠ 不能再带 "%"：4 个字符在 2x 下宽 46px，比侧栏还宽，会画进游戏
        //   画面里（tests/chrome_preview.py 的自检就是被这一条抓出来的）。
        //   三位数时省掉百分号 —— 上面就是电池图标，语义不会丢。
        snprintf(buf, sizeof(buf), "100");
    } else if (s.battery >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", s.battery);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    // 百分比用 2x：这是侧栏里最该一眼看清的数字。
    text_row(x0, 26, buf, true, 2);

    // ---- 十字键 ----
    dpad(x0, 42);

    // ---- 日期 ----
    if (s.time_valid) {
        text_row(x0, 88, weekday_name(s.wday), true, 1);
        snprintf(buf, sizeof(buf), "%02d/%02d", s.month, s.day);
    } else {
        text_row(x0, 88, "---", true, 1);
        snprintf(buf, sizeof(buf), "--/--");
    }
    text_row(x0, 98, buf, true, 1);

    // ---- 机身丝印 ----
    text_row(x0, 126, "GAME", true, 1);
    text_row(x0, 136, "BOY", true, 1);

    // ---- 点阵底纹 ----
    dot_field(x0, 162, 286);
}

void draw_right_bar(const Snapshot &s)
{
    const int x0 = kRightX0;

    // ---- 时间 ----
    // 2x 的 "HH:MM" 是 5*12 = 60px，塞不进 40px 宽的侧栏，所以拆成两行
    // 上下排：小时在上、分钟在下，中间一道分隔线。这是窄条里唯一还能
    // 保证可读的排法。
    char buf[8];
    if (s.time_valid) {
        snprintf(buf, sizeof(buf), "%02d", s.hour);
        text_row(x0, 8, buf, true, 2);
        snprintf(buf, sizeof(buf), "%02d", s.minute);
        text_row(x0, 26, buf, true, 2);
    } else {
        text_row(x0, 8, "--", true, 2);
        text_row(x0, 26, "--", true, 2);
    }
    box(x0 + 12, 23, x0 + 27, 23, true);

    // ---- A/B 键 ----
    ab_buttons(x0, 40);

    // ---- 手柄连接 ----
    pad_icon(x0, 76, s.pad_connected);
    text_row(x0, 94, "PAD", true, 1);
    text_row(x0, 104, s.pad_connected ? "ON" : "OFF", true, 1);

    // ---- 喇叭栅格 ----
    speaker_grille(x0, 132, 168);

    // ---- 音量 ----
    text_row(x0, 186, "VOL", true, 1);
    volume_bars(x0, 196, s.volume_step);

    // ---- 点阵底纹 ----
    dot_field(x0, 220, 286);
}

uint32_t s_last_signature = 0xFFFFFFFFu;
bool     s_have_signature = false;

} // namespace

void game_chrome_draw_all(void)
{
    if (!app_display().RLCD_Framebuffer()) {
        ESP_LOGW(kTag, "panel framebuffer unavailable, chrome skipped");
        return;
    }
    const Snapshot s = take_snapshot();
    // 侧栏底底色：整屏已经清成黑，这里只负责画内容。
    box(kLeftX0, 0, kLeftX0 + kBarWidth - 1, kDisplayHeight - 1, false);
    box(kRightX0, 0, kRightX0 + kBarWidth - 1, kDisplayHeight - 1, false);
    draw_left_bar(s);
    draw_right_bar(s);
    s_last_signature = signature_of(s);
    s_have_signature = true;
}

void game_chrome_present(void)
{
    DisplayPort &display = app_display();
    // 两条侧栏各自是连续的面板列区间：x 0..39 -> byte_x 0..19，
    // x 360..399 -> byte_x 180..199。见 RLCD_DisplayXRange() 的实现。
    display.RLCD_DisplayXRange(0, (uint16_t)(kBarWidth - 1));
    display.RLCD_DisplayXRange((uint16_t)kRightX0,
                               (uint16_t)(kRightX0 + kBarWidth - 1));
}

bool game_chrome_refresh(bool force)
{
    if (!app_display().RLCD_Framebuffer()) {
        return false;
    }
    const Snapshot s = take_snapshot();
    const uint32_t sig = signature_of(s);
    if (!force && s_have_signature && sig == s_last_signature) {
        return false;   // 内容没变，一次 SPI 都不发
    }
    box(kLeftX0, 0, kLeftX0 + kBarWidth - 1, kDisplayHeight - 1, false);
    box(kRightX0, 0, kRightX0 + kBarWidth - 1, kDisplayHeight - 1, false);
    draw_left_bar(s);
    draw_right_bar(s);
    s_last_signature = sig;
    s_have_signature = true;
    game_chrome_present();
    return true;
}
