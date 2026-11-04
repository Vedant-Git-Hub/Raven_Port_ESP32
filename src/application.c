/**
 * @file application.c
 * @brief Raven Studio demonstration application for the embedded host.
 *
 * The upstream Studio XML and application behaviour are preserved. Only the
 * native desktop loop is replaced by RGB-panel rendering and queued touch.
 */
#include "application.h"
#include "raven_display.h"
#include "raven_esp_backend.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <Raven.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char studio_xml_start[] asm("_binary_studio_xml_start");
extern const char studio_xml_end[] asm("_binary_studio_xml_end");

static const char* TAG = "raven_studio";

static void raven_error(RError code, RSeverity severity, const char* detail)
{
    ESP_LOGE(TAG, "Raven %s (%s): %s", Rerror_to_string(code), Rseverity_to_string(severity), detail);
}

static struct {
    Rid window;
    Rid graph;
    Rid timeline;
    Rid tint;
    Rid selected;
    Rid time;
    Rid play;
    int added;
} app;

static void find_widgets(void)
{
    app.graph = Rwidget_find(app.window, "graph");
    app.timeline = Rwidget_find(app.window, "timeline");
    app.tint = Rwidget_find(app.window, "tint");
    app.selected = Rwidget_find(app.window, "selected");
    app.time = Rwidget_find(app.window, "time");
    app.play = Rwidget_find(app.window, "play");
}

static int reaches(Rid graph, int node, int target, int depth)
{
    int i, k;
    if (node == target) return R_TRUE;
    if (depth > 64) return R_FALSE;
    for (i = 0; ; ++i) {
        int output = Rnode_graph_get_output(graph, node, i);
        if (!output) break;
        for (k = 0; k < Rnode_graph_get_link_count(graph); ++k) {
            int link = Rnode_graph_get_link(graph, k);
            if (Rnode_graph_get_link_output(graph, link) != output) continue;
            if (reaches(graph, Rnode_graph_get_pin_node(graph, Rnode_graph_get_link_input(graph, link)),
                        target, depth + 1)) return R_TRUE;
        }
    }
    return R_FALSE;
}

static int on_connect(Rid graph, int output_pin, int input_pin, void* user_data)
{
    (void)user_data;
    return !reaches(graph, Rnode_graph_get_pin_node(graph, input_pin),
                   Rnode_graph_get_pin_node(graph, output_pin), 0);
}

static void on_selection(Rid graph, int node, void* user_data)
{
    char text[160];
    int count = Rnode_graph_get_selection_count(graph);
    (void)node;
    (void)user_data;
    if (count == 0) snprintf(text, sizeof(text), "No node selected");
    else if (count == 1) snprintf(text, sizeof(text), "%.100s",
                                  Rnode_graph_get_node_title(graph, Rnode_graph_get_selected_node(graph, 0)));
    else snprintf(text, sizeof(text), "%d nodes selected", count);
    Rlabel_set_text(app.selected, text);
}

static void on_removed(Rid graph, int node, void* user_data)
{
    (void)graph;
    (void)node;
    (void)user_data;
}

static void action_tint(Rid sender, const char* action, void* user_data)
{
    float r, g, b, a;
    int i;
    (void)action;
    (void)user_data;
    Rcolor_picker_get_color(sender, &r, &g, &b, &a);
    for (i = 0; i < Rnode_graph_get_selection_count(app.graph); ++i) {
        Rnode_graph_set_node_color(app.graph, Rnode_graph_get_selected_node(app.graph, i), r, g, b);
    }
}

static void action_add(Rid sender, const char* action, void* user_data)
{
    char title[32];
    float x, y, zoom;
    int node;
    Rid knob;
    (void)sender;
    (void)action;
    (void)user_data;
    Rnode_graph_get_view(app.graph, &x, &y, &zoom);
    snprintf(title, sizeof(title), "Remap %d", ++app.added);
    node = Rnode_graph_add_node(app.graph, title, x - 70.0f + (float)(app.added % 5) * 24.0f,
                                y - 50.0f + (float)(app.added % 5) * 24.0f);
    Rnode_graph_add_input(app.graph, node, "Value", "number");
    Rnode_graph_add_output(app.graph, node, "Result", "number");
    Rnode_graph_set_node_color(app.graph, node, 0.39f, 0.82f, 1.0f);
    knob = Rwidget_create(R_WIDGET_SLIDER);
    Rslider_set_value(knob, 0.5f);
    Rwidget_set_size(knob, 150.0f, -1.0f);
    Rnode_graph_set_node_content(app.graph, node, knob);
    Rnode_graph_select_node(app.graph, node, R_FALSE);
    on_selection(app.graph, node, NULL);
}

