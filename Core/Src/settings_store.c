#include "settings_store.h"

#include "FreeRTOS.h"
#include "stdbool.h"
#include "stm32f1xx_hal.h"
#include "string.h"
#include "task.h"
#include <stddef.h>

#define SETTINGS_STORE_PAGE_MAGIC UINT32_C(0x53455453) /* ASCII 标记“SETS” */
#define SETTINGS_STORE_FORMAT_VERSION UINT16_C(1)
#define SETTINGS_STORE_COMMIT_VALUE UINT16_C(0x0000)
#define SETTINGS_STORE_ERASED_HALFWORD UINT16_C(0xFFFF)

typedef struct {
  uint32_t magic;
  uint16_t format_version;
  uint16_t header_size;
  uint32_t generation;
  uint32_t header_crc;
  uint16_t reserved;
  uint16_t commit_marker;
} settings_store_page_header_t;

typedef struct {
  settings_store_key_t key;
  uint16_t value_size;
  uint32_t sequence;
  uint32_t value_crc;
  uint32_t header_crc;
  uint16_t reserved;
  uint16_t commit_marker;
} settings_store_record_header_t;

typedef struct {
  settings_store_key_t key;
  uint16_t value_size;
  uint32_t sequence;
  uint32_t value_address;
} settings_store_record_reference_t;

typedef struct {
  settings_store_record_reference_t records[SETTINGS_STORE_MAX_KEY_COUNT];
  size_t record_count;
  uint32_t maximum_sequence;
  uint32_t tail_address;
  bool appendable;
} settings_store_scan_t;

typedef struct {
  uint32_t active_page_address;
  uint32_t generation;
  uint32_t next_sequence;
  bool initialized;
  bool busy;
} settings_store_service_t;

_Static_assert(sizeof(settings_store_page_header_t) == 20U,
               "Unexpected settings page header padding");
_Static_assert(sizeof(settings_store_record_header_t) == 20U,
               "Unexpected settings record header padding");
_Static_assert((SETTINGS_STORE_PAGE0_ADDRESS % SETTINGS_STORE_PAGE_SIZE) == 0U,
               "Settings page 0 is not page aligned");
_Static_assert((SETTINGS_STORE_PAGE1_ADDRESS % SETTINGS_STORE_PAGE_SIZE) == 0U,
               "Settings page 1 is not page aligned");

static settings_store_service_t settings_store_service;

static bool settings_store_lock(void);
static void settings_store_unlock(void);
static uint32_t settings_store_crc32(const void *data, size_t data_size);
static uint32_t settings_store_aligned_size(uint32_t size);
static uint32_t settings_store_other_page(uint32_t page_address);
static bool settings_store_generation_is_newer(uint32_t left,
                                               uint32_t right);
static bool settings_store_page_is_valid(uint32_t page_address,
                                         uint32_t *generation);
static settings_store_status_t
settings_store_scan_page(uint32_t page_address, settings_store_scan_t *scan);
static settings_store_status_t settings_store_append_record(
    uint32_t page_end, uint32_t *tail_address, settings_store_key_t key,
    const void *value, uint16_t value_size, uint32_t sequence);
static settings_store_status_t settings_store_compact_and_write(
    settings_store_key_t key, const void *value, uint16_t value_size,
    uint32_t sequence, const settings_store_scan_t *source_scan);
static settings_store_status_t
settings_store_write_internal(settings_store_key_t key, const void *value,
                              uint16_t value_size);
static settings_store_status_t
settings_store_prepare_page(uint32_t page_address, uint32_t generation);
static settings_store_status_t
settings_store_commit_page(uint32_t page_address);
static settings_store_status_t settings_store_erase_page(uint32_t page_address);
static settings_store_status_t settings_store_flash_program(
    uint32_t address, const void *data, size_t data_size);

