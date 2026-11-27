// 阅读模块实现：书库页与阅读页共用一张 400x300 canvas。
// 绘制方式与 ui_pomodoro.cpp 一致：直接写帧缓冲 + 精确 invalidate_rect 局刷。
//
// 中文走 zh_font_16（LVGL 自带全 CJK 位图字库，bpp=1，无灰阶），
// 西文小字走 fusion_pixel_12 / fusion_pixel_8 取模字库，封面是 packed 1bit 点阵。
// 数据来自 book_store：扫描 TF 卡 /sdcard/book，TXT 在数据层排版分页，
// 本文件只负责把页内容画出来，不再内置任何样例行。
#include "ui_reading.h"

#include "app_display_config.h"
#include "book_font.h"
#include "book_store.h"
#include "engine_api.h"         // pad_state() 状态栏蓝牙图标
#include "local_sensor_state.h" // get_local_sensor_snapshot 温湿度
#include "wifi_portal_state.h"  // wifi_station_ip_snapshot WiFi 状态
#include "ui_canvas_primitives.h"
#include "ui_page_state.h"

#include "assets/book_covers.h"
#include "assets/fusion_pixel_12_font.h"
#include "assets/fusion_pixel_8_font.h"
#include "assets/menu_icons.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <atomic>
#include <cstdio>
#include <cstring>

LV_FONT_DECLARE(zh_font_16);

