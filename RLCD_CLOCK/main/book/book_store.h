// 阅读数据层：扫描 TF 卡上的 /sdcard/book，把 TXT 读进 PSRAM 并排版分页。
//
// 目录约定（用户手动拷入，固件不建书库内容）：
//   /sdcard/book/*.txt    正文，UTF-8 / UTF-16(BOM) / GB2312 自动识别
//   /sdcard/book/*.epub   本轮只列进书库，打开时提示暂不支持
//   /sdcard/font/*.bin    预留，本轮只扫描并打日志，UI 仍用内置 zh_font_16
//
// 排版口径与 ui_reading.cpp 完全一致：正文统一用 zh_font_16 渲染，
// 半角 8px、全角 16px，内容宽 320px（20 个全角字），行距 17，段距 8，
// 每页最多 9 行且总高不超过 168px。
#pragma once

#include <stdint.h>

// 书库一屏 6 本，最多 64 本。
#define BOOK_STORE_MAX 64

enum BookType {
    BOOK_TYPE_TXT = 0,
    BOOK_TYPE_EPUB = 1,
    BOOK_TYPE_OTHER = 2,
};

struct BookMeta {
    char file[128];      // TF 卡上的文件名（UTF-8，含扩展名）
    char title_zh[96];   // 中文书名，取不到时为空串
    char title_en[96];   // 西文书名，取不到时为空串
    char author[64];     // 作者，取不到时为空串
    uint32_t size;       // 文件字节数
    uint32_t last_seq;   // 最近阅读序号，越大越近，0 表示从没打开过
    uint16_t pages;      // 已知总页数，0 表示还没打开过
    uint16_t progress;   // 已读到第几页（0 起）
    uint32_t seconds;    // 累计阅读秒数
    uint8_t type;        // BookType
};

// 挂载 TF 卡（复用 RomStore 的挂载点 /sdcard，幂等）并扫描 book 目录。
// 排序规则：最近打开过的排在前面（最近一次在最前），从没打开过的按文件名。
// 返回书数，0 表示没有卡、没有目录或目录为空。
int book_store_refresh(void);
int book_store_count(void);
const BookMeta *book_store_meta(int index);

// 按「最近阅读」重新排序，返回当前打开这本书在新顺序里的下标（没开书返回 -1）。
// 书还开着时内部会把打开下标一起搬过去，所以可以在 close() 之前调用。
int book_store_resort_recent(void);

// 打开第 index 本书并完成排版分页。成功返回 true。
// 失败原因见 book_store_diagnostic()，可直接上屏。
bool book_store_open(int index);
void book_store_close(void);
bool book_store_is_open(void);
int book_store_open_index(void);

int book_store_page_count(void);
int book_store_current_page(void);
// 跳页，越界夹住。返回是否真的变了。
bool book_store_goto_page(int page);

// 全屏模式：正文占满 400x300（左右各留 12px），按新宽度重新断行分页，
// 阅读位置用字节锚点接回去。没开书时只记下模式，下次打开照样生效。
bool book_store_set_fullscreen(bool on);
bool book_store_fullscreen(void);

// ---------- 目录 ----------
// 打开书时按行扫描章节标题（第 X 章/节/回/卷、序章、楔子、Chapter N 等），
// 记下每条标题所在的页。没有识别到标题时返回 0。
int book_store_toc_count(void);
int book_store_toc_page(int i);
// 把第 i 条目录的标题拷进 buf（NUL 结尾），返回字节数。
int book_store_toc_title(int i, char *buf, int cap);

// ---------- 书签 ----------
// 每本书最多 8 个，按页号升序存进 NVS（key 由文件名 hash 生成）。
bool book_store_is_bookmarked(int page);
int book_store_bookmark_count(void);
// 第 i 个书签的页号（升序），越界返回 -1。
int book_store_bookmark_page(int i);
// 切换某页的书签：返回 1 新增、0 移除、-1 已满。
int book_store_toggle_bookmark(int page);

// 取第 page 页的行，最多 max_lines 行。
// 行文本不是 NUL 结尾的独立字符串，而是指向整本文本的一段，
// 调用方用 book_store_line_len() 取长度，或直接用 book_store_copy_line()。
int book_store_page_lines(int page);
const char *book_store_line(int page, int nth, int *out_len, bool *out_gap);

// 把第 page 页第 nth 行拷进 buf（NUL 结尾，超长截断），返回实际字节数。
int book_store_copy_line(int page, int nth, char *buf, int cap, bool *out_gap);

// 书库里某本书的已读页（未打开过则从 NVS 读）。
int book_store_progress_of(int index);
// 当前打开的书读到第几页（0 起）。
int book_store_open_progress(void);

// 累计阅读秒数（跨开机持久化，按文件名 hash 存 NVS）。
uint32_t book_store_reading_seconds(int index);
// 把一段阅读时长累加到第 index 本书。seconds <= 0 时忽略。
void book_store_add_reading_seconds(int index, int seconds);

// 最近一次失败原因，中文，可直接上屏。
const char *book_store_diagnostic(void);

// TF 卡是否已挂载。
bool book_store_mounted(void);

// 启动自检：延迟约 10 秒（等 RomStore 先挂卡）扫一次书库，
// 打开第一本 TXT 并打印前几行，用来在不动按键的情况下验证
// 卡的挂载、目录、编码与排版。只执行一次。
void book_store_startup_selfcheck(void);
