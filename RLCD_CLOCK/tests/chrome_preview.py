#!/usr/bin/env python3
"""离线预览模拟器两侧的 Game Boy 机身装饰栏。

固件里那段代码跑在 ESP32-S3 上、写的是 RLCD 打包帧缓冲，改一次要编译 + 烧录
才能看到。这个脚本把同一套坐标和字库在 PIL 上重画一遍，直接出 PNG：
  python3 RLCD_CLOCK/tests/chrome_preview.py

⚠ 它是**镜像**不是复用：改了 gameboy_chrome.cpp 的版式，这里要同步改，
  否则预览会骗人。用途是看版式（有没有出界、有没有撞车），不是验证像素。
"""

from PIL import Image, ImageDraw

W, H = 400, 300
BAR = 40
WIN_X = 40
WIN_W = 320
LEFT_X0 = 0
RIGHT_X0 = WIN_X + WIN_W  # 360

FONT = {
    '0': (0x0E,0x11,0x13,0x15,0x19,0x11,0x0E),
    '1': (0x04,0x0C,0x04,0x04,0x04,0x04,0x0E),
    '2': (0x0E,0x11,0x01,0x02,0x04,0x08,0x1F),
    '3': (0x1F,0x02,0x04,0x02,0x01,0x11,0x0E),
    '4': (0x02,0x06,0x0A,0x12,0x1F,0x02,0x02),
    '5': (0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E),
    '6': (0x06,0x08,0x10,0x1E,0x11,0x11,0x0E),
    '7': (0x1F,0x01,0x02,0x04,0x08,0x08,0x08),
    '8': (0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E),
    '9': (0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C),
    'A': (0x0E,0x11,0x11,0x1F,0x11,0x11,0x11),
    'B': (0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E),
    'C': (0x0E,0x11,0x10,0x10,0x10,0x11,0x0E),
    'D': (0x1E,0x11,0x11,0x11,0x11,0x11,0x1E),
    'E': (0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F),
    'F': (0x1F,0x10,0x10,0x1E,0x10,0x10,0x10),
    'G': (0x0E,0x11,0x10,0x17,0x11,0x11,0x0E),
    'H': (0x11,0x11,0x11,0x1F,0x11,0x11,0x11),
    'I': (0x1F,0x04,0x04,0x04,0x04,0x04,0x1F),
    'J': (0x07,0x02,0x02,0x02,0x02,0x12,0x0C),
    'K': (0x11,0x12,0x14,0x18,0x14,0x12,0x11),
    'L': (0x10,0x10,0x10,0x10,0x10,0x10,0x1F),
    'M': (0x11,0x1B,0x15,0x15,0x11,0x11,0x11),
    'N': (0x11,0x19,0x15,0x13,0x11,0x11,0x11),
    'O': (0x0E,0x11,0x11,0x11,0x11,0x11,0x0E),
    'P': (0x1E,0x11,0x11,0x1E,0x10,0x10,0x10),
    'Q': (0x0E,0x11,0x11,0x11,0x15,0x13,0x0F),
    'R': (0x1E,0x11,0x11,0x1E,0x14,0x12,0x11),
    'S': (0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E),
    'T': (0x1F,0x04,0x04,0x04,0x04,0x04,0x04),
    'U': (0x11,0x11,0x11,0x11,0x11,0x11,0x0E),
    'V': (0x11,0x11,0x11,0x11,0x11,0x0A,0x04),
    'W': (0x11,0x11,0x11,0x15,0x15,0x1B,0x11),
    'X': (0x11,0x11,0x0A,0x04,0x0A,0x11,0x11),
    'Y': (0x11,0x11,0x0A,0x04,0x04,0x04,0x04),
    'Z': (0x1F,0x01,0x02,0x04,0x08,0x10,0x1F),
    ':': (0x00,0x04,0x04,0x00,0x04,0x04,0x00),
    '-': (0x00,0x00,0x00,0x0E,0x00,0x00,0x00),
    '/': (0x01,0x02,0x02,0x04,0x08,0x08,0x10),
    '%': (0x19,0x1A,0x02,0x04,0x08,0x0B,0x13),
    '.': (0x00,0x00,0x00,0x00,0x00,0x0C,0x0C),
    '!': (0x04,0x04,0x04,0x04,0x04,0x00,0x04),
    ' ': (0x00,0x00,0x00,0x00,0x00,0x00,0x00),
}
GW, GH, PITCH = 5, 7, 6
WEEKDAY = ("SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT")


