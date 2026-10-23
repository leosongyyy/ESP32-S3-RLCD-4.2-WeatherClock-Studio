// gameboy_pad_ble.c —— 通过 NimBLE 做 BLE HID Host，连接蓝牙手柄并解析报文。
//
// 设计要点（踩坑记录，改之前先读）：
//  1. 必须 CONFIG_BT_NIMBLE_ROLE_CENTRAL=y（本机做 GATT client）；HID 服务
//     0x1812 在远端手柄上，本机不需要开任何 HID service；
//  2. 连接时 gap_params.itvl_min/max 必须是 6..3200，max_latency 必须是 0..499，
//     否则 ble_gap_connect() 直接返回 BLE_ERR_INV_ARG；
//  3. 不要用 esp_hidh_dev_open()，它内部的 ble_gap_connect 回调最多阻塞 30 秒，
//     必须放在独立任务里，否则会把 BLE host 任务卡死。这里全部用异步回调；
//  4. 必须自己开 GAP 扫描（ble_gap_disc），HID host 不会帮你扫。
#include "gameboy_pad.h"
#include "gameboy_pad_bind.h"
#include "gameboy_hid.h"
#include "gameboy_input.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "os/os_mbuf.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

// 关于 bond 存储的两个坑（改之前先读）：
// 1. 不要 include "host/store/util/ble_store_util.h" —— IDF v5.5.3 没导出这个头，
//    ble_store_util_status_rr 的声明在 host/ble_store.h，已被 host/ble_hs.h 间接包含。
// 2. ble_store_config_init() 在 ble_store_config.c:1183 有定义，但公开头文件
//    store/config/ble_store_config.h 只声明了 read/write/delete，没有它。
//    IDF 自己的 test_apps/memory_release 也是用 extern 声明后再调的，这里照做。
//    它负责把 ble_hs_cfg 的三个 store 回调挂到 NVS 后端，
//    配合 CONFIG_BT_NIMBLE_NVS_PERSIST=y 才能让 bond 真正落盘。
extern void ble_store_config_init(void);

// 连接失败的 peer 暂时拉黑：现场抓到过一次「扫描到一个 rssi=-94 的设备，
// 连上去 9 秒超时，再等 4 秒重新扫描，然后又选到它」，
// 一轮 13 秒的死循环。真手柄只要在同一空间里，RSSI 通常远好于这个水平。
// 拉黑不是永久的，60 秒后自动放出来，手柄关了再开能重新被选中。
#define PAD_AVOID_AFTER_FAIL_MS  60000

#define PAD_TAG "gameboy_pad"

// HID over GATT
#define UUID_HID_SERVICE   0x1812
#define UUID_REPORT        0x2A4D
#define UUID_REPORT_MAP    0x2A4B   // HID Report Map：字段 bit 布局的权威依据
#define UUID_CCCD          0x2902
#define UUID_HID_INFO      0x2A4A

#define PAD_NAME_LEN       24
#define PAD_CCCD_SCAN_SPAN 10      // 值句柄之后最多找 10 个描述符
#define PAD_RETRY_DELAY_MS 4000
#define PAD_CONNECT_TIMEOUT_MS 9000

typedef enum {
    ST_OFF = 0,
    ST_STARTING,
    ST_SCANNING,
    ST_CONNECTING,
    ST_DISCOVERING,
    ST_READY,
    ST_RETRY_WAIT,
    ST_ERROR
} pad_internal_state_t;

static pad_internal_state_t s_state = ST_OFF;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool     s_subscribed;
static bool     s_disc_active;
// 「本次连接是否已经发起过 GATT 发现」。
//
// 用它而不是 `s_svc_start == 0` 作为"还没发现过"的门闸：s_svc_start 是**发现
// 的结果**（HID 服务的起始句柄），发现成功一次之后就一直是 19；拿它当门闸，
// 第二次及以后的连接门闸会被上一次的残留值永久关死 —— 现场表现就是链路明明
// 连上了，状态却永远停在 ST_CONNECTING，屏幕上一路显示"连接中"。
// 语义上该问的是"我发起过发现吗"，不是"我发现到东西了吗"。
static bool     s_disc_started;
// 链路建立时刻，给「连上了却迟迟不往下走」的看门狗用。
static uint32_t s_conn_since_ms;
static char     s_name[PAD_NAME_LEN];
static uint32_t s_reports;
static int      s_report_mode;          // 0 未锁定 / 1 Xbox BLE / 2 通用 Hat / 3 轴在前
static uint64_t s_retry_at_ms;
static bool     s_host_synced;
static bool     s_stop_requested;
static bool     s_nimble_inited;

// 手柄服务任务（见文件末尾「服务任务」一节）
static TaskHandle_t     s_service_task = NULL;
static volatile uint32_t s_last_visible_ms;
static bool             s_remote_active;
static bool             s_was_visible;

// 最近一次尝试连接的目标（连接失败的事件里不带地址，只能在发起时记下来）
static ble_addr_t s_attempt_addr;
static uint32_t   s_avoid_until_ms;
static bool       s_avoid_valid;

static uint16_t s_svc_start;
static uint16_t s_svc_end;
static uint16_t s_report_val;
static uint16_t s_report_end;
static uint16_t s_cccd_handle;
static bool     s_report_notifies;      // 选中的 report 特征是否带 NOTIFY
static uint8_t  s_report_props;         // 选中特征的属性字节，仅用于故障日志
static uint32_t s_ready_since_ms;       // 进入 READY 的时刻，用于「假连接」探测
static uint8_t  s_empty_ready_count;    // 连续「READY 但零报文」的次数

// HID Report Map（0x2A4B）。这是唯一能权威确定字段 bit 布局的东西：
// 有了它就不用再靠"报文像什么"去猜。本版**会真的解析它**，解析结果驱动的
// pad_apply_descriptor() 优先级高于下面所有启发式布局（mode 1/2/3）——
// 那些启发式只作为"描述符读不到/解析不出字段"时的兜底继续保留。
//
// ⚠ 解析时机：GATT 读完之后立刻解析（见 pad_on_report_map() 末尾），不等第一包报文。
//   Q36 for Android 这类设备静置时一包报文都不发（实测 reports=0 持续 150 秒），
//   惰性解析永远触发不了，字段表与绑定表清洗这些诊断全部沉默，问题只能靠猜。
#define PAD_MAP_MAX   256               // 上限，超出部分截断（正常 map 远小于此）
#define PAD_MAP_LINE  16                // 每条日志最多 16 字节，方便一次性 grep
static uint16_t s_report_map_handle;
static uint8_t  s_map[PAD_MAP_MAX];
static uint16_t s_map_len;
static bool     s_map_done;             // 本次连接已读完/已放弃，避免重复发起

// 描述符解析结果。
//
// s_hid_tried 是"我已经尝试过解析本次连接的描述符了"的门闸：描述符可能读失败、
// 也可能读到了但一个字段都解析不出来，这两种情况都要**只尝试一次**并永久回到
// 启发式路径，不能每帧重试（每帧重解析 200 字节描述符纯属浪费 CPU）。
// ⚠ 它与 s_map_done 不是一回事：s_map_done 说的是"读完了"，s_hid_tried 说的
//   是"解析过了"。读失败时 s_map_done 也会置位，此时必须靠 s_hid_tried 拦住
//   后续每帧的无效解析尝试。
static hid_map_info_t s_hid;
static bool           s_hid_tried;
static bool           s_hid_path_logged;      // "hid path:" 首帧日志只打一次
static bool           s_hid_nofield_logged;   // "没有可用字段"的降级日志只打一次

// 配对（加密）状态。连上之后先发起配对，等 ENC_CHANGE 到了再做 GATT 发现。
// s_sec_since_ms 是发起配对的时刻，用于「ENC_CHANGE 迟迟不来」的兜底超时。
static bool     s_enc_done;
static uint32_t s_sec_since_ms;

static uint8_t s_buttons[GB_BTN_COUNT];

// ---------------------------------------------------------------- 已知手柄
//
// 上次成功用过的手柄地址存在 NVS 里，下次进游戏页直接对它发连接请求，不必等
// 它恰好在扫描窗口里广播出来——用户反馈的「一进页面就要等十几秒」主要就是这
// 个等待。入口见 pad_begin_search()。

#define PAD_PEER_NVS_NAMESPACE "padpeer"
#define PAD_PEER_NVS_KEY       "last"
#define PAD_PEER_FAIL_LIMIT    2

static bool       s_peer_valid;
static ble_addr_t s_peer;
// 同一台设备本轮回连失败的次数，只活在内存里，不落盘。
static uint8_t    s_peer_fail;
// 本轮这次连接是不是"直连记住的手柄"（决定了 CONNECT 失败时该怎么降级）。
static bool       s_peer_attempt;

// 地址是否不可用：全零 / 全 0xFF 连不上（对端不会回应连接请求）。
// 判据和扫描路径里那个 guard 是同一套，别只改一边。
static bool pad_addr_unusable(const uint8_t *v)
{
    bool zero = true;
    bool ones = true;
    for (int i = 0; i < 6; i++) {
        if (v[i] != 0x00) { zero = false; }
        if (v[i] != 0xFF) { ones = false; }
    }
    return zero || ones;
}

