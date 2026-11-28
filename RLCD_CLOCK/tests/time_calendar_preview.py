#!/usr/bin/env python3
"""时间与日历页的离线版式预览。

坐标与固件 main/ui/ui_time_calendar.cpp 里的常量一一对应，
改这里之前先改固件，或者两边一起改，避免预览和真机对不上。

用法：
    python3 time_calendar_preview.py
    python3 time_calendar_preview.py --font /path/to/fusion_pixel_12.bin
输出：同目录 time_calendar_preview.png（400x300 原尺寸 + 2 倍放大）
"""

from __future__ import annotations

import argparse
import pathlib
import struct

from PIL import Image, ImageDraw, ImageFont

W, H = 400, 300
INK = 0
PAPER = 255

# ---------- 与固件一致的布局常量 ----------
STATUS_H = 34            # 状态栏，含底部 2px 黑线
TITLE_TEXT_X = 8
TITLE_TEXT_Y = 40
TITLE_H = 17
RULE_Y = 60
RULE_H = 3

TIME_ROW_Y = 66
TIME_ROW_H = 78
TIME_TEXT_X = 14
CARD_X0, CARD_X1 = 256, 392

LOWER_Y = 150
DIAL_CX, DIAL_CY, DIAL_R = 68, 214, 58
CAL_X0, CAL_Y0 = 132, 150
CAL_COLW = 37
CAL_HEADER_H = 14
CAL_ROW_H = 18
CAL_ROWS = 6

BAR_Y = 282

WEEK_EN = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"]

# 5x7 点阵，放大 6 倍后每字 30x42
GLYPHS = {
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
DIGIT_SCALE = 5
DIGIT_W = 5 * DIGIT_SCALE          # 25
DIGIT_H = 7 * DIGIT_SCALE          # 35
DIGIT_ADV = 34
COLON_ADV = 18


def load_cjk(size: int):
    for name in ("/System/Library/Fonts/PingFang.ttc",
                 "/System/Library/Fonts/Hiragino Sans GB.ttc",
                 "/System/Library/Fonts/STHeiti Light.ttc"):
        if pathlib.Path(name).exists():
            try:
                return ImageFont.truetype(name, size)
            except OSError:
                continue
    return ImageFont.load_default()


def time_width(text: str) -> int:
    w = 0
    for ch in text:
        w += COLON_ADV if ch == ":" else DIGIT_ADV
    return w - (DIGIT_ADV - DIGIT_WS) if False else w


DIGIT_WS = DIGIT_W


def draw_big_time(d: ImageDraw.ImageDraw, x: int, y: int, text: str) -> int:
    cx = x
    for ch in text:
        rows = GLYPHS.get(ch)
        if rows is None:
            cx += COLON_ADV
            continue
        adv = COLON_ADV if ch == ":" else DIGIT_ADV
        for ry, bits in enumerate(rows):
            for rx in range(5):
                if bits & (1 << (4 - rx)):
                    x0 = cx + rx * DIGIT_SCALE
                    y0 = y + ry * DIGIT_SCALE
                    d.rectangle([x0, y0, x0 + DIGIT_SCALE - 1, y0 + DIGIT_SCALE - 1],
                                fill=INK)
        cx += adv
    return cx


def big_time_width(text: str) -> int:
    return sum(COLON_ADV if ch == ":" else DIGIT_ADV for ch in text)


def draw_status_bar(d: ImageDraw.ImageDraw, font_cjk, small):
    d.text((6, 5), "06:12", font=small, fill=INK)
    d.text((52, 9), "2026-11-28", font=small, fill=INK)
    d.text((150, 8), "21°C", font=small, fill=INK)
    d.text((190, 8), "48%", font=small, fill=INK)
    # wifi / 蓝牙 / 电量，用简单线条占位
    d.rectangle([262, 10, 272, 20], outline=INK)
    d.rectangle([274, 13, 276, 17], fill=INK)
    d.text((286, 10), "BT", font=small, fill=INK)
    d.rectangle([330, 9, 366, 21], outline=INK)
    d.rectangle([368, 13, 371, 17], fill=INK)
    for k in range(4):
        if k < 3:
            d.rectangle([333 + k * 8, 12, 338 + k * 8, 18], fill=INK)
    d.text((374, 9), "75%", font=small, fill=INK)
    d.rectangle([0, STATUS_H - 2, W - 1, STATUS_H - 1], fill=INK)


def draw_title(d: ImageDraw.ImageDraw, font_en_bold, font_cjk):
    d.text((TITLE_TEXT_X, TITLE_TEXT_Y), "TIME & CALENDAR", font=font_en_bold, fill=INK)
    w = d.textlength("TIME & CALENDAR", font=font_en_bold)
    d.text((TITLE_TEXT_X + w + 14, TITLE_TEXT_Y - 1), "时间与日历", font=font_cjk, fill=INK)
    d.rectangle([0, RULE_Y, W - 1, RULE_Y + RULE_H - 1], fill=INK)


def draw_time_block(d: ImageDraw.ImageDraw, font_en, font_cjk, font_lunar):
    text = "10:30:45"
    tw = big_time_width(text)
    x = TIME_TEXT_X + ((CARD_X0 - 8) - TIME_TEXT_X - tw) // 2
    y = TIME_ROW_Y + (TIME_ROW_H - DIGIT_H) // 2
    draw_big_time(d, x, y, text)

    # 黑底日期卡
    d.rectangle([CARD_X0, TIME_ROW_Y, CARD_X1, TIME_ROW_Y + TIME_ROW_H - 1], fill=INK)
    def centered(s, font, top):
        w = d.textlength(s, font=font)
        d.text((CARD_X0 + (CARD_X1 - CARD_X0 - w) / 2, top), s, font=font, fill=PAPER)

    centered("JULY 14, 2025", font_en, TIME_ROW_Y + 7)
    centered("星期一 (Mon)", font_cjk, TIME_ROW_Y + 28)
    centered("农历 六月廿三", font_lunar, TIME_ROW_Y + 52)


def draw_analog(d: ImageDraw.ImageDraw, small):
    cx, cy, r = DIAL_CX, DIAL_CY, DIAL_R
    # 外圈 3px
    for i in range(3):
        d.ellipse([cx - r + i, cy - r + i, cx + r - i, cy + r - i], outline=INK)
    # 分钟刻度
    import math
    for m in range(60):
        if m % 5 == 0:
            continue
        a = math.radians(m * 6)
        x1 = cx + (r - 4) * math.sin(a)
        y1 = cy - (r - 4) * math.cos(a)
        d.point((x1, y1), fill=INK)
    # 小时刻度数字
    for h in range(1, 13):
        a = math.radians(h * 30)
        tx = cx + (r - 15) * math.sin(a)
        ty = cy - (r - 15) * math.cos(a)
        d.text((tx, ty), str(h), font=small, fill=INK, anchor="mm")
    # 指针：10:30:45
    def hand(angle_deg, length, width):
        a = math.radians(angle_deg)
        x1 = cx + length * math.sin(a)
        y1 = cy - length * math.cos(a)
        d.line([cx, cy, x1, y1], fill=INK, width=width)

    hand((10 + 30 / 60) * 30, r - 24, 5)
    hand((30 + 45 / 60) * 6, r - 10, 2)
    hand(45 * 6, r - 6, 1)
    d.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], fill=INK)


