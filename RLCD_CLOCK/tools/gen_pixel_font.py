#!/usr/bin/env python3
"""按「取模」方式从 Fusion Pixel BDF 生成紧凑字库。

核心：只保留项目实际用到的字符，而不是把整个 CJK（0x4E00-0x9FFF）都打进去。
现有 main/assets/zh_font_16.c 用 LVGL converter 转了全 CJK，6.2 MB；
本工具取模后通常只有 10~15 KB，烧录/PSRAM 压力降两个数量级。

输出：
  tests/fusion_pixel_font.py   预览脚本用的 Python 字库（dict: cp -> glyph）
  main/assets/fusion_pixel_12_font.h  C 取模字库（点阵 blob + 索引表，供固件）

用法：
  python3 tools/gen_pixel_font.py --bdf /path/to/fusion-pixel-12px-zh_hans.bdf

BDF 来源：https://fontpixel.com/zh/fonts/fusion-pixel/#downloads
  fusion-pixel-12px-proportional-zh_hans.bdf.gz
授权：字体为开源（OFL 系），取模后仍随附 LICENSE 说明。
"""
import argparse
import pathlib
import re
import sys

ASCII = ''.join(chr(c) for c in range(0x20, 0x7F))
# 界面里出现的杂符号（BDF 均覆盖）
SYMBOLS = '°·×—…←→▲▼●○■□✓！？，。：、（）《》'


def scan_source_cjk(root):
    """扫描 main/ 下 C/C++ 字符串字面量里的中文，取模式只取这些字。"""
    cjk = re.compile(r'[\u4e00-\u9fff]')
    chars = set()
    for p in pathlib.Path(root).rglob('*'):
        if p.suffix not in ('.c', '.cpp', '.h', '.hpp'):
            continue
        try:
            txt = p.read_text(encoding='utf-8')
        except Exception:
            continue
        txt = re.sub(r'//[^\n]*', '', txt)
        txt = re.sub(r'/\*.*?\*/', '', txt, flags=re.S)
        for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', txt):
            chars.update(cjk.findall(m.group(1)))
    return chars


