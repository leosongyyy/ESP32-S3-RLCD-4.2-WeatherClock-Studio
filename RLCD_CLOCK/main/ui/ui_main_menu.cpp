// 主菜单主页实现。像素级复刻 tests/mainmenu_preview.py 的版式：
// 文字用 Fusion Pixel 取模字库，图标用 Lucide SVG 栅格化的点阵，
// 全部写进一张 400x300 的 LVGL canvas（等价 1bit，只有黑白）。
#include "ui_main_menu.h"

#include "app_display_config.h"
#include "engine_api.h"         // 引擎层唯一 API 面（含手柄状态）
#include "local_sensor_state.h" // get_local_sensor_snapshot 温湿度
#include "wifi_portal_state.h"  // wifi_station_ip_snapshot WiFi 连接状态
#include "ui_canvas_primitives.h"
#include "ui_page_state.h"

#include "assets/fusion_pixel_12_font.h"
#include "assets/fusion_pixel_8_font.h"
#include "assets/menu_icons.h"
#include "calendar_lunar.h"      // 当天节气，天气卡片状态行
#include "pomodoro/pomodoro_classic.h"

#include <esp_log.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

constexpr int W = kDisplayWidth;
constexpr int H = kDisplayHeight;
constexpr char TAG[] = "ui_main_menu";

lv_obj_t *s_root = nullptr;
lv_obj_t *s_canvas = nullptr;
lv_color_t *s_buf = nullptr;
int s_selection = 0;
std::atomic<bool> s_shown{false};

// 数据结构与 Python pixel_font.py 的 GLYPHS 完全对应。
struct Glyph12 {
    uint16_t cp;
    uint16_t off;
    uint8_t w, h, xoff, yoff, dw, row_bytes;
};

