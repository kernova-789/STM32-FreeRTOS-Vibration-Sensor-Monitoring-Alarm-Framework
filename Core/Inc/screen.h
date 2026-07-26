#pragma once

#include "FreeRTOS.h"
#include "stdbool.h"
#include "stdint.h"

typedef enum {
  SCREEN_AXIS_X = 0,
  SCREEN_AXIS_Y,
  SCREEN_AXIS_Z,

  SCREEN_AXIS_COUNT,
} screen_axis_t;

/* 加速度统一以 0.01 g 为单位，例如 123 表示 1.23 g。 */
typedef struct {
  uint16_t current_centi_g[SCREEN_AXIS_COUNT];
  uint16_t peak_centi_g[SCREEN_AXIS_COUNT];
  bool alarm_active;
} screen_measurements_t;

typedef struct {
  bool enabled;
  uint16_t timeout_seconds;
} screen_auto_off_settings_t;

typedef struct {
  uint16_t min_centi_g[SCREEN_AXIS_COUNT];
  uint16_t max_centi_g[SCREEN_AXIS_COUNT];
  uint16_t trigger_period_ms;
  uint16_t trigger_count;
  uint16_t alarm_seconds;
  bool auto_stop_alarm;
  screen_auto_off_settings_t auto_off;
} screen_settings_t;

typedef enum {
  /* 保存并下发事件中携带的配置。 */
  SCREEN_EVENT_SETTINGS_COMMITTED = 0,
  /* 清除事件中指定轴的历史峰值。 */
  SCREEN_EVENT_CLEAR_PEAK_REQUEST,
  /* 停止当前已经触发的报警。 */
  SCREEN_EVENT_ALARM_CANCEL_REQUEST,
} screen_event_type_t;

typedef struct {
  screen_event_type_t type;
  union {
    screen_settings_t settings;
    screen_axis_t axis;
  } data;
} screen_event_t;

/*
 * 启动屏幕任务。传入 NULL 时使用安全默认配置。
 * 应用启动过程中还必须调用一次 keyboard_init()。
 */
BaseType_t screen_init(const screen_settings_t *initial_settings);

/* 最新值输入；尚未显示的旧测量值会被新值覆盖。 */
BaseType_t
screen_update_measurements(const screen_measurements_t *measurements);

/* 替换界面显示的配置，例如从 Flash 加载配置后调用。 */
BaseType_t screen_set_settings(const screen_settings_t *settings,
                               TickType_t timeout_ticks);

/* 接收用户操作产生的请求，只能在任务上下文调用。 */
BaseType_t screen_receive_event(screen_event_t *event,
                                TickType_t timeout_ticks);

/* 复制屏幕模块最近接收或确认的配置。 */
BaseType_t screen_get_settings(screen_settings_t *settings);

/*
 * 异步打开或关闭 LCD 显示。关闭期间仍保留帧缓冲和界面状态。
 */
BaseType_t screen_set_enabled(bool enabled);
BaseType_t screen_turn_on(void);
BaseType_t screen_turn_off(void);

/* 只更新自动息屏配置，只能在任务上下文调用。 */
BaseType_t screen_set_auto_off(
    const screen_auto_off_settings_t *auto_off_settings,
    TickType_t timeout_ticks);
