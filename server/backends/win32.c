/* server/backends/win32.c — Windows composite backend (docs/DESIGN.md §6.1),
 * exporting apad_backend_win32.
 *
 * apad_server_create() (server/include/apadserver.h) takes exactly ONE
 * `const apad_backend *`, and that is the right shape for a host: a host
 * has one virtual controller strategy and one KBM injection strategy, and
 * it should not have to know there happen to be two Windows-specific
 * technologies behind them. This file is what makes that still true now
 * that pads (server/backends/vigem.c, ViGEmBus) and keyboard/mouse/media
 * (server/backends/sendinput.c, SendInput) are two unrelated APIs with
 * nothing in common except both being Win32: apad_backend_win32 forwards
 * every pad call to apad_backend_vigem and every KBM call to
 * apad_backend_sendinput, behind the one apad_backend the library expects.
 * Pure forwarding, no state of its own beyond the two health snapshots
 * below.
 *
 * Three shapes were considered for this split, and the two rejected ones
 * are worth preserving here so they are not silently re-proposed later:
 *
 *   1. REJECTED: change apad_server_create()'s signature to take an array
 *      of backends. This breaks the host API (every existing call site —
 *      server/host/linux/main.c, server/host/windows/main.c,
 *      tools/server-harness — passes one pointer) and, worse, it forces
 *      libapadserver itself to learn how to ROUTE a call to the right
 *      element of the array, i.e. it moves win32.c's entire job into the
 *      platform-independent library. backend.h's whole point is that the
 *      library never learns which backend(s) exist; an array parameter
 *      undoes that the moment there is more than one.
 *
 *   2. REJECTED: fold SendInput into vigem.c directly (one file, two
 *      technologies). This makes vigem.c's already-hairy build (vendored
 *      C++ ViGEmClient.cpp, mingw case-shim headers, -isystem — see that
 *      file's header) responsible for a completely unrelated Win32 API,
 *      and it means a ViGEmBus outage takes KBM down with it for no
 *      reason: ViGEmBus was archived in November 2023 and is the single
 *      shakiest driver dependency in this project (backend.h's own header,
 *      docs/DESIGN.md §2.1) — exactly the dependency KBM has no reason to share.
 *      Keeping them apart means "ViGEmBus not installed" degrades this
 *      backend to KBM-only instead of taking the whole Windows backend
 *      down; see backend_init()/backend_health() below for how that shows
 *      up to a caller.
 *
 *   3. CONSIDERED, NOT BUILT: a generic runtime apad_backend_compose(const
 *      apad_backend *pads, const apad_backend *kbm) that any platform could
 *      call. Rejected for now because it needs either mutable per-instance
 *      vtable storage (apad_backend today is `const`, one static struct per
 *      backend, and every existing const apad_backend_* symbol in this
 *      tree is designed to be freely aliased/compared by pointer — a
 *      factory would have to allocate or use file-static "current leaf
 *      backend" pointers, which breaks the moment two composed backends
 *      are needed at once, e.g. a future test harness wanting both a real
 *      and a fake) for what is, today, exactly one call site. If a THIRD
 *      pad-technology/KBM-technology combination ever appears on another
 *      platform, build the generic version then — this note is the
 *      pointer for whoever needs it, so the decision is re-examined with
 *      real requirements instead of guessed at.
 *
 * The rule that must survive all of the above: nothing outside
 * server/backends/ may know which backend is active. server/host/windows/
 * main.c changes exactly one token from what it was before this file
 * existed — `&apad_backend_vigem` becomes `&apad_backend_win32` — and nothing
 * else about apad_server_create() or the host's own code changes.
 * server/backends/backends.h's _WIN32 branch now declares
 * apad_backend_win32 instead of apad_backend_vigem; apad_backend_vigem and
 * apad_backend_sendinput themselves are no longer declared there at all —
 * only THIS file (being inside server/backends/, the one place allowed to
 * know both exist) reaches for them, via its own local extern declarations
 * below, the same pattern server/host/windows/main.c used for
 * apad_backend_vigem before backends.h existed.
 *
 * Server code: ordinary hosted C, no state of its own to malloc.
 *
 * BUILD STATUS: cross-compiled clean with mingw-w64, never run — see
 * sendinput.c's header for the full disclosure; everything said there
 * about "built, not run" applies to the forwarding in this file too.
 */
#include <stddef.h>

#include "backend.h"

/* Deliberately NOT included via backends.h: backends.h's job is "which
 * backend(s) does a HOST link against for this platform", and after this
 * file exists that answer is exactly one symbol, apad_backend_win32. These
 * two externs are win32.c's own private knowledge of what it is built out
 * of — the same shape server/host/windows/main.c used for
 * apad_backend_vigem before backends.h existed (see backends.h's own
 * history comment). */
extern const apad_backend apad_backend_vigem;
extern const apad_backend apad_backend_sendinput;

/* ======================================================================== */
/* init / health / shutdown                                                 */
/* ======================================================================== */

/* Cached so backend_health() (which may be polled at UI refresh rate, same
 * reasoning as vigem.c's own g_last_init_rc) never has to re-dial
 * ViGEmBus. apad_backend_sendinput::init() cannot fail (see its own header:
 * "nothing to open") so there is nothing equivalent to cache for it. */
static int g_vigem_init_rc = -1;

