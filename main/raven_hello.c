#include "raven_display.h"
#include "raven_esp_backend.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <Raven.h>
#include <stdint.h>
#include <stdlib.h>

static const char* TAG = "raven_hello";
static Rid s_window;
static QueueHandle_t s_touch_events;

static struct {
    Rid pages;
    Rid title;
    Rid subtitle;
    Rid status;
    Rid navigation[3];
} s_ui;

typedef struct {
    int pressed;
    int x;
    int y;
} raven_touch_event_t;

static Rid add_label(Rid parent, const char* text, float size, float r, float g, float b)
{
    Rid label = Rwidget_add_child(parent, Rlabel_create(text));
    Rlabel_set_text_size(label, size);
    Rlabel_set_color(label, r, g, b, 1.0f);
    return label;
}

static Rid add_card(Rid parent, const char* eyebrow, const char* value,
                    const char* detail, float r, float g, float b)
{
    Rid card = Rwidget_add_child(parent, Rwidget_create(R_WIDGET_BORDER));
    Rid body = Rwidget_add_child(card, Rwidget_create(R_WIDGET_VBOX));
    Rborder_set_background(card, r, g, b, 0.28f);
    Rborder_set_outline(card, 1.0f, r, g, b, 0.72f);
    Rborder_set_corner_radius(card, 18.0f, 18.0f, 18.0f, 18.0f);
    Rborder_set_padding(card, 18.0f, 16.0f, 18.0f, 16.0f);
    Rborder_set_shadow(card, 10.0f, 0.0f, 4.0f);
    Rpanel_set_spacing(body, 7.0f);
    add_label(body, eyebrow, 14.0f, 0.76f, 0.84f, 0.95f);
    add_label(body, value, 30.0f, 1.0f, 1.0f, 1.0f);
    add_label(body, detail, 13.0f, 0.74f, 0.80f, 0.90f);
    return card;
}

static void select_screen(int screen)
{
    static const char* const titles[] = {"Good evening, Vedant", "Display controls", "About this port"};
    static const char* const subtitles[] = {
        "Raven UI dashboard · ESP32-S3",
        "Tune your interface with a live preview",
        "A compact C graphics demo for CrowPanel 7"
    };
    int i;
    if (screen < 0 || screen > 2) return;
    Rswitcher_set_index(s_ui.pages, screen);
    Rlabel_set_text(s_ui.title, titles[screen]);
    Rlabel_set_text(s_ui.subtitle, subtitles[screen]);
    for (i = 0; i < 3; ++i) {
        Rbutton_set_style(s_ui.navigation[i], i == screen ? R_BUTTON_PRIMARY : R_BUTTON_FLAT);
    }
}

static void on_navigation_clicked(Rid button, void* user_data)
{
    (void)button;
    select_screen((int)(intptr_t)user_data);
}

static void on_action_clicked(Rid button, void* user_data)
{
    (void)button;
    (void)user_data;
    Rlabel_set_text(s_ui.status, "Touch received · interface updated");
}

static void on_slider_changed(Rid slider, float value, void* user_data)
{
    char text[48];
    (void)slider;
    (void)user_data;
    snprintf(text, sizeof(text), "Brightness preview · %.0f%%", (double)value);
    Rlabel_set_text(s_ui.status, text);
}

