#include "keyboard.h"
#include "FreeRTOS.h"
#include "keyboard_config.h"
#include "queue.h"
#include "stdbool.h"
#include "stdint.h"
#include "stm32f1xx_hal.h"
#include "stm32f1xx_hal_gpio.h"
#include <stddef.h>

typedef struct {
  uint32_t dropped_repeat_events;
  uint32_t dropped_important_events;
} keyboard_event_statistics_t;
static volatile keyboard_event_statistics_t keyboard_event_statistics = {0};

typedef struct {
  bool stable_pressed;
  /* 用于首次按下和松开的防抖计数 */
  uint8_t debounce_count;
  /* 首次稳定按下的时间 */
  TickType_t pressed_tick;
  /* 上一次发送重复事件的时间 */
  TickType_t last_repeat_tick;
} keyboard_key_runtime_t;
static keyboard_key_runtime_t keyboard_key_runtime[KEYBOARD_KEY_COUNT] = {0};

typedef struct {
  GPIO_TypeDef *gpio_port;
  uint16_t gpio_pin;
  GPIO_PinState pressed_state; // 被按下时的电平
  bool repeat_enabled;         // 是否允许报告重复事件
} keyboard_key_config_t;
static const keyboard_key_config_t
    keyboard_key_config_table[KEYBOARD_KEY_COUNT] = {
        [KEYBOARD_KEY_UP] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_7,
                .pressed_state = GPIO_PIN_RESET,
                .repeat_enabled = true,
            },
        [KEYBOARD_KEY_DOWN] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_4,
                .pressed_state = GPIO_PIN_RESET,
                .repeat_enabled = true,
            },
        [KEYBOARD_KEY_LEFT] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_6,
                .pressed_state = GPIO_PIN_RESET,
                .repeat_enabled = true,
            },
        [KEYBOARD_KEY_RIGHT] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_8,
                .pressed_state = GPIO_PIN_RESET,
                .repeat_enabled = true,
            },
        [KEYBOARD_KEY_ENTER] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_5,
                .pressed_state = GPIO_PIN_RESET,
                .repeat_enabled = false,
            },
        [KEYBOARD_KEY_CANCEL] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_14,
                .pressed_state = GPIO_PIN_RESET,
                .repeat_enabled = false,
            },
};
static QueueHandle_t keyboard_queue = NULL;
static keyboard_state_mask_t keyboard_state_mask = 0U;

static bool keyboard_read_raw_pressed(keyboard_key_t key);
static bool keyboard_update_key(keyboard_key_t key, TickType_t current_tick,
                                keyboard_event_t *event);
static void keyboard_task(void *arg);
static keyboard_state_mask_t keyboard_key_to_mask(keyboard_key_t key);
static bool keyboard_event_is_important(const keyboard_event_t *event);
static BaseType_t keyboard_try_send_event(const keyboard_event_t *event);

