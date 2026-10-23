// 统一维护 Game Boy 模拟器接入相关的几何、存储与调度常量。
// 所有推导都基于 RLCD 横屏面板的字节布局（见 PHASE2_SPEC.md §2.1）。
#pragma once

#include "app_display_config.h"

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"

namespace game_config {

// Game Boy 原始分辨率。
inline constexpr int kGbWidth = 160;
inline constexpr int kGbHeight = 144;

// 整数倍放大与居中窗口（面板坐标）。
inline constexpr int kScale = 2;
inline constexpr int kWindowWidth = kGbWidth * kScale;   // 320
inline constexpr int kWindowHeight = kGbHeight * kScale; // 288
inline constexpr int kWindowX = 40;                      // 左右各留 40px 黑边
inline constexpr int kWindowY = 0;                       // 下方留 12px 黑边

// 面板打包布局：index = byte_x * kPanelBytesPerColumn + block_y。
inline constexpr int kPanelByteColumns = kDisplayWidth / 2;     // 200
inline constexpr int kPanelBytesPerColumn = kDisplayHeight / 4; // 75

// GB 窗口覆盖的面板列区间与瓦片行区间。
inline constexpr int kFirstByteX = kWindowX / 2;                     // 20
inline constexpr int kLastByteX = kFirstByteX + kGbWidth - 1;        // 179
// 窗口下方剩余的像素高度折算成瓦片行：byte_y = block_y*4 覆盖面板 y = block_y*4..+3，
// 所以窗口底边落在 (kFirstBlockY + 72) * 4 = 300 处，正好触底。
// 这里显式减掉 kWindowY，否则 kWindowY 一改成非 4 的倍数，
// 「两行一批」的配对行就会跨到第三个面板字节上，画面会整体错位一格。
inline constexpr int kFirstBlockY =
    (kDisplayHeight - kWindowHeight - kWindowY) / 4;                 // 3
inline constexpr int kBlockRows = kWindowHeight / 4;                 // 72
inline constexpr int kRowPairs = kGbHeight / 2;                      // 72

// 亮度阈值：GB 的 2bit 灰度中 0/1 视为白（bit=1），2/3 视为黑（bit=0）。
// 只有 kDitherBinary 模式用它，另外两个抖动模式走下面的密度/刻度表。
inline constexpr uint8_t kWhiteShadeLimit = 2;

// ---- 1bpp 面板上的 4 级灰度模拟 ----
//
// GB 是 4 级灰度（shade 0=最白、3=最黑），而 RLCD 面板只有纯黑/纯白两色，
// 必须靠**空间抖动**用面积换灰度。
//
// ⚠ 电子纸绝对不能用**时间抖动**（逐帧交替图案）。整屏刷新要几百毫秒到 1 秒
//   以上，游戏帧率只有个位数，交替图案只会变成缓慢而刺眼的闪烁，还会成倍
//   放大残影。图案必须每帧固定，只用空间维度。
//
// 放大倍率固定 2x（kScale == 2），所以**每个 GB 像素恰好对应面板上 2x2 = 4
// 个物理像素**，正好够用"点亮其中几个"表示 4 级明暗。这是本机能做到的最
// 精细的灰度，再细就要靠扩大抖动周期（模式 2）用邻域疏密来模拟。
enum DitherMode : uint8_t {
    // 不做灰度：shade < 2 全白、否则全黑。改造前的行为，留作对照/回退。
    kDitherBinary = 0,
    // 每个 GB 像素用自己 2x2 块的子像素密度表示 4 级明暗。
    // 单个像素就能独立表达灰度（像素画的保真度最高），默认用它。
    kDitherSpatial2x2 = 1,
    // 在 160x144 原生分辨率上做 4x4 Bayer 有序抖动，再最近邻放大 2x。
    // 单个像素非黑即白，靠 4x4 邻域内的疏密表达明暗（纹理更细，但孤立像素
    // 的灰度会丢失）。参考 gambatte-k2（Kindle 电子纸上的 GB 模拟器）的做法。
    kDitherBayer4x4 = 2,
};
inline constexpr DitherMode kDitherMode = kDitherSpatial2x2;

// 4 级灰度分别点亮 2x2 块里的几个子像素（0..4），下标 = shade。
//
// ⚠ 不是均分的 4/3/2/1。DMG 真实调色板 #9bbc0f / #8bac0f / #306230 / #0f380f
//   的相对亮度约为 1.00 / 0.88 / 0.32 / 0.00 —— 两个亮色（shade 0/1）挨得很
//   近，两个暗色（shade 2/3）分得比较开。按亮度折算成 4.0 / 3.5 / 1.3 / 0.0，
//   取整即 {4, 3, 1, 0}。若改成均分的 {4, 3, 2, 1}，shade 0 与 1 的差距会被
//   凭空放大一倍，画面整体偏"高对比"，失去原本的柔和层次。
inline constexpr uint8_t kShadeDensity[4] = {4, 3, 1, 0};

// 模式 2（4x4 Bayer）用的亮度刻度，0..16 制。与上表同一套比例：
// {4,3,1,0} / 4 * 16 = {16, 12, 4, 0}。
inline constexpr uint8_t kShadeLevel[4] = {16, 12, 4, 0};

// ROM 存储：板载 TF（microSD）卡槽，SDMMC slot 0 / 1-bit 模式挂到 /sdcard，
// ROM 全部从 TF 卡根目录下的 rom/gb 里读。
inline constexpr const char *kSdCardMountPoint = "/sdcard";
// kRomsMountPoint 现在是**目录**而不是挂载点，且不带结尾 '/'：
// 调用方（rom_store 的 build_path / rom_upload 的 snprintf）各自负责补分隔符。
inline constexpr const char *kRomsMountPoint = "/sdcard/rom/gb";
// SDMMC 引脚（1-bit）：D1/D2 在原理图上未连接，width 必须为 1，
// 4-bit 在任何速率下都不可用。卡槽的 CD 脚接地、CS 未焊接，
// 所以既不能用 GPIO 判断插卡，也不能走 sdspi 模式。
inline constexpr gpio_num_t kSdClkGpio = GPIO_NUM_38;
inline constexpr gpio_num_t kSdCmdGpio = GPIO_NUM_21;
inline constexpr gpio_num_t kSdD0Gpio = GPIO_NUM_39;
inline constexpr int kMaxRoms = 64;
// TF 卡没有分区大小限制，上限只剩 PSRAM：8MB 里要同时放 ROM 与 LVGL/网络，
// 取 4MB。GB/GBC 卡带超过 4MB 的极罕见。
inline constexpr size_t kMaxRomBytes = 4 * 1024 * 1024;        // 单 ROM 上限
inline constexpr size_t kMaxUploadBytes = 1300 * 1024;      // 上传上限（413 阈值）
// LFN 走 UTF-8（CONFIG_FATFS_API_ENCODING_UTF_8），一个汉字 3 字节。
// "俄罗斯方块.gb" 就占 17 字节，稍长一点的中文名很容易顶到原来的 64 上限，
// 所以放宽到 96。
// 代价是 RomCatalog 双缓冲（PSRAM）从 2×(4+256+4096)≈8.7KB 涨到
// 2×(4+384+6144)≈13.1KB，见 RomCatalog 注释。
inline constexpr size_t kMaxRomFileNameBytes = 96;          // 含结尾 '\0'
// 显示层的行文本缓冲大小。rom_store 里存的就是 UTF-8 原始字节（≤95），
// 直接喂 LVGL 即可，不再有"GBK→UTF-8 膨胀 3 字节/字"那一层。
// 但仍然必须比 kMaxRomFileNameBytes 大：行文本要加 "N. " 前缀，
// 且 snprintf 需要空间放结尾 '\0'，复用它会把最长名字截断。
inline constexpr size_t kRomDisplayNameBytes = 176;         // 含结尾 '\0'
inline constexpr const char *kNvsNamespace = "gameboy";
// 列表会被 sort_names() 按文件名重排，所以「按下标记住选择」是错的：
// 上传或删除任意一个 ROM 都会让下标漂移到别的游戏上。
// 主键改为文件名（自描述、可自愈），旧的下标键只作迁移兜底。
inline constexpr const char *kNvsSelectedRomNameKey = "rom_name_v1";
inline constexpr const char *kNvsSelectedRomKey = "rom_sel_v1";
// 音量存「档」而不是百分比：UI 一行只画 5 个格子，回绕（5→0→1）和边界
// 判定都在 0..5 的整数域里做，百分比只是渲染/下发 codec 时的派生值。
inline constexpr const char *kNvsVolumeStepKey = "vol_step_v1";

// 卡带 RAM（电池存档）。
inline constexpr size_t kCartRamCapacity = 16 * 8 * 1024;   // 16 个 8KB bank
inline constexpr const char *kSaveFileSuffix = ".sav";

// 调度。
// 16384：Peanut-GB 的 gb_run_frame() 在 Xtensa 上最坏要吃掉 3KB+ 栈
// （CGB 双速 + HDMA 分支 + 嵌套回调），再叠加 game_fb_push_line() 的
// 瓦片写入与 esp_log 的 vprintf，8192 在深栈 ROM 上会直接溢出踩相邻任务。
inline constexpr uint32_t kTaskStackBytes = 16384;
// 注意：本工程的任务优先级刻度是 network=4 / ui=3 / button=2，
// 规格里的 6 会把 ui_task 与 button_task 直接饿死（游戏运行中按键无响应），
// 因此模拟器任务取 1：永远让位给按键、UI 与网络任务。
inline constexpr int kTaskPriority = 1;
inline constexpr int kTaskCoreId = 1;                       // 与 ui_task 同核

// Game Boy 一帧的真实时长：70224 cycle / 4194304 Hz = 16743us。
inline constexpr int64_t kFramePeriodUs = 16743;
// 推屏（15000 字节 @ 5MHz SPI ≈ 24ms）比一帧还长，必须按实测自适应跳帧，
// 否则模拟器会被推屏拖成慢放。区间为 [1, kMaxPresentStride] 帧推一次。
inline constexpr uint32_t kMinPresentStride = 1;
inline constexpr uint32_t kMaxPresentStride = 8;
// 落后实时超过该阈值就重置时间基准，避免累积“欠帧”导致后续疯狂快进。
inline constexpr int64_t kPaceResyncThresholdUs = 150 * 1000;
inline constexpr uint32_t kStatsLogIntervalMs = 5000;

static_assert(kScale == 2, "panel blit derivation assumes a 2x integer scale");
static_assert(kWindowWidth == kGbWidth * kScale &&
                  kWindowHeight == kGbHeight * kScale,
              "game window must cover the scaled GB framebuffer");
static_assert(kWindowWidth <= kDisplayWidth && kWindowHeight <= kDisplayHeight,
              "game window must fit inside the panel");
static_assert(kWindowX % 2 == 0, "game window x offset must align on a byte boundary");
static_assert(kFirstByteX * 2 == kWindowX,
              "first panel byte x must map back to the window x offset");
static_assert(kLastByteX + kFirstByteX + 1 == kPanelByteColumns,
              "game window must be horizontally centred on the panel");
static_assert((kDisplayHeight - kWindowHeight) % 4 == 0,
              "vertical border must be a whole number of panel byte rows");
// 上面那条只保证「上下边距之和」能被 4 整除，不足以保证 kWindowY 自己对齐。
// 真正的约束是：窗口上下两个边距都必须落在整瓦片行边界上，否则窗口底边
// 会切进某个面板字节中间，「一个字节 = 两行 GB 像素」的配对整个不成立。
// 现在 kFirstBlockY 的定义已经含 kWindowY，下面三条把这条不变式钉死。
static_assert((kDisplayHeight - kWindowHeight - kWindowY) % 4 == 0,
              "remainder below the window must be a whole number of panel byte rows");
static_assert(kWindowY % 4 == 0,
              "window y offset must align on a panel byte row boundary");
static_assert(kFirstBlockY ==
                  (kDisplayHeight - kWindowHeight - kWindowY) / 4,
              "first block row must be derived from the remaining height");
static_assert(kFirstBlockY + kBlockRows == kPanelBytesPerColumn,
              "game window must reach the last panel byte row");
static_assert(kBlockRows == kRowPairs,
              "one panel byte row covers exactly one GB row pair");
static_assert(kFirstBlockY == 3, "derived first block row must stay 3");
static_assert(kMaxRomBytes == 4 * 1024 * 1024, "ROM cap must fit in PSRAM alongside the running emulator");
static_assert(kMaxUploadBytes <= kMaxRomBytes, "upload cap must not exceed the ROM cap");
static_assert(kCartRamCapacity == 131072, "cart RAM capacity must cover 16 banks");
static_assert(kMinPresentStride >= 1 && kMaxPresentStride >= kMinPresentStride,
              "present stride bounds must be valid");
static_assert(kFramePeriodUs > 0 && kPaceResyncThresholdUs > kFramePeriodUs,
              "pace resync threshold must exceed one frame period");

} // namespace game_config
