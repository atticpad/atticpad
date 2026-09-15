/* clients/nds/source/screen_wifi.c
 *
 * THE ONE SCREEN THE 3DS CLIENT DOES NOT HAVE. On a 3DS the network is up
 * before main() gets control; here, associating with an access point is a
 * user-visible operation that takes seconds, can fail, and may need a key
 * typed in. So it is a screen, with the same shape as every other one in this
 * client (four callbacks, an _ARM substate before anything blocking).
 *
 * THE ORDER, and why:
 *
 *   1. THE NETWORK THIS CLIENT ITSELF JOINED LAST TIME, key and all
 *      (config_nds.h), whenever one was saved. FIRST, ahead of the firmware's
 *      slots, because it is the only step whose outcome this client controls
 *      end to end: it knows the SSID, it knows the key, and DSWiFi will look
 *      for exactly that AP and nothing else.
 *
 *      This step is why the key is saved at all. Before it, every boot went
 *      through Wifi_AutoConnect(), which on the reporting user's DSi joined
 *      only SOMETIMES and otherwise dropped them into the picker to retype a
 *      WPA2 key -- see this file's SAVED_SEARCH_MS comment for what the
 *      library is actually doing during that "sometimes".
 *
 *   2. THE FIRMWARE'S OWN WFC SLOTS (Wifi_AutoConnect()), whenever there is
 *      at least one. It is the network the console's owner already configured
 *      in System Settings, and it needs no picker and no typing. The SDK
 *      sample's option A. Still reached on every boot where step 1 has
 *      nothing saved, and as the fallback when step 1 gives up.
 *
 *   3. THE SCAN LIST on failure, or on B, or when there are no slots. The
 *      sample's option B: Wifi_ScanMode(), then Wifi_GetNumAP()/
 *      Wifi_GetAPData() polled per frame, then Wifi_ConnectOpenAP() or
 *      Wifi_ConnectSecureAP() with a key.
 *
 * A FAILED STEP 1 NEVER DELETES THE CREDENTIALS. An access point that is
 * switched off, out of range or rebooting is not a wrong key, and forgetting
 * a network because the router was unplugged would recreate exactly the
 * problem this change exists to fix. The saved record is replaced only by
 * SUCCESSFULLY joining a different one from the picker.
 *
 * WHY AN UNJOINABLE NETWORK IS DRAWN AND NOT HIDDEN. A WPA/WPA2 AP cannot be
 * joined by a DS-mode radio at all -- that is the hardware (docs/SETUP-DS.md),
 * not a missing feature -- and DSWiFi says so through WFLAG_APDATA_COMPATIBLE.
 * Hiding those rows would leave a person staring at a list that does not
 * contain their own home network with no idea why. They are drawn dim, with
 * the reason on the row, and are not tappable: the same treatment
 * screen_session.c gives a mode tab the server did not offer and
 * ui_widgets.c gives a button the console does not have.
 *
 * THE BOOT COMBO LIVES HERE, because this is now the first screen. Held
 * L+R+Start during the ~2.5 s window, or SELECT at any time, opens the
 * self-test -- which needs no network, and on this console is the one thing
 * still worth doing when the radio will not come up.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "config_nds.h"
#include "kbd_nds.h"
#include "wifi_nds.h"

/* ------------------------------------------------------------------------ */
/* substates                                                                */
/* ------------------------------------------------------------------------ */

enum {
    WS_BOOT = 0,     /* L+R+Start window, ~2.5s                             */
    WS_SAVED_ARM,    /* draw "joining <ssid>" before issuing the join        */
    WS_SAVED,        /* saved SSID+key issued, polling assoc                */
    WS_AUTO_ARM,     /* draw "trying your saved network" before blocking     */
    WS_AUTO,         /* Wifi_AutoConnect() issued, polling assoc            */
    WS_SCAN,         /* the picker                                          */
    WS_KEY,          /* typing a network key on the libnds keyboard         */
    WS_JOIN,         /* connect issued, polling assoc                       */
    WS_DEAD          /* the radio never came up; self-test only             */
};

static int s_sub;
static int s_boot_frames;

/* The saved network for this launch, or NULL. Read once in wifi_enter():
 * config_nds.c hands out a pointer into its own record and that record is
 * rewritten by a successful picker join, so re-reading it mid-screen would
 * mean the fallback path could see the network it just replaced. */
static const apad_nds_config_net *s_saved;
static int      s_saved_tried;      /* step 1 ran this launch (pass or fail) */
static uint32_t s_saved_start;      /* apad_ticks_ms() when the join began   */
static int      s_auto_retried;     /* the one extra Wifi_AutoConnect()      */
static int      s_picker_rejoined;  /* the picker's own auto-join has fired   */

