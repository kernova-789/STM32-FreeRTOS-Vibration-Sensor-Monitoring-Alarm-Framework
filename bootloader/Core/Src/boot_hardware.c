#include "boot_hardware.h"

#include "stm32f1xx_hal.h"
#include <stdint.h>

typedef struct {
  GPIO_TypeDef *gpio_port;
  uint16_t gpio_pin;
  GPIO_PinState pressed_state;
  uint32_t pull;
} boot_button_config_t;

static const boot_button_config_t
    boot_button_config_table[BOOT_BUTTON_COUNT] = {
        [BOOT_BUTTON_ENTER] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_5,
                .pressed_state = GPIO_PIN_RESET,
                .pull = GPIO_PULLUP,
            },
        [BOOT_BUTTON_CANCEL] =
            {
                .gpio_port = GPIOB,
                .gpio_pin = GPIO_PIN_14,
                .pressed_state = GPIO_PIN_RESET,
                .pull = GPIO_PULLUP,
            },
};

typedef struct {
  GPIO_TypeDef *gpio_port;
  uint16_t gpio_pin;
  GPIO_PinState active_state;
} boot_indicator_config_t;

static const boot_indicator_config_t
    boot_indicator_config_table[BOOT_INDICATOR_COUNT] = {
        [BOOT_INDICATOR_RED] =
            {
                .gpio_port = GPIOA,
                .gpio_pin = GPIO_PIN_7,
                .active_state = GPIO_PIN_RESET,
            },
        [BOOT_INDICATOR_BLUE] =
            {
                .gpio_port = GPIOA,
                .gpio_pin = GPIO_PIN_5,
                .active_state = GPIO_PIN_RESET,
            },
};

static bool boot_hardware_button_valid(boot_button_t button);
static bool boot_hardware_indicator_valid(boot_indicator_t indicator);
static GPIO_PinState
boot_hardware_inactive_state(GPIO_PinState active_state);

void boot_hardware_init_fast_boot(void) {
  GPIO_InitTypeDef gpio = {0};
  const boot_indicator_config_t *red =
      &boot_indicator_config_table[BOOT_INDICATOR_RED];
  const boot_button_config_t *enter =
      &boot_button_config_table[BOOT_BUTTON_ENTER];

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*
   * 正常启动路径只配置启动决策和跳转失败指示所需的硬件，
   * 其余板级 GPIO 延后到异常启动路径中统一初始化。
   */
  HAL_GPIO_WritePin(red->gpio_port, red->gpio_pin,
                    boot_hardware_inactive_state(red->active_state));
  gpio.Pin = red->gpio_pin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(red->gpio_port, &gpio);

  gpio.Pin = enter->gpio_pin;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = enter->pull;
  HAL_GPIO_Init(enter->gpio_port, &gpio);
}

bool boot_hardware_button_is_pressed(boot_button_t button) {
  const boot_button_config_t *config;

  if (!boot_hardware_button_valid(button)) {
    return false;
  }
  config = &boot_button_config_table[button];
  return HAL_GPIO_ReadPin(config->gpio_port, config->gpio_pin) ==
         config->pressed_state;
}

void boot_hardware_indicator_set(boot_indicator_t indicator, bool enabled) {
  const boot_indicator_config_t *config;
  GPIO_PinState output_state;

  if (!boot_hardware_indicator_valid(indicator)) {
    return;
  }
  config = &boot_indicator_config_table[indicator];
  output_state =
      enabled ? config->active_state
              : boot_hardware_inactive_state(config->active_state);
  HAL_GPIO_WritePin(config->gpio_port, config->gpio_pin, output_state);
}

static bool boot_hardware_button_valid(boot_button_t button) {
  return ((uint32_t)button < (uint32_t)BOOT_BUTTON_COUNT);
}

static bool boot_hardware_indicator_valid(boot_indicator_t indicator) {
  return ((uint32_t)indicator < (uint32_t)BOOT_INDICATOR_COUNT);
}

static GPIO_PinState
boot_hardware_inactive_state(GPIO_PinState active_state) {
  return (active_state == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET;
}
