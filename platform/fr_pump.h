/* fr_pump.h — doing the work the friend asks for.
 *
 * The library asks for two kinds of work: requests over the network, and
 * reads and writes of files it has been given the paths of. Every host
 * routes them the same way, so it is done once, here: files now and in
 * place, everything else to the transport.
 *
 * Files synchronously, inside the call. A local read of a few hundred
 * kilobytes is faster than the thread it would take to avoid blocking
 * on, and the library is written so that a request completing before
 * friend_pending_io returns is ordinary rather than surprising.
 *
 * What a host does around it is its own: when it ticks, and what each
 * event means to whoever is looking.
 *
 *     for (;;) {
 *         fr_pump_io(fr, net, wait_ms);
 *         friend_tick(fr, fr_now_unix_ms());
 *         while (friend_next_event(fr, &event)) ...
 *     }
 */

#ifndef FR_PUMP_H
#define FR_PUMP_H

#include "friend.h"
#include "fr_net.h"

/* Does every request the friend has pending — the file work here and
 * now, the rest handed to net — tells net which ones the friend gave up
 * on, and then waits up to wait_ms for the network to make progress.
 *
 * net may be null: then network work fails at once, the way a dead
 * network fails it, file work still happens, and the wait is a sleep. */
void fr_pump_io(Friend_Context *fr, Fr_Net *net, uint32_t wait_ms);

/* Ticks and pumps until every photograph the friend holds is in its
 * photo file, or limit_ms has gone by. Until then a save describes a
 * friend whose newest pictures exist only in this process. Returns 1
 * when they are all written. net as for fr_pump_io: null fails whatever
 * network work comes due meanwhile. */
int32_t fr_pump_photos(Friend_Context *fr, Fr_Net *net, uint32_t limit_ms);

#endif /* FR_PUMP_H */
