#include "monitor_app.h"

#include "alarm_service.h"
#include "bootloader_ota.h"
#include "indicator.h"
#include "keyboard.h"
#include "product_settings.h"
#include "screen.h"
#include "sensor_registry.h"
#include "settings_store.h"
#include "string.h"
#include "task.h"
#include "vibration_sensor.h"

#define MONITOR_TASK_STACK_DEPTH (configMINIMAL_STACK_SIZE + 160U)
#define MONITOR_TASK_PRIORITY (tskIDLE_PRIORITY + 3U)
#define MONITOR_TASK_PERIOD_MS 2U
#define MONITOR_DISPLAY_PERIOD_MS 200U
#define MONITOR_COMMUNICATION_EVENTS_PER_CYCLE 32U
#define MONITOR_SENSOR_STARTUP_DELAY_MS 1200U
#define MONITOR_IOCTL_TIMEOUT_MS 50U
#define MONITOR_FLASH_SETTLE_DELAY_MS 2U
#define MONITOR_OTA_STREAM_STOP_SETTLE_DELAY_MS 10U
#define MONITOR_RECEIVE_INDICATOR_DURATION_MS 50U
#define MONITOR_NOTIFICATION_BOOTLOADER_REQUEST (UINT32_C(1) << 0U)

typedef struct {
  communication_sensor_id_t sensor_id;
  product_sensor_settings_t settings;
  screen_measurements_t measurements;
  alarm_service_t alarm;
  uint16_t display_window_max[VIBRATION_AXIS_COUNT];
  bool display_window_has_sample;
} monitor_sensor_context_t;

typedef struct {
  monitor_sensor_context_t sensors[COMMUNICATION_MAX_SENSOR_COUNT];
  size_t sensor_count;
  size_t displayed_sensor_index;
  TaskHandle_t task;
  TickType_t last_display_update_ticks;
  bool bootloader_request_pending;
} monitor_app_service_t;

static monitor_app_service_t monitor_service;

static void monitor_task(void *argument);
static void monitor_process_communication_events(void);
static void monitor_process_screen_events(void);
static void monitor_process_sample(monitor_sensor_context_t *sensor,
                                   const vibration_sensor_sample_t *sample,
                                   TickType_t timestamp_ticks);
static void monitor_poll_alarms(TickType_t current_ticks);
static void monitor_update_display(TickType_t current_ticks, bool force);
static bool monitor_apply_sensor_settings(
    const monitor_sensor_context_t *sensor);
static bool monitor_apply_all_sensor_settings(void);
static bool monitor_set_all_streaming(bool enabled);
static void monitor_update_alarm_outputs(void);
static void monitor_cancel_alarm(void);
static void monitor_clear_peak(screen_axis_t axis);
static void monitor_enter_bootloader(void);
static void monitor_set_status(bool healthy);
static monitor_sensor_context_t *
monitor_find_sensor(communication_sensor_id_t sensor_id);
static monitor_sensor_context_t *monitor_displayed_sensor(void);
static void monitor_alarm_policy_from_settings(
    const product_sensor_settings_t *settings,
    alarm_service_policy_t *policy);
static void monitor_screen_settings_from_product(
    const product_sensor_settings_t *source, screen_settings_t *destination);
static void monitor_product_settings_from_screen(
    const screen_settings_t *source, product_sensor_settings_t *destination);

