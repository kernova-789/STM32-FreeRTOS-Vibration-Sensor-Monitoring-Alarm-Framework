#include "screen.h"
#include "screen_config.h"

#include "keyboard.h"
#include "main.h"
#include "queue.h"
#include "screen_driver.h"
#include "string.h"
#include "task.h"

typedef enum {
  SCREEN_PAGE_HOME = 0,
  SCREEN_PAGE_LIMITS,
  SCREEN_PAGE_TRIGGER,
  SCREEN_PAGE_DISPLAY,

  SCREEN_PAGE_COUNT,
} screen_page_t;

typedef struct {
  QueueHandle_t measurement_queue;
  QueueHandle_t settings_queue;
  QueueHandle_t event_queue;
  QueueHandle_t power_queue;
  TaskHandle_t task;
  screen_driver_t driver;
  screen_measurements_t measurements;
  screen_settings_t committed_settings;
  screen_settings_t draft_settings;
  screen_page_t page;
  TickType_t last_activity_tick;
  uint8_t selection;
  bool editing;
  bool dirty;
  bool force_render;
} screen_service_t;

static screen_service_t screen_service;

static void screen_task(void *argument);
static void screen_normalize_settings(screen_settings_t *settings);
static void screen_process_inputs(void);
static void screen_handle_key(const keyboard_event_t *event);
static void screen_handle_navigation_key(keyboard_key_t key);
static void screen_handle_edit_key(keyboard_key_t key);
static void screen_change_page(int8_t direction);
static void screen_move_selection(int8_t direction);
static void screen_adjust_limit(keyboard_key_t key);
static void screen_adjust_trigger(keyboard_key_t key);
static void screen_adjust_display(keyboard_key_t key);
static void screen_commit_edit(void);
static void screen_send_simple_event(screen_event_type_t type);
static void screen_process_power_requests(void);
static void screen_check_auto_off(TickType_t current_time);
static void screen_wait_while_off(void);
static void screen_keyboard_activity(void *context);
static void screen_discard_keyboard_events(void);
static HAL_StatusTypeDef screen_render(void);
static void screen_render_home(void);
static void screen_render_limits(void);
static void screen_render_trigger(void);
static void screen_render_display(void);
static void screen_draw_selected_line(uint8_t row, const char *text,
                                      bool selected);
static void screen_format_centi_g(uint16_t value, char output[6]);
static void screen_format_u16_4(uint16_t value, char output[5]);
static uint8_t screen_selection_count(screen_page_t page);
static uint16_t screen_adjust_value(uint16_t value, uint16_t step,
                                    bool increase, uint16_t minimum,
                                    uint16_t maximum);