def parse_bdf(path):
    """返回 {cp: {'w','h','xoff','yoff','dw','rows':[int,...]}}。"""
    glyphs = {}
    cp = None
    g = None
    for raw in pathlib.Path(path).read_text(encoding='utf-8').splitlines():
        line = raw.strip()
        if line.startswith('STARTCHAR '):
            g = None
        elif line.startswith('ENCODING '):
            cp = int(line.split()[1])
            g = {'w': 0, 'h': 0, 'xoff': 0, 'yoff': 0, 'dw': 0, 'rows': []}
        elif g is not None and line.startswith('DWIDTH '):
            g['dw'] = int(line.split()[1])
        elif g is not None and line.startswith('BBX '):
            _, w, h, xo, yo = line.split()
            g['w'], g['h'], g['xoff'], g['yoff'] = int(w), int(h), int(xo), int(yo)
        elif g is not None and line.startswith('BITMAP'):
            g['_bitmap'] = True
        elif g is not None and g.get('_bitmap') and re.fullmatch(r'[0-9A-Fa-f]+', line):
            # BDF 每行 hex 是 MSB-first 的位图行，按字节存取
            b = bytes.fromhex(line if len(line) % 2 == 0 else '0' + line)
            g['rows'].append(int.from_bytes(b, 'big'))
        elif line.startswith('ENDCHAR'):
            # 空格等零行字形也要保留（rows 为空但 dw 有效），渲染器只步进不画点
            if g is not None and (g['rows'] or g['dw']):
                glyphs[cp] = g
            g = None
            cp = None
    return glyphs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bdf', required=True, help='fusion-pixel BDF 路径')
    ap.add_argument('--size', type=int, default=12, help='字号 8/10/12，决定 FONT_ASCENT 与 C 符号前缀')
    ap.add_argument('--src', default='main', help='扫描中文的源码根目录')
    ap.add_argument('--extra', default='', help='额外需要取模的字符（预览静态文本等）')
    ap.add_argument('--latin-only', action='store_true', help='只导出 ASCII，不扫描源码中文（用于小号英文字库）')
    ap.add_argument('--py-out', default='tests/fusion_pixel_font.py')
    ap.add_argument('--h-out', default='main/assets/fusion_pixel_12_font.h')
    args = ap.parse_args()

    bdf_glyphs = parse_bdf(args.bdf)
    if args.latin_only:
        wanted = set(ASCII)
    else:
        wanted = set(ASCII) | set(SYMBOLS) | scan_source_cjk(args.src)
    wanted |= set(args.extra)
    # 按码位排序，保证输出稳定
    cps = sorted({ord(c) for c in wanted if ord(c) in bdf_glyphs})
    missing = sorted({ord(c) for c in wanted if ord(c) not in bdf_glyphs})

    FONT_H = args.size
    # 大写字母墨迹高：8px→6，10px→7，12px→9（实测 Fusion Pixel 各尺寸）
    FONT_ASCENT = {8: 6, 10: 7, 12: 9}.get(args.size, 9)
    FONT_DESCENT = FONT_H - FONT_ASCENT
    SYM = f'fusion_pixel_{args.size}'   # C 符号前缀，避免多字号同时链接时冲突

    # 生成 Python 字库
    lines = [
        '"""由 tools/gen_pixel_font.py 自动生成，勿手工编辑。"""',
        f'# Fusion Pixel {args.size}px proportional 取模字库（仅含项目实际用到的字符）',
        '# glyph = (w, h, xoff, yoff, dw, row_bytes, rows)',
        '# BDF 位图行是 MSB 左对齐：第 col 列 = 行值 bit (row_bytes*8-1-col)',
        f'FONT_SIZE = {FONT_H}',
        f'FONT_ASCENT = {FONT_ASCENT}',
        f'FONT_DESCENT = {FONT_DESCENT}',
        'GLYPHS = {',
    ]
    total_pixels = 0
    for cp in cps:
        g = bdf_glyphs[cp]
        rows = g['rows']
        # BDF 的 hex 行按字节 MSB 对齐，不能裁到低 w 位，否则高位像素丢失
        nbytes = (g['w'] + 7) // 8
        total_pixels += sum(bin(r).count('1') for r in rows)
        ch = chr(cp)
        safe = ch.replace('\\', '\\\\').replace("'", "\\'")
        lines.append(f"    0x{cp:04X}:  # '{safe}'")
        lines.append(f"        ({g['w']}, {g['h']}, {g['xoff']}, {g['yoff']}, {g['dw']}, {nbytes}, {rows!r}),")
    lines.append('}')
    pathlib.Path(args.py_out).write_text('\n'.join(lines) + '\n', encoding='utf-8')

    # 生成 C 字库：点阵 blob + 排序索引表（二分查找）
    blob = bytearray()
    entries = []
    for cp in cps:
        g = bdf_glyphs[cp]
        rows = g['rows']
        nbytes = (g['w'] + 7) // 8
        off = len(blob)
        for r in rows:
            blob.extend(r.to_bytes(nbytes, 'big'))
        entries.append((cp, off, g['w'], g['h'], g['xoff'], g['yoff'], g['dw'], nbytes))

    c = [
        '/* 由 tools/gen_pixel_font.py 自动生成，勿手工编辑。',
        f' * 字体 Fusion Pixel (缝合像素) {args.size}px proportional，取模只保留项目实际用到的字符。',
        f' * 字符数 {len(cps)}，点阵 blob {len(blob)} 字节。',
        ' * 来源 https://fontpixel.com/zh/fonts/fusion-pixel/  作者 TakWolf (c) 2022',
        ' * 许可 SIL Open Font License 1.1 (OFL-1.1)，允许商用/修改/再分发；取模字库沿用该许可。',
        ' * 全文 https://openfontlicense.org/open-font-license-official-text/',
        ' */',
        '#pragma once',
        '#include <stdint.h>',
        '',
        'typedef struct {',
        '    uint16_t cp;',
        '    uint16_t off;   /* blob 内偏移 */',
        '    uint8_t  w, h, xoff;',
        '    int8_t   yoff;   /* BDF 相对基线偏移，可为负 */',
        '    uint8_t  dw, row_bytes;',
        f'}} {SYM}_glyph_t;',
        '',
        f'static const uint8_t {SYM}_bits[] = {{',
    ]
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        c.append('    ' + ' '.join(f'0x{b:02X},' for b in chunk))
    c.append('};')
    c.append('')
    c.append(f'static const {SYM}_glyph_t {SYM}_index[] = {{')
    for cp, off, w, h, xo, yo, dw, nb in entries:
        c.append(f'    {{0x{cp:04X}, {off}, {w}, {h}, {xo}, {yo}, {dw}, {nb}}},'
                 f" /* {chr(cp)!r} */")
    c.append('};')
    c.append(f'#define {SYM.upper()}_INDEX_COUNT {len(entries)}')
    c.append('')
    pathlib.Path(args.h_out).write_text('\n'.join(c) + '\n', encoding='utf-8')

    py_size = pathlib.Path(args.py_out).stat().st_size
    h_size = pathlib.Path(args.h_out).stat().st_size
    print(f'字模字符数: {len(cps)}')
    print(f'点阵 blob : {len(blob)} B (有像素 {total_pixels})')
    print(f'Python 字库: {py_size} B -> {args.py_out}')
    print(f'C 字库     : {h_size} B -> {args.h_out}')
    if missing:
        print(f'未在 BDF 中找到 {len(missing)} 字符: '
              + ''.join(chr(c) for c in missing[:60]))


if __name__ == '__main__':
    main()
