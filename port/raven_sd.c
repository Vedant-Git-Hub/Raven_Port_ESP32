/**
 * @file raven_sd.c
 * @brief SPI/FAT microSD implementation for the Elecrow 7-inch HMI.
 *
 * The board's verified V3 wiring is CS=GPIO10, MOSI=GPIO11, SCK=GPIO12,
 * MISO=GPIO13. The gallery intentionally scans only the card root and only
 * accepts BMP files, keeping the interactive UI predictable on an MCU.
 */
#include "raven_sd.h"

#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define RAVEN_SD_CS GPIO_NUM_10
#define RAVEN_SD_MOSI GPIO_NUM_11
#define RAVEN_SD_SCK GPIO_NUM_12
#define RAVEN_SD_MISO GPIO_NUM_13
#define RAVEN_SD_MAX_BMP_BYTES (2U * 1024U * 1024U)

static const char* TAG = "raven_sd";
static sdmmc_card_t* s_card;
static int s_bus_initialized;
static SemaphoreHandle_t s_mutex;

/** @brief Lazily create the recursive FATFS access lock. */
static int raven_sd_create_lock(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();
    return s_mutex != NULL;
}

int raven_sd_lock(TickType_t wait_ticks)
{
    return raven_sd_create_lock() &&
           xSemaphoreTakeRecursive(s_mutex, wait_ticks) == pdTRUE;
}

void raven_sd_unlock(void)
{
    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
}

/** @brief Compare entries without imposing case sensitivity on FAT filenames. */
static int raven_sd_compare_files(const void* left, const void* right)
{
    const raven_sd_file_t* a = left;
    const raven_sd_file_t* b = right;
    return strcasecmp(a->name, b->name);
}

/** @brief Return non-zero for a .bmp extension, regardless of case. */
static int raven_sd_is_bmp(const char* name)
{
    const char* extension = name ? strrchr(name, '.') : NULL;
    return extension && strcasecmp(extension, ".bmp") == 0;
}

int raven_sd_mount(void)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = RAVEN_SD_MOSI,
        .miso_io_num = RAVEN_SD_MISO,
        .sclk_io_num = RAVEN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16 * 1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t device_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t result;

    if (!raven_sd_lock(pdMS_TO_TICKS(1000))) return 0;
    if (s_card) {
        raven_sd_unlock();
        return 1;
    }
    host.slot = SPI2_HOST;
    device_config.host_id = host.slot;
    device_config.gpio_cs = RAVEN_SD_CS;
    result = spi_bus_initialize(host.slot, &bus_config, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus unavailable: %s", esp_err_to_name(result));
        raven_sd_unlock();
        return 0;
    }
    s_bus_initialized = 1;
    result = esp_vfs_fat_sdspi_mount(RAVEN_SD_MOUNT_POINT, &host, &device_config,
                                     &mount_config, &s_card);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(result));
        s_card = NULL;
        spi_bus_free(host.slot);
        s_bus_initialized = 0;
        raven_sd_unlock();
        return 0;
    }
    ESP_LOGI(TAG, "mounted SD card at %s", RAVEN_SD_MOUNT_POINT);
    raven_sd_unlock();
    return 1;
}

void raven_sd_unmount(void)
{
    if (!raven_sd_lock(pdMS_TO_TICKS(5000))) return;
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(RAVEN_SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
    if (s_bus_initialized) {
        spi_bus_free(SPI2_HOST);
        s_bus_initialized = 0;
    }
    raven_sd_unlock();
}

int raven_sd_is_mounted(void)
{
    return s_card != NULL;
}

int raven_sd_list_bmps(raven_sd_file_t* files, int capacity)
{
    DIR* directory;
    struct dirent* entry;
    int count = 0;

    if (!files || capacity <= 0 || !raven_sd_lock(pdMS_TO_TICKS(1000))) return 0;
    if (!s_card) {
        raven_sd_unlock();
        return 0;
    }
    directory = opendir(RAVEN_SD_MOUNT_POINT);
    if (!directory) {
        raven_sd_unlock();
        return 0;
    }
    while ((entry = readdir(directory)) != NULL && count < capacity) {
        struct stat info;
        raven_sd_file_t* file;
        size_t name_length;
        const size_t mount_length = sizeof(RAVEN_SD_MOUNT_POINT) - 1U;
        if (!raven_sd_is_bmp(entry->d_name)) continue;
        name_length = strnlen(entry->d_name, RAVEN_SD_NAME_SIZE);
        if (name_length == RAVEN_SD_NAME_SIZE ||
            mount_length + 1U + name_length + 1U > RAVEN_SD_PATH_SIZE) continue;
        file = &files[count];
        memcpy(file->name, entry->d_name, name_length + 1U);
        memcpy(file->path, RAVEN_SD_MOUNT_POINT, mount_length);
        file->path[mount_length] = '/';
        memcpy(file->path + mount_length + 1U, entry->d_name, name_length + 1U);
        if (stat(file->path, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        file->size_bytes = (uint32_t)info.st_size;
        count++;
    }
    closedir(directory);
    qsort(files, (size_t)count, sizeof(*files), raven_sd_compare_files);
    raven_sd_unlock();
    return count;
}

uint8_t* raven_sd_read_file(const char* path, size_t* size)
{
    FILE* file;
    long length;
    uint8_t* data;

    if (size) *size = 0;
    if (!path || !raven_sd_lock(pdMS_TO_TICKS(5000))) return NULL;
    if (!s_card) {
        raven_sd_unlock();
        return NULL;
    }
    file = fopen(path, "rb");
    if (!file) {
        raven_sd_unlock();
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        (size_t)length > RAVEN_SD_MAX_BMP_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        raven_sd_unlock();
        return NULL;
    }
    data = heap_caps_malloc((size_t)length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        heap_caps_free(data);
        fclose(file);
        raven_sd_unlock();
        return NULL;
    }
    fclose(file);
    if (size) *size = (size_t)length;
    raven_sd_unlock();
    return data;
}

void raven_sd_free(void* data)
{
    heap_caps_free(data);
}
