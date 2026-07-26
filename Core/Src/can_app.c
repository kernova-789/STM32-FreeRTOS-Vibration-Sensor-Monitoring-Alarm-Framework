#include "can_app.h"

#include "queue.h"
#include "semphr.h"
#include "string.h"
#include "task.h"

typedef struct {
  communication_physical_address_t identifier;
  uint8_t length;
  uint8_t data[CAN_APP_MAX_DATA_LENGTH];
} can_app_received_frame_t;

typedef struct {
  can_app_config_t configuration;
  communication_transport_driver_t driver;
  communication_transport_receive_t receive_callback;
  void *receive_callback_context;
  QueueHandle_t receive_queue;
  SemaphoreHandle_t transmit_mutex;
  TaskHandle_t receive_task;
  can_app_statistics_t statistics;
  bool configured;
  bool started;
} can_app_service_t;

static can_app_service_t can_app_service;

static communication_status_t can_app_start(
    void *context, communication_transport_receive_t receive_callback,
    void *callback_context);
static void can_app_stop(void *context);
static communication_status_t
can_app_send(void *context,
             communication_physical_address_t transmit_address,
             const uint8_t *payload, size_t payload_size,
             TickType_t timeout_ticks);
static communication_status_t can_app_configure_accept_all_filter(
    const can_app_config_t *configuration);
static void can_app_receive_task(void *argument);
static void can_app_receive_pending(CAN_HandleTypeDef *handle,
                                    uint32_t receive_fifo);
static communication_status_t
can_app_status_from_hal(HAL_StatusTypeDef status);
static bool can_app_timeout_expired(TickType_t start_tick,
                                    TickType_t timeout_ticks);

const communication_transport_driver_t *
can_app_driver(const can_app_config_t *configuration) {
  if ((configuration == NULL) || (configuration->handle == NULL) ||
      ((configuration->receive_fifo != CAN_RX_FIFO0) &&
       (configuration->receive_fifo != CAN_RX_FIFO1)) ||
      can_app_service.started) {
    return NULL;
  }

  memset(&can_app_service, 0, sizeof(can_app_service));
  can_app_service.configuration = *configuration;
  can_app_service.driver.name = "bxCAN";
  can_app_service.driver.context = &can_app_service;
  can_app_service.driver.maximum_payload_size = CAN_APP_MAX_DATA_LENGTH;
  can_app_service.driver.start = can_app_start;
  can_app_service.driver.stop = can_app_stop;
  can_app_service.driver.send = can_app_send;
  can_app_service.configured = true;

  return &can_app_service.driver;
}

void can_app_get_statistics(can_app_statistics_t *statistics) {
  if (statistics == NULL) {
    return;
  }

  taskENTER_CRITICAL();
  *statistics = can_app_service.statistics;
  taskEXIT_CRITICAL();
}

static communication_status_t can_app_start(
    void *context, communication_transport_receive_t receive_callback,
    void *callback_context) {
  can_app_service_t *service = (can_app_service_t *)context;
  communication_status_t status;

  if ((service != &can_app_service) || !service->configured ||
      (receive_callback == NULL)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }
  if (service->started) {
    return COMMUNICATION_STATUS_ALREADY_INITIALIZED;
  }

  service->receive_callback = receive_callback;
  service->receive_callback_context = callback_context;
  service->receive_queue =
      xQueueCreate(CAN_APP_RX_QUEUE_LENGTH, sizeof(can_app_received_frame_t));
  service->transmit_mutex = xSemaphoreCreateMutex();
  if ((service->receive_queue == NULL) || (service->transmit_mutex == NULL)) {
    if (service->receive_queue != NULL) {
      vQueueDelete(service->receive_queue);
    }
    if (service->transmit_mutex != NULL) {
      vSemaphoreDelete(service->transmit_mutex);
    }
    service->receive_queue = NULL;
    service->transmit_mutex = NULL;
    return COMMUNICATION_STATUS_NO_MEMORY;
  }

  if (xTaskCreate(can_app_receive_task, "can_rx",
                  CAN_APP_RX_TASK_STACK_DEPTH, service,
                  CAN_APP_RX_TASK_PRIORITY, &service->receive_task) != pdPASS) {
    vQueueDelete(service->receive_queue);
    vSemaphoreDelete(service->transmit_mutex);
    service->receive_queue = NULL;
    service->transmit_mutex = NULL;
    return COMMUNICATION_STATUS_NO_MEMORY;
  }

  status = can_app_configure_accept_all_filter(&service->configuration);
  if (status != COMMUNICATION_STATUS_OK) {
    can_app_stop(service);
    return status;
  }

  status = can_app_status_from_hal(HAL_CAN_Start(service->configuration.handle));
  if (status != COMMUNICATION_STATUS_OK) {
    can_app_stop(service);
    return status;
  }

  service->started = true;
  if (HAL_CAN_ActivateNotification(
          service->configuration.handle,
          (service->configuration.receive_fifo == CAN_RX_FIFO0)
              ? CAN_IT_RX_FIFO0_MSG_PENDING
              : CAN_IT_RX_FIFO1_MSG_PENDING) != HAL_OK) {
    can_app_stop(service);
    return COMMUNICATION_STATUS_TRANSPORT_ERROR;
  }

  return COMMUNICATION_STATUS_OK;
}

