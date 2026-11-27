// 阅读页字体：优先用 TF 卡上的 Fusion Pixel 12px 中文字库，
// 与主界面卡片模块名同一套取模字形；加载失败时回退内置 zh_font_16。
//
// 字库文件由 tools/gen_sd_font.py 生成，放 /sdcard/font/fusion_pixel_12.bin。
// ASCII（0x20-0x7E）不在卡上字库里，始终用固件内置的 fusion_pixel_12，
// 这样中英文都与主菜单一致，卡上文件也能小 30KB。
#pragma once

#include <stdint.h>

// 一个已解析的字形。bits 指向 PSRAM 里点阵的起点，按行打包，1bpp。
struct BookGlyph {
    int w, h;
    int xoff;   // 相对笔位的横向偏移
    int ytop;   // 相对行顶的纵向偏移，可为负
    int dw;     // 步进宽度
    int row_bytes;
    const uint8_t *bits;
};

// 从 /sdcard/font/fusion_pixel_12.bin 载入字库（幂等）。
// TF 卡没挂载、文件不存在、magic 不对都会返回 false，调用方走内置字体。
bool book_font_load(void);
void book_font_unload(void);
bool book_font_ready(void);

// 取非 ASCII 字形；字库未就绪或没有该字返回 nullptr。
// 返回的是模块内静态对象，只能立即使用，不要长期保存指针。
const BookGlyph *book_font_glyph(uint32_t cp);

// 排版度量：字库就绪时全角 12 / 行距 13 / 每页 13 行，
// 回退内置 16px 时全角 16 / 行距 17 / 每页 9 行。
int book_font_full_w(void);
int book_font_ascii_w(uint32_t cp);   // 内置 12px 取模字库的真实 dw

// 128 项 ASCII 宽度表，索引即 ASCII 码，字库未就绪时返回 nullptr。
// 排版循环要对每个西文字符取宽度，走这张 DRAM 里的表比逐个线性查
// flash 里的 fusion_pixel_12_index 快一个数量级。
const uint8_t *book_font_ascii_table(void);
int book_font_line_pitch(void);
int book_font_gap_pitch(void);
int book_font_max_lines(void);
int book_font_ascent(void);