BaseType_t screen_init(const screen_settings_t *initial_settings) {
  static const screen_settings_t default_settings = {
      .min_centi_g = {0U, 0U, 0U},
      .max_centi_g = {1600U, 1600U, 1600U},
      .trigger_period_ms = 1000U,
      .trigger_count = 10U,
      .alarm_seconds = 60U,
      .auto_stop_alarm = false,
      .auto_off =
          {
              .enabled = false,
              .timeout_seconds = SCREEN_AUTO_OFF_DEFAULT_SECONDS,
          },
  };
  BaseType_t result;

  if (screen_service.task != NULL) {
    return pdPASS;
  }

  screen_service.committed_settings =
      (initial_settings == NULL) ? default_settings : *initial_settings;
  screen_normalize_settings(&screen_service.committed_settings);
  screen_service.draft_settings = screen_service.committed_settings;
  screen_service.page = SCREEN_PAGE_HOME;
  screen_service.selection = 0U;
  screen_service.editing = false;
  screen_service.dirty = true;
  screen_service.force_render = true;
  screen_service.last_activity_tick = xTaskGetTickCount();

  screen_service.measurement_queue = xQueueCreate(
      SCREEN_MEASUREMENT_QUEUE_LENGTH, sizeof(screen_measurements_t));
  screen_service.settings_queue =
      xQueueCreate(SCREEN_SETTINGS_QUEUE_LENGTH, sizeof(screen_settings_t));
  screen_service.event_queue =
      xQueueCreate(SCREEN_EVENT_QUEUE_LENGTH, sizeof(screen_event_t));
  screen_service.power_queue =
      xQueueCreate(SCREEN_POWER_QUEUE_LENGTH, sizeof(bool));
  if ((screen_service.measurement_queue == NULL) ||
      (screen_service.settings_queue == NULL) ||
      (screen_service.event_queue == NULL) ||
      (screen_service.power_queue == NULL)) {
    if (screen_service.measurement_queue != NULL) {
      vQueueDelete(screen_service.measurement_queue);
    }
    if (screen_service.settings_queue != NULL) {
      vQueueDelete(screen_service.settings_queue);
    }
    if (screen_service.event_queue != NULL) {
      vQueueDelete(screen_service.event_queue);
    }
    if (screen_service.power_queue != NULL) {
      vQueueDelete(screen_service.power_queue);
    }
    screen_service.measurement_queue = NULL;
    screen_service.settings_queue = NULL;
    screen_service.event_queue = NULL;
    screen_service.power_queue = NULL;
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }

  result = xTaskCreate(screen_task, "screen", SCREEN_TASK_STACK_DEPTH, NULL,
                       SCREEN_TASK_PRIORITY, &screen_service.task);
  if (result != pdPASS) {
    vQueueDelete(screen_service.measurement_queue);
    vQueueDelete(screen_service.settings_queue);
    vQueueDelete(screen_service.event_queue);
    vQueueDelete(screen_service.power_queue);
    screen_service.measurement_queue = NULL;
    screen_service.settings_queue = NULL;
    screen_service.event_queue = NULL;
    screen_service.power_queue = NULL;
    screen_service.task = NULL;
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }
  return pdPASS;
}

BaseType_t
screen_update_measurements(const screen_measurements_t *measurements) {
  if ((measurements == NULL) || (screen_service.measurement_queue == NULL)) {
    return pdFAIL;
  }
  return xQueueOverwrite(screen_service.measurement_queue, measurements);
}

BaseType_t screen_set_settings(const screen_settings_t *settings,
                               TickType_t timeout_ticks) {
  screen_settings_t normalized;

  if ((settings == NULL) || (screen_service.settings_queue == NULL)) {
    return pdFAIL;
  }
  normalized = *settings;
  screen_normalize_settings(&normalized);
  return xQueueSend(screen_service.settings_queue, &normalized, timeout_ticks);
}

BaseType_t screen_receive_event(screen_event_t *event,
                                TickType_t timeout_ticks) {
  if ((event == NULL) || (screen_service.event_queue == NULL)) {
    return pdFAIL;
  }
  return xQueueReceive(screen_service.event_queue, event, timeout_ticks);
}

BaseType_t screen_get_settings(screen_settings_t *settings) {
  if ((settings == NULL) || (screen_service.task == NULL)) {
    return pdFAIL;
  }

  taskENTER_CRITICAL();
  *settings = screen_service.committed_settings;
  taskEXIT_CRITICAL();
  return pdPASS;
}

BaseType_t screen_set_enabled(bool enabled) {
  BaseType_t result;

  if (screen_service.power_queue == NULL) {
    return pdFAIL;
  }
  result = xQueueOverwrite(screen_service.power_queue, &enabled);
  if ((result == pdPASS) && (screen_service.task != NULL)) {
    (void)xTaskNotify(screen_service.task, SCREEN_NOTIFICATION_POWER_REQUEST,
                      eSetBits);
  }
  return result;
}

BaseType_t screen_turn_on(void) { return screen_set_enabled(true); }

BaseType_t screen_turn_off(void) { return screen_set_enabled(false); }

BaseType_t screen_set_auto_off(
    const screen_auto_off_settings_t *auto_off_settings,
    TickType_t timeout_ticks) {
  screen_settings_t settings;

  if ((auto_off_settings == NULL) ||
      (screen_get_settings(&settings) != pdPASS)) {
    return pdFAIL;
  }
  settings.auto_off = *auto_off_settings;
  return screen_set_settings(&settings, timeout_ticks);
}

