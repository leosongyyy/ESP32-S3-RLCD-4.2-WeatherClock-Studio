#!/usr/bin/env python3
"""check_deps.py — 引擎层门面守卫（构建期强制）

背景
----
ESP-IDF 的 REQUIRES 只管链接顺序，不管 include 可见性：main 组件把所有
子目录都放进了 INCLUDE_DIRS，所以写错方向照样编得过。必须静态扫描兜底。

分层（main/ 下按目录划逻辑层，依赖只允许 上层 → 引擎 → 底层）
    底层  core/(除 main.cpp) sensors/ audio/ storage/ assets/  + components/
    引擎  game/
    上层  ui/ input/ network/ ota/ alarm/ pomodoro/  + core/main.cpp

铁律
----
  [门面] 上层只能 #include "engine_api.h"，不得直连 game/ 下任何子头。
  [倒置] 引擎不得 #include 上层的头；上层动作走 engine_api.h 的回调注入。

引擎 → core/sensors/audio/components 是硬件适配依赖，合法，只统计不报错。
（game_config.h → app_display_config.h、gameboy_fb.cpp → app_hardware.h 等
  都属于这一类：引擎需要知道面板几何和显示端口，这不是层次倒置。）

用法
----
    python3 tools/check_deps.py            # 检查，违规退出码 1
    python3 tools/check_deps.py --verbose  # 附带打印合法依赖统计

接入：项目根 CMakeLists.txt 的 weather_clock_dep_guard target，每次构建自动跑。
"""
import glob
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN_DIR = os.path.join(ROOT, "main")

ENGINE_DIR = "game"
FACADE = "engine_api.h"
UPPER_DIRS = {"ui", "input", "network", "ota", "alarm", "pomodoro"}

SRC_EXT = (".c", ".cpp", ".h", ".hpp")


def source_files():
    """main/ 下所有源码，按所属逻辑目录分组。"""
    groups = defaultdict(list)
    for path in glob.glob(os.path.join(MAIN_DIR, "**", "*"), recursive=True):
        if not os.path.isfile(path) or not path.endswith(SRC_EXT):
            continue
        rel = os.path.relpath(path, MAIN_DIR).replace(os.sep, "/")
        if rel.startswith("build/"):
            continue
        groups[rel.split("/")[0]].append(path)
    return groups


def header_owners(groups):
    """头文件 basename -> 所属逻辑目录。

    main 组件的 INCLUDE_DIRS 把所有子目录都摊平了，include 时写的是裸文件名，
    所以这里按 basename 索引（同名时记到集合里，避免误判）。
    """
    owners = defaultdict(set)
    for d, files in groups.items():
        for f in files:
            if f.endswith(".h") or f.endswith(".hpp"):
                owners[os.path.basename(f)].add(d)
    return owners


def includes_of(path):
    with open(path, encoding="utf-8", errors="ignore") as f:
        text = f.read(40000)
    return re.findall(r'^\s*#\s*include\s+"([^"]+)"', text, re.M)


def main():
    verbose = "--verbose" in sys.argv
    groups = source_files()
    owners = header_owners(groups)

    game_headers = {
        os.path.basename(f)
        for f in groups.get(ENGINE_DIR, [])
        if f.endswith(".h") or f.endswith(".hpp")
    }
    if FACADE not in game_headers:
        print("❌ 缺少引擎门面 main/game/%s" % FACADE)
        return 1

    facade_only = ("[门面] 上层只能 include engine_api.h",
                   "[倒置] 引擎不得 include 上层头")

    errors = defaultdict(list)
    lower_deps = defaultdict(set)

    for d, files in sorted(groups.items()):
        for path in files:
            rel = os.path.relpath(path, MAIN_DIR).replace(os.sep, "/")
            for inc in includes_of(path):
                name = os.path.basename(inc)
                inc_dirs = owners.get(name, set())

                if d == ENGINE_DIR:
                    # 引擎 → 上层：硬错误
                    hit = inc_dirs & UPPER_DIRS
                    if hit:
                        errors[facade_only[1]].append(
                            "%s -> %s (%s)" % (rel, inc, "/".join(sorted(hit))))
                        continue
                    # 引擎 → 底层：合法，统计
                    for ld in inc_dirs - {ENGINE_DIR}:
                        lower_deps[rel].add(ld)
                    continue

                # 非引擎 → game 子头（含门面本身）：门面必须是唯一入口
                if ENGINE_DIR in inc_dirs and name != FACADE:
                    # game/ 自己 include 子头不算（上面 continue 已跳过）
                    errors[facade_only[0]].append("%s -> %s" % (rel, inc))

    total_src = sum(len(v) for v in groups.values())
    print("=" * 68)
    print("引擎门面守卫 (main/ 共 %d 个源码, 引擎 %d 个)" %
          (total_src, len(groups.get(ENGINE_DIR, []))))
    print("=" * 68)
    for title, items in errors.items():
        print("\n%s  %d 条" % (title, len(items)))
        for line in sorted(set(items)):
            print("   -", line)
    if verbose:
        print("\n[合法] 引擎 → 底层适配依赖 %d 条" %
              sum(len(v) for v in lower_deps.values()))
        for src, deps in sorted(lower_deps.items()):
            print("   - %s -> %s" % (src, ", ".join(sorted(deps))))

    n = sum(len(v) for v in errors.values())
    print("\n" + "=" * 68)
    if n:
        print("❌ 违规 %d 条 —— 上层统一走 engine_api.h，引擎跨层动作用回调注入" % n)
        return 1
    print("✅ 门面完整，无反向依赖")
    return 0


if __name__ == "__main__":
    sys.exit(main())