/* HOW LONG STEP 1 IS GIVEN BEFORE FALLING BACK -- and it bounds only the
 * SEARCHING half of the join, never the whole thing.
 *
 * Neither Wifi_ConnectSecureAP() nor Wifi_AutoConnect() has a timeout of its
 * own: read out of dswifi's source/arm9/access_point.c, Wifi_AssocStatus()
 * returns ASSOCSTATUS_SEARCHING on every poll, forever, until a beacon
 * matching the requested SSID turns up. The bound has to come from here.
 *
 * 15 SECONDS, AND THE NUMBER IS DERIVED, not picked. dswifi's ARM7 scan loop
 * (source/arm7/ntr/update.c, WIFIMODE_SCAN) advances one step whenever more
 * than one tick of W_US_COUNT1 has passed, and that counter's tick is 65.5 ms;
 * one step is either a channel change or one probe request, and a full cycle
 * is 13 channels x (2 + number of firmware WFC slots) steps. That is roughly
 * 5 s with no WFC slots and roughly 9 s with three -- so anything under ten
 * seconds can give up in the middle of the FIRST sweep, before the radio has
 * even listened on the AP's channel. 15 s buys a complete sweep plus margin on
 * the slowest configuration, and still leaves the whole ladder (15 s here +
 * wifi_nds.c's 30 s association timeout on the AutoConnect step) bounded.
 *
 * It deliberately does NOT bound authenticating/associating/DHCP: a WPA2 join
 * where the ARM7 has to derive the PMK from a passphrase sits in ASSOCIATING
 * for about five seconds by dswifi's own reckoning, and cutting that off would
 * fail exactly the case this feature exists for. apad_nds_wifi_searching()
 * draws that line; wifi_nds.c's ASSOC_TIMEOUT_MS owns the rest. */
#define SAVED_SEARCH_MS 15000u

#define BOOT_FRAMES 150   /* ~2.5s at 60Hz, sampled across many frames --
                           * keysHeld() can read 0 on the first scan or two
                           * after bring-up, so a single-frame check silently
                           * misses a combo that WAS held the whole time. */

/* ------------------------------------------------------------------------ */
/* the scan list                                                            */
/* ------------------------------------------------------------------------ */

#define AP_MAX      16
#define AP_ROWS      6

static apad_nds_ap s_aps[AP_MAX];
static int s_ap_count;
static int s_scroll;
static int s_sel;            /* index into s_aps of the AP being joined     */

/* The key being typed. NOT in app_ctx: the pairing PIN's rule (config_nds.h)
 * still applies to the ONE screen-to-screen context struct, so this lives
 * here, is handed to DSWiFi as bytes, and is wiped as soon as the join is
 * issued. 64 is Wifi_ConnectSecureAP()'s own WPA ceiling.
 *
 * (What has changed since this comment was first written is only that the
 * bytes now also reach the SD card, deliberately -- see config_nds.h. They
 * still never reach app_ctx, a banner, or the top screen.) */
static char s_key[65];
static uint8_t s_keybytes[64];

/* The credentials of the join CURRENTLY IN FLIGHT, held from begin_join()
 * until the radio says ASSOCIATED. Separate from s_key/s_keybytes because
 * those are wiped the moment the join is issued, and what gets written to the
 * card has to be what actually WORKED -- a key that failed must never replace
 * a saved one that did not. */
static char    s_pending_ssid[33];
static uint8_t s_pending_key[APAD_NDS_KEY_MAX];
static size_t  s_pending_len;
static int     s_pending_sec = -1;

/* ------------------------------------------------------------------------ */
/* layout -- 3DS bottom-screen (320x240) coordinates, ui.c scales           */
/* ------------------------------------------------------------------------ */

static const float kListTop  = 28.0f;
static const float kRowH     = 27.0f;
static const ui_box kRescanBtn = {   6.0f, 200.0f, 100.0f, 32.0f };
static const ui_box kJoinBtn   = { 214.0f, 200.0f, 100.0f, 32.0f };

static void row_box(int i, ui_box *out)
{
    out->x = 6.0f;
    out->y = kListTop + (float)i * kRowH;
    out->w = 308.0f;
    out->h = kRowH - 3.0f;
}

/* ------------------------------------------------------------------------ */
/* WEP / WPA key encoding                                                   */
/* ------------------------------------------------------------------------ */

/* Wifi_ConnectSecureAP() takes RAW KEY BYTES: "For WEP networks it must be 5,
 * 13 or 16 bytes long. For WPA networks it must be at most 64 bytes long."
 * (dswifi9.h). What a person types is text, and for WEP that text is
 * conventionally EITHER 5/13 ASCII characters OR 10/26 hex digits -- the same
 * key either way. Deciding between them cannot be left to the user on a
 * console with no room for a radio button, so the rule is:
 *
 *   a string of exactly 10 or 26 characters that is ALL hex digits is
 *   decoded as hex; anything else is taken as ASCII bytes verbatim.
 *
 * The ambiguity is real and bounded: a 10-character ASCII WEP passphrase made
 * only of the letters a-f and digits would be misread as hex. That is not a
 * valid ASCII WEP key length anyway (5, 13 and 16 are), so the misreading
 * cannot happen for a key the hardware would have accepted as ASCII. Recorded
 * here rather than left implicit -- UNVERIFIED AGAINST A REAL WEP AP; see the
 * report.
 *
 * Returns the byte count, or 0 if the text cannot be a key at all. */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t encode_key(const char *text, int security, uint8_t *out,
                         size_t out_cap)
{
    size_t n = strlen(text);
    size_t i;

    if (n == 0u) {
        return 0u;
    }

    if (security == APAD_AP_WEP && (n == 10u || n == 26u)) {
        int all_hex = 1;

        for (i = 0; i < n; i++) {
            if (hex_nibble(text[i]) < 0) {
                all_hex = 0;
                break;
            }
        }
        if (all_hex && (n / 2u) <= out_cap) {
            for (i = 0; i < n / 2u; i++) {
                out[i] = (uint8_t)((hex_nibble(text[2u * i]) << 4)
                                   | hex_nibble(text[2u * i + 1u]));
            }
            return n / 2u;
        }
    }

    if (security == APAD_AP_WEP && n != 5u && n != 13u && n != 16u) {
        return 0u;   /* the radio would reject it; say so before trying */
    }
    if (n > out_cap) {
        return 0u;
    }
    memcpy(out, text, n);
    return n;
}

