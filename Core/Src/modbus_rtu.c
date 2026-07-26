#include "modbus_rtu.h"

#include "semphr.h"
#include "string.h"

typedef struct {
  modbus_rtu_config_t configuration;
  communication_transport_driver_t driver;
  communication_transport_receive_t receive_callback;
  void *receive_callback_context;
  SemaphoreHandle_t transmit_mutex;
  bool configured;
  bool started;
} modbus_rtu_service_t;

static modbus_rtu_service_t modbus_rtu_service;

static communication_status_t modbus_rtu_start(
    void *context, communication_transport_receive_t receive_callback,
    void *callback_context);
static void modbus_rtu_stop(void *context);
static communication_status_t
modbus_rtu_send(void *context,
                communication_physical_address_t transmit_address,
                const uint8_t *payload, size_t payload_size,
                TickType_t timeout_ticks);

const communication_transport_driver_t *
modbus_rtu_driver(const modbus_rtu_config_t *configuration) {
  if ((configuration == NULL) || (configuration->write == NULL) ||
      modbus_rtu_service.started) {
    return NULL;
  }

  memset(&modbus_rtu_service, 0, sizeof(modbus_rtu_service));
  modbus_rtu_service.configuration = *configuration;
  modbus_rtu_service.driver.name = "Modbus RTU";
  modbus_rtu_service.driver.context = &modbus_rtu_service;
  modbus_rtu_service.driver.maximum_payload_size = MODBUS_RTU_MAX_PDU_SIZE;
  modbus_rtu_service.driver.start = modbus_rtu_start;
  modbus_rtu_service.driver.stop = modbus_rtu_stop;
  modbus_rtu_service.driver.send = modbus_rtu_send;
  modbus_rtu_service.configured = true;
  return &modbus_rtu_service.driver;
}

communication_status_t modbus_rtu_receive_adu(const uint8_t *adu,
                                              size_t adu_size) {
  uint16_t expected_crc;
  uint16_t received_crc;
  uint8_t unit_id;

  if (!modbus_rtu_service.started) {
    return COMMUNICATION_STATUS_NOT_INITIALIZED;
  }
  if ((adu == NULL) || (adu_size < 4U) ||
      (adu_size > MODBUS_RTU_MAX_ADU_SIZE)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  unit_id = adu[0];
  if ((unit_id == 0U) || (unit_id > 247U)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  expected_crc = modbus_rtu_crc16(adu, adu_size - 2U);
  received_crc = (uint16_t)adu[adu_size - 2U] |
                 ((uint16_t)adu[adu_size - 1U] << 8U);
  if (expected_crc != received_crc) {
    return COMMUNICATION_STATUS_TRANSPORT_ERROR;
  }

  return modbus_rtu_service.receive_callback(
      modbus_rtu_service.receive_callback_context, unit_id, &adu[1],
      adu_size - 3U);
}

uint16_t modbus_rtu_crc16(const uint8_t *data, size_t data_size) {
  uint16_t crc = UINT16_C(0xFFFF);

  if ((data == NULL) && (data_size > 0U)) {
    return 0U;
  }

  for (size_t index = 0U; index < data_size; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      if ((crc & 1U) != 0U) {
        crc = (uint16_t)((crc >> 1U) ^ UINT16_C(0xA001));
      } else {
        crc >>= 1U;
      }
    }
  }
  return crc;
}

static communication_status_t modbus_rtu_start(
    void *context, communication_transport_receive_t receive_callback,
    void *callback_context) {
  modbus_rtu_service_t *service = (modbus_rtu_service_t *)context;

  if ((service != &modbus_rtu_service) || !service->configured ||
      (receive_callback == NULL)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }
  if (service->started) {
    return COMMUNICATION_STATUS_ALREADY_INITIALIZED;
  }

  service->transmit_mutex = xSemaphoreCreateMutex();
  if (service->transmit_mutex == NULL) {
    return COMMUNICATION_STATUS_NO_MEMORY;
  }
  service->receive_callback = receive_callback;
  service->receive_callback_context = callback_context;
  service->started = true;
  return COMMUNICATION_STATUS_OK;
}

static void modbus_rtu_stop(void *context) {
  modbus_rtu_service_t *service = (modbus_rtu_service_t *)context;

  if ((service != &modbus_rtu_service) || !service->configured) {
    return;
  }

  service->started = false;
  if (service->transmit_mutex != NULL) {
    vSemaphoreDelete(service->transmit_mutex);
    service->transmit_mutex = NULL;
  }
  service->receive_callback = NULL;
  service->receive_callback_context = NULL;
}

static communication_status_t
modbus_rtu_send(void *context,
                communication_physical_address_t transmit_address,
                const uint8_t *payload, size_t payload_size,
                TickType_t timeout_ticks) {
  modbus_rtu_service_t *service = (modbus_rtu_service_t *)context;
  uint8_t adu[MODBUS_RTU_MAX_ADU_SIZE];
  uint16_t crc;
  communication_status_t status;

  if ((service != &modbus_rtu_service) || !service->started ||
      (payload == NULL) || (payload_size == 0U) ||
      (payload_size > MODBUS_RTU_MAX_PDU_SIZE) ||
      (transmit_address == 0U) || (transmit_address > 247U)) {
    return COMMUNICATION_STATUS_INVALID_ARGUMENT;
  }

  if (xSemaphoreTake(service->transmit_mutex, timeout_ticks) != pdPASS) {
    return (timeout_ticks == 0U) ? COMMUNICATION_STATUS_BUSY
                                 : COMMUNICATION_STATUS_TIMEOUT;
  }

  adu[0] = (uint8_t)transmit_address;
  memcpy(&adu[1], payload, payload_size);
  crc = modbus_rtu_crc16(adu, payload_size + 1U);
  adu[payload_size + 1U] = (uint8_t)(crc & 0xFFU);
  adu[payload_size + 2U] = (uint8_t)(crc >> 8U);

  status = service->configuration.write(
      service->configuration.port_context, adu, payload_size + 3U,
      timeout_ticks);
  xSemaphoreGive(service->transmit_mutex);
  return status;
}
