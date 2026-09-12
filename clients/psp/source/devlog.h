/*
 * clients/psp/source/devlog.h -- dev-only diagnostic log, two sinks.
 *
 * FILE sink (APAD_PSP_DEVLOG): a line-buffered log to ms0:/atticpad.log, the
 * channel that makes a headless run legible with no screen. STDOUT sink
 * (APAD_PSP_STDOUT): the same lines to stdout, which PSPLINK relays to pspsh
 * over USB live -- a transport independent of Wi-Fi, so it keeps printing
 * through the exact association drop that kills a network log. Either flag
 * (or both) compiles the functions in; neither is ever in a shipped build.
 */
#ifndef ATTICPAD_PSP_DEVLOG_H
#define ATTICPAD_PSP_DEVLOG_H

#if defined(APAD_PSP_DEVLOG) || defined(APAD_PSP_STDOUT)
void apad_devlog_open(const char *path);
void apad_devlog(const char *fmt, ...);
#else
#define apad_devlog_open(p)  ((void)0)
#define apad_devlog(...)     ((void)0)
#endif

#endif /* ATTICPAD_PSP_DEVLOG_H */
