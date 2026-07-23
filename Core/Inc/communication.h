#pragma once

#include "FreeRTOS.h"
#include "stdbool.h"
#include "stdint.h"

/*
 * Protocol-independent application message size.
 *
 * Keeping this value modest is important on an STM32F103 because messages are
 * copied into a FreeRTOS queue.  A backend may impose a smaller limit.
 */
#define COMMUNICATION_MAX_PAYLOAD_SIZE 64U

typedef enum {
  COMMUNICATION_STATUS_OK = 0,
  COMMUNICATION_STATUS_INVALID_ARGUMENT,
  COMMUNICATION_STATUS_NOT_INITIALIZED,
  COMMUNICATION_STATUS_ALREADY_INITIALIZED,
  COMMUNICATION_STATUS_BUSY,
  COMMUNICATION_STATUS_TIMEOUT,
  COMMUNICATION_STATUS_QUEUE_FULL,
  COMMUNICATION_STATUS_NO_MEMORY,
  COMMUNICATION_STATUS_IO_ERROR,
  COMMUNICATION_STATUS_PROTOCOL_ERROR,
  COMMUNICATION_STATUS_UNSUPPORTED,
} communication_status_t;

/*
 * These operations describe application intent, not a CAN frame type or a
 * Modbus function code.  Each backend maps them to its own wire format.
 */
typedef enum {
  COMMUNICATION_OPERATION_READ_REQUEST = 0,
  COMMUNICATION_OPERATION_WRITE_REQUEST,
  COMMUNICATION_OPERATION_READ_RESPONSE,
  COMMUNICATION_OPERATION_WRITE_RESPONSE,
  COMMUNICATION_OPERATION_EVENT,
  COMMUNICATION_OPERATION_ERROR_RESPONSE,

  COMMUNICATION_OPERATION_COUNT,
} communication_operation_t;

typedef struct {
  /* Remote logical node.  Modbus additionally uses 0 for broadcast writes. */
  uint16_t peer;

  /* 0 is the highest priority.  Backends without priorities ignore it. */
  uint8_t priority;
  communication_operation_t operation;

  /* Protocol-neutral object/register/service address. */
  uint16_t endpoint;

  /* Chosen by the requester and copied into the corresponding response. */
  uint16_t transaction;

  uint16_t payload_length;
  uint8_t payload[COMMUNICATION_MAX_PAYLOAD_SIZE];
} communication_message_t;

/* Receiver hooks supplied by the communication core to a backend. */
typedef struct {
  void *context;
  BaseType_t (*message_received)(void *context,
                                 const communication_message_t *message);
  BaseType_t (*message_received_from_isr)(
      void *context, const communication_message_t *message,
      BaseType_t *higher_priority_task_woken);
} communication_backend_receiver_t;

typedef struct {
  communication_status_t (*start)(
      void *context, const communication_backend_receiver_t *receiver);
  void (*stop)(void *context);
  communication_status_t (*send)(void *context,
                                 const communication_message_t *message,
                                 TickType_t timeout_ticks);
} communication_backend_ops_t;

typedef struct {
  const communication_backend_ops_t *ops;
  void *context;
} communication_backend_t;

typedef struct {
  uint32_t received_messages;
  uint32_t dropped_received_messages;
  uint32_t sent_messages;
  uint32_t failed_messages;
} communication_statistics_t;

/*
 * Initializes the single system communication service.
 * Call this from task context after the scheduler has started.
 * The backend object must remain valid until communication_deinit().
 */
communication_status_t
communication_init(const communication_backend_t *backend);

/*
 * Stops the current backend and releases the queues/mutex.
 * All users of the communication service must be quiescent before this call.
 */
communication_status_t communication_deinit(void);

bool communication_is_initialized(void);

/* Task-context API.  Sending is serialized so protocol backends stay simple. */
communication_status_t
communication_send(const communication_message_t *message,
                   TickType_t timeout_ticks);

communication_status_t communication_receive(communication_message_t *message,
                                             TickType_t timeout_ticks);

void communication_get_statistics(communication_statistics_t *statistics);

/* Convenience initializer; transaction and payload remain zero. */
void communication_message_init(communication_message_t *message, uint16_t peer,
                                communication_operation_t operation,
                                uint16_t endpoint);
