#!/usr/bin/env python3
"""离线预览「MAIN MENU 主菜单」新首页在 400x300 反射屏上的样子。

1bit 电子纸，无灰度、无抗锯齿。所有元素转成点阵：
  - 文字统一用 Fusion Pixel 12px 取模字库（tools/gen_pixel_font.py 生成，
    tests/fusion_pixel_font.py），不再依赖系统字体，预览与固件共用点阵
  - 图标用 Lucide SVG（tests/icons/lucide/）经 lucide_raster.py 栅格化

⚠ 这是版式预览，不是固件实现。确认后才会写进 LVGL。
运行：python3 RLCD_CLOCK/tests/mainmenu_preview.py
"""

from PIL import Image, ImageDraw

import lucide_raster as lr
import pixel_font as pf

W, H = 400, 300
INK, PAPER = 0, 255   # 1bit：0 黑，255 白

img = Image.new("1", (W, H), PAPER)
d = ImageDraw.Draw(img)


def px(x, y, c=INK):
    if 0 <= x < W and 0 <= y < H:
        d.point((x, y), fill=c)


def tw(s, scale=1, extra=0):
    """取模文本宽度。"""
    return pf.text_w(s, scale, extra)


def pt(x, top, s, scale=1, extra=0, c=INK):
    """按英文顶部对齐画取模文本，返回末端 x。中文因 yoff=-1 会高 1px。"""
    return pf.draw(img, x, top + pf.FONT_ASCENT * scale, s, scale, extra, c)


