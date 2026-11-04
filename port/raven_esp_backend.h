#pragma once

#include "esp_lcd_panel_ops.h"
#include <Raven.h>

void raven_esp_backend_set_panel(esp_lcd_panel_handle_t panel);
const RBackendVTable* raven_esp_backend(void);
