/*
 * clients/nds/source/wifi_nds.c -- DS / DSi network bring-up.
 *
 * MIRRORED, NOT REMEMBERED. Every DSWiFi call below and the order they are
 * made in comes from the SDK's own sample, vendored at
 * references/nds/examples/dswifi/full_ap_demo/source/main.c, as docs/CONVENTIONS.md
 * requires for a platform nobody here can test by hand. The sample is a menu
 * driven by a human; this is the same sequence with the menu replaced by
 * screen_wifi.c and a state machine in between, so that the frame loop keeps
 * running.
 *
 * The sequence, and where each piece comes from:
 *
 *   Wifi_InitDefault(INIT_ONLY | WIFI_ATTEMPT_DSI_MODE)   sample main()
 *   Wifi_EnableWifi()                                     sample `connect:`
 *   Wifi_GetData(WIFIGETDATA_NUMWFCAPS, 0, NULL)          sample `connect:`
 *   Wifi_AutoConnect()                                    sample option A
 *   Wifi_ScanMode() / GetNumAP() / GetAPData()            sample option B
 *   Wifi_SetIP(0,0,0,0,0)                                 sample option B
 *   Wifi_ConnectSecureAP(&ap, key, len)                   sample option B
 *   poll Wifi_AssocStatus() per frame                     sample wait loop
 *   Wifi_GetIPInfo()                                      sample info screen
 *
 * ONE DELIBERATE DIVERGENCE FROM THE SAMPLE. For an open AP the sample calls
 * Wifi_ConnectSecureAP(&ap, NULL, 0); this file calls Wifi_ConnectOpenAP(&ap).
 * dswifi9.h:406 documents the latter as exactly "connect to an AP without
 * encryption" and both are non-deprecated (unlike Wifi_ConnectAP, which is
 * marked __attribute__((deprecated)) at dswifi9.h:353). The named function
 * says what this client means; the sample's form is the general one because
 * the sample also handles WEP and WPA in the same code path. Recorded here
 * because the rule is that where a sample and a choice differ, the difference
 * gets said out loud.
 *
 * ONE STEP THE SAMPLE DOES NOT HAVE: apad_nds_wifi_join_saved(), which joins
 * a network by name with a key this console saved on an earlier boot, with no
 * scan list in front of it. The sample never needs it because a human is
 * always there to pick a row. It is still the sample's call
 * (Wifi_ConnectSecureAP) with the sample's Wifi_SetIP(0,0,0,0,0) in front;
 * only the Wifi_AccessPoint handed to it is built from a saved SSID instead of
 * copied out of Wifi_GetAPData(). dswifi9.h says in as many words that this is
 * allowed ("The user must fill either the bssid field or the ssid and ssid_len
 * fields. Other fields are ignored."), and dswifi's own source confirms the
 * function starts the scan itself -- see the comment on the function.
 *
 * NEVER Wifi_InitDefault(WFC_CONNECT). dswifi9.h:118-124 says it "can't
 * return for a few seconds until the connection has succeeded or failed" and
 * discourages it in its own words.
 *
 * NO THREAD, NO LOCK: see wifi_nds.h. Everything here runs on the ARM9 main
 * cothread, one step per frame.
 */
#include <string.h>

#include <nds.h>
#include <dswifi9.h>
#include <arpa/inet.h>

#include "atticpad/atticpad.h"

#include "wifi_nds.h"

/* How long to keep trying before giving up. Generous on purpose: DSWiFi's
 * scan rotates through the channels roughly once a second, and DHCP on a real
 * AP can take several seconds after association. */
#define ASSOC_TIMEOUT_MS  30000u

static struct {
    int  stage;
    int  assoc;
    int  ready;
    int  failed;
    const char *where;
    int  dsi_mode;
    int  wfc_slots;
    int  ap_count;
    char ssid[33];
    char ip[16];
    uint32_t stage_start;  /* apad_ticks_ms() when the current stage began   */
    int  rejected;         /* the last failure was CANNOTCONNECT, not our
                            * own ASSOC_TIMEOUT_MS -- see
                            * apad_nds_wifi_rejected()                       */
} g;

static void enter(int stage)
{
    g.stage = stage;
    g.stage_start = apad_ticks_ms();
}

static void fail(const char *where)
{
    g.where = where;
    g.ready = 0;
    g.failed = 1;
    enter(APAD_WIFI_FAILED);
}

