/* clients/nds/source/config_nds.c -- see config_nds.h.
 *
 * clients/3ds/source/config_3ds.c, ported: same one-line `key=value` format
 * (trivially parseable, forward-compatible because an unrecognised key is
 * skipped), same write-to-.tmp-then-rename() atomicity, same "a corrupt file
 * is handled exactly like no file" rule. Four DS-specific changes:
 *
 *   1. THE MOUNT HAS TO HAPPEN AND CAN FAIL. On the 3DS "sdmc:/" is simply
 *      there. Here fatInitDefault() (libnds's own libfat replacement, fat.h)
 *      has to bring up either a flashcart's DLDI driver or the DSi's internal
 *      slot, and it very often fails -- under an emulator with no DLDI image,
 *      or on a card whose loader did not patch the ROM. Every entry point
 *      below is a no-op in that case. It is checked ONCE, not per call: a
 *      failed mount stays failed.
 *   2. TWO ROOTS. "fat:/" (DS, DLDI) or "sd:/" (DSi, internal slot), per
 *      fatInitDefault()'s own documentation, mirrored from
 *      references/nds/examples/filesystem/. Chosen once in mount().
 *   3. THREE MORE FIELDS -- the SSID, the network key and its security type.
 *      See config_nds.h for why the key is written at all.
 *   4. ONE RECORD, TWO WRITERS. The address is saved by screen_session.c on
 *      the ACTIVE edge and the network by screen_wifi.c on association; they
 *      run at different moments and neither may clobber the other's half, so
 *      the file is parsed once into s_rec and every save writes s_rec whole.
 *      Read-modify-write with the "read" done at boot, so a save is one
 *      write and no re-parse.
 *
 * fatInitDefault() is declared WARN_UNUSED_RESULT, so its return is checked
 * rather than cast to void; on this build that is not a style choice, -Werror
 * makes it a compile error.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <fat.h>
#include <nds.h>

#include "config_nds.h"

/* Long enough for the longest line the format can produce: "wifi_key=" plus
 * 2*APAD_NDS_KEY_MAX hex digits plus CR, LF and NUL. */
#define CFG_LINE_MAX 160
#define CFG_VAL_MAX  40
#define CFG_HEX_MAX  (2 * APAD_NDS_KEY_MAX + 1)

static int  s_mounted;
static int  s_tried;
static char s_dir[32];
static char s_path[64];
static char s_tmp[64];

/* The whole record, parsed once at load and written whole at every save. */
static struct {
    char ip[CFG_VAL_MAX];
    char port[CFG_VAL_MAX];
    apad_nds_config_net net;
} s_rec;

int apad_nds_config_mount(void)
{
    const char *root;

    if (s_tried) {
        return s_mounted;
    }
    s_tried = 1;

    /* -1 for "no credentials known", NOT 0 ("open") -- config_nds.h explains
     * why the difference matters to screen_wifi.c. Set here rather than
     * relying on the zero-initialised static. */
    s_rec.net.security = -1;

    if (!fatInitDefault()) {
        /* No card, no DLDI driver, or a loader that did not patch this ROM.
         * Not fatal and not reported: a controller that refused to start
         * without an SD card would be a worse product than one that forgets
         * an address. */
        s_mounted = 0;
        return 0;
    }

    root = isDSiMode() ? "sd:" : "fat:";
    snprintf(s_dir,  sizeof s_dir,  "%s/atticpad", root);
    snprintf(s_path, sizeof s_path, "%s/atticpad/atticpad.cfg", root);
    snprintf(s_tmp,  sizeof s_tmp,  "%s/atticpad/atticpad.cfg.tmp", root);
    s_mounted = 1;
    return 1;
}

static void copy_out(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

/* ------------------------------------------------------------------------ */
/* hex                                                                      */
/* ------------------------------------------------------------------------ */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Lowercase, no separators, no prefix. `out` must hold 2*n+1 bytes. */
static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char kDigits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[2u * i]      = kDigits[(in[i] >> 4) & 0x0Fu];
        out[2u * i + 1u] = kDigits[in[i] & 0x0Fu];
    }
    out[2u * n] = '\0';
}

/* Returns the byte count written, or -1 if `text` is not a whole number of
 * hex bytes or does not fit. An EMPTY string decodes to 0 bytes, which is how
 * an open network is written. */
