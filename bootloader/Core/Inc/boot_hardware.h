#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  BOOT_BUTTON_ENTER = 0,
  BOOT_BUTTON_CANCEL,
  BOOT_BUTTON_COUNT,
} boot_button_t;

typedef enum {
  BOOT_INDICATOR_RED = 0,
  BOOT_INDICATOR_BLUE,
  BOOT_INDICATOR_COUNT,
} boot_indicator_t;

void boot_hardware_init_fast_boot(void);
bool boot_hardware_button_is_pressed(boot_button_t button);
void boot_hardware_indicator_set(boot_indicator_t indicator, bool enabled);

#ifdef __cplusplus
}
#endif
