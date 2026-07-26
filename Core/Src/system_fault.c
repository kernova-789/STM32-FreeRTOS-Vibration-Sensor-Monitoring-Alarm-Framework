#include "system_fault.h"

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f103xb.h"

/*
 * 状态绿灯连接到 PA6，低电平点亮。
 * 故障时不调用 indicator 模块，因为此时调度器、任务栈或动态内存
 * 可能已经失效，无法再安全地通过消息队列控制指示灯。
 */
#define SYSTEM_FAULT_GREEN_LED_PIN 6U
#define SYSTEM_FAULT_GREEN_LED_MODE_MASK                                      \
  (UINT32_C(0x0F) << (SYSTEM_FAULT_GREEN_LED_PIN * 4U))
#define SYSTEM_FAULT_GREEN_LED_OUTPUT_MODE                                    \
  (UINT32_C(0x01) << (SYSTEM_FAULT_GREEN_LED_PIN * 4U))

static void system_fault_turn_on_green_led(void) {
  /* 即使故障发生在 MX_GPIO_Init() 之前，也能直接初始化并点亮绿灯。 */
  RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
  GPIOA->CRL =
      (GPIOA->CRL & ~SYSTEM_FAULT_GREEN_LED_MODE_MASK) |
      SYSTEM_FAULT_GREEN_LED_OUTPUT_MODE;
  GPIOA->BRR = GPIO_BRR_BR6;
}

void system_fault_handle(void) {
  __disable_irq();
  system_fault_turn_on_green_led();

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
