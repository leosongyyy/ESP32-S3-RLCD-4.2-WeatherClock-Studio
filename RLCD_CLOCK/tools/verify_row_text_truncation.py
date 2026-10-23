#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""验证 ui_game.cpp 的 format_row_text()：截断必须按 UTF-8 字符边界、保留后缀、不超宽。

背景：用户真实 ROM 名「口袋妖怪黄+(繁)+(修正版)+(yishiluo)(8Mb).gb」在
zh_font_16 下渲染 362px，加 "64. " 前缀共 393px，而行标签只有 364px。
行标签是 LV_LABEL_LONG_CLIP，不主动截就会被硬切，三字节汉字切一半会在屏幕上
画出乱码方块。所以 format_row_text() 做了按字符边界的截断。

做法（和前一个脚本一样，重点是"不重抄代码"）：
  1. 从 main/ui/ui_game.cpp 里**原样抽取** utf8_char_bytes() /
     row_text_width_px() / format_row_text() 三个函数，以及
     kRowTextWidthPx / kRowTruncationMarker 两个常量（kRowTextWidthPx 是
     "kListWidth - 16 - 6" 这样的表达式，会在 Python 侧把 kListWidth /
     kDisplayWidth / kListX 一路代到底）；
  2. 从 main/assets/zh_font_16.c 解析出真实的字形宽度，生成一张主机端宽度表，
     用一个假 lv_txt_get_width() 顶上——宽度必须是真的，否则测了个寂寞；
  3. 编译进主机端 C++，逐条跑测试向量。

断言（每条都是硬断言，不满足就 FAIL）：
  A. 放得下的名字原样透传，不做任何改动；
  B. 超宽的名字输出宽度 <= kRowTextWidthPx；
  C. 输出始终是合法 UTF-8——这条就是"不许把三字节汉字切一半"的机器判据；
  D. 输入以 .gb / .gbc 结尾时，输出也必须以同样的后缀结尾；
  E. 截断确实发生了（输出 != 原样），且含省略号标记；
  F. nullptr / 空串 / 无扩展名 / 极小 out_bytes 都不许崩、不许溢出。

用法：
    python3 tools/verify_row_text_truncation.py
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
UI_GAME = os.path.join(ROOT, "main", "ui", "ui_game.cpp")
GAME_CONFIG = os.path.join(ROOT, "main", "game", "game_config.h")
DISPLAY_CONFIG = os.path.join(ROOT, "main", "core", "app_display_config.h")
FONT_C = os.path.join(ROOT, "main", "assets", "zh_font_16.c")

FUNC_SIGS = [
    "size_t utf8_char_bytes(const char *p)",
    "int row_text_width_px(const char *text, size_t bytes)",
    "void format_row_text(char *out, size_t out_bytes, int index, const char *name)",
]

# 用户真实文件名（从串口日志原文抄）。
USER_REAL_NAMES = [
    "魂斗罗+(简)+(unknown)(2Mb).gb",
    "坦克大战+(简)+(madcell)(0.25Mb).gb",
    "噗呦噗呦+(简)+(CKN)(2Mb).gb",
    "口袋妖怪黄+(繁)+(修正版)+(yishiluo)(8Mb).gb",
]

# 放得下的短名（用于验证"原样透传，不做多余截断"）。
SHORT_NAMES = [
    "魂斗罗.gb",
    "坦克大战.gb",
    "俄罗斯方块.gb",
    "超级马里奥.gbc",
    "Tetris.gb",
    "My Game.gb",
]

# 人造的超宽名：全中文最容易把边界逼到极限。
LONG_CJK_NAMES = [
    "口袋妖怪黄+(繁)+(修正版)+(yishiluo)(8Mb).gb",
    "俄" * 30 + ".gb",                       # 93 字节，卡在长度上限内
    "最终幻想Ⅳ+" + "魂" * 26 + ".gbc",        # 混排
    "塞尔达传说织梦岛简体中文完全修正版终极收藏.gbc",
    "a" * 92 + ".gb",                         # 95 字节 ASCII，纯窄字符
    "M" * 92 + ".gb",                         # 95 字节 ASCII，全宽字符（16px）
]


