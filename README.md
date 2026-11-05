# Raven SD Gallery on Elecrow 7-inch ESP32-S3 HMI

This repository is a self-contained ESP-IDF port of the
[Raven graphics library](https://github.com/oscar-soirey/Raven) for Elecrow's
800×480 ESP32-S3 RGB HMI. It provides a polished, touch-friendly microSD BMP
gallery: select an image from the card's file list, preview it directly on the
panel, or play the library as a full-screen slideshow.

The port uses an RGB565 software renderer that writes into the RGB panel's
PSRAM framebuffer. GT911 touch is normalized into Raven mouse events. Raven
renders on CPU1; the low-priority touch task runs on CPU0.

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
partition is 0x3F0000 bytes (just under 4 MB); PSRAM is configured at 80 MHz
and the 8 MB device is added to the ESP-IDF heap at boot.

## Desktop image converter

`image_converter.py` is a small Python desktop screen for preparing gallery
images before copying them to the microSD card. It accepts common image formats
supported by Pillow (JPEG, PNG, WebP, TIFF, GIF, BMP, and more), applies EXIF
orientation, flattens transparency, resizes to the HMI's exact 800×480 panel,
and writes an uncompressed 24-bit BMP.

Install the one dependency and open the converter:

```bash
python3 -m pip install -r requirements.txt
python3 image_converter.py
```

Choose a source image and destination path in the screen. **Fill display**
preserves aspect ratio and center-crops; **Fit entire image** preserves all
content and places dark bars where needed. The **Orientation** selector
automatically honors the source EXIF orientation and can also rotate it 90°
clockwise, 180°, or 90° counter-clockwise. The live preview shows the exact
800×480 result.

The same tool also works from a terminal:

```bash
python3 image_converter.py --input /path/photo.jpg --output /path/gallery_photo.bmp --mode crop --rotate 90
```

## SD gallery application

After reset, the application shows an image library on the left and a large
preview area on the right. It uses the onboard SPI microSD interface:

- CS GPIO10, MOSI GPIO11, SCK GPIO12, MISO GPIO13
- FAT/FAT32-formatted microSD card
- Root-level, uncompressed 24-bit BMP images at exactly 800×480

Copy images to the card root, insert it before boot or use **Rescan**, then:

1. Select a `.bmp` file in **Image Library**; it loads into the preview.
2. Use **Open selected** to load the currently highlighted file again.
3. Tap **Play slideshow** to display the selected image (or the first image
   when none is selected) across the complete panel. Each image remains on
   screen for three seconds. After the final image, the application returns to
   the gallery and leaves that last image in the preview.
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
