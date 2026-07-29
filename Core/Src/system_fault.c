#include "system_fault.h"

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f103xb.h"

/*
 * 状态蓝灯连接到 PA5，低电平点亮。
 * 故障时不调用 indicator 模块，因为此时调度器、任务栈或动态内存
 * 可能已经失效，无法再安全地通过消息队列控制指示灯。
 */
#define SYSTEM_FAULT_BLUE_LED_PIN 5U
#define SYSTEM_FAULT_GREEN_LED_PIN 6U
#define SYSTEM_FAULT_RED_LED_PIN 7U
#define SYSTEM_FAULT_STATUS_LED_MODE_MASK                                     \
  ((UINT32_C(0x0F) << (SYSTEM_FAULT_BLUE_LED_PIN * 4U)) |                     \
   (UINT32_C(0x0F) << (SYSTEM_FAULT_GREEN_LED_PIN * 4U)) |                    \
   (UINT32_C(0x0F) << (SYSTEM_FAULT_RED_LED_PIN * 4U)))
#define SYSTEM_FAULT_STATUS_LED_OUTPUT_MODE                                   \
  ((UINT32_C(0x01) << (SYSTEM_FAULT_BLUE_LED_PIN * 4U)) |                     \
   (UINT32_C(0x01) << (SYSTEM_FAULT_GREEN_LED_PIN * 4U)) |                    \
   (UINT32_C(0x01) << (SYSTEM_FAULT_RED_LED_PIN * 4U)))

static void system_fault_turn_on_blue_led(void) {
  /*
   * 即使故障发生在 MX_GPIO_Init() 之前，也能直接初始化状态灯：
   * PA5 拉低点亮蓝灯，PA6/PA7 拉高熄灭绿灯和红灯。
   */
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
  GPIOA->CRL =
      (GPIOA->CRL & ~SYSTEM_FAULT_STATUS_LED_MODE_MASK) |
      SYSTEM_FAULT_STATUS_LED_OUTPUT_MODE;
  GPIOA->BSRR = GPIO_BSRR_BS6 | GPIO_BSRR_BS7;
  GPIOA->BRR = GPIO_BRR_BR5;
}

void system_fault_handle(void) {
  __disable_irq();
  system_fault_turn_on_blue_led();

  for (;;) {
    __NOP();
  }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name) {
  (void)task;
  (void)task_name;
  system_fault_handle();
}

void vApplicationMallocFailedHook(void) { system_fault_handle(); }

void vApplicationAssertHook(const char *file, int line) {
  (void)file;
  (void)line;
  system_fault_handle();
}
