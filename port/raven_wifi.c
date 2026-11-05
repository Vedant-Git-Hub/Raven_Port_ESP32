/**
 * @file raven_wifi.c
 * @brief ESP-IDF Wi-Fi station and compact HTTP SD-card file manager.
 *
 * The browser speaks only to a LAN-local HTTP server. Files are limited to
 * safe root-level names and uploads are first written to a temporary file so
 * an interrupted transfer cannot leave a partial gallery image behind.
 */
#include "raven_wifi.h"

#include "raven_sd.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RAVEN_WIFI_MAX_UPLOAD_BYTES (4U * 1024U * 1024U)
#define RAVEN_WIFI_NAME_SIZE 80

static const char* TAG = "raven_wifi";
static SemaphoreHandle_t s_status_lock;
static httpd_handle_t s_http_server;
static esp_netif_t* s_station_netif;
static raven_wifi_status_t s_status = {
    .state = RAVEN_WIFI_IDLE,
    .message = "Enter your Wi-Fi name and password.",
};
static int s_initialized;
static int s_media_changed;

/** A deliberately small single-page browser interface kept in firmware. */
static const char s_web_page[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Raven SD</title><style>body{font:15px system-ui;margin:16px;max-width:760px;color:#17212b}"
    "h1{font-size:20px}button,input{font:inherit;padding:8px;margin:3px}button{cursor:pointer}"
    "li{display:flex;gap:8px;align-items:center;padding:8px;border-bottom:1px solid #ddd}"
    "li span{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.muted{color:#667}</style>"
    "<h1>Raven SD Gallery</h1><p class=muted>Upload BMP images or manage root-level SD files.</p>"
    "<input id=f type=file><button onclick=up()>Upload</button><span id=s></span><ul id=l></ul>"
    "<script>const e=document.getElementById('s'),l=document.getElementById('l');"
    "const esc=x=>x.replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));"
    "async function load(){let r=await fetch('/api/files');if(!r.ok){e.textContent='SD card unavailable';return}let a=await r.json();"
    "l.innerHTML=a.map(x=>`<li><span>${esc(x.name)} <small>${Math.ceil(x.size/1024)} KB</small></span>"
    "+`<a href='/file/${encodeURIComponent(x.name)}'>Download</a><button onclick=del('${encodeURIComponent(x.name)}')>Delete</button>"
    "+`<button onclick=cpy('${encodeURIComponent(x.name)}')>Copy</button><button onclick=ren('${encodeURIComponent(x.name)}')>Rename</button></li>`).join('')||'<li>No files found.</li>'}"
    "async function up(){let f=document.getElementById('f').files[0];if(!f)return;e.textContent='Uploading…';"
    "let r=await fetch('/upload?name='+encodeURIComponent(f.name),{method:'PUT',body:f});e.textContent=await r.text();load()}"
    "async function del(n){if(!confirm('Delete this file?'))return;let r=await fetch('/delete?name='+n,{method:'DELETE'});e.textContent=await r.text();load()}"
    "async function cpy(n){let v=prompt('Copy to filename:',decodeURIComponent(n));if(!v)return;let r=await fetch('/copy?from='+n+'&to='+encodeURIComponent(v),{method:'POST'});e.textContent=await r.text();load()}"
    "async function ren(n){let v=prompt('New filename:',decodeURIComponent(n));if(!v)return;let r=await fetch('/rename?from='+n+'&to='+encodeURIComponent(v),{method:'POST'});e.textContent=await r.text();load()}load();</script>";

/** @brief Update the station snapshot from an ESP-IDF event context. */
static void raven_wifi_set_status(raven_wifi_state_t state, const char* ip, const char* message)
{
    if (!s_status_lock) return;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_status.state = state;
    snprintf(s_status.ip, sizeof(s_status.ip), "%s", ip ? ip : "");
    snprintf(s_status.message, sizeof(s_status.message), "%s", message ? message : "");
    xSemaphoreGive(s_status_lock);
}

/** @brief Tell the Raven UI that browser operations changed the card root. */
static void raven_wifi_mark_media_changed(void)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    s_media_changed = 1;
    xSemaphoreGive(s_status_lock);
}

/** @brief Produce a conservative, root-level FAT filename from URL input. */
static int raven_wifi_decode_name(const char* encoded, char output[RAVEN_WIFI_NAME_SIZE])
{
    size_t in = 0;
    size_t out = 0;

    if (!encoded || !*encoded) return 0;
    while (encoded[in] && out + 1U < RAVEN_WIFI_NAME_SIZE) {
        unsigned char value = (unsigned char)encoded[in++];
        if (value == '%' && isxdigit((unsigned char)encoded[in]) &&
            isxdigit((unsigned char)encoded[in + 1U])) {
            int high = isdigit((unsigned char)encoded[in]) ? encoded[in] - '0' :
                       (tolower((unsigned char)encoded[in]) - 'a' + 10);
            int low = isdigit((unsigned char)encoded[in + 1U]) ? encoded[in + 1U] - '0' :
                      (tolower((unsigned char)encoded[in + 1U]) - 'a' + 10);
            value = (unsigned char)((high << 4) | low);
            in += 2U;
        }
        if (!(isalnum(value) || value == '.' || value == '_' || value == '-')) return 0;
        output[out++] = (char)value;
    }
    if (encoded[in] || out == 0U || output[0] == '.') return 0;
    output[out] = '\0';
    return 1;
}

/** @brief Read and validate a named URL query parameter. */
static int raven_wifi_query_name(httpd_req_t* request, const char* key,
                                 char output[RAVEN_WIFI_NAME_SIZE])
{
    char query[192];
    char encoded[160];
    size_t length = httpd_req_get_url_query_len(request);

    if (length == 0U || length >= sizeof(query) ||
        httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, encoded, sizeof(encoded)) != ESP_OK) return 0;
    return raven_wifi_decode_name(encoded, output);
}