BaseType_t keyboard_init(void) {
  /* 批量检测宏定义转换为ticks后是否合法 */
  const TickType_t scan_period = pdMS_TO_TICKS(KEYBOARD_SCAN_PERIOD_MS);
  const TickType_t repeat_start = pdMS_TO_TICKS(KEYBOARD_REPEAT_START_MS);
  const TickType_t fast_repeat_start =
      pdMS_TO_TICKS(KEYBOARD_FAST_REPEAT_START_MS);
  const TickType_t repeat_period = pdMS_TO_TICKS(KEYBOARD_REPEAT_PERIOD_MS);
  const TickType_t fast_repeat_period =
      pdMS_TO_TICKS(KEYBOARD_FAST_REPEAT_PERIOD_MS);
  if ((scan_period == 0U) || (repeat_start == 0U) ||
      (fast_repeat_start == 0U) || (repeat_period == 0U) ||
      (fast_repeat_period == 0U)) {
    return pdFAIL;
  }

  if (keyboard_queue != NULL) {
    return pdPASS;
  }
  keyboard_queue =
      xQueueCreate(KEYBOARD_EVENT_QUEUE_LENGTH, sizeof(keyboard_event_t));
  if (keyboard_queue == NULL) {
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }
  BaseType_t err =
      xTaskCreate(keyboard_task, "keyboard_task", KEYBOARD_TASK_STACK_DEPTH,
                  NULL, KEYBOARD_TASK_PRIORITY, NULL);
  if (err != pdPASS) {
    vQueueDelete(keyboard_queue);
    keyboard_queue = NULL;
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }
  return pdPASS;
}
BaseType_t keyboard_receive_event(keyboard_event_t *event,
                                  TickType_t timeout_ticks) {
  if ((event == NULL) || (keyboard_queue == NULL)) {
    return pdFAIL;
  }
  return xQueueReceive(keyboard_queue, event, timeout_ticks);
}
keyboard_state_mask_t keyboard_get_state_mask(void) {
  keyboard_state_mask_t snapshot;

  taskENTER_CRITICAL();
  snapshot = keyboard_state_mask;
  taskEXIT_CRITICAL();

  return snapshot;
}
BaseType_t keyboard_state_mask_pop_key(keyboard_state_mask_t *mask,
                                       keyboard_key_t *key) {
  if ((mask == NULL) || (key == NULL)) {
    return pdFALSE;
  }

  for (uint32_t key_value = (uint32_t)KEYBOARD_KEY_UP;
       key_value < (uint32_t)KEYBOARD_KEY_COUNT; ++key_value) {
    const keyboard_state_mask_t key_mask = UINT32_C(1) << key_value;

    if ((*mask & key_mask) == 0U) {
      continue;
    }

    *key = (keyboard_key_t)key_value;

    /* 清除已经取出的按键位 */
    *mask &= ~key_mask;

    return pdTRUE;
  }

  return pdFALSE;
}
bool keyboard_is_pressed(keyboard_key_t key) {
  if ((uint32_t)key >= (uint32_t)KEYBOARD_KEY_COUNT) {
    return false;
  }

  return (keyboard_get_state_mask() & keyboard_key_to_mask(key)) != 0U;
}

