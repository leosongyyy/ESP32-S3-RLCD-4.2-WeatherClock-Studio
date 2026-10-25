#!/usr/bin/env python3
"""hat 静止中位（锚点）判据的回归仿真。

为什么需要这个脚本
------------------
真机排查一次要连上手柄、按几分钟、再从串口日志里翻证据。而"哪种取值序列会
把判据骗过去"这种问题，用脚本喂序列几秒就能跑完。这个脚本把三版判据都实现
了一遍，用来说明每一次改动的必要性：

    v1 众数     —— 出现次数最多的当中心
    v2 转移度数 —— 转移图里度数最高的当中心
    v3 规范先验 —— 区间外的值 = null；否则见过 0 且 0 参与过转移 = 族 A

设备背景：Q36「for Android」，静置时一包报文都不发（实测 150 秒 0 包），
描述符声明 Hat Logical Min 0 / Max 7 + Null State，固件实际按族 A 发
（0 = 居中，1..8 顺时针，1=上 2=上右 3=右 4=下右 5=下 6=下左 7=左 8=左上）。

跑法：python3 tests/hat_anchor_seed_sim.py
"""

HIST = 16

# Q36 描述符里 hat 字段的声明
LMIN, LMAX = 0, 7

# 族 A 取值
CENTER = 0
UP, UP_RIGHT, RIGHT, DOWN_RIGHT = 1, 2, 3, 4
DOWN, DOWN_LEFT, LEFT, UP_LEFT = 5, 6, 7, 8


def in_range(v, lmin=LMIN, lmax=LMAX):
    return lmin <= v <= lmax


def popcount(x):
    return bin(x).count("1")


# ---------------------------------------------------------------- 三版判据

def pick_mode(hist, degree, lmin=LMIN, lmax=LMAX):
    """v1：众数。旧实现的历史判据，仅用于对比。"""
    best, best_cnt = -1, 0
    for v in range(HIST):
        if hist[v] and hist[v] > best_cnt:
            best, best_cnt = v, hist[v]
    return best


def pick_degree(hist, degree, lmin=LMIN, lmax=LMAX):
    """v2：转移度数降序 → 出现次数升序 → 区间外优先 → 取最小。"""
    best, best_deg, best_cnt = -1, 0, 0
    for v in range(HIST):
        if hist[v] == 0:
            continue
        deg, cnt = popcount(degree[v]), hist[v]
        if best < 0:
            best, best_deg, best_cnt = v, deg, cnt
            continue
        if deg != best_deg:
            if deg > best_deg:
                best, best_deg, best_cnt = v, deg, cnt
            continue
        if cnt != best_cnt:
            if cnt < best_cnt:
                best, best_cnt = v, cnt
            continue
        best_in, cand_in = in_range(best, lmin, lmax), in_range(v, lmin, lmax)
        if best_in and not cand_in:
            best, best_cnt = v, cnt
        elif best_in == cand_in and v < best:
            best, best_cnt = v, cnt
    return best


def resolve_center(hist, degree, lmin=LMIN, lmax=LMAX):
    """v3：规范先验。与 gameboy_pad_ble.c 的 hat_resolve_center() 对齐。"""
    oor, oor_cnt = -1, 0
    for v in range(HIST):
        if hist[v] == 0 or in_range(v, lmin, lmax) or degree[v] == 0:
            continue
        if oor < 0 or hist[v] > oor_cnt:
            oor, oor_cnt = v, hist[v]
    if oor >= 0:
        return oor
    if hist[0] and degree[0]:
        return 0
    return -1


# ---------------------------------------------------------------- 解码状态机

class HatDecoder:
    def __init__(self, picker, seeded=False):
        self.hist = [0] * HIST
        self.degree = [0] * HIST
        self.picker = picker
        self.seeded = seeded       # 旧实现：首样本直接当锚点
        self.have_anchor = False
        self.ready = False
        self.frozen = False
        self.anchor = 0
        self.prev_raw = 0
        self.prev_valid = False
        self.transitions = 0
        self.first_ms = 0

    def feed(self, raw, now_ms):
        if raw < HIST:
            self.hist[raw] += 1
            if self.prev_valid and self.prev_raw != raw:
                self.degree[self.prev_raw] |= 1 << raw
                self.degree[raw] |= 1 << self.prev_raw
                self.transitions += 1
            self.prev_raw, self.prev_valid = raw, True

            if not self.have_anchor:
                self.have_anchor = True
                self.first_ms = now_ms
                if self.seeded:
                    self.anchor, self.ready = raw, True
            elif not self.frozen:
                if self.transitions < 1:
                    pass
                else:
                    cand = self.picker(self.hist, self.degree)
                    if cand >= 0:
                        if not self.ready:
                            self.ready, self.anchor = True, cand
                        elif cand != self.anchor:
                            self.anchor = cand
                    if self.ready and (
                        self.transitions >= 8
                        or (self.transitions >= 3
                            and now_ms - self.first_ms >= 4000)
                    ):
                        self.frozen = True

        if not self.ready or raw == self.anchor:
            return 8
        if self.anchor == 0:
            return (raw - 1) & 7
        if raw < LMIN or raw > LMAX:
            return 8
        return (raw - LMIN) & 7


