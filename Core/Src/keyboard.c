#include "keyboard.h"
#include "FreeRTOS.h"
#include "projdefs.h"
#include "queue.h"
#include "semphr.h"
#include "stdint.h"
#include "stm32f1xx_hal.h"
#include "stdbool.h"

_Static_assert((KEYBOARD_SCAN_PERIOD_MS > 0U) ||  (KEYBOARD_SCAN_PERIOD_MS < 500U),
               "KEYBOARD_SCAN_PERIOD_MS must be between 0 and 500");
_Static_assert((KEYBOARD_PRESS_DEBOUNCE_SAMPLES > 0U) ||  (KEYBOARD_PRESS_DEBOUNCE_SAMPLES < 10U),
               "KEYBOARD_PRESS_DEBOUNCE_SAMPLES must be between 0 and 10");

typedef struct {
  GPIO_TypeDef *gpio_port;
  uint16_t gpio_pin;
  GPIO_PinState pressed_state;
} keyboard_gpio_t;
static const keyboard_gpio_t keyboard_gpio_table[KEYBOARD_KEY_COUNT] = {
    [KEYBOARD_UP_KEY] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_7,
            .pressed_state = GPIO_PIN_RESET,
        },
    [KEYBOARD_DOWN_KEY] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_4,
            .pressed_state = GPIO_PIN_RESET,
        },
    [KEYBOARD_LEFT_KEY] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_6,
            .pressed_state = GPIO_PIN_RESET,
        },
    [KEYBOARD_RIGHT_KEY] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_8,
            .pressed_state = GPIO_PIN_RESET,
        },
    [KEYBOARD_ENTER_KEY] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_5,
            .pressed_state = GPIO_PIN_RESET,
        },
    [KEYBOARD_CANCEL_KEY] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_14,
            .pressed_state = GPIO_PIN_RESET,
        },
};
static QueueHandle_t keyboard_queue = NULL;
static volatile keyboard_state_mask_t keyboard_state_mask = 0U;

typedef struct {
  bool stable_pressed;
  /* 只用于首次按下消抖,松开后重新按下时,该计数器重新开始计数 */
  uint8_t press_debounce_count;
  /* 首次稳定按下的时间 */
  TickType_t pressed_tick;
  /* 上一次发送重复事件的时间 */
  TickType_t last_repeat_tick;
} keyboard_key_runtime_t;

static keyboard_key_runtime_t
    keyboard_key_runtime[KEYBOARD_KEY_COUNT] = {0};

static bool keyboard_read_raw_pressed(enum keyboard key);

static bool keyboard_update_key(
    enum keyboard key,
    TickType_t current_tick,
    struct keyboard_state_msg_t *event);

static void keyboard_task(void *arg);

static keyboard_state_mask_t keyboard_key_to_mask(enum keyboard key);

BaseType_t keyboard_receive(struct keyboard_state_msg_t *msg,
                            TickType_t timeout_ticks) {
  if ((msg == NULL) || (keyboard_queue == NULL)) {
    return pdFAIL;
  }
  return xQueueReceive(keyboard_queue, msg, timeout_ticks);
}
uint32_t get_keyboard_state_mask(void) { return keyboard_state_mask; }
BaseType_t create_keyboard_task(void) {
  keyboard_queue = xQueueCreate(20, sizeof(struct keyboard_state_msg_t));
  if (keyboard_queue == NULL) {
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }
  BaseType_t err =
      xTaskCreate(keyboard_task, "keyboard_task", 64, NULL, 6, NULL);
  if (err != pdPASS) {
    vQueueDelete(keyboard_queue);
    keyboard_queue = NULL;
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }
  return pdPASS;
}

