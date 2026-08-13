/* server/src/jsonc.h — a small, hand-written JSONC (JSON + comments) parser
 * (docs/DESIGN.md D6, D9).
 *
 * D6 asked for "a jsmn-style tokenizer (~500 lines, single header) plus a
 * ~30-line comment-stripping pre-pass". What follows is a small hand-written
 * recursive-descent parser producing an in-memory DOM (json_value tree)
 * rather than a literal port of jsmn's flat token/size array. Reasoning:
 * server code may malloc freely (docs/CONVENTIONS.md, backend.h, this file's own
 * header comment convention), and profiles.c's job is reading a NESTED
 * schema (objects inside objects inside arrays) -- walking that by hand
 * from jsmn's flat, index-and-size-counted token stream is exactly the kind
 * of bookkeeping a small tree makes trivial and a flat scan makes fragile.
 * Same spirit as D6 (small, single-purpose, vendored, no package-manager
 * dependency), different, safer shape for this specific schema. Reported
 * as a deliberate deviation from the letter of D6, not a silent one.
 *
 * This is server-only code (server/src/, D9) -- core/'s no-malloc,
 * no-float, no-stdio constraints do not apply here.
 *
 * Compiled as its own translation unit, linked alongside mapping.c and
 * profiles.c by scripts/build.sh's build_server(). (An earlier revision of
 * this file was pulled into mapping.c's translation unit via a source
 * #include, working around a since-resolved build-script constraint; see
 * mapping.c's top comment.)
 */
#ifndef ATTICPAD_SERVER_JSONC_H
#define ATTICPAD_SERVER_JSONC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JSON_NULL = 0,
    JSON_FALSE,
    JSON_TRUE,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type;

typedef struct json_value json_value;

struct json_value {
    json_type     type;
    double        num;    /* JSON_NUMBER only */
    char         *str;    /* JSON_STRING only -- owned, NUL-terminated */
    json_value  **items;  /* JSON_ARRAY / JSON_OBJECT values -- owned */
    char        **keys;   /* JSON_OBJECT only -- owned, parallel to items[] */
    size_t        count;  /* element count for JSON_ARRAY / JSON_OBJECT */
};

/*
 * Parses `text` (a NUL-terminated JSONC document: JSON plus line and block
 * comments, with one tolerated trailing comma before `]` or `}`) into a
 * tree of json_value nodes.
 *
 * Returns NULL on any parse error and writes a short, human-readable
 * description into err[0..err_cap) (if err != NULL and err_cap > 0).
 * Never returns a partially-built tree: either the whole document parsed,
 * or nothing did. Caller owns the result and must json_free() it.
 */
json_value *json_parse(const char *text, char *err, size_t err_cap);

void json_free(json_value *v);

/* Convenience accessors. Every one degrades to `fallback` instead of
 * crashing on a NULL value, the wrong type, or (for json_obj_get) a
 * missing key -- a profile author's typo should skip a field, not take
 * the server down. */
const json_value *json_obj_get(const json_value *obj, const char *key);
double             json_num(const json_value *v, double fallback);
int                json_bool(const json_value *v, int fallback);
const char        *json_str(const json_value *v, const char *fallback);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_SERVER_JSONC_H */