static void can_app_stop(void *context) {
  can_app_service_t *service = (can_app_service_t *)context;

  if ((service != &can_app_service) || !service->configured) {
    return;
  }

  if (service->started) {
    (void)HAL_CAN_DeactivateNotification(
        service->configuration.handle,
        (service->configuration.receive_fifo == CAN_RX_FIFO0)
            ? CAN_IT_RX_FIFO0_MSG_PENDING
            : CAN_IT_RX_FIFO1_MSG_PENDING);
    (void)HAL_CAN_Stop(service->configuration.handle);
  }
  service->started = false;

  if (service->receive_task != NULL) {
    vTaskDelete(service->receive_task);
    service->receive_task = NULL;
  }
  if (service->receive_queue != NULL) {
    vQueueDelete(service->receive_queue);
    service->receive_queue = NULL;
  }
  if (service->transmit_mutex != NULL) {
    vSemaphoreDelete(service->transmit_mutex);
    service->transmit_mutex = NULL;
  }
  service->receive_callback = NULL;
  service->receive_callback_context = NULL;
}

static communication_status_t
can_app_send(void *context,
             communication_physical_address_t transmit_address,
             const uint8_t *payload, size_t payload_size,
             TickType_t timeout_ticks) {
  can_app_service_t *service = (can_app_service_t *)context;
  CAN_TxHeaderTypeDef header = {0};
  uint8_t data[CAN_APP_MAX_DATA_LENGTH] = {0};
  uint32_t mailbox = 0U;
  TickType_t start_tick;
  HAL_StatusTypeDef hal_status;

  if ((service != &can_app_service) || !service->started ||
      (payload == NULL) || (payload_size == 0U) ||
      (payload_size > CAN_APP_MAX_DATA_LENGTH) ||
      (transmit_address > UINT32_C(0x7FF))) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  if (xSemaphoreTake(service->transmit_mutex, timeout_ticks) != pdPASS) {
    return (timeout_ticks == 0U) ? COMMUNICATION_STATUS_BUSY
                                 : COMMUNICATION_STATUS_TIMEOUT;
  }

  header.StdId = transmit_address;
  header.ExtId = 0U;
  header.IDE = CAN_ID_STD;
  header.RTR = CAN_RTR_DATA;
  header.DLC = (uint32_t)payload_size;
  header.TransmitGlobalTime = DISABLE;
  memcpy(data, payload, payload_size);

  start_tick = xTaskGetTickCount();
  while (HAL_CAN_GetTxMailboxesFreeLevel(service->configuration.handle) == 0U) {
    if ((timeout_ticks == 0U) ||
        can_app_timeout_expired(start_tick, timeout_ticks)) {
      xSemaphoreGive(service->transmit_mutex);
      ++service->statistics.transmit_failures;
      return (timeout_ticks == 0U) ? COMMUNICATION_STATUS_BUSY
                                   : COMMUNICATION_STATUS_TIMEOUT;
    }
    vTaskDelay(1U);
  }

  hal_status = HAL_CAN_AddTxMessage(service->configuration.handle, &header,
                                   data, &mailbox);
  xSemaphoreGive(service->transmit_mutex);

  if (hal_status == HAL_OK) {
    ++service->statistics.transmitted_frames;
  } else {
    ++service->statistics.transmit_failures;
  }
  return can_app_status_from_hal(hal_status);
}

