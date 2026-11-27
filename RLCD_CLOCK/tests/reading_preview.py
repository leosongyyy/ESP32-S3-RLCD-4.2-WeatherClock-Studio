#!/usr/bin/env python3
"""用卡上那套 12px 字库离线渲染阅读页，验证排版密度与字形。

和固件的排版口径一致：内容宽 320px、全角 12px、行距 13、
段距 6、每页最多 12 行。输出 400x300 的 PNG，可直接对照真机。

用法：
    python3 tests/reading_preview.py --font ../sd-assets/font/fusion_pixel_12.bin
"""
import argparse
import struct
import pathlib

from PIL import Image, ImageDraw

W, H = 400, 300
BOX_X0, BOX_Y0, BOX_X1, BOX_Y1 = 8, 58, 344, 254
BODY_TOP, BODY_LEFT = 84, 16
CONTENT_W = 320
PITCH, GAP, MAX_LINES = 13, 6, 12
INK = (0, 0, 0)
PAPER = (255, 255, 255)

TEXT = """《妖鬼奇缘录》
第一章 东仓使者
远处山风之巅，皎洁的月光如天网洒落下来，微风激起一层一层叠浪。近乍一看，一女遥望天际，身后传来急促的呼吸声。她的衣衫被风卷起，发丝在月色里泛着微光。
“你还是来了。”身后那人说道。
她没有回头，只是把手按在剑柄上，指节泛白。远处的灯火一盏一盏地灭了，最后只剩下城楼角上那一盏，在风里摇摇欲坠。
“这城里已经没有活人了。”她说。"""

NO_START = set("，。、；：！？）〕］｝〉》」』】…·”’")


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
        w, h, xoff, ytop, dw = e[6], e[7], e[8], struct.unpack("<b", e[9:10])[0], e[10]
        glyphs[cp] = (off, w, h, xoff, ytop, dw)
    blob_start = base + count * 11
    return glyphs, data[blob_start: blob_start + blob], px, line_h, ascent


def layout(text, glyphs):
    """按 12px 全角宽度断行，返回 [(行文本, 是否段尾)]。"""
    lines = []
    for para in text.split("\n"):
        if not para.strip():
            if lines and not lines[-1][1]:
                lines[-1] = (lines[-1][0], True)
            continue
        cur, width = "", 0
        for ch in para:
            w = 12
            if width + w > CONTENT_W:
                if ch in NO_START and width + w <= CONTENT_W + 12:
                    cur += ch
                    width += w
                    continue
                lines.append((cur, False))
                cur, width = "", 0
            cur += ch
            width += w
        if cur:
            lines.append((cur, True))
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default="../sd-assets/font/fusion_pixel_12.bin")
    ap.add_argument("-o", "--out", default="reading_preview_12px.png")
    args = ap.parse_args()

    here = pathlib.Path(__file__).resolve().parent
    font_path = (here / args.font) if not pathlib.Path(args.font).is_absolute() else pathlib.Path(args.font)
    glyphs, blob, px, line_h, ascent = load_font(font_path)

    img = Image.new("RGB", (W, H), PAPER)
    d = ImageDraw.Draw(img)

    def draw_text(x, y, s, bold=False):
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
                        img.putpixel((cx + xoff + c, y + ytop + r), INK)
                        if bold:
                            img.putpixel((cx + xoff + c + 1, y + ytop + r), INK)
            cx += dw + (1 if bold else 0)
        return cx

    def draw_mixed(x, y, s, bold=False):
        """中英混排：ASCII 走 PIL 默认字体，中文走卡上 12px 字库。"""
        cx = x
        for ch in s:
            if ord(ch) < 0x80:
                d.text((cx, y), ch, fill=INK)
                cx += 6
            else:
                cx = draw_text(cx, y, ch, bold)
        return cx

    # 状态栏
    d.rectangle([0, 0, W - 1, 31], fill=PAPER, outline=None)
    d.text((6, 8), "09:41", fill=INK)
    d.text((70, 12), "2026-11-27", fill=INK)
    d.line([0, 32, W - 1, 32], fill=INK)
    d.line([0, 34, W - 1, 34], fill=INK)

    # 阅读题头
    draw_text(10, 39, "阅读", bold=True)
    d.text((66, 41), "READING", fill=INK)
    d.text((300, 41), "PAGE 1 / 386", fill=INK)

    # 正文框
    d.rectangle([BOX_X0, BOX_Y0, BOX_X1, BOX_Y1], outline=INK)
    lines = layout(TEXT, glyphs)
    shown = lines[:MAX_LINES]
    y = BODY_TOP
    for text, gap in shown:
        draw_text(BODY_LEFT, y, text)
        y += PITCH
        if gap:
            y += GAP

    # 右栏三个按钮 + 进度条（书签 / 目录 / 进度百分比，和固件一致）
    for i in range(3):
        top = BOX_Y0 + i * 42
        d.rectangle([350, top, 383, top + 33], outline=INK)
    # 书签：方框加底部 V 形缺口
    bx, by = 358, BOX_Y0 + 9
    d.rectangle([bx, by, bx + 16, by + 19], outline=INK)
    d.polygon([(bx + 4, by + 14), (bx + 8, by + 18), (bx + 12, by + 14),
               (bx + 12, by + 19), (bx + 4, by + 19)], fill=PAPER)
    # 目录：三条横线加左侧竖线
    lx, ly = 355, BOX_Y0 + 50
    d.line([lx + 4, ly, lx + 4, ly + 14], fill=INK)
    for k in range(3):
        d.line([lx + 8, ly + k * 6, lx + 22, ly + k * 6], fill=INK)
    d.text((357, BOX_Y0 + 94), "%", fill=INK)
    d.rectangle([390, BOX_Y0, 395, BOX_Y1], outline=INK)
    d.rectangle([391, BOX_Y0 + 1, 394, BOX_Y0 + 6], fill=INK)

    # 底栏
    d.line([0, 262, W - 1, 262], fill=INK)
    draw_mixed(10, 268, "TIME 本次 0 min")
    draw_mixed(140, 268, "TOTAL 累计 0 min")
    d.text((270, 268), "LEFT 385 pages", fill=INK)
    d.line([0, 282, W - 1, 282], fill=INK)
    draw_mixed(10, 288, "A2 全屏   ST MARK 书签   SE TOC 目录   B BACK 返回")

    out = here / args.out
    img.save(out)
    body_h = y - BODY_TOP
    print(f"wrote {out} | 排版 {len(lines)} 行，首页 {len(shown)} 行，正文高 {body_h}px")


if __name__ == "__main__":
    main()
