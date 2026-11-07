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
| `raven_display.c` | RGB panel timings/pins, backlight, GT911 I2C/GPIO38 interrupt, vblank wait | Replace with the target display/touch driver and native framebuffer access. |
| `raven_sd.c` | Onboard SPI microSD mount, FAT scan, and PSRAM file reads | Change the SPI pins/host or replace it with the target removable-media driver. |
| `raven_wifi.c` | ESP-IDF Wi-Fi station, LAN HTTP server, and synchronized SD-card operations | Replace with the target network stack or keep the same HTTP API on a Wi-Fi-capable ESP-IDF target. |
| `raven_esp_backend.c` | Raven `RBackendVTable`, RGB565 rendering, texture allocation, dirty regions | Retain for an RGB565 CPU framebuffer, or implement the same Raven backend vtable with the target GPU/DMA engine. |
| `raven_esp_thread.c` | Raven thread/mutex/clock services | Map these functions to the target RTOS or threading system. |
| `raven_esp_fs.c` | Filesystem, paths, URLs, and file chooser fallbacks | Keep stubs for immutable firmware, or connect LittleFS/FATFS as required. |
| `raven_esp_fs_watch.c` | Live resource updates | Leave as a stub unless the target has a filesystem watch mechanism. |
| `raven_esp_font_system.c` | Desktop font discovery | Add a known font bundle or platform font lookup if available. |
| `raven_esp_dialogs.c` | Native message/file dialogs | Replace only when the target provides an equivalent UI mechanism. |
| `components/` | Local ESP-IDF GT911 dependencies | Replace/remove these components if the new board uses a different touch controller. |

## Elecrow board configuration

`raven_display.c` carries the verified Elecrow 7-inch HMI configuration:

- RGB565, 800×480, two PSRAM framebuffers: one scanout buffer and one Raven
  back buffer
- 10 MHz pixel clock and the configured RGB timing porches/polarities
- GPIO 2 backlight enable
- GT911 on I2C0: SDA GPIO 19, SCL GPIO 20, 400 kHz
- MicroSD over SPI2: CS GPIO 10, MOSI GPIO 11, SCK GPIO 12, MISO GPIO 13

The renderer paints into the inactive framebuffer, then hands that complete
buffer to the ESP-IDF RGB driver for a frame-boundary DMA switch. Keep this
front/back-buffer handoff on an equivalent target; a single scanout buffer can
show top-to-bottom tearing during full-screen layout changes.

## Image decoding and full-screen slideshow

The project application uses `Rtexture_create()` to decode BMP files from the
FATFS-backed SD card. Raven keeps image decoding optional, therefore
`src/CMakeLists.txt` explicitly defines `RAVEN_WITH_STB_IMAGE=1`. Keep that
definition when moving the gallery to a new target; without it Raven reports
every image as unsupported.

The application requests 24-bit, uncompressed 800×480 BMP files. Raven
decodes them to RGBA8, while this backend samples the resulting texture and
writes RGB565 pixels into the panel framebuffer. The slideshow uses an
application-task deadline, armed for three seconds only after the preceding
slide has reached the panel; SD decode or full-frame rendering time therefore
cannot make it skip an image. It begins at the selected list entry (or index 0
when none is selected), temporarily collapses the gallery widget and shows
only an `Image` widget stretched to the panel bounds, then restores the gallery
after the final file. No display timing or touch path changes are made during
playback. The backend explicitly enters slideshow mode, detects the
full-screen image, and paints it directly into the inactive framebuffer rather
than clearing the visible scanout buffer first; this prevents a black flash
between slideshow images without changing Raven's architecture. Opaque images
larger than 256×256 are stored by the backend as RGB565 (768 KB at 800×480)
rather than Raven's source RGBA8 representation (1.536 MB). The gallery also
decodes the next file before releasing the prior slide, then frees that slide
before creating the replacement texture. This keeps all slideshow allocations
inside the 8 MB PSRAM budget.

## Wi-Fi SD manager

`raven_wifi.c` owns the ESP-IDF station interface and starts a port-80 HTTP
server only after DHCP succeeds. The application supplies the selected scanned
SSID and password from its touchscreen page. ESP-IDF stores that station
profile in Wi-Fi NVS (`WIFI_STORAGE_FLASH`), and the port reconnects it during
boot. The service is
intentionally LAN-only and unauthenticated;
add authentication and TLS before exposing an equivalent port outside a trusted
private network.

The HTTP worker and Raven task share FATFS through the recursive mutex exposed
by `raven_sd.c`. Uploads and copies use temporary files, then rename them only
after completion. The embedded browser page performs its optional image
conversion on the host computer with Canvas API, then uploads only the final
uncompressed 24-bit 800×480 BMP. A successful browser-side mutation raises a
small flag; the application consumes it on its rendering task and rescans the
gallery, so no Raven widget is touched from the HTTP worker context.

## Porting checklist

1. Configure the display resolution and pixel format in `raven_display.h`.
2. Implement display initialization, framebuffer ownership, and a safe frame
   boundary in `raven_display.c`.
3. Make `raven_esp_backend_set_panel()` and `raven_esp_backend()` target that
   framebuffer or replace the software backend vtable entirely.
4. Translate the target touch/pointer device into the normalized coordinates
   returned by `raven_touch_poll()`.
5. Adapt the thread, time, filesystem, font, dialog, removable-media, and network hooks as needed.
6. Add the target's components to `port/components` and update the
   `REQUIRES` list in `src/CMakeLists.txt`.
7. Enable the decoder needed by the application (for this gallery,
   `RAVEN_WITH_STB_IMAGE=1`) in the target component's compile definitions.
8. Build, flash, and perform the smoke test in the root README, including a
   complete slideshow cycle and a Wi-Fi upload on a trusted local network.

## Rules of the boundary

- Do not put board pin mappings or ESP-IDF APIs in `src`.
- Do not modify Raven to add board behaviour; use its platform/backend hooks.
- Keep dependencies required to build the port inside this project so a fresh
  clone does not depend on a neighboring example checkout.
