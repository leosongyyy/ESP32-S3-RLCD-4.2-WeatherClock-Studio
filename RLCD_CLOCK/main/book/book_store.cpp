// 阅读数据层实现：TF 卡扫描、编码识别、TXT 排版分页、进度落盘。
//
// 排版口径（必须与 ui_reading.cpp 的绘制保持一致）：
//   正文统一用 zh_font_16 渲染，ASCII 半角 8px、其余全角 16px；
//   内容宽 320px（20 个全角字），行距 17px，段距 8px；
//   每页最多 9 行、累计高度不超过 168px。
#include "book_store.h"

#include "book_font.h"
#include "gb2312_table.h"
#include "book_sample.h"

#include "engine_api.h"   // 门面：ROM 模块同时提供 SD 挂载

#include <ctype.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <nvs.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace {

constexpr const char *kTag = "BookStore";
constexpr const char *kBookDir = "/sdcard/book";
constexpr const char *kFontDir = "/sdcard/font";
constexpr const char *kNvsNamespace = "reading";

constexpr size_t kMaxFileBytes = 2 * 1024 * 1024;  // 单本上限，转码后约 3MB
constexpr int kPageMaxLines = 9;
constexpr int kPageMaxHeight = 168;
constexpr int kLinePitch = 17;
constexpr int kGapPitch = 8;
constexpr int kContentWidth = 320;

// 分页口径随「全屏」切换。普通模式正文在 8..344 的框里，全屏时左右各留 12px。
constexpr int kFullContentWidth = 376;
constexpr int kFullPageMaxHeight = 278;
bool s_full = false;
int s_content_w = kContentWidth;
int s_page_max_h = kPageMaxHeight;

BookMeta s_books[BOOK_STORE_MAX];
int s_count = 0;
char s_diag[96] = "未扫描";
// 全局阅读序号：每翻看一次 +1，写出到 NVS，重启后接着用。
// 比时间戳稳，设备没对时也不会因为 RTC 归零把顺序打乱。
uint32_t s_read_seq = 0;
// 当前打开的书文件名，重排序时用来把打开下标搬过去。
char s_cur_file[128] = {};

SemaphoreHandle_t s_lock = nullptr;

// 当前打开的书。
struct Line {
    uint32_t off;   // 在 s_text 里的字节偏移
    uint16_t len;   // 字节长度，不含换行
    uint16_t gap;   // 该行之后是否留段距
};

uint8_t *s_text = nullptr;
size_t s_text_len = 0;
Line *s_lines = nullptr;
int s_line_count = 0;
int s_line_cap = 0;
uint16_t *s_page_first = nullptr;
int s_page_count = 0;
int s_page_cap = 0;
int s_open_index = -1;
int s_page = 0;

// 目录条目：标题是 s_text 里的一段（off/len），page 是它所在的页码。
struct TocEntry {
    uint32_t off;
    uint16_t len;
    uint16_t page;
};
constexpr int kMaxToc = 600;
TocEntry *s_toc = nullptr;
int s_toc_count = 0;
int s_toc_cap = 0;

// 书签：页号升序，kNoBookmark 表示空槽。
constexpr int kMaxBookmarks = 8;
constexpr uint16_t kNoBookmark = 0xFFFF;
uint16_t s_bookmarks[kMaxBookmarks] = {};
int s_bookmark_count = 0;

void set_diag(const char *text)
{
    std::snprintf(s_diag, sizeof(s_diag), "%s", text);
}

class StoreLock {
public:
    StoreLock()
    {
        if (!s_lock) s_lock = xSemaphoreCreateMutex();
        held = s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) == pdTRUE;
    }
    ~StoreLock()
    {
        if (held) xSemaphoreGive(s_lock);
    }
    bool held = false;
};

// ---------- 编码 ----------
inline uint32_t decode_utf8(const uint8_t *p, size_t remain, int *used)
{
    const uint8_t c = p[0];
    if (c < 0x80) { *used = 1; return c; }
    if ((c & 0xE0) == 0xC0 && remain >= 2) {
        *used = 2;
        return ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
    }
    if ((c & 0xF0) == 0xE0 && remain >= 3) {
        *used = 3;
        return ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
    }
    if ((c & 0xF8) == 0xF0 && remain >= 4) {
        *used = 4;
        return ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
               ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
    }
    *used = 1;
    return 0xFFFD;
}

// 少量坏字节不算错（书里常混进个别截断字符），超过 1% 就判定不是 UTF-8。
bool looks_utf8(const uint8_t *s, size_t n)
{
    size_t i = 0, bad = 0;
    while (i < n) {
        const uint8_t c = s[i];
        if (c < 0x80) { ++i; continue; }
        int need;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else { ++bad; ++i; continue; }
        if (i + need >= n) break;
        bool ok = true;
        for (int k = 1; k <= need; ++k)
            if ((s[i + k] & 0xC0) != 0x80) { ok = false; break; }
        if (!ok) { ++bad; ++i; continue; }
        i += need + 1;
    }
    return bad * 100 <= n;
}

