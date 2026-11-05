#pragma once

#include "esp_lcd_panel_ops.h"

/** Native horizontal panel resolution in pixels. */
#define RAVEN_DISPLAY_WIDTH 800
/** Native vertical panel resolution in pixels. */
#define RAVEN_DISPLAY_HEIGHT 480

/** @brief Initialize the RGB panel, backlight, GT911 and frame-complete ISR. */
void raven_display_init(void);
/** @brief Return the initialized RGB panel for use by the Raven backend. */
esp_lcd_panel_handle_t raven_display_panel(void);
/** @brief Wait for a fresh vertical blank interval; returns non-zero on success. */
int raven_display_wait_for_vblank(void);
/**
 * @brief Poll the GT911 controller and return its primary contact.
 *
 * GPIO38 is reserved for the board's GT911 reset/address path, not used as an
 * ESP32 interrupt input by this port.
 */
void raven_touch_poll(int* pressed, int* x, int* y);
