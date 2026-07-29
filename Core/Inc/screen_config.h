#pragma once
#include "screen_driver.h"
#include "spi.h"


#define SCREEN_TASK_STACK_DEPTH (configMINIMAL_STACK_SIZE + 192U)
#define SCREEN_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define SCREEN_TASK_PERIOD_MS 20U
#define SCREEN_REFRESH_PERIOD_MS 200U

#define SCREEN_MEASUREMENT_QUEUE_LENGTH 1U
#define SCREEN_SETTINGS_QUEUE_LENGTH 2U
#define SCREEN_EVENT_QUEUE_LENGTH 8U

#define SCREEN_VALUE_MAX 9999U
#define SCREEN_ACCELERATION_MAX_CENTI_G 1600U
#define SCREEN_LIMIT_FINE_STEP 5U
#define SCREEN_LIMIT_COARSE_STEP 50U
#define SCREEN_TRIGGER_FINE_STEP 1U
#define SCREEN_TRIGGER_COARSE_STEP 100U

const screen_driver_config_t driver_config = {
    .spi = &hspi2,
    .chip_select_port = SCREEN_CS_GPIO_Port,
    .chip_select_pin = SCREEN_CS_Pin,
    .data_command_port = SCREEN_DC_GPIO_Port,
    .data_command_pin = SCREEN_DC_Pin,
    .reset_port = SCREEN_RESET_GPIO_Port,
    .reset_pin = SCREEN_RESET_Pin,
};
