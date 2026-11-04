# ESP32-S3 porting guide

## Purpose

Everything in this directory is the platform boundary between Raven and an
ESP-IDF board. Application code belongs in `../src`; Raven itself belongs in
`../raven`. To support a different display or MCU, work in this directory and
update `src/CMakeLists.txt` only if the replacement introduces different
source files or ESP-IDF components.

## Files to adapt

| File | Responsibility | Change for another platform |
| --- | --- | --- |
| `raven_display.c` | RGB panel timings/pins, backlight, GT911 I2C, vblank wait | Replace with the target display/touch driver and native framebuffer access. |
| `raven_sd.c` | Onboard SPI microSD mount, FAT scan, and PSRAM file reads | Change the SPI pins/host or replace it with the target removable-media driver. |
| `raven_esp_backend.c` | Raven `RBackendVTable`, RGB565 rendering, texture allocation, dirty regions | Retain for an RGB565 CPU framebuffer, or implement the same Raven backend vtable with the target GPU/DMA engine. |
| `raven_esp_thread.c` | Raven thread/mutex/clock services | Map these functions to the target RTOS or threading system. |
| `raven_esp_fs.c` | Filesystem, paths, URLs, and file chooser fallbacks | Keep stubs for immutable firmware, or connect LittleFS/FATFS as required. |
| `raven_esp_fs_watch.c` | Live resource updates | Leave as a stub unless the target has a filesystem watch mechanism. |
| `raven_esp_font_system.c` | Desktop font discovery | Add a known font bundle or platform font lookup if available. |
| `raven_esp_dialogs.c` | Native message/file dialogs | Replace only when the target provides an equivalent UI mechanism. |
| `components/` | Local ESP-IDF GT911 dependencies | Replace/remove these components if the new board uses a different touch controller. |

## Elecrow board configuration

`raven_display.c` carries the verified Elecrow 7-inch HMI configuration:

- RGB565, 800×480, one PSRAM scanout buffer
- 10 MHz pixel clock and the configured RGB timing porches/polarities
- GPIO 2 backlight enable
- GT911 on I2C0: SDA GPIO 19, SCL GPIO 20, 400 kHz
- MicroSD over SPI2: CS GPIO 10, MOSI GPIO 11, SCK GPIO 12, MISO GPIO 13

Keep the one-buffer model unless the replacement panel driver guarantees safe
multi-buffer synchronization. The renderer waits for vertical blank and
updates dirty geometry to reduce visible tearing/flicker.

## Porting checklist

1. Configure the display resolution and pixel format in `raven_display.h`.
2. Implement display initialization, framebuffer ownership, and a safe frame
   boundary in `raven_display.c`.
3. Make `raven_esp_backend_set_panel()` and `raven_esp_backend()` target that
   framebuffer or replace the software backend vtable entirely.
4. Translate the target touch/pointer device into the normalized coordinates
   returned by `raven_touch_poll()`.
5. Adapt the thread, time, filesystem, font, dialog, and removable-media hooks as needed.
6. Add the target's components to `port/components` and update the
   `REQUIRES` list in `src/CMakeLists.txt`.
7. Build, flash, and perform the smoke test in the root README.

## Rules of the boundary

- Do not put board pin mappings or ESP-IDF APIs in `src`.
- Do not modify Raven to add board behaviour; use its platform/backend hooks.
- Keep dependencies required to build the port inside this project so a fresh
  clone does not depend on a neighboring example checkout.
