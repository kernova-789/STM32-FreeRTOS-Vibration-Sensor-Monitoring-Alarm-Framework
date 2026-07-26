#pragma once

#include "communication.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CAN_DISPLAY_OLD 所用 CAN 载荷的适配驱动：
 *   字节 1 = 0x01：修改阈值前先关闭传感器报警；
 *   字节 1 = 0x02：设置 X/Y/Z 三轴上限；
 *   字节 1 = 0x03：开启连续上报；
 *   字节 1 = 0x04：停止连续上报。
 * 实时数据格式为 {0, 0, X高, X低, Y高, Y低, Z高, Z低}。
 */
typedef struct {
  uint16_t full_scale_centi_g;
  uint8_t command_prefix;
} legacy_can_vibration_sensor_config_t;

extern const communication_sensor_driver_t
    legacy_can_vibration_sensor_driver;

#ifdef __cplusplus
}
#endif
