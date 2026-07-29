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
  SCREEN_PAGE_SAMPLE_RATE,
  SCREEN_PAGE_WAVEFORM,

  SCREEN_PAGE_COUNT,
} screen_page_t;

#define SCREEN_WAVEFORM_SAMPLE_CAPACITY 109U
#define SCREEN_WAVEFORM_WINDOW_MS 5000U
#define SCREEN_WAVEFORM_COLUMN_DURATION_MS                                \
  ((SCREEN_WAVEFORM_WINDOW_MS + SCREEN_WAVEFORM_SAMPLE_CAPACITY - 1U) /   \
   SCREEN_WAVEFORM_SAMPLE_CAPACITY)
#define SCREEN_WAVEFORM_PLOT_LEFT 19U
#define SCREEN_WAVEFORM_PLOT_RIGHT 127U
#define SCREEN_WAVEFORM_PLOT_TOP 9U
#define SCREEN_WAVEFORM_PLOT_CENTER 32U
#define SCREEN_WAVEFORM_PLOT_BOTTOM 55U

typedef struct {
  QueueHandle_t measurement_queue;
  QueueHandle_t settings_queue;
  QueueHandle_t event_queue;
  TaskHandle_t task;
  screen_driver_t driver;
  screen_measurements_t measurements;
  screen_settings_t committed_settings;
  screen_settings_t draft_settings;
  int16_t waveform_min[SCREEN_AXIS_COUNT][SCREEN_WAVEFORM_SAMPLE_CAPACITY];
  int16_t waveform_max[SCREEN_AXIS_COUNT][SCREEN_WAVEFORM_SAMPLE_CAPACITY];
  bool waveform_valid[SCREEN_WAVEFORM_SAMPLE_CAPACITY];
  uint32_t waveform_sequence;
  uint32_t waveform_rendered_sequence;
  TickType_t waveform_column_start_ticks;
  uint8_t waveform_write_index;
  uint8_t waveform_count;
  bool waveform_column_active;
  screen_page_t page;
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
static void screen_adjust_sample_rate(keyboard_key_t key);
static void screen_commit_edit(void);
static void screen_send_simple_event(screen_event_type_t type);
static HAL_StatusTypeDef screen_render(void);
static void screen_render_home(void);
static void screen_render_limits(void);
static void screen_render_trigger(void);
static void screen_render_sample_rate(void);
static void screen_render_waveform(void);
static void screen_draw_selected_line(uint8_t row, const char *text,
                                      bool selected);
static void screen_format_centi_g(uint16_t value, char output[6]);
static void screen_format_u16_4(uint16_t value, char output[5]);
static void screen_format_sample_rate(
    vibration_sensor_sample_rate_t sample_rate, char output[8]);
static uint8_t screen_waveform_value_to_y(int16_t value);
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
      .sample_rate = VIBRATION_SENSOR_SAMPLE_RATE_533_34_HZ,
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

  screen_service.measurement_queue = xQueueCreate(
      SCREEN_MEASUREMENT_QUEUE_LENGTH, sizeof(screen_measurements_t));
  screen_service.settings_queue =
      xQueueCreate(SCREEN_SETTINGS_QUEUE_LENGTH, sizeof(screen_settings_t));
  screen_service.event_queue =
      xQueueCreate(SCREEN_EVENT_QUEUE_LENGTH, sizeof(screen_event_t));
  if ((screen_service.measurement_queue == NULL) ||
      (screen_service.settings_queue == NULL) ||
      (screen_service.event_queue == NULL)) {
    if (screen_service.measurement_queue != NULL) {
      vQueueDelete(screen_service.measurement_queue);
    }
    if (screen_service.settings_queue != NULL) {
      vQueueDelete(screen_service.settings_queue);
    }
    if (screen_service.event_queue != NULL) {
      vQueueDelete(screen_service.event_queue);
    }
    screen_service.measurement_queue = NULL;
    screen_service.settings_queue = NULL;
    screen_service.event_queue = NULL;
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }

  result = xTaskCreate(screen_task, "screen", SCREEN_TASK_STACK_DEPTH, NULL,
                       SCREEN_TASK_PRIORITY, &screen_service.task);
  if (result != pdPASS) {
    vQueueDelete(screen_service.measurement_queue);
    vQueueDelete(screen_service.settings_queue);
    vQueueDelete(screen_service.event_queue);
    screen_service.measurement_queue = NULL;
    screen_service.settings_queue = NULL;
    screen_service.event_queue = NULL;
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

BaseType_t
screen_push_waveform_sample(const screen_waveform_sample_t *sample) {
  const TickType_t column_ticks =
      (pdMS_TO_TICKS(SCREEN_WAVEFORM_COLUMN_DURATION_MS) == 0U)
          ? 1U
          : pdMS_TO_TICKS(SCREEN_WAVEFORM_COLUMN_DURATION_MS);

  if ((sample == NULL) || (screen_service.task == NULL)) {
    return pdFAIL;
  }

  taskENTER_CRITICAL();
  if (!screen_service.waveform_column_active) {
    screen_service.waveform_column_active = true;
    screen_service.waveform_column_start_ticks = sample->timestamp_ticks;
    screen_service.waveform_write_index = 0U;
    screen_service.waveform_count = 1U;
    screen_service.waveform_valid[0] = true;
    for (uint8_t axis = 0U; axis < SCREEN_AXIS_COUNT; ++axis) {
      screen_service.waveform_min[axis][0] = sample->signed_centi_g[axis];
      screen_service.waveform_max[axis][0] = sample->signed_centi_g[axis];
    }
  } else {
    const TickType_t elapsed_ticks =
        sample->timestamp_ticks - screen_service.waveform_column_start_ticks;

    if (elapsed_ticks >= column_ticks) {
      uint32_t elapsed_columns = elapsed_ticks / column_ticks;
      uint32_t columns_to_clear = elapsed_columns;

      if (columns_to_clear > SCREEN_WAVEFORM_SAMPLE_CAPACITY) {
        columns_to_clear = SCREEN_WAVEFORM_SAMPLE_CAPACITY;
      }
      for (uint32_t column = 0U; column < columns_to_clear; ++column) {
        screen_service.waveform_write_index =
            (uint8_t)((screen_service.waveform_write_index + 1U) %
                      SCREEN_WAVEFORM_SAMPLE_CAPACITY);
        screen_service
            .waveform_valid[screen_service.waveform_write_index] = false;
        if (screen_service.waveform_count <
            SCREEN_WAVEFORM_SAMPLE_CAPACITY) {
          ++screen_service.waveform_count;
        }
      }
      screen_service.waveform_column_start_ticks +=
          (TickType_t)(elapsed_columns * column_ticks);
      screen_service
          .waveform_valid[screen_service.waveform_write_index] = true;
      for (uint8_t axis = 0U; axis < SCREEN_AXIS_COUNT; ++axis) {
        screen_service
            .waveform_min[axis][screen_service.waveform_write_index] =
            sample->signed_centi_g[axis];
        screen_service
            .waveform_max[axis][screen_service.waveform_write_index] =
            sample->signed_centi_g[axis];
      }
    } else {
      const uint8_t write_index = screen_service.waveform_write_index;

      for (uint8_t axis = 0U; axis < SCREEN_AXIS_COUNT; ++axis) {
        if (sample->signed_centi_g[axis] <
            screen_service.waveform_min[axis][write_index]) {
          screen_service.waveform_min[axis][write_index] =
              sample->signed_centi_g[axis];
        }
        if (sample->signed_centi_g[axis] >
            screen_service.waveform_max[axis][write_index]) {
          screen_service.waveform_max[axis][write_index] =
              sample->signed_centi_g[axis];
        }
      }
    }
  }
  ++screen_service.waveform_sequence;
  taskEXIT_CRITICAL();
  return pdPASS;
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

static void screen_task(void *argument) {

  TickType_t last_wake_time = xTaskGetTickCount();
  TickType_t last_render_time = 0U;

  (void)argument;
  if (screen_driver_init(&screen_service.driver, &driver_config) != HAL_OK) {
    vTaskDelete(NULL);
    return;
  }

  screen_driver_draw_text_large(&screen_service.driver, 0U, 0U,
                                "DESIGNED BY", false);
  screen_driver_draw_text_large(&screen_service.driver, 1U, 0U,
                                "GITHUB", false);
  screen_driver_draw_text_large(&screen_service.driver, 3U, 0U,
                                "KERNOVA", false);
  (void)screen_driver_flush(&screen_service.driver);
  vTaskDelay(pdMS_TO_TICKS(1500U));
  last_wake_time = xTaskGetTickCount();
  screen_service.dirty = true;
  screen_service.force_render = true;

  for (;;) {
    const TickType_t current_time = xTaskGetTickCount();

    screen_process_inputs();

    if (screen_service.dirty &&
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
  if (!vibration_sensor_sample_rate_is_supported(settings->sample_rate)) {
    settings->sample_rate = VIBRATION_SENSOR_SAMPLE_RATE_533_34_HZ;
  }
}

static void screen_process_inputs(void) {
  keyboard_event_t key_event;
  screen_measurements_t measurements;
  screen_settings_t settings;

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
  }

  if (screen_service.page == SCREEN_PAGE_WAVEFORM) {
    uint32_t waveform_sequence;

    taskENTER_CRITICAL();
    waveform_sequence = screen_service.waveform_sequence;
    taskEXIT_CRITICAL();
    if (waveform_sequence != screen_service.waveform_rendered_sequence) {
      screen_service.dirty = true;
    }
  }
}

static void screen_handle_key(const keyboard_event_t *event) {
  if (event->type == KEYBOARD_EVENT_RELEASED) {
    return;
  }

  if ((event->type == KEYBOARD_EVENT_REPEAT) && !screen_service.editing) {
    return;
  }
  if ((event->type == KEYBOARD_EVENT_REPEAT) &&
      (screen_service.page == SCREEN_PAGE_TRIGGER) &&
      (screen_service.selection == 3U)) {
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
    } else if (screen_service.page != SCREEN_PAGE_WAVEFORM) {
      screen_service.draft_settings = screen_service.committed_settings;
      screen_service.editing = true;
      screen_service.dirty = true;
      screen_service.force_render = true;
    }
    break;
  case KEYBOARD_KEY_CANCEL:
    if (screen_service.page == SCREEN_PAGE_HOME) {
      screen_send_simple_event(SCREEN_EVENT_ALARM_CANCEL_REQUEST);
    } else if (screen_service.page != SCREEN_PAGE_WAVEFORM) {
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
    } else if (screen_service.page == SCREEN_PAGE_SAMPLE_RATE) {
      screen_adjust_sample_rate(key);
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

static void screen_adjust_sample_rate(keyboard_key_t key) {
  const bool increase =
      (key == KEYBOARD_KEY_UP) || (key == KEYBOARD_KEY_RIGHT);
  vibration_sensor_sample_rate_t sample_rate =
      screen_service.draft_settings.sample_rate;

  if (increase) {
    sample_rate =
        (sample_rate + 1U >= VIBRATION_SENSOR_SAMPLE_RATE_COUNT)
            ? VIBRATION_SENSOR_SAMPLE_RATE_533_34_HZ
            : (vibration_sensor_sample_rate_t)(sample_rate + 1U);
  } else {
    sample_rate =
        (sample_rate == VIBRATION_SENSOR_SAMPLE_RATE_533_34_HZ)
            ? (vibration_sensor_sample_rate_t)(
                  VIBRATION_SENSOR_SAMPLE_RATE_COUNT - 1U)
            : (vibration_sensor_sample_rate_t)(sample_rate - 1U);
  }
  screen_service.draft_settings.sample_rate = sample_rate;
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
  case SCREEN_PAGE_SAMPLE_RATE:
    screen_render_sample_rate();
    break;
  case SCREEN_PAGE_WAVEFORM:
    screen_render_waveform();
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

static void screen_render_sample_rate(void) {
  char rate[8];
  char line[] = " 0000.00 HZ";

  screen_driver_draw_text_large(&screen_service.driver, 0U, 0U, "SAMPLE RATE",
                                false);
  screen_format_sample_rate(screen_service.draft_settings.sample_rate, rate);
  memcpy(&line[1], rate, 7U);
  if (screen_service.selection == 0U) {
    screen_driver_fill_row(&screen_service.driver, 2U, true);
    screen_driver_fill_row(&screen_service.driver, 3U, true);
  }
  screen_driver_draw_text_large(&screen_service.driver, 1U, 0U, line,
                                screen_service.selection == 0U);
  screen_driver_draw_text_large(
      &screen_service.driver, 2U, 0U,
      screen_service.editing ? "UP/DN SELECT" : "ENTER EDIT", false);
  screen_driver_draw_text_large(
      &screen_service.driver, 3U, 0U,
      screen_service.editing ? "ENTER SAVE" : "LEFT/RIGHT", false);
}

static void screen_render_waveform(void) {
  static const char axis_names[SCREEN_AXIS_COUNT] = {'X', 'Y', 'Z'};
  int16_t minimums[SCREEN_WAVEFORM_SAMPLE_CAPACITY];
  int16_t maximums[SCREEN_WAVEFORM_SAMPLE_CAPACITY];
  bool valid[SCREEN_WAVEFORM_SAMPLE_CAPACITY];
  char header[] = "X WAVE 5S +/-16G";
  uint32_t sequence;
  uint8_t count;
  uint8_t start;
  uint8_t axis = screen_service.selection;
  bool has_valid = false;

  if (axis >= SCREEN_AXIS_COUNT) {
    axis = 0U;
  }

  taskENTER_CRITICAL();
  count = screen_service.waveform_count;
  start = (count == SCREEN_WAVEFORM_SAMPLE_CAPACITY)
              ? (uint8_t)((screen_service.waveform_write_index + 1U) %
                          SCREEN_WAVEFORM_SAMPLE_CAPACITY)
              : 0U;
  for (uint8_t index = 0U; index < count; ++index) {
    const uint8_t source =
        (uint8_t)((start + index) % SCREEN_WAVEFORM_SAMPLE_CAPACITY);

    minimums[index] = screen_service.waveform_min[axis][source];
    maximums[index] = screen_service.waveform_max[axis][source];
    valid[index] = screen_service.waveform_valid[source];
  }
  sequence = screen_service.waveform_sequence;
  taskEXIT_CRITICAL();
  screen_service.waveform_rendered_sequence = sequence;

  header[0] = axis_names[axis];

  screen_driver_draw_text(&screen_service.driver, 0U, 0U, header, false);
  screen_driver_draw_text(&screen_service.driver, 1U, 0U, "+16", false);
  screen_driver_draw_text(&screen_service.driver, 4U, 12U, "0", false);
  screen_driver_draw_text(&screen_service.driver, 6U, 0U, "-16", false);
  screen_driver_draw_text(&screen_service.driver, 7U,
                          SCREEN_WAVEFORM_PLOT_LEFT, "-5S", false);
  screen_driver_draw_text(&screen_service.driver, 7U, 122U, "0", false);

  screen_driver_draw_line(
      &screen_service.driver, (uint8_t)(SCREEN_WAVEFORM_PLOT_LEFT - 1U),
      (uint8_t)(SCREEN_WAVEFORM_PLOT_TOP - 1U),
      (uint8_t)(SCREEN_WAVEFORM_PLOT_LEFT - 1U),
      SCREEN_WAVEFORM_PLOT_BOTTOM, true);
  screen_driver_draw_line(
      &screen_service.driver, (uint8_t)(SCREEN_WAVEFORM_PLOT_LEFT - 1U),
      SCREEN_WAVEFORM_PLOT_BOTTOM, SCREEN_WAVEFORM_PLOT_RIGHT,
      SCREEN_WAVEFORM_PLOT_BOTTOM, true);
  for (uint8_t x = SCREEN_WAVEFORM_PLOT_LEFT;
       x <= SCREEN_WAVEFORM_PLOT_RIGHT; x = (uint8_t)(x + 4U)) {
    screen_driver_draw_pixel(&screen_service.driver, x,
                             SCREEN_WAVEFORM_PLOT_CENTER, true);
    if (x > (uint8_t)(SCREEN_WAVEFORM_PLOT_RIGHT - 4U)) {
      break;
    }
  }

  for (uint8_t index = 0U; index < count; ++index) {
    if (valid[index]) {
      has_valid = true;
      break;
    }
  }
  if (!has_valid) {
    screen_driver_draw_text(&screen_service.driver, 3U, 52U, "NO DATA",
                            false);
    return;
  }

  {
    const uint8_t first_x =
        (uint8_t)(SCREEN_WAVEFORM_PLOT_RIGHT - count + 1U);
    uint8_t previous_x = 0U;
    uint8_t previous_y = 0U;
    bool previous_valid = false;

    for (uint8_t index = 0U; index < count; ++index) {
      const uint8_t x = (uint8_t)(first_x + index);
      uint8_t top_y;
      uint8_t bottom_y;
      uint8_t center_y;

      if (!valid[index]) {
        previous_valid = false;
        continue;
      }
      top_y = screen_waveform_value_to_y(maximums[index]);
      bottom_y = screen_waveform_value_to_y(minimums[index]);
      center_y = (uint8_t)(((uint16_t)top_y + bottom_y) / 2U);
      screen_driver_draw_line(&screen_service.driver, x, top_y, x, bottom_y,
                              true);
      if (previous_valid) {
        screen_driver_draw_line(&screen_service.driver, previous_x, previous_y,
                                x, center_y, true);
      }
      previous_x = x;
      previous_y = center_y;
      previous_valid = true;
    }
  }
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

static void screen_format_sample_rate(
    vibration_sensor_sample_rate_t sample_rate, char output[8]) {
  uint32_t centi_hz =
      vibration_sensor_sample_rate_centi_hz(sample_rate);
  uint16_t integer_hz = (uint16_t)(centi_hz / 100U);
  uint8_t decimal_hz = (uint8_t)(centi_hz % 100U);

  output[0] =
      (integer_hz >= 1000U)
          ? (char)('0' + ((integer_hz / 1000U) % 10U))
          : ' ';
  output[1] = (char)('0' + ((integer_hz / 100U) % 10U));
  output[2] = (char)('0' + ((integer_hz / 10U) % 10U));
  output[3] = (char)('0' + (integer_hz % 10U));
  output[4] = '.';
  output[5] = (char)('0' + ((decimal_hz / 10U) % 10U));
  output[6] = (char)('0' + (decimal_hz % 10U));
  output[7] = '\0';
}

static uint8_t screen_waveform_value_to_y(int16_t value) {
  int32_t limited_value = value;
  int32_t y;

  if (limited_value > (int32_t)SCREEN_ACCELERATION_MAX_CENTI_G) {
    limited_value = SCREEN_ACCELERATION_MAX_CENTI_G;
  } else if (limited_value < -(int32_t)SCREEN_ACCELERATION_MAX_CENTI_G) {
    limited_value = -(int32_t)SCREEN_ACCELERATION_MAX_CENTI_G;
  }
  y = (int32_t)SCREEN_WAVEFORM_PLOT_CENTER -
      ((limited_value *
        ((int32_t)SCREEN_WAVEFORM_PLOT_CENTER -
         (int32_t)SCREEN_WAVEFORM_PLOT_TOP)) /
       (int32_t)SCREEN_ACCELERATION_MAX_CENTI_G);
  if (y < SCREEN_WAVEFORM_PLOT_TOP) {
    y = SCREEN_WAVEFORM_PLOT_TOP;
  } else if (y > SCREEN_WAVEFORM_PLOT_BOTTOM) {
    y = SCREEN_WAVEFORM_PLOT_BOTTOM;
  }
  return (uint8_t)y;
}

static uint8_t screen_selection_count(screen_page_t page) {
  switch (page) {
  case SCREEN_PAGE_HOME:
    return SCREEN_AXIS_COUNT;
  case SCREEN_PAGE_LIMITS:
    return 6U;
  case SCREEN_PAGE_TRIGGER:
    return 4U;
  case SCREEN_PAGE_SAMPLE_RATE:
    return 1U;
  case SCREEN_PAGE_WAVEFORM:
    return SCREEN_AXIS_COUNT;
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
