#include "screen_driver.h"

#include "FreeRTOS.h"
#include "string.h"
#include "task.h"

#define SCREEN_DRIVER_SPI_TIMEOUT_MS 100U
#define SCREEN_DRIVER_GLYPH_WIDTH 5U
#define SCREEN_DRIVER_CHARACTER_WIDTH 6U
#define SCREEN_DRIVER_LARGE_GLYPH_WIDTH 7U
#define SCREEN_DRIVER_LARGE_CHARACTER_WIDTH 8U
#define SCREEN_DRIVER_LARGE_ROW_COUNT (SCREEN_DRIVER_PAGE_COUNT / 2U)

typedef struct {
  char character;
  uint8_t columns[SCREEN_DRIVER_GLYPH_WIDTH];
} screen_glyph_t;

/* 每列字模的最低位对应字符顶部像素。 */
static const screen_glyph_t screen_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'(', {0x00, 0x1C, 0x22, 0x41, 0x00}},
    {')', {0x00, 0x41, 0x22, 0x1C, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'?', {0x02, 0x01, 0x51, 0x09, 0x06}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

static HAL_StatusTypeDef screen_driver_write(screen_driver_t *driver,
                                             bool is_data, const uint8_t *bytes,
                                             uint16_t length);
static HAL_StatusTypeDef screen_driver_command(screen_driver_t *driver,
                                               uint8_t command);
static const uint8_t *screen_driver_find_glyph(char character);
static uint16_t screen_driver_expand_glyph_column(uint8_t column);
static void screen_driver_delay(uint32_t milliseconds);

HAL_StatusTypeDef screen_driver_init(screen_driver_t *driver,
                                     const screen_driver_config_t *config) {
  static const uint8_t initialization_commands[] = {
      0x29U, /* 电源控制：电压转换器 */
      0x2BU, /* 电源控制：转换器和稳压器 */
      0x2FU, /* 电源控制：转换器、稳压器和跟随器 */
      0x24U, /* 电阻比 */
      0x81U, /* 电子音量命令 */
      0x20U, /* 电子音量值 */
      0xA2U, /* 1/9 偏压 */
      0xA0U, /* 段扫描方向 */
      0xC8U, /* COM 扫描方向 */
      0xAFU, /* 开启显示 */
      0xA4U, /* 正常显示显存内容 */
  };

  if ((driver == NULL) || (config == NULL) || (config->spi == NULL) ||
      (config->chip_select_port == NULL) ||
      (config->data_command_port == NULL) || (config->reset_port == NULL)) {
    return HAL_ERROR;
  }

  memset(driver, 0, sizeof(*driver));
  driver->config = *config;
  HAL_GPIO_WritePin(config->chip_select_port, config->chip_select_pin,
                    GPIO_PIN_SET);
  HAL_GPIO_WritePin(config->reset_port, config->reset_pin, GPIO_PIN_RESET);
  screen_driver_delay(100U);
  HAL_GPIO_WritePin(config->reset_port, config->reset_pin, GPIO_PIN_SET);
  screen_driver_delay(100U);
  /* 通过硬件引脚复位屏幕控制器。 */
  if (screen_driver_command(driver, 0xE2U) != HAL_OK) {
    return HAL_ERROR;
  }
  screen_driver_delay(10U);

  for (uint32_t command_index = 0U; command_index < sizeof(initialization_commands);
       ++command_index) {
    if (screen_driver_command(driver, initialization_commands[command_index]) !=
        HAL_OK) {
      return HAL_ERROR;
    }
    if (command_index == 2U) {
      screen_driver_delay(100U);
    } else {
      screen_driver_delay(10U);
    }
  }

  driver->initialized = true;
  driver->display_enabled = true;
  screen_driver_clear(driver);
  return screen_driver_flush(driver);
}

void screen_driver_clear(screen_driver_t *driver) {
  if (driver == NULL) {
    return;
  }
  memset(driver->framebuffer, 0, sizeof(driver->framebuffer));
}

void screen_driver_fill_row(screen_driver_t *driver, uint8_t row, bool on) {
  if ((driver == NULL) || (row >= SCREEN_DRIVER_PAGE_COUNT)) {
    return;
  }
  memset(&driver->framebuffer[(uint16_t)row * SCREEN_DRIVER_WIDTH],
         on ? 0xFF : 0x00, SCREEN_DRIVER_WIDTH);
}

void screen_driver_draw_text(screen_driver_t *driver, uint8_t row,
                             uint8_t column, const char *text, bool inverse) {
  if ((driver == NULL) || (text == NULL) || (row >= SCREEN_DRIVER_PAGE_COUNT)) {
    return;
  }

  while ((*text != '\0') && ((uint16_t)column + SCREEN_DRIVER_CHARACTER_WIDTH <=
                             SCREEN_DRIVER_WIDTH)) {
    const uint8_t *glyph = screen_driver_find_glyph(*text);
    uint8_t glyph_column;
    uint16_t buffer_index = ((uint16_t)row * SCREEN_DRIVER_WIDTH) + column;

    for (glyph_column = 0U; glyph_column < SCREEN_DRIVER_GLYPH_WIDTH;
         ++glyph_column) {
      driver->framebuffer[buffer_index + glyph_column] =
          inverse ? (uint8_t)~glyph[glyph_column] : glyph[glyph_column];
    }
    driver->framebuffer[buffer_index + SCREEN_DRIVER_GLYPH_WIDTH] =
        inverse ? 0xFFU : 0x00U;

    column += SCREEN_DRIVER_CHARACTER_WIDTH;
    ++text;
  }
}

void screen_driver_draw_text_large(screen_driver_t *driver, uint8_t row,
                                   uint8_t column, const char *text,
                                   bool inverse) {
  if ((driver == NULL) || (text == NULL) ||
      (row >= SCREEN_DRIVER_LARGE_ROW_COUNT)) {
    return;
  }

  while ((*text != '\0') &&
         ((uint16_t)column + SCREEN_DRIVER_LARGE_CHARACTER_WIDTH <=
          SCREEN_DRIVER_WIDTH)) {
    const uint8_t *glyph = screen_driver_find_glyph(*text);
    const uint16_t upper_page_offset =
        ((uint16_t)row * 2U * SCREEN_DRIVER_WIDTH) + column;
    const uint16_t lower_page_offset =
        upper_page_offset + SCREEN_DRIVER_WIDTH;

    for (uint8_t glyph_column = 0U;
         glyph_column < SCREEN_DRIVER_LARGE_GLYPH_WIDTH; ++glyph_column) {
      const uint8_t source_column =
          (uint8_t)(((uint16_t)glyph_column * SCREEN_DRIVER_GLYPH_WIDTH) /
                    SCREEN_DRIVER_LARGE_GLYPH_WIDTH);
      uint16_t expanded =
          screen_driver_expand_glyph_column(glyph[source_column]);

      if (inverse) {
        expanded = (uint16_t)~expanded;
      }
      driver->framebuffer[upper_page_offset + glyph_column] =
          (uint8_t)(expanded & 0xFFU);
      driver->framebuffer[lower_page_offset + glyph_column] =
          (uint8_t)(expanded >> 8U);
    }

    driver->framebuffer[upper_page_offset + SCREEN_DRIVER_LARGE_GLYPH_WIDTH] =
        inverse ? 0xFFU : 0x00U;
    driver->framebuffer[lower_page_offset + SCREEN_DRIVER_LARGE_GLYPH_WIDTH] =
        inverse ? 0xFFU : 0x00U;
    column += SCREEN_DRIVER_LARGE_CHARACTER_WIDTH;
    ++text;
  }
}

HAL_StatusTypeDef screen_driver_set_enabled(screen_driver_t *driver,
                                            bool enabled) {
  HAL_StatusTypeDef status;

  if ((driver == NULL) || !driver->initialized) {
    return HAL_ERROR;
  }
  if (driver->display_enabled == enabled) {
    return HAL_OK;
  }

  status = screen_driver_command(driver, enabled ? 0xAFU : 0xAEU);
  if (status == HAL_OK) {
    driver->display_enabled = enabled;
  }
  return status;
}

HAL_StatusTypeDef screen_driver_flush(screen_driver_t *driver) {
  if ((driver == NULL) || !driver->initialized) {
    return HAL_ERROR;
  }

  for (uint8_t page = 0U; page < SCREEN_DRIVER_PAGE_COUNT; ++page) {
    const uint8_t page_commands[] = {
        (uint8_t)(0xB0U | page),
        0x10U,
        0x00U,
    };
    if (screen_driver_write(driver, false, page_commands,
                            sizeof(page_commands)) != HAL_OK) {
      return HAL_ERROR;
    }
    if (screen_driver_write(
            driver, true,
            &driver->framebuffer[(uint16_t)page * SCREEN_DRIVER_WIDTH],
            SCREEN_DRIVER_WIDTH) != HAL_OK) {
      return HAL_ERROR;
    }
  }
  return HAL_OK;
}

static HAL_StatusTypeDef screen_driver_write(screen_driver_t *driver,
                                             bool is_data, const uint8_t *bytes,
                                             uint16_t length) {
  HAL_StatusTypeDef status;

  if ((driver == NULL) || (bytes == NULL) || (length == 0U)) {
    return HAL_ERROR;
  }

  HAL_GPIO_WritePin(driver->config.data_command_port,
                    driver->config.data_command_pin,
                    is_data ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(driver->config.chip_select_port,
                    driver->config.chip_select_pin, GPIO_PIN_RESET);
  status = HAL_SPI_Transmit(driver->config.spi, (uint8_t *)bytes, length,
                            SCREEN_DRIVER_SPI_TIMEOUT_MS);
  HAL_GPIO_WritePin(driver->config.chip_select_port,
                    driver->config.chip_select_pin, GPIO_PIN_SET);
  return status;
}

static HAL_StatusTypeDef screen_driver_command(screen_driver_t *driver,
                                               uint8_t command) {
  return screen_driver_write(driver, false, &command, 1U);
}

static const uint8_t *screen_driver_find_glyph(char character) {
  uint32_t glyph_index;
  const uint32_t glyph_count = sizeof(screen_font) / sizeof(screen_font[0]);

  if ((character >= 'a') && (character <= 'z')) {
    character = (char)(character - ('a' - 'A'));
  }
  for (glyph_index = 0U; glyph_index < glyph_count; ++glyph_index) {
    if (screen_font[glyph_index].character == character) {
      return screen_font[glyph_index].columns;
    }
  }

  /* 字符 '?' 位于字模表第 17 项，用作不支持字符的替代字形。 */
  return screen_font[17].columns;
}

static uint16_t screen_driver_expand_glyph_column(uint8_t column) {
  uint16_t expanded = 0U;

  /*
   * 将原字模每列的七个像素分别纵向复制一倍，并在上下各留一个空像素：
   * 1 + (7 * 2) + 1 = 16 像素。
   */
  for (uint8_t source_row = 0U; source_row < 7U; ++source_row) {
    if ((column & (uint8_t)(1U << source_row)) != 0U) {
      expanded |= (uint16_t)(3U << ((source_row * 2U) + 1U));
    }
  }
  return expanded;
}

static void screen_driver_delay(uint32_t milliseconds) {
  TickType_t ticks = pdMS_TO_TICKS(milliseconds);
  vTaskDelay((ticks == 0U) ? 1U : ticks);
}