/** @brief Format a regular root-level SD filename as a safe JSON string body. */
static void raven_wifi_json_escape(const char* input, char* output, size_t output_size)
{
    size_t out = 0;
    while (*input && out + 2U < output_size) {
        unsigned char c = (unsigned char)*input++;
        if (c == '"' || c == '\\') {
            output[out++] = '\\';
            output[out++] = (char)c;
        } else if (c >= 0x20U) {
            output[out++] = (char)c;
        }
    }
    output[out] = '\0';
}

/** @brief Return the compact management page. */
static esp_err_t raven_http_root(httpd_req_t* request)
{
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, s_web_page, HTTPD_RESP_USE_STRLEN);
}

/** @brief Return all regular root-level SD files as a minimal JSON list. */
static esp_err_t raven_http_list(httpd_req_t* request)
{
    DIR* directory;
    struct dirent* entry;
    int first = 1;

    if (!raven_sd_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    if (!raven_sd_is_mounted()) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    directory = opendir(RAVEN_SD_MOUNT_POINT);
    if (!directory) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_send_chunk(request, "[", 1);
    while ((entry = readdir(directory)) != NULL) {
        char path[RAVEN_SD_PATH_SIZE];
        char escaped[RAVEN_SD_NAME_SIZE * 2U];
        char item[256];
        struct stat info;
        if (entry->d_name[0] == '.' ||
            snprintf(path, sizeof(path), "%s/%s", RAVEN_SD_MOUNT_POINT, entry->d_name) >= (int)sizeof(path) ||
            stat(path, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        raven_wifi_json_escape(entry->d_name, escaped, sizeof(escaped));
        snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%lu}",
                 first ? "" : ",", escaped, (unsigned long)info.st_size);
        if (httpd_resp_send_chunk(request, item, HTTPD_RESP_USE_STRLEN) != ESP_OK) break;
        first = 0;
    }
    closedir(directory);
    raven_sd_unlock();
    return httpd_resp_send_chunk(request, "]", 1) == ESP_OK ?
           httpd_resp_send_chunk(request, NULL, 0) : ESP_FAIL;
}

/** @brief Stream a browser PUT body to SD and atomically replace its target. */
static esp_err_t raven_http_upload(httpd_req_t* request)
{
    char name[RAVEN_WIFI_NAME_SIZE];
    char path[RAVEN_SD_PATH_SIZE];
    char temporary[RAVEN_SD_PATH_SIZE];
    char buffer[1024];
    FILE* file;
    int remaining;

    if (!raven_wifi_query_name(request, "name", name))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Use a simple root-level filename");
    if (request->content_len <= 0 || request->content_len > (int)RAVEN_WIFI_MAX_UPLOAD_BYTES)
        return httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE, "Upload must be 1 to 4 MB");
    if (!raven_sd_lock(pdMS_TO_TICKS(5000))) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    if (!raven_sd_is_mounted()) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    snprintf(path, sizeof(path), "%s/%s", RAVEN_SD_MOUNT_POINT, name);
    snprintf(temporary, sizeof(temporary), "%s/.raven-upload.tmp", RAVEN_SD_MOUNT_POINT);
    unlink(temporary);
    file = fopen(temporary, "wb");
    if (!file) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not create SD file");
    }
    remaining = request->content_len;
    while (remaining > 0) {
        int wanted = remaining > (int)sizeof(buffer) ? (int)sizeof(buffer) : remaining;
        int received = httpd_req_recv(request, buffer, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (received <= 0 || fwrite(buffer, 1, (size_t)received, file) != (size_t)received) {
            fclose(file);
            unlink(temporary);
            raven_sd_unlock();
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
        }
        remaining -= received;
    }
    fclose(file);
    unlink(path);
    if (rename(temporary, path) != 0) {
        unlink(temporary);
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not finalize upload");
    }
    raven_sd_unlock();
    raven_wifi_mark_media_changed();
    ESP_LOGI(TAG, "uploaded %s", name);
    return httpd_resp_sendstr(request, "Upload complete");
}

