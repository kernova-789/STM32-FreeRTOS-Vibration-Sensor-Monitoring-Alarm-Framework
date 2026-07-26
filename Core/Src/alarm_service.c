#include "alarm_service.h"

#include "limits.h"
#include "string.h"

static alarm_service_policy_t
alarm_service_normalize_policy(const alarm_service_policy_t *policy);
static bool alarm_service_finish_elapsed_window(alarm_service_t *service,
                                                TickType_t current_ticks);
static bool alarm_service_check_auto_stop(alarm_service_t *service,
                                          TickType_t current_ticks);

void alarm_service_init(alarm_service_t *service,
                        const alarm_service_policy_t *policy,
                        TickType_t current_ticks) {
  if ((service == NULL) || (policy == NULL)) {
    return;
  }

  memset(service, 0, sizeof(*service));
  service->policy = alarm_service_normalize_policy(policy);
  service->window_start_ticks = current_ticks;
  service->initialized = true;
}

void alarm_service_set_policy(alarm_service_t *service,
                              const alarm_service_policy_t *policy,
                              TickType_t current_ticks) {
  if ((service == NULL) || (policy == NULL) || !service->initialized) {
    return;
  }

  service->policy = alarm_service_normalize_policy(policy);
  memset(service->violation_count, 0, sizeof(service->violation_count));
  service->window_start_ticks = current_ticks;

  /*
   * 报警已经触发时，修改自动停止参数应从确认新参数的时刻重新计时，
   * 避免缩短持续时间后立即产生难以理解的瞬时关闭。
   */
  if (service->alarm_active && service->policy.auto_stop) {
    service->alarm_start_ticks = current_ticks;
  }
}

bool alarm_service_process_sample(alarm_service_t *service,
                                  const vibration_sensor_sample_t *sample,
                                  TickType_t current_ticks) {
  bool state_changed;

  if ((service == NULL) || (sample == NULL) || !service->initialized) {
    return false;
  }

  state_changed = alarm_service_poll(service, current_ticks);
  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    const uint16_t magnitude = sample->magnitude_centi_g[axis];
    const bool outside_range =
        (magnitude < service->policy.minimum_centi_g[axis]) ||
        (magnitude > service->policy.maximum_centi_g[axis]);

    if (outside_range &&
        (service->violation_count[axis] < UINT16_MAX)) {
      ++service->violation_count[axis];
    }
  }
  return state_changed;
}

bool alarm_service_poll(alarm_service_t *service, TickType_t current_ticks) {
  bool state_changed;

  if ((service == NULL) || !service->initialized) {
    return false;
  }

  state_changed =
      alarm_service_finish_elapsed_window(service, current_ticks);
  if (alarm_service_check_auto_stop(service, current_ticks)) {
    state_changed = true;
  }
  return state_changed;
}

bool alarm_service_cancel(alarm_service_t *service,
                          TickType_t current_ticks) {
  const bool state_changed =
      (service != NULL) && service->initialized && service->alarm_active;

  if ((service == NULL) || !service->initialized) {
    return false;
  }

  memset(service->violation_count, 0, sizeof(service->violation_count));
  memset(service->active_axis, 0, sizeof(service->active_axis));
  service->alarm_active = false;
  service->alarm_start_ticks = current_ticks;
  service->window_start_ticks = current_ticks;
  return state_changed;
}

bool alarm_service_axis_active(const alarm_service_t *service,
                               vibration_axis_t axis) {
  if ((service == NULL) || !service->initialized ||
      ((uint32_t)axis >= (uint32_t)VIBRATION_AXIS_COUNT)) {
    return false;
  }
  return service->active_axis[axis];
}

bool alarm_service_is_active(const alarm_service_t *service) {
  return (service != NULL) && service->initialized && service->alarm_active;
}

static alarm_service_policy_t
alarm_service_normalize_policy(const alarm_service_policy_t *policy) {
  alarm_service_policy_t normalized = *policy;

  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    if (normalized.minimum_centi_g[axis] >
        normalized.maximum_centi_g[axis]) {
      normalized.minimum_centi_g[axis] =
          normalized.maximum_centi_g[axis];
    }
  }
  if (normalized.trigger_period_ticks == 0U) {
    normalized.trigger_period_ticks = 1U;
  }
  if (normalized.trigger_count == 0U) {
    normalized.trigger_count = 1U;
  }
  if (normalized.alarm_duration_ticks == 0U) {
    normalized.alarm_duration_ticks = 1U;
  }
  return normalized;
}

static bool alarm_service_finish_elapsed_window(alarm_service_t *service,
                                                TickType_t current_ticks) {
  const TickType_t elapsed_ticks =
      current_ticks - service->window_start_ticks;
  bool state_changed = false;
  bool was_active = service->alarm_active;

  if (elapsed_ticks < service->policy.trigger_period_ticks) {
    return false;
  }

  for (uint8_t axis = 0U; axis < VIBRATION_AXIS_COUNT; ++axis) {
    /*
     * 达到设定次数即触发，比老代码的“必须大于设定次数”更符合界面上
     * “触发次数”的含义。例如设为 10 时，第 10 次越限即可报警。
     */
    if (service->violation_count[axis] >=
        service->policy.trigger_count) {
      if (!service->active_axis[axis]) {
        state_changed = true;
      }
      service->active_axis[axis] = true;
      service->alarm_active = true;
    }
    service->violation_count[axis] = 0U;
  }

  if (!was_active && service->alarm_active) {
    service->alarm_start_ticks = current_ticks;
  }

  /*
   * 保留原有周期相位，同时一次跨过多个空周期时不逐周期循环，
   * 这样系统长时间忙碌后也不会出现耗时补算。
   */
  service->window_start_ticks +=
      (elapsed_ticks / service->policy.trigger_period_ticks) *
      service->policy.trigger_period_ticks;
  return state_changed;
}

static bool alarm_service_check_auto_stop(alarm_service_t *service,
                                          TickType_t current_ticks) {
  if (!service->alarm_active || !service->policy.auto_stop) {
    return false;
  }
  if ((current_ticks - service->alarm_start_ticks) <
      service->policy.alarm_duration_ticks) {
    return false;
  }

  memset(service->active_axis, 0, sizeof(service->active_axis));
  service->alarm_active = false;
  service->alarm_start_ticks = current_ticks;
  return true;
}
