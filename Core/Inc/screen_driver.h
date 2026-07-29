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
} screen_driver_t;

/* 128x64、ST7565 兼容屏幕的任务上下文接口。 */
HAL_StatusTypeDef screen_driver_init(screen_driver_t *driver,
                                     const screen_driver_config_t *config);

void screen_driver_clear(screen_driver_t *driver);
void screen_driver_fill_row(screen_driver_t *driver, uint8_t row, bool on);
void screen_driver_draw_pixel(screen_driver_t *driver, uint8_t x, uint8_t y,
                              bool on);
void screen_driver_draw_line(screen_driver_t *driver, uint8_t x0, uint8_t y0,
                             uint8_t x1, uint8_t y1, bool on);

/* 绘制 5x7 ASCII 字符串；row 范围为 0..7，column 使用像素列坐标。 */
void screen_driver_draw_text(screen_driver_t *driver, uint8_t row,
                             uint8_t column, const char *text, bool inverse);

/* 绘制 8x16 ASCII 字符串；row 范围为 0..3，column 使用像素列坐标。 */
void screen_driver_draw_text_large(screen_driver_t *driver, uint8_t row,
                                   uint8_t column, const char *text,
                                   bool inverse);

/* 按八个页地址分批把完整帧缓冲发送到屏幕。 */
HAL_StatusTypeDef screen_driver_flush(screen_driver_t *driver);