BaseType_t monitor_app_init(communication_sensor_id_t displayed_sensor_id) {
  screen_settings_t initial_screen_settings;
  TickType_t current_ticks;

  if ((displayed_sensor_id == COMMUNICATION_SENSOR_ID_INVALID) ||
      (product_sensor_count == 0U) ||
      (product_sensor_count > COMMUNICATION_MAX_SENSOR_COUNT)) {
    return pdFAIL;
  }
  if (monitor_service.task != NULL) {
    return (monitor_displayed_sensor()->sensor_id == displayed_sensor_id)
               ? pdPASS
               : pdFAIL;
  }

  current_ticks = xTaskGetTickCount();
  monitor_service.sensor_count = product_sensor_count;
  monitor_service.displayed_sensor_index = product_sensor_count;

  for (size_t index = 0U; index < product_sensor_count; ++index) {
    monitor_sensor_context_t *sensor = &monitor_service.sensors[index];
    alarm_service_policy_t policy;

    sensor->sensor_id = product_sensor_table[index].sensor_id;
    if (product_sensor_settings_load(sensor->sensor_id, &sensor->settings) !=
        SETTINGS_STORE_STATUS_OK) {
      product_sensor_settings_defaults(&sensor->settings);
    }
    monitor_alarm_policy_from_settings(&sensor->settings, &policy);
    alarm_service_init(&sensor->alarm, &policy, current_ticks);

    if (sensor->sensor_id == displayed_sensor_id) {
      monitor_service.displayed_sensor_index = index;
    }
  }
  if (monitor_service.displayed_sensor_index >=
      monitor_service.sensor_count) {
    return pdFAIL;
  }

  monitor_screen_settings_from_product(&monitor_displayed_sensor()->settings,
                                       &initial_screen_settings);

  if (keyboard_init() != pdPASS) {
    return pdFAIL;
  }
  if (indicator_init() != pdPASS) {
    return pdFAIL;
  }
  if (screen_init(&initial_screen_settings) != pdPASS) {
    return pdFAIL;
  }
  if (xTaskCreate(monitor_task, "monitor", MONITOR_TASK_STACK_DEPTH, NULL,
                  MONITOR_TASK_PRIORITY, &monitor_service.task) != pdPASS) {
    monitor_service.task = NULL;
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }
  return pdPASS;
}

BaseType_t monitor_app_request_bootloader(void) {
  TaskHandle_t task;

  taskENTER_CRITICAL();
  task = monitor_service.task;
  if (task == NULL) {
    monitor_service.bootloader_request_pending = true;
  }
  taskEXIT_CRITICAL();

  if (task == NULL) {
    return pdPASS;
  }
  return xTaskNotify(task, MONITOR_NOTIFICATION_BOOTLOADER_REQUEST,
                     eSetBits);
}

static void monitor_task(void *argument) {
  const TickType_t period_ticks = pdMS_TO_TICKS(MONITOR_TASK_PERIOD_MS);
  TickType_t last_wake_ticks;

  (void)argument;
  vTaskDelay(pdMS_TO_TICKS(MONITOR_SENSOR_STARTUP_DELAY_MS));
  monitor_set_status(monitor_apply_all_sensor_settings());
  last_wake_ticks = xTaskGetTickCount();
  monitor_service.last_display_update_ticks = last_wake_ticks;

  for (;;) {
    const TickType_t current_ticks = xTaskGetTickCount();
    uint32_t notifications = 0U;
    uint32_t notified_bits = 0U;

    taskENTER_CRITICAL();
    if (monitor_service.bootloader_request_pending) {
      notifications |= MONITOR_NOTIFICATION_BOOTLOADER_REQUEST;
      monitor_service.bootloader_request_pending = false;
    }
    taskEXIT_CRITICAL();
    (void)xTaskNotifyWait(0U, MONITOR_NOTIFICATION_BOOTLOADER_REQUEST,
                          &notified_bits, 0U);
    notifications |= notified_bits;
    if ((notifications & MONITOR_NOTIFICATION_BOOTLOADER_REQUEST) != 0U) {
      monitor_enter_bootloader();
    }

    monitor_process_communication_events();
    monitor_process_screen_events();
    monitor_poll_alarms(current_ticks);
    monitor_update_display(current_ticks, false);
    vTaskDelayUntil(&last_wake_ticks,
                    (period_ticks == 0U) ? 1U : period_ticks);
  }
}

static void monitor_process_communication_events(void) {
  communication_event_t event;
  bool external_data_received = false;

  for (size_t processed = 0U;
       processed < MONITOR_COMMUNICATION_EVENTS_PER_CYCLE; ++processed) {
    monitor_sensor_context_t *sensor;
    vibration_sensor_sample_t sample;

    if (communication_receive_event(&event, 0U) != pdPASS) {
      break;
    }
    external_data_received = true;
    sensor = monitor_find_sensor(event.sensor_id);
    if (sensor == NULL) {
      continue;
    }
    if (vibration_sensor_event_get_sample(&event, &sample)) {
      monitor_process_sample(sensor, &sample, event.timestamp_ticks);
    } else if (vibration_sensor_event_is_bootloader_request(&event)) {
      monitor_enter_bootloader();
    }
  }

  if (external_data_received) {
    (void)indicator_set_device(INDICATOR_DEVICE_STATUS_LED_GREEN, true,
                               MONITOR_RECEIVE_INDICATOR_DURATION_MS, 0U);
  }
}

