#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool boot_app_is_valid(void);
void boot_app_jump(void);

#ifdef __cplusplus
}
#endif