/** @brief Stream one named SD file to a browser download. */
static esp_err_t raven_http_download(httpd_req_t* request)
{
    char name[RAVEN_WIFI_NAME_SIZE];
    char path[RAVEN_SD_PATH_SIZE];
    char disposition[112];
    char buffer[1024];
    FILE* file;
    size_t read_size;
    const char* encoded_name = request->uri + strlen("/file/");

    if (!raven_wifi_decode_name(encoded_name, name))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid filename");
    if (!raven_sd_lock(pdMS_TO_TICKS(5000))) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    if (!raven_sd_is_mounted()) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    snprintf(path, sizeof(path), "%s/%s", RAVEN_SD_MOUNT_POINT, name);
    file = fopen(path, "rb");
    if (!file) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "File not found");
    }
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Content-Disposition", disposition);
    while ((read_size = fread(buffer, 1, sizeof(buffer), file)) > 0U) {
        if (httpd_resp_send_chunk(request, buffer, read_size) != ESP_OK) break;
    }
    fclose(file);
    raven_sd_unlock();
    return httpd_resp_send_chunk(request, NULL, 0);
}

/** @brief Delete a selected regular file from the SD root. */
static esp_err_t raven_http_delete(httpd_req_t* request)
{
    char name[RAVEN_WIFI_NAME_SIZE];
    char path[RAVEN_SD_PATH_SIZE];

    if (!raven_wifi_query_name(request, "name", name))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid filename");
    if (!raven_sd_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    if (!raven_sd_is_mounted()) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    snprintf(path, sizeof(path), "%s/%s", RAVEN_SD_MOUNT_POINT, name);
    if (unlink(path) != 0) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "File not found");
    }
    raven_sd_unlock();
    raven_wifi_mark_media_changed();
    ESP_LOGI(TAG, "deleted %s", name);
    return httpd_resp_sendstr(request, "Deleted");
}

