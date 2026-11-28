// 天气时钟第一屏的「时间与日历」版式。
//
// 整页写进一张 400x300 的 LVGL canvas：状态栏 + 题头 + 大像素时间 +
// 黑底日期卡 + 模拟表盘 + 当月月历 + 操作栏。所有坐标与
// tests/time_calendar_preview.py 一一对应，改版式时两边一起改。
//
// 中英文都用主菜单那套 Fusion Pixel 12 取模字库（12px），
// 大字时间用 DSEG7 Classic 36px 字模按 6/5 放大，保持计算器七段风格。
#include "ui_time_calendar.h"

#include "app_display_config.h"
#include "battery_runtime_state.h"
#include "book/book_font.h"      // /sdcard/font 全量中文字库，补内置子集缺的字
#include "calendar_lunar.h"
#include "engine_api.h"          // pad_state()，状态栏蓝牙图标
#include "local_sensor_state.h"  // 本地温湿度
#include "network/weather_state.h"
#include "ui_canvas_primitives.h"
#include "wifi_portal_state.h"

#include "assets/dseg_digits.h"
#include "assets/fusion_pixel_12_font.h"
#include "assets/menu_icons.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

constexpr int W = kDisplayWidth;    // 400
constexpr int H = kDisplayHeight;   // 300
constexpr char TAG[] = "ui_time_cal";

lv_obj_t *s_canvas = nullptr;
lv_color_t *s_buf = nullptr;
lv_obj_t *s_parent = nullptr;
int s_last_day_key = -1;
int s_last_minute_key = -1;
int s_last_second_key = -1;
int s_last_battery = -999;
uint32_t s_last_weather_version = 0;
int s_page_index = 0;  // 0=时间与日历，1=天气时钟第二页

// ---------- 版式常量（与预览脚本一致） ----------
constexpr int STATUS_H = 32;          // 状态栏 + 底部 3px 黑线
constexpr int TITLE_TOP = 40;         // 题头文字顶（scale=1，与书库「BOOKS 书库」同号）
constexpr int TITLE_RULE_Y = 56;      // 题头下的分隔线
constexpr int TITLE_RULE_H = 1;      // 与书库页 hline(y=56) 同为 1px
constexpr int TIME_ROW_Y = 62;        // 时间行
constexpr int TIME_ROW_H = 74;
constexpr int TIME_LEFT = 8;          // 时间区左边界
constexpr int CARD_X0 = 256;          // 黑底日期卡
constexpr int CARD_X1 = 392;
constexpr int LOWER_Y = 140;          // 下半区（表盘 + 月历）
constexpr int DIAL_CX = 68;
constexpr int DIAL_CY = 210;
constexpr int DIAL_R = 58;
constexpr int CAL_ROWS = 6;
constexpr int CAL_COLW = 37;          // 列宽保持改版前的大小
constexpr int CAL_ROW_H = 20;          // 行高比原来高 2px，数字落在格子正中
constexpr int CAL_HEADER_H = 13;
constexpr int CAL_X0 = 132;           // 表盘右侧留 6px
constexpr int CAL_Y0 = LOWER_Y;
constexpr int BAR_Y = 282;            // 操作栏分隔线

// 大字时间用 DSEG7 Classic 断码字模（dseg_digits.h，与 Keshikan DSEG7 Classic 同源）。
// DSEG36 数字 23x36、步进 30；按 6/5 定点放大到字高 43px、总宽 234px，
// 正好占满左上角时间区（x8..250，宽 243），白底黑字不反白。
constexpr int DSEG_SCALE_NUM = 6;
constexpr int DSEG_SCALE_DEN = 5;
constexpr char kDsegChars[] = "0123456789:";

constexpr int kIconCx[5] = {170, 225, 300, 322, 365};

const char *const kMonthEn[12] = {
    "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
    "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER",
};
const char *const kWeekdayEnShort[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *const kWeekdayZh[7] = {
    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六",
};
// 月历表头从周一开始，周六周日排在最后两列，画成反白。
const char *const kMonthHeaderMon[7] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
constexpr int kWeekendFirstCol = 5;   // SAT

static_assert(CAL_X0 >= 130 && CAL_X0 + CAL_COLW * 7 - 1 <= W - 1,
              "month grid must sit right of the dial and inside the display");
static_assert(CAL_Y0 + CAL_HEADER_H + CAL_ROWS * CAL_ROW_H <= BAR_Y,
              "month grid must not run into the action bar");
static_assert(DIAL_CY + DIAL_R < BAR_Y && DIAL_CY - DIAL_R >= LOWER_Y,
              "dial must stay inside the lower area");

// ---------- 画点原语 ----------
// 直接写帧缓冲，不走 lv_canvas_set_px：那个每点都会把整块 canvas 标脏。
inline void put(int x, int y, bool ink = true)
{
    if (!s_buf || x < 0 || x >= W || y < 0 || y >= H) return;
    s_buf[y * W + x] = ink ? lv_color_black() : lv_color_white();
}

void fill_rect(int x0, int y0, int x1, int y1, bool ink = true)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) put(x, y, ink);
}

void rect_frame(int x0, int y0, int x1, int y1, bool ink = true)
{
    for (int x = x0; x <= x1; ++x) { put(x, y0, ink); put(x, y1, ink); }
    for (int y = y0; y <= y1; ++y) { put(x0, y, ink); put(x1, y, ink); }
}

void hline(int x0, int x1, int y, bool ink = true)
{
    for (int x = x0; x <= x1; ++x) put(x, y, ink);
}

void vline(int x, int y0, int y1, bool ink = true)
{
    for (int y = y0; y <= y1; ++y) put(x, y, ink);
}

// 圆角实心矩形（按键帽），1bit 下只切四个角。
void rounded_fill(int x0, int y0, int x1, int y1, int r, bool ink = true)
{
    fill_rect(x0 + r, y0, x1 - r, y1, ink);
    fill_rect(x0, y0 + r, x1, y1 - r, ink);
    for (int dy = 0; dy < r; ++dy) {
        const int span = r - 1 - dy;
        hline(x0 + r - span, x1 - r + span, y0 + dy, ink);
        hline(x0 + r - span, x1 - r + span, y1 - dy, ink);
    }
}

