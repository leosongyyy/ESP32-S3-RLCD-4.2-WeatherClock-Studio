#!/usr/bin/env python3
"""生成 GB2312 双字节 -> Unicode 映射表（C 头文件）。

只覆盖 0xA1-0xF7 x 0xA1-0xFE 这一段（GB2312 全部符号区 + 汉字区），
共 87*94 = 8178 项，uint16 表示，编进固件约 16KB。
GBK 扩展区（0x81-0xA0 与低字节 0x40-0xA0）不在表内，遇到时输出占位符。

TXT 书 99% 是 GB2312 范围内，这个取舍是为了 app 分区只剩 140KB 的现实。

用法：python3 tools/gen_gb2312_table.py
"""
import pathlib

FIRST, LAST = 0xA1, 0xF7
COLS = 94  # 0xA1..0xFE


def main() -> None:
    rows = LAST - FIRST + 1
    table = [0] * (rows * COLS)
    filled = 0
    for b1 in range(FIRST, LAST + 1):
        for b2 in range(0xA1, 0xFF):
            try:
                ch = bytes([b1, b2]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            cp = ord(ch)
            if cp > 0xFFFF:
                continue
            table[(b1 - FIRST) * COLS + (b2 - 0xA1)] = cp
            filled += 1

    out = pathlib.Path(__file__).resolve().parents[1] / "main" / "book" / "gb2312_table.h"
    out.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "// GB2312 双字节到 Unicode 的映射表，由 tools/gen_gb2312_table.py 生成。",
        "// 覆盖 0xA1-0xF7 高位 x 0xA1-0xFE 低位，索引 = (b1-0xA1)*94 + (b2-0xA1)。",
        "// 0 表示该码位在 GB2312 中未定义。GBK 扩展区不在此表内。",
        "// 生成来源：Python gb2312 编解码器（等价于 GB2312-80）。",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define GB2312_TABLE_FIRST 0x{FIRST:02X}",
        f"#define GB2312_TABLE_LAST  0x{LAST:02X}",
        f"#define GB2312_TABLE_COLS  {COLS}",
        f"#define GB2312_TABLE_COUNT {rows * COLS}",
        "",
        f"static const uint16_t kGb2312Table[{rows * COLS}] = {{",
    ]
    for i in range(0, len(table), 16):
        chunk = ",".join(f"0x{v:04X}" for v in table[i:i + 16])
        lines.append("    " + chunk + ",")
    lines.append("};")
    lines.append("")
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {out} entries={len(table)} filled={filled} size={out.stat().st_size} bytes")


if __name__ == "__main__":
    main()