def draw_month_grid(d: ImageDraw.ImageDraw, small, font_cjk_small,
                    year=2025, month=7, today=14):
    import calendar as cal
    d.rectangle([CAL_X0, CAL_Y0, CAL_X0 + CAL_COLW * 7 - 1,
                 CAL_Y0 + CAL_HEADER_H + CAL_ROWS * CAL_ROW_H - 1], outline=INK)
    for i, name in enumerate(WEEK_EN):
        x = CAL_X0 + i * CAL_COLW
        if i:
            d.line([x, CAL_Y0, x, CAL_Y0 + CAL_HEADER_H + CAL_ROWS * CAL_ROW_H - 1],
                   fill=INK)
        w = d.textlength(name, font=small)
        d.text((x + (CAL_COLW - w) / 2, CAL_Y0 + 1), name, font=small, fill=INK)
    d.line([CAL_X0, CAL_Y0 + CAL_HEADER_H, CAL_X0 + CAL_COLW * 7 - 1,
            CAL_Y0 + CAL_HEADER_H], fill=INK)

    first_wd = (cal.weekday(year, month, 1) + 1) % 7     # 周日 = 0
    days = cal.monthrange(year, month)[1]
    cell = 0
    for day in range(1, days + 1):
        idx = first_wd + day - 1
        row, col = idx // 7, idx % 7
        x = CAL_X0 + col * CAL_COLW
        y = CAL_Y0 + CAL_HEADER_H + row * CAL_ROW_H
        if row >= CAL_ROWS:
            break
        if row:
            d.line([x, y, x + CAL_COLW - 1, y], fill=INK)
        if day == today:
            d.rectangle([x + 1, y + 1, x + CAL_COLW - 2, y + CAL_ROW_H - 1], fill=INK)
            d.text((x + 4, y + 2), f"{day}", font=font_cjk_small, fill=PAPER)
        else:
            d.text((x + 4, y + 2), f"{day}", font=font_cjk_small, fill=INK)


def draw_action_bar(d: ImageDraw.ImageDraw, small):
    d.line([0, BAR_Y, W - 1, BAR_Y], fill=INK)
    d.text((8, BAR_Y + 6), "SWITCH 切换", font=small, fill=INK)
    d.rectangle([120, BAR_Y + 4, 136, BAR_Y + 15], outline=INK)
    d.text((122, BAR_Y + 6), "A", font=small, fill=INK)
    d.text((140, BAR_Y + 6), "DETAIL 详情", font=small, fill=INK)
    d.rectangle([240, BAR_Y + 4, 254, BAR_Y + 15], outline=INK)
    d.text((243, BAR_Y + 6), "B", font=small, fill=INK)
    d.text((258, BAR_Y + 6), "BACK 返回", font=small, fill=INK)
    for k in range(5):
        bx = W - 46 + k * 9
        if k == 0:
            d.rectangle([bx, BAR_Y + 5, bx + 6, BAR_Y + 13], fill=INK)
        else:
            d.rectangle([bx, BAR_Y + 5, bx + 6, BAR_Y + 13], outline=INK)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", help="fusion_pixel_12.bin（本轮只做版式，可省略）")
    args = ap.parse_args()

    img = Image.new("1", (W, H), PAPER)
    d = ImageDraw.Draw(img)
    font_en_bold = load_cjk(15)
    font_en = load_cjk(13)
    font_cjk = load_cjk(16)
    font_lunar = load_cjk(16)
    small = load_cjk(11)
    cjk_small = load_cjk(12)

    draw_status_bar(d, font_cjk, small)
    draw_title(d, font_en_bold, font_cjk)
    draw_time_block(d, font_en, font_cjk, font_lunar)
    draw_analog(d, small)
    draw_month_grid(d, small, cjk_small)
    draw_action_bar(d, small)

    out = pathlib.Path(__file__).with_name("time_calendar_preview.png")
    img.save(out)
    img.resize((W * 2, H * 2), Image.NEAREST).save(
        pathlib.Path(__file__).with_name("time_calendar_preview_2x.png"))
    print("wrote", out)


if __name__ == "__main__":
    main()
