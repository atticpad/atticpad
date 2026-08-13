/* apad_qr.c — see apad_qr.h.
 *
 * The ONLY file in this app that includes quirc.h. Everything past
 * apad_pair_uri_parse() is core/'s job, not this file's, and everything
 * before it — finding a QR symbol in a bitmap — is quirc's job, not ours.
 * `clients/vendor/quirc/README.md` has the provenance and the reason this
 * lives in `clients/vendor/`, not under `clients/android/`.
 */

#include <stdlib.h>
#include <string.h>

#include "apad_qr.h"
#include "quirc.h"

struct apad_qr {
    struct quirc *q;
};

apad_qr *apad_qr_create(void)
{
    apad_qr *d = (apad_qr *)calloc(1, sizeof *d);

    if (d == NULL) {
        return NULL;
    }
    d->q = quirc_new();
    if (d->q == NULL) {
        free(d);
        return NULL;
    }
    return d;
}

void apad_qr_destroy(apad_qr *d)
{
    if (d == NULL) {
        return;
    }
    if (d->q != NULL) {
        quirc_destroy(d->q);
    }
    free(d);
}

int apad_qr_decode(apad_qr *d, const uint8_t *y, int width, int height,
                   int stride, apad_pair_uri *out)
{
    uint8_t *buf;
    int i, count;
    /* No QR code recognisable at all is the ordinary per-frame outcome while
     * the user lines up the camera, so it is the default rather than
     * something this function has to notice separately. */
    int last_rc = APAD_ERR_STATE;

    if (d == NULL || d->q == NULL || out == NULL || y == NULL
        || width <= 0 || height <= 0 || stride < width) {
        return APAD_ERR_ARG;
    }

    /* A no-op once a prior frame already resized to the same dimensions —
     * Camera2 delivers a fixed resolution for the lifetime of one capture
     * session, so this runs once in practice. */
    if (quirc_resize(d->q, width, height) < 0) {
        return APAD_ERR_ARG;           /* OOM inside quirc */
    }
    buf = quirc_begin(d->q, NULL, NULL);
    if (buf == NULL) {
        return APAD_ERR_ARG;
    }
    if (stride == width) {
        memcpy(buf, y, (size_t)width * (size_t)height);
    } else {
        /* Camera2's Y plane rowStride is commonly wider than the image
         * (alignment padding); quirc wants a tightly packed plane. */
        for (i = 0; i < height; i++) {
            memcpy(buf + (size_t)i * (size_t)width,
                   y + (size_t)i * (size_t)stride, (size_t)width);
        }
    }
    quirc_end(d->q);

    count = quirc_count(d->q);
    for (i = 0; i < count; i++) {
        struct quirc_code code;
        struct quirc_data data;

        quirc_extract(d->q, i, &code);
        if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
            continue;                  /* unreadable as a QR code; try the
                                        * next one, if this frame has more */
        }
        /* quirc NUL-terminates data.payload itself (decode.c) and always
         * leaves room for the terminator, so this is a valid C string
         * regardless of what the code actually contained. Everything past
         * this call is core/'s job — apad_pair_uri_parse() treats it as
         * hostile input, exactly as its own contract requires. */
        last_rc = apad_pair_uri_parse(out, (const char *)data.payload);
        if (last_rc == APAD_OK) {
            return APAD_OK;
        }
    }
    return last_rc;
}
