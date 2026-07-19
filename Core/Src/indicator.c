#include "indicator.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "stm32f103xb.h"
#include "stm32f1xx_hal.h"
#include "stm32f1xx_hal_gpio.h"

typedef struct {
  GPIO_TypeDef *gpio_port;
  uint16_t gpio_pin;
  GPIO_PinState active_state;
} indicator_device_t;
static const indicator_device_t indicator_device_table[indicator_device_count] =
    {
        [alarm_led_x] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_0,
                .active_state = GPIO_PIN_RESET,
            },
        [alarm_led_y] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_9,
                .active_state = GPIO_PIN_RESET,
            },
        [alarm_led_z] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_1,
                .active_state = GPIO_PIN_RESET,
            },
        [status_led_red] =
            {
                .gpio_port = GPIOA,
                .gpio_pin = GPIO_PIN_7,
                .active_state = GPIO_PIN_RESET,
            },
        [status_led_green] =
            {
                .gpio_port = GPIOA,
                .gpio_pin = GPIO_PIN_6,
                .active_state = GPIO_PIN_RESET,
            },
        [status_led_blue] =
            {
                .gpio_port = GPIOA,
                .gpio_pin = GPIO_PIN_5,
                .active_state = GPIO_PIN_RESET,
            },
        [buzzer] =
            {
                .gpio_port = GPIOA,
                .gpio_pin = GPIO_PIN_4,
                .active_state = GPIO_PIN_SET,
            },
};
static QueueHandle_t indicator_queue = NULL;
static TickType_t indicator_time_left[indicator_device_count] = {0};    //用于管理每个设备的超时情况

static void indicator_task(void *arg);
static void indicator_apply_state_mask(uint32_t state_mask);
static void indicator_set_device_state(enum indicator_device device_id,
                                       bool target_state, uint32_t duration_ms);
static void indicator_update_timers(TickType_t elapsed_ticks);
static TickType_t indicator_get_next_timeout(void);

QueueHandle_t get_indicator_queue(void) { return indicator_queue; }
BaseType_t create_indicator_task(void) {
  indicator_queue = xQueueCreate(10, sizeof(struct indicator_msg_t));
  if (indicator_queue == NULL) {
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }
  return xTaskCreate(indicator_task, "indicator_task", 64, NULL, 6, NULL);
}

static void indicator_task(void *arg) {
  struct indicator_msg_t indicator_order;
  TickType_t previous_tick = xTaskGetTickCount();
  while(1) {
    /* 没有定时设备时为portMAX_DELAY,有定时设备时为最近的到期时间 */
    TickType_t wait_ticks = indicator_get_next_timeout();
    BaseType_t received = xQueueReceive(indicator_queue, &indicator_order, wait_ticks);
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

    if (indicator_order.use_mask) {
      indicator_apply_state_mask(indicator_order.data.state_mask);
    } else {
      indicator_set_device_state(indicator_order.data.single.device_id,
                                 indicator_order.data.single.target_state,
                                 indicator_order.data.single.duration_ms);
    }
  }
}
/* 使用掩码控制全部指示设备并清除超时 */
static void indicator_write_state(enum indicator_device device_id,
                                  bool target_state) {
  if ((device_id < alarm_led_x) || (device_id >= indicator_device_count)) {
    return;
  }

  const indicator_device_t *device = &indicator_device_table[device_id];

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
  for (uint8_t device = alarm_led_x; device < indicator_device_count;
       device++) {
    if (state_mask & (1UL << device)) {
      HAL_GPIO_WritePin(indicator_device_table[device].gpio_port,
                        indicator_device_table[device].gpio_pin,
                        indicator_device_table[device].active_state);
    } else if (indicator_device_table[device].active_state == GPIO_PIN_SET) {
      HAL_GPIO_WritePin(indicator_device_table[device].gpio_port,
                        indicator_device_table[device].gpio_pin,
                        GPIO_PIN_RESET);
    } else {
      HAL_GPIO_WritePin(indicator_device_table[device].gpio_port,
                        indicator_device_table[device].gpio_pin, GPIO_PIN_SET);
    }
  }
}
/* 设置单个设备的状态以及超时 */
static void indicator_set_device_state(enum indicator_device device_id,
                                       bool target_state,
                                       uint32_t duration_ms) {
  if ((device_id < alarm_led_x) || (device_id >= indicator_device_count)) {
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
  for (uint8_t device = alarm_led_x; device < indicator_device_count;
       ++device) {

    TickType_t time_left = indicator_time_left[device];

    if (time_left == 0U) {
      continue;
    }
    if (elapsed_ticks >= time_left) {
      indicator_time_left[device] = 0U;
      indicator_write_state((enum indicator_device)device, false);
    } else {
      indicator_time_left[device] = time_left - elapsed_ticks;
    }
  }
}
/* 寻找最近一个要超时的设备 */
static TickType_t indicator_get_next_timeout(void) {
  TickType_t next_timeout = portMAX_DELAY;
  for (uint8_t device = alarm_led_x; device < indicator_device_count; device++) {
    TickType_t time_left = indicator_time_left[device];
    if ((time_left > 0U) && (time_left < next_timeout)) {
      next_timeout = time_left;
    }
  }
  return next_timeout;
}