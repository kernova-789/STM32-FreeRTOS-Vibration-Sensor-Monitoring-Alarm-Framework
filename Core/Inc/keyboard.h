#pragma once
#include "FreeRTOS.h"
#include "stdbool.h"
#include "stdint.h"

typedef uint32_t keyboard_state_mask_t;

typedef enum {
  KEYBOARD_KEY_UP = 0,
  KEYBOARD_KEY_DOWN,
  KEYBOARD_KEY_LEFT,
  KEYBOARD_KEY_RIGHT,
  KEYBOARD_KEY_ENTER,
  KEYBOARD_KEY_CANCEL,

  KEYBOARD_KEY_COUNT,
} keyboard_key_t;

typedef enum {
  KEYBOARD_EVENT_PRESSED = 0,
  KEYBOARD_EVENT_REPEAT,
  KEYBOARD_EVENT_RELEASED,
} keyboard_event_type_t;

typedef struct {
  keyboard_key_t key;
  keyboard_event_type_t type;
} keyboard_event_t;

/*
 * 可选的非阻塞按键活动回调。键盘任务产生经过消抖的 PRESSED 事件时调用，
 * 即使事件队列已满也会执行，用于可靠唤醒自动关闭的屏幕。
 */
typedef void (*keyboard_activity_callback_t)(void *context);
void keyboard_set_activity_callback(keyboard_activity_callback_t callback,
                                    void *context);

/*
 * 初始化键盘队列和扫描任务。
 *
 * 应在系统初始化阶段调用，不允许多个任务并发调用。
 * 重复调用时，如果已经初始化成功，则返回 pdPASS。
 */
BaseType_t keyboard_init(void);

/*
 * 任务上下文接口，不应在 ISR 中调用。
 */
BaseType_t keyboard_receive_event(keyboard_event_t *event,
                                  TickType_t timeout_ticks);

/*
 * 获取当前所有按键的稳定状态快照。
 * 任务上下文接口。
 */
keyboard_state_mask_t keyboard_get_state_mask(void);

/*
 * 从状态掩码中取出一个按下的按键。
 *
 * 每次返回枚举值最小的按键，并清除 mask 中对应的位。
 * 返回 pdTRUE 表示成功取出按键；
 * 返回 pdFALSE 表示参数无效或掩码中没有按键。
 */
BaseType_t keyboard_state_mask_pop_key(keyboard_state_mask_t *mask,
                                       keyboard_key_t *key);

/*
 * 查询指定按键当前是否处于稳定按下状态。
 * 非法按键值返回 false。
 */
bool keyboard_is_pressed(keyboard_key_t key);