class Canvas:
    def __init__(self):
        self.img = Image.new("1", (W, H), 0)      # 0 = 黑，1 = 白
        self.d = ImageDraw.Draw(self.img)

    def box(self, x0, y0, x1, y1, white=True):
        if x0 > x1 or y0 > y1:
            return
        self.d.rectangle([x0, y0, x1, y1], fill=1 if white else 0)

    def px(self, x, y, white=True):
        if 0 <= x < W and 0 <= y < H:
            self.d.point((x, y), fill=1 if white else 0)

    def disc(self, cx, cy, r, white=True):
        self.d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=1 if white else 0)

    def ring(self, cx, cy, r, t, white=True):
        self.disc(cx, cy, r, True)
        self.disc(cx, cy, r - t, False)

    def line(self, x0, y0, x1, y1, white=True):
        self.d.line([x0, y0, x1, y1], fill=1 if white else 0)

    def text(self, x, y, s, scale=1):
        cx = x
        for ch in s.upper():
            g = FONT.get(ch)
            if g:
                for r, bits in enumerate(g):
                    for c in range(GW):
                        if bits & (1 << (4 - c)):
                            if scale == 1:
                                self.px(cx + c, y + r)
                            else:
                                self.box(cx + c * scale, y + r * scale,
                                         cx + c * scale + scale - 1,
                                         y + r * scale + scale - 1)
            cx += PITCH * scale

    def text_row(self, bar_x0, y, s, scale=1):
        n = len(s)
        w = scale * ((n - 1) * PITCH + GW) if n else 0
        if w > BAR - 4 and scale > 1:
            scale = 1
            w = (n - 1) * PITCH + GW if n else 0
        x = bar_x0 + (BAR - w) // 2
        if x < bar_x0 + 2:
            x = bar_x0 + 2
        self.text(x, y, s, scale)


