/* fr_io.c — the slots, and the half of the pump the transport calls.
 *
 * The library opens nothing itself. It builds requests into slots and
 * the frontend's transport asks for them, does them, and reports back;
 * everything here is the bookkeeping either side of that. What a
 * completion means is decided by whoever asked for it, through the
 * router in fr_turn.c.
 */

#include "fr_io.h"

/* The least room an answer can be given. Below this the request is not
 * worth sending: even an error body would not fit. */
#define FR_IO_RECV_MIN FR_KB(8)

static Fr_Io_Slot *fr_io_acquire(Friend_Context *fr, Fr_Io_Purpose purpose,
                          Friend_Io_Kind kind, i32 subject)
{
    i32 i;

    for (i = 0; i < fr->io_capacity; ++i) {
        Fr_Io_Slot *slot = &fr->io_slots[i];
        Arena       arena;

        if (slot->state != FR_IO_STATE_FREE) continue;

        /* The slot's slice of the io arena survives being emptied: it is
         * where the slot lives, not something it was lent. */
        arena = slot->arena;
        arena_reset(&arena);
        memset(slot, 0, sizeof(*slot));
        slot->arena    = arena;
        slot->req.id   = fr->next_io_id++;
        slot->req.kind = kind;
        slot->state    = FR_IO_STATE_READY;
        slot->purpose  = purpose;
        slot->subject  = subject;
        fr->io_inflight += 1;
        return slot;
    }
    return 0;
}

static void fr_io_release(Friend_Context *fr, Fr_Io_Slot *slot)
{
    Arena arena = slot->arena;
    arena_reset(&arena);
    memset(slot, 0, sizeof(*slot));
    slot->arena = arena;
    slot->state = FR_IO_STATE_FREE;
    if (fr->io_inflight > 0) fr->io_inflight -= 1;
}

static b32 fr_io_failed_badly(const Fr_Io_Slot *slot)
{
    if (slot->failed) return 1;
    /* Only an HTTP request has a verdict in a number. A file read or a
     * websocket reports nothing — zero, which the pump's contract asks
     * for — and saying so is not the same as saying it went wrong. */
    if (slot->req.kind != FRIEND_IO_HTTPS) return 0;
    return slot->status < 200 || slot->status >= 300;
}

static Fr_Io_Slot *fr_io_find(Friend_Context *fr, u64 id)
{
    i32 i;
    for (i = 0; i < fr->io_capacity; ++i) {
        if (fr->io_slots[i].state != FR_IO_STATE_FREE &&
            fr->io_slots[i].req.id == id) {
            return &fr->io_slots[i];
        }
    }
    return 0;
}

/* Whatever is left of the slot's arena after the request was built is
 * where the answer goes. A streaming reply gets the lot, which is what
 * lets it be parsed in place rather than copied. All of it but the last
 * few hundred bytes, which are kept for the reason this request failed:
 * friend_io_failed copies that into the slot, and a slot with no room
 * left for it loses the one sentence that would have explained
 * anything. */
static b32 fr_io_give_recv_rest(Fr_Io_Slot *slot)
{
    size_t room = arena_remaining(&slot->arena);
    if (room < FR_IO_RECV_MIN + FR_ERROR_MAX) return 0;
    room -= FR_ERROR_MAX;
    slot->req.recv = (u8 *)arena_push_size_nz(&slot->arena, room, 1);
    if (!slot->req.recv) return 0;
    slot->req.recv_capacity = (u32)room;
    return 1;
}

/* The string, where the slot owns it. */
static b32 fr_io_keep(Fr_Io_Slot *slot, String s, String *out)
{
    const u8 *at = (const u8 *)s.data;
    if (s.len <= 0) {
        *out = str_nil;
        return 1;
    }
    if (at >= slot->arena.base && at < slot->arena.base + slot->arena.used) {
        *out = s;
        return 1;
    }
    *out = str_push_copy(&slot->arena, s);
    return out->len == s.len;
}

