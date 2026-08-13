/* clients/3ds/source/time_3ds.c
 *
 * Monotonic milliseconds for the 3DS (atticpad.h "Platform shim").
 *
 * osGetTime() (libctru, 3ds/os.h) returns milliseconds since 1900-01-01,
 * backed by the console's RTC, as a u64. This function truncates it to the
 * uint32_t the wire protocol and apad_time_after()/apad_time_since() expect
 * -- the wrap is real and intended, not a bug (see core/include/atticpad.h's
 * comment on apad_ticks_ms: "wraps at 2^32 (~49.7 days)... every comparison
 * goes through" the wrap-safe helpers). Nothing in this client treats the
 * value as an absolute time; it exists only to be subtracted from a later
 * call to the same function.
 */

#include <3ds.h>

#include "atticpad/atticpad.h"

uint32_t apad_ticks_ms(void)
{
    return (uint32_t)(osGetTime() & 0xFFFFFFFFull);
}
