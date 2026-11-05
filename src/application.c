/**
 * @file application.c
 * @brief Raven SD-card BMP gallery application for the Elecrow HMI.
 *
 * The application keeps UI state in Raven while the port layer owns card
 * access. Selecting an entry loads its BMP into a Raven texture and displays
 * it in the gallery preview.
 */
#include "application.h"
#include "raven_display.h"
#include "raven_esp_backend.h"
#include "raven_sd.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <Raven.h>
#include <stdio.h>
#include <string.h>

extern const char gallery_xml_start[] asm("_binary_gallery_xml_start");

static const char* TAG = "raven_gallery";

static struct {
    Rid window;
    Rid gallery;
    Rid files;
    Rid preview;
    Rid slideshow;
    Rid status;
    Rid selected;
    Rid count;
    Rid texture;
    raven_sd_file_t entries[RAVEN_SD_MAX_FILES];
    int entry_count;
    int slideshow_index;
    int slideshow_last_index;
    int slideshow_active;
    int slideshow_arm_after_draw;
    int64_t slideshow_deadline_us;
} app;

/** @brief Log Raven failures to the ESP-IDF monitor. */
static void raven_error(RError code, RSeverity severity, const char* detail)
{
    ESP_LOGE(TAG, "Raven %s (%s): %s", Rerror_to_string(code), Rseverity_to_string(severity), detail);
}

/** @brief Update the small status line below the preview. */
static void gallery_status(const char* text)
{
    Rlabel_set_text(app.status, text ? text : "");
}

/** @brief Release the selected texture and return the preview to its empty state. */
static void gallery_clear_preview(void)
{
    if (app.texture != R_INVALID_ID) {
        Rimage_set_texture(app.preview, R_INVALID_ID);
        Rtexture_destroy(app.texture);
        app.texture = R_INVALID_ID;
    }
    Rlabel_set_text(app.selected, "Choose a BMP from the library");
}

/** @brief Load one root-level SD-card BMP into the Raven image widget. */
static int gallery_show_image(int index)
{
    uint8_t* file_data;
    uint8_t* decoded_pixels;
    size_t file_size;
    Rid texture;
    int width = 0;
    int height = 0;
    char detail[128];

    if (index < 0 || index >= app.entry_count) return 0;
    gallery_status("Loading image from SD card...");
    file_data = raven_sd_read_file(app.entries[index].path, &file_size);
    if (!file_data) {
        gallery_status("Could not read this file. Use a FAT/FAT32 card and a 24-bit BMP.");
        return 0;
    }
    /* Decode before releasing the displayed texture.  The old slide remains
     * visible while the SD read and decode run, then its PSRAM is available
     * before Raven creates the replacement texture. */
    decoded_pixels = Rimage_decode(file_data, file_size, &width, &height);
    raven_sd_free(file_data);
    if (!decoded_pixels) {
        gallery_status("Unsupported BMP. Use an uncompressed 24-bit 800 x 480 BMP.");
        return 0;
    }
    if (width != RAVEN_DISPLAY_WIDTH || height != RAVEN_DISPLAY_HEIGHT) {
        snprintf(detail, sizeof(detail), "%s is %d x %d. Expected 800 x 480.",
                 app.entries[index].name, width, height);
        Rpixels_free(decoded_pixels);
        gallery_status(detail);
        return 0;
    }
    gallery_clear_preview();
    texture = Rtexture_create_from_pixels(decoded_pixels, width, height);
    Rpixels_free(decoded_pixels);
    if (texture == R_INVALID_ID) {
        gallery_status("Could not allocate this BMP in PSRAM.");
        return 0;
    }
    app.texture = texture;
    Rtexture_set_filter(texture, R_FILTER_LINEAR);
    Rimage_set_texture(app.preview, texture);
    Rimage_set_stretch(app.preview, R_STRETCH_FIT);
    Rlabel_set_text(app.selected, app.entries[index].name);
    snprintf(detail, sizeof(detail), "800 x 480  •  %lu KB  •  24-bit BMP",
             (unsigned long)(app.entries[index].size_bytes / 1024U));
    gallery_status(detail);
    return 1;
}

