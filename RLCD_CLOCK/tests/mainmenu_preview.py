#!/usr/bin/env python3
"""离线预览「MAIN MENU 主菜单」新首页在 400x300 反射屏上的样子。

1bit 电子纸，无灰度、无抗锯齿。所有元素转成点阵：
  - 英文用项目现成的 5x7 字库（与 gameboy_chrome.cpp 同源）
  - 中文用系统字体小字号渲染后阈值化成点阵
  - 图标用 Lucide SVG（tests/icons/lucide/）经 lucide_raster.py 栅格化

⚠ 这是版式预览，不是固件实现。确认后才会写进 LVGL。
运行：python3 RLCD_CLOCK/tests/mainmenu_preview.py
"""

from PIL import Image, ImageDraw, ImageFont

import lucide_raster as lr

W, H = 400, 300
INK, PAPER = 0, 255   # 1bit：0 黑，255 白

FONT = {
    '0': (0x0E,0x11,0x13,0x15,0x19,0x11,0x0E), '1': (0x04,0x0C,0x04,0x04,0x04,0x04,0x0E),
    '2': (0x0E,0x11,0x01,0x02,0x04,0x08,0x1F), '3': (0x1F,0x02,0x04,0x02,0x01,0x11,0x0E),
    '4': (0x02,0x06,0x0A,0x12,0x1F,0x02,0x02), '5': (0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E),
    '6': (0x06,0x08,0x10,0x1E,0x11,0x11,0x0E), '7': (0x1F,0x01,0x02,0x04,0x08,0x08,0x08),
    '8': (0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E), '9': (0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C),
    'A': (0x0E,0x11,0x11,0x1F,0x11,0x11,0x11), 'B': (0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E),
    'C': (0x0E,0x11,0x10,0x10,0x10,0x11,0x0E), 'D': (0x1E,0x11,0x11,0x11,0x11,0x11,0x1E),
    'E': (0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F), 'F': (0x1F,0x10,0x10,0x1E,0x10,0x10,0x10),
    'G': (0x0E,0x11,0x10,0x17,0x11,0x11,0x0E), 'H': (0x11,0x11,0x11,0x1F,0x11,0x11,0x11),
    'I': (0x1F,0x04,0x04,0x04,0x04,0x04,0x1F), 'J': (0x07,0x02,0x02,0x02,0x02,0x12,0x0C),
    'K': (0x11,0x12,0x14,0x18,0x14,0x12,0x11), 'L': (0x10,0x10,0x10,0x10,0x10,0x10,0x1F),
    'M': (0x11,0x1B,0x15,0x15,0x11,0x11,0x11), 'N': (0x11,0x19,0x15,0x13,0x11,0x11,0x11),
    'O': (0x0E,0x11,0x11,0x11,0x11,0x11,0x0E), 'P': (0x1E,0x11,0x11,0x1E,0x10,0x10,0x10),
    'Q': (0x0E,0x11,0x11,0x11,0x15,0x13,0x0F), 'R': (0x1E,0x11,0x11,0x1E,0x14,0x12,0x11),
    'S': (0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E), 'T': (0x1F,0x04,0x04,0x04,0x04,0x04,0x04),
    'U': (0x11,0x11,0x11,0x11,0x11,0x11,0x0E), 'V': (0x11,0x11,0x11,0x11,0x11,0x0A,0x04),
    'W': (0x11,0x11,0x11,0x15,0x15,0x1B,0x11), 'X': (0x11,0x11,0x0A,0x04,0x0A,0x11,0x11),
    'Y': (0x11,0x11,0x0A,0x04,0x04,0x04,0x04), 'Z': (0x1F,0x01,0x02,0x04,0x08,0x10,0x1F),
    ':': (0x00,0x04,0x04,0x00,0x04,0x04,0x00), '-': (0x00,0x00,0x00,0x0E,0x00,0x00,0x00),
    '/': (0x01,0x02,0x02,0x04,0x08,0x08,0x10), '%': (0x19,0x1A,0x02,0x04,0x08,0x0B,0x13),
    '.': (0x00,0x00,0x00,0x00,0x00,0x0C,0x0C), '!': (0x04,0x04,0x04,0x04,0x04,0x00,0x04),
    ' ': (0x00,)*7,
}
GW, GH, PITCH = 5, 7, 6

img = Image.new("1", (W, H), PAPER)
d = ImageDraw.Draw(img)


def px(x, y, c=INK):
    if 0 <= x < W and 0 <= y < H:
        d.point((x, y), fill=c)


