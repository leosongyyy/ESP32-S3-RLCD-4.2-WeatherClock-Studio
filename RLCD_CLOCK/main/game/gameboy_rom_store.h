// 声明 TF 卡 rom/gb 目录下的 ROM 枚举、选择、整份载入与 .sav 读写接口。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 挂载 TF 卡（幂等，挂载点 /sdcard，ROM 目录 /sdcard/rom/gb）。
// 失败返回 false，原因见 rom_store_diagnostic()。
bool rom_store_mount(void);
bool rom_store_mounted(void);

// 重新扫描目录，返回找到的 ROM 个数（0..kMaxRoms）。挂载未成功时返回 0。
int rom_store_refresh(void);
int rom_store_count(void);

// 第 index 个 ROM 的 ASCII 文件名与字节数；越界返回空串 / 0。
//
// 线程约定（P1-14）：count/name/size/selected/diagnostic 都是**无锁读**，
// 内部读的是写侧发布的一份不可变快照，所以任意任务随时调都安全，
// 不会读到半扫描的目录表或半个汉字。
// 唯一约束：返回的 const char* 只在**下一次 rom_store_refresh() 之前**有效，
// 需要长期持有请立刻拷进自己的缓冲（现有调用方都是这样做的）。
const char *rom_store_name(int index);
size_t rom_store_size(int index);

// 记录选择（写入 NVS）并夹到有效范围；count 为 0 时返回 false。
bool rom_store_select(int index);
// 按文件名选中（上传完成后用这个——列表是按名字排序的，"最后一个"不是刚传的）。
bool rom_store_select_by_name(const char *name);
int rom_store_selected(void);

// 把 ROM 整份读入 PSRAM（P2：回调只做指针读）。成功后指针在 unload 前保持有效。
//
// 刻意**不**提供「取当前缓冲裸指针」的接口：那会绕开 load/unload 的生命周期约定，
// 让调用方在缓冲已被 free 之后继续解引用。要 ROM 就调 rom_store_load()，
// 它会返回同一份缓冲的首地址。
const uint8_t *rom_store_load(int index, size_t *out_size);
void rom_store_unload(void);

// .sav 读写：文件名固定为 <rom 名>.sav。
bool rom_store_save_cart_ram(int index, const uint8_t *data, size_t size);
size_t rom_store_load_cart_ram(int index, uint8_t *data, size_t capacity);

// 上传/删除文件名合法性：[A-Za-z0-9._-]+ 且以 .gb/.gbc 结尾。
bool rom_store_name_is_valid(const char *name);

// 最近一次失败的人类可读原因（中文，可直接上屏）。
const char *rom_store_diagnostic(void);

// 启动自检：起一个一次性任务，延迟约 8 秒后挂一次 TF 卡并把卡的身份信息、
// rom/gb 目录实况打到日志（tag "RomStore"）。不阻塞启动，可重复调用但不重复执行。
// 用途：引脚/卡槽/卡本身出问题时，串口一抓就能区分是"没认到卡"还是"目录不对"。
void rom_store_startup_selfcheck(void);

#ifdef __cplusplus
}
#endif
