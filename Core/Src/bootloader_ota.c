#include "bootloader_ota.h"

#include "stm32f1xx_hal.h"

bootloader_ota_status_t bootloader_ota_enter(void) {
  FLASH_EraseInitTypeDef erase = {
      .TypeErase = FLASH_TYPEERASE_PAGES,
      .PageAddress = BOOTLOADER_OTA_REQUEST_PAGE_ADDRESS,
      .NbPages = 1U,
  };
  uint32_t page_error = 0U;
  HAL_StatusTypeDef hal_status;

  /*
   * 进入这里之前应用层已经停止传感器连续上报。这里保留系统 Tick 中断，
   * 使 HAL 的 Flash 超时检测仍然有效；Flash 忙期间 Cortex-M3 会自然等待。
   */
  hal_status = HAL_FLASH_Unlock();
  if (hal_status == HAL_OK) {
    hal_status = HAL_FLASHEx_Erase(&erase, &page_error);
  }
  if (hal_status == HAL_OK) {
    hal_status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                   BOOTLOADER_OTA_FLAG_ADDRESS,
                                   BOOTLOADER_OTA_REQUEST_FLAG);
  }
  (void)HAL_FLASH_Lock();

  if ((hal_status != HAL_OK) || (page_error != UINT32_MAX)) {
    return BOOTLOADER_OTA_STATUS_FLASH_ERROR;
  }

  if (*(const volatile uint16_t *)(uintptr_t)BOOTLOADER_OTA_FLAG_ADDRESS !=
      BOOTLOADER_OTA_REQUEST_FLAG) {
    return BOOTLOADER_OTA_STATUS_VERIFY_ERROR;
  }

  __DSB();
  __ISB();
  NVIC_SystemReset();
  for (;;) {
  }
}
