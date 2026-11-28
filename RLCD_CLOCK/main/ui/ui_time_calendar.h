// 声明天气时钟第一屏「时间与日历」版式的构建与重绘接口。
#pragma once

#include "lvgl.h"

// 在天气时钟页 root 上挂一张 400x300 的全屏 canvas 并画一次。
// 重复调用是幂等的，已建过就直接重画。
void build_time_calendar_page(lv_obj_t *parent);

// 重画整屏（进页面、跨分钟、跨天时调用）。
void time_calendar_redraw(const struct tm *local, int battery_percent);

// 每轮调一次：分钟、日期或电量变了才重画，避免无谓全刷。
void time_calendar_tick(const struct tm &local, int battery_percent);

// 天气时钟有两个子页：0=时间与日历，1=天气时钟第二页。
// 切换会立刻重画整屏；第二页目前是静态 UI，功能数据后续再接。
void time_calendar_set_page(int page);
void time_calendar_toggle_page(void);
int time_calendar_page(void);

// 低电量与配网页面要盖住这块内容，跟着既有可见性规则一起切。
void set_time_calendar_visible(bool visible);
bool time_calendar_visible(void);

// 页面销毁时释放 canvas 缓冲（自己删对象）。
void clear_time_calendar_page(void);

// 整屏被 lv_obj_clean() 清掉（开机结束那一步）时只丢引用：
// 对象已经被 LVGL 删了，这里绝不能再 lv_obj_del，否则二次释放。
void forget_time_calendar_page(void);