static void screen_task(void *argument) {

  TickType_t last_wake_time = xTaskGetTickCount();
  TickType_t last_render_time = 0U;

  (void)argument;
  if (screen_driver_init(&screen_service.driver, &driver_config) != HAL_OK) {
    vTaskDelete(NULL);
    return;
  }
  keyboard_set_activity_callback(screen_keyboard_activity,
                                 xTaskGetCurrentTaskHandle());

  screen_driver_draw_text_large(&screen_service.driver, 1U, 12U,
                                "DISPLAY READY", false);
  (void)screen_driver_flush(&screen_service.driver);
  vTaskDelay(pdMS_TO_TICKS(500U));
  last_wake_time = xTaskGetTickCount();
  screen_service.dirty = true;
  screen_service.force_render = true;
  screen_service.last_activity_tick = last_wake_time;

  for (;;) {
    uint32_t ignored_notifications;
    TickType_t current_time;

    /*
     * 轮询队列前先清除与队列中已有事件对应的旧通知。
     * 此后到达的新通知会继续保留；若本轮关闭屏幕，它仍能唤醒屏幕任务。
     */
    (void)xTaskNotifyWait(0U, UINT32_MAX, &ignored_notifications, 0U);
    screen_process_inputs();
    current_time = xTaskGetTickCount();
    screen_check_auto_off(current_time);
    if (!screen_service.driver.display_enabled) {
      screen_wait_while_off();
      last_wake_time = xTaskGetTickCount();
      continue;
    }

    if (screen_service.driver.display_enabled && screen_service.dirty &&
        (screen_service.force_render ||
         ((current_time - last_render_time) >=
          pdMS_TO_TICKS(SCREEN_REFRESH_PERIOD_MS)))) {
      if (screen_render() == HAL_OK) {
        screen_service.dirty = false;
        screen_service.force_render = false;
        last_render_time = current_time;
      }
    }
    vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(SCREEN_TASK_PERIOD_MS));
  }
}

static void screen_normalize_settings(screen_settings_t *settings) {
  uint8_t axis;

  for (axis = 0U; axis < SCREEN_AXIS_COUNT; ++axis) {
    if (settings->min_centi_g[axis] >
        SCREEN_ACCELERATION_MAX_CENTI_G) {
      settings->min_centi_g[axis] =
          SCREEN_ACCELERATION_MAX_CENTI_G;
    }
    if (settings->max_centi_g[axis] >
        SCREEN_ACCELERATION_MAX_CENTI_G) {
      settings->max_centi_g[axis] =
          SCREEN_ACCELERATION_MAX_CENTI_G;
    }
    if (settings->max_centi_g[axis] < settings->min_centi_g[axis]) {
      settings->max_centi_g[axis] = settings->min_centi_g[axis];
    }
  }
  if (settings->trigger_period_ms == 0U) {
    settings->trigger_period_ms = 1U;
  } else if (settings->trigger_period_ms > SCREEN_VALUE_MAX) {
    settings->trigger_period_ms = SCREEN_VALUE_MAX;
  }
  if (settings->trigger_count == 0U) {
    settings->trigger_count = 1U;
  } else if (settings->trigger_count > SCREEN_VALUE_MAX) {
    settings->trigger_count = SCREEN_VALUE_MAX;
  }
  if (settings->alarm_seconds == 0U) {
    settings->alarm_seconds = 1U;
  } else if (settings->alarm_seconds > SCREEN_VALUE_MAX) {
    settings->alarm_seconds = SCREEN_VALUE_MAX;
  }
  if (settings->auto_off.timeout_seconds == 0U) {
    settings->auto_off.timeout_seconds = 1U;
  } else if (settings->auto_off.timeout_seconds > SCREEN_VALUE_MAX) {
    settings->auto_off.timeout_seconds = SCREEN_VALUE_MAX;
  }
}

