#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

/** @brief FAT filesystem mount point used by the Elecrow microSD slot. */
#define RAVEN_SD_MOUNT_POINT "/sdcard"
/** @brief Maximum BMP entries the gallery keeps in memory at one time. */
#define RAVEN_SD_MAX_FILES 40
/** @brief Maximum display name, including the terminating NUL. */
#define RAVEN_SD_NAME_SIZE 96
/** @brief Maximum path, including the terminating NUL. */
#define RAVEN_SD_PATH_SIZE 128

/** One supported BMP file discovered in the root of the mounted card. */
typedef struct {
    char name[RAVEN_SD_NAME_SIZE];
    char path[RAVEN_SD_PATH_SIZE];
    uint32_t size_bytes;
} raven_sd_file_t;

/** @brief Mount the onboard microSD card over SPI and expose its FAT volume. */
int raven_sd_mount(void);
/** @brief Unmount the card and release the SPI host. Safe when not mounted. */
void raven_sd_unmount(void);
/** @brief Return non-zero when the FAT filesystem is currently mounted. */
int raven_sd_is_mounted(void);
/**
 * @brief Lock SD-card filesystem access across the gallery and HTTP server.
 *
 * The lock is recursive, so port helpers may safely call one another while a
 * browser transfer owns the card.
 */
int raven_sd_lock(TickType_t wait_ticks);
/** @brief Release a lock acquired with raven_sd_lock(). */
void raven_sd_unlock(void);
/** @brief Enumerate root-level .bmp files, sorted by filename. */
int raven_sd_list_bmps(raven_sd_file_t* files, int capacity);
/**
 * @brief Read a BMP into PSRAM for immediate decoding by Raven.
 *
 * The caller must release a successful result with raven_sd_free().
 */
uint8_t* raven_sd_read_file(const char* path, size_t* size);
/** @brief Release a buffer returned by raven_sd_read_file(). */
void raven_sd_free(void* data);
