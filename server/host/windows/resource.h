/* server/host/windows/resource.h -- resource IDs for atticpad.rc.
 *
 * Shared between the .rc (compiled by windres) and the C that calls
 * LoadIcon()/LoadImage(), so the number lives in exactly one place. */
#ifndef ATTICPAD_HOST_WINDOWS_RESOURCE_H
#define ATTICPAD_HOST_WINDOWS_RESOURCE_H

/* The application/tray icon. Explorer uses the LOWEST-numbered icon
 * resource as the .exe's shell icon, so this stays 1. */
#define IDI_APPICON 1

#endif