// 只在收到本次连接的**第一条**报文时调用（见 pad_handle_report()）：
// 报文真的来了才证明这台设备是可用的手柄。记住一个"连得上但永远不上报"的
// 地址（订阅错特征、或需要 Output Report 握手的 Switch Pro 之类）只会让之后
// 每次进页面都先白等一个直连超时。
static void pad_peer_save(const ble_addr_t *a)
{
    if (!a || pad_addr_unusable(a->val)) {
        return;
    }

    // 内存里先更新：NVS 写失败只影响"下次上电还记不记得"，本次会话照样能直连。
    s_peer = *a;
    s_peer_valid = true;
    s_peer_fail = 0;

    uint8_t blob[7];
    memcpy(blob, a->val, 6);
    blob[6] = a->type;

    nvs_handle_t h;
    if (nvs_open(PAD_PEER_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(PAD_TAG, "known gamepad (not persisted)");
        return;
    }
    esp_err_t err = nvs_set_blob(h, PAD_PEER_NVS_KEY, blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(PAD_TAG, "known gamepad (not persisted): %s",
                 esp_err_to_name(err));
    }
}

// 上电后读一次。所有失败路径都只是"没有已知手柄"，绝不当致命错误处理 ——
// NVS 分区没初始化/没这个 key 都得能正常走扫描路径。
static void pad_peer_load(void)
{
    s_peer_valid = false;

    nvs_handle_t h;
    if (nvs_open(PAD_PEER_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(PAD_TAG, "no known gamepad yet");
        return;
    }
    uint8_t blob[7] = { 0 };
    size_t len = sizeof(blob);
    const esp_err_t err = nvs_get_blob(h, PAD_PEER_NVS_KEY, blob, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(blob)) {
        ESP_LOGI(PAD_TAG, "no known gamepad yet");
        return;
    }
    if (pad_addr_unusable(blob)) {
        ESP_LOGI(PAD_TAG, "no known gamepad yet (unusable addr)");
        return;
    }

    memcpy(s_peer.val, blob, 6);
    s_peer.type = blob[6];
    s_peer_valid = true;
    s_peer_fail = 0;
    ESP_LOGI(PAD_TAG, "known gamepad %02x:%02x:%02x:%02x:%02x:%02x type=%u",
             s_peer.val[5], s_peer.val[4], s_peer.val[3],
             s_peer.val[2], s_peer.val[1], s_peer.val[0],
             (unsigned)s_peer.type);
}

// ---------------------------------------------------------------- 状态

pad_state_t pad_state(void)
{
    switch (s_state) {
    case ST_STARTING:    return PAD_STATE_STARTING;
    case ST_SCANNING:    return PAD_STATE_SCANNING;
    case ST_CONNECTING:  return PAD_STATE_CONNECTING;
    case ST_DISCOVERING: return PAD_STATE_DISCOVERING;
    case ST_READY:       return PAD_STATE_READY;
    case ST_RETRY_WAIT:  return PAD_STATE_RETRY_WAIT;
    case ST_ERROR:       return PAD_STATE_ERROR;
    case ST_OFF:
    default:             return PAD_STATE_OFF;
    }
}

const char *pad_state_text(void)
{
    switch (pad_state()) {
    case PAD_STATE_STARTING:    return "BT INIT";
    case PAD_STATE_SCANNING:    return "BT SCANNING";
    case PAD_STATE_CONNECTING:  return "BT CONNECTING";
    case PAD_STATE_DISCOVERING: return "BT PAIRING";
    case PAD_STATE_READY:       return "BT PAD READY";
    case PAD_STATE_RETRY_WAIT:  return "BT RETRY";
    case PAD_STATE_ERROR:       return "BT ERROR";
    case PAD_STATE_OFF:
    default:                    return "BT OFF";
    }
}

const char *pad_name(void)
{
    return s_name;
}

bool pad_button(gb_button_t button)
{
    if ((int)button < 0 || (int)button >= GB_BTN_COUNT) {
        return false;
    }
    return s_buttons[button] != 0;
}

uint32_t pad_report_count(void)
{
    return s_reports;
}

int pad_report_mode(void)
{
    return s_report_mode;
}

static void pad_clear_buttons(void)
{
    memset(s_buttons, 0, sizeof(s_buttons));
}

// ⚠ 覆盖语义：pressed=false 会**清掉**该位。凡是「多个来源都要往同一个键写」
// 的场景（hat 先写方向、按钮组再叠加）必须改用 pad_or()，否则后写的会把先写
// 的结果抹掉 —— 现场症状是方向键按了没反应，而日志里 hat 值是对的。
static void pad_set(gb_button_t b, bool pressed)
{
    s_buttons[b] = pressed ? 1 : 0;
}

// 只置位、不清位。用于「hat 先写方向、按钮组再叠加」这类需要合并的场合。
static void pad_or(gb_button_t b, bool pressed)
{
    if (pressed) {
        s_buttons[b] = 1;
    }
}

// ---------------------------------------------------------------- 报文解析
//
// 不同手柄的 report 布局差异极大。这里做四件事：
//  1. 先判定 payload 到底带不带 report ID。HOGP 规范说通知里不带（ID 由
//     Report Reference 描述符描述），但大量从经典 BT HID 移植过来的固件会塞
//     在第一个字节。两种设备在 GATT 层完全一样，没有外部标志，只能靠报文
//     本身判断 —— 判错的后果是所有偏移整体错 1 字节，按钮全乱。
//  2. 按三套布局解析：Xbox Series/One S 的 BLE 报文、通用 HID Gamepad，
//     以及「轴在前」的通用 pad（mode 3，见 pad_apply_axes_first()）。
//  3. 每帧先清空按键再解析。任何早退路径（短报文、分包）都必须让按键归零，
//     否则上一帧的按键会永久粘住 —— 现场症状是「角色自己一直往一个方向走」，
//     而日志一切正常，极难查。
//  4. 把原始报文转储出来（见 pad_dump_raw()）。布局全是猜的，没有原始字节
//     就没法事后复盘，这条日志比任何状态机都管用。
//
// 已知未覆盖（留给后续的自学习校准）：dpad 用 4 个独立 Button usage 而非 Hat
// 的设备，以及必须先写 Output Report 握手才会开始上报的 Switch Pro / DualSense。
//
// 声明：定义在文件末尾的服务任务一节。原始报文转储的限流要用到，这里提前声明。
static uint32_t pad_now_ms(void);

#define PAD_ID_PROBE_COUNT 5

enum { PAD_ID_UNKNOWN = 0, PAD_ID_PRESENT = 1, PAD_ID_ABSENT = 2 };

static uint8_t s_id_state = PAD_ID_UNKNOWN;
static uint8_t s_id_first;
static bool    s_id_varying;
static uint8_t s_id_probe;
static bool    s_hat_null15_seen;

// ---- hat 编码族的运行时自校准（描述符路径专用）----
//
// ⚠ 描述符不可全信。Q36 的描述符写 Logical Min 0 / Max 7 + Null state，照规范
//   解就是"0=上"，可它的固件实际发的是"0=居中、1..8 顺时针"。描述符是从别处
//   抄来的、与固件不一致，廉价手柄上极其普遍。只按描述符解码，静止时的 0 会被
//   读成 dir=0=上 ⇒ 方向键永久卡在上（现场症状：一连上手柄，ROM 列表就自己
//   往上滚）。
//
// 所以改成**看实测值**：连上之后 hat 一直没变的那个值就是静止中位，用它当锚点
// 反推编码族，描述符只作为次要依据。
//
// ⚠ 这里踩过一次大坑，判据改过两版，结论写在这里免得以后又改回去：
//
//   第一版「众数 = 静止中位」是**错的**。Q36 这类手柄只在状态变化时才发报文
//   （实测静置 150 秒一包都不发），于是直方图数的是**事件**而不是时间。玩横版
//   游戏时"按住右键 + 连点 A"每点一下都会带一包 hat 仍是右的报文，右键那个桶
//   飞快涨，而中位那个桶一动不动；领先 8 票就换锚点，几秒钟就把锚点换成"右"。
//   锚点一换，按住右键变成居中、松开右键变成"上"（(0-lmin)&7==0）—— 表现就是
//   "玩一会按钮就错乱"，而且重启才好，因为锚点只在内存里。
//
//   第二版（也是现在的做法）换成**转移图**：只有当 hat 的值真的从一个变成另一个
//   时才在两者之间连一条边。中心是所有方向的枢纽，度数必然最高；而"长按不动"
//   和"按住的同时连点 A"都不产生新边，污染不了它。
//
//   再叠一条硬约束：锚点在校准窗口结束后**永久冻结**。运行期任何"我好像更常
//   见到这个值"的自适应都是同一个陷阱的变体 —— 用户玩什么、玩多久不该影响
//   设备本身的编码约定。
#define PAD_HAT_HIST_MAX 16
static uint16_t s_hat_hist[PAD_HAT_HIST_MAX];    // 见过哪些值、各见过几次
static uint16_t s_hat_degree[PAD_HAT_HIST_MAX];  // 转移邻接位图：bit v = 与 v 直接相邻过
static bool     s_hat_have_anchor;   // 已经拿到过第一个 hat 样本
static uint32_t s_hat_anchor_raw;    // "静止中位"原始值
static bool     s_hat_anchor_logged; // 锚点日志限流，避免每帧刷屏
static uint32_t s_hat_prev_raw;      // 上一包的 hat 值，用于连边
static bool     s_hat_prev_valid;
static uint32_t s_hat_transitions;   // 已经发生多少次 hat 变化
static uint32_t s_hat_first_ms;      // 第一个样本的时刻
static bool     s_hat_frozen;        // 校准窗口已过，锚点不再改动

// 校准窗口：凑够这么多条转移边就锁死锚点。8 条边在选 ROM 那几下上下键里就能
// 凑齐，用户基本察觉不到校准过程。
#define PAD_HAT_CALIB_TRANSITIONS 8
// 兜底：万一用户连上就按住某个方向不放（hat 一直不变，转移边永远是 0），
// 到点也得锁。但**至少要有 3 条边**才允许走时间兜底 —— 一条边都没有就锁，
// 锁住的其实是"连上时手指正按着的那个方向"，那比不锁还糟。
#define PAD_HAT_CALIB_MIN_TRANS  3
#define PAD_HAT_CALIB_WINDOW_MS  4000

// 见过多少个不同的 hat 取值。真正的 hat 最多只有 9 个（8 个方向 + 1 个居中），
// 见到的不同值超过 9 个就说明这一位**根本不是 hat** —— 多半是报表挑错/
// report ID 剥没剥的判断错了，读到了摇杆轴字节上。那种字节会连续变化，
// 按 hat 解就是方向键自己乱跳。这条是兜底的最后一道闸。
#define PAD_HAT_MAX_DISTINCT 9
static uint8_t s_hat_distinct;
static bool    s_hat_distinct_logged;

// ---- hat 锚点选取 ----

// 值是否落在描述符声明的逻辑区间内。区间外的值按 HID 规范就是 null（居中）。
static bool hat_value_in_range(uint32_t v, int32_t lmin, int32_t lmax)
{
    if (lmax < lmin) {
        return false;               // 描述符本身异常，别拿它当判据
    }
    const int64_t iv = (int64_t)v;
    return iv >= (int64_t)lmin && iv <= (int64_t)lmax;
}

// 在见过的值里挑锚点。判据依次是：
//
//   1) 转移度数高的优先。中心是所有方向的枢纽，度数必然最高；单个方向只和
//      中心相邻，度数为 1。
//   2) 度数并列时，**出现次数少的**优先。中位是"每按一次方向就路过一次"的
//      枢纽，而长按方向不动会猛涨那个方向的计数（按住的同时连点 A 也一样，
//      每点一下都带一包 hat 同值的报文），所以计数少的一侧才是中位。
//   3) 还是并列时，区间外的值优先 —— 按规范它就是 null，也就是居中（族 B 的
//      8 / 15）。
//   4) 最后取最小的 —— 族 A 的"0 = 居中"是最常见的约定。
//
// 这三条裁决覆盖了两种族在"用户只按过一两个方向"这种最坏情况下的判定：
//   族 A 只按了右：候选 {0(中), 3(右)}，度数并列 → 计数少的是 0 → 对；
//   族 B 只按了上：候选 {8(中), 0(上)}，度数并列 → 计数少的是 8 → 对。
//
// ⚠ 这个函数的返回值只取决于 hist / degree 的当前内容，是**确定性**的，所以
//   调用方可以在校准窗口内每次都无条件采纳它，不会来回抖（数据只会单调增长）。
// 返回 -1 表示还没有任何样本。
static int hat_pick_anchor(int32_t lmin, int32_t lmax)
{
    int      best = -1;
    unsigned best_deg = 0;
    uint16_t best_cnt = 0;
    for (uint16_t v = 0; v < PAD_HAT_HIST_MAX; ++v) {
        if (s_hat_hist[v] == 0u) {
            continue;
        }
        const unsigned deg =
            (unsigned)__builtin_popcount((unsigned)s_hat_degree[v]);
        const uint16_t cnt = s_hat_hist[v];
        if (best < 0) {
            best = (int)v;
            best_deg = deg;
            best_cnt = cnt;
            continue;
        }
        if (deg != best_deg) {
            if (deg > best_deg) {
                best = (int)v;
                best_deg = deg;
                best_cnt = cnt;
            }
            continue;
        }
        if (cnt != best_cnt) {
            if (cnt < best_cnt) {
                best = (int)v;
                best_cnt = cnt;
            }
            continue;
        }
        const bool best_in = hat_value_in_range((uint32_t)best, lmin, lmax);
        const bool cand_in = hat_value_in_range((uint32_t)v, lmin, lmax);
        if (best_in && !cand_in) {
            best = (int)v;           // 区间外的更像 null
            best_cnt = cnt;
        } else if (best_in == cand_in && (uint32_t)v < (uint32_t)best) {
            best = (int)v;
            best_cnt = cnt;
        }
    }
    return best;
}

// 方向键：同时兼容两族 hat 约定
//   族 A：0 = 居中，1..8 顺时针
//   族 B：0 = 上，1..7 顺时针，15 = 居中
// 判别靠"有没有见过 15"：见过就说明这台设备用族 B。
// 注意 0 在族 B 里表示 Up，不是居中 —— 这就是"上方向按了没反应"的根因。
static void pad_apply_hat(uint8_t hat)
{
    if (hat == 15) {
        s_hat_null15_seen = true;
        return;                       // 本帧就是居中，按键已由调用方清零
    }
    if (hat == 0 && s_hat_null15_seen) {
        hat = 1;                      // 族 B 里 0 表示 Up
    }
    if (hat == 0 || hat > 8) {
        return;                       // 居中或未知值，保持已清零状态
    }
    pad_set(GB_BTN_UP, hat == 1 || hat == 2 || hat == 8);
    pad_set(GB_BTN_RIGHT, hat == 2 || hat == 3 || hat == 4);
    pad_set(GB_BTN_DOWN, hat == 4 || hat == 5 || hat == 6);
    pad_set(GB_BTN_LEFT, hat == 6 || hat == 7 || hat == 8);
}

// 下面两个 apply 收到的 b 是**已剥掉 report ID** 的数据体。
static void pad_apply_xbox_ble(const uint8_t *b, uint16_t len)
{
    // Xbox BLE：b[0] = flags(0x01 / 0x14 带时间戳)，b[3] = hat + 方向位，
    // b[4] = A/B/X/Y/LB/RB/View/Menu。
    // 长度门槛只要 5：原来卡 len >= 12 把 10/11 字节的变体全挡在门外，
    // 那样每帧都走早退，配合"不清按键"就是最长的那种卡键。
    if (len < 5) {
        return;
    }
    const uint8_t btns = b[4];

    pad_set(GB_BTN_A, (btns & 0x01) != 0);
    pad_set(GB_BTN_B, (btns & 0x02) != 0);
    pad_set(GB_BTN_SELECT, (btns & 0x20) != 0);   // View
    pad_set(GB_BTN_START, (btns & 0x10) != 0);    // Menu

    pad_apply_hat((uint8_t)(b[3] & 0x0F));
}

static void pad_apply_generic(const uint8_t *b, uint16_t len)
{
    // 通用布局：b[0] = 按钮位（bit0 A、bit1 B、bit2 Select、bit3 Start），
    // b[1] 低 4 位 = Hat。
    if (len < 2) {
        return;
    }
    const uint8_t b1 = b[0];

    pad_set(GB_BTN_A, (b1 & 0x01) != 0);
    pad_set(GB_BTN_B, (b1 & 0x02) != 0);
    pad_set(GB_BTN_SELECT, (b1 & 0x04) != 0);
    pad_set(GB_BTN_START, (b1 & 0x08) != 0);

    pad_apply_hat((uint8_t)(b[1] & 0x0F));
}

// mode 3「轴在前」布局：b[0..3] = LX/LY/RX/RY（中位 0x80），b[4] = hat，
// b[5] = 按键低字节，b[6] = 按键高字节，b[7..] 厂商字节。
// 典型设备：Q36「for Android」这类 10 字节报文的通用手柄。
//
// ⚠ b[5]/b[6] 的位序是按通用 HID gamepad 习惯做的**初始猜测**，不是实测值。
// 拿到 report map（"report map len=" / "map[nnn]:" 日志）与按键转储
// （"raw #n:" 日志）之后必须回来按 Usage 顺序校正。
//
// ⚠ 按钮组一律用 pad_or() 叠加，不能用 pad_set()：pad_apply_hat() 已经先写了
// 方向键，pad_set() 的覆盖语义会把 hat 的结果清掉（方向键按了没反应）。
static void pad_apply_axes_first(const uint8_t *b, uint16_t len)
{
    // 门槛 6：至少要能读到 b[5]（按键低字节）。b[6] 本版不解析，所以不要求 7。
    if (len < 6) {
        return;
    }
    // 先让 hat 写方向，后面的按钮组只做叠加。
    pad_apply_hat((uint8_t)(b[4] & 0x0F));

    const uint8_t lo = b[5];
    pad_or(GB_BTN_A,      (lo & 0x01) != 0);
    pad_or(GB_BTN_B,      (lo & 0x02) != 0);
    pad_or(GB_BTN_SELECT, (lo & 0x04) != 0);
    pad_or(GB_BTN_START,  (lo & 0x08) != 0);

    // TODO(mode 3)：b[6]（按键高字节）的位序待实测校正。
    // 常见的候选是 X/Y/L/R（0x01/0x02/0x04/0x08）或 L1/R1/L2/R2，
    // 但也可能整字节是厂商保留位。等 report map 出来按 Usage 顺序补 pad_or()，
    // 在没证据之前不猜 —— 猜错的表现是"按 A 却触发了别的键"，比不接更难查。
}

// ---------------------------------------------------------------- 描述符路径
//
// 用 HID Report Map 解析出来的字段表解析报文，优先级高于上面三套启发式布局。
//
// 为什么要这条路：启发式只能靠「报文看起来像什么」去猜布局，猜错一次整场会话
// 按键就全废。现场踩过的那次是：把左摇杆 X 轴（静置 0x80、带 ±1 LSB 抖动）当
// 成按钮字节，bit0 反复翻转 = A 键被持续误触发，用户看到的就是「没按任何键，
// 角色却一直在动」的幽灵按键。描述符里写明了哪些位是轴、哪些位是按钮、各自的
// 取值范围 —— 有了它就不需要猜：轴**永远不参与按钮判定**，摇杆抖动落在死区里
// 一律不触发。

// 描述符里的 Button usage 编号（HID Usage Page 0x09）→ GB 键。
//
// 编号规则是 HID 规定的：1=A（下键）、2=B（右键）、3=X（左键）、4=Y（上键）、
// 9=Select/Back、10=Start/Forward、11=Home/Guide。
// 3 和 4 是 1 和 2 的「第二组位置」（见 gameboy_pad_bind.h 里枚举的说明）：
// 有些人习惯用外侧那两个键，手上这只手柄恰好把它们印成了 X / Y。
//
// ⚠ 11（Home/Guide）**刻意不映射**：用户明确要求去掉主页键；而且不少手柄用它
//   做"回系统/切换模式"，误触发会直接把用户踢出游戏，比不接更难查。
// 返回 GB_BTN_COUNT 表示「忽略」。
static gb_button_t pad_desc_button_to_gb(uint16_t btn)
{
    switch (btn) {
    case 1:
    case 3:  return GB_BTN_A;          // A / X
    case 2:
    case 4:  return GB_BTN_B;          // B / Y
    case 9:  return GB_BTN_SELECT;
    case 10: return GB_BTN_START;
    default: return GB_BTN_COUNT;      // 忽略（含 11 = Home/Guide）
    }
}

// 同一个按钮编号 → 绑定页上的动作下标。与上面那张表是**同一个映射的两面**：
// 那边写 GB 键（X 落到 A），这边写绑定页自己的格子（X 落到 X 圈），
// 两个圈不能互相覆盖（这也是为什么下面必须用「先收集再一次性写入」）。
// 返回 -1 表示忽略。
static int pad_desc_button_to_bind(uint16_t btn)
{
    switch (btn) {
    case 1:  return (int)PAD_BIND_A;
    case 3:  return (int)PAD_BIND_X;
    case 2:  return (int)PAD_BIND_B;
    case 4:  return (int)PAD_BIND_Y;
    case 9:  return (int)PAD_BIND_SELECT;
    case 10: return (int)PAD_BIND_START;
    default: return -1;                // 含 11 = Home/Guide
    }
}

// 把「某个 GB 键被按下」收集进结果表。
//
// ⚠ 必须收集而**不能**直接 pad_set()：pad_set()（见其定义处）是覆盖语义，
//   同一个 GB 键被按钮 1（A）和按钮 3（X）两个来源各写一次时，后一条会把前一
//   条抹掉 —— 表现是「只按 X 没反应，按 A 才有」。收集完由调用方统一写。
static void pad_desc_mark(bool covered[GB_BTN_COUNT], bool pressed[GB_BTN_COUNT],
                          bool desc_live[PAD_BIND_COUNT],
                          gb_button_t gb, int bind, bool on)
{
    if (!on) {
        return;
    }
    // ⚠ 必须先挡掉越界的 gb：pad_desc_button_to_gb() 用 GB_BTN_COUNT 当"忽略"
    //   哨兵值，直接拿它当下标会写到 covered[8] —— 数组只有 8 个元素，这是一次
    //   越界写，在栈上会把相邻变量改掉，症状随机且极难查。
    if ((int)gb >= 0 && (int)gb < (int)GB_BTN_COUNT) {
        covered[(int)gb] = true;
        pressed[(int)gb] = true;
    }
    if (bind >= 0 && bind < (int)PAD_BIND_COUNT) {
        desc_live[bind] = true;
    }
}

// 读某个轴（usage 编号）的当前值，并算出中点与死区。
// 返回 false 表示这个字段里没有该 usage。
static bool pad_desc_axis_value(const uint8_t *body, uint16_t body_len,
                                const hid_field_t *f, uint16_t wanted,
                                int32_t *out_val, int32_t *out_mid,
                                int32_t *out_dead)
{
    if (f->kind != (uint8_t)HID_FIELD_AXIS) {
        return false;
    }
    if ((f->main_flags & 0x01u) != 0u) {
        return false;                   // Constant：不是真轴
    }
    const uint16_t lo = f->usage_min;
    const uint16_t hi = (f->usage_max >= lo) ? f->usage_max : lo;
    if (wanted < lo || wanted > hi) {
        return false;
    }
    // X/Y 常常被合成一个 usage 0x30..0x31 的字段：第 k 个条目的 usage 是 lo+k。
    const uint16_t idx = (uint16_t)(wanted - lo);
    if (idx >= f->count) {
        return false;
    }
    const uint32_t off = (uint32_t)f->bit_offset +
                         (uint32_t)idx * (uint32_t)f->bit_size;
    if (off > 0xFFFFu || f->bit_size == 0u || f->bit_size > 32u) {
        return false;
    }
    uint32_t raw = hid_extract_bits(body, body_len, (uint16_t)off, f->bit_size);

    // 有符号还原：轴的范围经常写成 -127..127，raw 是该范围内的无符号编码，
    // 直接当正数读的话中位会整个跑偏，摇杆静置就被判成"推到底"。
    int32_t val = (int32_t)raw;
    if (f->bit_size < 32u && f->logical_min < 0) {
        const uint32_t sign_bit = 1u << (f->bit_size - 1u);
        if ((raw & sign_bit) != 0u) {
            val = (int32_t)raw - (int32_t)(1u << f->bit_size);
        }
    }

    const int32_t lmin = f->logical_min;
    const int32_t lmax = f->logical_max;
    *out_val  = val;
    *out_mid  = (lmin + lmax + 1) / 2;
    *out_dead = ((lmax - lmin) / 2) / 4;    // 25% 死区，见调用处的说明
    return true;
}

// 返回 true 表示已用描述符完成解析，调用方不再走启发式。
static bool pad_apply_descriptor(const uint8_t *data, uint16_t len)
{
    uint8_t       rid = 0;
    const uint8_t *body = NULL;
    uint16_t      body_len = 0;

    // ⚠ 返回值是 0 / 1 / 2，不是 bool：2 表示"尾部多了 1~2 字节或走了长度兜底"，
    //   即这一帧只是大致对得上，不敢打包票。
    const int pick = hid_pick_report(&s_hid, data, len, &rid, &body, &body_len);
    if (pick <= 0) {
        return false;                   // 描述符与报文对不上，交给启发式兜底
    }
    if (body == NULL || body_len == 0u) {
        return false;
    }

    // ---- 安全网：这个 report 里一个用得上的字段都没有就让位 ----
    //
    // 描述符路径一旦接管，下面的启发式就被整段跳过。若挑中的 report 只有用不上
    // 的字段（全是填充位 / 厂商自定义 / 无法映射成方向键的轴），这条路只会永远
    // 吐 0 键 —— 用户看到"手柄完全没反应"，比改造前（启发式至少还能动）更糟。
    // 这里先数一次可用字段，为 0 就主动退回启发式，保证**任何情况下都不会比
    // 改造前差**。
    if (hid_report_usable_fields(&s_hid, rid) == 0u) {
        // ⚠ 用首次标志限流：这行若每帧都打，100 包/秒会把日志彻底刷爆，真正
        //   有用的按键边沿反而被冲掉（pad_dump_raw() 那套限流是同一个道理）。
        if (!s_hid_nofield_logged) {
            s_hid_nofield_logged = true;
            ESP_LOGW(PAD_TAG,
                     "hid path: rid=%u has no button/hat/axis field, falling back",
                     (unsigned)rid);
        }
        return false;
    }

    if (pick == 2 && !s_hid_path_logged) {
        // 宽松命中：把"尾部多出来几个字节"记下来。这个数字是判断"是不是挑错
        // report 了"的关键证据 —— 多出来越多，越可能是选错了而不是填充。
        uint32_t bits = 0;
        for (uint16_t i = 0; i < s_hid.field_count; ++i) {
            if (s_hid.fields[i].report_id == rid) {
                bits += (uint32_t)s_hid.fields[i].bit_size *
                        (uint32_t)s_hid.fields[i].count;
            }
        }
        const int extra = (int)body_len - (int)((bits + 7u) / 8u);
        ESP_LOGI(PAD_TAG, "hid path: rid=%u loose match, extra=%d bytes",
                 (unsigned)rid, extra);
    }

    bool covered[GB_BTN_COUNT];
    bool pressed[GB_BTN_COUNT];
    bool desc_live[PAD_BIND_COUNT];
    memset(covered, 0, sizeof(covered));
    memset(pressed, 0, sizeof(pressed));
    memset(desc_live, 0, sizeof(desc_live));

    // ---- 第一遍：按钮 ----
    for (uint16_t i = 0; i < s_hid.field_count; ++i) {
        const hid_field_t *f = &s_hid.fields[i];
        if (f->report_id != rid) {
            continue;                   // 别的 input report，与本包无关
        }
        if (f->kind != (uint8_t)HID_FIELD_BUTTON) {
            continue;                   // 轴 / hat / 填充位都不在这一遍处理
        }
        if ((f->main_flags & 0x01u) != 0u) {
            continue;                   // Constant 填充位，绝不当按键读
        }
        if (f->bit_size != 1u) {
            continue;                   // 非 1 位的按钮字段极其罕见，不猜
        }
        if (f->usage_max > f->usage_min) {
            // 范围用法（Usage Min..Usage Max）：第 k 个条目的按钮号是 min+k。
            for (uint16_t k = 0; k < f->count; ++k) {
                const uint32_t off = (uint32_t)f->bit_offset +
                                     (uint32_t)k * (uint32_t)f->bit_size;
                if (off > 0xFFFFu) {
                    break;
                }
                const uint32_t v = hid_extract_bits(body, body_len,
                                                    (uint16_t)off, f->bit_size);
                if (v == 0u) {
                    continue;
                }
                const uint16_t btn = (uint16_t)(f->usage_min + k);
                pad_desc_mark(covered, pressed, desc_live,
                              pad_desc_button_to_gb(btn),
                              pad_desc_button_to_bind(btn), true);
            }
        } else {
            // 单一用法：整个字段就是一个按钮。
            const uint32_t v = hid_extract_bits(body, body_len, f->bit_offset,
                                                f->bit_size);
            if (v != 0u) {
                pad_desc_mark(covered, pressed, desc_live,
                              pad_desc_button_to_gb(f->usage_min),
                              pad_desc_button_to_bind(f->usage_min), true);
            }
        }
    }

    // ---- 第二遍：hat ----
    //
    // 把 hat 的原始值归一化成 dir：0..7 是方向（0=上，顺时针），8=居中，-1=没
    // 有 hat 字段。
    // 两族约定都要兼容（与 pad_apply_hat() 里那套是同一个问题，但这里可以用
    // 描述符里的 Logical Min/Max 直接判定，不用靠"有没有见过 15"去猜）：
    //   族 A：logical_min == 0 且 logical_max >= 8 → 0=居中，1..8 顺时针
    //   其余：logical_min..logical_max 顺时针，越界视为居中
    int  hat_dir = -1;
    bool hat_found = false;
    for (uint16_t i = 0; i < s_hid.field_count; ++i) {
        const hid_field_t *f = &s_hid.fields[i];
        if (f->report_id != rid) {
            continue;
        }
        if (f->kind != (uint8_t)HID_FIELD_HAT) {
            continue;
        }
        if ((f->main_flags & 0x01u) != 0u) {
            continue;
        }
        hat_found = true;
        const uint32_t raw = hid_extract_bits(body, body_len, f->bit_offset,
                                              f->bit_size);
        // ---- 校准锚点 ----
        //
        // 只数"值发生了变化"的转移边，不数报文条数。中心是所有方向的枢纽，
        // 度数必然最高；长按不动、以及按住方向的同时连点 A 都不产生新边，
        // 因此这两种最常见的玩法都污染不了结果（老版数报文条数，就是被
        // "按住右键连点 A"骗过去的）。
        if (raw < PAD_HAT_HIST_MAX) {
            if (s_hat_hist[raw] == 0u) {
                ++s_hat_distinct;
            }
            if (s_hat_hist[raw] < 0xFFFFu) {
                ++s_hat_hist[raw];
            }

            if (s_hat_prev_valid && s_hat_prev_raw != raw &&
                s_hat_prev_raw < PAD_HAT_HIST_MAX) {
                s_hat_degree[s_hat_prev_raw] |= (uint16_t)(1u << raw);
                s_hat_degree[raw] |= (uint16_t)(1u << s_hat_prev_raw);
                ++s_hat_transitions;
            }
            s_hat_prev_raw = raw;
            s_hat_prev_valid = true;

            if (!s_hat_have_anchor) {
                s_hat_have_anchor = true;
                s_hat_anchor_raw = raw;      // 首样本先顶上，别让首帧无锚点
                s_hat_first_ms = (uint32_t)(esp_timer_get_time() / 1000);
            } else if (!s_hat_frozen) {
                // 校准窗口内：每次都按当前证据重挑。hat_pick_anchor() 是确定性
                // 的，数据又只增不减，所以不会来回抖；而"度数必须严格更高才换"
                // 那种写法会把上面 2/3/4 三条并列裁决全挡掉 —— 用户只按过一个
                // 方向时恰好就是并列，锚点会一直锁在错的那个上。
                const int cand = hat_pick_anchor(f->logical_min,
                                                 f->logical_max);
                if (cand >= 0 && (uint32_t)cand != s_hat_anchor_raw) {
                    ESP_LOGI(PAD_TAG,
                             "hat anchor %u -> %u (deg %u cnt %u)",
                             (unsigned)s_hat_anchor_raw, (unsigned)cand,
                             (unsigned)__builtin_popcount(
                                 (unsigned)s_hat_degree[cand]),
                             (unsigned)s_hat_hist[cand]);
                    s_hat_anchor_raw = (uint32_t)cand;
                }
                const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
                const bool enough_edges =
                    s_hat_transitions >= PAD_HAT_CALIB_TRANSITIONS ||
                    (s_hat_transitions >= PAD_HAT_CALIB_MIN_TRANS &&
                     (now_ms - s_hat_first_ms) >= PAD_HAT_CALIB_WINDOW_MS);
                if (enough_edges) {
                    s_hat_frozen = true;
                    ESP_LOGI(PAD_TAG,
                             "hat anchor locked raw=%u deg=%u cnt=%u "
                             "(trans=%u, %u ms)",
                             (unsigned)s_hat_anchor_raw,
                             (unsigned)__builtin_popcount(
                                 (unsigned)s_hat_degree[s_hat_anchor_raw]),
                             (unsigned)s_hat_hist[s_hat_anchor_raw],
                             (unsigned)s_hat_transitions,
                             (unsigned)(now_ms - s_hat_first_ms));
                }
            }
            // ⚠ 冻结之后这里什么都不做。锚点是设备的编码约定，跟用户玩了多久、
            //   按了哪些键无关，运行期再自适应只会把"玩一会就错乱"再犯一遍。
        }

        int dir;
        if (raw == s_hat_anchor_raw) {
            // 与静止中位一致 ⇒ 没按方向。这一条是挡住"卡上/卡下"的主力：
            //   无论设备用哪一族，静止值本身都不能被当成方向。
            dir = 8;
        } else {
            const int64_t rv = (int64_t)raw;
            if (s_hat_anchor_raw == 0u) {
                // 静止值是 0 ⇒ 设备用族 A（0=居中，1..8 顺时针）。
                // & 7 把 raw==8（左上）折成 dir==7。
                dir = (int)((raw - 1u) & 7u);
            } else if (rv < (int64_t)f->logical_min ||
                       rv > (int64_t)f->logical_max) {
                // 越界 = 居中（族 B 的 null，常见 8 或 15）
                dir = 8;
            } else {
                // 静止值非 0 且在合法区间内 ⇒ 按描述符解：0=上，顺时针
                dir = (int)((raw - (uint32_t)f->logical_min) & 7u);
            }
        }

        if (!s_hat_anchor_logged && s_hat_have_anchor) {
            s_hat_anchor_logged = true;
            ESP_LOGI(PAD_TAG,
                     "hat anchor raw=%u (lmin=%d lmax=%d size=%u frozen=%d)",
                     (unsigned)s_hat_anchor_raw, (int)f->logical_min,
                     (int)f->logical_max, (unsigned)f->bit_size,
                     (int)s_hat_frozen);
        }
        if (dir != 8) {
            hat_dir = dir;              // 取第一个非居中的 hat
            break;
        }
    }

    // ⚠ 下面两个分支都要能走到，所以判据写成"hat 存在且可信" / "hat 存在但
    //   不可信" / "hat 不存在"，不能拿 hat_found = false 去复用轴兜底分支 ——
    //   偏移错的时候轴字节同样是垃圾，走轴兜底只是换个方式继续乱跳。
    const bool hat_trusted = hat_found &&
                             (s_hat_distinct <= PAD_HAT_MAX_DISTINCT);

    if (hat_found && !hat_trusted) {
        // 见到太多不同取值 ⇒ 这一位不是 hat。此时**方向键整体停用**：宁可没有
        // 方向键，也不能让它自己乱跳 —— 乱跳会直接顶掉用户正在做的操作
        // （选 ROM、打游戏），比缺一个方向键严重得多。
        if (!s_hat_distinct_logged) {
            s_hat_distinct_logged = true;
            ESP_LOGW(PAD_TAG,
                     "hat field saw %u distinct values (>%u), not a real hat; "
                     "dpad disabled (report/field offset is probably wrong)",
                     (unsigned)s_hat_distinct,
                     (unsigned)PAD_HAT_MAX_DISTINCT);
        }
    } else if (hat_trusted) {
        if (hat_dir >= 0 && hat_dir != 8) {
            pad_desc_mark(covered, pressed, desc_live, GB_BTN_UP,
                          (int)PAD_BIND_UP,
                          hat_dir == 0 || hat_dir == 1 || hat_dir == 7);
            pad_desc_mark(covered, pressed, desc_live, GB_BTN_RIGHT,
                          (int)PAD_BIND_RIGHT,
                          hat_dir == 1 || hat_dir == 2 || hat_dir == 3);
            pad_desc_mark(covered, pressed, desc_live, GB_BTN_DOWN,
                          (int)PAD_BIND_DOWN,
                          hat_dir == 3 || hat_dir == 4 || hat_dir == 5);
            pad_desc_mark(covered, pressed, desc_live, GB_BTN_LEFT,
                          (int)PAD_BIND_LEFT,
                          hat_dir == 5 || hat_dir == 6 || hat_dir == 7);
        }
        // dir == 8（居中）：什么都不写，保持已清零的状态。
    } else {
        // ---- 方向键兜底：只用轴 ----
        //
        // ⚠ 只在**没有任何 hat 字段**时启用。有 hat 却居中时绝不能退到轴：
        //   那样摇杆的一点点漂移就会覆盖掉用户"松开方向键"的意图。
        //
        // ⚠ 死区是这个函数的**核心**。25% 死区意味着 0..255 的轴在 128±31 以内
        //   一律不动。没有死区的话，静置在中位 0x80 的摇杆只要抖 1 LSB 就会被
        //   判成"往某个方向推" —— 这正是本次要消灭的幽灵按键。
        int32_t xv = 0, xm = 0, xd = 0;
        int32_t yv = 0, ym = 0, yd = 0;
        bool have_x = false;
        bool have_y = false;
        // 第一轮找 X(0x30)/Y(0x31)；找不到再退而求其次找 Rx(0x33)/Ry(0x34)
        // —— 有些手柄的十字键就是挂在右摇杆那组 usage 上的。
        for (int pass = 0; pass < 2 && !(have_x && have_y); ++pass) {
            const uint16_t want_x = (pass == 0) ? 0x30u : 0x33u;
            const uint16_t want_y = (pass == 0) ? 0x31u : 0x34u;
            for (uint16_t i = 0; i < s_hid.field_count; ++i) {
                const hid_field_t *f = &s_hid.fields[i];
                if (f->report_id != rid) {
                    continue;
                }
                if (!have_x &&
                    pad_desc_axis_value(body, body_len, f, want_x, &xv, &xm, &xd)) {
                    have_x = true;
                }
                if (!have_y &&
                    pad_desc_axis_value(body, body_len, f, want_y, &yv, &ym, &yd)) {
                    have_y = true;
                }
            }
        }
        if (have_x) {
            pad_desc_mark(covered, pressed, desc_live, GB_BTN_RIGHT,
                          (int)PAD_BIND_RIGHT, (xv - xm) > xd);
            pad_desc_mark(covered, pressed, desc_live, GB_BTN_LEFT,
                          (int)PAD_BIND_LEFT, (xv - xm) < -xd);
        }
        if (have_y) {
            pad_desc_mark(covered, pressed, desc_live, GB_BTN_DOWN,
                          (int)PAD_BIND_DOWN, (yv - ym) > yd);
            pad_desc_mark(covered, pressed, desc_live, GB_BTN_UP,
                          (int)PAD_BIND_UP, (yv - ym) < -yd);
        }
    }

    // ---- 统一写入 ----
    for (int b = 0; b < (int)GB_BTN_COUNT; ++b) {
        if (covered[b]) {
            pad_set((gb_button_t)b, pressed[b]);
        }
    }
    // 描述符推导的实时状态喂给绑定页：用户没绑任何键也能看到圈跟着亮。
    pad_bind_update_live_desc(desc_live);

    if (!s_hid_path_logged) {
        s_hid_path_logged = true;
        ESP_LOGI(PAD_TAG, "hid path: rid=%u body_len=%u",
                 (unsigned)rid, (unsigned)body_len);
    }
    return true;
}

// 「四轴居中」特征：剥 ID 之后的前 4 字节都落在 0x78..0x88（中位 0x80 ±8），
// 说明这是一个轴在前的布局。取 ±8 是为了容忍中位漂移和摇杆轻微噪声，
// 同时不会把 0x00（通用布局的空按钮字节）或 0x01（Xbox flags）误判进来。
static bool pad_axes_first_signature(const uint8_t *b, uint16_t len)
{
    if (len < 6) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        const uint8_t v = b[i];
        if (v < 0x78 || v > 0x88) {
            return false;
        }
    }
    return true;
}

static bool pad_xbox_signature(const uint8_t *d, uint16_t len)
{
    // 只匹配 d[0]/d[1] 两个字节太弱：不含 report ID 的设备，只要第一个按钮
    // 字节恰好是 0x01、下一字节恰好是 0x01/0x14，就会误锁到 Xbox 布局，
    // 然后整场会话全错、日志还理直气壮地写着 "locked to mode 1"。
    // 加上长度门槛（Xbox BLE 裸报文 16 字节，含/不含 ID 都远大于 15），
    // DS4 的 10 字节精简报文和各类短报文会被自然排除。
    return len >= 15 && d[0] == 0x01 && (d[1] == 0x01 || d[1] == 0x14);
}

// ---------------------------------------------------------------- 原始报文转储
//
// 布局是猜出来的，猜错了只能靠原始字节复盘。所以这里打的是**剥 report ID
// 之前**的原始 data/len —— 一旦打了剥完之后的数据，事后就分不清到底是
// 偏移判错了还是布局猜错了。
#define PAD_RAW_DUMP_COUNT       64    // 连接初期无条件转储的包数
#define PAD_RAW_DUMP_THROTTLE_MS 120   // 之后「内容变化」包的最小打印间隔
#define PAD_RAW_MAX_BYTES        32    // 与 BLE_GAP_EVENT_NOTIFY_RX 里的 buf 一致

static uint8_t  s_last_dump[PAD_RAW_MAX_BYTES];
static uint16_t s_last_dump_len;
static uint32_t s_last_dump_ms;

static void pad_dump_raw(const uint8_t *d, uint16_t len)
{
    const uint16_t n = len < PAD_RAW_MAX_BYTES ? len
                                               : (uint16_t)PAD_RAW_MAX_BYTES;
    const bool changed = (n != s_last_dump_len) ||
                         (memcmp(d, s_last_dump, n) != 0);
    const bool in_window = (s_reports <= PAD_RAW_DUMP_COUNT);
    const uint32_t now = pad_now_ms();

    if (!in_window) {
        if (!changed) {
            return;                     // 内容没变，没什么可看的
        }
        // 摇杆是模拟量，静置时也在抖：不限流的话 100 包/秒会把日志刷爆，
        // 真正想看的按键边沿反而被噪音冲掉。
        if ((uint32_t)(now - s_last_dump_ms) < PAD_RAW_DUMP_THROTTLE_MS) {
            return;
        }
    }

    s_last_dump_ms = now;
    memcpy(s_last_dump, d, n);
    s_last_dump_len = n;

    // 按实际长度打印，不硬编码 10 —— 换一台手柄长度就变了。
    char hex[PAD_RAW_MAX_BYTES * 3 + 1];
    for (uint16_t i = 0; i < n; i++) {
        snprintf(hex + i * 3, 4, "%02x ", (unsigned)d[i]);
    }
    hex[n * 3] = '\0';
    ESP_LOGI(PAD_TAG, "raw #%u: %s", (unsigned)s_reports, hex);
}

// 最近一条原始报文的长度。绑定表的判据里有 `len == report_len` 这一条，
// 长度不符就等于绑定全废，所以现场排障时必须能同时看到"表里记的长度"和
// "现在实际收到的长度"。
static uint16_t s_last_report_len;

// 绑定命中计数：每个动作自开机以来被判定为"按下"的次数。
//
// 为什么要累计而不是只打瞬时日志：现场排查必须和用户操作在**同一时间窗口**
// 撞上才有效，而这是最难协调的一件事（5 分钟一轮的抓取里用户刚好没动手，
// 日志就完全空白，什么结论都得不到）。累计计数让"用户按过了"这件事变成
// 板上钉钉的历史事实，之后任何时候抓日志都能读出来。
static uint32_t s_bind_hits[PAD_BIND_COUNT];
// 报文长度与绑定表不符的报文数。pad_bind_pressed() 见长度不符直接 return
// false，所以这个是「绑定表当下其实是废的」的直接证据。
static uint32_t s_bind_len_bad;
// 开机以来出现过的最大按键位图。用来回答"到底有没有任何一个键被读到过"。
static uint8_t  s_mask_ever;

// 统计「当前报文长度是否和绑定表对不上」。只看一条就够：整张表是同一次会话
// 里捕获的，第一条样本的长度就能代表表里其它条目。
static void bind_len_account(uint16_t len)
{
    for (int a = 0; a < (int)PAD_BIND_COUNT; ++a) {
        const pad_bind_entry_t e = pad_bind_get((pad_bind_action_t)a);
        if (!e.valid) {
            continue;
        }
        if (e.report_len != len) {
            ++s_bind_len_bad;
        }
        return;
    }
}

static void pad_dump_bind_table(void);

// ---------------------------------------------------------------- 描述符解析
//
// ⚠ 解析时机是**读完 Report Map 立刻解析**，不是等到第一包报文。
//   原来挂在 pad_handle_report() 上的惰性解析在这台设备（Q36 for Android）上
//   永远不触发：它静置时一包报文都不发（实测 reports=0 持续 150 秒），于是
//   所有基于描述符的诊断（字段表 / 绑定表清洗）都看不到，问题只能靠猜。
//
// 两处调用点：
//   1. pad_on_report_map() 末尾（读完立刻解析）—— 主要路径；
//   2. pad_handle_report() 开头 —— 兜底。描述符读取比第一包报文晚时（GATT 读
//      是在订阅成功之后才发起的）由它补上，靠 s_hid_tried 去重。
static void pad_hid_try_parse(void)
{
    if (s_hid_tried || !s_map_done || s_map_len == 0u) {
        return;                         // 还没读完 / 读失败 / 本次已解析过
    }
    s_hid_tried = true;                 // 无论成功失败都只试一次

    s_hid.valid = hid_parse_report_map(s_map, s_map_len, &s_hid);
    ESP_LOGI(PAD_TAG, "hid map parsed: fields=%u has_id=%d",
             (unsigned)s_hid.field_count, (int)s_hid.has_report_id);

    // ---- 逐字段 ----
    //
    // ⚠ CONST 也必须打，而且 **flags 必须打**：0x42（Variable + Null state，
    //   即 Hat）与 0x01（Constant，即填充位）在 kind 上区分得出来，但只有
    //   flags 能证明"描述符确实是这么写的"。漏掉填充位的话，事后对账时无法
    //   确认那 4 位到底是"没解析出来"还是"本来就是填充"。
    for (uint16_t i = 0;
         i < s_hid.field_count && i < (uint16_t)HID_MAX_FIELDS; ++i) {
        const hid_field_t *f = &s_hid.fields[i];
        ESP_LOGI(PAD_TAG,
                 "hid field rid=%u kind=%d off=%u size=%u cnt=%u "
                 "page=0x%02X usage=%u..%u flags=0x%02X lmin=%d lmax=%d",
                 (unsigned)f->report_id, (int)f->kind,
                 (unsigned)f->bit_offset, (unsigned)f->bit_size,
                 (unsigned)f->count, (unsigned)f->usage_page,
                 (unsigned)f->usage_min, (unsigned)f->usage_max,
                 (unsigned)f->main_flags,
                 (int)f->logical_min, (int)f->logical_max);
    }

    // ---- 每个含 input 字段的 report 的位宽 ----
    //
    // 这一行决定了"通知到底该是几个字节"，也是判断"挑中了对不对 report"的
    // 直接证据：与 hid path 里 body_len 对不上就说明挑错了。
    {
        uint8_t  ids[16];
        uint32_t bits[16];
        uint8_t  n = 0;
        for (uint16_t i = 0;
             i < s_hid.field_count && i < (uint16_t)HID_MAX_FIELDS; ++i) {
            const hid_field_t *f = &s_hid.fields[i];
            uint8_t slot = 0xFF;
            for (uint8_t j = 0; j < n; ++j) {
                if (ids[j] == f->report_id) {
                    slot = j;
                    break;
                }
            }
            if (slot == 0xFF) {
                if (n >= (uint8_t)(sizeof(ids) / sizeof(ids[0]))) {
                    continue;           // 到上限了，剩下的不再统计（正常到不了）
                }
                slot = n;
                ids[slot] = f->report_id;
                bits[slot] = 0;
                ++n;
            }
            bits[slot] += (uint32_t)f->bit_size * (uint32_t)f->count;
        }
        for (uint8_t j = 0; j < n; ++j) {
            ESP_LOGI(PAD_TAG, "hid reports: id=%u bits=%u bytes=%u",
                     (unsigned)ids[j], (unsigned)bits[j],
                     (unsigned)((bits[j] + 7u) / 8u));
        }
    }

    // ---- 汇总：这三样决定了"能不能免绑定直接玩" ----
    {
        uint32_t buttons = 0;           // 按钮**位数**（不是字段数）
        bool has_hat = false;
        int  ax_x = -1;
        int  ax_y = -1;
        for (uint16_t i = 0;
             i < s_hid.field_count && i < (uint16_t)HID_MAX_FIELDS; ++i) {
            const hid_field_t *f = &s_hid.fields[i];
            if (f->kind == (uint8_t)HID_FIELD_BUTTON) {
                buttons += (uint32_t)f->bit_size * (uint32_t)f->count;
            } else if (f->kind == (uint8_t)HID_FIELD_HAT) {
                has_hat = true;
            } else if (f->kind == (uint8_t)HID_FIELD_AXIS) {
                // 按 usage **范围**判定：X/Y 常被合成一个 usage 0x30..0x31
                // 的字段，只看 usage_min 会漏掉 Y。
                const uint16_t lo = f->usage_min;
                const uint16_t hi = (f->usage_max >= lo) ? f->usage_max : lo;
                if (lo <= 0x30u && 0x30u <= hi) { ax_x = 0x30; }
                if (lo <= 0x31u && 0x31u <= hi) { ax_y = 0x31; }
            }
        }
        ESP_LOGI(PAD_TAG, "hid derived: buttons=%u hat=%s axX=%d axY=%d",
                 (unsigned)buttons, has_hat ? "yes" : "no", ax_x, ax_y);
    }

    // ---- 用描述符校验并清洗已保存的绑定表 ----
    //
    // 为什么必须做：用户手上那批绑定是**旧版捕获逻辑**记下来的 —— 升序取第一
    // 个变化的字节，而 Q36 的 byte0..3 恰好是摇杆轴。这些绑定活在 NVS 里，
    // 光改捕获逻辑救不了它们：不主动清掉，用户烧完新固件还是会看到按键乱跳。
    //
    // ⚠ 判定顺序（按"能不能当按键用"从宽到严）：
    //   BUTTON / HAT   → 保留（这两类才是按键）
    //   AXIS           → **作废**：模拟轴永远在抖，判定式会自己反复触发
    //   CONST          → **作废**：填充位不是按键，值恒为 0 或 1
    //   未知 / OTHER   → 保留并警告：描述符没覆盖到的字节不动它（宁可放过，
    //                    也不要因为解析不全就清掉用户手动绑好的键）
    // 每次连接只跑一次（本函数由 s_hid_tried 门闸保证）。
    if (s_hid.valid) {
        pad_bind_init();                // 幂等：确保表已从 NVS 加载
        unsigned kept = 0;
        unsigned discarded = 0;
        for (int a = 0; a < (int)PAD_BIND_COUNT; ++a) {
            const pad_bind_entry_t e = pad_bind_get((pad_bind_action_t)a);
            if (!e.valid) {
                continue;
            }
            const char *nm = pad_bind_action_name((pad_bind_action_t)a);
            const hid_field_t *f = hid_field_at_raw_byte(&s_hid, e.byte_index);
            if (f == NULL) {
                ESP_LOGW(PAD_TAG, "bind check: %s byte=%u -> unknown, kept",
                         nm, (unsigned)e.byte_index);
                ++kept;
            } else if (f->kind == (uint8_t)HID_FIELD_BUTTON) {
                ESP_LOGI(PAD_TAG, "bind check: %s byte=%u -> BUTTON ok",
                         nm, (unsigned)e.byte_index);
                ++kept;
            } else if (f->kind == (uint8_t)HID_FIELD_HAT) {
                ESP_LOGI(PAD_TAG, "bind check: %s byte=%u -> HAT ok",
                         nm, (unsigned)e.byte_index);
                ++kept;
            } else if (f->kind == (uint8_t)HID_FIELD_AXIS) {
                ESP_LOGW(PAD_TAG,
                         "bind check: %s byte=%u -> AXIS, discarded "
                         "(analog axis can't be a button)",
                         nm, (unsigned)e.byte_index);
                pad_bind_clear((pad_bind_action_t)a);
                ++discarded;
            } else if (f->kind == (uint8_t)HID_FIELD_CONST) {
                ESP_LOGW(PAD_TAG,
                         "bind check: %s byte=%u -> PADDING, discarded",
                         nm, (unsigned)e.byte_index);
                pad_bind_clear((pad_bind_action_t)a);
                ++discarded;
            } else {
                ESP_LOGW(PAD_TAG, "bind check: %s byte=%u -> unknown, kept",
                         nm, (unsigned)e.byte_index);
                ++kept;
            }
        }
        ESP_LOGI(PAD_TAG, "bind check: %u kept, %u discarded",
                 kept, discarded);
    }

    // 每次连接都把完整绑定明细打一遍：上面清洗完了什么、留下了什么，一条日志
    // 链就能看全，不用再让用户复述"我绑过哪些键"。
    pad_dump_bind_table();
}

// 给 gameboy_pad_bind.c 用的取值接口（声明在 gameboy_pad_bind.h，那里只有
// 前置声明，靠的就是 hid_map_info_t 有结构体 tag）。
const hid_map_info_t *pad_hid_map(void)
{
    if (!s_hid.valid) {
        return NULL;                    // 没解析好 / 描述符读不到
    }
    return &s_hid;
}

static void pad_handle_report(const uint8_t *data, uint16_t len)
{
    if (!data || len < 2) {
        return;
    }
    s_last_report_len = len;
    s_reports++;
    s_empty_ready_count = 0;   // 报文真的来了，清掉「假连接」计数

    // 本次连接的第一条报文 = 这台设备真的工作，此刻才把它记进 NVS 供下次直连。
    // ⚠ 放在这里而不是下面「first report」那条日志附近：中间有一段绑定捕获的
    //   早退（WAIT/RELEASE 直接 return），一进绑定页就开始捕获的话，放后面会
    //   一次都存不下来。s_reports 每次连接都会清零，所以 ==1 就是"本次第一条"。
    if (s_reports == 1) {
        pad_peer_save(&s_attempt_addr);
    }

    // 捕获状态机必须看每一包原始报文（未剥 report ID 的 data/len）：绑定记下的
    // 字节下标就是相对这份原始数据的。放在解析之前，捕获期间也不会被跳过。
    pad_bind_on_raw_report(data, len);

    // 绑定页 UI 的按键闪烁：拿这一包原始报文把所有动作的实时状态算一遍。
    // 放在捕获早退**之前**：用户此刻正在按屏幕上选中那个框对应的物理键，画面
    // 上那个框应当跟着亮起来，这才算把反馈给到了。
    pad_bind_update_live(data, len);

    // 捕获进行中（WAIT/RELEASE）跳过正常解析：用户此刻正按着手柄上某个物理键
    // 来指认动作，半成品绑定 + 布局猜测会把这一串变化解析成"一直按着 A"之类
    // 的乱码喂给模拟器。
    // ⚠ 只跳 WAIT/RELEASE。DONE/FAIL 是终态，等 UI 读完后调 cancel 才回 IDLE，
    //   它们绝不能阻塞解析，否则一次失败的绑定会让摇杆永久失效。
    const pad_bind_capture_stage_t cap = pad_bind_capture_stage();
    if (cap == PAD_BIND_CAP_WAIT || cap == PAD_BIND_CAP_RELEASE) {
        pad_clear_buttons();
        // 捕获期间不走描述符路径（下面会早退），那份实时状态不会更新；不清的
        // 话上一帧亮着的圈会一直亮着，干扰用户指认"我现在按的是哪个键"。
        pad_bind_update_live_desc(NULL);
        return;
    }

    // 转储放在解析之前，用的就是原始 data/len，没有经过任何偏移处理。
    pad_dump_raw(data, len);

    // 先清零再解析：任何早退路径都不能让上一帧的按键残留。
    pad_clear_buttons();

    // 描述符（HID Report Map）解析。
    //
    // ⚠ 主路径在 pad_on_report_map() 末尾（读完立刻解析）。这里是**兜底**：
    //   GATT 读取是在订阅成功之后才发起的（见 pad_on_subscribe()），可能比第一
    //   包报文**晚**，那种情况下第一次报文到这儿时描述符还没读完，得靠这里补。
    //   函数内部由 s_hid_tried 门闸去重，无论成功失败都只解析一次。
    pad_hid_try_parse();

    // 描述符优先、启发式兜底。
    //
    // ⚠ 描述符读失败 / 解析不出字段时（s_hid_tried 已置位但 s_hid.valid 为假），
    //   这里会自然落回 else 分支走启发式 —— 所以下面的三态探测代码一个字都
    //   不能删。
    if (s_hid.valid) {
        if (pad_apply_descriptor(data, len)) {
            // 描述符路径命中，跳过启发式
        } else {
            // 描述符有效，但这一包对不上（换 report / 长度变了 / 来了个
            // Consumer 报之类）。
            //
            // ⚠ 这里绝对不能退回启发式。启发式是拿"报文看起来像什么"去套布局，
            //   拿一包 2 字节的 Consumer 报去套 10 字节的游戏杆布局，解出来全是
            //   垃圾位；更要命的是这类报文经常是"最后一包"，而 pad_clear_buttons()
            //   只在**收到报文时**才执行 —— 垃圾按键会一直挂到用户下次按键为止，
            //   表现就是"玩着玩着角色自己动起来"或者"按什么都没反应"。
            //   描述符在手的设备，认不出这一包就当没按，比瞎猜安全得多。
            pad_bind_update_live_desc(NULL);
        }
    } else {
        // report ID 三态探测。
        // ⚠ 判据不是「d[0] 变不变」，而是「d[0] 像不像一个 report ID」。
        //   旧判据（不变且非 0 → 有 ID）在「轴在前」的 pad 上必错：d[0] 是左摇杆
        //   X，按任何键都不变；采样窗口只有 PAD_ID_PROBE_COUNT 包 ≈ 48ms，用户
        //   根本来不及按键，varying 恒为 0；再叠加 d[0] = 0x80（摇杆中位）非 0，
        //   就判成「有 ID」，于是整场会话的偏移都错 1 字节，按键全废。
        //   report ID 是设备自己在 report map 里定义的 1 字节标签，实测设备几乎
        //   都取 1..8（上限放宽到 0x10 留余量）。0x80 这类值只可能是摇杆轴中位
        //   或按钮位，绝不可能是 report ID，所以显式排除 0x7C..0x84 这一段。
        if (s_id_state == PAD_ID_UNKNOWN) {
            if (s_id_probe == 0) {
                s_id_first = data[0];
            } else if (data[0] != s_id_first) {
                s_id_varying = true;
            }
            if (++s_id_probe >= PAD_ID_PROBE_COUNT) {
                const bool plausible_id =
                    (s_id_first >= 1 && s_id_first <= 0x10) &&
                    !(s_id_first >= 0x7C && s_id_first <= 0x84);
                s_id_state = (!s_id_varying && plausible_id) ? PAD_ID_PRESENT
                                                            : PAD_ID_ABSENT;
                ESP_LOGI(PAD_TAG,
                         "payload %s report id (first=0x%02x varying=%d)",
                         (s_id_state == PAD_ID_PRESENT) ? "HAS" : "NO",
                         s_id_first, (int)s_id_varying);
            }
        }

        // 未确定（PAD_ID_UNKNOWN）阶段按**无 ID** 处理：
        //  - HOGP 规范就是这么定的 —— 通知里的 payload 不带 report ID，ID 由
        //    Report Reference 描述符描述。「无 ID」才是正确的默认值。
        //  - 判错的代价也不对称：按「有 ID」判错，前 5 包偏移全错；按「无 ID」
        //    判错，同样只错前 5 包，但至少与规范一致的设备从第 1 包起就是对的。
        const bool strip_id = (s_id_state == PAD_ID_PRESENT);
        const uint8_t *body = strip_id ? (data + 1) : data;
        const uint16_t body_len = (uint16_t)(len - (strip_id ? 1 : 0));

        if (s_report_mode == 0) {
            // mode 3 的判据放在剥 ID 之后：四轴居中特征看的是 body[0..3]，
            // 若这台设备真带 report ID，不剥掉的话第一格是 ID 而不是 LX，
            // 特征就永远匹配不上。
            if (pad_axes_first_signature(body, body_len)) {
                s_report_mode = 3;
            } else {
                s_report_mode = pad_xbox_signature(data, len) ? 1 : 2;
            }
            ESP_LOGI(PAD_TAG, "report layout locked to mode %d (len=%u)",
                     s_report_mode, (unsigned)len);
        }

        if (s_report_mode == 1) {
            pad_apply_xbox_ble(body, body_len);
        } else if (s_report_mode == 3) {
            pad_apply_axes_first(body, body_len);
        } else {
            pad_apply_generic(body, body_len);
        }
    }   // ← 描述符路径未命中，启发式兜底结束

    // 上面三套 pad_apply_*() 全是"报文看起来像什么"的猜测，猜错一次整场会话
    // 按键全废。绑定表是用户亲手指认过的，权威性高于任何启发式布局猜测，
    // 所以在启发式结果之后覆盖一遍。
    // ⚠ 必须用**原始** data/len（未剥 report ID）：绑定记录的下标就是相对
    //   原始报文的。用 body 会在带 ID 的设备上整体偏 1 字节。
    {
        // GB 只有 8 个键，而可以绑定的动作有 10 个：X / Y 是 A / B 的「第二组
        // 位置」（见 gameboy_pad_bind.h 里枚举的说明），所以同一 GB 键可能被
        // 两个绑定指向。
        // ⚠ 必须先把结果收集起来再一次性 set，不能在循环里直接 pad_set()：
        //   pad_set() 是覆盖语义，同一键被写两次时后一条会把前一条抹掉，
        //   于是「只按 X」的结果会被后面那条 A 的判定冲掉。
        static const struct { uint8_t bind; uint8_t gb; } kBindMap[] = {
            { PAD_BIND_A, GB_BTN_A }, { PAD_BIND_B, GB_BTN_B },
            { PAD_BIND_X, GB_BTN_A }, { PAD_BIND_Y, GB_BTN_B },
            { PAD_BIND_UP, GB_BTN_UP }, { PAD_BIND_DOWN, GB_BTN_DOWN },
            { PAD_BIND_LEFT, GB_BTN_LEFT }, { PAD_BIND_RIGHT, GB_BTN_RIGHT },
            { PAD_BIND_START, GB_BTN_START }, { PAD_BIND_SELECT, GB_BTN_SELECT },
        };
        bool covered[GB_BTN_COUNT] = { false };
        bool pressed[GB_BTN_COUNT] = { false };
        for (size_t i = 0; i < sizeof(kBindMap) / sizeof(kBindMap[0]); ++i) {
            const pad_bind_action_t bind = (pad_bind_action_t)kBindMap[i].bind;
            if (!pad_bind_get(bind).valid) {
                continue;   // 没绑的动作保留启发式布局的结果，别乱动
            }
            const bool on = pad_bind_pressed(bind, data, len);
            if (on) {
                ++s_bind_hits[(int)bind];
            }
            covered[kBindMap[i].gb] = true;
            pressed[kBindMap[i].gb] = pressed[kBindMap[i].gb] || on;
        }
        for (int b = 0; b < (int)GB_BTN_COUNT; ++b) {
            if (covered[b]) {
                pad_set((gb_button_t)b, pressed[b]);
            }
        }
        bind_len_account(len);
    }

    // 首次收到报文时打印一次 raw，便于现场核对布局
    if (s_reports == 1) {
        char hex[3 * 20 + 1];
        int n = len < 20 ? len : 20;
        for (int i = 0; i < n; i++) {
            snprintf(hex + i * 3, 4, "%02X ", data[i]);
        }
        hex[n * 3] = '\0';
        ESP_LOGI(PAD_TAG, "first report: %s", hex);
        pad_dump_bind_table();
    }
}

// 绑定表现场核对：把每一条有效绑定和当前报文长度打出来。
//
// 「绑定完了但模拟器没反应」这类问题，99% 是这两类之一，日志里必须能一眼
// 分开——否则只能靠猜：
//   1) 表压根是空的（NVS 没写进去 / 覆盖了）：bound=0；
//   2) 表里有东西，但 report_len 与当前报文长度不符。pad_bind_pressed() 见到
//      长度不符直接 return false，等于绑定全废，而用户那边看到的是"绑过了"。
//      换过手柄模式、或捕获时脱剥 ID 的状态和现在不同，都会撞上这个。
static void pad_dump_bind_table(void)
{
    int valid_count = 0;
    for (int a = 0; a < (int)PAD_BIND_COUNT; ++a) {
        const pad_bind_entry_t e = pad_bind_get((pad_bind_action_t)a);
        if (!e.valid) {
            continue;
        }
        ++valid_count;
        ESP_LOGI(PAD_TAG,
                 "bind %-6s len=%u idx=%u mask=0x%02x expect=0x%02x",
                 pad_bind_action_name((pad_bind_action_t)a),
                 (unsigned)e.report_len, (unsigned)e.byte_index,
                 (unsigned)e.mask, (unsigned)e.expect);
    }
    ESP_LOGI(PAD_TAG, "bind table: %d/%d bound, current report len=%u",
             valid_count, (int)PAD_BIND_COUNT, (unsigned)s_last_report_len);
}

// ---------------------------------------------------------------- GATT 发现

static int pad_on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                      uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                      void *arg);
static int pad_on_chr(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_chr *chr, void *arg);
static int pad_on_svc(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_svc *svc, void *arg);
static void pad_start_discovery(void);
static int pad_on_subscribe(uint16_t conn, const struct ble_gatt_error *err,
                            struct ble_gatt_attr *attr, void *arg);