// 任意方向线段（Bresenham），width 为横向粗度。
void thick_line(int x0, int y0, int x1, int y1, int width, bool ink = true)
{
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        const int half = width / 2;
        fill_rect(x0 - half, y0 - half, x0 - half + width - 1, y0 - half + width - 1, ink);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// 圆环：dy 行上算 x 跨度，用 span 保证 1px 也能闭上。
void circle_ring(int cx, int cy, int r, int thickness, bool ink = true)
{
    for (int y = -r; y <= r; ++y) {
        const int inner = r - thickness;
        const int x_out = static_cast<int>(std::sqrt(static_cast<double>(r * r - y * y)) + 0.5);
        if (inner > 0 && std::abs(y) <= inner) {
            const int x_in = static_cast<int>(
                std::sqrt(static_cast<double>(inner * inner - y * y)) + 0.5);
            hline(cx + x_in, cx + x_out, cy + y, ink);
            hline(cx - x_out, cx - x_in, cy + y, ink);
        } else {
            hline(cx - x_out, cx + x_out, cy + y, ink);
        }
    }
}

void fill_circle(int cx, int cy, int r, bool ink = true)
{
    for (int y = -r; y <= r; ++y) {
        const int x = static_cast<int>(std::sqrt(static_cast<double>(r * r - y * y)) + 0.5);
        hline(cx - x, cx + x, cy + y, ink);
    }
}

// ---------- 12px 像素字 ----------
struct Glyph12 {
    uint16_t cp;
    uint16_t off;
    uint8_t w, h, xoff, yoff, dw, row_bytes;
};

const fusion_pixel_12_glyph_t *find_builtin_glyph(uint32_t cp)
{
    int lo = 0, hi = FUSION_PIXEL_12_INDEX_COUNT - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint32_t cur = fusion_pixel_12_index[mid].cp;
        if (cur == cp) return &fusion_pixel_12_index[mid];
        if (cur < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

// 统一的内置 / TF 卡字形视图。内置字库的 yoff 相对基线，卡上 FPX1 的
// ytop 相对行顶，两者换算方式不同，用 line_top 区分。
struct Glyph12Ref {
    int w, h, xoff, yoff, dw, row_bytes;
    const uint8_t *bits;
    bool line_top;
};

Glyph12Ref s_glyph_view = {};

const Glyph12Ref *find_glyph12(uint32_t cp)
{
    // 中文走卡上 19450 字全库，缺字时再落回固件内置子集。
    // ASCII 与标点继续走内置字库，保证和主菜单的英数字形完全一致。
    if (cp >= 0x4E00 && cp <= 0x9FFF && book_font_ready()) {
        const BookGlyph *g = book_font_glyph(cp);
        if (g) {
            s_glyph_view = {g->w, g->h, g->xoff, g->ytop, g->dw,
                            g->row_bytes, g->bits, true};
            return &s_glyph_view;
        }
    }
    const fusion_pixel_12_glyph_t *g = find_builtin_glyph(cp);
    if (!g) return nullptr;
    s_glyph_view = {g->w, g->h, g->xoff, g->yoff, g->dw,
                    g->row_bytes, fusion_pixel_12_bits + g->off, false};
    return &s_glyph_view;
}

// 字形墨迹顶端的 y 坐标（top 是英文大写顶部，即行顶 + ascent）。
int glyph_top12(const Glyph12Ref &g, int top, int scale)
{
    constexpr int ascent = 9;
    return g.line_top ? top + g.yoff * scale
                      : (top + ascent * scale) - (g.yoff + g.h) * scale;
}

uint32_t utf8_next(const char **p)
{
    const auto *s = reinterpret_cast<const unsigned char *>(*p);
    uint32_t cp = s[0];
    int n = 1;
    if (s[0] < 0x80) {
        n = 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        cp = s[0] & 0x1F; n = 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = s[0] & 0x0F; n = 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        cp = s[0] & 0x07; n = 4;
    }
    for (int i = 1; i < n; ++i) cp = (cp << 6) | (s[i] & 0x3F);
    *p = reinterpret_cast<const char *>(s + n);
    return cp;
}

void blit_glyph(const Glyph12Ref *g, int x, int top,
                int scale, bool ink, bool bold = false)
{
    if (!g || g->row_bytes == 0) return;
    const int glyph_top = glyph_top12(*g, top, scale);
    for (int r = 0; r < g->h; ++r) {
        for (int col = 0; col < g->w; ++col) {
            const uint8_t byte = g->bits[r * g->row_bytes + col / 8];
            if (byte & (1 << (7 - (col % 8)))) {
                const int px = x + (g->xoff + col) * scale;
                fill_rect(px, glyph_top + r * scale,
                          px + (bold ? 1 : 0), glyph_top + r * scale + scale - 1,
                          ink);
            }
        }
    }
}

// 主字库 12px，top 是英文大写顶部。返回末端 x。
// bold 只加粗墨迹（每个点向右多画 1px），不改变字距。
int text12(int x, int top, const char *s, int scale = 1, int extra = 0,
           bool ink = true, bool bold = false)
{
    const char *p = s;
    int cx = x;
    while (*p) {
        const uint32_t cp = utf8_next(&p);
        const Glyph12Ref *g = find_glyph12(cp);
        if (g) {
            blit_glyph(g, cx, top, scale, ink, bold);
            cx += g->dw * scale + extra;
        } else {
            cx += 6 * scale + extra;
        }
    }
    return cx;
}

int text_width12(const char *s, int scale = 1, int extra = 0)
{
    const char *p = s;
    int adv = 0;
    while (*p) {
        const uint32_t cp = utf8_next(&p);
        const Glyph12Ref *g = find_glyph12(cp);
        adv += (g ? g->dw : 6) * scale + extra;
    }
    return adv;
}

int text12_center(int cx, int top, const char *s, int scale = 1,
                  int extra = 0, bool ink = true, bool bold = false)
{
    return text12(cx - text_width12(s, scale, extra) / 2, top, s,
                  scale, extra, ink, bold);
}

struct InkBounds {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool valid = false;
};

void grow_bounds(InkBounds &b, int x, int y)
{
    if (!b.valid) {
        b.x0 = b.x1 = x;
        b.y0 = b.y1 = y;
        b.valid = true;
        return;
    }
    if (x < b.x0) b.x0 = x;
    if (x > b.x1) b.x1 = x;
    if (y < b.y0) b.y0 = y;
    if (y > b.y1) b.y1 = y;
}

// 计算 text12 从 (0,0) 画 S 时的实际墨迹边界，用于按格子居中。
// 步进宽度和墨迹边界不是一回事，尤其加粗后右侧还会多 1px。
InkBounds text12_ink_bounds(const char *s, int scale = 1, int extra = 0,
                            bool bold = false)
{
    InkBounds b;
    const char *p = s;
    int cx = 0;
    while (*p) {
        const uint32_t cp = utf8_next(&p);
        const Glyph12Ref *g = find_glyph12(cp);
        if (!g) {
            cx += 6 * scale + extra;
            continue;
        }
        const int glyph_top = glyph_top12(*g, 0, scale);
        for (int r = 0; r < g->h; ++r) {
            for (int col = 0; col < g->w; ++col) {
                const uint8_t byte = g->bits[r * g->row_bytes + col / 8];
                if ((byte & (1 << (7 - (col % 8)))) == 0) continue;
                const int px = cx + (g->xoff + col) * scale;
                const int py = glyph_top + r * scale;
                grow_bounds(b, px, py);
                if (bold) grow_bounds(b, px + 1, py);
            }
        }
        cx += g->dw * scale + extra;
    }
    return b;
}

// 在 [x0..x1] x [y0..y1] 格子里按实际墨迹上下左右居中。
void text12_center_in_box(int x0, int y0, int x1, int y1, const char *s,
                          int scale = 1, int extra = 0, bool ink = true,
                          bool bold = false)
{
    const InkBounds b = text12_ink_bounds(s, scale, extra, bold);
    if (!b.valid) return;
    const int draw_x = x0 + (x1 - x0 + 1 - (b.x1 - b.x0 + 1)) / 2 - b.x0;
    const int draw_y = y0 + (y1 - y0 + 1 - (b.y1 - b.y0 + 1)) / 2 - b.y0;
    text12(draw_x, draw_y, s, scale, extra, ink, bold);
}

// ---------- 图标 ----------
const menu_icon_t *find_icon(const char *name)
{
    for (int i = 0; i < MENU_ICON_COUNT; ++i)
        if (std::strcmp(kMenuIcons[i].name, name) == 0) return &kMenuIcons[i];
    return nullptr;
}

void draw_icon(const char *name, int x, int y, bool ink = true)
{
    const auto *ic = find_icon(name);
    if (!ic) return;
    for (int row = 0; row < ic->h; ++row) {
        for (int b = 0; b < ic->stride; ++b) {
            const uint8_t byte = ic->bits[row * ic->stride + b];
            for (int bit = 0; bit < 8; ++bit) {
                if (byte & (1 << (7 - bit))) put(x + b * 8 + bit, y + row, ink);
            }
        }
    }
}

// ---------- 状态栏（与主菜单同款） ----------
bool wifi_connected()
{
    char ip[64] = {};
    return wifi_station_ip_snapshot(ip, sizeof(ip));
}

void draw_status_bar(const struct tm *local, int battery_percent)
{
    char buf[32];
    const int hh = local ? local->tm_hour : 9;
    const int mm = local ? local->tm_min : 41;
    const int year = local ? local->tm_year + 1900 : 2026;
    const int mon = local ? local->tm_mon + 1 : 11;
    const int day = local ? local->tm_mday : 24;
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
    int cursor = text12(6, 6, buf, 2);
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, mon, day);
    text12(cursor + 8, 10, buf, 1, -1);

    float temperature = 0.0f, humidity = 0.0f;
    const bool sensor_ok = get_local_sensor_snapshot(&temperature, &humidity,
                                                     nullptr, nullptr);
    char tbuf[16], hbuf[16];
    if (sensor_ok) {
        std::snprintf(tbuf, sizeof(tbuf), "%.0f°C", temperature);
        std::snprintf(hbuf, sizeof(hbuf), "%.0f%%", humidity);
    } else {
        std::snprintf(tbuf, sizeof(tbuf), "--°C");
        std::snprintf(hbuf, sizeof(hbuf), "--%%");
    }
    for (int i = 0; i < 2; ++i) {
        const char *icon = i == 0 ? "thermometer_sun_18" : "droplets_18";
        const char *text = i == 0 ? tbuf : hbuf;
        const int cx = kIconCx[i];
        const int group_w = 18 + 2 + text_width12(text);
        const int sx = cx - group_w / 2;
        draw_icon(icon, sx, 7);
        text12(sx + 20, 11, text);
    }
    draw_icon(wifi_connected() ? "wifi_18" : "wifi_off_18", kIconCx[2] - 9, 7);
    draw_icon(pad_state() == PAD_STATE_READY ? "bluetooth_18" : "bluetooth_off_18",
              kIconCx[3] - 9, 7);

    char pctbuf[12];
    const int pct = battery_percent < 0 ? 75 : battery_percent;
    std::snprintf(pctbuf, sizeof(pctbuf), "%d%%", pct);
    const int group_w = 28 + 3 + text_width12(pctbuf);
    const int bx = kIconCx[4] - group_w / 2;
    rect_frame(bx, 10, bx + 24, 22);
    fill_rect(bx + 25, 14, bx + 27, 18);
    const int filled = (pct >= 75) ? 4 : (pct >= 50) ? 3 : (pct >= 25) ? 2 : (pct > 0) ? 1 : 0;
    for (int k = 0; k < 4; ++k) {
        const int qx = bx + 2 + k * 5;
        if (k < filled) fill_rect(qx, 12, qx + 3, 20);
        else rect_frame(qx, 12, qx + 3, 20);
    }
    text12(bx + 31, 12, pctbuf);

    fill_rect(0, STATUS_H, W - 1, STATUS_H + 2);
}

// ---------- 题头 ----------
// 字号与书库页的「BOOKS 书库」一致：12px 西文 + 12px 中文，标题不再用放大字。
void draw_title()
{
    const int x = text12(10, TITLE_TOP, "TIME & CALENDAR");
    text12(x + 5, TITLE_TOP, "时间与日历");
    fill_rect(0, TITLE_RULE_Y, W - 1, TITLE_RULE_Y + TITLE_RULE_H - 1);
}

// ---------- 时间 + 日期卡 ----------
// DSEG36 字库只有 0-9 和冒号，没有负号；负温用数字中部一条横线表示。
// 坐标按 6/5 放大后给出，相对文本原点 x=0、baseline=0。
constexpr int kDsegMinusX = 6;
constexpr int kDsegMinusW = 20;
constexpr int kDsegMinusY = -24;
constexpr int kDsegMinusH = 5;
constexpr int kDsegMinusAdv = 32;

// 按 6/5 放大后的实际墨迹边界（相对文本原点 x=0、baseline=0）。
// DSEG 字模的 x_offset/墨迹宽度和 x_advance 不一致，居中必须按墨迹算。
InkBounds dseg_text_ink_bounds(const char *s)
{
    InkBounds b;
    int cx = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == '-') {
            for (int tx = 0; tx < kDsegMinusW; ++tx) {
                for (int ty = 0; ty < kDsegMinusH; ++ty) {
                    grow_bounds(b, cx + kDsegMinusX + tx, kDsegMinusY + ty);
                }
            }
            cx += kDsegMinusAdv;
            continue;
        }
        const char *hit = std::strchr(kDsegChars, *p);
        if (!hit) continue;
        const int idx = static_cast<int>(hit - kDsegChars);
        const DsegGlyph &g = kDSEG36Glyphs[idx];
        const uint8_t *bits = kDSEG36Bitmaps + g.bitmap_offset;
        const int gx = cx + g.x_offset * DSEG_SCALE_NUM / DSEG_SCALE_DEN;
        const int gy = g.y_offset * DSEG_SCALE_NUM / DSEG_SCALE_DEN;
        const int gw = g.width * DSEG_SCALE_NUM / DSEG_SCALE_DEN;
        const int gh = g.height * DSEG_SCALE_NUM / DSEG_SCALE_DEN;
        for (int ty = 0; ty < gh; ++ty) {
            const int sy = ty * DSEG_SCALE_DEN / DSEG_SCALE_NUM;
            for (int tx = 0; tx < gw; ++tx) {
                const int sx = tx * DSEG_SCALE_DEN / DSEG_SCALE_NUM;
                const uint32_t bit = static_cast<uint32_t>(sy) * g.width + sx;
                if ((bits[bit / 8] & (0x80U >> (bit % 8))) == 0) continue;
                grow_bounds(b, gx + tx, gy + ty);
            }
        }
        cx += g.x_advance * DSEG_SCALE_NUM / DSEG_SCALE_DEN;
    }
    return b;
}

