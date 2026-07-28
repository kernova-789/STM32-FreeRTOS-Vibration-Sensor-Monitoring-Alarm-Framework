#include "boot_flash.h"

#include "stm32f1xx_hal.h"

static boot_flash_status_t boot_flash_erase_page(uint32_t page_address);
static void boot_flash_watchdog_refresh(void);

bool boot_flash_update_requested(void) {
  return *(const volatile uint16_t *)(uintptr_t)BOOT_REQUEST_FLAG_ADDRESS ==
         BOOT_REQUEST_FLAG_VALUE;
}

boot_flash_status_t boot_flash_prepare_update(void) {
  boot_flash_status_t status = BOOT_FLASH_STATUS_OK;

  if (HAL_FLASH_Unlock() != HAL_OK) {
    return BOOT_FLASH_STATUS_ERASE_ERROR;
  }

  for (uint32_t page = 0U; page < BOOT_APP_PAGE_COUNT; ++page) {
    status =
        boot_flash_erase_page(BOOT_APP_START_ADDRESS +
                              (page * BOOT_FLASH_PAGE_SIZE));
    boot_flash_watchdog_refresh();
    if (status != BOOT_FLASH_STATUS_OK) {
      break;
    }
  }

  if (status == BOOT_FLASH_STATUS_OK) {
    status = boot_flash_erase_page(BOOT_REQUEST_PAGE_ADDRESS);
  }

  (void)HAL_FLASH_Lock();
  return status;
}

boot_flash_status_t boot_flash_clear_update_request(void) {
  boot_flash_status_t status;

  if (HAL_FLASH_Unlock() != HAL_OK) {
    return BOOT_FLASH_STATUS_ERASE_ERROR;
  }
  status = boot_flash_erase_page(BOOT_REQUEST_PAGE_ADDRESS);
  (void)HAL_FLASH_Lock();
  return status;
}

boot_flash_status_t boot_flash_write(uint32_t image_offset,
                                     const uint8_t *data,
                                     size_t data_size) {
  const uint32_t application_size =
      BOOT_APP_END_ADDRESS - BOOT_APP_START_ADDRESS;
  uint32_t address;
  HAL_StatusTypeDef hal_status = HAL_OK;

  if ((data == NULL) || (data_size == 0U) || ((data_size & 1U) != 0U) ||
      ((image_offset & 1U) != 0U) || (image_offset > application_size) ||
      (data_size > (size_t)(application_size - image_offset))) {
    return BOOT_FLASH_STATUS_INVALID_ARGUMENT;
  }

  address = BOOT_APP_START_ADDRESS + image_offset;
  if (HAL_FLASH_Unlock() != HAL_OK) {
    return BOOT_FLASH_STATUS_PROGRAM_ERROR;
  }

  for (size_t index = 0U; index < data_size; index += 2U) {
    const uint16_t halfword =
        (uint16_t)data[index] | ((uint16_t)data[index + 1U] << 8U);

    hal_status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                   address + (uint32_t)index, halfword);
    if (hal_status != HAL_OK) {
      break;
    }
  }

  (void)HAL_FLASH_Lock();
  boot_flash_watchdog_refresh();
  if (hal_status != HAL_OK) {
    return BOOT_FLASH_STATUS_PROGRAM_ERROR;
  }

  for (size_t index = 0U; index < data_size; ++index) {
    if (*(const volatile uint8_t *)(uintptr_t)(address + (uint32_t)index) !=
        data[index]) {
      return BOOT_FLASH_STATUS_VERIFY_ERROR;
    }
  }

  return BOOT_FLASH_STATUS_OK;
}

static boot_flash_status_t boot_flash_erase_page(uint32_t page_address) {
  FLASH_EraseInitTypeDef erase = {
      .TypeErase = FLASH_TYPEERASE_PAGES,
      .PageAddress = page_address,
      .NbPages = 1U,
  };
  uint32_t page_error = UINT32_MAX;

  if ((HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) ||
      (page_error != UINT32_MAX)) {
    return BOOT_FLASH_STATUS_ERASE_ERROR;
  }
  return BOOT_FLASH_STATUS_OK;
}

static void boot_flash_watchdog_refresh(void) {
  IWDG->KR = UINT16_C(0xAAAA);
}
