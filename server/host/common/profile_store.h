/* server/host/common/profile_store.h -- the filesystem half of JSONC
 * per-device mapping profiles (docs/DESIGN.md §6.2, §6.4), shared by both hosts.
 *
 * server/src/profiles.c is library code and does no filesystem access on
 * purpose (its own top comment): it parses memory blobs
 * (apad_profile_source), never a path. Everything on the "find and read
 * files off disk" side of that seam used to be two near-identical,
 * independently-maintained copies -- server/host/linux/main.c's
 * load_profile_files() (opendir/readdir) and server/host/windows/main.c's
 * twin (FindFirstFileA/FindNextFileA) -- which is exactly the kind of drift
 * ipaddr.h's own top comment warns about for the same "one interface, two
 * platform bodies" shape. This file is that consolidation: profile_store_scan()
 * is the ONE function both hosts and webui.h's route handlers call, with the
 * genuine platform fork (POSIX directory iteration vs Win32) kept as two
 * complete bodies behind #ifdef _WIN32, same pattern as
 * host_enumerate_own_ipv4() in ipaddr.h.
 *
 * NEW in this file, for the remapping-editor task (docs/DESIGN.md's web-based
 * profile editor): writing and deleting a profile file, atomically, from the
 * UI's PUT/DELETE routes (webui.h) -- something neither host needed before
 * profiles were ever mutated at runtime.
 *
 * Header-only, same build-script-constraint reason as every other file in
 * this directory (see webui.h's top comment).
 *
 * NAMING CONVENTION this file assumes and enforces on the write path (an
 * ambiguity worth stating plainly rather than leaving implicit): the `name`
 * a caller passes to profile_store_save()/profile_store_delete()/
 * profile_store_is_shipped() is the FILENAME STEM -- "<profiles_dir>/<name>.jsonc"
 * -- not necessarily the JSON "profile" field a file's own content declares.
 * Every profile shipped in server/profiles/ already keeps those two in sync
 * (3ds-default.jsonc's "profile" is "3ds-default", and so on), and
 * profile_json.h's PUT handler forces them to stay in sync for anything this
 * API writes (it always sets the saved file's "profile" field to the route's
 * {name}, overriding whatever the request body said) -- but nothing stops a
 * profile file dropped into the directory by hand from having a "profile"
 * field that does NOT match its own filename. In that case
 * apad_profiles_find(json_name) still finds it by its DECLARED identity (the
 * loaded apad_profile::name, used by apad_profiles_find/apad_server_set_profile),
 * while GET/PUT/DELETE /api/profile/{name} address it by FILENAME -- two
 * different keys that happen to coincide for every file this tree ships or
 * this API ever writes, but are not the same key in general. Recorded here,
 * not fixed: profiles.c does not track which source file produced a given
 * apad_profile after loading (it only logs the label, docs/DESIGN.md §6.4's
 * library/host split), so resolving "the file behind this loaded profile
 * name" would need a change to that struct, which is out of this task's
 * scope (profiles.c's frozen matching/vocabulary parts aside, adding a
 * source-file field to apad_profile is a bigger change than a filesystem
 * helper header is the place to make unilaterally).
 */