static void build_home_page(Rid page)
{
    Rid hero = Rwidget_add_child(page, Rwidget_create(R_WIDGET_BORDER));
    Rid hero_body = Rwidget_add_child(hero, Rwidget_create(R_WIDGET_VBOX));
    Rid cards = Rwidget_add_child(page, Rwidget_create(R_WIDGET_HBOX));
    Rid button;
    Rborder_set_background(hero, 0.16f, 0.31f, 0.72f, 0.92f);
    Rborder_set_outline(hero, 1.0f, 0.34f, 0.76f, 1.0f, 0.9f);
    Rborder_set_corner_radius(hero, 22.0f, 22.0f, 22.0f, 22.0f);
    Rborder_set_padding(hero, 24.0f, 20.0f, 24.0f, 20.0f);
    Rborder_set_shadow(hero, 14.0f, 0.0f, 5.0f);
    Rpanel_set_spacing(hero_body, 8.0f);
    add_label(hero_body, "RAVEN / ESP32-S3", 14.0f, 0.68f, 0.89f, 1.0f);
    add_label(hero_body, "A calmer way to build embedded UI.", 28.0f, 1.0f, 1.0f, 1.0f);
    add_label(hero_body, "A colorful software-rendered dashboard, shaped for your CrowPanel.", 14.0f, 0.86f, 0.92f, 1.0f);
    button = Rwidget_add_child(hero_body, Rbutton_create("Explore controls"));
    Rbutton_set_style(button, R_BUTTON_PRIMARY);
    Rbutton_set_clicked_callback(button, on_navigation_clicked, (void*)(intptr_t)1);
    Rslot_set_halign(button, R_HALIGN_LEFT);
    Rslot_set_padding(button, 0.0f, 7.0f, 0.0f, 0.0f);
    Rpanel_set_spacing(cards, 14.0f);
    Rslot_set_fill(cards, 1.0f);
    Rslot_set_fill(add_card(cards, "RENDER ENGINE", "Raven", "CPU1 software canvas", 0.40f, 0.20f, 0.74f), 1.0f);
    Rslot_set_fill(add_card(cards, "TOUCH LINK", "Ready", "GT911 on I2C", 0.05f, 0.55f, 0.48f), 1.0f);
    Rslot_set_fill(add_card(cards, "DISPLAY", "800 × 480", "RGB565 color space", 0.92f, 0.36f, 0.18f), 1.0f);
}

static void build_controls_page(Rid page)
{
    Rid card = Rwidget_add_child(page, Rwidget_create(R_WIDGET_BORDER));
    Rid body = Rwidget_add_child(card, Rwidget_create(R_WIDGET_VBOX));
    Rid slider;
    Rid progress;
    Rid button;
    Rborder_set_background(card, 0.08f, 0.19f, 0.29f, 0.94f);
    Rborder_set_outline(card, 1.0f, 0.18f, 0.72f, 0.78f, 0.8f);
    Rborder_set_corner_radius(card, 20.0f, 20.0f, 20.0f, 20.0f);
    Rborder_set_padding(card, 24.0f, 22.0f, 24.0f, 22.0f);
    Rborder_set_shadow(card, 12.0f, 0.0f, 4.0f);
    Rpanel_set_spacing(body, 14.0f);
    add_label(body, "Display mood", 26.0f, 1.0f, 1.0f, 1.0f);
    add_label(body, "These widgets exercise Raven controls without needing external assets.", 14.0f, 0.72f, 0.84f, 0.95f);
    add_label(body, "BRIGHTNESS PREVIEW", 13.0f, 0.37f, 0.88f, 0.84f);
    slider = Rwidget_add_child(body, Rwidget_create(R_WIDGET_SLIDER));
    Rslider_set_range(slider, 0.0f, 100.0f);
    Rslider_set_value(slider, 72.0f);
    Rslider_set_changed_callback(slider, on_slider_changed, NULL);
    add_label(body, "SYSTEM HEALTH", 13.0f, 0.98f, 0.73f, 0.34f);
    progress = Rwidget_add_child(body, Rwidget_create(R_WIDGET_PROGRESSBAR));
    Rprogress_bar_set_value(progress, 0.78f);
    button = Rwidget_add_child(body, Rbutton_create("Apply a fresh color profile"));
    Rbutton_set_style(button, R_BUTTON_PRIMARY);
    Rbutton_set_clicked_callback(button, on_action_clicked, NULL);
    Rslot_set_halign(button, R_HALIGN_LEFT);
    Rslot_set_padding(button, 0.0f, 4.0f, 0.0f, 0.0f);
}

