#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""逐字节验证 name_chars_allowed()：黑名单放行面 + UTF-8 每个字节都过得去。

背景（这个脚本为什么存在）：
  FATFS API 编码配成 UTF-8 之后，readdir() 交出来的中文文件名是 UTF-8 字节
  （首字节 0xC2-0xF4、续字节 0x80-0xBF）。更要紧的是，name_chars_allowed()
  的策略从「ASCII 白名单」翻转过「黑名单」——因为白名单漏一个字符就是静默
  丢掉一个 ROM：用户看着游戏页显示「请插入 TF 卡」，而串口日志打着
  rom count=8，两边矛盾且没有任何一方报错。真实案例是用户那批名字
  「魂斗罗+(简)+(unknown)(2Mb).gb」里的 '+'、'('、')'。

做法（重点是"不重抄代码"）：
  1. 从 main/game/gameboy_rom_store.cpp 里**原样抽取** name_chars_allowed()
     的函数体文本，连同 main/game/game_config.h 里的 kMaxRomFileNameBytes 值；
  2. 拼成一个主机端 C++ 程序，把抽出来的函数编译进去；
  3. Python 侧独立实现一份黑名单模型，逐条生成测试向量喂给 C++；
  4. 两边结果逐条比对，任何不一致都带 hex 原文报出来。

