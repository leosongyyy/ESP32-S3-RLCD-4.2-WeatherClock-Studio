#!/usr/bin/env python3
"""生成书库样例书的 1bit 封面点阵。

反射屏只有黑白，没有灰度也没有抗锯齿，所以封面不能放照片，要用
「图形剪影 + 高对比构图」在 50x64 里表达。本脚本直接按设备尺寸绘制，
输出：

  main/assets/book_covers.h       固件用的 packed 1bit 点阵
  tests/covers_preview.png        4 倍放大的contact sheet，供人工确认

运行：python3 RLCD_CLOCK/tools/gen_book_covers.py
"""

from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

CW, CH = 50, 64
INK, PAPER = 0, 255

ROOT = Path(__file__).resolve().parent.parent
OUT_H = ROOT / "main" / "assets" / "book_covers.h"
OUT_PNG = ROOT / "tests" / "covers_preview.png"


def new():
    img = Image.new("1", (CW, CH), PAPER)
    return img, ImageDraw.Draw(img)


def frame(d, inset=0):
    d.rectangle((inset, inset, CW - 1 - inset, CH - 1 - inset), outline=INK)


def skyline(d, base, heights, x0=3, x1=CW - 4):
    """一排高低矩形，做城市天际线/书架剪影。"""
    n = len(heights)
    step = (x1 - x0 + 1) / n
    for i, h in enumerate(heights):
        a = int(x0 + i * step)
        b = int(x0 + (i + 1) * step) - 1
        d.rectangle((a, base - h, b, base), fill=INK)


def mountains(d, base):
    d.polygon([(0, base), (10, base - 16), (20, base - 6), (30, base - 20),
               (40, base - 8), (CW - 1, base - 18), (CW - 1, base)], fill=INK)
    # 山峰留白，做出层次
    d.polygon([(10, base - 16), (14, base - 9), (6, base - 9)], fill=PAPER)


def star(d, x, y, r=2):
    d.line((x - r, y, x + r, y), fill=INK)
    d.line((x, y - r, x, y + r), fill=INK)


def cover_little_prince():
    img, d = new()
    d.rectangle((0, 0, CW - 1, CH - 1), fill=INK)
    d.rectangle((2, 2, CW - 3, CH - 3), fill=PAPER)
    for x, y in ((9, 10), (40, 8), (34, 19), (12, 22), (24, 7), (17, 15)):
        star(d, x, y, 1)
    star(d, 42, 28, 2)
    # 星球：半圆顶，坐在地平线上
    d.ellipse((6, 34, 44, 72), fill=INK)
    d.rectangle((0, 62, CW - 1, CH - 1), fill=INK)
    d.line((0, 61, CW - 1, 61), fill=INK)
    # 小人：圆头 + 身体 + 围巾 + 双腿
    d.ellipse((22, 24, 29, 31), fill=INK)
    d.polygon([(24, 32), (27, 32), (28, 45), (23, 45)], fill=INK)
    d.line((22, 34, 18, 37), fill=INK)
    d.line((29, 34, 33, 37), fill=INK)
    d.line((24, 45, 21, 53), fill=INK)
    d.line((27, 45, 30, 53), fill=INK)
    # 身后的小花
    d.line((35, 44, 35, 53), fill=INK)
    d.ellipse((33, 41, 37, 45), fill=PAPER)
    d.ellipse((33, 41, 37, 45), outline=INK)
    frame(d, 1)
    return img


def cover_1984():
    img, d = new()
    d.rectangle((0, 0, CW - 1, CH - 1), fill=INK)
    d.rectangle((2, 2, CW - 3, CH - 3), fill=PAPER)
    f = ImageFont.load_default()
    d.text((7, 7), "1984", font=f, fill=INK)
    # 一只眼睛
    d.ellipse((6, 24, 43, 46), fill=INK)
    d.ellipse((9, 27, 40, 43), fill=PAPER)
    d.ellipse((20, 29, 30, 41), fill=INK)
    d.ellipse((24, 33, 26, 36), fill=PAPER)
    d.line((8, 52, 41, 52), fill=INK)
    d.line((12, 56, 37, 56), fill=INK)
    frame(d, 1)
    return img


def cover_pride():
    img, d = new()
    frame(d)
    # 顶部装饰带 + 菱形花纹
    d.rectangle((3, 3, CW - 4, 12), fill=INK)
    for x in range(6, CW - 6, 6):
        d.polygon([(x, 6), (x + 2, 8), (x, 10), (x - 2, 8)], fill=PAPER)
    # 两个人像剪影：头 + 肩
    d.ellipse((10, 18, 22, 30), fill=INK)
    d.polygon([(6, 52), (8, 36), (14, 32), (18, 32), (24, 36), (26, 52)], fill=INK)
    d.ellipse((28, 22, 40, 34), fill=INK)
    d.polygon([(24, 52), (26, 40), (32, 36), (36, 36), (42, 40), (44, 52)], fill=INK)
    d.rectangle((3, 52, CW - 4, 53), fill=INK)
    # 底部书名占位线
    d.rectangle((14, 57, CW - 15, 59), fill=INK)
    return img


