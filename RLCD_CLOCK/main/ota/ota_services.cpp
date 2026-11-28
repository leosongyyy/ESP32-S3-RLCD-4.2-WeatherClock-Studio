// OTA 在线更新已从本固件移除（v1.4 起）。
// 这里只保留仍被其它模块引用的轻量接口，避免改动散落在 UI/输入各处的守卫调用。
// 真正的下载、校验、manifest 解析代码已全部删除。
#include "ota_services.h"

#include "app_task_readiness.h"

#include <esp_log.h>
#include <esp_ota_ops.h>

bool ota_flow_active()
{
    // OTA 已移除，永远不会处于升级流程中。
    return false;
}

void ota_reset_status_if_idle()
{
    // 保留空实现，兼容尚未清理的旧调用点。
}

void ota_handle_info_key()
{
    // OTA 已移除，保留空入口避免旧调用点编译失败。
}

void ota_mark_running_app_valid()
{
    // 启动时确认当前分区有效。OTA 已移除，正常分区无需回滚，
    // 但保留这个检查可以让曾经 OTA 过来、仍处于 PENDING_VERIFY 的分区
    // 在本次启动被确认，避免之后意外回滚到旧版本。
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI("WeatherClock", "app marked valid");
        }
    }
}

void ota_task(void *)
{
    // OTA 已移除。仍占一个常驻任务位并立即上报 ready，
    // 保持 create_regular_app_tasks 的 kAllRegularAppTaskBits 轮询与开机健康检查不变。
    regular_app_task_mark_ready(RegularAppTaskId::kOta);
    vTaskDelete(nullptr);
}
