/* clients/3ds/diagnostics/udp_diagnostic.c
 *
 * NOT built by clients/3ds/Makefile (this directory is not in SOURCES) --
 * this is instrumentation kept for cheap reinstatement, not shipped client
 * code. It earned its place once already: it turned three rounds of blind
 * netload-and-guess into one netload with a decisive answer (libctru's SOC
 * returning EINVAL from bind() to port 0 -- no ephemeral-port assignment --
 * which is what led to the shim/net_bsd.c fix in commit a5b492d).
 *
 * To reinstate on this or another libctru-based platform (PSP/Vita/Switch
 * share the "every target has a BSD-sockets-shaped API" assumption,
 * docs/DESIGN.md S3, so the same probe shape applies if apad_udp_open() ever fails
 * mysteriously there too):
 *
 *   1. Copy the two functions below into the platform's main.c (or drop this
 *      file into that platform's SOURCES directory and declare
 *      apad3ds_soc_bufsize()-equivalent accordingly).
 *   2. Add the same #include set this file needs: <errno.h> <string.h>
 *      <unistd.h> <sys/socket.h> <netinet/in.h> <arpa/inet.h>, plus
 *      whatever the platform calls its socInit()-equivalent header.
 *   3. Call run_udp_diagnostic(<init-result>) from the apad_udp_open()
 *      failure path instead of just printing "failed" and giving up.
 *   4. Once the real bug is found and fixed in shim/, take the probe back
 *      out of the shipped client -- it opens six sockets at boot, which is
 *      fine for one diagnostic netload and wrong for every run after that.
 *
 * The three previous rounds on this exact console, for context on why this
 * shape (report ALL of several candidate failure points in one shot, with
 * errno, rather than one hypothesis at a time) mattered:
 *   - shim/net_bsd.c missing <arpa/inet.h>          (commit 975d01b)
 *   - shim/net_bsd.c socket() with protocol 0        (commit ccc47db, not
 *     actually the bug this file's probe was built for, but fixed anyway)
 *   - shim/net_bsd.c binding unconditionally, even   (commit a5b492d --
 *     for an ephemeral (port 0) request                found BY this probe)
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <3ds.h>

#include "atticpad/atticpad.h"

/* Declared in soc_3ds.c; not part of this file. */
size_t apad3ds_soc_bufsize(void);

static void print_errno_line(const char *label, int rc)
{
    printf("%s -> rc=%d errno=%d (%s)\n", label, rc, errno, strerror(errno));
}

void run_udp_diagnostic(Result soc_rc)
{
    int fd1, fd2;
    int rc;
    struct sockaddr_in sa;

    consoleClear();
    printf("AtticPad 3DS -- UDP diagnostic\n\n");
    printf("soc buffer: %u bytes (0x%X), 0x1000-aligned\n",
           (unsigned)apad3ds_soc_bufsize(), (unsigned)apad3ds_soc_bufsize());
    printf("socInit result: 0x%08lX\n\n", (unsigned long)soc_rc);

    errno = 0;
    fd1 = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    printf("1. socket(..,SOCK_DGRAM,IPPROTO_UDP) -> fd=%d errno=%d (%s)\n",
           fd1, errno, strerror(errno));

    errno = 0;
    fd2 = socket(AF_INET, SOCK_DGRAM, 0);
    printf("2. socket(..,SOCK_DGRAM,0)           -> fd=%d errno=%d (%s)\n",
           fd2, errno, strerror(errno));

    if (fd1 >= 0) {
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons(0);
        errno = 0;
        rc = bind(fd1, (struct sockaddr *)&sa, (socklen_t)sizeof sa);
        print_errno_line("3. bind(fd1, ANY, port 0)   ", rc);
    } else {
        printf("3. bind(fd1, ANY, port 0)    -> SKIPPED (fd1 invalid)\n");
    }

    if (fd2 >= 0) {
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons((uint16_t)APAD_DEFAULT_PORT);
        errno = 0;
        rc = bind(fd2, (struct sockaddr *)&sa, (socklen_t)sizeof sa);
        print_errno_line("4. bind(fd2, ANY, port 21100)", rc);
    } else {
        printf("4. bind(fd2, ANY, port 21100) -> SKIPPED (fd2 invalid)\n");
    }

    if (fd1 >= 0) (void)close(fd1);
    if (fd2 >= 0) (void)close(fd2);

    {
        apad_sock *s5 = apad_udp_open(0);
        printf("5. apad_udp_open(0)     -> %s\n", s5 != NULL ? "OK" : "NULL");
        if (s5 != NULL) apad_udp_close(s5);
    }
    {
        apad_sock *s6 = apad_udp_open((uint16_t)APAD_DEFAULT_PORT);
        printf("6. apad_udp_open(21100) -> %s\n", s6 != NULL ? "OK" : "NULL");
        if (s6 != NULL) apad_udp_close(s6);
    }

    printf("\nPress B to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_B) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}
