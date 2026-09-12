/*
 * clients/psp/source/net_psp.h -- PSP network bring-up, published to the
 * frame loop by a thread. See net_psp.c for the ordering rules.
 */
#ifndef ATTICPAD_PSP_NET_H
#define ATTICPAD_PSP_NET_H

#define APAD_PSP_NET_SLOTS 16    /* saved-connection slots the PSP UI offers */

typedef struct {
    int  state;          /* last PSP_NET_APCTL_STATE_* seen, -1 before start */
    int  ready;          /* 1 once GOT_IP: sockets are usable                */
    int  failed;         /* 1 if bring-up gave up                            */
    int  joinfail;       /* 1 if the failure is "the network refused us":    *
                          * retryable, and another slot may work            */
    int  err;            /* the failing call's return code, when failed      */
    const char *stage;   /* which call failed, for the fatal screen          */
    char ip[16];         /* this PSP's address, display only                 */
    int  slot;           /* the saved-connection slot being used (1..n)      */
    int  power_save;     /* 1 if the console's WLAN power save is on: the   *
                          * radio sleeps between beacons and UDP suffers    */
    char name[32];       /* that slot's name, as the PSP's settings show it  */
} apad_psp_net_status;

/* Saved connections, read from the console's settings: no network needed.
 * Returns how many exist; slots[] receives their 1-based numbers. */
int  apad_psp_net_slots(int *slots, int max);
/* The name the user gave a slot (falls back to its SSID). */
void apad_psp_net_slot_name(int slot, char *out, unsigned cap);

int  apad_psp_net_modules(void);
/* Start (or, after a join failure, restart) bring-up on `slot`. */
int  apad_psp_net_start(int slot);
void apad_psp_net_get(apad_psp_net_status *out);

/* Live association state, queried straight from the radio (no thread, no
 * network traffic): 1 while the link holds an IP, 0 the moment it drops.
 * A session that dies on the idle timeout and a reconnect that gets no
 * answer are the same event when this returns 0 -- the AP went away, and
 * the fix is to bring the link back up, not to keep dialling into the void. */
int  apad_psp_net_associated(void);

#endif /* ATTICPAD_PSP_NET_H */