static void screen_process_inputs(void) {
  keyboard_event_t key_event;
  screen_measurements_t measurements;
  screen_settings_t settings;

  screen_process_power_requests();

  while (keyboard_receive_event(&key_event, 0U) == pdPASS) {
    screen_handle_key(&key_event);
  }
  while (xQueueReceive(screen_service.measurement_queue, &measurements, 0U) ==
         pdPASS) {
    screen_service.measurements = measurements;
    if (screen_service.page == SCREEN_PAGE_HOME) {
      screen_service.dirty = true;
    }
  }
  while (xQueueReceive(screen_service.settings_queue, &settings, 0U) ==
         pdPASS) {
    screen_normalize_settings(&settings);
    taskENTER_CRITICAL();
    screen_service.committed_settings = settings;
    taskEXIT_CRITICAL();
    screen_service.draft_settings = settings;
    screen_service.editing = false;
    screen_service.dirty = true;
    screen_service.force_render = true;
    screen_service.last_activity_tick = xTaskGetTickCount();
  }
}

static void screen_process_power_requests(void) {
  bool display_enabled;

  while (xQueueReceive(screen_service.power_queue, &display_enabled, 0U) ==
         pdPASS) {
    if (screen_driver_set_enabled(&screen_service.driver, display_enabled) !=
        HAL_OK) {
      continue;
    }
    if (display_enabled) {
      screen_service.last_activity_tick = xTaskGetTickCount();
      screen_service.dirty = true;
      screen_service.force_render = true;
    }
  }
}

static void screen_check_auto_off(TickType_t current_time) {
  TickType_t timeout_ticks;
  uint32_t timeout_seconds;

  if (!screen_service.driver.display_enabled ||
      !screen_service.committed_settings.auto_off.enabled) {
    return;
  }
  if (keyboard_get_state_mask() != 0U) {
    screen_service.last_activity_tick = current_time;
    return;
  }

  timeout_seconds =
      (uint32_t)screen_service.committed_settings.auto_off.timeout_seconds;
  timeout_ticks =
      (TickType_t)(timeout_seconds * (uint32_t)configTICK_RATE_HZ);
  if (timeout_ticks == 0U) {
    timeout_ticks = 1U;
  }

  if ((current_time - screen_service.last_activity_tick) >= timeout_ticks) {
    (void)screen_driver_set_enabled(&screen_service.driver, false);
  }
}

static void screen_wait_while_off(void) {
  uint32_t notifications;

  while (!screen_service.driver.display_enabled) {
    /*
     * 阻塞前先检查一次队列。若请求恰好在检查后到达，
     * 对应的任务通知会负责唤醒，因此不需要周期轮询。
     */
    screen_process_power_requests();
    if (screen_service.driver.display_enabled) {
      break;
    }
    if (xTaskNotifyWait(0U, UINT32_MAX, &notifications, portMAX_DELAY) !=
        pdTRUE) {
      continue;
    }

    if ((notifications & SCREEN_NOTIFICATION_POWER_REQUEST) != 0U) {
      screen_process_power_requests();
    }
    if ((notifications & SCREEN_NOTIFICATION_KEY_ACTIVITY) != 0U) {
      screen_discard_keyboard_events();
      if (screen_driver_set_enabled(&screen_service.driver, true) == HAL_OK) {
        screen_service.last_activity_tick = xTaskGetTickCount();
        screen_service.dirty = true;
        screen_service.force_render = true;
      }
    }
  }
}

static void screen_keyboard_activity(void *context) {
  TaskHandle_t screen_task_handle = (TaskHandle_t)context;

  if (screen_task_handle != NULL) {
    (void)xTaskNotify(screen_task_handle, SCREEN_NOTIFICATION_KEY_ACTIVITY,
                      eSetBits);
  }
}

static void screen_discard_keyboard_events(void) {
  keyboard_event_t event;

  while (keyboard_receive_event(&event, 0U) == pdPASS) {
  }
}

