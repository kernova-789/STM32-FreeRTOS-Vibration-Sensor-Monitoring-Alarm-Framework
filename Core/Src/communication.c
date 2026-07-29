#include "communication.h"

#include "queue.h"
#include "string.h"

struct communication_device {
  communication_sensor_config_t configuration;
};

typedef struct {
  const communication_transport_driver_t *transport;
  struct communication_device devices[COMMUNICATION_MAX_SENSOR_COUNT];
  size_t device_count;
  communication_system_frame_handler_t system_frame_handler;
  void *system_frame_handler_context;
  QueueHandle_t event_queue;
  uint32_t dropped_events;
  bool initialized;
} communication_service_t;

static communication_service_t communication_service;

static communication_status_t communication_validate_configuration(
    const communication_config_t *configuration);
static communication_device_t *
communication_find_sensor(communication_sensor_id_t sensor_id);
static communication_device_t *communication_find_receive_address(
    communication_physical_address_t receive_address);
static communication_status_t communication_transport_receive(
    void *callback_context,
    communication_physical_address_t receive_address, const uint8_t *payload,
    size_t payload_size);

communication_status_t
communication_init(const communication_config_t *configuration) {
  communication_status_t status;
  size_t initialized_count = 0U;

  if (communication_service.initialized) {
    return COMMUNICATION_STATUS_ALREADY_INITIALIZED;
  }

  status = communication_validate_configuration(configuration);
  if (status != COMMUNICATION_STATUS_OK) {
    return status;
  }

  memset(&communication_service, 0, sizeof(communication_service));
  communication_service.transport = configuration->transport;
  communication_service.device_count = configuration->sensor_count;
  communication_service.system_frame_handler =
      configuration->system_frame_handler;
  communication_service.system_frame_handler_context =
      configuration->system_frame_handler_context;

  for (size_t index = 0U; index < configuration->sensor_count; ++index) {
    communication_service.devices[index].configuration =
        configuration->sensors[index];
  }

  communication_service.event_queue = xQueueCreate(
      COMMUNICATION_EVENT_QUEUE_LENGTH, sizeof(communication_event_t));
  if (communication_service.event_queue == NULL) {
    memset(&communication_service, 0, sizeof(communication_service));
    return COMMUNICATION_STATUS_NO_MEMORY;
  }

  for (size_t index = 0U; index < communication_service.device_count; ++index) {
    communication_device_t *device = &communication_service.devices[index];

    if (device->configuration.driver->init != NULL) {
      status = device->configuration.driver->init(device);
      if (status != COMMUNICATION_STATUS_OK) {
        initialized_count = index;
        goto fail;
      }
    }
    initialized_count = index + 1U;
  }

  /*
   * start() 可能立即打开接收中断，因此必须先标记服务已经就绪，
   * 避免第一帧有效数据被回调函数误判为“尚未初始化”。
   */
  communication_service.initialized = true;
  status = communication_service.transport->start(
      communication_service.transport->context, communication_transport_receive,
      &communication_service);
  if (status != COMMUNICATION_STATUS_OK) {
    communication_service.initialized = false;
    goto fail;
  }

  return COMMUNICATION_STATUS_OK;

fail:
  while (initialized_count > 0U) {
    communication_device_t *device;

    --initialized_count;
    device = &communication_service.devices[initialized_count];
    if (device->configuration.driver->deinit != NULL) {
      device->configuration.driver->deinit(device);
    }
  }
  vQueueDelete(communication_service.event_queue);
  memset(&communication_service, 0, sizeof(communication_service));
  return status;
}

void communication_deinit(void) {
  if (!communication_service.initialized) {
    return;
  }

  communication_service.initialized = false;
  if (communication_service.transport->stop != NULL) {
    communication_service.transport->stop(
        communication_service.transport->context);
  }

  for (size_t index = communication_service.device_count; index > 0U; --index) {
    communication_device_t *device =
        &communication_service.devices[index - 1U];

    if (device->configuration.driver->deinit != NULL) {
      device->configuration.driver->deinit(device);
    }
  }

  vQueueDelete(communication_service.event_queue);
  memset(&communication_service, 0, sizeof(communication_service));
}

communication_status_t
communication_ioctl(communication_sensor_id_t sensor_id,
                    communication_ioctl_command_t command,
                    const void *argument, size_t argument_size,
                    TickType_t timeout_ticks) {
  communication_device_t *device;

  if (!communication_service.initialized) {
    return COMMUNICATION_STATUS_NOT_INITIALIZED;
  }

  device = communication_find_sensor(sensor_id);
  if (device == NULL) {
    return COMMUNICATION_STATUS_NOT_FOUND;
  }
  if (device->configuration.driver->ioctl == NULL) {
    return COMMUNICATION_STATUS_UNSUPPORTED;
  }

  return device->configuration.driver->ioctl(
      device, command, argument, argument_size, timeout_ticks);
}

BaseType_t communication_receive_event(communication_event_t *event,
                                       TickType_t timeout_ticks) {
  if ((event == NULL) || !communication_service.initialized ||
      (communication_service.event_queue == NULL)) {
    return pdFAIL;
  }

  return xQueueReceive(communication_service.event_queue, event,
                       timeout_ticks);
}

