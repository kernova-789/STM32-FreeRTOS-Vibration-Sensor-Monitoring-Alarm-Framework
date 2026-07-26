#pragma once

#include "FreeRTOS.h"
#include "projdefs.h"
#include "stdbool.h"
#include "stdint.h"
#include "task.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COMMUNICATION_MAX_SENSOR_COUNT 8U
#define COMMUNICATION_EVENT_DATA_CAPACITY 16U
#define COMMUNICATION_EVENT_QUEUE_LENGTH 16U

#define COMMUNICATION_SENSOR_ID_INVALID ((communication_sensor_id_t)0U)

/*
 * 高 16 位标识传感器驱动类别，低 16 位标识该类别中的命令或事件。
 * 不同传感器驱动可独立分配低位编号，不会意外使用相同的完整编号。
 */
#define COMMUNICATION_CODE(driver_class, number)                               \
  ((((uint32_t)(driver_class)) << 16U) | ((uint32_t)(number) & UINT32_C(0xFFFF)))
#define COMMUNICATION_CODE_CLASS(code) ((uint16_t)((uint32_t)(code) >> 16U))
#define COMMUNICATION_CODE_NUMBER(code) ((uint16_t)((uint32_t)(code)))

typedef uint16_t communication_sensor_id_t;
typedef uint32_t communication_physical_address_t;
typedef uint32_t communication_ioctl_command_t;
typedef uint32_t communication_event_type_t;

typedef enum {
  COMMUNICATION_STATUS_OK = 0,
  COMMUNICATION_STATUS_INVALID_ARGUMENT,
  COMMUNICATION_STATUS_NOT_INITIALIZED,
  COMMUNICATION_STATUS_ALREADY_INITIALIZED,
  COMMUNICATION_STATUS_NOT_FOUND,
  COMMUNICATION_STATUS_UNSUPPORTED,
  COMMUNICATION_STATUS_BUSY,
  COMMUNICATION_STATUS_TIMEOUT,
  COMMUNICATION_STATUS_QUEUE_FULL,
  COMMUNICATION_STATUS_TRANSPORT_ERROR,
  COMMUNICATION_STATUS_DRIVER_ERROR,
  COMMUNICATION_STATUS_NO_MEMORY,
} communication_status_t;

typedef struct {
  communication_sensor_id_t sensor_id;
  communication_event_type_t type;
  TickType_t timestamp_ticks;
  uint8_t data_size;
  uint8_t data[COMMUNICATION_EVENT_DATA_CAPACITY];
} communication_event_t;

typedef struct communication_device communication_device_t;

/*
 * 传感器驱动负责设备特有的报文格式：
 * ioctl() 把带类型的应用命令转换为总线报文，
 * on_receive() 把总线报文转换为带类型的通信事件。
 */
typedef struct {
  const char *name;
  communication_status_t (*init)(communication_device_t *device);
  void (*deinit)(communication_device_t *device);
  communication_status_t (*ioctl)(communication_device_t *device,
                                  communication_ioctl_command_t command,
                                  const void *argument, size_t argument_size,
                                  TickType_t timeout_ticks);
  communication_status_t (*on_receive)(communication_device_t *device,
                                       const uint8_t *payload,
                                       size_t payload_size);
} communication_sensor_driver_t;

typedef struct {
  /* 应用层和持久化配置共同使用的稳定逻辑 ID。 */
  communication_sensor_id_t sensor_id;

  /*
   * 物理地址由当前传输层解释：
   * - CAN：11 位标准帧 ID；
   * - Modbus RTU：范围为 1..247 的从站地址。
   */
  communication_physical_address_t receive_address;
  communication_physical_address_t transmit_address;

  const communication_sensor_driver_t *driver;
  /* 可由同类设备共享的只读协议参数。 */
  const void *driver_config;
  /* 由当前物理设备独占的可选运行时状态。 */
  void *driver_context;
} communication_sensor_config_t;

typedef communication_status_t (*communication_transport_receive_t)(
    void *callback_context,
    communication_physical_address_t receive_address, const uint8_t *payload,
    size_t payload_size);

typedef struct {
  const char *name;
  void *context;
  size_t maximum_payload_size;

  communication_status_t (*start)(
      void *context, communication_transport_receive_t receive_callback,
      void *callback_context);
  void (*stop)(void *context);
  communication_status_t (*send)(
      void *context, communication_physical_address_t transmit_address,
      const uint8_t *payload, size_t payload_size, TickType_t timeout_ticks);
} communication_transport_driver_t;

typedef struct {
  const communication_transport_driver_t *transport;
  const communication_sensor_config_t *sensors;
  size_t sensor_count;
} communication_config_t;

/*
 * 初始化一条活动总线及其连接的全部传感器。
 * 传感器配置表会复制到模块内部，但 driver_config 指针指向的数据必须始终有效。
 */
communication_status_t
communication_init(const communication_config_t *configuration);
void communication_deinit(void);

/*
 * 类似 Linux ioctl 的设备控制入口。command 和 argument 由具体传感器驱动定义。
 * argument_size 让驱动能够检查参数结构大小，避免直接强制转换 void *。
 */
communication_status_t
communication_ioctl(communication_sensor_id_t sensor_id,
                    communication_ioctl_command_t command,
                    const void *argument, size_t argument_size,
                    TickType_t timeout_ticks);

/* 接收传感器驱动发布的带类型事件，只能在任务上下文调用。 */
BaseType_t communication_receive_event(communication_event_t *event,
                                       TickType_t timeout_ticks);

/*
 * 供传感器驱动实现使用的辅助接口，普通应用代码通常不应直接调用。
 */
communication_sensor_id_t
communication_device_sensor_id(const communication_device_t *device);
const void *
communication_device_driver_config(const communication_device_t *device);
void *
communication_device_driver_context(const communication_device_t *device);
communication_status_t
communication_device_send(communication_device_t *device,
                          const uint8_t *payload, size_t payload_size,
                          TickType_t timeout_ticks);
communication_status_t
communication_device_publish(communication_device_t *device,
                             communication_event_type_t type,
                             const void *data, size_t data_size);

#ifdef __cplusplus
}
#endif