穷举覆盖：
  - 0x00-0xFF 全部 256 个单字节（各自嵌进 "aX.gb" 里各测一次）
  - UTF-8 首字节 0xC2-0xF4（51 个）单独一组
  - UTF-8 续字节 0x80-0xBF（64 个）单独一组
  - 用户真实文件名（含 + ( ）整名 + 每个字节单独拆出来测
  - 含空格的名字、各种符号名
  - 必须拒绝的字节（控制字符 / DEL / 9 个 FAT 非法字符）
  - ".." 路径穿越、长度边界、内嵌 NUL

变异测试（证明这个脚本不是永远 PASS）：
  默认跑真实源码；加 --mutate 时逐个注入缺陷，每个都必须被抓到。

用法：
    python3 tools/verify_utf8_name_chars.py
    python3 tools/verify_utf8_name_chars.py --mutate
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CPP_PATH = os.path.join(ROOT, "main", "game", "gameboy_rom_store.cpp")
CONFIG_PATH = os.path.join(ROOT, "main", "game", "game_config.h")

FUNC_SIG = "bool name_chars_allowed(const char *name)"


def extract_function(path):
    """从真实源码里原样抽出 name_chars_allowed() 的完整文本。

    抽不到就直接退出：说明函数被改名或删了，测试不该静默跳过——
    一个永远通过的测试比没有测试更糟。
    """
    with open(path, "r", encoding="utf-8") as handle:
        lines = handle.read().split("\n")

    start = None
    for i, line in enumerate(lines):
        if line.startswith(FUNC_SIG):
            start = i
            break
    if start is None:
        sys.exit("FATAL: 在 %s 里找不到 %s" % (path, FUNC_SIG))

    end = None
    for j in range(start + 1, len(lines)):
        if lines[j] == "}":
            end = j
            break
    if end is None:
        sys.exit("FATAL: name_chars_allowed() 没找到收尾的 '}'")

    # 签名行 + 函数体，保持原缩进原样返回，不做任何改写。
    return "\n".join(lines[start : end + 1])


def extract_max_name_bytes(path):
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    m = re.search(r"kMaxRomFileNameBytes\s*=\s*(\d+)", text)
    if not m:
        sys.exit("FATAL: 在 %s 里找不到 kMaxRomFileNameBytes" % path)
    return int(m.group(1))


# ---------------------------------------------------------------------------
# Python 侧独立实现的黑名单模型。
# 这里刻意照着"规格"重写一遍而不是翻译 C 代码，这样两边是真正独立的：
# 如果 C 实现有 bug，两边会不一致，而不是同步错。
# ---------------------------------------------------------------------------
# FAT 非法字符 + 路径分隔符 + 通配符。这 9 个是"确定有害"的。
FAT_ILLEGAL = '/\\:*?"<>|'


def model(name_bytes, max_name_bytes):
    """按 C 字符串语义复现 name_chars_allowed() 的判定。"""
    # strlen 在第一个 NUL 处停止，内嵌 NUL 之后的字节根本不参与判定。
    s = name_bytes.split(b"\x00")[0]
    if len(s) == 0 or len(s) >= max_name_bytes:
        return False
    for b in s:
        # 控制字符与 DEL：不可见，日志/UI 会错位。
        if b < 0x20 or b == 0x7F:
            return False
        # 黑名单：确定有害的 9 个字节。其余一律放行（含空格与各种符号）。
        if b in (ord(c) for c in FAT_ILLEGAL):
            return False
    if b".." in s:
        return False
    return True


# 用户设备上真实出现的 ROM 名（从串口日志原文抄，含 '+' '(' ')'）。
# 这批名字在白名单时代全被静默过滤，是这次翻转策略的直接原因。
USER_REAL_NAMES = [
    "魂斗罗+(简)+(unknown)(2Mb).gb",
    "坦克大战+(简)+(madcell)(0.25Mb).gb",
    "噗呦噗呦+(简)+(CKN)(2Mb).gb",
    "口袋妖怪黄+(繁)+(修正版)+(yishiluo)(8Mb).gb",
]

# 其他常见中文 ROM 名。
REAL_NAMES = [
    "魂斗罗.gb",
    "坦克大战.gb",
    "噗呦噗呦.gbc",
    "口袋妖怪黄.gbc",
    "俄罗斯方块.gb",
    "超级马里奥.gb",
    "塞尔达传说.gb",
    "星之卡比.gbc",
]

# 含空格与各种符号的名字。这些在白名单时代全被拒，现在必须全放行。
SYMBOL_NAMES = [
    "My Game.gb",
    "game (v1.0) [beta].gbc",
    "a+b(c)[d]{e}!f@g#h$i%j^k&l~m'n,o;p=q.gb",
    "100%.gb",
    "  leading and trailing  .gb",
    "超级马里奥 兄弟.gbc",
    "Zelda - Link's Awakening.gb",
    "Contra (U) [!].gb",
]

# 必须被拒绝的字节：C0 控制字符、DEL、以及 FAT/Windows 路径里的 9 个危险字符。
MUST_REJECT_BYTES = (
    list(range(0x00, 0x20)) + [0x7F] + [ord(c) for c in FAT_ILLEGAL]
)

# 0xF5-0xFF：UTF-8 合法首字节上限是 0xF4，0xF5-0xFF 在 UTF-8 里非法，
# 但黑名单策略下它们不"确定有害"，所以放行。单独列出来点名这个决定。
UPPER_EDGE_BYTES = [0xF5, 0xFE, 0xFF]


def build_vectors(max_name_bytes):
    """返回 [(group, name_bytes, expected_bool), ...]"""
    vectors = []

    # 1) 穷举：每一个单字节都嵌进 "aX.gb" 测一次。这是最核心的一条。
    for b in range(0x00, 0x100):
        name = b"a" + bytes([b]) + b".gb"
        vectors.append(("single_byte_0x%02X" % b, name, model(name, max_name_bytes)))

    # 2) UTF-8 首字节 0xC2-0xF4，单独点名。
    for b in range(0xC2, 0xF5):
        name = b"a" + bytes([b]) + b".gb"
        vectors.append(("utf8_lead_0x%02X" % b, name, model(name, max_name_bytes)))

    # 3) UTF-8 续字节 0x80-0xBF，单独点名。
    for b in range(0x80, 0xC0):
        name = b"a" + bytes([b]) + b".gb"
        vectors.append(("utf8_cont_0x%02X" % b, name, model(name, max_name_bytes)))

    # 4) 上限边缘字节。
    for b in UPPER_EDGE_BYTES:
        name = b"a" + bytes([b]) + b".gb"
        vectors.append(("upper_edge_0x%02X" % b, name, model(name, max_name_bytes)))

    # 5) 用户真实文件名整名判定——这次 bug 的直接回归用例。
    for name in USER_REAL_NAMES:
        nb = name.encode("utf-8")
        vectors.append(("user_real_name", nb, model(nb, max_name_bytes)))

    # 6) 用户真实文件名的每一个字节单独拆出来测。
    #    "整名过"不能推出"每个字节过"——万一实现里有按位置的特殊分支，
    #    整名测试是发现不了的。这一组就是堵这个口子。
    for name in USER_REAL_NAMES:
        for b in name.encode("utf-8"):
            nb = b"a" + bytes([b]) + b".gb"
            vectors.append(("user_real_byte_0x%02X" % b, nb, model(nb, max_name_bytes)))

    # 7) 其他中文名整名 + 逐字节。
    for name in REAL_NAMES:
        nb = name.encode("utf-8")
        vectors.append(("real_cjk_name", nb, model(nb, max_name_bytes)))
        for b in nb:
            sb = b"a" + bytes([b]) + b".gb"
            vectors.append(("real_cjk_byte_0x%02X" % b, sb, model(sb, max_name_bytes)))

    # 8) 含空格与各种符号的名字，必须全部放行。
    for name in SYMBOL_NAMES:
        nb = name.encode("utf-8")
        vectors.append(("symbol_name", nb, model(nb, max_name_bytes)))

    # 9) 必须拒绝的字节。
    for b in MUST_REJECT_BYTES:
        name = b"a" + bytes([b]) + b".gb"
        vectors.append(("must_reject_0x%02X" % b, name, model(name, max_name_bytes)))

    # 10) 路径穿越与合法 ASCII 名。这里刻意多放几条带 '/' 与 '\\' 的：
    #     少了它们，"黑名单漏掉 '/'" 这个变异只能被 3 条向量抓到，太脆。
    for name in [
        b"..",
        b"..gb",
        b"a..gb",
        b"..\\..\\x.gb",
        b"a/b.gb",
        b"a\\b.gb",
        b"../x.gb",
        b"/etc/passwd",
        b"rom/gb/x.gb",
        b"C:\\windows\\x.gb",
        b"a:b.gb",
        b"a*b.gb",
        b"a?b.gb",
        b'a"b.gb',
        b"a<b.gb",
        b"a>b.gb",
        b"a|b.gb",
        b"Tetris.gb",
        b"a_b-c.gb",
        b"A1.gbc",
    ]:
        vectors.append(("ascii_case", name, model(name, max_name_bytes)))

    # 11) 长度边界：length < kMaxRomFileNameBytes 才放行。
    ok_len = b"a" * (max_name_bytes - 1)
    over_len = b"a" * max_name_bytes
    vectors.append(("length_max_ok", ok_len, model(ok_len, max_name_bytes)))
    vectors.append(("length_over", over_len, model(over_len, max_name_bytes)))
    cjk_ok = "俄" * 31 + ".gb"  # 31*3 + 3 = 96 字节 == 上限，应被拒
    vectors.append(("length_cjk_at_limit", cjk_ok.encode("utf-8"),
                    model(cjk_ok.encode("utf-8"), max_name_bytes)))
    cjk_under = "俄" * 30 + ".gb"  # 30*3 + 3 = 93 字节 < 96，应放行
    vectors.append(("length_cjk_under_limit", cjk_under.encode("utf-8"),
                    model(cjk_under.encode("utf-8"), max_name_bytes)))
    # 空格名也要过长度边界（空格现在合法，不能再拿它当填充以外的特殊字符）。
    sp_ok = b" " * (max_name_bytes - 1)
    vectors.append(("length_space_max_ok", sp_ok, model(sp_ok, max_name_bytes)))

    # 12) 内嵌 NUL：strlen 会在 NUL 处截断，判定只看截断后的部分。
    for name in [
        b"a\x00/gb",
        b"\x00abc.gb",
        b"\xd0\xa6.gb\x00\x2f\x2e\x2e",
        b"a b\x00/gb",  # 空格合法，但 NUL 之后的 '/' 不该被看到
    ]:
        vectors.append(("embedded_nul", name, model(name, max_name_bytes)))

    return vectors


# ---------------------------------------------------------------------------
# 变异测试：把真实函数改坏，确认脚本真的会 FAIL。
# 每个变异都是对抽取出来的函数文本做精确字符串替换，换不到就报错退出——
# 说明源码结构变了，测试需要跟着更新，而不是静默跳过。
# ---------------------------------------------------------------------------
BLACKLIST_IF = """        if (c == '/' || c == '\\\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
        }"""

DOTDOT_IF = """    if (strstr(name, "..") != nullptr) {
        return false;
    }"""

CONTROL_IF = """        if (c < 0x20 || c == 0x7F) {
            return false;
        }"""

LEN_IF = "length >= game_config::kMaxRomFileNameBytes"

WHITELIST_LOOP = """        if (c == '/' || c == '\\\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
        }"""

MUTATIONS = [
    (
        "黑名单漏掉 '/'（路径分隔符）",
        lambda f: f.replace("c == '/' || c == '\\\\'", "c == '\\\\'"),
    ),
    (
        "黑名单漏掉 '..' 路径穿越防护",
        lambda f: f.replace(DOTDOT_IF, ""),
    ),
    (
        "黑名单漏掉控制字符检查",
        lambda f: f.replace(CONTROL_IF, "        if (false) {\n            return false;\n        }"),
    ),
    (
        "黑名单漏掉 ':' '|' '?' 等其余危险字符",
        lambda f: f.replace(
            "c == ':' || c == '*' || c == '?' ||\n            c == '\"' || c == '<' || c == '>' || c == '|'",
            "c == '*'",
        ),
    ),
    (
        "长度上限 off-by-one（>= 改成 >）",
        lambda f: f.replace(LEN_IF, LEN_IF.replace(">=", ">")),
    ),
    (
        "退回 ASCII 白名单（把黑名单整段换成 alnum/._-/0x80-0xFE）",
        lambda f: f.replace(
            BLACKLIST_IF,
            """        const bool alnum = (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9');
        if (alnum || c == '.' || c == '_' || c == '-') {
            continue;
        }
        if (c >= 0x80 && c <= 0xFE) {
            continue;
        }
        return false;""",
        ),
    ),
]


# 主机端测试程序：把抽出来的真实函数编译进来，从 stdin 读向量，逐条执行。
RUNNER = r"""// 本文件由 tools/verify_utf8_name_chars.py 自动生成，不要手工编辑。
// 里面的 name_chars_allowed() 是从 main/game/gameboy_rom_store.cpp 原样抽取的。
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace game_config {
inline constexpr size_t kMaxRomFileNameBytes = __MAX_NAME_BYTES__;
}

__FUNCTION__

int main()
{
    char line[16384];
    long n = 0;
    long fail = 0;
    while (fgets(line, sizeof(line), stdin) != nullptr) {
        char *nl = strchr(line, '\n');
        if (nl != nullptr) {
            *nl = '\0';
        }
        if (line[0] == '\0') {
            continue;
        }
        // 行格式：group|hexbytes|expected
        char *bar1 = strchr(line, '|');
        char *bar2 = strrchr(line, '|');
        if (bar1 == nullptr || bar2 == nullptr || bar1 == bar2) {
            printf("MALFORMED %s\n", line);
            return 3;
        }
        *bar1 = '\0';
        *bar2 = '\0';
        const char *group = line;
        const char *hex = bar1 + 1;
        const int expect = atoi(bar2 + 1);

        static char buf[8192];
        size_t blen = 0;
        const char *h = hex;
        while (h[0] != '\0' && h[1] != '\0' && blen + 1 < sizeof(buf)) {
            unsigned v = 0;
            if (sscanf(h, "%2x", &v) != 1) {
                printf("BADHEX %s\n", hex);
                return 3;
            }
            buf[blen++] = (char)(unsigned char)v;
            h += 2;
        }
        buf[blen] = '\0';

        const bool got = name_chars_allowed(buf);
        if (got != (expect != 0)) {
            printf("FAIL group=%s expect=%d got=%d hex=", group, expect,
                   (int)got);
            for (size_t i = 0; i < blen; ++i) {
                printf("%02X ", (unsigned char)buf[i]);
            }
            printf("\n");
            ++fail;
        }
        ++n;
    }
    printf("TOTAL %ld FAIL %ld\n", n, fail);
    return fail == 0 ? 0 : 1;
}
"""


def build_source(func_text, max_name_bytes):
    src = RUNNER.replace("__FUNCTION__", func_text)
    src = src.replace("__MAX_NAME_BYTES__", str(max_name_bytes))
    if "__FUNCTION__" in src or "__MAX_NAME_BYTES__" in src:
        sys.exit("FATAL: 模板占位符没替换干净")
    return src


def run_vectors(func_text, vectors, max_name_bytes, label):
    """编译 + 跑一组向量，返回 (passed_bool, total, fail, stdout)。"""
    src = build_source(func_text, max_name_bytes)
    stdin_data = "".join(
        "%s|%s|%d\n" % (g, n.hex(), 1 if e else 0) for g, n, e in vectors
    )
    with tempfile.TemporaryDirectory() as tmp:
        cpp = os.path.join(tmp, "t.cpp")
        binary = os.path.join(tmp, "t")
        with open(cpp, "w", encoding="utf-8") as handle:
            handle.write(src)
        try:
            subprocess.run(
                ["g++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-o", binary, cpp],
                check=True,
                capture_output=True,
            )
        except subprocess.CalledProcessError as exc:
            print("%s: 编译失败" % label)
            print(exc.stderr.decode("utf-8", "replace"))
            return False, 0, 0, ""
        proc = subprocess.run(
            [binary], input=stdin_data, capture_output=True, text=True
        )

    total = fail = 0
    for line in proc.stdout.split("\n"):
        if line.startswith("TOTAL"):
            _t, total_s, _f, fail_s = line.split()
            total, fail = int(total_s), int(fail_s)
    ok = proc.returncode == 0 and fail == 0
    return ok, total, fail, proc.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mutate", action="store_true",
                    help="逐个注入缺陷，确认脚本每个都能抓到")
    args = ap.parse_args()

    func_text = extract_function(CPP_PATH)
    max_name_bytes = extract_max_name_bytes(CONFIG_PATH)
    print("抽取自 %s" % os.path.relpath(CPP_PATH, ROOT))
    print("  name_chars_allowed() %d 行（原样，未改写）" % func_text.count("\n"))
    print("  kMaxRomFileNameBytes = %d" % max_name_bytes)

    vectors = build_vectors(max_name_bytes)
    print("测试向量 %d 条" % len(vectors))

    ok, total, fail, out = run_vectors(func_text, vectors, max_name_bytes, "真实源码")
    print(out.strip())
    if not ok:
        sys.exit("FAILED: 真实源码未通过，先修代码再跑变异测试")

    # 分组统计，报告里要能点名每一类各多少条。
    groups = {}
    for g, _n, _e in vectors:
        key = g.split("_0x")[0]
        groups[key] = groups.get(key, 0) + 1
    for key in sorted(groups):
        print("  %-24s %d" % (key, groups[key]))

    # 关键点名的结论，直接打出来，别让读报告的人自己去数。
    lead = range(0xC2, 0xF5)
    cont = range(0x80, 0xC0)
    ok_lead = all(model(b"a" + bytes([b]) + b".gb", max_name_bytes) for b in lead)
    ok_cont = all(model(b"a" + bytes([b]) + b".gb", max_name_bytes) for b in cont)
    print("UTF-8 首字节 0xC2-0xF4（%d 个）全部放行: %s" % (len(list(lead)), ok_lead))
    print("UTF-8 续字节 0x80-0xBF（%d 个）全部放行: %s" % (len(list(cont)), ok_cont))

    # 策略翻转前后，哪些字节从「被拒」变成「放行」。报告里要能点名。
    # 探针不能用 "aX.gb"：X 取 '.' 时会得到 "a..gb"，撞上 ".." 规则，
    # 把本来放行的 '.' 误判成被拒。用 "zXz.gb"，任何单字节都形不成 ".."。
    def old_whitelist_allows(b):
        is_alnum = (ord("A") <= b <= ord("Z")) or (ord("a") <= b <= ord("z")) or (
            ord("0") <= b <= ord("9")
        )
        return is_alnum or b in (ord("."), ord("_"), ord("-")) or 0x80 <= b <= 0xFE

    def probe(b):
        return model(b"z" + bytes([b]) + b"z.gb", max_name_bytes)

    # 0x00 特殊：它不是"被放行"，而是截断了 C 字符串，之后的字节根本不参与判定。
    # 单独说明，不并进"新放行"清单。
    scannable = [b for b in range(0x01, 0x100)]
    newly = [b for b in scannable if not old_whitelist_allows(b) and probe(b)]
    removed = [b for b in scannable if old_whitelist_allows(b) and not probe(b)]
    print()
    print("策略翻转：%d 个字节从「被拒」变为「放行」" % len(newly))
    print("  可打印部分: %s" % "".join(chr(b) for b in newly if 0x20 <= b <= 0x7E))
    print("  非打印部分: %s" % " ".join("0x%02X" % b for b in newly if not (0x20 <= b <= 0x7E)))
    print("  仍被拒的可打印字符: %s" % "".join(
        chr(b) for b in range(0x20, 0x7F) if not probe(b)))
    print("  0x00 不参与判定（C 字符串在此终止），不算放行也不算拒绝")
    if removed:
        print("  警告：有字节从「放行」变为「被拒」: %s" %
              " ".join("0x%02X" % b for b in removed))
    else:
        print("  没有任何字节从「放行」退化为「被拒」")
    print("全部通过")

    if not args.mutate:
        return

    # ---- 变异测试 ----
    print()
    print("=" * 70)
    print("变异测试：逐个把函数改坏，每个都必须被抓到")
    print("=" * 70)
    all_caught = True
    for label, mutate in MUTATIONS:
        mutated = mutate(func_text)
        if mutated == func_text:
            print("  [跳过] %s —— 替换没命中，源码结构可能变了" % label)
            all_caught = False
            continue
        ok_m, total_m, fail_m, out_m = run_vectors(
            mutated, vectors, max_name_bytes, "变异: " + label
        )
        if ok_m:
            print("  [漏报!] %s —— 改了却没被抓到，测试有盲区" % label)
            all_caught = False
        else:
            first = next(
                (l for l in out_m.split("\n") if l.startswith("FAIL")), "")
            print("  [抓住] %-42s %d/%d 条 FAIL" % (label, fail_m, total_m))
            if first:
                print("         例: %s" % first[:110])
    print()
    print("变异测试总结: %s" % ("全部抓住" if all_caught else "存在漏报或未命中"))
    if not all_caught:
        sys.exit(1)


if __name__ == "__main__":
    main()
