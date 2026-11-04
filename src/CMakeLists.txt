set(RAVEN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../raven")

file(GLOB RAVEN_CORE_SOURCES "${RAVEN_ROOT}/src/core/*.c")
list(REMOVE_ITEM RAVEN_CORE_SOURCES
    "${RAVEN_ROOT}/src/core/thread_posix.c"
    "${RAVEN_ROOT}/src/core/thread_win32.c"
    "${RAVEN_ROOT}/src/core/fs.c"
    "${RAVEN_ROOT}/src/core/fs_watch.c")
file(GLOB RAVEN_RENDER_SOURCES "${RAVEN_ROOT}/src/render/*.c")
list(REMOVE_ITEM RAVEN_RENDER_SOURCES "${RAVEN_ROOT}/src/render/font_system.c")
file(GLOB RAVEN_WIDGET_SOURCES "${RAVEN_ROOT}/src/widget/*.c")
file(GLOB RAVEN_WIDGETS_SOURCES "${RAVEN_ROOT}/src/widgets/*.c")
list(REMOVE_ITEM RAVEN_WIDGETS_SOURCES "${RAVEN_ROOT}/src/widgets/dialogs.c")
file(GLOB RAVEN_XML_SOURCES "${RAVEN_ROOT}/src/xml/*.c")

idf_component_register(
    SRCS
        "main.c"
        "application.c"
        "../port/raven_display.c"
        "../port/raven_esp_backend.c"
        "../port/raven_esp_thread.c"
        "../port/raven_esp_fs.c"
        "../port/raven_esp_fs_watch.c"
        "../port/raven_esp_font_system.c"
        "../port/raven_esp_dialogs.c"
        ${RAVEN_CORE_SOURCES}
        ${RAVEN_RENDER_SOURCES}
        "${RAVEN_ROOT}/src/render/null/null_backend.c"
        "${RAVEN_ROOT}/src/resources/default_font.c"
        "${RAVEN_ROOT}/src/resources/mono_font.c"
        "${RAVEN_ROOT}/src/platform/platform.c"
        "${RAVEN_ROOT}/src/platform/null/null_platform.c"
        "${RAVEN_ROOT}/src/platform/host/host_platform.c"
        ${RAVEN_WIDGET_SOURCES}
        ${RAVEN_WIDGETS_SOURCES}
        ${RAVEN_XML_SOURCES}
    INCLUDE_DIRS "include" "../port/include" "${RAVEN_ROOT}/include"
    PRIV_INCLUDE_DIRS "${RAVEN_ROOT}/src" "${RAVEN_ROOT}/third_party"
    REQUIRES esp_lcd driver esp_timer esp_hw_support esp_mm espressif__esp_lcd_touch espressif__esp_lcd_touch_gt911)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    RAVEN_PLATFORM_ESP_IDF=1
    RAVEN_EMBEDDED=1
    RVN_VERSION_STRING="0.2.0-esp32s3")

# Use Raven's actual featured Studio interface, embedded as immutable firmware
# data because the ESP32 port deliberately has no desktop filesystem watcher.
target_add_binary_data(${COMPONENT_LIB} "${RAVEN_ROOT}/examples/studio/studio.xml" TEXT RENAME_TO studio_xml)
