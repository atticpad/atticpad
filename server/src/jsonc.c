/* server/src/jsonc.c — implementation of the JSONC parser declared in
 * jsonc.h. See that file's header comment for why this is a hand-written
 * DOM parser rather than a literal jsmn port. Its own translation unit,
 * linked by scripts/build.sh's build_server() alongside mapping.c and
 * profiles.c.
 */
#include "jsonc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- comment stripping (D6: "a ~30-line pre-pass") --------------------- */

/*
 * Strips line comments and block comments from `in`, respecting string
 * literals (a comment-opening sequence inside a quoted JSON string is just
 * text, not a comment) and their backslash escapes, so a profile that
 * legitimately contains those characters in a button label or region name
 * is not corrupted. Returns a freshly malloc'd, NUL-terminated buffer;
 * NULL only on allocation failure. The output can be shorter than the
 * input (comments are dropped, not blanked), which is fine -- nothing
 * downstream depends on byte offsets matching the original file.
 */
static char *strip_comments(const char *in, size_t len)
{
    enum { ST_NORMAL, ST_STRING, ST_STRING_ESC, ST_LINE_COMMENT,
           ST_BLOCK_COMMENT, ST_BLOCK_COMMENT_STAR } state = ST_NORMAL;
    char *out = malloc(len + 1);
    size_t o = 0, i;

    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < len; i++) {
        char c = in[i];

        switch (state) {
        case ST_NORMAL:
            if (c == '"') {
                state = ST_STRING;
                out[o++] = c;
            } else if (c == '/' && i + 1 < len && in[i + 1] == '/') {
                state = ST_LINE_COMMENT;
                i++;
            } else if (c == '/' && i + 1 < len && in[i + 1] == '*') {
                state = ST_BLOCK_COMMENT;
                i++;
            } else {
                out[o++] = c;
            }
            break;
        case ST_STRING:
            out[o++] = c;
            state = (c == '\\') ? ST_STRING_ESC
                     : (c == '"') ? ST_NORMAL : ST_STRING;
            break;
        case ST_STRING_ESC:
            out[o++] = c;
            state = ST_STRING;
            break;
        case ST_LINE_COMMENT:
            if (c == '\n') {
                out[o++] = c;   /* keep line structure for error messages */
                state = ST_NORMAL;
            }
            break;
        case ST_BLOCK_COMMENT:
            if (c == '*') {
                state = ST_BLOCK_COMMENT_STAR;
            }
            break;
        case ST_BLOCK_COMMENT_STAR:
            if (c == '/') {
                state = ST_NORMAL;
            } else if (c != '*') {
                state = ST_BLOCK_COMMENT;
            }
            break;
        }
    }
    out[o] = '\0';
    return out;
}

/* ---- parser -------------------------------------------------------------*/

typedef struct {
    const char *p, *end;
    char       *err;
    size_t      err_cap;
    int         failed;
} json_ctx;

static void set_err(json_ctx *c, const char *msg)
{
    if (!c->failed && c->err != NULL && c->err_cap > 0) {
        (void)snprintf(c->err, c->err_cap, "%s", msg);
    }
    c->failed = 1;
}

static void skip_ws(json_ctx *c)
{
    while (c->p < c->end
           && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
        c->p++;
    }
}

static json_value *new_value(json_type t)
{
    json_value *v = calloc(1, sizeof *v);
    if (v != NULL) {
        v->type = t;
    }
    return v;
}

void json_free(json_value *v)
{
    size_t i;

    if (v == NULL) {
        return;
    }
    switch (v->type) {
    case JSON_STRING:
        free(v->str);
        break;
    case JSON_ARRAY:
        for (i = 0; i < v->count; i++) {
            json_free(v->items[i]);
        }
        free(v->items);
        break;
    case JSON_OBJECT:
        for (i = 0; i < v->count; i++) {
            free(v->keys[i]);
            json_free(v->items[i]);
        }
        free(v->keys);
        free(v->items);
        break;
    default:
        break;
    }
    free(v);
}

/* Appends `child` (with owned key `key`, or key == NULL for an array) to
 * `container`. Returns 0 on success, -1 on allocation failure (caller frees
 * `child`/`key` itself). */
static int container_append(json_value *container, char *key, json_value *child)
{
    /* No stored capacity field, so this reallocs by exactly one element
     * every append (O(n) reallocs for a container with n children) rather
     * than growing geometrically. Deliberate: profile files have at most a
     * few dozen entries in any one array/object, parsed once at startup or
     * on demand, never on the INPUT_STATE hot path -- simple and correct
     * beats clever here. */
    size_t cap = container->count + 1;

    {
        json_value **new_items = realloc(container->items, cap * sizeof *new_items);
        if (new_items == NULL) {
            return -1;
        }
        container->items = new_items;
    }
    if (key != NULL) {
        char **new_keys = realloc(container->keys, cap * sizeof *new_keys);
        if (new_keys == NULL) {
            return -1;
        }
        container->keys = new_keys;
    }
    container->items[container->count] = child;
    if (key != NULL) {
        container->keys[container->count] = key;
    }
    container->count++;
    return 0;
}