settings_store_status_t settings_store_init(void) {
  bool page0_valid;
  bool page1_valid;
  uint32_t page0_generation = 0U;
  uint32_t page1_generation = 0U;
  settings_store_scan_t scan;
  settings_store_status_t status;

  if (settings_store_service.initialized) {
    return SETTINGS_STORE_STATUS_OK;
  }
  if (!settings_store_lock()) {
    return SETTINGS_STORE_STATUS_BUSY;
  }

  page0_valid = settings_store_page_is_valid(SETTINGS_STORE_PAGE0_ADDRESS,
                                             &page0_generation);
  page1_valid = settings_store_page_is_valid(SETTINGS_STORE_PAGE1_ADDRESS,
                                             &page1_generation);
  if (page0_valid &&
      (settings_store_scan_page(SETTINGS_STORE_PAGE0_ADDRESS, &scan) !=
       SETTINGS_STORE_STATUS_OK)) {
    page0_valid = false;
  }
  if (page1_valid &&
      (settings_store_scan_page(SETTINGS_STORE_PAGE1_ADDRESS, &scan) !=
       SETTINGS_STORE_STATUS_OK)) {
    page1_valid = false;
  }

  if (!page0_valid && !page1_valid) {
    status = settings_store_prepare_page(SETTINGS_STORE_PAGE0_ADDRESS, 1U);
    if (status == SETTINGS_STORE_STATUS_OK) {
      status = settings_store_commit_page(SETTINGS_STORE_PAGE0_ADDRESS);
    }
    if (status != SETTINGS_STORE_STATUS_OK) {
      settings_store_unlock();
      return status;
    }
    settings_store_service.active_page_address =
        SETTINGS_STORE_PAGE0_ADDRESS;
    settings_store_service.generation = 1U;
  } else if (page0_valid &&
             (!page1_valid ||
              settings_store_generation_is_newer(page0_generation,
                                                 page1_generation))) {
    settings_store_service.active_page_address =
        SETTINGS_STORE_PAGE0_ADDRESS;
    settings_store_service.generation = page0_generation;
  } else {
    settings_store_service.active_page_address =
        SETTINGS_STORE_PAGE1_ADDRESS;
    settings_store_service.generation = page1_generation;
  }

  status = settings_store_scan_page(
      settings_store_service.active_page_address, &scan);
  if (status != SETTINGS_STORE_STATUS_OK) {
    settings_store_unlock();
    return status;
  }

  settings_store_service.next_sequence = scan.maximum_sequence + 1U;
  if (settings_store_service.next_sequence == 0U) {
    settings_store_service.next_sequence = 1U;
  }
  settings_store_service.initialized = true;
  settings_store_unlock();
  return SETTINGS_STORE_STATUS_OK;
}

settings_store_status_t
settings_store_read(settings_store_key_t key, void *value,
                    size_t value_capacity, size_t *value_size) {
  settings_store_scan_t scan;
  settings_store_record_reference_t *record = NULL;
  settings_store_status_t status;

  if ((key == 0U) || (key == UINT16_MAX) || (value_size == NULL) ||
      ((value == NULL) && (value_capacity > 0U))) {
    return SETTINGS_STORE_STATUS_INVALID_ARGUMENT;
  }
  if (!settings_store_service.initialized) {
    return SETTINGS_STORE_STATUS_NOT_INITIALIZED;
  }
  if (!settings_store_lock()) {
    return SETTINGS_STORE_STATUS_BUSY;
  }

  status = settings_store_scan_page(
      settings_store_service.active_page_address, &scan);
  if (status != SETTINGS_STORE_STATUS_OK) {
    settings_store_unlock();
    return status;
  }

  for (size_t index = 0U; index < scan.record_count; ++index) {
    if (scan.records[index].key == key) {
      record = &scan.records[index];
      break;
    }
  }

  if ((record == NULL) || (record->value_size == 0U)) {
    settings_store_unlock();
    return SETTINGS_STORE_STATUS_NOT_FOUND;
  }

  *value_size = record->value_size;
  if (value_capacity < record->value_size) {
    settings_store_unlock();
    return SETTINGS_STORE_STATUS_DATA_TOO_LARGE;
  }

  memcpy(value, (const void *)(uintptr_t)record->value_address,
         record->value_size);
  settings_store_unlock();
  return SETTINGS_STORE_STATUS_OK;
}