static b32 fr_io_post(Friend_Context *fr, Fr_Io_Slot *slot, Fr_Request req)
{
    Friend_Io_Request *out = &slot->req;
    b32                ok  = !req.failed;

    ok = ok && fr_io_keep(slot, req.method, &out->method);
    ok = ok && fr_io_keep(slot, req.url, &out->url);
    ok = ok && fr_io_keep(slot, req.headers, &out->headers);
    ok = ok && fr_io_keep(slot, req.path, &out->path);
    if (out->kind == FRIEND_IO_FILE_WRITE) {
        out->body = req.body;
    } else {
        ok = ok && fr_io_keep(slot, req.body, &out->body);
    }
    if (!ok) {
        fr_io_release(fr, slot);
        fr_error(fr, FRIEND_ERR_FULL,
                 str_lit("a request did not fit in its share of the I/O "
                         "budget: raise limits.io_bytes"));
        return 0;
    }
    out->offset  = req.offset;
    out->length  = req.length;
    out->channel = req.channel;
    out->flags   = req.stream ? FRIEND_IO_FLAG_STREAM : 0;
    if (req.recv) {
        out->recv          = req.recv;
        out->recv_capacity = req.recv_capacity;
        return 1;
    }
    if (out->kind != FRIEND_IO_HTTPS && out->kind != FRIEND_IO_WS_OPEN) {
        return 1;
    }
    if (!fr_io_give_recv_rest(slot)) {
        fr_io_release(fr, slot);
        fr_error(fr, FRIEND_ERR_FULL,
                 str_lit("no room left in the request's slot for the "
                         "answer: raise limits.io_bytes"));
        return 0;
    }
    return 1;
}

static String fr_io_answer(const Fr_Io_Slot *slot)
{
    return str_make((const char *)slot->req.recv, (i32)slot->recv_len);
}

static void fr_io_give_up(Friend_Context *fr, Fr_Io_Slot *slot)
{
    if (slot->state == FR_IO_STATE_READY) {
        fr_io_release(fr, slot);
    } else if (slot->state == FR_IO_STATE_OUTSTANDING) {
        slot->abandoned = 1;
    }
}

static void fr_io_abandon(Friend_Context *fr, Fr_Io_Purpose purpose)
{
    i32 i;
    for (i = 0; i < fr->io_capacity; ++i) {
        if (fr->io_slots[i].purpose == purpose) {
            fr_io_give_up(fr, &fr->io_slots[i]);
        }
    }
}

static void fr_io_abandon_all(Friend_Context *fr)
{
    i32 i;
    for (i = 0; i < fr->io_capacity; ++i) {
        Fr_Io_Slot *slot = &fr->io_slots[i];
        if (slot->state == FR_IO_STATE_FREE) continue;
        fr_io_give_up(fr, slot);
        slot->purpose = FR_IO_PURPOSE_NONE;
    }
}

static b32 fr_io_in_flight(Friend_Context *fr, Fr_Io_Purpose purpose, i32 subject)
{
    i32 i;
    for (i = 0; i < fr->io_capacity; ++i) {
        const Fr_Io_Slot *slot = &fr->io_slots[i];
        if (slot->state == FR_IO_STATE_FREE) continue;
        if (slot->purpose == purpose && slot->subject == subject) return 1;
    }
    return 0;
}

static b32 fr_io_pending(Friend_Context *fr, Fr_Io_Purpose purpose)
{
    i32 i;
    for (i = 0; i < fr->io_capacity; ++i) {
        const Fr_Io_Slot *slot = &fr->io_slots[i];
        if (slot->state == FR_IO_STATE_FREE || slot->abandoned) continue;
        if (slot->purpose == purpose) return 1;
    }
    return 0;
}

/* ================================================================== *
 *  The transport's side
 * ================================================================== */

