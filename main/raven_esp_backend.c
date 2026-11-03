#include "raven_esp_backend.h"

#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"

#include <math.h>
#include <string.h>

#define RAVEN_ESP_MAX_TEXTURES 16

typedef struct raven_esp_texture_t {
    Rid id;
    RTextureDesc desc;
    uint8_t* pixels;
} raven_esp_texture_t;

static struct {
    esp_lcd_panel_handle_t panel;
    uint16_t* framebuffer;
    raven_esp_texture_t textures[RAVEN_ESP_MAX_TEXTURES];
} s_backend;

static const char* TAG = "raven_backend";

static size_t texture_bytes(const RTextureDesc* desc)
{
    return (size_t)desc->width * desc->height * (desc->format == R_TEXTURE_ALPHA8 ? 1 : 4);
}

static raven_esp_texture_t* find_texture(Rid id)
{
    int i;
    for (i = 0; i < RAVEN_ESP_MAX_TEXTURES; ++i) {
        if (s_backend.textures[i].id == id) return &s_backend.textures[i];
    }
    return NULL;
}

static uint8_t* allocate_texture(size_t size)
{
    uint8_t* pixels = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) ESP_LOGE(TAG, "PSRAM texture allocation failed: %u bytes", (unsigned)size);
    return pixels;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xf8) << 8) | ((uint16_t)(g & 0xfc) << 3) | (b >> 3));
}

static void unpack565(uint16_t color, uint8_t* r, uint8_t* g, uint8_t* b)
{
    *r = (uint8_t)((color >> 8) & 0xf8);
    *g = (uint8_t)((color >> 3) & 0xfc);
    *b = (uint8_t)((color << 3) & 0xf8);
}

static void blend(uint16_t* destination, uint32_t color, uint8_t coverage)
{
    uint8_t dr, dg, db;
    uint8_t sr = (uint8_t)color;
    uint8_t sg = (uint8_t)(color >> 8);
    uint8_t sb = (uint8_t)(color >> 16);
    uint16_t alpha = ((color >> 24) & 0xffu) * coverage / 255u;
    if (alpha == 255) {
        *destination = rgb565(sr, sg, sb);
        return;
    }
    if (alpha == 0) return;
    unpack565(*destination, &dr, &dg, &db);
    sr = (uint8_t)((sr * alpha + dr * (255 - alpha) + 127) / 255);
    sg = (uint8_t)((sg * alpha + dg * (255 - alpha) + 127) / 255);
    sb = (uint8_t)((sb * alpha + db * (255 - alpha) + 127) / 255);
    *destination = rgb565(sr, sg, sb);
}

