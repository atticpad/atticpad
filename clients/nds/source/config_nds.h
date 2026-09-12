/*
 * clients/nds/source/config_nds.h -- the connect screen's persisted defaults
 * and the network this console last joined.
 *
 * clients/3ds/source/config_3ds.h, ported. Same policy for the SERVER side of
 * the record, and the policy is the point: the address this console last
 * reached ACTIVE with is persisted, and THE PAIRING PIN NEVER IS. That mirrors
 * Android's and the 3DS's, and docs/PROTOCOL.md S10 is why -- a secret that
 * survives a power cycle on removable media is not a secret with the
 * properties S10 assumes.
 *
 * TWO FIELDS MORE THAN THE 3DS, and one REVERSED DECISION. On the 3DS the
 * network is already up when the app starts, so there is nothing to remember
 * about it. Here the first thing a returning user has to redo is pick their
 * access point out of a scan list and type its key, so the SSID, THE KEY and
 * the security type are all saved now.
 *
 * THE KEY USED TO BE DELIBERATELY THROWN AWAY. It is not any more, at the
 * user's explicit request (2026-09-10), because the two secrets are not the
 * same kind of thing and the old comment here conflated them:
 *
 *   - The S10 pairing secret authorises THIS console to drive a specific PC.
 *     It is derived per pairing, it is short-lived, and a copy of it on an SD
 *     card that leaves the console is a copy of an authorisation. Still never
 *     written.
 *   - A Wi-Fi key authorises anything to join a LAN, and the console ALREADY
 *     stores one: the firmware's own WFC slots hold the SSID, the WEP key and
 *     (on a DSi) the WPA passphrase AND its precomputed PMK in NVRAM, which is
 *     exactly as readable to anyone holding the console. Refusing to write the
 *     same secret to the same console's SD card bought no security and cost
 *     the user a WPA2 key typed on a touchscreen keyboard at every boot.
 *
 * So it is written, in hex, in the clear, in <root>/atticpad/atticpad.cfg.
 * ANYONE WITH THE CARD CAN READ IT. That is a real property of this feature
 * and is documented rather than hidden; a user who does not want it can delete
 * the file, and a build that does not want it need only never call
 * apad_nds_config_save_network().
 *
 * WHY HEX AND NOT TEXT. A WEP key is 5, 13 or 16 RAW BYTES and is routinely
 * not printable -- screen_wifi.c's encode_key() turns "10 or 26 hex digits"
 * into those bytes before DSWiFi ever sees them. Storing what the user typed
 * would mean re-running that guess on load; storing the bytes DSWiFi was
 * actually handed means the re-join is byte-identical to the join that worked.
 *
 * WHERE THE FILE LIVES, and why it is not one path. fatInitDefault()'s own
 * documentation (libnds fat.h) says the initial working directory is "fat:/"
 * on a DS, where the card is reached through the flashcart's DLDI driver, and
 * "sd:/" on a DSi, where it is the console's internal slot. Two roots, one
 * layout: <root>/atticpad/atticpad.cfg. apad_nds_config_mount() picks the root
 * once and everything below uses it.
 *
 * A FRESH UNIT MUST NEVER DIAL AN ADDRESS NOBODY CHOSE. That is a real bug
 * this project shipped once (2026-08-12 on the 3DS: "it still prefills the
 * server on first boot and tries to connect", from a hardcoded literal in
 * main.c). Every failure mode here -- no card, no DLDI driver, no file, a
 * corrupt line, a value too long for the caller's buffer -- leaves the
 * caller's address buffer EMPTY, never partially filled.
 *
 * SILENT WHEN THERE IS NO CARD AT ALL, which is the common case under an
 * emulator with no DLDI image configured -- and must be, because a controller
 * that refuses to boot without an SD card would be a worse product than one
 * that forgets an address.
 *
 * THE FILE FORMAT, one `key=value` per line, unknown keys skipped:
 *
 *     ip=192.168.1.14        the server this console last reached ACTIVE
 *     port=21100
 *     ssid=MyNetwork         the network the PICKER joined (never the one
 *                            Wifi_AutoConnect() found -- DSWiFi cannot say
 *                            which that was)
 *     wifi_sec=3             0 open, 1 WEP, 2 WPA, 3 WPA2 (wifi_nds.h's
 *                            APAD_AP_*, which are DSWiFi's own values)
 *     wifi_key=68756e746572  the RAW key bytes as lowercase hex, EMPTY for an
 *                            open network
 *
 * FORWARD- AND BACKWARD-COMPATIBLE. A build that predates wifi_sec/wifi_key
 * skips them; a file that predates them (ssid only) is read as "the SSID is
 * known, the credentials are NOT" -- security stays -1, and screen_wifi.c
 * takes the old code path for that record rather than guessing that a missing
 * key line meant an open network.
 */
