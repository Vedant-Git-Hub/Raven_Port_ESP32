# Raven Studio on Elecrow ESP32-S3 HMI

This is a self-contained ESP-IDF project for the 800×480 Elecrow RGB HMI.
It runs Raven's upstream Studio example using an RGB565 software backend and
translates GT911 touch input into Raven mouse events.

## Project layout

```
src/
  main.c              Board entry point, task placement, and touch event queue
  application.c       Raven Studio application and its Raven callbacks
  application.h       Small boundary between the entry point and application
  CMakeLists.txt      Builds the application with Raven and the selected port
raven/                Complete, vendored Raven source checkout
port/
  raven_display.*     RGB-panel and GT911 implementation for this HMI
  raven_esp_*         ESP-IDF replacements for Raven desktop services
  components/         Vendored ESP LCD Touch and GT911 ESP-IDF components
```

`src` is application code and `raven` is the library source plus its minimal
ESP-IDF compatibility changes. All platform-specific code lives under `port`.
To carry the application to a different platform, retain `src` and `raven`,
then replace the relevant files and ESP-IDF components in `port`. No files are
read from the separate I2C example project at build time.

## Build

This project was built with ESP-ADF's ESP-IDF 5.5.2:

```bash
export IDF_PATH=/home/vedant/esp/esp-adf/esp-idf
/home/vedant/.espressif/python_env/idf5.5_py3.10_env/bin/python "$IDF_PATH/tools/idf.py" build
```

The firmware is `build/raven_hello.bin`. For a connected board:

```bash
export IDF_PATH=/home/vedant/esp/esp-adf/esp-idf
/home/vedant/.espressif/python_env/idf5.5_py3.10_env/bin/python "$IDF_PATH/tools/idf.py" -p /dev/ttyUSB0 flash monitor
```

## Embedded scope

The port supports Raven's retained widget tree, layouts, labels, buttons,
hit-testing, alpha font atlases, RGB(A) textures, rounded rectangles,
clipping, and the upstream Studio node graph example. Rendering is CPU
software rendering directly into the panel's PSRAM scanout buffer.

Desktop-only services are intentionally stubbed: desktop filesystem and file
watching, native dialogs, URL launching, desktop font discovery, process
execution, Vulkan, and native windowing. The port is a starting point for a
hardware-accelerated renderer or a LittleFS-backed filesystem if needed.