static int point_in_round_rect(const RDrawInstance* instance, float x, float y, float inset)
{
    float x0 = instance->rect[0] + inset;
    float y0 = instance->rect[1] + inset;
    float x1 = instance->rect[2] - inset;
    float y1 = instance->rect[3] - inset;
    float radius;
    float cx, cy, dx, dy;
    int corner;
    if (x < x0 || x >= x1 || y < y0 || y >= y1) return 0;
    if (x < (x0 + x1) * 0.5f) corner = y < (y0 + y1) * 0.5f ? 0 : 3;
    else corner = y < (y0 + y1) * 0.5f ? 1 : 2;
    radius = instance->radii[corner] - inset;
    if (radius <= 0.0f) return 1;
    cx = (corner == 0 || corner == 3) ? x0 + radius : x1 - radius;
    cy = (corner == 0 || corner == 1) ? y0 + radius : y1 - radius;
    if ((corner == 0 && (x >= cx || y >= cy)) ||
        (corner == 1 && (x <= cx || y >= cy)) ||
        (corner == 2 && (x <= cx || y <= cy)) ||
        (corner == 3 && (x >= cx || y <= cy))) return 1;
    dx = x - cx;
    dy = y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

static uint32_t sample_texture(const raven_esp_texture_t* texture, float u, float v, uint8_t* alpha_only)
{
    uint32_t x, y;
    const uint8_t* source;
    if (!texture || !texture->pixels || texture->desc.width == 0 || texture->desc.height == 0) {
        *alpha_only = 0;
        return 0;
    }
    u = fminf(1.0f, fmaxf(0.0f, u));
    v = fminf(1.0f, fmaxf(0.0f, v));
    x = (uint32_t)(u * (texture->desc.width - 1));
    y = (uint32_t)(v * (texture->desc.height - 1));
    source = texture->pixels + ((size_t)y * texture->desc.width + x) *
        (texture->desc.format == R_TEXTURE_ALPHA8 ? 1 : 4);
    if (texture->desc.format == R_TEXTURE_ALPHA8) {
        *alpha_only = *source;
        return 0xffffffffu;
    }
    *alpha_only = source[3];
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
        ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
}

static void draw_instance(uint16_t* framebuffer, const RDrawData* data,
                          const RDrawInstance* instance, const raven_esp_texture_t* texture)
{
    int x0 = (int)floorf(fmaxf(instance->rect[0], instance->clip[0]));
    int y0 = (int)floorf(fmaxf(instance->rect[1], instance->clip[1]));
    int x1 = (int)ceilf(fminf(instance->rect[2], instance->clip[2]));
    int y1 = (int)ceilf(fminf(instance->rect[3], instance->clip[3]));
    int x, y;
    const uint32_t primitive = instance->flags & R_PRIM_MASK;
    const int textured = (instance->flags & R_INSTANCE_TEXTURED) != 0;
    const float width = instance->rect[2] - instance->rect[0];
    const float height = instance->rect[3] - instance->rect[1];
    const float border = instance->border_width;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)data->framebuffer_width) x1 = (int)data->framebuffer_width;
    if (y1 > (int)data->framebuffer_height) y1 = (int)data->framebuffer_height;
    if (x1 <= x0 || y1 <= y0 || width <= 0 || height <= 0) return;

    for (y = y0; y < y1; ++y) {
        for (x = x0; x < x1; ++x) {
            const float px = (float)x + 0.5f;
            const float py = (float)y + 0.5f;
            uint32_t color = instance->color;
            uint8_t coverage = 255;
            if (primitive == R_PRIM_RECT || primitive == R_PRIM_SHADOW || primitive == R_PRIM_GLASS ||
                primitive == R_PRIM_BLUR || primitive == R_PRIM_SHADER) {
                if (!point_in_round_rect(instance, px, py, 0.0f)) continue;
                if (primitive == R_PRIM_RECT && border > 0.0f && !point_in_round_rect(instance, px, py, border)) {
                    color = instance->border_color;
                }
            }
            if (textured) {
                const float tx = (px - instance->rect[0]) / width;
                const float ty = (py - instance->rect[1]) / height;
                const float u = instance->uv[0] + tx * (instance->uv[2] - instance->uv[0]);
                const float v = instance->uv[1] + ty * (instance->uv[3] - instance->uv[1]);
                uint8_t texture_alpha;
                uint32_t sample = sample_texture(texture, u, v, &texture_alpha);
                if (texture && texture->desc.format == R_TEXTURE_ALPHA8) {
                    coverage = texture_alpha;
                } else {
                    uint8_t r = (uint8_t)color * (uint8_t)sample / 255u;
                    uint8_t g = (uint8_t)(color >> 8) * (uint8_t)(sample >> 8) / 255u;
                    uint8_t b = (uint8_t)(color >> 16) * (uint8_t)(sample >> 16) / 255u;
                    uint8_t a = (uint8_t)(color >> 24) * texture_alpha / 255u;
                    color = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
                }
            }
            blend(&framebuffer[(size_t)y * data->framebuffer_width + x], color, coverage);
        }
    }
}

static int backend_init(const RBackendInitInfo* info)
{
    (void)info;
    return s_backend.panel != NULL;
}

static void backend_shutdown(void)
{
    int i;
    for (i = 0; i < RAVEN_ESP_MAX_TEXTURES; ++i) {
        heap_caps_free(s_backend.textures[i].pixels);
        memset(&s_backend.textures[i], 0, sizeof(s_backend.textures[i]));
    }
}

static int backend_create_surface(Rid window, const RNativeWindow* native, Ruint width, Ruint height)
{
    (void)window;
    (void)native;
    if (width != 800 || height != 480) {
        ESP_LOGE(TAG, "only the 800x480 Elecrow panel is supported (%ux%u requested)", (unsigned)width, (unsigned)height);
        return R_FALSE;
    }
    return R_TRUE;
}

static void backend_destroy_surface(Rid window)
{
    (void)window;
}