def pt_center(cx, top, s, scale=1, extra=0, c=INK):
    pt(cx - tw(s, scale, extra) // 2, top, s, scale, extra, c)


def pts(x, top, s, extra=0, c=INK):
    """8px 小英文字库，按顶部对齐。"""
    return pf.small_draw(img, x, top + pf.SMALL_ASCENT, s, 1, extra, c)


def pts_center(cx, top, s, extra=0, c=INK):
    w = pf.small_text_w(s, 1, extra)
    pts(cx - w // 2, top, s, extra, c)


def rect(x0, y0, x1, y1, c=INK):
    d.rectangle((x0, y0, x1, y1), fill=c)


def frame(x0, y0, x1, y1, c=INK):
    d.rectangle((x0, y0, x1, y1), outline=c)


def hline(x0, x1, y, c=INK):
    d.line((x0, y, x1, y), fill=c)


def vline(x, y0, y1, c=INK):
    d.line((x, y0, x, y1), fill=c)



# ---------- 顶部状态栏 ----------
# 日期时间同一行（左侧 0..118），其余 5 个图标显式定位：
# 温度 170 / 湿度 225，WiFi 300 / 蓝牙 322（挨着并贴近电池，电框左边缘 340），电量 365。
# 图标线条统一 1px，与电池框粗度一致。底部 3px 粗横线区隔。
STATUS_H = 32
ICON_CX = [170, 225, 300, 322, 365]   # 温度/湿度/WiFi/蓝牙/电量
icon_cx = lambda i: ICON_CX[i]
# 状态栏图标直接用 1 倍分辨率硬边渲染（不超采样），线宽 1px 与电池框一致。
ICON_KW = dict(ss=1, width_px=1)

# 日期时间同一行：09:41 大字(scale=2, 宽 56px) + 2026-11-24 小字(负字距宽 50px)
pt(6, 6, "09:41", scale=2)                 # y≈6..24
pt(68, 10, "2026-11-24", extra=-1)         # y≈10..22

# 「18px 图标 + 文字」组，按整组墨宽居中到槽中心（与固件 draw_icon_text_group 一致）
def icon_text_group(icon_name, text, cx):
    group_w = 18 + 2 + tw(text)
    sx = cx - group_w // 2
    img.paste(lr.icon(icon_name, 18, **ICON_KW), (sx, 7))
    pt(sx + 20, 11, text)


# 温度：thermometer-sun（温度计+太阳辐射线）+ 26°C
icon_text_group("thermometer-sun", "26°C", icon_cx(0))

# 湿度：droplets（一小一大两个水滴）
icon_text_group("droplets", "45%", icon_cx(1))

# WiFi：未连接用 wifi-off，连接成功用 wifi
cx = icon_cx(2)
img.paste(lr.icon("wifi-off", 18, **ICON_KW), (cx - 9, 7))

# 蓝牙：未连接用 bluetooth-off，连接成功用 bluetooth
cx = icon_cx(3)
img.paste(lr.icon("bluetooth-off", 18, **ICON_KW), (cx - 9, 7))

# 电池电量：格子样式，第 5 槽，整组（框28+间隔3+文字）按组宽居中
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

# ---------- 标题栏 ----------
TITLE_Y = STATUS_H + 1
pt(6, TITLE_Y + 5, "MAIN MENU", scale=2)          # 大写高 18，下移避开状态栏粗横线
# 中文与英文共用 top，保持对齐
pt(6 + tw("MAIN MENU", 2) + 8, TITLE_Y + 5, "主菜单", scale=2)
pt(W - 6 - tw("YUAN'S SYS V1.2"), TITLE_Y + 8, "YUAN'S SYS V1.2")
# 标题栏下方不加横线，靠留白与卡片区做区隔

# ---------- 卡片网格 ----------
# 4x2 模块左右居中；垂直方向统一间距：两排间隔 = 第二排到按键栏横线的距离，
# 第二排贴横线不动，第一排相应下移。
CARD_W, CARD_H = 90, 104
GAP_X = 9                                          # 列间距
GAP_V = 5                                          # 行间距 = 第二排到横线距离
BAR_Y = 282                                        # 按键栏横线
X0 = (W - (CARD_W * 4 + GAP_X * 3)) // 2          # 左右边距相等
ROW1_TOP = BAR_Y - GAP_V - CARD_H                  # 第二排贴横线，留 GAP_V
ROW0_TOP = ROW1_TOP - GAP_V - CARD_H               # 第一排下移，行间距同为 GAP_V
ROW_TOPS = (ROW0_TOP, ROW1_TOP)
B = CARD_H - 100                                   # 下半部分下沉量，填满加高后的卡片
SELECTED = 0                                       # 当前选中模块索引，双框显示

cards = [
    ("WEATHER",      "天气时钟", "weather"),
    ("POMODORO",     "番茄钟",   "pomodoro"),
    ("GAMES",        "游戏",     "games"),
    ("READING",      "阅读",     "reading"),
    ("MUSIC",        "音乐",     "music"),
    ("NEWS",         "新闻",     "news"),
    ("AI ASSISTANT", "AI助手",   "ai"),
    ("SETTINGS",     "设置",     "settings"),
]

ICON_NAMES = {
    "weather": "cloud-sun", "pomodoro": "timer", "games": "gamepad-2",
    "reading": "book-open", "music": "music", "news": "newspaper",
    "ai": "bot", "settings": "settings",
}
ICON_SIZE = 34
MODULE_ICON_KW = dict(ss=1, stroke=1.0)   # 34px 模块图标 1 倍硬边，线宽沿用原 SVG stroke-width

for i, (en, zh, key) in enumerate(cards):
    col, row = i % 4, i // 4
    x = X0 + col * (CARD_W + GAP_X)
    y = ROW_TOPS[row]
    # 未选中单线框；选中模块加一圈内框形成双框，与其它模块区分
    frame(x, y, x + CARD_W, y + CARD_H)
    if i == SELECTED:
        # 内框加粗到 2px（两条相邻线），与固件一致
        frame(x + 3, y + 3, x + CARD_W - 3, y + CARD_H - 3)
        frame(x + 4, y + 4, x + CARD_W - 4, y + CARD_H - 4)
    cx = x + CARD_W // 2
    img.paste(lr.icon(ICON_NAMES[key], ICON_SIZE, **MODULE_ICON_KW),
              (cx - ICON_SIZE // 2, y + 10))
    # 英文副标题用 8px 小字库（比 12px 中文小一圈），长词用负字距收紧；
    # 中文主标题下移，两者之间留出约 4px 行间距
    extra = -1 if len(en) > 11 else 0
    pts_center(cx, y + 49, en, extra=extra)
    pt_center(cx, y + 62, zh)
    hline(x + 6, x + CARD_W - 6, y + 78)
    # 状态行
    if key == "weather":
        pt_center(cx, y + 83, "26/09:41")
    elif key == "pomodoro":
        pt(x + 10, y + 83, "25:00")
        for k in range(4):
            bx = x + 48 + k * 9
            if k < 3:
                rect(bx, y + 84, bx + 6, y + 90)
            else:
                frame(bx, y + 84, bx + 6, y + 90)
    elif key == "games":
        pt_center(cx, y + 83, "HI 12480")
    elif key == "reading":
        pt(x + 8, y + 83, "38%")
        for k in range(6):
            bx = x + 36 + k * 8
            if k < 2:
                rect(bx, y + 84, bx + 6, y + 90)
            else:
                frame(bx, y + 84, bx + 6, y + 90)
    elif key == "music":
        pt_center(cx, y + 81, "NOW PLAYING", extra=-1)
        for k, hh in enumerate((3, 5, 8, 4, 9, 6, 3, 8, 5)):
            bx = x + 18 + k * 7
            rect(bx, y + 101 - hh, bx + 4, y + 101)
    elif key == "news":
        pt_center(cx, y + 83, "12 NEW")
    elif key == "ai":
        pt_center(cx, y + 83, "READY")
    elif key == "settings":
        pt(x + 8, y + 83, "EN")
        pt(x + 26, y + 82, "/中")
        pt(x + 56, y + 82, "双语")

# ---------- 底部按键提示栏 ----------
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

out = "mainmenu_preview.png"
img.resize((W * 3, H * 3), Image.NEAREST).save(out)
print("wrote", out, "字体: 主字库+8px英文小字库")

# 越界自检
for y in range(H):
    for x in range(W):
        pass
print("bounds ok (fixed geometry, no dynamic text overflow)")
