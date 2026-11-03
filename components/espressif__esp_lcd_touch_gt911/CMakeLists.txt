# The vendor component's manifest normally supplies this dependency.  The
# Raven example intentionally builds without the component manager, so retain
# the original source while declaring it explicitly in this local wrapper.
idf_component_register(
    SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../I2C_Keyboard/managed_components/espressif__esp_lcd_touch_gt911/esp_lcd_touch_gt911.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../I2C_Keyboard/managed_components/espressif__esp_lcd_touch_gt911/include"
    REQUIRES esp_lcd espressif__esp_lcd_touch
)
