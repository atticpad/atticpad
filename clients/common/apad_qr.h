/* apad_qr.h — §10.3 QR decode, on top of vendored quirc.
 *
 * THE PROTOCOL LIVES IN C, NOT KOTLIN (docs/CONVENTIONS.md), and that applies here too:
 * a camera frame comes in, quirc finds and decodes a QR symbol, and the
 * resulting byte string goes STRAIGHT into apad_pair_uri_parse() — the same
 * function the deep-link path uses. Kotlin never sees a pixel and never sees
 * a raw URI string; it sees an address and a secret, or an error code.
 *
 * This file knows nothing about `atticpad://`. It is a thin adapter between
 * a grayscale image and a NUL-terminated byte string; the URI grammar lives
 * entirely in core/, per docs/PROTOCOL.md §10.3's "three independent
 * implementations ... must agree byte for byte."
 *
 * Threading: independent of apad_client. A `apad_qr` handle has no relation
 * to a session and may be driven from whatever thread owns the camera
 * callback — it is not the net thread's single-thread contract from
 * apad_client.h.
 */
#ifndef ATTICPAD_COMMON_APAD_QR_H
#define ATTICPAD_COMMON_APAD_QR_H

#include <stdint.h>

#include "atticpad/atticpad.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct apad_qr apad_qr;

/* One recognizer, reused frame to frame for the lifetime of one in-app scan.
 * NULL on allocation failure. */
apad_qr *apad_qr_create(void);
void apad_qr_destroy(apad_qr *d);

/*
 * Feeds ONE grayscale frame to the recognizer and, if it contains a QR code
 * that decodes as a conforming §10.3 pairing URI, fills `*out`.
 *
 *   y       the grayscale plane, `height` rows of `stride` bytes each; only
 *           the first `width` bytes of each row are the image (Camera2's
 *           YUV_420_888 Y plane's rowStride commonly exceeds width — this
 *           function strips that padding, the caller does not have to).
 *   width, height   > 0.
 *   stride  >= width.
 *
 * Returns APAD_OK with `*out` filled, or:
 *
 *   APAD_ERR_STATE    no QR code was found in this frame at all, or the one
 *                     found could not even be read as a QR symbol (blur,
 *                     bad angle, wrong ECC). This is the ORDINARY per-frame
 *                     outcome while the user is still lining up the camera —
 *                     not an error worth surfacing.
 *   APAD_ERR_VERSION  a QR code decoded and its payload parsed as a §10.3
 *                     URI, but `v` is not 1 — "this server is newer than I
 *                     am". Reused from apad_pair_uri_parse(), same meaning.
 *   APAD_ERR_ARG      a QR code decoded but its payload was not a conforming
 *                     §10.3 URI at all (some other QR code entirely — a
 *                     poster, a URL, a wifi-config code), or the arguments to
 *                     this function were invalid.
 *
 * `*out` is written ONLY on APAD_OK, matching apad_pair_uri_parse()'s own
 * contract — never partially filled.
 *
 * Hostile input past this point: a QR code is whatever someone printed, and
 * everything downstream of quirc_decode() treats the payload exactly as
 * apad_pair_uri_parse() already documents it must.
 */
int apad_qr_decode(apad_qr *d, const uint8_t *y, int width, int height,
                   int stride, apad_pair_uri *out);

#ifdef __cplusplus
}
#endif

#endif /* ATTICPAD_COMMON_APAD_QR_H */
