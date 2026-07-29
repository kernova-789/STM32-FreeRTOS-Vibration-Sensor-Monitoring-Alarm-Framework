#pragma once

#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 这些地址和标志值必须与现有 Bootloader 保持一致。
 * Bootloader 在 0x0801FF30 读到 0x0000 后进入 CAN 升级模式。
 */
#define BOOTLOADER_OTA_REQUEST_PAGE_ADDRESS UINT32_C(0x0801FC00)
#define BOOTLOADER_OTA_FLAG_ADDRESS UINT32_C(0x0801FF30)
#define BOOTLOADER_OTA_REQUEST_FLAG UINT16_C(0x0000)
#define BOOTLOADER_OTA_REQUEST_FRAME_SIZE 8U
#define BOOTLOADER_OTA_REQUEST_COMMAND_INDEX 1U
#define BOOTLOADER_OTA_REQUEST_COMMAND UINT8_C(0x06)

typedef enum {
  BOOTLOADER_OTA_STATUS_OK = 0,
  BOOTLOADER_OTA_STATUS_FLASH_ERROR,
  BOOTLOADER_OTA_STATUS_VERIFY_ERROR,
} bootloader_ota_status_t;

/*
 * 与 Bootloader 握手判定保持一致：标准数据帧长度为 8，第二字节为 0x06。
 * CAN ID 由传输层处理，不参与 OTA 请求判定。
 */
bool bootloader_ota_is_request_frame(const uint8_t *payload,
                                     size_t payload_size);

/*
 * 擦除 OTA 请求页并写入升级标志。
 * 请求页与产品配置页完全分离，不在这里备份或转换任何业务配置。
 * 成功后本函数直接触发系统复位，不会返回。
 */
bootloader_ota_status_t bootloader_ota_enter(void);

#ifdef __cplusplus
}
#endif
