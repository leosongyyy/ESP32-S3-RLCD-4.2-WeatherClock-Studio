#!/usr/bin/env python3
"""用取模字库 fusion_pixel_font.py 在 1bit PIL 画布上渲染文本。

BDF 位图行 MSB 左对齐：第 col 列 = 行值 bit (row_bytes*8-1-col)。
y 基准：字形顶部落在 (baseline - ascent + yoff)，与 BDF 坐标一致。
"""
from fusion_pixel_font import GLYPHS, FONT_ASCENT, FONT_DESCENT, FONT_SIZE

INK, PAPER = 0, 255


def text_w(s, scale=1, extra=0):
    """取模字符串总步进宽度。"""
    adv = 0
    for ch in s:
        g = GLYPHS.get(ord(ch))
        if g is None:
            adv += 6 * scale + extra
            continue
        adv += g[4] * scale + extra
    return adv


def draw(img, x, baseline, s, scale=1, extra=0, c=INK, fallback_dot=True):
    """画取模文本，baseline 为西文基线（大写字母底部）。返回末端 x。"""
    px = img.load()
    W, H = img.size
    cx = x
    for ch in s:
        g = GLYPHS.get(ord(ch))
        if g is None:
            if fallback_dot and ch.strip():
                # 缺字画一个小方块提示，便于发现取模遗漏
                for yy in range(FONT_ASCENT * scale):
                    for xx in range(5 * scale):
                        if xx in (0, 5 * scale - 1) or yy in (0, FONT_ASCENT * scale - 1):
                            X, Y = cx + xx, baseline - FONT_ASCENT * scale + yy
                            if 0 <= X < W and 0 <= Y < H:
                                px[X, Y] = c
            cx += 6 * scale + extra
            continue
        w, h, xoff, yoff, dw, row_bytes, rows = g
        top = baseline - FONT_ASCENT * scale + yoff * scale
        for r, row in enumerate(rows):
            for col in range(w):
                if row & (1 << (row_bytes * 8 - 1 - col)):
                    for sy in range(scale):
                        for sx in range(scale):
                            X = cx + (xoff + col) * scale + sx
                            Y = top + r * scale + sy
                            if 0 <= X < W and 0 <= Y < H:
                                px[X, Y] = c
        cx += dw * scale + extra
    return cx


def draw_center(img, cx, baseline, s, scale=1, extra=0, c=INK):
    draw(img, cx - text_w(s, scale, extra) // 2, baseline, s, scale, extra, c)
