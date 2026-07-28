#include "boot_can.h"

#include "boot_ui.h"
#include "can.h"
#include <string.h>

typedef enum {
  BOOT_CAN_MODE_HANDSHAKE = 0,
  BOOT_CAN_MODE_PACKET,
} boot_can_mode_t;

static volatile boot_can_mode_t receive_mode = BOOT_CAN_MODE_HANDSHAKE;
static volatile bool handshake_pending;
static volatile bool packet_pending;
static volatile bool receive_error;
static volatile uint16_t packet_index;
static uint8_t packet_buffer[BOOT_OTA_PACKET_SIZE];

static uint32_t boot_can_enter_critical(void);
static void boot_can_exit_critical(uint32_t previous_primask);

void boot_can_reset(void) {
  const uint32_t previous_primask = boot_can_enter_critical();

  receive_mode = BOOT_CAN_MODE_HANDSHAKE;
  handshake_pending = false;
  packet_pending = false;
  receive_error = false;
  packet_index = 0U;
  memset(packet_buffer, 0, sizeof(packet_buffer));

  boot_can_exit_critical(previous_primask);
}

void boot_can_start_packet_reception(void) {
  const uint32_t previous_primask = boot_can_enter_critical();

  receive_mode = BOOT_CAN_MODE_PACKET;
  handshake_pending = false;
  packet_pending = false;
  receive_error = false;
  packet_index = 0U;

  boot_can_exit_critical(previous_primask);
}

bool boot_can_take_handshake(void) {
  bool pending;
  const uint32_t previous_primask = boot_can_enter_critical();

  pending = handshake_pending;
  handshake_pending = false;

  boot_can_exit_critical(previous_primask);
  return pending;
}

bool boot_can_take_packet(uint8_t packet[BOOT_OTA_PACKET_SIZE]) {
  bool pending = false;
  const uint32_t previous_primask = boot_can_enter_critical();

  if ((packet != NULL) && packet_pending) {
    memcpy(packet, packet_buffer, sizeof(packet_buffer));
    packet_pending = false;
    packet_index = 0U;
    pending = true;
  }

  boot_can_exit_critical(previous_primask);
  return pending;
}

bool boot_can_has_receive_error(void) {
  bool error;
  const uint32_t previous_primask = boot_can_enter_critical();

  error = receive_error;

  boot_can_exit_critical(previous_primask);
  return error;
}

HAL_StatusTypeDef boot_can_send_reply(void) {
  static const uint8_t reply[BOOT_CAN_FRAME_SIZE] = {
      0U, BOOT_REPLY_COMMAND, BOOT_REPLY_STATUS, 0U, 0U, 0U, 0U, 0U,
  };

  for (uint8_t attempt = 0U; attempt < 3U; ++attempt) {
    const HAL_StatusTypeDef status = send_can_std_frame_8bytes(reply);

    if (status == HAL_OK) {
      return HAL_OK;
    }
    HAL_Delay(1U);
  }
  return HAL_ERROR;
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *handle) {
  CAN_RxHeaderTypeDef header;
  uint8_t frame[BOOT_CAN_FRAME_SIZE];

  if ((handle == NULL) || (handle->Instance != CAN1) ||
      (HAL_CAN_GetRxMessage(handle, CAN_RX_FIFO0, &header, frame) != HAL_OK)) {
    receive_error = true;
    return;
  }

  boot_ui_notify_can_activity();

  if ((header.IDE != CAN_ID_STD) || (header.RTR != CAN_RTR_DATA)) {
    if (receive_mode == BOOT_CAN_MODE_PACKET) {
      receive_error = true;
    }
    return;
  }

  if (receive_mode == BOOT_CAN_MODE_HANDSHAKE) {
    if ((header.DLC == BOOT_CAN_FRAME_SIZE) &&
        (frame[1] == BOOT_CAN_HANDSHAKE_COMMAND)) {
      handshake_pending = true;
    }
    return;
  }

  /*
   * 振动传感器使用标准 ID 0x001，在 Bootloader 运行期间可能仍会短暂发送。
   * 该数据不属于上位机升级数据流，不能占用 264 字节升级包中的位置。
   */
  if (header.StdId == BOOT_CAN_IGNORED_SENSOR_ID) {
    return;
  }

  if ((header.DLC != BOOT_CAN_FRAME_SIZE) || packet_pending ||
      ((uint32_t)packet_index + BOOT_CAN_FRAME_SIZE >
       BOOT_OTA_PACKET_SIZE)) {
    receive_error = true;
    return;
  }

  memcpy(&packet_buffer[packet_index], frame, BOOT_CAN_FRAME_SIZE);
  packet_index = (uint16_t)(packet_index + BOOT_CAN_FRAME_SIZE);
  if (packet_index == BOOT_OTA_PACKET_SIZE) {
    packet_pending = true;
  }
}

static uint32_t boot_can_enter_critical(void) {
  const uint32_t previous_primask = __get_PRIMASK();

  __disable_irq();
  return previous_primask;
}

static void boot_can_exit_critical(uint32_t previous_primask) {
  if (previous_primask == 0U) {
    __enable_irq();
  }
}
