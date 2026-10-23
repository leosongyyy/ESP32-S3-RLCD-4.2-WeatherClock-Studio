# Vendored minigb_apu（Game Boy APU 模拟）。
#
# 来源：https://github.com/deltabeard/minigb_apu master
#       （Copyright (c) 2017 Alex Baines / (c) 2019 Mahyar Koshkouei，MIT，见 LICENSE）
# 选型依据：Peanut-GB 作者 deltabeard 维护的同源库，含 2025-08-15 的
# "apu: fix wave channel volume control"（344d9814）——Peanut-GB 仓库
# examples/sdl2/minigb_apu/ 下的副本是 2025-02-03 的旧版，缺该修复：
# 旧版在音量移位之后才减 wave 偏置，NR32 非 100% 音量时输出单向直流偏移。
#
# 源码原样引入，未做任何改动。使用方必须在 include "minigb_apu.h" 之前
# 定义 AUDIO_SAMPLE_RATE 与 MINIGB_APU_AUDIO_FORMAT_S16SYS——本组件自己的
# 编译也依赖它们（AUDIO_SAMPLES 等派生常量在头文件里算）。用 PUBLIC 定义
# 注入：minigb_apu.c 拿到，所有 REQUIRES 本组件的编译单元也拿到，
# 与 main/game/game_audio_config.h 的值由该头的 #error 锁保持一致。
idf_component_register(
    SRCS
        "minigb_apu.c"
    INCLUDE_DIRS
        "."
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    AUDIO_SAMPLE_RATE=32000
    MINIGB_APU_AUDIO_FORMAT_S16SYS
)
