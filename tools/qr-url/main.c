/*
 * qr-url -- print a QR code, as SVG, for a string given on the command line.
 *
 * WHY THIS EXISTS: the 3DS install path is "FBI -> Remote Install -> scan a
 * QR of the .cia URL", which removes the SD-card-and-card-reader step
 * entirely for a console already on the Wi-Fi. That QR has to come from
 * somewhere, and it must be the SAME encoder the server already uses for
 * pairing (server/host/common/qr.h, over the vendored qrcodegen) rather than
 * a website or a second library: one encoder, one set of behaviours, one
 * thing to trust.
 *
 * It takes the text as an argument instead of hardcoding a URL so a release
 * workflow can point it at whatever asset URL that tag actually has, and so
 * regenerating the committed image is a command someone can read.
 *
 *     tools/qr-url/build.sh
 *     ./tools/qr-url/qr-url 'https://example/atticpad-3ds.cia' > qr.svg
 *
 * Rasterise however you like; docs/img/fbi-install-qr.png was produced with
 * headless Chromium, and the result was decoded back with the vendored quirc
 * to prove the image round-trips to the exact URL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qr.h"

int main(int argc, char **argv)
{
    char *svg;

    if (argc != 2 || argv[1][0] == '\0') {
        fprintf(stderr, "usage: %s <text-to-encode>\n", argv[0]);
        return 2;
    }

    svg = qr_render_pairing_svg(argv[1]);
    if (svg == NULL) {
        fprintf(stderr, "qr-url: %s does not fit any QR version (or malloc failed)\n",
                argv[1]);
        return 1;
    }
    fputs(svg, stdout);
    fputc('\n', stdout);
    free(svg);
    return 0;
}
