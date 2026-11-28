// 集中清理工作页、辅助页对象引用和对应绘制缓存。
#include "ui_object_refs.h"

#include "ui_aux_pages.h"
#include "ui_battery.h"
#include "ui_progress.h"
#include "ui_settings_page.h"
#include "ui_clock.h"
#include "ui_draw_cache.h"
#include "ui_page_state.h"
#include "ui_setup_status.h"
#include "ui_settings_ota_panel.h"
#include "ui_time_calendar.h"

namespace {
void clear_work_status_refs()
{
    clear_clock_header_object_refs();
    clear_work_status_label_refs();
    clear_work_status_icon_refs();
}

void clear_weather_clock_refs()
{
    clear_clock_local_sensor_object_refs();
    clear_clock_weather_panel_object_refs();
    clear_clock_surface_object_refs();
    clear_lower_panel_object_refs();
    clear_setup_status_object_refs();
}

void clear_ui_draw_cache_state()
{
    invalidate_clock_time_draw_cache();
    invalidate_clock_second_progress_draw_cache();
    invalidate_status_gif_draw_cache();
    invalidate_work_status_draw_cache();
}

}

void clear_clock_object_refs()
{
    clear_work_page_root_refs();
    clear_work_status_refs();
    clear_weather_clock_refs();
    clear_work_page_day_progress_refs();
    clear_work_page_battery_refs();
    // 整屏 canvas 跟着 page root 一起被 lv_obj_clean() 删掉了，
    // 这里必须丢掉引用，否则重建天气时钟页时不会重新建，新首页永远不出现。
    forget_time_calendar_page();
    clear_ui_draw_cache_state();
}

void clear_info_object_refs()
{
    clear_auxiliary_page_root_refs();
    clear_aux_page_object_refs();
    clear_settings_page_object_refs();
    clear_settings_ota_panel_object_refs();
}
