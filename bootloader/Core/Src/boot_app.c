#include "boot_app.h"

#include "boot_config.h"
#include "boot_hardware.h"
#include "main.h"
#include "stm32f1xx_hal.h"

typedef void (*boot_application_entry_t)(void);

bool boot_app_is_valid(void) {
  const uint32_t stack_pointer =
      *(const volatile uint32_t *)(uintptr_t)BOOT_APP_START_ADDRESS;
  const uint32_t reset_handler =
      *(const volatile uint32_t *)(uintptr_t)(BOOT_APP_START_ADDRESS + 4U);
  const uint32_t reset_address = reset_handler & ~UINT32_C(1);

  const bool stack_valid =
      (stack_pointer >= BOOT_SRAM_START_ADDRESS) &&
      (stack_pointer <= BOOT_SRAM_END_ADDRESS) &&
      ((stack_pointer & UINT32_C(0x3)) == 0U);
  const bool reset_valid =
      ((reset_handler & UINT32_C(1)) != 0U) &&
      (reset_address >= BOOT_APP_START_ADDRESS) &&
      (reset_address < BOOT_APP_END_ADDRESS);

  return stack_valid && reset_valid;
}

void boot_app_jump(void) {
  const uint32_t stack_pointer =
      *(const volatile uint32_t *)(uintptr_t)BOOT_APP_START_ADDRESS;
  const uint32_t reset_handler =
      *(const volatile uint32_t *)(uintptr_t)(BOOT_APP_START_ADDRESS + 4U);
  const boot_application_entry_t application_entry =
      (boot_application_entry_t)(uintptr_t)reset_handler;

  /*
   * 红色指示灯为低电平点亮。正常应用程序会在初始化 GPIO 时将其熄灭；
   * 如果跳转在此之前发生故障，红灯会保持点亮，以便观察。
   */
  boot_hardware_indicator_set(BOOT_INDICATOR_RED, true);

  /*
   * 只有异常启动路径执行 HAL_Init 后才会启用 TIM4。
   * 快速启动路径在 HAL_Init 前跳转时，不应访问该外设。
   */
  if ((RCC->APB1ENR & RCC_APB1ENR_TIM4EN) != 0U) {
    TIM4->DIER = 0U;
    TIM4->CR1 = 0U;
    TIM4->SR = 0U;
    RCC->APB1ENR &= ~RCC_APB1ENR_TIM4EN;
  }
  NVIC_DisableIRQ(TIM4_IRQn);
  SysTick->CTRL = 0U;
  SysTick->LOAD = 0U;
  SysTick->VAL = 0U;

  __disable_irq();
  for (uint32_t index = 0U; index < 8U; ++index) {
    NVIC->ICER[index] = UINT32_MAX;
    NVIC->ICPR[index] = UINT32_MAX;
  }
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

  SCB->VTOR = BOOT_APP_START_ADDRESS;
  __set_CONTROL(0U);
  __set_BASEPRI(0U);
  __set_FAULTMASK(0U);
  __set_MSP(stack_pointer);
  __DSB();
  __ISB();
  __enable_irq();
  application_entry();

  /* 应用程序的复位处理函数不应返回。 */
  for (;;) {
    boot_hardware_indicator_set(BOOT_INDICATOR_RED, true);
  }
}
