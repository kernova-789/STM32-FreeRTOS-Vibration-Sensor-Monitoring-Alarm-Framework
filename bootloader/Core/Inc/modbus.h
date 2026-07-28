#ifndef MODBUS_H
#define MODBUS_H

#include "stdint.h"
#define OTA_Protocol_CAN

#ifndef OTA_Protocol_CAN
#define OTA_Protocol_485
#endif

uint8_t OTA485_ProcessPacket(uint8_t *rx_buf, uint16_t rx_len);


#endif