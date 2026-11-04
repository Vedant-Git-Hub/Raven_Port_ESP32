/**
 * @file raven_esp_dialogs.c
 * @brief Safe stubs for Raven's native desktop dialog services.
 *
 * ESP-IDF has no native process/file dialog facility.  Keep Raven's normal
 * shutdown path intact while deliberately omitting the desktop dialog API
 * implementation from this first MCU port.
 */
#include "widgets/common.h"
#include "Raven.h"

void rvn_dialogs_shutdown(void)
{
}

RMessageResult Rmessage_box(Rid window, const char* title, const char* text, RMessageButtons buttons)
{
    (void)window;
    (void)title;
    (void)text;
    (void)buttons;
    return R_MESSAGE_RESULT_CANCEL;
}
