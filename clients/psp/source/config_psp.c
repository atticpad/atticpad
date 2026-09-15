/*
 * clients/psp/source/config_psp.c -- ms0:/atticpad/atticpad.cfg.
 *
 * Plain text, one key=value per line, unknown keys skipped (a future build's
 * key must not make an older build discard the file). Keys: ip, port, slot.
 *
 * THE WRITE IS A DIRECT TRUNCATE, not the 3DS's tmp+rename. The 3DS's sdmc
 * rename() overwrites; the DS's FatFs rename() does NOT (config_nds.c's
 * hard-won note), and the PSP's sceIoRename() behind newlib is the same FAT
 * family with the same doubt. Rather than depend on which way it falls, this
 * writes the final file with fopen("w"), which truncates in place and needs
 * no rename at all. The cost is that a crash mid-write could leave a short
 * file; apad_psp_config_load() already tolerates that, taking only the lines
 * it can parse and ignoring the rest.
 *
 * ONE MODULE-STATIC RECORD, so the two writers (slot on radio-up, ip/port on
 * session-active) each rewrite the whole file from the merged record and
 * neither drops the other's field. Parsed once by load(); a writer called
 * before load() still works (the record starts empty) but main.c loads first.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "config_psp.h"

#define CFG_DIR   "ms0:/atticpad"
#define CFG_PATH  "ms0:/atticpad/atticpad.cfg"
#define CFG_VAL_MAX 32

static struct {
    char ip[CFG_VAL_MAX];
    char port[CFG_VAL_MAX];
    int  slot;
    int  loaded;
} g_rec;

static void write_record(void)
{
    FILE *f;

    /* Nothing worth a file yet: no address and no slot. Do not create an
     * empty file that the next load would treat as a (harmless) corrupt one. */
    if (g_rec.ip[0] == '\0' && g_rec.slot <= 0) {
        return;
    }

    (void)mkdir(CFG_DIR, 0777);   /* EEXIST after the first save is expected */

    f = fopen(CFG_PATH, "w");     /* "w" truncates: no rename needed */
    if (f == NULL) {
        return;
    }
    if (g_rec.ip[0] != '\0') {
        fprintf(f, "ip=%s\n", g_rec.ip);
    }
    if (g_rec.port[0] != '\0') {
        fprintf(f, "port=%s\n", g_rec.port);
    }
    if (g_rec.slot > 0) {
        fprintf(f, "slot=%d\n", g_rec.slot);
    }
    fclose(f);
}

int apad_psp_config_load(char *ip_out, size_t ip_cap,
                         char *port_out, size_t port_cap)
{
    FILE *f;
    char line[64];

    memset(&g_rec, 0, sizeof g_rec);
    g_rec.loaded = 1;
    if (ip_out != NULL && ip_cap > 0) {
        ip_out[0] = '\0';
    }

    f = fopen(CFG_PATH, "r");
    if (f == NULL) {
        return 0;                 /* fresh stick: empty address, lowest slot */
    }
    while (fgets(line, sizeof line, f) != NULL) {
        size_t n = strlen(line);

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (strncmp(line, "ip=", 3) == 0) {
            snprintf(g_rec.ip, sizeof g_rec.ip, "%.31s", line + 3);
        } else if (strncmp(line, "port=", 5) == 0) {
            snprintf(g_rec.port, sizeof g_rec.port, "%.31s", line + 5);
        } else if (strncmp(line, "slot=", 5) == 0) {
            long v = strtol(line + 5, NULL, 10);
            if (v >= 1 && v <= 999) {
                g_rec.slot = (int)v;
            }
        }
        /* any other line silently skipped */
    }
    fclose(f);

    if (ip_out != NULL && g_rec.ip[0] != '\0' && strlen(g_rec.ip) < ip_cap) {
        snprintf(ip_out, ip_cap, "%s", g_rec.ip);
        if (port_out != NULL && port_cap > 0 && g_rec.port[0] != '\0'
            && strlen(g_rec.port) < port_cap) {
            snprintf(port_out, port_cap, "%s", g_rec.port);
        }
    } else if (ip_out != NULL) {
        /* A slot without a usable ip is fine; leave the address empty and
         * still return the slot below. */
        ip_out[0] = '\0';
    }
    return g_rec.slot;
}

void apad_psp_config_save_slot(int slot)
{
    if (slot <= 0) {
        return;
    }
    if (!g_rec.loaded) {
        memset(&g_rec, 0, sizeof g_rec);
        g_rec.loaded = 1;
    }
    if (g_rec.slot == slot) {
        return;                   /* unchanged: no write */
    }
    g_rec.slot = slot;
    write_record();
}

void apad_psp_config_save_server(const char *ip, const char *port)
{
    if (ip == NULL || ip[0] == '\0' || port == NULL || port[0] == '\0') {
        return;
    }
    if (!g_rec.loaded) {
        memset(&g_rec, 0, sizeof g_rec);
        g_rec.loaded = 1;
    }
    if (strcmp(g_rec.ip, ip) == 0 && strcmp(g_rec.port, port) == 0) {
        return;                   /* unchanged: no write */
    }
    snprintf(g_rec.ip, sizeof g_rec.ip, "%.31s", ip);
    snprintf(g_rec.port, sizeof g_rec.port, "%.31s", port);
    write_record();
}