def cover_hobbit():
    img, d = new()
    d.rectangle((0, 0, CW - 1, CH - 1), fill=INK)
    d.rectangle((2, 2, CW - 3, CH - 3), fill=PAPER)
    # 月亮
    d.ellipse((35, 6, 44, 15), fill=INK)
    d.ellipse((37, 4, 46, 13), fill=PAPER)
    # 三层山脊，用留白分开
    d.polygon([(2, 46), (12, 22), (22, 46)], fill=INK)
    d.polygon([(16, 46), (28, 16), (40, 46)], fill=INK)
    d.polygon([(34, 46), (44, 26), (47, 46)], fill=INK)
    d.polygon([(28, 16), (33, 30), (23, 30)], fill=PAPER)
    d.polygon([(12, 22), (16, 33), (8, 33)], fill=PAPER)
    # 地面与圆门
    d.rectangle((2, 46, CW - 3, CH - 3), fill=INK)
    d.ellipse((19, 42, 31, 54), fill=PAPER)
    d.ellipse((21, 44, 29, 52), outline=INK)
    d.line((25, 45, 25, 51), fill=INK)
    d.rectangle((22, 55, 28, 56), fill=PAPER)
    frame(d, 1)
    return img


def cover_harry():
    img, d = new()
    d.rectangle((0, 0, CW - 1, CH - 1), fill=INK)
    d.rectangle((2, 2, CW - 3, CH - 3), fill=PAPER)
    # 城堡尖顶
    for cx, top in ((10, 12), (20, 7), (30, 14), (40, 9)):
        d.polygon([(cx - 4, 44), (cx, top), (cx + 4, 44)], fill=INK)
        d.rectangle((cx - 4, 30, cx + 4, 44), fill=INK)
    d.rectangle((4, 44, CW - 5, 50), fill=INK)
    # 拱门
    d.pieslice((17, 46, 33, 62), 180, 360, fill=PAPER)
    d.rectangle((17, 54, 32, 61), fill=PAPER)
    d.line((0, 52, CW - 1, 52), fill=INK)
    frame(d, 1)
    return img


def cover_gatsby():
    img, d = new()
    d.rectangle((0, 0, CW - 1, CH - 1), fill=INK)
    d.rectangle((3, 3, CW - 4, CH - 4), fill=PAPER)
    frame(d)
    # Art-Deco 放射线
    cx, cy = CW // 2, 30
    for k in range(-4, 5):
        d.line((cx, cy, cx + k * 8, 4), fill=INK)
    d.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), fill=PAPER)
    d.ellipse((cx - 6, cy - 6, cx + 6, cy + 6), outline=INK)
    d.line((6, cy, CW - 7, cy), fill=INK)
    skyline(d, CH - 5, [10, 6, 14, 8, 12, 5])
    d.rectangle((0, CH - 5, CW - 1, CH - 1), fill=INK)
    return img


COVERS = [
    ("little_prince", cover_little_prince),
    ("nineteen84", cover_1984),
    ("pride", cover_pride),
    ("hobbit", cover_hobbit),
    ("harry", cover_harry),
    ("gatsby", cover_gatsby),
]


def pack(img):
    """把 1bit 图打包成按行字节对齐的点阵，MSB 在左。"""
    px = img.load()
    stride = (CW + 7) // 8
    out = bytearray()
    for y in range(CH):
        for b in range(stride):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                if x < CW and px[x, y] == INK:
                    byte |= 1 << (7 - bit)
            out.append(byte)
    return bytes(out)


def main():
    stride = (CW + 7) // 8
    blocks = []
    sheet = Image.new("1", (CW * 6 + 7 * 5, CH), PAPER)
    for i, (name, fn) in enumerate(COVERS):
        img = fn()
        assert img.size == (CW, CH), (name, img.size)
        sheet.paste(img, (i * (CW + 7), 0))
        data = pack(img)
        assert len(data) == stride * CH
        lines = []
        for y in range(CH):
            row = data[y * stride:(y + 1) * stride]
            lines.append("    " + ", ".join(f"0x{b:02X}" for b in row) + ",")
        blocks.append((name, "\n".join(lines)))

    header = [
        "// 书库样例书的 1bit 封面点阵，由 tools/gen_book_covers.py 生成，勿手工编辑。",
        f"// 每张 {CW}x{CH} 像素，按行字节对齐（stride={(CW + 7) // 8}），MSB 在左，1 为墨。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    const char *id;",
        "    int w;",
        "    int h;",
        "    int stride;",
        "    const uint8_t *bits;",
        "} book_cover_t;",
        "",
    ]
    for name, body in blocks:
        header.append(f"static const uint8_t book_cover_{name}_bits[] = {{")
        header.append(body)
        header.append("};")
        header.append("")
    header.append("static const book_cover_t kBookCovers[] = {")
    for name, _ in blocks:
        header.append(f'    {{"{name}", {CW}, {CH}, {(CW + 7) // 8}, book_cover_{name}_bits}},')
    header.append("};")
    header.append("")
    header.append("static const int kBookCoverCount = "
                  "static_cast<int>(sizeof(kBookCovers) / sizeof(kBookCovers[0]));")
    header.append("")

    OUT_H.write_text("\n".join(header), encoding="utf-8")

    zoom = sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST)
    zoom.save(OUT_PNG)
    print(f"wrote {OUT_H} ({len(blocks)} covers)")
    print(f"wrote {OUT_PNG} ({zoom.width}x{zoom.height})")


if __name__ == "__main__":
    main()