static void screen_handle_key(const keyboard_event_t *event) {
  if (event->type == KEYBOARD_EVENT_RELEASED) {
    return;
  }
  screen_service.last_activity_tick = xTaskGetTickCount();

  if ((event->type == KEYBOARD_EVENT_REPEAT) && !screen_service.editing) {
    return;
  }
  if ((event->type == KEYBOARD_EVENT_REPEAT) &&
      (((screen_service.page == SCREEN_PAGE_TRIGGER) &&
        (screen_service.selection == 3U)) ||
       ((screen_service.page == SCREEN_PAGE_DISPLAY) &&
        (screen_service.selection == 0U)))) {
    return;
  }

  if (screen_service.editing) {
    screen_handle_edit_key(event->key);
  } else {
    screen_handle_navigation_key(event->key);
  }
}

static void screen_handle_navigation_key(keyboard_key_t key) {
  switch (key) {
  case KEYBOARD_KEY_UP:
    screen_move_selection(-1);
    break;
  case KEYBOARD_KEY_DOWN:
    screen_move_selection(1);
    break;
  case KEYBOARD_KEY_LEFT:
    screen_change_page(-1);
    break;
  case KEYBOARD_KEY_RIGHT:
    screen_change_page(1);
    break;
  case KEYBOARD_KEY_ENTER:
    if (screen_service.page == SCREEN_PAGE_HOME) {
      screen_send_simple_event(SCREEN_EVENT_CLEAR_PEAK_REQUEST);
    } else {
      screen_service.draft_settings = screen_service.committed_settings;
      screen_service.editing = true;
      screen_service.dirty = true;
      screen_service.force_render = true;
    }
    break;
  case KEYBOARD_KEY_CANCEL:
    if (screen_service.page == SCREEN_PAGE_HOME) {
      screen_send_simple_event(SCREEN_EVENT_ALARM_CANCEL_REQUEST);
    } else {
      screen_service.page = SCREEN_PAGE_HOME;
      screen_service.selection = 0U;
      screen_service.dirty = true;
      screen_service.force_render = true;
    }
    break;
  default:
    break;
  }
}

static void screen_handle_edit_key(keyboard_key_t key) {
  switch (key) {
  case KEYBOARD_KEY_ENTER:
    screen_commit_edit();
    break;
  case KEYBOARD_KEY_CANCEL:
    screen_service.draft_settings = screen_service.committed_settings;
    screen_service.editing = false;
    screen_service.dirty = true;
    screen_service.force_render = true;
    break;
  case KEYBOARD_KEY_UP:
  case KEYBOARD_KEY_DOWN:
  case KEYBOARD_KEY_LEFT:
  case KEYBOARD_KEY_RIGHT:
    if (screen_service.page == SCREEN_PAGE_LIMITS) {
      screen_adjust_limit(key);
    } else if (screen_service.page == SCREEN_PAGE_TRIGGER) {
      screen_adjust_trigger(key);
    } else if (screen_service.page == SCREEN_PAGE_DISPLAY) {
      screen_adjust_display(key);
    }
    screen_service.dirty = true;
    screen_service.force_render = true;
    break;
  default:
    break;
  }
}

static void screen_change_page(int8_t direction) {
  int8_t next_page = (int8_t)screen_service.page + direction;

  if (next_page < 0) {
    next_page = (int8_t)SCREEN_PAGE_COUNT - 1;
  } else if (next_page >= (int8_t)SCREEN_PAGE_COUNT) {
    next_page = 0;
  }
  screen_service.page = (screen_page_t)next_page;
  screen_service.selection = 0U;
  screen_service.dirty = true;
  screen_service.force_render = true;
}

static void screen_move_selection(int8_t direction) {
  const uint8_t selection_count = screen_selection_count(screen_service.page);
  int8_t next_selection = (int8_t)screen_service.selection + direction;

  if (next_selection < 0) {
    next_selection = (int8_t)selection_count - 1;
  } else if (next_selection >= (int8_t)selection_count) {
    next_selection = 0;
  }
  screen_service.selection = (uint8_t)next_selection;
  screen_service.dirty = true;
  screen_service.force_render = true;
}