// 按 num/den 最近邻放大 DSEG 字模，直接写 canvas（白底黑字）。
void draw_dseg_glyph(const DsegGlyph &g, const uint8_t *bits,
                     int x, int baseline)
{
    const int gw = g.width * DSEG_SCALE_NUM / DSEG_SCALE_DEN;
    const int gh = g.height * DSEG_SCALE_NUM / DSEG_SCALE_DEN;
    const int gx = x + g.x_offset * DSEG_SCALE_NUM / DSEG_SCALE_DEN;
    const int gy = baseline + g.y_offset * DSEG_SCALE_NUM / DSEG_SCALE_DEN;
    for (int ty = 0; ty < gh; ++ty) {
        const int sy = ty * DSEG_SCALE_DEN / DSEG_SCALE_NUM;
        for (int tx = 0; tx < gw; ++tx) {
            const int sx = tx * DSEG_SCALE_DEN / DSEG_SCALE_NUM;
            const uint32_t bit = static_cast<uint32_t>(sy) * g.width + sx;
            if ((bits[bit / 8] & (0x80U >> (bit % 8))) == 0) continue;
            put(gx + tx, gy + ty);
        }
    }
}

void draw_dseg_time_text(const char *s, int x, int baseline)
{
    for (const char *p = s; *p; ++p) {
        if (*p == '-') {
            fill_rect(x + kDsegMinusX, baseline + kDsegMinusY,
                      x + kDsegMinusX + kDsegMinusW - 1,
                      baseline + kDsegMinusY + kDsegMinusH - 1);
            x += kDsegMinusAdv;
            continue;
        }
        const char *hit = std::strchr(kDsegChars, *p);
        if (!hit) continue;
        const int idx = static_cast<int>(hit - kDsegChars);
        const DsegGlyph &g = kDSEG36Glyphs[idx];
        draw_dseg_glyph(g, kDSEG36Bitmaps + g.bitmap_offset, x, baseline);
        x += g.x_advance * DSEG_SCALE_NUM / DSEG_SCALE_DEN;
    }
}