namespace {

constexpr int W = kDisplayWidth;
constexpr int H = kDisplayHeight;
constexpr char TAG[] = "ui_reading";

lv_obj_t *s_root = nullptr;
lv_obj_t *s_canvas = nullptr;
lv_color_t *s_buf = nullptr;
std::atomic<bool> s_shown{false};

ReadingView s_view = ReadingView::kShelf;
int s_selection = 0;
int s_shelf_page = 0;
std::atomic<uint32_t> s_state_version{1};

// 数据层扫描一次就够，退出阅读模块后置回 false，下次进来重扫。
bool s_store_loaded = false;
// 打开失败时的提示，显示在书库题头右侧，下一次成功操作清除。
char s_notice[64] = {};
// 正文页的临时提示（书签已满之类），翻页或换书时清掉。
char s_reader_notice[40] = {};
// 本次阅读起点，退出书时结算并写进 NVS 累计。
int64_t s_open_us = 0;

// 目录 / 书签弹层。tab 0 = 目录，1 = 书签。
bool s_panel_open = false;
int s_panel_tab = 0;
int s_panel_sel = 0;
int s_panel_scroll = 0;

// 全屏阅读：只剩正文，状态栏/操作栏/右栏都不画，分页口径也切到全屏那套。
bool s_fullscreen = false;

inline void bump_state()
{
    s_state_version.fetch_add(1, std::memory_order_release);
}

// ---------- 画点原语 ----------
inline void put(int x, int y, bool ink)
{
    if (!s_buf || x < 0 || x >= W || y < 0 || y >= H) return;
    s_buf[y * W + x] = ink ? lv_color_black() : lv_color_white();
}

void invalidate_rect(int x1, int y1, int x2, int y2)
{
    invalidate_canvas_rect(s_canvas, x1, y1, x2, y2);
}

void fill_rect(int x0, int y0, int x1, int y1, bool ink = true)
{
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

void rounded_fill(int x0, int y0, int x1, int y1, int r, bool ink = true)
{
    fill_rect(x0 + r, y0, x1 - r, y1, ink);
    fill_rect(x0, y0 + r, x1, y1 - r, ink);
    for (int dy = 0; dy < r; ++dy) {
        int span = r - 1 - dy;
        fill_rect(x0 + r - span, y0 + dy, x1 - r + span, y0 + dy, ink);
        fill_rect(x0 + r - span, y1 - dy, x1 - r + span, y1 - dy, ink);
    }
}

// ---------- 取模字库（12px / 8px） ----------
uint32_t utf8_next(const char **p)
{
    const unsigned char *s = reinterpret_cast<const unsigned char *>(*p);
    uint32_t cp = s[0];
    int n = 1;
    if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; n = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; n = 3; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; n = 4; }
    for (int i = 1; i < n; ++i) cp = (cp << 6) | (s[i] & 0x3F);
    *p = reinterpret_cast<const char *>(s + n);
    return cp;
}

const fusion_pixel_12_glyph_t *find_glyph12(uint32_t cp)
{
    int lo = 0, hi = FUSION_PIXEL_12_INDEX_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t cur = fusion_pixel_12_index[mid].cp;
        if (cur == cp) return &fusion_pixel_12_index[mid];
        if (cur < cp) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

const fusion_pixel_8_glyph_t *find_glyph8(uint32_t cp)
{
    int lo = 0, hi = FUSION_PIXEL_8_INDEX_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t cur = fusion_pixel_8_index[mid].cp;
        if (cur == cp) return &fusion_pixel_8_index[mid];
        if (cur < cp) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

template <typename G>
void blit_glyph(const G *g, const uint8_t *bits, int x, int glyph_top,
                int scale, bool ink, bool bold_x)
{
    if (!g || g->row_bytes == 0) return;
    for (int r = 0; r < g->h; ++r) {
        for (int col = 0; col < g->w; ++col) {
            int byte = bits[g->off + r * g->row_bytes + col / 8];
            if (!(byte & (1 << (7 - (col % 8))))) continue;
            for (int sy = 0; sy < scale; ++sy)
                for (int sx = 0; sx < scale + (bold_x ? 1 : 0); ++sx)
                    put(x + (g->xoff + col) * scale + sx,
                        glyph_top + r * scale + sy, ink);
        }
    }
}

// 12px 西文，top 是墨迹上沿（ascent=9）。
int text12(int x, int top, const char *s, int scale = 1, int extra = 0,
           bool ink = true, bool bold_x = false)
{
    constexpr int ascent = 9;
    const char *p = s;
    int cx = x;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        const auto *g = find_glyph12(cp);
        if (g) {
            int glyph_top = (top + ascent * scale) - (g->yoff + g->h) * scale;
            blit_glyph(g, fusion_pixel_12_bits, cx, glyph_top, scale, ink, bold_x);
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
        uint32_t cp = utf8_next(&p);
        const auto *g = find_glyph12(cp);
        adv += (g ? g->dw : 6) * scale + extra;
    }
    return adv;
}

void text12_right(int right, int top, const char *s, int extra = 0)
{
    text12(right - text_width12(s, 1, extra), top, s, 1, extra);
}

// 8px 西文，ascent=6。
int text8(int x, int top, const char *s)
{
    constexpr int ascent = 6;
    const char *p = s;
    int cx = x;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        const auto *g = find_glyph8(cp);
        if (g) {
            int glyph_top = (top + ascent) - (g->yoff + g->h);
            blit_glyph(g, fusion_pixel_8_bits, cx, glyph_top, 1, true, false);
            cx += g->dw;
        } else {
            cx += 4;
        }
    }
    return cx;
}

// ---------- 中文（zh_font_16，1bpp，line_height 17，base_line 3） ----------

void cjk16(int x, int top, const char *s, bool bold = false, bool ink = true)
{
    if (!s || !*s) return;
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = ink ? lv_color_black() : lv_color_white();
    dsc.font = &zh_font_16;
    lv_canvas_draw_text(s_canvas, x, top, W - x, &dsc, s);
    if (bold) {
        lv_canvas_draw_text(s_canvas, x + 1, top, W - x - 1, &dsc, s);
    }
}

int cjk16_width(const char *s)
{
    if (!s || !*s) return 0;
    return lv_txt_get_width(s, (uint32_t)std::strlen(s), &zh_font_16, 0,
                            LV_TEXT_FLAG_NONE);
}


// ---------- 12px 中英混排（与主菜单模块名同一套字形） ----------
// 字库就绪时中文取自 TF 卡全字库、ASCII 取自内置 fusion_pixel_12；
// 未就绪时整体回退 zh_font_16，排版度量也同步回退。
void text_body(int x, int y, const char *s, bool bold = false, bool ink = true)
{
    if (!s || !*s) return;
    if (!book_font_ready()) {
        // 12px 行顶换算到 16px 行顶差 5px，否则底部按钮文案会掉出屏幕。
        cjk16(x, y - 5, s, bold, ink);
        return;
    }
    const int ascent = book_font_ascent();
    const char *p = s;
    int cx = x;
    while (*p) {
        const uint32_t cp = utf8_next(&p);
        if (cp < 0x80) {
            const auto *g = find_glyph12(cp);
            if (g && g->row_bytes) {
                const int glyph_top = (y + ascent) - (g->yoff + g->h);
                blit_glyph(g, fusion_pixel_12_bits, cx, glyph_top, 1, ink, bold);
                cx += g->dw + (bold ? 1 : 0);
            } else {
                cx += 6;
            }
        } else {
            const BookGlyph *g = book_font_glyph(cp);
            if (!g) { cx += book_font_full_w(); continue; }
            for (int r = 0; r < g->h; ++r) {
                for (int c = 0; c < g->w; ++c) {
                    if (g->bits[r * g->row_bytes + c / 8] & (1 << (7 - (c % 8))))
                        put(cx + g->xoff + c, y + g->ytop + r, ink);
                }
            }
            if (bold) {
                for (int r = 0; r < g->h; ++r) {
                    for (int c = 0; c < g->w; ++c) {
                        if (g->bits[r * g->row_bytes + c / 8] & (1 << (7 - (c % 8))))
                            put(cx + g->xoff + c + 1, y + g->ytop + r, ink);
                    }
                }
            }
            cx += g->dw + (bold ? 1 : 0);
        }
    }
}

int text_body_width(const char *s)
{
    if (!s || !*s) return 0;
    if (!book_font_ready()) return cjk16_width(s);
    const char *p = s;
    int adv = 0;
    while (*p) {
        const uint32_t cp = utf8_next(&p);
        if (cp < 0x80) {
            const auto *g = find_glyph12(cp);
            adv += g ? g->dw : 6;
        } else {
            adv += book_font_full_w();
        }
    }
    return adv;
}

// ---------- 图标 ----------
const menu_icon_t *find_icon(const char *name)
{
    for (int i = 0; i < MENU_ICON_COUNT; ++i)
        if (std::strcmp(kMenuIcons[i].name, name) == 0) return &kMenuIcons[i];
    return nullptr;
}

void draw_icon(const char *name, int x, int y)
{
    const auto *ic = find_icon(name);
    if (!ic) return;
    for (int row = 0; row < ic->h; ++row) {
        for (int b = 0; b < ic->stride; ++b) {
            uint8_t byte = ic->bits[row * ic->stride + b];
            for (int bit = 0; bit < 8; ++bit)
                if (byte & (1 << (7 - bit))) put(x + b * 8 + bit, y + row, true);
        }
    }
}

void draw_bookmark(int x, int y, bool filled)
{
    for (int row = 0; row < 15; ++row) {
        int cut = row < 9 ? 0 : (row - 8);
        for (int col = cut; col < 12 - cut; ++col) {
            bool edge = (row == 0 || col == cut || col == 11 - cut || row == 14);
            if (filled || edge) put(x + col, y + row, true);
        }
    }
}

void draw_list_icon(int x, int y)
{
    for (int i = 0; i < 3; ++i) {
        int ly = y + i * 5;
        fill_rect(x, ly, x + 1, ly + 1);
        hline(x + 4, x + 15, ly);
        hline(x + 4, x + 15, ly + 1);
    }
}

// ---------- 封面 ----------
const book_cover_t *find_cover(const char *id)
{
    for (int i = 0; i < kBookCoverCount; ++i)
        if (std::strcmp(kBookCovers[i].id, id) == 0) return &kBookCovers[i];
    return nullptr;
}

// 没有真实封面图时，按文件名稳定挑一张内置封面，同一本书每次都一样。
const char *cover_for(const char *file)
{
    static const char *const kIds[] = {"little_prince", "nineteen84", "pride",
                                       "hobbit", "harry", "gatsby"};
    uint32_t h = 2166136261u;
    while (*file) {
        h ^= static_cast<uint8_t>(*file++);
        h *= 16777619u;
    }
    return kIds[h % (sizeof(kIds) / sizeof(kIds[0]))];
}

void draw_cover(const char *id, int x, int y)
{
    const auto *cv = find_cover(id);
    if (!cv) { rect_frame(x, y, x + 49, y + 63); return; }
    for (int row = 0; row < cv->h; ++row) {
        for (int b = 0; b < cv->stride; ++b) {
            uint8_t byte = cv->bits[row * cv->stride + b];
            for (int bit = 0; bit < 8; ++bit) {
                int col = b * 8 + bit;
                if (col < cv->w && (byte & (1 << (7 - bit))))
                    put(x + col, y + row, true);
            }
        }
    }
}

// ---------- 状态栏（与主菜单、番茄钟同款） ----------
constexpr int STATUS_H = 32;
constexpr int ICON_CX[5] = {170, 225, 300, 322, 365};

bool wifi_connected()
{
    char ip[kWifiStationIpTextLen] = {};
    return wifi_station_ip_snapshot(ip, sizeof(ip));
}

void draw_time(const struct tm *local)
{
    char buf[32];
    int hh = local ? local->tm_hour : 9;
    int mm = local ? local->tm_min : 41;
    int year = local ? local->tm_year + 1900 : 2026;
    int mon = local ? local->tm_mon + 1 : 11;
    int day = local ? local->tm_mday : 24;
    snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
    text12(6, 6, buf, 2);
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, mon, day);
    text12(68, 10, buf, 1, -1);
}

void draw_battery(int battery_percent)
{
    char pctbuf[12];
    int pct = battery_percent < 0 ? 75 : battery_percent;
    snprintf(pctbuf, sizeof(pctbuf), "%d%%", pct);
    int group_w = 28 + 3 + text_width12(pctbuf);
    int bx = ICON_CX[4] - group_w / 2;
    rect_frame(bx, 10, bx + 24, 22);
    fill_rect(bx + 25, 14, bx + 27, 18);
    int filled = (pct >= 75) ? 4 : (pct >= 50) ? 3 : (pct >= 25) ? 2 : (pct > 0) ? 1 : 0;
    for (int k = 0; k < 4; ++k) {
        int qx = bx + 2 + k * 5;
        if (k < filled) fill_rect(qx, 12, qx + 3, 20);
        else rect_frame(qx, 12, qx + 3, 20);
    }
    text12(bx + 31, 12, pctbuf);
}

void draw_icon_text_group(const char *icon, const char *text, int cx)
{
    int group_w = 18 + 2 + text_width12(text);
    int sx = cx - group_w / 2;
    draw_icon(icon, sx, 7);
    text12(sx + 20, 11, text);
}

void draw_env(float temperature, float humidity, bool sensor_ok)
{
    char tbuf[16], hbuf[16];
    if (sensor_ok) {
        snprintf(tbuf, sizeof(tbuf), "%.0f°C", temperature);
        snprintf(hbuf, sizeof(hbuf), "%.0f%%", humidity);
    } else {
        snprintf(tbuf, sizeof(tbuf), "--°C");
        snprintf(hbuf, sizeof(hbuf), "--%%");
    }
    draw_icon_text_group("thermometer_sun_18", tbuf, ICON_CX[0]);
    draw_icon_text_group("droplets_18", hbuf, ICON_CX[1]);
}

void draw_status_bar(const struct tm *local, int battery_percent)
{
    float temperature = 0.0f, humidity = 0.0f;
    const bool sensor_ok = get_local_sensor_snapshot(&temperature, &humidity,
                                                     nullptr, nullptr);
    draw_time(local);
    draw_env(temperature, humidity, sensor_ok);
    draw_icon(wifi_connected() ? "wifi_18" : "wifi_off_18", ICON_CX[2] - 9, 7);
    draw_icon(pad_state() == PAD_STATE_READY ? "bluetooth_18" : "bluetooth_off_18",
              ICON_CX[3] - 9, 7);
    draw_battery(battery_percent);
    fill_rect(0, STATUS_H, W - 1, STATUS_H + 2);
}

// ---------- 底栏 ----------
constexpr int BAR_Y = 282;
constexpr int BAR_TEXT_TOP = 288;

void draw_action_bar(bool reader, int dot_index)
{
    hline(0, W - 1, BAR_Y);

    if (!reader) {
        draw_icon("chevron_left_12", 8, BAR_Y + 4);
        draw_icon("chevron_right_12", 22, BAR_Y + 4);
        int x = text12(38, BAR_TEXT_TOP, "PAGE");
        text_body(x + 4, BAR_TEXT_TOP, "翻页");
        vline(96, BAR_Y + 3, H - 3);

        rounded_fill(102, BAR_Y + 4, 116, BAR_Y + 15, 2);
        text12(107, BAR_Y + 6, "A", 1, 0, false);
        x = text12(121, BAR_TEXT_TOP, "OPEN");
        text_body(x + 4, BAR_TEXT_TOP, "打开");
        vline(228, BAR_Y + 3, H - 3);

        rounded_fill(234, BAR_Y + 4, 248, BAR_Y + 15, 2);
        text12(239, BAR_Y + 6, "B", 1, 0, false);
        x = text12(253, BAR_TEXT_TOP, "BACK");
        text_body(x + 4, BAR_TEXT_TOP, "返回");

        for (int k = 0; k < 5; ++k) {
            int bx = W - 40 + k * 9;
            if (k < dot_index) fill_rect(bx, BAR_Y + 5, bx + 6, BAR_Y + 13);
            else rect_frame(bx, BAR_Y + 5, bx + 6, BAR_Y + 13);
        }
        return;
    }

    // 正文栏：<<>> A2 全屏 / ST 书签 / SE 目录 / B 返回
    draw_icon("chevron_left_12", 8, BAR_Y + 4);
    draw_icon("chevron_right_12", 22, BAR_Y + 4);

    rounded_fill(38, BAR_Y + 4, 54, BAR_Y + 15, 2);
    text12(41, BAR_Y + 6, "A2", 1, 0, false);
    text_body(60, BAR_TEXT_TOP, "全屏");
    vline(92, BAR_Y + 3, H - 3);

    rounded_fill(98, BAR_Y + 4, 114, BAR_Y + 15, 2);
    text12(101, BAR_Y + 6, "ST", 1, 0, false);
    int x = text12(120, BAR_TEXT_TOP, "MARK");
    text_body(x + 4, BAR_TEXT_TOP, "书签");
    vline(178, BAR_Y + 3, H - 3);

    rounded_fill(184, BAR_Y + 4, 200, BAR_Y + 15, 2);
    text12(187, BAR_Y + 6, "SE", 1, 0, false);
    x = text12(206, BAR_TEXT_TOP, "TOC");
    text_body(x + 4, BAR_TEXT_TOP, "目录");
    vline(258, BAR_Y + 3, H - 3);

    rounded_fill(264, BAR_Y + 4, 278, BAR_Y + 15, 2);
    text12(269, BAR_Y + 6, "B", 1, 0, false);
    x = text12(283, BAR_TEXT_TOP, "BACK");
    text_body(x + 4, BAR_TEXT_TOP, "返回");

    for (int k = 0; k < 5; ++k) {
        int bx = W - 40 + k * 9;
        if (k < dot_index) fill_rect(bx, BAR_Y + 5, bx + 6, BAR_Y + 13);
        else rect_frame(bx, BAR_Y + 5, bx + 6, BAR_Y + 13);
    }
}

// ---------- 书库版式 ----------
constexpr int SHELF_HEADER_TOP = 38;
constexpr int SHELF_GRID_TOP = 62;
constexpr int SHELF_CELL_W = 184;
constexpr int SHELF_CELL_H = 72;
constexpr int SHELF_COL_X[2] = {8, 208};
constexpr int COVER_W = 50;
constexpr int COVER_H = 64;
constexpr int kShelfCols = 2;
constexpr int kShelfRows = 3;
constexpr int kShelfPerPage = kShelfCols * kShelfRows;

void cell_origin(int local_index, int *x, int *y)
{
    *x = SHELF_COL_X[local_index % kShelfCols];
    *y = SHELF_GRID_TOP + (local_index / kShelfCols) * SHELF_CELL_H;
}

// 书名截断：16px 中文一行最多 7 字，超了补三个 ASCII 点。
void draw_cjk_clipped(int x, int top, const char *s, int max_width)
{
    if (text_body_width(s) <= max_width) {
        text_body(x, top, s);
        return;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", s);
    size_t len = std::strlen(buf);
    while (len > 0) {
        buf[len] = '\0';
        if (text_body_width(buf) + 12 <= max_width) break;
        do { --len; } while (len > 0 &&
                              (static_cast<unsigned char>(buf[len]) & 0xC0) == 0x80);
    }
    std::strncat(buf, "...", sizeof(buf) - std::strlen(buf) - 1);
    text_body(x, top, buf);
}

void draw_ascii_clipped(int x, int top, const char *s, int max_width)
{
    if (text_width12(s) <= max_width) {
        text12(x, top, s);
        return;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", s);
    size_t len = std::strlen(buf);
    while (len > 0 && text_width12(buf) + 18 > max_width) {
        buf[--len] = '\0';
    }
    std::strncat(buf, "...", sizeof(buf) - std::strlen(buf) - 1);
    text12(x, top, buf);
}

void draw_shelf_cell(int local_index, bool selected)
{
    int cx, cy;
    cell_origin(local_index, &cx, &cy);
    fill_rect(cx, cy, cx + SHELF_CELL_W - 1, cy + SHELF_CELL_H - 1, false);

    const int index = s_shelf_page * kShelfPerPage + local_index;
    const BookMeta *b = book_store_meta(index);
    if (!b) return;

    draw_cover(cover_for(b->file), cx + 2, cy + 3);

    const int tx = cx + COVER_W + 8;
    const int text_w = cx + SHELF_CELL_W - tx - 3;
    const bool has_en = b->title_en[0] != '\0';
    const bool has_zh = b->title_zh[0] != '\0';

    int y = cy + 5;
    if (has_en) {
        draw_ascii_clipped(tx, y, b->title_en, text_w);
        y = cy + 22;
    }
    if (has_zh) {
        draw_cjk_clipped(tx, y, b->title_zh, text_w);
    } else if (!has_en) {
        draw_ascii_clipped(tx, cy + 22, b->file, text_w);
    }

    // 底部一行：优先作者，没有就显示类型和大小
    char bottom[64];
    if (b->author[0]) {
        std::snprintf(bottom, sizeof(bottom), "%s", b->author);
    } else if (b->type == BOOK_TYPE_EPUB) {
        std::snprintf(bottom, sizeof(bottom), "EPUB  %u KB",
                      static_cast<unsigned>(b->size / 1024 + 1));
    } else {
        std::snprintf(bottom, sizeof(bottom), "TXT  %u KB",
                      static_cast<unsigned>(b->size / 1024 + 1));
    }
    text8(tx, cy + 52, bottom);

    if (selected) {
        rect_frame(cx, cy, cx + SHELF_CELL_W - 1, cy + SHELF_CELL_H - 1);
        rect_frame(cx + 1, cy + 1, cx + SHELF_CELL_W - 2, cy + SHELF_CELL_H - 2);
    }
}

void draw_shelf_header()
{
    int x = text12(10, SHELF_HEADER_TOP + 2, "BOOKS", 1, 0, true, true);
    text_body(x + 5, SHELF_HEADER_TOP + 2, "书库", true);

    char buf[96];
    if (s_notice[0]) {
        std::snprintf(buf, sizeof(buf), "%s", s_notice);
    } else {
        std::snprintf(buf, sizeof(buf), "%d BOOKS", book_store_count());
    }
    text12_right(392, SHELF_HEADER_TOP + 2, buf);
    hline(0, W - 1, 56);
}

void draw_shelf_empty()
{
    const int cx = W / 2;
    const char *line1 = "未检测到书";
    const char *line2 = "把 .txt 拷到 TF 卡 /sdcard/book";
    text_body(cx - text_body_width(line1) / 2, 110, line1, true);
    text_body(cx - text_body_width(line2) / 2, 140, line2);
    const char *diag = book_store_diagnostic();
    if (diag && std::strcmp(diag, "ok") != 0) {
        text_body(cx - text_body_width(diag) / 2, 170, diag);
    }
}

void draw_shelf()
{
    fill_rect(0, STATUS_H + 3, W - 1, BAR_Y - 1, false);
    draw_shelf_header();
    const int count = book_store_count();
    if (count <= 0) {
        draw_shelf_empty();
        draw_action_bar(false, 0);
        return;
    }
    for (int i = 0; i < kShelfPerPage; ++i) {
        const int index = s_shelf_page * kShelfPerPage + i;
        if (index >= count) break;
        draw_shelf_cell(i, index == s_selection);
    }
    int dots = s_shelf_page + 1;
    if (dots > 5) dots = 5;
    draw_action_bar(false, dots);
}

// ---------- 阅读页版式 ----------
constexpr int BOX_X0 = 8, BOX_Y0 = 58, BOX_X1 = 344, BOX_Y1 = 254;
constexpr int BODY_TOP = 84;
constexpr int BODY_LEFT = BOX_X0 + 8;
constexpr int RAIL_X = 350, RAIL_W = 34;
constexpr int BAR_X = 390, BAR_W = 6;
constexpr int kLineBytes = 128;

int page_percent(int page, int total)
{
    if (total <= 0) return 0;
    int pct = (page + 1) * 100 / total;
    return pct > 100 ? 100 : pct;
}

void draw_reader_header()
{
    int x = text12(10, 40, "READING", 1, 0, true, true);
    text_body(x + 5, 40, "阅读", true);

    char buf[48];
    std::snprintf(buf, sizeof(buf), "PAGE %d / %d", book_store_current_page() + 1,
                  book_store_page_count());
    text12_right(392, 40, buf);
}

void draw_reader_rail()
{
    const int page = book_store_current_page();
    const int total = book_store_page_count();
    const int pct = page_percent(page, total);

    rect_frame(RAIL_X, BOX_Y0, RAIL_X + RAIL_W - 1, BOX_Y0 + RAIL_W - 1);
    draw_bookmark(RAIL_X + 11, BOX_Y0 + 9, book_store_is_bookmarked(page));

    rect_frame(RAIL_X, BOX_Y0 + 42, RAIL_X + RAIL_W - 1, BOX_Y0 + 42 + RAIL_W - 1);
    draw_list_icon(RAIL_X + 9, BOX_Y0 + 52);
    // 目录/书签弹层打开时，把对应的右栏方框画成双线，提示当前落在哪个弹层。
    if (s_panel_open) {
        const int top = (s_panel_tab == 1) ? BOX_Y0 : BOX_Y0 + 42;
        rect_frame(RAIL_X + 1, top + 1, RAIL_X + RAIL_W - 2, top + RAIL_W - 2);
    }

    rect_frame(RAIL_X, BOX_Y0 + 84, RAIL_X + RAIL_W - 1, BOX_Y0 + 84 + RAIL_W - 1);
    char pbuf[16];
    std::snprintf(pbuf, sizeof(pbuf), "%d", pct);
    text12(RAIL_X + (RAIL_W - text_width12(pbuf)) / 2, BOX_Y0 + 92, pbuf);

    rect_frame(BAR_X, BOX_Y0, BAR_X + BAR_W - 1, BOX_Y1);
    const int filled = (BOX_Y1 - BOX_Y0) * pct / 100;
    if (filled > 0) fill_rect(BAR_X + 1, BOX_Y0 + 1, BAR_X + BAR_W - 2,
                              BOX_Y0 + filled);
}

void draw_reader_footer()
{
    hline(0, W - 1, 262);
    const int page = book_store_current_page();
    const int total = book_store_page_count();

    // 本次阅读时长，从打开这本书开始算。
    int session_min = 0;
    if (s_open_us > 0) {
        session_min = static_cast<int>((esp_timer_get_time() - s_open_us) / 60000000);
    }
    const int index = book_store_open_index();
    const int total_min =
        (index >= 0) ? static_cast<int>(book_store_reading_seconds(index) / 60) : 0;

    char sbuf[24], tbuf[24], lbuf[24];
    std::snprintf(sbuf, sizeof(sbuf), "%d min", session_min);
    if (total_min >= 60) {
        std::snprintf(tbuf, sizeof(tbuf), "%dh %dm", total_min / 60, total_min % 60);
    } else {
        std::snprintf(tbuf, sizeof(tbuf), "%d min", total_min);
    }
    std::snprintf(lbuf, sizeof(lbuf), "%d pages", total - page - 1);

    int x = text12(10, 268, "TIME");
    text_body(x + 4, 268, "本次");
    x = text12(x + 4 + 32 + 4, 268, sbuf);
    vline(x + 6, 266, 278);

    x = text12(x + 16, 268, "TOTAL");
    text_body(x + 4, 268, "累计");
    x = text12(x + 4 + 32 + 4, 268, tbuf);
    vline(x + 6, 266, 278);

    x = text12(x + 16, 268, "LEFT");
    text_body(x + 4, 268, "剩余");
    text12(x + 4 + 32 + 4, 268, lbuf);
    draw_icon("chevron_right_12", 386, 266);
}

void draw_reader_body()
{
    const int page = book_store_current_page();
    const int count = book_store_page_lines(page);
    char line[kLineBytes];
    const int pitch = book_font_line_pitch();
    const int gap_pitch = book_font_gap_pitch();
    int y = BODY_TOP;
    for (int i = 0; i < count; ++i) {
        bool gap = false;
        line[0] = '\0';
        book_store_copy_line(page, i, line, sizeof(line), &gap);
        if (line[0]) text_body(BODY_LEFT, y, line);
        y += pitch;
        if (gap) y += gap_pitch;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", page + 1);
    text12_right(BOX_X1 - 8, BOX_Y1 - 13, buf);

    // 左下角的临时提示（书签已满之类），和右下角页码错开。
    // 这行是中文，必须走 12px 中文字库，text12 只有 ASCII 位图。
    if (s_reader_notice[0]) {
        text_body(BOX_X0 + 8, BOX_Y1 - 13, s_reader_notice);
    }
}

// ---------- 目录 / 书签弹层 ----------
constexpr int PANEL_ROW_PITCH = 15;

struct PanelLayout {
    int x0, y0, x1, y1;
    int tab_y;
    int row_top;
    int rows;
};

// 普通模式弹层盖在正文框里；全屏模式没有框，铺满整屏。
PanelLayout panel_layout()
{
    PanelLayout p;
    if (s_fullscreen) {
        p.x0 = 16; p.y0 = 10; p.x1 = 384; p.y1 = 290;
    } else {
        p.x0 = 14; p.y0 = 62; p.x1 = 344; p.y1 = 250;
    }
    p.tab_y = p.y0 + 8;
    p.row_top = p.y0 + 34;
    p.rows = (p.y1 - p.row_top - 26) / PANEL_ROW_PITCH;
    if (p.rows < 1) p.rows = 1;
    return p;
}

int panel_item_count()
{
    return s_panel_tab == 0 ? book_store_toc_count() : book_store_bookmark_count();
}

// 目录弹层的条目文案：目录 tab 用章节标题，书签 tab 用该页第一行。
void panel_item_label(int i, char *buf, int cap)
{
    buf[0] = '\0';
    if (i < 0) return;
    if (s_panel_tab == 0) {
        book_store_toc_title(i, buf, cap);
        return;
    }
    const int page = book_store_bookmark_page(i);
    if (page < 0) return;
    bool gap = false;
    book_store_copy_line(page, 0, buf, cap, &gap);
    if (!buf[0]) std::snprintf(buf, cap, "第 %d 页", page + 1);
}

int panel_item_page(int i)
{
    if (i < 0) return -1;
    return s_panel_tab == 0 ? book_store_toc_page(i) : book_store_bookmark_page(i);
}

// 按宽度截断后绘制，ink=false 用于反白行。
void draw_body_clipped(int x, int y, const char *s, int max_w, bool ink)
{
    if (!s || !*s) return;
    if (text_body_width(s) <= max_w) {
        text_body(x, y, s, false, ink);
        return;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", s);
    size_t len = std::strlen(buf);
    while (len > 0) {
        buf[len] = '\0';
        if (text_body_width(buf) + 12 <= max_w) break;
        do { --len; } while (len > 0 &&
                              (static_cast<unsigned char>(buf[len]) & 0xC0) == 0x80);
    }
    std::strncat(buf, "...", sizeof(buf) - std::strlen(buf) - 1);
    text_body(x, y, buf, false, ink);
}

void draw_panel_tabs(const PanelLayout &p)
{
    const char *labels[2] = {"目录", "书签"};
    for (int t = 0; t < 2; ++t) {
        const int x = p.x0 + 18 + t * 78;
        const bool on = (s_panel_tab == t);
        text_body(x, p.tab_y, labels[t], on);
        if (on) fill_rect(x, p.tab_y + 13, x + text_body_width(labels[t]) - 1,
                          p.tab_y + 14);
    }
    hline(p.x0 + 8, p.x1 - 8, p.y0 + 26);
}

void draw_panel_empty(const PanelLayout &p, const char *line1, const char *line2)
{
    const int cx = (p.x0 + p.x1) / 2;
    text_body(cx - text_body_width(line1) / 2, p.y0 + 96, line1, true);
    if (line2 && *line2)
        text_body(cx - text_body_width(line2) / 2, p.y0 + 122, line2);
}

void draw_reader_panel()
{
    const PanelLayout p = panel_layout();
    // 弹层盖住正文区；普通模式保留状态栏、题头、右栏和底栏。
    fill_rect(p.x0, p.y0, p.x1, p.y1, false);
    rect_frame(p.x0, p.y0, p.x1, p.y1);
    rect_frame(p.x0 + 1, p.y0 + 1, p.x1 - 1, p.y1 - 1);
    draw_panel_tabs(p);

    // 长按书签的结果提示画在页签这一行的右端，全屏时底栏没有位置放。
    if (s_reader_notice[0]) {
        const int w = text_body_width(s_reader_notice);
        text_body(p.x1 - 14 - w, p.tab_y, s_reader_notice);
    }

    const int count = panel_item_count();
    if (count <= 0) {
        if (s_panel_tab == 0) {
            draw_panel_empty(p, "未识别到章节标题", "换个带「第X章」的 TXT 试试");
        } else {
            draw_panel_empty(p, "还没有书签", "阅读时按 ST 可以加一个");
        }
    } else {
        if (s_panel_sel >= count) s_panel_sel = count - 1;
        if (s_panel_sel < 0) s_panel_sel = 0;
        if (s_panel_sel < s_panel_scroll) s_panel_scroll = s_panel_sel;
        if (s_panel_sel >= s_panel_scroll + p.rows)
            s_panel_scroll = s_panel_sel - p.rows + 1;

        char label[96];
        for (int row = 0; row < p.rows; ++row) {
            const int i = s_panel_scroll + row;
            if (i >= count) break;
            const int y = p.row_top + row * PANEL_ROW_PITCH;
            const bool on = (i == s_panel_sel);
            if (on) fill_rect(p.x0 + 6, y - 2, p.x1 - 12, y + 12);
            panel_item_label(i, label, sizeof(label));
            draw_body_clipped(p.x0 + 12, y, label, p.x1 - p.x0 - 74, !on);
            char pbuf[16];
            const int page = panel_item_page(i);
            std::snprintf(pbuf, sizeof(pbuf), "P.%d", page < 0 ? 0 : page + 1);
            const int w = text_width12(pbuf);
            text12(p.x1 - 12 - w, y + 1, pbuf, 1, 0, !on);
        }
        // 长目录给一条右侧滚动条，省得用户以为只有这么多章。
        if (count > p.rows) {
            const int bar_x = p.x1 - 6;
            const int top = p.row_top - 2;
            const int bottom = top + p.rows * PANEL_ROW_PITCH - 4;
            vline(bar_x, top, bottom);
            const int span = bottom - top;
            const int thumb = span * p.rows / count;
            const int offset = span * s_panel_scroll / (count - p.rows);
            fill_rect(bar_x - 1, top + offset, bar_x + 1,
                      top + offset + (thumb < 4 ? 4 : thumb));
        }
    }

    char hint[48];
    std::snprintf(hint, sizeof(hint), "%d/%d", count > 0 ? s_panel_sel + 1 : 0, count);
    text12(p.x0 + 12, p.y1 - 15, "A JUMP  B CLOSE  <> TAB  HOLD MARK");
    text12_right(p.x1 - 10, p.y1 - 15, hint);
}

// ---------- 全屏阅读：只剩文字 ----------
constexpr int FULL_TOP = 8;
constexpr int FULL_LEFT = 12;

void draw_fullscreen_reader()
{
    fill_rect(0, 0, W - 1, H - 1, false);
    const int page = book_store_current_page();
    const int count = book_store_page_lines(page);
    char line[kLineBytes];
    const int pitch = book_font_line_pitch();
    const int gap_pitch = book_font_gap_pitch();
    int y = FULL_TOP;
    for (int i = 0; i < count; ++i) {
        bool gap = false;
        line[0] = '\0';
        book_store_copy_line(page, i, line, sizeof(line), &gap);
        if (line[0]) text_body(FULL_LEFT, y, line);
        y += pitch;
        if (gap) y += gap_pitch;
    }
    if (s_reader_notice[0]) {
        const int w = text_width12(s_reader_notice);
        text12(W - 10 - w, H - 12, s_reader_notice);
    }
    if (s_panel_open) draw_reader_panel();
}

void draw_reader()
{
    if (s_fullscreen) {
        draw_fullscreen_reader();
        return;
    }
    fill_rect(0, STATUS_H + 3, W - 1, BAR_Y - 1, false);
    draw_reader_header();

    rect_frame(BOX_X0, BOX_Y0, BOX_X1, BOX_Y1);

    // 题头：书名 + 作者，中文优先
    const BookMeta *b = book_store_meta(book_store_open_index());
    const int head_top = BOX_Y0 + 6;
    int x = BOX_X0 + 8;
    if (b) {
        if (b->title_zh[0]) {
            text_body(x, head_top, b->title_zh, true);
            x += text_body_width(b->title_zh);
        }
        if (b->title_en[0]) {
            x = text12(x + (b->title_zh[0] ? 8 : 0), head_top, b->title_en);
        }
        if (b->author[0]) {
            vline(x + 7, BOX_Y0 + 5, BOX_Y0 + 15);
            x = text12(x + 12, head_top, b->author);
        }
    }
    hline(BOX_X0 + 6, BOX_X1 - 6, BOX_Y0 + 18);

    draw_reader_body();
    if (s_panel_open) draw_reader_panel();
    draw_reader_rail();
    draw_reader_footer();

    int dots = page_percent(book_store_current_page(), book_store_page_count()) / 21 + 1;
    if (dots > 5) dots = 5;
    draw_action_bar(true, dots);
}

} // namespace

// ---------------- 公共接口 ----------------

void build_reading_ui()
{
    if (s_root) return;
    s_root = create_page_root();
    if (!s_root) {
        ESP_LOGW(TAG, "create page root failed");
        return;
    }
    s_canvas = lv_canvas_create(s_root);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_size(s_canvas, W, H);
    lv_obj_set_style_pad_all(s_canvas, 0, 0);
    lv_obj_set_style_border_width(s_canvas, 0, 0);

    s_buf = alloc_canvas_buffer(W, H);
    if (!s_buf) {
        ESP_LOGW(TAG, "canvas buffer alloc failed");
        return;
    }
    lv_canvas_set_buffer(s_canvas, s_buf, W, H, LV_IMG_CF_TRUE_COLOR);
    set_auxiliary_page_root(AuxiliaryPage::kReading, s_root);
}

lv_obj_t *reading_root()
{
    return auxiliary_page_root(AuxiliaryPage::kReading);
}

bool reading_shown()
{
    return s_shown.load(std::memory_order_acquire);
}

void reading_show()
{
    build_reading_ui();
    s_shown.store(true, std::memory_order_release);
    bump_state();
}

void reading_hide()
{
    if (s_open_us > 0) {
        const int sec = static_cast<int>((esp_timer_get_time() - s_open_us) / 1000000);
        book_store_add_reading_seconds(book_store_open_index(), sec);
        s_open_us = 0;
    }
    book_store_close();
    if (s_fullscreen) {
        s_fullscreen = false;
        book_store_set_fullscreen(false);
    }
    s_view = ReadingView::kShelf;
    s_store_loaded = false;
    s_panel_open = false;
    s_reader_notice[0] = '\0';
    s_shown.store(false, std::memory_order_release);
}

ReadingView reading_view()
{
    return s_view;
}

uint32_t reading_state_version()
{
    return s_state_version.load(std::memory_order_acquire);
}

void reading_open_selected()
{
    const int count = book_store_count();
    if (s_selection < 0 || s_selection >= count) return;

    s_notice[0] = '\0';
    if (!book_store_open(s_selection)) {
        std::snprintf(s_notice, sizeof(s_notice), "%s", book_store_diagnostic());
        ESP_LOGW(TAG, "open book %d failed: %s", s_selection, s_notice);
        bump_state();
        return;
    }
    s_view = ReadingView::kReader;
    s_open_us = esp_timer_get_time();
    s_panel_open = false;
    s_reader_notice[0] = '\0';
    bump_state();
}

bool reading_back_to_shelf()
{
    if (s_view == ReadingView::kShelf) return false;
    if (s_open_us > 0) {
        const int sec = static_cast<int>((esp_timer_get_time() - s_open_us) / 1000000);
        book_store_add_reading_seconds(book_store_open_index(), sec);
        s_open_us = 0;
    }
    // 先按最近阅读重排（书还开着，内部会把打开下标搬过去），再关书。
    const int keep = book_store_resort_recent();
    book_store_close();
    // 书架始终用普通版式，全屏状态在退回来时清掉。
    if (s_fullscreen) {
        s_fullscreen = false;
        book_store_set_fullscreen(false);
    }
    s_view = ReadingView::kShelf;
    s_panel_open = false;
    s_reader_notice[0] = '\0';
    if (keep >= 0 && keep < book_store_count()) {
        // 刚读的这本现在排在最前面，选中项跟着它走，回书架就能直接看到。
        s_selection = keep;
        s_shelf_page = keep / kShelfPerPage;
    }
    bump_state();
    return true;
}

void reading_move_selection(int dx, int dy)
{
    const int count = book_store_count();
    if (count <= 0) return;

    int local = s_selection - s_shelf_page * kShelfPerPage;
    if (local < 0 || local >= kShelfPerPage) local = 0;
    const int col = local % kShelfCols;
    const int row = local / kShelfCols;
    int next_col = col + dx;
    int next_row = row + dy;

    // 左右越界时翻一页书架
    if (next_col < 0) {
        if (s_shelf_page > 0) {
            --s_shelf_page;
            s_selection = s_shelf_page * kShelfPerPage + 1;
            bump_state();
        }
        return;
    }
    if (next_col >= kShelfCols) {
        if ((s_shelf_page + 1) * kShelfPerPage < count) {
            ++s_shelf_page;
            s_selection = s_shelf_page * kShelfPerPage;
            bump_state();
        }
        return;
    }
    if (next_row < 0) next_row = 0;
    if (next_row >= kShelfRows) next_row = kShelfRows - 1;

    int next = s_shelf_page * kShelfPerPage + next_row * kShelfCols + next_col;
    if (next >= count) next = count - 1;
    if (next != s_selection) {
        s_selection = next;
        bump_state();
    }
}

int reading_selection()
{
    return s_selection;
}

void reading_turn_page(int delta)
{
    if (book_store_page_count() <= 0) return;
    const int next = book_store_current_page() + delta;
    s_reader_notice[0] = '\0';
    if (book_store_goto_page(next)) bump_state();
}

// ---------- 书签 / 目录弹层 ----------
bool reading_panel_shown()
{
    return s_panel_open && s_view == ReadingView::kReader;
}

void reading_panel_open(int tab)
{
    if (s_view != ReadingView::kReader) return;
    s_panel_open = true;
    s_panel_tab = (tab == 1) ? 1 : 0;
    s_panel_sel = 0;
    s_panel_scroll = 0;
    if (s_panel_tab == 0) {
        // 目录默认停在当前位置之前最近的一条，翻目录时不用从头找。
        const int cur = book_store_current_page();
        const int n = book_store_toc_count();
        int best = 0;
        for (int i = 0; i < n; ++i) {
            if (book_store_toc_page(i) <= cur) best = i;
            else break;
        }
        s_panel_sel = best;
    }
    bump_state();
}

void reading_panel_close()
{
    if (!s_panel_open) return;
    s_panel_open = false;
    bump_state();
}

void reading_panel_move(int delta)
{
    if (!s_panel_open || delta == 0) return;
    const int count = panel_item_count();
    if (count <= 0) return;
    int next = s_panel_sel + delta;
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    if (next == s_panel_sel) return;
    s_panel_sel = next;
    bump_state();
}

void reading_panel_switch_tab(int dir)
{
    if (!s_panel_open || dir == 0) return;
    const int next = (dir > 0) ? 1 : 0;
    if (next == s_panel_tab) return;
    s_panel_tab = next;
    s_panel_sel = 0;
    s_panel_scroll = 0;
    bump_state();
}

void reading_panel_activate()
{
    if (!s_panel_open) return;
    const int page = panel_item_page(s_panel_sel);
    if (page >= 0) {
        s_reader_notice[0] = '\0';
        book_store_goto_page(page);
    }
    s_panel_open = false;
    bump_state();
}

int reading_toggle_bookmark()
{
    if (s_view != ReadingView::kReader) return -1;
    const int page = book_store_current_page();
    const int r = book_store_toggle_bookmark(page);
    if (r > 0) {
        std::snprintf(s_reader_notice, sizeof(s_reader_notice), "已加书签 P.%d", page + 1);
    } else if (r == 0) {
        std::snprintf(s_reader_notice, sizeof(s_reader_notice), "已删书签 P.%d", page + 1);
    } else {
        std::snprintf(s_reader_notice, sizeof(s_reader_notice), "书签已满 8 个");
    }
    bump_state();
    return r;
}

// 弹层里长按 BOOT（没有手柄时唯一的加书签通道）。
// 目录页签：给当前阅读页加/删书签；书签页签：删掉选中那条，顺带能清理旧书签。
void reading_panel_bookmark()
{
    if (!reading_panel_shown()) return;

    if (s_panel_tab == 1 && book_store_bookmark_count() > 0) {
        const int page = book_store_bookmark_page(s_panel_sel);
        if (page < 0) return;
        book_store_toggle_bookmark(page);
        std::snprintf(s_reader_notice, sizeof(s_reader_notice), "已删书签 P.%d", page + 1);
        // 删完列表变短，选中项跟着收一格，免得指到空行。
        const int left = book_store_bookmark_count();
        if (s_panel_sel >= left) s_panel_sel = left > 0 ? left - 1 : 0;
        bump_state();
        return;
    }

    reading_toggle_bookmark();
    if (s_panel_tab == 1) {
        // 刚加的书签按页号插在中间，选中项跟过去，用户能立刻看见。
        const int cur = book_store_current_page();
        for (int i = 0; i < book_store_bookmark_count(); ++i) {
            if (book_store_bookmark_page(i) == cur) { s_panel_sel = i; break; }
        }
        bump_state();
    }
}

bool reading_fullscreen()
{
    return s_fullscreen;
}

void reading_toggle_fullscreen()
{
    if (s_view != ReadingView::kReader) return;
    // 换排版时页号会对不上，先把弹层关掉，免得选中的目录项指到别的页。
    s_panel_open = false;
    s_reader_notice[0] = '\0';
    s_fullscreen = !s_fullscreen;
    if (!book_store_set_fullscreen(s_fullscreen)) {
        // 分页失败（内存不足）就退回原样，别把用户卡在白屏里。
        s_fullscreen = !s_fullscreen;
        book_store_set_fullscreen(s_fullscreen);
        std::snprintf(s_reader_notice, sizeof(s_reader_notice), "切换全屏失败");
    }
    bump_state();
}

void reading_redraw_all(const struct tm *local, int battery_percent)
{
    if (!s_buf) return;
    if (!s_store_loaded) {
        // 首次进阅读模块：先扫书（内部会等 SD 挂载就绪），再载字库。
        // 顺序不能反：卡还没挂上时载字库一定失败，会白白回退到 16px，
        // 而字库又决定排版度量，回退后整本书的页数都是按 16px 算的。
        const int n = book_store_refresh();
        if (!book_font_load()) {
            ESP_LOGW(TAG, "12px font not loaded, falling back to built-in 16px");
        }
        s_store_loaded = true;
        if (n > 0) {
            if (s_selection >= n) s_selection = 0;
            s_shelf_page = s_selection / kShelfPerPage;
        }
        ESP_LOGI(TAG, "book store refreshed: %d books", n);
    }
    fill_rect(0, 0, W - 1, H - 1, false);
    // 全屏阅读时状态栏不画，省得两套版式互相擦。
    if (!(s_view == ReadingView::kReader && s_fullscreen))
        draw_status_bar(local, battery_percent);
    if (s_view == ReadingView::kReader) draw_reader();
    else draw_shelf();
    invalidate_rect(0, 0, W - 1, H - 1);
}

void reading_refresh_status_bar(const struct tm *local, int battery_percent)
{
    if (!s_buf) return;
    if (s_fullscreen) return;   // 全屏没有状态栏
    fill_rect(0, 0, W - 1, STATUS_H + 2, false);
    draw_status_bar(local, battery_percent);
    invalidate_rect(0, 0, W - 1, STATUS_H + 2);
}

void reading_refresh_selection(int previous_index)
{
    if (!s_buf) return;
    if (s_view != ReadingView::kShelf) return;
    const int page_start = s_shelf_page * kShelfPerPage;
    if (previous_index >= page_start && previous_index < page_start + kShelfPerPage) {
        const int local = previous_index - page_start;
        draw_shelf_cell(local, false);
        int px, py;
        cell_origin(local, &px, &py);
        invalidate_rect(px, py, px + SHELF_CELL_W - 1, py + SHELF_CELL_H - 1);
    }
    const int cur = s_selection - page_start;
    if (cur >= 0 && cur < kShelfPerPage) {
        draw_shelf_cell(cur, true);
        int cx, cy;
        cell_origin(cur, &cx, &cy);
        invalidate_rect(cx, cy, cx + SHELF_CELL_W - 1, cy + SHELF_CELL_H - 1);
    }
}