settings_store_status_t
settings_store_write(settings_store_key_t key, const void *value,
                     size_t value_size) {
  settings_store_status_t status;

  if ((key == 0U) || (key == UINT16_MAX) || (value == NULL) ||
      (value_size == 0U)) {
    return SETTINGS_STORE_STATUS_INVALID_ARGUMENT;
  }
  if (value_size > SETTINGS_STORE_MAX_VALUE_SIZE) {
    return SETTINGS_STORE_STATUS_DATA_TOO_LARGE;
  }
  if (!settings_store_service.initialized) {
    return SETTINGS_STORE_STATUS_NOT_INITIALIZED;
  }
  if (!settings_store_lock()) {
    return SETTINGS_STORE_STATUS_BUSY;
  }

  status = settings_store_write_internal(key, value, (uint16_t)value_size);
  settings_store_unlock();
  return status;
}

settings_store_status_t settings_store_delete(settings_store_key_t key) {
  settings_store_status_t status;

  if ((key == 0U) || (key == UINT16_MAX)) {
    return SETTINGS_STORE_STATUS_INVALID_ARGUMENT;
  }
  if (!settings_store_service.initialized) {
    return SETTINGS_STORE_STATUS_NOT_INITIALIZED;
  }
  if (!settings_store_lock()) {
    return SETTINGS_STORE_STATUS_BUSY;
  }

  status = settings_store_write_internal(key, NULL, 0U);
  settings_store_unlock();
  return status;
}

settings_store_status_t settings_store_get_info(settings_store_info_t *info) {
  settings_store_scan_t scan;
  settings_store_status_t status;
  uint16_t valid_key_count = 0U;

  if (info == NULL) {
    return SETTINGS_STORE_STATUS_INVALID_ARGUMENT;
  }
  if (!settings_store_service.initialized) {
    return SETTINGS_STORE_STATUS_NOT_INITIALIZED;
  }
  if (!settings_store_lock()) {
    return SETTINGS_STORE_STATUS_BUSY;
  }

  status = settings_store_scan_page(
      settings_store_service.active_page_address, &scan);
  if (status == SETTINGS_STORE_STATUS_OK) {
    for (size_t index = 0U; index < scan.record_count; ++index) {
      if (scan.records[index].value_size > 0U) {
        ++valid_key_count;
      }
    }
    info->active_page_address =
        settings_store_service.active_page_address;
    info->generation = settings_store_service.generation;
    info->next_sequence = settings_store_service.next_sequence;
    info->used_bytes =
        (uint16_t)(scan.tail_address -
                   settings_store_service.active_page_address);
    info->valid_key_count = valid_key_count;
  }

  settings_store_unlock();
  return status;
}

settings_store_status_t settings_store_factory_reset(void) {
  settings_store_status_t status;

  if (!settings_store_lock()) {
    return SETTINGS_STORE_STATUS_BUSY;
  }

  status = settings_store_erase_page(SETTINGS_STORE_PAGE0_ADDRESS);
  if (status == SETTINGS_STORE_STATUS_OK) {
    status = settings_store_erase_page(SETTINGS_STORE_PAGE1_ADDRESS);
  }
  if (status == SETTINGS_STORE_STATUS_OK) {
    status = settings_store_prepare_page(SETTINGS_STORE_PAGE0_ADDRESS, 1U);
  }
  if (status == SETTINGS_STORE_STATUS_OK) {
    status = settings_store_commit_page(SETTINGS_STORE_PAGE0_ADDRESS);
  }

  if (status == SETTINGS_STORE_STATUS_OK) {
    settings_store_service.active_page_address =
        SETTINGS_STORE_PAGE0_ADDRESS;
    settings_store_service.generation = 1U;
    settings_store_service.next_sequence = 1U;
    settings_store_service.initialized = true;
  }
  settings_store_unlock();
  return status;
}

static bool settings_store_lock(void) {
  bool acquired = false;

  taskENTER_CRITICAL();
  if (!settings_store_service.busy) {
    settings_store_service.busy = true;
    acquired = true;
  }
  taskEXIT_CRITICAL();
  return acquired;
}

static void settings_store_unlock(void) {
  taskENTER_CRITICAL();
  settings_store_service.busy = false;
  taskEXIT_CRITICAL();
}

static uint32_t settings_store_crc32(const void *data, size_t data_size) {
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = UINT32_MAX;

  for (size_t index = 0U; index < data_size; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask =
          (uint32_t)-(int32_t)(crc & UINT32_C(1));
      crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
    }
  }
  return ~crc;
}

