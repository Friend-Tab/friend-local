/* fr_io.h — requests in flight.
 *
 * Every request the library makes, of every kind, sits in one of a fixed
 * number of slots between being built and being answered. A slot is
 * acquired, has a request posted into it, is handed to the transport by
 * friend_pending_io, and is released by the pump once its completion has
 * been routed.
 */

#ifndef FR_IO_H
#define FR_IO_H

#include "fr_state.h"

static Fr_Io_Slot *fr_io_acquire(Friend_Context *fr, Fr_Io_Purpose purpose,
                          Friend_Io_Kind kind, i32 subject);
static void        fr_io_release(Friend_Context *fr, Fr_Io_Slot *slot);
static b32         fr_io_failed_badly(const Fr_Io_Slot *slot);

/* A request as whoever built it hands it over. Only the fields its kind
 * uses are read. */
typedef struct Fr_Request {
    String method;
    String url;
    String headers; /* CRLF-joined, no trailing CRLF */
    String body;
    String path;    /* file requests */
    u64    offset;
    u32    length;
    u64    channel; /* a websocket's open request, for a send or a close */
    /* Where a file read lands when it is not the slot's own arena: the
     * transport writes straight into the page the reader looks at. */
    u8    *recv;
    u32    recv_capacity;
    /* The answer is wanted as it arrives rather than when it is whole.
     * Whoever builds the request knows, because it is a property of the
     * wire shape: a streamed reply, raw speech samples. */
    b32 stream;
    /* The arena ran out while this was being built, so the body is a
     * truncated prefix. Never send it: a half-written JSON object is
     * worse than no request. */
    b32 failed;
} Fr_Request;

/* Puts a built request in the slot. Every string it points at that is
 * not already in the slot's arena is copied there — a path or a URL in
 * the configuration is gone the moment the friend is reconfigured, and
 * the request may not be handed out until after that. The one exception
 * is a file write's body, which is the photo staging arena itself: that
 * only ever bumps, so it stays where it is while the write is out, and a
 * copy would cost a second buffer the size of the photographs.
 *
 * The kinds that are answered (HTTPS, and the websocket open whose
 * buffer every message lands in) are given the rest of the slot's arena
 * for the answer. Returns 0 when the request did not fit, with the
 * reason in friend_last_error, and the slot released. */
static b32 fr_io_post(Friend_Context *fr, Fr_Io_Slot *slot, Fr_Request req);


/* What came back, as bytes. */
static String fr_io_answer(const Fr_Io_Slot *slot);


/* The friend no longer wants any answer of this purpose. A request still
 * waiting to be handed out is dropped; one the transport has is marked,
 * and reported once through friend_abandoned_io. */
static void fr_io_abandon(Friend_Context *fr, Fr_Io_Purpose purpose);

/* Every request, for a friend that is being replaced: abandoned the same
 * way, and given no purpose, so whatever still comes back for one is
 * routed nowhere. */
static void fr_io_abandon_all(Friend_Context *fr);

/* Is a request of this purpose about this subject already out? */
static b32 fr_io_in_flight(Friend_Context *fr, Fr_Io_Purpose purpose, i32 subject);

/* How often to come back while something is moving that the transport
 * will not announce: a stream half read, a speaker still playing. */
#define FR_IO_POLL_MS 20u

/* How long the requests can be left: 0 when one is waiting to be handed
 * out, routed or read, FR_IO_POLL_MS while any are out, and UINT32_MAX
 * when none are. */
static u32 fr_io_wait_ms(Friend_Context *fr);

/* Is any request of this purpose out whose answer is still wanted? */
static b32 fr_io_pending(Friend_Context *fr, Fr_Io_Purpose purpose);

#endif /* FR_IO_H */