#ifndef ATTICPAD_HOST_COMMON_PROFILE_STORE_H
#define ATTICPAD_HOST_COMMON_PROFILE_STORE_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "profiles.h"   /* apad_profiles_builtin_default() -- so
                          * profile_store_is_shipped() can block that name
                          * too, derived rather than hardcoded a third time
                          * (see that function's own comment below) */

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>   /* mkdir: profile_store_save creates the directory */
#include <sys/types.h>
#endif

/* Independent of server/src/profiles.h's own APAD_PROFILES_MAX (the size of
 * the LIBRARY's loaded-profile table): this one bounds the directory scan,
 * that one bounds what parsing keeps -- same value (16) today, same
 * reasoning both linux/main.c's and windows/main.c's old HOST_MAX_PROFILE_FILES
 * used before this file existed, but the library must not be handed a
 * filesystem limit and the host must not be handed a table size. */
#define PROFILE_STORE_MAX_FILES 16

typedef struct {
    char *label;   /* full path, what diagnostics name */
    char *name;    /* basename minus ".jsonc", the default profile name if
                     * the file's own JSON omits "profile" */
    char *text;    /* the file's bytes, owned, NUL-terminated */
} profile_store_file;

/* The profiles this tree SHIPS (server/profiles/, files named *.jsonc, minus the
 * extension) -- read-only through the web API per this task's brief: PUT
 * and DELETE both refuse these with 409 (webui.h), so an operator's local
 * edits and a fresh checkout's tracked files can never collide. Derived from
 * the shipped FILENAMES, once, here -- not hardcoded a second time at each
 * call site -- so adding a third shipped profile later is a one-line change
 * in exactly one place. */
static const char *const profile_store_shipped_names[] = {
    "3ds-default", "generic-default"
};
#define PROFILE_STORE_SHIPPED_COUNT \
    (int)(sizeof profile_store_shipped_names / sizeof profile_store_shipped_names[0])

/*
 * Blocks the FILENAME LIST above, plus -- separately -- the built-in
 * default's own name (apad_profiles_builtin_default(), "builtin-default"
 * today, read from the live struct rather than hardcoded a third time:
 * profile_json.h's own "builtin" flag already does this exact lookup the
 * same way). Without this, PUT /api/profile/builtin-default was NOT
 * refused -- it has no backing *.jsonc file, so profile_store_shipped_names
 * never named it -- and would have written a real
 * server/profiles/builtin-default.jsonc, giving apad_profiles_find()
 * (server/src/profiles.c, exact-name lookup) two different things both
 * called "builtin-default": the compiled-in fallback that is never loaded
 * from a file, and now a loaded one shadowing or colliding with it
 * depending on load order (found live, protocol-audit style, not by
 * inspection alone). The built-in has no filename stem of its own to
 * compare against -- it never came from a file -- so this checks the NAME
 * only, same key GET/PUT/DELETE /api/profile/{name} already address every
 * other profile by.
 */
static int profile_store_is_shipped(const char *name)
{
    int i;
    const apad_profile *builtin;

    if (name == NULL) {
        return 0;
    }
    for (i = 0; i < PROFILE_STORE_SHIPPED_COUNT; i++) {
        if (strcmp(profile_store_shipped_names[i], name) == 0) {
            return 1;
        }
    }
    builtin = apad_profiles_builtin_default();
    if (builtin != NULL && strcmp(builtin->name, name) == 0) {
        return 1;
    }
    return 0;
}

/*
 * A `name` this file will accept as a filename stem: 1..63 bytes of
 * [A-Za-z0-9_-] only. This is the one gate between an HTTP route segment
 * (webui.h's /api/profile/{name}, un-URL-decoded, this file's top comment on
 * the naming convention) and a path this process opens/writes/deletes --
 * without it, "../../../../etc/cron.d/x" would be a syntactically valid
 * {name}. Deliberately stricter than "no slashes": every name this tree
 * ships or this API itself ever generates (3ds-default, generic-default,
 * 00-test-swap, ...) already fits this alphabet, so nothing legitimate is
 * excluded, and refusing anything else outright is simpler to reason about
 * than trying to enumerate every path-traversal trick across two platforms'
 * filename rules (Windows alone treats "CON", trailing dots, and ':' as
 * special in ways POSIX does not).
 */
static int profile_store_name_valid(const char *name)
{
    size_t i, n;

    if (name == NULL) {
        return 0;
    }
    n = strlen(name);
    if (n == 0u || n >= 64u) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return 0;
        }
    }
    return 1;
}

static int profile_store_has_jsonc_suffix(const char *name)
{
    size_t n = strlen(name);
    static const char suffix[] = ".jsonc";
    size_t sn = sizeof suffix - 1;
    return n > sn && strcmp(name + (n - sn), suffix) == 0;
}

static int profile_store_cmp_str(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sa, *sb);
}

/* Not just an mingw concern (though that is the sharper one -- mingw's
 * strdup() availability depends on which CRT feature-test macros are in
 * effect under -std=c11, same issue server/host/windows/main.c's own
 * xstrdup() was written to sidestep): strdup() itself is POSIX, not C99/C11,
 * so a private implementation used on both platforms is one function this
 * header does not have to fight either toolchain's headers over. */
static char *profile_store_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

static char *profile_store_read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;
    size_t n;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        (void)fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (size_t)size, f);
    (void)fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Strips a trailing ".jsonc" off `name` IN PLACE -- the default profile
 * identity when a file's own JSON omits "profile" (server/src/profiles.c:
 * build_profile()'s `default_name` parameter). */
static void profile_store_strip_suffix(char *name)
{
    size_t n = strlen(name);
    if (n > 6 && strcmp(name + n - 6, ".jsonc") == 0) {
        name[n - 6] = '\0';
    }
}

