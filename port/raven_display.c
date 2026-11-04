/**
 * @file raven_display.c
 * @brief Elecrow 800x480 RGB panel and GT911 implementation.
 *
 * This file is the board-specific display and touch portion of the port.
 */
#include "raven_display.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TOUCH_I2C_NUM I2C_NUM_0

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_panel_io_handle_t s_touch_io;
static SemaphoreHandle_t s_vblank;

/* The RGB driver calls this from its DMA ISR immediately after it has sent a
 * frame.  The following front porch/VSYNC/back porch is the safest short
 * window in which a single framebuffer can be modified. */
static bool IRAM_ATTR raven_display_frame_complete(esp_lcd_panel_handle_t panel,
                                                    const esp_lcd_rgb_panel_event_data_t* edata,
                                                    void* user_ctx)
{
    BaseType_t task_woken = pdFALSE;
    (void)panel;
    (void)edata;
    (void)user_ctx;
    if (s_vblank) xSemaphoreGiveFromISR(s_vblank, &task_woken);
    return task_woken == pdTRUE;
}

void raven_display_init(void)
{
    const esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16,
        /* Baseline that renders the Raven hello screen on this board. */
        .num_fbs = 1,
        .psram_trans_align = 64,
        .hsync_gpio_num = 39,
        .vsync_gpio_num = 40,
        .de_gpio_num = 41,
        .pclk_gpio_num = 0,
        .disp_gpio_num = -1,
        .data_gpio_nums = {15, 7, 6, 5, 4, 9, 46, 3, 8, 16, 1, 14, 21, 47, 48, 45},
        .timings = {
            .pclk_hz = 10000000UL,
            .h_res = RAVEN_DISPLAY_WIDTH,
            .v_res = RAVEN_DISPLAY_HEIGHT,
            .hsync_front_porch = 40,
            .hsync_pulse_width = 48,
            .hsync_back_porch = 40,
            .vsync_front_porch = 1,
            .vsync_pulse_width = 31,
            .vsync_back_porch = 13,
            .flags = {
                .hsync_idle_low = true,
                .vsync_idle_low = true,
                .de_idle_high = false,
                .pclk_active_neg = true,
                .pclk_idle_high = false,
            },
        },
        .flags = {.fb_in_psram = true},
    };
    const i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_19,
        .scl_io_num = GPIO_NUM_20,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    const esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    const esp_lcd_touch_io_gt911_config_t gt911_config = {.dev_addr = touch_io_config.dev_addr};
    const esp_lcd_touch_config_t touch_config = {
        .x_max = RAVEN_DISPLAY_WIDTH,
        .y_max = RAVEN_DISPLAY_HEIGHT,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        .driver_data = (void*)&gt911_config,
    };

    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_2, 1));
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    s_vblank = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_vblank ? ESP_OK : ESP_ERR_NO_MEM);
    {
        const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
            .on_frame_buf_complete = raven_display_frame_complete,
        };
        ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &callbacks, NULL));
    }

    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_NUM, &i2c_config));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_NUM, i2c_config.mode, 0, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(TOUCH_I2C_NUM, &touch_io_config, &s_touch_io));
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(s_touch_io, &touch_config, &s_touch));
}

esp_lcd_panel_handle_t raven_display_panel(void)
{
    return s_panel;
}

int raven_display_wait_for_vblank(void)
{
    if (!s_vblank) return 0;
    /* A binary semaphore may contain the previous frame's notification.
     * Drain it so a successful wait always means a fresh blank interval. */
    while (xSemaphoreTake(s_vblank, 0) == pdTRUE) {
    }
    return xSemaphoreTake(s_vblank, pdMS_TO_TICKS(100)) == pdTRUE;
}

void raven_touch_poll(int* pressed, int* x, int* y)
{
    uint16_t touch_x = 0, touch_y = 0, strength = 0;
    uint8_t count = 0;
    *pressed = 0;
    esp_lcd_touch_read_data(s_touch);
    if (esp_lcd_touch_get_coordinates(s_touch, &touch_x, &touch_y, &strength, &count, 1)) {
        *x = touch_x;
        *y = touch_y;
        *pressed = count != 0;
    }
}
