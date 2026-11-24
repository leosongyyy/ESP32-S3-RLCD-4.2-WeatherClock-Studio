#!/usr/bin/env python3
"""把 Lucide 的 24x24 描边 SVG 栅格化成 1bit 点阵。

本机没有可用的 cairo / rlPyCairo 后端，所以不依赖它们。Lucide 图标只由
path / circle / line / rect / polyline / polygon 组成，基本不带 transform，
解析成折线再用 PIL 超采样描边即可得到干净的点阵。

预览和固件共用同一份点阵输出，避免「预览好看、烧上去变形」。
"""

import math
import re
from pathlib import Path
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

SS = 4  # 超采样倍率
_TOKEN = re.compile(r"[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")
_NUM = re.compile(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")
ICON_DIR = Path(__file__).resolve().parent / "icons" / "lucide"


def _lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def _cubic(p0, p1, p2, p3, n=12):
    return [_lerp(_lerp(_lerp(p0, p1, t / n), _lerp(p1, p2, t / n), t / n),
                 _lerp(_lerp(p1, p2, t / n), _lerp(p2, p3, t / n), t / n), t / n)
            for t in range(1, n + 1)]


def _quad(p0, p1, p2, n=12):
    return [_lerp(_lerp(p0, p1, t / n), _lerp(p1, p2, t / n), t / n)
            for t in range(1, n + 1)]


def _arc(p0, rx, ry, phi_deg, large, sweep, p1):
    """SVG 椭圆弧端点参数化，返回不含 p0、含 p1 的折线点。"""
    x0, y0 = p0
    x1, y1 = p1
    if rx == 0 or ry == 0 or p0 == p1:
        return [p1]
    phi = math.radians(phi_deg)
    cp, sp = math.cos(phi), math.sin(phi)
    hx, hy = (x0 - x1) / 2.0, (y0 - y1) / 2.0
    x1p, y1p = cp * hx + sp * hy, -sp * hx + cp * hy
    rx, ry = abs(rx), abs(ry)
    lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry)
    if lam > 1.0:
        k = math.sqrt(lam)
        rx *= k
        ry *= k
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if bool(large) == bool(sweep):
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x0 + x1) / 2.0
    cy = sp * cxp + cp * cyp + (y0 + y1) / 2.0
    th1 = math.atan2((y1p - cyp) / ry, (x1p - cxp) / rx)
    th2 = math.atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx)
    delta = th2 - th1
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    if sweep and delta < 0:
        delta += 2 * math.pi
    steps = max(4, math.ceil(abs(delta) / (math.pi / 16)))
    out = []
    for i in range(1, steps + 1):
        th = th1 + delta * i / steps
        ct, st = math.cos(th), math.sin(th)
        out.append((cx + cp * rx * ct - sp * ry * st,
                    cy + sp * rx * ct + cp * ry * st))
    return out


