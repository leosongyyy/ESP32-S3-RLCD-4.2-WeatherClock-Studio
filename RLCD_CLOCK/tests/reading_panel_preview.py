#!/usr/bin/env python3
"""离线渲染阅读页的目录/书签弹层，校验版式（行高、截断、反白、滚动条）。

几何常量与 ui_reading.cpp 里的 PANEL_* 保持一致，字库用卡上那套 12px。
用法：python3 tests/reading_panel_preview.py --font ../sd-assets/font/fusion_pixel_12.bin
"""
import argparse
import pathlib
import struct

from PIL import Image, ImageDraw

W, H = 400, 300
SCALE = 2

# 与固件一致
PANEL_X0, PANEL_Y0, PANEL_X1, PANEL_Y1 = 14, 62, 344, 250
ROW_TOP = PANEL_Y0 + 34
ROW_PITCH = 15
ROWS = 8
TAB_Y = PANEL_Y0 + 8
BOX_X0, BOX_Y0, BOX_X1, BOX_Y1 = 8, 58, 344, 254
RAIL_X, RAIL_W = 350, 34
BAR_X, BAR_W = 390, 6
BAR_Y = 282
BAR_TEXT_TOP = 288

INK = (0, 0, 0)
PAPER = (255, 255, 255)

TOC = [
    ("第一章 东仓使者", 1),
    ("第二章 山中旧事", 9),
    ("第三章 夜半歌声", 17),
    ("第四章 月下招魂", 25),
    ("第五章 城里没有活人了", 33),
    ("第六章 剑出鞘", 41),
    ("第七章 她终于回头", 49),
    ("第八章 风起于青萍之末", 57),
    ("第九章 长夜将尽", 65),
]


def load_font(path):
    data = pathlib.Path(path).read_bytes()
    if data[:4] != b"FPX1":
        raise SystemExit("bad magic")
    count, px, line_h, ascent, blob = struct.unpack("<HHHHI", data[4:16])
    glyphs = {}
    base = 16
    for i in range(count):
        e = data[base + i * 11: base + (i + 1) * 11]
        cp, off = struct.unpack("<HI", e[:6])
        w, h, xoff = e[6], e[7], e[8]
        ytop = struct.unpack("<b", e[9:10])[0]
        dw = e[10]
        glyphs[cp] = (off, w, h, xoff, ytop, dw)
    blob_start = base + count * 11
    return glyphs, data[blob_start: blob_start + blob], px, ascent


