#include "can_app.h"

#include "FreeRTOS.h"
#include "string.h"
#include "task.h"

#define CAN_APP_RX_FIFO CAN_RX_FIFO0
#define CAN_APP_RX_NOTIFICATION CAN_IT_RX_FIFO0_MSG_PENDING
#define CAN_APP_REASSEMBLY_SLOTS 4U
#define CAN_APP_DEFAULT_REASSEMBLY_TIMEOUT_MS 100U

#define CAN_APP_CONTROL_START 0x80U
#define CAN_APP_CONTROL_END 0x40U
#define CAN_APP_CONTROL_SEQUENCE_MASK 0x3FU

typedef struct {
  bool used;
  uint8_t source;
  uint16_t message_id;
  uint8_t expected_sequence;
  uint16_t received_length;
  uint32_t last_activity_ms;
  communication_message_t message;
} can_app_reassembly_t;

typedef struct {
  can_app_config_t config;
  communication_backend_receiver_t receiver;
  can_app_reassembly_t slots[CAN_APP_REASSEMBLY_SLOTS];
  uint16_t next_message_id;
  bool configured;
  bool started;
} can_app_context_t;

static can_app_context_t can_app_context = {0};
static can_app_context_t *can_app_active_context = NULL;

static communication_status_t
can_app_start(void *context, const communication_backend_receiver_t *receiver);
static void can_app_stop(void *context);
static communication_status_t
can_app_send(void *context, const communication_message_t *message,
             TickType_t timeout_ticks);
static communication_status_t
can_app_send_frame(can_app_context_t *context, uint32_t extended_id,
                   const uint8_t *data, uint8_t length, TickType_t start_tick,
                   TickType_t timeout_ticks);
static void can_app_process_frame(can_app_context_t *context,
                                  const CAN_RxHeaderTypeDef *header,
                                  const uint8_t *data,
                                  BaseType_t *higher_priority_task_woken);
static void can_app_process_start_frame(can_app_context_t *context,
                                        uint8_t source, uint16_t message_id,
                                        uint8_t priority,
                                        const CAN_RxHeaderTypeDef *header,
                                        const uint8_t *data,
                                        BaseType_t *higher_priority_task_woken);
static void can_app_process_continuation_frame(
    can_app_context_t *context, uint8_t source, uint16_t message_id,
    const CAN_RxHeaderTypeDef *header, const uint8_t *data,
    BaseType_t *higher_priority_task_woken);
static can_app_reassembly_t *can_app_find_slot(can_app_context_t *context,
                                               uint8_t source,
                                               uint16_t message_id);
static can_app_reassembly_t *can_app_allocate_slot(can_app_context_t *context,
                                                   uint8_t source,
                                                   uint16_t message_id);
static void can_app_expire_slots(can_app_context_t *context);
static void can_app_deliver(can_app_context_t *context,
                            can_app_reassembly_t *slot,
                            BaseType_t *higher_priority_task_woken);

static const communication_backend_ops_t can_app_ops = {
    .start = can_app_start,
    .stop = can_app_stop,
    .send = can_app_send,
};

static communication_backend_t can_app_backend_instance = {
    .ops = &can_app_ops,
    .context = &can_app_context,
};

const communication_backend_t *can_app_backend(const can_app_config_t *config) {
  if ((config == NULL) || (config->handle == NULL) ||
      (config->local_node == CAN_APP_BROADCAST_NODE) ||
      (config->filter_bank > 13U)) {
    return NULL;
  }

  memset(&can_app_context, 0, sizeof(can_app_context));
  can_app_context.config = *config;
  if (can_app_context.config.reassembly_timeout_ms == 0U) {
    can_app_context.config.reassembly_timeout_ms =
        CAN_APP_DEFAULT_REASSEMBLY_TIMEOUT_MS;
  }
  can_app_context.configured = true;
  return &can_app_backend_instance;
}

