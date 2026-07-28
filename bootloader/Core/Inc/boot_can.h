#pragma once

#include "boot_config.h"
#include "stm32f1xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void boot_can_reset(void);
void boot_can_start_packet_reception(void);

bool boot_can_take_handshake(void);
bool boot_can_take_packet(uint8_t packet[BOOT_OTA_PACKET_SIZE]);
bool boot_can_has_receive_error(void);

HAL_StatusTypeDef boot_can_send_reply(void);

#ifdef __cplusplus
}
#endif
