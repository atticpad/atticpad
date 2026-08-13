/* server/host/common/qr.h -- §10.3 pairing-URI QR rendering for the server
 * UI, on BOTH hosts (docs/PROTOCOL.md §10.3, docs/DESIGN.md §5.5 "a QR code on the
 * server, for clients with a camera").
 *
 * Was server/host/linux/qr.h. Moved here unchanged except for this comment
 * and the inet_pton include below: nothing in it was ever Linux-specific --
 * core's apad_pair_uri_build() builds the URI and the vendored qrcodegen
 * encodes it, neither of which touches an OS API. It sat under linux/ only
 * because Linux was the sole host with a UI.
 *
 * Two things live here, both thin:
 *
 *   1. qr_build_pairing_uri() -- a one-line wrapper around core's
 *      apad_pair_uri_build() (core/src/codec.c, declared in
 *      atticpad/atticpad.h). THIS FILE MUST NEVER REIMPLEMENT §10.3's
 *      grammar -- that is the whole reason the builder lives in core/:
 *      three independent implementations (this server, the Android client,
 *      the 3DS client) parse the string it produces, and duplicating the
 *      encoder duplicates its bugs across all three the moment one drifts.
 *
 *   2. qr_render_pairing_svg() -- renders a qrcodegen bitmap (see below) as
 *      an inline SVG document. This part is NOT vendored and NOT
 *      spec-governed: nothing on the wire or in §10.3 dictates how a QR
 *      code's modules become pixels, so this is ordinary server-side
 *      rendering code, written for this project.
 *
 * Vendoring: the actual QR encoder is nayuki/QR-Code-generator's C port,
 * vendored byte-identical at server/vendor/qrcodegen/ -- see that
 * directory's README.md for pin, licence and hashes. Pulled in with a
 * single #include of the vendored .c file, for the same
 * cannot-touch-scripts/build.sh reason server/host/linux/{webui,assets,
 * ipaddr}.h are header-only (see server/vendor/README.md's own "How it is
 * compiled in" section for the full argument, and server-dev agent memory
 * server-ui-and-rtt for the precedent this follows).
 *
 * Header-only, included only by server/host/linux/webui.h, which is in turn
 * included only by main.c -- so despite pulling in a third-party .c body,
 * this is still exactly one translation unit, same as every other file in
 * this directory.
 */
#ifndef ATTICPAD_HOST_COMMON_QR_H
#define ATTICPAD_HOST_COMMON_QR_H

/* inet_pton(): <arpa/inet.h> on POSIX, <ws2tcpip.h> on Winsock. This is
 * the file's ONLY OS call -- sockcompat.h already pulls in the Windows
 * side (and gets the winsock2-before-windows.h ordering right), so the
 * POSIX header is all that is left to name here. */
#include "sockcompat.h"
#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atticpad/atticpad.h"

/* The vendored encoder itself. See this file's top comment and
 * server/vendor/README.md for why an #include of a third-party .c file is
 * the correct move here, not a shortcut. Not one byte of the included file
 * has been modified from upstream. */
#include "../../vendor/qrcodegen/qrcodegen.c"

/* ---- 1. the URI: core's job, this is only a thin, honest wrapper -------- */

/* Buffer size for qr_build_pairing_uri()'s `out`: APAD_PAIR_URI_MAX bytes
 * (docs/PROTOCOL.md §10.3's own ceiling, core/include/atticpad/protocol.h)
 * plus the NUL apad_pair_uri_build() always writes on success. This is a
 * buffer-sizing constant, not a second copy of the protocol limit -- it is
 * defined FROM APAD_PAIR_URI_MAX, same discipline as apadserver.h's
 * APAD_PAIRING_SECRET_MAX (server-dev agent memory pairing-state-machine). */
#define QR_PAIR_URI_BUF (APAD_PAIR_URI_MAX + 1u)

/* Builds the §10.3 URI for `ip_str` (a dotted-quad string, as
 * host_enumerate_own_ipv4() in ipaddr.h already produces), `port` (host
 * order) and `secret` into `out[QR_PAIR_URI_BUF]`. Returns the byte count
 * written (apad_pair_uri_build()'s own convention -- NOT APAD_OK, see that
 * function's doc comment in atticpad.h), or a negative apad_result on
 * failure: a malformed IP string, port 0, or a secret that fails §10.1
 * (should never happen for a secret this server generated itself, but this
 * function does not special-case that -- a rejection here is a real bug
 * report, not a spec violation, so it is surfaced rather than hidden). */
static int qr_build_pairing_uri(char *out, const char *ip_str, uint16_t port,
                                const char *secret)
{
    apad_addr addr;
    struct in_addr in4;

    memset(&addr, 0, sizeof addr);
    if (inet_pton(AF_INET, ip_str, &in4) != 1) {
        return APAD_ERR_ARG;
    }
    memcpy(addr.ip, &in4, sizeof addr.ip);   /* both network order, 4 bytes */
    addr.port = port;

    return apad_pair_uri_build(out, QR_PAIR_URI_BUF, &addr, secret);
}