// 大字时间单独抽出：整屏重画和每秒局刷都用它。
void draw_big_time_text(const struct tm *local)
{
    char text[16];
    if (local) {
        std::snprintf(text, sizeof(text), "%02d:%02d:%02d",
                      local->tm_hour, local->tm_min, local->tm_sec);
    } else {
        std::snprintf(text, sizeof(text), "00:00:00");
    }
    const InkBounds b = dseg_text_ink_bounds(text);
    // 按屏幕左边到日期卡左边这一整块留白居中，闭区间宽度要 +1。
    // 99:99:99 实测左边 15px、右边 16px，1px 是奇偶像素下限。
    constexpr int ALIGN_X0 = 0;
    constexpr int ALIGN_X1 = CARD_X0 - 1;
    const int x = ALIGN_X0 +
                  (ALIGN_X1 - ALIGN_X0 + 1 - (b.x1 - b.x0 + 1)) / 2 - b.x0;
    const int baseline = TIME_ROW_Y + TIME_ROW_H - 15;  // 字模按 6/5 放大后 y_offset≈-43
    draw_dseg_time_text(text, x, baseline);
}

void draw_time_and_date(const struct tm *local)
{
    draw_big_time_text(local);

    // 黑底卡片
    fill_rect(CARD_X0, TIME_ROW_Y, CARD_X1, TIME_ROW_Y + TIME_ROW_H - 1);

    char line[64];
    const int row1 = TIME_ROW_Y + 7;
    const int row2 = TIME_ROW_Y + 30;
    const int row3 = TIME_ROW_Y + 53;
    const int cx = (CARD_X0 + CARD_X1) / 2;

    if (local) {
        const int mon = local->tm_mon;
        std::snprintf(line, sizeof(line), "%s %d, %d", kMonthEn[mon],
                      local->tm_mday, local->tm_year + 1900);
    } else {
        std::snprintf(line, sizeof(line), "---- --, ----");
    }
    text12_center(cx, row1, line, 1, 0, false);

    if (local && local->tm_wday >= 0 && local->tm_wday < 7) {
        std::snprintf(line, sizeof(line), "%s (%s)", kWeekdayZh[local->tm_wday],
                      kWeekdayEnShort[local->tm_wday]);
    } else {
        std::snprintf(line, sizeof(line), "星期-");
    }
    text12_center(cx, row2, line, 1, 0, false);

    CalendarDayInfo info;
    if (local && calendar_day_info(*local, &info)) {
        std::snprintf(line, sizeof(line), "农历 %s%s",
                      calendar_lunar_month_text(info), calendar_lunar_day_text(info));
    } else {
        std::snprintf(line, sizeof(line), "农历 --");
    }
    text12_center(cx, row3, line, 1, 0, false);
}

// ---------- 模拟表盘 ----------
void draw_dial(const struct tm *local)
{
    const int cx = DIAL_CX, cy = DIAL_CY, r = DIAL_R;
    circle_ring(cx, cy, r, 3);

    // 60 个分钟刻度：整点位画长刻度，其余只点一个像素
    for (int m = 0; m < 60; ++m) {
        const double a = m * 6.0 * 3.14159265358979 / 180.0;
        const double s = std::sin(a), c = std::cos(a);
        if (m % 5 == 0) {
            const int x0 = cx + static_cast<int>((r - 9) * s + 0.5);
            const int y0 = cy - static_cast<int>((r - 9) * c + 0.5);
            const int x1 = cx + static_cast<int>((r - 4) * s + 0.5);
            const int y1 = cy - static_cast<int>((r - 4) * c + 0.5);
            thick_line(x0, y0, x1, y1, 2);
        } else {
            const int x = cx + static_cast<int>((r - 5) * s + 0.5);
            const int y = cy - static_cast<int>((r - 5) * c + 0.5);
            put(x, y);
        }
    }

    // 1..12 数字
    char num[16];
    for (int h = 1; h <= 12; ++h) {
        const double a = h * 30.0 * 3.14159265358979 / 180.0;
        const int x = cx + static_cast<int>((r - 19) * std::sin(a) + 0.5);
        const int y = cy - static_cast<int>((r - 19) * std::cos(a) + 0.5);
        std::snprintf(num, sizeof(num), "%d", h);
        text12_center(x, y - 5, num);
    }

    const int hour = local ? local->tm_hour % 12 : 10;
    const int minute = local ? local->tm_min : 30;
    const int second = local ? local->tm_sec : 45;

    auto hand = [&](double deg, int len, int width) {
        const double a = deg * 3.14159265358979 / 180.0;
        const int x1 = cx + static_cast<int>(len * std::sin(a) + 0.5);
        const int y1 = cy - static_cast<int>(len * std::cos(a) + 0.5);
        thick_line(cx, cy, x1, y1, width);
    };
    hand((hour + minute / 60.0) * 30.0, r - 26, 5);
    hand((minute + second / 60.0) * 6.0, r - 15, 3);
    hand(second * 6.0, r - 8, 1);
    fill_circle(cx, cy, 3);
}

// ---------- 月历 ----------
// 表头 MON..SUN，最后两列（周六、周日）整格反白。
void draw_month_grid(const struct tm *local)
{
    const int year = local ? local->tm_year + 1900 : 2025;
    const int month = local ? local->tm_mon + 1 : 7;
    const int today = local ? local->tm_mday : 14;

    const int grid_h = CAL_HEADER_H + CAL_ROWS * CAL_ROW_H;
    const int x1 = CAL_X0 + CAL_COLW * 7 - 1;
    const int y1 = CAL_Y0 + grid_h - 1;

    // 周末表头先铺黑，表头文字再反白画上去。
    for (int i = kWeekendFirstCol; i < 7; ++i) {
        const int x = CAL_X0 + i * CAL_COLW;
        fill_rect(x, CAL_Y0, x + CAL_COLW - 1, CAL_Y0 + CAL_HEADER_H - 1);
    }

    rect_frame(CAL_X0, CAL_Y0, x1, y1);
    for (int i = 1; i < 7; ++i) vline(CAL_X0 + i * CAL_COLW, CAL_Y0, y1);
    hline(CAL_X0, x1, CAL_Y0 + CAL_HEADER_H);
    for (int row = 1; row < CAL_ROWS; ++row) {
        hline(CAL_X0, x1, CAL_Y0 + CAL_HEADER_H + row * CAL_ROW_H);
    }

    // 表头按格子实际墨迹居中，并横向加粗一点。
    for (int i = 0; i < 7; ++i) {
        const int x = CAL_X0 + i * CAL_COLW;
        text12_center_in_box(x, CAL_Y0, x + CAL_COLW - 1,
                             CAL_Y0 + CAL_HEADER_H - 1,
                             kMonthHeaderMon[i], 1, 0,
                             i < kWeekendFirstCol, true);
    }

    // calendar_first_weekday() 以周日为 0，这里换成周一起算。
    const int first_wd = (calendar_first_weekday(year, month) + 6) % 7;
    const int days = calendar_days_in_month(year, month);
    char num[16];
    for (int day = 1; day <= days; ++day) {
        const int idx = first_wd + day - 1;
        const int row = idx / 7;
        const int col = idx % 7;
        if (row >= CAL_ROWS) break;
        const int x = CAL_X0 + col * CAL_COLW;
        const int y = CAL_Y0 + CAL_HEADER_H + row * CAL_ROW_H;
        std::snprintf(num, sizeof(num), "%d", day);
        if (day == today) {
            fill_rect(x + 1, y + 1, x + CAL_COLW - 2, y + CAL_ROW_H - 2);
            text12_center_in_box(x + 1, y + 1, x + CAL_COLW - 2,
                                 y + CAL_ROW_H - 2, num, 1, 0, false);
        } else {
            text12_center_in_box(x, y, x + CAL_COLW - 1,
                                 y + CAL_ROW_H - 1, num);
        }
    }
}