static json_value *parse_value(json_ctx *c);

/* Parses a JSON string literal at c->p (which must point at the opening
 * quote). Returns a malloc'd, NUL-terminated, unescaped C string, or NULL
 * on error (set_err already called). Handles \" \\ \/ \b \f \n \r \t and
 * \uXXXX for the Basic Multilingual Plane (surrogate pairs are decoded as
 * two separate UTF-16 code units re-encoded independently rather than
 * combined into one code point -- adequate for the button/profile labels
 * this format actually carries; no shipped profile needs astral-plane
 * characters). */
static char *parse_string_raw(json_ctx *c)
{
    char *buf;
    size_t cap, len = 0;
    const char *p;

    if (c->p >= c->end || *c->p != '"') {
        set_err(c, "expected string");
        return NULL;
    }
    p = c->p + 1;
    cap = 32;
    buf = malloc(cap);
    if (buf == NULL) {
        set_err(c, "out of memory");
        return NULL;
    }

    while (p < c->end && *p != '"') {
        unsigned int cp;
        char c0 = *p;

        if (len + 4 >= cap) {   /* room for a worst-case UTF-8 encode below */
            char *nb;
            cap *= 2;
            nb = realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                set_err(c, "out of memory");
                return NULL;
            }
            buf = nb;
        }

        if (c0 != '\\') {
            buf[len++] = c0;
            p++;
            continue;
        }
        p++;
        if (p >= c->end) {
            free(buf);
            set_err(c, "unterminated escape");
            return NULL;
        }
        switch (*p) {
        case '"':  buf[len++] = '"';  p++; break;
        case '\\': buf[len++] = '\\'; p++; break;
        case '/':  buf[len++] = '/';  p++; break;
        case 'b':  buf[len++] = '\b'; p++; break;
        case 'f':  buf[len++] = '\f'; p++; break;
        case 'n':  buf[len++] = '\n'; p++; break;
        case 'r':  buf[len++] = '\r'; p++; break;
        case 't':  buf[len++] = '\t'; p++; break;
        case 'u':
            p++;
            if (c->end - p < 4) {
                free(buf);
                set_err(c, "truncated \\u escape");
                return NULL;
            }
            {
                unsigned int v = 0;
                int i;
                for (i = 0; i < 4; i++) {
                    char h = p[i];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned int)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned int)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned int)(h - 'A' + 10);
                    else {
                        free(buf);
                        set_err(c, "invalid \\u escape");
                        return NULL;
                    }
                }
                cp = v;
            }
            p += 4;
            /* Re-encode as UTF-8. */
            if (cp < 0x80u) {
                buf[len++] = (char)cp;
            } else if (cp < 0x800u) {
                buf[len++] = (char)(0xC0u | (cp >> 6));
                buf[len++] = (char)(0x80u | (cp & 0x3Fu));
            } else {
                buf[len++] = (char)(0xE0u | (cp >> 12));
                buf[len++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                buf[len++] = (char)(0x80u | (cp & 0x3Fu));
            }
            break;
        default:
            free(buf);
            set_err(c, "unknown escape sequence");
            return NULL;
        }
    }
    if (p >= c->end) {
        free(buf);
        set_err(c, "unterminated string");
        return NULL;
    }
    buf[len] = '\0';
    c->p = p + 1;   /* past closing quote */
    return buf;
}

static json_value *parse_string(json_ctx *c)
{
    json_value *v;
    char *s = parse_string_raw(c);
    if (s == NULL) {
        return NULL;
    }
    v = new_value(JSON_STRING);
    if (v == NULL) {
        free(s);
        set_err(c, "out of memory");
        return NULL;
    }
    v->str = s;
    return v;
}

static json_value *parse_number(json_ctx *c)
{
    const char *start = c->p;
    char *endptr = NULL;
    double d;
    json_value *v;

    if (c->p < c->end && *c->p == '-') {
        c->p++;
    }
    if (c->p >= c->end || !isdigit((unsigned char)*c->p)) {
        set_err(c, "invalid number");
        return NULL;
    }
    while (c->p < c->end && isdigit((unsigned char)*c->p)) {
        c->p++;
    }
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        while (c->p < c->end && isdigit((unsigned char)*c->p)) {
            c->p++;
        }
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-')) {
            c->p++;
        }
        while (c->p < c->end && isdigit((unsigned char)*c->p)) {
            c->p++;
        }
    }
    /* The whole stripped document is one NUL-terminated buffer, so strtod
     * reading past c->p (but never past the real end, since it stops at
     * the first non-numeric char, which start..c->p already bounds) is
     * safe. */
    d = strtod(start, &endptr);
    (void)endptr;
    v = new_value(JSON_NUMBER);
    if (v == NULL) {
        set_err(c, "out of memory");
        return NULL;
    }
    v->num = d;
    return v;
}

static int match_literal(json_ctx *c, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) {
        return 0;
    }
    c->p += n;
    return 1;
}

