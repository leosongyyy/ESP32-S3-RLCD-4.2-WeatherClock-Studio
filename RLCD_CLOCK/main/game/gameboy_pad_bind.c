// gameboy_pad_bind.c —— 蓝牙手柄按键绑定表：用户亲手指认 + 纯位运算判定。
//
// 为什么有这一层：gameboy_pad_ble.c 里的 pad_apply_*() 靠"报文看起来像什么"
// 猜布局，猜错一次整场会话按键全废（现场踩过：Q36 for Android 的 10 字节报文
// 首字节是左摇杆中位 0x80，被判成"带 report ID"，偏移错 1 字节，A/B/方向键
// 恒为 0）。绑定表换一条路：用户在屏幕上指着某个动作、按下手柄上的物理键，
// 这里把"哪一字节的哪些位变成了什么"直接记下来，之后判定只是
//   (data[byte_index] & mask) == expect
// 不再依赖任何布局假设。
//
// 线程模型（改之前先读）：
//  - pad_bind_pressed() 由 BLE host 任务每包调用 9 次，所以是纯位运算、不加锁；
//  - 绑定表只在用户操作（捕获落盘 / UI 清除）时整体覆盖写入，写入粒度是
//    一个结构体，读侧最坏读到"旧的一条"或"新的一条"，不会读到半条而误触发；
//  - 捕获状态机由 BLE host 任务（pad_bind_on_raw_report）推进，UI 只读
//    pad_bind_capture_stage()，两边不做握手，靠状态字本身表达进度。

#include "gameboy_pad_bind.h"

#include <string.h>

// ⚠ 本文件要读 hid_field_t 的字段（kind / bit_offset ...），所以必须拿完整类型。
//   gameboy_pad_bind.h 里只有前置声明（不完整类型），这里补上真正定义。
//   gameboy_hid.h 不依赖任何其它头文件，include 它不会形成环。
#include "gameboy_hid.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

#define TAG "pad_bind"

// ---------------------------------------------------------------- NVS 布局
//
// ⚠ 不要把 pad_bind_entry_t 直接 nvs_set_blob()：结构体里有 padding
// （uint8_t + uint16_t + 3×uint8_t），不同编译器/不同 IDF 版本的 sizeof 与
// 填充位置可能不同，跨版本升级会读出垃圾 —— 而且读出来的 valid 很可能还是
// 1，凭空多出一堆"永远按下"的绑定。所以固定成扁平的 55 字节：
//
//   blob[0]        = 版本号（当前 1）
//   blob[1+6i ..]  = 第 i 条（i 按 pad_bind_action_t 的枚举顺序）
//      +0      valid（0/1）
//      +1      byte_index
//      +2      mask
//      +3      expect
//      +4..+5  report_len（小端）
#define PAD_BIND_NVS_NAMESPACE "padbind"
#define PAD_BIND_NVS_KEY       "map"
// v1 会接受「摇杆自抖动」造成的绑定；v2 加了最小按住时长要求。
// v3：动作集合变了（去掉 HOME、加入 X / Y），NVS blob 的条目顺序随之改变，
//     旧记录语义已错位，必须一次性作废让用户重绑 —— 否则会出现"按 A 实际
//     触发了别的键"这种最难查的串位。
#define PAD_BIND_BLOB_VERSION  3
#define PAD_BIND_ENTRY_BYTES   6
#define PAD_BIND_BLOB_BYTES    61   // 1 + 10 × 6

_Static_assert(PAD_BIND_BLOB_BYTES ==
                   1 + (int)PAD_BIND_COUNT * PAD_BIND_ENTRY_BYTES,
               "pad bind blob layout drifted from pad_bind_action_t order");

// 捕获时要保存基线/候选报文。BLE 层每次 notify 最多拷 32 字节进来
// （见 gameboy_pad_ble.c 的 pad_handle_report 调用点），这里取同一个上限。
#define PAD_BIND_RAW_MAX 32

// ---------------------------------------------------------------- 状态

static bool             s_inited;
static pad_bind_entry_t s_table[PAD_BIND_COUNT];

// 捕获状态机
static pad_bind_capture_stage_t s_stage = PAD_BIND_CAP_IDLE;
static pad_bind_action_t        s_cap_action = PAD_BIND_A;
static int64_t                  s_cap_started_us;
static pad_bind_entry_t         s_pending;      // 抓到但还没落盘的那一条

// 「所有键都松开」的基线。硬件的当前状态无法自己判断，只能靠"连续两包完全
// 一致"来近似（见 capture_wait_step 的注释）。
static uint8_t  s_base[PAD_BIND_RAW_MAX];
static uint16_t s_base_len;
static bool     s_have_base;
// 基线要连续几包完全一致才认。原来是 2 包，改成 3：模拟摇杆 ADC 在静止时
// 也会在 ±1 LSB 之间跳，两包刚好同值不算稀奇，三包能把概率再压一档。
#define PAD_BIND_BASELINE_CONFIRM    3

