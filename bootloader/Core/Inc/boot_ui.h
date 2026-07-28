#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void boot_ui_init(void);
void boot_ui_tick_1ms(void);
void boot_ui_notify_can_activity(void);
void boot_ui_set_can_activity_enabled(bool enabled);
void boot_ui_show_waiting(bool application_invalid);
void boot_ui_show_upgrading(void);
void boot_ui_show_error(void);

#ifdef __cplusplus
}
#endif
