/* fr_net.h — the transport the friend does not have.
 *
 * friend.h hands out Friend_Io_Request descriptions and takes bytes
 * back. Something has to actually make the calls, and on a desktop that
 * something is sockets and TLS. This is that, supplied so a frontend
 * does not have to write it, and separate from the library so a frontend
 * that would rather use its own can.
 *
 * It is one block of memory, one thread, and no callbacks: submit the
 * requests the friend asked for, poll, and the friend is told what
 * arrived. Nothing here calls back into your code.
 *
 *     Fr_Net *net = fr_net_begin(block, size, FR_NET_CONNECTIONS, 0,
 *                                why, sizeof(why));
 *     for (;;) {
 *         Friend_Io_Request reqs[16];
 *         uint64_t          gone[16];
 *         n = friend_pending_io(fr, reqs, 16);
 *         for (i = 0; i < n; ++i) fr_net_submit(net, fr, &reqs[i]);
 *         n = friend_abandoned_io(fr, gone, 16);
 *         for (i = 0; i < n; ++i) fr_net_abandon(net, gone[i]);
 *         fr_net_poll(net, fr, friend_next_deadline_ms(fr));
 *         friend_tick(fr, now_ms());
 *     }
 *
 * fr_pump.h is that loop with the friend's file work in it as well.
 *
 * TLS is vendored mbedTLS. Certificates are verified against a CA
 * bundle: the path you pass, or the usual places if you pass none. A
 * friend is somebody's private conversations, so a failed verification
 * fails the request rather than warning and continuing.
 *
 * A server that hangs up while it is being written to is a failed
 * request, never a SIGPIPE: nothing that links this has to ignore the
 * signal.
 */

#ifndef FR_NET_H
#define FR_NET_H

#include "friend.h"
#include <stddef.h>

typedef struct Fr_Net Fr_Net;

/* Two more than a reply ever needs at once, and enough for consolidation
 * to make progress while one is in flight. */
#define FR_NET_CONNECTIONS 8

/* How much memory for this many simultaneous connections. */
size_t fr_net_size(int32_t max_connections);

/* Places the transport in your block. ca_bundle_path may be null, in
 * which case the usual Linux and macOS locations are tried in order.
 * Returns null, with the reason in out_error (which may be null), if the
 * block is too small or no CA bundle could be read — the second is a
 * real failure, not a warning: without roots there is nothing to verify
 * against. Nothing is left to free after a failure. */
Fr_Net *fr_net_begin(void *memory, size_t size, int32_t max_connections,
                     const char *ca_bundle_path, char *out_error,
                     size_t error_size);

/* Closes every connection. The block is yours again. */
void fr_net_end(Fr_Net *net);

/* Takes on a request the friend asked for. A request that cannot start
 * yet because every connection is busy waits inside the transport and
 * starts at a later poll; one that cannot be made at all — a URL that
 * makes no sense, a server that cannot be reached, more waiting than
 * there is room for — is failed to the friend with the reason. */
void fr_net_submit(Fr_Net *net, Friend_Context *fr,
                   const Friend_Io_Request *request);

/* Does whatever can be done without blocking, waiting at most wait_ms
 * for something to happen first (0 returns at once); with nothing open,
 * that is a sleep. Progress, completion and failure are reported to the
 * friend as they happen; it is friend_tick that acts on them. */
void fr_net_poll(Fr_Net *net, Friend_Context *fr, uint32_t wait_ms);

/* Stop caring about a request the friend has abandoned. Its answer is
 * read and thrown away, and an abandoned websocket is closed. Either way
 * the friend is still told when it is over, which is what lets it have
 * the request's slot back. */
void fr_net_abandon(Fr_Net *net, uint64_t id);

/* How many connections are busy. */
int32_t fr_net_active(Fr_Net *net);

#endif /* FR_NET_H */
