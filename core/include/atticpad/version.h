/* AtticPad project version — the single source of truth.
 *
 * This is the PRODUCT version. It is NOT the protocol version: the wire
 * format is AtticPad protocol v1, frozen 2026-08-09, and lives in
 * docs/PROTOCOL.md as APAD_VERSION in protocol.h. The two move independently
 * — the product will reach 1.0 long before the wire format reaches v2, and a
 * v2 wire format would not reset the product version.
 *
 * Scheme: 0.<milestone>.<patch> until the project is broadly usable.
 *   0.1.x  M1 — core, Linux server, conformance vectors, CI
 *   0.2.x  M2 — 3DS client, mapping engine, gyro
 *   0.3.x  M3 — Android
 *   0.4.x  M4 — Windows server (never released; folded into 0.5.0)
 *   0.5.x  server-driven touch layouts, visual pad tester
 *   1.0.0  when a non-technical user can install it and play a game
 *
 * Anything that ships an artifact should display this: the 3DS client's top
 * screen, the server's startup banner, and the SMDH description. A bug report
 * that does not name a version costs more to triage than the version costs to
 * print.
 */
#ifndef ATTICPAD_VERSION_H
#define ATTICPAD_VERSION_H

#define APAD_VERSION_MAJOR 0
#define APAD_VERSION_MINOR 5
#define APAD_VERSION_PATCH 0

/* Bump to an empty string for a release build. "-dev" means built from a
 * working tree that may not match any tag. */
#define APAD_VERSION_SUFFIX "-rc3"

#define APAD_VERSION_STR "0.5.0" APAD_VERSION_SUFFIX

#endif /* ATTICPAD_VERSION_H */
