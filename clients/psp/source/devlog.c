#if defined(APAD_PSP_DEVLOG) || defined(APAD_PSP_STDOUT)
#include <stdarg.h>
#include <stdio.h>

#include "devlog.h"

#ifdef APAD_PSP_DEVLOG
static char g_log_path[64];
#endif

void apad_devlog_open(const char *path)
{
#ifdef APAD_PSP_DEVLOG
    FILE *f;
    /* Truncate once at open, then never hold the handle: each apad_devlog()
     * reopens in append mode and closes again. Holding it open leaves the
     * memory stick's FAT directory size stale until fclose, so a host reading
     * the card over USB sees only the first sync -- which looked exactly like
     * the app stalling after three lines. Open/close per line keeps the
     * on-disk size current at the cost of speed, which a debug log can pay. */
    unsigned n = 0;
    while (path[n] != '\0' && n < sizeof g_log_path - 1) { g_log_path[n] = path[n]; n++; }
    g_log_path[n] = '\0';
    f = fopen(g_log_path, "w");
    if (f != NULL) { fclose(f); }
#else
    (void)path;
#endif
}

void apad_devlog(const char *fmt, ...)
{
    va_list ap;

#ifdef APAD_PSP_DEVLOG
    if (g_log_path[0] != '\0') {
        FILE *f = fopen(g_log_path, "a");
        if (f != NULL) {
            va_start(ap, fmt);
            vfprintf(f, fmt, ap);
            va_end(ap);
            fputc('\n', f);
            fclose(f);   /* close per line: updates the FAT size every write */
        }
    }
#endif
#ifdef APAD_PSP_STDOUT
    /* Prefixed so a line is obvious in the pspsh console among PSPLINK's own
     * chatter, and flushed per line so a stall shows exactly where. PSPLINK
     * relays this over USB, so it survives the Wi-Fi drop we are chasing. */
    va_start(ap, fmt);
    fputs("[apad] ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
#endif
}
#else
typedef int apad_devlog_translation_unit_not_empty;
#endif