static void monitor_process_screen_events(void) {
  screen_event_t event;

  while (screen_receive_event(&event, 0U) == pdPASS) {
    switch (event.type) {
    case SCREEN_EVENT_SETTINGS_COMMITTED: {
      monitor_sensor_context_t *displayed = monitor_displayed_sensor();
      product_sensor_settings_t proposed_settings = displayed->settings;
      screen_settings_t rollback_screen_settings;
      alarm_service_policy_t policy;
      bool settings_saved = false;
      bool operation_ok;

      monitor_product_settings_from_screen(&event.data.settings,
                                           &proposed_settings);

      /*
       * STM32F1 擦除 Flash 页时不能正常执行 Flash 中的中断代码。
       * 写配置前暂停全部传感器的数据流，避免多个传感器继续上报并挤满
       * bxCAN 只有三帧深度的硬件 FIFO。
       */
      operation_ok = monitor_set_all_streaming(false);
      if (operation_ok) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_FLASH_SETTLE_DELAY_MS));
        if (product_sensor_settings_save(displayed->sensor_id,
                                         &proposed_settings) ==
            SETTINGS_STORE_STATUS_OK) {
          settings_saved = true;
          displayed->settings = proposed_settings;
          monitor_alarm_policy_from_settings(&displayed->settings, &policy);
          alarm_service_set_policy(&displayed->alarm, &policy,
                                   xTaskGetTickCount());
          if (!monitor_apply_sensor_settings(displayed)) {
            operation_ok = false;
          }
        } else {
          operation_ok = false;
        }
      }

      /*
       * 数据流未能全部停止或 Flash 保存失败时，持久化配置仍是旧值。
       * 将界面恢复为旧配置，避免界面显示值与实际生效值不一致。
       */
      if (!settings_saved) {
        monitor_screen_settings_from_product(
            &displayed->settings, &rollback_screen_settings);
        if (screen_set_settings(&rollback_screen_settings, 0U) != pdPASS) {
          operation_ok = false;
        }
      }
      if (!monitor_set_all_streaming(true)) {
        operation_ok = false;
      }
      monitor_set_status(operation_ok);
      monitor_update_alarm_outputs();
      break;
    }

    case SCREEN_EVENT_CLEAR_PEAK_REQUEST:
      monitor_clear_peak(event.data.axis);
      break;

    case SCREEN_EVENT_ALARM_CANCEL_REQUEST:
      monitor_cancel_alarm();
      break;

    default:
      break;
    }
  }
}

static void monitor_process_sample(monitor_sensor_context_t *sensor,
                                   const vibration_sensor_sample_t *sample,
                                   TickType_t timestamp_ticks) {
  screen_waveform_sample_t waveform_sample;
  bool output_changed;

  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    const uint16_t current = sample->magnitude_centi_g[axis];

    if (current > sensor->display_window_max[axis]) {
      sensor->display_window_max[axis] = current;
    }
    if (current > sensor->measurements.peak_centi_g[axis]) {
      sensor->measurements.peak_centi_g[axis] = current;
    }
    waveform_sample.signed_centi_g[axis] =
        (sample->signed_raw[axis] < 0)
            ? (int16_t)-(int32_t)sample->magnitude_centi_g[axis]
            : (int16_t)sample->magnitude_centi_g[axis];
  }
  sensor->display_window_has_sample = true;
  if (sensor == monitor_displayed_sensor()) {
    waveform_sample.timestamp_ticks = timestamp_ticks;
    (void)screen_push_waveform_sample(&waveform_sample);
  }

  output_changed =
      alarm_service_process_sample(&sensor->alarm, sample, timestamp_ticks);
  if (output_changed) {
    monitor_update_alarm_outputs();
    monitor_update_display(timestamp_ticks, true);
  }
}

static void monitor_poll_alarms(TickType_t current_ticks) {
  bool output_changed = false;

  for (size_t index = 0U; index < monitor_service.sensor_count; ++index) {
    if (alarm_service_poll(&monitor_service.sensors[index].alarm,
                           current_ticks)) {
      output_changed = true;
    }
  }
  if (output_changed) {
    monitor_update_alarm_outputs();
    monitor_update_display(current_ticks, true);
  }
}