def _path_subpaths(d):
    """解析 path d，返回 [(点列, 是否闭合), ...]。"""
    toks = _TOKEN.findall(d or "")
    i = 0
    cmd = None
    cur = start = (0.0, 0.0)
    last_c = last_q = None
    subs, pts = [], []

    def num(flag=False):
        nonlocal i
        tok = toks[i]
        # SVG 允许把弧线的两个 flag 和后面的坐标挤在一起，例如 A2 2 0 0022 17
        # 表示 large=0, sweep=0, x=22, y=17。flag 只占一个字符，必须先拆。
        if flag and len(tok) > 1 and tok[0] in "01" and not tok.startswith("0.") \
                and not tok.startswith("1."):
            toks[i] = tok[1:]
            return float(tok[0])
        i += 1
        return float(tok)

    def close_sub():
        nonlocal pts
        if len(pts) >= 2:
            subs.append((pts, True))
        pts = []

    while i < len(toks):
        t = toks[i]
        if t.isalpha():
            cmd = t
            i += 1
        if cmd is None:
            raise ValueError("path 在指令前出现了数字")
        if cmd in "Zz":
            if pts:
                pts.append(start)
                close_sub()
            cur = start
            last_c = last_q = None
            cmd = None
            continue

        rel = cmd.islower()
        op = cmd.upper()
        if op == "M":
            x, y = num(), num()
            if rel:
                x += cur[0]
                y += cur[1]
            if pts:
                close_sub()
            cur = start = (x, y)
            pts = [cur]
            cmd = "l" if rel else "L"   # 后续坐标对等价于 L/l
            last_c = last_q = None
            continue
        if op == "L":
            x, y = num(), num()
            if rel:
                x += cur[0]
                y += cur[1]
            pts.append((x, y))
            cur = (x, y)
        elif op == "H":
            x = num()
            if rel:
                x += cur[0]
            pts.append((x, cur[1]))
            cur = (x, cur[1])
        elif op == "V":
            y = num()
            if rel:
                y += cur[1]
            pts.append((cur[0], y))
            cur = (cur[0], y)
        elif op == "C":
            x1, y1, x2, y2, x, y = (num() for _ in range(6))
            if rel:
                x1 += cur[0]; y1 += cur[1]
                x2 += cur[0]; y2 += cur[1]
                x += cur[0]; y += cur[1]
            pts.extend(_cubic(cur, (x1, y1), (x2, y2), (x, y)))
            last_c = (x2, y2)
            cur = (x, y)
        elif op == "S":
            x2, y2, x, y = (num() for _ in range(4))
            if rel:
                x2 += cur[0]; y2 += cur[1]
                x += cur[0]; y += cur[1]
            c1 = (2 * cur[0] - last_c[0], 2 * cur[1] - last_c[1]) if last_c else cur
            pts.extend(_cubic(cur, c1, (x2, y2), (x, y)))
            last_c = (x2, y2)
            cur = (x, y)
        elif op == "Q":
            x1, y1, x, y = (num() for _ in range(4))
            if rel:
                x1 += cur[0]; y1 += cur[1]
                x += cur[0]; y += cur[1]
            pts.extend(_quad(cur, (x1, y1), (x, y)))
            last_q = (x1, y1)
            cur = (x, y)
        elif op == "T":
            x, y = num(), num()
            if rel:
                x += cur[0]
                y += cur[1]
            q1 = (2 * cur[0] - last_q[0], 2 * cur[1] - last_q[1]) if last_q else cur
            pts.extend(_quad(cur, q1, (x, y)))
            last_q = q1
            cur = (x, y)
        elif op == "A":
            rx, ry, phi = num(), num(), num()
            large, sweep = num(flag=True), num(flag=True)
            x, y = num(), num()
            if rel:
                x += cur[0]
                y += cur[1]
            pts.extend(_arc(cur, rx, ry, phi, int(large), int(sweep), (x, y)))
            cur = (x, y)
        else:
            raise ValueError("不支持的 path 指令 %s" % cmd)
        if op not in "CSQT":
            last_c = last_q = None

    if pts:
        subs.append((pts, False))
    return subs


def _rect_points(x, y, w, h, rx, ry):
    if rx <= 0 and ry <= 0:
        return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]
    out = []
    for cx, cy, a0 in ((x + w - rx, y + ry, -90),
                       (x + w - rx, y + h - ry, 0),
                       (x + rx, y + h - ry, 90),
                       (x + rx, y + ry, 180)):
        for k in range(7):
            a = math.radians(a0 + k * 15)
            out.append((cx + rx * math.cos(a), cy + ry * math.sin(a)))
    return out


def _shapes(el):
    tag = el.tag.split("}")[-1]
    if tag == "path":
        return _path_subpaths(el.get("d", ""))
    if tag == "circle":
        cx, cy, r = (float(el.get(k, 0)) for k in ("cx", "cy", "r"))
        return [([(cx + r * math.cos(2 * math.pi * k / 64),
                   cy + r * math.sin(2 * math.pi * k / 64)) for k in range(64)], True)]
    if tag == "line":
        x1, y1, x2, y2 = (float(el.get(k, 0)) for k in ("x1", "y1", "x2", "y2"))
        return [([(x1, y1), (x2, y2)], False)]
    if tag == "rect":
        x, y = float(el.get("x", 0)), float(el.get("y", 0))
        w, h = float(el.get("width", 0)), float(el.get("height", 0))
        rx = float(el.get("rx", 0) or 0)
        ry = float(el.get("ry", rx) or rx)
        return [(_rect_points(x, y, w, h, rx, ry), True)]
    if tag in ("polyline", "polygon"):
        nums = [float(v) for v in _NUM.findall(el.get("points", ""))]
        pts = list(zip(nums[0::2], nums[1::2]))
        return [(pts, tag == "polygon")]
    return []


