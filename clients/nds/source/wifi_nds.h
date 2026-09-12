/*
 * clients/nds/source/wifi_nds.h -- DS / DSi network bring-up.
 *
 * The role clients/psp/source/net_psp.h plays on the PSP and soc_3ds.c on the
 * 3DS: get DSWiFi to the point where shim/net_bsd.c's sockets work, then stay
 * out of the protocol's way. Nothing here knows what a datagram is.
 *
 * UNLIKE THE PSP THIS IS NOT A THREAD. DSWiFi's own examples poll
 * Wifi_AssocStatus() from the frame loop with cothread_yield_irq(IRQ_VBLANK)
 * between polls, and lwIP already runs in its own cothread underneath, so a
 * second thread would buy nothing and would need a lock. apad_nds_wifi_poll()
 * is a step function: call it once per frame and read the status.
 *
 * WHAT CHANGED WHEN THE SCREENS LANDED. The bring-up spike's version of this
 * file drove the whole sequence itself -- try the firmware slots, fall back to
 * a scan, pick the first open AP -- because there was no UI to ask. There is
 * one now (screen_wifi.c), and the policy moved there: this file is a set of
 * steps the screen orders, not a policy. The steps and their order are still
 * the SDK sample's (references/nds/examples/dswifi/full_ap_demo/source/
 * main.c); only who decides between them has changed.
 *
 * JOINING IS BY SSID, NOT BY LIST INDEX, and that is deliberate. DSWiFi's AP
 * list is live: entries appear, vanish and REORDER as beacons arrive, so the
 * index a user tapped one frame is not reliably the same AP the next. The
 * screen shows a snapshot and joins by name.
 */
#ifndef ATTICPAD_NDS_WIFI_H
#define ATTICPAD_NDS_WIFI_H

#include <stddef.h>

/* Bring-up stages. Public so a screen can render a transition without
 * reaching into this file. */
typedef enum {
    APAD_WIFI_IDLE = 0,     /* radio up, nothing attempted yet              */
    APAD_WIFI_SCANNING,     /* Wifi_ScanMode(): the AP list is filling      */
    APAD_WIFI_ASSOCIATING,  /* connect issued, polling Wifi_AssocStatus()   */
    APAD_WIFI_READY,        /* associated and holding an IPv4 address       */
    APAD_WIFI_FAILED        /* gave up; `where` says which step             */
} apad_nds_wifi_stage;

/* Security type, mirroring DSWiFi's Wifi_ApSecurityType value for value.
 * Restated here so a screen file does not have to include <dswifi9.h> just to
 * name "open" -- this header is the whole DSWiFi surface the rest of the
 * client sees, and keeping it that way is what stops a screen from reaching
 * past it. wifi_nds.c carries the compile-time check that the two agree. */
enum {
    APAD_AP_OPEN = 0,
    APAD_AP_WEP  = 1,
    APAD_AP_WPA  = 2,
    APAD_AP_WPA2 = 3
};

/* One access point, as a screen needs it. A SNAPSHOT: see the header comment
 * on why the index this came from is not stable. */
typedef struct {
    char ssid[33];
    int  security;     /* APAD_AP_*                                        */
    int  compatible;   /* the radio can actually join it (WFLAG_APDATA_
                        * COMPATIBLE). 0 for WPA/WPA2 on a DS-mode radio,
                        * which is HARDWARE, not a library limitation --
                        * docs/SETUP-DS.md                                  */
    int  rssi;
    int  channel;
} apad_nds_ap;

typedef struct {
    int         stage;
    int         assoc;       /* last Wifi_AssocStatus(), -1 before any poll  */
    int         ready;
    int         failed;
    const char *where;       /* which step failed, for the fatal line        */
    int         dsi_mode;    /* DSi-mode Wi-Fi came up (WPA2 possible)       */
    int         wfc_slots;   /* APs configured in the firmware              */
    int         ap_count;    /* APs seen by the last scan poll               */
    char        ssid[33];    /* the AP being joined / joined                 */
    char        ip[16];      /* this console's IPv4 address, display only    */
} apad_nds_wifi_status;

/*
 * Bring the radio up. Returns 0 on success, -1 if Wifi_InitDefault() failed.
 * Does NOT connect to anything.
 *
 * `force_ds_mode` mirrors the SDK sample: hold L at boot and DSWiFi is forced
 * into DS mode even on a DSi. In DS mode only open and WEP APs can be joined
 * -- that is the radio, not the library.
 *
 * NEVER Wifi_InitDefault(WFC_CONNECT): dswifi9.h says in its own words that
 * it "can't return for a few seconds until the connection has succeeded or
 * failed" and discourages it. A client frozen for four seconds with nothing
 * on screen is indistinguishable from a crash.
 */
