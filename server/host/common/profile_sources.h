/* server/host/common/profile_sources.h -- assemble the profile set a host
 * hands to the library: files on disk, plus the profiles compiled into the
 * binary for anything disk does not override.
 *
 * This exists because the same assembly is needed in three places -- each
 * host's startup and the web UI's reload-after-save -- and when it lived in
 * three places it diverged immediately. Startup learned to merge the built-in
 * profiles; reload did not, so saving one edited profile through the editor
 * silently dropped every shipped profile from the RUNNING set while the next
 * restart quietly restored them. One function, three callers.
 *
 * Ownership, which is the part that already caused one crash: entries taken
 * from `files` own heap memory and must be released with
 * profile_store_free_files(*out_file_count). Entries taken from the built-ins
 * are static storage and MUST NOT be freed. The two counts are therefore
 * different numbers and are returned separately -- conflating them frees
 * uninitialised stack pointers, which is exactly what shipped in 0.4.0-rc2.
 */
#ifndef ATTICPAD_HOST_COMMON_PROFILE_SOURCES_H
#define ATTICPAD_HOST_COMMON_PROFILE_SOURCES_H

#include <string.h>

#include "profile_store.h"
#include "profiles_builtin.h"

/*
 * Scans `dir`, then appends every built-in profile whose name no file there
 * already uses.
 *
 * Disk entries come FIRST on purpose: profiles.c matches a device against the
 * first loaded profile whose match_device fits, so an edited copy on disk has
 * to precede the shipped one to win.
 *
 * Returns the number of `sources` filled. `*out_file_count` receives the
 * number of leading entries that came from disk and therefore need freeing.
 */
static size_t profile_sources_build(const char *dir,
                                    profile_store_file *files,
                                    apad_profile_source *sources,
                                    size_t cap,
                                    size_t *out_file_count)
{
    size_t file_count, source_count, i, j;

    file_count = profile_store_scan(dir, files, cap);
    for (i = 0; i < file_count; i++) {
        sources[i].label = files[i].label;
        sources[i].name  = files[i].name;
        sources[i].text  = files[i].text;
    }
    source_count = file_count;

    for (j = 0; j < ATTICPAD_BUILTIN_PROFILE_COUNT; j++) {
        int overridden = 0;
        for (i = 0; i < file_count; i++) {
            if (files[i].name != NULL &&
                strcmp(files[i].name, ATTICPAD_BUILTIN_PROFILES[j].name) == 0) {
                overridden = 1;
                break;
            }
        }
        if (overridden || source_count >= cap) {
            continue;
        }
        sources[source_count].label = ATTICPAD_BUILTIN_PROFILES[j].label;
        sources[source_count].name  = ATTICPAD_BUILTIN_PROFILES[j].name;
        sources[source_count].text  = ATTICPAD_BUILTIN_PROFILES[j].text;
        source_count++;
    }

    if (out_file_count != NULL) {
        *out_file_count = file_count;
    }
    return source_count;
}

#endif /* ATTICPAD_HOST_COMMON_PROFILE_SOURCES_H */
