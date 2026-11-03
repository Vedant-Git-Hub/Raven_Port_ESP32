#include "raven_display.h"
#include "raven_esp_backend.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <Raven.h>

static const char* TAG = "raven_hello";
static Rid s_window;
static Rid s_status;
static QueueHandle_t s_touch_events;

typedef struct {
    int pressed;
    int x;
    int y;
} raven_touch_event_t;

static void on_button_clicked(Rid button, void* user_data)
{
    (void)button;
    (void)user_data;
    Rlabel_set_text(s_status, "Touch input is working!");
}

static void raven_task(void* user_data)
{
    (void)user_data;
    RHostPlatformDesc host = {0};
    RInitDesc init = {
        .backend = R_BACKEND_CUSTOM,
        .app_name = "Raven ESP32-S3 Hello",
        .worker_threads = -1,
        .disable_render_thread = R_TRUE,
    };
    Rid box;
    Rid button;
    int previous_pressed = 0;
    Rhost_platform_register(&host);
    Rbackend_register(raven_esp_backend());
    if (!Rinit_ex(&init)) {
        ESP_LOGE(TAG, "Raven initialization failed");
        return;
    }

    s_window = Rwindow_create("Raven on ESP32-S3", RAVEN_DISPLAY_WIDTH, RAVEN_DISPLAY_HEIGHT,
                              R_WINDOW_NO_RESIZE | R_WINDOW_BORDERLESS);
    Rhost_window_set_size(s_window, RAVEN_DISPLAY_WIDTH, RAVEN_DISPLAY_HEIGHT, 1.0f);
    box = Rwidget_create(R_WIDGET_VBOX);
    Rpanel_set_spacing(box, 16.0f);
    Rslot_set_halign(box, R_HALIGN_CENTER);
    Rslot_set_valign(box, R_VALIGN_CENTER);
    Rwindow_set_content(s_window, box);

    Rwidget_add_child(box, Rlabel_create("Hello from Raven"));
    s_status = Rwidget_add_child(box, Rlabel_create("ESP32-S3 software renderer"));
    button = Rwidget_add_child(box, Rbutton_create("Touch me"));
    Rbutton_set_clicked_callback(button, on_button_clicked, NULL);

    ESP_LOGI(TAG, "Raven renderer is running on CPU1");
    while (Rdraw_frame()) {
        raven_touch_event_t event;

        /* Keep all Raven API calls on this one core/task. Core 0 supplies
         * input through the queue below, so the renderer has no cross-core
         * widget or context access. */
        while (xQueueReceive(s_touch_events, &event, 0) == pdPASS) {
            if (event.pressed) Rhost_inject_mouse_move(s_window, event.x, event.y, 0);
            if (event.pressed != previous_pressed) {
                Rhost_inject_mouse_button(s_window, event.x, event.y,
                                          R_MOUSE_LEFT, event.pressed, 0);
                previous_pressed = event.pressed;
            }
        }
        /* CPU rendering a full 800x480 frame is expensive and the RGB panel
         * only scans at about 20 Hz with the selected timing.  Cap rendering
         * to 10 FPS to avoid PSRAM contention with LCD DMA. */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    Rshutdown();
    vTaskDelete(NULL);
}

void app_main(void)
{
    raven_touch_event_t event = {0};
    int previous_pressed = 0;

    /* Keep RGB LCD setup and its ESP-IDF servicing on CPU0. */
    raven_display_init();
    raven_esp_backend_set_panel(raven_display_panel());
    s_touch_events = xQueueCreate(16, sizeof(raven_touch_event_t));
    if (!s_touch_events) {
        ESP_LOGE(TAG, "unable to create touch-event queue");
        return;
    }
    if (xTaskCreatePinnedToCore(raven_task, "raven_render", 12288, NULL, 5,
                                NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "unable to start Raven renderer on CPU1");
        vQueueDelete(s_touch_events);
        return;
    }

    ESP_LOGI(TAG, "touch polling is running on CPU0");
    for (;;) {
        raven_touch_poll(&event.pressed, &event.x, &event.y);
        /* Coalesce steady touch samples if the renderer is busy, but retain
         * every press/release transition for the button event path. */
        if (event.pressed || event.pressed != previous_pressed) {
            (void)xQueueSend(s_touch_events, &event, 0);
            previous_pressed = event.pressed;
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}