def extract_functions(path):
    """从真实源码原样抽取若干函数，返回 {签名: 函数文本}。"""
    with open(path, "r", encoding="utf-8") as handle:
        lines = handle.read().split("\n")
    out = {}
    for sig in FUNC_SIGS:
        start = None
        for i, line in enumerate(lines):
            if line.startswith(sig):
                start = i
                break
        if start is None:
            sys.exit("FATAL: 在 %s 里找不到 %s" % (path, sig))
        end = None
        for j in range(start + 1, len(lines)):
            if lines[j] == "}":
                end = j
                break
        if end is None:
            sys.exit("FATAL: %s 没找到收尾的 '}'" % sig)
        out[sig] = "\n".join(lines[start : end + 1])
    return out


def extract_constants(ui_src, display_src):
    """抽出 kRowTextWidthPx / kRowTruncationMarker，并把表达式代成数字。"""
    m = re.search(r'constexpr int kRowTextWidthPx\s*=\s*([^;]+);', ui_src)
    if not m:
        sys.exit("FATAL: 找不到 kRowTextWidthPx")
    expr = m.group(1).strip()

    # 逐层代换：kRowTextWidthPx -> kListWidth -> kDisplayWidth/kListX
    known = {}
    for name, src in (("kListWidth", ui_src), ("kListX", ui_src),
                      ("kDisplayWidth", display_src)):
        mm = re.search(r"constexpr int %s\s*=\s*([^;]+);" % name, src)
        if not mm:
            sys.exit("FATAL: 找不到常量 %s" % name)
        known[name] = mm.group(1).strip()

    resolved = expr
    for _ in range(10):
        changed = False
        for name, val in known.items():
            if re.search(r"\b%s\b" % name, resolved):
                resolved = re.sub(r"\b%s\b" % name, "(" + val + ")", resolved)
                changed = True
        if not changed:
            break
    if not re.fullmatch(r"[0-9 ()+*/-]+", resolved):
        sys.exit("FATAL: kRowTextWidthPx 表达式没法解析成数字: %s" % resolved)
    width_px = eval(resolved, {"__builtins__": {}}, {})

    m2 = re.search(r'constexpr const char \*kRowTruncationMarker\s*=\s*"([^"]*)"', ui_src)
    if not m2:
        sys.exit("FATAL: 找不到 kRowTruncationMarker")
    marker = m2.group(1)
    return int(width_px), marker


def extract_display_name_bytes(path):
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    m = re.search(r"kRomDisplayNameBytes\s*=\s*(\d+)", text)
    if not m:
        sys.exit("FATAL: 在 %s 里找不到 kRomDisplayNameBytes" % path)
    return int(m.group(1))