static void action_frame(Rid sender, const char* action, void* user_data)
{
    (void)sender;
    (void)action;
    (void)user_data;
    Rnode_graph_frame_all(app.graph);
}

static void on_time(Rid timeline, double seconds, void* user_data)
{
    char text[32];
    int whole = (int)seconds;
    int frame = (int)((seconds - (double)whole) * Rtimeline_get_frame_rate(timeline) + 0.5);
    (void)user_data;
    snprintf(text, sizeof(text), "%d:%02d:%02d", whole / 60, whole % 60, frame);
    Rlabel_set_text(app.time, text);
}

static void action_play(Rid sender, const char* action, void* user_data)
{
    int playing = !Rtimeline_is_playing(app.timeline);
    (void)sender;
    (void)action;
    (void)user_data;
    if (playing && Rtimeline_get_time(app.timeline) >= Rtimeline_get_duration(app.timeline)) {
        Rtimeline_set_time(app.timeline, 0.0);
    }
    Rtimeline_set_playing(app.timeline, playing);
    Rbutton_set_text(app.play, playing ? "Pause" : "Play");
}

static void action_theme(Rid sender, const char* action, void* user_data)
{
    static const RTheme themes[] = {R_THEME_DARK, R_THEME_LIGHT, R_THEME_GLASS_DARK, R_THEME_GLASS_LIGHT};
    int index = Rcombo_box_get_selected(sender);
    (void)action;
    (void)user_data;
    if (index >= 0 && index < 4) Rtheme_apply(themes[index]);
}

static void connect_widgets(void)
{
    find_widgets();
    Rnode_graph_set_connect_callback(app.graph, on_connect, NULL);
    Rnode_graph_set_selection_callback(app.graph, on_selection, NULL);
    Rnode_graph_set_removed_callback(app.graph, on_removed, NULL);
    Rtimeline_set_time_callback(app.timeline, on_time, NULL);
    Rtimeline_set_loop(app.timeline, R_TRUE);
    on_time(app.timeline, Rtimeline_get_time(app.timeline), NULL);
}

void raven_application_task(void* user_data)
{
    RHostPlatformDesc host = {0};
    RInitDesc init = {
        .backend = R_BACKEND_CUSTOM,
        .app_name = "Raven Studio - ESP32-S3",
        .worker_threads = -1,
        .disable_render_thread = R_TRUE,
    };
    int previous_pressed = 0;
    QueueHandle_t touch_events = (QueueHandle_t)user_data;

    Rhost_platform_register(&host);
    Rbackend_register(raven_esp_backend());
    Rerror_set_callback(raven_error);
    if (!Rinit_ex(&init)) {
        ESP_LOGE(TAG, "Raven initialization failed");
        vTaskDelete(NULL);
        return;
    }

    Rtheme_apply(R_THEME_DARK);
    /* Studio is rich enough without continuous spring frames on a one-buffer
     * RGB panel. Its graph, picker and timeline remain fully interactive. */
    Rset_animations_enabled(R_FALSE);
    Raction_bind("studio.tint", action_tint, NULL);
    Raction_bind("studio.add", action_add, NULL);
    Raction_bind("studio.frame", action_frame, NULL);
    Raction_bind("studio.play", action_play, NULL);
    Raction_bind("studio.theme", action_theme, NULL);

    app.window = Rwindow_create_from_xml(studio_xml_start, strlen(studio_xml_start));
    if (app.window == R_INVALID_ID) {
        ESP_LOGE(TAG, "unable to load embedded Studio XML");
        Rshutdown();
        vTaskDelete(NULL);
        return;
    }
    Rhost_window_set_size(app.window, RAVEN_DISPLAY_WIDTH, RAVEN_DISPLAY_HEIGHT, 1.0f);
    connect_widgets();
    Rwindow_show(app.window);

    ESP_LOGI(TAG, "Raven Studio is running on CPU1");
    while (Rdraw_frame()) {
        raven_touch_event_t event;
        while (xQueueReceive(touch_events, &event, 0) == pdPASS) {
            if (event.pressed) Rhost_inject_mouse_move(app.window, event.x, event.y, 0);
            if (event.pressed != previous_pressed) {
                Rhost_inject_mouse_button(app.window, event.x, event.y,
                                          R_MOUSE_LEFT, event.pressed, 0);
                previous_pressed = event.pressed;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    Rshutdown();
    vTaskDelete(NULL);
}
