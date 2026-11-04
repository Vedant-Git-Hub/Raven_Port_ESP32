/**
 * @file raven_esp_thread.c
 * @brief FreeRTOS implementation of Raven's thread and synchronization hooks.
 */
#include "core/thread.h"

#include "esp_chip_info.h"
#include "esp_timer.h"

#include <stdlib.h>

typedef struct raven_esp_thread_start_t {
    rvn_thread_fn function;
    void* arg;
    SemaphoreHandle_t done;
} raven_esp_thread_start_t;

static void raven_esp_thread_entry(void* arg)
{
    raven_esp_thread_start_t* start = arg;
    rvn_thread_fn function = start->function;
    void* function_arg = start->arg;
    SemaphoreHandle_t done = start->done;
    free(start);
    function(function_arg);
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}

int rvn_thread_create(rvn_thread_t* thread, rvn_thread_fn function, void* arg, const char* name)
{
    raven_esp_thread_start_t* start;
    if (!thread || !function) return R_FALSE;
    thread->done = xSemaphoreCreateBinary();
    if (!thread->done) return R_FALSE;
    start = malloc(sizeof(*start));
    if (!start) {
        vSemaphoreDelete(thread->done);
        thread->done = NULL;
        return R_FALSE;
    }
    start->function = function;
    start->arg = arg;
    start->done = thread->done;
    if (xTaskCreate(raven_esp_thread_entry, name ? name : "raven", 4096, start, 5, &thread->handle) != pdPASS) {
        free(start);
        vSemaphoreDelete(thread->done);
        thread->done = NULL;
        return R_FALSE;
    }
    return R_TRUE;
}

void rvn_thread_join(rvn_thread_t* thread)
{
    if (!thread || !thread->done) return;
    xSemaphoreTake(thread->done, portMAX_DELAY);
    vSemaphoreDelete(thread->done);
    thread->done = NULL;
    thread->handle = NULL;
}

uint64_t rvn_thread_id(void)
{
    return (uint64_t)(uintptr_t)xTaskGetCurrentTaskHandle();
}

int rvn_cpu_count(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    return info.cores > 0 ? info.cores : 1;
}

void rvn_sleep_ms(int milliseconds)
{
    if (milliseconds <= 0) {
        taskYIELD();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS((uint32_t)milliseconds));
}

double rvn_clock_seconds(void)
{
    return (double)esp_timer_get_time() * 1e-6;
}

void rvn_mutex_init(rvn_mutex_t* mutex) { mutex->handle = xSemaphoreCreateMutex(); }
void rvn_mutex_destroy(rvn_mutex_t* mutex) { if (mutex->handle) vSemaphoreDelete(mutex->handle); mutex->handle = NULL; }
void rvn_mutex_lock(rvn_mutex_t* mutex) { xSemaphoreTake(mutex->handle, portMAX_DELAY); }
void rvn_mutex_unlock(rvn_mutex_t* mutex) { xSemaphoreGive(mutex->handle); }

void rvn_rwlock_init(rvn_rwlock_t* lock) { lock->handle = xSemaphoreCreateMutex(); }
void rvn_rwlock_destroy(rvn_rwlock_t* lock) { if (lock->handle) vSemaphoreDelete(lock->handle); lock->handle = NULL; }
void rvn_rwlock_read_lock(rvn_rwlock_t* lock) { xSemaphoreTake(lock->handle, portMAX_DELAY); }
void rvn_rwlock_read_unlock(rvn_rwlock_t* lock) { xSemaphoreGive(lock->handle); }
void rvn_rwlock_write_lock(rvn_rwlock_t* lock) { xSemaphoreTake(lock->handle, portMAX_DELAY); }
void rvn_rwlock_write_unlock(rvn_rwlock_t* lock) { xSemaphoreGive(lock->handle); }

void rvn_cond_init(rvn_cond_t* cond) { cond->handle = xSemaphoreCreateBinary(); }
void rvn_cond_destroy(rvn_cond_t* cond) { if (cond->handle) vSemaphoreDelete(cond->handle); cond->handle = NULL; }
void rvn_cond_wait(rvn_cond_t* cond, rvn_mutex_t* mutex)
{
    rvn_mutex_unlock(mutex);
    xSemaphoreTake(cond->handle, portMAX_DELAY);
    rvn_mutex_lock(mutex);
}

int rvn_cond_wait_timeout(rvn_cond_t* cond, rvn_mutex_t* mutex, double seconds)
{
    TickType_t ticks;
    if (seconds <= 0.0) return R_FALSE;
    ticks = pdMS_TO_TICKS((uint32_t)(seconds * 1000.0 + 0.999));
    if (ticks == 0) ticks = 1;
    rvn_mutex_unlock(mutex);
    int signalled = xSemaphoreTake(cond->handle, ticks) == pdTRUE;
    rvn_mutex_lock(mutex);
    return signalled ? R_TRUE : R_FALSE;
}
void rvn_cond_signal(rvn_cond_t* cond) { xSemaphoreGive(cond->handle); }
void rvn_cond_broadcast(rvn_cond_t* cond) { xSemaphoreGive(cond->handle); }