// ---------- 操作栏 ----------
void draw_action_bar(int page_index, int page_count)
{
    hline(0, W - 1, BAR_Y);
    constexpr int TOP = BAR_Y + 6;

    draw_icon("chevron_left_12", 8, BAR_Y + 4);
    draw_icon("chevron_right_12", 22, BAR_Y + 4);
    int x = text12(38, TOP - 1, "SWITCH");
    x = text12(x + 4, TOP, "切换");
    vline(x + 8, BAR_Y + 3, H - 3);

    // 中间这段原来还有 DETAIL，400px 宽排不下三段中英混排，只留中文。
    int cap_x = x + 16;
    rounded_fill(cap_x, BAR_Y + 4, cap_x + 16, BAR_Y + 15, 2);
    text12(cap_x + 3, BAR_Y + 6, "A", 1, 0, false);
    x = text12(cap_x + 20, TOP, "详情");
    vline(x + 8, BAR_Y + 3, H - 3);

    cap_x = x + 16;
    rounded_fill(cap_x, BAR_Y + 4, cap_x + 16, BAR_Y + 15, 2);
    text12(cap_x + 3, BAR_Y + 6, "B", 1, 0, false);
    x = text12(cap_x + 20, TOP - 1, "BACK");
    text12(x + 4, TOP, "返回");

    const int dots = page_count > 0 ? page_count : 5;
    const int clamped = page_index < 0 ? 0 : (page_index >= dots ? dots - 1 : page_index);
    for (int k = 0; k < dots && k < 8; ++k) {
        const int bx = W - 8 - (dots - k) * 9;
        if (k < clamped) fill_rect(bx, BAR_Y + 5, bx + 6, BAR_Y + 13);
        else rect_frame(bx, BAR_Y + 5, bx + 6, BAR_Y + 13);
    }
}

// ---------- 天气时钟第二页（静态 UI，功能数据后续再接） ----------
enum SmallWeatherIcon {
    kSmallSun = 0,
    kSmallPartly,
    kSmallRain,
    kSmallCloud,
    kSmallMoon,
    kSmallMoonCloud,
};

void draw_icon_scaled(const char *name, int x, int y, int scale)
{
    const auto *ic = find_icon(name);
    if (!ic) return;
    for (int row = 0; row < ic->h; ++row) {
        for (int b = 0; b < ic->stride; ++b) {
            const uint8_t byte = ic->bits[row * ic->stride + b];
            for (int bit = 0; bit < 8; ++bit) {
                if ((byte & (1 << (7 - bit))) == 0) continue;
                fill_rect(x + (b * 8 + bit) * scale, y + row * scale,
                          x + (b * 8 + bit) * scale + scale - 1,
                          y + row * scale + scale - 1);
            }
        }
    }
}

void text12_right(int x1, int top, const char *s, int scale = 1,
                  int extra = 0, bool ink = true, bool bold = false)
{
    text12(x1 - text_width12(s, scale, extra), top, s, scale, extra, ink, bold);
}

void dotted_hline(int x0, int x1, int y)
{
    for (int x = x0; x <= x1; x += 2) put(x, y);
}

void dotted_vline(int x, int y0, int y1)
{
    for (int y = y0; y <= y1; y += 2) put(x, y);
}

void draw_small_cloud(int x, int y)
{
    // 实心云：三个实心圆叠出云朵，底部填平。
    fill_circle(x + 3, y + 5, 2);
    fill_circle(x + 6, y + 4, 3);
    fill_circle(x + 10, y + 5, 2);
    fill_rect(x + 2, y + 5, x + 12, y + 7);
}

void draw_small_weather_icon(int kind, int x, int y)
{
    switch (kind) {
    case kSmallSun:
        fill_circle(x + 6, y + 6, 4);
        hline(x + 0, x + 1, y + 6);
        hline(x + 11, x + 12, y + 6);
        vline(x + 6, y + 0, y + 1);
        vline(x + 6, y + 11, y + 12);
        put(x + 2, y + 2); put(x + 10, y + 2);
        put(x + 2, y + 10); put(x + 10, y + 10);
        break;
    case kSmallPartly:
        fill_circle(x + 3, y + 4, 2);
        draw_small_cloud(x + 3, y + 4);
        break;
    case kSmallRain:
        draw_small_cloud(x + 0, y + 0);
        fill_circle(x + 4, y + 10, 1);
        fill_circle(x + 8, y + 10, 1);
        fill_circle(x + 12, y + 10, 1);
        break;
    case kSmallCloud:
        draw_small_cloud(x + 0, y + 0);
        break;
    case kSmallMoon:
        fill_circle(x + 7, y + 6, 5);
        fill_circle(x + 10, y + 4, 5, false);
        break;
    case kSmallMoonCloud:
        fill_circle(x + 4, y + 4, 4);
        fill_circle(x + 6, y + 3, 4, false);
        draw_small_cloud(x + 5, y + 4);
        break;
    }
}

void draw_uv_bars(int x0, int y0, int count)
{
    for (int i = 0; i < 10; ++i) {
        const int bx = x0 + i * 13;
        rect_frame(bx, y0, bx + 11, y0 + 6);
        if (i < count) fill_rect(bx + 1, y0 + 1, bx + 10, y0 + 5);
    }
}

struct HourSample {
    char hour[8] = {};
    char temp[8] = {};
    SmallWeatherIcon icon = kSmallPartly;
};

struct DaySample {
    char week_cn[8] = {};
    char date[8] = {};
    SmallWeatherIcon icon = kSmallPartly;
    int bar = 0;
    char temps[24] = {};
};

// 第二页显示用的全部文本，启动时从天气快照填充；未联网时留 "--" 占位。
struct WeatherPageData {
    bool ready = false;
    char city[32] = {};
    char city_en[24] = {};
    char temp[8] = {};
    char apparent[16] = {};
    char high[16] = {};
    char low[16] = {};
    char condition_en[16] = {};
    char condition_cn[32] = {};
    char wind[40] = {};
    int uv = 0;
    char updated[8] = {};
    HourSample hours[6];
    DaySample days[5];
};

WeatherPageData s_weather = {};

SmallWeatherIcon small_icon_from_code(const char *code, bool night)
{
    const int v = std::atoi(code ? code : "0");
    if (v == 100) return night ? kSmallMoon : kSmallSun;
    if (v == 101 || v == 102 || v == 103) return night ? kSmallMoonCloud : kSmallPartly;
    if (v == 104 || v == 501 || v == 502) return kSmallCloud;
    if (v >= 300 && v <= 399) return kSmallRain;
    return kSmallPartly;
}

void condition_label(const char *cn, char *out, size_t size)
{
    const char *label = "WEATHER";
    if (std::strcmp(cn, "晴") == 0) label = "SUNNY";
    else if (std::strcmp(cn, "多云") == 0) label = "CLOUDY";
    else if (std::strcmp(cn, "阴") == 0) label = "OVERCAST";
    else if (std::strcmp(cn, "雾") == 0) label = "FOG";
    else if (std::strstr(cn, "雷")) label = "STORM";
    else if (std::strstr(cn, "雪")) label = "SNOW";
    else if (std::strstr(cn, "雨")) label = "RAIN";
    std::snprintf(out, size, "%s", label);
}

