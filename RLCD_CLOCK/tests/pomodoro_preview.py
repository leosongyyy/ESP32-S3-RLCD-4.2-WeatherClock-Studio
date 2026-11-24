#!/usr/bin/env python3
"""离线预览 POMODORO 番茄钟工作页在 400x300 反射屏上的样子。

1bit 电子纸，无灰度、无抗锯齿。文字用 Fusion Pixel 取模字库
（pixel_font.py），图标经 lucide_raster.py 1 倍硬边栅格化。番茄直接用
用户指定的一对填充式 SVG（icons/tomato/，空心与实心各一），
1 倍硬边栅格化，进度圆环用像素点阵绘制。

只复刻中间 UI，状态栏与底栏沿用主菜单同款样式。
⚠ 这是版式预览，不是固件实现。确认后才会写进 LVGL。
运行：python3 RLCD_CLOCK/tests/pomodoro_preview.py
"""

from PIL import Image, ImageDraw
import math
import xml.etree.ElementTree as ET
from pathlib import Path

import pixel_font as pf
import lucide_raster as lr

W, H = 400, 300
INK, PAPER = 0, 255   # 1bit：0 黑，255 白

img = Image.new("1", (W, H), PAPER)
d = ImageDraw.Draw(img)


def px(x, y, c=INK):
    if 0 <= x < W and 0 <= y < H:
        d.point((x, y), fill=c)


def tw(s, scale=1, extra=0):
    return pf.text_w(s, scale, extra)


def pt(x, top, s, scale=1, extra=0, c=INK, bold=False, thick_y=None):
    """按顶部对齐画取模文本，返回末端 x。bold=粗体（参考图风格）。"""
    if bold:
        return pf._draw_bold(img, pf.main.GLYPHS, pf.FONT_ASCENT, x,
                             top + pf.FONT_ASCENT * scale, s, scale,
                             extra, c, thick_y=thick_y)
    return pf.draw(img, x, top + pf.FONT_ASCENT * scale, s, scale, extra, c)