const fusion_pixel_12_glyph_t *find_glyph(uint32_t cp)
{
    // 索引按码点升序，二分查找。
    int lo = 0, hi = FUSION_PIXEL_12_INDEX_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t cur = fusion_pixel_12_index[mid].cp;
        if (cur == cp) return &fusion_pixel_12_index[mid];
        if (cur < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

const fusion_pixel_8_glyph_t *find_glyph_small(uint32_t cp)
{
    int lo = 0, hi = FUSION_PIXEL_8_INDEX_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t cur = fusion_pixel_8_index[mid].cp;
        if (cur == cp) return &fusion_pixel_8_index[mid];
        if (cur < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

// UTF-8 取一个码点，返回消耗字节数。
uint32_t utf8_next(const char **p)
{
    const unsigned char *s = reinterpret_cast<const unsigned char *>(*p);
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
    for (int i = 1; i < n; ++i) {
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p = reinterpret_cast<const char *>(s + n);
    return cp;
}

// ---------- 画点原语（写 LVGL canvas，黑=墨） ----------
// 直接写 canvas 帧缓冲，不走 lv_canvas_set_px。
// lv_canvas_set_px 内部每画一个点都会 lv_obj_invalidate 整个 canvas，
// 全屏白底重画会把脏区撑到整屏 400px，超过局刷阈值触发电子纸全刷（闪烁）。
// 这里只改像素，标脏交给各局部重画函数用 invalidate_rect 精确控制。
inline void put(int x, int y, bool ink)
{
    if (!s_buf || x < 0 || x >= W || y < 0 || y >= H) return;
    s_buf[y * W + x] = ink ? lv_color_black() : lv_color_white();
}

// 精确标脏一个矩形（canvas 挂在 0,0，canvas 坐标即屏幕坐标）。
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

// 圆角实心矩形（按键帽），半径很小，1bit 下只切四个角。
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

// ---------- 文字 ----------
// 对齐 pixel_font._draw：top = baseline - (yoff + h)，
// 调用处给的 top 是「英文顶部」，baseline = top + ascent。
template <typename G>
void blit_glyph(const G *g, const uint8_t *bits,
                int x, int glyph_top, int scale, bool ink)
{
    if (!g || g->row_bytes == 0) return;  // 空格等零行字形
    for (int r = 0; r < g->h; ++r) {
        for (int col = 0; col < g->w; ++col) {
            int byte = bits[g->off + r * g->row_bytes + col / 8];
            if (byte & (1 << (7 - (col % 8)))) {
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale; ++sx)
                        put(x + (g->xoff + col) * scale + sx,
                            glyph_top + r * scale + sy, ink);
            }
        }
    }
}

// 主字库（12px），extra 为负字距。返回末端 x。
int text12(int x, int top, const char *s, int scale = 1, int extra = 0,
           bool ink = true)
{
    constexpr int ascent = 9;  // 12px 大写墨迹高
    const char *p = s;
    int cx = x;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        const auto *g = find_glyph(cp);
        if (g) {
            int glyph_top = (top + ascent * scale) - (g->yoff + g->h) * scale;
            blit_glyph(g, fusion_pixel_12_bits, cx, glyph_top, scale, ink);
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
        const auto *g = find_glyph(cp);
        adv += (g ? g->dw : 6) * scale + extra;
    }
    return adv;
}

void text12_center(int cx, int top, const char *s, int scale = 1,
                   int extra = 0, bool ink = true)
{
    text12(cx - text_width12(s, scale, extra) / 2, top, s, scale, extra, ink);
}

// 小字库（8px，仅 ASCII）。
int text8(int x, int top, const char *s, int extra = 0, bool ink = true)
{
    constexpr int ascent = 6;
    int cx = x;
    for (const char *p = s; *p; ++p) {
        const auto *g = find_glyph_small(static_cast<uint8_t>(*p));
        if (g) {
            int glyph_top = (top + ascent) - (g->yoff + g->h);
            blit_glyph(g, fusion_pixel_8_bits, cx, glyph_top, 1, ink);
            cx += g->dw + extra;
        } else {
            cx += 6 + extra;
        }
    }
    return cx;
}

int text_width8(const char *s, int extra = 0)
{
    int adv = 0;
    for (const char *p = s; *p; ++p) {
        const auto *g = find_glyph_small(static_cast<uint8_t>(*p));
        adv += (g ? g->dw : 6) + extra;
    }
    return adv;
}

void text8_center(int cx, int top, const char *s, int extra = 0, bool ink = true)
{
    text8(cx - text_width8(s, extra) / 2, top, s, extra, ink);
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
            for (int bit = 0; bit < 8; ++bit) {
                if (byte & (1 << (7 - bit))) put(x + b * 8 + bit, y + row, true);
            }
        }
    }
}

// ---------- 版式常量（与预览一致） ----------
constexpr int STATUS_H = 32;
// 右侧五个状态图标（温度/湿度/WiFi/蓝牙/电量）的显式中心坐标，与预览 mainmenu_preview.py 一致。
// 日期时间占左侧 0..118；温度 170 / 湿度 225；WiFi 300 / 蓝牙 322 紧挨并贴近电池（电框左边缘 340）；电量 365。
constexpr int ICON_CX[5] = {170, 225, 300, 322, 365};
constexpr int icon_cx(int i) { return ICON_CX[i]; }
constexpr int TITLE_Y = STATUS_H + 1;
constexpr int CARD_W = 90, CARD_H = 104;
constexpr int GAP_X = 9, GAP_V = 5;
constexpr int BAR_Y = 282;
constexpr int X0 = (W - (CARD_W * 4 + GAP_X * 3)) / 2;
constexpr int ROW1_TOP = BAR_Y - GAP_V - CARD_H;
constexpr int ROW0_TOP = ROW1_TOP - GAP_V - CARD_H;
constexpr int ICON_SIZE = 34;

struct Card {
    const char *en, *zh, *icon;
    MainMenuAction action;
};

constexpr Card kCards[8] = {
    {"WEATHER",      "天气时钟", "cloud_sun_34",  MainMenuAction::kWeatherClock},
    {"POMODORO",     "番茄钟",   "timer_34",      MainMenuAction::kPomodoro},
    {"GAMES",        "游戏",     "gamepad_2_34",  MainMenuAction::kGameBoy},
    {"READING",      "阅读",     "book_open_34",  MainMenuAction::kNone},
    {"MUSIC",        "音乐",     "music_34",      MainMenuAction::kNone},
    {"NEWS",         "新闻",     "newspaper_34",  MainMenuAction::kNone},
    {"AI ASSISTANT", "AI助手",   "bot_34",        MainMenuAction::kNone},
    {"SETTINGS",     "设置",     "settings_34",   MainMenuAction::kSettings},
};

// 状态栏左上角日期时间区（同一行：HH:MM 大字 + YYYY-MM-DD 小字）。
// HH:MM scale=2 宽 56px，日期 scale=1 负字距宽 50px，整块 4..118。
constexpr int TIME_X0 = 2, TIME_X1 = 120;
constexpr int TIME_Y0 = 0, TIME_Y1 = STATUS_H - 2;

void draw_time(const struct tm *local)
{
    char buf[32];
    int hh = local ? local->tm_hour : 9;
    int mm = local ? local->tm_min : 41;
    int year = local ? local->tm_year + 1900 : 2026;
    int mon = local ? local->tm_mon + 1 : 11;
    int day = local ? local->tm_mday : 24;
    snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
    text12(6, 6, buf, 2);                  // 大字，绘制 y≈6..24
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, mon, day);
    text12(68, 10, buf, 1, -1);            // 小字，绘制 y≈10..22
}

// 电量（第 5 槽，含百分比文字）的局刷范围。
constexpr int BATT_X1 = icon_cx(4) - 30;
constexpr int BATT_X2 = W - 2;
constexpr int BATT_Y0 = 8, BATT_Y1 = STATUS_H - 2;

void draw_battery(int battery_percent)
{
    char pctbuf[12];
    int pct = battery_percent < 0 ? 75 : battery_percent;
    snprintf(pctbuf, sizeof(pctbuf), "%d%%", pct);
    // 整组 = 电框含电极 28px + 间隔 3px + 百分比文字宽，按组宽居中到槽中心。
    int group_w = 28 + 3 + text_width12(pctbuf);
    int bx = icon_cx(4) - group_w / 2;
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

// 温度（第 1 槽）、湿度（第 2 槽）、WiFi（第 3 槽）、蓝牙（第 4 槽）是可变数据，单独抽出供局刷。
constexpr int ENV_X1 = icon_cx(0) - 24, ENV_X2 = icon_cx(1) + 28;
constexpr int ENV_Y0 = 5, ENV_Y1 = STATUS_H - 2;
constexpr int WIFI_X1 = icon_cx(2) - 12, WIFI_X2 = icon_cx(2) + 12;
constexpr int WIFI_Y0 = 5, WIFI_Y1 = STATUS_H - 2;
constexpr int BT_X1 = icon_cx(3) - 12, BT_X2 = icon_cx(3) + 12;
constexpr int BT_Y0 = 5, BT_Y1 = STATUS_H - 2;

// 画「18px 图标 + 文字」组，按整组墨宽居中到槽中心 cx。
void draw_icon_text_group(const char *icon, const char *text, int cx)
{
    int group_w = 18 + 2 + text_width12(text);  // 图标 18 + 间隔 2 + 文字
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
    draw_icon_text_group("thermometer_sun_18", tbuf, icon_cx(0));
    draw_icon_text_group("droplets_18", hbuf, icon_cx(1));
}

// 蓝牙（第 5 格）：未连接用 bluetooth-off，已连接用 bluetooth，不再用圆点。
void draw_bt_icon(bool connected)
{
    int cx = icon_cx(3);
    draw_icon(connected ? "bluetooth_18" : "bluetooth_off_18", cx - 9, 7);
}

// WiFi（第 4 格）：未连接用 wifi-off，已连接用 wifi，不再用圆点。
void draw_wifi_icon(bool connected)
{
    int cx = icon_cx(2);
    draw_icon(connected ? "wifi_18" : "wifi_off_18", cx - 9, 7);
}

// 整条状态栏（首次全量构建时用）。
void draw_status_bar(const struct tm *local, int battery_percent,
                     float temperature, float humidity, bool sensor_ok,
                     bool pad_ready, bool wifi_on)
{
    draw_time(local);
    draw_env(temperature, humidity, sensor_ok);
    draw_wifi_icon(wifi_on);
    draw_bt_icon(pad_ready);
    draw_battery(battery_percent);

    // 3px 粗横线，与下方区隔
    fill_rect(0, STATUS_H, W - 1, STATUS_H + 2);
}

void draw_title_bar()
{
    text12(6, TITLE_Y + 5, "MAIN MENU", 2);
    int x = 6 + text_width12("MAIN MENU", 2) + 8;
    text12(x, TITLE_Y + 5, "主菜单", 2);
    const char *ver = "YUAN'S SYS V1.3";
    text12(W - 6 - text_width12(ver), TITLE_Y + 8, ver);
}

void draw_card(int i, const struct tm *local, bool selected)
{
    int col = i % 4, row = i / 4;
    int x = X0 + col * (CARD_W + GAP_X);
    int y = row == 0 ? ROW0_TOP : ROW1_TOP;
    const Card &card = kCards[i];

    rect_frame(x, y, x + CARD_W, y + CARD_H);
    if (selected) {
        // 内框加粗到 2px（两条相邻线），让选中态更明显
        rect_frame(x + 3, y + 3, x + CARD_W - 3, y + CARD_H - 3);
        rect_frame(x + 4, y + 4, x + CARD_W - 4, y + CARD_H - 4);
    }

    int cx = x + CARD_W / 2;
    draw_icon(card.icon, cx - ICON_SIZE / 2, y + 10);

    int extra = static_cast<int>(std::strlen(card.en)) > 11 ? -1 : 0;
    text8_center(cx, y + 49, card.en, extra);
    text12_center(cx, y + 62, card.zh);
    hline(x + 6, x + CARD_W - 6, y + 78);

    // 状态行（沿用预览的静态占位数据）
    if (i == 0) {  // weather：当天星期几，逢节气追加「 节气」
        static constexpr const char *kWeekdays[7] = {
            "周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        int wday = local ? local->tm_wday : 3;
        if (wday < 0 || wday > 6) wday = 3;
        char status[24];
        const char *term = nullptr;
        if (local) {
            int year = local->tm_year + 1900;
            int mon = local->tm_mon + 1;
            term = calendar_solar_term_text(year, mon, local->tm_mday);
        }
        if (term && term[0]) {
            snprintf(status, sizeof(status), "%s %s", kWeekdays[wday], term);
        } else {
            snprintf(status, sizeof(status), "%s", kWeekdays[wday]);
        }
        text12_center(cx, y + 83, status);
    } else if (i == 1) {  // pomodoro
        // 模式时长（25:00 专注 / 05:00 短休 / 15:00 长休）与今日完成数
        // 都取自经典番茄核心，和番茄钟页保持同步。
        ClassicSnapshot snap = {};
        classic_get_snapshot(&snap);
        char mode_time[12];
        int mm = static_cast<int>(snap.phase_duration_sec / 60);
        int ss = static_cast<int>(snap.phase_duration_sec % 60);
        snprintf(mode_time, sizeof(mode_time), "%02d:%02d", mm, ss);
        text12(x + 10, y + 83, mode_time);
        int done = snap.today_count > 4 ? 4 : snap.today_count;
        for (int k = 0; k < 4; ++k) {
            int bx = x + 48 + k * 9;
            if (k < done) fill_rect(bx, y + 84, bx + 6, y + 90);
            else rect_frame(bx, y + 84, bx + 6, y + 90);
        }
    } else if (i == 2) {  // games
        text12_center(cx, y + 83, "HI 12480");
    } else if (i == 3) {  // reading
        text12(x + 8, y + 83, "38%");
        for (int k = 0; k < 6; ++k) {
            int bx = x + 36 + k * 8;
            if (k < 2) fill_rect(bx, y + 84, bx + 6, y + 90);
            else rect_frame(bx, y + 84, bx + 6, y + 90);
        }
    } else if (i == 4) {  // music
        text12_center(cx, y + 81, "NOW PLAYING", -1);
        const int heights[9] = {3, 5, 8, 4, 9, 6, 3, 8, 5};
        for (int k = 0; k < 9; ++k) {
            int bx = x + 18 + k * 7;
            fill_rect(bx, y + 101 - heights[k], bx + 4, y + 101);
        }
    } else if (i == 5) {  // news
        text12_center(cx, y + 83, "12 NEW");
    } else if (i == 6) {  // ai
        text12_center(cx, y + 83, "READY");
    } else {  // settings
        text12(x + 8, y + 83, "EN");
        text12(x + 26, y + 82, "/中");
        text12(x + 56, y + 82, "双语");
    }
}

void draw_bottom_bar()
{
    hline(0, W - 1, BAR_Y);
    // 图标与按键框下移 1px，使墨迹中心与文字中线 y=292 对齐。
    draw_icon("dpad_12", 8, BAR_Y + 4);
    int x = 30;
    x = text12(x, BAR_Y + 6, "MOVE", 1, -1);   // scale=1, extra=-1
    text12(x + 4, BAR_Y + 6, "移动");
    vline(88, BAR_Y + 3, H - 3);

    rounded_fill(94, BAR_Y + 4, 108, BAR_Y + 15, 2);
    text12(99, BAR_Y + 6, "A", 1, 0, false);
    x = text12(113, BAR_Y + 5, "SELECT");
    text12(x + 4, BAR_Y + 6, "确认");
    vline(196, BAR_Y + 3, H - 3);

    rounded_fill(202, BAR_Y + 4, 216, BAR_Y + 15, 2);
    text12(207, BAR_Y + 6, "B", 1, 0, false);
    x = text12(221, BAR_Y + 5, "BACK");
    text12(x + 4, BAR_Y + 6, "返回");

    for (int k = 0; k < 4; ++k) {
        int bx = W - 40 + k * 9;
        if (k < 3) fill_rect(bx, BAR_Y + 5, bx + 6, BAR_Y + 13);
        else rect_frame(bx, BAR_Y + 5, bx + 6, BAR_Y + 13);
    }
}

// 卡片外框范围（含外框本身，局刷标脏用）。
void card_bounds(int i, int *x1, int *y1, int *x2, int *y2)
{
    int col = i % 4, row = i / 4;
    *x1 = X0 + col * (CARD_W + GAP_X);
    *y1 = row == 0 ? ROW0_TOP : ROW1_TOP;
    *x2 = *x1 + CARD_W;
    *y2 = *y1 + CARD_H;
}

// 只重画一张卡片：擦掉该卡矩形 → 按指定选中态重画 → 精确标脏这一块。
// 单卡宽 90px，远低于 280px 局刷阈值，切换选中不再全屏闪。
void redraw_card(int i, bool selected)
{
    if (!s_canvas) return;
    int x1, y1, x2, y2;
    card_bounds(i, &x1, &y1, &x2, &y2);
    fill_rect(x1, y1, x2, y2, false);
    draw_card(i, nullptr, selected);
    invalidate_rect(x1, y1, x2, y2);
}

// WiFi 是否已连上 AP。拿到 station IP 即认为连接成功（跨任务安全快照）。
bool wifi_connected()
{
    char ip[kWifiStationIpTextLen] = {};
    return wifi_station_ip_snapshot(ip, sizeof(ip));
}

// 全量重画（首次构建用）：画完后标脏整屏，让首帧建立正确底色。
void redraw(const struct tm *local, int battery_percent)
{
    if (!s_canvas) return;
    float temperature = 0.0f, humidity = 0.0f;
    const bool sensor_ok = get_local_sensor_snapshot(&temperature, &humidity,
                                                     nullptr, nullptr);
    const bool pad_ready = pad_state() == PAD_STATE_READY;
    fill_rect(0, 0, W - 1, H - 1, false);  // 全局白底
    draw_status_bar(local, battery_percent, temperature, humidity, sensor_ok,
                    pad_ready, wifi_connected());
    draw_title_bar();
    for (int i = 0; i < 8; ++i) draw_card(i, local, i == s_selection);
    draw_bottom_bar();
    invalidate_rect(0, 0, W - 1, H - 1);
}

} // namespace

void build_main_menu_ui()
{
    if (s_root) return;
    // 主菜单要显示今日番茄数，这里首次构建时确保经典核心已加载 NVS。
    // classic_init 幂等，进入番茄钟页再调也不会重复初始化。
    // 此时 NVS 与时区已在 app_main 就绪，可安全做跨天清零与统计读取。
    classic_init();
    s_root = create_page_root();
    if (!s_root) {
        ESP_LOGW(TAG, "create page root failed");
        return;
    }
    // 必须用 lv_canvas_create：lv_canvas_set_buffer/lv_canvas_set_px 会把对象
    // 强转为 lv_canvas_t* 写 img 扩展区，用 lv_obj_create 会越界踩坏相邻对象。
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
    set_auxiliary_page_root(AuxiliaryPage::kMainMenu, s_root);
    redraw(nullptr, -1);
}

lv_obj_t *main_menu_root()
{
    return auxiliary_page_root(AuxiliaryPage::kMainMenu);
}

bool main_menu_shown()
{
    return s_shown.load(std::memory_order_acquire);
}

void main_menu_show()
{
    build_main_menu_ui();
    s_shown.store(true, std::memory_order_release);
}

void main_menu_hide()
{
    s_shown.store(false, std::memory_order_release);
}

int main_menu_selection()
{
    return s_selection;
}

// 物理 BOOT 的循环步进。8 个卡片按行优先索引 0..7 排列，
// +1 从 3 走到 4（第一排末尾进第二排），从 7 回到 0；
// -1 反向，从 0 回到 7。
void main_menu_step(int delta)
{
    if (delta == 0) return;
    int next = (s_selection + delta) % 8;
    if (next < 0) next += 8;
    main_menu_set_selection(next);
}

void main_menu_set_selection(int index)
{
    if (index < 0 || index >= 8) return;
    if (index == s_selection) return;
    const int old = s_selection;
    s_selection = index;
    // 只重画旧卡（去双框）和新卡（加双框），两块各自标脏，走局刷。
    redraw_card(old, false);
    redraw_card(index, true);
}

void main_menu_move(int dx, int dy)
{
    int col = s_selection % 4;
    int row = s_selection / 4;
    if (dx < 0 && col > 0) --col;
    else if (dx > 0 && col < 3) ++col;
    else if (dy < 0 && row > 0) --row;
    else if (dy > 0 && row < 1) ++row;
    else return;
    main_menu_set_selection(row * 4 + col);
}

MainMenuAction main_menu_current_action()
{
    return kCards[s_selection].action;
}

// 状态栏局部刷新：时间 / 温湿度 / 蓝牙圆点 / 电量各自独立标脏，
// 每块都远低于 280px 局刷阈值，走局刷不闪。
// UI 任务在主菜单帧持 LVGL 锁调用本函数。
bool update_main_menu(const struct tm *local, int battery_percent)
{
    if (!s_canvas) return false;
    static char last_time[64] = "";
    static char last_env[24] = "";
    static int last_pct = -999;
    static int last_bt = -1;
    static int last_wifi = -1;
    static int last_pomo_key = -1;
    static int last_date_key = -1;

    char time_key[64];
    snprintf(time_key, sizeof(time_key), "%04d%02d%02d%02d%02d",
             local ? local->tm_year + 1900 : 0,
             local ? local->tm_mon + 1 : 0,
             local ? local->tm_mday : 0,
             local ? local->tm_hour : 0,
             local ? local->tm_min : 0);
    const bool time_changed = std::strcmp(time_key, last_time) != 0;
    const bool pct_changed = battery_percent != last_pct;

    float temperature = 0.0f, humidity = 0.0f;
    const bool sensor_ok = get_local_sensor_snapshot(&temperature, &humidity,
                                                     nullptr, nullptr);
    char env_key[24];
    snprintf(env_key, sizeof(env_key), "%d|%.0f|%.0f",
             sensor_ok ? 1 : 0, temperature, humidity);
    const bool env_changed = std::strcmp(env_key, last_env) != 0;

    const int pad_ready = pad_state() == PAD_STATE_READY ? 1 : 0;
    const bool bt_changed = pad_ready != last_bt;
    const int wifi_on = wifi_connected() ? 1 : 0;
    const bool wifi_changed = wifi_on != last_wifi;

    // 番茄钟卡片：模式时长或今日完成数变化时局刷该卡（单卡 90px，局刷不闪）。
    ClassicSnapshot snap = {};
    classic_get_snapshot(&snap);
    const int done = snap.today_count > 4 ? 4 : snap.today_count;
    const int pomo_key = static_cast<int>(snap.phase) * 100 + done;
    const bool pomo_changed = pomo_key != last_pomo_key;

    // 天气卡片状态行（星期几 / 节气）按天变化，跨天重画卡片 0。
    const int date_key = local ? (local->tm_year * 10000 +
                                  (local->tm_mon + 1) * 100 + local->tm_mday)
                               : 0;
    const bool date_changed = date_key != last_date_key;

    if (!time_changed && !pct_changed && !env_changed && !bt_changed &&
        !wifi_changed && !pomo_changed && !date_changed) return false;

    if (time_changed) {
        std::strncpy(last_time, time_key, sizeof(last_time) - 1);
        fill_rect(TIME_X0, TIME_Y0, TIME_X1, TIME_Y1, false);
        draw_time(local);
        invalidate_rect(TIME_X0, TIME_Y0, TIME_X1, TIME_Y1);
    }
    if (env_changed) {
        std::strncpy(last_env, env_key, sizeof(last_env) - 1);
        fill_rect(ENV_X1, ENV_Y0, ENV_X2, ENV_Y1, false);
        draw_env(temperature, humidity, sensor_ok);
        invalidate_rect(ENV_X1, ENV_Y0, ENV_X2, ENV_Y1);
    }
    if (bt_changed) {
        last_bt = pad_ready;
        fill_rect(BT_X1, BT_Y0, BT_X2, BT_Y1, false);
        draw_bt_icon(pad_ready != 0);
        invalidate_rect(BT_X1, BT_Y0, BT_X2, BT_Y1);
    }
    if (wifi_changed) {
        last_wifi = wifi_on;
        fill_rect(WIFI_X1, WIFI_Y0, WIFI_X2, WIFI_Y1, false);
        draw_wifi_icon(wifi_on != 0);
        invalidate_rect(WIFI_X1, WIFI_Y0, WIFI_X2, WIFI_Y1);
    }
    if (pct_changed) {
        last_pct = battery_percent;
        fill_rect(BATT_X1, BATT_Y0, BATT_X2, BATT_Y1, false);
        draw_battery(battery_percent);
        invalidate_rect(BATT_X1, BATT_Y0, BATT_X2, BATT_Y1);
    }
    if (pomo_changed) {
        last_pomo_key = pomo_key;
        redraw_card(1, s_selection == 1);
    }
    if (date_changed) {
        last_date_key = date_key;
        redraw_card(0, s_selection == 0);
    }
    return true;
}