communication_sensor_id_t
communication_device_sensor_id(const communication_device_t *device) {
  if (device == NULL) {
    return COMMUNICATION_SENSOR_ID_INVALID;
  }
  return device->configuration.sensor_id;
}

const void *
communication_device_driver_config(const communication_device_t *device) {
  if (device == NULL) {
    return NULL;
  }
  return device->configuration.driver_config;
}

void *
communication_device_driver_context(const communication_device_t *device) {
  if (device == NULL) {
    return NULL;
  }
  return device->configuration.driver_context;
}

communication_status_t
communication_device_send(communication_device_t *device,
                          const uint8_t *payload, size_t payload_size,
                          TickType_t timeout_ticks) {
  if (!communication_service.initialized) {
    return COMMUNICATION_STATUS_NOT_INITIALIZED;
  }
  if ((device == NULL) || (payload == NULL) || (payload_size == 0U)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }
  if (payload_size > communication_service.transport->maximum_payload_size) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  return communication_service.transport->send(
      communication_service.transport->context,
      device->configuration.transmit_address, payload, payload_size,
      timeout_ticks);
}

communication_status_t
communication_device_publish(communication_device_t *device,
                             communication_event_type_t type,
                             const void *data, size_t data_size) {
  communication_event_t event = {0};

  if (!communication_service.initialized) {
    return COMMUNICATION_STATUS_NOT_INITIALIZED;
  }
  if ((device == NULL) ||
      ((data == NULL) && (data_size > 0U)) ||
      (data_size > COMMUNICATION_EVENT_DATA_CAPACITY)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  event.sensor_id = device->configuration.sensor_id;
  event.type = type;
  event.timestamp_ticks = xTaskGetTickCount();
  event.data_size = (uint8_t)data_size;
  if (data_size > 0U) {
    memcpy(event.data, data, data_size);
  }

  if (xQueueSend(communication_service.event_queue, &event, 0U) != pdPASS) {
    ++communication_service.dropped_events;
    return COMMUNICATION_STATUS_QUEUE_FULL;
  }
  return COMMUNICATION_STATUS_OK;
}

static communication_status_t communication_validate_configuration(
    const communication_config_t *configuration) {
  if ((configuration == NULL) || (configuration->transport == NULL) ||
      (configuration->sensors == NULL) ||
      (configuration->sensor_count == 0U) ||
      (configuration->sensor_count > COMMUNICATION_MAX_SENSOR_COUNT)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }
  if ((configuration->transport->maximum_payload_size == 0U) ||
      (configuration->transport->start == NULL) ||
      (configuration->transport->send == NULL)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  for (size_t index = 0U; index < configuration->sensor_count; ++index) {
    const communication_sensor_config_t *sensor =
        &configuration->sensors[index];

    if ((sensor->sensor_id == COMMUNICATION_SENSOR_ID_INVALID) ||
        (sensor->driver == NULL) || (sensor->driver->on_receive == NULL)) {
      return COMMUNICATION_STATUS_INVALID_ARGUMENT;
    }

    for (size_t other = 0U; other < index; ++other) {
      const communication_sensor_config_t *previous =
          &configuration->sensors[other];

      /*
       * 逻辑 ID 和接收地址都必须一一对应。
       * 对于使用公共命令地址的协议，多个设备可以共享发送地址。
       */
      if ((previous->sensor_id == sensor->sensor_id) ||
          (previous->receive_address == sensor->receive_address)) {
        return COMMUNICATION_STATUS_INVALID_ARGUMENT;
      }
    }
  }
  return COMMUNICATION_STATUS_OK;
}

static communication_device_t *
communication_find_sensor(communication_sensor_id_t sensor_id) {
  for (size_t index = 0U; index < communication_service.device_count; ++index) {
    if (communication_service.devices[index].configuration.sensor_id ==
        sensor_id) {
      return &communication_service.devices[index];
    }
  }
  return NULL;
}

static communication_device_t *communication_find_receive_address(
    communication_physical_address_t receive_address) {
  for (size_t index = 0U; index < communication_service.device_count; ++index) {
    if (communication_service.devices[index].configuration.receive_address ==
        receive_address) {
      return &communication_service.devices[index];
    }
  }
  return NULL;
}

static communication_status_t communication_transport_receive(
    void *callback_context,
    communication_physical_address_t receive_address, const uint8_t *payload,
    size_t payload_size) {
  communication_device_t *device;

  if ((callback_context != &communication_service) ||
      !communication_service.initialized || (payload == NULL) ||
      (payload_size == 0U) ||
      (payload_size >
       communication_service.transport->maximum_payload_size)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  /*
   * OTA 等系统级控制帧不属于某一个传感器的数据地址。
   * 必须在 receive_address 查表和普通数据事件队列之前处理，否则使用其他
   * CAN ID 的升级命令会被当作“未知设备”丢弃。
   */
  if ((communication_service.system_frame_handler != NULL) &&
      communication_service.system_frame_handler(
          receive_address, payload, payload_size,
          communication_service.system_frame_handler_context)) {
    return COMMUNICATION_STATUS_OK;
  }

  device = communication_find_receive_address(receive_address);
  if (device == NULL) {
    return COMMUNICATION_STATUS_NOT_FOUND;
  }

  return device->configuration.driver->on_receive(device, payload,
                                                   payload_size);
}
