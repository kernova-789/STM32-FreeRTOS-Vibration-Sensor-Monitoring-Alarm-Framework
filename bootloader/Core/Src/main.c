/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : 裸机 CAN Bootloader
 ******************************************************************************
 */
/* USER CODE END Header */

#include "main.h"

#include "boot_app.h"
#include "boot_can.h"
#include "boot_flash.h"
#include "boot_hardware.h"
#include "boot_ui.h"
#include "can.h"
#include "gpio.h"
#include "spi.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  BOOTLOADER_STATE_WAIT_HANDSHAKE = 0,
  BOOTLOADER_STATE_RECEIVE_IMAGE,
  BOOTLOADER_STATE_ERROR,
} bootloader_state_t;

typedef enum {
  BOOT_PACKET_RESULT_MORE = 0,
  BOOT_PACKET_RESULT_COMPLETE,
  BOOT_PACKET_RESULT_ERROR,
} boot_packet_result_t;

static bootloader_state_t bootloader_state;
static uint8_t received_packet[BOOT_OTA_PACKET_SIZE];
static uint32_t expected_packet_number;
static uint32_t image_write_offset;
static uint32_t last_packet_tick;
static uint8_t reply_retry_count;

void SystemClock_Config(void);

static bool boot_early_enter_button_pressed(void);
static bool boot_button_pressed(boot_button_t button);
static void bootloader_begin_update(void);
static void bootloader_process_receiving(void);
static void bootloader_process_timeout(void);
static boot_packet_result_t
bootloader_process_packet(const uint8_t packet[BOOT_OTA_PACKET_SIZE]);
static bool
bootloader_packet_checksum_valid(const uint8_t packet[BOOT_OTA_PACKET_SIZE]);
static uint32_t
bootloader_packet_number(const uint8_t packet[BOOT_OTA_PACKET_SIZE]);
static void bootloader_enter_error(void);
static void bootloader_watchdog_refresh(void);

int main(void) {
  bool application_valid;
  bool update_requested;
  bool force_update;

  /*
   * 尽量缩短正常启动路径。在完成启动决策前，不初始化 HAL、1 ms 时基、
   * PLL、CAN、SPI 或显示屏。
   */
  boot_hardware_init_fast_boot();

  update_requested = boot_flash_update_requested();
  force_update = boot_early_enter_button_pressed();
  application_valid = boot_app_is_valid();

  /*
   * PB5 的优先级高于其他所有启动条件。如果未按住 PB5，且应用程序有效、
   * 没有升级请求，则立即进入应用程序。
   */
  if (!force_update && !update_requested && application_valid) {
    boot_app_jump();
  }

  /*
   * 以下代码只处理异常启动路径：强制升级、已保存的升级请求，
   * 或应用程序无效。
   */
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();

  /*
   * 仅当 PB5 未强制进入 Bootloader 且应用程序有效时，PB14 才能取消
   * 已保存的升级请求。清除标志后立即跳转，不预先初始化 CAN、SPI 或显示屏。
   */
  if (!force_update && update_requested && application_valid &&
      boot_button_pressed(BOOT_BUTTON_CANCEL)) {
    if (boot_flash_clear_update_request() == BOOT_FLASH_STATUS_OK) {
      boot_app_jump();
    }
  }

  /*
   * 在耗时较长的 LCD 初始化之前启动 CAN，避免丢失提前到达的
   * 0x06 握手帧。
   */
  boot_can_reset();
  MX_CAN_Init();
  MX_SPI2_Init();
  boot_ui_init();
  boot_ui_show_waiting(!application_valid);

  bootloader_state = BOOTLOADER_STATE_WAIT_HANDSHAKE;

  for (;;) {
    bootloader_watchdog_refresh();

    switch (bootloader_state) {
    case BOOTLOADER_STATE_WAIT_HANDSHAKE:
      if (boot_can_take_handshake()) {
        bootloader_begin_update();
      }
      break;

    case BOOTLOADER_STATE_RECEIVE_IMAGE:
      bootloader_process_receiving();
      break;

    case BOOTLOADER_STATE_ERROR:
    default:
      /*
       * 对固定的上位机升级会话而言，协议、校验和、包序号或 Flash 错误
       * 都是致命错误。此后不再确认数据包，也不尝试启动只写入了一部分的
       * 应用程序。
       */
      break;
    }

    __WFI();
  }
}

