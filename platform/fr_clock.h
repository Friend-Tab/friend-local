/* fr_clock.h — the two things the library needs from the clock.
 *
 * The library has no clock of its own: friend_tick takes the time, and
 * every timestamp and "three days ago" in every prompt is worked out
 * from it. It also takes a fixed offset from UTC rather than a timezone,
 * so it never has to carry a zone database — which means somebody has
 * to ask the operating system what the offset is. That is this.
 */

#ifndef FR_CLOCK_H
#define FR_CLOCK_H

#include <stdint.h>

uint64_t fr_now_unix_ms(void);

/* What the clock on the wall reads, relative to UTC. */
int32_t fr_local_utc_offset_minutes(void);

#endif /* FR_CLOCK_H */