i32 friend_pending_io(Friend_Context *fr, Friend_Io_Request *out,
                      i32 max_requests)
{
    i32 written = 0;
    i32 i;

    if (!fr || !out || max_requests <= 0) return 0;
    for (i = 0; i < fr->io_capacity && written < max_requests; ++i) {
        Fr_Io_Slot *slot = &fr->io_slots[i];

        if (slot->state != FR_IO_STATE_READY) continue;

        out[written++] = slot->req;
        slot->state    = FR_IO_STATE_OUTSTANDING;
    }
    return written;
}

/* An id the friend handed out once and has since let go of — reported
 * abandoned, or given up on with the friend it belonged to — is not a
 * mistake to finish; one it never handed out is. */
static Friend_Result fr_io_unknown(Friend_Context *fr, u64 id)
{
    return id != 0 && id < fr->next_io_id ? FRIEND_OK
                                          : FRIEND_ERR_BAD_ARGUMENT;
}

Friend_Result friend_io_progress(Friend_Context *fr, u64 id, u32 appended)
{
    Fr_Io_Slot *slot;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    slot = fr_io_find(fr, id);
    if (!slot || slot->state != FR_IO_STATE_OUTSTANDING) {
        return FRIEND_ERR_BAD_ARGUMENT;
    }
    if (appended > slot->req.recv_capacity - slot->recv_len) {
        return fr_error(fr, FRIEND_ERR_BAD_ARGUMENT,
                        str_lit("more bytes were appended to a response "
                                "than the buffer had room for"));
    }
    /* The buffer is append-only for the life of a request: a transport
     * is told where to write once, by being handed the pointer, and
     * moving bytes around underneath it would be a lie. A streamed reply
     * is parsed in place and the whole stream has to fit, which is what
     * the slot's share of the I/O budget is sized for. */
    slot->recv_len += appended;
    return FRIEND_OK;
}

Friend_Result friend_io_complete(Friend_Context *fr, u64 id, i32 status)
{
    Fr_Io_Slot *slot;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    slot = fr_io_find(fr, id);
    if (!slot) return fr_io_unknown(fr, id);
    slot->status = status;
    slot->state  = FR_IO_STATE_DONE;
    return FRIEND_OK;
}

Friend_Result friend_io_failed(Friend_Context *fr, u64 id, Friend_String why)
{
    Fr_Io_Slot *slot;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    slot = fr_io_find(fr, id);
    if (!slot) return fr_io_unknown(fr, id);
    slot->failed = 1;
    slot->state  = FR_IO_STATE_DONE;
    /* Into the slot, which outlives the tick that reads it. */
    slot->error = str_push_copy(&slot->arena, why);
    return FRIEND_OK;
}

i32 friend_abandoned_io(Friend_Context *fr, u64 *out, i32 max_ids)
{
    i32 written = 0;
    i32 i;

    if (!fr || !out || max_ids <= 0) return 0;
    for (i = 0; i < fr->io_capacity && written < max_ids; ++i) {
        Fr_Io_Slot *slot = &fr->io_slots[i];
        if (slot->state == FR_IO_STATE_FREE) continue;
        if (!slot->abandoned) continue;
        /* Told, the transport owes nothing more: the slot is the
         * friend's again, and whatever is said about this id later is
         * ignored. */
        out[written++] = slot->req.id;

        fr_io_release(fr, slot);
    }
    return written;
}

static u32 fr_io_wait_ms(Friend_Context *fr)
{
    i32 i;
    b32 outstanding = 0;

    for (i = 0; i < fr->io_capacity; ++i) {
        Fr_Io_Slot *slot = &fr->io_slots[i];
        switch (slot->state) {
        case FR_IO_STATE_FREE:
            break;
        case FR_IO_STATE_READY:
        case FR_IO_STATE_DONE:
            return 0; /* there is work to do right now */
        case FR_IO_STATE_OUTSTANDING:
            if (slot->recv_len > slot->consumed) return 0;
            outstanding = 1;
            break;
        }
    }
    /* Nothing to do but wait on the network, which the frontend is
     * better placed to do than a timer. */
    return outstanding ? FR_IO_POLL_MS : 0xFFFFFFFFu;
}