static bool boot_early_enter_button_pressed(void) {
  uint32_t reload;

  if (!boot_hardware_button_is_pressed(BOOT_BUTTON_ENTER)) {
    return false;
  }

  /*
   * 检测到 PB5 为低电平后，在不启动 HAL TIM4 时基的情况下消抖 20 ms。
   * SystemInit 此时让内核运行在 HSI 上，SystemCoreClock 记录该时钟频率。
   * 在进入任一后续路径前，都会再次停止并清空 SysTick。
   */
  reload = ((SystemCoreClock / 1000U) * 20U) - 1U;
  SysTick->LOAD = reload;
  SysTick->VAL = 0U;
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
  while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0U) {
  }
  SysTick->CTRL = 0U;
  SysTick->LOAD = 0U;
  SysTick->VAL = 0U;

  return boot_hardware_button_is_pressed(BOOT_BUTTON_ENTER);
}

static bool boot_button_pressed(boot_button_t button) {
  if (!boot_hardware_button_is_pressed(button)) {
    return false;
  }
  HAL_Delay(20U);
  return boot_hardware_button_is_pressed(button);
}

static void bootloader_begin_update(void) {
  boot_flash_status_t flash_status;

  boot_ui_show_upgrading();

  /*
   * 上位机会等待回复后再发送固件包，因此可在此处同步擦除。
   * 地址为 0x0801F400 和 0x0801F800 的两个配置页不会被改动。
   *
   * 先让握手消息对应的蓝灯脉冲完整显示，再暂停活动灯。
   * 这样长时间擦除 Flash 时，即使时基中断被延后，蓝灯也不会一直亮着。
   */
  HAL_Delay(BOOT_CAN_ACTIVITY_ON_MS);
  boot_ui_set_can_activity_enabled(false);
  flash_status = boot_flash_prepare_update();
  boot_ui_set_can_activity_enabled(true);
  if (flash_status != BOOT_FLASH_STATUS_OK) {
    bootloader_enter_error();
    return;
  }

  boot_can_start_packet_reception();
  if (boot_can_send_reply() != HAL_OK) {
    bootloader_enter_error();
    return;
  }

  expected_packet_number = 0U;
  image_write_offset = 0U;
  reply_retry_count = 0U;
  last_packet_tick = HAL_GetTick();
  bootloader_state = BOOTLOADER_STATE_RECEIVE_IMAGE;
}

static void bootloader_process_receiving(void) {
  if (boot_can_has_receive_error()) {
    bootloader_enter_error();
    return;
  }

  if (boot_can_take_packet(received_packet)) {
    const boot_packet_result_t result =
        bootloader_process_packet(received_packet);

    last_packet_tick = HAL_GetTick();
    reply_retry_count = 0U;

    if (result == BOOT_PACKET_RESULT_ERROR) {
      bootloader_enter_error();
      return;
    }
    if (result == BOOT_PACKET_RESULT_COMPLETE) {
      /*
       * 通过复位而不是直接跳转来启动应用程序，使外设和中断状态
       * 与正常上电复位时一致。
       */
      HAL_Delay(50U);
      NVIC_SystemReset();
    }

    if (boot_can_send_reply() != HAL_OK) {
      bootloader_enter_error();
    }
    return;
  }

  bootloader_process_timeout();
}

static void bootloader_process_timeout(void) {
  const uint32_t elapsed = HAL_GetTick() - last_packet_tick;
  const uint32_t next_retry_time =
      BOOT_REPLY_RETRY_INTERVAL_MS * ((uint32_t)reply_retry_count + 1U);

  if (elapsed > BOOT_RECEIVE_TIMEOUT_MS) {
    bootloader_enter_error();
    return;
  }

  if ((reply_retry_count < BOOT_REPLY_RETRY_COUNT) &&
      (elapsed >= next_retry_time)) {
    if (boot_can_send_reply() != HAL_OK) {
      bootloader_enter_error();
      return;
    }
    ++reply_retry_count;
  }
}