static bool keyboard_read_raw_pressed(keyboard_key_t key) {
  const keyboard_key_config_t *gpio = &keyboard_key_config_table[key];

  return HAL_GPIO_ReadPin(gpio->gpio_port, gpio->gpio_pin) ==
         gpio->pressed_state;
}
static bool keyboard_update_key(keyboard_key_t key, TickType_t current_tick,
                                keyboard_event_t *event) {
  keyboard_key_runtime_t *runtime = &keyboard_key_runtime[key];
  const bool raw_pressed = keyboard_read_raw_pressed(key);

  /*
   * 原始状态与稳定状态不同，表示可能正在发生状态转换。
   */
  if (raw_pressed != runtime->stable_pressed) {
    const uint8_t required_samples =
        raw_pressed ? (uint8_t)KEYBOARD_PRESS_DEBOUNCE_SAMPLES
                    : (uint8_t)KEYBOARD_RELEASE_DEBOUNCE_SAMPLES;

    if (runtime->debounce_count < required_samples) {
      ++runtime->debounce_count;
    }

    if (runtime->debounce_count < required_samples) {
      return false;
    }

    runtime->debounce_count = 0U;
    runtime->stable_pressed = raw_pressed;

    event->key = key;

    if (raw_pressed) {
      runtime->pressed_tick = current_tick;
      runtime->last_repeat_tick = current_tick;
      event->type = KEYBOARD_EVENT_PRESSED;
    } else {
      event->type = KEYBOARD_EVENT_RELEASED;
    }

    return true;
  }

  /*
   * 原始状态重新回到稳定状态，之前累计的消抖样本作废。
   */
  runtime->debounce_count = 0U;

  if (!runtime->stable_pressed) {
    return false;
  }

  if (!keyboard_key_config_table[key].repeat_enabled) {
    return false;
  }

  const TickType_t held_ticks = current_tick - runtime->pressed_tick;

  if (held_ticks < pdMS_TO_TICKS(KEYBOARD_REPEAT_START_MS)) {
    return false;
  }

  const TickType_t repeat_period =
      held_ticks < pdMS_TO_TICKS(KEYBOARD_FAST_REPEAT_START_MS)
          ? pdMS_TO_TICKS(KEYBOARD_REPEAT_PERIOD_MS)
          : pdMS_TO_TICKS(KEYBOARD_FAST_REPEAT_PERIOD_MS);

  if ((current_tick - runtime->last_repeat_tick) < repeat_period) {
    return false;
  }

  runtime->last_repeat_tick = current_tick;

  event->key = key;
  event->type = KEYBOARD_EVENT_REPEAT;

  return true;
}
static void keyboard_task(void *arg) {
  (void)arg;
  const TickType_t scan_period = pdMS_TO_TICKS(KEYBOARD_SCAN_PERIOD_MS);

  TickType_t last_wake_time = xTaskGetTickCount();

  while (1) {
    keyboard_state_mask_t new_state_mask = 0U;

    /* 每个按键在一次扫描中最多产生一条消息 */
    keyboard_event_t pending_events[KEYBOARD_KEY_COUNT];

    size_t pending_event_count = 0U;

    /* 同一次扫描中的所有按键使用同一个时间戳 */
    const TickType_t current_tick = xTaskGetTickCount();

    for (uint32_t key_value = (uint32_t)KEYBOARD_KEY_UP;
         key_value < (uint32_t)KEYBOARD_KEY_COUNT; ++key_value) {
      const keyboard_key_t key = (keyboard_key_t)key_value;

      keyboard_event_t event;

      if (keyboard_update_key(key, current_tick, &event)) {
        pending_events[pending_event_count] = event;
        ++pending_event_count;
      }

      if (keyboard_key_runtime[key].stable_pressed) {
        new_state_mask |= keyboard_key_to_mask(key);
      }
    }

    /* 先发布最新状态。
     * 这样高优先级任务收到队列消息后，再读取状态掩码时，
     * 得到的是与该事件相对应的新状态 */
    taskENTER_CRITICAL();
    keyboard_state_mask = new_state_mask;
    taskEXIT_CRITICAL();

    /* 第一轮先发送 PRESSED 和 RELEASED */
    for (size_t event_index = 0U; event_index < pending_event_count;
         ++event_index) {
      const keyboard_event_t *event = &pending_events[event_index];
      if (!keyboard_event_is_important(event)) {
        continue;
      }
      (void)keyboard_try_send_event(event);
    }
    /* 第二轮发送
     * REPEAT,当只剩预留空间时,keyboard_try_send_event(),会自动丢弃重复事件 */
    for (size_t event_index = 0U; event_index < pending_event_count;
         ++event_index) {
      const keyboard_event_t *event = &pending_events[event_index];
      if (keyboard_event_is_important(event)) {
        continue;
      }
      (void)keyboard_try_send_event(event);
    }

    vTaskDelayUntil(&last_wake_time, scan_period);
  }
}
static keyboard_state_mask_t keyboard_key_to_mask(keyboard_key_t key) {
  return UINT32_C(1) << (uint32_t)key;
}
static bool keyboard_event_is_important(const keyboard_event_t *event) {
  return (event->type == KEYBOARD_EVENT_PRESSED) ||
         (event->type == KEYBOARD_EVENT_RELEASED);
}
static BaseType_t keyboard_try_send_event(const keyboard_event_t *event) {
  if ((event == NULL) || (keyboard_queue == NULL)) {
    return pdFAIL;
  }

  const UBaseType_t available_spaces = uxQueueSpacesAvailable(keyboard_queue);

  /*
   * PRESSED 和 RELEASED 是重要事件，可以使用所有剩余空间。
   */
  if (keyboard_event_is_important(event)) {
    if (available_spaces == 0U) {
      ++keyboard_event_statistics.dropped_important_events;
      return errQUEUE_FULL;
    }

    const BaseType_t result = xQueueSend(keyboard_queue, event, 0U);

    if (result != pdPASS) {
      ++keyboard_event_statistics.dropped_important_events;
    }

    return result;
  }

  /*
   * REPEAT 事件不能使用为重要事件预留的位置。
   *
   * 例如预留 5 个位置：
   * available_spaces == 6 时允许发送；
   * available_spaces == 5 时禁止发送。
   */
  if (available_spaces <= KEYBOARD_IMPORTANT_EVENT_RESERVED_SLOTS) {
    ++keyboard_event_statistics.dropped_repeat_events;
    return errQUEUE_FULL;
  }

  const BaseType_t result = xQueueSend(keyboard_queue, event, 0U);

  if (result != pdPASS) {
    ++keyboard_event_statistics.dropped_repeat_events;
  }

  return result;
}