static void monitor_update_display(TickType_t current_ticks, bool force) {
  monitor_sensor_context_t *displayed = monitor_displayed_sensor();
  const TickType_t display_period_ticks =
      pdMS_TO_TICKS(MONITOR_DISPLAY_PERIOD_MS);
  bool any_alarm = false;

  if (!force &&
      ((current_ticks - monitor_service.last_display_update_ticks) <
       ((display_period_ticks == 0U) ? 1U : display_period_ticks))) {
    return;
  }

  for (size_t index = 0U; index < monitor_service.sensor_count; ++index) {
    monitor_sensor_context_t *sensor = &monitor_service.sensors[index];

    if (sensor->display_window_has_sample) {
      for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
        sensor->measurements.current_centi_g[axis] =
            sensor->display_window_max[axis];
        sensor->display_window_max[axis] = 0U;
      }
      sensor->display_window_has_sample = false;
    } else if (!force) {
      /*
       * 与老代码一致：一个显示周期没有收到数据时，“当前值”回到 0，
       * 历史峰值仍保持不变，直到用户单独清除。
       */
      memset(sensor->measurements.current_centi_g, 0,
             sizeof(sensor->measurements.current_centi_g));
    }
  }

  for (size_t index = 0U; index < monitor_service.sensor_count; ++index) {
    any_alarm |=
        alarm_service_is_active(&monitor_service.sensors[index].alarm);
  }
  displayed->measurements.alarm_active = any_alarm;
  (void)screen_update_measurements(&displayed->measurements);
  monitor_service.last_display_update_ticks = current_ticks;
}

static bool monitor_apply_sensor_settings(
    const monitor_sensor_context_t *sensor) {
  bool operation_ok = true;

  if (vibration_sensor_set_sample_rate(
          sensor->sensor_id, sensor->settings.sample_rate,
          pdMS_TO_TICKS(MONITOR_IOCTL_TIMEOUT_MS)) !=
      COMMUNICATION_STATUS_OK) {
    operation_ok = false;
  }
  if (vibration_sensor_set_thresholds(
          sensor->sensor_id, &sensor->settings.thresholds,
          pdMS_TO_TICKS(MONITOR_IOCTL_TIMEOUT_MS)) !=
      COMMUNICATION_STATUS_OK) {
    operation_ok = false;
  }
  return operation_ok;
}

static bool monitor_apply_all_sensor_settings(void) {
  bool operation_ok = true;

  for (size_t index = 0U; index < monitor_service.sensor_count; ++index) {
    if (!monitor_apply_sensor_settings(&monitor_service.sensors[index])) {
      operation_ok = false;
    }
  }
  if (!monitor_set_all_streaming(true)) {
    operation_ok = false;
  }
  return operation_ok;
}

static bool monitor_set_all_streaming(bool enabled) {
  bool operation_ok = true;

  for (size_t index = 0U; index < monitor_service.sensor_count; ++index) {
    if (vibration_sensor_set_streaming(
            monitor_service.sensors[index].sensor_id, enabled,
            pdMS_TO_TICKS(MONITOR_IOCTL_TIMEOUT_MS)) !=
        COMMUNICATION_STATUS_OK) {
      operation_ok = false;
    }
  }
  return operation_ok;
}

static void monitor_update_alarm_outputs(void) {
  static const indicator_device_t axis_indicators[VIBRATION_AXIS_COUNT] = {
      INDICATOR_DEVICE_ALARM_LED_X,
      INDICATOR_DEVICE_ALARM_LED_Y,
      INDICATOR_DEVICE_ALARM_LED_Z,
  };
  bool axis_active[VIBRATION_AXIS_COUNT] = {false};
  bool alarm_active = false;

  for (size_t index = 0U; index < monitor_service.sensor_count; ++index) {
    const alarm_service_t *alarm = &monitor_service.sensors[index].alarm;

    for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
      axis_active[axis] |=
          alarm_service_axis_active(alarm, (vibration_axis_t)axis);
    }
    alarm_active |= alarm_service_is_active(alarm);
  }

  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    (void)indicator_set_device(axis_indicators[axis],
                               axis_active[axis], 0U, 0U);
  }
  (void)indicator_set_device(INDICATOR_DEVICE_BUZZER, alarm_active, 0U, 0U);
}