int  apad_nds_wifi_init(int force_ds_mode);

/* Start the firmware's own WFC slots (Wifi_AutoConnect(); IP settings come
 * from flash). Only meaningful when status.wfc_slots > 0. -> ASSOCIATING. */
void apad_nds_wifi_begin_autoconnect(void);

/* Enter scan mode. The list fills over the next few seconds as the ARM7
 * rotates through the channels. -> SCANNING. */
void apad_nds_wifi_begin_scan(void);

/* Snapshot up to `max` visible APs into `out`, sorted strongest first.
 * Returns how many were written. Safe to call every frame while SCANNING. */
int  apad_nds_wifi_scan_list(apad_nds_ap *out, int max);

/* Join the visible AP named `ssid`. `key`/`key_len` is NULL/0 for an open
 * network; otherwise the RAW KEY BYTES -- 5, 13 or 16 for WEP, up to 64 for
 * WPA (dswifi9.h's own wording on Wifi_ConnectSecureAP). screen_wifi.c owns
 * turning what a person typed into those bytes.
 *
 * Returns 0 on success (-> ASSOCIATING), -1 if no visible AP has that name or
 * DSWiFi rejected the arguments. */
int  apad_nds_wifi_join(const char *ssid, const void *key, size_t key_len);

/* The same join WITHOUT a scan list in front of it: for a network whose name
 * and key this console saved on an earlier boot (config_nds.h), where there
 * is nothing to pick and nobody to pick it.
 *
 * Wifi_ConnectSecureAP() only needs the ssid/ssid_len fields of its argument
 * ("The user must fill either the bssid field or the ssid and ssid_len
 * fields. Other fields are ignored." -- dswifi9.h) and starts its own scan
 * internally, so the AP does NOT have to be visible yet when this is called;
 * DSWiFi reports ASSOCSTATUS_SEARCHING until the beacon turns up. That last
 * part is why the CALLER needs a bound -- see apad_nds_wifi_searching().
 *
 * Returns 0 on success (-> ASSOCIATING), -1 if DSWiFi rejected the
 * arguments. */
int  apad_nds_wifi_join_saved(const char *ssid, const void *key,
                              size_t key_len);

/* Abandon whatever connect is in flight and go back to IDLE.
 *
 * REQUIRED BEFORE FALLING BACK, not cosmetic: Wifi_AutoConnect() and
 * Wifi_ConnectSecureAP() both leave DSWiFi in an internal "searching for that
 * AP" state that has NO timeout of its own -- if a later frame's scan finds
 * the AP, DSWiFi associates with it, whatever screen the client has since
 * moved on to. Wifi_DisconnectAP() is what disarms that. */
void apad_nds_wifi_abort(void);

/* 1 while the radio is still LOOKING for the requested AP (it has not started
 * authenticating/associating/DHCP yet). The distinction matters because the
 * two halves need very different patience: a WPA2 join whose PMK the ARM7 has
 * to derive from a passphrase spends about five seconds inside
 * ASSOCIATING with nothing to show for it (dswifi's own comment in
 * arm7/twl/ath/wmi.twl.c: "This calculation takes about 5 seconds"), whereas
 * "still searching" after one full channel sweep means the AP is not there.
 * A caller that wants to give up early must give up only on THIS. */
int  apad_nds_wifi_searching(void);

/* 1 when the LAST failure was DSWiFi's own ASSOCSTATUS_CANNOTCONNECT (the AP
 * or the library refused) rather than this file's association timeout. A
 * retry is worth something in the first case and nothing in the second. */
int  apad_nds_wifi_rejected(void);

/* Advance one step. Call ONCE PER FRAME. Returns the current stage. */
int  apad_nds_wifi_poll(void);

/* Snapshot the status. Cheap; safe to call every frame. */
void apad_nds_wifi_get(apad_nds_wifi_status *out);

/* Is the radio ASSOCIATED right now? A live read of Wifi_AssocStatus(), not
 * the cached stage -- the association can drop at any moment and nothing
 * polls for it once screen_wifi.c has handed over. The connect screen asks
 * this when a probe finds nothing, so that "the Wi-Fi dropped" and "no server
 * at that address" are different sentences instead of one confusing one. */
int apad_nds_wifi_associated(void);

/* Human-readable names, for the screen. apad_nds_wifi_assoc_name() wraps
 * DSWiFi's own ASSOCSTATUS_STRINGS[] table so a screen does not have to index
 * it (and cannot index it out of range). */
const char *apad_nds_wifi_stage_name(int stage);
const char *apad_nds_wifi_security_name(int security);
const char *apad_nds_wifi_assoc_name(int assoc);

#endif /* ATTICPAD_NDS_WIFI_H */