static void keyboard_task(void *arg) {
  const TickType_t scan_period =
      pdMS_TO_TICKS(KEYBOARD_SCAN_PERIOD_MS);

  /* 如果 FreeRTOS Tick 频率太低，10 ms 可能转换为 0 Tick */
  configASSERT(scan_period > 0U);

  TickType_t last_wake_time = xTaskGetTickCount();

  while(1) {
    keyboard_state_mask_t new_state_mask = 0U;

    /* 每个按键在一次扫描中最多产生一条消息 */
    struct keyboard_state_msg_t
        pending_events[KEYBOARD_KEY_COUNT];

    size_t pending_event_count = 0U;

    /* 同一次扫描中的所有按键使用同一个时间戳 */
    const TickType_t current_tick = xTaskGetTickCount();

    for (uint32_t key_value = (uint32_t)KEYBOARD_UP_KEY;
         key_value < (uint32_t)KEYBOARD_KEY_COUNT;
         ++key_value) {
      const enum keyboard key = (enum keyboard)key_value;

      struct keyboard_state_msg_t event;

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

    /* 发布完状态以后再发送事件,不等待队列空位,避免队列满时破坏10ms扫描周期 */
    for (size_t event_index = 0U;
         event_index < pending_event_count;
         ++event_index) {
      const BaseType_t result =
          xQueueSend(keyboard_queue,
                     &pending_events[event_index],
                     0U);

      if (result != pdPASS) {
        /* 队列已满。
         * 可以在这里增加丢失消息计数、错误日志或者断言，
         * 具体策略取决于你的系统要求 */
      }
    }
    vTaskDelayUntil(&last_wake_time, scan_period);
  }
}

static bool keyboard_update_key(
    enum keyboard key,
    TickType_t current_tick,
    struct keyboard_state_msg_t *event) {
  keyboard_key_runtime_t *runtime = &keyboard_key_runtime[key];
  const bool raw_pressed = keyboard_read_raw_pressed(key);

  /* 当前还没有确认按下 */
  if (!runtime->stable_pressed) {
    if (!raw_pressed) {
      /* 没有持续检测到按下，重新开始消抖计数 */
      runtime->press_debounce_count = 0U;
      return false;
    }

    /* 原始输入显示按下，累计连续样本 */
    if (runtime->press_debounce_count <
        KEYBOARD_PRESS_DEBOUNCE_SAMPLES) {
      ++runtime->press_debounce_count;
    }

    if (runtime->press_debounce_count <
        KEYBOARD_PRESS_DEBOUNCE_SAMPLES) {
      return false;
    }

    /* 连续多个样本均为按下，正式接受这次按下 */
    runtime->stable_pressed = true;
    runtime->press_debounce_count = 0U;
    runtime->pressed_tick = current_tick;
    runtime->last_repeat_tick = current_tick;

    event->key = key;
    event->event = KEYBOARD_EVENT_PRESSED;

    return true;
  }

  /* 当前处于稳定按下状态,根据要求,松开不进行消抖,检测到松开后立即接受 */
  if (!raw_pressed) {
    runtime->stable_pressed = false;
    runtime->press_debounce_count = 0U;

    event->key = key;
    event->event = KEYBOARD_EVENT_RELEASED;

    return true;
  }

  /* 按键仍然保持按下，判断是否需要发送重复事件。
   * TickType_t 通常是无符号类型，使用减法计算时间间隔
   * 可以正常处理 Tick 计数器回绕。*/
  const TickType_t held_ticks =
      current_tick - runtime->pressed_tick;

  if (held_ticks < pdMS_TO_TICKS(KEYBOARD_REPEAT_START_MS)) {
    return false;
  }

  TickType_t repeat_period;

  if (held_ticks <
      pdMS_TO_TICKS(KEYBOARD_FAST_REPEAT_START_MS)) {
    repeat_period =
        pdMS_TO_TICKS(KEYBOARD_REPEAT_PERIOD_MS);
  } else {
    repeat_period =
        pdMS_TO_TICKS(KEYBOARD_FAST_REPEAT_PERIOD_MS);
  }

  if ((current_tick - runtime->last_repeat_tick) <
      repeat_period) {
    return false;
  }

  /* 使用当前时间作为新的基准。
   * 如果任务曾经被延迟，不会为了补偿丢失的周期而突然连续
   * 向队列发送大量历史消息。*/
  runtime->last_repeat_tick = current_tick;

  event->key = key;
  event->event = KEYBOARD_EVENT_REPEAT;

  return true;
}
static bool keyboard_read_raw_pressed(enum keyboard key) {
  const keyboard_gpio_t *gpio = &keyboard_gpio_table[key];

  return HAL_GPIO_ReadPin(gpio->gpio_port, gpio->gpio_pin) ==
         gpio->pressed_state;
}
static keyboard_state_mask_t keyboard_key_to_mask(enum keyboard key) {
  return UINT32_C(1) << (uint32_t)key;
}
