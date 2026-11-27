// 阅读页字体加载实现：读 TF 卡上的 FPX1 字库到 PSRAM，做二分查找。
//
// 不把整份索引复制成结构体数组，而是直接在原始 buffer 上按偏移解码：
// 19450 个字形，索引占 214KB，省下来比每次多几条 unaligned 读更划算。
#include "book_font.h"

#include "engine_api.h"                 // 门面：TF 卡挂载
#include "assets/fusion_pixel_12_font.h" // ASCII 回落字形与真实步进宽度

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr const char *kTag = "BookFont";
constexpr const char *kFontPath = "/sdcard/font/fusion_pixel_12.bin";
constexpr uint32_t kMaxFileBytes = 2 * 1024 * 1024;
constexpr int kIndexBytes = 11;

uint8_t *s_data = nullptr;
uint32_t s_size = 0;
int s_count = 0;
int s_px = 0;
int s_line_h = 0;
int s_ascent = 0;
bool s_ready = false;
BookGlyph s_view = {};
// ASCII 宽度快查表，加载字库时一次性从 fusion_pixel_12_index 摊平。
// 放在 .bss（DRAM），排版循环里逐字取宽度不再碰 flash 索引。
uint8_t s_ascii_w[128] = {};


inline uint16_t rd16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t rd32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

const uint8_t *index_entry(int i)
{
    return s_data + 16 + i * kIndexBytes;
}

int entry_cp(int i)
{
    return rd16(index_entry(i));
}

uint32_t blob_offset(void)
{
    return 16 + static_cast<uint32_t>(s_count) * kIndexBytes;
}

}  // namespace

// ASCII 宽度表构建：定义在下方，字库加载成功后调用一次。
void build_ascii_table(void);

bool book_font_load(void)
{
    if (s_ready) return true;
    if (!rom_store_mount()) {
        ESP_LOGW(kTag, "sd not mounted, fallback to built-in 16px");
        return false;
    }
    FILE *f = std::fopen(kFontPath, "rb");
    if (!f) {
        ESP_LOGW(kTag, "font file missing: %s, fallback to built-in 16px", kFontPath);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 16 || size > static_cast<long>(kMaxFileBytes)) {
        std::fclose(f);
        ESP_LOGW(kTag, "font file size %ld out of range", size);
        return false;
    }
    auto *data = static_cast<uint8_t *>(
        heap_caps_malloc(static_cast<size_t>(size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) {
        std::fclose(f);
        ESP_LOGW(kTag, "no psram for font");
        return false;
    }
    const size_t got = std::fread(data, 1, static_cast<size_t>(size), f);
    std::fclose(f);
    if (got != static_cast<size_t>(size)) {
        heap_caps_free(data);
        ESP_LOGW(kTag, "short read %u/%ld", static_cast<unsigned>(got), size);
        return false;
    }
    if (std::memcmp(data, "FPX1", 4) != 0) {
        heap_caps_free(data);
        ESP_LOGW(kTag, "bad magic, not a FPX1 font");
        return false;
    }
    const int count = rd16(data + 4);
    const int px = rd16(data + 6);
    const int line_h = rd16(data + 8);
    const int ascent = rd16(data + 10);
    const uint32_t blob = rd32(data + 12);
    const size_t need = 16u + static_cast<size_t>(count) * kIndexBytes + blob;
    if (count <= 0 || need != static_cast<size_t>(size)) {
        heap_caps_free(data);
        ESP_LOGW(kTag, "font layout mismatch count=%d blob=%u size=%ld", count,
                 static_cast<unsigned>(blob), size);
        return false;
    }
    s_data = data;
    s_size = static_cast<uint32_t>(size);
    s_count = count;
    s_px = px;
    s_line_h = line_h;
    s_ascent = ascent;
    s_ready = true;
    build_ascii_table();
    ESP_LOGI(kTag, "loaded %s: %d glyphs, %d px, line_h=%d, %u bytes", kFontPath, count,
             px, line_h, static_cast<unsigned>(s_size));
    return true;
}

void book_font_unload(void)
{
    if (s_data) {
        heap_caps_free(s_data);
        s_data = nullptr;
    }
    s_ready = false;
    s_size = 0;
    s_count = 0;
}

bool book_font_ready(void)
{
    return s_ready;
}

const BookGlyph *book_font_glyph(uint32_t cp)
{
    if (!s_ready || cp < 0x80) return nullptr;
    int lo = 0, hi = s_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int cur = entry_cp(mid);
        if (cur == static_cast<int>(cp)) {
            const uint8_t *e = index_entry(mid);
            const uint32_t off = rd32(e + 2);
            const int w = e[6];
            const int h = e[7];
            const int row_bytes = (w + 7) / 8;
            if (off + static_cast<uint32_t>(row_bytes) * h > s_size) return nullptr;
            s_view.w = w;
            s_view.h = h;
            s_view.xoff = e[8];
            s_view.ytop = static_cast<int8_t>(e[9]);
            s_view.dw = e[10];
            s_view.row_bytes = row_bytes;
            s_view.bits = s_data + blob_offset() + off;
            return &s_view;
        }
        if (cur < static_cast<int>(cp)) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

int book_font_full_w(void)
{
    return s_ready ? (s_px > 0 ? s_px : 12) : 16;
}

void build_ascii_table(void)
{
    for (int i = 0; i < 128; ++i) s_ascii_w[i] = 6;   // 缺字按 6px 占位
    for (int i = 0; i < FUSION_PIXEL_12_INDEX_COUNT; ++i) {
        const uint32_t cp = fusion_pixel_12_index[i].cp;
        if (cp < 128) s_ascii_w[cp] = static_cast<uint8_t>(fusion_pixel_12_index[i].dw);
    }
}

const uint8_t *book_font_ascii_table(void)
{
    return s_ready ? s_ascii_w : nullptr;
}

int book_font_ascii_w(uint32_t cp)
{
    if (!s_ready) return 8;
    return cp < 128 ? s_ascii_w[cp] : 6;
}

int book_font_line_pitch(void)
{
    return s_ready ? (s_line_h > 0 ? s_line_h + 2 : 13) : 17;
}

int book_font_gap_pitch(void)
{
    return s_ready ? 6 : 8;
}

int book_font_max_lines(void)
{
    return s_ready ? 12 : 9;
}

int book_font_ascent(void)
{
    return s_ready && s_ascent > 0 ? s_ascent : 14;
}