static int backend_create_texture(Rid id, const RTextureDesc* desc, const void* pixels)
{
    int i;
    raven_esp_texture_t* texture = find_texture(id);
    if (!texture) {
        for (i = 0; i < RAVEN_ESP_MAX_TEXTURES; ++i) {
            if (s_backend.textures[i].id == 0) {
                texture = &s_backend.textures[i];
                texture->id = id;
                break;
            }
        }
    }
    if (!texture) return R_FALSE;
    heap_caps_free(texture->pixels);
    texture->pixels = allocate_texture(texture_bytes(desc));
    if (!texture->pixels) {
        memset(texture, 0, sizeof(*texture));
        return R_FALSE;
    }
    texture->desc = *desc;
    if (pixels) memcpy(texture->pixels, pixels, texture_bytes(desc));
    else memset(texture->pixels, 0, texture_bytes(desc));
    return R_TRUE;
}

static void backend_update_texture(Rid id, Ruint x, Ruint y, Ruint width, Ruint height, const void* pixels)
{
    Ruint row;
    raven_esp_texture_t* texture = find_texture(id);
    const size_t bytes_per_pixel = texture && texture->desc.format == R_TEXTURE_ALPHA8 ? 1 : 4;
    if (!texture || !pixels || x + width > texture->desc.width || y + height > texture->desc.height) return;
    for (row = 0; row < height; ++row) {
        memcpy(texture->pixels + ((size_t)(y + row) * texture->desc.width + x) * bytes_per_pixel,
               (const uint8_t*)pixels + (size_t)row * width * bytes_per_pixel,
               (size_t)width * bytes_per_pixel);
    }
}

static void backend_set_texture_filter(Rid id, RFilterType filter)
{
    raven_esp_texture_t* texture = find_texture(id);
    if (texture) texture->desc.filter = filter;
}

static void backend_destroy_texture(Rid id)
{
    raven_esp_texture_t* texture = find_texture(id);
    if (!texture) return;
    heap_caps_free(texture->pixels);
    memset(texture, 0, sizeof(*texture));
}

static void backend_render_frame(const RDrawData* data, RCapture* capture)
{
    Ruint i;
    uint16_t* framebuffer;
    uint8_t r = (uint8_t)(fminf(1.0f, fmaxf(0.0f, data->clear_color[0])) * 255.0f);
    uint8_t g = (uint8_t)(fminf(1.0f, fmaxf(0.0f, data->clear_color[1])) * 255.0f);
    uint8_t b = (uint8_t)(fminf(1.0f, fmaxf(0.0f, data->clear_color[2])) * 255.0f);
    if (!s_backend.framebuffer || data->framebuffer_width != 800 || data->framebuffer_height != 480) return;
    framebuffer = s_backend.framebuffer;
    for (i = 0; i < data->framebuffer_width * data->framebuffer_height; ++i) framebuffer[i] = rgb565(r, g, b);
    for (i = 0; i < data->command_count; ++i) {
        const RDrawCommand* command = &data->commands[i];
        const raven_esp_texture_t* texture = find_texture(command->texture);
        Ruint instance;
        if (command->type != R_DRAW_INSTANCES && command->type != R_DRAW_SHADER) continue;
        for (instance = command->first_instance;
             instance < command->first_instance + command->instance_count && instance < data->instance_count;
             ++instance) {
            draw_instance(framebuffer, data, &data->instances[instance], texture);
        }
    }
    if (capture) {
        capture->pixels = NULL;
        capture->width = 0;
        capture->height = 0;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_backend.panel, 0, 0, 800, 480, framebuffer));
}

static void backend_wait_idle(void)
{
}

void raven_esp_backend_set_panel(esp_lcd_panel_handle_t panel)
{
    void* framebuffer0 = NULL;
    s_backend.panel = panel;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &framebuffer0));
    s_backend.framebuffer = framebuffer0;
}

const RBackendVTable* raven_esp_backend(void)
{
    static const RBackendVTable backend = {
        .name = "esp-lcd-rgb565",
        .Init = backend_init,
        .Shutdown = backend_shutdown,
        .CreateSurface = backend_create_surface,
        .DestroySurface = backend_destroy_surface,
        .CreateTexture = backend_create_texture,
        .UpdateTexture = backend_update_texture,
        .SetTextureFilter = backend_set_texture_filter,
        .DestroyTexture = backend_destroy_texture,
        .RenderFrame = backend_render_frame,
        .WaitIdle = backend_wait_idle,
        .CreateShader = NULL,
        .DestroyShader = NULL,
    };
    return &backend;
}
