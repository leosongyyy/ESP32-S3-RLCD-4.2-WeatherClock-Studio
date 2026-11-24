#!/usr/bin/env python3
"""GB 灰度算法主机验证 + 新旧对比预览图。

直接照抄 gameboy_fb.cpp 的三套图案表（2x2 旋转 / 分相位 Bayer4x4），
在主机侧生成 2x 放大后的子像素点阵，验证：
  T1 纯白/纯黑在任意坐标下严格成立（无噪点）
  T2 中间两档的填充率 == kShadeDensity（4/3/1/0），逐块守恒
  T3 分相位方案 4 个相位的图案互不相同（网格真被打散）
  T4 新旧方案的平均黑点数相同（升级只改分布，不改亮度）
再渲染一张对比 PNG：4 级灰阶条 + 两档棋盘格，左右并排。

用法：python3 tests/dither_preview.py
输出：tests/dither_preview.png
"""
import os
from PIL import Image, ImageDraw

W, H = 160, 144
SCALE = 2  # 每个 GB 像素 = 2x2 子像素

# ---- 与 game_config.h 一致 ----
SHADE_DENSITY = [4, 3, 1, 0]   # 亮子像素数
SHADE_LEVEL   = [16, 12, 4, 0] # 亮度刻度 0..16

# ---- 模式 1：2x2 有序抖动 4 旋转 ----
BAYER2 = [
    [[0, 2], [3, 1]],
    [[3, 0], [1, 2]],
    [[1, 3], [2, 0]],
    [[2, 1], [0, 3]],
]
SUBBIT = [[1, 0], [3, 2]]  # [行][列] -> nibble 位号

def density_nibble(rot, shade):
    d = SHADE_DENSITY[shade]
    p = 0
    for i in range(2):
        for j in range(2):
            if BAYER2[rot][i][j] < d:
                p |= 1 << SUBBIT[i][j]
    return p

DENSITY_LUT = [[density_nibble(r, s) for s in range(4)] for r in range(4)]

# ---- 模式 3：分相位 Bayer4x4 ----
PHASE_BAYER = [
    [ 0,  6,  4,  1],
    [10, 15, 14,  9],
    [ 5, 13, 12,  7],
    [ 2, 11,  8,  3],
]

def phase_nibble(lp, cp, shade):
    d = 16 - SHADE_LEVEL[shade]  # 黑度
    p = 0
    for i in range(2):
        for j in range(2):
            by = (lp * 2 + i) & 3
            bx = (cp * 2 + j) & 3
            if not (d > PHASE_BAYER[by][bx]):
                p |= 1 << SUBBIT[i][j]
    return p

PHASE_LUT = [[[phase_nibble(lp, cp, s) for s in range(4)]
              for cp in range(2)] for lp in range(2)]

def nibble_old(shade, line, col):
    return DENSITY_LUT[(line + col) & 3][shade]

def nibble_new(shade, line, col):
    return PHASE_LUT[line & 1][col & 1][shade]

def popcount4(n):
    return bin(n).count("1")

# ---- 验证 ----
def verify():
    print("=== GB 灰度算法主机验证 ===")
    # T1: 纯白全亮 / 纯黑全灭
    ok = True
    for y in range(H):
        for x in range(W):
            if nibble_new(0, y, x) != 0x0F or nibble_new(3, y, x) != 0x00:
                ok = False
                break
        if not ok:
            break
    print(f"  T1 纯白/纯黑全屏守恒        {'PASS' if ok else 'FAIL'}")
    assert ok

    # T2: 4 相位下 shade1/2 的亮子像素数 == kShadeDensity
    ok = True
    for lp in range(2):
        for cp in range(2):
            for sh in (1, 2):
                if popcount4(PHASE_LUT[lp][cp][sh]) != SHADE_DENSITY[sh]:
                    ok = False
    print(f"  T2 中间档密度 == DMG 曲线   {'PASS' if ok else 'FAIL'}")
    assert ok

    # T3: 4 相位图案互不相同
    ok = len({PHASE_LUT[lp][cp][1] for lp in range(2) for cp in range(2)}) == 4
    print(f"  T3 四相位图案互不相同       {'PASS' if ok else 'FAIL'}")
    assert ok

    # T4: 新旧方案在 40x40 采样窗里黑点数相同（亮度不变）
    # 注意旧方案 4 旋转是全局按 (line+col)&3 轮换的，直接算密度不依赖邻域。
    ok = True
    for sh in range(4):
        old_dark = sum(4 - popcount4(nibble_old(sh, y, x))
                       for y in range(40) for x in range(40))
        new_dark = sum(4 - popcount4(nibble_new(sh, y, x))
                       for y in range(40) for x in range(40))
        if old_dark != new_dark:
            ok = False
            print(f"    shade{sh}: old_dark={old_dark} new_dark={new_dark}")
    print(f"  T4 新旧方案平均亮度相同     {'PASS' if ok else 'FAIL'}")
    assert ok
    print()

# ---- 渲染 ----
def make_test_frame():
    """构造一屏测试画面：上半 4 级灰阶竖条，下半 shade1/shade2 棋盘格。"""
    frame = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if y < 72:
                # 4 级灰阶竖条，每 40 列一档
                frame[y][x] = min(3, x // 40)
            elif y < 108:
                # shade1 棋盘格，8x8 一块
                frame[y][x] = 1 if ((x // 8) + (y // 8)) % 2 == 0 else 0
            else:
                # shade2 棋盘格
                frame[y][x] = 2 if ((x // 8) + (y // 8)) % 2 == 0 else 0
    return frame

def render(frame, nibble_fn, scale=3):
    """把 GB 帧渲染成子像素点阵图，每个子像素放大 scale 倍。"""
    img = Image.new("RGB", (W * SCALE * scale, H * SCALE * scale), "white")
    px = img.load()
    for y in range(H):
        for x in range(W):
            n = nibble_fn(frame[y][x], y, x)
            for i in range(2):
                for j in range(2):
                    lit = bool(n & (1 << SUBBIT[i][j]))
                    color = (255, 255, 255) if lit else (24, 24, 24)
                    sy0 = (y * 2 + i) * scale
                    sx0 = (x * 2 + j) * scale
                    for dy in range(scale):
                        for dx in range(scale):
                            px[sx0 + dx, sy0 + dy] = color
    return img

def main():
    verify()
    frame = make_test_frame()
    scale = 2
    old = render(frame, nibble_old, scale)
    new = render(frame, nibble_new, scale)
    pad = 24
    out = Image.new("RGB", (old.width * 2 + pad * 3,
                            old.height + pad * 2 + 28), (245, 245, 245))
    out.paste(old, (pad, pad + 28))
    out.paste(new, (old.width + pad * 2, pad + 28))
    draw = ImageDraw.Draw(out)
    draw.text((pad, 8), "OLD: 2x2 rotation (4 thresholds)", fill=(20, 20, 20))
    draw.text((old.width + pad * 2, 8), "NEW: phase Bayer4x4 (16 thresholds)",
              fill=(20, 20, 20))
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "dither_preview.png")
    out.save(path)
    print("wrote", path)

if __name__ == "__main__":
    main()
