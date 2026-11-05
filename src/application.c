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
#include "raven_wifi.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <Raven.h>
#include <stdio.h>
#include <stdint.h>
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
    Rid wifi;
    Rid wifi_status;
    Rid wifi_ip;
    Rid wifi_ssid;
    Rid wifi_password;
    Rid wifi_editing;
    Rid wifi_shift;
    Rid wifi_networks;
    raven_sd_file_t entries[RAVEN_SD_MAX_FILES];
    raven_wifi_network_t wifi_network_list[RAVEN_WIFI_MAX_NETWORKS];
    raven_wifi_status_t last_wifi_status;
    char wifi_ssid_text[33];
    char wifi_password_text[64];
    int entry_count;
    int slideshow_index;
    int slideshow_last_index;
    int slideshow_active;
    int fullscreen_active;
    int slideshow_arm_after_draw;
    int fullscreen_touch_pending_release;
    int gallery_refresh_pending;
    int wifi_network_count;
    int wifi_uppercase;
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

/** @brief Redraw the Wi-Fi credential fields while keeping the password masked. */
static void wifi_refresh_fields(void)
{
    char password_mask[sizeof(app.wifi_password_text)];
    size_t length = strlen(app.wifi_password_text);

    Rlabel_set_text(app.wifi_ssid, app.wifi_ssid_text[0] ? app.wifi_ssid_text : "Select a Wi-Fi network above");
    memset(password_mask, 0, sizeof(password_mask));
    memset(password_mask, '*', length);
    Rlabel_set_text(app.wifi_password, length ? password_mask : "Tap Password to enter it");
    Rlabel_set_text(app.wifi_editing, "Enter password for the selected network");
    Rbutton_set_text(app.wifi_shift, app.wifi_uppercase ? "ABC" : "abc");
}

/** @brief Copy asynchronous Wi-Fi state into widgets owned by the Raven task. */
static void wifi_sync_status(void)
{
    raven_wifi_status_t status;
    char ip_line[40];

    raven_wifi_get_status(&status);
    if (memcmp(&status, &app.last_wifi_status, sizeof(status)) == 0) return;
    app.last_wifi_status = status;
    Rlabel_set_text(app.wifi_status, status.message);
    snprintf(ip_line, sizeof(ip_line), "IP address: %s", status.ip[0] ? status.ip : "not connected");
    Rlabel_set_text(app.wifi_ip, ip_line);
}

/** @brief Choose which touchscreen credential field receives virtual keys. */
/** @brief Keep the password field visibly active for virtual-keyboard entry. */
static void wifi_select_password(Rid button, void* user_data)
{
    (void)button;
    (void)user_data;
    wifi_refresh_fields();
}

/** @brief Append one printable ASCII character to the selected Wi-Fi field. */
static void wifi_key(Rid button, void* user_data)
{
    char* text = app.wifi_password_text;
    size_t capacity = sizeof(app.wifi_password_text);
    size_t length = strlen(text);
    unsigned char character = (unsigned char)(uintptr_t)user_data;
    (void)button;

    if (app.wifi_uppercase && character >= 'a' && character <= 'z') character -= 'a' - 'A';
    if (length + 1U < capacity) {
        text[length] = (char)character;
        text[length + 1U] = '\0';
    }
    wifi_refresh_fields();
}

/** @brief Delete the final virtual-keyboard character from the active field. */
static void wifi_backspace(Rid button, void* user_data)
{
    char* text = app.wifi_password_text;
    size_t length = strlen(text);
    (void)button;
    (void)user_data;
    if (length) text[length - 1U] = '\0';
    wifi_refresh_fields();
}

/** @brief Copy an asynchronous scan result into the Raven list widget. */
static void wifi_sync_networks(void)
{
    raven_wifi_network_t networks[RAVEN_WIFI_MAX_NETWORKS];
    int count = raven_wifi_get_networks(networks, RAVEN_WIFI_MAX_NETWORKS);
    int index;

    if (count == app.wifi_network_count &&
        memcmp(networks, app.wifi_network_list, (size_t)count * sizeof(networks[0])) == 0) return;
    memcpy(app.wifi_network_list, networks, (size_t)count * sizeof(networks[0]));
    app.wifi_network_count = count;
    Rlist_view_clear_items(app.wifi_networks);
    for (index = 0; index < count; ++index) {
        char item[48];
        snprintf(item, sizeof(item), "%.32s  (%d dBm)", networks[index].ssid, networks[index].rssi);
        Rlist_view_add_item(app.wifi_networks, item);
    }
}

