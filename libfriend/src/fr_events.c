/* fr_events.c — what the friend tells the frontend, and when it is
 * worth saving.
 *
 * Events wait in a ring until friend_next_event drains them. One that is
 * about a message names it by its event index, not by where it sits: the
 * window shifts and the scrollback drops its oldest between the moment
 * the event is queued and the moment a frontend reads it, so the
 * position a frontend is given is worked out as it is handed over. */

#include "fr_state.h"

static void fr_events_reset(Friend_Context *fr)
{
    fr->event_count      = 0;
    fr->event_first      = 0;
    fr->event_overflowed = 0;
}

static void fr_queue(Friend_Context *fr, Friend_Event event, u64 message)
{
    Fr_Queued_Event *at;
    if (fr->event_count >= fr->event_capacity) {
        /* The caller is not draining. Dropping the newest keeps the
         * sequence it has already seen intact and in order. */
        fr->event_overflowed = 1;
        return;
    }
    at = &fr->event_queue[(fr->event_first + fr->event_count) %
                          fr->event_capacity];
    at->event   = event;
    at->message = message;
    fr->event_count += 1;
}

static void fr_push_event(Friend_Context *fr, Friend_Event event)
{
    fr_queue(fr, event, 0);
}

static void fr_push_event_text(Friend_Context *fr, Friend_Event_Kind kind,
                        String text)
{
    Friend_Event event;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.text = text;
    fr_queue(fr, event, 0);
}

static void fr_push_message_event(Friend_Context *fr, Friend_Event_Kind kind,
                           String text, u64 message)
{
    Friend_Event event;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.text = text;
    fr_queue(fr, event, message + 1);
}

static void fr_push_memories_changed(Friend_Context *fr)
{
    Friend_Event event;
    memset(&event, 0, sizeof(event));
    event.kind  = FRIEND_EVENT_MEMORIES_CHANGED;
    event.count = fr->memory_count;
    fr_queue(fr, event, 0);
}

static void fr_mark_dirty(Friend_Context *fr)
{
    if (fr->unsaved) return;
    fr->unsaved = 1;
    fr_push_event_text(fr, FRIEND_EVENT_STATE_DIRTY, str_nil);
}

int32_t friend_next_event(Friend_Context *fr, Friend_Event *out)
{
    Fr_Queued_Event *next;

    if (!fr || !out) return 0;
    if (fr->event_count <= 0) {
        if (fr->event_overflowed) {
            memset(out, 0, sizeof(*out));
            out->kind  = FRIEND_EVENT_ERROR;
            out->index = -1;
            out->text  = str_lit("event queue overflowed: events were "
                                 "dropped because they were not drained");
            fr->event_overflowed = 0;
            return 1;
        }
        return 0;
    }
    next            = &fr->event_queue[fr->event_first];
    *out            = next->event;
    out->index      = next->message
                          ? fr_message_position(fr, next->message - 1)
                          : -1;
    fr->event_first = (fr->event_first + 1) % fr->event_capacity;
    fr->event_count -= 1;
    return 1;
}
