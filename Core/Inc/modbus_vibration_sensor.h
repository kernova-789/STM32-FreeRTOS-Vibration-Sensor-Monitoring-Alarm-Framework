#pragma once

#include "communication.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Modbus 振动传感器示例协议：使用功能码 0x03 读取三个有符号保持寄存器，
 * 使用功能码 0x10 写入三个阈值寄存器。
 * 寄存器表不同的传感器应实现自己的驱动，不应在 communication.c 中增加分支。
 */
typedef struct {
  uint16_t sample_start_register;
  uint16_t threshold_start_register;
  uint16_t milli_g_per_sample_lsb;
} modbus_vibration_sensor_config_t;

extern const communication_sensor_driver_t modbus_vibration_sensor_driver;

#ifdef __cplusplus
}
#endif
