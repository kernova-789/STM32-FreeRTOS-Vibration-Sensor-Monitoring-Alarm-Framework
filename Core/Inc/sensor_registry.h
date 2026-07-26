#pragma once

#include "communication.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 逻辑 ID 是应用层身份，不应随物理传感器 CAN ID 的改变而改变。
 */
typedef enum {
  SENSOR_ID_VIBRATION_PRIMARY = 1U,
  SENSOR_ID_VIBRATION_SECONDARY = 2U,
} product_sensor_id_t;

#define PRODUCT_PRIMARY_SENSOR_ID                                             \
  ((communication_sensor_id_t)SENSOR_ID_VIBRATION_PRIMARY)

/*
 * 配置表定义在 sensor_registry.c 中。
 * 这里是稳定逻辑 ID、实际 CAN ID 和传感器驱动之间唯一的映射位置。
 */
extern const communication_sensor_config_t product_sensor_table[];
extern const size_t product_sensor_count;

#ifdef __cplusplus
}
#endif
