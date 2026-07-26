#include "indicator.h"
#include "FreeRTOS.h"
#include "projdefs.h"
#include "queue.h"
#include "stm32f103xb.h"
#include "stm32f1xx_hal.h"
#include "stm32f1xx_hal_gpio.h"

_Static_assert(INDICATOR_DEVICE_COUNT <= 32U,
               "Indicator state mask only supports up to 32 devices");

#define INDICATOR_QUEUE_LENGTH 10U
#define INDICATOR_TASK_STACK_DEPTH 64U
#define INDICATOR_TASK_PRIORITY (tskIDLE_PRIORITY + 4U)

#define INDICATOR_VALID_MASK                                                   \
  ((UINT32_C(1) << INDICATOR_DEVICE_COUNT) - UINT32_C(1))

enum indicator_command {
  indicator_command_set_mask,
  indicator_command_set_device,
};
struct indicator_msg_t {
  enum indicator_command command;
  union {
    uint32_t state_mask; // 每一位控制一个设备，置1为激活
    struct {
      indicator_device_t device_id;
      bool target_state; // true为激活
      uint32_t
          duration_ms; // 激活持续时间(ms)，target_state=true时生效，0为永久激活
    } single;
  } data;
};
typedef struct {
  GPIO_TypeDef *gpio_port;
  uint16_t gpio_pin;
  GPIO_PinState active_state;
} indicator_device_config_t;
static const indicator_device_config_t
    indicator_device_table[INDICATOR_DEVICE_COUNT] = {
        [INDICATOR_DEVICE_ALARM_LED_X] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_0,
                .active_state = GPIO_PIN_RESET,
            },
        [INDICATOR_DEVICE_ALARM_LED_Y] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_9,
                .active_state = GPIO_PIN_RESET,
            },
        [INDICATOR_DEVICE_ALARM_LED_Z] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_1,
                .active_state = GPIO_PIN_RESET,
            },
        [INDICATOR_DEVICE_STATUS_LED_RED] =
            {
                .gpio_port = GPIOA,
                .gpio_pin = GPIO_PIN_7,
                .active_state = GPIO_PIN_RESET,
            },
        [INDICATOR_DEVICE_STATUS_LED_GREEN] =
            {
                .gpio_port = GPIOA,
                .gpio_pin = GPIO_PIN_6,
                .active_state = GPIO_PIN_RESET,
            },
        [INDICATOR_DEVICE_STATUS_LED_BLUE] =
            {
                .gpio_port = GPIOA,
                .gpio_pin = GPIO_PIN_5,
                .active_state = GPIO_PIN_RESET,
            },
        [INDICATOR_DEVICE_BUZZER] =
            {
                .gpio_port = GPIOA,
                .gpio_pin = GPIO_PIN_4,
                .active_state = GPIO_PIN_SET,
            },
};
static QueueHandle_t indicator_queue = NULL;
static TickType_t indicator_time_left[INDICATOR_DEVICE_COUNT] = {
    0}; // 用于管理每个设备的超时情况

static void indicator_task(void *arg);
static bool indicator_build_mask_msg(struct indicator_msg_t *msg,
                                     uint32_t state_mask);
static bool indicator_build_device_msg(struct indicator_msg_t *msg,
                                       indicator_device_t device_id,
                                       bool target_state, uint32_t duration_ms);
static void indicator_apply_state_mask(uint32_t state_mask);
static void indicator_set_device_state(indicator_device_t device_id,
                                       bool target_state, uint32_t duration_ms);
static void indicator_update_timers(TickType_t elapsed_ticks);
static TickType_t indicator_get_next_timeout(void);