static void build_about_page(Rid page)
{
    Rid row = Rwidget_add_child(page, Rwidget_create(R_WIDGET_HBOX));
    Rid story = Rwidget_add_child(row, Rwidget_create(R_WIDGET_BORDER));
    Rid story_body = Rwidget_add_child(story, Rwidget_create(R_WIDGET_VBOX));
    Rid badge = Rwidget_add_child(row, Rwidget_create(R_WIDGET_BORDER));
    Rid badge_body = Rwidget_add_child(badge, Rwidget_create(R_WIDGET_VBOX));
    Rpanel_set_spacing(row, 16.0f);
    Rslot_set_fill(row, 1.0f);
    Rslot_set_fill(story, 2.0f);
    Rslot_set_fill(badge, 1.0f);
    Rborder_set_background(story, 0.14f, 0.12f, 0.30f, 0.96f);
    Rborder_set_outline(story, 1.0f, 0.58f, 0.42f, 0.98f, 0.75f);
    Rborder_set_corner_radius(story, 20.0f, 20.0f, 20.0f, 20.0f);
    Rborder_set_padding(story, 24.0f, 22.0f, 24.0f, 22.0f);
    Rpanel_set_spacing(story_body, 12.0f);
    add_label(story_body, "Built for the curious", 27.0f, 1.0f, 1.0f, 1.0f);
    add_label(story_body, "Raven brings retained widgets, layout and interaction to an ESP32-S3. This demo keeps the architecture small: one renderer, one touch queue and an RGB panel.", 15.0f, 0.79f, 0.78f, 0.96f);
    Rborder_set_background(badge, 0.07f, 0.47f, 0.56f, 0.92f);
    Rborder_set_corner_radius(badge, 20.0f, 20.0f, 20.0f, 20.0f);
    Rborder_set_padding(badge, 22.0f, 22.0f, 22.0f, 22.0f);
    Rpanel_set_spacing(badge_body, 10.0f);
    add_label(badge_body, "PORT STATUS", 13.0f, 0.77f, 1.0f, 0.96f);
    add_label(badge_body, "ONLINE", 30.0f, 1.0f, 1.0f, 1.0f);
    add_label(badge_body, "ESP-IDF · C · Raven", 14.0f, 0.79f, 0.95f, 1.0f);
}

