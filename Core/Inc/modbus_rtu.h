#pragma once

#include "communication.h"
#include "stdint.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODBUS_RTU_MAX_PDU_SIZE 253U
#define MODBUS_RTU_MAX_ADU_SIZE (1U + MODBUS_RTU_MAX_PDU_SIZE + 2U)

/*
 * 板级 UART/RS-485 适配层实现此回调。
 * 它需要在必要时切换收发器方向，并发送完整的 Modbus RTU ADU。
 */
typedef communication_status_t (*modbus_rtu_write_t)(
    void *port_context, const uint8_t *adu, size_t adu_size,
    TickType_t timeout_ticks);

typedef struct {
  modbus_rtu_write_t write;
  void *port_context;
} modbus_rtu_config_t;

const communication_transport_driver_t *
modbus_rtu_driver(const modbus_rtu_config_t *configuration);

/*
 * UART 空闲线/DMA 任务通过此接口提交一帧完整 RTU ADU。
 * 本接口只能在任务上下文调用，校验地址和 CRC 后才会把 PDU 路由给设备驱动。
 */
communication_status_t modbus_rtu_receive_adu(const uint8_t *adu,
                                              size_t adu_size);

uint16_t modbus_rtu_crc16(const uint8_t *data, size_t data_size);

#ifdef __cplusplus
}
#endif
