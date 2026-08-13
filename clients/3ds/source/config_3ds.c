/* clients/3ds/source/config_3ds.c -- see config_3ds.h.
 *
 * THE PATH: sdmc:/3ds/atticpad/atticpad.cfg. There is no references/3ds/
 * sample for persistent app config -- the pinned devkitARM image's own
 * examples/3ds/sdmc (grepped directly, not recalled) shows fopen()/fread()
 * working with no explicit archive-mount call, which is what this file
 * relies on, but neither it nor anything else in this tree demonstrates
 * mkdir() or rename() against an "sdmc:/" path. sdmc:/3ds/<AppName>/ is,
 * however, the path EVERY piece of 3DS homebrew this project has already
 * read for other reasons uses for exactly this purpose -- FBI, Checkpoint
 * and Anemone3DS (camera_3ds.c's header cites the first and third directly)
 * all keep their own config/data under sdmc:/3ds/<name>/. Following that
 * convention, rather than inventing a new one, is what "mirror, don't
 * invent" means when there is no in-repo sample to mirror. UNVERIFIED ON
 * HARDWARE: this exact mkdir()+fopen()+rename() sequence against sdmc:/3ds/
 * has not been run on physical hardware yet. See docs/PORTING.md.
 *
 * FORMAT: two `key=value` lines, ip= then port=, nothing else. Trivially
 * parseable (one strncmp per line, no tokenizer), naturally
 * forward-compatible (an unrecognised key is just skipped, so a later
 * version of this file adding a third line does not break an older
 * apad3ds_config_load()), and there is nothing here sensitive enough to need
 * more than that -- see config_3ds.h: the pairing secret never reaches this
 * file at all, so there is no format decision to make about protecting it.
 *
 * WRITE TIME: screen_session.c calls apad3ds_config_save() once, the first
 * frame a connect reaches ACTIVE (session_update()'s ctx->connected 0->1
 * edge) -- never from main()'s exit path. That matters for the same reason
 * soc_3ds.c's atexit(socExit) is ordered the way it is: APTHOOK_ONSUSPEND/
 * ONEXIT (main.c's apt_hook_cb()) can tear this app down with no further
 * frames drawn, and this file's writes must never race that teardown or run
 * after it. Writing at ACTIVE time, on the main thread, synchronously,
 * before the frame that reports ACTIVE finishes, means the write is long
 * done by the time any teardown path could run -- there is no background
 * writer here to race in the first place.
 *
 * ATOMICITY: write to a .tmp file, fclose(), then rename() over the real
 * path. rename() on a POSIX-shaped filesystem replaces the destination in
 * one directory-entry update, so a reader (or a power loss) never observes
 * a file that is half this save and half the previous one. Every failure
 * path removes the .tmp file rather than leaving it behind for the next
 * save to find in an unknown state.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "config_3ds.h"

#define APAD3DS_CFG_SDDIR  "sdmc:/3ds"
#define APAD3DS_CFG_DIR    "sdmc:/3ds/atticpad"
#define APAD3DS_CFG_PATH   "sdmc:/3ds/atticpad/atticpad.cfg"
#define APAD3DS_CFG_TMP    "sdmc:/3ds/atticpad/atticpad.cfg.tmp"

/* Local copies sized generously past app_ctx's ip_text[16]/port_text[6]
 * (app.h) -- a saved value longer than either is treated as corrupt by
 * apad3ds_config_load()'s caller-buffer-fit check below, never truncated
 * into one silently. */
#define APAD3DS_CFG_LINE_MAX 64
#define APAD3DS_CFG_VAL_MAX  32

int apad3ds_config_load(char *ip_out, size_t ip_cap,
                        char *port_out, size_t port_cap)
{
    FILE *f;
    char line[APAD3DS_CFG_LINE_MAX];
    char ip[APAD3DS_CFG_VAL_MAX];
    char port[APAD3DS_CFG_VAL_MAX];
    int have_ip = 0, have_port = 0;

    if (ip_out == NULL || ip_cap == 0) {
        return 0;
    }
    ip_out[0] = '\0';

    f = fopen(APAD3DS_CFG_PATH, "r");
    if (f == NULL) {
        /* No file yet (fresh unit, or one whose config was never written) --
         * not an error, and nothing more to say: the caller's field stays
         * empty, exactly like a fresh unit. */
        return 0;
    }

    ip[0] = '\0';
    port[0] = '\0';

    while (fgets(line, sizeof line, f) != NULL) {
        size_t n = strlen(line);

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (strncmp(line, "ip=", 3) == 0) {
            /* Precision, not just a width, so gcc's -Wformat-truncation can
             * SEE the copy fits sizeof ip -- APAD3DS_CFG_VAL_MAX - 1 spelled
             * out as a literal, same reasoning as the port branch below. A
             * value line longer than this truncates here rather than
             * overflowing; the caller-buffer-fit check further down still
             * has the final say over whether a truncated ip is usable. */
            snprintf(ip, sizeof ip, "%.31s", line + 3);
            have_ip = 1;
        } else if (strncmp(line, "port=", 5) == 0) {
            snprintf(port, sizeof port, "%.31s", line + 5);
            have_port = 1;
        }
        /* Any other line (a future key this build predates, a stray blank
         * line, garbage) is silently skipped -- "tolerate a corrupt file by
         * ignoring it" is per-line here, not just per-file. */
    }
    fclose(f);

    if (!have_ip || ip[0] == '\0' || strlen(ip) >= ip_cap) {
        /* No usable ip= line, or one that would not fit the caller's
         * buffer. Either way this is "corrupt", handled the same as "no
         * file": ip_out is already "" from above, and port_out is left
         * untouched so the caller's own default survives. */
        return 0;
    }
    snprintf(ip_out, ip_cap, "%s", ip);

    if (have_port && port_out != NULL && port_cap > 0 && port[0] != '\0'
        && strlen(port) < port_cap) {
        snprintf(port_out, port_cap, "%s", port);
    }
    return 1;
}

void apad3ds_config_save(const char *ip, const char *port)
{
    FILE *f;

    if (ip == NULL || ip[0] == '\0' || port == NULL || port[0] == '\0') {
        /* Never write a half-filled record: a blank ip or port here would
         * make the NEXT load()'s corrupt-file path fire for no reason, over
         * a save that should simply not have happened. */
        return;
    }

    /* Best effort: an already-existing directory (EEXIST) is the expected
     * case on every save after the first, and any other mkdir() failure is
     * caught downstream by the fopen() below, which is what actually decides
     * whether this save happens. */
    (void)mkdir(APAD3DS_CFG_SDDIR, 0777);
    (void)mkdir(APAD3DS_CFG_DIR, 0777);

    f = fopen(APAD3DS_CFG_TMP, "w");
    if (f == NULL) {
        return;
    }
    if (fprintf(f, "ip=%s\nport=%s\n", ip, port) < 0) {
        fclose(f);
        remove(APAD3DS_CFG_TMP);
        return;
    }
    if (fclose(f) != 0) {
        remove(APAD3DS_CFG_TMP);
        return;
    }

    if (rename(APAD3DS_CFG_TMP, APAD3DS_CFG_PATH) != 0) {
        /* The temp file is now either a leftover from a failed rename or
         * nothing at all; either way nothing should be left behind for the
         * next save to trip over. Failure here is silent by design (see
         * config_3ds.h) -- there is nowhere on this client to report it and
         * nothing about a controller session that should hinge on it. */
        remove(APAD3DS_CFG_TMP);
    }
}