static void wipe_key(void)
{
    memset(s_key, 0, sizeof s_key);
    memset(s_keybytes, 0, sizeof s_keybytes);
}

static void wipe_pending(void)
{
    memset(s_pending_ssid, 0, sizeof s_pending_ssid);
    memset(s_pending_key, 0, sizeof s_pending_key);
    s_pending_len = 0u;
    s_pending_sec = -1;
}

/* ------------------------------------------------------------------------ */
/* the boot ladder                                                          */
/* ------------------------------------------------------------------------ */

/* Can step 1 (the saved network) be attempted at all? */
static int saved_usable(app_ctx *ctx, const apad_nds_wifi_status *w)
{
    if (s_saved == NULL || s_saved->ssid[0] == '\0') {
        return 0;
    }
    if (s_saved->security < 0) {
        /* A record written by a build that predates credential saving: the
         * SSID is known and the key is NOT. Guessing "no wifi_key line means
         * open" would spend the whole SAVED_SEARCH_MS budget failing to join
         * a WPA2 network as if it were open, every boot. The old path (the
         * firmware slots, then the picker, which still offers this SSID
         * first) is exactly right for it, and the first successful picker
         * join upgrades the record in place. */
        return 0;
    }
    if ((s_saved->security == APAD_AP_WPA || s_saved->security == APAD_AP_WPA2)
        && !w->dsi_mode) {
        /* HARDWARE, not a missing feature: the DS-mode radio cannot do WPA at
         * all (docs/SETUP-DS.md). Reached when the card moves to a DS or a DS
         * Lite, or when L was held at boot to force DS mode. Say so rather
         * than spending fifteen seconds failing silently. */
        app_note(ctx, 1, "\"%s\" needs DSi mode", s_saved->ssid);
        return 0;
    }
    return 1;
}

/* THE BOOT LADDER, in one place so every entry into it takes the same steps in
 * the same order. `done` is how many steps have already had their turn:
 * 0 = none, 1 = the saved network, 2 = the firmware's WFC slots. A step with
 * nothing to do is skipped, and the picker is the floor. */
static int boot_ladder(app_ctx *ctx, int done)
{
    apad_nds_wifi_status w;

    apad_nds_wifi_get(&w);

    if (done < 1 && saved_usable(ctx, &w)) {
        return WS_SAVED_ARM;
    }
    if (done < 2 && w.wfc_slots > 0) {
        return WS_AUTO_ARM;
    }
    apad_nds_wifi_begin_scan();
    return WS_SCAN;
}

/* ------------------------------------------------------------------------ */
/* screen callbacks                                                         */
/* ------------------------------------------------------------------------ */

static void wifi_enter(app_ctx *ctx)
{
    apad_nds_wifi_status w;

    s_scroll = 0;
    s_sel = -1;
    s_ap_count = 0;
    s_saved_tried = 0;
    s_auto_retried = 0;
    s_picker_rejoined = 0;
    wipe_key();
    wipe_pending();
    apad_kbd_hide();

    /* Read ONCE. config_nds.c hands back a pointer into its own record and a
     * successful picker join rewrites that record, so a later re-read could
     * hand the fallback path the network it had just replaced. */
    s_saved = apad_nds_config_network();

    apad_nds_wifi_get(&w);
    if (w.stage == APAD_WIFI_FAILED && w.ready == 0 && w.assoc < 0
        && w.where[0] != '\0' && strcmp(w.where, "Wifi_InitDefault") == 0) {
        s_sub = WS_DEAD;
        return;
    }
    if (ctx->want_boot_combo) {
        s_sub = WS_BOOT;
        s_boot_frames = BOOT_FRAMES;
        return;
    }
    s_sub = boot_ladder(ctx, 0);
}

/* Everything that happens once the radio says ASSOCIATED, in one place so
 * both the WFC path and the picker path go through it. */
