/**
 * @file main.c
 * @brief ESP-IDF entry point, task placement, and touch event forwarding.
 *
 * Board-specific functions are supplied by the selected implementation in
 * @c port; application.c remains independent of those details.
 */
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
static TaskHandle_t s_application_task;

/**
 * @brief Poll GT911 promptly and wake Raven whenever a meaningful point changes.
 *
 * This HMI does not expose a reliable MCU interrupt line for GT911: GPIO38 is
 * part of its reset/address path. A 5 ms poll therefore provides predictable
 * input without reconfiguring that board-critical pin.
 */
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
            if (xQueueSend(s_touch_events, &event, 0) == pdPASS) {
                previous_pressed = event.pressed;
                previous_x = event.x;
                previous_y = event.y;
                if (s_application_task) xTaskNotifyGive(s_application_task);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/** @brief Initialize the board port and start the input and Raven tasks. */
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
                                s_touch_events, 5, &s_application_task, 1) != pdPASS ||
        xTaskCreatePinnedToCore(touch_task, "touch_poll", 4096,
                                NULL, 6, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "unable to start Studio tasks");
        return;
    }
    vTaskDelete(NULL);
}
