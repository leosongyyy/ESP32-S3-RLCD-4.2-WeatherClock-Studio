#!/usr/bin/env python3
"""用取模字库在 1bit PIL 画布上渲染文本。

默认主字库 fusion_pixel_font.py（12px，含中文），另备小字库
fusion_pixel_8_font.py（8px，仅 ASCII）用于卡片里的英文副标题。
BDF 位图行 MSB 左对齐：第 col 列 = 行值 bit (row_bytes*8-1-col)。
y 基准：字形顶部落在 (baseline - ascent + yoff)，与 BDF 坐标一致。
"""
import fusion_pixel_8_font as small
import fusion_pixel_font as main
from fusion_pixel_font import FONT_ASCENT, FONT_DESCENT, FONT_SIZE

INK, PAPER = 0, 255
SMALL_ASCENT = small.FONT_ASCENT   # 8px 小字的大写墨迹高，供按顶部对齐用


def _text_w(glyphs, s, scale, extra):
    adv = 0
    for ch in s:
        g = glyphs.get(ord(ch))
        adv += (g[4] if g else 6) * scale + extra
    return adv


def _draw(img, glyphs, ascent, x, baseline, s, scale, extra, c, fallback_dot):
    px = img.load()
    W, H = img.size
    cx = x
    for ch in s:
        g = glyphs.get(ord(ch))
        if g is None:
            if fallback_dot and ch.strip():
                for yy in range(ascent * scale):
                    for xx in range(5 * scale):
                        if xx in (0, 5 * scale - 1) or yy in (0, ascent * scale - 1):
                            X, Y = cx + xx, baseline - ascent * scale + yy
                            if 0 <= X < W and 0 <= Y < H:
                                px[X, Y] = c
            cx += 6 * scale + extra
            continue
        w, h, xoff, yoff, dw, row_bytes, rows = g
        # BDF yoff 是字形底边相对基线的偏移(向上为正)，顶边 = yoff + h
        top = baseline - (yoff + h) * scale
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


# 主字库（12px，含中文）
def text_w(s, scale=1, extra=0):
    return _text_w(main.GLYPHS, s, scale, extra)


def draw(img, x, baseline, s, scale=1, extra=0, c=INK, fallback_dot=True):
    return _draw(img, main.GLYPHS, main.FONT_ASCENT, x, baseline, s,
                 scale, extra, c, fallback_dot)


def draw_center(img, cx, baseline, s, scale=1, extra=0, c=INK):
    draw(img, cx - text_w(s, scale, extra) // 2, baseline, s, scale, extra, c)


# 小字库（8px，仅 ASCII），用于卡片英文副标题
def small_text_w(s, scale=1, extra=0):
    return _text_w(small.GLYPHS, s, scale, extra)


def small_draw(img, x, baseline, s, scale=1, extra=0, c=INK):
    return _draw(img, small.GLYPHS, small.FONT_ASCENT, x, baseline, s,
                 scale, extra, c, False)


def small_draw_center(img, cx, baseline, s, scale=1, extra=0, c=INK):
    small_draw(img, cx - small_text_w(s, scale, extra) // 2,
               baseline, s, scale, extra, c)
