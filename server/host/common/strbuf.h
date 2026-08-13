/* server/host/common/strbuf.h -- a tiny growable string buffer plus a JSON
 * string-literal escaper, factored out of webui.h so that profile_json.h
 * (server-side JSON serialisation for the profile editor, docs/DESIGN.md's
 * remapping-editor task) can build response bodies without either
 * duplicating this code or making profile_json.h depend on webui.h (which
 * would invert the intended layering: webui.h is the HTTP/routing layer and
 * pulls IN profile_json.h, not the other way around).
 *
 * Header-only, same build-script-constraint reason as every other file in
 * this directory (see webui.h's top comment): scripts/build.sh's
 * build_server()/build_windows() name an exact fixed list of .c files, and
 * splitting shared logic across headers rather than one file is
 * organisation, not a workaround -- none of these add a translation unit.
 *
 * No OS dependency at all (unlike sockcompat.h/ipaddr.h next to it): this is
 * pure malloc/realloc/memcpy, identical on every host.
 */
#ifndef ATTICPAD_HOST_COMMON_STRBUF_H
#define ATTICPAD_HOST_COMMON_STRBUF_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} ui_strbuf;

static void sb_init(ui_strbuf *sb)
{
    sb->cap = 512;
    sb->buf = malloc(sb->cap);
    sb->len = 0;
    if (sb->buf != NULL) {
        sb->buf[0] = '\0';
    }
}

static void sb_ensure(ui_strbuf *sb, size_t extra)
{
    if (sb->buf == NULL) {
        return;
    }
    while (sb->len + extra + 1u > sb->cap) {
        size_t ncap = sb->cap * 2u;
        char  *nbuf = realloc(sb->buf, ncap);
        if (nbuf == NULL) {
            return;   /* best effort: further appends below become no-ops */
        }
        sb->buf = nbuf;
        sb->cap = ncap;
    }
}

static void sb_appendn(ui_strbuf *sb, const char *s, size_t n)
{
    sb_ensure(sb, n);
    if (sb->buf == NULL || sb->len + n + 1u > sb->cap) {
        return;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_append(ui_strbuf *sb, const char *s)
{
    sb_appendn(sb, s, strlen(s));
}

static void sb_appendf(ui_strbuf *sb, const char *fmt, ...)
{
    char    tmp[256];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb_appendn(sb, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1u);
    }
}

/* JSON string, quotes included. Escapes the handful of bytes JSON requires
 * and \u-escapes anything below 0x20; does not attempt UTF-8 validation --
 * every string this file's callers emit either came from this process's own
 * text (device names, profile names, log lines already NUL-terminated C
 * strings) or from §2's own text-field decode, which is bounded but not
 * validated as UTF-8 either (atticpad.h apad_text_get's doc comment).
 *
 * A NULL `s` renders as an empty string rather than dereferencing: this is
 * reached from paths fed pointers a HOST supplied (or, for profile_json.h,
 * fields a profile file omitted), and a NULL there should get a
 * wrong-looking response, not a dead server -- see webui.h's original
 * comment on this function (moved here verbatim) for the Windows
 * server_name incident that made this non-hypothetical. */
static void sb_json_string(ui_strbuf *sb, const char *s)
{
    if (s == NULL) {
        sb_append(sb, "\"\"");
        return;
    }
    sb_append(sb, "\"");
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  sb_append(sb, "\\\""); break;
        case '\\': sb_append(sb, "\\\\"); break;
        case '\n': sb_append(sb, "\\n");  break;
        case '\r': sb_append(sb, "\\r");  break;
        case '\t': sb_append(sb, "\\t");  break;
        default:
            if (c < 0x20u) {
                sb_appendf(sb, "\\u%04x", (unsigned)c);
            } else {
                char one[2];
                one[0] = (char)c;
                one[1] = '\0';
                sb_append(sb, one);
            }
        }
    }
    sb_append(sb, "\"");
}

/* Hands ownership of the buffer to the caller (who must free() it) and
 * resets `sb` to the same empty state sb_init() would leave it in were it
 * called again -- NOT the same as sb_free()+sb_init(), which would free the
 * bytes this function exists to hand out. Used by profile_json.h to build a
 * complete JSONC file's text in a ui_strbuf and then return it as a plain
 * malloc'd C string without an extra strdup()/free() round trip (and
 * without depending on strdup()'s mingw feature-test-macro availability --
 * see server/host/windows/main.c's own xstrdup() for why that is a real
 * portability concern in this tree, not a hypothetical one). May return
 * NULL, exactly as `sb->buf` may already be NULL (an earlier allocation
 * failure) -- callers must check, same as any other allocation in this
 * codebase. */
static char *sb_take(ui_strbuf *sb)
{
    char *out = sb->buf;
    sb->buf = NULL;
    sb->len = sb->cap = 0;
    return out;
}

static void sb_free(ui_strbuf *sb)
{
    free(sb->buf);
    sb->buf = NULL;
    sb->len = sb->cap = 0;
}

#endif /* ATTICPAD_HOST_COMMON_STRBUF_H */