static uint32_t settings_store_aligned_size(uint32_t size) {
  return (size + 1U) & ~UINT32_C(1);
}

static uint32_t settings_store_other_page(uint32_t page_address) {
  return (page_address == SETTINGS_STORE_PAGE0_ADDRESS)
             ? SETTINGS_STORE_PAGE1_ADDRESS
             : SETTINGS_STORE_PAGE0_ADDRESS;
}

static bool settings_store_generation_is_newer(uint32_t left,
                                               uint32_t right) {
  return (int32_t)(left - right) > 0;
}

static bool settings_store_page_is_valid(uint32_t page_address,
                                         uint32_t *generation) {
  const settings_store_page_header_t *header =
      (const settings_store_page_header_t *)(uintptr_t)page_address;
  uint32_t expected_crc;

  if ((header->magic != SETTINGS_STORE_PAGE_MAGIC) ||
      (header->format_version != SETTINGS_STORE_FORMAT_VERSION) ||
      (header->header_size != sizeof(*header)) ||
      (header->commit_marker != SETTINGS_STORE_COMMIT_VALUE)) {
    return false;
  }

  expected_crc =
      settings_store_crc32(header, offsetof(settings_store_page_header_t,
                                            header_crc));
  if (expected_crc != header->header_crc) {
    return false;
  }

  if (generation != NULL) {
    *generation = header->generation;
  }
  return true;
}

static settings_store_status_t
settings_store_scan_page(uint32_t page_address, settings_store_scan_t *scan) {
  const uint32_t page_end = page_address + SETTINGS_STORE_PAGE_SIZE;
  uint32_t cursor = page_address + sizeof(settings_store_page_header_t);

  if ((scan == NULL) || !settings_store_page_is_valid(page_address, NULL)) {
    return SETTINGS_STORE_STATUS_CORRUPT;
  }
  memset(scan, 0, sizeof(*scan));
  scan->tail_address = cursor;
  scan->appendable = true;

  while ((cursor + sizeof(settings_store_record_header_t)) <= page_end) {
    const settings_store_record_header_t *header =
        (const settings_store_record_header_t *)(uintptr_t)cursor;
    const uint32_t record_size =
        sizeof(*header) + settings_store_aligned_size(header->value_size);
    const uint8_t *value =
        (const uint8_t *)(uintptr_t)(cursor + sizeof(*header));
    settings_store_record_reference_t *reference = NULL;

    if ((header->key == UINT16_MAX) &&
        (header->value_size == UINT16_MAX)) {
      scan->tail_address = cursor;
      return SETTINGS_STORE_STATUS_OK;
    }

    if ((header->commit_marker != SETTINGS_STORE_COMMIT_VALUE) ||
        (header->value_size > SETTINGS_STORE_MAX_VALUE_SIZE) ||
        (record_size > (page_end - cursor))) {
      /*
       * 这种情况通常表示追加最后一条记录时掉电。
       * 已提交记录仍可读取，但再次写入前必须先执行换页整理。
       */
      scan->tail_address = page_end;
      scan->appendable = false;
      return SETTINGS_STORE_STATUS_OK;
    }

    if ((header->key == 0U) || (header->key == UINT16_MAX) ||
        (header->header_crc !=
         settings_store_crc32(
             header, offsetof(settings_store_record_header_t, header_crc))) ||
        (header->value_crc !=
         settings_store_crc32(value, header->value_size))) {
      return SETTINGS_STORE_STATUS_CORRUPT;
    }

    for (size_t index = 0U; index < scan->record_count; ++index) {
      if (scan->records[index].key == header->key) {
        reference = &scan->records[index];
        break;
      }
    }
    if (reference == NULL) {
      if (scan->record_count >= SETTINGS_STORE_MAX_KEY_COUNT) {
        return SETTINGS_STORE_STATUS_FULL;
      }
      reference = &scan->records[scan->record_count++];
    }

    reference->key = header->key;
    reference->value_size = header->value_size;
    reference->sequence = header->sequence;
    reference->value_address =
        cursor + sizeof(settings_store_record_header_t);
    if (settings_store_generation_is_newer(header->sequence,
                                           scan->maximum_sequence) ||
        (scan->maximum_sequence == 0U)) {
      scan->maximum_sequence = header->sequence;
    }

    cursor += record_size;
    scan->tail_address = cursor;
  }

  if (cursor != page_end) {
    scan->appendable = false;
    scan->tail_address = page_end;
  }
  return SETTINGS_STORE_STATUS_OK;
}