static communication_status_t
can_app_start(void *context, const communication_backend_receiver_t *receiver) {
  can_app_context_t *can_context = (can_app_context_t *)context;
  CAN_FilterTypeDef filter = {0};

  if ((can_context == NULL) || !can_context->configured ||
      (can_context->config.handle == NULL) || (receiver == NULL) ||
      (receiver->message_received_from_isr == NULL)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }
  if ((can_app_active_context != NULL) || can_context->started) {
    return COMMUNICATION_STATUS_BUSY;
  }

  /* Accept frames in hardware, then select destination and envelope format in
   * software.  This leaves filter-bank policy out of application code. */
  filter.FilterBank = can_context->config.filter_bank;
  filter.FilterMode = CAN_FILTERMODE_IDMASK;
  filter.FilterScale = CAN_FILTERSCALE_32BIT;
  filter.FilterIdHigh = 0U;
  filter.FilterIdLow = 0U;
  filter.FilterMaskIdHigh = 0U;
  filter.FilterMaskIdLow = 0U;
  filter.FilterFIFOAssignment = CAN_APP_RX_FIFO;
  filter.FilterActivation = ENABLE;
  filter.SlaveStartFilterBank = 14U;

  if (HAL_CAN_ConfigFilter(can_context->config.handle, &filter) != HAL_OK) {
    return COMMUNICATION_STATUS_IO_ERROR;
  }
  if (HAL_CAN_Start(can_context->config.handle) != HAL_OK) {
    return COMMUNICATION_STATUS_IO_ERROR;
  }

  can_context->receiver = *receiver;
  can_context->started = true;
  can_app_active_context = can_context;

  if (HAL_CAN_ActivateNotification(can_context->config.handle,
                                   CAN_APP_RX_NOTIFICATION) != HAL_OK) {
    can_app_active_context = NULL;
    can_context->started = false;
    memset(&can_context->receiver, 0, sizeof(can_context->receiver));
    (void)HAL_CAN_Stop(can_context->config.handle);
    return COMMUNICATION_STATUS_IO_ERROR;
  }

  return COMMUNICATION_STATUS_OK;
}

static void can_app_stop(void *context) {
  can_app_context_t *can_context = (can_app_context_t *)context;

  if ((can_context == NULL) || !can_context->started) {
    return;
  }

  (void)HAL_CAN_DeactivateNotification(can_context->config.handle,
                                       CAN_APP_RX_NOTIFICATION);
  (void)HAL_CAN_Stop(can_context->config.handle);
  can_app_active_context = NULL;
  can_context->started = false;
  memset(&can_context->receiver, 0, sizeof(can_context->receiver));
  memset(can_context->slots, 0, sizeof(can_context->slots));
}

static communication_status_t
can_app_send(void *context, const communication_message_t *message,
             TickType_t timeout_ticks) {
  can_app_context_t *can_context = (can_app_context_t *)context;
  uint32_t extended_id;
  uint16_t message_id;
  uint16_t payload_offset = 0U;
  uint8_t sequence = 0U;
  uint8_t frame[8] = {0};
  uint8_t frame_length;
  TickType_t start_tick;
  communication_status_t status;

  if ((can_context == NULL) || !can_context->started || (message == NULL)) {
    return COMMUNICATION_STATUS_NOT_INITIALIZED;
  }
  if (message->peer > UINT8_MAX) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  message_id = can_context->next_message_id & 0x03FFU;
  can_context->next_message_id = (message_id + 1U) & 0x03FFU;

  /* 29-bit identifier: priority[28:26], source[25:18], destination[17:10],
   * message-id[9:0]. */
  extended_id = ((uint32_t)(message->priority & 0x07U) << 26U) |
                ((uint32_t)can_context->config.local_node << 18U) |
                ((uint32_t)message->peer << 10U) | message_id;

  frame[0] = CAN_APP_CONTROL_START;
  frame[1] = (uint8_t)message->operation;
  frame[2] = (uint8_t)(message->endpoint >> 8U);
  frame[3] = (uint8_t)message->endpoint;
  frame[4] = (uint8_t)(message->transaction >> 8U);
  frame[5] = (uint8_t)message->transaction;
  frame[6] = (uint8_t)message->payload_length;
  frame_length = 7U;

  if (message->payload_length > 0U) {
    frame[7] = message->payload[0];
    payload_offset = 1U;
    frame_length = 8U;
  }
  if (payload_offset == message->payload_length) {
    frame[0] |= CAN_APP_CONTROL_END;
  }

  start_tick = xTaskGetTickCount();
  status = can_app_send_frame(can_context, extended_id, frame, frame_length,
                              start_tick, timeout_ticks);
  if (status != COMMUNICATION_STATUS_OK) {
    return status;
  }

  while (payload_offset < message->payload_length) {
    uint16_t remaining = message->payload_length - payload_offset;
    uint8_t chunk = (remaining > 7U) ? 7U : (uint8_t)remaining;

    ++sequence;
    frame[0] = sequence & CAN_APP_CONTROL_SEQUENCE_MASK;
    if (chunk == remaining) {
      frame[0] |= CAN_APP_CONTROL_END;
    }
    memcpy(&frame[1], &message->payload[payload_offset], chunk);
    frame_length = chunk + 1U;

    status = can_app_send_frame(can_context, extended_id, frame, frame_length,
                                start_tick, timeout_ticks);
    if (status != COMMUNICATION_STATUS_OK) {
      return status;
    }
    payload_offset += chunk;
  }

  return COMMUNICATION_STATUS_OK;
}