static void monitor_cancel_alarm(void) {
  const TickType_t current_ticks = xTaskGetTickCount();

  for (size_t index = 0U; index < monitor_service.sensor_count; ++index) {
    (void)alarm_service_cancel(&monitor_service.sensors[index].alarm,
                               current_ticks);
  }
  monitor_update_alarm_outputs();
  monitor_update_display(current_ticks, true);
}

static void monitor_clear_peak(screen_axis_t axis) {
  monitor_sensor_context_t *displayed = monitor_displayed_sensor();

  if ((uint32_t)axis >= (uint32_t)SCREEN_AXIS_COUNT) {
    return;
  }
  displayed->measurements.peak_centi_g[axis] = 0U;
  monitor_update_display(xTaskGetTickCount(), true);
}

static void monitor_enter_bootloader(void) {
  if (!monitor_set_all_streaming(false)) {
    monitor_set_status(false);
    (void)monitor_set_all_streaming(true);
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(MONITOR_OTA_STREAM_STOP_SETTLE_DELAY_MS));

  /*
   * 成功时 bootloader_ota_enter() 会直接复位。只有 Flash 擦写或校验失败
   * 才会返回，此时恢复数据流并点亮红色状态灯，产品仍可继续运行。
   */
  if (bootloader_ota_enter() != BOOTLOADER_OTA_STATUS_OK) {
    monitor_set_status(false);
    (void)monitor_set_all_streaming(true);
  }
}

static void monitor_set_status(bool healthy) {
  (void)indicator_set_device(INDICATOR_DEVICE_STATUS_LED_RED, !healthy, 0U,
                             0U);
}

static monitor_sensor_context_t *
monitor_find_sensor(communication_sensor_id_t sensor_id) {
  for (size_t index = 0U; index < monitor_service.sensor_count; ++index) {
    if (monitor_service.sensors[index].sensor_id == sensor_id) {
      return &monitor_service.sensors[index];
    }
  }
  return NULL;
}

static monitor_sensor_context_t *monitor_displayed_sensor(void) {
  return &monitor_service.sensors[monitor_service.displayed_sensor_index];
}

static void monitor_alarm_policy_from_settings(
    const product_sensor_settings_t *settings,
    alarm_service_policy_t *policy) {
  memset(policy, 0, sizeof(*policy));
  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    policy->minimum_centi_g[axis] = settings->minimum_centi_g[axis];
    policy->maximum_centi_g[axis] =
        settings->thresholds.maximum_centi_g[axis];
  }
  policy->trigger_period_ticks =
      pdMS_TO_TICKS(settings->trigger_period_ms);
  policy->trigger_count = settings->trigger_count;
  /*
   * 秒数直接乘以 Tick 频率，避免先换算成毫秒再调用 pdMS_TO_TICKS()
   * 时发生 32 位中间结果溢出。
   */
  policy->alarm_duration_ticks =
      (TickType_t)((uint32_t)settings->alarm_seconds *
                   (uint32_t)configTICK_RATE_HZ);
  policy->auto_stop = settings->auto_stop_alarm;
}

static void monitor_screen_settings_from_product(
    const product_sensor_settings_t *source, screen_settings_t *destination) {
  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    destination->min_centi_g[axis] = source->minimum_centi_g[axis];
    destination->max_centi_g[axis] =
        source->thresholds.maximum_centi_g[axis];
  }
  destination->trigger_period_ms = source->trigger_period_ms;
  destination->trigger_count = source->trigger_count;
  destination->alarm_seconds = source->alarm_seconds;
  destination->auto_stop_alarm = source->auto_stop_alarm;
  destination->sample_rate = source->sample_rate;
}

static void monitor_product_settings_from_screen(
    const screen_settings_t *source, product_sensor_settings_t *destination) {
  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    destination->minimum_centi_g[axis] = source->min_centi_g[axis];
    destination->thresholds.maximum_centi_g[axis] =
        source->max_centi_g[axis];
  }
  destination->trigger_period_ms = source->trigger_period_ms;
  destination->trigger_count = source->trigger_count;
  destination->alarm_seconds = source->alarm_seconds;
  destination->auto_stop_alarm = source->auto_stop_alarm;
  destination->sample_rate = source->sample_rate;
}
