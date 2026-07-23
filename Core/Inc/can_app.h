#pragma once

#include "communication.h"
#include "stm32f1xx_hal.h"

/* Broadcast address used by the CAN application envelope. */
#define CAN_APP_BROADCAST_NODE 255U

typedef struct {
  CAN_HandleTypeDef *handle;
  uint8_t local_node;

  /* bxCAN filter bank used by this backend (normally 0 on a single-CAN MCU). */
  uint8_t filter_bank;

  /* Incomplete fragmented messages are discarded after this interval. */
  uint32_t reassembly_timeout_ms;
} can_app_config_t;

/*
 * Creates the single CAN backend instance.
 * The configuration is copied; the returned object has static lifetime.
 * Call communication_init() with the returned pointer.
 */
const communication_backend_t *can_app_backend(const can_app_config_t *config);
