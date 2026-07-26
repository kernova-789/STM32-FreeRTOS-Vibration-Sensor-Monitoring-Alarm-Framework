#include "legacy_can_vibration_sensor.h"

#include "string.h"
#include "vibration_sensor.h"

#define LEGACY_CAN_COMMAND_DISABLE_ALARM 0x01U
#define LEGACY_CAN_COMMAND_SET_THRESHOLDS 0x02U
#define LEGACY_CAN_COMMAND_START_STREAM 0x03U
#define LEGACY_CAN_COMMAND_STOP_STREAM 0x04U
#define LEGACY_CAN_BOOTLOADER_MARKER 0x06U

static communication_status_t
legacy_can_vibration_init(communication_device_t *device);
static communication_status_t legacy_can_vibration_ioctl(
    communication_device_t *device, communication_ioctl_command_t command,
    const void *argument, size_t argument_size, TickType_t timeout_ticks);
static communication_status_t
legacy_can_vibration_receive(communication_device_t *device,
                             const uint8_t *payload, size_t payload_size);
static uint16_t legacy_can_threshold_to_wire(
    uint16_t threshold_centi_g,
    const legacy_can_vibration_sensor_config_t *configuration);
static int16_t legacy_can_decode_offset_binary(uint16_t raw);
static uint16_t legacy_can_raw_to_centi_g(
    int16_t raw,
    const legacy_can_vibration_sensor_config_t *configuration);

const communication_sensor_driver_t legacy_can_vibration_sensor_driver = {
    .name = "legacy CAN vibration sensor",
    .init = legacy_can_vibration_init,
    .deinit = NULL,
    .ioctl = legacy_can_vibration_ioctl,
    .on_receive = legacy_can_vibration_receive,
};

static communication_status_t
legacy_can_vibration_init(communication_device_t *device) {
  const legacy_can_vibration_sensor_config_t *configuration =
      communication_device_driver_config(device);

  if ((configuration == NULL) ||
      (configuration->full_scale_centi_g == 0U)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }
  return COMMUNICATION_STATUS_OK;
}

static communication_status_t legacy_can_vibration_ioctl(
    communication_device_t *device, communication_ioctl_command_t command,
    const void *argument, size_t argument_size, TickType_t timeout_ticks) {
  const legacy_can_vibration_sensor_config_t *configuration =
      communication_device_driver_config(device);
  uint8_t frame[8] = {0};

  if (configuration == NULL) {
    return COMMUNICATION_STATUS_DRIVER_ERROR;
  }
  frame[0] = configuration->command_prefix;

  switch (command) {
  case VIBRATION_SENSOR_COMMAND_SET_THRESHOLDS: {
    const vibration_sensor_thresholds_t *thresholds =
        (const vibration_sensor_thresholds_t *)argument;
    communication_status_t status;

    if ((thresholds == NULL) || (argument_size != sizeof(*thresholds))) {
      return COMMUNICATION_STATUS_INVALID_ARGUMENT;
    }

    frame[1] = LEGACY_CAN_COMMAND_DISABLE_ALARM;
    status = communication_device_send(device, frame, sizeof(frame),
                                       timeout_ticks);
    if (status != COMMUNICATION_STATUS_OK) {
      return status;
    }

    frame[1] = LEGACY_CAN_COMMAND_SET_THRESHOLDS;
    for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
      const uint16_t wire_value = legacy_can_threshold_to_wire(
          thresholds->maximum_centi_g[axis], configuration);
      frame[2U + (axis * 2U)] = (uint8_t)(wire_value >> 8U);
      frame[3U + (axis * 2U)] = (uint8_t)(wire_value & 0xFFU);
    }
    return communication_device_send(device, frame, sizeof(frame),
                                     timeout_ticks);
  }

  case VIBRATION_SENSOR_COMMAND_SET_STREAMING: {
    const bool *enabled = (const bool *)argument;

    if ((enabled == NULL) || (argument_size != sizeof(*enabled))) {
      return COMMUNICATION_STATUS_INVALID_ARGUMENT;
    }
    frame[1] = *enabled ? LEGACY_CAN_COMMAND_START_STREAM
                        : LEGACY_CAN_COMMAND_STOP_STREAM;
    return communication_device_send(device, frame, sizeof(frame),
                                     timeout_ticks);
  }

  case VIBRATION_SENSOR_COMMAND_READ_SAMPLE:
    return COMMUNICATION_STATUS_UNSUPPORTED;

  default:
    return COMMUNICATION_STATUS_UNSUPPORTED;
  }
}

static communication_status_t
legacy_can_vibration_receive(communication_device_t *device,
                             const uint8_t *payload, size_t payload_size) {
  const legacy_can_vibration_sensor_config_t *configuration =
      communication_device_driver_config(device);

  if ((configuration == NULL) || (payload == NULL)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  if ((payload_size == 8U) && (payload[0] == 0U) && (payload[1] == 0U)) {
    vibration_sensor_sample_t sample = {0};

    for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
      const uint16_t wire_value =
          ((uint16_t)payload[2U + (axis * 2U)] << 8U) |
          payload[3U + (axis * 2U)];
      sample.signed_raw[axis] =
          legacy_can_decode_offset_binary(wire_value);
      sample.magnitude_centi_g[axis] = legacy_can_raw_to_centi_g(
          sample.signed_raw[axis], configuration);
    }
    return communication_device_publish(
        device, VIBRATION_SENSOR_EVENT_SAMPLE, &sample, sizeof(sample));
  }

  if ((payload_size >= 2U) &&
      (payload[1] == LEGACY_CAN_BOOTLOADER_MARKER)) {
    return communication_device_publish(
        device, VIBRATION_SENSOR_EVENT_BOOTLOADER_REQUEST, NULL, 0U);
  }

  if (payload_size <= COMMUNICATION_EVENT_DATA_CAPACITY) {
    return communication_device_publish(device, VIBRATION_SENSOR_EVENT_RAW_FRAME,
                                        payload, payload_size);
  }
  return COMMUNICATION_STATUS_UNSUPPORTED;
}

static uint16_t legacy_can_threshold_to_wire(
    uint16_t threshold_centi_g,
    const legacy_can_vibration_sensor_config_t *configuration) {
  uint32_t magnitude;

  if (threshold_centi_g >= configuration->full_scale_centi_g) {
    return UINT16_MAX;
  }

  magnitude =
      ((uint32_t)threshold_centi_g * UINT32_C(32767)) /
      configuration->full_scale_centi_g;
  return (uint16_t)(UINT32_C(32768) + magnitude);
}

static int16_t legacy_can_decode_offset_binary(uint16_t raw) {
  return (int16_t)((int32_t)raw - INT32_C(32768));
}

static uint16_t legacy_can_raw_to_centi_g(
    int16_t raw,
    const legacy_can_vibration_sensor_config_t *configuration) {
  uint32_t magnitude =
      (raw < 0) ? (uint32_t)(-(int32_t)raw) : (uint32_t)raw;
  uint32_t centi_g =
      (magnitude * configuration->full_scale_centi_g) / UINT32_C(32768);

  if (centi_g > UINT16_MAX) {
    centi_g = UINT16_MAX;
  }
  return (uint16_t)centi_g;
}