const char *apad_nds_wifi_stage_name(int stage)
{
    switch (stage) {
    case APAD_WIFI_IDLE:        return "idle";
    case APAD_WIFI_SCANNING:    return "scanning";
    case APAD_WIFI_ASSOCIATING: return "associating";
    case APAD_WIFI_READY:       return "ready";
    case APAD_WIFI_FAILED:      return "failed";
    default:                    return "?";
    }
}

/* wifi_nds.h restates DSWiFi's security enum so screen files need not include
 * <dswifi9.h>. If that ever stops being value-for-value true, this is where
 * the build breaks rather than where a WEP network silently becomes "open". */
_Static_assert((int)AP_SECURITY_OPEN == APAD_AP_OPEN
               && (int)AP_SECURITY_WEP == APAD_AP_WEP
               && (int)AP_SECURITY_WPA == APAD_AP_WPA
               && (int)AP_SECURITY_WPA2 == APAD_AP_WPA2,
               "wifi_nds.h's APAD_AP_* must match Wifi_ApSecurityType");

const char *apad_nds_wifi_security_name(int security)
{
    switch (security) {
    case APAD_AP_OPEN: return "open";
    case APAD_AP_WEP:  return "WEP";
    case APAD_AP_WPA:  return "WPA";
    case APAD_AP_WPA2: return "WPA2";
    default:           return "?";
    }
}

const char *apad_nds_wifi_assoc_name(int assoc)
{
    if (assoc < 0 || assoc > ASSOCSTATUS_CANNOTCONNECT) {
        return "starting";
    }
    return ASSOCSTATUS_STRINGS[assoc];
}

/* Not strncpy: a 32-character SSID is exactly sizeof-1, so a strncpy bounded
 * at 32 fills the buffer and writes no NUL. Same trap as
 * clients/psp/source/net_psp.c. */
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);

    if (n > cap - 1u) {
        n = cap - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int apad_nds_wifi_init(int force_ds_mode)
{
    unsigned int flags;

    memset(&g, 0, sizeof g);
    g.assoc = -1;
    g.where = "";

    /* The sample's own flag expression. WIFI_ATTEMPT_DSI_MODE is what makes
     * WPA2 possible at all -- on a DS or DS Lite radio it simply falls back,
     * so it costs nothing to ask. WIFI_DS_MODE_ONLY forces the old radio even
     * on a DSi, which is how a user reproduces a DS-only problem. */
    flags = INIT_ONLY | (force_ds_mode ? WIFI_DS_MODE_ONLY
                                       : WIFI_ATTEMPT_DSI_MODE);

    if (!Wifi_InitDefault(flags)) {
        fail("Wifi_InitDefault");
        return -1;
    }
    g.dsi_mode = (!force_ds_mode && isDSiMode()) ? 1 : 0;

    Wifi_EnableWifi();

    /* The sample reads this to warn that "option A will fail" when there are
     * no firmware WFC slots. Reading it first turns a several-second detour
     * into ASSOCSTATUS_CANNOTCONNECT into a line the screen can show before
     * anything is attempted. */
    g.wfc_slots = Wifi_GetData(WIFIGETDATA_NUMWFCAPS, 0, NULL);

    enter(APAD_WIFI_IDLE);
    return 0;
}

void apad_nds_wifi_begin_autoconnect(void)
{
    g.assoc = -1;
    g.failed = 0;
    g.rejected = 0;
    g.where = "";
    copy_str(g.ssid, sizeof g.ssid, "");
    Wifi_AutoConnect();     /* IP settings come from flash with this */
    enter(APAD_WIFI_ASSOCIATING);
}

void apad_nds_wifi_begin_scan(void)
{
    g.assoc = -1;
    g.failed = 0;
    g.rejected = 0;
    g.where = "";
    g.ap_count = 0;
    Wifi_EnableWifi();      /* the sample re-enables before re-scanning */
    Wifi_ScanMode();
    enter(APAD_WIFI_SCANNING);
}

int apad_nds_wifi_scan_list(apad_nds_ap *out, int max)
{
    int count = Wifi_GetNumAP();
    int n = 0;
    int i, j;

    g.ap_count = count;
    if (out == NULL || max <= 0) {
        return 0;
    }

    for (i = 0; i < count && n < max; i++) {
        Wifi_AccessPoint ap;
        apad_nds_ap e;

        /* Wifi_GetAPData() can return WIFI_RETURN_LOCKFAILED, which means
         * "try again later", not "no such AP". Skipping the entry this frame
         * is exactly right: the next frame re-reads it. */
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK) {
            continue;
        }
        if (ap.ssid[0] == '\0') {
            continue;   /* hidden SSID: nothing to show and nothing to tap */
        }
        memset(&e, 0, sizeof e);
        copy_str(e.ssid, sizeof e.ssid, ap.ssid);
        e.security   = (int)ap.security_type;
        e.compatible = (ap.flags & WFLAG_APDATA_COMPATIBLE) ? 1 : 0;
        e.rssi       = (int)ap.rssi;
        e.channel    = (int)ap.channel;

        /* Drop a duplicate SSID (the same network on two channels or two
         * radios): the screen joins BY NAME, so a second row for the same
         * name would be a row that does nothing different. */
        for (j = 0; j < n; j++) {
            if (strcmp(out[j].ssid, e.ssid) == 0) {
                break;
            }
        }
        if (j < n) {
            continue;
        }
        out[n++] = e;
    }

    /* Strongest first. Insertion sort over at most `max` entries, once per
     * frame -- the list is a dozen rows at most and this keeps the strongest
     * network under the user's thumb instead of wherever the ARM7 happened to
     * record it. NOTE the DS/DSi difference dswifi_common.h documents: rssi
     * is 0..255 on a DS and a NEGATIVE dBm value on a DSi. Larger is stronger
     * in both, so one comparison is correct for both. */
    for (i = 1; i < n; i++) {
        apad_nds_ap key = out[i];

        for (j = i - 1; j >= 0 && out[j].rssi < key.rssi; j--) {
            out[j + 1] = out[j];
        }
        out[j + 1] = key;
    }
    return n;
}

