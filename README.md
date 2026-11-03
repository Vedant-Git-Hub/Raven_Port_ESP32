# Raven Hello for Elecrow ESP32-S3 HMI

This is an independent ESP-IDF project for the 800x480 Elecrow RGB HMI. It
uses Raven's host-platform API and a small custom `RBackendVTable` software
renderer that writes RGB565 pixels into the ESP LCD RGB panel's PSRAM scanout
buffer. The GT911 touch controller is translated into Raven mouse events.

The display timing, pin mapping, backlight GPIO, and GT911 I2C configuration
come from `../I2C_Keyboard`; that project is only read as a source of
configuration and component code and is not modified. The panel uses its
established 10 MHz pixel clock. This initial software renderer uses one PSRAM
scanout buffer and Raven repaints it only when the UI changes.

## Build

This checkout's working toolchain is ESP-ADF's bundled ESP-IDF 5.5.2 (the
standalone `esp-idf` directory has a toolchain-version mismatch). From this
directory run:

```bash
export IDF_PATH=/home/vedant/esp/esp-adf/esp-idf
/home/vedant/.espressif/python_env/idf5.5_py3.10_env/bin/python "$IDF_PATH/tools/idf.py" build
```

The verified output is `build/raven_hello.bin`. To program a connected board,
replace the example serial device as appropriate:

```bash
export IDF_PATH=/home/vedant/esp/esp-adf/esp-idf
/home/vedant/.espressif/python_env/idf5.5_py3.10_env/bin/python "$IDF_PATH/tools/idf.py" -p /dev/ttyACM0 flash monitor
```

The program displays **Hello from Raven** and a **Touch me** button. Tapping
the button changes its status text to confirm touch-to-Raven input delivery.

## Port structure

- `main/raven_hello.c` creates the Raven widgets and injects GT911 touch input.
- `main/raven_display.c` owns the RGB panel and GT911 initialization.
- `main/raven_esp_backend.c` implements Raven's custom rendering backend for
  RGB565 PSRAM framebuffers.
- `main/raven_esp_thread.c` maps Raven synchronization to FreeRTOS.
- `main/raven_esp_fs*.c`, `raven_esp_font_system.c`, and
  `raven_esp_dialogs.c` provide MCU-safe replacements for desktop services.
- `components/espressif__esp_lcd_touch_gt911` is a local CMake wrapper around
  the existing GT911 component; it supplies an explicit dependency needed when
  building without ESP-IDF's component manager.

The Raven clone is expected beside the ESP directory at `/home/vedant/Raven`.
The project CMake file deliberately references that clone rather than copying
Raven source files into this project.

## Current embedded scope

The hello application supports Raven's normal retained widget tree, layout,
labels, buttons, hit testing, touch events, alpha font atlas, RGB(A) textures,
rounded rectangles, and clipping. Rendering is CPU software rendering, so this
is a functional starting point rather than a high-FPS GPU backend.

Desktop-specific services are deliberately disabled or stubbed: native file
dialogs, host filesystem browsing/writing, external URL launching, desktop
font discovery, process execution, live file watching, Vulkan, and native
windowing. Blur, glass, and shader draw primitives currently fall back to a
plain rounded-rectangle rendering path. Add SPIFFS/LittleFS support and a
hardware-accelerated backend later if the application needs those features.
