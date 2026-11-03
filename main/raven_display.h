#pragma once

#include "esp_lcd_panel_ops.h"

#define RAVEN_DISPLAY_WIDTH 800
#define RAVEN_DISPLAY_HEIGHT 480

void raven_display_init(void);
esp_lcd_panel_handle_t raven_display_panel(void);
int raven_display_wait_for_vblank(void);
void raven_touch_poll(int* pressed, int* x, int* y);
