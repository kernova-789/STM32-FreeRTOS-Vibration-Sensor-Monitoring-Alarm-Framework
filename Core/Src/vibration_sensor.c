#include "vibration_sensor.h"

#include "string.h"

static const uint32_t vibration_sensor_sample_rates_centi_hz
    [VIBRATION_SENSOR_SAMPLE_RATE_COUNT] = {
        UINT32_C(53334),  UINT32_C(88890),  UINT32_C(106668),
        UINT32_C(133335), UINT32_C(266670), UINT32_C(296300),
        UINT32_C(533334),
};

communication_status_t vibration_sensor_set_thresholds(
    communication_sensor_id_t sensor_id,
    const vibration_sensor_thresholds_t *thresholds,
    TickType_t timeout_ticks) {
  if (thresholds == NULL) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  return communication_ioctl(
      sensor_id, VIBRATION_SENSOR_COMMAND_SET_THRESHOLDS, thresholds,
      sizeof(*thresholds), timeout_ticks);
}

communication_status_t
vibration_sensor_set_streaming(communication_sensor_id_t sensor_id,
                               bool enabled, TickType_t timeout_ticks) {
  return communication_ioctl(
      sensor_id, VIBRATION_SENSOR_COMMAND_SET_STREAMING, &enabled,
      sizeof(enabled), timeout_ticks);
}

communication_status_t vibration_sensor_set_sample_rate(
    communication_sensor_id_t sensor_id,
    vibration_sensor_sample_rate_t sample_rate, TickType_t timeout_ticks) {
  if (!vibration_sensor_sample_rate_is_supported(sample_rate)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }
  return communication_ioctl(
      sensor_id, VIBRATION_SENSOR_COMMAND_SET_SAMPLE_RATE, &sample_rate,
      sizeof(sample_rate), timeout_ticks);
}

communication_status_t
vibration_sensor_read_sample(communication_sensor_id_t sensor_id,
                             TickType_t timeout_ticks) {
  return communication_ioctl(sensor_id, VIBRATION_SENSOR_COMMAND_READ_SAMPLE,
                             NULL, 0U, timeout_ticks);
}

bool vibration_sensor_sample_rate_is_supported(
    vibration_sensor_sample_rate_t sample_rate) {
  return sample_rate < VIBRATION_SENSOR_SAMPLE_RATE_COUNT;
}

uint32_t vibration_sensor_sample_rate_centi_hz(
    vibration_sensor_sample_rate_t sample_rate) {
  if (!vibration_sensor_sample_rate_is_supported(sample_rate)) {
    return 0U;
  }
  return vibration_sensor_sample_rates_centi_hz[sample_rate];
}

bool vibration_sensor_event_get_sample(const communication_event_t *event,
                                       vibration_sensor_sample_t *sample) {
  if ((event == NULL) || (sample == NULL) ||
      (event->type != VIBRATION_SENSOR_EVENT_SAMPLE) ||
      (event->data_size != sizeof(*sample))) {
    return false;
  }

  memcpy(sample, event->data, sizeof(*sample));
  return true;
}

bool vibration_sensor_event_is_bootloader_request(
    const communication_event_t *event) {
  return (event != NULL) &&
         (event->type == VIBRATION_SENSOR_EVENT_BOOTLOADER_REQUEST) &&
         (event->data_size == 0U);
}
