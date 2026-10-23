// 实现 GB 按键位图与 gb->direct.joypad 寄存器值换算。
#include "gameboy_input.h"

#include <atomic>

namespace {
// 位图采用 Peanut-GB 的 JOYPAD_* 位序（见 peanut_gb.h）：
//   bit0 A / bit1 B / bit2 SELECT / bit3 START
//   bit4 RIGHT / bit5 LEFT / bit6 UP / bit7 DOWN
// 注意 gb_button_t 的枚举顺序与这个位序并不相同（START/SELECT 互换、
// 方向键是反序），所以必须显式映射，不能用 1 << button。
constexpr uint8_t kJoypadA = 0x01;
constexpr uint8_t kJoypadB = 0x02;
constexpr uint8_t kJoypadSelect = 0x04;
constexpr uint8_t kJoypadStart = 0x08;
constexpr uint8_t kJoypadRight = 0x10;
constexpr uint8_t kJoypadLeft = 0x20;
constexpr uint8_t kJoypadUp = 0x40;
constexpr uint8_t kJoypadDown = 0x80;

// 1 = 按下。单核写、双核读，用原子量避免撕裂读。
// 本机按键与蓝牙手柄分开记账：手柄每帧刷新自己的位图，
// 若共用一张表，手柄"松开"会把手正按着的本机按键一起清掉。
std::atomic<uint8_t> s_local_mask{0};
std::atomic<uint8_t> s_remote_mask{0};

constexpr uint8_t joypad_bit(gb_button_t button)
{
    switch (button) {
    case GB_BTN_A:
        return kJoypadA;
    case GB_BTN_B:
        return kJoypadB;
    case GB_BTN_SELECT:
        return kJoypadSelect;
    case GB_BTN_START:
        return kJoypadStart;
    case GB_BTN_RIGHT:
        return kJoypadRight;
    case GB_BTN_LEFT:
        return kJoypadLeft;
    case GB_BTN_UP:
        return kJoypadUp;
    case GB_BTN_DOWN:
        return kJoypadDown;
    default:
        return 0;
    }
}

constexpr uint8_t kAllJoypadBits =
    kJoypadA | kJoypadB | kJoypadSelect | kJoypadStart |
    kJoypadRight | kJoypadLeft | kJoypadUp | kJoypadDown;

static_assert((uint8_t)GB_BTN_COUNT == 8, "GB button count must be eight");
static_assert(kAllJoypadBits == 0xFF, "JOYPAD bit map must cover all eight bits");
static_assert(joypad_bit(GB_BTN_A) == kJoypadA, "A must map to JOYPAD_A");
static_assert(joypad_bit(GB_BTN_START) == kJoypadStart,
              "START must map to JOYPAD_START");
static_assert(joypad_bit(GB_BTN_SELECT) == kJoypadSelect,
              "SELECT must map to JOYPAD_SELECT");
static_assert(joypad_bit(GB_BTN_UP) == kJoypadUp, "UP must map to JOYPAD_UP");
} // namespace

void gameboy_pad_set(gb_button_t button, bool pressed)
{
    const uint8_t bit = joypad_bit(button);
    if (bit == 0) {
        return;
    }
    if (pressed) {
        s_local_mask.fetch_or(bit);
    } else {
        s_local_mask.fetch_and((uint8_t)~bit);
    }
}

void gameboy_pad_set_remote(gb_button_t button, bool pressed)
{
    const uint8_t bit = joypad_bit(button);
    if (bit == 0) {
        return;
    }
    if (pressed) {
        s_remote_mask.fetch_or(bit);
    } else {
        s_remote_mask.fetch_and((uint8_t)~bit);
    }
}

void gameboy_pad_clear_remote(void)
{
    s_remote_mask.store(0);
}

bool gameboy_pad_connected(void)
{
    // READY 才代表 GATT 发现完成且 CCCD 已订阅，此时报文才真的会来。
    return pad_state() == PAD_STATE_READY;
}

void gameboy_pad_release_all(void)
{
    s_local_mask.store(0);
    s_remote_mask.store(0);
}

uint8_t gameboy_pad_pressed_mask(void)
{
    return (uint8_t)(s_local_mask.load() | s_remote_mask.load());
}

uint8_t gameboy_pad_joypad_register(void)
{
    // Peanut-GB 语义：gb_reset() 把 direct.joypad 置为 0xFF，
    // JOYP 读取时 `hram_io |= joypad`，硬件上 0 才是按下，因此这里取反。
    return (uint8_t)~(s_local_mask.load() | s_remote_mask.load());
}
