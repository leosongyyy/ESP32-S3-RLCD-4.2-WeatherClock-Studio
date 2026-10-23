// 把 Game Boy 扫描线直接写成 RLCD 打包帧缓冲，不做 160x144 中间缓冲。
//
// 面板字节推导见 PHASE2_SPEC.md §2.1，但**不要照抄那份文档正文的公式**：
// 该文基于 staging 参考实现的 Y0=4（block_y ∈ [2,73]、k = 146 - 2*block_y），
// 而主工程实际取 kWindowY = 0（block_y ∈ [3,74]、k = 148 - 2*block_y）。
// 两者差 4 个面板像素，照抄会让画面整体下移 2 个 GB 行并出现错位瓦片行。
// 改这里时以 game_config.h 的常量 + 本文件末尾的 static_assert 为准。
//
//   index = byte_x * 75 + block_y
//   一个面板字节覆盖 x = 2*byte_x..2*byte_x+1、y = 296-4*block_y..299-4*block_y
//   在 2 倍放大下这两列来自同一个 GB 源列 c = byte_x - 20，
//   4 行来自 GB 源行 k = 148 - 2*block_y 与 k+1，且
//   bit7..bit4 = 源行 k+1，bit3..bit0 = 源行 k。
#include "gameboy_fb.h"

#include "app_hardware.h"
#include "game_config.h"

#include <esp_log.h>

namespace {
constexpr const char *kFbLogTag = "GameBoyFb";

// 两行 GB 像素组成的行配对缓冲，共 320 字节。
// ⚠ 存的是**每个 GB 像素的 2x2 子像素图案**（4 位），不是 0/1 位：
//   面板字节的高 4 位 = 源行 k+1、低 4 位 = 源行 k，两个半字节位序相同，
//   所以只要在 push_line 阶段把 shade 换算成 4 位图案，写盘时直接拼即可。
//   灰度换算全部集中在 shade_to_nibble()，写盘通路与抖动模式无关。
uint8_t s_row_pair[2][game_config::kGbWidth];
bool s_row_ready[2] = {false, false};
bool s_frame_dirty = false;

// ---- 模式 1（kDitherSpatial2x2）的图案表 ----

// 2x2 有序抖动矩阵的 4 种旋转，基准是标准 2x2 Bayer [[0,2],[3,1]]。
//
// ⚠ 为什么要 4 种旋转：若所有像素都用同一个取向，"1/4 亮"的水滴永远点在
//   左上角、"3/4 亮"永远缺左下角，整片中间灰会连成一张**规则的点阵网格**，
//   在电子纸上非常显眼、很像噪点。按 (row + col) & 3 轮换 4 种取向，
//   相邻像素的图案互相错开，网格被打散成近似随机的细纹。
constexpr uint8_t kBayer2[4][2][2] = {
    {{0, 2}, {3, 1}},   // 基准
    {{3, 0}, {1, 2}},   // 顺时针 90°
    {{1, 3}, {2, 0}},   // 180°
    {{2, 1}, {0, 3}},   // 顺时针 270°
};

// 2x2 子像素在面板字节半字节里的位号，索引 [行][列]，行 0 = 上、列 0 = 左。
//
// 由文件头的位序推导：每个 GB 行占 2 个面板行、2 个面板列，组内位序恒为
//   bit3 = 下-左   bit2 = 下-右   bit1 = 上-左   bit0 = 上-右
// 上半组（源行 k+1，bit7..4）与下半组（源行 k，bit3..0）的位序完全一致，
// 这正是"同一个 4 位图案能整体左移 4 位复用"的前提。
constexpr uint8_t kSubBit[2][2] = {{1, 0}, {3, 2}};

struct DensityLut {
    uint8_t v[4][4];    // [旋转][shade] -> 4 位子像素图案
};

// 密度 d 的子像素点亮规则就是有序抖动本身：子像素亮 ⟺ 该位置的阈值 < d。
// d=4 全亮、d=0 全黑，d=1/2/3 由矩阵决定点亮哪几个，因此同一密度下
// 4 种旋转给出 4 种不同的点法，正好用来打散网格。
constexpr uint8_t density_nibble(int rot, int shade)
{
    uint8_t p = 0;
    const uint8_t d = game_config::kShadeDensity[shade];
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            if (kBayer2[rot][i][j] < d) {
                p = (uint8_t)(p | (uint8_t)(1U << kSubBit[i][j]));
            }
        }
    }
    return p;
}

