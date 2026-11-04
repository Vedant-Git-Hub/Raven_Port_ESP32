#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct {
    int pressed;
    int x;
    int y;
} raven_touch_event_t;

/* Runs Raven's retained-mode Studio UI on the rendering core. */
void raven_application_task(void* user_data);