static apad_screen_id wifi_ready(app_ctx *ctx)
{
    apad_nds_wifi_status w;

    apad_nds_wifi_get(&w);
    ctx->wifi_ready = 1;
    if (w.ssid[0] != '\0') {
        snprintf(ctx->ssid, sizeof ctx->ssid, "%s", w.ssid);
    }

    /* THE MOMENT THE CREDENTIALS ARE PERSISTED: association, not a server
     * session. Waiting for the connect screen to reach ACTIVE (which is when
     * the ADDRESS is saved, screen_session.c) would lose the network on every
     * boot where the user joins Wi-Fi and never finds a server -- the exact
     * boot where being asked for the key again hurts most. s_pending_* is
     * empty on the WFC and saved-network paths, so only a PICKER join
     * (begin_join()) writes here, and only after it worked. */
    if (s_pending_ssid[0] != '\0' && s_pending_sec >= 0) {
        apad_nds_config_save_network(s_pending_ssid,
                                     (s_pending_len > 0u) ? s_pending_key
                                                          : NULL,
                                     s_pending_len, s_pending_sec);
    }
    wipe_pending();
    wipe_key();
    apad_kbd_hide();

    /* THE WFC PATH CANNOT NAME THE NETWORK IT JOINED. DSWiFi's
     * Wifi_GetData() offers only MACADDRESS, NUMWFCAPS and RSSI
     * (dswifi9.h's WIFIGETDATA enum -- checked, not recalled), and
     * Wifi_AutoConnect() takes no argument and reports nothing back, so
     * after the firmware's own slots connect there is no API that says
     * which SSID won. w.ssid is only filled on the picker path, where
     * this client chose the name itself. */

    /* The socket cannot exist before there is an interface to bind it to, so
     * the session engine is created here rather than in bring-up. */
    if (!app_client_start(ctx)) {
        return APAD_SCREEN_FATAL;
    }
    if (ctx->ssid[0] != '\0') {
        app_note(ctx, 0, "Joined \"%s\" -- %s", ctx->ssid, w.ip);
    } else {
        app_note(ctx, 0, "Joined the console's network -- %s", w.ip);
    }
    return APAD_SCREEN_CONNECT;
}

static void begin_join(app_ctx *ctx, int idx)
{
    size_t len = 0u;

    (void)ctx;
    if (idx < 0 || idx >= s_ap_count) {
        return;
    }
    if (s_aps[idx].security != APAD_AP_OPEN) {
        len = encode_key(s_key, s_aps[idx].security, s_keybytes,
                         sizeof s_keybytes);
        if (len == 0u) {
            /* Shortened from "That key is not a valid %s key" -- with a long
             * security-name suffix (WPA2-PSK etc.) the original ran past
             * this screen's 256px banner width. */
            app_note(ctx, 2, "not a valid %s key",
                     apad_nds_wifi_security_name(s_aps[idx].security));
            s_sub = WS_KEY;
            return;
        }
    }
    if (apad_nds_wifi_join(s_aps[idx].ssid, (len > 0u) ? s_keybytes : NULL,
                           len) != 0) {
        /* Shortened from "Could not start joining \"%s\"" for the same
         * reason -- an SSID can be up to 32 characters on its own. */
        app_note(ctx, 2, "could not join \"%s\"", s_aps[idx].ssid);
        s_sub = WS_SCAN;
        apad_nds_wifi_begin_scan();
        return;
    }
    /* Held until the radio says ASSOCIATED; see wifi_ready(). Copied before
     * wipe_key() clears the source. */
    snprintf(s_pending_ssid, sizeof s_pending_ssid, "%s", s_aps[idx].ssid);
    if (len > 0u) {
        memcpy(s_pending_key, s_keybytes, len);
    }
    s_pending_len = len;
    s_pending_sec = s_aps[idx].security;

    wipe_key();
    apad_kbd_hide();
    s_sub = WS_JOIN;
}