/** @brief Copy a regular root-level SD file through a temporary destination. */
static esp_err_t raven_http_copy(httpd_req_t* request)
{
    char from[RAVEN_WIFI_NAME_SIZE];
    char to[RAVEN_WIFI_NAME_SIZE];
    char source_path[RAVEN_SD_PATH_SIZE];
    char target_path[RAVEN_SD_PATH_SIZE];
    char temporary[RAVEN_SD_PATH_SIZE];
    char buffer[1024];
    FILE* source;
    FILE* target;
    size_t read_size;
    int copied;

    if (!raven_wifi_query_name(request, "from", from) ||
        !raven_wifi_query_name(request, "to", to) || strcmp(from, to) == 0)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Choose two different valid filenames");
    if (!raven_sd_lock(pdMS_TO_TICKS(5000)))
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    if (!raven_sd_is_mounted()) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    snprintf(source_path, sizeof(source_path), "%s/%s", RAVEN_SD_MOUNT_POINT, from);
    snprintf(target_path, sizeof(target_path), "%s/%s", RAVEN_SD_MOUNT_POINT, to);
    snprintf(temporary, sizeof(temporary), "%s/.raven-copy.tmp", RAVEN_SD_MOUNT_POINT);
    if (access(target_path, F_OK) == 0 || !(source = fopen(source_path, "rb"))) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Source missing or destination exists");
    }
    unlink(temporary);
    target = fopen(temporary, "wb");
    if (!target) {
        fclose(source);
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not create copy");
    }
    while ((read_size = fread(buffer, 1, sizeof(buffer), source)) > 0U &&
           fwrite(buffer, 1, read_size, target) == read_size) {}
    copied = !ferror(source) && !ferror(target);
    fclose(source);
    fclose(target);
    if (!copied || rename(temporary, target_path) != 0) {
        unlink(temporary);
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Copy failed");
    }
    raven_sd_unlock();
    raven_wifi_mark_media_changed();
    ESP_LOGI(TAG, "copied %s to %s", from, to);
    return httpd_resp_sendstr(request, "Copied");
}

/** @brief Rename a root-level SD file without copying its contents. */
static esp_err_t raven_http_rename(httpd_req_t* request)
{
    char from[RAVEN_WIFI_NAME_SIZE];
    char to[RAVEN_WIFI_NAME_SIZE];
    char old_path[RAVEN_SD_PATH_SIZE];
    char new_path[RAVEN_SD_PATH_SIZE];

    if (!raven_wifi_query_name(request, "from", from) ||
        !raven_wifi_query_name(request, "to", to))
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid filename");
    if (!raven_sd_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    if (!raven_sd_is_mounted()) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "SD card unavailable");
    }
    snprintf(old_path, sizeof(old_path), "%s/%s", RAVEN_SD_MOUNT_POINT, from);
    snprintf(new_path, sizeof(new_path), "%s/%s", RAVEN_SD_MOUNT_POINT, to);
    if (rename(old_path, new_path) != 0) {
        raven_sd_unlock();
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Rename failed; target may already exist");
    }
    raven_sd_unlock();
    raven_wifi_mark_media_changed();
    ESP_LOGI(TAG, "renamed %s to %s", from, to);
    return httpd_resp_sendstr(request, "Renamed");
}

