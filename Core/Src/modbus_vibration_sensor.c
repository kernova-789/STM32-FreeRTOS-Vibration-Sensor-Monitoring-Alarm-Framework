#include "modbus_vibration_sensor.h"

#include "vibration_sensor.h"

#define MODBUS_FUNCTION_READ_HOLDING_REGISTERS 0x03U
#define MODBUS_FUNCTION_WRITE_MULTIPLE_REGISTERS 0x10U

static communication_status_t
modbus_vibration_init(communication_device_t *device);
static communication_status_t modbus_vibration_ioctl(
    communication_device_t *device, communication_ioctl_command_t command,
    const void *argument, size_t argument_size, TickType_t timeout_ticks);
static communication_status_t
modbus_vibration_receive(communication_device_t *device,
                         const uint8_t *payload, size_t payload_size);

const communication_sensor_driver_t modbus_vibration_sensor_driver = {
    .name = "Modbus register vibration sensor",
    .init = modbus_vibration_init,
    .deinit = NULL,
    .ioctl = modbus_vibration_ioctl,
    .on_receive = modbus_vibration_receive,
};

static communication_status_t
modbus_vibration_init(communication_device_t *device) {
  const modbus_vibration_sensor_config_t *configuration =
      communication_device_driver_config(device);

  if ((configuration == NULL) ||
      (configuration->milli_g_per_sample_lsb == 0U)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }
  return COMMUNICATION_STATUS_OK;
}

static communication_status_t modbus_vibration_ioctl(
    communication_device_t *device, communication_ioctl_command_t command,
    const void *argument, size_t argument_size, TickType_t timeout_ticks) {
  const modbus_vibration_sensor_config_t *configuration =
      communication_device_driver_config(device);

  if (configuration == NULL) {
    return COMMUNICATION_STATUS_DRIVER_ERROR;
  }

  switch (command) {
  case VIBRATION_SENSOR_COMMAND_READ_SAMPLE: {
    uint8_t pdu[5] = {
        MODBUS_FUNCTION_READ_HOLDING_REGISTERS,
        (uint8_t)(configuration->sample_start_register >> 8U),
        (uint8_t)(configuration->sample_start_register & 0xFFU),
        0x00U,
        VIBRATION_AXIS_COUNT,
    };

    if ((argument != NULL) || (argument_size != 0U)) {
      return COMMUNICATION_STATUS_INVALID_ARGUMENT;
    }
    return communication_device_send(device, pdu, sizeof(pdu), timeout_ticks);
  }

  case VIBRATION_SENSOR_COMMAND_SET_THRESHOLDS: {
    const vibration_sensor_thresholds_t *thresholds =
        (const vibration_sensor_thresholds_t *)argument;
    uint8_t pdu[12] = {
        MODBUS_FUNCTION_WRITE_MULTIPLE_REGISTERS,
        (uint8_t)(configuration->threshold_start_register >> 8U),
        (uint8_t)(configuration->threshold_start_register & 0xFFU),
        0x00U,
        VIBRATION_AXIS_COUNT,
        (uint8_t)(VIBRATION_AXIS_COUNT * 2U),
    };

    if ((thresholds == NULL) || (argument_size != sizeof(*thresholds))) {
      return COMMUNICATION_STATUS_INVALID_ARGUMENT;
    }
    for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
      pdu[6U + (axis * 2U)] =
          (uint8_t)(thresholds->maximum_centi_g[axis] >> 8U);
      pdu[7U + (axis * 2U)] =
          (uint8_t)(thresholds->maximum_centi_g[axis] & 0xFFU);
    }
    return communication_device_send(device, pdu, sizeof(pdu), timeout_ticks);
  }

  case VIBRATION_SENSOR_COMMAND_SET_STREAMING:
    return COMMUNICATION_STATUS_UNSUPPORTED;

  default:
    return COMMUNICATION_STATUS_UNSUPPORTED;
  }
}

static communication_status_t
modbus_vibration_receive(communication_device_t *device,
                         const uint8_t *payload, size_t payload_size) {
  const modbus_vibration_sensor_config_t *configuration =
      communication_device_driver_config(device);

  if ((configuration == NULL) || (payload == NULL) || (payload_size < 2U)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  if ((payload[0] == MODBUS_FUNCTION_READ_HOLDING_REGISTERS) &&
      (payload[1] == (VIBRATION_AXIS_COUNT * 2U)) &&
      (payload_size == (size_t)(2U + (VIBRATION_AXIS_COUNT * 2U)))) {
    vibration_sensor_sample_t sample = {0};

    for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
      const uint16_t raw_unsigned =
          ((uint16_t)payload[2U + (axis * 2U)] << 8U) |
          payload[3U + (axis * 2U)];
      const int16_t raw = (int16_t)raw_unsigned;
      const uint32_t magnitude =
          (raw < 0) ? (uint32_t)(-(int32_t)raw) : (uint32_t)raw;
      uint32_t centi_g =
          (magnitude * configuration->milli_g_per_sample_lsb) / 10U;

      if (centi_g > UINT16_MAX) {
        centi_g = UINT16_MAX;
      }
      sample.signed_raw[axis] = raw;
      sample.magnitude_centi_g[axis] = (uint16_t)centi_g;
    }
    return communication_device_publish(
        device, VIBRATION_SENSOR_EVENT_SAMPLE, &sample, sizeof(sample));
  }

  if (payload_size <= COMMUNICATION_EVENT_DATA_CAPACITY) {
    return communication_device_publish(device, VIBRATION_SENSOR_EVENT_RAW_FRAME,
                                        payload, payload_size);
  }
  return COMMUNICATION_STATUS_UNSUPPORTED;
}