static void screen_adjust_limit(keyboard_key_t key) {
  const uint8_t axis = screen_service.selection / 2U;
  const bool is_maximum = (screen_service.selection & 1U) != 0U;
  const bool increase = (key == KEYBOARD_KEY_UP) || (key == KEYBOARD_KEY_RIGHT);
  const uint16_t step =
      ((key == KEYBOARD_KEY_LEFT) || (key == KEYBOARD_KEY_RIGHT))
          ? SCREEN_LIMIT_COARSE_STEP
          : SCREEN_LIMIT_FINE_STEP;

  if (is_maximum) {
    screen_service.draft_settings.max_centi_g[axis] = screen_adjust_value(
        screen_service.draft_settings.max_centi_g[axis], step, increase,
        screen_service.draft_settings.min_centi_g[axis],
        SCREEN_ACCELERATION_MAX_CENTI_G);
  } else {
    screen_service.draft_settings.min_centi_g[axis] = screen_adjust_value(
        screen_service.draft_settings.min_centi_g[axis], step, increase, 0U,
        screen_service.draft_settings.max_centi_g[axis]);
  }
}

static void screen_adjust_trigger(keyboard_key_t key) {
  const bool increase = (key == KEYBOARD_KEY_UP) || (key == KEYBOARD_KEY_RIGHT);
  const uint16_t step =
      ((key == KEYBOARD_KEY_LEFT) || (key == KEYBOARD_KEY_RIGHT))
          ? SCREEN_TRIGGER_COARSE_STEP
          : SCREEN_TRIGGER_FINE_STEP;
  uint16_t *value = NULL;

  switch (screen_service.selection) {
  case 0U:
    value = &screen_service.draft_settings.trigger_period_ms;
    break;
  case 1U:
    value = &screen_service.draft_settings.trigger_count;
    break;
  case 2U:
    value = &screen_service.draft_settings.alarm_seconds;
    break;
  case 3U:
    screen_service.draft_settings.auto_stop_alarm =
        !screen_service.draft_settings.auto_stop_alarm;
    break;
  default:
    break;
  }
  if (value != NULL) {
    *value = screen_adjust_value(*value, step, increase, 1U, SCREEN_VALUE_MAX);
  }
}

static void screen_adjust_display(keyboard_key_t key) {
  const bool increase = (key == KEYBOARD_KEY_UP) ||
                        (key == KEYBOARD_KEY_RIGHT);
  const uint16_t step =
      ((key == KEYBOARD_KEY_LEFT) || (key == KEYBOARD_KEY_RIGHT))
          ? SCREEN_AUTO_OFF_COARSE_STEP_SECONDS
          : SCREEN_AUTO_OFF_FINE_STEP_SECONDS;

  switch (screen_service.selection) {
  case 0U:
    screen_service.draft_settings.auto_off.enabled =
        !screen_service.draft_settings.auto_off.enabled;
    break;
  case 1U:
    screen_service.draft_settings.auto_off.timeout_seconds =
        screen_adjust_value(
            screen_service.draft_settings.auto_off.timeout_seconds, step,
            increase, 1U, SCREEN_VALUE_MAX);
    break;
  default:
    break;
  }
}

static void screen_commit_edit(void) {
  screen_event_t event;

  screen_normalize_settings(&screen_service.draft_settings);
  taskENTER_CRITICAL();
  screen_service.committed_settings = screen_service.draft_settings;
  taskEXIT_CRITICAL();
  screen_service.editing = false;
  screen_service.dirty = true;
  screen_service.force_render = true;
  screen_service.last_activity_tick = xTaskGetTickCount();

  event.type = SCREEN_EVENT_SETTINGS_COMMITTED;
  event.data.settings = screen_service.committed_settings;
  (void)xQueueSend(screen_service.event_queue, &event, 0U);
}

static void screen_send_simple_event(screen_event_type_t type) {
  screen_event_t event = {.type = type};

  if (type == SCREEN_EVENT_CLEAR_PEAK_REQUEST) {
    event.data.axis = (screen_axis_t)screen_service.selection;
  }
  (void)xQueueSend(screen_service.event_queue, &event, 0U);
}

