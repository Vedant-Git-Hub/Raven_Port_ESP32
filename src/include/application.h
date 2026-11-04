#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** A normalized GT911 event sent from the input task to Raven. */
typedef struct {
    int pressed;
    int x;
    int y;
} raven_touch_event_t;

/**
 * @brief Run Raven's retained-mode Studio UI on the rendering core.
 *
 * @param user_data QueueHandle_t containing #raven_touch_event_t entries.
 */
void raven_application_task(void* user_data);