/** @brief Leave full-screen playback and return to the gallery preview. */
static void gallery_stop_slideshow(const char* message)
{
    app.slideshow_active = 0;
    app.slideshow_arm_after_draw = 0;
    app.slideshow_deadline_us = 0;
    raven_esp_backend_set_slideshow_mode(0);
    Rimage_set_texture(app.slideshow, R_INVALID_ID);
    Rwidget_set_visibility(app.slideshow, R_COLLAPSED);
    Rwidget_set_visibility(app.gallery, R_VISIBLE);
    if (message) gallery_status(message);
}

/** @brief Advance the player after the current image's three-second hold. */
static void gallery_advance_slideshow(void)
{
    if (!app.slideshow_active || app.slideshow_index >= app.slideshow_last_index) {
        gallery_stop_slideshow("Slideshow complete. Back in the image library.");
        return;
    }
    ++app.slideshow_index;
    if (!gallery_show_image(app.slideshow_index)) {
        gallery_stop_slideshow("Slideshow stopped because an image could not be loaded.");
        return;
    }
    Rimage_set_texture(app.slideshow, app.texture);
    /* The application loop arms the next one-shot timer only after this
     * replacement texture has completed a full panel render.  A repeating
     * timer starts counting during SD decode/render work and can therefore
     * visibly skip slides on this MCU. */
    app.slideshow_arm_after_draw = 1;
    ESP_LOGI(TAG, "slideshow presenting %d of %d", app.slideshow_index + 1,
             app.slideshow_last_index + 1);
}

/** @brief Start a three-second-per-image, panel-filling gallery slideshow. */
static void gallery_start_slideshow(Rid button, void* user_data)
{
    int selected;
    (void)button;
    (void)user_data;
    if (app.entry_count == 0) {
        gallery_status("No BMP images are available for a slideshow.");
        return;
    }
    selected = Rlist_view_get_selected(app.files);
    app.slideshow_index = selected >= 0 ? selected : 0;
    app.slideshow_last_index = app.entry_count - 1;
    if (!gallery_show_image(app.slideshow_index)) return;

    Rimage_set_texture(app.slideshow, app.texture);
    Rimage_set_stretch(app.slideshow, R_STRETCH_FIT);
    Rwidget_set_visibility(app.gallery, R_COLLAPSED);
    Rwidget_set_visibility(app.slideshow, R_VISIBLE);
    app.slideshow_active = 1;
    app.slideshow_arm_after_draw = 1;
    raven_esp_backend_set_slideshow_mode(1);
    ESP_LOGI(TAG, "slideshow started: image %d through %d", app.slideshow_index + 1,
             app.slideshow_last_index + 1);
}

/** @brief Start the three-second hold only after a new slide reaches the panel. */
static void gallery_mark_slideshow_presented(void)
{
    if (!app.slideshow_arm_after_draw) return;
    app.slideshow_arm_after_draw = 0;
    if (!app.slideshow_active) return;
    app.slideshow_deadline_us = esp_timer_get_time() + 3000000LL;
}

/** @brief Populate the virtual Raven list with BMP files from the card root. */
static void gallery_scan(int remount)
{
    char detail[96];
    int index;

    if (remount) raven_sd_unmount();
    gallery_clear_preview();
    Rlist_view_clear_items(app.files);
    app.entry_count = 0;
    if (!raven_sd_mount()) {
        Rlabel_set_text(app.count, "CARD OFFLINE");
        gallery_status("Insert a FAT/FAT32 microSD card, then tap Rescan.");
        return;
    }

    app.entry_count = raven_sd_list_bmps(app.entries, RAVEN_SD_MAX_FILES);
    for (index = 0; index < app.entry_count; ++index) {
        Rlist_view_add_item(app.files, app.entries[index].name);
    }
    snprintf(detail, sizeof(detail), "%d BMP %s", app.entry_count,
             app.entry_count == 1 ? "IMAGE" : "IMAGES");
    Rlabel_set_text(app.count, detail);
    if (app.entry_count == 0) {
        gallery_status("No BMP files found in the card root. Add 24-bit 800 x 480 BMP images.");
    } else {
        gallery_status("Select an image to preview it. Tap Rescan after changing the card.");
    }
}