# ---------------------------------------------------------------------------
# 从 zh_font_16.c 解析真实字形宽度，生成主机端宽度表。
# ---------------------------------------------------------------------------
def build_width_table():
    with open(FONT_C, "r", encoding="utf-8", errors="replace") as handle:
        src = handle.read()

    adv_raw = [int(x) for x in re.findall(
        r"\.adv_w\s*=\s*(\d+)",
        re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", src, re.S).group(1))]

    cmaps = []
    for mm in re.finditer(
            r"\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),\s*"
            r"\.glyph_id_start\s*=\s*(\d+),\s*\.unicode_list\s*=\s*(\w+),\s*"
            r"\.glyph_id_ofs_list\s*=\s*(\w+),\s*\.list_length\s*=\s*(\d+),\s*"
            r"\.type\s*=\s*(\w+)", src):
        rs, rl, gs, ul, gl, ll, ty = mm.groups()
        cmaps.append((int(rs), int(rl), int(gs), ul, gl, int(ll), ty))

    def parse_list(name):
        if name == "NULL":
            return None
        m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\}", src, re.S)
        return [int(x) for x in re.findall(r"\d+", m.group(1))] if m else None

    lists = {}
    for c in cmaps:
        for idx in (3, 4):
            if c[idx] != "NULL" and c[idx] not in lists:
                lists[c[idx]] = parse_list(c[idx])

    def glyph_id(cp):
        for rs, rl, gs, ul, gl, ll, ty in cmaps:
            if rs <= cp < rs + rl:
                off = cp - rs
                if ty.endswith("SPARSE_TINY") or ty.endswith("SPARSE_FULL"):
                    u = lists.get(ul)
                    if u is None:
                        return None
                    try:
                        return gs + u.index(cp)
                    except ValueError:
                        return None
                if ty.endswith("FORMAT0_FULL"):
                    g = lists.get(gl)
                    if g is None or off >= len(g):
                        return None
                    return None if g[off] == 0 else gs + g[off]
                return gs + off
        return None

    def px(cp):
        g = glyph_id(cp)
        if g is None or g >= len(adv_raw):
            return None
        # 与 lv_font_fmt_txt.c:177 一致：(adv_w + 8) >> 4
        return (adv_raw[g] + 8) >> 4

    return px


# 主机端测试程序。lv_txt_get_width 用真实宽度表顶上。
RUNNER = r"""// 本文件由 tools/verify_row_text_truncation.py 自动生成，不要手工编辑。
// utf8_char_bytes()/row_text_width_px()/format_row_text() 是从
// main/ui/ui_game.cpp 原样抽取的；宽度表是从 main/assets/zh_font_16.c
// 解析出来的真实字形宽度。
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

namespace game_config {
inline constexpr size_t kRomDisplayNameBytes = __DISPLAY_NAME_BYTES__;
}

constexpr int kRowTextWidthPx = __ROW_TEXT_WIDTH_PX__;
constexpr const char *kRowTruncationMarker = "__MARKER__";

// ---- LVGL 桩：只要宽度度量对，其他用不到 ----
typedef int32_t lv_coord_t;
typedef uint8_t lv_text_flag_t;
typedef struct { int unused; } lv_font_t;
static const lv_font_t zh_font_16 = {0};
#define LV_TEXT_FLAG_NONE 0x00

// 真实字形宽度表（UTF-32 码点 -> 像素）。ASCII 逐字精确，
// 非 ASCII 只列测试里用到的码点，查不到按 16px（CJK 就是这个值）。
static const struct { uint32_t cp; int w; } kWidths[] = {
__WIDTH_TABLE__
};

static int lookup_width(uint32_t cp)
{
    for (const auto &e : kWidths) {
        if (e.cp == cp) return e.w;
    }
    return (cp >= 0x80) ? 16 : 8;
}

// 按 UTF-8 逐字符解码后累加，和 lv_txt_get_width 的行为一致。
extern "C" lv_coord_t lv_txt_get_width(const char *txt, uint32_t length,
                                       const lv_font_t *, lv_coord_t, lv_text_flag_t)
{
    if (txt == nullptr || length == 0) return 0;
    lv_coord_t width = 0;
    uint32_t i = 0;
    while (i < length) {
        const unsigned char c = (unsigned char)txt[i];
        uint32_t cp = 0;
        uint32_t clen = 1;
        if (c < 0x80) { cp = c; clen = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < length) {
            cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)txt[i+1] & 0x3F); clen = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < length) {
            cp = ((uint32_t)(c & 0x0F) << 12) | (((unsigned char)txt[i+1] & 0x3F) << 6)
               | ((unsigned char)txt[i+2] & 0x3F); clen = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < length) {
            cp = ((uint32_t)(c & 0x07) << 18) | (((unsigned char)txt[i+1] & 0x3F) << 12)
               | (((unsigned char)txt[i+2] & 0x3F) << 6) | ((unsigned char)txt[i+3] & 0x3F);
            clen = 4;
        } else { cp = c; clen = 1; }
        width += lookup_width(cp);
        i += clen;
    }
    return width;
}

// ---- 抽取出来的真实函数 ----
__FUNCTIONS__

// ---- 测试用例名（由脚本注入，与 Python 侧同一份） ----
__NAMES__

// ---- 测试 ----
static int g_pass = 0;
static int g_fail = 0;
static int g_truncated_cases = 0;
static int g_passthrough_cases = 0;

static bool valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) { ++p; continue; }
        int need;
        uint32_t cp;
        if ((*p & 0xE0) == 0xC0) { need = 1; cp = *p & 0x1F; }
        else if ((*p & 0xF0) == 0xE0) { need = 2; cp = *p & 0x0F; }
        else if ((*p & 0xF8) == 0xF0) { need = 3; cp = *p & 0x07; }
        else return false;
        ++p;
        for (int k = 0; k < need; ++k) {
            if ((*p & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (*p & 0x3F);
            ++p;
        }
        if (need == 1 && cp < 0x80) return false;
        if (need == 2 && cp < 0x800) return false;
        if (need == 3 && cp < 0x10000) return false;
    }
    return true;
}

static void check(const char *what, bool ok, const char *detail = nullptr)
{
    if (ok) { ++g_pass; return; }
    ++g_fail;
    printf("FAIL %s%s%s\n", what, detail ? " -- " : "", detail ? detail : "");
}

// 前缀字节数，仅用于 [body-kept] 断言定位。
static size_t prefix_len_hint(int index)
{
    char p[16] = {};
    snprintf(p, sizeof(p), "%d. ", index + 1);
    return strlen(p);
}

// 注意：expect_truncation 不是外部传进来的猜想，而是由"完整行宽是否超预算"
// 实测推导出来的——否则就会犯"以为 4 个用户名都会被截"这种错：
// 噗呦噗呦+(简)+(CKN)(2Mb).gb 实测只有 247px，加前缀也放得下，本就不该截。
static void run_case(int index, const char *name)
{
    char out[game_config::kRomDisplayNameBytes] = {};
    format_row_text(out, sizeof(out), index, name);

    const std::string full = std::to_string(index + 1) + ". " + (name ? name : "");
    char label[512];
    snprintf(label, sizeof(label), "row %d name=%s", index, name ? name : "(null)");

    const int full_w = lv_txt_get_width(full.c_str(), (uint32_t)full.size(),
                                        &zh_font_16, 0, LV_TEXT_FLAG_NONE);
    const bool expect_truncation = full_w > kRowTextWidthPx;
    if (expect_truncation) ++g_truncated_cases; else ++g_passthrough_cases;

    const int w = lv_txt_get_width(out, (uint32_t)strlen(out), &zh_font_16, 0,
                                   LV_TEXT_FLAG_NONE);
    char detail[1024];

    // A/B：不超宽。放得下时等于原样；超宽时必须 <= 预算。
    snprintf(detail, sizeof(detail), "width=%d budget=%d full_w=%d out=%s", w,
             kRowTextWidthPx, full_w, out);
    check((std::string(label) + " [width]").c_str(), w <= kRowTextWidthPx, detail);

    // C：合法 UTF-8（不许把三字节汉字切一半）。
    check((std::string(label) + " [utf8]").c_str(), valid_utf8(out), out);

    // D：后缀保留。
    if (name) {
        const size_t nl = strlen(name);
        const bool has_suffix = nl >= 3 && name[nl-3] == '.' &&
            (name[nl-2] == 'g' || name[nl-2] == 'G') &&
            (name[nl-1] == 'b' || name[nl-1] == 'B');
        const bool has_suffix4 = nl >= 4 && name[nl-4] == '.' &&
            (name[nl-3] == 'g' || name[nl-3] == 'G') &&
            (name[nl-2] == 'b' || name[nl-2] == 'B') &&
            (name[nl-1] == 'c' || name[nl-1] == 'C');
        if (has_suffix || has_suffix4) {
            const size_t ol = strlen(out);
            const bool out_has = ol >= 3 && out[ol-3] == '.' &&
                (out[ol-2] == 'g' || out[ol-2] == 'G') &&
                (out[ol-1] == 'b' || out[ol-1] == 'B');
            const bool out_has4 = ol >= 4 && out[ol-4] == '.' &&
                (out[ol-3] == 'g' || out[ol-3] == 'G') &&
                (out[ol-2] == 'b' || out[ol-2] == 'B') &&
                (out[ol-1] == 'c' || out[ol-1] == 'C');
            check((std::string(label) + " [suffix]").c_str(),
                  out_has || out_has4, out);
        }
    }

    // E：截断行为与实测宽度推导出的预期一致。
    if (expect_truncation) {
        const bool changed = full != out;
        check((std::string(label) + " [truncated]").c_str(), changed, out);
        const bool has_marker = strstr(out, kRowTruncationMarker) != nullptr;
        check((std::string(label) + " [marker]").c_str(), has_marker, out);
        // F：主体至少留 1 个字符。否则退化成 "N. ....gb"，信息全丢。
        const char *body_start = out + prefix_len_hint(index);
        check((std::string(label) + " [body-kept]").c_str(),
              strstr(out, kRowTruncationMarker) > body_start, out);
    } else {
        check((std::string(label) + " [passthrough]").c_str(), full == out, out);
    }
}

int main()
{
    // 放得下的名字：必须原样透传（预期由实测推导，不是硬编码）。
    for (const char *n : SHORT_NAMES) {
        run_case(0, n);
        run_case(63, n);
    }
    // 用户真实名：短的原样透传，最长的必须截断。
    for (const char *n : USER_REAL_NAMES) {
        run_case(0, n);
        run_case(63, n);
    }
    for (const char *n : LONG_CJK_NAMES) {
        for (int idx = 0; idx < 64; idx += 7) {
            run_case(idx, n);
        }
    }
    // 全下标扫一遍最长名，确保前缀宽度变化不破坏不变量。
    for (int idx = 0; idx < 64; ++idx) {
        run_case(idx, LONG_CJK_NAMES[0]);
    }
    // 退化输入：不许崩、不许溢出、输出必须合法。
    {
        char out[game_config::kRomDisplayNameBytes] = {};
        format_row_text(out, sizeof(out), 0, nullptr);
        check("nullptr name -> empty-ish", strlen(out) <= 4, out);
        format_row_text(out, sizeof(out), 0, "");
        check("empty name", strlen(out) <= 4, out);
        format_row_text(out, sizeof(out), 0, "nodot");
        check("no-dot name", valid_utf8(out), out);
        format_row_text(out, sizeof(out), 0, ".gb");
        check("dot-leading name", valid_utf8(out), out);
        format_row_text(out, sizeof(out), -1, "x.gb");
        check("negative index", valid_utf8(out), out);
        // 极小 out_bytes：snprintf 必须安全截断，不许写越界。
        char tiny[8] = {};
        format_row_text(tiny, sizeof(tiny), 63, LONG_CJK_NAMES[0]);
        check("tiny out_bytes", strlen(tiny) < sizeof(tiny), tiny);
        // out_bytes = 0：必须直接返回，不许碰缓冲区。
        format_row_text(nullptr, 0, 0, "x.gb");
        check("null out", true);
    }
    // 截断路径必须真的被跑到过，否则上面全过也没意义。
    check("truncation path exercised", g_truncated_cases > 0);
    check("passthrough path exercised", g_passthrough_cases > 0);
    printf("(截断用例 %d 条，透通用例 %d 条)\n", g_truncated_cases,
           g_passthrough_cases);

    printf("PASS %d FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
"""


def emit_names(var, names):
    lines = ["static const char *%s[] = {" % var]
    for n in names:
        lines.append('    "%s",' % n.replace("\\", "\\\\").replace('"', '\\"'))
    lines.append("};")
    return "\n".join(lines)


# 变异测试：把抽出来的函数改坏，确认这个脚本真的会 FAIL。
# 注入点换不到旧字符串就 assert 失败——说明源码结构变了，测试要跟着更新，
# 而不是静默跳过。
def mutations(funcs):
    sig_utf8 = "size_t utf8_char_bytes(const char *p)"
    sig_fmt = "void format_row_text(char *out, size_t out_bytes, int index, const char *name)"

    def cut_utf8(f):
        # 退化成"每字节一个字符"——必然把三字节汉字切成两半。
        head = f.split("\n", 1)[0]
        return head + "\n{\n    (void)p;\n    return 1;\n}"

    def drop_suffix(f):
        old = ('snprintf(out, out_bytes, "%s%.*s%s%s", prefix, (int)keep, safe,\n'
               '             kRowTruncationMarker, suffix);')
        new = ('snprintf(out, out_bytes, "%s%.*s%s", prefix, (int)keep, safe,\n'
               '             kRowTruncationMarker);')
        assert old in f, "drop_suffix 注入点没找到"
        return f.replace(old, new)

    def huge_budget(f):
        return f.replace("kRowTextWidthPx", "kRowTextWidthPx * 100")

    def no_advance(f):
        old = """        used += cw;
        i += clen;
        keep = i;"""
        new = """        used += cw;
        i += clen;"""
        assert old in f, "no_advance 注入点没找到"
        return f.replace(old, new)

    return [
        ("utf8_char_bytes 退化成每字节 1 字符（会切开三字节汉字）",
         {sig_utf8: cut_utf8(funcs[sig_utf8])}),
        ("截断后丢掉扩展名后缀", {sig_fmt: drop_suffix(funcs[sig_fmt])}),
        ("宽度预算放大 100 倍（等于不截断）",
         {sig_fmt: huge_budget(funcs[sig_fmt])}),
        ("keep 不随循环前进（主体全丢）", {sig_fmt: no_advance(funcs[sig_fmt])}),
    ]


def compile_and_run(funcs, names_text, row_w, marker, display_bytes, table):
    funcs_text = "\n\n".join(funcs[s] for s in FUNC_SIGS)
    src = RUNNER.replace("__FUNCTIONS__", funcs_text)
    src = src.replace("__NAMES__", names_text)
    src = src.replace("__ROW_TEXT_WIDTH_PX__", str(row_w))
    src = src.replace("__MARKER__", marker)
    src = src.replace("__DISPLAY_NAME_BYTES__", str(display_bytes))
    src = src.replace("__WIDTH_TABLE__", table)
    for token in ("__FUNCTIONS__", "__NAMES__", "__ROW_TEXT_WIDTH_PX__", "__MARKER__",
                  "__DISPLAY_NAME_BYTES__", "__WIDTH_TABLE__"):
        if token in src:
            sys.exit("FATAL: 模板占位符没替换干净: %s" % token)

    with tempfile.TemporaryDirectory() as tmp:
        cpp = os.path.join(tmp, "t.cpp")
        binary = os.path.join(tmp, "t")
        with open(cpp, "w", encoding="utf-8") as handle:
            handle.write(src)
        try:
            subprocess.run(
                ["g++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-o", binary, cpp],
                check=True, capture_output=True,
            )
        except subprocess.CalledProcessError as exc:
            print("编译失败:")
            print(exc.stderr.decode("utf-8", "replace"))
            sys.exit(1)
        # errors="replace"：变异测试会故意产出非法 UTF-8，别让解码先崩了。
        return subprocess.run([binary], capture_output=True, text=True,
                              errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mutate", action="store_true",
                    help="逐个注入缺陷，确认脚本每个都能抓到")
    args = ap.parse_args()

    with open(UI_GAME, "r", encoding="utf-8") as handle:
        ui_src = handle.read()
    with open(DISPLAY_CONFIG, "r", encoding="utf-8") as handle:
        display_src = handle.read()

    funcs = extract_functions(UI_GAME)
    row_w, marker = extract_constants(ui_src, display_src)
    display_bytes = extract_display_name_bytes(GAME_CONFIG)
    px = build_width_table()

    print("抽取自 %s" % os.path.relpath(UI_GAME, ROOT))
    for sig, text in funcs.items():
        print("  %s ... %d 行（原样，未改写）" % (sig.split("(")[0], text.count("\n")))
    print("  kRowTextWidthPx = %d px" % row_w)
    print("  kRowTruncationMarker = %r" % marker)
    print("  kRomDisplayNameBytes = %d" % display_bytes)

    # 宽度表：ASCII 全量 + 测试里用到的非 ASCII 码点。
    cps = set(range(0x20, 0x7F))
    for names in (USER_REAL_NAMES, SHORT_NAMES, LONG_CJK_NAMES):
        for n in names:
            for ch in n:
                if ord(ch) >= 0x80:
                    cps.add(ord(ch))
    rows = []
    for cp in sorted(cps):
        w = px(cp)
        if w is None:
            continue
        rows.append("    {0x%04X, %d}," % (cp, w))
    table = "\n".join(rows)
    print("  宽度表 %d 条（来自 zh_font_16.c 真实字形）" % len(rows))

    names_text = "\n\n".join([
        emit_names("SHORT_NAMES", SHORT_NAMES),
        emit_names("USER_REAL_NAMES", USER_REAL_NAMES),
        emit_names("LONG_CJK_NAMES", LONG_CJK_NAMES),
    ])

    proc = compile_and_run(funcs, names_text, row_w, marker, display_bytes, table)
    print()
    print(proc.stdout.strip())
    if proc.stderr.strip():
        print("stderr: " + proc.stderr.strip())
    if proc.returncode != 0:
        sys.exit("FAILED: 真实源码未通过，先修代码再跑变异测试")

    if not args.mutate:
        return

    print()
    print("=" * 70)
    print("变异测试：逐个把函数改坏，每个都必须被抓到")
    print("=" * 70)
    all_caught = True
    for label, mutated in mutations(funcs):
        merged = dict(funcs)
        merged.update(mutated)   # 变异字典只含被改的那个函数，其余用真实的
        proc_m = compile_and_run(merged, names_text, row_w, marker,
                                 display_bytes, table)
        fails = [l for l in proc_m.stdout.split("\n") if l.startswith("FAIL")]
        if proc_m.returncode == 0 or not fails:
            print("  [漏报!] %s —— 改了却没被抓到，测试有盲区" % label)
            all_caught = False
        else:
            print("  [抓住] %-46s %d 条 FAIL" % (label, len(fails)))
            print("         例: %s" % fails[0][:110])
    print()
    print("变异测试总结: %s" % ("全部抓住" if all_caught else "存在漏报"))
    if not all_caught:
        sys.exit(1)


if __name__ == "__main__":
    main()
