# The original registry manifest calls this dependency "esp_lcd_touch".
# This project vendors it under its ESP-IDF component name so offline builds
# resolve it without the component manager.
idf_component_register(SRCS "esp_lcd_touch_gt911.c" INCLUDE_DIRS "include"
                       REQUIRES "esp_lcd" "espressif__esp_lcd_touch")
