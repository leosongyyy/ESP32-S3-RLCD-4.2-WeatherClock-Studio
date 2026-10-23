// 实现 TF（microSD）卡的挂载、ROM 枚举、选择记忆与存档读写。
// ROM 目录固定为 TF 卡根目录下的 rom/gb。
#include "gameboy_rom_store.h"

#include "game_config.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>

#include <driver/sdmmc_host.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

namespace {
constexpr const char *kRomLogTag = "RomStore";
// "/sdcard/rom/gb/"(15) + 文件名(最坏 95 字节) + ".sav"(4) + '\0' = 115。
// UTF-8 下一个汉字 3 字节，95 字节大约是最坏情况（全 3 字节汉字会被长度上限
// 提前截断），这里按字节算不按字符算。留到 176 是为了让最长的名字也绝不会
// 撞上这条边界——build_path() 撞边界会静默返回空串，fopen 直接失败且没有任何日志。
constexpr size_t kPathBytes = 176;
constexpr int kFatMaxFiles = 4;
constexpr int64_t kMountRetryIntervalUs = 5 * 1000 * 1000;
constexpr uint32_t kStoreLockTimeoutMs = 2000;

// VFS 的 mkdir 没有 -p，建 /sdcard/rom/gb 必须先建 /sdcard/rom。
constexpr const char *kRomsParentDir = "/sdcard/rom";

sdmmc_card_t *s_card = nullptr;
std::atomic<bool> s_mounted{false};
std::atomic<int> s_selected{0};
int64_t s_next_mount_attempt_us = 0;

// ui_task（列表刷新）、httpd 任务（上传/删除）与按键任务（载入 ROM）
// 会并发访问同一份目录表，用一把互斥量串行化**写**侧。
// 读侧无锁：见下面的双缓冲目录表。
SemaphoreHandle_t s_store_mutex = nullptr;

void ensure_store_mutex()
{
    if (!s_store_mutex) {
        s_store_mutex = xSemaphoreCreateMutex();
    }
}

// RAII 守卫：拿不到锁时 held 为 false，调用方直接放弃本次操作。
struct StoreLock {
    bool held = false;
    StoreLock()
    {
        ensure_store_mutex();
        held = s_store_mutex &&
               xSemaphoreTake(s_store_mutex, pdMS_TO_TICKS(kStoreLockTimeoutMs)) == pdTRUE;
    }
    ~StoreLock()
    {
        if (held && s_store_mutex) {
            xSemaphoreGive(s_store_mutex);
        }
    }
    StoreLock(const StoreLock &) = delete;
    StoreLock &operator=(const StoreLock &) = delete;
};

// ---------------------------------------------------------------------------
// 目录表双缓冲（P1-14）
//
// 旧实现让 s_names/s_sizes/s_count 同时被写侧（refresh_locked，持锁）和读侧
// （rom_store_count/name/size/selected，不持锁）访问：读侧要么拿到半扫描的表
// （count 已经加、名字还没填），要么在 sort_names() 的头插排序过程中读到
// 被 memcpy 移位的半截文件名。表现出来就是列表偶发闪烁、某一行显示成另一个
// 游戏的名字，而且因为指针没有越界，崩溃日志里什么都看不到。
//
// 现在写侧只往**未发布**的那一份里填，填完（含排序）用一次 release store
// 发布；读侧一次 acquire load 拿到一份自洽的不可变快照。
// 两份都在 PSRAM，总开销 2 x (4 + 256 + 4096) ≈ 8.7KB。
//
// 指针有效期：某一份被发布之后，要等**下一次** refresh 才会被复用，
// 所以调用方拿到 const char* 后可以放心用到下一次 refresh 之前。
// 现有调用方都是立即拷走（format_row_text / snprintf），余量充足。
// ---------------------------------------------------------------------------
struct RomCatalog {
    int count;
    size_t sizes[game_config::kMaxRoms];
    char names[game_config::kMaxRoms][game_config::kMaxRomFileNameBytes];
};

EXT_RAM_BSS_ATTR RomCatalog s_catalog_buffers[2];
std::atomic<const RomCatalog *> s_published_catalog{&s_catalog_buffers[0]};

// 读侧入口：无锁，一次 acquire load。
const RomCatalog *catalog()
{
    return s_published_catalog.load(std::memory_order_acquire);
}

void publish_catalog(RomCatalog *target)
{
    s_published_catalog.store(target, std::memory_order_release);
}

uint8_t *s_rom_buffer = nullptr;
size_t s_rom_buffer_bytes = 0;

// 诊断串同样是读侧无锁、写侧多任务：双缓冲 + 一次 release store 发布，
// 保证 UI 永远读不到半个 UTF-8 汉字。
constexpr size_t kDiagnosticBytes = 128;
char s_diagnostic_buffers[2][kDiagnosticBytes] = {"TF 卡未挂载", "TF 卡未挂载"};
std::atomic<uint32_t> s_diagnostic_index{0};
portMUX_TYPE s_diagnostic_mux = portMUX_INITIALIZER_UNLOCKED;

void set_diagnostic(const char *text, esp_err_t err)
{
    // 先在栈上格式化好，再进临界区只做 memcpy + 换指针：
    // snprintf 关中断跑十几微秒会把 WiFi/BLE 的中断一起推迟，没必要。
    char line[kDiagnosticBytes] = {};
    const char *const safe = text ? text : "";
    if (err == ESP_OK) {
        snprintf(line, sizeof(line), "%s", safe);
    } else {
        snprintf(line, sizeof(line), "%s (%s)", safe, esp_err_to_name(err));
    }
    portENTER_CRITICAL(&s_diagnostic_mux);
    const uint32_t next = s_diagnostic_index.load(std::memory_order_relaxed) ^ 1U;
    memcpy(s_diagnostic_buffers[next], line, kDiagnosticBytes);
    s_diagnostic_index.store(next, std::memory_order_release);
    portEXIT_CRITICAL(&s_diagnostic_mux);
}

bool name_has_rom_suffix(const char *name)
{
    if (!name) {
        return false;
    }
    const size_t length = strlen(name);
    if (length < 4) {
        return false;
    }
    const char *suffix = name + length - 3;
    char lowered[4] = {0};
    for (int i = 0; i < 3; ++i) {
        const char c = suffix[i];
        lowered[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    // 允许 .gb 与 .gbc；".gbc" 的 3 字节后缀是 "gbc"。
    if (strcmp(lowered, "gbc") == 0) {
        return true;
    }
    const char *two = name + length - 2;
    const char c0 = two[0];
    const char c1 = two[1];
    return (c0 == 'g' && c1 == 'b') || (c0 == 'G' && c1 == 'B');
}

// 文件名合法性检查。**策略是黑名单，不是白名单**——这个区别是踩过坑换来的。
//
// 白名单漏一个字符，就是静默丢掉一个 ROM：用户看到游戏页显示「请插入 TF 卡」，
// 而串口日志里 self-check 明明打着 rom count=8（log_rom_inventory() 自己 opendir
// 计数，不走这个函数），两边对不上且没有任何一方报错。现场极难排查。
// 真实案例：用户那批 ROM 叫「魂斗罗+(简)+(unknown)(2Mb).gb」「坦克大战+(简)+
// (madcell)(0.25Mb).gb」……'+'、'('、')' 全不在旧白名单（A-Za-z0-9._-）里，
// 于是 8 个 ROM 全部被过滤，界面一个都不显示。
//
// 所以这里反过来：只拒绝**确实会造成危害**的字节，其余一律放行。
// 拒绝清单：
//   1. '/' 与 '\\' —— 路径分隔符。拼进 build_path() 会穿越出 rom/gb 目录，
//      也能构造相对路径，必须拒。
//   2. ':' '*' '?' '"' '<' '>' '|' —— FAT/Windows 非法字符，同时也是
//      通配符与 shell 危险字符。放进路径会让 fopen/stat 语义偏离预期。
//   3. 控制字符（< 0x20，含 TAB/CR/LF）与 0x7F (DEL) —— 不可见字节，
//      会让日志和 UI 显示错位，且没有任何合法用途。
//   4. 长度 >= kMaxRomFileNameBytes —— 缓冲区边界。
//   5. 含 ".." —— 路径穿越兜底（'/' 已被拒，这里是双保险）。
//
// 放行清单里几个要特别说明的：
//   - 空格：'My Game.gb' 这类名字很常见，空格对路径无危害，放行。
//   - '+' '(' ')' '[' ']' '&' '%' '#' '~' 等符号：同上，无害。
//   - 0x80-0xFF 高位字节：UTF-8 多字节序列（首字节 0xC2-0xF4、续字节 0x80-0xBF）
//     与 GBK 双字节都落在这里。本函数只做字节检查、不做编码解析，
//     整段放行可同时覆盖 LFN(UTF-8) 与 8.3 短名(OEM/CP936) 两种来源。
//   - '.'：'.gb'/'.gbc' 后缀要靠它；隐藏文件（前导 '.'）在 refresh_locked()
//     里已经单独跳过，不依赖这里。
//   - '%'：放行它有个已文档化的副作用——配网页的 name 参数来自 URL query，
//     而 httpd_query_key_value() 不做百分号解码，于是 %XX 形态的中文名
//     能穿过本函数，最终写出一个 literal "%E4%B8%AD.gb" 这样的垃圾文件。
//     该风险仅限 curl 之类手工构造的请求（portal 上传有前端正则拦截），
//     2026-10-21 与用户确认暂不修，详见 network/rom_upload.cpp 文件头
//     「已知限制」。修的时候记得把 '%' 挪回必拒项，防 %2500 双重编码穿透。
bool name_chars_allowed(const char *name)
{
    if (!name) {
        return false;
    }
    const size_t length = strlen(name);
    if (length == 0 || length >= game_config::kMaxRomFileNameBytes) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)name[i];
        // 控制字符与 DEL：不可见，日志/UI 会错位。
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        // FAT 非法字符 + 路径分隔符 + 通配符。这 9 个是「确定有害」的，
        // 除此之外的字节一律放行——宁可放过一个奇怪的名字，
        // 也不要因为漏了一个常见字符而静默丢掉用户的 ROM。
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    if (strstr(name, "..") != nullptr) {
        return false;
    }
    return true;
}

// 拼 "<rom 目录>/<name><suffix>"。kRomsMountPoint 不带结尾 '/'，分隔符在这里补。
void build_path(const char *name, const char *suffix, char *out, size_t out_bytes)
{
    if (!out || out_bytes == 0) {
        return;
    }
    const char *sep = "/";
    const char *parts[4] = {game_config::kRomsMountPoint,
                            sep,
                            name ? name : "",
                            suffix ? suffix : ""};
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < 4; ++i) {
        const size_t need = strlen(parts[i]);
        if (used + need + 1 > out_bytes) {
            out[0] = '\0';
            return;
        }
        memcpy(out + used, parts[i], need);
        used += need;
    }
    out[used] = '\0';
}

// 拷贝一段已知合法（长度 < kMaxRomFileNameBytes）的文件名，不触发截断告警。
void copy_rom_name(char *out, const char *name)
{
    const size_t bytes = strlen(name);
    memcpy(out, name, bytes);
    out[bytes] = '\0';
}

// ⚠ 前置条件：调用本函数的时刻，模拟器必须已经停下（gb 的 rom 指针不再被解引用）。
//
// 这个前提不是本文件能保证的，它由 gameboy_runtime.cpp 的 stop 顺序提供：
//     save_cart_ram_if_dirty() -> game_emu_stop() -> rom_store_unload() -> s_running.store(false)
// 即「释放缓冲」严格发生在「模拟器已停」之后、「running 变 false」之前。
// 配合 game_emu_start() 内部 `if (s_running) return` 的早退，任何并发的重新载入
// 都会在看到 running==false 之前被挡住。
//
// 谁要重排 gameboy_runtime.cpp 里那四行的次序，先回来读这段：
// 一旦 free 挪到 game_emu_stop() 之前，或者挪到 s_running.store(false) 之后，
// 游戏任务持有的 gb->rom 就会变成悬空指针，而且不会有任何编译或运行期告警。
void clear_rom_buffer()
{
    if (s_rom_buffer) {
        heap_caps_free(s_rom_buffer);
        s_rom_buffer = nullptr;
    }
    s_rom_buffer_bytes = 0;
}

// 只在写侧的私有缓冲上跑（发布之前），读侧看不到排序过程中的中间态。
void sort_names(RomCatalog *c)
{
    if (!c) {
        return;
    }
    for (int i = 1; i < c->count; ++i) {
        char key_name[game_config::kMaxRomFileNameBytes];
        memcpy(key_name, c->names[i], sizeof(key_name));
        const size_t key_size = c->sizes[i];
        int j = i - 1;
        while (j >= 0 && strcmp(c->names[j], key_name) > 0) {
            memcpy(c->names[j + 1], c->names[j], sizeof(c->names[j]));
            c->sizes[j + 1] = c->sizes[j];
            --j;
        }
        memcpy(c->names[j + 1], key_name, sizeof(key_name));
        c->sizes[j + 1] = key_size;
    }
}

// 选择项存「文件名」而不是「下标」：s_names 会被 sort_names() 按文件名重排，
// 上传或删除任意一个 ROM 都会让下标指向别的游戏——用户看到的症状是
// 「上次玩的是宝可梦，重开却进了俄罗斯方块」，而且 .sav 也写到了别的游戏头上。
// 文件名是自描述的：ROM 被删了只是解析失败（保持当前选择），不会串到别人身上。
bool read_selected_name_from_nvs(char *out, size_t out_bytes)
{
    if (!out || out_bytes == 0) {
        return false;
    }
    out[0] = '\0';
    nvs_handle_t handle = 0;
    if (nvs_open(game_config::kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t length = out_bytes;
    const esp_err_t err = nvs_get_str(handle,
                                      game_config::kNvsSelectedRomNameKey,
                                      out,
                                      &length);
    nvs_close(handle);
    return err == ESP_OK && out[0] != '\0';
}

// 旧版本存的是下标，只在名字解析失败时用作一次性迁移来源。
int read_selected_index_from_nvs()
{
    nvs_handle_t handle = 0;
    if (nvs_open(game_config::kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return -1;
    }
    int32_t value = -1;
    if (nvs_get_i32(handle, game_config::kNvsSelectedRomKey, &value) != ESP_OK) {
        value = -1;
    }
    nvs_close(handle);
    return (int)value;
}

void write_selected_to_nvs(const RomCatalog *c, int index)
{
    if (!c || c->count <= 0) {
        return;
    }
    if (index < 0 || index >= c->count) {
        index = 0;
    }
    nvs_handle_t handle = 0;
    if (nvs_open(game_config::kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(kRomLogTag, "nvs open failed, selection not persisted");
        return;
    }
    (void)nvs_set_str(handle,
                      game_config::kNvsSelectedRomNameKey,
                      c->names[index]);
    // 下标键保留：老固件回滚时还能读到"大致对的那个"，新固件永远优先名字。
    (void)nvs_set_i32(handle, game_config::kNvsSelectedRomKey, (int32_t)index);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

// 解析出当前应选中的下标。out_needs_write 为真时表示必须把结果回写到 NVS
// （名字键缺失或 ROM 被删的迁移场景），避免每次刷新都写一遍 flash。
int resolve_selected_from_nvs(const RomCatalog *c, bool *out_needs_write)
{
    if (out_needs_write) {
        *out_needs_write = false;
    }
    if (!c || c->count <= 0) {
        return 0;
    }
    char name[game_config::kMaxRomFileNameBytes] = {};
    if (read_selected_name_from_nvs(name, sizeof(name))) {
        for (int i = 0; i < c->count; ++i) {
            if (strcmp(c->names[i], name) == 0) {
                return i;
            }
        }
        // 上次选中的 ROM 已被删除/改名：保持当前选择，别随机跳到别的游戏。
        ESP_LOGI(kRomLogTag, "selected rom no longer present: %s", name);
        if (out_needs_write) {
            *out_needs_write = true;
        }
        const int current = s_selected.load(std::memory_order_relaxed);
        return (current >= 0 && current < c->count) ? current : 0;
    }
    const int legacy = read_selected_index_from_nvs();
    if (out_needs_write) {
        *out_needs_write = true;
    }
    return (legacy >= 0 && legacy < c->count) ? legacy : 0;
}
} // namespace

// 重新扫描目录（调用方必须已持有 s_store_mutex）。
static int refresh_locked(void);

// 确保一个目录存在（不存在就建）。VFS 的 mkdir 没有 -p，必须逐级调用。
// 建不出来（只读卡、写保护、卡已拔）只记 errno 并返回 false：
// 卡上可能本来就有 rom/gb，后面 refresh 会再判断能不能读，
// 不能因为建目录失败就把整个挂载判成失败。
bool ensure_dir(const char *path)
{
    if (!path) {
        return false;
    }
    struct stat info = {};
    if (stat(path, &info) == 0) {
        return S_ISDIR(info.st_mode) != 0;
    }
    if (mkdir(path, 0755) != 0) {
        ESP_LOGW(kRomLogTag, "mkdir %s failed errno=%d", path, errno);
        return false;
    }
    // 建成功也要留痕：日后用户报"插了卡读不到 ROM"时，
    // 这条能区分"目录本来就在"和"我们刚建的、卡上其实没这个目录"。
    ESP_LOGI(kRomLogTag, "mkdir %s ok", path);
    return true;
}

// 挂载成功后把 /sdcard/rom/gb 补出来，省得用户必须自己在电脑上建目录。
void ensure_rom_dir()
{
    if (!ensure_dir(kRomsParentDir)) {
        return;
    }
    (void)ensure_dir(game_config::kRomsMountPoint);
}

bool rom_store_mount(void)
{
    if (s_mounted.load(std::memory_order_relaxed)) {
        return true;
    }
    // 卡不存在时 SDMMC 初始化要走完整的探测超时（几百毫秒），
    // 供 UI 每帧调用的场景必须限流，不能每帧都试一次。
    const int64_t now_us = esp_timer_get_time();
    if (now_us < s_next_mount_attempt_us) {
        return false;
    }
    StoreLock guard;
    if (!guard.held) {
        return false;
    }
    if (s_mounted.load(std::memory_order_relaxed)) {
        return true;
    }
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    // 绝不格式化用户 TF 卡：卡上可能有照片、别的设备的文件或用户的存档，
    // format_if_mount_failed=true 会在用户毫不知情的情况下整卡抹掉，
    // UI 只会显示"未安装 ROM"，用户根本想不到是设备自己格的。
    // 宁可挂载失败并明确提示"未检测到 TF 卡"，让人自己拿到电脑上处理。
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = kFatMaxFiles;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // 板载卡槽接在 slot 0。SDMMC_HOST_DEFAULT() 默认是 slot 1，必须显式改。
    host.slot = SDMMC_HOST_SLOT_0;
    // max_freq_khz 保持 SDMMC_HOST_DEFAULT() 的 20MHz：D1/D2 未连接只能跑 1-bit，
    // 高频在 1-bit 上没有意义，只会增加误码。

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    // D1/D2 原理图未连接 → 只能 1-bit。这一行是硬约束，改成 4 会直接挂不上卡。
    slot_config.width = 1;
    slot_config.clk = game_config::kSdClkGpio;
    slot_config.cmd = game_config::kSdCmdGpio;
    slot_config.d0 = game_config::kSdD0Gpio;
    // 未接的数据线全部置 NC，避免驱动去动 SDMMC_SLOT_CONFIG_DEFAULT()
    // 里那组默认 GPIO（S3 上是 4/12/13，其中 12/13 已被 LCD/I2C 占用）。
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
    slot_config.d4 = GPIO_NUM_NC;
    slot_config.d5 = GPIO_NUM_NC;
    slot_config.d6 = GPIO_NUM_NC;
    slot_config.d7 = GPIO_NUM_NC;
    // 卡槽 CD 脚接地、没有 WP 脚：只能靠"尝试挂载"判断卡在不在。
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(game_config::kSdCardMountPoint,
                                                  &host,
                                                  &slot_config,
                                                  &mount_config,
                                                  &s_card);
    if (err != ESP_OK) {
        s_card = nullptr;
        // 卡没插、卡接触不良、卡坏了都走这里，对用户来说就是"没认到卡"。
        set_diagnostic("未检测到 TF 卡", err);
        ESP_LOGE(kRomLogTag,
                 "sdmmc mount failed: %s, refusing to format user TF card",
                 esp_err_to_name(err));
        s_mounted.store(false, std::memory_order_relaxed);
        s_next_mount_attempt_us = now_us + kMountRetryIntervalUs;
        return false;
    }
    ensure_rom_dir();
    s_mounted.store(true, std::memory_order_relaxed);
    s_next_mount_attempt_us = 0;
    set_diagnostic("已挂载", ESP_OK);
    (void)refresh_locked();
    return true;
}

bool rom_store_mounted(void)
{
    return s_mounted.load(std::memory_order_relaxed);
}

// 写侧唯一入口。全部填充都在**未发布**的那一份上完成，最后一次性发布；
// 读侧在整个过程中看到的仍然是上一份完整的表。
static int refresh_locked(void)
{
    const RomCatalog *const current = catalog();
    RomCatalog *const target =
        (current == &s_catalog_buffers[0]) ? &s_catalog_buffers[1]
                                           : &s_catalog_buffers[0];
    target->count = 0;
    if (!s_mounted.load(std::memory_order_relaxed)) {
        set_diagnostic("TF 卡未挂载", ESP_OK);
        publish_catalog(target);
        return 0;
    }
    DIR *dir = opendir(game_config::kRomsMountPoint);
    if (!dir) {
        set_diagnostic("TF 卡 rom/gb 不可读", ESP_OK);
        publish_catalog(target);
        return 0;
    }
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (target->count >= game_config::kMaxRoms) {
            break;
        }
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!name_has_rom_suffix(entry->d_name) ||
            !name_chars_allowed(entry->d_name)) {
            continue;
        }
        char path[kPathBytes] = {0};
        build_path(entry->d_name, nullptr, path, sizeof(path));
        struct stat info = {};
        if (stat(path, &info) != 0 || info.st_size <= 0) {
            continue;
        }
        if ((size_t)info.st_size > game_config::kMaxRomBytes) {
            ESP_LOGW(kRomLogTag,
                     "skip oversized rom %s bytes=%u",
                     entry->d_name,
                     (unsigned)info.st_size);
            continue;
        }
        copy_rom_name(target->names[target->count], entry->d_name);
        target->sizes[target->count] = (size_t)info.st_size;
        ++target->count;
    }
    closedir(dir);

    sort_names(target);

    // 选择项先落到 s_selected，再发布目录表：读侧看到新表的瞬间，
    // 配合的下标也已经就位（rom_store_selected() 另外还有 clamp 兜底）。
    bool needs_write = false;
    const int selected = resolve_selected_from_nvs(target, &needs_write);
    s_selected.store(selected, std::memory_order_relaxed);
    if (needs_write && target->count > 0) {
        write_selected_to_nvs(target, selected);
    }
    set_diagnostic(target->count > 0 ? "就绪" : "TF 卡 rom/gb 无 ROM", ESP_OK);
    publish_catalog(target);
    return target->count;
}

int rom_store_refresh(void)
{
    StoreLock guard;
    if (!guard.held) {
        return catalog()->count;
    }
    return refresh_locked();
}

int rom_store_count(void)
{
    return catalog()->count;
}

const char *rom_store_name(int index)
{
    const RomCatalog *const c = catalog();
    if (index < 0 || index >= c->count) {
        return "";
    }
    return c->names[index];
}

size_t rom_store_size(int index)
{
    const RomCatalog *const c = catalog();
    if (index < 0 || index >= c->count) {
        return 0;
    }
    return c->sizes[index];
}

bool rom_store_select(int index)
{
    if (catalog()->count <= 0) {
        return false;
    }
    StoreLock guard;
    if (!guard.held) {
        return false;
    }
    // 锁内重新取一次：等锁期间 refresh 可能已经换了一份表。
    const RomCatalog *const c = catalog();
    if (c->count <= 0) {
        return false;
    }
    if (index < 0) {
        index = 0;
    }
    if (index >= c->count) {
        index = c->count - 1;
    }
    s_selected.store(index, std::memory_order_relaxed);
    write_selected_to_nvs(c, index);
    return true;
}

bool rom_store_select_by_name(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    StoreLock guard;
    if (!guard.held) {
        return false;
    }
    const RomCatalog *const c = catalog();
    for (int i = 0; i < c->count; ++i) {
        if (strcmp(c->names[i], name) == 0) {
            s_selected.store(i, std::memory_order_relaxed);
            write_selected_to_nvs(c, i);
            return true;
        }
    }
    return false;
}

int rom_store_selected(void)
{
    const RomCatalog *const c = catalog();
    if (c->count <= 0) {
        return 0;
    }
    const int selected = s_selected.load(std::memory_order_relaxed);
    if (selected < 0) {
        return 0;
    }
    if (selected >= c->count) {
        return c->count - 1;
    }
    return selected;
}

const uint8_t *rom_store_load(int index, size_t *out_size)
{
    if (out_size) {
        *out_size = 0;
    }
    if (index < 0 || index >= catalog()->count) {
        set_diagnostic("ROM 序号越界", ESP_OK);
        return nullptr;
    }
    StoreLock guard;
    if (!guard.held) {
        set_diagnostic("ROM 存储忙，请重试", ESP_OK);
        return nullptr;
    }
    clear_rom_buffer();

    // 锁内重新取表：等锁期间 refresh 可能已发布新的一份，
    // 这时下标对应的是**另一个**文件，必须按新表重算。
    const RomCatalog *const c = catalog();
    if (index < 0 || index >= c->count) {
        set_diagnostic("ROM 序号越界", ESP_OK);
        return nullptr;
    }
    const size_t bytes = c->sizes[index];
    if (bytes == 0 || bytes > game_config::kMaxRomBytes) {
        set_diagnostic("ROM 大小无效", ESP_OK);
        return nullptr;
    }
    char path[kPathBytes] = {0};
    build_path(c->names[index], nullptr, path, sizeof(path));
    FILE *file = fopen(path, "rb");
    if (!file) {
        set_diagnostic("ROM 打开失败", ESP_OK);
        return nullptr;
    }
    uint8_t *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) {
        fclose(file);
        set_diagnostic("PSRAM 不足，无法载入 ROM", ESP_OK);
        ESP_LOGE(kRomLogTag, "rom buffer allocation failed bytes=%u", (unsigned)bytes);
        return nullptr;
    }
    const size_t read_bytes = fread(buffer, 1, bytes, file);
    fclose(file);
    if (read_bytes != bytes) {
        heap_caps_free(buffer);
        set_diagnostic("ROM 读取不完整", ESP_OK);
        ESP_LOGE(kRomLogTag,
                 "rom read incomplete got=%u expected=%u",
                 (unsigned)read_bytes,
                 (unsigned)bytes);
        return nullptr;
    }
    s_rom_buffer = buffer;
    s_rom_buffer_bytes = bytes;
    if (out_size) {
        *out_size = bytes;
    }
    set_diagnostic("ROM 已载入", ESP_OK);
    return s_rom_buffer;
}

void rom_store_unload(void)
{
    StoreLock guard;
    if (!guard.held) {
        return;
    }
    clear_rom_buffer();
}

bool rom_store_save_cart_ram(int index, const uint8_t *data, size_t size)
{
    if (index < 0 || index >= catalog()->count || !data || size == 0) {
        return false;
    }
    StoreLock guard;
    if (!guard.held) {
        return false;
    }
    const RomCatalog *const c = catalog();
    if (index < 0 || index >= c->count) {
        return false;
    }
    char path[kPathBytes] = {0};
    build_path(c->names[index], game_config::kSaveFileSuffix, path, sizeof(path));
    FILE *file = fopen(path, "wb");
    if (!file) {
        set_diagnostic("存档写入失败", ESP_OK);
        ESP_LOGW(kRomLogTag, "save open failed path=%s errno=%d", path, errno);
        return false;
    }
    const size_t written = fwrite(data, 1, size, file);
    const int flush_error = fflush(file);
    fclose(file);
    if (written != size || flush_error != 0) {
        set_diagnostic("存档写入不完整", ESP_OK);
        return false;
    }
    return true;
}

size_t rom_store_load_cart_ram(int index, uint8_t *data, size_t capacity)
{
    if (index < 0 || index >= catalog()->count || !data || capacity == 0) {
        return 0;
    }
    StoreLock guard;
    if (!guard.held) {
        return 0;
    }
    const RomCatalog *const c = catalog();
    if (index < 0 || index >= c->count) {
        return 0;
    }
    char path[kPathBytes] = {0};
    build_path(c->names[index], game_config::kSaveFileSuffix, path, sizeof(path));
    struct stat info = {};
    if (stat(path, &info) != 0 || info.st_size <= 0) {
        return 0;
    }
    const size_t bytes =
        ((size_t)info.st_size < capacity) ? (size_t)info.st_size : capacity;
    FILE *file = fopen(path, "rb");
    if (!file) {
        return 0;
    }
    const size_t read_bytes = fread(data, 1, bytes, file);
    fclose(file);
    return read_bytes;
}

bool rom_store_name_is_valid(const char *name)
{
    return name_chars_allowed(name) && name_has_rom_suffix(name);
}

const char *rom_store_diagnostic(void)
{
    const uint32_t index = s_diagnostic_index.load(std::memory_order_acquire) & 1U;
    return s_diagnostic_buffers[index];
}

// ---------------------------------------------------------------------------
// 启动自检（一次性，非周期）
//
// 存在的理由：rom_store_mount() 的调用点是"进入游戏页"和"启动游戏"，
// 也就是必须有人按 BOOT 键翻到游戏页才会碰 TF 卡。而启动阶段打出来的日志
// 又早于串口打开，抓不到。结果是：硬件上卡没插、卡槽虚焊、引脚定义错，
// 三种故障在日志里长得一模一样——都是"什么都没有"。
//
// 所以固件在启动收尾后自己延迟几秒挂一次卡，把卡的身份信息（容量 / CSD /
// OCR / 厂商名）和 rom/gb 目录的实况全打出来。有了这段，串口一抓就能分辨
// 是"没认到卡"（挂不上，看 esp_err_to_name）还是"认到了但目录不对"。
// 这段对长期诊断"用户插了卡为什么读不到"同样有用，不要当成临时探针删掉。
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t kSelfCheckDelayMs = 8000;
constexpr uint32_t kSelfCheckStackBytes = 6144;
constexpr UBaseType_t kSelfCheckPriority = 1;
// 挂在网络核（core 0）：SDMMC 初始化期间会有几百毫秒的探测阻塞，
// 别去和 core 1 上的 ui_task / 模拟器抢核。
constexpr BaseType_t kSelfCheckCoreId = 0;
constexpr int kSelfCheckMaxListedRoms = 8;
// 自检里打的文件名 hex 字节数（留一点余量，别刷屏）。
constexpr size_t kSelfCheckHexBytes = 24;

// 卡的身份信息。cid.name 是 char[8] 且不保证以 '\0' 结尾，用 %.8s 限长打印。
void log_card_identity()
{
    if (!s_card) {
        ESP_LOGW(kRomLogTag, "self-check: card handle is null");
        return;
    }
    const sdmmc_card_t &card = *s_card;
    const uint64_t total_bytes =
        (uint64_t)card.csd.capacity * (uint64_t)card.csd.sector_size;
    ESP_LOGI(kRomLogTag,
             "self-check: card name=%.8s mfg=0x%02x oem=0x%04x rev=%d serial=0x%08x date=%d",
             card.cid.name,
             (unsigned)card.cid.mfg_id,
             (unsigned)card.cid.oem_id,
             card.cid.revision,
             (unsigned)card.cid.serial,
             card.cid.date);
    ESP_LOGI(kRomLogTag,
             "self-check: csd ver=%d sectors=%d sector_size=%d total=%lluB (%llu MiB) read_bl=%d ccc=0x%03x tr_speed=%d",
             card.csd.csd_ver,
             card.csd.capacity,
             card.csd.sector_size,
             (unsigned long long)total_bytes,
             (unsigned long long)(total_bytes / (1024ULL * 1024ULL)),
             card.csd.read_block_len,
             (unsigned)card.csd.card_command_class,
             card.csd.tr_speed);
    ESP_LOGI(kRomLogTag,
             "self-check: ocr=0x%08x rca=0x%04x mem=%u sdio=%u mmc=%u width=%ubit ddr=%u max_freq=%ukHz real_freq=%dkHz scr_bus_width=0x%x",
             (unsigned)card.ocr,
             (unsigned)card.rca,
             (unsigned)card.is_mem,
             (unsigned)card.is_sdio,
             (unsigned)card.is_mmc,
             (unsigned)(1U << card.log_bus_width),
             (unsigned)card.is_ddr,
             (unsigned)card.max_freq_khz,
             card.real_freq_khz,
             (unsigned)card.scr.bus_width);
}

// rom/gb 目录是否存在 + 里面有几个 ROM + 前几个文件名。
void log_rom_inventory()
{
    struct stat info = {};
    const bool exists =
        stat(game_config::kRomsMountPoint, &info) == 0 && S_ISDIR(info.st_mode);
    ESP_LOGI(kRomLogTag,
             "self-check: rom dir %s %s",
             game_config::kRomsMountPoint,
             exists ? "exists" : "missing");
    if (!exists) {
        return;
    }
    DIR *dir = opendir(game_config::kRomsMountPoint);
    if (!dir) {
        ESP_LOGW(kRomLogTag, "self-check: opendir failed errno=%d", errno);
        return;
    }
    int rom_total = 0;
    int listed = 0;
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (!name_has_rom_suffix(entry->d_name)) {
            continue;
        }
        ++rom_total;
        if (listed >= kSelfCheckMaxListedRoms) {
            continue;
        }
        char path[kPathBytes] = {0};
        build_path(entry->d_name, nullptr, path, sizeof(path));
        struct stat file_info = {};
        const long bytes =
            (stat(path, &file_info) == 0) ? (long)file_info.st_size : -1L;
        ESP_LOGI(kRomLogTag, "self-check: rom[%d] %s bytes=%ld", listed,
                 entry->d_name, bytes);
        // FATFS API 编码配成 UTF-8（CONFIG_FATFS_API_ENCODING_UTF_8）之后，
        // readdir() 交出来的就是 UTF-8 字节，上面那行的 %s 就是最终显示文本，
        // 中间不再有"日志看着对、UI 显示不对"的转码层。
        //
        // 那为什么还打 hex：终端能不能渲染 UTF-8、抓串口的工具会不会动字节，
        // 取决于串口工具的设置而不是固件，而 hex 是字节的客观记录。真出现中文
        // 显示异常时，hex 能直接回答"API 层到底给的是 UTF-8 还是 GBK"——
        // '0' 是 0x30，UTF-8 三字节汉字以 0xE4-0xE9 起头，
        // GBK 双字节以 0xB0-0xF7 起头，一眼可分。
        // 所以这不是冗余输出，是把"编码对不对"的争论变成可核对的事实。
        char hex[3 * kSelfCheckHexBytes + 1] = {};
        for (size_t i = 0; i < kSelfCheckHexBytes; ++i) {
            const unsigned char byte = (unsigned char)entry->d_name[i];
            snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", (unsigned)byte);
            if (byte == '\0') {
                break;
            }
        }
        ESP_LOGI(kRomLogTag, "self-check: rom[%d] hex=%s", listed, hex);
        ++listed;
    }
    closedir(dir);
    ESP_LOGI(kRomLogTag,
             "self-check: rom count=%d (listed %d, table cap %d)",
             rom_total,
             listed,
             game_config::kMaxRoms);
}

void selfcheck_task(void *)
{
    // 延迟启动：先让 boot 动画、配网、常规任务都起来，
    // 别让 SDMMC 探测的几百毫秒挡在启动路径上。
    vTaskDelay(pdMS_TO_TICKS(kSelfCheckDelayMs));
    ESP_LOGI(kRomLogTag, "self-check: start (sdmmc slot0 1bit clk=%d cmd=%d d0=%d)",
             (int)game_config::kSdClkGpio,
             (int)game_config::kSdCmdGpio,
             (int)game_config::kSdD0Gpio);
    if (!rom_store_mount()) {
        // 挂载失败的原因 rom_store_mount() 已经打过了（含 esp_err_to_name 与
        // "refusing to format user TF card"）。这里补两条：
        // 一是被 5s 限流挡住时那边不会重打，二是把上屏的诊断串也落进日志。
        ESP_LOGW(kRomLogTag,
                 "self-check: mount failed or throttled; diagnostic=%s",
                 rom_store_diagnostic());
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(kRomLogTag, "self-check: mounted at %s", game_config::kSdCardMountPoint);
    log_card_identity();
    log_rom_inventory();
    ESP_LOGI(kRomLogTag, "self-check: done");
    vTaskDelete(nullptr);
}
} // namespace

void rom_store_startup_selfcheck(void)
{
    // 只创建一次：自检本身就是一次性的，重入只会让日志出现两份互相矛盾的结论。
    static bool scheduled = false;
    if (scheduled) {
        return;
    }
    if (xTaskCreatePinnedToCore(selfcheck_task,
                                "rom_selfcheck",
                                kSelfCheckStackBytes,
                                nullptr,
                                kSelfCheckPriority,
                                nullptr,
                                kSelfCheckCoreId) != pdPASS) {
        ESP_LOGW(kRomLogTag, "self-check task create failed");
        return;
    }
    scheduled = true;
}
