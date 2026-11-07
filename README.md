# Raven SD Gallery on Elecrow 7-inch ESP32-S3 HMI

This repository is a self-contained ESP-IDF port of the
[Raven graphics library](https://github.com/oscar-soirey/Raven) for Elecrow's
800×480 ESP32-S3 RGB HMI. It provides a polished, touch-friendly microSD BMP
gallery: select an image from the card's file list, preview it directly on the
panel, play the library as a full-screen slideshow, or manage the SD card over
the HMI's Wi-Fi connection from a browser.

The port uses an RGB565 software renderer with front/back PSRAM framebuffers.
GT911 touch is normalized into Raven mouse events through GPIO38. Raven
renders on CPU1 while the interrupt-driven touch task runs on CPU0.

## Repository layout

```
src/                    Application-only code
  main.c                ESP-IDF entry point and task setup
  application.c         SD-card gallery application
  gallery.xml           Gallery layout embedded into firmware
  include/              Application public headers
raven/                  Full vendored Raven source tree
port/                   All board/OS/rendering adaptation code
  include/              Port public headers
  components/           Vendored ESP-IDF touch components
  raven_sd.c            SPI microSD and FAT/FAT32 implementation
  raven_wifi.c          Wi-Fi station and LAN HTTP SD-card manager
  Readme_port.md        Board-porting guide
Doxyfile                Documentation configuration
```

`src` must not contain board pins, panel APIs, or platform-specific logic.
Those belong in `port`, allowing a new platform to retain the application and
Raven while replacing only the port layer. See
[the porting guide](port/Readme_port.md) for the exact boundary and checklist.

## Requirements

- Elecrow 7-inch ESP32-S3 HMI (800×480 RGB panel, GT911 touch, microSD slot)
- 4 MB flash and 8 MB PSRAM (the verified hardware configuration)
- ESP-IDF 5.5.x; this checkout was verified with ESP-ADF's bundled ESP-IDF
  5.5.2
- Python environment installed by ESP-IDF
- USB serial access to the board
- A 2.4 GHz Wi-Fi network shared by the HMI and the computer used for uploads

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

The project uses the HMI's full 4 MB flash configuration. Its factory-app
partition is 0x3F0000 bytes (just under 4 MB); PSRAM is configured at the
stable 80 MHz rate and the 8 MB device is added to the ESP-IDF heap at boot.
The `stable_v1.0` tag marks the original known-good memory configuration.
`stable_v4.0` marks the current validated gallery release, including the
continuous slideshow, browser-side converter, and LAN-only mDNS address.

## Browser image converter

The built-in Wi-Fi SD manager includes the image converter; no Python tool or
desktop dependency is required. Select any image supported by the host browser,
choose the resize and orientation settings, inspect the live 800×480 preview,
then choose **Convert & Upload**. Conversion happens on the host computer and
only the final uncompressed 24-bit BMP is sent to the HMI.

**Fill display** preserves aspect ratio and centre-crops. **Fit entire image**
preserves all content and adds the gallery's dark letterbox background. The
orientation selector first respects the source's automatic EXIF orientation,
then can apply 90° clockwise, 180°, or 90° counter-clockwise rotation.

On current Chrome and Edge browsers, the tested input formats are JPEG/JPG,
PNG, WebP, GIF (its first frame), and AVIF. SVG may work where the host browser
can rasterize it; TIFF and HEIC should be converted before use. Regardless of
the source, the uploaded result is always an uncompressed 24-bit 800×480 BMP.

## SD gallery application

After reset, the application shows an image library on the left and a large
preview area on the right. It uses the onboard SPI microSD interface:

- CS GPIO10, MOSI GPIO11, SCK GPIO12, MISO GPIO13
- FAT/FAT32-formatted microSD card
- Root-level, uncompressed 24-bit BMP images at exactly 800×480

Copy images to the card root, insert it before boot or use **Rescan**, then:

1. Select a `.bmp` file in **Image Library**; it loads into the preview.
2. Use **Open selected** to show the currently highlighted file full-screen;
   touch anywhere on the display to return to the library.
3. Tap **Play slideshow** to display the selected image (or the first image
   when none is selected) across the complete panel. Each image remains on
   screen for three seconds. After the final image, playback loops from the
   first image continuously. Touching anywhere during a slideshow stops it
   immediately and returns to the gallery.
4. Use **Rescan** after inserting or replacing a card; it remounts the card
   and rebuilds the list.

The status bar explains missing-card, unreadable-file, unsupported-image, and
wrong-resolution errors. A displayed image verifies PSRAM framebuffer
rendering, GT911 touch, FATFS, SPI microSD, Raven texture decoding, dirty
region rendering, and vertical-blank synchronization.

### Supported image format

Raven's `stb_image` decoder is enabled in the ESP-IDF build. For predictable
memory use and full-screen playback, use only uncompressed Windows BMP files
with these properties:

- 24-bit BGR pixels
- Exactly 800 × 480 pixels
- Stored in the card's root directory

Other image formats may be decoded by Raven but are outside this application's
tested and supported workflow.

For the 8 MB PSRAM target, full-screen opaque gallery images are compacted by
the ESP32-S3 backend from Raven's RGBA8 source into RGB565 texture storage.
This uses 768 KB per displayed 800×480 image and allows every slide to load
reliably without exhausting PSRAM.

## Wi-Fi SD-card manager

The **Wi-Fi** button opens a touch-only setup screen and automatically scans
nearby visible 2.4 GHz networks. Select an SSID from that list, enter only its
password with the on-screen keyboard, then tap **Connect and start SD
manager**. Tap **Scan** to refresh the list. The password is masked on screen.
Credentials are retained in ESP-IDF Wi-Fi NVS and reconnect automatically
after reset.

Once DHCP completes, use this LAN-only address on a computer connected to the
same Wi-Fi network:

```text
http://vedantsravengallary.local
```

The HMI advertises this address with mDNS, so it follows DHCP IP-address
changes automatically. It is not a public internet domain and works only on
the local network. If a Linux computer cannot resolve `.local` names, install
and enable its standard mDNS resolver (Avahi).

The compact built-in page lists regular files in the SD-card root and supports:

- Uploading ready-to-display BMP files from the computer (up to 4 MB per
  transfer), with a live progress bar and a clear completed or failed transfer
  message
- Converting browser-supported image files on the computer itself, then
  viewing an exact 800×480 preview, selecting Fill/Fit resize behavior and
  rotation, then uploading only the final uncompressed 24-bit BMP. The browser
  uses host-PC resources, so ESP32 PSRAM is not used for conversion and the
  source image is never copied to the SD card.
- Downloading, copying, renaming/moving, and deleting root-level files
- Automatic refresh of the HMI image list after a browser-side change

Gallery images remain subject to the 24-bit, 800×480 BMP requirements above.
For safety, browser-created names are limited to ordinary root-level FAT names
(letters, numbers, spaces, `.`, `_`, `-`, parentheses, brackets, apostrophes,
`!`, and `~`), up to 79 characters. The HTTP manager
has no login or TLS to keep the firmware compact, so use it only on a trusted
private LAN and never expose the HMI to the public internet.

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
retained widgets, layouts, text, textures, clipping, hit testing, list views,
timers, and BMP texture decoding. Desktop-specific features—native windows, filesystem
browsing/watching, desktop font discovery, dialogs, URL launch, process
execution, and Vulkan—are stubbed deliberately. The gallery's removable-media
support is isolated in `port/raven_sd.c`; `port/raven_esp_fs.c` remains a safe
stub for Raven's desktop-style filesystem APIs.
