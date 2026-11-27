#!/usr/bin/env python3
"""生成放在 TF 卡上的 12px 中文字库（Fusion Pixel，与主界面同款字形）。

为什么不做成 C 数组：全 CJK 12px 取模约 700KB，app 分区放不下，
所以正文的中文字形放 TF 卡 /sdcard/font/fusion_pixel_12.bin，运行时读进 PSRAM。
ASCII（0x20-0x7E）不写进这个文件，固件继续用内置的 fusion_pixel_12 取模字库，
这样中英文都与主菜单的模块名严格同一套字形。

文件格式（小端）：
    magic  4s   "FPX1"
    count  u16  字形数
    px     u16  字号（12）
    line_h u16  行高
    ascent u16  基线到行顶
    blob   u32  点阵字节数
    index  count × 11 字节：
        cp   u16
        off  u32   blob 内偏移
        w,h,xoff,ytop,dw  u8 ×5（ytop 是相对行顶的偏移，可为负，按 int8 读）
    blob   count 段按行打包的 1bpp 点阵

用法：
    python3 tools/gen_sd_font.py --ttf /path/to/fusion-pixel-12px.ttf \
        -o /path/to/fusion_pixel_12.bin
"""
import argparse
import pathlib
import struct

from PIL import Image, ImageDraw, ImageFont

PX = 12
LINE_H = 11
ASCENT = 9
RANGES = [(0x00A0, 0x00FF), (0x2000, 0x206F), (0x3000, 0x303F),
          (0x4E00, 0x9FFF), (0xFF00, 0xFFEF)]
# PIL 的 anchor="ls" 基线比 BDF 取模低 1px，统一上提，保证与主菜单字库对齐。
Y_SHIFT = 1


def render(font: ImageFont.FreeTypeFont, ch: str):
    """把单字渲染成 1bpp 点阵，返回 (w, h, xoff, ytop, dw, rows_bytes)。"""
    canvas = Image.new("L", (48, 48), 0)
    baseline = 24
    ImageDraw.Draw(canvas).text((8, baseline), ch, font=font, fill=255, anchor="ls")
    px = canvas.load()
    pts = [(x, y) for y in range(48) for x in range(48) if px[x, y] > 127]
    if not pts:
        return None
    x0 = min(p[0] for p in pts)
    x1 = max(p[0] for p in pts)
    y0 = min(p[1] for p in pts)
    y1 = max(p[1] for p in pts)
    w = x1 - x0 + 1
    h = y1 - y0 + 1
    xoff = x0 - 8
    ytop = y0 - baseline + ASCENT - Y_SHIFT
    dw = int(round(font.getlength(ch)))
    if dw <= 0:
        dw = PX
    row_bytes = (w + 7) // 8
    out = bytearray()
    for r in range(h):
        acc = 0
        for c in range(w):
            if px[x0 + c, y0 + r] > 127:
                acc |= 1 << (7 - (c % 8))
            if c % 8 == 7:
                out.append(acc)
                acc = 0
        if w % 8:
            out.append(acc)
    assert len(out) == row_bytes * h
    return w, h, xoff, ytop, dw, row_bytes, bytes(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True)
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--px", type=int, default=PX)
    args = ap.parse_args()

    font = ImageFont.truetype(args.ttf, args.px)
    entries = []
    blob = bytearray()
    for lo, hi in RANGES:
        for cp in range(lo, hi + 1):
            if 0xD800 <= cp <= 0xDFFF:
                continue
            try:
                ch = chr(cp)
            except ValueError:
                continue
            r = render(font, ch)
            if r is None:
                continue
            w, h, xoff, ytop, dw, row_bytes, data = r
            if len(blob) > 0xFFFFFFFF:
                raise SystemExit("blob too large")
            entries.append((cp, len(blob), w, h, xoff & 0xFF, ytop & 0xFF, dw & 0xFF))
            blob += data

    entries.sort(key=lambda e: e[0])
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        f.write(b"FPX1")
        f.write(struct.pack("<HHHHI", len(entries), args.px, LINE_H, ASCENT, len(blob)))
        for cp, off, w, h, xoff, ytop, dw in entries:
            f.write(struct.pack("<HIBBBBB", cp, off, w, h, xoff, ytop, dw))
        f.write(blob)
    print(f"wrote {out} glyphs={len(entries)} blob={len(blob)} total={out.stat().st_size}")


if __name__ == "__main__":
    main()