def is_down(d):
    return d in (3, 4, 5)


def seq_of(*pairs):
    """[(raw, t_ms), ...]，t 缺省自动 100ms 递增。"""
    out, t = [], 0
    for raw in pairs:
        out.append((raw, t))
        t += 300
    return out


def run(label, picker, seq, want_down_at, counted, seeded=False):
    """counted=False 的历史判据只作对照，不计入通过与否。

    它们在这些场景上**本来就该错** —— 脚本的价值正是把"错在哪"固定下来，
    免得以后有人把旧判据改回来还以为没问题。
    """
    dec = HatDecoder(picker, seeded=seeded)
    got = {i for i, (r, t) in enumerate(seq) if is_down(dec.feed(r, t))}
    ok = got == want_down_at
    if counted:
        tag = "PASS" if ok else "FAIL"
    else:
        tag = "对照" if not ok else "对照(偶对)"
    print(f"    [{tag:9s}] {label:14s} 下出现在 {sorted(got)}"
          + ("" if ok else f"  期望 {sorted(want_down_at)}"))
    return 0 if (not counted or ok) else 1


VERSIONS = [
    ("v1 众数",     pick_mode,       False),
    ("v2 转移度数", pick_degree,     False),
    ("v3 规范先验", resolve_center,  True),
]


def scenario(title, seq, want, note=""):
    print(f"\n{title}")
    if note:
        print(f"    {note}")
    bad = 0
    for name, picker, counted in VERSIONS:
        bad += run(name, picker, seq, want, counted)
    return bad


def main():
    print("hat 静止中位判据 —— 三版对比回归")
    print("=" * 66)
    fails = 0

    # 1. 连上后先按下并长按：Q36 不发重复报文，整个按住过程只有开头一包
    fails += scenario(
        "[1] 连上先按「下」并长按（只有 1 包，raw=5）",
        seq_of(DOWN, DOWN, DOWN),
        set(),
        "此时期望：不出方向（信息不足，宁可不出也不能乱出）")

    # 2. 按下 → 松开 → 再按下：第二轮必须出「下」
    fails += scenario(
        "[2] 下 → 松开 → 下",
        seq_of(DOWN, CENTER, DOWN),
        {2},
        "松开那包是第一条转移边，之后必须能正常出下")

    # 3. 按住下 + 连点 A：每点一下 A 都带一包 hat 仍是 5 的报文
    fails += scenario(
        "[3] 按住「下」的同时连点 A（6 包 raw 恒为 5）",
        seq_of(DOWN, DOWN, DOWN, DOWN, DOWN, DOWN, DOWN),
        set(),
        "众数判据的死穴：那个方向的计数被刷爆")

    # 4. 沿十字键转一圈不回中心：下 → 下右 → 右 → 上右 → 上
    fails += scenario(
        "[4] 沿 hat 转一圈（下→下右→右→上右→上），不回中心",
        seq_of(DOWN, DOWN_RIGHT, RIGHT, UP_RIGHT, UP),
        set(),
        "度数判据的死穴：多个取值度数完全并列，会挑中随手一个")

    # 5. 转一圈之后松手回中心，再按「下」必须出下
    fails += scenario(
        "[5] 转一圈 → 松手回中心 → 再按「下」",
        seq_of(DOWN, DOWN_RIGHT, RIGHT, UP_RIGHT, UP, CENTER, DOWN),
        {6},
        "见过中心之后必须立刻判对")

    # 6. 连上就按住不放 5 秒（时间兜底不能锁死首样本）
    fails += scenario(
        "[6] 连上就按住「下」不放 5 秒（1 个取值，0 条转移边）",
        seq_of(DOWN, DOWN, DOWN, DOWN, DOWN, DOWN),
        set(),
        "PAD_HAT_MIN_TRANSITIONS 必须挡住时间兜底")

    # 7. 完整玩法：上下左右各按一次，每次都松开
    fails += scenario(
        "[7] 常规玩法：上 松 下 松 左 松 右 松",
        seq_of(UP, CENTER, DOWN, CENTER, LEFT, CENTER, RIGHT, CENTER),
        {2},
        "只有「下」那一包应判出下")

    # 8. 存档命中（中位已从 NVS 恢复）：连上第一包就该出「下」
    print("\n[8] 存档命中（s_hat_ready 开局即为真）—— 第一包就要能出下")
    print("    这是把中位持久化到 NVS 的唯一目的：免掉「先按一下没反应」")
    dec = HatDecoder(resolve_center)
    dec.ready = True            # pad_hat_nvs_load() 的效果
    dec.anchor = CENTER
    dec.have_anchor = True
    seq = seq_of(DOWN, CENTER, DOWN)
    got = {i for i, (r, t) in enumerate(seq) if is_down(dec.feed(r, t))}
    ok = got == {0, 2}
    print(f"    [{'PASS' if ok else 'FAIL'}] {'存档命中':14s} 下出现在 {sorted(got)}"
          + ("" if ok else "  期望 [0, 2]"))
    if not ok:
        fails += 1

    print("\n" + "=" * 66)
    if fails:
        print(f"结果：{fails} 项未通过")
        return 1
    print("结果：全部通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
