/* clients/3ds/source/config_3ds.h
 *
 * The connect screen's persisted defaults: the address this console last
 * reached ACTIVE with. One small plain-text file on SD, so a returning unit
 * does not have to be re-typed on a numpad every launch -- and so a FRESH
 * unit, or one whose config was never written, starts with an EMPTY address
 * and never dials anywhere the person holding it did not choose (2026-08-12
 * report: "it still prefills the server (this box I believe) on first boot
 * and tries to connect" -- main.c used to hardcode this project's own dev
 * server's LAN address as ctx.ip_text's initial value; that literal is gone,
 * this file is the replacement).
 *
 * MIRRORS ANDROID'S POLICY EXACTLY: the last-connected address is persisted,
 * the pairing PIN/key never is. See config_3ds.c's header for why that is
 * not merely a preference here either.
 *
 * A NEW, SMALL PLATFORM MODULE rather than folding this into main.c, the
 * same shape as soc_3ds.c and time_3ds.c: one 3DS-specific concern (here,
 * the SD filesystem) behind two calls, so the file that calls it does not
 * need to know the path, the format, or the atomic-write trick. Unlike
 * soc_3ds.c's bring-up call, the save half is also called from
 * screen_session.c (the moment a session reaches ACTIVE), not only from
 * main.c -- so, like camera_3ds.h, this header is included directly by
 * whichever screen file needs it rather than routed through app.h.
 */
#ifndef ATTICPAD_3DS_CONFIG_H
#define ATTICPAD_3DS_CONFIG_H

#include <stddef.h>

/* Loads the saved address into ip_out/port_out (both NUL-terminated
 * C strings, existing callers pass ctx->ip_text/ctx->port_text and their
 * sizeof()s directly). Returns 1 when a saved address was applied, 0
 * otherwise -- and 0 covers every non-address case identically: no file, an
 * unreadable file, a corrupt line, or a saved value too long for the
 * caller's buffer. On a 0 return *ip_out is left as an empty string (never
 * partially filled) and *port_out is left untouched, so the caller's own
 * default (main.c sets APAD_DEFAULT_PORT before calling this) survives. A
 * saved port is only ever applied alongside a saved ip -- this file never
 * fills port_out on its own, so a corrupt/partial save cannot silently
 * change the port while leaving the address blank.
 *
 * Does not touch anything outside the two buffers: not ctx->from_announce,
 * not ctx->have_secret (a loaded address is exactly like one somebody just
 * typed -- unproven until the connect screen's own probe/handshake says
 * otherwise, docs/PROTOCOL.md S8), and it validates nothing about the
 * address itself beyond "fits the buffer" -- do_connect() (screen_connect.c)
 * already calls apad_addr_parse() on every connect attempt regardless of
 * where ip_text came from, so a corrupt-but-buffer-sized value here still
 * fails safely there. */
int apad3ds_config_load(char *ip_out, size_t ip_cap,
                        char *port_out, size_t port_cap);

/* Best-effort save of the given ip/port. Called by screen_session.c at the
 * moment a connect first reaches ACTIVE (not before, and not on every frame
 * of a long session) -- see config_3ds.c for the exact write mechanics and
 * failure handling. A NULL or empty ip or port is a no-op: this file never
 * writes a half-filled record over a previously good one.
 *
 * NEVER PASS A SECRET OR PIN HERE. This file is plain text on removable
 * media that survives the console being powered off, which is exactly the
 * property docs/PROTOCOL.md S10 says a pairing secret must not have outside
 * the session engine's one in-memory copy -- mirrors Android's own policy,
 * which persists the last-connected address and never the key. */
void apad3ds_config_save(const char *ip, const char *port);

#endif /* ATTICPAD_3DS_CONFIG_H */
