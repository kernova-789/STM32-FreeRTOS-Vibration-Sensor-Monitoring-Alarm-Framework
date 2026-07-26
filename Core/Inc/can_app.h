#pragma once

#include "can.h"
#include "communication.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_APP_MAX_DATA_LENGTH 8U
#define CAN_APP_RX_QUEUE_LENGTH 24U
#define CAN_APP_RX_TASK_STACK_DEPTH (configMINIMAL_STACK_SIZE + 96U)
#define CAN_APP_RX_TASK_PRIORITY (tskIDLE_PRIORITY + 5U)

typedef struct {
  CAN_HandleTypeDef *handle;
  uint32_t receive_fifo;
  uint32_t filter_bank;
} can_app_config_t;

typedef struct {
  uint32_t received_frames;
  uint32_t transmitted_frames;
  uint32_t discarded_frames;
  uint32_t receive_queue_overflows;
  uint32_t transmit_failures;
} can_app_statistics_t;

/*
 * 构造 communication_transport_driver_t 的 CAN 实现。
 * 返回对象具有静态生命周期；当前产品只使用一个 bxCAN 外设实例。
 */
const communication_transport_driver_t *
can_app_driver(const can_app_config_t *configuration);

void can_app_get_statistics(can_app_statistics_t *statistics);

#ifdef __cplusplus
}
#endif