// 变化至少要持续几包才当成一次按键。
//
// 这是整个绑定模块的**最重要的一条防御**，缺了它就会出现「没按按键却在不停
// 触发」。成因是：很多手柄的摇杆字节（典型是 report 开头几个轴）在完全没碰
// 的情况下也会自抖动，比如 0x80 ↔ 0x81 来回跳。捕获只要求「出现一个变化的
// 字节」，于是抖动一跳就被当成"用户按下了某个键"，记出来的这条绑定就成了
// 一个自己会反复触发的幽灵键，用户看起来是按键乱跳、模拟器没法玩。
//
// 真实的按键不可能只持续一两包：人手最快的点按也是几十毫秒，BLE HID 按
// 100~125Hz 上报就是十几包。反过来，抖动是逐包翻转的，撑不过三包。
#define PAD_BIND_MIN_HOLD_REPORTS    3

static uint8_t  s_cand[PAD_BIND_RAW_MAX];   // 基线候选（还没被第二包确认）
static uint16_t s_cand_len;
static uint8_t  s_cand_count;
static uint8_t  s_release_count;
// 候选记录锁定那一刻，目标字节的取值 + 该值已经连续保持了几包。
// 用来把「抖动过的那个瞬间」和「用户真的按住不放」区分开。
static uint8_t  s_hold_byte;
static uint8_t  s_hold_count;
// 「本包没有按钮/Hat 字节变化」这条警告的上次打印时刻（限流用，见使用处）。
// 用户按住摇杆不放时每一包都会命中那条分支，不限流会刷爆日志。
static int64_t  s_nobtn_log_us;
// 多次取样：第一次得到的样本，以及已经确认了几次完全相同的「按下-松开」。
static pad_bind_entry_t s_ref;
static uint8_t          s_cycles;

// UI 用的实时按下状态，由 pad_bind_update_live() 每包整体重算。
static bool s_live[PAD_BIND_COUNT];

// UI 用的实时按下状态**第二来源**：由 pad_bind_update_live_desc() 从 HID Report
// Map 描述符推导出来，与绑定表无关。
//
// 为什么分成两个数组而不是合并：两者的生命周期不同。s_live 每收到一包报文就
// 整体重算（松开自动体现）；s_live_desc 只在描述符路径真的解析出结果时才更新，
// 描述符不可用时必须能单独清零。合并成一个的话，"描述符不可用"会被误当成
// "用户松手了"，而"用户松手"又没法把描述符推导的那份清掉。
// pad_bind_action_live() 取二者的或。
static bool s_live_desc[PAD_BIND_COUNT];

// ---------------------------------------------------------------- 表校验

// 一条绑定能不能用。写盘前和读盘后各查一次：
//  - mask == 0 会让 (byte & 0) == 0 恒真 —— 表现是"这个键永久按下"，
//    玩家会看到角色自己一直跑，比不绑定糟糕得多；
//  - expect 必须是 mask 的子集，否则这个条件永远不成立（等于静默失效）；
//  - byte_index 必须落在绑定时那帧报文的长度之内。
static bool entry_usable(const pad_bind_entry_t *e)
{
    if (!e || !e->valid) {
        return false;
    }
    if (e->mask == 0) {
        return false;
    }
    if ((uint8_t)(e->expect & (uint8_t)(~e->mask)) != 0) {
        return false;
    }
    if (e->report_len == 0 || e->byte_index >= e->report_len) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- NVS 读写

static void table_load(void)
{
    memset(s_table, 0, sizeof(s_table));

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(PAD_BIND_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // 命名空间还没建（从没绑定过）或 NVS 本身有问题：都当"未绑定"，
        // 绝不因为读不到就把整个游戏页卡住。
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s, bindings treated as empty",
                 PAD_BIND_NVS_NAMESPACE, esp_err_to_name(err));
        return;
    }

    uint8_t blob[PAD_BIND_BLOB_BYTES];
    size_t blob_len = sizeof(blob);
    err = nvs_get_blob(handle, PAD_BIND_NVS_KEY, blob, &blob_len);
    nvs_close(handle);

    if (err != ESP_OK) {
        // ESP_ERR_NVS_NOT_FOUND 是正常的首次启动路径，用 info 级别即可。
        ESP_LOGI(TAG, "no saved bindings (%s)", esp_err_to_name(err));
        return;
    }
    if (blob_len != PAD_BIND_BLOB_BYTES) {
        ESP_LOGW(TAG, "blob size %u != %d, bindings treated as empty",
                 (unsigned)blob_len, PAD_BIND_BLOB_BYTES);
        return;
    }
    if (blob[0] != (uint8_t)PAD_BIND_BLOB_VERSION) {
        ESP_LOGW(TAG, "blob version %u != %d, bindings treated as empty",
                 (unsigned)blob[0], PAD_BIND_BLOB_VERSION);
        return;
    }

    int usable = 0;
    for (int i = 0; i < (int)PAD_BIND_COUNT; ++i) {
        const uint8_t *src = &blob[1 + i * PAD_BIND_ENTRY_BYTES];
        pad_bind_entry_t e = {
            .valid = (uint8_t)(src[0] != 0 ? 1 : 0),
            .report_len = (uint16_t)((uint16_t)src[4] | ((uint16_t)src[5] << 8)),
            .byte_index = src[1],
            .mask = src[2],
            .expect = src[3],
        };
        if (!e.valid) {
            continue;
        }
        if (!entry_usable(&e)) {
            ESP_LOGW(TAG, "%s: saved entry rejected (byte=%u mask=0x%02x "
                          "expect=0x%02x len=%u)",
                     pad_bind_action_name((pad_bind_action_t)i),
                     (unsigned)e.byte_index, (unsigned)e.mask,
                     (unsigned)e.expect, (unsigned)e.report_len);
            continue;
        }
        s_table[i] = e;
        ++usable;
    }
    ESP_LOGI(TAG, "loaded %d binding(s) from nvs", usable);
}

