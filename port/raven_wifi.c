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
#include "freertos/task.h"
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
static raven_wifi_network_t s_networks[RAVEN_WIFI_MAX_NETWORKS];
static int s_network_count;
static int s_scan_in_progress;
static int s_scan_was_connected;
static char s_scan_ip[sizeof(s_status.ip)];
static int s_initialized;
static int s_media_changed;

static void raven_wifi_set_status(raven_wifi_state_t state, const char* ip,
                                  const char* message);

/**
 * @brief Connect to a station profile already retained by the Wi-Fi driver.
 *
 * ESP-IDF stores station profiles in its NVS Wi-Fi namespace when flash
 * storage is selected. Keeping this helper separate from the touchscreen
 * connection path ensures a reboot never needs credentials entered again.
 */
static void raven_wifi_connect_saved_network(void)
{
    wifi_config_t saved_config = {0};

    if (esp_wifi_get_config(WIFI_IF_STA, &saved_config) != ESP_OK ||
        !saved_config.sta.ssid[0]) {
        raven_wifi_set_status(RAVEN_WIFI_IDLE, NULL,
                              "Tap Scan to select a Wi-Fi network.");
        return;
    }

    raven_wifi_set_status(RAVEN_WIFI_CONNECTING, NULL,
                          "Connecting to saved Wi-Fi network...");
    if (esp_wifi_connect() != ESP_OK) {
        raven_wifi_set_status(RAVEN_WIFI_FAILED, NULL,
                              "Could not start saved Wi-Fi connection.");
    }
}

