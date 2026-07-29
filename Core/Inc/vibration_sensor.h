#pragma once

#include "communication.h"
#include "stdbool.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIBRATION_SENSOR_DRIVER_CLASS UINT16_C(0x0001)

/*
 * 所有振动传感器实现共享的语义命令。
 * CAN 驱动和 Modbus 驱动可以把同一命令转换成完全不同的物理报文。
 */
typedef enum {
  VIBRATION_SENSOR_COMMAND_SET_THRESHOLDS =
      COMMUNICATION_CODE(VIBRATION_SENSOR_DRIVER_CLASS, 0x0001U),
  VIBRATION_SENSOR_COMMAND_SET_STREAMING =
      COMMUNICATION_CODE(VIBRATION_SENSOR_DRIVER_CLASS, 0x0002U),
  VIBRATION_SENSOR_COMMAND_READ_SAMPLE =
      COMMUNICATION_CODE(VIBRATION_SENSOR_DRIVER_CLASS, 0x0003U),
  VIBRATION_SENSOR_COMMAND_SET_SAMPLE_RATE =
      COMMUNICATION_CODE(VIBRATION_SENSOR_DRIVER_CLASS, 0x0004U),
} vibration_sensor_command_t;

typedef enum {
  VIBRATION_SENSOR_EVENT_SAMPLE =
      COMMUNICATION_CODE(VIBRATION_SENSOR_DRIVER_CLASS, 0x0001U),
  VIBRATION_SENSOR_EVENT_BOOTLOADER_REQUEST =
      COMMUNICATION_CODE(VIBRATION_SENSOR_DRIVER_CLASS, 0x0002U),
  VIBRATION_SENSOR_EVENT_RAW_FRAME =
      COMMUNICATION_CODE(VIBRATION_SENSOR_DRIVER_CLASS, 0x00FFU),
} vibration_sensor_event_type_t;

typedef enum {
  VIBRATION_AXIS_X = 0,
  VIBRATION_AXIS_Y,
  VIBRATION_AXIS_Z,
  VIBRATION_AXIS_COUNT,
} vibration_axis_t;

/* 物理量单位为 0.01 g，1600 表示 16.00 g。 */
typedef struct {
  uint16_t maximum_centi_g[VIBRATION_AXIS_COUNT];
} vibration_sensor_thresholds_t;

typedef struct {
  int16_t signed_raw[VIBRATION_AXIS_COUNT];
  uint16_t magnitude_centi_g[VIBRATION_AXIS_COUNT];
} vibration_sensor_sample_t;

/*
 * 采样率枚举值与传感器命令 01 05 00 NN 中的 NN 完全一致。
 * 0x07（8889 Hz）和 0x08（13333.5 Hz）有意不列入支持范围。
 */
typedef uint8_t vibration_sensor_sample_rate_t;
enum {
  VIBRATION_SENSOR_SAMPLE_RATE_533_34_HZ = 0U,
  VIBRATION_SENSOR_SAMPLE_RATE_888_90_HZ,
  VIBRATION_SENSOR_SAMPLE_RATE_1066_68_HZ,
  VIBRATION_SENSOR_SAMPLE_RATE_1333_35_HZ,
  VIBRATION_SENSOR_SAMPLE_RATE_2666_70_HZ,
  VIBRATION_SENSOR_SAMPLE_RATE_2963_00_HZ,
  VIBRATION_SENSOR_SAMPLE_RATE_5333_34_HZ,

  VIBRATION_SENSOR_SAMPLE_RATE_COUNT,
};

/*
 * 面向应用层的类型安全接口。函数先校验并封装参数，
 * 内部再通过 communication_ioctl() 分派到对应设备驱动。
 */
communication_status_t vibration_sensor_set_thresholds(
    communication_sensor_id_t sensor_id,
    const vibration_sensor_thresholds_t *thresholds,
    TickType_t timeout_ticks);
communication_status_t
vibration_sensor_set_streaming(communication_sensor_id_t sensor_id,
                               bool enabled, TickType_t timeout_ticks);
communication_status_t vibration_sensor_set_sample_rate(
    communication_sensor_id_t sensor_id,
    vibration_sensor_sample_rate_t sample_rate, TickType_t timeout_ticks);
communication_status_t
vibration_sensor_read_sample(communication_sensor_id_t sensor_id,
                             TickType_t timeout_ticks);

bool vibration_sensor_sample_rate_is_supported(
    vibration_sensor_sample_rate_t sample_rate);
uint32_t vibration_sensor_sample_rate_centi_hz(
    vibration_sensor_sample_rate_t sample_rate);

bool vibration_sensor_event_get_sample(const communication_event_t *event,
                                       vibration_sensor_sample_t *sample);
bool vibration_sensor_event_is_bootloader_request(
    const communication_event_t *event);

#ifdef __cplusplus
}
#endif