static bool table_save(void)
{
    uint8_t blob[PAD_BIND_BLOB_BYTES];
    blob[0] = (uint8_t)PAD_BIND_BLOB_VERSION;
    for (int i = 0; i < (int)PAD_BIND_COUNT; ++i) {
        const pad_bind_entry_t *e = &s_table[i];
        uint8_t *dst = &blob[1 + i * PAD_BIND_ENTRY_BYTES];
        dst[0] = (uint8_t)(e->valid ? 1 : 0);
        dst[1] = e->byte_index;
        dst[2] = e->mask;
        dst[3] = e->expect;
        dst[4] = (uint8_t)(e->report_len & 0xFF);
        dst[5] = (uint8_t)((e->report_len >> 8) & 0xFF);
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(PAD_BIND_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s, rw) failed: %s, changes kept in RAM only",
                 PAD_BIND_NVS_NAMESPACE, esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, PAD_BIND_NVS_KEY, blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs write failed: %s, changes kept in RAM only",
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- 对外：表访问

void pad_bind_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;    // 先置位：table_load() 里有日志，不需要防重入递归

    // nvs_flash_init() 由系统别处已完成（网络配置/设置持久化都依赖它），
    // 这里绝不能再调一次 —— 重复 init 在部分 IDF 版本会返回 ESP_ERR_NVS_-
    // NO_FREE_PAGES 之类的噪音，甚至重算分区表。
    table_load();
    ESP_LOGI(TAG, "bind table ready, any=%d all=%d",
             (int)pad_bind_any_set(), (int)pad_bind_all_set());
}

pad_bind_entry_t pad_bind_get(pad_bind_action_t action)
{
    pad_bind_entry_t zero = { 0 };
    if ((int)action < 0 || (int)action >= (int)PAD_BIND_COUNT) {
        return zero;
    }
    return s_table[(int)action];
}

void pad_bind_set(pad_bind_action_t action, const pad_bind_entry_t *entry)
{
    if ((int)action < 0 || (int)action >= (int)PAD_BIND_COUNT) {
        ESP_LOGW(TAG, "set with invalid action %d", (int)action);
        return;
    }
    if (entry == NULL || !entry->valid) {
        pad_bind_clear(action);
        return;
    }
    if (!entry_usable(entry)) {
        ESP_LOGW(TAG, "set rejected for %s: byte=%u mask=0x%02x expect=0x%02x "
                      "len=%u",
                 pad_bind_action_name(action), (unsigned)entry->byte_index,
                 (unsigned)entry->mask, (unsigned)entry->expect,
                 (unsigned)entry->report_len);
        return;
    }

    // 表是整表写盘的，写之前必须确保已经从 NVS 读过，否则会把其它动作的
    // 绑定一起冲成 0。（pad_start() 里也会 init，这里只是兜底。）
    pad_bind_init();

    s_table[(int)action] = *entry;
    const bool saved = table_save();
    ESP_LOGI(TAG, "bound %s: byte=%u mask=0x%02x expect=0x%02x len=%u%s",
             pad_bind_action_name(action), (unsigned)entry->byte_index,
             (unsigned)entry->mask, (unsigned)entry->expect,
             (unsigned)entry->report_len, saved ? "" : " (not persisted)");
}

void pad_bind_clear(pad_bind_action_t action)
{
    if ((int)action < 0 || (int)action >= (int)PAD_BIND_COUNT) {
        ESP_LOGW(TAG, "clear with invalid action %d", (int)action);
        return;
    }
    pad_bind_init();

    memset(&s_table[(int)action], 0, sizeof(s_table[(int)action]));
    (void)table_save();
    ESP_LOGI(TAG, "cleared %s", pad_bind_action_name(action));
}

void pad_bind_clear_all(void)
{
    pad_bind_init();

    memset(s_table, 0, sizeof(s_table));
    // 清绑定时一并清掉描述符推导的那份：用户主动"全部清除"是想回到干净状态，
    // 屏幕上还留着亮着的圈会让人以为清除没生效。
    memset(s_live_desc, 0, sizeof(s_live_desc));
    (void)table_save();
    ESP_LOGI(TAG, "cleared all bindings");
}

bool pad_bind_all_set(void)
{
    for (int i = 0; i < (int)PAD_BIND_COUNT; ++i) {
        if (!entry_usable(&s_table[i])) {
            return false;
        }
    }
    return true;
}

bool pad_bind_any_set(void)
{
    for (int i = 0; i < (int)PAD_BIND_COUNT; ++i) {
        if (entry_usable(&s_table[i])) {
            return true;
        }
    }
    return false;
}

// 热路径：BLE 层每包调用 9 次，只做位运算，不加锁、不打日志。
bool pad_bind_pressed(pad_bind_action_t action, const uint8_t *data, uint16_t len)
{
    if ((int)action < 0 || (int)action >= (int)PAD_BIND_COUNT) {
        return false;
    }
    const pad_bind_entry_t e = s_table[(int)action];
    if (!e.valid || e.mask == 0 || data == NULL) {
        // mask == 0 是表被写坏的症状（恒真 = 永久按下），宁可当未绑定。
        // 正常路径下 pad_bind_set()/table_load() 已经拦住了这种记录。
        return false;
    }
    if (len != e.report_len) {
        return false;   // 换了手柄/换了模式：绑定作废，而不是误触发
    }
    if (e.byte_index >= len) {
        return false;
    }
    return (uint8_t)(data[e.byte_index] & e.mask) == e.expect;
}

// ---------------------------------------------------------------- 捕获状态机
//
// 硬件层无法自己判断"用户现在有没有按键"，所以流程设计成两段：
//   1. WAIT：先连续两包完全一致来近似"所有键都松开"（刚连上时手柄还在初始化，
//      第一包经常是抖的，只取一包会把抖动当成基线）；
//   2. 抓到变化进 RELEASE：必须看到用户松手（报文逐字节回到基线）才落盘，
//      否则用户按住不放时会被判成"一直按着"，绑定一存下来就是卡键。
//
// 超时是**内部**兜底：UI 会按 PAD_BIND_CAPTURE_TIMEOUT_MS 提示并 cancel，
// 但万一 UI 那边没跑（页面被切走、任务被饿死），BLE 层会永远停在"跳过正常
// 解析"的分支里，症状是摇杆彻底失效且日志没有任何线索。所以状态机自己也在
// 每条入口上查一次超时，超时就自己进 FAIL —— FAIL 不阻塞解析。

static void capture_reset_baseline(void)
{
    s_have_base = false;
    s_base_len = 0;
    s_cand_len = 0;
    s_cand_count = 0;
    s_release_count = 0;
    s_hold_byte = 0;
    s_hold_count = 0;
}

static bool capture_timed_out(void)
{
    if (s_cap_started_us == 0) {
        return false;
    }
    const int64_t elapsed_us = esp_timer_get_time() - s_cap_started_us;
    return elapsed_us > (int64_t)PAD_BIND_CAPTURE_TIMEOUT_MS * 1000;
}

// 到点了就 abort。注意「已确认次数」也必须清零：三次取样必须连贯，超时说明
// 用户换了想法（或者中间那个键压根不是他想绑的），带着半成品续上没有意义。
static void capture_abort(void)
{
    s_cycles = 0;
    memset(&s_ref, 0, sizeof(s_ref));
    capture_reset_baseline();
    memset(&s_pending, 0, sizeof(s_pending));
    s_stage = PAD_BIND_CAP_WAIT;
}

static void capture_check_timeout(void)
{
    if (s_stage != PAD_BIND_CAP_WAIT && s_stage != PAD_BIND_CAP_RELEASE) {
        return;
    }
    if (!capture_timed_out()) {
        return;
    }
    ESP_LOGW(TAG, "capture timeout after %d ms (action=%s, %u/%u samples), aborting",
             PAD_BIND_CAPTURE_TIMEOUT_MS, pad_bind_action_name(s_cap_action),
             (unsigned)s_cycles, (unsigned)PAD_BIND_REQUIRED_CYCLES);
    s_stage = PAD_BIND_CAP_FAIL;
    s_cap_started_us = 0;
    capture_abort();
    s_stage = PAD_BIND_CAP_FAIL;   // abort 会把状态拉回 WAIT，这里必须覆盖回来
}

// WAIT 阶段：先拿到基线，再等第一个字节变化。
static void capture_wait_step(const uint8_t *data, uint16_t len)
{
    if (!s_have_base) {
        // 连续两包完全一致（长度 + 内容）才认定为基线。只比一包的话，
        // 刚连上的第一包常常还带着上一次按键残留 / 控制器初始化抖动。
        if (s_cand_count > 0 && s_cand_len == len &&
            memcmp(s_cand, data, len) == 0) {
            if (s_cand_count < 0xFF) {
                ++s_cand_count;
            }
            if (s_cand_count >= PAD_BIND_BASELINE_CONFIRM) {
                memcpy(s_base, s_cand, len);
                s_base_len = len;
                s_have_base = true;
                ESP_LOGI(TAG, "baseline locked: len=%u", (unsigned)len);
            }
            return;
        }
        memcpy(s_cand, data, len);
        s_cand_len = len;
        s_cand_count = 1;
        return;
    }

    if (len != s_base_len) {
        // 报了不同长度的报文 = 换了 report ID 模式 / 换了设备。
        // 基线直接作废，重新等两包一致。
        ESP_LOGW(TAG, "report len %u != baseline %u, re-acquiring baseline",
                 (unsigned)len, (unsigned)s_base_len);
        capture_abort();
        return;
    }

    // ---- 选「哪个字节的变化」代表这次按键 ----
    //
    // ⚠⚠ 这是整个绑定模块最容易出错、后果也最严重的一步。原来的写法是
    //    「升序扫第一个不同的字节」，注释理由是"靠后的往往同时被摇杆噪声改写"。
    //    对 Q36 for Android 这类设备这个理由**是反的**：它的 byte0..3 就是
    //    左/右摇杆的 X/Y/Z/Rz，静置在中位 0x80 且带 ±1 LSB 抖动。用户按 A 的
    //    瞬间摇杆只要抖 1 LSB，data[0] 就变了 —— 于是把**摇杆轴**记成了 A 键。
    //    绑定判定式 (data[0] & mask) == expect 落在一个自己会跳的字节上，bit0
    //    反复翻转，屏幕上的键就一直跳（幽灵按键）。
    //    现有的 PAD_BIND_MIN_HOLD_REPORTS / 稳定字节校验挡不住它：用户按 A 时
    //    手没碰摇杆，那三包里轴字节照样是稳定的，校验照样通过。
    //
    // 现在的做法：**先问描述符**。HID Report Map 写明了哪些字节是按钮、哪些是
    // Hat、哪些是轴、哪些是填充，只有 BUTTON / HAT 字节有资格承载一次按键。
    // 轴、填充位、油门刹车这类模拟量即使变了也一律跳过。
    const hid_map_info_t *hid = pad_hid_map();
    uint16_t pick = 0;
    bool found = false;

    if (hid != NULL) {
        // 描述符可用路径：只在 BUTTON / HAT 字节里找变化，BUTTON 优先于 HAT，
        // 同类里取下标最小的（严格更优才替换 = 保留最先遇到的那个）。
        bool best_is_button = false;
        for (uint16_t i = 0; i < len; ++i) {
            if ((uint8_t)(data[i] ^ s_base[i]) == 0) {
                continue;
            }
            const hid_field_t *f = hid_field_at_raw_byte(hid, i);
            if (f == NULL) {
                continue;               // 描述符没覆盖到这个字节，当它不存在
            }
            const bool is_button = (f->kind == (uint8_t)HID_FIELD_BUTTON);
            const bool is_hat    = (f->kind == (uint8_t)HID_FIELD_HAT);
            if (!is_button && !is_hat) {
                // 轴 / 填充位 / 厂商自定义：这些字节的变化不是按键。
                // 静默跳过，不落到这里 —— 这正是要挡住的那类误绑定。
                continue;
            }
            if (!found || (is_button && !best_is_button)) {
                found = true;
                pick = i;
                best_is_button = is_button;
            }
        }
        if (!found) {
            // 本包没有任何"描述符承认的按钮/Hat 字节"变化：用户要么只是动了
            // 摇杆，要么按的键不在描述符声明的字段里。**保持 WAIT**，绝不退化
            // 成"那就取第一个变化的字节" —— 那个退路就是幽灵按键的来源。
            //
            // ⚠ 必须限流：用户按住摇杆不放时每一包都会走到这里，不限流的话
            //   100 包/秒会把日志刷爆，真正有用的按键边沿反而被冲掉。
            const int64_t now_us = esp_timer_get_time();
            if (now_us - s_nobtn_log_us > 1000000) {
                s_nobtn_log_us = now_us;
                ESP_LOGW(TAG,
                         "%s: no button/hat byte changed in this report "
                         "(descriptor-constrained), still waiting",
                         pad_bind_action_name(s_cap_action));
            }
            return;
        }
    } else {
        // ---- 兜底：没有可用描述符时 ----
        //
        // 保留原来的升序扫描，但要先把"看起来是摇杆轴"的那 4 个字节排除掉。
        //
        // 为什么：没有描述符的设备上，最靠前的 4 个字节**通常**就是左/右摇杆
        // （X/Y/Z/Rz），静置在中位 0x80 且带 ±1 LSB 抖动 —— 与上面 Q36 那次完全
        // 同一类错误。判据沿用 gameboy_pad_ble.c 的 pad_axes_first_signature()：
        // 连续 4 个字节全部落在 0x78..0x88（中位 0x80 ±8）。取 ±8 是为了容忍
        // 中位漂移，同时不会把 0x00（空按钮字节）或 0x01（Xbox flags）误判进来。
        //
        // 通知带不带 report ID 同样两种解释都试（偏移 0 与偏移 1），与
        // hid_field_at_raw_byte() 的处理保持一致。
        uint32_t skip_mask = 0;
        for (uint16_t off = 0; off < 2u; ++off) {
            if (len < (uint16_t)(off + 4u)) {
                continue;
            }
            bool axes = true;
            for (uint16_t k = 0; k < 4u; ++k) {
                const uint8_t v = data[(uint16_t)(off + k)];
                if (v < 0x78u || v > 0x88u) {
                    axes = false;
                    break;
                }
            }
            if (axes) {
                for (uint16_t k = 0; k < 4u; ++k) {
                    skip_mask |= (uint32_t)1u << (uint16_t)(off + k);
                }
            }
        }
        for (uint16_t i = 0; i < len && i < 32u; ++i) {
            if ((skip_mask & ((uint32_t)1u << i)) != 0u) {
                continue;
            }
            if ((uint8_t)(data[i] ^ s_base[i]) == 0) {
                continue;
            }
            found = true;
            pick = i;
            break;
        }
        if (!found) {
            return;
        }
    }

    s_pending.valid = 1;
    s_pending.report_len = len;
    s_pending.byte_index = (uint8_t)pick;
    s_pending.mask = (uint8_t)(data[pick] ^ s_base[pick]);
    // expect = 按下时的位值。Hat/枚举型字段一次变化涉及多个位，这样
    // "上=0x00、下=0x04"这类编码也能正确区分。
    s_pending.expect = (uint8_t)(data[pick] & s_pending.mask);
    s_release_count = 0;
    s_hold_byte = data[pick];
    s_hold_count = 1;
    s_stage = PAD_BIND_CAP_RELEASE;
    ESP_LOGI(TAG, "%s change detected: byte=%u mask=0x%02x expect=0x%02x "
                  "len=%u, waiting for release",
             pad_bind_action_name(s_cap_action), (unsigned)pick,
             (unsigned)s_pending.mask, (unsigned)s_pending.expect,
             (unsigned)len);
}

// 两次取样是否指向同一个物理按键。报文长度也必须一致：长度变了说明设备换了
// 模式（或换了 report ID 的携带方式），前后样本没有可比性。
static bool bind_sample_same(const pad_bind_entry_t *a,
                             const pad_bind_entry_t *b)
{
    return a->valid == b->valid &&
           a->report_len == b->report_len &&
           a->byte_index == b->byte_index &&
           a->mask == b->mask &&
           a->expect == b->expect;
}

// RELEASE 阶段：等用户松手，然后把它记成一次取样。
static void capture_release_step(const uint8_t *data, uint16_t len)
{
    if (len != s_base_len) {
        ESP_LOGW(TAG, "report len changed during release (%u -> %u), restart",
                 (unsigned)s_base_len, (unsigned)len);
        capture_abort();
        return;
    }
    if (memcmp(data, s_base, len) != 0) {
        // 还没松手。这里必须先排除「其实是噪声变了」：目标字节如果又换了一个
        // 值，说明它压根不受一次按键支配（典型是模拟轴在跳），这条候选立即
        // 作废，回到 WAIT 重新取基线。
        if (s_pending.valid && s_pending.byte_index < len &&
            data[s_pending.byte_index] != s_hold_byte) {
            ESP_LOGW(TAG,
                     "%s: byte=%u unstable %02x -> %02x, looks like analog "
                     "noise, discarding candidate",
                     pad_bind_action_name(s_cap_action),
                     (unsigned)s_pending.byte_index,
                     (unsigned)s_hold_byte, (unsigned)data[s_pending.byte_index]);
            capture_abort();
            return;
        }
        s_release_count = 0;   // 还按着（或又按了别的），继续等
        if (s_hold_count < 0xFF) {
            ++s_hold_count;
        }
        return;
    }

    // 已经回到基线。但"回到基线"本身还不够：摇杆抖动是 0x80 -> 0x81 -> 0x80
    // 这样逐包翻转的，中途那一包也会被上面看成"按下了"，再下一包就又回到基线。
    // 只看松手与否，抖动照样能凑成一条完整绑定 —— 而那条绑定锁死在一个
    // 自己会跳的字节上，结果就是用户没碰手柄，按键却一直在触发。
    // 所以要求：这个变化至少连续保持了 PAD_BIND_MIN_HOLD_REPORTS 包，才承认
    // 它是一次真正的按键。见该宏的定义处说明。
    if (s_hold_count < PAD_BIND_MIN_HOLD_REPORTS) {
        ESP_LOGW(TAG,
                 "%s: change lasted only %u report(s) (byte=%u mask=0x%02x), "
                 "too short to be a keypress, discarding",
                 pad_bind_action_name(s_cap_action), (unsigned)s_hold_count,
                 (unsigned)s_pending.byte_index, (unsigned)s_pending.mask);
        capture_abort();
        return;
    }

    if (s_release_count < 0xFF) {
        ++s_release_count;
    }
    if (s_release_count < 2) {
        return;
    }

    // ---- 一次完整的「按下-松开」到手，拿它去和前几次样本比对 ----
    //
    // 为什么要连续三次：一次变化就落盘是最容易串键的做法。控制器里"哪里有
    // 变化"这一个信号承载的信息太少 —— 摇杆微动、误碰到某个键、手柄自己重发
    // 一包状态，都会凑成一次合格的按下-松开。要求连续三次拿到**完全相同**的
    // 样本（同一字节、同样的 mask、同样的 expect、同样的报文长度），偶发的串
    // 键自然会被挡在外面：串来的那次会长成别的样子。
    if (s_cycles > 0 && !bind_sample_same(&s_pending, &s_ref)) {
        ESP_LOGW(TAG,
                 "%s: sample %u differs from the first one "
                 "(byte %u/%u mask 0x%02x/0x%02x expect 0x%02x/0x%02x), "
                 "restarting the 3-press sequence",
                 pad_bind_action_name(s_cap_action), (unsigned)(s_cycles + 1),
                 (unsigned)s_pending.byte_index, (unsigned)s_ref.byte_index,
                 (unsigned)s_pending.mask, (unsigned)s_ref.mask,
                 (unsigned)s_pending.expect, (unsigned)s_ref.expect);
        capture_abort();
        return;
    }
    if (s_cycles == 0) {
        s_ref = s_pending;
        ESP_LOGI(TAG, "%s: reference sample locked byte=%u mask=0x%02x "
                      "expect=0x%02x, press it %d times total",
                 pad_bind_action_name(s_cap_action), (unsigned)s_ref.byte_index,
                 (unsigned)s_ref.mask, (unsigned)s_ref.expect,
                 PAD_BIND_REQUIRED_CYCLES);
    }
    ++s_cycles;
    memset(&s_pending, 0, sizeof(s_pending));
    s_hold_byte = 0;
    s_hold_count = 0;
    s_release_count = 0;

    if (s_cycles < PAD_BIND_REQUIRED_CYCLES) {
        // 还差几次：基线和已攒的次数都留着，只等下一次按下。
        s_stage = PAD_BIND_CAP_WAIT;
        ESP_LOGI(TAG, "%s: sample %u/%u accepted, keep pressing the same key",
                 pad_bind_action_name(s_cap_action), (unsigned)s_cycles,
                 (unsigned)PAD_BIND_REQUIRED_CYCLES);
        return;
    }

    pad_bind_set(s_cap_action, &s_ref);
    s_cycles = 0;
    memset(&s_ref, 0, sizeof(s_ref));
    s_cap_started_us = 0;
    s_stage = PAD_BIND_CAP_DONE;
    ESP_LOGI(TAG, "capture done: %s", pad_bind_action_name(s_cap_action));
}

void pad_bind_capture_start(pad_bind_action_t action)
{
    // 整表写盘，先确保已经从 NVS 加载过，否则会把其它动作的绑定冲掉。
    pad_bind_init();

    s_cap_started_us = esp_timer_get_time();
    memset(&s_pending, 0, sizeof(s_pending));
    capture_reset_baseline();
    // 三次取样是一个整体，开新一轮必须从头计数。留着上一次的次数会让用户界面
    // 显示"第 3/3 次"，而用户其实一次都还没按。
    s_cycles = 0;
    memset(&s_ref, 0, sizeof(s_ref));

    if ((int)action < 0 || (int)action >= (int)PAD_BIND_COUNT) {
        ESP_LOGW(TAG, "capture start with invalid action %d", (int)action);
        s_stage = PAD_BIND_CAP_FAIL;
        return;
    }
    s_cap_action = action;
    s_stage = PAD_BIND_CAP_WAIT;
    ESP_LOGI(TAG, "capture start: %s, press the same key %d times",
             pad_bind_action_name(action), PAD_BIND_REQUIRED_CYCLES);
}

void pad_bind_capture_cancel(void)
{
    const bool was_active = (s_stage == PAD_BIND_CAP_WAIT ||
                             s_stage == PAD_BIND_CAP_RELEASE);
    s_stage = PAD_BIND_CAP_IDLE;
    s_cap_started_us = 0;
    memset(&s_pending, 0, sizeof(s_pending));
    capture_reset_baseline();
    s_cycles = 0;
    memset(&s_ref, 0, sizeof(s_ref));
    // 描述符推导的那份实时状态也要清：退出绑定页后不再有人喂它，留着会让页面
    // 上某个圈一直亮着，用户会误判成按键卡住。
    memset(s_live_desc, 0, sizeof(s_live_desc));
    if (was_active) {
        ESP_LOGI(TAG, "capture cancelled: %s",
                 pad_bind_action_name(s_cap_action));
    }
}

pad_bind_capture_stage_t pad_bind_capture_stage(void)
{
    // 在这里也查一次超时：UI 会周期轮询本函数，即使 BLE 侧一包报文都没来
    // （手柄断线），也能把状态推到 FAIL，避免 UI 永远显示"请按下按键"。
    capture_check_timeout();
    return s_stage;
}

uint8_t pad_bind_capture_cycles(void)
{
    return s_cycles;
}

void pad_bind_update_live(const uint8_t *data, uint16_t len)
{
    for (int a = 0; a < (int)PAD_BIND_COUNT; ++a) {
        // 整体重算而不是"按下时置位"：这样"松开"不需要额外的清理时机，
        // 手柄断线也不会把某个动作永久留在按下态。
        s_live[a] = pad_bind_pressed((pad_bind_action_t)a, data, len);
    }
}

void pad_bind_update_live_desc(const bool live[PAD_BIND_COUNT])
{
    if (live == NULL) {
        // 描述符路径当前没有结论（未解析 / 描述符不可用 / 手柄断线）：清零，
        // 否则绑定页上那个圈会永远亮着 —— 用户会以为按键卡住了。
        memset(s_live_desc, 0, sizeof(s_live_desc));
        return;
    }
    memcpy(s_live_desc, live, sizeof(s_live_desc));
}

bool pad_bind_action_live(pad_bind_action_t action)
{
    if ((int)action < 0 || (int)action >= (int)PAD_BIND_COUNT) {
        return false;
    }
    // 两个来源取或：绑定表（用户亲手指认过）与描述符（权威布局推导）。
    // 描述符生效后用户没绑任何键也能玩，此时 s_live 全 false，靠 s_live_desc
    // 才能把绑定页上的圈点亮 —— 否则用户会以为手柄没连上。
    return s_live[(int)action] || s_live_desc[(int)action];
}

void pad_bind_on_raw_report(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    if (s_stage != PAD_BIND_CAP_WAIT && s_stage != PAD_BIND_CAP_RELEASE) {
        return;   // 没在捕获 / 已经终态，直接走正常解析
    }

    capture_check_timeout();
    if (s_stage != PAD_BIND_CAP_WAIT && s_stage != PAD_BIND_CAP_RELEASE) {
        return;   // 刚刚超时，这次不推进
    }

    if (len > PAD_BIND_RAW_MAX) {
        // 超出比较缓冲：忽略这一包。不置 FAIL —— 偶发超长包不该毁掉整个
        // 捕获流程，真拿不到基线时超时兜底会收尾。
        return;
    }

    if (s_stage == PAD_BIND_CAP_WAIT) {
        capture_wait_step(data, len);
    } else {
        capture_release_step(data, len);
    }
}

// ---------------------------------------------------------------- 动作名

const char *pad_bind_action_name(pad_bind_action_t action)
{
    // 顺序必须和 pad_bind_action_t 完全一致，编译期用数组长度卡住：
    // 枚举加一项而这里忘了加，"kNames 少一个初始化项"会直接报错，不会等到
    // 用户跑起来才发现某个按键显示成 "?"。
    static const char *const kNames[PAD_BIND_COUNT] = {
        "A", "B", "X", "Y", "上", "下", "左", "右", "START", "SELECT",
    };
    _Static_assert(sizeof(kNames) / sizeof(kNames[0]) ==
                       (size_t)PAD_BIND_COUNT,
                   "pad bind action names must cover every action");
    if ((int)action < 0 || (int)action >= (int)PAD_BIND_COUNT) {
        return "?";
    }
    return kNames[(int)action];
}
