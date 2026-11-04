#pragma once

#include "esp_lcd_panel_ops.h"
#include <Raven.h>

/** @brief Bind the RGB panel whose PSRAM framebuffer Raven will render into. */
void raven_esp_backend_set_panel(esp_lcd_panel_handle_t panel);
/** @brief Return the ESP32-S3 RGB565 software implementation of Raven's backend. */
const RBackendVTable* raven_esp_backend(void);
