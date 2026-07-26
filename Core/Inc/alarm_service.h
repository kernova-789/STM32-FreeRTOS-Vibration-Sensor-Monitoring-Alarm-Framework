#pragma once

#include "FreeRTOS.h"
#include "stdbool.h"
#include "stdint.h"
#include "vibration_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 报警策略使用已经换算好的物理量，单位与 vibration_sensor_sample_t 一致，
 * 均为 0.01 g。周期和持续时间使用 FreeRTOS Tick，避免服务内部依赖界面单位。
 */
typedef struct {
  uint16_t minimum_centi_g[VIBRATION_AXIS_COUNT];
  uint16_t maximum_centi_g[VIBRATION_AXIS_COUNT];
  TickType_t trigger_period_ticks;
  uint16_t trigger_count;
  TickType_t alarm_duration_ticks;
  bool auto_stop;
} alarm_service_policy_t;

/*
 * 每个传感器拥有一个独立实例。计数器只统计当前触发周期内的越限样本，
 * active_axis 保存已经锁存的各轴报警状态。
 */
typedef struct {
  alarm_service_policy_t policy;
  TickType_t window_start_ticks;
  TickType_t alarm_start_ticks;
  uint16_t violation_count[VIBRATION_AXIS_COUNT];
  bool active_axis[VIBRATION_AXIS_COUNT];
  bool alarm_active;
  bool initialized;
} alarm_service_t;

void alarm_service_init(alarm_service_t *service,
                        const alarm_service_policy_t *policy,
                        TickType_t current_ticks);

/*
 * 更新策略后从当前时刻开始新的统计周期。已经锁存的报警不会因修改阈值
 * 自动清除；它仍需由用户取消，或由新的自动停止计时结束。
 */
void alarm_service_set_policy(alarm_service_t *service,
                              const alarm_service_policy_t *policy,
                              TickType_t current_ticks);

/*
 * 输入一个样本并累计越限次数。返回 true 表示报警输出状态发生了变化。
 */
bool alarm_service_process_sample(alarm_service_t *service,
                                  const vibration_sensor_sample_t *sample,
                                  TickType_t current_ticks);

/*
 * 推进统计周期并检查自动停止计时。即使暂时没有新样本，应用层也应周期调用。
 * 返回 true 表示报警输出状态发生了变化。
 */
bool alarm_service_poll(alarm_service_t *service, TickType_t current_ticks);

/* 清除所有已锁存报警和当前周期计数。 */
bool alarm_service_cancel(alarm_service_t *service,
                          TickType_t current_ticks);

bool alarm_service_axis_active(const alarm_service_t *service,
                               vibration_axis_t axis);
bool alarm_service_is_active(const alarm_service_t *service);

#ifdef __cplusplus
}
#endif