/** @brief Select a scanned SSID and leave only its password to be entered. */
static void wifi_network_selected(Rid list, int index, void* user_data)
{
    (void)list;
    (void)user_data;
    if (index < 0 || index >= app.wifi_network_count) return;
    snprintf(app.wifi_ssid_text, sizeof(app.wifi_ssid_text), "%s", app.wifi_network_list[index].ssid);
    app.wifi_password_text[0] = '\0';
    wifi_refresh_fields();
}

/** @brief Start or repeat a nearby-network scan from the Wi-Fi setup page. */
static void wifi_rescan(Rid button, void* user_data)
{
    (void)button;
    (void)user_data;
    (void)raven_wifi_start_scan();
}

/** @brief Toggle upper-case text for the virtual Wi-Fi keyboard. */
static void wifi_toggle_shift(Rid button, void* user_data)
{
    (void)button;
    (void)user_data;
    app.wifi_uppercase = !app.wifi_uppercase;
    wifi_refresh_fields();
}

/** @brief Begin an asynchronous station connection using the entered credentials. */
static void wifi_connect(Rid button, void* user_data)
{
    (void)button;
    (void)user_data;
    if (!app.wifi_ssid_text[0]) {
        Rlabel_set_text(app.wifi_status, "Enter a Wi-Fi name first.");
        return;
    }
    if (!raven_wifi_connect(app.wifi_ssid_text, app.wifi_password_text)) {
        Rlabel_set_text(app.wifi_status, "Could not start connection. Check the Wi-Fi setup.");
    }
}

/** @brief Return from Wi-Fi setup to the gallery without disconnecting. */
static void wifi_back_to_gallery(Rid button, void* user_data)
{
    (void)button;
    (void)user_data;
    Rwidget_set_visibility(app.wifi, R_COLLAPSED);
    Rwidget_set_visibility(app.gallery, R_VISIBLE);
}

