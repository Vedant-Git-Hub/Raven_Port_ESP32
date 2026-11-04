/**
 * @file raven_esp_fs_watch.c
 * @brief No-op filesystem watcher for immutable embedded firmware resources.
 */
#include "core/fs_watch.h"

/* Embedded resources are immutable while the firmware runs. */
int rvn_fs_watch_start(rvn_fs_watch_callback_t callback, void* user)
{
    (void)callback;
    (void)user;
    return R_FALSE;
}

int rvn_fs_watch_add(const char* path)
{
    (void)path;
    return R_FALSE;
}

void rvn_fs_watch_stop(void)
{
}