static communication_status_t
can_app_send_frame(can_app_context_t *context, uint32_t extended_id,
                   const uint8_t *data, uint8_t length, TickType_t start_tick,
                   TickType_t timeout_ticks) {
  CAN_TxHeaderTypeDef header = {0};
  uint32_t mailbox;

  header.ExtId = extended_id;
  header.IDE = CAN_ID_EXT;
  header.RTR = CAN_RTR_DATA;
  header.DLC = length;
  header.TransmitGlobalTime = DISABLE;

  for (;;) {
    HAL_StatusTypeDef hal_status;

    if (HAL_CAN_GetTxMailboxesFreeLevel(context->config.handle) > 0U) {
      hal_status = HAL_CAN_AddTxMessage(context->config.handle, &header,
                                        (uint8_t *)data, &mailbox);
      if (hal_status == HAL_OK) {
        return COMMUNICATION_STATUS_OK;
      }
      if (hal_status != HAL_BUSY) {
        return COMMUNICATION_STATUS_IO_ERROR;
      }
    }

    if (timeout_ticks == 0U) {
      return COMMUNICATION_STATUS_BUSY;
    }
    if ((timeout_ticks != portMAX_DELAY) &&
        ((xTaskGetTickCount() - start_tick) >= timeout_ticks)) {
      return COMMUNICATION_STATUS_TIMEOUT;
    }
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
      return COMMUNICATION_STATUS_BUSY;
    }
    vTaskDelay(1U);
  }
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *handle) {
  can_app_context_t *context = can_app_active_context;
  BaseType_t higher_priority_task_woken = pdFALSE;

  if ((context == NULL) || (handle != context->config.handle)) {
    return;
  }

  while (HAL_CAN_GetRxFifoFillLevel(handle, CAN_APP_RX_FIFO) > 0U) {
    CAN_RxHeaderTypeDef header;
    uint8_t data[8] = {0};

    if (HAL_CAN_GetRxMessage(handle, CAN_APP_RX_FIFO, &header, data) !=
        HAL_OK) {
      break;
    }
    can_app_process_frame(context, &header, data, &higher_priority_task_woken);
  }

  portYIELD_FROM_ISR(higher_priority_task_woken);
}

static void can_app_process_frame(can_app_context_t *context,
                                  const CAN_RxHeaderTypeDef *header,
                                  const uint8_t *data,
                                  BaseType_t *higher_priority_task_woken) {
  uint8_t source;
  uint8_t destination;
  uint8_t priority;
  uint16_t message_id;

  if ((header->IDE != CAN_ID_EXT) || (header->RTR != CAN_RTR_DATA) ||
      (header->DLC == 0U)) {
    return;
  }

  priority = (uint8_t)((header->ExtId >> 26U) & 0x07U);
  source = (uint8_t)((header->ExtId >> 18U) & 0xFFU);
  destination = (uint8_t)((header->ExtId >> 10U) & 0xFFU);
  message_id = (uint16_t)(header->ExtId & 0x03FFU);

  if ((destination != context->config.local_node) &&
      (destination != CAN_APP_BROADCAST_NODE)) {
    return;
  }

  can_app_expire_slots(context);
  if ((data[0] & CAN_APP_CONTROL_START) != 0U) {
    can_app_process_start_frame(context, source, message_id, priority, header,
                                data, higher_priority_task_woken);
  } else {
    can_app_process_continuation_frame(context, source, message_id, header,
                                       data, higher_priority_task_woken);
  }
}

