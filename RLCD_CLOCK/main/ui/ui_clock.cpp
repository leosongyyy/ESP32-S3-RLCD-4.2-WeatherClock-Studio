// 构建和刷新天气时钟主页的时间、天气、温湿度和状态区域。
#include "ui_clock.h"
#include "ui_clock_header_objects.h"
#include "ui_clock_sensor_objects.h"
#include "ui_clock_surface_objects.h"
#include "ui_clock_weather_panel_objects.h"
#include "ui_clock_weather_text.h"

#include "app_constexpr.h"
#include "app_metadata.h"
#include "local_sensor_state.h"
#include "status_gif_contract.h"
#include "ui_battery.h"
#include "ui_bitmap.h"
#include "ui_canvas_primitives.h"
#include "ui_clock_layout.h"
#include "ui_fonts.h"
#include "ui_icons.h"
#include "ui_page_state.h"
#include "ui_progress.h"
#include "ui_setup_status.h"
#include "ui_status_gif.h"
#include "ui_time_calendar.h"
#include "ui_widgets.h"
#include "work_page_ids.h"

#include <esp_log.h>

namespace {
using namespace ui_clock_layout;

#define CLOCK_DATE_LABEL_CREATE_FAILED_LOG "clock date label create failed"
#define CLOCK_ALERT_PILL_CREATE_FAILED_LOG "clock alert pill create failed"
#define CLOCK_ALERT_ICON_CANVAS_CREATE_FAILED_LOG "clock alert icon canvas create failed"
#define CLOCK_ALERT_LABEL_CREATE_FAILED_LOG "clock alert label create failed"
#define CLOCK_STATUS_ICON_CANVAS_CREATE_FAILED_FORMAT "clock status icon canvas create failed x=%d"
#define CLOCK_LABEL_CREATE_FAILED_FORMAT "clock label create failed name=%s"
#define CLOCK_ICON_CANVAS_CREATE_FAILED_FORMAT "clock icon canvas create failed name=%s"
#define CLOCK_TREND_CANVAS_CREATE_FAILED_FORMAT "clock trend canvas create failed name=%s"
#define CLOCK_FILL_CANVAS_CREATE_FAILED_FORMAT "clock fill canvas create failed name=%s"
constexpr const char *kClockComponentFallbackName = "component";
constexpr const char *kClockComponentWeatherCity = "weather_city";
constexpr const char *kClockComponentWeatherIcon = "weather_icon";
constexpr const char *kClockComponentWeatherInfo = "weather_info";
constexpr const char *kClockComponentWeatherTemp = "weather_temp";
constexpr const char *kClockComponentWeatherHumidity = "weather_humi";
constexpr const char *kClockComponentTempIcon = "temp_icon";
constexpr const char *kClockComponentHumidityIcon = "humi_icon";
constexpr const char *kClockComponentTempValue = "temp_value";
constexpr const char *kClockComponentHumidityValue = "humi_value";
constexpr const char *kClockComponentTempTrend = "temp_trend";
constexpr const char *kClockComponentHumidityTrend = "humi_trend";
constexpr const char *kClockComponentTime = "time";
constexpr const char *kClockComponentSecond = "second";
constexpr const char *kClockComponentStatusGif = "status_gif";
constexpr const char *kClockComponentLowBatteryIcon = "low_battery_icon";
lv_color_t *s_low_battery_icon_canvas_buffer;

const char *clock_component_name(const char *name)
{
    return cstr_nonempty(name) ? name : kClockComponentFallbackName;
}

void build_clock_lower_icon(lv_obj_t *screen,
                            lv_obj_t **canvas,
                            lv_color_t **buffer,
                            int x,
                            int y,
                            int width,
                            int height,
                            int bytes_per_row,
                            const uint8_t *bits,
                            const char *name)
{
    if (!screen || !canvas || !buffer || !bits) {
        return;
    }
    if (!ensure_canvas_buffer(buffer, width, height)) {
        return;
    }
    *canvas = lv_canvas_create(screen);
    if (!*canvas) {
        ESP_LOGW(TAG, CLOCK_ICON_CANVAS_CREATE_FAILED_FORMAT, clock_component_name(name));
        return;
    }
    configure_canvas_base(*canvas, *buffer, x, y, width, height);
    draw_1bit_icon(*canvas, width, height, bytes_per_row, bits, lv_color_black(), lv_color_white());
}
} // namespace

void build_clock_ui()
{
    if (work_page_root(kWorkPageWeatherClock)) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    set_work_page_root(kWorkPageWeatherClock, screen);

    // 上游旧版天气时钟的 header / 天气栏 / 温湿栏 / DSEG 时间 / 分隔与进度条
    // 已全部下线，页面只保留三类内容。
    ClockSurfaceObjectRefs &surface = mutable_clock_surface_object_refs();
    build_clock_lower_icon(screen,
                           &surface.low_battery_icon_canvas,
                           &s_low_battery_icon_canvas_buffer,
                           kClockLowBatteryIconX,
                           kClockLowBatteryIconY,
                           LOW_BATTERY_ICON_WIDTH,
                           LOW_BATTERY_ICON_HEIGHT,
                           LOW_BATTERY_ICON_BYTES_PER_ROW,
                           low_battery_icon_bits,
                           kClockComponentLowBatteryIcon);
    if (surface.low_battery_icon_canvas) {
        lv_obj_add_flag(surface.low_battery_icon_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    build_setup_status_panel(screen);

    // 「时间与日历」整屏 canvas，即本页的全部日常 UI。
    build_time_calendar_page(screen);
}
