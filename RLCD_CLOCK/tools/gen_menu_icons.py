#!/usr/bin/env python3
"""把主菜单用到的 Lucide SVG 图标栅格化成 C 点阵，供固件直接 blit。

与 tests/lucide_raster.py 共用同一套栅格化逻辑，保证预览和固件图标一致。
输出 main/assets/menu_icons.h：每个图标一组 MSB-first 的 1bit 行字节，
1=墨(黑)，0=纸(白)。
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TESTS = HERE.parent / "tests"
sys.path.insert(0, str(TESTS))
import lucide_raster as lr  # noqa: E402

# (C 符号后缀, svg 名, 尺寸, stroke)
ICONS = [
    ("cloud_sun_34", "cloud-sun", 34, 1.0),
    ("timer_34", "timer", 34, 1.0),
    ("gamepad_2_34", "gamepad-2", 34, 1.0),
    ("book_open_34", "book-open", 34, 1.0),
    ("music_34", "music", 34, 1.0),
    ("newspaper_34", "newspaper", 34, 1.0),
    ("bot_34", "bot", 34, 1.0),
    ("settings_34", "settings", 34, 1.0),
    ("thermometer_sun_18", "thermometer-sun", 18, 1.0),
    ("droplets_18", "droplets", 18, 1.0),
    ("wifi_18", "wifi", 18, 1.35),
    ("wifi_off_18", "wifi-off", 18, 1.35),
    ("bluetooth_18", "bluetooth", 18, 1.35),
    ("bluetooth_off_18", "bluetooth-off", 18, 1.35),
    ("chevron_left_12", "chevron-left", 12, 1.0),
    ("chevron_right_12", "chevron-right", 12, 1.0),
    ("dpad_12", "dpad", 12, 1.0),
]

# 不超采样，直接在目标分辨率画硬边图。
# 状态栏 18px 用 1px 线宽，与电池框一致。
DIRECT_1X = {
    "thermometer_sun_18": 1, "droplets_18": 1,
    "wifi_18": 1, "wifi_off_18": 1, "bluetooth_18": 1, "bluetooth_off_18": 1,
    "dpad_12": 1,
}

# 模块卡 34px 用 1 倍图，但线宽保留 Lucide 原 SVG 的 stroke-width=2
# 按比例换算（34px 下约 3px），不手动写死像素值。
SS1_ORIGINAL_STROKE = {
    "cloud_sun_34", "timer_34", "gamepad_2_34", "book_open_34",
    "music_34", "newspaper_34", "bot_34", "settings_34",
}


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
        "/* 由 tools/gen_menu_icons.py 自动生成，勿手工编辑。",
        " * 主菜单 Lucide 图标点阵，1=墨(黑)，0=纸(白)，MSB-first 行对齐。",
        " * 来源 Lucide (https://lucide.dev) ISC 许可，经 tests/lucide_raster.py 栅格化。",
        " */",
        "#pragma once",
        "#include <stdint.h>",
        "#include <stddef.h>",
        "",
        "typedef struct {",
        "    const char *name;",
        "    uint8_t w, h, stride;",
        "    const uint8_t *bits;",
        "} menu_icon_t;",
        "",
    ]
    total = 0
    for sym, svg, size, stroke in ICONS:
        if sym in DIRECT_1X:
            kw = dict(ss=1, width_px=DIRECT_1X[sym])
        elif sym in SS1_ORIGINAL_STROKE:
            kw = dict(ss=1, stroke=stroke)   # 1 倍图，线宽沿用原 SVG stroke-width
        else:
            kw = dict(stroke=stroke)
        im = lr.render_svg(TESTS / "icons" / "lucide" / (svg + ".svg"),
                           size, **kw)
        bits, stride = pack(im)
        total += len(bits)
        lines.append(f"static const uint8_t menu_{sym}_bits[] = {{")
        for i in range(0, len(bits), 16):
            lines.append("    " + " ".join(f"0x{b:02X}," for b in bits[i:i + 16]))
        lines.append("};")
        lines.append("")
    lines.append("static const menu_icon_t kMenuIcons[] = {")
    for sym, _svg, size, _stroke in ICONS:
        stride = (size + 7) // 8
        lines.append(f'    {{"{sym}", {size}, {size}, {stride}, menu_{sym}_bits}},')
    lines.append("};")
    lines.append("")
    lines.append("#define MENU_ICON_COUNT " + str(len(ICONS)))
    lines.append("")
    out = HERE.parent / "main" / "assets" / "menu_icons.h"
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {out}  icons={len(ICONS)}  bytes={total}")


if __name__ == "__main__":
    main()