static apad_screen_id wifi_update(app_ctx *ctx)
{
    apad_nds_wifi_status w;

    /* SELECT is the self-test's only reliable route on the sibling console
     * (a shoulder button can be physically dead), and this screen is the one
     * the boot combo runs on, so it is honoured in every substate. */
    if (app_pressed(ctx, KEY_SELECT)) {
        ctx->selftest_return = APAD_SCREEN_WIFI;
        ctx->want_boot_combo = 0;
        return APAD_SCREEN_SELFTEST;
    }

    apad_nds_wifi_poll();
    apad_nds_wifi_get(&w);

    switch (s_sub) {
    case WS_DEAD:
        return APAD_SCREEN_WIFI;

    case WS_BOOT:
        /* Level-triggered on purpose: this combo is HELD by definition and
         * needs no released-once gate. */
        if ((ctx->keys_held & (KEY_L | KEY_R | KEY_START))
            == (KEY_L | KEY_R | KEY_START)) {
            ctx->want_boot_combo = 0;
            ctx->selftest_return = APAD_SCREEN_WIFI;
            return APAD_SCREEN_SELFTEST;
        }
        if (ctx->touch_pressed || app_pressed(ctx, KEY_A | KEY_B)) {
            ctx->want_boot_combo = 0;
            s_boot_frames = 0;
        }
        if (--s_boot_frames <= 0) {
            ctx->want_boot_combo = 0;
            s_sub = boot_ladder(ctx, 0);
        }
        return APAD_SCREEN_WIFI;

    case WS_SAVED_ARM:
        /* One drawn frame before the join, so "Joining <ssid>" is on screen
         * before anything can take seconds. Same shape as WS_AUTO_ARM. */
        s_saved_tried = 1;
        s_saved_start = apad_ticks_ms();
        if (s_saved == NULL
            || apad_nds_wifi_join_saved(s_saved->ssid,
                                        (s_saved->key_len > 0u) ? s_saved->key
                                                                : NULL,
                                        s_saved->key_len) != 0) {
            /* DSWiFi refused the arguments outright -- nothing was started,
             * so there is nothing to abort. */
            app_note(ctx, 1, "could not start joining \"%s\"",
                     (s_saved != NULL) ? s_saved->ssid : "");
            s_sub = boot_ladder(ctx, 1);
            return APAD_SCREEN_WIFI;
        }
        s_sub = WS_SAVED;
        return APAD_SCREEN_WIFI;

    case WS_SAVED:
        if (w.stage == APAD_WIFI_READY) {
            return wifi_ready(ctx);
        }
        if (w.stage == APAD_WIFI_FAILED || app_pressed(ctx, KEY_B)
            || (apad_nds_wifi_searching()
                && apad_time_since(apad_ticks_ms(), s_saved_start)
                       > SAVED_SEARCH_MS)) {
            /* THE CREDENTIALS STAY ON THE CARD. An AP that is switched off,
             * out of range or still booting is not a wrong key, and this is
             * also the path a user takes by pressing B to reach the picker
             * on purpose. Only a successful join of a DIFFERENT network
             * replaces the record.
             *
             * The abort is not cosmetic: without it DSWiFi keeps hunting for
             * this SSID in the background and would associate with it later,
             * behind whatever screen the client had moved on to. */
            apad_nds_wifi_abort();
            app_note(ctx, 1, "\"%s\" not found -- looking for others",
                     (s_saved != NULL) ? s_saved->ssid : "");
            s_sub = boot_ladder(ctx, 1);
        }
        return APAD_SCREEN_WIFI;

    case WS_AUTO_ARM:
        /* One drawn frame before Wifi_AutoConnect(), so "using the console's
         * Wi-Fi settings" is on screen before anything can take seconds. */
        s_sub = WS_AUTO;
        apad_nds_wifi_begin_autoconnect();
        return APAD_SCREEN_WIFI;

    case WS_AUTO:
        if (w.stage == APAD_WIFI_READY) {
            return wifi_ready(ctx);
        }
        if (w.stage == APAD_WIFI_FAILED || app_pressed(ctx, KEY_B)) {
            /* ONE EXTRA ATTEMPT, and only for DSWiFi's own CANNOTCONNECT.
             * Worth doing because that verdict costs nothing to reach and can
             * be a transient: the association itself was attempted and
             * refused, so a second Wifi_AutoConnect() re-runs the scan from
             * scratch for a couple of seconds. NOT worth doing after
             * wifi_nds.c's own 30 s association timeout (apad_nds_wifi_
             * rejected() is 0 there) -- retrying that would cost another half
             * minute to reach the same place, and it is the timeout, not the
             * library, that gave up. Not applied to the picker's WS_JOIN
             * either: there a person is watching and can press the row again.
             */
            if (w.stage == APAD_WIFI_FAILED && apad_nds_wifi_rejected()
                && !s_auto_retried) {
                s_auto_retried = 1;
                apad_nds_wifi_begin_autoconnect();
                return APAD_SCREEN_WIFI;
            }
            /* The firmware's APs are not reachable from here (or the user
             * said so). Fall back to the picker -- the sample's option B. */
            apad_nds_wifi_abort();
            s_sub = boot_ladder(ctx, 2);
        }
        return APAD_SCREEN_WIFI;

    case WS_SCAN: {
        int i;

        s_ap_count = apad_nds_wifi_scan_list(s_aps, AP_MAX);
        if (s_scroll > s_ap_count - AP_ROWS) {
            s_scroll = s_ap_count - AP_ROWS;
        }
        if (s_scroll < 0) {
            s_scroll = 0;
        }

        if (app_pressed(ctx, KEY_UP) && s_scroll > 0) {
            s_scroll--;
        }
        if (app_pressed(ctx, KEY_DOWN) && s_scroll + AP_ROWS < s_ap_count) {
            s_scroll++;
        }

        /* THE NETWORK THIS CONSOLE CHOSE LAST TIME, for the one case the
         * saved-credentials step above cannot cover: a record written by a
         * build that predates the key being saved (SSID only, see
         * saved_usable()). Open networks only -- with no key there is nothing
         * else this can do -- and the first successful join through here
         * writes a full record, so a given console takes this path at most
         * once in its life.
         *
         * ONCE PER VISIT TO THIS SCREEN (s_picker_rejoined). Without that
         * guard a join that fails lands back in WS_SCAN, matches again, and
         * retries forever, with the list unusable in between: the user can
         * never reach the row they actually want. Skipped entirely when the
         * saved-credentials step has already had its turn, so a network that
         * just failed to answer is not immediately retried by another name.
         */
        if (!s_saved_tried && !s_picker_rejoined && ctx->ssid[0] != '\0') {
            for (i = 0; i < s_ap_count; i++) {
                if (strcmp(s_aps[i].ssid, ctx->ssid) == 0
                    && s_aps[i].security == APAD_AP_OPEN
                    && s_aps[i].compatible) {
                    s_sel = i;
                    s_picker_rejoined = 1;
                    begin_join(ctx, i);
                    return APAD_SCREEN_WIFI;
                }
            }
        }

#ifdef APAD_AUTO_AP
        /* Dev hook: a headless run has nobody to tap a row. Joins the named
         * network as soon as it appears, open networks only -- a key would
         * need typing, which is the thing this hook exists to avoid. */
        for (i = 0; i < s_ap_count; i++) {
            if (strcmp(s_aps[i].ssid, APAD_AUTO_AP) == 0
                && s_aps[i].security == APAD_AP_OPEN
                && s_aps[i].compatible) {
                s_sel = i;
                begin_join(ctx, i);
                return APAD_SCREEN_WIFI;
            }
        }
#endif

        if (ctx->touch_pressed) {
            int px = (int)ctx->touch.px, py = (int)ctx->touch.py;

            if (ui_box_hit(&kRescanBtn, px, py)) {
                apad_nds_wifi_begin_scan();
                s_scroll = 0;
                return APAD_SCREEN_WIFI;
            }
            for (i = 0; i < AP_ROWS; i++) {
                ui_box b;
                int idx = s_scroll + i;

                if (idx >= s_ap_count) {
                    break;
                }
                row_box(i, &b);
                if (!ui_box_hit(&b, px, py)) {
                    continue;
                }
                if (!s_aps[idx].compatible) {
                    /* Drawn but not tappable -- see this file's header. The
                     * message says why rather than doing nothing silently.
                     * Shortened from "%s needs DSi mode -- this radio cannot
                     * join it", which ran past 256px with a long security
                     * name. */
                    app_note(ctx, 1, "%s needs DSi mode",
                             apad_nds_wifi_security_name(s_aps[idx].security));
                    return APAD_SCREEN_WIFI;
                }
                s_sel = idx;
                if (s_aps[idx].security == APAD_AP_OPEN) {
                    begin_join(ctx, idx);
                } else {
                    wipe_key();
                    apad_kbd_show();
                    s_sub = WS_KEY;
                }
                return APAD_SCREEN_WIFI;
            }
        }
        return APAD_SCREEN_WIFI;
    }

    case WS_KEY: {
        int key = apad_kbd_poll();

        if (app_pressed(ctx, KEY_B)) {
            wipe_key();
            apad_kbd_hide();
            s_sub = WS_SCAN;
            return APAD_SCREEN_WIFI;
        }
        if (key == '\n' || app_pressed(ctx, KEY_A)) {
            begin_join(ctx, s_sel);
            return APAD_SCREEN_WIFI;
        }
        (void)apad_kbd_apply(key, s_key, sizeof s_key, NULL);

        /* A tap on the JOIN button, for someone who never looks up from the
         * touchscreen. Only above the keyboard's own area -- below
         * UI_KBD_TOP_Y the contact belongs to libnds. */
        if (ctx->touch_pressed
            && (int)ctx->touch.py < (int)(UI_KBD_TOP_Y * 4.0f / 5.0f)
            && ui_box_hit(&kJoinBtn, (int)ctx->touch.px, (int)ctx->touch.py)) {
            begin_join(ctx, s_sel);
        }
        return APAD_SCREEN_WIFI;
    }

    case WS_JOIN:
        if (w.stage == APAD_WIFI_READY) {
            return wifi_ready(ctx);
        }
        if (w.stage == APAD_WIFI_FAILED) {
            /* Nothing is written: a key that did not work must never replace
             * one that did. */
            wipe_pending();
            apad_nds_wifi_abort();
            app_note(ctx, 2, "Could not join \"%s\" -- %s", w.ssid, w.where);
            s_sub = WS_SCAN;
            apad_nds_wifi_begin_scan();
        }
        return APAD_SCREEN_WIFI;

    default:
        return APAD_SCREEN_WIFI;
    }
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                  */
/* ------------------------------------------------------------------------ */

static void wifi_draw_top(app_ctx *ctx)
{
    apad_nds_wifi_status w;
    const float W = UI_TOP_W - 16.0f;
    const char *right;

    apad_nds_wifi_get(&w);

    switch (s_sub) {
    case WS_BOOT:  right = "starting"; break;
    case WS_SAVED_ARM:
    case WS_SAVED: right = "saved network"; break;
    case WS_AUTO_ARM:
    case WS_AUTO:  right = "console setup"; break;
    case WS_SCAN:  right = "choose a network"; break;
    case WS_KEY:   right = "network key"; break;
    case WS_JOIN:  right = "joining"; break;
    default:       right = "no radio"; break;
    }
    ui_header(UI_TOP_W, ctx->is_dsi ? "AtticPad DSi" : "AtticPad DS", right,
              (s_sub == WS_DEAD) ? ui_c_bad() : ui_c_accent());

    if (s_sub == WS_SAVED_ARM || s_sub == WS_SAVED) {
        /* Named, unlike the WFC path below it, because on this one the client
         * chose the network itself and knows which it is. Wrapped, not
         * shrunk-and-truncated: an SSID can be 32 characters.
         *
         * THE X IS THE CENTRE, not the left edge: ui.c's draw_wrap_line()
         * subtracts half the measured line width for UI_ALIGN_CENTER, exactly
         * as ui_textf_fit() does. Passing a left edge here drew the sentence
         * off the left of the screen -- caught in melonDS, 2026-09-10. */
        ui_textf_wrap(UI_TOP_W * 0.5f, 40.0f, W, UI_S_HEAD, ui_c_text(),
                      UI_ALIGN_CENTER, 0.0f, "Joining %s",
                      (s_saved != NULL) ? s_saved->ssid : "");
    } else {
        ui_textf_fit(UI_TOP_W * 0.5f, 44.0f, UI_S_HEAD, ui_c_text(),
                     UI_ALIGN_CENTER, W, "%s",
                     (s_sub == WS_DEAD) ? "Wi-Fi did not start"
                     : (s_sub == WS_BOOT) ? "AtticPad"
                     : (s_sub == WS_AUTO_ARM || s_sub == WS_AUTO)
                           ? "Trying the console's networks"
                     : (s_sub == WS_JOIN) ? "Joining..."
                     : (s_sub == WS_KEY) ? "Type the network key"
                                         : "Choose a network");
    }

    if (s_sub == WS_DEAD) {
        /* The bottom screen carries the one hint this failure state needs
         * (see wifi_draw_bottom()); repeating it here was the same sentence
         * twice on two screens at once (2026-09-09 decluttering pass). */
        return;
    }

    if (s_sub == WS_SCAN) {
        ui_textf_fit(UI_TOP_W * 0.5f, 78.0f, UI_S_SMALL, ui_c_dim(),
                     UI_ALIGN_CENTER, W, "%d found%s", w.ap_count,
                     (w.ap_count == 0) ? " so far -- still looking" : "");
    } else if (s_sub == WS_JOIN || s_sub == WS_AUTO || s_sub == WS_SAVED) {
        ui_textf_fit(UI_TOP_W * 0.5f, 78.0f, UI_S_SMALL, ui_c_dim(),
                     UI_ALIGN_CENTER, W, "%s",
                     apad_nds_wifi_assoc_name(w.assoc));
    } else if (s_sub == WS_KEY && s_sel >= 0 && s_sel < s_ap_count) {
        ui_textf_fit(UI_TOP_W * 0.5f, 78.0f, UI_S_SMALL, ui_c_dim(),
                     UI_ALIGN_CENTER, W, "%s (%s)", s_aps[s_sel].ssid,
                     apad_nds_wifi_security_name(s_aps[s_sel].security));
    }

    if (ctx->banner[0] != '\0') {
        uint32_t c = (ctx->banner_level >= 2) ? ui_c_bad()
                   : (ctx->banner_level == 1) ? ui_c_warn() : ui_c_dim();

        /* UI_STATUS_BOTTOM (198) is where app_draw_status_top() ends, leaving
         * only ~40 3DS px (32 DS px) to the screen edge -- tight enough that
         * this uses line_height 0.0f (the DS-native 8px-face step) rather
         * than UI_LINE(UI_S_SMALL)'s roomier 15px rhythm, to fit up to three
         * lines of a long banner in that budget instead of two. Wrapped
         * rather than fit-and-truncated because there is no smaller face
         * left to shrink into once 8x8 still does not fit one line. */
        ui_textf_wrap(8.0f, UI_STATUS_BOTTOM + 2.0f, W, UI_S_SMALL, c,
                      UI_ALIGN_LEFT, 0.0f, "%s", ctx->banner);
    }
    if (ctx->show_diag) {
        ui_textf_fit(8.0f, 226.0f, UI_S_TINY, ui_c_border(), UI_ALIGN_LEFT, W,
                     "stage %s  wfc %d  assoc %d  dsi %d",
                     apad_nds_wifi_stage_name(w.stage), w.wfc_slots, w.assoc,
                     w.dsi_mode);
    }
}

static void draw_ap_rows(void)
{
    int i;

    for (i = 0; i < AP_ROWS; i++) {
        int idx = s_scroll + i;
        ui_box b;
        const apad_nds_ap *ap;
        uint32_t ink;

        row_box(i, &b);
        if (idx >= s_ap_count) {
            ui_panel(&b, ui_c_bg(), ui_c_border());
            continue;
        }
        ap = &s_aps[idx];
        ui_panel(&b, ap->compatible ? ui_c_panel() : ui_c_bg(),
                 ui_c_border());
        ink = ap->compatible ? ui_c_text() : ui_c_border();

        ui_textf_fit(b.x + 6.0f, b.y + 2.0f, UI_S_BODY, ink, UI_ALIGN_LEFT,
                     b.w - 90.0f, "%s", ap->ssid);
        ui_textf_fit(b.x + b.w - 6.0f, b.y + 2.0f, UI_S_TINY,
                     ap->compatible ? ui_c_dim() : ui_c_border(),
                     UI_ALIGN_RIGHT, 84.0f, "%s%s",
                     apad_nds_wifi_security_name(ap->security),
                     ap->compatible ? "" : " - needs DSi");
    }
}

static void wifi_draw_bottom(app_ctx *ctx)
{
    ui_header(UI_BOT_W, "AtticPad -- network", NULL, ui_c_dim());

    switch (s_sub) {
    case WS_DEAD:
        /* The top screen's header/state word already says "no radio" and
         * "STARTUP FAILED"-shaped text (wifi_draw_top()) -- this screen's own
         * content is just the one hint with no on-screen affordance
         * (2026-09-09 decluttering pass: this used to also repeat "The Wi-Fi
         * hardware did not start" AND a differently-worded copy of this same
         * SELECT hint on the top screen at once). */
        ui_textf_fit(UI_BOT_W * 0.5f, 70.0f, UI_S_SMALL, ui_c_dim(),
                     UI_ALIGN_CENTER, UI_BOT_W - 20.0f,
                     "SELECT runs the built-in health check");
        return;

    case WS_BOOT:
        ui_textf_fit(UI_BOT_W * 0.5f, 70.0f, UI_S_BODY, ui_c_dim(),
                     UI_ALIGN_CENTER, UI_BOT_W - 20.0f, "starting %ds",
                     (s_boot_frames + 59) / 60);
        return;

    case WS_SAVED_ARM:
    case WS_SAVED:
        ui_textf_wrap(UI_BOT_W * 0.5f, 60.0f, UI_BOT_W - 20.0f, UI_S_BODY,
                      ui_c_text(), UI_ALIGN_CENTER, 0.0f, "Reconnecting to %s",
                      (s_saved != NULL) ? s_saved->ssid : "");
        ui_textf_fit(UI_BOT_W * 0.5f, 110.0f, UI_S_SMALL, ui_c_dim(),
                     UI_ALIGN_CENTER, UI_BOT_W - 20.0f,
                     "B: choose a different one");
        return;

    case WS_AUTO_ARM:
    case WS_AUTO:
        ui_textf_fit(UI_BOT_W * 0.5f, 70.0f, UI_S_BODY, ui_c_text(),
                     UI_ALIGN_CENTER, UI_BOT_W - 20.0f,
                     "Using the console's Wi-Fi settings");
        ui_textf_fit(UI_BOT_W * 0.5f, 100.0f, UI_S_SMALL, ui_c_dim(),
                     UI_ALIGN_CENTER, UI_BOT_W - 20.0f,
                     "B: choose a different one");
        return;

    case WS_KEY:
        /* Everything readable must stay above UI_KBD_TOP_Y: the libnds
         * keyboard's background layer owns the screen below it. */
        ui_textf_fit(10.0f, 30.0f, UI_S_SMALL, ui_c_dim(), UI_ALIGN_LEFT,
                     UI_BOT_W - 20.0f, "%s",
                     (s_sel >= 0 && s_sel < s_ap_count) ? s_aps[s_sel].ssid
                                                        : "");
        {
            ui_box f = { 10.0f, 50.0f, 300.0f, 30.0f };

            ui_panel(&f, ui_c_panel_hi(), ui_c_accent());
            ui_textf_fit(f.x + 6.0f, f.y + 6.0f, UI_S_BODY, ui_c_text(),
                         UI_ALIGN_LEFT, f.w - 12.0f, "%s_", s_key);
        }
        /* JOIN is a button below and Enter/A are implied by it -- only the
         * key-FORMAT hint (no on-screen affordance for that) and the B
         * convention (no back button in this substate) survive the
         * 2026-09-09 decluttering pass, which dropped "Enter or A joins.  B
         * goes back." from in front of them. */
        ui_textf_fit(10.0f, 88.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT,
                     UI_BOT_W - 20.0f,
                     "WEP: 5 or 13 characters, or 10/26 hex");
        ui_textf_fit(10.0f, 100.0f, UI_S_TINY, ui_c_dim(), UI_ALIGN_LEFT,
                     UI_BOT_W - 20.0f, "B: back");
        ui_button(&kJoinBtn, "JOIN", 0, 1);
        return;

    case WS_JOIN:
        ui_textf_fit(UI_BOT_W * 0.5f, 70.0f, UI_S_BODY, ui_c_text(),
                     UI_ALIGN_CENTER, UI_BOT_W - 20.0f, "Joining %s",
                     (s_sel >= 0 && s_sel < s_ap_count) ? s_aps[s_sel].ssid
                                                        : "");
        return;

    case WS_SCAN:
    default:
        draw_ap_rows();
        ui_button(&kRescanBtn, "RESCAN", 0, 0);
        ui_textf_fit(UI_BOT_W * 0.5f, 210.0f, UI_S_TINY, ui_c_dim(),
                     UI_ALIGN_CENTER, 100.0f, "up/down scrolls");
        (void)ctx;
        return;
    }
}

const apad_screen apad_screen_wifi = {
    "network",
    wifi_enter,
    wifi_update,
    wifi_draw_top,
    wifi_draw_bottom
};