static int pad_on_report_map(uint16_t conn, const struct ble_gatt_error *err,
                             struct ble_gatt_attr *attr, void *arg);

static int pad_on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                      uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                      void *arg)
{
    (void)conn;
    (void)chr_val_handle;
    (void)arg;

    // ⚠ 不要用 `if (!err)` 判断"发现了一个描述符"。NimBLE 的
    // ble_gattc_error()（ble_gattc.c:1508）返回的是**静态结构体地址**，
    // err 永远非 NULL，所以 !err 恒为假——每一个被发现的描述符都会掉进
    // 下面的错误分支，把刚连上的手柄踢掉。正确判据是 err->status：
    //   status==0 且实体指针非空 = 发现了一个；status==BLE_HS_EDONE = 结束。
    if (err != NULL && err->status != 0 && err->status != BLE_HS_EDONE) {
        ESP_LOGW(PAD_TAG, "dsc discovery failed: %d", err->status);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (dsc != NULL) {
        if (ble_uuid_u16(&dsc->uuid.u) == UUID_CCCD) {
            s_cccd_handle = dsc->handle;
        }
        return 0;
    }

    // dsc == NULL 只出现在发现结束时。
    s_disc_active = false;

    if (s_cccd_handle == 0) {
        // 没扫到 CCCD 时的退化猜测。HID 标准布局是
        //   Report Value -> 0x2908 Report Reference -> 0x2902 CCCD
        // 所以正确偏移是 +2，不是 +1。原来写 +1 会在标准布局设备上
        // 写到 0x2908 上——GATT 写会"成功"，但永远收不到通知。
        uint16_t guess = (uint16_t)(s_report_val + 2);
        if (s_report_end != 0 && guess > s_report_end) {
            guess = (uint16_t)(s_report_val + 1);
        }
        s_cccd_handle = guess;
        ESP_LOGW(PAD_TAG, "CCCD not found, fallback to %u", s_cccd_handle);
    }

    // 写 CCCD = 0x0001（notify）
    uint8_t val[2] = { 0x01, 0x00 };
    int rc = ble_gattc_write_flat(conn, s_cccd_handle, val, sizeof(val),
                                  pad_on_subscribe, NULL);
    if (rc != 0) {
        ESP_LOGW(PAD_TAG, "cccd write failed: %d", rc);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

// ---------------------------------------------------------------- Report Map
//
// Report Map（0x2A4B）是 HID 设备自己申报的字段描述符集合：哪几位对应哪个
// Usage、是按钮还是轴、取值范围多少，全都写在里面。它是确定报文 bit 布局的
// 唯一权威依据 —— 靠「报文看起来像什么」去猜，猜错一次就要再连一次手柄。
// 本版只把它读回来打日志，解析规则由人看完日志后回来改 pad_apply_*()。
//
// 用 ble_gattc_read_long() 而不是 ble_gattc_read()：map 通常 50~200 字节，
// 而一次普通 ATT 读最多带回 (MTU-1) 字节，短 MTU 下会被截断。read_long 会用
// Read Blob Request 自动分段读完整个属性，并多次回调本函数（每次带回
// attr->offset + 一个分片），读完后回调一次 status=BLE_HS_EDONE、attr=NULL。
static int pad_on_report_map(uint16_t conn, const struct ble_gatt_error *err,
                             struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;

    // ⚠ 已断开、或这次回调属于上一次连接（conn 对不上）时，一个字节都别写。
    // GATT 过程是在 ble_hs 的 host 任务里异步回来的，断连之后它仍可能带着
    // 最后一包落地，那会把新连接的状态搅乱。
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || conn != s_conn) {
        return 0;
    }

    if (err == NULL) {
        return 0;
    }

    if (err->status != 0 && err->status != BLE_HS_EDONE) {
        // 读不到就算了：这只是诊断信息，绝不能因此踢掉一条正常工作的链路。
        ESP_LOGW(PAD_TAG, "report map read failed: %d", err->status);
        s_map_done = true;
        return 0;
    }

    if (attr != NULL && attr->om != NULL) {
        const uint16_t off = attr->offset;
        if (off < PAD_MAP_MAX) {
            uint16_t n = (uint16_t)OS_MBUF_PKTLEN(attr->om);
            if (n > PAD_MAP_MAX - off) {
                n = (uint16_t)(PAD_MAP_MAX - off);   // 超出上限的部分直接截断
            }
            if (n > 0 &&
                ble_hs_mbuf_to_flat(attr->om, &s_map[off], n, NULL) == 0) {
                if ((uint16_t)(off + n) > s_map_len) {
                    s_map_len = (uint16_t)(off + n);
                }
            }
        }
        return 0;                       // 还有后续分片，等下一次回调
    }

    // attr == NULL（配合 status==0 或 BLE_HS_EDONE）= 整个属性读完了。
    if (s_map_done) {
        return 0;                       // 收尾只做一次
    }
    s_map_done = true;

    ESP_LOGI(PAD_TAG, "report map len=%u", (unsigned)s_map_len);
    for (uint16_t off = 0; off < s_map_len; off += PAD_MAP_LINE) {
        uint16_t n = (uint16_t)(s_map_len - off);
        if (n > PAD_MAP_LINE) {
            n = PAD_MAP_LINE;
        }
        char hex[PAD_MAP_LINE * 3 + 1];
        for (uint16_t i = 0; i < n; i++) {
            snprintf(hex + i * 3, 4, "%02x ", (unsigned)s_map[off + i]);
        }
        hex[n * 3] = '\0';
        ESP_LOGI(PAD_TAG, "map[%03u]: %s", (unsigned)off, hex);
    }

    // ⚠ 读完**立刻**解析，不等第一包报文。
    //   原因：Q36 for Android 这类设备静置时一包报文都不发（实测 reports=0 持续
    //   整整 150 秒），挂在 pad_handle_report() 上的惰性解析永远触发不了，于是
    //   字段表、绑定表清洗这些诊断一条都看不到。放在这里，连接后马上就能在日志
    //   里看到完整的字段布局。
    pad_hid_try_parse();
    return 0;
}

// CCCD 写完之后调用。读不到只打一条日志，不影响 HID ready 流程。
static void pad_read_report_map(void)
{
    if (s_report_map_handle == 0 || s_map_done ||
        s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;                         // 设备没暴露 0x2A4B，或本次已经读过了
    }
    const int rc = ble_gattc_read_long(s_conn, s_report_map_handle, 0,
                                       pad_on_report_map, NULL);
    if (rc != 0) {
        ESP_LOGW(PAD_TAG, "report map read start failed: %d", rc);
        s_map_done = true;
    }
}

static int pad_on_subscribe(uint16_t conn, const struct ble_gatt_error *err,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    if (err && err->status != 0) {
        ESP_LOGW(PAD_TAG, "subscribe failed: %d", err->status);
        return 0;
    }
    s_subscribed = true;
    s_state = ST_READY;
    s_ready_since_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(PAD_TAG, "HID ready: conn=%u report_val=%u cccd=%u notify=%d",
             conn, s_report_val, s_cccd_handle, (int)s_report_notifies);

    // 订阅成功后才读 Report Map：这时链路已经确定可用，而且读取失败也不会
    // 干扰已经就绪的 HID 通路（pad_on_report_map() 只打日志，不碰状态机）。
    pad_read_report_map();
    return 0;
}

static int pad_on_chr(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn;
    (void)arg;

    // ⚠ 同 pad_on_dsc()：err 永远非 NULL，不能用 !err 判断"发现了一个特征"。
    if (err != NULL && err->status != 0 && err->status != BLE_HS_EDONE) {
        ESP_LOGW(PAD_TAG, "chr discovery failed: %d", err->status);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (chr != NULL) {
        const uint16_t u = ble_uuid_u16(&chr->uuid.u);
        if (u == UUID_REPORT_MAP) {
            // Report Map 是唯一能权威确定字段 bit 布局的东西，见
            // pad_on_report_map()。这里只记句柄，真正读取在订阅成功之后。
            s_report_map_handle = chr->val_handle;
            ESP_LOGI(PAD_TAG, "report map chr: val=%u", s_report_map_handle);
            return 0;
        }
        if (u == UUID_REPORT) {
            // HID 服务里通常有 3 个以上 0x2A4D：Input（带 NOTIFY）、
            // Output（震动/LED，只有 WRITE/WRITE_NO_RSP）、Feature（校准）。
            // 规范不保证声明顺序，见谁选谁很可能选中 Output —— 那种情况
            // CCCD 仍能写成功、状态也会进 READY，但永远收不到 notify，
            // 表现就是「显示已连接、按什么键都没反应」。
            // 所以优先选带 NOTIFY 属性的那个，只有设备完全不报属性时才退回先到先得。
            const bool notifies =
                (chr->properties & BLE_GATT_CHR_PROP_NOTIFY) != 0;
            if (s_report_val == 0 || (notifies && !s_report_notifies)) {
                s_report_val = chr->val_handle;
                // 描述符扫描窗口不能越过 HID 服务末尾，否则会扫到隔壁服务。
                uint16_t end = (uint16_t)(chr->val_handle + PAD_CCCD_SCAN_SPAN);
                if (s_svc_end != 0 && end > s_svc_end) {
                    end = s_svc_end;
                }
                s_report_end = end;
                s_report_notifies = notifies;
                s_report_props = chr->properties;
                ESP_LOGI(PAD_TAG, "report chr: val=%u notify=%d props=0x%02x",
                         s_report_val, (int)notifies, chr->properties);
            }
            return 0;
        }
        return 0;
    }

    // chr == NULL 只出现在发现结束时（status == BLE_HS_EDONE）。
    s_disc_active = false;

    if (s_report_val == 0) {
        ESP_LOGW(PAD_TAG, "no input report characteristic");
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    s_cccd_handle = 0;
    int rc = ble_gattc_disc_all_dscs(s_conn, s_report_val, s_report_end, pad_on_dsc, NULL);
    if (rc != 0) {
        ESP_LOGW(PAD_TAG, "dsc discovery start failed: %d", rc);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int pad_on_svc(uint16_t conn, const struct ble_gatt_error *err,
                      const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn;
    (void)arg;

    // ⚠ 同 pad_on_dsc()：err 永远非 NULL，不能用 !err 判断"发现了一个服务"。
    // 这里一旦判错，现象是"连上了立刻被自己踢掉"——实测日志里就是
    // connected 之后紧跟一条 svc discovery failed: 0（那个 0 其实是成功）。
    if (err != NULL && err->status != 0 && err->status != BLE_HS_EDONE) {
        ESP_LOGW(PAD_TAG, "svc discovery failed: %d", err->status);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (svc != NULL) {
        if (s_svc_start == 0) {
            s_svc_start = svc->start_handle;
            s_svc_end = svc->end_handle;
            ESP_LOGI(PAD_TAG, "HID service: %u..%u", s_svc_start, s_svc_end);
        }
        return 0;
    }

    // svc == NULL 只出现在发现结束时（status == BLE_HS_EDONE）。
    s_disc_active = false;

    if (s_svc_start == 0) {
        ESP_LOGW(PAD_TAG, "no HID service on this device");
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    int rc = ble_gattc_disc_all_chrs(s_conn, s_svc_start, s_svc_end, pad_on_chr, NULL);
    if (rc != 0) {
        ESP_LOGW(PAD_TAG, "chr discovery start failed: %d", rc);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static void pad_start_discovery(void)
{
    // 一次连接只发起一次发现。ENC_CHANGE 回调和 pad_service_task() 里的兜底
    // 超时可能在同一轮里同时满足条件，重复发起会让 NimBLE 回 BLE_HS_EBUSY，
    // 并把 s_disc_active / 句柄搅成半完成状态，比不发起更难查。
    if (s_disc_started || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    s_disc_started = true;
    s_state = ST_DISCOVERING;
    s_svc_start = 0;
    s_svc_end = 0;
    s_report_val = 0;
    s_report_end = 0;
    s_report_notifies = false;
    s_report_props = 0;
    s_cccd_handle = 0;
    s_subscribed = false;

    ble_uuid16_t hid = BLE_UUID16_INIT(UUID_HID_SERVICE);
    int rc = ble_gattc_disc_svc_by_uuid(s_conn, &hid.u, pad_on_svc, NULL);
    if (rc != 0) {
        ESP_LOGW(PAD_TAG, "svc discovery start failed: %d", rc);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    s_disc_active = true;
}

// ---------------------------------------------------------------- GAP

static void pad_scan_start(void);
static bool pad_connect_to(const ble_addr_t *addr);   // 定义在 pad_gap_event 之前
static int  pad_gap_event(struct ble_gap_event *event, void *arg);
static bool pad_try_known_peer(void);
static void pad_begin_search(void);
// pad_now_ms() 的前向声明已移到「报文解析」一节（转储限流要用）；
// pad_avoid_addr() 同样定义在文件末尾，pad_gap_event 要用。
static void pad_avoid_addr(const ble_addr_t *addr);

static void pad_scan_start(void)
{
    if (s_stop_requested) {
        return;
    }
    struct ble_gap_disc_params dp = { 0 };
    dp.itvl = 0;
    dp.window = 0;
    dp.filter_policy = 0;
    dp.limited = 0;
    dp.passive = 0;
    dp.filter_duplicates = 1;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &dp, pad_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(PAD_TAG, "ble_gap_disc failed: %d", rc);
        s_state = ST_ERROR;
        return;
    }
    s_state = ST_SCANNING;
    ESP_LOGI(PAD_TAG, "scanning for BLE HID gamepad");
}

// 先试记住的手柄，再退回广播扫描。
//
// ⚠ 直连和扫描不是二选一的优化，而是两条完全不同的路径：ble_gap_connect()
//   会让 controller 针对这个地址主动做连接建立，不需要主机先开 ble_gap_disc()，
//   所以手柄只要在"可被回连"的广播状态（哪怕间隔很慢）就能接上；扫描路径则
//   要等它恰好落在扫描窗口里被收到，慢广播的手柄可能等十几秒。
static void pad_begin_search(void)
{
    if (pad_try_known_peer()) {
        return;
    }
    pad_scan_start();
}

// 直连记住的手柄。返回 false 表示"没有可直连的目标"，调用方退回扫描。
static bool pad_try_known_peer(void)
{
    if (!s_peer_valid || s_stop_requested) {
        return false;
    }
    // 已经有链路就别再连：ble_gap_connect() 再发一次只会白占一个连接槽。
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }
    if (s_state == ST_CONNECTING || s_state == ST_DISCOVERING ||
        s_state == ST_READY) {
        return false;
    }
    // 刚直连失败过、且还在拉黑期内的，别马上再试：一轮是 9 秒超时 + 4 秒重扫，
    // 反复撞这个地址会把整个重连流程拖成几十秒。
    if (s_avoid_valid) {
        const uint32_t now = pad_now_ms();
        if ((uint32_t)(now - s_avoid_until_ms) < PAD_AVOID_AFTER_FAIL_MS &&
            memcmp(s_peer.val, s_attempt_addr.val, 6) == 0) {
            return false;
        }
    }
    // 本轮已经连着失败两次就放弃直连，退回扫描。这个出口必须有：手柄换了
    // RPA（可解析私有地址）之后旧地址永远连不上，没有上限就会每一轮都白等
    // 一个 9 秒超时，比直接扫描还慢。
    if (s_peer_fail >= PAD_PEER_FAIL_LIMIT) {
        return false;
    }

    s_peer_attempt = true;
    if (!pad_connect_to(&s_peer)) {
        return false;   // pad_connect_to() 内部已把它拉黑
    }
    ESP_LOGI(PAD_TAG,
             "reconnecting to known gamepad %02x:%02x:%02x:%02x:%02x:%02x",
             s_peer.val[5], s_peer.val[4], s_peer.val[3],
             s_peer.val[2], s_peer.val[1], s_peer.val[0]);
    return true;
}

static bool pad_adv_is_gamepad(const struct ble_gap_disc_desc *disc)
{
    const uint8_t *d = disc->data;
    const uint8_t len = disc->length_data;

    uint16_t appearance = 0;
    bool has_hid = false;

    uint8_t i = 0;
    while (i + 1 < len) {
        const uint8_t flen = d[i];
        if (flen == 0 || (uint16_t)(i + flen) >= (uint16_t)(len + 1)) {
            break;
        }
        const uint8_t type = d[i + 1];
        const uint8_t *val = &d[i + 2];
        const uint8_t vlen = (uint8_t)(flen - 1);

        if (type == 0x19 && vlen >= 2) {           // Appearance
            appearance = (uint16_t)(val[0] | (val[1] << 8));
        } else if (type == 0x03 && vlen >= 2) {    // 16-bit UUID 列表
            for (uint8_t k = 0; k + 1 < vlen; k += 2) {
                if ((uint16_t)(val[k] | (val[k + 1] << 8)) == UUID_HID_SERVICE) {
                    has_hid = true;
                }
            }
        } else if (type == 0x02 && vlen >= 2) {    // 不完整 16-bit UUID
            for (uint8_t k = 0; k + 1 < vlen; k += 2) {
                if ((uint16_t)(val[k] | (val[k + 1] << 8)) == UUID_HID_SERVICE) {
                    has_hid = true;
                }
            }
        }
        i = (uint8_t)(i + flen + 1);
    }

    // Appearance 0x03C0..0x03CF 是 HID 类设备（键盘/鼠标/手柄）
    const bool appearance_hid = appearance >= 0x03C0 && appearance <= 0x03CF;
    return has_hid || appearance_hid;
}

// 把广播里的设备名抠出来，找不到就返回 false。
// 现场最大的问题是「扫到一台设备但不知道它是什么」：日志里只有一个 MAC 和一个
// RSSI，没法判断这是用户的手柄还是隔壁的电视。有名字基本一眼就能认。
static bool pad_adv_name(const struct ble_gap_disc_desc *disc,
                         char *out, size_t out_bytes)
{
    if (!disc || !out || out_bytes == 0) {
        return false;
    }
    out[0] = '\0';
    const uint8_t *d = disc->data;
    const uint8_t len = disc->length_data;
    uint8_t i = 0;
    while (i + 1 < len) {
        const uint8_t flen = d[i];
        if (flen == 0 || (uint16_t)(i + flen) >= (uint16_t)(len + 1)) {
            break;
        }
        const uint8_t type = d[i + 1];
        const uint8_t *val = &d[i + 2];
        const uint8_t vlen = (uint8_t)(flen - 1);
        if (type == 0x08 || type == 0x09) {          // Shortened / Complete local name
            size_t n = vlen < (out_bytes - 1) ? vlen : (out_bytes - 1);
            for (size_t k = 0; k < n; ++k) {
                out[k] = (char)(val[k] >= 0x20 && val[k] < 0x7F ? val[k] : '?');
            }
            out[n] = '\0';
            return n > 0;
        }
        i = (uint8_t)(i + flen + 1);
    }
    return false;
}

// 对指定地址发起连接。扫描路径和"直连记住的手柄"共用这一份连接参数。
// ⚠ 必须先记下目标再发请求：GAP 的连接结果事件里不带地址，失败时只能靠
//   s_attempt_addr 去拉黑，否则下一轮还会选中同一个连不上的设备。
static bool pad_connect_to(const ble_addr_t *addr)
{
    struct ble_gap_conn_params cp = { 0 };
    cp.scan_itvl = 0x0010;
    cp.scan_window = 0x0010;
    cp.itvl_min = 24;      // 30ms，必须是 6..3200
    cp.itvl_max = 40;      // 50ms
    cp.latency = 0;        // 必须 < 500
    cp.supervision_timeout = 0x01F4;   // 5s
    cp.min_ce_len = 0;
    cp.max_ce_len = 0;

    s_attempt_addr = *addr;
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, addr, PAD_CONNECT_TIMEOUT_MS, &cp,
                             pad_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(PAD_TAG, "connect failed: %d", rc);
        pad_avoid_addr(addr);
        return false;
    }
    s_state = ST_CONNECTING;
    return true;
}

static int pad_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (!pad_adv_is_gamepad(&event->disc)) {
            return 0;
        }
        // 只连「可连接」的广播。SCAN_IND(2) 和 NONCONN_IND(3) 是纯广播包，
        // 对它们发连接请求必然等到超时 —— 一次就是 9 秒，之后再等 4 秒重扫。
        // 现场抓到的第一个候选就是这类，白等了一整轮。
        // 常量值（ADV_IND=0 / DIR_IND=1 / SCAN_IND=2 / NONCONN_IND=3 / SCAN_RSP=4）
        // 见 nimble/include/nimble/hci_common.h:2750。
        if (event->disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
            event->disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND) {
            ESP_LOGD(PAD_TAG, "skip non-connectable adv (type=%u)",
                     (unsigned)event->disc.event_type);
            return 0;
        }
        // 不依赖 NimBLE 的 addr 格式化宏（各版本在 util.h / ble_hs.h 之间飘），
        // 直接打印 6 字节小端地址，省掉版本差异带来的编译失败。
        const uint8_t *ad = event->disc.addr.val;
        char adv_name[PAD_NAME_LEN] = "";
        const bool has_name =
            pad_adv_name(&event->disc, adv_name, sizeof(adv_name));
        ESP_LOGI(PAD_TAG,
                 "candidate gamepad rssi=%d type=%u name=%s "
                 "addr=%02x:%02x:%02x:%02x:%02x:%02x",
                 (int)event->disc.rssi, (unsigned)event->disc.event_type,
                 has_name ? adv_name : "(none)",
                 ad[5], ad[4], ad[3], ad[2], ad[1], ad[0]);

        // 刚连失败过的、且还在拉黑期内的，直接跳过。
        if (s_avoid_valid) {
            const uint32_t now_ms = pad_now_ms();
            if ((uint32_t)(now_ms - s_avoid_until_ms) < PAD_AVOID_AFTER_FAIL_MS &&
                memcmp(event->disc.addr.val, s_attempt_addr.val, 6) == 0) {
                ESP_LOGW(PAD_TAG, "skipping %s: connect failed recently",
                         has_name ? adv_name : "(unnamed)");
                return 0;
            }
            s_avoid_valid = false;
        }

        // 全零/全 F 的地址**连不上**：对这种地址发连接请求，对端根本不会
        // 回应，结果就是白等一个 9 秒超时，再被拉黑 60 秒，一轮 13 秒。
        // 现场抓到过：手柄处于某种「只广播、不响应连接请求」的状态时，
        // controller 报上来的地址就是 00:00:00:00:00:00 —— 上一次还好好地
        // 报着 03:25:00:33:35:ac，重启手柄/重新进配对模式之后才恢复正常。
        // 这里显式拒绝，把 9 秒省下来继续扫别的候选。
        {
            bool zero = true;
            bool ones = true;
            for (int i = 0; i < 6; i++) {
                if (ad[i] != 0x00) { zero = false; }
                if (ad[i] != 0xFF) { ones = false; }
            }
            if (zero || ones) {
                ESP_LOGW(PAD_TAG, "skip unusable addr %02x:%02x:%02x:%02x:%02x:%02x",
                         ad[5], ad[4], ad[3], ad[2], ad[1], ad[0]);
                return 0;
            }
        }

        // 广播包原始内容转储：地址异常时只能靠它判断手柄到底在播什么
        // （AD flags / UUID / 名字 / 厂商自定义段）。上限 31 字节。
        {
            char hex[3 * 31 + 1];
            uint16_t n = event->disc.length_data;
            if (n > 31) { n = 31; }
            for (uint16_t i = 0; i < n; i++) {
                snprintf(hex + i * 3, 4, "%02x ", (unsigned)event->disc.data[i]);
            }
            hex[n * 3] = '\0';
            ESP_LOGI(PAD_TAG, "adv data: %s", hex);
        }

        if (ble_gap_disc_cancel() != 0) {
            ESP_LOGW(PAD_TAG, "disc cancel failed");
        }

        if (!pad_connect_to(&event->disc.addr)) {
            pad_scan_start();
            return 0;
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(PAD_TAG, "scan complete (reason=%d)", event->disc_complete.reason);
        if (s_state == ST_SCANNING && !s_stop_requested) {
            // 超时后重新扫
            pad_scan_start();
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(PAD_TAG, "connect status=%d (13=BLE_HS_ETIMEOUT)",
                     event->connect.status);
            // 直连记住的手柄失败：只在本轮放弃它，退回扫描，**不擦 NVS**。
            // 用户把手柄关了再开是最常见的操作，下次上电还应该记得这台设备；
            // 因为一次没连上就永久忘掉，等于把新增的重连功能又废掉了。
            if (s_peer_attempt) {
                s_peer_attempt = false;
                if (s_peer_fail < 0xFF) {
                    ++s_peer_fail;
                }
                if (s_peer_fail >= PAD_PEER_FAIL_LIMIT && s_peer_valid) {
                    s_peer_valid = false;
                    ESP_LOGW(PAD_TAG, "forget known gamepad after %u failed "
                             "reconnects, fall back to scan",
                             (unsigned)s_peer_fail);
                }
            }
            // 失败的目标进拉黑名单，避免下一轮扫描又选中它再空等 9 秒。
            pad_avoid_addr(&s_attempt_addr);
            s_state = ST_RETRY_WAIT;
            s_retry_at_ms = (uint64_t)(esp_timer_get_time() / 1000) + PAD_RETRY_DELAY_MS;
            return 0;
        }
        // 连上了就说明记住的地址有效（哪怕它是上一轮用过的旧 RPA），
        // 本轮的失败计数归零。
        s_peer_fail = 0;
        s_peer_attempt = false;
        s_conn = event->connect.conn_handle;
        s_conn_since_ms = pad_now_ms();
        pad_clear_buttons();
        s_report_mode = 0;
        // ⚠ 下面这一组是**本次连接**的发现状态，不是可以跨连接复用的缓存，
        // 每次连上都要清干净。少清 s_svc_start 这一个就够卡死整条链路：
        // ENC_CHANGE 回调和 pad_service_task() 里的配对兜底原本都以
        // `s_svc_start == 0` 当"还没发现过"的门闸，残留的 19 会把门闸永久关死，
        // 于是收不到 ENC_CHANGE 的连接永远停在 ST_CONNECTING —— 屏幕上就是
        // 一路"连接中"，而链路其实早就连上了。现在门闸改用 s_disc_started，
        // 但句柄本身仍然必须清：不清的话 pad_on_dsc() 的"没扫到 CCCD 就猜
        // 偏移"分支会拿到上一次连接的过期句柄。
        s_svc_start = 0;
        s_svc_end = 0;
        s_report_val = 0;
        s_report_end = 0;
        s_cccd_handle = 0;
        s_report_notifies = false;
        s_report_props = 0;
        s_disc_active = false;
        s_disc_started = false;
        s_subscribed = false;
        // 每次连接都必须清零：下面「READY 但零报文」的假连接探测靠 s_reports==0
        // 判定，而 s_reports 是累计量。不清零的话，只要这台机器成功收到过一条
        // 报文，之后任何一次重连（Playing 中途断线、切页面回来、换手柄）即使
        // 订阅错特征也永远触发不了兜底 —— 恰好是最需要它的场景会静默失效。
        s_reports = 0;
        s_empty_ready_count = 0;
        // report ID / hat 约定按连接重新探测：换手柄后格式可能完全不同。
        s_id_state = PAD_ID_UNKNOWN;
        s_id_probe = 0;
        s_id_varying = false;
        s_id_first = 0;
        s_hat_null15_seen = false;
        // hat 锚点也必须重来：静止中位是**每台设备实测**出来的，换手柄后
        // 沿用上一台的锚点等于把上一台的中位当这一台的方向。
        memset(s_hat_hist, 0, sizeof(s_hat_hist));
        memset(s_hat_degree, 0, sizeof(s_hat_degree));
        s_hat_have_anchor = false;
        s_hat_anchor_raw = 0;
        s_hat_anchor_logged = false;
        s_hat_prev_raw = 0;
        s_hat_prev_valid = false;
        s_hat_transitions = 0;
        s_hat_first_ms = 0;
        s_hat_frozen = false;
        s_hat_distinct = 0;
        s_hat_distinct_logged = false;
        // Report Map 相关：换一台设备 map 就完全不同，句柄和缓冲都必须重来。
        s_report_map_handle = 0;
        s_map_len = 0;
        s_map_done = false;
        // ⚠ 解析结果同样必须清：不清的话新设备会沿用上一台手柄的字段表，
        //   而 s_hid.valid 仍为真 —— 于是整场会话都在用错误的布局解析，
        //   启发式还被永久跳过（比完全不解析更糟，因为日志看起来"很正常"）。
        s_hid_tried = false;
        s_hid.valid = false;
        s_hid.field_count = 0;
        s_hid.has_report_id = false;
        s_hid_path_logged = false;
        s_hid_nofield_logged = false;
        // 原始报文转储：不复位的话，新连接的第一包会被判成"和上一台设备
        // 的最后一包内容相同"而不打印，恰好漏掉最该看的那一条。
        s_last_dump_len = 0;
        s_last_dump_ms = 0;
        memset(s_last_dump, 0, sizeof(s_last_dump));
        ESP_LOGI(PAD_TAG, "connected, handle=%u", s_conn);

        // ⚠ 必须先发起配对，再做服务发现。
        // BLE 的顺序是「先连链路，再配对」：连上只代表链路搭好了，设备要
        // 等主机发起配对（bonding）才认为"我归属这个主机"。实测 Q36（HID
        // Android 模式）就是如此——链路层连上、GATT 发现与 CCCD 订阅全部
        // 成功（HID ready），但只要主机不发起配对，它的**配对灯一直闪、
        // 一条 HID 报文都不发**，表现为「显示已连接、按键毫无反应」。
        // 之前这里直接 pad_start_discovery()，从没配对过，所以永远收不到数据。
        // 配对参数已在 pad_host_init() 配成 Just Works（NO_IO + bonding +
        // mitm=0），不需要用户交互。
        s_enc_done = false;
        s_sec_since_ms = pad_now_ms();
        int sec_rc = ble_gap_security_initiate(s_conn);
        if (sec_rc != 0) {
            // 设备不支持/不需要加密：直接走发现，别把整条链路卡死。
            ESP_LOGW(PAD_TAG, "security initiate failed: %d, discover anyway",
                     sec_rc);
            pad_start_discovery();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(PAD_TAG, "disconnected, reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_disc_active = false;
        s_disc_started = false;
        s_conn_since_ms = 0;
        s_enc_done = false;
        s_sec_since_ms = 0;
        s_name[0] = '\0';
        pad_clear_buttons();
        // 描述符推导的那份实时状态也要清：断线后不再有新报文喂它，留着会让
        // 绑定页上某个圈一直亮着，用户会误判成"按键卡住了"。
        pad_bind_update_live_desc(NULL);
        if (!s_stop_requested) {
            s_state = ST_RETRY_WAIT;
            s_retry_at_ms = (uint64_t)(esp_timer_get_time() / 1000) + PAD_RETRY_DELAY_MS;
        } else {
            s_state = ST_OFF;
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.om) {
            const uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t buf[32];
            uint16_t copy = len < sizeof(buf) ? len : sizeof(buf);
            if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, copy, NULL) == 0) {
                pad_handle_report(buf, copy);
            }
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        // 加密（配对）完成。HOGP 手柄普遍要求加密链路才会开始上报，
        // 所以等到这一步再做 GATT 发现——否则 CCCD 可能撞在加密完成之前写入。
        ESP_LOGI(PAD_TAG, "enc change status=%d", event->enc_change.status);
        if (event->enc_change.status == 0) {
            s_enc_done = true;
        } else {
            // 配对失败也要往下走发现。相当一部分手柄不加密照样上报报文，
            // 而把整条链路卡在"连接中"是**一定**坏的结果，不加密去发现只是
            // 「可能」坏（真要求加密的话 CCCD 写入会报 ATT 错误，日志看得见）。
            // 两者之间没什么好权衡的。
            ESP_LOGW(PAD_TAG, "pairing failed status=%d, discover anyway",
                     event->enc_change.status);
        }
        // 无论配对成功失败都往下走：s_disc_started 保证一次连接只发现一次。
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            pad_start_discovery();
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // Just Works（NO_IO）下不会走到需要用户输入 PIN 的流程。
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(PAD_TAG, "mtu=%d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(PAD_TAG, "conn updated itvl=%d", desc.conn_itvl);
        }
        return 0;

    default:
        return 0;
    }
}

// ---------------------------------------------------------------- host 生命周期

static void pad_on_sync(void)
{
    uint8_t addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &addr_type);
    if (rc != 0) {
        ESP_LOGE(PAD_TAG, "ble_hs_id_infer_auto failed: %d", rc);
        s_state = ST_ERROR;
        return;
    }
    s_host_synced = true;
    ESP_LOGI(PAD_TAG, "host synced, addr_type=%u", addr_type);
    pad_begin_search();
}

static void pad_on_reset(int reason)
{
    ESP_LOGW(PAD_TAG, "host reset, reason=%d", reason);
    s_host_synced = false;
    s_state = ST_RETRY_WAIT;
    s_retry_at_ms = (uint64_t)(esp_timer_get_time() / 1000) + PAD_RETRY_DELAY_MS;
}

static void pad_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool pad_start(void)
{
    if (s_state != ST_OFF && s_state != ST_ERROR) {
        s_stop_requested = false;
        return true;
    }

    s_stop_requested = false;

    // 用户重新进入游戏页 = 一次全新的尝试，把「假连接」计数清零。
    // 否则一次失败会把本页后续所有尝试都锁死在 ERROR。
    if (s_state == ST_OFF) {
        s_empty_ready_count = 0;
    }

    // NimBLE host 只初始化一次。离开游戏页只是停扫描/断连，
    // 再次进入时不能重复 nimble_port_init()，否则会返回 ESP_ERR_INVALID_STATE。
    if (!s_nimble_inited) {
        s_state = ST_STARTING;

        // 绑定表只加载一次（内部有 s_inited 幂等保护）。放在这里而不是每次
        // pad_start()：NVS 读盘没必要反复做，而且这是"只执行一次"的路径。
        pad_bind_init();
        // 上次用过的手柄地址，同样只读一次（nvs_flash_init() 由系统别处完成）。
        pad_peer_load();

        esp_err_t err = nimble_port_init();
        if (err != ESP_OK) {
            ESP_LOGE(PAD_TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
            s_state = ST_ERROR;
            return false;
        }

        ble_hs_cfg.sync_cb = pad_on_sync;
        ble_hs_cfg.reset_cb = pad_on_reset;
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

        // SMP：手柄里的 Xbox Series/One S 强制要求 LE Secure Connections 绑定，
        // 不配这几项会表现为「配对瞬间断开、且不留 bond、每次重连重复失败」。
        // 用 NO_IO（Just Works）兼容绝大多数消费级手柄；bonding 打开才能免重复配对。
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 0;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist =
            BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist =
            BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

        // 本机只做 Central，不对外提供任何 GATT 服务，
        // 因此不调 ble_svc_gap_init()，省掉 GAP service 的常驻 RAM。
        // bond 落 NVS（依赖 CONFIG_BT_NIMBLE_NVS_PERSIST=y）。
        ble_store_config_init();

        nimble_port_freertos_init(pad_host_task);
        s_nimble_inited = true;
        ESP_LOGI(PAD_TAG, "nimble host started");
        return true;
    }

    s_state = ST_SCANNING;
    if (s_host_synced) {
        pad_begin_search();
    }
    return true;
}

void pad_stop(void)
{
    // 栈初始化失败是"本次上电不再尝试"，不能被一次页面切换洗掉，
    // 否则服务任务会在 ST_ERROR 和 ST_OFF 之间来回抖动、反复刷日志。
    if (s_state == ST_ERROR) {
        return;
    }

    s_stop_requested = true;

    if (!s_nimble_inited) {
        s_state = ST_OFF;
        return;
    }

    if (s_state == ST_SCANNING) {
        ble_gap_disc_cancel();
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    gameboy_pad_clear_remote();
    s_remote_active = false;
    // s_conn 已经是 NONE 的话不会再有 DISCONNECT 事件，发现状态得在这里自己清。
    s_disc_active = false;
    s_disc_started = false;
    s_conn_since_ms = 0;
    s_state = ST_OFF;
    ESP_LOGI(PAD_TAG, "pad stopped");
}

// ---------------------------------------------------------------- 服务任务
//
// 一个 20ms 周期的小任务：按「游戏页是否可见」自动开关 BLE，
// 并把报文解析结果同步进 gameboy_input 的 remote 位图。
// 手柄输入不必逐帧刷新 —— 模拟器每帧读的是位图，20ms 更新足够跟手。

#define PAD_SERVICE_PERIOD_MS          20
#define PAD_SERVICE_VISIBLE_TIMEOUT_MS 1500
// 「可见」只用来判断用户在本页；真正关 BLE 要再等一段宽限。
// 只是扫着（没连上）时短一点，纯省电；链路已建立时长一点——见 pad_service_task()。
#define PAD_SERVICE_STOP_GRACE_IDLE_MS 3000
#define PAD_SERVICE_STOP_GRACE_LINK_MS 20000
#define PAD_SERVICE_TASK_STACK         4096
#define PAD_SERVICE_TASK_PRIORITY      2

// READY 之后多久收不到任何 HID 报文就判定是「假连接」。
// 典型成因：订阅到了 Output Report（只有 WRITE 没有 NOTIFY），
// 或者 CCCD 句柄猜错——两种情况 CCCD 都写成功、状态也进 READY，
// 但报文永远不会来。不在这个点主动断开的话，用户看到的就是
// 「面板显示已连接，按什么键都没反应」，且没有任何线索。
//
// ⚠ 窗口不能太短。相当多的手柄是**只在按键状态变化时才上报**，
// 静置时一个包都不发（实测 Q36「for Android」模式就是这样：
// HID ready 之后静置 3 秒零报文，按一下键立刻开始上报）。
// 原来取 3000ms 会把这类正常手柄误判成假连接——现象是
// 「连上 → 已连接 → 3 秒后断开 → 待重连」无限循环，看起来像
// 手柄不兼容，其实是判据太急。放宽到 15s，给用户按一下键的时间。
// 一旦收到第一条报文，s_reports 就不再为 0，探测自然停止。
#define PAD_READY_PROBE_MS             15000
#define PAD_EMPTY_READY_LIMIT          3

// 发起配对后等 ENC_CHANGE 的最长时间。超时就不再等，直接进入 GATT 发现。
// 见 pad_service_task() 里的配对兜底。
#define PAD_SEC_TIMEOUT_MS             2000

// 看门狗：链路连上之后，状态最多允许在 ST_CONNECTING 停留多久。
// 超过就直接往下走 GATT 发现，不管中间卡在哪一步。
//
// 这一条刻意**不依赖任何配对/发现相关的中间状态**。ENC_CHANGE 回调和下面
// PAD_SEC_TIMEOUT_MS 那条兜底各有各的触发条件，只要其中任何一个环节的静态
// 状态残留（历史上真实发生过：s_svc_start 残留把门闸永久关死），它们会集体
// 失效，而用户的观感是完全卡死在"连接中"、又没有任何日志。看门狗只认
// 「链路还活着 + 时间到了」这两个最不可能出错的事实。
#define PAD_CONNECT_GUARD_MS           4000

static uint32_t pad_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void pad_avoid_addr(const ble_addr_t *addr)
{
    if (!addr) {
        return;
    }
    s_attempt_addr = *addr;
    s_avoid_until_ms = pad_now_ms();
    s_avoid_valid = true;
}

// 「手柄的按键到底有没有被读到」——这是现场被问得最多、也最难自证的一件事。
// 两条日志分工：
//   - mask 变化即打：只要有 ANY 按键位变化就打一行。绑 key 之后按下去毫无
//     反应时，这一行能立刻区分「报文没来」和「报文来了但判据没命中」。
//   - 周期心跳：即使一个键都不按也要打，用来证明链路活着、以及 s_reports
//     还在涨。只按了但没反应的场景下，静默本身也是一种错误信号。
#define PAD_INPUT_HEARTBEAT_MS 3000
#define PAD_BIND_HITS_BEAT_MS  10000

static uint8_t  s_last_input_mask;
static uint32_t s_input_beat_ms;
static uint32_t s_bind_beat_ms;

static void pad_sync_remote(void)
{
    if (s_state != ST_READY) {
        if (s_remote_active) {
            gameboy_pad_clear_remote();
            s_remote_active = false;
        }
        return;
    }
    s_remote_active = true;
    for (int i = 0; i < (int)GB_BTN_COUNT; ++i) {
        gameboy_pad_set_remote((gb_button_t)i, pad_button((gb_button_t)i));
    }

    const uint32_t now = pad_now_ms();
    const uint8_t mask = gameboy_pad_pressed_mask();
    if (mask != 0) {
        s_mask_ever |= mask;
    }
    if (mask != s_last_input_mask) {
        ESP_LOGI(PAD_TAG,
                 "input mask 0x%02X (A=%d B=%d SEL=%d START=%d R=%d L=%d U=%d D=%d)",
                 (unsigned)mask,
                 (mask >> 0) & 1, (mask >> 1) & 1, (mask >> 2) & 1,
                 (mask >> 3) & 1, (mask >> 4) & 1, (mask >> 5) & 1,
                 (mask >> 6) & 1, (mask >> 7) & 1);
        s_last_input_mask = mask;
    }
    if ((uint32_t)(now - s_input_beat_ms) > PAD_INPUT_HEARTBEAT_MS) {
        s_input_beat_ms = now;
        ESP_LOGI(PAD_TAG,
                 "pad heartbeat: state=READY reports=%u mode=%d len=%u "
                 "mask=0x%02X hid=%d",
                 (unsigned)s_reports, s_report_mode,
                 (unsigned)s_last_report_len, (unsigned)mask,
                 s_hid.valid ? 1 : 0);
    }
    // 累计计数：即使此刻一个键都没按，也能看出"用户之前按过什么"。
    // 这一行是排查「按了没反应」的主力证据，见 s_bind_hits 的定义处说明。
    if ((uint32_t)(now - s_bind_beat_ms) > PAD_BIND_HITS_BEAT_MS) {
        s_bind_beat_ms = now;
        ESP_LOGI(PAD_TAG,
                 "bind hits: A=%u B=%u X=%u Y=%u UP=%u DOWN=%u LEFT=%u RIGHT=%u "
                 "START=%u SELECT=%u | len_mismatch=%u ever_mask=0x%02X",
                 (unsigned)s_bind_hits[PAD_BIND_A],
                 (unsigned)s_bind_hits[PAD_BIND_B],
                 (unsigned)s_bind_hits[PAD_BIND_X],
                 (unsigned)s_bind_hits[PAD_BIND_Y],
                 (unsigned)s_bind_hits[PAD_BIND_UP],
                 (unsigned)s_bind_hits[PAD_BIND_DOWN],
                 (unsigned)s_bind_hits[PAD_BIND_LEFT],
                 (unsigned)s_bind_hits[PAD_BIND_RIGHT],
                 (unsigned)s_bind_hits[PAD_BIND_START],
                 (unsigned)s_bind_hits[PAD_BIND_SELECT],
                 (unsigned)s_bind_len_bad, (unsigned)s_mask_ever);
    }
}

static void pad_service_task(void *arg)
{
    (void)arg;
    for (;;) {
        const uint32_t now = pad_now_ms();
        const bool visible = s_last_visible_ms != 0 &&
                             (uint32_t)(now - s_last_visible_ms) <
                                 PAD_SERVICE_VISIBLE_TIMEOUT_MS;

        if (visible) {
            // 可见性上升沿 = 用户重新进入本页，给一次全新的尝试机会：
            // 清掉上次「假连接」计数，并把 ERROR 复位，否则一次失败会把
            // 本页后续所有进入都锁死，只能重启机器。
            if (!s_was_visible) {
                s_empty_ready_count = 0;
                if (s_state == ST_ERROR) {
                    s_state = ST_OFF;
                }
            }
            if (s_state == ST_OFF) {
                (void)pad_start();
            }
        } else if (s_state != ST_OFF && s_state != ST_ERROR) {
            // ⚠ 关 BLE 必须比"可见性"宽松得多。
            // 进绑定页 / 切游戏列表时电子纸要整屏刷新，UI 任务会被阻塞 1~3 秒，
            // 这期间没人调 pad_service_notify_visible()。原来用 1500ms 的可见性
            // 窗口直接 pad_stop()，于是「一进绑定页就断连，再花十几秒重连」——
            // 现场日志里 `pad stopped` 紧跟 `disconnected reason=534` 就是它。
            // 现在改成"离开页面后宽限 N 毫秒"，且链路已建立时给更长宽限。
            const uint32_t invisible_ms = (s_last_visible_ms == 0)
                                              ? 0xFFFFFFFFu
                                              : (uint32_t)(now - s_last_visible_ms);
            const bool link_up = (s_conn != BLE_HS_CONN_HANDLE_NONE);
            const uint32_t grace = link_up ? PAD_SERVICE_STOP_GRACE_LINK_MS
                                           : PAD_SERVICE_STOP_GRACE_IDLE_MS;
            if (invisible_ms > grace) {
                pad_stop();
            }
        }
        s_was_visible = visible;

        // 配对兜底：发起 ble_gap_security_initiate() 之后，有些设备既不返回
        // 错误也永远不来 ENC_CHANGE（例如对端不支持加密、或压根不理这个请求）。
        // 不能把 GATT 发现一直压着——那样状态会永远停在"连接中"，屏幕上看到
        // 的就是卡住。超过 PAD_SEC_TIMEOUT_MS 就照常发现，最坏是回到未加密
        // 路径（CCCD 写入若真的要求加密会报 ATT 错误，日志里看得见）。
        if (!s_disc_started && s_sec_since_ms != 0 &&
            s_conn != BLE_HS_CONN_HANDLE_NONE &&
            (uint32_t)(now - s_sec_since_ms) > PAD_SEC_TIMEOUT_MS) {
            ESP_LOGW(PAD_TAG, "no ENC_CHANGE in %d ms, discover without pairing",
                     PAD_SEC_TIMEOUT_MS);
            s_sec_since_ms = 0;
            pad_start_discovery();
        }

        // 看门狗：见 PAD_CONNECT_GUARD_MS 的定义处说明。
        if (s_state == ST_CONNECTING && !s_disc_started &&
            s_conn != BLE_HS_CONN_HANDLE_NONE && s_conn_since_ms != 0 &&
            (uint32_t)(now - s_conn_since_ms) > PAD_CONNECT_GUARD_MS) {
            ESP_LOGW(PAD_TAG, "stuck in connecting for %d ms, force discovery",
                     PAD_CONNECT_GUARD_MS);
            pad_start_discovery();
        }

        // 「假连接」探测：进了 READY 却一条报文都没收到。
        // 连续撞满上限就停在 ERROR，避免无限重连刷日志；用户离开再进本页可重试。
        if (s_state == ST_READY && s_reports == 0 &&
            (uint32_t)(now - s_ready_since_ms) > PAD_READY_PROBE_MS) {
            ++s_empty_ready_count;
            ESP_LOGW(PAD_TAG,
                     "READY but no HID report in %d ms (attempt %u/%u), "
                     "report_val=%u cccd=%u notify=%d props=0x%02x",
                     PAD_READY_PROBE_MS, s_empty_ready_count,
                     PAD_EMPTY_READY_LIMIT, s_report_val, s_cccd_handle,
                     (int)s_report_notifies, s_report_props);
            if (s_empty_ready_count >= PAD_EMPTY_READY_LIMIT) {
                ESP_LOGE(PAD_TAG, "giving up on this gamepad for now");
                s_state = ST_ERROR;
                if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
                    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
                }
            } else {
                // 这条提示是给现场排查用的：Switch Pro / Joy-Con / DualSense
                // 必须先收一条 Output Report（含 subcommand / CRC32）才会开始
                // 上报，本版没实现这个握手。它们的特征值订阅是对的（notify=1），
                // 所以从订阅层面看不出问题，只有在日志里明说才能避免白查。
                ESP_LOGW(PAD_TAG,
                         "if this device is Switch Pro/Joy-Con or DualSense, it "
                         "needs an HID output-report handshake to start "
                         "streaming; not supported in this build");
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
                s_ready_since_ms = now;   // 断开前的观察窗口复位，别每 20ms 触发一次
            }
        }

        pad_poll();
        pad_sync_remote();
        vTaskDelay(pdMS_TO_TICKS(PAD_SERVICE_PERIOD_MS));
    }
}

void pad_service_ensure(void)
{
    if (s_service_task) {
        return;
    }
    if (xTaskCreate(pad_service_task,
                    "gameboy_pad",
                    PAD_SERVICE_TASK_STACK,
                    NULL,
                    PAD_SERVICE_TASK_PRIORITY,
                    &s_service_task) != pdPASS) {
        s_service_task = NULL;
        ESP_LOGE(PAD_TAG, "service task create failed");
    }
}

void pad_service_notify_visible(void)
{
    s_last_visible_ms = pad_now_ms();
}

// ---------------------------------------------------------------- 重连轮询
//
// 由游戏任务每帧调用（或 UI 任务周期调用），不做独立任务，省栈。

void pad_poll(void)
{
    if (s_state != ST_RETRY_WAIT) {
        return;
    }
    const uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
    if (now < s_retry_at_ms) {
        return;
    }
    if (!s_host_synced) {
        return;
    }
    if (s_stop_requested) {
        s_state = ST_OFF;
        return;
    }
    pad_begin_search();
}