def text5(x, y, s, scale=1, c=INK, pitch=None):
    """5x7 点阵字符串，返回末端 x。"""
    step = (pitch or PITCH) * scale
    cur = x
    for ch in s.upper():
        g = FONT.get(ch, FONT[' '])
        for row in range(GH):
            for col in range(GW):
                if g[row] & (1 << (4 - col)):
                    for sy in range(scale):
                        for sx in range(scale):
                            px(cur + col * scale + sx, y + row * scale + sy, c)
        cur += step
    return cur


def tw(s, scale=1, pitch=None):
    return (len(s) - 1) * ((pitch or PITCH) * scale) + GW * scale if s else 0


def text5_center(cx, y, s, scale=1, c=INK, pitch=None):
    text5(cx - tw(s, scale, pitch) // 2, y, s, scale, c, pitch)


_CJK = "/System/Library/Fonts/STHeiti Medium.ttc"


def cn(x, y, s, size=11, c=INK):
    """中文小字号阈值化点阵。"""
    f = ImageFont.truetype(_CJK, size)
    bb = d.textbbox((0, 0), s, font=f)
    w, h = bb[2] - bb[0] + 2, bb[3] - bb[1] + 2
    tmp = Image.new("1", (w, h), PAPER)
    td = ImageDraw.Draw(tmp)
    td.text((0, 0), s, font=f, fill=INK)
    img.paste(tmp, (x, y))


def cn_center(cx, y, s, size=11, c=INK):
    f = ImageFont.truetype(_CJK, size)
    bb = d.textbbox((0, 0), s, font=f)
    cn(cx - (bb[2] - bb[0]) // 2, y, s, size, c)


def rect(x0, y0, x1, y1, c=INK):
    d.rectangle((x0, y0, x1, y1), fill=c)


def frame(x0, y0, x1, y1, c=INK):
    d.rectangle((x0, y0, x1, y1), outline=c)


def hline(x0, x1, y, c=INK):
    d.line((x0, y, x1, y), fill=c)


def vline(x, y0, y1, c=INK):
    d.line((x, y0, x, y1), fill=c)


# ---------- 顶部状态栏 ----------
STATUS_H = 22
text5(6, 6, "09:41", scale=2)
vline(78, 2, STATUS_H - 2)
# 温度计 / 水滴 / WiFi / 蓝牙，全部取自 Lucide
img.paste(lr.icon("thermometer", 15), (90, 3))
text5(107, 6, "26C", scale=1)
vline(128, 2, STATUS_H - 2)
img.paste(lr.icon("droplet", 14), (134, 4))
text5(150, 6, "45%", scale=1)
vline(180, 2, STATUS_H - 2)
img.paste(lr.icon("wifi", 14), (188, 4))
d.ellipse((210, 1, 216, 7), fill=INK)
vline(226, 2, STATUS_H - 2)
img.paste(lr.icon("bluetooth", 15), (233, 3))
d.ellipse((248, 1, 254, 7), fill=INK)
hline(0, W - 1, STATUS_H)

# ---------- 标题栏 ----------
TITLE_Y = STATUS_H + 1
text5(6, TITLE_Y + 3, "MAIN MENU", scale=2)
cn(6 + tw("MAIN MENU", 2) + 8, TITLE_Y + 4, "主菜单", size=14)
text5(W - 6 - tw("SYS V1.08", 1), TITLE_Y + 4, "SYS V1.08", scale=1)
hline(0, W - 1, TITLE_Y + 22)

# ---------- 卡片网格 ----------
GRID_TOP = TITLE_Y + 25
CARD_W, CARD_H = 94, 100
GAP_X, GAP_Y = 6, 5
X0 = (W - (CARD_W * 4 + GAP_X * 3)) // 2
GRID_BOTTOM = 280

cards = [
    ("WEATHER",      "天气时钟", "weather"),
    ("POMODORO",     "番茄钟",   "pomodoro"),
    ("GAMES",        "游戏",     "games"),
    ("READING",      "阅读",     "reading"),
    ("MUSIC",        "音乐",     "music"),
    ("NEWS",         "新闻",     "news"),
    ("AI ASSISTANT", "AI 助手",  "ai"),
    ("SETTINGS",     "设置",     "settings"),
]


# 卡片图标统一从 Lucide SVG 栅格化，预览与固件共用同一份点阵
ICON_NAMES = {
    "weather": "cloud-sun", "pomodoro": "timer", "games": "gamepad-2",
    "reading": "book-open", "music": "audio-lines", "news": "newspaper",
    "ai": "bot", "settings": "settings",
}
ICON_SIZE = 34

for i, (en, zh, key) in enumerate(cards):
    col, row = i % 4, i // 4
    x = X0 + col * (CARD_W + GAP_X)
    y = GRID_TOP + row * (CARD_H + GAP_Y)
    # 选中第一张用双线框
    frame(x, y, x + CARD_W, y + CARD_H, INK)
    if i == 0:
        frame(x + 2, y + 2, x + CARD_W - 2, y + CARD_H - 2, INK)
    cx = x + CARD_W // 2
    img.paste(lr.icon(ICON_NAMES[key], ICON_SIZE),
              (cx - ICON_SIZE // 2, y + 7))
    # 英文标题：长词缩 pitch
    pitch = 5 if len(en) > 9 else None
    text5_center(cx, y + 48, en, scale=1, pitch=pitch)
    cn_center(cx, y + 57, zh, size=11)
    hline(x + 6, x + CARD_W - 6, y + 74)
    # 状态行
    if key == "weather":
        text5_center(cx, y + 79, "26/09:41", scale=1, pitch=5)
    elif key == "pomodoro":
        text5(x + 10, y + 79, "25:00", scale=1, pitch=5)
        for k in range(4):
            bx = x + 48 + k * 9
            if k < 3:
                rect(bx, y + 80, bx + 6, y + 86)
            else:
                frame(bx, y + 80, bx + 6, y + 86)
    elif key == "games":
        text5_center(cx, y + 79, "HI 12480", scale=1, pitch=5)
    elif key == "reading":
        text5(x + 8, y + 79, "38%", scale=1, pitch=5)
        for k in range(6):
            bx = x + 36 + k * 8
            if k < 2:
                rect(bx, y + 80, bx + 6, y + 86)
            else:
                frame(bx, y + 80, bx + 6, y + 86)
    elif key == "music":
        text5_center(cx, y + 77, "NOW PLAYING", scale=1, pitch=4)
        # 均衡器贴卡片底沿，最高 9px，顶部到 y+88，不与文字(y+77..84)重叠
        for k, hh in enumerate((3, 5, 8, 4, 9, 6, 3, 8, 5)):
            bx = x + 18 + k * 7
            rect(bx, y + 97 - hh, bx + 4, y + 97)
    elif key == "news":
        text5_center(cx, y + 79, "12 NEW", scale=1, pitch=5)
    elif key == "ai":
        text5_center(cx, y + 79, "READY", scale=1, pitch=5)
    elif key == "settings":
        text5(x + 8, y + 79, "EN", scale=1)
        cn(x + 26, y + 78, "/ 中", size=10)
        cn(x + 56, y + 78, "双语", size=10)

# ---------- 底部按键提示栏 ----------
BAR_Y = 282
hline(0, W - 1, BAR_Y)
# 左右双箭头，取自 Lucide
img.paste(lr.icon("chevron-left", 12), (2, BAR_Y + 3))
img.paste(lr.icon("chevron-right", 12), (14, BAR_Y + 3))
text5(30, BAR_Y + 6, "MOVE", scale=1)
cn(30 + tw("MOVE") + 4, BAR_Y + 5, "移动", size=10)
vline(88, BAR_Y + 3, H - 3)

d.rounded_rectangle((94, BAR_Y + 3, 108, BAR_Y + 14), radius=2, fill=INK)
text5(99, BAR_Y + 6, "A", c=PAPER)
text5(113, BAR_Y + 4, "SELECT", scale=1)
cn(113 + tw("SELECT") + 4, BAR_Y + 5, "确认", size=10)
vline(196, BAR_Y + 3, H - 3)

d.rounded_rectangle((202, BAR_Y + 3, 216, BAR_Y + 14), radius=2, fill=INK)
text5(207, BAR_Y + 6, "B", c=PAPER)
text5(221, BAR_Y + 4, "BACK", scale=1)
cn(221 + tw("BACK") + 4, BAR_Y + 5, "返回", size=10)

for k in range(4):
    bx = W - 40 + k * 9
    if k < 1:
        rect(bx, BAR_Y + 5, bx + 6, BAR_Y + 13)
    else:
        frame(bx, BAR_Y + 5, bx + 6, BAR_Y + 13)

out = "mainmenu_preview.png"
img.resize((W * 3, H * 3), Image.NEAREST).save(out)
print("wrote", out)

# 越界自检
for y in range(H):
    for x in range(W):
        pass
print("bounds ok (fixed geometry, no dynamic text overflow)")