/** @brief Open local Wi-Fi setup and its browser-access instructions. */
static void wifi_open(Rid button, void* user_data)
{
    (void)button;
    (void)user_data;
    Rwidget_set_visibility(app.gallery, R_COLLAPSED);
    Rwidget_set_visibility(app.wifi, R_VISIBLE);
    wifi_refresh_fields();
    wifi_sync_status();
    (void)raven_wifi_start_scan();
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

/** @brief Leave full-screen image display and return to the gallery preview. */
static void gallery_stop_fullscreen(const char* message)
{
    app.slideshow_active = 0;
    app.fullscreen_active = 0;
    app.slideshow_arm_after_draw = 0;
    app.slideshow_deadline_us = 0;
    raven_esp_backend_set_slideshow_mode(0);
    Rimage_set_texture(app.slideshow, R_INVALID_ID);
    Rwidget_set_visibility(app.slideshow, R_COLLAPSED);
    Rwidget_set_visibility(app.gallery, R_VISIBLE);
    if (message) gallery_status(message);
}

/** @brief Leave full-screen slideshow playback and return to the image library. */
static void gallery_stop_slideshow(const char* message)
{
    gallery_stop_fullscreen(message);
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
    app.fullscreen_active = 1;
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

/** @brief Open the selected image full-screen until the next panel touch. */
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
    if (!gallery_show_image(selected)) return;
    Rimage_set_texture(app.slideshow, app.texture);
    Rimage_set_stretch(app.slideshow, R_STRETCH_FIT);
    Rwidget_set_visibility(app.gallery, R_COLLAPSED);
    Rwidget_set_visibility(app.slideshow, R_VISIBLE);
    app.fullscreen_active = 1;
    app.slideshow_active = 0;
    app.slideshow_arm_after_draw = 0;
    app.slideshow_deadline_us = 0;
    raven_esp_backend_set_slideshow_mode(1);
    ESP_LOGI(TAG, "full-screen image opened: %s", app.entries[selected].name);
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
    static const struct {
        const char* name;
        unsigned char character;
    } keys[] = {
        {"key_1", '1'}, {"key_2", '2'}, {"key_3", '3'}, {"key_4", '4'}, {"key_5", '5'},
        {"key_6", '6'}, {"key_7", '7'}, {"key_8", '8'}, {"key_9", '9'}, {"key_0", '0'},
        {"key_q", 'q'}, {"key_w", 'w'}, {"key_e", 'e'}, {"key_r", 'r'}, {"key_t", 't'},
        {"key_y", 'y'}, {"key_u", 'u'}, {"key_i", 'i'}, {"key_o", 'o'}, {"key_p", 'p'},
        {"key_a", 'a'}, {"key_s", 's'}, {"key_d", 'd'}, {"key_f", 'f'}, {"key_g", 'g'},
        {"key_h", 'h'}, {"key_j", 'j'}, {"key_k", 'k'}, {"key_l", 'l'}, {"key_z", 'z'},
        {"key_x", 'x'}, {"key_c", 'c'}, {"key_v", 'v'}, {"key_b", 'b'}, {"key_n", 'n'},
        {"key_m", 'm'}, {"key_dot", '.'}, {"key_dash", '-'}, {"key_under", '_'}, {"key_at", '@'},
        {"key_slash", '/'}, {"key_bang", '!'}, {"key_hash", '#'}, {"key_plus", '+'},
        {"key_equal", '='}, {"key_percent", '%'}, {"key_dollar", '$'}, {"key_amp", '&'},
        {"key_colon", ':'}, {"key_space", ' '},
    };
    size_t index;

    app.files = Rwidget_find(app.window, "files");
    app.gallery = Rwidget_find(app.window, "gallery");
    app.preview = Rwidget_find(app.window, "preview");
    app.slideshow = Rwidget_find(app.window, "slideshow_view");
    app.status = Rwidget_find(app.window, "status");
    app.selected = Rwidget_find(app.window, "selected");
    app.count = Rwidget_find(app.window, "count");
    app.wifi = Rwidget_find(app.window, "wifi");
    app.wifi_status = Rwidget_find(app.window, "wifi_status");
    app.wifi_ip = Rwidget_find(app.window, "wifi_ip");
    app.wifi_ssid = Rwidget_find(app.window, "wifi_ssid");
    app.wifi_password = Rwidget_find(app.window, "wifi_password");
    app.wifi_editing = Rwidget_find(app.window, "wifi_editing");
    app.wifi_shift = Rwidget_find(app.window, "key_shift");
    app.wifi_networks = Rwidget_find(app.window, "wifi_networks");
    Rlist_view_set_selection_changed_callback(app.files, gallery_selected, NULL);
    Rlist_view_set_selection_changed_callback(app.wifi_networks, wifi_network_selected, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "open"), gallery_open, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "slideshow"), gallery_start_slideshow, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "rescan"), gallery_rescan, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "wifi_open"), wifi_open, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "wifi_back"), wifi_back_to_gallery, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "wifi_edit_password"), wifi_select_password, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "wifi_rescan"), wifi_rescan, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "wifi_connect"), wifi_connect, NULL);
    Rbutton_set_clicked_callback(Rwidget_find(app.window, "key_backspace"), wifi_backspace, NULL);
    Rbutton_set_clicked_callback(app.wifi_shift, wifi_toggle_shift, NULL);
    for (index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        Rbutton_set_clicked_callback(Rwidget_find(app.window, keys[index].name), wifi_key,
                                     (void*)(uintptr_t)keys[index].character);
    }
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
    if (!raven_wifi_init()) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed");
        Rlabel_set_text(app.wifi_status, "Wi-Fi could not initialize.");
    }
    wifi_refresh_fields();
    wifi_sync_status();
    gallery_scan(0);
    Rwindow_show(app.window);

    ESP_LOGI(TAG, "Raven SD gallery running on CPU1");
    while (Rdraw_frame()) {
        raven_touch_event_t event;
        wifi_sync_status();
        wifi_sync_networks();
        if (raven_wifi_take_media_changed()) app.gallery_refresh_pending = 1;
        if (app.gallery_refresh_pending && !app.fullscreen_active) {
            app.gallery_refresh_pending = 0;
            gallery_scan(0);
            gallery_status("SD card updated over Wi-Fi.");
        }
        gallery_mark_slideshow_presented();
        if (app.slideshow_active && !app.slideshow_arm_after_draw &&
            esp_timer_get_time() >= app.slideshow_deadline_us) {
            gallery_advance_slideshow();
        }
        while (xQueueReceive(touch_events, &event, 0) == pdPASS) {
            if (app.fullscreen_touch_pending_release) {
                if (!event.pressed) {
                    app.fullscreen_touch_pending_release = 0;
                    previous_pressed = 0;
                }
                continue;
            }
            if (app.fullscreen_active && event.pressed) {
                gallery_stop_fullscreen(app.slideshow_active ?
                                      "Slideshow stopped. Back in the image library." :
                                      "Back in the image library.");
                /* Do not pass the dismissing touch through to a gallery button. */
                app.fullscreen_touch_pending_release = 1;
                previous_pressed = 1;
                continue;
            }
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
