/**
 * @file raven_esp_fs.c
 * @brief Safe no-filesystem implementation of Raven's desktop file services.
 *
 * The functions preserve Raven's API contracts while deliberately refusing
 * persistent desktop file, directory, download, and URL operations.
 */
#include "core/fs.h"
#include "Raven.h"

#include <string.h>

void rvn_dir_free(rvn_dir_entry_t* entries, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i) rvn_free(entries[i].name);
    rvn_free(entries);
}

const char* rvn_path_name(const char* path)
{
    const char* name = path ? path : "";
    const char* cursor;
    for (cursor = name; *cursor; ++cursor) if (*cursor == '/' && cursor[1]) name = cursor + 1;
    return name;
}

char* rvn_path_child(const char* folder, const char* name)
{
    const size_t folder_length = folder ? strlen(folder) : 0;
    const size_t name_length = name ? strlen(name) : 0;
    const int separator = folder_length > 0 && folder[folder_length - 1] != '/';
    char* result = rvn_alloc(folder_length + name_length + (separator ? 2 : 1));
    if (folder_length) memcpy(result, folder, folder_length);
    if (separator) result[folder_length] = '/';
    if (name_length) memcpy(result + folder_length + separator, name, name_length);
    result[folder_length + separator + name_length] = '\0';
    return result;
}

char* rvn_path_parent(const char* path)
{
    size_t length;
    if (!path || !path[0]) return NULL;
    length = strlen(path);
    while (length > 1 && path[length - 1] == '/') --length;
    while (length > 0 && path[length - 1] != '/') --length;
    if (length == 0) return NULL;
    while (length > 1 && path[length - 1] == '/') --length;
    return rvn_strndup(path, length);
}

char* rvn_path_absolute(const char* path)
{
    return rvn_strdup(path ? path : "");
}

char* rvn_path_home(void)
{
    return rvn_strdup("/");
}

int rvn_dir_read(const char* path, rvn_dir_entry_t** entries, uint32_t* count)
{
    (void)path;
    if (entries) *entries = NULL;
    if (count) *count = 0;
    return R_FALSE;
}

int rvn_path_exists(const char* path) { (void)path; return R_FALSE; }
int rvn_path_is_dir(const char* path) { (void)path; return R_FALSE; }
int rvn_file_stamp(const char* path, uint64_t* modified, uint64_t* size)
{
    (void)path;
    if (modified) *modified = 0;
    if (size) *size = 0;
    return R_FALSE;
}
int rvn_make_dir(const char* path) { (void)path; return R_FALSE; }
int rvn_write_file(const char* path, const void* data, size_t size)
{
    (void)path;
    (void)data;
    (void)size;
    return R_FALSE;
}

char* Rfile_read(const char* path, size_t* size)
{
    return rvn_read_file(path, size);
}

int Rfile_write(const char* path, const void* data, size_t size)
{
    return rvn_write_file(path, data, size);
}

int Ropen_url(const char* url)
{
    (void)url;
    return R_FALSE;
}
int rvn_is_http_url(const char* value)
{
    return value && ((rvn_strnicmp(value, "http://", 7) == 0) || (rvn_strnicmp(value, "https://", 8) == 0));
}
int rvn_fs_download_url(const char* url, uint8_t** data, size_t* size)
{
    (void)url;
    if (data) *data = NULL;
    if (size) *size = 0;
    return R_FALSE;
}
void rvn_fs_places(rvn_place_t** places, uint32_t* count)
{
    if (places) *places = NULL;
    if (count) *count = 0;
}
void rvn_fs_places_free(rvn_place_t* places, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        rvn_free(places[i].label);
        rvn_free(places[i].path);
    }
    rvn_free(places);
}

int rvn_pattern_match(const char* patterns, const char* name)
{
    const char* cursor = patterns;
    const size_t name_length = name ? strlen(name) : 0;
    if (!patterns || !patterns[0]) return R_TRUE;
    while (*cursor) {
        const char* end = cursor;
        size_t length;
        while (*end && *end != ';' && *end != ',' && *end != ' ') ++end;
        length = (size_t)(end - cursor);
        if ((length == 1 && cursor[0] == '*') || (length == 3 && memcmp(cursor, "*.*", 3) == 0)) return R_TRUE;
        if (length >= 2 && cursor[0] == '*' && name_length >= length - 1 &&
            rvn_strnicmp(name + name_length - (length - 1), cursor + 1, length - 1) == 0) return R_TRUE;
        if (length == name_length && rvn_strnicmp(cursor, name, length) == 0) return R_TRUE;
        cursor = end;
        while (*cursor == ';' || *cursor == ',' || *cursor == ' ') ++cursor;
    }
    return R_FALSE;
}