static int hex_decode(const char *text, uint8_t *out, size_t out_cap)
{
    size_t n = strlen(text);
    size_t i;

    if ((n & 1u) != 0u || (n / 2u) > out_cap) {
        return -1;
    }
    for (i = 0; i < n / 2u; i++) {
        int hi = hex_nibble(text[2u * i]);
        int lo = hex_nibble(text[2u * i + 1u]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2u);
}

/* ------------------------------------------------------------------------ */
/* load                                                                     */
/* ------------------------------------------------------------------------ */

int apad_nds_config_load(char *ip_out, size_t ip_cap,
                         char *port_out, size_t port_cap,
                         char *ssid_out, size_t ssid_cap)
{
    FILE *f;
    char line[CFG_LINE_MAX];
    char ssid[CFG_VAL_MAX];
    char keyhex[CFG_HEX_MAX];
    int have_ip = 0, have_port = 0, have_ssid = 0;
    int have_sec = 0, have_key = 0;
    int sec = -1;

    if (ip_out == NULL || ip_cap == 0u) {
        return 0;
    }
    ip_out[0] = '\0';
    if (!s_mounted) {
        return 0;
    }

    f = fopen(s_path, "r");
    if (f == NULL) {
        return 0;
    }

    ssid[0] = '\0';
    keyhex[0] = '\0';

    while (fgets(line, sizeof line, f) != NULL) {
        size_t n = strlen(line);

        while (n > 0u && (line[n - 1u] == '\n' || line[n - 1u] == '\r')) {
            line[--n] = '\0';
        }
        /* Precisions spelled out as literals so gcc's -Wformat-truncation can
         * see the copy fits -- same reason config_3ds.c does. */
        if (strncmp(line, "ip=", 3) == 0) {
            snprintf(s_rec.ip, sizeof s_rec.ip, "%.39s", line + 3);
            have_ip = 1;
        } else if (strncmp(line, "port=", 5) == 0) {
            snprintf(s_rec.port, sizeof s_rec.port, "%.39s", line + 5);
            have_port = 1;
        } else if (strncmp(line, "ssid=", 5) == 0) {
            snprintf(ssid, sizeof ssid, "%.39s", line + 5);
            have_ssid = 1;
        } else if (strncmp(line, "wifi_sec=", 9) == 0) {
            const char *v = line + 9;

            /* One digit, nothing clever. An out-of-range or non-numeric value
             * leaves sec at -1, i.e. "credentials unknown", which is the same
             * treatment a missing line gets. */
            if (v[0] >= '0' && v[0] <= '3' && v[1] == '\0') {
                sec = v[0] - '0';
                have_sec = 1;
            }
        } else if (strncmp(line, "wifi_key=", 9) == 0) {
            snprintf(keyhex, sizeof keyhex, "%.128s", line + 9);
            have_key = 1;
        }
        /* Anything else -- a future key this build predates, a blank line,
         * garbage -- is skipped. Tolerance is per line, not per file. */
    }
    fclose(f);

    /* -- the network half ------------------------------------------------ */
    if (have_ssid && ssid[0] != '\0' && strlen(ssid) < sizeof s_rec.net.ssid) {
        copy_out(s_rec.net.ssid, sizeof s_rec.net.ssid, ssid);

        if (have_sec) {
            int len = have_key ? hex_decode(keyhex, s_rec.net.key,
                                            sizeof s_rec.net.key)
                               : 0;

            if (len < 0) {
                /* A wifi_key line that is not hex is a corrupt record, and a
                 * corrupt record is treated exactly like a missing one: the
                 * SSID survives (it is still a hint for the picker) but the
                 * credentials do not, so nothing tries to join with a key it
                 * could only have guessed at. */
                s_rec.net.key_len = 0u;
                s_rec.net.security = -1;
            } else {
                s_rec.net.key_len = (size_t)len;
                s_rec.net.security = sec;
            }
        }
    }

    /* The SSID is independent of the address: a user may have joined a
     * network and never reached a server, and remembering the network is
     * still worth doing. Applied before the ip check below for that reason. */
    if (s_rec.net.ssid[0] != '\0' && ssid_out != NULL && ssid_cap > 0u
        && strlen(s_rec.net.ssid) < ssid_cap) {
        copy_out(ssid_out, ssid_cap, s_rec.net.ssid);
    }

    /* -- the address half ------------------------------------------------ */
    if (!have_ip || s_rec.ip[0] == '\0' || strlen(s_rec.ip) >= ip_cap) {
        return 0;
    }
    copy_out(ip_out, ip_cap, s_rec.ip);

    if (have_port && port_out != NULL && port_cap > 0u && s_rec.port[0] != '\0'
        && strlen(s_rec.port) < port_cap) {
        copy_out(port_out, port_cap, s_rec.port);
    }
    return 1;
}

const apad_nds_config_net *apad_nds_config_network(void)
{
    if (s_rec.net.ssid[0] == '\0') {
        return NULL;
    }
    return &s_rec.net;
}

/* ------------------------------------------------------------------------ */
/* save                                                                     */
/* ------------------------------------------------------------------------ */

/* Writes s_rec whole. Both public savers go through this, which is what makes
 * the two halves independent without a re-parse. */
static void write_record(void)
{
    FILE *f;
    char keyhex[CFG_HEX_MAX];
    int rc;

    if (!s_mounted) {
        return;
    }

    /* EEXIST is the expected case on every save after the first; any other
     * mkdir() failure is caught by the fopen() below, which is what actually
     * decides whether this save happens. */
    (void)mkdir(s_dir, 0777);

    f = fopen(s_tmp, "w");
    if (f == NULL) {
        return;
    }

    rc = fprintf(f, "ip=%s\nport=%s\nssid=%s\n", s_rec.ip, s_rec.port,
                 s_rec.net.ssid);
    if (rc >= 0 && s_rec.net.ssid[0] != '\0' && s_rec.net.security >= 0) {
        /* Only written when they are actually known. A record whose
         * credentials were never captured (or were read back corrupt) keeps
         * the old two-line shape, and reads back as "SSID known, credentials
         * not" on the next boot -- which is exactly what it is. */
        hex_encode(s_rec.net.key, s_rec.net.key_len, keyhex);
        rc = fprintf(f, "wifi_sec=%d\nwifi_key=%s\n", s_rec.net.security,
                     keyhex);
    }
    if (rc < 0) {
        fclose(f);
        remove(s_tmp);
        return;
    }
    if (fclose(f) != 0) {
        remove(s_tmp);
        return;
    }
    /* THE RENAME CANNOT OVERWRITE ON THIS PLATFORM, and that is not a detail:
     * it silently broke every save after the first.
     *
     * BlocksDS's FAT is FatFs, and rename() lands on f_rename() through
     * libnds's fat_rename() (libnds source/arm9/libc/fat_device.c). FatFs
     * documents that "any object with this name except old_name must not be
     * exist, or the function fails with FR_EXIST" -- unlike POSIX rename(),
     * and unlike the 3DS's sdmc, which is why config_3ds.c never needed this.
     * MEASURED under melonDS before it was understood: the first save of a
     * launch landed and the second (the address, saved a second later on the
     * session's ACTIVE edge) left the file holding only the first save's
     * fields, because this rename returned EEXIST and the code below then
     * tidily deleted the new data.
     *
     * So: try the atomic rename FIRST (it is atomic where the filesystem
     * allows it, and this file keeps working unchanged if libnds ever gains
     * POSIX semantics), and only if that fails remove the old file and rename
     * again. The window between the remove and the rename is the one moment a
     * power cut loses the record -- accepted, because the alternative measured
     * above is a file that can never be updated at all. */
    if (rename(s_tmp, s_path) != 0) {
        (void)remove(s_path);
        if (rename(s_tmp, s_path) != 0) {
            /* Failure is silent by design: there is nowhere on this client to
             * report it and nothing about a controller session that should
             * hinge on it. Clean up so the next save does not find a stale
             * .tmp. */
            remove(s_tmp);
        }
    }
}

void apad_nds_config_save_address(const char *ip, const char *port)
{
    if (!s_mounted) {
        return;
    }
    if (ip == NULL || ip[0] == '\0' || port == NULL || port[0] == '\0') {
        /* Never write a half-filled address over a good one. */
        return;
    }
    if (strlen(ip) >= sizeof s_rec.ip || strlen(port) >= sizeof s_rec.port) {
        return;
    }
    copy_out(s_rec.ip, sizeof s_rec.ip, ip);
    copy_out(s_rec.port, sizeof s_rec.port, port);
    write_record();
}

void apad_nds_config_save_network(const char *ssid, const void *key,
                                  size_t key_len, int security)
{
    if (!s_mounted) {
        return;
    }
    if (ssid == NULL || ssid[0] == '\0'
        || strlen(ssid) >= sizeof s_rec.net.ssid) {
        return;
    }
    if (key_len > sizeof s_rec.net.key || (key == NULL && key_len > 0u)) {
        return;
    }
    if (security < 0 || security > 3) {
        return;
    }

    /* Replaces the previous network outright -- config_nds.h's "choosing a
     * different network in the picker forgets the old one" rule. There is no
     * list of networks and deliberately no UI to delete one. */
    memset(&s_rec.net, 0, sizeof s_rec.net);
    copy_out(s_rec.net.ssid, sizeof s_rec.net.ssid, ssid);
    if (key_len > 0u) {
        memcpy(s_rec.net.key, key, key_len);
    }
    s_rec.net.key_len = key_len;
    s_rec.net.security = security;
    write_record();
}