def battery_icon(cv, x0, y0, pct):
    b, t = x0 + 8, x0 + 30
    y1 = y0 + 12
    cv.box(b, y0, t, y1)
    cv.box(t + 1, y0 + 4, t + 4, y1 - 4)
    cv.box(b + 2, y0 + 2, t - 2, y1 - 2, False)
    level = 0 if pct <= 0 else min(5, (pct + 19) // 20)
    for i in range(level):
        sx = b + 2 + i * 4
        cv.box(sx, y0 + 3, sx + 2, y1 - 3)


def dpad(cv, x0, y0):
    cx = x0 + 20
    cv.box(x0 + 4, y0 + 10, x0 + 35, y0 + 22)
    cv.box(cx - 6, y0, cx + 6, y0 + 32)
    cv.box(x0 + 6, y0 + 12, x0 + 33, y0 + 20, False)
    cv.box(cx - 4, y0 + 2, cx + 4, y0 + 30, False)
    cv.box(cx - 2, y0 + 14, cx + 1, y0 + 17)


def ab_buttons(cv, x0, y0):
    bx, by = x0 + 13, y0 + 18
    ax, ay = x0 + 29, y0 + 6
    cv.ring(bx, by, 7, 2)
    cv.ring(ax, ay, 7, 2)
    cv.text(bx - 2, by - 3, "B")
    cv.text(ax - 2, ay - 3, "A")


def pad_icon(cv, x0, y0, connected):
    a, b = x0 + 7, x0 + 32
    y1 = y0 + 13
    cv.box(a, y0, b, y1)
    cv.box(a + 2, y0 + 2, b - 2, y1 - 2, False)
    cv.box(a + 4, y0 + 5, a + 10, y0 + 7)
    cv.box(a + 6, y0 + 3, a + 8, y0 + 9)
    cv.box(b - 12, y0 + 4, b - 9, y0 + 7)
    cv.box(b - 7, y0 + 6, b - 4, y0 + 9)
    if not connected:
        cv.line(a + 1, y1 - 1, b - 1, y0 + 1)


def speaker_grille(cv, x0, y_top, y_bottom):
    for i in range(6):
        sx = x0 + 6 + i * 4
        cv.line(sx, y_bottom, sx + 8, y_top)


def volume_bars(cv, x0, y0, step):
    for i in range(5):
        sx = x0 + 5 + i * 6
        cv.box(sx, y0, sx + 4, y0 + 8, i < step)
        if i >= step:
            cv.box(sx, y0, sx + 4, y0)
            cv.box(sx, y0 + 8, sx + 4, y0 + 8)
            cv.box(sx, y0, sx, y0 + 8)
            cv.box(sx + 4, y0, sx + 4, y0 + 8)


def dot_field(cv, x0, y0, y1):
    for y in range(y0, y1, 8):
        for x in range(x0 + 6, x0 + 34, 6):
            cv.box(x, y, x + 1, y + 1)


def draw_left(cv, battery, month, day, wday):
    x0 = LEFT_X0
    battery_icon(cv, x0, 8, battery)
    cv.text_row(x0, 26, "100" if battery >= 100 else "%d%%" % battery, 2)
    dpad(cv, x0, 42)
    cv.text_row(x0, 88, WEEKDAY[wday])
    cv.text_row(x0, 98, "%02d/%02d" % (month, day))
    cv.text_row(x0, 126, "GAME")
    cv.text_row(x0, 136, "BOY")
    dot_field(cv, x0, 162, 286)


def draw_right(cv, hour, minute, connected, vol):
    x0 = RIGHT_X0
    cv.text_row(x0, 8, "%02d" % hour, 2)
    cv.text_row(x0, 26, "%02d" % minute, 2)
    cv.box(x0 + 12, 23, x0 + 27, 23)
    ab_buttons(cv, x0, 40)
    pad_icon(cv, x0, 76, connected)
    cv.text_row(x0, 94, "PAD")
    cv.text_row(x0, 104, "ON" if connected else "OFF")
    speaker_grille(cv, x0, 132, 168)
    cv.text_row(x0, 186, "VOL")
    volume_bars(cv, x0, 196, vol)
    dot_field(cv, x0, 220, 286)


def game_area_placeholder(cv):
    """中间 GB 区域画个 2x2 抖动的中灰，示意游戏画面。"""
    for y in range(0, 288):
        for x in range(WIN_X, WIN_X + WIN_W):
            on = ((x // 2) + (y // 2)) % 4 < 2
            if on:
                cv.px(x, y)


def main():
    cv = Canvas()
    game_area_placeholder(cv)
    draw_left(cv, battery=78, month=10, day=25, wday=6)
    draw_right(cv, hour=13, minute=49, connected=True, vol=3)

    # 存到脚本自己所在的目录，别污染仓库根目录（也别依赖调用时的 cwd）。
    import os
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "chrome_preview.png")
    cv.img.resize((W * 3, H * 3), Image.NEAREST).save(out)
    print("wrote", out)

    # 自检：单独画一遍侧栏（不带中间画面），任何一笔落到 GB 窗口里就报错。
    # 侧栏只有 x 0..39 与 360..399，越界就会压在游戏画面上。
    chk = Canvas()
    draw_left(chk, battery=100, month=12, day=31, wday=0)
    draw_right(chk, hour=23, minute=59, connected=False, vol=5)
    px = chk.img.load()
    bad = [(x, y) for y in range(H) for x in range(BAR, RIGHT_X0) if px[x, y]]
    if bad:
        raise SystemExit("out of bounds pixels: %s" % bad[:8])
    print("bounds ok (no chrome pixel inside the GB window)")


if __name__ == "__main__":
    main()
