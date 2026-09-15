/* clients/psp/source/config_psp.h
 *
 * The connect screen's persisted defaults on the memory stick: the server
 * address this console last reached ACTIVE with, and the saved-network SLOT
 * whose radio last came up. A returning unit prefills its address and tries
 * the network that worked last boot first, instead of retyping on a keypad
 * and cycling slots by hand every launch. A fresh stick, or one whose config
 * was never written, starts with an EMPTY address and the lowest saved slot,
 * so nothing dials anywhere the person holding it did not choose.
 *
 * MIRRORS THE 3DS AND DS POLICY: the last-connected address is persisted, the
 * pairing PIN/secret NEVER is (docs/PROTOCOL.md S10, config_3ds.h). The PSP
 * has nothing the DS's wifi_key raised -- the console's own Network Settings
 * already hold the AP's key; this file stores only WHICH saved slot to try,
 * by its number, never a credential.
 *
 * ONE RECORD, TWO WRITERS, the DS's pattern (config_nds.c): the slot is saved
 * the instant the radio reports an address (screen_connect.c), the ip/port on
 * the session's ACTIVE edge (screen_session.c). Both go through the one
 * module-static record parsed at load, so neither writer drops the other's
 * half.
 */
#ifndef ATTICPAD_PSP_CONFIG_H
#define ATTICPAD_PSP_CONFIG_H

#include <stddef.h>

/* Parse ms0:/atticpad/atticpad.cfg into the module-static record and fill the
 * caller's buffers. Returns the saved network slot (1..n), or 0 when none was
 * saved (fresh stick, unreadable or corrupt file, out-of-range slot). On a 0
 * or missing ip the caller's ip_out is left "" and port_out untouched, so the
 * caller's own default port survives. A saved port is only applied alongside
 * a saved ip. Validates nothing about the address beyond "fits the buffer":
 * do_connect() calls apad_addr_parse() on every attempt regardless. */
int  apad_psp_config_load(char *ip_out, size_t ip_cap,
                          char *port_out, size_t port_cap);

/* Best-effort save of the saved-network slot that just came up. A slot <= 0
 * is a no-op. Merges into the record, so the saved ip/port are preserved. */
void apad_psp_config_save_slot(int slot);

/* Best-effort save of the address a session just reached ACTIVE with. A NULL
 * or empty ip or port is a no-op (never writes a half record). Merges into
 * the record, so the saved slot is preserved. NEVER pass a secret or PIN. */
void apad_psp_config_save_server(const char *ip, const char *port);

#endif /* ATTICPAD_PSP_CONFIG_H */