static boot_packet_result_t
bootloader_process_packet(const uint8_t packet[BOOT_OTA_PACKET_SIZE]) {
  const uint32_t packet_number = bootloader_packet_number(packet);
  const bool final_packet =
      packet_number == BOOT_OTA_FINAL_PACKET_NUMBER;
  const uint32_t application_size =
      BOOT_APP_END_ADDRESS - BOOT_APP_START_ADDRESS;

  if (!bootloader_packet_checksum_valid(packet) ||
      (!final_packet && (packet_number != expected_packet_number)) ||
      (image_write_offset >
       (application_size - BOOT_OTA_PACKET_DATA_SIZE))) {
    return BOOT_PACKET_RESULT_ERROR;
  }

  /*
   * 固定的 CAN 上位机程序使用 0xFFFFFFFF 作为结束包序号，
   * 但为兼容旧版 Bootloader，仍会写入该包中的 256 字节数据区。
   */
  if (boot_flash_write(image_write_offset,
                       &packet[BOOT_OTA_PACKET_DATA_OFFSET],
                       BOOT_OTA_PACKET_DATA_SIZE) !=
      BOOT_FLASH_STATUS_OK) {
    return BOOT_PACKET_RESULT_ERROR;
  }

  image_write_offset += BOOT_OTA_PACKET_DATA_SIZE;

  if (final_packet) {
    return boot_app_is_valid() ? BOOT_PACKET_RESULT_COMPLETE
                               : BOOT_PACKET_RESULT_ERROR;
  }

  ++expected_packet_number;
  return BOOT_PACKET_RESULT_MORE;
}

static bool
bootloader_packet_checksum_valid(const uint8_t packet[BOOT_OTA_PACKET_SIZE]) {
  uint8_t checksum = 0U;

  for (uint16_t index = 0U;
       index < BOOT_OTA_PACKET_CHECKSUM_INDEX; ++index) {
    checksum = (uint8_t)(checksum + packet[index]);
  }
  return checksum == packet[BOOT_OTA_PACKET_CHECKSUM_INDEX];
}

static uint32_t
bootloader_packet_number(const uint8_t packet[BOOT_OTA_PACKET_SIZE]) {
  return ((uint32_t)packet[BOOT_OTA_PACKET_NUMBER_OFFSET] << 24U) |
         ((uint32_t)packet[BOOT_OTA_PACKET_NUMBER_OFFSET + 1U] << 16U) |
         ((uint32_t)packet[BOOT_OTA_PACKET_NUMBER_OFFSET + 2U] << 8U) |
         (uint32_t)packet[BOOT_OTA_PACKET_NUMBER_OFFSET + 3U];
}

static void bootloader_enter_error(void) {
  bootloader_state = BOOTLOADER_STATE_ERROR;
  boot_ui_show_error();
}

static void bootloader_watchdog_refresh(void) {
  IWDG->KR = UINT16_C(0xAAAA);
}

void SystemClock_Config(void) {
  RCC_OscInitTypeDef oscillator = {0};
  RCC_ClkInitTypeDef clocks = {0};

  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  oscillator.HSEState = RCC_HSE_ON;
  oscillator.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  oscillator.HSIState = RCC_HSI_ON;
  oscillator.PLL.PLLState = RCC_PLL_ON;
  oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  oscillator.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
    Error_Handler();
  }

  clocks.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                     RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clocks.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clocks.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clocks.APB1CLKDivider = RCC_HCLK_DIV2;
  clocks.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_2) != HAL_OK) {
    Error_Handler();
  }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *timer) {
  if (timer->Instance == TIM4) {
    HAL_IncTick();
    boot_ui_tick_1ms();
  }
}

void Error_Handler(void) {
  /*
   * 此路径可能在 MX_GPIO_Init() 之前执行，因此重新执行快速启动硬件配置。
   * 红灯点亮后，程序停止期间会保持该状态。
   */
  boot_hardware_init_fast_boot();
  boot_hardware_indicator_set(BOOT_INDICATOR_RED, true);

  __disable_irq();
  for (;;) {
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {
  (void)file;
  (void)line;
  Error_Handler();
}
#endif
