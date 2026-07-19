#pragma once
#include "FREERTOS.h"
#include "queue.h"
#include "stdint.h"

#define KEYBOARD_SCAN_RATE_HZ 5

enum keyboard{
    no_key = 0,
    up_key,
    down_key,
    left_key,
    right_key,
    enter_key,
    cancel_key,

    keyboard_key_count,
};

struct keyboard_state_msg_t{
    enum keyboard pressed_keys;
};

QueueHandle_t get_keyboard_queue(void);
uint32_t get_keyboard_state_mask(void);
BaseType_t create_keyboard_task(void);