/** @brief Start the compact LAN-only HTTP server after DHCP succeeds. */
static void raven_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = raven_http_root };
    const httpd_uri_t list = { .uri = "/api/files", .method = HTTP_GET, .handler = raven_http_list };
    const httpd_uri_t upload = { .uri = "/upload", .method = HTTP_PUT, .handler = raven_http_upload };
    const httpd_uri_t download = { .uri = "/file/*", .method = HTTP_GET, .handler = raven_http_download };
    const httpd_uri_t delete_file = { .uri = "/delete", .method = HTTP_DELETE, .handler = raven_http_delete };
    const httpd_uri_t copy_file = { .uri = "/copy", .method = HTTP_POST, .handler = raven_http_copy };
    const httpd_uri_t rename_file = { .uri = "/rename", .method = HTTP_POST, .handler = raven_http_rename };

    if (s_http_server) return;
    config.max_uri_handlers = 7;
    config.stack_size = 6144;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "could not start HTTP server");
        return;
    }
    httpd_register_uri_handler(s_http_server, &root);
    httpd_register_uri_handler(s_http_server, &list);
    httpd_register_uri_handler(s_http_server, &upload);
    httpd_register_uri_handler(s_http_server, &download);
    httpd_register_uri_handler(s_http_server, &delete_file);
    httpd_register_uri_handler(s_http_server, &copy_file);
    httpd_register_uri_handler(s_http_server, &rename_file);
    ESP_LOGI(TAG, "SD manager started on port 80");
}

/** @brief Translate Wi-Fi and DHCP notifications into UI-visible state. */
static void raven_wifi_event(void* argument, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = event_data;
        char message[96];
        snprintf(message, sizeof(message), "Connection failed (reason %d). Check credentials.",
                 disconnected ? disconnected->reason : -1);
        raven_wifi_set_status(RAVEN_WIFI_FAILED, NULL, message);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* got_ip = event_data;
        char ip[16];
        char message[96];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&got_ip->ip_info.ip));
        snprintf(message, sizeof(message), "Connected. Open http://%s on your computer.", ip);
        raven_http_start();
        raven_wifi_set_status(RAVEN_WIFI_CONNECTED, ip, message);
        ESP_LOGI(TAG, "%s", message);
    }
}

int raven_wifi_init(void)
{
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t result;

    if (s_initialized) return 1;
    s_status_lock = xSemaphoreCreateMutex();
    if (!s_status_lock) return 0;
    result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        result = nvs_flash_init();
    }
    if (result != ESP_OK) return 0;
    if ((result = esp_netif_init()) != ESP_OK && result != ESP_ERR_INVALID_STATE) return 0;
    if ((result = esp_event_loop_create_default()) != ESP_OK && result != ESP_ERR_INVALID_STATE) return 0;
    s_station_netif = esp_netif_create_default_wifi_sta();
    if (!s_station_netif || esp_wifi_init(&config) != ESP_OK) return 0;
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, raven_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, raven_wifi_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) return 0;
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_initialized = 1;
    ESP_LOGI(TAG, "Wi-Fi station ready");
    return 1;
}

int raven_wifi_connect(const char* ssid, const char* password)
{
    wifi_config_t config = {0};
    size_t ssid_length = ssid ? strlen(ssid) : 0U;
    size_t password_length = password ? strlen(password) : 0U;

    if (!s_initialized || ssid_length == 0U || ssid_length > 32U || password_length > 63U) return 0;
    memcpy(config.sta.ssid, ssid, ssid_length);
    memcpy(config.sta.password, password, password_length);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    raven_wifi_set_status(RAVEN_WIFI_CONNECTING, NULL, "Connecting to Wi-Fi...");
    esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        raven_wifi_set_status(RAVEN_WIFI_FAILED, NULL, "Could not start Wi-Fi connection.");
        return 0;
    }
    return 1;
}

void raven_wifi_get_status(raven_wifi_status_t* status)
{
    if (!status) return;
    if (!s_status_lock) {
        *status = s_status;
        return;
    }
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_status_lock);
}

int raven_wifi_take_media_changed(void)
{
    int changed;
    if (!s_status_lock) return 0;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    changed = s_media_changed;
    s_media_changed = 0;
    xSemaphoreGive(s_status_lock);
    return changed;
}