/** @brief Preview an image as soon as its list row becomes selected. */
static void gallery_selected(Rid list, int index, void* user_data)
{
    (void)list;
    (void)user_data;
    gallery_show_image(index);
}

/** @brief Explicitly open the selected image from the touch-friendly button. */
static void gallery_open(Rid button, void* user_data)
{
    int selected;
    (void)button;
    (void)user_data;
    selected = Rlist_view_get_selected(app.files);
    if (selected < 0) {
        gallery_status("Select a BMP file in the library first.");
        return;
    }
    gallery_show_image(selected);
}

/** @brief Remount and rescan media so card changes do not require reset. */
static void gallery_rescan(Rid button, void* user_data)
{
    (void)button;
    (void)user_data;
    gallery_scan(1);
}

/** @brief Resolve named widgets and connect all gallery interactions. */
static void gallery_connect_widgets(void)
{
    app.files = Rwidget_find(app.window, "files");
    app.gallery = Rwidget_find(app.window, "gallery");
    app.preview = Rwidget_find(app.window, "preview");
    app.slideshow = Rwidget_find(app.window, "slideshow_view");
    app.status = Rwidget_find(app.window, "status");
    app.selected = Rwidget_find(app.window, "selected");
    app.count = Rwidget_find(app.window, "count");
    Rlist_view_set_selection_changed_callback(app.files, gallery_selected, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "open"), gallery_open, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "slideshow"), gallery_start_slideshow, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "rescan"), gallery_rescan, NULL);
}

void raven_application_task(void* user_data)
{
    RHostPlatformDesc host = {0};
    RInitDesc init = {
        .backend = R_BACKEND_CUSTOM,
        .app_name = "Raven SD Gallery",
        .worker_threads = -1,
        .disable_render_thread = R_TRUE,
    };
    QueueHandle_t touch_events = (QueueHandle_t)user_data;
    int previous_pressed = 0;

    Rhost_platform_register(&host);
    Rbackend_register(raven_esp_backend());
    Rerror_set_callback(raven_error);
    if (!Rinit_ex(&init)) {
        ESP_LOGE(TAG, "Raven initialization failed");
        vTaskDelete(NULL);
        return;
    }

    app.texture = R_INVALID_ID;
    Rtheme_apply(R_THEME_DARK);
    Rset_animations_enabled(R_FALSE);
    app.window = Rwindow_create_from_xml(gallery_xml_start, strlen(gallery_xml_start));
    if (app.window == R_INVALID_ID) {
        ESP_LOGE(TAG, "unable to load embedded gallery XML");
        Rshutdown();
        vTaskDelete(NULL);
        return;
    }
    Rhost_window_set_size(app.window, RAVEN_DISPLAY_WIDTH, RAVEN_DISPLAY_HEIGHT, 1.0f);
    gallery_connect_widgets();
    gallery_scan(0);
    Rwindow_show(app.window);

    ESP_LOGI(TAG, "Raven SD gallery running on CPU1");
    while (Rdraw_frame()) {
        raven_touch_event_t event;
        gallery_mark_slideshow_presented();
        if (app.slideshow_active && !app.slideshow_arm_after_draw &&
            esp_timer_get_time() >= app.slideshow_deadline_us) {
            gallery_advance_slideshow();
        }
        while (xQueueReceive(touch_events, &event, 0) == pdPASS) {
            if (event.pressed) Rhost_inject_mouse_move(app.window, event.x, event.y, 0);
            if (event.pressed != previous_pressed) {
                Rhost_inject_mouse_button(app.window, event.x, event.y,
                                          R_MOUSE_LEFT, event.pressed, 0);
                previous_pressed = event.pressed;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    gallery_clear_preview();
    raven_sd_unmount();
    Rshutdown();
    vTaskDelete(NULL);
}