void city_english(const char *cn, char *out, size_t size)
{
    struct NamePair { const char *cn; const char *en; };
    static const NamePair pairs[] = {
        {"成都", "CHENGDU"}, {"上海", "SHANGHAI"}, {"北京", "BEIJING"},
        {"广州", "GUANGZHOU"}, {"深圳", "SHENZHEN"}, {"杭州", "HANGZHOU"},
        {"重庆", "CHONGQING"}, {"武汉", "WUHAN"}, {"西安", "XIAN"},
        {"南京", "NANJING"}, {"苏州", "SUZHOU"}, {"天津", "TIANJIN"},
    };
    out[0] = '\0';
    for (const NamePair &p : pairs) {
        if (std::strstr(cn, p.cn)) { std::snprintf(out, size, "%s", p.en); return; }
    }
}

void wind_label(const char *dir, const char *scale, char *out, size_t size)
{
    struct WindPair { const char *full; const char *abbr; const char *base; };
    static const WindPair pairs[] = {
        {"北风", "N", "北"}, {"东北风", "NE", "东北"}, {"东风", "E", "东"},
        {"东南风", "SE", "东南"}, {"南风", "S", "南"}, {"西南风", "SW", "西南"},
        {"西风", "W", "西"}, {"西北风", "NW", "西北"},
    };
    const char *abbr = "--", *base = "--";
    for (const WindPair &p : pairs) {
        if (std::strcmp(dir, p.full) == 0) { abbr = p.abbr; base = p.base; break; }
    }
    std::snprintf(out, size, "%s %s %s 级", abbr, base, scale && *scale ? scale : "0");
}