static int backend_init(void)
{
    int sendinput_rc;

    g_vigem_init_rc = apad_backend_vigem.init();
    sendinput_rc     = apad_backend_sendinput.init();

    /* Pads and KBM are independent facilities on Windows (that is this
     * whole file's reason to exist) — a user with no ViGEmBus installed
     * still gets a working keyboard/mouse/media session, and §6.1's
     * abstraction means neither this file's caller nor the client-facing
     * protocol needs to know pads specifically are the part that is down.
     * Only fail outright (which server/src/server.c turns into
     * apad_server_create() returning NULL — see apadserver.h's own doc
     * comment on that) when NEITHER facility works, i.e. this backend
     * would do nothing at all. sendinput_rc is 0 today unconditionally,
     * so in practice this branch means "and SendInput itself somehow
     * failed too" — kept as a real check rather than assuming that can
     * never happen, since a future sendinput.c change might give it a
     * real failure mode. */
    if (g_vigem_init_rc != 0 && sendinput_rc != 0) {
        return -1;
    }
    return 0;
}

static void backend_shutdown(void)
{
    apad_backend_vigem.shutdown();
    apad_backend_sendinput.shutdown();
}

/*
 * health() reports the PAD backend's problem when it has one, not the KBM
 * backend's — a deliberate asymmetry, not an oversight:
 *
 *   - apad_backend_sendinput::health() is, by construction, ALWAYS OK (see
 *     its own header: no driver, no permission, no version to get wrong).
 *     Surfacing "sendinput: OK" ahead of a real ViGEmBus problem would
 *     bury the one thing a user can actually act on underneath a status
 *     that never has anything to say.
 *   - "ViGEmBus not installed — click to install" (docs/DESIGN.md §6.3) is
 *     exactly the situation this project's own UI is built to surface and
 *     act on; there is no equivalent action for a SendInput problem
 *     because SendInput has none.
 *
 * This does NOT hide a KBM problem from a user: sendinput.c's health() has
 * nothing to hide (always OK), and the one real limitation KBM injection
 * has — a game with exclusive raw-input capture or anti-cheat ignoring
 * injected input — travels through APAD_KBM_CAP_SYNTHETIC into §6.19's
 * INPUTCAPS.status bit and the client's own warning banner, NOT through
 * health(). See sendinput.c's backend_health() for why that split is
 * correct rather than a place a real fault could go unreported: SYNTHETIC
 * is a permanent property of the mechanism, health() is "is something
 * wrong right now", and this composite's own health() only has to decide
 * which backend's "is something wrong right now" answer to relay, since
 * sendinput.c's is always the same non-answer.
 */
static void backend_health(apad_backend_health *out)
{
    apad_backend_vigem.health(out);
}

/* ======================================================================== */
/* Pad calls forward to apad_backend_vigem, unconditionally.                */
/* ======================================================================== */
/* server/src/server.c calls create_pad/update_pad/destroy_pad with no NULL
 * check (they are mandatory hooks, unlike the five KBM ones) — forwarding
 * unconditionally is correct even when g_vigem_init_rc != 0, because
 * apad_backend_vigem's own create_pad() already checks g_client != NULL
 * internally and fails cleanly (returns -1) rather than crashing; that is
 * the SAME path a live ViGEmBus disconnect after a successful init would
 * take, so this file needs no separate guard duplicating vigem.c's own. */

static int win32_create_pad(int slot, apad_pad_type type)
{
    return apad_backend_vigem.create_pad(slot, type);
}

static int win32_update_pad(int slot, const apad_pad_state *state)
{
    return apad_backend_vigem.update_pad(slot, state);
}

static int win32_poll_feedback(int slot, apad_feedback *out)
{
    return apad_backend_vigem.poll_feedback(slot, out);
}

static void win32_destroy_pad(int slot)
{
    apad_backend_vigem.destroy_pad(slot);
}

/* ======================================================================== */
/* KBM calls forward to apad_backend_sendinput, unconditionally.            */
/* ======================================================================== */

static uint32_t win32_kbm_caps(void)
{
    return apad_backend_sendinput.kbm_caps();
}

static int win32_create_kbm(int slot, apad_kbm_device dev)
{
    return apad_backend_sendinput.create_kbm(slot, dev);
}

static int win32_kbm_events(int slot, const apad_kbm_event_out *ev, size_t n)
{
    return apad_backend_sendinput.kbm_events(slot, ev, n);
}

static int win32_mouse_motion(int slot, const apad_mouse_motion *m)
{
    return apad_backend_sendinput.mouse_motion(slot, m);
}

static void win32_destroy_kbm(int slot, apad_kbm_device dev)
{
    apad_backend_sendinput.destroy_kbm(slot, dev);
}

const apad_backend apad_backend_win32 = {
    .init          = backend_init,
    .create_pad    = win32_create_pad,
    .update_pad    = win32_update_pad,
    .poll_feedback = win32_poll_feedback,
    .destroy_pad   = win32_destroy_pad,
    .shutdown      = backend_shutdown,
    .name          = "win32",
    .health        = backend_health,
    .kbm_caps      = win32_kbm_caps,
    .create_kbm    = win32_create_kbm,
    .kbm_events    = win32_kbm_events,
    .mouse_motion  = win32_mouse_motion,
    .destroy_kbm   = win32_destroy_kbm
};
