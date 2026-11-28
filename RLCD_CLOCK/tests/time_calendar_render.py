#!/usr/bin/env python3
"""用固件那套 Fusion Pixel 12 字库，按 ui_time_calendar.cpp 的绘制逻辑
逐像素复刻「时间与日历」页，输出 400x300 单色图。

和 time_calendar_preview.py 的区别：那个用系统中文字体看版式，
这个连字形都和真机一致，用来判断 12px 字是不是太小、有没有缺字。

用法：
    python3 time_calendar_render.py
    python3 time_calendar_render.py --date 2026-11-28 --time 11:47:23
    python3 time_calendar_render.py --raw-time 99:99:99  # 测大字左右边距
    python3 time_calendar_render.py --page 2             # 天气时钟第二页
"""

from __future__ import annotations

import argparse
import datetime as dt
import pathlib
import struct
import sys

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from fusion_pixel_font import FONT_ASCENT, GLYPHS  # noqa: E402

# 固件里天气页的中文走 /sdcard/font/fusion_pixel_12.bin，
# 这里直接读仓库里的同一份字库，预览才不会漏「都」「川」这类字。
_SD_FONT = next(
    (p for p in (
        pathlib.Path(__file__).resolve().parents[1] / "sd-assets/font/fusion_pixel_12.bin",
        pathlib.Path(__file__).resolve().parents[2] / "sd-assets/font/fusion_pixel_12.bin",
    ) if p.exists()),
    None,
)
SD_GLYPHS = {}


def load_sd_font():
    if _SD_FONT is None:
        return
    data = _SD_FONT.read_bytes()
    if data[:4] != b"FPX1":
        return
    count, _px, _line_h, _ascent, _blob = struct.unpack_from("<HHHHI", data, 4)
    base = 16 + count * 11
    for i in range(count):
        cp, off, w, h, xoff, ytop, dw = struct.unpack_from("<HIBBBBB", data, 16 + i * 11)
        if ytop >= 128:
            ytop -= 256  # 生成器按 &0xFF 存，C++ 侧按 int8_t 读
        row_bytes = (w + 7) // 8
        blob = data[base + off: base + off + row_bytes * h]
        rows = [int.from_bytes(blob[r * row_bytes:(r + 1) * row_bytes], "big")
                for r in range(h)]
        SD_GLYPHS[cp] = (w, h, xoff, ytop, dw, row_bytes, rows)


load_sd_font()


def glyph_for(ch):
    cp = ord(ch)
    if 0x4E00 <= cp <= 0x9FFF and cp in SD_GLYPHS:
        return SD_GLYPHS[cp], True
    return GLYPHS.get(cp), False

W, H = 400, 300
INK = 0
PAPER = 255

STATUS_H = 32
TITLE_TOP = 40
TITLE_RULE_Y = 56
TITLE_RULE_H = 1  # 与书库页 hline(y=56) 同为 1px
TIME_ROW_Y = 62
TIME_ROW_H = 74
TIME_LEFT = 8
CARD_X0, CARD_X1 = 256, 392
LOWER_Y = 140
DIAL_CX, DIAL_CY, DIAL_R = 68, 210, 58
CAL_ROWS = 6
CAL_COLW = 37
CAL_ROW_H = 20
CAL_HEADER_H = 13
CAL_X0 = 132
CAL_Y0 = LOWER_Y
WEEKEND_FIRST_COL = 5
BAR_Y = 282

DIGIT_SCALE = 5
DIGIT_W = 5 * DIGIT_SCALE
DIGIT_H = 7 * DIGIT_SCALE
DIGIT_ADV = 34
COLON_ADV = 18


import re as _re
_dseg_header = pathlib.Path(__file__).resolve().parents[1] / "main/assets/dseg_digits.h"
_dseg_src = _dseg_header.read_text()
_dseg_bm = [int(x,16) for x in _re.findall(r'0x[0-9A-Fa-f]+', _re.search(r'kDSEG36Bitmaps\[\] = \{(.*?)\};', _dseg_src, _re.S).group(1))]
_dseg_gl = [tuple(int(v) for v in t) for t in _re.findall(r'\{(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\}', _re.search(r'kDSEG36Glyphs\[\] = \{(.*?)\};', _dseg_src, _re.S).group(1))]
_DSEG_CHARS="0123456789:"
_DSN,_DSD=6,5
# DSEG36 字库无负号，负温用数字中部横线表示（6/5 放大后坐标，相对 baseline=0）
_MINUS_X,_MINUS_W,_MINUS_Y,_MINUS_H,_MINUS_ADV = 6,20,-24,5,32

