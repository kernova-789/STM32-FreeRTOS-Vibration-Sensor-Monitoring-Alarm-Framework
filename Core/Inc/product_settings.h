#pragma once

#include "communication.h"
#include "settings_store.h"
#include "stdbool.h"
#include "stdint.h"
#include "vibration_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PRODUCT_SENSOR_SETTINGS_VERSION UINT16_C(2)
#define PRODUCT_SENSOR_SETTINGS_LEGACY_VERSION UINT16_C(1)

/*
 * 持久化配置以稳定的逻辑传感器 ID 为键，不在这里保存物理 CAN ID。
 */
typedef struct {
  uint16_t minimum_centi_g[VIBRATION_AXIS_COUNT];
  vibration_sensor_thresholds_t thresholds;
  uint16_t trigger_period_ms;
  uint16_t trigger_count;
  uint16_t alarm_seconds;
  bool auto_stop_alarm;
  vibration_sensor_sample_rate_t sample_rate;
} product_sensor_settings_t;

void product_sensor_settings_defaults(product_sensor_settings_t *settings);

/*
 * 没有已保存记录时，load() 返回默认配置和 OK。
 * 记录版本不受支持时，封装层返回 CORRUPT，防止按错误布局解释旧数据。
 */
settings_store_status_t
product_sensor_settings_load(communication_sensor_id_t sensor_id,
                             product_sensor_settings_t *settings);
settings_store_status_t
product_sensor_settings_save(communication_sensor_id_t sensor_id,
                             const product_sensor_settings_t *settings);

#ifdef __cplusplus
}
#endif