#ifndef ATTICPAD_NDS_CONFIG_H
#define ATTICPAD_NDS_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* Wifi_ConnectSecureAP()'s own ceiling: "For WPA networks it must be at most
 * 64 bytes long" (dswifi9.h). Same number screen_wifi.c's s_keybytes uses. */
#define APAD_NDS_KEY_MAX 64

/* The whole saved record, in memory. Read with apad_nds_config_network();
 * never written to directly. */
typedef struct {
    char    ssid[33];
    uint8_t key[APAD_NDS_KEY_MAX];
    size_t  key_len;                 /* 0 == open network                   */
    int     security;                /* APAD_AP_* (0..3), or -1 = UNKNOWN,
                                      * which means "this record predates
                                      * credential saving", NOT "open"      */
} apad_nds_config_net;

/* Mounts the card. Call ONCE at startup, before the first load. Returns 1 if
 * a filesystem is usable, 0 otherwise; a 0 return is not an error and must not
 * stop the client -- every other call here then does nothing.
 *
 * fatInitDefault() can take a noticeable moment on a real flashcart, so this
 * belongs in bring-up rather than in a frame. */
int  apad_nds_config_mount(void);

/* Loads the file into this module's record AND copies the address fields into
 * the caller's buffers. Returns 1 when a saved address was applied, 0
 * otherwise. On a 0 return *ip_out is an empty string and the other two are
 * left untouched, so a caller's own defaults survive -- and the network half
 * of the record may still have been filled, since a user may have joined a
 * network and never reached a server. Any of the out pointers may be NULL. */
int  apad_nds_config_load(char *ip_out, size_t ip_cap,
                          char *port_out, size_t port_cap,
                          char *ssid_out, size_t ssid_cap);

/* The saved network, or NULL when none was read. The pointer is into this
 * module's own record and stays valid for the life of the program; its
 * CONTENTS change on the next apad_nds_config_save_network(). */
const apad_nds_config_net *apad_nds_config_network(void);

/* Best-effort save of the SERVER half. Called by screen_session.c at the
 * moment a connect first reaches ACTIVE -- not before (a mistyped address is
 * never what gets saved) and not on every frame of a long session (this writes
 * to a card).
 *
 * READ-MODIFY-WRITE: the network half of the record is carried through
 * untouched. It used to take the SSID as an argument and drop it when that
 * argument was empty, which would now silently forget a network every time a
 * Wifi_AutoConnect() boot (where the SSID is unknowable) reached a session.
 *
 * NEVER PASS A PIN HERE. */
void apad_nds_config_save_address(const char *ip, const char *port);

/* Best-effort save of the NETWORK half, replacing whatever was there. Called
 * by screen_wifi.c the instant a picker join reaches ASSOCIATED -- not on the
 * way to a server, because a user may never reach one on that boot and the
 * network is still worth remembering.
 *
 * `key`/`key_len` are the RAW BYTES handed to Wifi_ConnectSecureAP(), or
 * NULL/0 for an open network. `security` is an APAD_AP_* value.
 *
 * The address half is carried through untouched. */
void apad_nds_config_save_network(const char *ssid, const void *key,
                                  size_t key_len, int security);

#endif /* ATTICPAD_NDS_CONFIG_H */