static settings_store_status_t settings_store_append_record(
    uint32_t page_end, uint32_t *tail_address, settings_store_key_t key,
    const void *value, uint16_t value_size, uint32_t sequence) {
  settings_store_record_header_t header;
  const uint32_t padded_value_size = settings_store_aligned_size(value_size);
  const uint32_t record_size = sizeof(header) + padded_value_size;
  settings_store_status_t status;

  if ((tail_address == NULL) || ((*tail_address + record_size) > page_end)) {
    return SETTINGS_STORE_STATUS_FULL;
  }

  memset(&header, 0xFF, sizeof(header));
  header.key = key;
  header.value_size = value_size;
  header.sequence = sequence;
  header.value_crc = settings_store_crc32(value, value_size);
  header.header_crc = settings_store_crc32(
      &header, offsetof(settings_store_record_header_t, header_crc));

  status = settings_store_flash_program(
      *tail_address, &header,
      offsetof(settings_store_record_header_t, commit_marker));
  if ((status == SETTINGS_STORE_STATUS_OK) && (value_size > 0U)) {
    status = settings_store_flash_program(
        *tail_address + sizeof(header), value, value_size);
  }
  if (status == SETTINGS_STORE_STATUS_OK) {
    const uint16_t commit = SETTINGS_STORE_COMMIT_VALUE;
    status = settings_store_flash_program(
        *tail_address +
            offsetof(settings_store_record_header_t, commit_marker),
        &commit, sizeof(commit));
  }

  if (status == SETTINGS_STORE_STATUS_OK) {
    *tail_address += record_size;
  }
  return status;
}

static settings_store_status_t settings_store_compact_and_write(
    settings_store_key_t key, const void *value, uint16_t value_size,
    uint32_t sequence, const settings_store_scan_t *source_scan) {
  const uint32_t target_page = settings_store_other_page(
      settings_store_service.active_page_address);
  const uint32_t target_end = target_page + SETTINGS_STORE_PAGE_SIZE;
  const uint32_t new_generation = settings_store_service.generation + 1U;
  uint32_t target_tail = target_page + sizeof(settings_store_page_header_t);
  settings_store_status_t status;

  status = settings_store_prepare_page(target_page, new_generation);
  if (status != SETTINGS_STORE_STATUS_OK) {
    return status;
  }

  for (size_t index = 0U; index < source_scan->record_count; ++index) {
    const settings_store_record_reference_t *record =
        &source_scan->records[index];

    if ((record->key == key) || (record->value_size == 0U)) {
      continue;
    }
    status = settings_store_append_record(
        target_end, &target_tail, record->key,
        (const void *)(uintptr_t)record->value_address, record->value_size,
        record->sequence);
    if (status != SETTINGS_STORE_STATUS_OK) {
      return status;
    }
  }

  status = settings_store_append_record(target_end, &target_tail, key, value,
                                        value_size, sequence);
  if (status == SETTINGS_STORE_STATUS_OK) {
    status = settings_store_commit_page(target_page);
  }
  if (status == SETTINGS_STORE_STATUS_OK) {
    settings_store_service.active_page_address = target_page;
    settings_store_service.generation = new_generation;
  }
  return status;
}