constexpr DensityLut make_density_lut(void)
{
    DensityLut t{};
    for (int r = 0; r < 4; ++r) {
        for (int s = 0; s < 4; ++s) {
            t.v[r][s] = density_nibble(r, s);
        }
    }
    return t;
}

constexpr DensityLut kDensityLut = make_density_lut();

constexpr int popcount4(uint8_t nibble)
{
    return ((nibble >> 3) & 1) + ((nibble >> 2) & 1) +
           ((nibble >> 1) & 1) + (nibble & 1);
}

// ---- 模式 2（kDitherBayer4x4）的阈值矩阵 ----
//
// 标准 4x4 Bayer 有序抖动矩阵，值域 0..15。在 160x144 原生分辨率上按
// (line & 3, column & 3) 取阈值，与 kShadeLevel 比较后决定这个 GB 像素
// 是纯黑还是纯白，再交给 2x 最近邻放大 —— 与 gambatte-k2 的做法一致。
constexpr uint8_t kBayer4x4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5},
};

// GB 的 2bit shade（0=最白、3=最黑）→ 该像素在面板上的 2x2 子像素图案。
//
// 三种模式都收敛到同一个"4 位图案"表示，写盘通路因此只有一份。
// line/column 是 GB 原生坐标，抖动图案必须按**源坐标**取，不能按面板坐标，
// 否则图案会随放大倍率拉伸、周期对不上。
inline uint8_t shade_to_nibble(uint8_t shade, int line, int column)
{
    switch (game_config::kDitherMode) {
    case game_config::kDitherBinary:
        return (shade < game_config::kWhiteShadeLimit) ? 0x0FU : 0x00U;
    case game_config::kDitherBayer4x4: {
        const uint8_t level = game_config::kShadeLevel[shade];
        const bool white = level > kBayer4x4[line & 3][column & 3];
        return white ? 0x0FU : 0x00U;
    }
    case game_config::kDitherSpatial2x2:
    default:
        return kDensityLut.v[(line + column) & 3][shade];
    }
}

// GB 偶数行 k 对应的面板瓦片行：k = 0 -> 74（屏幕底部），k = 142 -> 3（顶部）。
constexpr int block_row_for_even_line(int even_line)
{
    return game_config::kFirstBlockY + game_config::kRowPairs - 1 -
           (even_line / 2);
}

static_assert(game_config::kGbWidth > 0, "GB row width must be positive");
// 图案表的不变式：最白一档必须四个子像素全亮、最黑一档全灭，中间两档的
// 点亮个数必须严格等于 kShadeDensity（否则灰度层次会悄悄塌成两级）。
// 四条断言覆盖全部 4 种旋转，只验 rot=0 会漏掉旋转实现写错的情况。
static_assert(kDensityLut.v[0][0] == 0x0F && kDensityLut.v[1][0] == 0x0F &&
                  kDensityLut.v[2][0] == 0x0F && kDensityLut.v[3][0] == 0x0F,
              "shade 0 must light every subpixel");
static_assert(kDensityLut.v[0][3] == 0x00 && kDensityLut.v[1][3] == 0x00 &&
                  kDensityLut.v[2][3] == 0x00 && kDensityLut.v[3][3] == 0x00,
              "shade 3 must light no subpixel");
static_assert(popcount4(kDensityLut.v[0][1]) == game_config::kShadeDensity[1] &&
                  popcount4(kDensityLut.v[1][1]) == game_config::kShadeDensity[1] &&
                  popcount4(kDensityLut.v[2][1]) == game_config::kShadeDensity[1] &&
                  popcount4(kDensityLut.v[3][1]) == game_config::kShadeDensity[1],
              "shade 1 density must match the config table");
