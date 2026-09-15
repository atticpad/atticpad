/* Wrapper TU -- see sh_codec.c.
 *
 * OUTCOME B of the step-1 shim question (references/nds/README.md):
 * shim/net_bsd.c's apad_udp_recv() never times out on this stack when a
 * positive timeout is asked for and nothing ever answers -- DSWiFi's lwIP
 * only re-checks its own deadline when a signal wakes the waiting cothread,
 * and a socket nothing answers never signals (root-caused against DSWiFi's
 * vendored lwIP sys.c; see shim/net_nds.c's header for the exact function
 * and the citation). This platform therefore gets its own shim,
 * shim/net_nds.c: every function byte-for-byte net_bsd.c's except
 * apad_udp_recv(), which keeps its own deadline outside lwIP and polls with
 * a proven zero-timeout select() between cothread_yield_irq(IRQ_VBLANK)
 * calls.
 *
 * Supersedes this file's own former note ("Outcome A ... shim/net_bsd.c
 * compiles and links UNCHANGED ... so this platform gets no net_nds.c"),
 * which measured only a live session where a datagram was always already
 * queued by the time the timed recv ran -- the one case where the bug is
 * invisible. */
#include "../../../shim/net_nds.c"