static settings_store_status_t
settings_store_write_internal(settings_store_key_t key, const void *value,
                              uint16_t value_size) {
  settings_store_scan_t scan;
  settings_store_status_t status;
  bool key_already_exists = false;
  uint32_t sequence = settings_store_service.next_sequence;
  uint32_t tail;
  const uint32_t page_end =
      settings_store_service.active_page_address + SETTINGS_STORE_PAGE_SIZE;
  const uint32_t needed =
      sizeof(settings_store_record_header_t) +
      settings_store_aligned_size(value_size);

  status = settings_store_scan_page(
      settings_store_service.active_page_address, &scan);
  if (status != SETTINGS_STORE_STATUS_OK) {
    return status;
  }

  for (size_t index = 0U; index < scan.record_count; ++index) {
    if (scan.records[index].key == key) {
      key_already_exists = true;
      break;
    }
  }
  if (!key_already_exists &&
      (scan.record_count >= SETTINGS_STORE_MAX_KEY_COUNT)) {
    return SETTINGS_STORE_STATUS_FULL;
  }

  tail = scan.tail_address;
  if (scan.appendable && ((page_end - tail) >= needed)) {
    status = settings_store_append_record(page_end, &tail, key, value,
                                          value_size, sequence);
  } else {
    status = settings_store_compact_and_write(key, value, value_size, sequence,
                                              &scan);
  }

  if (status == SETTINGS_STORE_STATUS_OK) {
    ++settings_store_service.next_sequence;
    if (settings_store_service.next_sequence == 0U) {
      settings_store_service.next_sequence = 1U;
    }
  }
  return status;
}

static settings_store_status_t
settings_store_prepare_page(uint32_t page_address, uint32_t generation) {
  settings_store_page_header_t header;
  settings_store_status_t status = settings_store_erase_page(page_address);

  if (status != SETTINGS_STORE_STATUS_OK) {
    return status;
  }

  memset(&header, 0xFF, sizeof(header));
  header.magic = SETTINGS_STORE_PAGE_MAGIC;
  header.format_version = SETTINGS_STORE_FORMAT_VERSION;
  header.header_size = sizeof(header);
  header.generation = generation;
  header.header_crc = settings_store_crc32(
      &header, offsetof(settings_store_page_header_t, header_crc));

  return settings_store_flash_program(
      page_address, &header,
      offsetof(settings_store_page_header_t, commit_marker));
}

static settings_store_status_t
settings_store_commit_page(uint32_t page_address) {
  const uint16_t commit = SETTINGS_STORE_COMMIT_VALUE;

  return settings_store_flash_program(
      page_address + offsetof(settings_store_page_header_t, commit_marker),
      &commit, sizeof(commit));
}

static settings_store_status_t settings_store_erase_page(uint32_t page_address) {
  FLASH_EraseInitTypeDef erase = {
      .TypeErase = FLASH_TYPEERASE_PAGES,
      .PageAddress = page_address,
      .NbPages = 1U,
  };
  uint32_t page_error = 0U;
  HAL_StatusTypeDef hal_status;

  if (HAL_FLASH_Unlock() != HAL_OK) {
    return SETTINGS_STORE_STATUS_FLASH_ERROR;
  }
  hal_status = HAL_FLASHEx_Erase(&erase, &page_error);
  (void)HAL_FLASH_Lock();

  if ((hal_status != HAL_OK) || (page_error != UINT32_MAX)) {
    return SETTINGS_STORE_STATUS_FLASH_ERROR;
  }
  return SETTINGS_STORE_STATUS_OK;
}

static settings_store_status_t settings_store_flash_program(
    uint32_t address, const void *data, size_t data_size) {
  const uint8_t *bytes = (const uint8_t *)data;
  HAL_StatusTypeDef hal_status = HAL_OK;

  if (((address & 1U) != 0U) || (data == NULL) || (data_size == 0U)) {
    return SETTINGS_STORE_STATUS_INVALID_ARGUMENT;
  }
  if (HAL_FLASH_Unlock() != HAL_OK) {
    return SETTINGS_STORE_STATUS_FLASH_ERROR;
  }

  for (size_t offset = 0U; offset < data_size; offset += 2U) {
    uint16_t halfword = bytes[offset];

    if ((offset + 1U) < data_size) {
      halfword |= (uint16_t)bytes[offset + 1U] << 8U;
    } else {
      halfword |= UINT16_C(0xFF00);
    }
    hal_status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                   address + (uint32_t)offset, halfword);
    if (hal_status != HAL_OK) {
      break;
    }
  }

  (void)HAL_FLASH_Lock();
  return (hal_status == HAL_OK) ? SETTINGS_STORE_STATUS_OK
                                : SETTINGS_STORE_STATUS_FLASH_ERROR;
}