def pt_center(cx, top, s, scale=1, extra=0, c=INK, bold=False, thick_y=None):
    pt(cx - tw(s, scale, extra) // 2, top, s, scale, extra, c, bold, thick_y)


def pts(x, top, s, extra=0, c=INK):
    return pf.small_draw(img, x, top + pf.SMALL_ASCENT, s, 1, extra, c)


def pts_bold(x, top, s, extra=0, c=INK):
    return pf.small_bold_draw(img, x, top + pf.SMALL_ASCENT, s, 1, extra, c)


def rect(x0, y0, x1, y1, c=INK):
    d.rectangle((x0, y0, x1, y1), fill=c)


def frame(x0, y0, x1, y1, c=INK, width=1):
    d.rectangle((x0, y0, x1, y1), outline=c, width=width)


def hline(x0, x1, y, c=INK, width=1):
    d.line((x0, y, x1, y), fill=c, width=width)


def vline(x, y0, y1, c=INK):
    d.line((x, y0, x, y1), fill=c)


def chamfer_frame(x0, y0, x1, y1, cut=12, c=INK, width=2):
    """切角矩形（八边形）边框。"""
    p = [(x0 + cut, y0), (x1 - cut, y0), (x1, y0 + cut), (x1, y1 - cut),
         (x1 - cut, y1), (x0 + cut, y1), (x0, y1 - cut), (x0, y0 + cut)]
    d.line(p + [p[0]], fill=c, width=width, joint="curve")


# ---------------- 番茄图标（严格按用户指定的一对 SVG 原样渲染，1 倍硬边） ----------------
TOMATO_DIR = Path(__file__).resolve().parent / "icons" / "tomato"
_TOMATO_CACHE = {}


def _tomato(name, size):
    """直接栅格化 SVG，不加粗、不细化、不加网点，完全按图标原始形状。"""
    key = (name, size)
    if key not in _TOMATO_CACHE:
        _TOMATO_CACHE[key] = lr.render_filled_svg(TOMATO_DIR / (name + ".svg"), size)
    return _TOMATO_CACHE[key]


def draw_tomato(cx, cy, size, filled=False):
    """按中心点贴番茄图标。size 为边长，左上角 = 中心 - size/2。"""
    src = _tomato("tomato_solid" if filled else "tomato_outline", size)
    img.paste(src, (cx - size // 2, cy - size // 2))


def draw_small_tomato(cx, cy, r=8, filled=True):
    draw_tomato(cx, cy, r * 2, filled=filled)


# ---------------- 顶部状态栏（与主菜单完全一致） ----------------
# 日期时间同一行（左侧 0..118），其余 5 个图标显式定位：
# 温度 170 / 湿度 225，WiFi 300 / 蓝牙 322（挨着并贴近电池，电框左边缘 340），电量 365。
# 图标线条统一 1px，与电池框粗度一致。底部 3px 粗横线区隔。
STATUS_H = 32
ICON_CX = [170, 225, 300, 322, 365]   # 温度/湿度/WiFi/蓝牙/电量
icon_cx = lambda i: ICON_CX[i]
ICON_KW = dict(ss=1, width_px=1)      # 1 倍硬边，线宽 1px

# 日期时间同一行：09:41 大字(scale=2) + 2026-11-24 小字(负字距)
pt(6, 6, "09:41", scale=2)
pt(68, 10, "2026-11-24", extra=-1)

# 「18px 图标 + 文字」组，按整组墨宽居中到槽中心（与固件一致）
def icon_text_group(icon_name, text, cx):
    group_w = 18 + 2 + tw(text)
    sx = cx - group_w // 2
    img.paste(lr.icon(icon_name, 18, **ICON_KW), (sx, 7))
    pt(sx + 20, 11, text)


icon_text_group("thermometer-sun", "26°C", icon_cx(0))
icon_text_group("droplets", "45%", icon_cx(1))
cx = icon_cx(2)
img.paste(lr.icon("wifi-off", 18, **ICON_KW), (cx - 9, 7))
cx = icon_cx(3)
img.paste(lr.icon("bluetooth-off", 18, **ICON_KW), (cx - 9, 7))

# 电池电量：整组（框28+间隔3+文字）按组宽居中
cx = icon_cx(4)
group_w = 28 + 3 + tw("75%")
bx = cx - group_w // 2
frame(bx, 10, bx + 24, 22)
rect(bx + 25, 14, bx + 27, 18)
for k in range(4):
    qx = bx + 2 + k * 5
    if k < 3:
        rect(qx, 12, qx + 3, 20)
    else:
        frame(qx, 12, qx + 3, 20)
pt(bx + 31, 12, "75%")
rect(0, STATUS_H, W - 1, STATUS_H + 2)   # 3px 粗横线，与下方区域区隔

# ---------------- 中间 UI（坐标/字号按参考截图实测对齐） ----------------
# 参考图状态栏粗线在 y22..23，当前主菜单状态栏粗线在 y34..36，
# 所以中间 UI 整体下移 12px，保持与参考图一致的相对间距。
DY = 12
# 参考 bbox：标题 y30..44 / 大番茄 x219..278 y54..113 /
# 圆环 x13..190 / 切角框 x47..166 y87..205 / NEXT y253..264
TITLE_TOP = 30 + DY
# 左侧标题整组放大一档(scale=2)，整体居中对齐下方圆环中心 RCX=107
_TGAP = 8
_TW = tw("TOMATO", 2) + _TGAP + tw("番茄钟", 2)
_TITLE_CX = 107
tx = _TITLE_CX - _TW // 2
pt(tx, TITLE_TOP + 1, "TOMATO", scale=2)
pt(tx + tw("TOMATO", 2) + _TGAP, TITLE_TOP + 1, "番茄钟", scale=2)
# 右侧标题整组居中对齐下方 MODE/STATS 方形框中心 x=301.5，两者留 6px 间距
_RGAP = 6
_RW = tw("SESSION", 2) + _RGAP + tw("第 3 节")
_rx = 301 - _RW // 2
pt(_rx, TITLE_TOP + 1, "SESSION", scale=2)
pt(_rx + tw("SESSION", 2) + _RGAP, TITLE_TOP + 2, "第 3 节")

# --- 左侧圆环进度 ---
RCX, RCY, RR = 107, 158, 86   # 圆环中心对齐切角框中心(106.5,158)，两者同心
N = 30            # 方块数
done = 10         # 实心方块数（进度约 1/3）
for i in range(N):
    a = -math.pi / 2 + 2 * math.pi * i / N
    sx = RCX + RR * math.cos(a)
    sy = RCY + RR * math.sin(a)
    s = 6
    x0, y0 = int(round(sx)) - s // 2, int(round(sy)) - s // 2
    if i < done:
        rect(x0, y0, x0 + s - 1, y0 + s - 1, INK)
    else:
        frame(x0, y0, x0 + s - 1, y0 + s - 1)

# 中央切角框（参考 x47..166 y87..205）
chamfer_frame(47, 87 + DY, 166, 205 + DY, cut=12, width=2)
# 大字倒计时（参考 x58..159 y118..161，粗体 scale=4）
pt_center(RCX, 116 + DY, "18:42", scale=4, bold=True, thick_y=0)
# FOCUS / 专注中（参考 y171..180 / y185..196）
pt_center(RCX, 169 + DY, "FOCUS", extra=1)
pt_center(RCX, 183 + DY, "专注中")

# --- 右上：番茄 + TODAY ---
# 番茄组总宽不得超过下方方形框 x215..388（宽174）。
# 大番茄 59px 贴框左(216..274)，4 个小番茄(直径20、间距29)排到 386 收尾。
draw_tomato(245, 84 + DY, 59, filled=False)
pt(294, 58 + DY, "TODAY")
pt(294 + tw("TODAY") + 6, 58 + DY, "今日")
pt(294, 74 + DY, "完成 3 个番茄")
for i in range(4):
    draw_small_tomato(290 + i * 29, 103 + DY, r=10, filled=(i < 3))

# --- MODE 模式框（参考 x216..388 y122..198） ---
MX0, MY0, MX1, MY1 = 215, 122 + DY, 388, 198 + DY
frame(MX0, MY0, MX1, MY1, width=2)
pt(MX0 + 7, MY0 + 5, "MODE")
pt(MX0 + 7 + tw("MODE") + 6, MY0 + 5, "模式")
# 三行：圆点 + 名称 + 时间（右对齐），选中行浅灰底
rows = [
    ("FOCUS 专注", "25:00", True),
    ("SHORT BREAK 短休", "05:00", False),
    ("LONG BREAK 长休", "15:00", False),
]
ry0 = MY0 + 20
for i, (label, t, sel) in enumerate(rows):
    y = ry0 + i * 18
    # 圆点（参考实心圆点约 7px）
    dotcx, dotcy = MX0 + 13, y + 5
    for yy in range(-4, 5):
        for xx in range(-4, 5):
            if xx * xx + yy * yy <= 14:
                if sel or xx * xx + yy * yy > 6:
                    px(dotcx + xx, dotcy + yy)
    pt(MX0 + 23, y, label, extra=-1)
    pt(MX1 - 5 - tw(t, 1, 0), y, t)

# --- STATS 统计框（参考 x216..388 y204..243） ---
SX0, SY0, SX1, SY1 = 215, 204 + DY, 388, 243 + DY
frame(SX0, SY0, SX1, SY1, width=2)
pt(SX0 + 7, SY0 + 5, "STATS")
pt(SX0 + 7 + tw("STATS") + 6, SY0 + 5, "统计")
pt(SX0 + 7, SY0 + 23, "累计 1h 15m · 中断 0 次")

# --- NEXT 行（参考 y253..264） ---
hline(0, W - 1, 249 + DY)
# 整行在上下横线(y261 / y282)之间垂直居中：下移3px使上下边距相等
pt(10, 255 + DY, "NEXT")
# NEXT 与"下一次"间距 = 一个空格宽，与"短休"到"05:00"的间距一致
pt(10 + tw("NEXT") + tw(" "), 255 + DY, "下一次  短休 05:00 后自动开始")

# ---------------- 底部按键提示栏（与主菜单完全一致） ----------------
BAR_Y = 282
hline(0, W - 1, BAR_Y)
img.paste(lr.icon("dpad", 12, ss=1, width_px=1), (8, BAR_Y + 4))
pt(30, BAR_Y + 6, "MOVE", extra=-1)
pt(30 + tw("MOVE", extra=-1) + 4, BAR_Y + 6, "移动")
vline(88, BAR_Y + 3, H - 3)

d.rounded_rectangle((94, BAR_Y + 4, 108, BAR_Y + 15), radius=2, fill=INK)
pt(99, BAR_Y + 6, "A", c=PAPER)
pt(113, BAR_Y + 5, "SELECT")
pt(113 + tw("SELECT") + 4, BAR_Y + 6, "确认")
vline(196, BAR_Y + 3, H - 3)

d.rounded_rectangle((202, BAR_Y + 4, 216, BAR_Y + 15), radius=2, fill=INK)
pt(207, BAR_Y + 6, "B", c=PAPER)
pt(221, BAR_Y + 5, "BACK")
pt(221 + tw("BACK") + 4, BAR_Y + 6, "返回")

for k in range(4):
    bx = W - 40 + k * 9
    if k < 3:
        rect(bx, BAR_Y + 5, bx + 6, BAR_Y + 13)
    else:
        frame(bx, BAR_Y + 5, bx + 6, BAR_Y + 13)

img.save("pomodoro_preview.png")
print("saved pomodoro_preview.png")
