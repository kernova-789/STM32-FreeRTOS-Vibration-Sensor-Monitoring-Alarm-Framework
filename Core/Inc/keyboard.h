#pragma once
#include "FREERTOS.h"
#include "queue.h"
#include "stdint.h"
#define keyboard_state_mask_t uint32_t

#define KEYBOARD_SCAN_PERIOD_MS              10U
#define KEYBOARD_PRESS_DEBOUNCE_SAMPLES       3U

#define KEYBOARD_REPEAT_START_MS            500U
#define KEYBOARD_FAST_REPEAT_START_MS      1500U

#define KEYBOARD_REPEAT_PERIOD_MS           100U
#define KEYBOARD_FAST_REPEAT_PERIOD_MS       10U

enum keyboard{
    // no_key = 0,
    KEYBOARD_UP_KEY,
    KEYBOARD_DOWN_KEY,
    KEYBOARD_LEFT_KEY,
    KEYBOARD_RIGHT_KEY,
    KEYBOARD_ENTER_KEY,
    KEYBOARD_CANCEL_KEY,

    KEYBOARD_KEY_COUNT,
};

enum keyboard_event {
  KEYBOARD_EVENT_PRESSED = 0,
  KEYBOARD_EVENT_REPEAT,
  KEYBOARD_EVENT_RELEASED,
};

struct keyboard_state_msg_t {
  enum keyboard key;
  enum keyboard_event event;
};

BaseType_t keyboard_receive(struct keyboard_state_msg_t *msg,
                            TickType_t timeout_ticks);
uint32_t get_keyboard_state_mask(void);
BaseType_t create_keyboard_task(void);