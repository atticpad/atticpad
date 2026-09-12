/* server/backends/backends.h — the registry of backends compiled in for
 * this platform (docs/DESIGN.md §6.1, §6.3).
 *
 * backend.h is the pure interface: it defines apad_backend and its friends
 * and deliberately declares NO backend symbol, because "the interface does
 * not change" and "hard-code the one backend that currently exists" were
 * mildly contradictory the moment a second backend (server/backends/vigem.c)
 * existed. This header is where "which backend(s) exist for this platform"
 * actually lives, guarded by the same platform macro the build already
 * chooses source files by (scripts/build.sh names uinput.c for `server`,
 * vigem.c for `windows` -- this header mirrors that choice as a compile-time
 * declaration instead of leaving it to be redeclared ad hoc).
 *
 * A host includes THIS header, not backend.h directly, to get the extern(s)
 * it needs to link against. Nothing here PICKS a backend -- that decision is
 * still made once, explicitly, at the host's own apad_server_create() call
 * (server/host/linux/main.c passes &apad_backend_uinput,
 * server/host/windows/main.c passes &apad_backend_vigem). Adding a third
 * backend means adding one more guarded extern here; it never touches
 * backend.h.
 */
#ifndef ATTICPAD_SERVER_BACKENDS_H
#define ATTICPAD_SERVER_BACKENDS_H

#include "backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
/* server/backends/win32.c — the composite that forwards pads to
 * server/backends/vigem.c (ViGEmBus) and keyboard/mouse/media to
 * server/backends/sendinput.c (User32 SendInput), behind one apad_backend.
 * See win32.c's own header for why a composite and not one file doing
 * both. apad_backend_vigem and apad_backend_sendinput themselves are
 * intentionally NOT declared here: only win32.c, inside server/backends/,
 * is meant to know both exist -- a host links against apad_backend_win32
 * alone. docs/DESIGN.md §2.1: ViGEmBus was retired in November 2023 and is
 * feature-frozen; vigem.c is the ONE file that would need to change if it
 * ever breaks outright, and win32.c is what keeps that change from also
 * touching KBM. */
extern const apad_backend apad_backend_win32;
#else
/* server/backends/uinput.c — Linux /dev/uinput. Default branch: every
 * platform this tree supports today other than Windows is Linux. */
extern const apad_backend apad_backend_uinput;
#endif

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_SERVER_BACKENDS_H */