static void build_interface(void)
{
    Rid root = Rwidget_create(R_WIDGET_HBOX);
    Rid sidebar = Rwidget_add_child(root, Rwidget_create(R_WIDGET_BORDER));
    Rid side_body = Rwidget_add_child(sidebar, Rwidget_create(R_WIDGET_VBOX));
    Rid content = Rwidget_add_child(root, Rwidget_create(R_WIDGET_VBOX));
    Rid header = Rwidget_add_child(content, Rwidget_create(R_WIDGET_HBOX));
    Rid heading = Rwidget_add_child(header, Rwidget_create(R_WIDGET_VBOX));
    Rid footer;
    int i;
    static const char* const nav_text[] = {"Home", "Controls", "About"};

    Rwindow_set_clear_color(s_window, 0.025f, 0.04f, 0.10f, 1.0f);
    Rwindow_set_content(s_window, root);
    Rwidget_set_size(sidebar, 174.0f, -1.0f);
    Rslot_set_fill(content, 1.0f);
    Rborder_set_background(sidebar, 0.055f, 0.085f, 0.18f, 0.98f);
    Rborder_set_outline(sidebar, 1.0f, 0.18f, 0.27f, 0.48f, 0.75f);
    Rborder_set_padding(sidebar, 16.0f, 22.0f, 16.0f, 18.0f);
    Rpanel_set_spacing(side_body, 10.0f);
    add_label(side_body, "RAVEN", 28.0f, 0.54f, 0.86f, 1.0f);
    add_label(side_body, "DISPLAY STUDIO", 11.0f, 0.60f, 0.67f, 0.84f);
    Rwidget_add_child(side_body, Rwidget_create(R_WIDGET_SPACER));
    for (i = 0; i < 3; ++i) {
        s_ui.navigation[i] = Rwidget_add_child(side_body, Rbutton_create(nav_text[i]));
        Rbutton_set_style(s_ui.navigation[i], R_BUTTON_FLAT);
        Rbutton_set_clicked_callback(s_ui.navigation[i], on_navigation_clicked, (void*)(intptr_t)i);
    }
    footer = add_label(side_body, "v0.1 · ESP32-S3", 12.0f, 0.45f, 0.56f, 0.72f);
    Rslot_set_padding(footer, 0.0f, 12.0f, 0.0f, 0.0f);

    Rslot_set_padding(content, 24.0f, 20.0f, 24.0f, 20.0f);
    Rpanel_set_spacing(content, 16.0f);
    Rslot_set_fill(heading, 1.0f);
    Rpanel_set_spacing(heading, 3.0f);
    s_ui.title = add_label(heading, "Good evening, Vedant", 27.0f, 0.96f, 0.98f, 1.0f);
    s_ui.subtitle = add_label(heading, "Raven UI dashboard · ESP32-S3", 13.0f, 0.55f, 0.69f, 0.88f);
    s_ui.status = Rwidget_add_child(header, Rlabel_create("Touch ready"));
    Rlabel_set_color(s_ui.status, 0.36f, 0.94f, 0.68f, 1.0f);
    Rslot_set_halign(s_ui.status, R_HALIGN_RIGHT);
    Rslot_set_valign(s_ui.status, R_VALIGN_CENTER);

    s_ui.pages = Rwidget_add_child(content, Rwidget_create(R_WIDGET_SWITCHER));
    Rslot_set_fill(s_ui.pages, 1.0f);
    {
        Rid page = Rwidget_add_child(s_ui.pages, Rwidget_create(R_WIDGET_VBOX));
        Rpanel_set_spacing(page, 16.0f);
        build_home_page(page);
    }
    {
        Rid page = Rwidget_add_child(s_ui.pages, Rwidget_create(R_WIDGET_VBOX));
        Rpanel_set_spacing(page, 16.0f);
        build_controls_page(page);
    }
    {
        Rid page = Rwidget_add_child(s_ui.pages, Rwidget_create(R_WIDGET_VBOX));
        Rpanel_set_spacing(page, 16.0f);
        build_about_page(page);
    }
    select_screen(0);
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
    int previous_pressed = 0;
    Rhost_platform_register(&host);
    Rbackend_register(raven_esp_backend());
    if (!Rinit_ex(&init)) {
        ESP_LOGE(TAG, "Raven initialization failed");
        return;
    }

    Rtheme_apply(R_THEME_GLASS_DARK);
    Rtheme_set_accent(0.17f, 0.62f, 1.0f);
    /* A full software-rendered RGB frame costs too much on this panel for
     * spring animation.  Keep the rich visual style, but make interaction
     * states immediate so an idle display never keeps repainting itself. */
    Rset_animations_enabled(R_FALSE);
    s_window = Rwindow_create("Raven Display Studio", RAVEN_DISPLAY_WIDTH, RAVEN_DISPLAY_HEIGHT,
                              R_WINDOW_NO_RESIZE | R_WINDOW_BORDERLESS);
    Rhost_window_set_size(s_window, RAVEN_DISPLAY_WIDTH, RAVEN_DISPLAY_HEIGHT, 1.0f);
    build_interface();

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

static void touch_task(void* user_data)
{
    (void)user_data;
    raven_touch_event_t event = {0};
    int previous_pressed = 0;
    int previous_x = -1000;
    int previous_y = -1000;

    ESP_LOGI(TAG, "touch polling is running on CPU0 at low priority");
    for (;;) {
        raven_touch_poll(&event.pressed, &event.x, &event.y);
        /* GT911 is polled quickly, but a static finger must not create 125
         * identical mouse-move events per second.  Preserve press/release
         * edges and only forward meaningful motion for drags/sliders. */
        if (event.pressed != previous_pressed ||
            (event.pressed && (abs(event.x - previous_x) >= 4 || abs(event.y - previous_y) >= 4))) {
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
    /* Priority 0 is reserved for FreeRTOS's idle task. */
    if (xTaskCreatePinnedToCore(touch_task, "touch_poll", 4096, NULL, 1,
                                NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "unable to start touch polling task");
        return;
    }
    vTaskDelete(NULL);
}