#ifdef _WIN32

/*
 * Read every *.jsonc directly under `dir` into `out`, filename-sorted, and
 * return how many landed. Windows body: FindFirstFileA/FindNextFileA rather
 * than mingw's <dirent.h> emulation, to avoid depending on that emulation
 * matching POSIX readdir order (it does not; hence the sort below, which is
 * load-bearing on both platforms -- see apad_profiles_match()'s doc comment:
 * the first loaded profile whose match.device substring-matches wins, and an
 * empty match.device matches everything, so a wildcard profile offered
 * first would shadow every specific one behind it).
 */
static size_t profile_store_scan(const char *dir, profile_store_file *out,
                                 size_t max)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[MAX_PATH];
    char *names[PROFILE_STORE_MAX_FILES];
    int name_count = 0;
    int i;
    size_t count = 0;
    BOOL more;

    (void)snprintf(pattern, sizeof pattern, "%s\\*", dir);
    pattern[sizeof pattern - 1] = '\0';

    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;   /* missing directory: never fatal, see this file's header */
    }

    more = TRUE;
    while (more && name_count < PROFILE_STORE_MAX_FILES) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            profile_store_has_jsonc_suffix(fd.cFileName)) {
            names[name_count] = profile_store_strdup(fd.cFileName);
            if (names[name_count] != NULL) {
                name_count++;
            }
        }
        more = FindNextFileA(h, &fd);
    }
    (void)FindClose(h);

    qsort(names, (size_t)name_count, sizeof names[0], profile_store_cmp_str);

    for (i = 0; i < name_count; i++) {
        char path[512];
        char *text;

        (void)snprintf(path, sizeof path, "%s\\%s", dir, names[i]);
        path[sizeof path - 1] = '\0';
        text = profile_store_read_whole_file(path);
        if (text == NULL || count >= max) {
            free(text);
            free(names[i]);
            continue;
        }
        profile_store_strip_suffix(names[i]);
        out[count].label = profile_store_strdup(path);
        out[count].name  = names[i];   /* ownership moves to out[] */
        out[count].text  = text;
        if (out[count].label == NULL) {
            free(names[i]);
            free(text);
            continue;
        }
        count++;
    }
    return count;
}

/* Atomic write: a temp file in the SAME directory (so the rename below is
 * guaranteed to be on one filesystem/volume, never a cross-device copy) then
 * MOVEFILE_REPLACE_EXISTING -- POSIX rename() replaces an existing
 * destination unconditionally, but plain Win32 MoveFileA does not, and this
 * write path (webui.h's PUT /api/profile/{name}) is exactly the case where
 * the destination already exists (editing a previously-saved profile).
 * Returns 1 on success, 0 otherwise; refuses a shipped or invalid name up
 * front so a caller cannot accidentally overwrite a tracked file through a
 * bug elsewhere -- webui.h's route handler already checks this too, but the
 * check belongs here as well: this is the only function in the tree that
 * actually touches the filesystem for a write, and it must not trust every
 * caller to have remembered the rule. */
