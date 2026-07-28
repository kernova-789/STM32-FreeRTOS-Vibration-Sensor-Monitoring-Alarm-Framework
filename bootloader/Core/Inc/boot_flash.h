#pragma once

#include "boot_config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  BOOT_FLASH_STATUS_OK = 0,
  BOOT_FLASH_STATUS_INVALID_ARGUMENT,
  BOOT_FLASH_STATUS_ERASE_ERROR,
  BOOT_FLASH_STATUS_PROGRAM_ERROR,
  BOOT_FLASH_STATUS_VERIFY_ERROR,
} boot_flash_status_t;

bool boot_flash_update_requested(void);
boot_flash_status_t boot_flash_prepare_update(void);
boot_flash_status_t boot_flash_clear_update_request(void);
boot_flash_status_t boot_flash_write(uint32_t image_offset,
                                     const uint8_t *data,
                                     size_t data_size);

#ifdef __cplusplus
}
#endif
