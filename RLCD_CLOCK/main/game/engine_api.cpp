// engine_api.h 的实现。目前只承载回调注入槽位，引擎 API 本体仍由各子模块实现。
#include "engine_api.h"

#include <esp_log.h>

namespace {
constexpr char kLogTag[] = "EngineApi";
engine_display_barrier_cb_t s_display_barrier_cb = nullptr;
}  // namespace

extern "C" void engine_api_set_display_barrier_cb(engine_display_barrier_cb_t cb)
{
    s_display_barrier_cb = cb;
    if (cb) {
        ESP_LOGI(kLogTag, "display barrier callback registered");
    }
}

extern "C" uint32_t engine_api_display_barrier_count(void)
{
    return s_display_barrier_cb ? s_display_barrier_cb() : 0U;
}