def advance(s, glyphs, px):
    return sum(glyphs[ord(c)][5] if ord(c) in glyphs else px for c in s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default="../sd-assets/font/fusion_pixel_12.bin")
    ap.add_argument("-o", "--out", default="reading_panel_preview.png")
    args = ap.parse_args()
    here = pathlib.Path(__file__).resolve().parent
    fp = pathlib.Path(args.font)
    if not fp.is_absolute():
        fp = here / fp
    glyphs, blob, px, ascent = load_font(fp)

    img = Image.new("RGB", (W, H), PAPER)
    d = ImageDraw.Draw(img)

    def draw_text(x, y, s, bold=False, ink=INK):
        cx = x
        for ch in s:
            g = glyphs.get(ord(ch))
            if not g:
                cx += px
                continue
            off, w, h, xoff, ytop, dw = g
            row_bytes = (w + 7) // 8
            for r in range(h):
                for c in range(w):
                    if blob[off + r * row_bytes + c // 8] & (1 << (7 - (c % 8))):
                        img.putpixel((cx + xoff + c, y + ytop + r), ink)
                        if bold:
                            img.putpixel((cx + xoff + c + 1, y + ytop + r), ink)
            cx += dw + (1 if bold else 0)
        return cx

    def clipped(x, y, s, max_w, ink=INK):
        if advance(s, glyphs, px) <= max_w:
            draw_text(x, y, s, ink=ink)
            return
        cut = s
        while cut and advance(cut, glyphs, px) + 12 > max_w:
            cut = cut[:-1]
        draw_text(x, y, cut + "...", ink=ink)

    # 状态栏（简化）
    d.text((6, 8), "00:12", fill=INK)
    d.text((72, 12), "2026-11-28", fill=INK)
    d.line([0, 32, W - 1, 32], fill=INK)
    d.line([0, 34, W - 1, 34], fill=INK)

    # 阅读题头
    draw_text(10, 40, "阅读", bold=True)
    d.text((50, 42), "READING", fill=INK)
    d.text((292, 42), "PAGE 1 / 3166", fill=INK)

    # 正文框 + 占位正文
    d.rectangle([BOX_X0, BOX_Y0, BOX_X1, BOX_Y1], outline=INK)
    draw_text(16, 84, "远处山风之巅，皎洁的月光如天网洒落下来，")
    draw_text(16, 97, "微风激起一层一层叠浪。近乍一看，一女遥望天际。")

    # 弹层
    d.rectangle([PANEL_X0, PANEL_Y0, PANEL_X1, PANEL_Y1], fill=PAPER, outline=INK)
    d.rectangle([PANEL_X0 + 1, PANEL_Y0 + 1, PANEL_X1 - 1, PANEL_Y1 - 1], outline=INK)
    for t, label in enumerate(["目录", "书签"]):
        x = PANEL_X0 + 18 + t * 78
        on = t == 0
        draw_text(x, TAB_Y, label, bold=on)
        if on:
            d.rectangle([x, TAB_Y + 13, x + advance(label, glyphs, px) - 1, TAB_Y + 14], fill=INK)
    d.line([PANEL_X0 + 8, PANEL_Y0 + 26, PANEL_X1 - 8, PANEL_Y0 + 26], fill=INK)
    notice = "已加书签 P.4"
    draw_text(PANEL_X1 - 14 - advance(notice, glyphs, px), TAB_Y, notice)

    sel, scroll = 3, 0
    for row in range(ROWS):
        i = scroll + row
        if i >= len(TOC):
            break
        y = ROW_TOP + row * ROW_PITCH
        on = i == sel
        if on:
            d.rectangle([PANEL_X0 + 6, y - 2, PANEL_X1 - 12, y + 11], fill=INK)
        title, page = TOC[i]
        clipped(PANEL_X0 + 12, y, title, PANEL_X1 - PANEL_X0 - 74, PAPER if on else INK)
        pbuf = "P.%d" % page
        d.text((PANEL_X1 - 12 - advance(pbuf, glyphs, px), y + 1), pbuf,
               fill=PAPER if on else INK)

    # 滚动条
    bar_x = PANEL_X1 - 6
    top = ROW_TOP - 2
    bottom = top + ROWS * ROW_PITCH - 4
    d.line([bar_x, top, bar_x, bottom], fill=INK)
    span = bottom - top
    thumb = span * ROWS // len(TOC)
    offset = span * scroll // max(1, len(TOC) - ROWS)
    d.rectangle([bar_x - 1, top + offset, bar_x + 1, top + offset + max(4, thumb)], fill=INK)

    d.text((PANEL_X0 + 12, PANEL_Y1 - 15), "A JUMP  B CLOSE  <> TAB  HOLD MARK", fill=INK)
    hint = "%d/%d" % (sel + 1, len(TOC))
    d.text((PANEL_X1 - 10 - advance(hint, glyphs, px), PANEL_Y1 - 15), hint, fill=INK)

    # 右栏三个按钮（书签已填充）
    for i in range(3):
        t = BOX_Y0 + i * 42
        d.rectangle([RAIL_X, t, RAIL_X + RAIL_W - 1, t + RAIL_W - 1], outline=INK)
    d.rectangle([RAIL_X + 1, BOX_Y0 + 43, RAIL_X + RAIL_W - 2, BOX_Y0 + 42 + RAIL_W - 2], outline=INK)
    d.rectangle([BAR_X, BOX_Y0, BAR_X + BAR_W - 1, BOX_Y1], outline=INK)
    d.rectangle([BAR_X + 1, BOX_Y0 + 1, BAR_X + BAR_W - 2, BOX_Y0 + 3], fill=INK)

    # 底栏
    d.line([0, 262, W - 1, 262], fill=INK)
    d.text((10, 268), "TIME 本次 0 min", fill=INK)
    d.line([0, BAR_Y, W - 1, BAR_Y], fill=INK)
    d.rectangle([38, BAR_Y + 4, 54, BAR_Y + 15], outline=INK)
    d.text((41, BAR_Y + 6), "A2", fill=INK)
    draw_text(60, BAR_TEXT_TOP, "全屏")
    d.line([92, BAR_Y + 3, 92, H - 3], fill=INK)
    d.rectangle([98, BAR_Y + 4, 114, BAR_Y + 15], outline=INK)
    d.text((101, BAR_Y + 6), "ST", fill=INK)
    d.text((120, BAR_TEXT_TOP), "MARK", fill=INK)
    draw_text(150, BAR_TEXT_TOP, "书签")
    d.line([178, BAR_Y + 3, 178, H - 3], fill=INK)
    d.rectangle([184, BAR_Y + 4, 200, BAR_Y + 15], outline=INK)
    d.text((187, BAR_Y + 6), "SE", fill=INK)
    d.text((206, BAR_TEXT_TOP), "TOC", fill=INK)
    draw_text(232, BAR_TEXT_TOP, "目录")
    d.line([258, BAR_Y + 3, 258, H - 3], fill=INK)
    d.rectangle([264, BAR_Y + 4, 278, BAR_Y + 15], outline=INK)
    d.text((269, BAR_Y + 6), "B", fill=INK)
    d.text((283, BAR_TEXT_TOP), "BACK", fill=INK)
    draw_text(311, BAR_TEXT_TOP, "返回")

    out = here / args.out
    img.resize((W * SCALE, H * SCALE), Image.NEAREST).save(out)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
