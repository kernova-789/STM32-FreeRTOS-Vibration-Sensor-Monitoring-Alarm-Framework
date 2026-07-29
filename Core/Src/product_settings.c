#include "product_settings.h"

#include "settings_store.h"
#include "string.h"

#define PRODUCT_SENSOR_SETTINGS_KEY_BASE UINT16_C(0x1000)
#define PRODUCT_SENSOR_SETTINGS_FLAG_AUTO_STOP UINT16_C(0x0001)

typedef struct {
  uint16_t version;
  uint16_t record_size;
  uint16_t minimum_centi_g[VIBRATION_AXIS_COUNT];
  uint16_t maximum_centi_g[VIBRATION_AXIS_COUNT];
  uint16_t trigger_period_ms;
  uint16_t trigger_count;
  uint16_t alarm_seconds;
  uint16_t flags;
  uint16_t sample_rate;
  uint16_t reserved[3];
} product_sensor_settings_record_t;

_Static_assert(sizeof(product_sensor_settings_record_t) == 32U,
               "Product settings Flash layout must remain compatible");
_Static_assert(sizeof(product_sensor_settings_record_t) <=
                   SETTINGS_STORE_MAX_VALUE_SIZE,
               "Product settings record exceeds Flash-store value limit");

static bool product_sensor_settings_key(communication_sensor_id_t sensor_id,
                                        settings_store_key_t *key);
static void product_sensor_settings_normalize(
    product_sensor_settings_t *settings);

void product_sensor_settings_defaults(product_sensor_settings_t *settings) {
  if (settings == NULL) {
    return;
  }

  memset(settings, 0, sizeof(*settings));
  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    settings->thresholds.maximum_centi_g[axis] = 1600U;
  }
  settings->trigger_period_ms = 1000U;
  settings->trigger_count = 10U;
  settings->alarm_seconds = 60U;
  settings->auto_stop_alarm = false;
  settings->sample_rate = VIBRATION_SENSOR_SAMPLE_RATE_533_34_HZ;
}

settings_store_status_t
product_sensor_settings_load(communication_sensor_id_t sensor_id,
                             product_sensor_settings_t *settings) {
  product_sensor_settings_record_t record;
  settings_store_key_t key;
  settings_store_status_t status;
  size_t record_size = 0U;

  if ((settings == NULL) || !product_sensor_settings_key(sensor_id, &key)) {
    return SETTINGS_STORE_STATUS_INVALID_ARGUMENT;
  }

  product_sensor_settings_defaults(settings);
  status = settings_store_read(key, &record, sizeof(record), &record_size);
  if (status == SETTINGS_STORE_STATUS_NOT_FOUND) {
    return SETTINGS_STORE_STATUS_OK;
  }
  if (status != SETTINGS_STORE_STATUS_OK) {
    return status;
  }
  if ((record_size != sizeof(record)) ||
      ((record.version != PRODUCT_SENSOR_SETTINGS_VERSION) &&
       (record.version != PRODUCT_SENSOR_SETTINGS_LEGACY_VERSION)) ||
      (record.record_size != sizeof(record))) {
    return SETTINGS_STORE_STATUS_CORRUPT;
  }

  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    settings->minimum_centi_g[axis] = record.minimum_centi_g[axis];
    settings->thresholds.maximum_centi_g[axis] =
        record.maximum_centi_g[axis];
  }
  settings->trigger_period_ms = record.trigger_period_ms;
  settings->trigger_count = record.trigger_count;
  settings->alarm_seconds = record.alarm_seconds;
  settings->auto_stop_alarm =
      (record.flags & PRODUCT_SENSOR_SETTINGS_FLAG_AUTO_STOP) != 0U;
  if (record.version == PRODUCT_SENSOR_SETTINGS_VERSION) {
    settings->sample_rate =
        (vibration_sensor_sample_rate_t)record.sample_rate;
  }
  product_sensor_settings_normalize(settings);
  return SETTINGS_STORE_STATUS_OK;
}

settings_store_status_t
product_sensor_settings_save(communication_sensor_id_t sensor_id,
                             const product_sensor_settings_t *settings) {
  product_sensor_settings_record_t record = {0};
  product_sensor_settings_t normalized;
  settings_store_key_t key;

  if ((settings == NULL) || !product_sensor_settings_key(sensor_id, &key)) {
    return SETTINGS_STORE_STATUS_INVALID_ARGUMENT;
  }

  normalized = *settings;
  product_sensor_settings_normalize(&normalized);
  record.version = PRODUCT_SENSOR_SETTINGS_VERSION;
  record.record_size = sizeof(record);
  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    record.minimum_centi_g[axis] = normalized.minimum_centi_g[axis];
    record.maximum_centi_g[axis] =
        normalized.thresholds.maximum_centi_g[axis];
  }
  record.trigger_period_ms = normalized.trigger_period_ms;
  record.trigger_count = normalized.trigger_count;
  record.alarm_seconds = normalized.alarm_seconds;
  if (normalized.auto_stop_alarm) {
    record.flags |= PRODUCT_SENSOR_SETTINGS_FLAG_AUTO_STOP;
  }
  record.sample_rate = normalized.sample_rate;

  return settings_store_write(key, &record, sizeof(record));
}

static bool product_sensor_settings_key(communication_sensor_id_t sensor_id,
                                        settings_store_key_t *key) {
  uint32_t key_value;

  if ((sensor_id == COMMUNICATION_SENSOR_ID_INVALID) || (key == NULL)) {
    return false;
  }
  key_value = (uint32_t)PRODUCT_SENSOR_SETTINGS_KEY_BASE + sensor_id;
  if (key_value >= UINT16_MAX) {
    return false;
  }
  *key = (settings_store_key_t)key_value;
  return true;
}

static void product_sensor_settings_normalize(
    product_sensor_settings_t *settings) {
  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    if (settings->thresholds.maximum_centi_g[axis] > 1600U) {
      settings->thresholds.maximum_centi_g[axis] = 1600U;
    }
    if (settings->minimum_centi_g[axis] >
        settings->thresholds.maximum_centi_g[axis]) {
      settings->minimum_centi_g[axis] =
          settings->thresholds.maximum_centi_g[axis];
    }
  }
  if (settings->trigger_period_ms == 0U) {
    settings->trigger_period_ms = 1U;
  }
  if (settings->trigger_count == 0U) {
    settings->trigger_count = 1U;
  }
  if (settings->alarm_seconds == 0U) {
    settings->alarm_seconds = 1U;
  }
  if (!vibration_sensor_sample_rate_is_supported(settings->sample_rate)) {
    settings->sample_rate = VIBRATION_SENSOR_SAMPLE_RATE_533_34_HZ;
  }
}
