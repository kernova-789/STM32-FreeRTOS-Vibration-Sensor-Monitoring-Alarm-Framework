#pragma once

#include "FreeRTOS.h"
#include "communication.h"
#include "projdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 启动产品应用协调服务。配置表中的全部传感器都会被监测和参与报警，
 * displayed_sensor_id 只决定当前屏幕显示、编辑哪一个传感器的数据。
 */
BaseType_t monitor_app_init(communication_sensor_id_t displayed_sensor_id);

/*
 * 从任务上下文请求进入 Bootloader。实际的停回报、Flash 操作和复位由
 * monitor 任务执行，调用者不会在通信接收任务里直接擦写 Flash。
 */
BaseType_t monitor_app_request_bootloader(void);

#ifdef __cplusplus
}
#endif
