#include "boot_ui.h"

#include "boot_config.h"
#include "boot_hardware.h"
#include "screen.h"
#include "stm32f1xx_hal.h"

#define BOOT_UI_ARRAY_LENGTH(array)                                         \
  ((uint8_t)(sizeof(array) / sizeof((array)[0])))

/*
 * 这些字模索引序列描述的是 Bootloader 界面内容，
 * 因此放在本模块中，而不是底层显示驱动中。
 */
static const uint8_t boot_ui_text_upgrading[] = {0x00U, 0x01U, 0x02U};
static const uint8_t boot_ui_text_error[] = {0x03U, 0x04U, 0x05U};
static const uint8_t boot_ui_text_connect_pc[] = {
    0x06U, 0x07U, 0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU};
static const uint8_t boot_ui_text_retry[] = {0x06U, 0x0EU, 0x0FU};

typedef enum {
  BOOT_UI_BLUE_PHASE_IDLE = 0,
  BOOT_UI_BLUE_PHASE_ON,
  BOOT_UI_BLUE_PHASE_OFF,
} boot_ui_blue_phase_t;

static volatile bool boot_ui_can_activity_pending;
static volatile bool boot_ui_can_activity_enabled = true;
static volatile boot_ui_blue_phase_t boot_ui_blue_phase;
static volatile uint32_t boot_ui_blue_phase_time_left_ms;

static bool boot_ui_take_can_activity(void);
static void boot_ui_start_blue_pulse(void);

void boot_ui_init(void) {
  ScreenInit();
}

void boot_ui_tick_1ms(void) {
  if (!boot_ui_can_activity_enabled) {
    return;
  }

  switch (boot_ui_blue_phase) {
  case BOOT_UI_BLUE_PHASE_IDLE:
    if (boot_ui_take_can_activity()) {
      boot_ui_start_blue_pulse();
    }
    break;

  case BOOT_UI_BLUE_PHASE_ON:
    if (boot_ui_blue_phase_time_left_ms > 0U) {
      --boot_ui_blue_phase_time_left_ms;
    }
    if (boot_ui_blue_phase_time_left_ms == 0U) {
      boot_hardware_indicator_set(BOOT_INDICATOR_BLUE, false);
      boot_ui_blue_phase = BOOT_UI_BLUE_PHASE_OFF;
      boot_ui_blue_phase_time_left_ms = BOOT_CAN_ACTIVITY_OFF_MS;
    }
    break;

  case BOOT_UI_BLUE_PHASE_OFF:
  default:
    if (boot_ui_blue_phase_time_left_ms > 0U) {
      --boot_ui_blue_phase_time_left_ms;
    }
    if (boot_ui_blue_phase_time_left_ms == 0U) {
      if (boot_ui_take_can_activity()) {
        boot_ui_start_blue_pulse();
      } else {
        boot_ui_blue_phase = BOOT_UI_BLUE_PHASE_IDLE;
      }
    }
    break;
  }
}

void boot_ui_notify_can_activity(void) {
  if (boot_ui_can_activity_enabled) {
    boot_ui_can_activity_pending = true;
  }
}

void boot_ui_set_can_activity_enabled(bool enabled) {
  const uint32_t previous_primask = __get_PRIMASK();

  __disable_irq();
  boot_ui_can_activity_enabled = enabled;
  if (!enabled) {
    boot_ui_can_activity_pending = false;
    boot_ui_blue_phase = BOOT_UI_BLUE_PHASE_IDLE;
    boot_ui_blue_phase_time_left_ms = 0U;
    boot_hardware_indicator_set(BOOT_INDICATOR_BLUE, false);
  }
  if (previous_primask == 0U) {
    __enable_irq();
  }
}

void boot_ui_show_waiting(bool application_invalid) {
  Clear_Screen();
  if (application_invalid) {
    Disp_HZ16str(0U, 39U, boot_ui_text_error,
                 BOOT_UI_ARRAY_LENGTH(boot_ui_text_error));
    boot_hardware_indicator_set(BOOT_INDICATOR_RED, true);
  }
  Disp_HZ16str(1U, 0U, boot_ui_text_connect_pc,
               BOOT_UI_ARRAY_LENGTH(boot_ui_text_connect_pc));
}

void boot_ui_show_upgrading(void) {
  Clear_Screen();
  boot_hardware_indicator_set(BOOT_INDICATOR_RED, false);
  Disp_HZ16str(1U, 39U, boot_ui_text_upgrading,
               BOOT_UI_ARRAY_LENGTH(boot_ui_text_upgrading));
}

void boot_ui_show_error(void) {
  Clear_Screen();
  boot_hardware_indicator_set(BOOT_INDICATOR_RED, true);
  Disp_HZ16str(0U, 39U, boot_ui_text_error,
               BOOT_UI_ARRAY_LENGTH(boot_ui_text_error));
  Disp_HZ16str(1U, 39U, boot_ui_text_retry,
               BOOT_UI_ARRAY_LENGTH(boot_ui_text_retry));
}

static bool boot_ui_take_can_activity(void) {
  const bool pending = boot_ui_can_activity_pending;

  boot_ui_can_activity_pending = false;
  return pending;
}

static void boot_ui_start_blue_pulse(void) {
  boot_hardware_indicator_set(BOOT_INDICATOR_BLUE, true);
  boot_ui_blue_phase = BOOT_UI_BLUE_PHASE_ON;
  boot_ui_blue_phase_time_left_ms = BOOT_CAN_ACTIVITY_ON_MS;
}
