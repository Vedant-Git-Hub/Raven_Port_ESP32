/**
 * @file raven_esp_font_system.c
 * @brief Disable desktop font discovery on ESP-IDF.
 */
#include "render/font.h"

/* ESP-IDF has no desktop font directories. Applications can still provide a
   deliberate fallback through Rfont_create and Rfont_set_fallback. */
int rvn_font_system_candidates(uint32_t codepoint, const char** paths, int max)
{
    (void)codepoint;
    (void)paths;
    (void)max;
    return 0;
}

void rvn_font_system_shutdown(void)
{
}
