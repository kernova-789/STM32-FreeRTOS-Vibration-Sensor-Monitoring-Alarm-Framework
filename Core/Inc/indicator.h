#pragma once
#include "FreeRTOS.h"
#include "stdbool.h"
#include "stdint.h"

typedef enum {
  INDICATOR_DEVICE_ALARM_LED_X,
  INDICATOR_DEVICE_ALARM_LED_Y,
  INDICATOR_DEVICE_ALARM_LED_Z,

  INDICATOR_DEVICE_STATUS_LED_RED,
  INDICATOR_DEVICE_STATUS_LED_GREEN,
  INDICATOR_DEVICE_STATUS_LED_BLUE,

  INDICATOR_DEVICE_BUZZER,

  INDICATOR_DEVICE_COUNT,
} indicator_device_t;

/*
 * 指示设备状态掩码类型。
 *
 * 每一位对应一个 indicator_device_t：
 * - 位为 1：设备处于激活状态；
 * - 位为 0：设备处于非激活状态。
 */
typedef uint32_t indicator_state_mask_t;

/*
 * 根据设备编号生成对应的状态掩码。
 *
 * device 必须是有效的 indicator_device_t，取值范围为：
 * [0, INDICATOR_DEVICE_COUNT)。
 */
#define INDICATOR_MASK(device) (UINT32_C(1) << (uint32_t)(device))

/**
 * @brief 初始化指示设备控制模块。
 *
 * 创建内部命令队列并启动指示设备控制任务。
 *
 * 本函数应在系统初始化阶段调用，不允许多个任务并发调用。
 * 当前实现不允许重复初始化；模块已经初始化时返回失败。
 *
 * @return pdPASS 初始化成功。
 * @return 非 pdPASS 队列或任务创建失败，或者模块已经初始化。
 */
BaseType_t indicator_init(void);

/**
 * @brief 使用状态掩码设置全部指示设备。
 *
 * 掩码中的每一位对应一个指示设备：
 * - 位为 1：激活对应设备；
 * - 位为 0：关闭对应设备。
 *
 * 高于 INDICATOR_DEVICE_COUNT 的无效位会被忽略。
 *
 * 建议该命令覆盖所有设备之前的定时状态，即调用后取消所有设备
 * 尚未到期的自动关闭计时。
 *
 * 本函数只能在任务上下文中调用，不能在 ISR 中调用。
 *
 * @param state_mask 目标设备状态掩码。
 * @param wait_ticks 队列已满时等待可用空间的最大 Tick 数。
 *                   传入 0 表示不等待。
 *
 * @return pdPASS 命令成功写入队列。
 * @return pdFAIL 模块未初始化，或者命令未能写入队列。
 */
BaseType_t indicator_set_mask(indicator_state_mask_t state_mask, TickType_t wait_ticks);

/**
 * @brief 在中断上下文中使用状态掩码设置全部指示设备。
 *
 * 功能与 indicator_set_mask() 相同，但本函数只能在 ISR 中调用。
 * 本函数不会阻塞。
 *
 * 调用者通常应在进入 ISR 后将 *higher_priority_task_woken 初始化为
 * pdFALSE，并在本函数返回后根据其值调用 portYIELD_FROM_ISR()。
 *
 * @param state_mask 目标设备状态掩码。
 * @param higher_priority_task_woken
 *        用于接收本次队列发送是否唤醒了更高优先级任务。
 *
 * @return pdPASS 命令成功写入队列。
 * @return pdFAIL 模块未初始化，或者队列中没有可用空间。
 */
BaseType_t indicator_set_mask_from_isr(indicator_state_mask_t state_mask,
                                       BaseType_t *higher_priority_task_woken);

/**
 * @brief 设置单个指示设备的状态。
 *
 * 当 target_state 为 true 时：
 * - duration_ms 为 0：设备保持激活，直到收到新的控制命令；
 * - duration_ms 大于 0：设备激活指定时间后自动关闭。
 *
 * 当 target_state 为 false 时，设备立即关闭，duration_ms 被忽略，
 * 同时取消该设备之前尚未完成的自动关闭计时。
 *
 * 对同一设备发送的新命令会覆盖该设备之前的定时状态。
 *
 * 本函数只能在任务上下文中调用，不能在 ISR 中调用。
 *
 * @param device_id   要控制的设备。
 * @param target_state true 表示激活设备，false 表示关闭设备。
 * @param duration_ms 设备保持激活的时间，单位为毫秒。
 * @param wait_ticks  队列已满时等待可用空间的最大 Tick 数。
 *                    传入 0 表示不等待。
 *
 * @return pdPASS 命令成功写入队列。
 * @return pdFAIL 参数无效、模块未初始化，或者命令未能写入队列。
 */
BaseType_t indicator_set_device(indicator_device_t device_id, bool target_state,
                                uint32_t duration_ms, TickType_t wait_ticks);

/**
 * @brief 在中断上下文中设置单个指示设备的状态。
 *
 * 功能与 indicator_set_device() 相同，但本函数只能在 ISR 中调用，
 * 并且不会阻塞。
 *
 * 调用者通常应在进入 ISR 后将 *higher_priority_task_woken 初始化为
 * pdFALSE，并在本函数返回后根据其值调用 portYIELD_FROM_ISR()。
 *
 * @param device_id   要控制的设备。
 * @param target_state true 表示激活设备，false 表示关闭设备。
 * @param duration_ms 设备保持激活的时间，单位为毫秒。
 * @param higher_priority_task_woken
 *        用于接收本次队列发送是否唤醒了更高优先级任务。
 *
 * @return pdPASS 命令成功写入队列。
 * @return pdFAIL 参数无效、模块未初始化，或者队列中没有可用空间。
 */
BaseType_t
indicator_set_device_from_isr(indicator_device_t device_id, bool target_state,
                              uint32_t duration_ms,
                              BaseType_t *higher_priority_task_woken);