def render_svg(path, size, stroke=1.0, threshold=160, ss=None, width_px=None):
    """把一个 Lucide SVG 渲染成 size x size 的 1bit PIL 图（0=黑，255=白）。

    ss=None 用模块默认超采样(SS=4)；ss=1 表示不超采样，直接在目标分辨率画硬边。
    width_px 给定时线宽直接按目标像素计（超采样时内部乘 ss），便于和电池 1px 硬边对齐。
    """
    root = ET.parse(str(path)).getroot()
    vb = [float(v) for v in root.get("viewBox", "0 0 24 24").replace(",", " ").split()]
    vx, vy, vw, vh = vb
    ss = SS if ss is None else ss
    S = size * ss
    scale = S / max(vw, vh)
    canvas = Image.new("L", (S, S), 255)
    dr = ImageDraw.Draw(canvas)
    base = {"fill": root.get("fill", "none"),
            "stroke": root.get("stroke", "#000000"),
            "stroke-width": float(root.get("stroke-width", "1"))}

    for el in root.iter():
        tag = el.tag.split("}")[-1]
        if tag == "svg" or el is root:
            continue
        if any(k in el.attrib for k in ("transform", "clip-path", "mask")):
            raise ValueError("Lucide 图标不应含 transform/clip-path/mask，请人工处理")
        a = dict(base)
        for k in base:
            if k in el.attrib:
                a[k] = el.attrib[k]
        a["stroke-width"] = float(a["stroke-width"])
        for pts, closed in _shapes(el):
            if len(pts) < 2:
                continue
            sp = [((x - vx) * scale, (y - vy) * scale) for x, y in pts]
            if a["fill"] != "none" and closed and len(sp) >= 3:
                dr.polygon(sp, fill=0)
            if a["stroke"] == "none":
                continue
            if width_px is not None:
                w = max(1, round(width_px * ss))
            else:
                w = max(1, round(a["stroke-width"] * scale * stroke))
            line = sp + [sp[0]] if closed else sp
            dr.line(line, fill=0, width=w, joint="curve")
            if not closed:
                r = w / 2.0
                for x, y in (sp[0], sp[-1]):
                    dr.ellipse((x - r, y - r, x + r, y + r), fill=0)

    if ss == 1:
        small = canvas
    else:
        small = canvas.resize((size, size), Image.LANCZOS)
    return small.point(lambda p: 0 if p < threshold else 255).convert("1")


def render_filled_svg(path, size, threshold=128):
    """把一个纯填充式 SVG 渲染成 size x size 的 1bit 图（0=黑，255=白）。

    与 render_svg 不同，这类图标（如用户提供的番茄）不带 stroke，完全靠
    path 的填充轮廓成形。逐条闭合子路径用 polygon 填黑，路径自身的环绕
    关系自然留出镂空。按目标分辨率 1 倍硬边栅格化，不做超采样。
    """
    root = ET.parse(str(path)).getroot()
    vb = [float(v) for v in root.get("viewBox", "0 0 24 24").replace(",", " ").split()]
    vx, vy, vw, vh = vb
    scale = size / max(vw, vh)
    small = Image.new("L", (size, size), 255)
    dr = ImageDraw.Draw(small)
    for el in root.iter():
        tag = el.tag.split("}")[-1]
        if tag != "path":
            continue
        if any(k in el.attrib for k in ("transform", "clip-path", "mask")):
            raise ValueError("填充图标不应含 transform/clip-path/mask，请人工处理")
        for pts, closed in _shapes(el):
            if not closed or len(pts) < 3:
                continue
            sp = [((x - vx) * scale, (y - vy) * scale) for x, y in pts]
            dr.polygon(sp, fill=0)
    return small.point(lambda p: 0 if p < threshold else 255).convert("1")


def icon(name, size, **kw):
    """按图标名渲染，例如 icon('gamepad-2', 32)。"""
    return render_svg(ICON_DIR / (name + ".svg"), size, **kw)