static_assert(popcount4(kDensityLut.v[0][2]) == game_config::kShadeDensity[2] &&
                  popcount4(kDensityLut.v[1][2]) == game_config::kShadeDensity[2] &&
                  popcount4(kDensityLut.v[2][2]) == game_config::kShadeDensity[2] &&
                  popcount4(kDensityLut.v[3][2]) == game_config::kShadeDensity[2],
              "shade 2 density must match the config table");
// 4 种旋转必须真的互不相同，否则"打散网格"的用意就没落地。
static_assert(kDensityLut.v[0][1] != kDensityLut.v[1][1] &&
                  kDensityLut.v[0][1] != kDensityLut.v[2][1] &&
                  kDensityLut.v[0][1] != kDensityLut.v[3][1],
              "bayer rotations must yield distinct shade-1 patterns");
static_assert(game_config::kPanelBytesPerColumn == 75,
              "panel byte column height must stay 75");
static_assert(block_row_for_even_line(0) == 74,
              "GB line pair 0 must land on the last panel byte row");
static_assert(block_row_for_even_line(game_config::kGbHeight - 2) == 3,
              "GB line pair 71 must land on panel byte row 3");

void write_panel_row(const uint8_t *row_even, const uint8_t *row_odd, int block_y)
{
    uint8_t *framebuffer = app_display().RLCD_Framebuffer();
    if (!framebuffer) {
        return;
    }
    const size_t stride = (size_t)game_config::kPanelBytesPerColumn;
    size_t offset = (size_t)game_config::kFirstByteX * stride + (size_t)block_y;
    for (int column = 0; column < game_config::kGbWidth; ++column) {
        // 高 4 位 = 源行 k+1（row_odd，画面上方），低 4 位 = 源行 k（row_even）。
        // ⚠ 顺序不能反：写反会让画面每两行上下对调。
        //    主机端穷举验证见 tools/blit_verify.c（15000 字节全比对）。
        framebuffer[offset] =
            (uint8_t)((uint8_t)(row_odd[column] << 4) | row_even[column]);
        offset += stride;
    }
}
} // namespace

void game_fb_frame_begin(void)
{
    s_row_ready[0] = false;
    s_row_ready[1] = false;
    s_frame_dirty = false;
}

void game_fb_push_line(uint32_t line, const uint8_t *pixels)
{
    if (!pixels || line >= (uint32_t)game_config::kGbHeight) {
        return;
    }
    const uint32_t slot = line & 1U;
    uint8_t *row = s_row_pair[slot];
    const int gb_line = (int)line;
    for (int column = 0; column < game_config::kGbWidth; ++column) {
        // 灰度换算只在这里发生一次，写盘通路拿到的已经是 2x2 子像素图案。
        row[column] = shade_to_nibble((uint8_t)(pixels[column] & 0x03U),
                                      gb_line, column);
    }
    s_row_ready[slot] = true;

    // 只在奇数行到来时配对写出：此时 (line-1, line) 正是面板的一个瓦片行。
    if (slot != 1U || !s_row_ready[0]) {
        return;
    }
    const int even_line = (int)line - 1;
    write_panel_row(s_row_pair[0], s_row_pair[1], block_row_for_even_line(even_line));
    s_row_ready[0] = false;
    s_row_ready[1] = false;
    s_frame_dirty = true;
}

bool game_fb_prepare_surface(void)
{
    DisplayPort &display = app_display();
    if (!display.RLCD_Framebuffer()) {
        ESP_LOGE(kFbLogTag, "panel framebuffer unavailable");
        return false;
    }
    // 全屏黑底：GB 窗口之外（左右各 40px、上下 12px）保持黑边。
    display.RLCD_ColorClear((uint8_t)ColorBlack);
    if (!display.RLCD_FillRange(0, (uint16_t)(game_config::kFirstByteX - 1), (uint8_t)ColorBlack) ||
        !display.RLCD_FillRange((uint16_t)(game_config::kLastByteX + 1),
                                (uint16_t)(game_config::kPanelByteColumns - 1),
                                (uint8_t)ColorBlack)) {
        ESP_LOGW(kFbLogTag, "panel border fill rejected");
    }
    // 黑边只在整屏推一次，之后逐帧只推 GB 窗口（省 20% SPI 时间）。
    display.RLCD_Present();
    s_row_ready[0] = false;
    s_row_ready[1] = false;
    s_frame_dirty = true;
    return true;
}

