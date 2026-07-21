#pragma once
#include "keyboard.h"
#include <limits.h>

#define KEYBOARD_SCAN_PERIOD_MS 10U
#define KEYBOARD_PRESS_DEBOUNCE_SAMPLES 3U
#define KEYBOARD_RELEASE_DEBOUNCE_SAMPLES 2U

#define KEYBOARD_REPEAT_START_MS 500U
#define KEYBOARD_FAST_REPEAT_START_MS 1500U

#define KEYBOARD_REPEAT_PERIOD_MS 100U
#define KEYBOARD_FAST_REPEAT_PERIOD_MS 10U

/* 队列的总长度 */
#define KEYBOARD_EVENT_QUEUE_LENGTH 20U
/* 为 PRESSED 和 RELEASED事件预留的队列空间 */
#define KEYBOARD_IMPORTANT_EVENT_RESERVED_SLOTS 6U

#define KEYBOARD_TASK_STACK_DEPTH (configMINIMAL_STACK_SIZE + 64U)

#define KEYBOARD_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
_Static_assert((KEYBOARD_EVENT_QUEUE_LENGTH > 0U),
               "KEYBOARD_EVENT_QUEUE_LENGTH must be > 0");
_Static_assert(KEYBOARD_IMPORTANT_EVENT_RESERVED_SLOTS <
                   KEYBOARD_EVENT_QUEUE_LENGTH,
               "reserved slots must be smaller than queue length");

_Static_assert((KEYBOARD_SCAN_PERIOD_MS > 0U) &&
                   (KEYBOARD_SCAN_PERIOD_MS < 500U),
               "KEYBOARD_SCAN_PERIOD_MS must be in range [1, 499]");
_Static_assert((KEYBOARD_PRESS_DEBOUNCE_SAMPLES > 0U) &&
                   (KEYBOARD_PRESS_DEBOUNCE_SAMPLES < 10U),
               "KEYBOARD_PRESS_DEBOUNCE_SAMPLES must be in range [1, 9]");

_Static_assert((KEYBOARD_RELEASE_DEBOUNCE_SAMPLES > 0U) &&
                   (KEYBOARD_RELEASE_DEBOUNCE_SAMPLES < 10U),
               "KEYBOARD_RELEASE_DEBOUNCE_SAMPLES must be in range [1, 9]");

_Static_assert(KEYBOARD_KEY_COUNT <= (sizeof(keyboard_state_mask_t) * CHAR_BIT),
               "keyboard_state_mask_t is too small");

_Static_assert(KEYBOARD_FAST_REPEAT_START_MS >= KEYBOARD_REPEAT_START_MS,
               "fast repeat must not start before normal repeat");

_Static_assert(KEYBOARD_FAST_REPEAT_PERIOD_MS <= KEYBOARD_REPEAT_PERIOD_MS,
               "fast repeat period must not exceed normal repeat period");

_Static_assert(KEYBOARD_REPEAT_PERIOD_MS >= KEYBOARD_SCAN_PERIOD_MS,
               "repeat period cannot be shorter than scan period");

_Static_assert(KEYBOARD_FAST_REPEAT_PERIOD_MS >= KEYBOARD_SCAN_PERIOD_MS,
               "fast repeat period cannot be shorter than scan period");