void weekday_cn(const char *date, char *out, size_t size)
{
    int y = 0, m = 0, d = 0;
    if (std::sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) {
        std::snprintf(out, size, "周-");
        return;
    }
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = 12;
    mktime(&t);
    static const char *names[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    std::snprintf(out, size, "%s", names[t.tm_wday >= 0 && t.tm_wday < 7 ? t.tm_wday : 0]);
}

void month_day(const char *date, char *out, size_t size)
{
    int y = 0, m = 0, d = 0;
    if (std::sscanf(date, "%d-%d-%d", &y, &m, &d) != 3) {
        std::snprintf(out, size, "--/--");
        return;
    }
    std::snprintf(out, size, "%02d/%02d", m, d);
}

void refresh_weather_page_data()
{
    WeatherData w = {};
    WeatherForecastData f = {};
    s_weather = {};
    std::snprintf(s_weather.city, sizeof(s_weather.city), "成都");
    city_english(s_weather.city, s_weather.city_en, sizeof(s_weather.city_en));
    std::snprintf(s_weather.temp, sizeof(s_weather.temp), "--");
    std::snprintf(s_weather.apparent, sizeof(s_weather.apparent), "--°");
    std::snprintf(s_weather.condition_en, sizeof(s_weather.condition_en), "WEATHER");
    std::snprintf(s_weather.condition_cn, sizeof(s_weather.condition_cn), "--");
    std::snprintf(s_weather.updated, sizeof(s_weather.updated), "--:--");
    if (!get_weather_full_snapshot(&w, nullptr, &f, nullptr) || !w.temp[0]) return;
    s_weather.ready = true;
    std::snprintf(s_weather.city, sizeof(s_weather.city), "%s", w.city[0] ? w.city : "成都");
    city_english(s_weather.city, s_weather.city_en, sizeof(s_weather.city_en));
    std::snprintf(s_weather.temp, sizeof(s_weather.temp), "%s", w.temp);
    std::snprintf(s_weather.apparent, sizeof(s_weather.apparent), "%s°",
                  w.apparent[0] ? w.apparent : "--");
    std::snprintf(s_weather.condition_cn, sizeof(s_weather.condition_cn), "%s", w.text);
    // 极长天气文案（如「雷阵雨伴冰雹」）截到 3 个汉字，避免撞上左侧大图标。
    if (std::strlen(s_weather.condition_cn) > 12) s_weather.condition_cn[9] = '\0';
    condition_label(w.text, s_weather.condition_en, sizeof(s_weather.condition_en));

    if (f.ready && f.count > 0) {
        const WeatherForecastDay &today = f.days[0];
        std::snprintf(s_weather.high, sizeof(s_weather.high), "%s°",
                      today.temp_max[0] ? today.temp_max : "--");
        std::snprintf(s_weather.low, sizeof(s_weather.low), "%s°",
                      today.temp_min[0] ? today.temp_min : "--");
        wind_label(today.wind_dir, today.wind_scale, s_weather.wind, sizeof(s_weather.wind));
        const char *uv = w.uv_index[0] ? w.uv_index : today.uv_index;
        s_weather.uv = uv && *uv ? std::atoi(uv) : 0;
    } else {
        std::snprintf(s_weather.high, sizeof(s_weather.high), "--°");
        std::snprintf(s_weather.low, sizeof(s_weather.low), "--°");
        std::snprintf(s_weather.wind, sizeof(s_weather.wind), "-- -- 0 级");
    }

    // 24H 取 6 个等间隔点，覆盖完整一天；不足 6 点时留占位。
    for (int i = 0; i < 6; ++i) {
        HourSample &out = s_weather.hours[i];
        const int idx = f.hourly_count > 0 ? (i * f.hourly_count) / 6 : -1;
        if (idx >= 0 && f.hourly[idx].valid) {
            const WeatherHourlyPoint &p = f.hourly[idx];
            std::snprintf(out.hour, sizeof(out.hour), "%s", p.hour);
            std::snprintf(out.temp, sizeof(out.temp), "%s°", p.temp);
            const int hh = std::atoi(p.hour);
            out.icon = small_icon_from_code(p.icon, hh < 6 || hh >= 18);
        } else {
            std::snprintf(out.hour, sizeof(out.hour), "--");
            std::snprintf(out.temp, sizeof(out.temp), "--");
            out.icon = kSmallPartly;
        }
    }

    // 5 DAYS 优先显示明天起的 5 天，预报不足 6 天时从今天起。
    const int start = f.count >= 6 ? 1 : 0;
    for (int i = 0; i < 5; ++i) {
        DaySample &out = s_weather.days[i];
        const int idx = start + i;
        if (idx < f.count && f.days[idx].valid) {
            const WeatherForecastDay &d = f.days[idx];
            weekday_cn(d.date, out.week_cn, sizeof(out.week_cn));
            month_day(d.date, out.date, sizeof(out.date));
            out.icon = small_icon_from_code(d.icon, false);
            out.bar = std::atoi(d.temp_max);
            if (out.bar < 0) out.bar = 0;
            if (out.bar > 34) out.bar = 34;
            std::snprintf(out.temps, sizeof(out.temps), "%s/%s°",
                          d.temp_min[0] ? d.temp_min : "--",
                          d.temp_max[0] ? d.temp_max : "--");
        } else {
            std::snprintf(out.week_cn, sizeof(out.week_cn), "周-");
            std::snprintf(out.date, sizeof(out.date), "--/--");
            std::snprintf(out.temps, sizeof(out.temps), "--/--°");
        }
    }

    WeatherCacheStatusSnapshot cache = {};
    if (weather_cache_status_snapshot_load(&cache) && cache.last_sync_time > 0) {
        struct tm local = {};
        const time_t stamp = cache.last_sync_time;
        if (localtime_r(&stamp, &local)) {
            std::snprintf(s_weather.updated, sizeof(s_weather.updated), "%02d:%02d",
                          local.tm_hour, local.tm_min);
            return;
        }
    }
    std::snprintf(s_weather.updated, sizeof(s_weather.updated), "--:--");
}

void draw_24h_panel()
{
    constexpr int X0 = 10, Y0 = 150, X1 = 198, Y1 = 260;
    rect_frame(X0, Y0, X1, Y1);
    // 面板标题在 Y0..Y0+24 这一行里上下居中。
    text12(X0 + 5, Y0 + 6, "24H 逐时预报", 1, 0, true, true);
    dotted_hline(X0 + 1, X1 - 1, Y0 + 24);
    // 六列等宽，左右各留 1px；宽度 (X1-X0-2)/6 = 31。
    constexpr int INNER_X0 = X0 + 1;
    constexpr int COL_W = 31;
    for (int i = 1; i < 6; ++i) {
        dotted_vline(INNER_X0 + i * COL_W, Y0 + 25, Y1 - 1);
    }

    // 图标 + 时间 + 温度三行，在内容区 Y0+25..Y1-1（高 85px）里三等分，
    // 每个内容的墨迹中心对齐各自格子的中心，做到上下平均分布。
    constexpr int CONTENT_Y0 = Y0 + 25;              // 175
    constexpr int SECTION_H = (Y1 - 1 - CONTENT_Y0 + 1) / 3;  // 28
    constexpr int SEC1_CY = CONTENT_Y0 + SECTION_H / 2;             // 189
    constexpr int SEC2_CY = CONTENT_Y0 + SECTION_H + SECTION_H / 2; // 217
    constexpr int SEC3_CY = CONTENT_Y0 + 2 * SECTION_H + SECTION_H / 2; // 245
    for (int i = 0; i < 6; ++i) {
        const int cx = INNER_X0 + i * COL_W + COL_W / 2;
        // 图标墨迹约 13px、中心在 y+6；文字墨迹 9px、中心在 top+4。
        draw_small_weather_icon(s_weather.hours[i].icon, cx - 7, SEC1_CY - 6);
        text12_center(cx, SEC2_CY - 4, s_weather.hours[i].hour, 1, 0, true, false);
        text12_center(cx, SEC3_CY - 4, s_weather.hours[i].temp, 1, 0, true, false);
    }
}

void draw_5day_panel()
{
    constexpr int X0 = 205, Y0 = 150, X1 = 392, Y1 = 260;
    rect_frame(X0, Y0, X1, Y1);
    text12(X0 + 6, Y0 + 6, "5 DAYS 五日预报", 1, 0, true, true);
    dotted_hline(X0 + 1, X1 - 1, Y0 + 24);

    // 中文星期、日期各占固定列，起始位置对齐。
    constexpr int WEEK_CN_X = X0 + 6;   // 211
    constexpr int DATE_X = 243;         // 中文列宽 24 + 间距 8

    // 温度右对齐到面板右边距内（X1-4=388），位数再长也不会越界；
    // 图标与温度条左移让位，保证最长 "-15/-20°" 也不与温度条重叠。
    constexpr int TEMP_RIGHT = X1 - 4;   // 388

    for (int i = 0; i < 5; ++i) {
        const int ry = Y0 + 28 + i * 16;
        text12(WEEK_CN_X, ry, s_weather.days[i].week_cn);
        text12(DATE_X, ry, s_weather.days[i].date);
        draw_small_weather_icon(s_weather.days[i].icon, 282, ry - 2);
        rect_frame(300, ry + 2, 334, ry + 9);
        fill_rect(302, ry + 4, 302 + s_weather.days[i].bar, ry + 7);
        text12_right(TEMP_RIGHT, ry, s_weather.days[i].temps);
    }
}

void draw_weather_page(const struct tm *local, int battery_percent)
{
    // 中文用卡上全量字库（成都的「都」不在固件内置子集里）。
    if (!book_font_ready()) (void)book_font_load();
    refresh_weather_page_data();

    fill_rect(0, 0, W - 1, H - 1, false);
    draw_status_bar(local, battery_percent);

    text12(10, TITLE_TOP, "WEATHER 天气预报", 1, 0, true, false);
    char place[64] = {};
    if (s_weather.city_en[0]) {
        std::snprintf(place, sizeof(place), "%s %s · 天气",
                      s_weather.city_en, s_weather.city);
    } else {
        std::snprintf(place, sizeof(place), "%s · 天气", s_weather.city);
    }
    text12_right(392, TITLE_TOP, place, 1, 0, true, false);
    fill_rect(0, TITLE_RULE_Y, W - 1, TITLE_RULE_Y + TITLE_RULE_H - 1);

    // 左上主天气：沿用主菜单 34px 晴间多云图标，放大 2 倍。
    draw_icon_scaled("cloud_sun_34", 18, 72, 2);

    // 大温度按真实墨迹居中在 x=112，负号变宽也不会碰到 WIND 卡。
    const InkBounds tb = dseg_text_ink_bounds(s_weather.temp);
    draw_dseg_time_text(s_weather.temp, 112 - tb.x0, 112);
    circle_ring(112 + (tb.x1 - tb.x0) + 5, 76, 3, 1);

    // 体感行右对齐到 238（WIND 卡左边距 6px），长文案也不会撞进卡片。
    char feel[80] = {};
    std::snprintf(feel, sizeof(feel), "%s %s · 体感 %s",
                  s_weather.condition_en, s_weather.condition_cn, s_weather.apparent);
    text12_right(238, 116, feel, 1, 0, true, false);
    char range[48] = {};
    std::snprintf(range, sizeof(range), "HIGH %s  LOW %s",
                  s_weather.high, s_weather.low);
    text12(114, 132, range);

    // 右上风力 / UV 卡。分隔线落在卡片中线，上下两格等高。
    rect_frame(244, 62, 392, 138);
    text12(250, 68, "WIND 风力", 1, 0, true, true);
    text12(250, 86, s_weather.wind);
    dotted_hline(250, 386, 100);
    char uv_text[24] = {};
    std::snprintf(uv_text, sizeof(uv_text), "UV 指数 %d", s_weather.uv);
    text12(250, 106, uv_text, 1, 0, true, true);
    draw_uv_bars(250, 122, s_weather.uv);

    draw_24h_panel();
    draw_5day_panel();

    char updated[80] = {};
    std::snprintf(updated, sizeof(updated),
                  "UPDATED 更新于 %s  ·  AUTO REFRESH 自动刷新。",
                  s_weather.updated);
    text12(10, 267, updated);
    draw_action_bar(s_page_index, 5);
}

void draw_all(const struct tm *local, int battery_percent)
{
    if (s_page_index == 1) {
        draw_weather_page(local, battery_percent);
        return;
    }
    fill_rect(0, 0, W - 1, H - 1, false);
    draw_status_bar(local, battery_percent);
    draw_title();
    draw_time_and_date(local);
    draw_dial(local);
    draw_month_grid(local);
    draw_action_bar(s_page_index, 5);
}

// 精确标脏一个矩形（canvas 挂在 0,0，canvas 坐标就是屏幕坐标）。
// 只用局部重画函数调用，避免 lv_obj_invalidate(s_canvas) 把脏区撑到整屏。
void invalidate_rect(int x1, int y1, int x2, int y2)
{
    invalidate_canvas_rect(s_canvas, x1, y1, x2, y2);
}

// ---------- 秒级局刷区域 ----------
// 大字时间：x 8..250（CARD_X0-6）、y 62..135，宽 243 < 280 阈值。
constexpr int TIME_DIRTY_X0 = TIME_LEFT;
constexpr int TIME_DIRTY_X1 = CARD_X0 - 6;
constexpr int TIME_DIRTY_Y0 = TIME_ROW_Y;
constexpr int TIME_DIRTY_Y1 = TIME_ROW_Y + TIME_ROW_H - 1;

// 表盘：外接方框 cx±(r+3)、cy±(r+3)，宽约 123 < 280 阈值。
constexpr int DIAL_DIRTY_X0 = DIAL_CX - DIAL_R - 3;
constexpr int DIAL_DIRTY_X1 = DIAL_CX + DIAL_R + 3;
constexpr int DIAL_DIRTY_Y0 = DIAL_CY - DIAL_R - 3;
constexpr int DIAL_DIRTY_Y1 = DIAL_CY + DIAL_R + 3;

// 状态栏左上 HH:MM（scale=2），宽约 66px。
constexpr int STATUS_TIME_X0 = 2;
constexpr int STATUS_TIME_X1 = 72;
constexpr int STATUS_TIME_Y0 = 0;
constexpr int STATUS_TIME_Y1 = STATUS_H - 3;

// 秒级重画大字时间：先擦白区域（否则数字只画 1、旧笔画会残留），再画，再标脏。
void redraw_big_time(const struct tm *local)
{
    fill_rect(TIME_DIRTY_X0, TIME_DIRTY_Y0, TIME_DIRTY_X1, TIME_DIRTY_Y1, false);
    draw_big_time_text(local);
    invalidate_rect(TIME_DIRTY_X0, TIME_DIRTY_Y0, TIME_DIRTY_X1, TIME_DIRTY_Y1);
}

// 秒级重画表盘：擦白外接方框，重画整盘（含走针），再标脏。
void redraw_dial(const struct tm *local)
{
    fill_rect(DIAL_DIRTY_X0, DIAL_DIRTY_Y0, DIAL_DIRTY_X1, DIAL_DIRTY_Y1, false);
    draw_dial(local);
    invalidate_rect(DIAL_DIRTY_X0, DIAL_DIRTY_Y0, DIAL_DIRTY_X1, DIAL_DIRTY_Y1);
}

// 分钟级重画状态栏 HH:MM：只擦时间区，不碰右边的日期/电量图标。
void redraw_status_time(const struct tm *local)
{
    fill_rect(STATUS_TIME_X0, STATUS_TIME_Y0, STATUS_TIME_X1, STATUS_TIME_Y1, false);
    char buf[16];
    const int hh = local ? local->tm_hour : 9;
    const int mm = local ? local->tm_min : 41;
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
    text12(6, 6, buf, 2);
    invalidate_rect(STATUS_TIME_X0, STATUS_TIME_Y0, STATUS_TIME_X1, STATUS_TIME_Y1);
}

} // namespace

