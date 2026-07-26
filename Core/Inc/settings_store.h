#pragma once

#include "settings_store_config.h"
#include "stdint.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t settings_store_key_t;

typedef enum {
  SETTINGS_STORE_STATUS_OK = 0,
  SETTINGS_STORE_STATUS_NOT_FOUND,
  SETTINGS_STORE_STATUS_INVALID_ARGUMENT,
  SETTINGS_STORE_STATUS_NOT_INITIALIZED,
  SETTINGS_STORE_STATUS_BUSY,
  SETTINGS_STORE_STATUS_DATA_TOO_LARGE,
  SETTINGS_STORE_STATUS_FULL,
  SETTINGS_STORE_STATUS_CORRUPT,
  SETTINGS_STORE_STATUS_FLASH_ERROR,
} settings_store_status_t;

typedef struct {
  uint32_t active_page_address;
  uint32_t generation;
  uint32_t next_sequence;
  uint16_t used_bytes;
  uint16_t valid_key_count;
} settings_store_info_t;

/*
 * 带版本和 CRC 保护的日志结构键值存储。
 *
 * 每条记录最后写提交标记。当前页写满后，把仍有效的记录复制到另一页，
 * 复制全部完成后才提交新页。因此写入过程中掉电时，旧代或新代至少有一页有效。
 *
 * 所有接口均为同步接口，只能在任务上下文调用。
 */
settings_store_status_t settings_store_init(void);
settings_store_status_t
settings_store_read(settings_store_key_t key, void *value,
                    size_t value_capacity, size_t *value_size);
settings_store_status_t
settings_store_write(settings_store_key_t key, const void *value,
                     size_t value_size);
settings_store_status_t settings_store_delete(settings_store_key_t key);
settings_store_status_t settings_store_get_info(settings_store_info_t *info);

/* 擦除两个配置页，仅用于用户明确触发的恢复出厂流程。 */
settings_store_status_t settings_store_factory_reset(void);

#ifdef __cplusplus
}
#endif