static HAL_StatusTypeDef screen_render(void) {
  screen_driver_clear(&screen_service.driver);
  switch (screen_service.page) {
  case SCREEN_PAGE_HOME:
    screen_render_home();
    break;
  case SCREEN_PAGE_LIMITS:
    screen_render_limits();
    break;
  case SCREEN_PAGE_TRIGGER:
    screen_render_trigger();
    break;
  case SCREEN_PAGE_DISPLAY:
    screen_render_display();
    break;
  default:
    return HAL_ERROR;
  }
  return screen_driver_flush(&screen_service.driver);
}

static void screen_render_home(void) {
  static const char axis_names[SCREEN_AXIS_COUNT] = {'X', 'Y', 'Z'};
  uint8_t axis;

  screen_driver_draw_text_large(
      &screen_service.driver, 0U, 0U,
      screen_service.measurements.alarm_active ? "ALM LIVE PEAK(G)"
                                               : "  LIVE  PEAK (G)",
      false);
  for (axis = 0U; axis < SCREEN_AXIS_COUNT; ++axis) {
    char line[] = "X 00.00 00.00";
    char current_value[6];
    char peak_value[6];

    line[0] = axis_names[axis];
    screen_format_centi_g(screen_service.measurements.current_centi_g[axis],
                          current_value);
    screen_format_centi_g(screen_service.measurements.peak_centi_g[axis],
                          peak_value);
    memcpy(&line[2], current_value, 5U);
    memcpy(&line[8], peak_value, 5U);
    screen_draw_selected_line((uint8_t)(axis + 1U), line,
                              screen_service.selection == axis);
  }
}

static void screen_render_limits(void) {
  static const char axis_names[SCREEN_AXIS_COUNT] = {'X', 'Y', 'Z'};
  uint8_t axis;

  screen_driver_draw_text_large(
      &screen_service.driver, 0U, 0U,
      screen_service.editing ? "  MIN   MAX EDIT" : "  MIN   MAX  (G)",
      false);
  for (axis = 0U; axis < SCREEN_AXIS_COUNT; ++axis) {
    char axis_name[2] = {axis_names[axis], '\0'};
    char minimum[6];
    char maximum[6];

    screen_format_centi_g(screen_service.draft_settings.min_centi_g[axis],
                          minimum);
    screen_format_centi_g(screen_service.draft_settings.max_centi_g[axis],
                          maximum);
    screen_driver_draw_text_large(&screen_service.driver,
                                  (uint8_t)(axis + 1U), 0U, axis_name, false);
    screen_driver_draw_text_large(
        &screen_service.driver, (uint8_t)(axis + 1U), 16U, minimum,
        screen_service.selection == (uint8_t)(axis * 2U));
    screen_driver_draw_text_large(
        &screen_service.driver, (uint8_t)(axis + 1U), 64U, maximum,
        screen_service.selection == (uint8_t)((axis * 2U) + 1U));
  }
}

static void screen_render_trigger(void) {
  char line[17] = "PERIOD 0000MS   ";
  char value[5];

  screen_format_u16_4(screen_service.draft_settings.trigger_period_ms, value);
  memcpy(&line[7], value, 4U);
  if (screen_service.editing && (screen_service.selection == 0U)) {
    line[15] = 'E';
  }
  screen_draw_selected_line(0U, line, screen_service.selection == 0U);

  memcpy(line, "COUNT  0000     ", sizeof(line));
  screen_format_u16_4(screen_service.draft_settings.trigger_count, value);
  memcpy(&line[7], value, 4U);
  if (screen_service.editing && (screen_service.selection == 1U)) {
    line[15] = 'E';
  }
  screen_draw_selected_line(1U, line, screen_service.selection == 1U);

  memcpy(line, "ALARM  0000S    ", sizeof(line));
  screen_format_u16_4(screen_service.draft_settings.alarm_seconds, value);
  memcpy(&line[7], value, 4U);
  if (screen_service.editing && (screen_service.selection == 2U)) {
    line[15] = 'E';
  }
  screen_draw_selected_line(2U, line, screen_service.selection == 2U);

  memcpy(line, "AUTO   NO       ", sizeof(line));
  if (screen_service.draft_settings.auto_stop_alarm) {
    memcpy(&line[7], "YES", 3U);
  }
  if (screen_service.editing && (screen_service.selection == 3U)) {
    line[15] = 'E';
  }
  screen_draw_selected_line(3U, line, screen_service.selection == 3U);
}