/** A deliberately small single-page browser interface kept in firmware. */
static const char s_web_page[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Raven SD</title><style>body{font:15px system-ui;margin:16px;max-width:760px;color:#17212b}"
    "h1{font-size:20px}button,input{font:inherit;padding:8px;margin:3px}button{cursor:pointer}"
    "li{display:flex;gap:8px;align-items:center;padding:8px;border-bottom:1px solid #ddd}"
    "li span{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.muted{color:#667}.opt{margin:10px 0}.opt label{display:inline-block;margin:4px 12px 4px 0}"
    "canvas{display:block;width:min(500px,100%);height:auto;border:1px solid #bbc;margin:8px 0;background:#0b1020}"
    "#progress{display:flex;align-items:center;gap:8px;min-height:24px;margin:4px 0}#p{width:210px;height:14px}#s{flex:1}</style>"
    "<h1>Raven SD Gallery</h1><p class=muted>Upload gallery BMP files or manage root-level SD files.</p>"
    "<input id=f type=file accept='.bmp,image/bmp'><button id=u onclick=up()>Upload BMP</button>"
    "<p class=muted>Or convert any browser-supported image on this computer, then upload a display-ready 800x480 BMP.</p>"
    "<input id=c type=file accept='image/*'><button id=v onclick=cv()>Convert & Upload</button>"
    "<div class=opt><b>Resize: </b><label><input type=radio name=m value=crop checked>Fill display (center crop)</label><label><input type=radio name=m value=fit>Fit entire image (dark letterbox)</label>"
    "<label>Orientation <select id=r><option value=0>Automatic / 0°</option><option value=90>90° clockwise</option><option value=180>180°</option><option value=270>90° counter-clockwise</option></select></label></div>"
    "<b>800 x 480 output preview</b><canvas id=pr width=500 height=300></canvas><div id=progress><progress id=p max=100 value=0 hidden></progress><span id=s></span></div>"
    "<span id=ps class=muted>Choose an image to preview its exact display result.</span><ul id=l></ul>"
    "<script>const e=document.getElementById('s'),l=document.getElementById('l'),p=document.getElementById('p'),u=document.getElementById('u'),v=document.getElementById('v'),g=document.getElementById('c'),r=document.getElementById('r'),pr=document.getElementById('pr'),ps=document.getElementById('ps');"
    "const esc=x=>x.replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));"
    "async function load(){let r=await fetch('/api/files');if(!r.ok){e.textContent='SD card unavailable';return}let a=await r.json();"
    "l.innerHTML=a.map(x=>`<li><span>${esc(x.name)} <small>${Math.ceil(x.size/1024)} KB</small></span>"
    "<a href='/file/${encodeURIComponent(x.name)}'>Download</a><button onclick=del('${encodeURIComponent(x.name)}')>Delete</button>"
    "<button onclick=cpy('${encodeURIComponent(x.name)}')>Copy</button><button onclick=ren('${encodeURIComponent(x.name)}')>Rename</button></li>`).join('')||'<li>No files found.</li>'}"
    "const frame=()=>new Promise(r=>requestAnimationFrame(r));"
    "const mode=()=>document.querySelector('input[name=m]:checked').value;"
    "async function bitmap(f){if(!window.createImageBitmap)throw Error('This browser cannot convert images locally');return createImageBitmap(f,{imageOrientation:'from-image'})}"
    "function paint(q,w,h,b){let t=+r.value,a=t%180?b.height:b.width,d=t%180?b.width:b.height,k=mode()==='fit'?Math.min(w/a,h/d):Math.max(w/a,h/d);q.fillStyle='#0b1020';q.fillRect(0,0,w,h);q.save();q.translate(w/2,h/2);q.rotate(t*Math.PI/180);q.imageSmoothingQuality='high';q.drawImage(b,-b.width*k/2,-b.height*k/2,b.width*k,b.height*k);q.restore()}"
    "async function preview(){let f=g.files[0];if(!f)return;try{let b=await bitmap(f);paint(pr.getContext('2d'),500,300,b);b.close();ps.textContent='Ready: this is the exact 800 x 480 display result.'}catch(a){ps.textContent='Preview unavailable: '+a.message}}"
    "g.onchange=preview;r.onchange=preview;document.querySelectorAll('input[name=m]').forEach(x=>x.onchange=preview);"
    "function put(b,n,done){return new Promise((ok,no)=>{let x=new XMLHttpRequest();p.hidden=false;p.value=0;e.textContent='Uploading BMP: 0%';"
    "x.upload.onprogress=a=>{if(a.lengthComputable){p.value=100*a.loaded/a.total;e.textContent='Uploading BMP: '+Math.round(p.value)+'%'}};"
    "x.onload=()=>{if(x.status>=200&&x.status<300){p.value=100;e.textContent=done;load();ok()}else no(Error(x.responseText||'server returned '+x.status))};"
    "x.onerror=()=>no(Error('network error during upload'));x.onabort=()=>no(Error('upload cancelled'));"
    "x.open('PUT','/upload?name='+encodeURIComponent(n));x.send(b)})}"
    "async function up(){let f=document.getElementById('f').files[0];if(!f){e.textContent='Choose a BMP first.';return}u.disabled=true;"
    "try{await put(f,f.name,'Upload complete: '+f.name+' is ready in the gallery.')}catch(a){e.textContent='Failed: '+a.message}u.disabled=false}"
    "async function mkbmp(f){"
    "p.hidden=false;p.value=0;e.textContent='Converting image on this computer: 0%';await frame();"
    "let b=await bitmap(f),c=document.createElement('canvas'),q=c.getContext('2d');c.width=800;c.height=480;paint(q,800,480,b);b.close();"
    "let a=q.getImageData(0,0,800,480).data,o=new Uint8Array(1152054),d=new DataView(o.buffer);d.setUint16(0,19778,true);d.setUint32(2,1152054,true);d.setUint32(10,54,true);d.setUint32(14,40,true);d.setInt32(18,800,true);d.setInt32(22,480,true);d.setUint16(26,1,true);d.setUint16(28,24,true);d.setUint32(34,1152000,true);"
    "for(let y=0;y<480;y++){let z=54+(479-y)*2400;for(let x=0;x<800;x++){let i=(y*800+x)*4,j=z+x*3;o[j]=a[i+2];o[j+1]=a[i+1];o[j+2]=a[i]}if(!(y%48)){p.value=Math.round((y+1)*100/480);e.textContent='Converting image on this computer: '+p.value+'%';await frame()}}return new Blob([o],{type:'image/bmp'})}"
    "async function cv(){let f=g.files[0];if(!f){e.textContent='Choose an image first.';return}v.disabled=true;"
    "try{let b=await mkbmp(f),n=(f.name.replace(/\\.[^.]*$/,'')||'converted')+'.bmp';e.textContent='Conversion complete. Uploading BMP: 0%';await put(b,n,'Upload complete: '+n+' is ready in the gallery.')}catch(a){e.textContent='Failed: '+a.message}v.disabled=false}"
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

/** @brief Scan on a worker task so touchscreen rendering is never blocked. */
static void raven_wifi_scan_task(void* user_data)
{
    static wifi_ap_record_t records[RAVEN_WIFI_MAX_NETWORKS];
    wifi_scan_config_t config = { .show_hidden = false };
    uint16_t available = RAVEN_WIFI_MAX_NETWORKS;
    esp_err_t result;
    int count = 0;
    int was_connected;
    char ip[sizeof(s_status.ip)];
    int index;

    (void)user_data;
    result = esp_wifi_scan_start(&config, true);
    if (result == ESP_OK) result = esp_wifi_scan_get_ap_records(&available, records);
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    was_connected = s_scan_was_connected;
    snprintf(ip, sizeof(ip), "%s", s_scan_ip);
    if (result == ESP_OK) {
        for (index = 0; index < available && count < RAVEN_WIFI_MAX_NETWORKS; ++index) {
            int duplicate = 0;
            int prior;
            if (!records[index].ssid[0]) continue;
            for (prior = 0; prior < count; ++prior) {
                if (strcmp(s_networks[prior].ssid, (const char*)records[index].ssid) == 0) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) continue;
            snprintf(s_networks[count].ssid, sizeof(s_networks[count].ssid), "%s", records[index].ssid);
            s_networks[count].rssi = records[index].rssi;
            ++count;
        }
        s_network_count = count;
    }
    s_scan_in_progress = 0;
    xSemaphoreGive(s_status_lock);
    if (result == ESP_OK) {
        char message[96];
        if (was_connected) {
            snprintf(message, sizeof(message), "%d networks found. Open http://%s on your computer.", count, ip);
            raven_wifi_set_status(RAVEN_WIFI_CONNECTED, ip, message);
        } else {
            snprintf(message, sizeof(message), "%d networks found. Select one, then enter its password.", count);
            raven_wifi_set_status(RAVEN_WIFI_IDLE, NULL, message);
        }
    } else {
        raven_wifi_set_status(RAVEN_WIFI_FAILED, NULL, "Wi-Fi scan failed. Tap Scan again.");
    }
    vTaskDelete(NULL);
}

/**
 * @brief Validate and URL-decode a normal root-level FAT filename.
 *
 * Browsers preserve spaces, parentheses, and apostrophes in a selected file's
 * name (or encode them in the upload URL).  These are valid FAT long-file-name
 * characters, so accept them while still rejecting paths, control characters,
 * and shell-like punctuation that is not needed by this LAN file manager.
 */
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
        if (!(isalnum(value) || value == ' ' || value == '.' || value == '_' ||
              value == '-' || value == '(' || value == ')' || value == '[' ||
              value == ']' || value == '\'' || value == '!' || value == '~')) return 0;
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
    /* FATFS treats dot-prefixed names specially; retain a valid 8.3 temp file. */
    snprintf(temporary, sizeof(temporary), "%s/RAVENUPL.TMP", RAVEN_SD_MOUNT_POINT);
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
    /* Persist the latest touchscreen selection rather than losing it at reset. */
    if (esp_wifi_set_storage(WIFI_STORAGE_FLASH) != ESP_OK) return 0;
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) return 0;
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_initialized = 1;
    ESP_LOGI(TAG, "Wi-Fi station ready");
    raven_wifi_connect_saved_network();
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

int raven_wifi_start_scan(void)
{
    int started;

    if (!s_initialized) return 0;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    started = !s_scan_in_progress;
    if (started) {
        s_scan_in_progress = 1;
        s_scan_was_connected = s_status.state == RAVEN_WIFI_CONNECTED;
        snprintf(s_scan_ip, sizeof(s_scan_ip), "%s", s_status.ip);
    }
    xSemaphoreGive(s_status_lock);
    if (!started) return 0;
    raven_wifi_set_status(RAVEN_WIFI_SCANNING, s_scan_was_connected ? s_scan_ip : NULL,
                          "Scanning for nearby Wi-Fi networks...");
    if (xTaskCreate(raven_wifi_scan_task, "wifi_scan", 4096, NULL, 3, NULL) != pdPASS) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        s_scan_in_progress = 0;
        xSemaphoreGive(s_status_lock);
        raven_wifi_set_status(RAVEN_WIFI_FAILED, NULL, "Could not start Wi-Fi scan.");
        return 0;
    }
    return 1;
}

int raven_wifi_get_networks(raven_wifi_network_t* networks, int capacity)
{
    int count;

    if (!networks || capacity <= 0 || !s_status_lock) return 0;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    count = s_network_count < capacity ? s_network_count : capacity;
    memcpy(networks, s_networks, (size_t)count * sizeof(*networks));
    xSemaphoreGive(s_status_lock);
    return count;
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
