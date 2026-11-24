// 番茄钟工作页实现，逐点复刻 tests/pomodoro_preview.py 的版式。
// 写进一张 400x300 的 LVGL canvas（等价 1bit，只有黑白），
// 直接写帧缓冲 + 精确 invalidate_rect 局刷，避免电子纸整屏闪烁。
//
// 本文件只做绘制与局刷，不含计时逻辑。外部填好 PomodoroView 后调用
// pomodoro_redraw_all / pomodoro_refresh_* 即可。
#include "ui_pomodoro.h"

#include "app_display_config.h"
#include "engine_api.h"         // pad_state() 状态栏蓝牙图标
#include "local_sensor_state.h" // get_local_sensor_snapshot 温湿度
#include "wifi_portal_state.h"  // wifi_station_ip_snapshot WiFi 状态
#include "ui_canvas_primitives.h"
#include "ui_page_state.h"

#include "assets/fusion_pixel_12_font.h"
#include "assets/menu_icons.h"
#include "assets/pomodoro_icons.h"
#include "pomodoro/pomodoro_classic.h"

#include <esp_log.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

constexpr int W = kDisplayWidth;
constexpr int H = kDisplayHeight;
constexpr char TAG[] = "ui_pomodoro";

lv_obj_t *s_root = nullptr;
lv_obj_t *s_canvas = nullptr;
lv_color_t *s_buf = nullptr;
std::atomic<bool> s_shown{false};