int apad_nds_wifi_join(const char *ssid, const void *key, size_t key_len)
{
    int count = Wifi_GetNumAP();
    int i;

    if (ssid == NULL || ssid[0] == '\0') {
        return -1;
    }

    for (i = 0; i < count; i++) {
        Wifi_AccessPoint ap;
        int rc;

        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK) {
            continue;
        }
        if (strcmp(ap.ssid, ssid) != 0) {
            continue;
        }

        /* All zeroes = ask DHCP for everything. The sample does this before
         * connecting to a scanned AP; the WFC path does not, because
         * Wifi_AutoConnect() loads the IP settings from flash. */
        Wifi_SetIP(0, 0, 0, 0, 0);

        if (key == NULL || key_len == 0u) {
            rc = Wifi_ConnectOpenAP(&ap);
        } else {
            rc = Wifi_ConnectSecureAP(&ap, key, key_len);
        }
        if (rc != 0) {
            return -1;
        }
        copy_str(g.ssid, sizeof g.ssid, ssid);
        g.assoc = -1;
        g.failed = 0;
        g.rejected = 0;
        g.where = "";
        enter(APAD_WIFI_ASSOCIATING);
        return 0;
    }
    return -1;
}

/* THE SAVED-NETWORK JOIN. No scan list in front of it, because on the boot
 * this exists for there is nothing on screen to pick from yet and the AP may
 * not have been beaconed on the current scan channel yet either.
 *
 * READ OUT OF DSWIFI'S OWN SOURCE, not assumed: Wifi_ConnectSecureAP()
 * (dswifi source/arm9/access_point.c) validates ssid_len, stores the key,
 * copies the caller's Wifi_AccessPoint into its own `wifi_connect_point`, and
 * then calls Wifi_ScanMode() ITSELF; Wifi_AssocStatus() reports
 * ASSOCSTATUS_SEARCHING every poll until Wifi_FindMatchingAP() matches that
 * SSID against a received beacon, at which point it hands the ARM7 the
 * BEACON's record (channel, security type, BSSID) rather than ours. So filling
 * ssid and ssid_len is genuinely enough, and the security type does NOT need
 * to be supplied -- which is why config_nds.c's saved wifi_sec is only ever
 * used to decide whether to ATTEMPT the join, never to describe it to DSWiFi.
 *
 * ssid_len is the one field the picker path gets for free from
 * Wifi_GetAPData() and this one has to compute. A zero here is a silent
 * "match nothing forever", so it is checked.
 */