BaseType_t indicator_init(void) {
  /* 队列和任务只能创建一次 */
  if (indicator_queue != NULL) {
    return pdFALSE;
  }
  indicator_queue =
      xQueueCreate(INDICATOR_QUEUE_LENGTH, sizeof(struct indicator_msg_t));
  if (indicator_queue == NULL) {
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }
  BaseType_t err =
      xTaskCreate(indicator_task, "indicator_task", INDICATOR_TASK_STACK_DEPTH,
                  NULL, INDICATOR_TASK_PRIORITY, NULL);
  if (err != pdPASS) {
    vQueueDelete(indicator_queue);
    indicator_queue = NULL;
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }
  return pdPASS;
}
BaseType_t indicator_set_mask(uint32_t state_mask, TickType_t wait_ticks) {
  struct indicator_msg_t msg;

  if (indicator_queue == NULL) {
    return pdFAIL;
  }
  if (indicator_build_mask_msg(&msg, state_mask) == false) {
    return pdFAIL;
  }
  return xQueueSend(indicator_queue, &msg, wait_ticks);
}
BaseType_t indicator_set_mask_from_isr(uint32_t state_mask,
                                       BaseType_t *higher_priority_task_woken) {
  struct indicator_msg_t msg;
  if (indicator_queue == NULL) {
    return pdFAIL;
  }
  if (indicator_build_mask_msg(&msg, state_mask) == false) {
    return pdFAIL;
  }
  return xQueueSendFromISR(indicator_queue, &msg, higher_priority_task_woken);
}
BaseType_t indicator_set_device(indicator_device_t device_id, bool target_state,
                                uint32_t duration_ms, TickType_t wait_ticks) {
  struct indicator_msg_t msg;
  if (indicator_queue == NULL) {
    return pdFAIL;
  }
  if (indicator_build_device_msg(&msg, device_id, target_state, duration_ms) ==
      false) {
    return pdFAIL;
  }
  return xQueueSend(indicator_queue, &msg, wait_ticks);
}
BaseType_t
indicator_set_device_from_isr(indicator_device_t device_id, bool target_state,
                              uint32_t duration_ms,
                              BaseType_t *higher_priority_task_woken) {
  struct indicator_msg_t msg;
  if (indicator_queue == NULL) {
    return pdFAIL;
  }
  if (indicator_build_device_msg(&msg, device_id, target_state, duration_ms) ==
      false) {
    return pdFAIL;
  }
  return xQueueSendFromISR(indicator_queue, &msg, higher_priority_task_woken);
}
static void indicator_task(void *arg) {
  struct indicator_msg_t indicator_order;
  TickType_t previous_tick = xTaskGetTickCount();

  (void)arg;
  while (1) {
    /* 没有定时设备时为portMAX_DELAY,有定时设备时为最近的到期时间 */
    TickType_t wait_ticks = indicator_get_next_timeout();
    BaseType_t received =
        xQueueReceive(indicator_queue, &indicator_order, wait_ticks);
    /* 无论是收到消息,还是等待超时,都先计算实际经过的时间 */
    TickType_t current_tick = xTaskGetTickCount();

    TickType_t elapsed_ticks = current_tick - previous_tick;

    previous_tick = current_tick;

    /* 关闭已经到期的设备,并更新其他设备剩余时间 */
    indicator_update_timers(elapsed_ticks);

    if (received != pdPASS) {
      /* 没有收到消息,说明是某个定时器到期 */
      continue;
    }

    switch (indicator_order.command) {
    case indicator_command_set_mask:
      indicator_apply_state_mask(indicator_order.data.state_mask);
      break;

    case indicator_command_set_device:
      indicator_set_device_state(indicator_order.data.single.device_id,
                                 indicator_order.data.single.target_state,
                                 indicator_order.data.single.duration_ms);
      break;

    default:
      break;
    }
  }
}
static bool indicator_build_mask_msg(struct indicator_msg_t *msg,
                                     uint32_t state_mask) {
  if (msg == NULL) {
    return false;
  }
  *msg = (struct indicator_msg_t){
      .command = indicator_command_set_mask,
      /* 清除无效的高位,避免调用方误控制不存在的设备 */
      .data.state_mask = state_mask & INDICATOR_VALID_MASK,
  };
  return true;
}
static bool indicator_build_device_msg(struct indicator_msg_t *msg,
                                       indicator_device_t device_id,
                                       bool target_state,
                                       uint32_t duration_ms) {
  if (msg == NULL) {
    return false;
  }
  if ((device_id < INDICATOR_DEVICE_ALARM_LED_X) ||
      (device_id >= INDICATOR_DEVICE_COUNT)) {
    return false;
  }
  *msg = (struct indicator_msg_t){
      .command = indicator_command_set_device,
      .data.single =
          {
              .device_id = device_id,
              .target_state = target_state,
              .duration_ms = duration_ms,
          },
  };
  return true;
}
/* 使用掩码控制全部指示设备并清除超时 */
static void indicator_write_state(indicator_device_t device_id,
                                  bool target_state) {
  if ((device_id < INDICATOR_DEVICE_ALARM_LED_X) ||
      (device_id >= INDICATOR_DEVICE_COUNT)) {
    return;
  }

  const indicator_device_config_t *device = &indicator_device_table[device_id];

  GPIO_PinState output_state;

  if (target_state == true) {
    output_state = device->active_state;
  } else {
    output_state =
        (device->active_state == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET;
  }
  HAL_GPIO_WritePin(device->gpio_port, device->gpio_pin, output_state);
}
static void indicator_apply_state_mask(uint32_t state_mask) {
  state_mask &= INDICATOR_VALID_MASK;

  for (uint32_t device = 0U; device < (uint32_t)INDICATOR_DEVICE_COUNT;
       ++device) {
    const indicator_device_t device_id = (indicator_device_t)device;
    const bool target_state = (state_mask & INDICATOR_MASK(device_id)) != 0U;

    /* 掩码命令覆盖之前的单设备定时命令。 */
    indicator_time_left[device_id] = 0U;
    indicator_write_state(device_id, target_state);
  }
}
/* 设置单个设备的状态以及超时 */
static void indicator_set_device_state(indicator_device_t device_id,
                                       bool target_state,
                                       uint32_t duration_ms) {
  if ((device_id < INDICATOR_DEVICE_ALARM_LED_X) ||
      (device_id >= INDICATOR_DEVICE_COUNT)) {
    return;
  }
  indicator_write_state(device_id, target_state);

  /*
   * 任何新指令都先取消之前的计时。
   * 如果后面需要定时，再设置新的剩余时间。
   */
  indicator_time_left[device_id] = 0;

  if (target_state && (duration_ms > 0U)) {
    TickType_t duration_ticks = pdMS_TO_TICKS(duration_ms);

    /*
     * 当duration_ms小于一个系统Tick时，
     * pdMS_TO_TICKS可能得到0，这里至少等待一个Tick。
     */
    if (duration_ticks == 0U) {
      duration_ticks = 1U;
    }

    indicator_time_left[device_id] = duration_ticks;
  }
}
/* 更新所有的超时剩余时间 */
static void indicator_update_timers(TickType_t elapsed_ticks) {
  for (uint8_t device = INDICATOR_DEVICE_ALARM_LED_X;
       device < INDICATOR_DEVICE_COUNT; ++device) {

    TickType_t time_left = indicator_time_left[device];

    if (time_left == 0U) {
      continue;
    }
    if (elapsed_ticks >= time_left) {
      indicator_time_left[device] = 0U;
      indicator_write_state((indicator_device_t)device, false);
    } else {
      indicator_time_left[device] = time_left - elapsed_ticks;
    }
  }
}
/* 寻找最近一个要超时的设备 */
static TickType_t indicator_get_next_timeout(void) {
  TickType_t next_timeout = portMAX_DELAY;
  for (uint8_t device = INDICATOR_DEVICE_ALARM_LED_X;
       device < INDICATOR_DEVICE_COUNT; device++) {
    TickType_t time_left = indicator_time_left[device];
    if ((time_left > 0U) && (time_left < next_timeout)) {
      next_timeout = time_left;
    }
  }
  return next_timeout;
}