# C++ 整数除法是向 0 截断，Python 的 // 是向下取整；负 y_offset 会差 1px。
def cdiv(a, b):
    return abs(a) // b * (1 if a >= 0 else -1)

def dseg_text_ink_bounds(txt):
    min_x = min_y = None
    max_x = max_y = None
    cx = 0
    for ch in txt:
        if ch == '-':
            for tx in range(_MINUS_W):
                for ty in range(_MINUS_H):
                    px, py = cx + _MINUS_X + tx, _MINUS_Y + ty
                    min_x = px if min_x is None else min(min_x, px)
                    max_x = px if max_x is None else max(max_x, px)
                    min_y = py if min_y is None else min(min_y, py)
                    max_y = py if max_y is None else max(max_y, py)
            cx += _MINUS_ADV
            continue
        if ch not in _DSEG_CHARS:
            continue
        off, w, h, adv, xoff, yoff = _dseg_gl[_DSEG_CHARS.index(ch)]
        gx = cx + cdiv(xoff * _DSN, _DSD)
        gy = cdiv(yoff * _DSN, _DSD)
        gw = w * _DSN // _DSD
        gh = h * _DSN // _DSD
        for ty in range(gh):
            sy = ty * _DSD // _DSN
            for tx in range(gw):
                sx = tx * _DSD // _DSN
                bit = sy * w + sx
                if not (_dseg_bm[off + bit // 8] & (0x80 >> (bit % 8))):
                    continue
                px, py = gx + tx, gy + ty
                min_x = px if min_x is None else min(min_x, px)
                max_x = px if max_x is None else max(max_x, px)
                min_y = py if min_y is None else min(min_y, py)
                max_y = py if max_y is None else max(max_y, py)
        cx += adv * _DSN // _DSD
    return (min_x, min_y, max_x, max_y)

def draw_dseg_text(txt,x,baseline,ink=True):
    cx=x
    for ch in txt:
        if ch == '-':
            fill_rect(cx+_MINUS_X, baseline+_MINUS_Y,
                      cx+_MINUS_X+_MINUS_W-1, baseline+_MINUS_Y+_MINUS_H-1, ink)
            cx += _MINUS_ADV
            continue
        off,w,h,adv,xoff,yoff=_dseg_gl[_DSEG_CHARS.index(ch)]
        gw=w*_DSN//_DSD; gh=h*_DSN//_DSD
        gx=cx+cdiv(xoff*_DSN,_DSD); gy=baseline+cdiv(yoff*_DSN,_DSD)
        for ty in range(gh):
            sy=ty*_DSD//_DSN
            for tx in range(gw):
                sx=tx*_DSD//_DSN
                bit=sy*w+sx
                if _dseg_bm[off+bit//8] & (0x80>>(bit%8)):
                    put(gx+tx,gy+ty,ink)
        cx+=adv*_DSN//_DSD

BIG = {
    "0": [0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110],
    "1": [0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110],
    "2": [0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111],
    "3": [0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110],
    "4": [0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010],
    "5": [0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110],
    "6": [0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110],
    "7": [0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000],
    "8": [0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110],
    "9": [0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100],
    ":": [0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000],
}

MONTH_EN = ["JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
            "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"]
WEEK_EN = ["MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"]
WEEK_ZH = ["星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"]
WEEK_SHORT = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
LUNAR_MONTH = ["", "正月", "二月", "三月", "四月", "五月", "六月",
               "七月", "八月", "九月", "十月", "冬月", "腊月"]
LUNAR_DAY = ["", "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
             "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
             "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"]

img = Image.new("1", (W, H), PAPER)
px = img.load()


def put(x, y, ink=True):
    if 0 <= x < W and 0 <= y < H:
        px[x, y] = INK if ink else PAPER


def fill_rect(x0, y0, x1, y1, ink=True):
    if x0 > x1: x0, x1 = x1, x0
    if y0 > y1: y0, y1 = y1, y0
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            put(x, y, ink)


def rect_frame(x0, y0, x1, y1, ink=True):
    for x in range(x0, x1 + 1):
        put(x, y0, ink); put(x, y1, ink)
    for y in range(y0, y1 + 1):
        put(x0, y, ink); put(x1, y, ink)


def hline(x0, x1, y, ink=True):
    for x in range(x0, x1 + 1):
        put(x, y, ink)


def vline(x, y0, y1, ink=True):
    for y in range(y0, y1 + 1):
        put(x, y, ink)


def rounded_fill(x0, y0, x1, y1, r, ink=True):
    fill_rect(x0 + r, y0, x1 - r, y1, ink)
    fill_rect(x0, y0 + r, x1, y1 - r, ink)
    for dy in range(r):
        span = r - 1 - dy
        hline(x0 + r - span, x1 - r + span, y0 + dy, ink)
        hline(x0 + r - span, x1 - r + span, y1 - dy, ink)


def thick_line(x0, y0, x1, y1, width, ink=True):
    import math
    dx, sx = abs(x1 - x0), (1 if x0 < x1 else -1)
    dy, sy = -abs(y1 - y0), (1 if y0 < y1 else -1)
    err = dx + dy
    while True:
        half = width // 2
        fill_rect(x0 - half, y0 - half, x0 - half + width - 1, y0 - half + width - 1, ink)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy; x0 += sx
        if e2 <= dx:
            err += dx; y0 += sy


def text12(x, top, s, scale=1, extra=0, ink=True, bold=False):
    cx = x
    for ch in s:
        g, sd = glyph_for(ch)
        if not g:
            cx += 6 * scale + extra
            continue
        w, h, xoff, yoff, dw, row_bytes, rows = g
        # SD 字库的 ytop 相对行顶，内置字库的 yoff 相对基线。
        glyph_top = top + yoff * scale if sd else (top + FONT_ASCENT * scale) - (yoff + h) * scale
        for r in range(h):
            val = rows[r]
            for col in range(w):
                if val & (1 << (row_bytes * 8 - 1 - col)):
                    px = cx + (xoff + col) * scale
                    fill_rect(px, glyph_top + r * scale,
                              px + (1 if bold else 0),
                              glyph_top + r * scale + scale - 1, ink)
        cx += dw * scale + extra
    return cx


def text_width12(s, scale=1, extra=0):
    w = 0
    for ch in s:
        g, _sd = glyph_for(ch)
        w += (g[4] if g else 6) * scale + extra
    return w


def text12_center(cx, top, s, scale=1, extra=0, ink=True, bold=False):
    return text12(cx - text_width12(s, scale, extra) // 2, top, s,
                  scale, extra, ink, bold)


def text12_ink_bounds(s, scale=1, extra=0, bold=False):
    min_x = min_y = None
    max_x = max_y = None
    cx = 0
    for ch in s:
        g, sd = glyph_for(ch)
        if not g:
            cx += 6 * scale + extra
            continue
        w, h, xoff, yoff, dw, row_bytes, rows = g
        glyph_top = yoff * scale if sd else FONT_ASCENT * scale - (yoff + h) * scale
        for r in range(h):
            val = rows[r]
            for col in range(w):
                if not (val & (1 << (row_bytes * 8 - 1 - col))):
                    continue
                px = cx + (xoff + col) * scale
                py = glyph_top + r * scale
                min_x = px if min_x is None else min(min_x, px)
                max_x = px if max_x is None else max(max_x, px)
                min_y = py if min_y is None else min(min_y, py)
                max_y = py if max_y is None else max(max_y, py)
                if bold:
                    max_x = max(max_x, px + 1)
        cx += dw * scale + extra
    return (min_x, min_y, max_x, max_y)


def text12_center_in_box(x0, y0, x1, y1, s, scale=1, extra=0,
                         ink=True, bold=False):
    bx0, by0, bx1, by1 = text12_ink_bounds(s, scale, extra, bold)
    if bx0 is None:
        return
    draw_x = x0 + (x1 - x0 + 1 - (bx1 - bx0 + 1)) // 2 - bx0
    draw_y = y0 + (y1 - y0 + 1 - (by1 - by0 + 1)) // 2 - by0
    text12(draw_x, draw_y, s, scale, extra, ink, bold)


def draw_big(x, y, s, ink=True):
    cx = x
    for ch in s:
        rows = BIG.get(ch)
        if rows is None:
            cx += 8
            continue
        adv = COLON_ADV if ch == ":" else DIGIT_ADV
        for r, bits in enumerate(rows):
            for c in range(5):
                if bits & (1 << (4 - c)):
                    fill_rect(cx + c * DIGIT_SCALE, y + r * DIGIT_SCALE,
                              cx + c * DIGIT_SCALE + DIGIT_SCALE - 1,
                              y + r * DIGIT_SCALE + DIGIT_SCALE - 1, ink)
        cx += adv


def big_width(s):
    return sum(COLON_ADV if ch == ":" else DIGIT_ADV for ch in s)


def circle_ring(cx, cy, r, thickness, ink=True):
    import math
    for y in range(-r, r + 1):
        inner = r - thickness
        x_out = int(math.sqrt(r * r - y * y) + 0.5)
        if inner > 0 and abs(y) <= inner:
            x_in = int(math.sqrt(inner * inner - y * y) + 0.5)
            hline(cx + x_in, cx + x_out, cy + y, ink)
            hline(cx - x_out, cx - x_in, cy + y, ink)
        else:
            hline(cx - x_out, cx + x_out, cy + y, ink)


def fill_circle(cx, cy, r, ink=True):
    import math
    for y in range(-r, r + 1):
        x = int(math.sqrt(r * r - y * y) + 0.5)
        hline(cx - x, cx + x, cy + y, ink)


ICON_CX = [170, 225, 300, 322, 365]


def draw_status_bar(now):
    cur = text12(6, 6, now.strftime("%H:%M"), 2)
    text12(cur + 8, 10, now.strftime("%Y-%m-%d"), 1, -1)
    text12(150, 11, "21°C")
    text12(205, 11, "48%")
    text12(291, 11, "WiFi")
    text12(313, 11, "BT")

    pct = 75
    pctbuf = f"{pct}%"
    group_w = 28 + 3 + text_width12(pctbuf)
    bx = ICON_CX[4] - group_w // 2
    rect_frame(bx, 10, bx + 24, 22)
    fill_rect(bx + 25, 14, bx + 27, 18)
    filled = 4 if pct >= 75 else 3 if pct >= 50 else 2 if pct >= 25 else 1 if pct > 0 else 0
    for k in range(4):
        qx = bx + 2 + k * 5
        if k < filled:
            fill_rect(qx, 12, qx + 3, 20)
        else:
            rect_frame(qx, 12, qx + 3, 20)
    text12(bx + 31, 12, pctbuf)
    fill_rect(0, STATUS_H, W - 1, STATUS_H + 2)


def draw_title():
    x = text12(10, TITLE_TOP, "TIME & CALENDAR")
    text12(x + 5, TITLE_TOP, "时间与日历")
    fill_rect(0, TITLE_RULE_Y, W - 1, TITLE_RULE_Y + TITLE_RULE_H - 1)


def draw_time_and_date(now, lunar_text, raw_time):
    bx0, _, bx1, _ = dseg_text_ink_bounds(raw_time)
    x = ((CARD_X0 - 1) + 1 - (bx1 - bx0 + 1)) // 2 - bx0
    baseline = TIME_ROW_Y + TIME_ROW_H - 15
    draw_dseg_text(raw_time, x, baseline, True)

    fill_rect(CARD_X0, TIME_ROW_Y, CARD_X1, TIME_ROW_Y + TIME_ROW_H - 1)
    cx = (CARD_X0 + CARD_X1) // 2
    line = f"{MONTH_EN[now.month - 1]} {now.day}, {now.year}"
    text12_center(cx, TIME_ROW_Y + 7, line, 1, 0, False)
    line = f"{WEEK_ZH[(now.weekday() + 1) % 7]} ({WEEK_SHORT[(now.weekday() + 1) % 7]})"
    text12_center(cx, TIME_ROW_Y + 30, line, 1, 0, False)
    text12_center(cx, TIME_ROW_Y + 53, lunar_text, 1, 0, False)


def draw_dial(now):
    import math
    cx, cy, r = DIAL_CX, DIAL_CY, DIAL_R
    circle_ring(cx, cy, r, 3)
    for m in range(60):
        a = math.radians(m * 6)
        s, c = math.sin(a), math.cos(a)
        if m % 5 == 0:
            thick_line(cx + int((r - 9) * s + 0.5), cy - int((r - 9) * c + 0.5),
                       cx + int((r - 4) * s + 0.5), cy - int((r - 4) * c + 0.5), 2)
        else:
            put(cx + int((r - 5) * s + 0.5), cy - int((r - 5) * c + 0.5))
    for h in range(1, 13):
        a = math.radians(h * 30)
        x = cx + int((r - 19) * math.sin(a) + 0.5)
        y = cy - int((r - 19) * math.cos(a) + 0.5)
        text12_center(x, y - 5, str(h))

    hour, minute, second = now.hour % 12, now.minute, now.second

    def hand(deg, length, width):
        a = math.radians(deg)
        thick_line(cx, cy, cx + int(length * math.sin(a) + 0.5),
                   cy - int(length * math.cos(a) + 0.5), width)

    hand((hour + minute / 60) * 30, r - 26, 5)
    hand((minute + second / 60) * 6, r - 15, 3)
    hand(second * 6, r - 8, 1)
    fill_circle(cx, cy, 3)


def draw_month(now):
    import calendar
    y, m, today = now.year, now.month, now.day
    grid_h = CAL_HEADER_H + CAL_ROWS * CAL_ROW_H
    x1 = CAL_X0 + CAL_COLW * 7 - 1
    y1 = CAL_Y0 + grid_h - 1

    for i in range(WEEKEND_FIRST_COL, 7):
        x = CAL_X0 + i * CAL_COLW
        fill_rect(x, CAL_Y0, x + CAL_COLW - 1, CAL_Y0 + CAL_HEADER_H - 1)

    rect_frame(CAL_X0, CAL_Y0, x1, y1)
    for i in range(1, 7):
        vline(CAL_X0 + i * CAL_COLW, CAL_Y0, y1)
    hline(CAL_X0, x1, CAL_Y0 + CAL_HEADER_H)
    for row in range(1, CAL_ROWS):
        hline(CAL_X0, x1, CAL_Y0 + CAL_HEADER_H + row * CAL_ROW_H)

    for i in range(7):
        x = CAL_X0 + i * CAL_COLW
        text12_center_in_box(x, CAL_Y0, x + CAL_COLW - 1,
                             CAL_Y0 + CAL_HEADER_H - 1, WEEK_EN[i], 1, 0,
                             i < WEEKEND_FIRST_COL, True)

    first_wd = (calendar.weekday(y, m, 1) + 0) % 7   # Monday = 0
    days = calendar.monthrange(y, m)[1]
    for day in range(1, days + 1):
        idx = first_wd + day - 1
        row, col = idx // 7, idx % 7
        if row >= CAL_ROWS:
            break
        x = CAL_X0 + col * CAL_COLW
        yy = CAL_Y0 + CAL_HEADER_H + row * CAL_ROW_H
        if day == today:
            fill_rect(x + 1, yy + 1, x + CAL_COLW - 2, yy + CAL_ROW_H - 2)
            text12_center_in_box(x + 1, yy + 1, x + CAL_COLW - 2,
                                 yy + CAL_ROW_H - 2, str(day), 1, 0, False)
        else:
            text12_center_in_box(x, yy, x + CAL_COLW - 1,
                                 yy + CAL_ROW_H - 1, str(day))


def draw_action_bar(page=0):
    hline(0, W - 1, BAR_Y)
    top = BAR_Y + 4
    text12(8, top, "< >")
    x = text12(38, top - 1, "SWITCH")
    x = text12(x + 4, top + 2, "切换")
    vline(x + 8, BAR_Y + 3, H - 3)
    cap_x = x + 16
    rounded_fill(cap_x, BAR_Y + 4, cap_x + 16, BAR_Y + 15, 2)
    text12(cap_x + 3, BAR_Y + 6, "A", 1, 0, False)
    x = text12(cap_x + 20, top + 2, "详情")
    vline(x + 8, BAR_Y + 3, H - 3)
    cap_x = x + 16
    rounded_fill(cap_x, BAR_Y + 4, cap_x + 16, BAR_Y + 15, 2)
    text12(cap_x + 3, BAR_Y + 6, "B", 1, 0, False)
    x = text12(cap_x + 20, top - 1, "BACK")
    text12(x + 4, top + 2, "返回")
    for k in range(5):
        bx = W - 8 - (5 - k) * 9
        if k == page:
            fill_rect(bx, BAR_Y + 5, bx + 6, BAR_Y + 13)
        else:
            rect_frame(bx, BAR_Y + 5, bx + 6, BAR_Y + 13)


# ---------- 天气时钟第二页（静态 UI） ----------
def text12_right(x1, top, s, scale=1, extra=0, ink=True, bold=False):
    text12(x1 - text_width12(s, scale, extra), top, s, scale, extra, ink, bold)


def dotted_hline(x0, x1, y):
    for x in range(x0, x1 + 1, 2):
        put(x, y)


def dotted_vline(x, y0, y1):
    for y in range(y0, y1 + 1, 2):
        put(x, y)


def draw_small_cloud(x, y):
    # 实心云：三个实心圆叠出云朵，底部填平
    fill_circle(x + 3, y + 5, 2)
    fill_circle(x + 6, y + 4, 3)
    fill_circle(x + 10, y + 5, 2)
    fill_rect(x + 2, y + 5, x + 12, y + 7)


def draw_small_weather_icon(kind, x, y):
    if kind == 0:  # sun 实心圆 + 光芒
        fill_circle(x + 6, y + 6, 4)
        hline(x + 0, x + 1, y + 6)
        hline(x + 11, x + 12, y + 6)
        vline(x + 6, y + 0, y + 1)
        vline(x + 6, y + 11, y + 12)
        for px, py in ((2, 2), (10, 2), (2, 10), (10, 10)):
            put(x + px, y + py)
    elif kind == 1:  # partly 实心小太阳 + 实心云
        fill_circle(x + 3, y + 4, 2)
        draw_small_cloud(x + 3, y + 4)
    elif kind == 2:  # rain 实心云 + 三个实心圆点雨滴
        draw_small_cloud(x + 0, y + 0)
        fill_circle(x + 4, y + 10, 1)
        fill_circle(x + 8, y + 10, 1)
        fill_circle(x + 12, y + 10, 1)
    elif kind == 3:  # cloud
        draw_small_cloud(x + 0, y + 0)
    elif kind == 4:  # moon 实心圆挖空成月牙
        fill_circle(x + 7, y + 6, 5)
        fill_circle(x + 10, y + 4, 5, False)
    elif kind == 5:  # moon + cloud
        fill_circle(x + 4, y + 4, 4)
        fill_circle(x + 6, y + 3, 4, False)
        draw_small_cloud(x + 5, y + 4)


def draw_uv_bars(x0, y0, count):
    for i in range(10):
        bx = x0 + i * 13
        rect_frame(bx, y0, bx + 11, y0 + 6)
        if i < count:
            fill_rect(bx + 1, y0 + 1, bx + 10, y0 + 5)


# 与 C++ 侧 refresh_weather_page_data() 读到的成都真实天气同构。
# --negative 可以把所有温度换成负值，回归检查溢出与对齐。
WEATHER_SAMPLE = {
    "city": "成都", "city_en": "CHENGDU",
    "temp": "24", "apparent": "25°",
    "condition_en": "CLOUDY", "condition_cn": "多云",
    "high": "27°", "low": "18°",
    "wind": "NW 西北 3 级", "uv": 5,
    "updated": "17:40",
    "hours": [
        ("18", "25°", 1), ("22", "21°", 5), ("02", "18°", 4),
        ("06", "17°", 4), ("10", "22°", 1), ("14", "26°", 0),
    ],
    "days": [
        ("周六", "11/28", 1, 24, "18/27°"),
        ("周日", "11/29", 2, 20, "17/25°"),
        ("周一", "11/30", 0, 26, "19/28°"),
        ("周二", "12/01", 3, 18, "16/24°"),
        ("周三", "12/02", 1, 22, "18/26°"),
    ],
}


def apply_negative(sample):
    s = dict(sample)
    s["temp"] = "-15"
    s["apparent"] = "-18°"
    s["high"] = "-5°"
    s["low"] = "-21°"
    s["wind"] = "NW 西北 5 级"
    s["uv"] = 10
    s["hours"] = list(sample["hours"])
    s["hours"][0] = ("18", "-15°", 0)
    s["hours"][1] = ("22", "-20°", 1)
    s["hours"][4] = ("10", "-8°", 4)
    s["days"] = list(sample["days"])
    s["days"][2] = ("周一", "11/30", 2, 20, "-15/-20°")
    return s


def draw_24h_panel(data):
    x0, y0, x1, y1 = 10, 150, 198, 260
    rect_frame(x0, y0, x1, y1)
    text12(x0 + 5, y0 + 6, "24H 逐时预报", 1, 0, True, True)
    dotted_hline(x0 + 1, x1 - 1, y0 + 24)
    inner_x0 = x0 + 1
    col_w = 31
    for i in range(1, 6):
        dotted_vline(inner_x0 + i * col_w, y0 + 25, y1 - 1)
    # 图标+时间+温度三行，在内容区 y0+25..y1-1（高 85px）里三等分，
    # 每个内容墨迹中心对齐格子中心。时间与温度不加粗。
    content_y0 = y0 + 25
    section_h = (y1 - 1 - content_y0 + 1) // 3
    sec1_cy = content_y0 + section_h // 2
    sec2_cy = content_y0 + section_h + section_h // 2
    sec3_cy = content_y0 + 2 * section_h + section_h // 2
    for i, (hour, temp, icon) in enumerate(data["hours"]):
        cx = inner_x0 + i * col_w + col_w // 2
        draw_small_weather_icon(icon, cx - 7, sec1_cy - 6)
        text12_center(cx, sec2_cy - 4, hour, 1, 0, True, False)
        text12_center(cx, sec3_cy - 4, temp, 1, 0, True, False)


def draw_5day_panel(data):
    x0, y0, x1, y1 = 205, 150, 392, 260
    rect_frame(x0, y0, x1, y1)
    text12(x0 + 6, y0 + 6, "5 DAYS 五日预报", 1, 0, True, True)
    dotted_hline(x0 + 1, x1 - 1, y0 + 24)
    # 中文星期、日期各占固定列，起始位置对齐。
    # 温度右对齐到面板右边距内（x1-4=388），位数再长也不会越界。
    week_cn_x, date_x, temp_right = x0 + 6, 243, x1 - 4
    for i, (week_cn, date, icon, bar, temps) in enumerate(data["days"]):
        ry = y0 + 28 + i * 16
        text12(week_cn_x, ry, week_cn)
        text12(date_x, ry, date)
        draw_small_weather_icon(icon, 282, ry - 2)
        rect_frame(300, ry + 2, 334, ry + 9)
        fill_rect(302, ry + 4, 302 + bar, ry + 7)
        text12_right(temp_right, ry, temps)


def draw_big_sun_cloud(x, y):
    circle_ring(x + 28, y + 16, 13, 2)
    for dx, dy in ((0, -20), (0, 20), (-20, 0), (20, 0),
                   (-14, -14), (14, -14), (-14, 14), (14, 14)):
        fill_rect(x + 28 + dx - 2, y + 16 + dy - 2,
                  x + 28 + dx + 2, y + 16 + dy + 2)
    circle_ring(x + 34, y + 34, 14, 2)
    circle_ring(x + 52, y + 32, 12, 2)
    circle_ring(x + 68, y + 38, 9, 2)
    hline(x + 18, x + 76, y + 50)


def draw_weather_page(now, data):
    fill_rect(0, 0, W - 1, H - 1, False)
    draw_status_bar(now)
    text12(10, TITLE_TOP, "WEATHER 天气预报", 1, 0, True, False)
    place = f"{data['city_en']} {data['city']} · 天气"
    text12_right(392, TITLE_TOP, place, 1, 0, True, False)
    fill_rect(0, TITLE_RULE_Y, W - 1, TITLE_RULE_Y + TITLE_RULE_H - 1)

    draw_big_sun_cloud(18, 72)
    bx0, _, bx1, _ = dseg_text_ink_bounds(data["temp"])
    draw_dseg_text(data["temp"], 112 - bx0, 112)
    circle_ring(112 + (bx1 - bx0) + 5, 76, 3, 1)

    # 体感行右对齐到 238（WIND 卡左边距 6px），负温加长也不会撞卡片。
    feel = f"{data['condition_en']} {data['condition_cn']} · 体感 {data['apparent']}"
    text12_right(238, 116, feel, 1, 0, True, False)
    text12(114, 132, f"HIGH {data['high']}  LOW {data['low']}")

    rect_frame(244, 62, 392, 138)
    text12(250, 68, "WIND 风力", 1, 0, True, True)
    text12(250, 86, data["wind"])
    dotted_hline(250, 386, 100)
    text12(250, 106, f"UV 指数 {data['uv']}", 1, 0, True, True)
    draw_uv_bars(250, 122, data["uv"])

    draw_24h_panel(data)
    draw_5day_panel(data)
    text12(10, 267,
           f"UPDATED 更新于 {data['updated']}  ·  AUTO REFRESH 自动刷新。")
    draw_action_bar(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", default=None, help="YYYY-MM-DD")
    ap.add_argument("--time", default=None, help="HH:MM:SS")
    ap.add_argument("--raw-time", default=None,
                    help="大字时间测试字符串，可传 99:99:99；状态栏/表盘仍用有效时间")
    ap.add_argument("--lunar", default="农历 十月十六")
    ap.add_argument("--page", type=int, choices=(1, 2), default=1,
                    help="1=时间与日历，2=天气时钟第二页")
    ap.add_argument("--negative", action="store_true",
                    help="第二页温度样例全改负值，检查溢出与对齐")
    args = ap.parse_args()

    if args.date:
        base = dt.datetime.strptime(args.date, "%Y-%m-%d")
    else:
        base = dt.datetime.now()
    raw_time = base.strftime("%H:%M:%S")
    if args.time:
        hh, mm, ss = (int(v) for v in args.time.split(":"))
        raw_time = args.time
        # 超出时钟范围的值只用于大字宽度测试，日期/表盘保持有效时间。
        if 0 <= hh <= 23 and 0 <= mm <= 59 and 0 <= ss <= 59:
            base = base.replace(hour=hh, minute=mm, second=ss)
    if args.raw_time:
        raw_time = args.raw_time

    if args.page == 2:
        sample = apply_negative(WEATHER_SAMPLE) if args.negative else WEATHER_SAMPLE
        draw_weather_page(base, sample)
        stem = "time_calendar_weather_render"
    else:
        draw_status_bar(base)
        draw_title()
        draw_time_and_date(base, args.lunar, raw_time)
        draw_dial(base)
        draw_month(base)
        draw_action_bar(0)
        stem = "time_calendar_render"
        bx0, _, bx1, _ = dseg_text_ink_bounds(raw_time)
        ink_w = bx1 - bx0 + 1
        x = ((CARD_X0 - 1) + 1 - ink_w) // 2 - bx0
        left_margin = x + bx0
        right_margin = (CARD_X0 - 1) - (x + bx1)
        print(f"time={raw_time} ink_x={x + bx0}..{x + bx1} "
              f"left={left_margin} right={right_margin}")

    out = pathlib.Path(__file__).with_name(stem + ".png")
    img.save(out)
    img.resize((W * 2, H * 2), Image.NEAREST).save(
        pathlib.Path(__file__).with_name(stem + "_2x.png"))
    print("wrote", out)


if __name__ == "__main__":
    main()