void build_time_calendar_page(lv_obj_t *parent)
{
    if (!parent) return;
    // 页面 root 可能被重建（开机结束时的 lv_obj_clean 就会），
    // 那时旧 canvas 已经随之销毁：只认还活着、且挂在同一个父对象上的那个。
    if (s_canvas && (!lv_obj_is_valid(s_canvas) || s_parent != parent)) {
        if (lv_obj_is_valid(s_canvas)) lv_obj_del(s_canvas);
        s_canvas = nullptr;
    }
    if (!s_canvas) {
        if (!ensure_canvas_buffer(&s_buf, W, H)) {
            ESP_LOGW(TAG, "canvas buffer alloc failed");
            return;
        }
        s_canvas = lv_canvas_create(parent);
        if (!s_canvas) {
            ESP_LOGW(TAG, "canvas create failed");
            return;
        }
        s_parent = parent;
        configure_canvas_base(s_canvas, s_buf, 0, 0, W, H);
    }
    lv_obj_move_foreground(s_canvas);
    ESP_LOGI(TAG, "page ready: canvas=%p buf=%p parent=%p hidden=%d psram_free=%u",
             static_cast<void *>(s_canvas), static_cast<void *>(s_buf),
             static_cast<void *>(parent),
             lv_obj_has_flag(s_canvas, LV_OBJ_FLAG_HIDDEN) ? 1 : 0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    time_calendar_redraw(nullptr, battery_percent_load());
}

void time_calendar_redraw(const struct tm *local, int battery_percent)
{
    if (!s_canvas || !s_buf) return;
    if (!lv_obj_is_valid(s_canvas)) {
        ESP_LOGW(TAG, "canvas object gone, skip redraw");
        s_canvas = nullptr;
        return;
    }
    draw_all(local, battery_percent);
    lv_obj_invalidate(s_canvas);
}

void clear_time_calendar_page(void)
{
    if (s_canvas) {
        lv_obj_del(s_canvas);
        s_canvas = nullptr;
    }
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = nullptr;
    }
    s_parent = nullptr;
    s_last_day_key = -1;
    s_last_minute_key = -1;
    s_last_second_key = -1;
    s_last_battery = -999;
    s_last_weather_version = 0;
}

void forget_time_calendar_page(void)
{
    // 调用点（finish_boot_screen）刚做过 lv_obj_clean(lv_scr_act())，
    // s_canvas 已经是悬空指针：只丢引用 + 释放自己的缓冲，不能再删对象。
    s_canvas = nullptr;
    s_parent = nullptr;
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = nullptr;
    }
    s_last_day_key = -1;
    s_last_minute_key = -1;
    s_last_second_key = -1;
    s_last_battery = -999;
    s_last_weather_version = 0;
}

void time_calendar_tick(const struct tm &local, int battery_percent)
{
    if (!s_canvas || !s_buf) return;
    if (lv_obj_has_flag(s_canvas, LV_OBJ_FLAG_HIDDEN)) return;

    const int day_key = (local.tm_year + 1900) * 10000 +
                        (local.tm_mon + 1) * 100 + local.tm_mday;
    const int minute_key = day_key * 10000 + local.tm_hour * 100 + local.tm_min;
    const int second_key = minute_key * 100 + local.tm_sec;

    // 首次绘制、跨天、电量变化：日期卡/月历/农历/图标都要换，整屏重画。
    if (day_key != s_last_day_key || battery_percent != s_last_battery) {
        s_last_day_key = day_key;
        s_last_minute_key = minute_key;
        s_last_second_key = second_key;
        s_last_battery = battery_percent;
        time_calendar_redraw(&local, battery_percent);
        return;
    }

    // 天气数据版本变化：第二页整页重画一次（联网刷新后自动更新）。
    const uint32_t weather_version = weather_state_version_load();
    if (s_page_index == 1 && weather_version != s_last_weather_version) {
        s_last_weather_version = weather_version;
        time_calendar_redraw(&local, battery_percent);
        return;
    }

    // 每秒：只刷大字时间 + 表盘指针（第二页是静态版式，不刷这两块）。
    if (s_page_index == 0 && second_key != s_last_second_key) {
        s_last_second_key = second_key;
        redraw_big_time(&local);
        redraw_dial(&local);
    }
    // 分钟翻转：状态栏 HH:MM 单独局刷（大时间/表盘已在上面刷过）。
    if (minute_key != s_last_minute_key) {
        s_last_minute_key = minute_key;
        redraw_status_time(&local);
    }
}

void time_calendar_set_page(int page)
{
    s_page_index = page == 1 ? 1 : 0;
    time_t now = std::time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    time_calendar_redraw(&local, battery_percent_load());
}

void time_calendar_toggle_page(void)
{
    time_calendar_set_page(s_page_index == 0 ? 1 : 0);
}

int time_calendar_page(void)
{
    return s_page_index;
}

void set_time_calendar_visible(bool visible)
{
    if (!s_canvas) {
        ESP_LOGI(TAG, "set visible(%d) before canvas exists", visible ? 1 : 0);
        return;
    }
    const bool now_hidden = lv_obj_has_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    if (now_hidden != visible) return;
    ESP_LOGI(TAG, "set visible(%d) canvas=%p", visible ? 1 : 0,
             static_cast<void *>(s_canvas));
    if (visible) lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
}

bool time_calendar_visible(void)
{
    return s_canvas && !lv_obj_has_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
}