static void screen_render_display(void) {
  char line[17] = "AUTO OFF NO     ";
  char value[5];

  screen_driver_draw_text_large(
      &screen_service.driver, 0U, 0U,
      screen_service.editing ? "DISPLAY     EDIT" : "DISPLAY SETTINGS",
      false);

  if (screen_service.draft_settings.auto_off.enabled) {
    memcpy(&line[9], "YES", 3U);
  }
  if (screen_service.editing && (screen_service.selection == 0U)) {
    line[15] = 'E';
  }
  screen_draw_selected_line(1U, line, screen_service.selection == 0U);

  memcpy(line, "IDLE   0000S    ", sizeof(line));
  screen_format_u16_4(
      screen_service.draft_settings.auto_off.timeout_seconds, value);
  memcpy(&line[7], value, 4U);
  if (screen_service.editing && (screen_service.selection == 1U)) {
    line[15] = 'E';
  }
  screen_draw_selected_line(2U, line, screen_service.selection == 1U);

  screen_driver_draw_text_large(&screen_service.driver, 3U, 0U,
                                "WAKE ANY KEY", false);
}

static void screen_draw_selected_line(uint8_t row, const char *text,
                                      bool selected) {
  if (selected) {
    screen_driver_fill_row(&screen_service.driver, (uint8_t)(row * 2U), true);
    screen_driver_fill_row(&screen_service.driver,
                           (uint8_t)((row * 2U) + 1U), true);
  }
  screen_driver_draw_text_large(&screen_service.driver, row, 0U, text,
                                selected);
}

static void screen_format_centi_g(uint16_t value, char output[6]) {
  if (value > SCREEN_VALUE_MAX) {
    value = SCREEN_VALUE_MAX;
  }
  output[0] = (char)('0' + ((value / 1000U) % 10U));
  output[1] = (char)('0' + ((value / 100U) % 10U));
  output[2] = '.';
  output[3] = (char)('0' + ((value / 10U) % 10U));
  output[4] = (char)('0' + (value % 10U));
  output[5] = '\0';
}

static void screen_format_u16_4(uint16_t value, char output[5]) {
  if (value > SCREEN_VALUE_MAX) {
    value = SCREEN_VALUE_MAX;
  }
  output[0] = (char)('0' + ((value / 1000U) % 10U));
  output[1] = (char)('0' + ((value / 100U) % 10U));
  output[2] = (char)('0' + ((value / 10U) % 10U));
  output[3] = (char)('0' + (value % 10U));
  output[4] = '\0';
}

static uint8_t screen_selection_count(screen_page_t page) {
  switch (page) {
  case SCREEN_PAGE_HOME:
    return SCREEN_AXIS_COUNT;
  case SCREEN_PAGE_LIMITS:
    return 6U;
  case SCREEN_PAGE_TRIGGER:
    return 4U;
  case SCREEN_PAGE_DISPLAY:
    return 2U;
  default:
    return 1U;
  }
}

static uint16_t screen_adjust_value(uint16_t value, uint16_t step,
                                    bool increase, uint16_t minimum,
                                    uint16_t maximum) {
  if (increase) {
    if ((value >= maximum) || (step >= (uint16_t)(maximum - value))) {
      return maximum;
    }
    return (uint16_t)(value + step);
  }
  if ((value <= minimum) || (step >= (uint16_t)(value - minimum))) {
    return minimum;
  }
  return (uint16_t)(value - step);
}
