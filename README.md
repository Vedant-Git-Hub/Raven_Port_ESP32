# Raven Studio on Elecrow 7-inch ESP32-S3 HMI

This repository is a self-contained ESP-IDF port of the
[Raven graphics library](https://github.com/oscar-soirey/Raven) for Elecrow's
800×480 ESP32-S3 RGB HMI. It runs Raven's upstream **Studio** sample: a
retained-mode interface with a node graph, timeline, colour picker, theme
selector, and interactive controls.

The port uses an RGB565 software renderer that writes into the RGB panel's
PSRAM framebuffer. GT911 touch is normalized into Raven mouse events. Raven
renders on CPU1; the low-priority touch task runs on CPU0.

## Repository layout

```
src/                    Application-only code
  main.c                ESP-IDF entry point and task setup
  application.c         Raven Studio test application
  include/              Application public headers
raven/                  Full vendored Raven source tree
port/                   All board/OS/rendering adaptation code
  include/              Port public headers
  components/           Vendored ESP-IDF touch components
  Readme_port.md        Board-porting guide
Doxyfile                Documentation configuration
```

`src` must not contain board pins, panel APIs, or platform-specific logic.
Those belong in `port`, allowing a new platform to retain the application and
Raven while replacing only the port layer. See
[the porting guide](port/Readme_port.md) for the exact boundary and checklist.

## Requirements

- Elecrow 7-inch ESP32-S3 HMI (800×480 RGB panel, GT911 touch)
- ESP-IDF 5.5.x; this checkout was verified with ESP-ADF's bundled ESP-IDF
  5.5.2
- Python environment installed by ESP-IDF
- USB serial access to the board

The source and required GT911 components are already in this repository; no
separate Raven clone or I2C example project is needed.

## Build and flash

From the repository root:

```bash
export IDF_PATH=/home/vedant/esp/esp-adf/esp-idf
/home/vedant/.espressif/python_env/idf5.5_py3.10_env/bin/python "$IDF_PATH/tools/idf.py" build
```

Flash a connected board (change the port if necessary):

```bash
export IDF_PATH=/home/vedant/esp/esp-adf/esp-idf
/home/vedant/.espressif/python_env/idf5.5_py3.10_env/bin/python "$IDF_PATH/tools/idf.py" -p /dev/ttyUSB0 flash monitor
```

The built application is `build/raven_hello.bin`.

## Studio test application

The supplied application is an integration test rather than a static hello
screen. After reset, it should show the Raven Studio interface and support:

1. Touching a graph node to select it; its title is shown in the selection
   status area.
2. Dragging nodes around the graph canvas.
3. Using the colour picker to change a selected node's tint.
4. Pressing **Add** to create a new `Remap` node and **Frame** to fit the graph.
5. Pressing **Play/Pause** to exercise the timeline, and selecting each theme
   in the theme control.

Successful interaction verifies PSRAM framebuffer rendering, GT911 I2C touch,
FreeRTOS task separation, XML asset embedding, Raven widget events, dirty
region rendering, and vertical-blank synchronization.

## Documentation

Doxygen documents all project-authored source under `src` and `port`. The
vendored Raven tree and the vendored ESP-IDF component sources are excluded so
the output remains focused on the application and port boundary.

```bash
doxygen Doxyfile
```

Open `build/doxygen/html/index.html` after generation.

## Current scope

This is a functional MCU port, not a GPU implementation. It supports Raven's
retained widgets, layouts, text, textures, clipping, hit testing, and the
Studio node graph. Desktop-specific features—native windows, filesystem
browsing/watching, desktop font discovery, dialogs, URL launch, process
execution, and Vulkan—are stubbed deliberately. `port/raven_esp_fs.c` is the
place to add LittleFS/FATFS support later.
