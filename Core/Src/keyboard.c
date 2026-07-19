#include "keyboard.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "stdint.h"
#include "stm32f1xx_hal.h"

typedef struct {
  GPIO_TypeDef *gpio_port;
  uint16_t gpio_pin;
  GPIO_PinState pressed_state;
} keyboard_gpio_t;
static const keyboard_gpio_t keyboard_gpio_table[keyboard_key_count] = {
    [up_key] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_7,
            .pressed_state = GPIO_PIN_RESET,
        },
    [down_key] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_4,
            .pressed_state = GPIO_PIN_RESET,
        },
    [left_key] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_6,
            .pressed_state = GPIO_PIN_RESET,
        },
    [right_key] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_8,
            .pressed_state = GPIO_PIN_RESET,
        },
    [enter_key] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_5,
            .pressed_state = GPIO_PIN_RESET,
        },
    [cancel_key] =
        {
            .gpio_port = GPIOB,
            .gpio_pin = GPIO_PIN_14,
            .pressed_state = GPIO_PIN_RESET,
        },
};
static QueueHandle_t keyboard_queue = NULL;
static uint32_t keyboard_state_mask = 0U;
static struct keyboard_state_msg_t current_key_state;

static void keyboard_check_and_report_key(enum keyboard which_key);
static void keyboard_task(void *arg);

QueueHandle_t get_keyboard_queue(void) { return keyboard_queue; }
uint32_t get_keyboard_state_mask(void) { return keyboard_state_mask; }
BaseType_t create_keyboard_task(void) {
  keyboard_queue = xQueueCreate(20, sizeof(struct keyboard_state_msg_t));
  if (keyboard_queue == NULL) {
    return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
  }
  return xTaskCreate(keyboard_task, "keyboard_task", 64, NULL, 6, NULL);
}

static void keyboard_check_and_report_key(enum keyboard which_key) {
  if (HAL_GPIO_ReadPin(keyboard_gpio_table[which_key].gpio_port,
                       keyboard_gpio_table[which_key].gpio_pin) ==
      keyboard_gpio_table[which_key].pressed_state) {
    keyboard_state_mask |= (1UL << which_key);
    current_key_state.pressed_keys = which_key;
    xQueueSend(keyboard_queue, &current_key_state, pdMS_TO_TICKS(50));
  }else{
    keyboard_state_mask &= ~(1UL << which_key);
  }
}
static void keyboard_task(void *arg) {
  TickType_t last_wake_time;
  const TickType_t scan_period = pdMS_TO_TICKS(1000 / KEYBOARD_SCAN_RATE_HZ);
  last_wake_time = xTaskGetTickCount();
  while (1) {
    vTaskDelayUntil(&last_wake_time, scan_period);
    for(uint8_t key = up_key; key < keyboard_key_count ; key++){
        keyboard_check_and_report_key(key);
    }
  }
}