// ---------- 字体查表（与主菜单 ui_main_menu.cpp 一致） ----------
const fusion_pixel_12_glyph_t *find_glyph(uint32_t cp)
{
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

// ---------- 画点原语（直接写 canvas 帧缓冲） ----------
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

// ---------- 文字 ----------
template <typename G>
void blit_glyph(const G *g, const uint8_t *bits, int x, int glyph_top,
                int scale, bool ink)
{
    if (!g || g->row_bytes == 0) return;
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

// 粗体（只横向加粗，对应预览 bold=True, thick_y=0）：每个置位点横向多画 1px。
template <typename G>
void blit_glyph_bold_x(const G *g, const uint8_t *bits, int x, int glyph_top,
                       int scale, bool ink)
{
    if (!g || g->row_bytes == 0) return;
    for (int r = 0; r < g->h; ++r) {
        for (int col = 0; col < g->w; ++col) {
            int byte = bits[g->off + r * g->row_bytes + col / 8];
            if (byte & (1 << (7 - (col % 8)))) {
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale + 1; ++sx)
                        put(x + (g->xoff + col) * scale + sx,
                            glyph_top + r * scale + sy, ink);
            }
        }
    }
}

int text12(int x, int top, const char *s, int scale = 1, int extra = 0,
           bool ink = true, bool bold_x = false)
{
    constexpr int ascent = 9;
    const char *p = s;
    int cx = x;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        const auto *g = find_glyph(cp);
        if (g) {
            int glyph_top = (top + ascent * scale) - (g->yoff + g->h) * scale;
            if (bold_x) blit_glyph_bold_x(g, fusion_pixel_12_bits, cx, glyph_top, scale, ink);
            else blit_glyph(g, fusion_pixel_12_bits, cx, glyph_top, scale, ink);
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
                   int extra = 0, bool ink = true, bool bold_x = false)
{
    text12(cx - text_width12(s, scale, extra) / 2, top, s, scale, extra, ink, bold_x);
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

// 按中心点贴番茄图标（size 边长）。outline/solid 两个索引。
void draw_tomato(int cx, int cy, int icon_index)
{
    const auto &ic = kPomodoroIcons[icon_index];
    int x0 = cx - ic.w / 2, y0 = cy - ic.h / 2;
    for (int row = 0; row < ic.h; ++row) {
        for (int b = 0; b < ic.stride; ++b) {
            uint8_t byte = ic.bits[row * ic.stride + b];
            for (int bit = 0; bit < 8; ++bit)
                if (byte & (1 << (7 - bit))) put(x0 + b * 8 + bit, y0 + row, true);
        }
    }
}

// ---------- 版式常量（与预览一致） ----------
constexpr int STATUS_H = 32;
constexpr int ICON_CX[5] = {170, 225, 300, 322, 365};
constexpr int BAR_Y = 282;
constexpr int DY = 12;  // 中间 UI 相对参考截图整体下移

// 顶部状态栏（与主菜单完全一致）。
constexpr int TIME_X0 = 2, TIME_X1 = 120, TIME_Y0 = 0, TIME_Y1 = STATUS_H - 2;
constexpr int BATT_X1 = ICON_CX[4] - 30, BATT_X2 = W - 2;
constexpr int BATT_Y0 = 8, BATT_Y1 = STATUS_H - 2;
constexpr int ENV_X1 = ICON_CX[0] - 24, ENV_X2 = ICON_CX[1] + 28;
constexpr int ENV_Y0 = 5, ENV_Y1 = STATUS_H - 2;
constexpr int WIFI_X1 = ICON_CX[2] - 12, WIFI_X2 = ICON_CX[2] + 12;
constexpr int WIFI_Y0 = 5, WIFI_Y1 = STATUS_H - 2;
constexpr int BT_X1 = ICON_CX[3] - 12, BT_X2 = ICON_CX[3] + 12;
constexpr int BT_Y0 = 5, BT_Y1 = STATUS_H - 2;

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

void draw_bt_icon(bool connected)
{
    draw_icon(connected ? "bluetooth_18" : "bluetooth_off_18", ICON_CX[3] - 9, 7);
}

void draw_wifi_icon(bool connected)
{
    draw_icon(connected ? "wifi_18" : "wifi_off_18", ICON_CX[2] - 9, 7);
}

bool wifi_connected()
{
    char ip[kWifiStationIpTextLen] = {};
    return wifi_station_ip_snapshot(ip, sizeof(ip));
}

void draw_status_bar(const struct tm *local, int battery_percent)
{
    float temperature = 0.0f, humidity = 0.0f;
    const bool sensor_ok = get_local_sensor_snapshot(&temperature, &humidity,
                                                     nullptr, nullptr);
    draw_time(local);
    draw_env(temperature, humidity, sensor_ok);
    draw_wifi_icon(wifi_connected());
    draw_bt_icon(pad_state() == PAD_STATE_READY);
    draw_battery(battery_percent);
    fill_rect(0, STATUS_H, W - 1, STATUS_H + 2);
}

// ---------- 静态元素（标题栏 / 切角框 / 底栏 / 横线） ----------
// 标题组居中常量：左侧对齐圆环中心 107，右侧对齐方形框中心 301。
constexpr int TITLE_CX_L = 107;
constexpr int TITLE_CX_R = 301;
constexpr int TITLE_TOP = 30 + DY;

void draw_title(int session)
{
    // 左侧 TOMATO(scale2) + 番茄钟(scale2)，gap 8，整组居中 107。
    constexpr int gap = 8;
    int w1 = text_width12("TOMATO", 2);
    int w2 = text_width12("番茄钟", 2);
    int total = w1 + gap + w2;
    int tx = TITLE_CX_L - total / 2;
    text12(tx, TITLE_TOP + 1, "TOMATO", 2);
    text12(tx + w1 + gap, TITLE_TOP + 1, "番茄钟", 2);

    // 右侧 SESSION(scale2) + 第 N 节(scale1)，gap 6，整组居中 301。
    // 节数随阶段切换递增，按实际文本宽度算，保证位数变化时仍居中。
    char session_text[16];
    std::snprintf(session_text, sizeof(session_text), "第 %d 节", session);
    constexpr int rgap = 6;
    int rw1 = text_width12("SESSION", 2);
    int rw2 = text_width12(session_text);
    int rtotal = rw1 + rgap + rw2;
    int rx = TITLE_CX_R - rtotal / 2;
    text12(rx, TITLE_TOP + 1, "SESSION", 2);
    text12(rx + rw1 + rgap, TITLE_TOP + 2, session_text);
}

// 切角矩形（八边形）边框，width 用双线近似预览的 2px。
void chamfer_frame(int x0, int y0, int x1, int y1, int cut)
{
    int p[8][2] = {
        {x0 + cut, y0}, {x1 - cut, y0}, {x1, y0 + cut}, {x1, y1 - cut},
        {x1 - cut, y1}, {x0 + cut, y1}, {x0, y1 - cut}, {x0, y0 + cut},
    };
    for (int i = 0; i < 8; ++i) {
        int j = (i + 1) % 8;
        // Bresenham 连线（双线：中心 + 向内 1px）。
        int xa = p[i][0], ya = p[i][1], xb = p[j][0], yb = p[j][1];
        int dx = std::abs(xb - xa), sx = xa < xb ? 1 : -1;
        int dy = -std::abs(yb - ya), sy = ya < yb ? 1 : -1;
        int err = dx + dy;
        while (true) {
            put(xa, ya, true);
            put(xa + 1, ya + 1, true);
            if (xa == xb && ya == yb) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; xa += sx; }
            if (e2 <= dx) { err += dx; ya += sy; }
        }
    }
}

void draw_bottom_bar()
{
    hline(0, W - 1, BAR_Y);
    draw_icon("dpad_12", 8, BAR_Y + 4);
    int x = text12(30, BAR_Y + 6, "MOVE", 1, -1);
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

// ---------- 动态元素：圆环 / 倒计时 / 番茄统计 / MODE / STATS / NEXT ----------
constexpr int RCX = 107, RCY = 158, RR = 86;
constexpr int N_RING = 30;
constexpr int CHAM_X0 = 47, CHAM_Y0 = 87 + DY, CHAM_X1 = 166, CHAM_Y1 = 205 + DY;

// 圆环中心到倒计时框中心：与预览一致（RCX=107, RCY=158, 框 x47..166/y99..217）。
void draw_ring(const PomodoroView *view)
{
    for (int i = 0; i < N_RING; ++i) {
        double a = -M_PI / 2 + 2.0 * M_PI * i / N_RING;
        int sx = RCX + static_cast<int>(std::lround(RR * std::cos(a)));
        int sy = RCY + static_cast<int>(std::lround(RR * std::sin(a)));
        constexpr int s = 6;
        int x0 = sx - s / 2, y0 = sy - s / 2;
        if (i < view->progress_done) fill_rect(x0, y0, x0 + s - 1, y0 + s - 1);
        else rect_frame(x0, y0, x0 + s - 1, y0 + s - 1);
    }
}

void draw_countdown(const PomodoroView *view)
{
    chamfer_frame(CHAM_X0, CHAM_Y0, CHAM_X1, CHAM_Y1, 12);
    // 18:42 大字，scale=4 横向加粗（bold_x），整组居中 RCX。
    text12_center(RCX, 116 + DY, view->remaining, 4, 0, true, true);
    text12_center(RCX, 169 + DY, view->phase_en, 1, 1);
    text12_center(RCX, 183 + DY, view->running ? view->phase_zh : "已暂停");
}

// 倒计时区局刷范围：圆环 + 切角框 + 文字（预览圆环 x13..200，框 y99..217）。
constexpr int CD_X1 = 10, CD_Y1 = 68, CD_X2 = 202, CD_Y2 = 222;

constexpr int TODAY_X1 = 214, TODAY_Y1 = 60, TODAY_X2 = 390, TODAY_Y2 = 132;

void draw_today(const PomodoroView *view)
{
    draw_tomato(245, 84 + DY, kPomodoroIcon_tomato_outline_59);
    text12(294, 58 + DY, "TODAY");
    text12(294 + text_width12("TODAY") + 6, 58 + DY, "今日");
    text12(294, 74 + DY, view->today);
    for (int i = 0; i < 4; ++i) {
        int cx = 290 + i * 29;
        int icon = (i < view->today_count) ? kPomodoroIcon_tomato_solid_20
                                           : kPomodoroIcon_tomato_outline_20;
        draw_tomato(cx, 103 + DY, icon);
    }
}

constexpr int MX0 = 215, MY0 = 122 + DY, MX1 = 388, MY1 = 198 + DY;
constexpr int MODE_X1 = 213, MODE_Y1 = MY0 - 2, MODE_X2 = 390, MODE_Y2 = MY1 + 2;

void draw_mode(const PomodoroView *view)
{
    rect_frame(MX0, MY0, MX1, MY1);
    text12(MX0 + 7, MY0 + 5, "MODE");
    text12(MX0 + 7 + text_width12("MODE") + 6, MY0 + 5, "模式");

    const char *labels[3] = {"FOCUS 专注", "SHORT BREAK 短休", "LONG BREAK 长休"};
    const char *times[3] = {"25:00", "05:00", "15:00"};
    int ry0 = MY0 + 20;
    for (int i = 0; i < 3; ++i) {
        int y = ry0 + i * 18;
        // 圆点（半径约 3.7px），选中实心，未选中空心。
        int dotcx = MX0 + 13, dotcy = y + 5;
        for (int yy = -4; yy <= 4; ++yy) {
            for (int xx = -4; xx <= 4; ++xx) {
                int r2 = xx * xx + yy * yy;
                if (r2 <= 14 && (i == view->selected_mode || r2 > 6))
                    put(dotcx + xx, dotcy + yy, true);
            }
        }
        text12(MX0 + 23, y, labels[i], 1, -1);
        text12(MX1 - 5 - text_width12(times[i]), y, times[i]);
    }
}

constexpr int SX0 = 215, SY0 = 204 + DY, SX1 = 388, SY1 = 243 + DY;
constexpr int STATS_X1 = 213, STATS_Y1 = SY0 - 2, STATS_X2 = 390, STATS_Y2 = SY1 + 2;

void draw_stats(const PomodoroView *view)
{
    rect_frame(SX0, SY0, SX1, SY1);
    text12(SX0 + 7, SY0 + 5, "STATS");
    text12(SX0 + 7 + text_width12("STATS") + 6, SY0 + 5, "统计");
    text12(SX0 + 7, SY0 + 23, view->stats);
}

constexpr int NEXT_X1 = 0, NEXT_Y1 = 262, NEXT_X2 = W - 1, NEXT_Y2 = 280;

void draw_next(const PomodoroView *view)
{
    hline(0, W - 1, 249 + DY);
    text12(10, 255 + DY, "NEXT");
    text12(10 + text_width12("NEXT") + text_width12(" "), 255 + DY, view->next);
}

void clear_rect(int x1, int y1, int x2, int y2)
{
    fill_rect(x1, y1, x2, y2, false);
}

} // namespace

// ---------------- 公共接口 ----------------

void pomodoro_view_defaults(PomodoroView *view)
{
    if (!view) return;
    std::strncpy(view->remaining, "18:42", sizeof(view->remaining) - 1);
    std::strncpy(view->today, "完成 3 个番茄", sizeof(view->today) - 1);
    std::strncpy(view->stats, "累计 1h 15m · 中断 0 次", sizeof(view->stats) - 1);
    std::strncpy(view->next, "下一次  短休 05:00 后自动开始", sizeof(view->next) - 1);
    view->progress_done = 10;
    view->selected_mode = 0;
    view->session = 3;
    view->today_count = 3;
    view->running = true;
    view->phase_en = "FOCUS";
    view->phase_zh = "专注中";
}

void build_pomodoro_ui()
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
    set_auxiliary_page_root(AuxiliaryPage::kPomodoro, s_root);
}

void pomodoro_view_from_snapshot(PomodoroView *view, const ClassicSnapshot *snap)
{
    if (!view || !snap) return;
    int mm = static_cast<int>(snap->remaining_sec / 60);
    int ss = static_cast<int>(snap->remaining_sec % 60);
    std::snprintf(view->remaining, sizeof(view->remaining), "%02d:%02d", mm, ss);

    std::snprintf(view->today, sizeof(view->today), "完成 %d 个番茄",
                  snap->today_count);

    uint32_t mins = snap->total_focus_sec / 60;
    uint32_t hours = mins / 60;
    mins %= 60;
    std::snprintf(view->stats, sizeof(view->stats), "累计 %uh %um · 中断 %u 次",
                  static_cast<unsigned>(hours), static_cast<unsigned>(mins),
                  static_cast<unsigned>(snap->interrupt_count));

    // 下一阶段预告：专注完成看 focus_round 决定短休/长休，休息完成回专注。
    const char *next_name;
    uint32_t next_sec;
    if (snap->phase == ClassicPhase::kFocus) {
        bool long_break = snap->focus_round + 1 >= 4;
        next_name = long_break ? "长休" : "短休";
        next_sec = classic_phase_duration(long_break ? ClassicPhase::kLongBreak
                                                     : ClassicPhase::kShortBreak);
    } else {
        next_name = "专注";
        next_sec = classic_phase_duration(ClassicPhase::kFocus);
    }
    std::snprintf(view->next, sizeof(view->next), "下一次  %s %02u:%02u 后自动开始",
                  next_name, static_cast<unsigned>(next_sec / 60),
                  static_cast<unsigned>(next_sec % 60));

    // 圆环进度：已走过的时间占比，30 格。
    uint32_t elapsed = snap->phase_duration_sec > snap->remaining_sec
                           ? snap->phase_duration_sec - snap->remaining_sec
                           : 0;
    view->progress_done = static_cast<int>(
        (static_cast<uint32_t>(elapsed) * 30u + snap->phase_duration_sec / 2u) /
        snap->phase_duration_sec);
    if (view->progress_done > 30) view->progress_done = 30;
    if (view->progress_done < 0) view->progress_done = 0;

    view->today_count = snap->today_count > 4 ? 4 : snap->today_count;
    view->selected_mode = snap->selected_mode;
    view->session = snap->session > 0 ? snap->session : 1;
    view->running = snap->run_state == ClassicRunState::kRunning;
    view->phase_en = snap->phase_en;
    view->phase_zh = snap->phase_zh;
}

lv_obj_t *pomodoro_root()
{
    return auxiliary_page_root(AuxiliaryPage::kPomodoro);
}

bool pomodoro_shown()
{
    return s_shown.load(std::memory_order_acquire);
}

void pomodoro_show()
{
    build_pomodoro_ui();
    s_shown.store(true, std::memory_order_release);
}

void pomodoro_hide()
{
    s_shown.store(false, std::memory_order_release);
}

void pomodoro_redraw_all(const PomodoroView *view, const struct tm *local,
                         int battery_percent)
{
    if (!s_canvas || !view) return;
    fill_rect(0, 0, W - 1, H - 1, false);
    draw_status_bar(local, battery_percent);
    draw_title(view->session);
    draw_ring(view);
    draw_countdown(view);
    draw_today(view);
    draw_mode(view);
    draw_stats(view);
    draw_next(view);
    draw_bottom_bar();
    invalidate_rect(0, 0, W - 1, H - 1);
}

void pomodoro_refresh_status_bar(const struct tm *local, int battery_percent)
{
    if (!s_canvas) return;
    constexpr int SB_X1 = 0, SB_Y1 = 0, SB_X2 = W - 1, SB_Y2 = STATUS_H + 2;
    clear_rect(SB_X1, SB_Y1, SB_X2, SB_Y2);
    draw_status_bar(local, battery_percent);
    invalidate_rect(SB_X1, SB_Y1, SB_X2, SB_Y2);
}

void pomodoro_refresh_title(const PomodoroView *view)
{
    if (!s_canvas || !view) return;
    // 整条标题带局刷（高度约 24px，与状态栏同理）。数字位数变化时
    // SESSION 整组会左右移动，全宽刷避免裁切；左右标题一起重画即可。
    constexpr int TX1 = 0, TY1 = TITLE_TOP - 2, TX2 = W - 1, TY2 = TITLE_TOP + 22;
    clear_rect(TX1, TY1, TX2, TY2);
    draw_title(view->session);
    invalidate_rect(TX1, TY1, TX2, TY2);
}

void pomodoro_refresh_countdown(const PomodoroView *view, int battery_percent)
{
    if (!s_canvas || !view) return;
    // 圆环 + 切角框 + 倒计时文字整块局刷（宽约 192px，低于局刷阈值）。
    clear_rect(CD_X1, CD_Y1, CD_X2, CD_Y2);
    draw_ring(view);
    draw_countdown(view);
    invalidate_rect(CD_X1, CD_Y1, CD_X2, CD_Y2);

    if (battery_percent >= 0) {
        clear_rect(BATT_X1, BATT_Y0, BATT_X2, BATT_Y1);
        draw_battery(battery_percent);
        invalidate_rect(BATT_X1, BATT_Y0, BATT_X2, BATT_Y1);
    }
}

void pomodoro_refresh_today(const PomodoroView *view)
{
    if (!s_canvas || !view) return;
    clear_rect(TODAY_X1, TODAY_Y1, TODAY_X2, TODAY_Y2);
    draw_today(view);
    invalidate_rect(TODAY_X1, TODAY_Y1, TODAY_X2, TODAY_Y2);
}

void pomodoro_refresh_mode(const PomodoroView *view)
{
    if (!s_canvas || !view) return;
    clear_rect(MODE_X1, MODE_Y1, MODE_X2, MODE_Y2);
    draw_mode(view);
    invalidate_rect(MODE_X1, MODE_Y1, MODE_X2, MODE_Y2);
}

void pomodoro_refresh_stats(const PomodoroView *view)
{
    if (!s_canvas || !view) return;
    clear_rect(STATS_X1, STATS_Y1, STATS_X2, STATS_Y2);
    draw_stats(view);
    invalidate_rect(STATS_X1, STATS_Y1, STATS_X2, STATS_Y2);
}

void pomodoro_refresh_next(const PomodoroView *view)
{
    if (!s_canvas || !view) return;
    clear_rect(NEXT_X1, NEXT_Y1, NEXT_X2, NEXT_Y2);
    draw_next(view);
    invalidate_rect(NEXT_X1, NEXT_Y1, NEXT_X2, NEXT_Y2);
}
