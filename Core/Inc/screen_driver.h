#pragma once

#include "stdbool.h"
#include "stdint.h"
#include "stm32f1xx_hal.h"

#define SCREEN_DRIVER_WIDTH 128U
#define SCREEN_DRIVER_HEIGHT 64U
#define SCREEN_DRIVER_PAGE_COUNT (SCREEN_DRIVER_HEIGHT / 8U)
#define SCREEN_DRIVER_BUFFER_SIZE                                              \
  (SCREEN_DRIVER_WIDTH * SCREEN_DRIVER_PAGE_COUNT)

typedef struct {
  SPI_HandleTypeDef *spi;
  GPIO_TypeDef *chip_select_port;
  uint16_t chip_select_pin;
  GPIO_TypeDef *data_command_port;
  uint16_t data_command_pin;
  GPIO_TypeDef *reset_port;
  uint16_t reset_pin;
} screen_driver_config_t;

typedef struct {
  screen_driver_config_t config;
  uint8_t framebuffer[SCREEN_DRIVER_BUFFER_SIZE];
  bool initialized;
  bool display_enabled;
} screen_driver_t;

/* Task-context API for the 128x64 ST7565-compatible display. */
HAL_StatusTypeDef screen_driver_init(screen_driver_t *driver,
                                     const screen_driver_config_t *config);

void screen_driver_clear(screen_driver_t *driver);
void screen_driver_fill_row(screen_driver_t *driver, uint8_t row, bool on);

/* Draws a 5x7 ASCII string. row is 0..7 and column is a pixel column. */
void screen_driver_draw_text(screen_driver_t *driver, uint8_t row,
                             uint8_t column, const char *text, bool inverse);

/* Draws an 8x16 ASCII string. row is 0..3 and column is a pixel column. */
void screen_driver_draw_text_large(screen_driver_t *driver, uint8_t row,
                                   uint8_t column, const char *text,
                                   bool inverse);

/* Turns the LCD pixels on/off without clearing the framebuffer. */
HAL_StatusTypeDef screen_driver_set_enabled(screen_driver_t *driver,
                                            bool enabled);

/* Sends the complete framebuffer to the display in eight page bursts. */
HAL_StatusTypeDef screen_driver_flush(screen_driver_t *driver);