static void can_app_process_start_frame(
    can_app_context_t *context, uint8_t source, uint16_t message_id,
    uint8_t priority, const CAN_RxHeaderTypeDef *header, const uint8_t *data,
    BaseType_t *higher_priority_task_woken) {
  can_app_reassembly_t *slot;
  uint16_t total_length;
  uint16_t first_length;
  bool end;

  if ((header->DLC < 7U) || ((data[0] & CAN_APP_CONTROL_SEQUENCE_MASK) != 0U) ||
      (data[1] >= (uint8_t)COMMUNICATION_OPERATION_COUNT)) {
    return;
  }

  total_length = data[6];
  first_length = (uint16_t)(header->DLC - 7U);
  end = (data[0] & CAN_APP_CONTROL_END) != 0U;
  if ((total_length > COMMUNICATION_MAX_PAYLOAD_SIZE) ||
      (first_length > total_length) ||
      (end && (first_length != total_length)) ||
      (!end && (first_length == total_length))) {
    return;
  }

  slot = can_app_allocate_slot(context, source, message_id);
  if (slot == NULL) {
    return;
  }

  slot->message.peer = source;
  slot->message.priority = priority;
  slot->message.operation = (communication_operation_t)data[1];
  slot->message.endpoint =
      (uint16_t)(((uint16_t)data[2] << 8U) | (uint16_t)data[3]);
  slot->message.transaction =
      (uint16_t)(((uint16_t)data[4] << 8U) | (uint16_t)data[5]);
  slot->message.payload_length = total_length;
  if (first_length > 0U) {
    memcpy(slot->message.payload, &data[7], first_length);
  }
  slot->received_length = first_length;
  slot->expected_sequence = 1U;
  slot->last_activity_ms = HAL_GetTick();

  if (end) {
    can_app_deliver(context, slot, higher_priority_task_woken);
  }
}

static void can_app_process_continuation_frame(
    can_app_context_t *context, uint8_t source, uint16_t message_id,
    const CAN_RxHeaderTypeDef *header, const uint8_t *data,
    BaseType_t *higher_priority_task_woken) {
  can_app_reassembly_t *slot = can_app_find_slot(context, source, message_id);
  uint8_t sequence = data[0] & CAN_APP_CONTROL_SEQUENCE_MASK;
  uint16_t chunk_length;
  uint16_t remaining;
  bool end = (data[0] & CAN_APP_CONTROL_END) != 0U;

  if ((slot == NULL) || (header->DLC < 2U) ||
      (sequence != slot->expected_sequence)) {
    if (slot != NULL) {
      slot->used = false;
    }
    return;
  }

  chunk_length = (uint16_t)(header->DLC - 1U);
  remaining = slot->message.payload_length - slot->received_length;
  if ((chunk_length > remaining) || (end && (chunk_length != remaining)) ||
      (!end && (chunk_length == remaining))) {
    slot->used = false;
    return;
  }

  memcpy(&slot->message.payload[slot->received_length], &data[1], chunk_length);
  slot->received_length += chunk_length;
  ++slot->expected_sequence;
  slot->last_activity_ms = HAL_GetTick();

  if (end) {
    can_app_deliver(context, slot, higher_priority_task_woken);
  }
}

static can_app_reassembly_t *can_app_find_slot(can_app_context_t *context,
                                               uint8_t source,
                                               uint16_t message_id) {
  uint32_t index;

  for (index = 0U; index < CAN_APP_REASSEMBLY_SLOTS; ++index) {
    can_app_reassembly_t *slot = &context->slots[index];
    if (slot->used && (slot->source == source) &&
        (slot->message_id == message_id)) {
      return slot;
    }
  }
  return NULL;
}

static can_app_reassembly_t *can_app_allocate_slot(can_app_context_t *context,
                                                   uint8_t source,
                                                   uint16_t message_id) {
  can_app_reassembly_t *slot = can_app_find_slot(context, source, message_id);
  uint32_t index;

  if (slot == NULL) {
    for (index = 0U; index < CAN_APP_REASSEMBLY_SLOTS; ++index) {
      if (!context->slots[index].used) {
        slot = &context->slots[index];
        break;
      }
    }
  }
  if (slot == NULL) {
    return NULL;
  }

  memset(slot, 0, sizeof(*slot));
  slot->used = true;
  slot->source = source;
  slot->message_id = message_id;
  return slot;
}

static void can_app_expire_slots(can_app_context_t *context) {
  uint32_t now = HAL_GetTick();
  uint32_t index;

  for (index = 0U; index < CAN_APP_REASSEMBLY_SLOTS; ++index) {
    can_app_reassembly_t *slot = &context->slots[index];
    if (slot->used && ((now - slot->last_activity_ms) >=
                       context->config.reassembly_timeout_ms)) {
      slot->used = false;
    }
  }
}

static void can_app_deliver(can_app_context_t *context,
                            can_app_reassembly_t *slot,
                            BaseType_t *higher_priority_task_woken) {
  if ((slot->received_length == slot->message.payload_length) &&
      (context->receiver.message_received_from_isr != NULL)) {
    (void)context->receiver.message_received_from_isr(
        context->receiver.context, &slot->message, higher_priority_task_woken);
  }
  slot->used = false;
}
