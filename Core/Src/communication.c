#include "communication.h"

#include "queue.h"
#include "semphr.h"
#include "string.h"
#include "task.h"

#define COMMUNICATION_RX_QUEUE_LENGTH 8U

typedef struct {
  const communication_backend_t *backend;
  QueueHandle_t receive_queue;
  SemaphoreHandle_t send_mutex;
  volatile communication_statistics_t statistics;
} communication_service_t;

static communication_service_t communication_service = {0};

static bool
communication_message_is_valid(const communication_message_t *message);
static BaseType_t
communication_backend_receive(void *context,
                              const communication_message_t *message);
static BaseType_t
communication_backend_receive_from_isr(void *context,
                                       const communication_message_t *message,
                                       BaseType_t *higher_priority_task_woken);

communication_status_t
communication_init(const communication_backend_t *backend) {
  communication_status_t status;
  const communication_backend_receiver_t receiver = {
      .context = &communication_service,
      .message_received = communication_backend_receive,
      .message_received_from_isr = communication_backend_receive_from_isr,
  };

  if ((backend == NULL) || (backend->ops == NULL) ||
      (backend->ops->start == NULL) || (backend->ops->send == NULL)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }
  if (communication_service.backend != NULL) {
    return COMMUNICATION_STATUS_ALREADY_INITIALIZED;
  }

  communication_service.receive_queue = xQueueCreate(
      COMMUNICATION_RX_QUEUE_LENGTH, sizeof(communication_message_t));
  if (communication_service.receive_queue == NULL) {
    return COMMUNICATION_STATUS_NO_MEMORY;
  }

  communication_service.send_mutex = xSemaphoreCreateMutex();
  if (communication_service.send_mutex == NULL) {
    vQueueDelete(communication_service.receive_queue);
    communication_service.receive_queue = NULL;
    return COMMUNICATION_STATUS_NO_MEMORY;
  }

  memset((void *)&communication_service.statistics, 0,
         sizeof(communication_service.statistics));
  communication_service.backend = backend;

  /* The service is visible before start(), so an interrupt enabled by the
   * backend can safely publish a received frame immediately. */
  status = backend->ops->start(backend->context, &receiver);
  if (status != COMMUNICATION_STATUS_OK) {
    communication_service.backend = NULL;
    vSemaphoreDelete(communication_service.send_mutex);
    vQueueDelete(communication_service.receive_queue);
    communication_service.send_mutex = NULL;
    communication_service.receive_queue = NULL;
    return status;
  }

  return COMMUNICATION_STATUS_OK;
}

communication_status_t communication_deinit(void) {
  const communication_backend_t *backend = communication_service.backend;
  SemaphoreHandle_t mutex;
  QueueHandle_t queue;

  if (backend == NULL) {
    return COMMUNICATION_STATUS_NOT_INITIALIZED;
  }

  mutex = communication_service.send_mutex;
  if (xSemaphoreTake(mutex, portMAX_DELAY) != pdPASS) {
    return COMMUNICATION_STATUS_BUSY;
  }

  if (backend->ops->stop != NULL) {
    backend->ops->stop(backend->context);
  }

  queue = communication_service.receive_queue;
  communication_service.backend = NULL;
  communication_service.receive_queue = NULL;
  communication_service.send_mutex = NULL;

  (void)xSemaphoreGive(mutex);
  vSemaphoreDelete(mutex);
  vQueueDelete(queue);

  return COMMUNICATION_STATUS_OK;
}

bool communication_is_initialized(void) {
  return communication_service.backend != NULL;
}

communication_status_t
communication_send(const communication_message_t *message,
                   TickType_t timeout_ticks) {
  communication_status_t status;
  const communication_backend_t *backend = communication_service.backend;

  if (backend == NULL) {
    return COMMUNICATION_STATUS_NOT_INITIALIZED;
  }
  if (!communication_message_is_valid(message)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }
  if (xSemaphoreTake(communication_service.send_mutex, timeout_ticks) !=
      pdPASS) {
    return COMMUNICATION_STATUS_TIMEOUT;
  }

  status = backend->ops->send(backend->context, message, timeout_ticks);
  if (status == COMMUNICATION_STATUS_OK) {
    ++communication_service.statistics.sent_messages;
  } else {
    ++communication_service.statistics.failed_messages;
  }

  (void)xSemaphoreGive(communication_service.send_mutex);
  return status;
}

communication_status_t communication_receive(communication_message_t *message,
                                             TickType_t timeout_ticks) {
  if (communication_service.backend == NULL) {
    return COMMUNICATION_STATUS_NOT_INITIALIZED;
  }
  if (message == NULL) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }
  if (xQueueReceive(communication_service.receive_queue, message,
                    timeout_ticks) != pdPASS) {
    return COMMUNICATION_STATUS_TIMEOUT;
  }
  return COMMUNICATION_STATUS_OK;
}

void communication_get_statistics(communication_statistics_t *statistics) {
  if (statistics == NULL) {
    return;
  }

  taskENTER_CRITICAL();
  *statistics = communication_service.statistics;
  taskEXIT_CRITICAL();
}

void communication_message_init(communication_message_t *message, uint16_t peer,
                                communication_operation_t operation,
                                uint16_t endpoint) {
  if (message == NULL) {
    return;
  }
  memset(message, 0, sizeof(*message));
  message->peer = peer;
  message->operation = operation;
  message->endpoint = endpoint;
}

static bool
communication_message_is_valid(const communication_message_t *message) {
  return (message != NULL) &&
         ((uint32_t)message->operation <
          (uint32_t)COMMUNICATION_OPERATION_COUNT) &&
         (message->priority <= 7U) &&
         (message->payload_length <= COMMUNICATION_MAX_PAYLOAD_SIZE);
}

static BaseType_t
communication_backend_receive(void *context,
                              const communication_message_t *message) {
  communication_service_t *service = (communication_service_t *)context;
  BaseType_t result;

  if ((service == NULL) || (service->receive_queue == NULL) ||
      !communication_message_is_valid(message)) {
    return pdFAIL;
  }

  result = xQueueSend(service->receive_queue, message, 0U);
  if (result == pdPASS) {
    ++service->statistics.received_messages;
  } else {
    ++service->statistics.dropped_received_messages;
  }
  return result;
}

static BaseType_t
communication_backend_receive_from_isr(void *context,
                                       const communication_message_t *message,
                                       BaseType_t *higher_priority_task_woken) {
  communication_service_t *service = (communication_service_t *)context;
  BaseType_t result;

  if ((service == NULL) || (service->receive_queue == NULL) ||
      !communication_message_is_valid(message)) {
    return pdFAIL;
  }

  result = xQueueSendFromISR(service->receive_queue, message,
                             higher_priority_task_woken);
  if (result == pdPASS) {
    ++service->statistics.received_messages;
  } else {
    ++service->statistics.dropped_received_messages;
  }
  return result;
}