static int profile_store_save(const char *dir, const char *name,
                              const char *jsonc_text)
{
    char tmp_path[MAX_PATH];
    char final_path[MAX_PATH];
    FILE *f;
    size_t len, written;

    if (!profile_store_name_valid(name) || profile_store_is_shipped(name)) {
        return 0;
    }
    /* Create the directory if it is not there yet. Since the shipped profiles
     * became compiled-in, "no profiles directory" is the NORMAL state of a
     * freshly downloaded server -- it serves the built-ins happily -- so the
     * first edit a user makes is also the first time anything needs the
     * directory to exist. Without this the editor could only ever save on a
     * machine that already had one, which is the bug this comment replaces.
     * ERROR_ALREADY_EXISTS is success for our purposes. */
    if (!CreateDirectoryA(dir, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
    (void)snprintf(tmp_path, sizeof tmp_path, "%s\\.%s.jsonc.tmp", dir, name);
    (void)snprintf(final_path, sizeof final_path, "%s\\%s.jsonc", dir, name);
    tmp_path[sizeof tmp_path - 1]     = '\0';
    final_path[sizeof final_path - 1] = '\0';

    f = fopen(tmp_path, "wb");
    if (f == NULL) {
        return 0;
    }
    len = strlen(jsonc_text);
    written = fwrite(jsonc_text, 1, len, f);
    if (fclose(f) != 0 || written != len) {
        (void)DeleteFileA(tmp_path);
        return 0;
    }
    if (!MoveFileExA(tmp_path, final_path, MOVEFILE_REPLACE_EXISTING)) {
        (void)DeleteFileA(tmp_path);
        return 0;
    }
    return 1;
}

static int profile_store_delete(const char *dir, const char *name)
{
    char path[MAX_PATH];

    if (!profile_store_name_valid(name) || profile_store_is_shipped(name)) {
        return 0;
    }
    (void)snprintf(path, sizeof path, "%s\\%s.jsonc", dir, name);
    path[sizeof path - 1] = '\0';
    return DeleteFileA(path) ? 1 : 0;
}

#else  /* POSIX */

/*
 * Read every *.jsonc in `dir` into `out`, filename-sorted, and return how
 * many landed. POSIX body: opendir/readdir. See the Windows body's comment
 * above for why the sort is load-bearing, not tidiness.
 */
static size_t profile_store_scan(const char *dir, profile_store_file *out,
                                 size_t max)
{
    DIR *d;
    struct dirent *ent;
    char *names[PROFILE_STORE_MAX_FILES];
    int name_count = 0;
    int i;
    size_t count = 0;

    d = opendir(dir);
    if (d == NULL) {
        return 0;   /* missing directory: never fatal, see this file's header */
    }

    while ((ent = readdir(d)) != NULL && name_count < PROFILE_STORE_MAX_FILES) {
        if (!profile_store_has_jsonc_suffix(ent->d_name)) {
            continue;
        }
        names[name_count] = profile_store_strdup(ent->d_name);
        if (names[name_count] != NULL) {
            name_count++;
        }
    }
    (void)closedir(d);

    qsort(names, (size_t)name_count, sizeof names[0], profile_store_cmp_str);

    for (i = 0; i < name_count; i++) {
        char path[512];
        char *text;

        (void)snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        text = profile_store_read_whole_file(path);
        if (text == NULL || count >= max) {
            free(text);
            free(names[i]);
            continue;
        }
        profile_store_strip_suffix(names[i]);
        out[count].label = profile_store_strdup(path);
        out[count].name  = names[i];   /* ownership moves to out[] */
        out[count].text  = text;
        if (out[count].label == NULL) {
            free(names[i]);
            free(text);
            continue;
        }
        count++;
    }
    return count;
}

/* Atomic write: a temp file in the SAME directory, then rename() -- POSIX
 * guarantees rename() is atomic and replaces an existing destination when
 * both paths are on the same filesystem, which a temp file created
 * alongside the target always is. See the Windows body's comment for why
 * this is deliberately double-checked here rather than trusted to every
 * caller. */
static int profile_store_save(const char *dir, const char *name,
                              const char *jsonc_text)
{
    char tmp_path[512];
    char final_path[512];
    FILE *f;
    size_t len, written;

    if (!profile_store_name_valid(name) || profile_store_is_shipped(name)) {
        return 0;
    }
    /* See the Windows half: with the shipped profiles compiled in, a server
     * that has never been edited has no profiles directory at all, so the
     * first save has to create it. EEXIST is success. */
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return 0;
    }
    (void)snprintf(tmp_path, sizeof tmp_path, "%s/.%s.jsonc.tmp", dir, name);
    (void)snprintf(final_path, sizeof final_path, "%s/%s.jsonc", dir, name);

    f = fopen(tmp_path, "wb");
    if (f == NULL) {
        return 0;
    }
    len = strlen(jsonc_text);
    written = fwrite(jsonc_text, 1, len, f);
    if (fclose(f) != 0 || written != len) {
        (void)remove(tmp_path);
        return 0;
    }
    if (rename(tmp_path, final_path) != 0) {
        (void)remove(tmp_path);
        return 0;
    }
    return 1;
}

static int profile_store_delete(const char *dir, const char *name)
{
    char path[512];

    if (!profile_store_name_valid(name) || profile_store_is_shipped(name)) {
        return 0;
    }
    (void)snprintf(path, sizeof path, "%s/%s.jsonc", dir, name);
    return remove(path) == 0;
}

#endif /* _WIN32 */

static void profile_store_free_files(profile_store_file *files, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        free(files[i].label);
        free(files[i].name);
        free(files[i].text);
    }
}

#endif /* ATTICPAD_HOST_COMMON_PROFILE_STORE_H */
