#!/usr/bin/env python3
"""Hat 锚点选取策略的离线仿真（镜像 main/game/gameboy_pad_ble.c 的策略）。

为什么要这个脚本：hat 锚点判据被推翻过两次，两次都是"玩着玩着方向键错乱"，
而且都得靠重启才恢复 —— 因为锚点只活在内存里，真机复现一次要几分钟还得
人工按手柄。这里把判据抽出来喂脚本，改判据时先跑一遍，几秒钟就能知道有没有
把老毛病改回来。

⚠ 这是**策略镜像**不是共用代码：改 gameboy_pad_ble.c 里的 hat_pick_anchor()
/ 校准窗口常量时，必须同步改这里，否则这份仿真就失去意义了。

覆盖的场景里有两个是老版本必翻车的，务必保住：
  - "只按过右键就长按"：老版数报文条数，长按 + 连点 A 会让右键那个桶反超中位，
    锚点被换成右键，于是按住右键变成居中、松开变成"上"。
  - "连上就按住右不放"：转移边一直是 0，时间兜底会把"连上时手指正按着的那个
    方向"锁成锚点，所以时间兜底要求至少 3 条边。

用法：python3 tests/hat_anchor_policy_sim.py
"""

HIST = 16
CALIB_TRANS = 8
CALIB_MIN_TRANS = 3
CALIB_MS = 4000


class Hat:
    def __init__(self, lmin, lmax, center):
        self.lmin, self.lmax, self.center = lmin, lmax, center
        self.hist = [0] * HIST
        self.deg = [0] * HIST
        self.anchor = None
        self.have = False
        self.prev = None
        self.trans = 0
        self.first_ms = 0
        self.frozen = False
        self.distinct = 0

    def pc(self, x):
        return bin(x).count('1')

    def in_range(self, v):
        return self.lmin <= v <= self.lmax

    def pick(self):
        best, best_deg, best_cnt = -1, 0, 0
        for v in range(HIST):
            if self.hist[v] == 0:
                continue
            d, c = self.pc(self.deg[v]), self.hist[v]
            if best < 0:
                best, best_deg, best_cnt = v, d, c
                continue
            if d != best_deg:
                if d > best_deg:
                    best, best_deg, best_cnt = v, d, c
                continue
            if c != best_cnt:
                if c < best_cnt:
                    best, best_cnt = v, c
                continue
            bi, ci = self.in_range(best), self.in_range(v)
            if bi and not ci:
                best, best_cnt = v, c
            elif bi == ci and v < best:
                best, best_cnt = v, c
        return best

    def feed(self, raw, ms):
        if raw >= HIST:
            return
        if self.hist[raw] == 0:
            self.distinct += 1
        self.hist[raw] += 1
        if self.prev is not None and self.prev != raw and self.prev < HIST:
            self.deg[self.prev] |= 1 << raw
            self.deg[raw] |= 1 << self.prev
            self.trans += 1
        self.prev = raw
        if not self.have:
            self.have = True
            self.anchor = raw
            self.first_ms = ms
        elif not self.frozen:
            cand = self.pick()
            if cand >= 0 and cand != self.anchor:
                self.anchor = cand
            now = ms
            enough = (self.trans >= CALIB_TRANS or
                      (self.trans >= CALIB_MIN_TRANS and
                       (now - self.first_ms) >= CALIB_MS))
            if enough:
                self.frozen = True

    def dir_of(self, raw):
        if raw == self.anchor:
            return 8
        if self.anchor == 0:
            return (raw - 1) & 7
        if raw < self.lmin or raw > self.lmax:
            return 8
        return (raw - self.lmin) & 7


NAMES = ['Up', 'UR', 'Right', 'DR', 'Down', 'DL', 'Left', 'UL']
FA = {n: i + 1 for i, n in enumerate(NAMES)}     # 族 A：0=中，1..8
FB = {n: i for i, n in enumerate(NAMES)}         # 族 B：0..7，8=中


def run(lmin, lmax, center, mapping, script, label):
    h = Hat(lmin, lmax, center)
    ms = 0
    wrong = total = 0
    frozen_at = None
    for name, n in script:
        raw = center if name is None else mapping[name]
        for _ in range(n):
            h.feed(raw, ms)
            ms += 30
            got = h.dir_of(raw)
            want = 8 if name is None else NAMES.index(name)
            total += 1
            if h.frozen:
                if frozen_at is None:
                    frozen_at = ms
                if got != want:
                    wrong += 1
    good = (h.anchor == center) and wrong == 0
    print(f'{"OK " if good else "FAIL"} {label:44s} anchor={h.anchor} '
          f'(应为 {center}) frozen@{frozen_at}ms 错判={wrong}/{total}')
    return good


cases = [
    (0, 7, 0, FA, [('Right', 1), (None, 1), ('Right', 1), (None, 1),
                   ('Left', 1), (None, 1), ('Down', 1), (None, 1),
                   ('Right', 400)],
     'A 选ROM上下左右各一下，之后长按右'),
    (0, 7, 0, FA, [('Right', 1), (None, 1), ('Right', 200)],
     'A 只按过右键就长按（老版必翻车）'),
    (0, 7, 0, FA, [('Right', 1), (None, 1), ('Left', 1), (None, 1),
                   ('Down', 1), (None, 1), ('Up', 1), (None, 1),
                   ('Right', 300)],
     'A 长按右期间连点A键（hat 值不变）'),
    (0, 7, 8, FB, [('Right', 1), (None, 1), ('Left', 1), (None, 1),
                   ('Down', 1), (None, 1), ('Up', 1), (None, 1),
                   ('Right', 400)],
     'B 各方向都按过，之后长按右'),
    (0, 7, 8, FB, [('Up', 1), (None, 1), ('Up', 200)],
     'B 只按过上键就长按'),
    (0, 7, 0, FA, [('Right', 1), (None, 1), ('Right', 1), (None, 1),
                   ('Right', 1), (None, 1), ('Right', 1), (None, 1),
                   ('Right', 300)],
     'A 连上时右键已按住，反复松按'),
    (0, 7, 0, FA, [('Right', 1), (None, 1), ('Right', 1), (None, 1),
                   ('Right', 1), (None, 1), ('Right', 500)],
     'A 连上就按住右不放（转移边为0）'),
    (0, 7, 0, FA, [('Down', 1), (None, 1), ('Down', 1), (None, 1),
                   ('Up', 1), (None, 1), ('Down', 1), (None, 1),
                   ('Down', 1), (None, 1), ('Right', 1), (None, 1),
                   ('Right', 1), (None, 1), ('Left', 1), (None, 1),
                   ('Down', 600)],
     'A 正常玩：上下选ROM后长按下'),
    (0, 7, 8, FB, [('Down', 1), (None, 1), ('Up', 1), (None, 1),
                   ('Down', 1), (None, 1), ('Up', 1), (None, 1),
                   ('Right', 1), (None, 1), ('Left', 1), (None, 1),
                   ('Down', 600)],
     'B 正常玩：上下选ROM后长按下'),
    (0, 7, 15, {**FB}, [('Right', 1), (None, 1), ('Left', 1), (None, 1),
                        ('Down', 1), (None, 1), ('Right', 400)],
     'B(15=中) 各方向都按过'),
]

ok = all(run(*c) for c in cases)
print()
print('全部通过' if ok else '有 FAIL，见上')