/* ---- 2. the SVG: ours, not the spec's ------------------------------------
 *
 * Rendering choice: inline SVG, generated server-side and served as its own
 * response (image/svg+xml) from GET /api/pair/qr.svg, rather than a PNG/
 * bitmap or a <canvas>-drawn QR in client-side JS.
 *
 *   - No image codec needed. A PNG encoder is a much bigger dependency for
 *     a black-and-white grid than this 40-line renderer; this project
 *     already refuses a framework/CDN for the UI (docs/CONVENTIONS.md), and a raster
 *     codec would be the same kind of unjustified addition.
 *   - No canvas/JS QR-drawing library either -- that would mean shipping a
 *     SECOND QR implementation (client-side) purely to turn the same
 *     module grid into pixels, when the server already has the grid in
 *     hand from qrcodegen_getModule().
 *   - SVG is resolution-independent: a phone held at arm's length from a
 *     monitor sees crisp module edges regardless of the monitor's pixel
 *     density, which a fixed-size raster would not guarantee.
 *   - <img src="/api/pair/qr.svg"> is one HTTP GET with no JS QR logic in
 *     the page at all -- consistent with assets.h's "no framework, no
 *     image pipeline" framing.
 *
 * Legibility, made explicit rather than left to chance:
 *   - QUIET_MODULES = 4, the QR standard's own minimum quiet zone (ISO/IEC
 *     18004) -- cropping it is a common reason an otherwise-valid code
 *     fails to scan.
 *   - PX_PER_MODULE = 8: comfortably above the ~4 px/module floor generally
 *     recommended for a camera scanning a code off a monitor at a short
 *     distance: enough that a webcam-quality phone camera resolves module
 *     edges cleanly rather than aliasing them.
 *   - shape-rendering="crispEdges" on the root <svg> so a browser does not
 *     anti-alias module boundaries into a blurry gradient at small
 *     on-screen sizes -- the exact failure mode QUIET_MODULES/PX_PER_MODULE
 *     are trying to avoid.
 *   - An explicit white background rect: a QR code's contrast requirement
 *     is between LIGHT and DARK modules, not between dark modules and
 *     "whatever background colour happens to be behind an <img> tag" --
 *     assets.h's page uses a dark theme (#14161a), so this is not
 *     decoration, it is required for the code to scan at all against this
 *     page's own background.
 *   - Consecutive dark modules on one row are merged into a single <rect>
 *     (a run-length pass) rather than one <rect> per module: same rendered
 *     grid, far fewer SVG elements, nothing to do with legibility itself.
 *
 * `text` is normally the §10.3 URI (<= APAD_PAIR_URI_MAX = 128 bytes),
 * comfortably inside a low QR version, but this function does not assume
 * that -- it tries every version from qrcodegen_VERSION_MIN up so a caller
 * error never turns into a silent truncation. qrcodegen_Ecc_MEDIUM (~15%
 * recovery) is used rather than LOW: a phone camera reading a code off a
 * monitor deals with screen glare and moire that a printed code does not,
 * and MEDIUM does not meaningfully grow the URI-sized codes this function
 * actually renders.
 *
 * Returns a malloc'd, NUL-terminated SVG document (caller frees) on
 * success, or NULL if the text does not fit any QR version (cannot happen
 * for a <=128-byte §10.3 URI, but qrcodegen_encodeText() itself can fail,
 * and returning NULL rather than asserting is what lets the HTTP handler
 * turn that into a normal error response instead of crashing the server
 * over a malformed profile string finding its way in here some other way).
 */
#define QR_QUIET_MODULES   4
#define QR_PX_PER_MODULE   8

static char *qr_render_pairing_svg(const char *text)
{
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    int     size, dim_px, y;
    size_t  cap, len = 0;
    char   *out;

    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        return NULL;
    }
    size   = qrcodegen_getSize(qr);
    dim_px = (size + 2 * QR_QUIET_MODULES) * QR_PX_PER_MODULE;

    /* Worst case: every module its own <rect> (no run got merged), each
     * needing at most ~48 bytes ("<rect x="NNN" y="NNN" width="NNN"
     * height="8"/>\n") plus a few hundred bytes of fixed header/footer.
     * size <= 177 (version 40) so size*size <= 31329 -- this is a one-shot
     * scratch allocation for a single HTTP response, freed right after
     * ui_send_response() copies it out, not a resource anything holds. */
    cap = (size_t)size * (size_t)size * 48u + 1024u;
    out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }

#define QSV_APPEND(...) \
    do { \
        int _n = snprintf(out + len, cap - len, __VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= cap - len) { free(out); return NULL; } \
        len += (size_t)_n; \
    } while (0)

    QSV_APPEND("<svg xmlns=\"http://www.w3.org/2000/svg\" "
              "viewBox=\"0 0 %d %d\" width=\"%d\" height=\"%d\" "
              "shape-rendering=\"crispEdges\">",
              dim_px, dim_px, dim_px, dim_px);
    QSV_APPEND("<rect width=\"%d\" height=\"%d\" fill=\"#ffffff\"/>",
              dim_px, dim_px);

    for (y = 0; y < size; y++) {
        int x = 0;
        while (x < size) {
            int run_x, run_len;

            if (!qrcodegen_getModule(qr, x, y)) {
                x++;
                continue;
            }
            run_x = x;
            run_len = 0;
            while (x < size && qrcodegen_getModule(qr, x, y)) {
                run_len++;
                x++;
            }
            QSV_APPEND("<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
                      "fill=\"#000000\"/>",
                      (run_x + QR_QUIET_MODULES) * QR_PX_PER_MODULE,
                      (y + QR_QUIET_MODULES) * QR_PX_PER_MODULE,
                      run_len * QR_PX_PER_MODULE, QR_PX_PER_MODULE);
        }
    }

    QSV_APPEND("</svg>\n");
#undef QSV_APPEND

    return out;
}

#endif /* ATTICPAD_HOST_COMMON_QR_H */