static communication_status_t can_app_configure_accept_all_filter(
    const can_app_config_t *configuration) {
  CAN_FilterTypeDef filter = {0};

  filter.FilterIdHigh = 0U;
  filter.FilterIdLow = 0U;
  filter.FilterMaskIdHigh = 0U;
  filter.FilterMaskIdLow = 0U;
  filter.FilterFIFOAssignment = configuration->receive_fifo;
  filter.FilterBank = configuration->filter_bank;
  filter.FilterMode = CAN_FILTERMODE_IDMASK;
  filter.FilterScale = CAN_FILTERSCALE_32BIT;
  filter.FilterActivation = ENABLE;
  filter.SlaveStartFilterBank = 14U;

  return can_app_status_from_hal(
      HAL_CAN_ConfigFilter(configuration->handle, &filter));
}

static void can_app_receive_task(void *argument) {
  can_app_service_t *service = (can_app_service_t *)argument;
  can_app_received_frame_t frame;

  for (;;) {
    if (xQueueReceive(service->receive_queue, &frame, portMAX_DELAY) != pdPASS) {
      continue;
    }
    if (service->receive_callback != NULL) {
      (void)service->receive_callback(
          service->receive_callback_context, frame.identifier, frame.data,
          frame.length);
    }
  }
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *handle) {
  can_app_receive_pending(handle, CAN_RX_FIFO0);
}

void HAL_CAN_RxFifo1MsgPendingCallback(CAN_HandleTypeDef *handle) {
  can_app_receive_pending(handle, CAN_RX_FIFO1);
}

static void can_app_receive_pending(CAN_HandleTypeDef *handle,
                                    uint32_t receive_fifo) {
  can_app_received_frame_t frame;
  CAN_RxHeaderTypeDef header;
  BaseType_t higher_priority_task_woken = pdFALSE;

  if (!can_app_service.started ||
      (handle != can_app_service.configuration.handle) ||
      (can_app_service.configuration.receive_fifo != receive_fifo)) {
    return;
  }

  while (HAL_CAN_GetRxFifoFillLevel(handle, receive_fifo) > 0U) {
    memset(&frame, 0, sizeof(frame));
    memset(&header, 0, sizeof(header));

    if (HAL_CAN_GetRxMessage(handle, receive_fifo, &header, frame.data) !=
        HAL_OK) {
      ++can_app_service.statistics.discarded_frames;
      break;
    }
    if ((header.IDE != CAN_ID_STD) || (header.RTR != CAN_RTR_DATA) ||
        (header.DLC == 0U) || (header.DLC > CAN_APP_MAX_DATA_LENGTH)) {
      ++can_app_service.statistics.discarded_frames;
      continue;
    }

    frame.identifier = header.StdId;
    frame.length = (uint8_t)header.DLC;
    if (xQueueSendFromISR(can_app_service.receive_queue, &frame,
                          &higher_priority_task_woken) != pdPASS) {
      ++can_app_service.statistics.receive_queue_overflows;
    } else {
      ++can_app_service.statistics.received_frames;
    }
  }

  portYIELD_FROM_ISR(higher_priority_task_woken);
}

static communication_status_t
can_app_status_from_hal(HAL_StatusTypeDef status) {
  switch (status) {
  case HAL_OK:
    return COMMUNICATION_STATUS_OK;
  case HAL_BUSY:
    return COMMUNICATION_STATUS_BUSY;
  case HAL_TIMEOUT:
    return COMMUNICATION_STATUS_TIMEOUT;
  default:
    return COMMUNICATION_STATUS_TRANSPORT_ERROR;
  }
}

static bool can_app_timeout_expired(TickType_t start_tick,
                                    TickType_t timeout_ticks) {
  return (xTaskGetTickCount() - start_tick) >= timeout_ticks;
}
