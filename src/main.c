/* Board entry point.  Keep target-specific display and touch plumbing in
 * ../port so application.c remains Raven application code. */
#include "application.h"
#include "raven_display.h"
#include "raven_esp_backend.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdlib.h>

static const char* TAG = "raven_main";
static QueueHandle_t s_touch_events;

static void touch_task(void* user_data)
{
    raven_touch_event_t event = {0};
    int previous_pressed = 0;
    int previous_x = -1000;
    int previous_y = -1000;
    (void)user_data;

    for (;;) {
        raven_touch_poll(&event.pressed, &event.x, &event.y);
        if (event.pressed != previous_pressed ||
            (event.pressed && (abs(event.x - previous_x) >= 4 ||
                               abs(event.y - previous_y) >= 4))) {
            (void)xQueueSend(s_touch_events, &event, 0);
            previous_pressed = event.pressed;
            previous_x = event.x;
            previous_y = event.y;
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void app_main(void)
{
    raven_display_init();
    raven_esp_backend_set_panel(raven_display_panel());

    s_touch_events = xQueueCreate(16, sizeof(raven_touch_event_t));
    if (!s_touch_events) {
        ESP_LOGE(TAG, "unable to create touch queue");
        return;
    }
    if (xTaskCreatePinnedToCore(raven_application_task, "raven_studio", 16384,
                                s_touch_events, 5, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(touch_task, "touch_poll", 4096,
                                NULL, 1, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "unable to start Studio tasks");
        return;
    }
    vTaskDelete(NULL);
}