static json_value *parse_array(json_ctx *c)
{
    json_value *v = new_value(JSON_ARRAY);
    if (v == NULL) {
        set_err(c, "out of memory");
        return NULL;
    }
    c->p++;   /* consume '[' */
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return v;
    }
    for (;;) {
        json_value *elem;

        skip_ws(c);
        elem = parse_value(c);
        if (elem == NULL) {
            json_free(v);
            return NULL;
        }
        if (container_append(v, NULL, elem) != 0) {
            json_free(elem);
            json_free(v);
            set_err(c, "out of memory");
            return NULL;
        }
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            skip_ws(c);
            if (c->p < c->end && *c->p == ']') {   /* tolerated trailing comma */
                c->p++;
                return v;
            }
            continue;
        }
        if (c->p < c->end && *c->p == ']') {
            c->p++;
            return v;
        }
        set_err(c, "expected ',' or ']' in array");
        json_free(v);
        return NULL;
    }
}

static json_value *parse_object(json_ctx *c)
{
    json_value *v = new_value(JSON_OBJECT);
    if (v == NULL) {
        set_err(c, "out of memory");
        return NULL;
    }
    c->p++;   /* consume '{' */
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return v;
    }
    for (;;) {
        char *key;
        json_value *val;

        skip_ws(c);
        if (c->p >= c->end || *c->p != '"') {
            set_err(c, "expected string key in object");
            json_free(v);
            return NULL;
        }
        key = parse_string_raw(c);
        if (key == NULL) {
            json_free(v);
            return NULL;
        }
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') {
            free(key);
            set_err(c, "expected ':' after object key");
            json_free(v);
            return NULL;
        }
        c->p++;
        skip_ws(c);
        val = parse_value(c);
        if (val == NULL) {
            free(key);
            json_free(v);
            return NULL;
        }
        if (container_append(v, key, val) != 0) {
            free(key);
            json_free(val);
            json_free(v);
            set_err(c, "out of memory");
            return NULL;
        }
        skip_ws(c);
        if (c->p < c->end && *c->p == ',') {
            c->p++;
            skip_ws(c);
            if (c->p < c->end && *c->p == '}') {   /* tolerated trailing comma */
                c->p++;
                return v;
            }
            continue;
        }
        if (c->p < c->end && *c->p == '}') {
            c->p++;
            return v;
        }
        set_err(c, "expected ',' or '}' in object");
        json_free(v);
        return NULL;
    }
}

static json_value *parse_value(json_ctx *c)
{
    skip_ws(c);
    if (c->p >= c->end) {
        set_err(c, "unexpected end of input");
        return NULL;
    }
    switch (*c->p) {
    case '{': return parse_object(c);
    case '[': return parse_array(c);
    case '"': return parse_string(c);
    case 't':
        if (match_literal(c, "true")) {
            return new_value(JSON_TRUE);
        }
        break;
    case 'f':
        if (match_literal(c, "false")) {
            return new_value(JSON_FALSE);
        }
        break;
    case 'n':
        if (match_literal(c, "null")) {
            return new_value(JSON_NULL);
        }
        break;
    default:
        if (*c->p == '-' || isdigit((unsigned char)*c->p)) {
            return parse_number(c);
        }
        break;
    }
    set_err(c, "unexpected character");
    return NULL;
}

json_value *json_parse(const char *text, char *err, size_t err_cap)
{
    char *stripped;
    json_ctx c;
    json_value *root;
    size_t len;

    if (text == NULL) {
        if (err != NULL && err_cap > 0) {
            (void)snprintf(err, err_cap, "NULL input");
        }
        return NULL;
    }
    len = strlen(text);
    stripped = strip_comments(text, len);
    if (stripped == NULL) {
        if (err != NULL && err_cap > 0) {
            (void)snprintf(err, err_cap, "out of memory stripping comments");
        }
        return NULL;
    }

    c.p = stripped;
    c.end = stripped + strlen(stripped);
    c.err = err;
    c.err_cap = err_cap;
    c.failed = 0;

    root = parse_value(&c);
    if (root != NULL) {
        skip_ws(&c);
        if (c.p != c.end) {
            json_free(root);
            root = NULL;
            set_err(&c, "trailing content after top-level value");
        }
    }
    free(stripped);
    return root;
}

const json_value *json_obj_get(const json_value *obj, const char *key)
{
    size_t i;

    if (obj == NULL || obj->type != JSON_OBJECT || key == NULL) {
        return NULL;
    }
    for (i = 0; i < obj->count; i++) {
        if (strcmp(obj->keys[i], key) == 0) {
            return obj->items[i];
        }
    }
    return NULL;
}

double json_num(const json_value *v, double fallback)
{
    return (v != NULL && v->type == JSON_NUMBER) ? v->num : fallback;
}

int json_bool(const json_value *v, int fallback)
{
    if (v == NULL) {
        return fallback;
    }
    if (v->type == JSON_TRUE) {
        return 1;
    }
    if (v->type == JSON_FALSE) {
        return 0;
    }
    return fallback;
}

const char *json_str(const json_value *v, const char *fallback)
{
    return (v != NULL && v->type == JSON_STRING) ? v->str : fallback;
}
