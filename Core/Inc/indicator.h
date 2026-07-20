#pragma once
#include "FreeRTOS.h"
#include "queue.h"
#include "stdbool.h"
#include "stdint.h"

enum indicator_device {
  alarm_led_x,
  alarm_led_y,
  alarm_led_z,

  status_led_red,
  status_led_green,
  status_led_blue,

  buzzer,

  indicator_device_count,
};

#define INDICATOR_MASK(device) (UINT32_C(1) << (device))

BaseType_t create_indicator_task(void);

BaseType_t indicator_set_mask(uint32_t state_mask, TickType_t wait_ticks);

BaseType_t indicator_set_mask_from_isr(uint32_t state_mask,
                                       BaseType_t *higher_priority_task_woken);

BaseType_t indicator_set_device(enum indicator_device device_id,
                                bool target_state, uint32_t duration_ms,
                                TickType_t wait_ticks);
BaseType_t
indicator_set_device_from_isr(enum indicator_device device_id,
                              bool target_state, uint32_t duration_ms,
                              BaseType_t *higher_priority_task_woken);