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

/* GPIO38 only wakes this task when GT911 has new data.  Keep it just below
 * ESP-IDF's Wi-Fi task: the brief I2C transaction should preempt ordinary
 * work, without starving the networking stack. */
#define RAVEN_TOUCH_TASK_PRIORITY 22

/* Raven's renderer is isolated on CPU1, so it can prepare the next display
 * buffer promptly after the input task signals a user action. */
#define RAVEN_APPLICATION_TASK_PRIORITY 8

/**
 * @brief Read GT911 after its GPIO38 interrupt and wake Raven on changes.
 *
 * The display port owns the ISR and only wakes this task from it. I2C transfers
 * stay in task context, so touch input cannot block an interrupt handler.
 */
static void touch_task(void* user_data)
{
    raven_touch_event_t event = {0};
    int previous_pressed = 0;
    int previous_x = -1000;
    int previous_y = -1000;
    (void)user_data;

    for (;;) {
        raven_touch_wait_for_interrupt();
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
                                s_touch_events, RAVEN_APPLICATION_TASK_PRIORITY,
                                &s_application_task, 1) != pdPASS ||
        xTaskCreatePinnedToCore(touch_task, "touch_poll", 4096,
                                NULL, RAVEN_TOUCH_TASK_PRIORITY, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "unable to start Studio tasks");
        return;
    }
    vTaskDelete(NULL);
}
