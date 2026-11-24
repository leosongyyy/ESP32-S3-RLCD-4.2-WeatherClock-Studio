#!/usr/bin/env python3
"""把番茄钟用到的一对番茄 SVG 栅格化成 C 点阵，供固件直接 blit。

与 tests/pomodoro_preview.py 共用 lucide_raster.render_filled_svg，
保证预览和固件的番茄图标逐点一致。输出 main/assets/pomodoro_icons.h：
每个图标一组 MSB-first 的 1bit 行字节，1=墨(黑)，0=纸(白)，严格按
用户提供的一对 SVG 原样渲染，不加粗、不细化、不加网点。
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
TESTS = ROOT / "tests"
sys.path.insert(0, str(TESTS))
import lucide_raster as lr  # noqa: E402

TOMATO_DIR = TESTS / "icons" / "tomato"
OUT = ROOT / "main" / "assets" / "pomodoro_icons.h"

# (C 符号后缀, svg 文件, 尺寸)
ICONS = [
    ("tomato_outline_59", "tomato_outline.svg", 59),
    ("tomato_outline_20", "tomato_outline.svg", 20),
    ("tomato_solid_20", "tomato_solid.svg", 20),
]


def pack(im):
    """1bit PIL 图 → 行字节数组，1=墨。"""
    px = im.load()
    w, h = im.size
    stride = (w + 7) // 8
    out = bytearray()
    for y in range(h):
        for b in range(stride):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                if x < w and px[x, y] < 128:
                    byte |= 1 << (7 - bit)
            out.append(byte)
    return bytes(out), stride


def main():
    lines = [
        "/* 由 tools/gen_pomodoro_icons.py 自动生成，勿手工编辑。",
        " * 番茄钟图标点阵，1=墨(黑)，0=纸(白)，MSB-first 行对齐。",
        " * 由用户指定的一对填充式番茄 SVG 1 倍硬边栅格化，严格按原样。",
        " */",
        "#pragma once",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
        "typedef struct {",
        "    uint8_t w, h, stride;",
        "    const uint8_t *bits;",
        "} pomodoro_icon_t;",
        "",
    ]

    entries = []
    for sym, svg, size in ICONS:
        im = lr.render_filled_svg(TOMATO_DIR / svg, size)
        bits, stride = pack(im)
        lines.append(f"static const uint8_t pomodoro_{sym}_bits[] = {{")
        for i in range(0, len(bits), 16):
            chunk = ", ".join(f"0x{b:02X}" for b in bits[i:i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
        entries.append((sym, size, stride, f"pomodoro_{sym}_bits"))

    lines.append("static const pomodoro_icon_t kPomodoroIcons[] = {")
    for sym, size, stride, bits_name in entries:
        lines.append(f'    {{{size}, {size}, {stride}, {bits_name}}},')
    lines.append("};")
    lines.append("")
    lines.append("enum PomodoroIconIndex {")
    for i, (sym, _, _, _) in enumerate(entries):
        lines.append(f"    kPomodoroIcon_{sym} = {i},")
    lines.append("    kPomodoroIconCount,")
    lines.append("};")
    lines.append("")

    OUT.write_text("\n".join(lines), encoding="utf-8")
    total = sum(len(bits) for _, _, _, _ in entries)
    print(f"wrote {OUT.relative_to(ROOT)} ({len(entries)} icons, {total} bytes)")


if __name__ == "__main__":
    main()