size_t encode_utf8(uint8_t *dst, uint32_t cp)
{
    if (cp < 0x80) { dst[0] = static_cast<uint8_t>(cp); return 1; }
    if (cp < 0x800) {
        dst[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
        dst[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
        dst[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
    dst[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 4;
}

uint32_t gb2312_to_cp(uint8_t b1, uint8_t b2)
{
    if (b1 < GB2312_TABLE_FIRST || b1 > GB2312_TABLE_LAST) return 0xFFFD;
    if (b2 < 0xA1 || b2 > 0xFE) return 0xFFFD;
    const uint16_t v = kGb2312Table[(b1 - GB2312_TABLE_FIRST) * GB2312_TABLE_COLS +
                                     (b2 - 0xA1)];
    return v ? v : 0xFFFD;
}

// ---------- 读卡 + 转码（流式） ----------
// 整读再整转是串行的：1.4MB 的书读卡约 1.2s、转码约 0.5s。
// 改成 64KB 分块边读边转，两段重叠，总耗时接近较慢的那一段。
constexpr size_t kReadChunk = 64 * 1024;

enum class SrcEnc { kUtf8, kUtf16le, kUtf16be, kGbk };

// 只看首块判编码：BOM 优先，其次按 UTF-8 合法性嗅探，兜底 GBK。
// 首块 64KB 里一般已经有正文，纯 ASCII 开头、后面才出现中文的书极少。
SrcEnc detect_enc(const uint8_t *head, size_t n, size_t *skip)
{
    *skip = 0;
    if (n >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) {
        *skip = 3;
        return SrcEnc::kUtf8;
    }
    if (n >= 2 && head[0] == 0xFF && head[1] == 0xFE) {
        *skip = 2;
        return SrcEnc::kUtf16le;
    }
    if (n >= 2 && head[0] == 0xFE && head[1] == 0xFF) {
        *skip = 2;
        return SrcEnc::kUtf16be;
    }
    return looks_utf8(head, n) ? SrcEnc::kUtf8 : SrcEnc::kGbk;
}

// 从已打开的 FILE 流式读入并转成 UTF-8，结果在 PSRAM。失败返回 nullptr。
uint8_t *load_text_stream(FILE *f, size_t file_size, size_t *out_len,
                          const char **out_enc)
{
    // GBK/GB2312 与 UTF-16 都是 2 字节转 3 字节，最坏 1.5 倍。
    const size_t cap = file_size + file_size / 2 + 64;
    auto *dst = static_cast<uint8_t *>(
        heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!dst) {
        set_diag("内存不足，无法载入这本书");
        return nullptr;
    }
    auto *buf = static_cast<uint8_t *>(
        heap_caps_malloc(kReadChunk + 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        heap_caps_free(dst);
        set_diag("内存不足，无法载入这本书");
        return nullptr;
    }

    size_t avail = std::fread(buf, 1, kReadChunk, f);
    if (avail == 0) {
        heap_caps_free(buf);
        heap_caps_free(dst);
        set_diag("读取文件失败");
        return nullptr;
    }

    size_t skip = 0;
    const SrcEnc enc = detect_enc(buf, avail, &skip);
    if (enc == SrcEnc::kUtf8) *out_enc = skip ? "UTF-8 BOM" : "UTF-8";
    else if (enc == SrcEnc::kUtf16le) *out_enc = "UTF-16LE";
    else if (enc == SrcEnc::kUtf16be) *out_enc = "UTF-16BE";
    else *out_enc = "GB2312/GBK";

    size_t o = 0;
    bool eof = false;
    for (;;) {
        if (avail > skip) {
            // limit 是本轮可安全处理的边界；块尾若压着半个字符，留到下一块。
            size_t limit = avail;
            if (!eof) {
                if (enc == SrcEnc::kGbk) {
                    if (buf[limit - 1] >= 0x80) --limit;   // 双字节首字节，收尾待续
                } else if (enc != SrcEnc::kUtf8) {
                    limit &= ~static_cast<size_t>(1);      // UTF-16 半个码元
                }
            }
            if (limit < skip) limit = skip;
            size_t consumed = limit;

            if (enc == SrcEnc::kUtf8) {
                // UTF-8 直通：块边界截断也不影响，下一块接着拷即可。
                const size_t n = limit - skip;
                if (o + n + 1 <= cap) {
                    std::memcpy(dst + o, buf + skip, n);
                    o += n;
                }
            } else if (enc == SrcEnc::kGbk) {
                // GB2312 区转换；扩展区落在表外，输出全角问号占位。
                size_t i = skip;
                while (i < limit) {
                    const uint8_t c = buf[i];
                    uint32_t cp;
                    if (c < 0x80) {
                        cp = c;
                        ++i;
                    } else if (i + 1 < limit) {
                        cp = gb2312_to_cp(c, buf[i + 1]);
                        i += 2;
                    } else {
                        cp = 0xFFFD;
                        ++i;
                    }
                    if (o + 4 > cap) break;
                    o += encode_utf8(dst + o, cp);
                }
            } else {
                size_t i = skip;
                while (i + 1 < limit) {
                    uint32_t u = enc == SrcEnc::kUtf16le
                                     ? (static_cast<uint32_t>(buf[i]) |
                                        (static_cast<uint32_t>(buf[i + 1]) << 8))
                                     : ((static_cast<uint32_t>(buf[i]) << 8) |
                                        static_cast<uint32_t>(buf[i + 1]));
                    i += 2;
                    if (u >= 0xD800 && u <= 0xDBFF) {
                        if (i + 1 < limit) {
                            const uint32_t lo =
                                enc == SrcEnc::kUtf16le
                                    ? (static_cast<uint32_t>(buf[i]) |
                                       (static_cast<uint32_t>(buf[i + 1]) << 8))
                                    : ((static_cast<uint32_t>(buf[i]) << 8) |
                                       static_cast<uint32_t>(buf[i + 1]));
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                                i += 2;
                            }
                        } else if (!eof) {
                            i -= 2;          // 代理对跨块，整对留到下一块
                            consumed = i;
                            break;
                        }
                    }
                    if (o + 4 > cap) break;
                    o += encode_utf8(dst + o, u);
                }
            }

            const size_t left = avail - consumed;
            if (left) std::memmove(buf, buf + consumed, left);
            avail = left;
            skip = 0;
        }

        if (eof) break;
        const size_t got = std::fread(buf + avail, 1, kReadChunk, f);
        if (got == 0) {
            eof = true;      // 再走一轮把剩余数据（不再收尾）处理完
            continue;
        }
        avail += got;
    }

    heap_caps_free(buf);
    dst[o] = '\0';
    *out_len = o;
    return dst;
}

// ---------- 排版 ----------

bool no_line_start(uint32_t cp)
{
    switch (cp) {
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1B:  // ；
    case 0xFF1A:  // ：
    case 0xFF1F:  // ？
    case 0xFF01:  // ！
    case 0xFF09:  // ）
    case 0x3009:
    case 0x300B:
    case 0x300D:
    case 0x300F:
    case 0x3011:
    case 0x3015:
    case 0x2026:  // …
    case 0x00B7:  // ·
    case 0x201D:
    case 0x2019:
    case 0xFF5D:  // ｝
        return true;
    default:
        return false;
    }
}

void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = std::strlen(src);
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

bool join_path(char *out, size_t cap, const char *dir, const char *name)
{
    const size_t d = std::strlen(dir);
    const size_t n = std::strlen(name);
    if (d + 1 + n + 1 > cap) return false;
    std::memcpy(out, dir, d);
    out[d] = '/';
    std::memcpy(out + d + 1, name, n + 1);
    return true;
}

bool ensure_line_cap(int need)
{
    if (need <= s_line_cap) return true;
    int cap = s_line_cap ? s_line_cap : 256;
    while (cap < need) cap *= 2;
    auto *p = static_cast<Line *>(heap_caps_realloc(
        s_lines, sizeof(Line) * cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p) {
        const size_t want = sizeof(Line) * static_cast<size_t>(cap);
        ESP_LOGW(kTag, "line table alloc failed: cap=%d want=%uKB psram_free=%uKB",
                 cap, static_cast<unsigned>(want / 1024),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        set_diag("行表内存不足");
        return false;
    }
    s_lines = p;
    s_line_cap = cap;
    return true;
}

void push_line(uint32_t off, uint32_t len, bool gap)
{
    if (len == 0) return;
    if (s_line_count >= s_line_cap && !ensure_line_cap(s_line_count + 1)) return;
    s_lines[s_line_count++] = Line{off, static_cast<uint16_t>(len),
                                   static_cast<uint16_t>(gap ? 1 : 0)};
}

// 每页行数上限：普通模式沿用字库给的 12 行（9 行是 16px 回退），
// 全屏按可用高度直接算，12px 行距下能到 21 行。
int layout_max_lines()
{
    if (!s_full) return book_font_max_lines();
    const int pitch = book_font_line_pitch();
    const int n = s_page_max_h / pitch;
    return n < 1 ? 1 : n;
}

bool layout_text()
{
    s_line_count = 0;
    // 320px 宽、12px 字，一行约 26 个全角字（78 字节），按 len/48 估上界；
    // 短行多时 push_line 会按需翻倍。原来按 len/16 估，虚高约 5 倍，
    // 2.1MB 的文本要预留 2MB 行表，是打开大书失败的直接原因。
    if (!ensure_line_cap(static_cast<int>(s_text_len / 48) + 512)) return false;
    const uint8_t *text = s_text;
    const uint8_t *end = text + s_text_len;
    const uint8_t *p = text;
    const uint8_t *line_start = text;
    const uint8_t *last_space = nullptr;
    int w = 0;
    // 宽度度量提到循环外：ASCII 走 DRAM 里的 128 项快查表，
    // 全角是常量，避免每个字都跨文件调函数 + 线性扫 flash 索引表。
    const uint8_t *ascii_w = book_font_ascii_table();
    const int full_w = book_font_full_w();

    while (p < end) {
        const uint8_t c = *p;
        if (c == '\r') { ++p; continue; }
        if (c == '\n') {
            push_line(static_cast<uint32_t>(line_start - text),
                      static_cast<uint32_t>(p - line_start), true);
            ++p;
            // 连续空行合并成一个段距
            while (p < end && (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t')) ++p;
            line_start = p;
            last_space = nullptr;
            w = 0;
            continue;
        }

        int used = 1;
        int cw = full_w;
        uint32_t cp = 0;
        if (c < 0x80) {
            cp = c;
            used = 1;
            cw = ascii_w ? ascii_w[c] : 8;
        } else if (c >= 0xE0 && c < 0xF0) {
            // 三字节区（汉字、全角标点）：宽度固定，码点只在断行判禁则时才解，
            // 大多数行不会走到断行，这样能省掉绝大多数解码。
            used = 3;
        } else {
            cp = decode_utf8(p, static_cast<size_t>(end - p), &used);
        }
        if (p + used > end) used = static_cast<int>(end - p);
        if (used <= 0) break;

        if (w + cw > s_content_w) {
            if (cp == 0) cp = decode_utf8(p, static_cast<size_t>(end - p), &used);
            // 行首禁则：标点挤进当前行，最多超一个全角宽
            if (no_line_start(cp) && w + cw <= s_content_w + 16) {
                w += cw;
                p += used;
                continue;
            }
            // 优先在最近一个空格处断，避免把英文单词劈开
            if (last_space && last_space > line_start) {
                push_line(static_cast<uint32_t>(line_start - text),
                          static_cast<uint32_t>(last_space - line_start), false);
                const uint8_t *q = last_space;
                while (q < p && *q == ' ') ++q;
                line_start = q;
                p = q;
            } else {
                push_line(static_cast<uint32_t>(line_start - text),
                          static_cast<uint32_t>(p - line_start), false);
                line_start = p;
            }
            last_space = nullptr;
            w = 0;
            continue;
        }

        if (cp == ' ') last_space = p;
        w += cw;
        p += used;
    }
    if (line_start < end) {
        push_line(static_cast<uint32_t>(line_start - text),
                  static_cast<uint32_t>(end - line_start), false);
    }

    if (s_line_count == 0) {
        set_diag("这本书是空的");
        return false;
    }

    // 分页：一页至少 8 行，页数上界按行数/8 估，避免按 /2 虚高 6 倍。
    const int need = s_line_count / 8 + 8;
    if (need > s_page_cap) {
        auto *p2 = static_cast<uint16_t *>(heap_caps_realloc(
            s_page_first, sizeof(uint16_t) * need,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!p2) {
            set_diag("页表内存不足");
            return false;
        }
        s_page_first = p2;
        s_page_cap = need;
    }
    s_page_count = 0;
    s_page_first[s_page_count++] = 0;
    const int pitch = book_font_line_pitch();
    const int gap = book_font_gap_pitch();
    const int max_lines = layout_max_lines();
    int h = 0, lines = 0;
    for (int i = 0; i < s_line_count; ++i) {
        const int add = pitch + (s_lines[i].gap ? gap : 0);
        if (lines > 0 && (lines >= max_lines || h + add > s_page_max_h)) {
            s_page_first[s_page_count++] = static_cast<uint16_t>(i);
            h = 0;
            lines = 0;
        }
        h += add;
        ++lines;
    }
    return true;
}

int page_line_count(int page)
{
    if (page < 0 || page >= s_page_count) return 0;
    const int start = s_page_first[page];
    const int end = (page + 1 < s_page_count) ? s_page_first[page + 1] : s_line_count;
    int n = end - start;
    const int max_lines = layout_max_lines();
    if (n > max_lines) n = max_lines;
    return n < 0 ? 0 : n;
}

// ---------- 目录扫描 ----------
// 中文字面量的字节长度（UTF-8），避免手写 3/6/9。
constexpr int lit_len(const char *s)
{
    int n = 0;
    while (s[n]) ++n;
    return n;
}

bool bytes_starts_with(const uint8_t *p, int n, const char *lit)
{
    const int l = lit_len(lit);
    return n >= l && std::memcmp(p, lit, static_cast<size_t>(l)) == 0;
}

bool bytes_ends_with(const uint8_t *p, int n, const char *lit)
{
    const int l = lit_len(lit);
    return n >= l && std::memcmp(p + n - l, lit, static_cast<size_t>(l)) == 0;
}

bool bytes_contains(const uint8_t *p, int n, const char *lit)
{
    const int l = lit_len(lit);
    if (l <= 0 || n < l) return false;
    for (int i = 0; i + l <= n; ++i)
        if (std::memcmp(p + i, lit, static_cast<size_t>(l)) == 0) return true;
    return false;
}

// 章节标题判定。宁可漏判也不要把正文段当标题，所以三条硬条件同时满足才算：
//   1) 整行以换行结束（gap），说明它本身就是独立的一行，不是折行产物；
//   2) 不超过 72 字节（24 个汉字），标题一般不会更长；
//   3) 不以句读标点收尾，正文句子常带。
bool is_chapter_head(const uint8_t *p, int n, bool gap)
{
    if (!gap || n <= 0 || n > 72) return false;
    static const char *const kEnds[] = {"。", "？", "！", "，", "、", "；", "：", "…"};
    for (const char *e : kEnds)
        if (bytes_ends_with(p, n, e)) return false;
    // 行首空白（缩进）跳过后再判
    for (;;) {
        if (n > 0 && (*p == ' ' || *p == '\t')) { ++p; --n; continue; }
        if (n >= 3 && std::memcmp(p, "\xE3\x80\x80", 3) == 0) { p += 3; n -= 3; continue; }
        break;
    }
    if (n <= 0) return false;

    static const char *const kPrefix[] = {"序章", "序言", "序",   "楔子", "引子",
                                          "前言", "尾声", "后记", "终章", "番外",
                                          "外传", "附录", "后序", "缘起"};
    for (const char *s : kPrefix)
        if (bytes_starts_with(p, n, s)) return true;

    // 「第 X 章/节/回/卷/篇/集/幕」，在行首 24 字节内找关键字（「第一百二十三章」
    // 这种长序号要去到第 18 字节），避免正文里的「第三章讲到……」被算进来。
    if (bytes_starts_with(p, n, "第")) {
        const int lim = n < 24 ? n : 24;
        static const char *const kUnit[] = {"章", "节", "回", "卷", "篇", "集", "幕", "部"};
        for (const char *u : kUnit)
            if (bytes_contains(p + 3, lim - 3, u)) return true;
    }

    // 「卷一 风起」这类：卷 + 序号，排除「卷起袖子」这种动词开头。
    if (bytes_starts_with(p, n, "卷") && n > 3) {
        static const char *const kNum[] = {"一", "二", "三", "四", "五", "六",
                                           "七", "八", "九", "十", "零", "第",
                                           "壹", "贰", "叁", "肆", "伍", "陆"};
        for (const char *u : kNum)
            if (bytes_starts_with(p + 3, n - 3, u)) return true;
        if (p[3] >= '0' && p[3] <= '9') return true;
    }

    // 英文常见写法：Chapter 1 / CHAPTER IV / Part Two / Section 3
    if (bytes_starts_with(p, n, "Chapter") || bytes_starts_with(p, n, "CHAPTER") ||
        bytes_starts_with(p, n, "Part ") || bytes_starts_with(p, n, "PART ") ||
        bytes_starts_with(p, n, "Section") || bytes_starts_with(p, n, "SECTION")) {
        return true;
    }
    return false;
}

int page_of_line(int line)
{
    int lo = 0, hi = s_page_count - 1, ans = 0;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (s_page_first[mid] <= line) { ans = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return ans;
}

// 字节偏移 → 行号（取偏移不超过 off 的最后一行）。行首偏移升序，可二分。
int line_at_or_before(uint32_t off)
{
    if (!s_lines || s_line_count <= 0) return 0;
    int lo = 0, hi = s_line_count - 1, ans = 0;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (s_lines[mid].off <= off) { ans = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return ans;
}

bool ensure_toc_cap(int need)
{
    if (need <= s_toc_cap) return true;
    int cap = s_toc_cap ? s_toc_cap : 64;
    while (cap < need) cap *= 2;
    if (cap > kMaxToc) cap = kMaxToc;
    auto *p = static_cast<TocEntry *>(heap_caps_realloc(
        s_toc, sizeof(TocEntry) * static_cast<size_t>(cap),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p) {
        ESP_LOGW(kTag, "toc alloc failed: cap=%d", cap);
        return false;
    }
    s_toc = p;
    s_toc_cap = cap;
    return true;
}

void build_toc()
{
    s_toc_count = 0;
    if (s_line_count <= 0) return;
    for (int i = 0; i < s_line_count && s_toc_count < kMaxToc; ++i) {
        const Line &ln = s_lines[i];
        if (!ln.gap) continue;
        const uint8_t *p = s_text + ln.off;
        if (!is_chapter_head(p, ln.len, ln.gap != 0)) continue;
        if (s_toc_count >= s_toc_cap && !ensure_toc_cap(s_toc_count + 1)) break;
        s_toc[s_toc_count++] = TocEntry{ln.off, ln.len,
                                        static_cast<uint16_t>(page_of_line(i))};
    }
    ESP_LOGI(kTag, "toc: %d entries", s_toc_count);
    if (s_toc_count > 0) {
        const TocEntry &a = s_toc[0];
        const TocEntry &b = s_toc[s_toc_count - 1];
        ESP_LOGI(kTag, "toc first: p%d %.*s | last: p%d %.*s", a.page + 1, a.len,
                 reinterpret_cast<const char *>(s_text + a.off), b.page + 1, b.len,
                 reinterpret_cast<const char *>(s_text + b.off));
    }
}

// ---------- NVS 进度 ----------
uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= static_cast<uint8_t>(*s++);
        h *= 16777619u;
    }
    return h;
}

void progress_key(const char *file, char *out, size_t cap)
{
    std::snprintf(out, cap, "p%08x", static_cast<unsigned>(fnv1a(file)));
}

uint16_t load_progress(const char *file)
{
    char key[16];
    progress_key(file, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t v = 0;
    const esp_err_t err = nvs_get_u32(h, key, &v);
    nvs_close(h);
    return (err == ESP_OK && v < 0xFFFF) ? static_cast<uint16_t>(v) : 0;
}

void save_progress(const char *file, uint16_t page)
{
    char key[16];
    progress_key(file, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, page);
    nvs_commit(h);
    nvs_close(h);
}

// 页号只在同一套排版口径下有效（普通/全屏页数差一倍），所以另存一份
// 「本页首行的字节偏移」当锚点，换模式或换字库后也能回到原处。
constexpr uint32_t kNoOffset = 0xFFFFFFFFu;

uint32_t load_progress_offset(const char *file)
{
    char key[16];
    std::snprintf(key, sizeof(key), "o%08x", static_cast<unsigned>(fnv1a(file)));
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return kNoOffset;
    uint32_t v = kNoOffset;
    const esp_err_t err = nvs_get_u32(h, key, &v);
    nvs_close(h);
    return err == ESP_OK ? v : kNoOffset;
}

void save_progress_offset(const char *file, uint32_t off)
{
    char key[16];
    std::snprintf(key, sizeof(key), "o%08x", static_cast<unsigned>(fnv1a(file)));
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, off);
    nvs_commit(h);
    nvs_close(h);
}

// 当前页首行的字节偏移，没有内容时返回 0。
uint32_t current_page_offset()
{
    if (s_page_count <= 0 || !s_lines || !s_page_first) return 0;
    const int line = s_page_first[s_page];
    if (line < 0 || line >= s_line_count) return 0;
    return s_lines[line].off;
}

// 累计阅读时长的 key 与页进度共用文件名 hash，前缀换一个字母区分。
uint32_t load_seconds(const char *file)
{
    char key[16];
    std::snprintf(key, sizeof(key), "t%08x", static_cast<unsigned>(fnv1a(file)));
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t v = 0;
    const esp_err_t err = nvs_get_u32(h, key, &v);
    nvs_close(h);
    return err == ESP_OK ? v : 0;
}

void save_seconds(const char *file, uint32_t sec)
{
    char key[16];
    std::snprintf(key, sizeof(key), "t%08x", static_cast<unsigned>(fnv1a(file)));
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, sec);
    nvs_commit(h);
    nvs_close(h);
}

// ---------- NVS 阅读序号（书架排序用） ----------
uint32_t load_read_seq()
{
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t v = 0;
    const esp_err_t err = nvs_get_u32(h, "seq", &v);
    nvs_close(h);
    return err == ESP_OK ? v : 0;
}

void save_read_seq(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "seq", v);
    nvs_commit(h);
    nvs_close(h);
}

uint32_t load_book_seq(const char *file)
{
    char key[16];
    std::snprintf(key, sizeof(key), "r%08x", static_cast<unsigned>(fnv1a(file)));
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t v = 0;
    const esp_err_t err = nvs_get_u32(h, key, &v);
    nvs_close(h);
    return err == ESP_OK ? v : 0;
}

void save_book_seq(const char *file, uint32_t v)
{
    char key[16];
    std::snprintf(key, sizeof(key), "r%08x", static_cast<unsigned>(fnv1a(file)));
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

// ---------- NVS 书签 ----------
// 一本书 8 个书签页号打包成一个 16 字节 blob，key 前缀 b。
void bookmark_key(const char *file, char *out, size_t cap)
{
    std::snprintf(out, cap, "b%08x", static_cast<unsigned>(fnv1a(file)));
}

void load_bookmarks(const char *file)
{
    s_bookmark_count = 0;
    for (int i = 0; i < kMaxBookmarks; ++i) s_bookmarks[i] = kNoBookmark;
    if (!file || !*file) return;
    char key[16];
    bookmark_key(file, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
    uint16_t raw[kMaxBookmarks] = {};
    size_t len = sizeof(raw);
    const esp_err_t err = nvs_get_blob(h, key, raw, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(raw)) return;
    // 落到数组里时保持升序，顺手丢掉越界项。
    for (int i = 0; i < kMaxBookmarks; ++i) {
        if (raw[i] == kNoBookmark || raw[i] >= 0x7FFF) continue;
        s_bookmarks[s_bookmark_count++] = raw[i];
    }
    ESP_LOGI(kTag, "bookmarks: %d loaded", s_bookmark_count);
}

void save_bookmarks(const char *file)
{
    if (!file || !*file) return;
    char key[16];
    bookmark_key(file, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    uint16_t raw[kMaxBookmarks];
    for (int i = 0; i < kMaxBookmarks; ++i) {
        raw[i] = (i < s_bookmark_count) ? s_bookmarks[i] : kNoBookmark;
    }
    nvs_set_blob(h, key, raw, sizeof(raw));
    nvs_commit(h);
    nvs_close(h);
}

// ---------- 目录 ----------
bool has_book_ext(const char *name)
{
    const size_t n = std::strlen(name);
    if (n < 5) return false;
    const char *dot = std::strrchr(name, '.');
    if (!dot) return false;
    char ext[8];
    std::snprintf(ext, sizeof(ext), "%s", dot);
    for (char *q = ext; *q; ++q) *q = static_cast<char>(tolower(*q));
    return std::strcmp(ext, ".txt") == 0 || std::strcmp(ext, ".epub") == 0;
}

bool contains_cjk(const char *s)
{
    const auto *p = reinterpret_cast<const unsigned char *>(s);
    while (*p) {
        if (*p >= 0xE0) return true;
        ++p;
    }
    return false;
}

// 文件名 "书名 - 作者.txt" / "书名_作者.txt" 都能拆。
void parse_names(const char *file, BookMeta *m)
{
    char stem[128];
    copy_str(stem, sizeof(stem), file);
    char *dot = std::strrchr(stem, '.');
    if (dot) *dot = '\0';

    char *sep = std::strstr(stem, " - ");
    if (!sep) sep = std::strchr(stem, '_');
    char *author = nullptr;
    if (sep) {
        *sep = '\0';
        author = sep + (sep[1] == ' ' ? 3 : 1);
    }
    while (*stem == ' ') std::memmove(stem, stem + 1, std::strlen(stem));
    if (author) {
        while (*author == ' ' || *author == '-' || *author == '_') ++author;
        copy_str(m->author, sizeof(m->author), author);
    }
    if (contains_cjk(stem)) {
        copy_str(m->title_zh, sizeof(m->title_zh), stem);
    } else {
        copy_str(m->title_en, sizeof(m->title_en), stem);
    }
}

bool ensure_dir(const char *path)
{
    struct stat st = {};
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) != 0;
    if (mkdir(path, 0755) != 0) {
        ESP_LOGW(kTag, "mkdir %s failed", path);
        return false;
    }
    return true;
}

void seed_sample_if_missing()
{
    DIR *d = opendir(kBookDir);
    if (d) {
        bool has = false;
        while (auto *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            if (has_book_ext(e->d_name)) { has = true; break; }
        }
        closedir(d);
        if (has) return;
    } else if (!ensure_dir(kBookDir)) {
        set_diag("无法创建 /sdcard/book 目录");
        return;
    }

    char path[160];
    if (!join_path(path, sizeof(path), kBookDir, kSampleBookName)) {
        set_diag("样例书路径过长");
        return;
    }
    FILE *f = std::fopen(path, "wb");
    if (!f) {
        ESP_LOGW(kTag, "seed sample failed: cannot open %s", path);
        set_diag("书库为空，且写入样例书失败");
        return;
    }
    const size_t len = std::strlen(kSampleBookText);
    const size_t wrote = std::fwrite(kSampleBookText, 1, len, f);
    std::fclose(f);
    ESP_LOGI(kTag, "seeded sample book %s (%u/%u bytes)", path,
             static_cast<unsigned>(wrote), static_cast<unsigned>(len));
}

void log_font_dir()
{
    DIR *d = opendir(kFontDir);
    if (!d) {
        ESP_LOGI(kTag, "font dir %s not found", kFontDir);
        return;
    }
    int bin = 0, ttf = 0, other = 0;
    while (auto *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const char *dot = std::strrchr(e->d_name, '.');
        if (!dot) { ++other; continue; }
        if (strcasecmp(dot, ".bin") == 0) ++bin;
        else if (strcasecmp(dot, ".ttf") == 0) ++ttf;
        else ++other;
    }
    closedir(d);
    ESP_LOGI(kTag, "font dir: %d bin, %d ttf, %d other (not loaded yet)", bin, ttf, other);
}

// 最近读过的排前面；都没读过（序号 0）时按文件名，保证顺序稳定。
int cmp_book(const void *a, const void *b)
{
    const auto *x = static_cast<const BookMeta *>(a);
    const auto *y = static_cast<const BookMeta *>(b);
    if (x->last_seq != y->last_seq) return x->last_seq < y->last_seq ? 1 : -1;
    return std::strcmp(x->file, y->file);
}

int index_of_file(const char *file)
{
    if (!file || !*file) return -1;
    for (int i = 0; i < s_count; ++i)
        if (std::strcmp(s_books[i].file, file) == 0) return i;
    return -1;
}

void free_open_state()
{
    if (s_text) { heap_caps_free(s_text); s_text = nullptr; }
    if (s_lines) { heap_caps_free(s_lines); s_lines = nullptr; }
    if (s_page_first) { heap_caps_free(s_page_first); s_page_first = nullptr; }
    if (s_toc) { heap_caps_free(s_toc); s_toc = nullptr; }
    s_text_len = 0;
    s_line_count = 0;
    s_line_cap = 0;
    s_page_count = 0;
    s_page_cap = 0;
    s_page = 0;
    s_toc_count = 0;
    s_toc_cap = 0;
    s_bookmark_count = 0;
    for (int i = 0; i < kMaxBookmarks; ++i) s_bookmarks[i] = kNoBookmark;
    s_open_index = -1;
}

}  // namespace

// ---------------- 公共接口 ----------------

int book_store_refresh(void)
{
    StoreLock lock;
    if (!lock.held) {
        set_diag("书库忙，稍后再试");
        return 0;
    }
    s_count = 0;
    s_read_seq = load_read_seq();
    // 首次进阅读模块时 RomStore 可能还在异步挂卡，这里给最多约 1 秒的重试，
    // 否则会先扫出 0 本、显示空书库，用户以为卡没插好。
    bool mounted = rom_store_mount();
    for (int i = 0; i < 10 && !mounted; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
        mounted = rom_store_mount();
    }
    if (!mounted) {
        set_diag("未检测到 TF 卡");
        return 0;
    }
    if (!ensure_dir(kBookDir)) {
        set_diag("无法创建 /sdcard/book 目录");
        return 0;
    }
    seed_sample_if_missing();
    log_font_dir();

    DIR *d = opendir(kBookDir);
    if (!d) {
        set_diag("无法打开 /sdcard/book 目录");
        return 0;
    }
    while (auto *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (!has_book_ext(e->d_name)) continue;
        if (s_count >= BOOK_STORE_MAX) break;

        char path[512];
        if (!join_path(path, sizeof(path), kBookDir, e->d_name)) continue;
        struct stat st = {};
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        BookMeta &m = s_books[s_count];
        std::memset(&m, 0, sizeof(m));
        copy_str(m.file, sizeof(m.file), e->d_name);
        m.size = static_cast<uint32_t>(st.st_size);
        const char *dot = std::strrchr(e->d_name, '.');
        m.type = (dot && strcasecmp(dot, ".epub") == 0) ? BOOK_TYPE_EPUB : BOOK_TYPE_TXT;
        parse_names(e->d_name, &m);
        m.pages = 0;
        m.progress = load_progress(m.file);
        m.seconds = load_seconds(m.file);
        m.last_seq = load_book_seq(m.file);
        ++s_count;
    }
    closedir(d);

    if (s_count > 1) qsort(s_books, s_count, sizeof(BookMeta), cmp_book);
    // 扫描前若还有书开着（正常流程不会），把打开下标挪到新位置，
    // 否则后面保存进度会写错书。
    if (s_open_index >= 0) s_open_index = index_of_file(s_cur_file);

    if (s_count == 0) {
        set_diag("书库为空，拷 .txt 到 /sdcard/book");
        return 0;
    }
    set_diag("ok");
    ESP_LOGI(kTag, "scanned %d books in %s", s_count, kBookDir);
    for (int i = 0; i < s_count; ++i) {
        ESP_LOGI(kTag, "  [%d] %s  %u bytes  type=%d  progress=%u", i,
                 s_books[i].file, static_cast<unsigned>(s_books[i].size),
                 static_cast<int>(s_books[i].type),
                 static_cast<unsigned>(s_books[i].progress));
    }
    return s_count;
}

int book_store_count(void)
{
    return s_count;
}

const BookMeta *book_store_meta(int index)
{
    if (index < 0 || index >= s_count) return nullptr;
    return &s_books[index];
}

bool book_store_open(int index)
{
    StoreLock lock;
    if (!lock.held) {
        set_diag("书库忙，稍后再试");
        return false;
    }
    if (index < 0 || index >= s_count) {
        set_diag("书目越界");
        return false;
    }
    book_font_load();
    const BookMeta &m = s_books[index];
    if (m.type == BOOK_TYPE_EPUB) {
        set_diag("EPUB 暂未支持，先看 TXT");
        return false;
    }
    if (m.size == 0 || m.size > kMaxFileBytes) {
        set_diag(m.size == 0 ? "文件是空的" : "文件超过 2MB 上限");
        return false;
    }

    char path[512];
    if (!join_path(path, sizeof(path), kBookDir, m.file)) {
        set_diag("文件路径过长");
        return false;
    }
    const int64_t t_open_us = esp_timer_get_time();
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        set_diag("打开文件失败");
        return false;
    }
    // 先释放上一本书占的 PSRAM，再分配新书，避免峰值叠加。
    free_open_state();

    const char *enc = "UTF-8";
    s_text = load_text_stream(f, m.size, &s_text_len, &enc);
    std::fclose(f);
    if (!s_text) return false;
    // 容量按最坏 1.5 倍预留，缩到实长把 PSRAM 让给行表。
    // 缩小 realloc 通常原地完成，不产生拷贝。
    if (auto *shrunk = static_cast<uint8_t *>(heap_caps_realloc(
            s_text, s_text_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
        s_text = shrunk;
    }
    const int64_t t_conv_us = esp_timer_get_time();

    if (!layout_text()) {
        free_open_state();
        return false;
    }
    const int64_t t_layout_us = esp_timer_get_time();
    build_toc();
    load_bookmarks(m.file);

    s_open_index = index;
    copy_str(s_cur_file, sizeof(s_cur_file), m.file);
    // 记一次「最近阅读」：全局序号 +1 并落到这本书上，书架据此排序。
    if (s_read_seq == 0) s_read_seq = load_read_seq();
    ++s_read_seq;
    save_read_seq(s_read_seq);
    save_book_seq(m.file, s_read_seq);
    s_books[index].last_seq = s_read_seq;

    int saved = m.progress;
    // 有字节锚点时以它为准：页号在切换全屏/换字库后会失配，锚点不会。
    const uint32_t anchor = load_progress_offset(m.file);
    if (anchor != kNoOffset) {
        saved = page_of_line(line_at_or_before(anchor));
    }
    if (saved < 0 || saved >= s_page_count) saved = 0;
    s_page = saved;
    s_books[index].pages = static_cast<uint16_t>(s_page_count);

    ESP_LOGI(kTag, "open timing: read+convert=%lldms layout=%lldms",
             static_cast<long long>((t_conv_us - t_open_us) / 1000),
             static_cast<long long>((t_layout_us - t_conv_us) / 1000));
    ESP_LOGI(kTag, "opened [%d] %s enc=%s text=%u bytes lines=%d pages=%d at page %d "
             "psram_free=%uKB",
             index, m.file, enc, static_cast<unsigned>(s_text_len), s_line_count,
             s_page_count, s_page,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    set_diag("ok");
    return true;
}

void book_store_close(void)
{
    StoreLock lock;
    if (!lock.held) return;
    if (s_open_index >= 0 && s_open_index < s_count) {
        save_progress(s_books[s_open_index].file, static_cast<uint16_t>(s_page));
        save_progress_offset(s_books[s_open_index].file, current_page_offset());
        s_books[s_open_index].progress = static_cast<uint16_t>(s_page);
    }
    free_open_state();
    s_open_index = -1;
    s_cur_file[0] = '\0';
}

int book_store_resort_recent(void)
{
    if (s_count <= 0) return -1;
    StoreLock lock;
    if (!lock.held) return -1;
    if (s_count > 1) qsort(s_books, s_count, sizeof(BookMeta), cmp_book);
    if (s_open_index >= 0) s_open_index = index_of_file(s_cur_file);
    return s_open_index;
}

bool book_store_is_open(void)
{
    return s_open_index >= 0;
}

int book_store_open_index(void)
{
    return s_open_index;
}

int book_store_page_count(void)
{
    return s_page_count;
}

int book_store_current_page(void)
{
    return s_page;
}

bool book_store_goto_page(int page)
{
    if (s_page_count <= 0) return false;
    if (page < 0) page = 0;
    if (page >= s_page_count) page = s_page_count - 1;
    if (page == s_page) return false;
    s_page = page;
    if (s_open_index >= 0 && s_open_index < s_count) {
        save_progress(s_books[s_open_index].file, static_cast<uint16_t>(s_page));
        save_progress_offset(s_books[s_open_index].file, current_page_offset());
        s_books[s_open_index].progress = static_cast<uint16_t>(s_page);
    }
    return true;
}

bool book_store_set_fullscreen(bool on)
{
    if (s_full == on) return true;
    s_full = on;
    s_content_w = on ? kFullContentWidth : kContentWidth;
    s_page_max_h = on ? kFullPageMaxHeight : kPageMaxHeight;
    if (s_open_index < 0) return true;   // 没开书，下次打开按新口径分页
    StoreLock lock;
    if (!lock.held) return false;

    const uint32_t anchor = current_page_offset();
    if (!layout_text()) return false;
    s_page = page_of_line(line_at_or_before(anchor));
    build_toc();          // 页号全变了，目录要跟着重算
    ESP_LOGI(kTag, "fullscreen=%d pages=%d page=%d", on ? 1 : 0, s_page_count,
             s_page);
    return true;
}

bool book_store_fullscreen(void)
{
    return s_full;
}

int book_store_page_lines(int page)
{
    return page_line_count(page);
}

const char *book_store_line(int page, int nth, int *out_len, bool *out_gap)
{
    const int count = page_line_count(page);
    if (nth < 0 || nth >= count) return nullptr;
    const Line &ln = s_lines[s_page_first[page] + nth];
    if (out_len) *out_len = ln.len;
    if (out_gap) *out_gap = ln.gap != 0;
    return reinterpret_cast<const char *>(s_text + ln.off);
}

int book_store_copy_line(int page, int nth, char *buf, int cap, bool *out_gap)
{
    if (!buf || cap <= 0) return 0;
    int len = 0;
    const char *src = book_store_line(page, nth, &len, out_gap);
    if (!src) {
        buf[0] = '\0';
        return 0;
    }
    if (len > cap - 1) len = cap - 1;
    std::memcpy(buf, src, static_cast<size_t>(len));
    buf[len] = '\0';
    return len;
}

int book_store_progress_of(int index)
{
    if (index < 0 || index >= s_count) return 0;
    return s_books[index].progress;
}

int book_store_open_progress(void)
{
    return s_page;
}

// ---------- 目录 ----------
int book_store_toc_count(void)
{
    return s_toc_count;
}

int book_store_toc_page(int i)
{
    if (i < 0 || i >= s_toc_count) return -1;
    return s_toc[i].page;
}

int book_store_toc_title(int i, char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    buf[0] = '\0';
    if (i < 0 || i >= s_toc_count) return 0;
    int len = s_toc[i].len;
    if (len > cap - 1) len = cap - 1;
    std::memcpy(buf, s_text + s_toc[i].off, static_cast<size_t>(len));
    buf[len] = '\0';
    return len;
}

// ---------- 书签 ----------
bool book_store_is_bookmarked(int page)
{
    for (int i = 0; i < s_bookmark_count; ++i)
        if (s_bookmarks[i] == static_cast<uint16_t>(page)) return true;
    return false;
}

int book_store_bookmark_count(void)
{
    return s_bookmark_count;
}

int book_store_bookmark_page(int i)
{
    if (i < 0 || i >= s_bookmark_count) return -1;
    return s_bookmarks[i];
}

int book_store_toggle_bookmark(int page)
{
    if (page < 0 || page >= s_page_count || s_open_index < 0) return -1;
    StoreLock lock;
    if (!lock.held) return -1;

    const uint16_t target = static_cast<uint16_t>(page);
    for (int i = 0; i < s_bookmark_count; ++i) {
        if (s_bookmarks[i] != target) continue;
        for (int k = i; k + 1 < s_bookmark_count; ++k) s_bookmarks[k] = s_bookmarks[k + 1];
        --s_bookmark_count;
        s_bookmarks[s_bookmark_count] = kNoBookmark;
        save_bookmarks(s_books[s_open_index].file);
        ESP_LOGI(kTag, "bookmark removed p%d, %d left", page + 1, s_bookmark_count);
        return 0;
    }
    if (s_bookmark_count >= kMaxBookmarks) {
        ESP_LOGW(kTag, "bookmark full (%d)", kMaxBookmarks);
        return -1;
    }
    // 插到升序位置
    int pos = s_bookmark_count;
    while (pos > 0 && s_bookmarks[pos - 1] > target) {
        s_bookmarks[pos] = s_bookmarks[pos - 1];
        --pos;
    }
    s_bookmarks[pos] = target;
    ++s_bookmark_count;
    save_bookmarks(s_books[s_open_index].file);
    ESP_LOGI(kTag, "bookmark added p%d, %d total", page + 1, s_bookmark_count);
    return 1;
}

uint32_t book_store_reading_seconds(int index)
{
    if (index < 0 || index >= s_count) return 0;
    return s_books[index].seconds;
}

void book_store_add_reading_seconds(int index, int seconds)
{
    if (index < 0 || index >= s_count || seconds <= 0) return;
    StoreLock lock;
    if (!lock.held) return;
    uint32_t total = s_books[index].seconds + static_cast<uint32_t>(seconds);
    s_books[index].seconds = total;
    save_seconds(s_books[index].file, total);
}

const char *book_store_diagnostic(void)
{
    return s_diag;
}

bool book_store_mounted(void)
{
    return rom_store_mounted();
}

void book_store_startup_selfcheck(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    xTaskCreate(
        [](void *) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            const int n = book_store_refresh();
            ESP_LOGI(kTag, "selfcheck: %d books, diag=%s", n, book_store_diagnostic());
            if (n <= 0) { vTaskDelete(nullptr); return; }
            for (int i = 0; i < n; ++i) {
                const BookMeta *m = book_store_meta(i);
                if (m) ESP_LOGI(kTag, "  [%d] %s pages=%u progress=%u", i, m->file,
                               static_cast<unsigned>(m->pages),
                               static_cast<unsigned>(m->progress));
            }
            // 只是扫目录，不打开书：开书会写「最近阅读」序号，把用户的书架顺序
            // 改成自检挑中的那本。要现场验证排版时把这里改成 true 再烧一次。
            constexpr bool kOpenOneBook = false;
            if constexpr (kOpenOneBook) {
                int target = -1;
                for (int i = 0; i < n; ++i) {
                    const BookMeta *m = book_store_meta(i);
                    if (m && m->type == BOOK_TYPE_TXT) { target = i; break; }
                }
                if (target >= 0) {
                    const int64_t t0 = esp_timer_get_time();
                    if (book_store_open(target)) {
                        const int64_t ms = (esp_timer_get_time() - t0) / 1000;
                        ESP_LOGI(kTag, "selfcheck: opened [%d] %s, %d pages, %lld ms",
                                 target, book_store_meta(target)->file,
                                 book_store_page_count(), static_cast<long long>(ms));
                        char line[128];
                        const int lines = book_store_page_lines(0);
                        for (int i = 0; i < lines; ++i) {
                            book_store_copy_line(0, i, line, sizeof(line), nullptr);
                            ESP_LOGI(kTag, "  | %s", line);
                        }
                        book_store_close();
                    } else {
                        ESP_LOGW(kTag, "selfcheck: open failed: %s",
                                 book_store_diagnostic());
                    }
                }
            }
            vTaskDelete(nullptr);
        },
        "book_check", 6144, nullptr, 3, nullptr);
}
