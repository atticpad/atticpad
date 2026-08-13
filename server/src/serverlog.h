/* server/src/serverlog.h — library-internal diagnostics plumbing.
 *
 * Not part of libapadserver's public surface (that is
 * server/include/apadserver.h). Exists so profiles.c can report a bad
 * profile the same way server.c reports a session teardown: formatted into
 * a stack buffer inside the library, handed to the host's sink as one
 * finished line. The library never picks a stream -- docs/DESIGN.md §6.4, "the
 * logging sink" is the host's.
 */
#ifndef ATTICPAD_SERVER_SERVERLOG_H
#define ATTICPAD_SERVER_SERVERLOG_H

#include "apadserver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A resolved sink: the host's callback plus the host's opaque pointer.
 * Passing this around by pointer (rather than reaching for a file-static)
 * keeps two servers in one process from stealing each other's logging. */
typedef struct {
    apad_server_log_fn fn;
    void              *user;
} apad_log_sink;

/* The longest line any caller produces is a profile diagnostic carrying a
 * path (up to the loader's 512-byte path buffer) plus ~200 bytes of
 * explanation. Anything longer is truncated rather than split: a truncated
 * diagnostic is a cosmetic loss, a heap allocation on a logging path is a
 * failure mode. */
#define APAD_LOG_LINE_MAX 768

/* Format and emit one line. `sink` may be NULL, and sink->fn may be NULL;
 * both discard. Defined in server.c. */
void apad_logf(const apad_log_sink *sink, apad_log_level level,
               const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_SERVER_SERVERLOG_H */
