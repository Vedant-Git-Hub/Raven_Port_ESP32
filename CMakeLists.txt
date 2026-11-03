cmake_minimum_required(VERSION 3.16)

# These are local, previously-resolved component sources.  Keeping them in
# EXTRA_COMPONENT_DIRS lets ESP-IDF discover their component names; the root
# manifest pins their dependencies to the same local paths.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../I2C_Keyboard/managed_components/espressif__esp_lcd_touch"
    "${CMAKE_CURRENT_LIST_DIR}/components")

# The two components above are already present locally.  Do not make the
# component manager resolve their registry metadata during an offline build.
set(ENV{IDF_COMPONENT_MANAGER} "0")

# Only pull the application and its declared dependencies; Raven does not
# need Wi-Fi, BLE, HTTP, or the other default example components.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(raven_hello)