int apad_nds_wifi_join_saved(const char *ssid, const void *key,
                             size_t key_len)
{
    Wifi_AccessPoint ap;
    size_t n;
    int rc;

    if (ssid == NULL || ssid[0] == '\0') {
        return -1;
    }
    n = strlen(ssid);
    if (n > 32u) {
        return -1;
    }

    memset(&ap, 0, sizeof ap);
    memcpy(ap.ssid, ssid, n);
    ap.ssid[n] = '\0';
    ap.ssid_len = (u8)n;

    /* All zeroes = ask DHCP for everything, exactly as on the picker path.
     * The WFC path is the only one that does not do this, because
     * Wifi_AutoConnect() loads the IP settings from flash. */
    Wifi_SetIP(0, 0, 0, 0, 0);

    if (key == NULL || key_len == 0u) {
        rc = Wifi_ConnectOpenAP(&ap);
    } else {
        rc = Wifi_ConnectSecureAP(&ap, key, key_len);
    }
    if (rc != 0) {
        return -1;
    }
    copy_str(g.ssid, sizeof g.ssid, ssid);
    g.assoc = -1;
    g.failed = 0;
    g.rejected = 0;
    g.where = "";
    enter(APAD_WIFI_ASSOCIATING);
    return 0;
}

void apad_nds_wifi_abort(void)
{
    /* Wifi_DisconnectAP() is what actually disarms the pending connect: it
     * sets DSWiFi's wifi_connect_state back to its error state, so a beacon
     * arriving after the caller has given up cannot still associate behind
     * the client's back. Without it, abandoning a Wifi_AutoConnect() or a
     * Wifi_ConnectSecureAP() only stops this file from LOOKING. */
    Wifi_DisconnectAP();
    g.assoc = -1;
    g.failed = 0;
    g.rejected = 0;
    g.ready = 0;
    g.where = "";
    copy_str(g.ssid, sizeof g.ssid, "");
    enter(APAD_WIFI_IDLE);
}

int apad_nds_wifi_searching(void)
{
    /* ASSOCSTATUS_DISCONNECTED and ASSOCSTATUS_SEARCHING are the two values
     * DSWiFi reports before it has found the requested AP; -1 is this file's
     * own "no poll yet". AUTHENTICATING/ASSOCIATING/ACQUIRINGDHCP all mean the
     * AP was found and something slow but real is happening. */
    return g.assoc < 0 || g.assoc == ASSOCSTATUS_DISCONNECTED
        || g.assoc == ASSOCSTATUS_SEARCHING;
}

int apad_nds_wifi_rejected(void)
{
    return g.rejected;
}

static void capture_ip(void)
{
    struct in_addr gateway, mask, dns1, dns2;
    struct in_addr ip;
    const char *s;

    ip = Wifi_GetIPInfo(&gateway, &mask, &dns1, &dns2);
    s = inet_ntoa(ip);
    copy_str(g.ip, sizeof g.ip, (s != NULL) ? s : "");
}

int apad_nds_wifi_poll(void)
{
    switch (g.stage) {
    case APAD_WIFI_IDLE:
    case APAD_WIFI_READY:
    case APAD_WIFI_FAILED:
        break;

    case APAD_WIFI_ASSOCIATING: {
        int status = Wifi_AssocStatus();

        g.assoc = status;

        if (status == ASSOCSTATUS_ASSOCIATED) {
            capture_ip();
            g.ready = 1;        /* written after ip: a reader that sees
                                 * ready==1 sees a complete address */
            enter(APAD_WIFI_READY);
            break;
        }
        if (status == ASSOCSTATUS_CANNOTCONNECT) {
            g.rejected = 1;
            fail("could not associate");
            break;
        }
        if (apad_time_since(apad_ticks_ms(), g.stage_start) > ASSOC_TIMEOUT_MS) {
            fail("association timed out");
        }
        break;
    }

    case APAD_WIFI_SCANNING:
        /* No timeout: the screen owns how long a person is willing to look at
         * a list, and a scan that has found nothing yet is not a failure. */
        g.ap_count = Wifi_GetNumAP();
        break;

    default:
        break;
    }

    return g.stage;
}

int apad_nds_wifi_associated(void)
{
    return Wifi_AssocStatus() == ASSOCSTATUS_ASSOCIATED;
}

void apad_nds_wifi_get(apad_nds_wifi_status *out)
{
    if (out == NULL) {
        return;
    }
    out->stage     = g.stage;
    out->assoc     = g.assoc;
    out->ready     = g.ready;
    out->failed    = g.failed;
    out->where     = g.where;
    out->dsi_mode  = g.dsi_mode;
    out->wfc_slots = g.wfc_slots;
    out->ap_count  = g.ap_count;
    memcpy(out->ssid, g.ssid, sizeof out->ssid);
    memcpy(out->ip, g.ip, sizeof out->ip);
}