bool game_fb_present(void)
{
    if (!s_frame_dirty) {
        return false;
    }
    DisplayPort &display = app_display();
    if (!display.RLCD_Framebuffer()) {
        return false;
    }
    // GB 窗口在帧缓冲里是连续的一段：byte_x 20..179，每条 75 字节。
    // 边距是静态黑边，无需每帧重推，用局部刷新通路即可。
    display.RLCD_DisplayXRange((uint16_t)game_config::kWindowX,
                               (uint16_t)(game_config::kWindowX +
                                          game_config::kWindowWidth - 1));
    s_frame_dirty = false;
    return true;
}

void game_fb_fill_rect(int x0, int y0, int x1, int y1, bool white)
{
    uint8_t *framebuffer = app_display().RLCD_Framebuffer();
    if (!framebuffer || x0 > x1 || y0 > y1) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= kDisplayWidth) {
        x1 = kDisplayWidth - 1;
    }
    if (y1 >= kDisplayHeight) {
        y1 = kDisplayHeight - 1;
    }
    // 与 display_bsp.cpp 的 RLCD_SetPixel() 同一套位映射：
    //   index = (x >> 1) * 75 + ((299 - y) >> 2)，bit = 7 - ((local_y << 1) | local_x)
    // 这里逐像素写，和 game_fb_push_line() 的行配对通路互不影响（配对通路写的是
    // 同一批 bit，只是每次都写满一整对行）。
    for (int y = y0; y <= y1; ++y) {
        const int inv_y = kDisplayHeight - 1 - y;
        const int block_y = inv_y >> 2;
        const int local_y = inv_y & 3;
        for (int x = x0; x <= x1; ++x) {
            const int index =
                (x >> 1) * game_config::kPanelBytesPerColumn + block_y;
            const int bit = 7 - ((local_y << 1) | (x & 1));
            if (white) {
                framebuffer[index] |= (uint8_t)(1U << bit);
            } else {
                framebuffer[index] &= (uint8_t)~(1U << bit);
            }
        }
    }
}

bool game_fb_draw_pause_badge(void)
{
    if (!app_display().RLCD_Framebuffer()) {
        return false;
    }
    // 居中 100x72 的黑底白框，里面两条竖杠 = 通用「暂停」符号。
    // 不用字体：屏幕此刻归门闸独占，不能跨任务调 LVGL，画点位是唯一安全做法。
    constexpr int kBoxX0 = 150;
    constexpr int kBoxX1 = 249;
    constexpr int kBoxY0 = 114;
    constexpr int kBoxY1 = 185;
    constexpr int kBarWidth = 14;
    constexpr int kBarY0 = 126;
    constexpr int kBarY1 = 173;
    constexpr int kBarLeftX = kBoxX0 + 24;
    constexpr int kBarRightX = kBoxX0 + 62;

    game_fb_fill_rect(kBoxX0, kBoxY0, kBoxX1, kBoxY1, false);
    game_fb_fill_rect(kBarLeftX, kBarY0, kBarLeftX + kBarWidth - 1, kBarY1, true);
    game_fb_fill_rect(kBarRightX, kBarY0, kBarRightX + kBarWidth - 1, kBarY1, true);
    // 描白边框：浅色场景下纯黑底框会糊在背景里看不清。
    game_fb_fill_rect(kBoxX0, kBoxY0, kBoxX1, kBoxY0 + 1, true);
    game_fb_fill_rect(kBoxX0, kBoxY1 - 1, kBoxX1, kBoxY1, true);
    game_fb_fill_rect(kBoxX0, kBoxY0, kBoxX0 + 1, kBoxY1, true);
    game_fb_fill_rect(kBoxX1 - 1, kBoxY0, kBoxX1, kBoxY1, true);
    s_frame_dirty = true;
    return true;
}
