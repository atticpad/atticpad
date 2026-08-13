/* server/host/common/ui_mdns_status.h -- the host-agnostic snapshot of §7
 * tier-1 (mDNS) status that server/host/common/webui.h's /api/state reports.
 *
 * webui.h used to include server/host/linux/mdns.h directly and read its
 * `mdns_responder` type. That worked while webui.h was Linux-only; it
 * cannot work once the same webui.h is also compiled into the Windows host
 * (server/host/windows/main.c), because mdns_responder is a live raw
 * multicast socket plus per-interface RFC 6762 rate-limit state -- real
 * mDNS is explicitly OUT OF SCOPE for the Windows host in this task (see
 * this task's brief: "Leave mdns.h where it is") and server/host/linux/
 * mdns.h is neither moved nor made to compile on Windows.
 *
 * This struct is the seam that makes that possible: it carries exactly the
 * fields /api/state's "mdns" JSON block already reported, with no reference
 * to a socket, a schedule, or anything else that only makes sense while a
 * responder is actually running.
 *
 *   - server/host/linux/mdns.h fills one of these from a live
 *     mdns_responder via mdns_ui_status() (defined there, not here -- that
 *     function is the only thing in the whole UI stack that is allowed to
 *     know what a mdns_responder's fields mean, same "one place owns the
 *     type" discipline as everything else in this split).
 *   - A host with no mDNS implementation at all (server/host/windows/
 *     main.c, today) builds one directly with
 *     ui_mdns_status_not_implemented() below -- no mdns.h include at all.
 *
 * webui.h only ever reads this struct. That is what keeps it buildable on a
 * host that never links server/host/linux/mdns.h.
 *
 * Header-only, for the same build-script-constraint reason as every other
 * file in this directory (see webui.h's top comment).
 */
#ifndef ATTICPAD_HOST_COMMON_UI_MDNS_STATUS_H
#define ATTICPAD_HOST_COMMON_UI_MDNS_STATUS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Matches server/host/linux/mdns.h's own MDNS_TXT_MAX_ITEMS (8) and the
 * `items[MDNS_TXT_MAX_ITEMS][96]` buffer mdns_txt_items() fills there --
 * mdns_ui_status() copies out of exactly that shape, so these two constants
 * are sized to never truncate a real TXT record set. Kept as independent
 * constants (not #include-shared with mdns.h, which this file must not
 * depend on) -- mdns_ui_status() is the one place a drift between them would
 * ever matter, and it is a two-line function to keep in sync by hand. */
#define UI_MDNS_TXT_MAX_ITEMS 8
#define UI_MDNS_TXT_ITEM_MAX  96

typedef struct {
    /* 0 on a host with no mDNS implementation at all (Windows, today) --
     * THE FIELD TO BRANCH ON. Every other field is zeroed/empty in that
     * case except `message`, which explains what to use instead (tier 2
     * broadcast discovery, tier 3 manual IP entry -- docs/PROTOCOL.md §7). */
    int      implemented;
    int      ok;              /* mdns_state == MDNS_STATE_RUNNING           */
    const char *state;        /* "running"/"disabled"/"bind_failed"/...,    *
                                * or "not_implemented" -- static storage,    *
                                * never freed, never mutated through this    *
                                * pointer                                    */
    const char *service;      /* MDNS_SERVICE ("_atticpad._udp.local"), or  *
                                * "" when !implemented                       */
    char     instance[256];   /* "<name>._atticpad._udp.local"              */
    char     host[128];       /* "atticpad-<hostname>.local"                */
    uint16_t bind_port;
    uint16_t service_port;
    char     message[200];    /* a sentence, what happened                  */
    char     remedy[200];     /* what a human can DO about it, or ""        */
    char     txt[UI_MDNS_TXT_MAX_ITEMS][UI_MDNS_TXT_ITEM_MAX];
    size_t   ntxt;
    uint32_t queries_rx;
    uint32_t responses_tx;
    uint32_t dropped_tx;
    uint32_t announces_tx;
} ui_mdns_status;

/* What a host with no mDNS implementation at all reports. docs/PROTOCOL.md
 * §7's tier-1 obligation is "if it is unavailable, the server UI MUST say
 * so" -- not "every host must implement it" -- and tiers 2 (broadcast
 * DISCOVER) and 3 (manual IP entry, which this same UI already displays
 * prominently) exist precisely to carry that load. `message` says so in
 * words a human reads in the UI, not just `implemented:false` for a front
 * end to interpret. */
/* `static inline`, not plain `static`: this is the WINDOWS host's
 * constructor and the Linux host never calls it (it has a real responder
 * and uses mdns_ui_status() instead). A plain static would then be an
 * unused function in every Linux TU that includes this header, which is
 * -Werror=unused-function under this project's build flags. GCC and Clang
 * both exempt `static inline` from that warning precisely for header-only
 * helpers like this one. */
static inline void ui_mdns_status_not_implemented(ui_mdns_status *out)
{
    memset(out, 0, sizeof *out);
    out->implemented = 0;
    out->ok          = 0;
    out->state       = "not_implemented";
    out->service     = "";
    (void)snprintf(out->message, sizeof out->message,
                  "automatic discovery is not available on this PC -- "
                  "devices can still find it by searching, or you can "
                  "enter its address on them directly (shown above)");
    out->remedy[0] = '\0';
}

#endif /* ATTICPAD_HOST_COMMON_UI_MDNS_STATUS_H */
