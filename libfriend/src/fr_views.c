/* fr_views.c — reading the friend.
 *
 * Views, not copies. Every string these hand back points into the block,
 * so a frontend draws straight from the records and nothing is marshalled
 * on the way out. That is the whole reason a message's text is stored
 * contiguously wherever it lives — inline in the window's records, packed
 * in the scrollback's stream — and never in pieces.
 *
 * The conversation is two things: the scrollback the frontend scrolls
 * through, oldest first, then the window the model is shown. They are
 * presented as one list because that is what a conversation is; the split
 * is about which end the friend is thinking about, not about which end the
 * user is reading.
 */

#include "fr_io.h"
#include "fr_recall.h"

static b32 fr_message_at(Friend_Context *fr, i32 index, Fr_Scroll_View *out)
{
    if (index < 0) return 0;
    if (index < fr->scroll.count) return fr_scroll_view(fr, index, out);
    index -= fr->scroll.count;
    if (index >= fr->active_count) return 0;
    fr_event_view(&fr->active_events[index], out);
    return 1;
}

static i32 fr_active_find(Friend_Context *fr, u64 event_index)
{
    i32 i;
    for (i = fr->active_count - 1; i >= 0; --i) {
        if (fr->active_events[i].index == event_index) return i;
    }
    return -1;
}

static i32 fr_message_position(Friend_Context *fr, u64 event_index)
{
    i32 at = fr_active_find(fr, event_index);
    if (at >= 0) return fr->scroll.count + at;
    return fr_scroll_find(fr, event_index);
}

i32 friend_message_count(Friend_Context *fr)
{
    if (!fr) return 0;
    return fr->scroll.count + fr->active_count;
}

Friend_Result friend_message(Friend_Context *fr, i32 index,
                             Friend_Message *out)
{
    Fr_Scroll_View view;

    if (!fr || !out) return FRIEND_ERR_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!fr_message_at(fr, index, &view)) return FRIEND_ERR_BAD_ARGUMENT;
    out->kind             = (Friend_Message_Kind)view.header.kind;
    out->text             = view.text;
    out->unix_ms          = view.header.occurred_unix_ms;
    out->attachment_count = view.header.image_count;
    out->was_spoken       = view.header.was_transcribed;
    out->is_streaming     = view.streaming;
    return FRIEND_OK;
}

Friend_String friend_name(Friend_Context *fr)
{
    if (!fr) return str_nil;
    return fr->friend_name;
}

u64 friend_created_unix_ms(Friend_Context *fr)
{
    return fr ? fr->created_unix_ms : 0;
}

Friend_String friend_last_prompt(Friend_Context *fr)
{
    if (!fr) return str_nil;
    return fr->last_prompt;
}

Friend_String friend_interiority(Friend_Context *fr)
{
    if (!fr) return str_nil;
    return str_make(fr->interiority_buffer, fr->interiority_len);
}

static void fr_count_photos(Friend_Context *fr, const Fr_Image_Ref *images,
                            i32 count, Friend_Stats *stats)
{
    i32 i;
    for (i = 0; i < count; ++i) {
        if (images[i].flags & FR_IMAGE_PENDING) continue;
        stats->photos += 1;
        if (!fr_blob_on_disk(fr, &images[i])) stats->photos_waiting += 1;
    }
}

Friend_Stats friend_stats(Friend_Context *fr)
{
    Friend_Stats stats;
    i32          i;

    memset(&stats, 0, sizeof(stats));
    if (!fr) return stats;

    stats.memories      = fr->memory_count;
    stats.connections   = fr->edge_count;
    stats.active_events = fr->active_count;
    stats.replies_total = fr->replies_total;
    stats.epoch_samples = fr->epoch_samples;
    stats.epoch_length  = fr_epoch_length(fr);
    stats.io_inflight   = fr->io_inflight;
    stats.scrollback    = fr->scroll.count;
    stats.messages_lost = fr->scroll.lost;
    stats.messages_unremembered = fr->scroll.unremembered;
    stats.bytes_total     = fr->block_size;
    stats.bytes_used      = fr->layout.total;
    stats.bytes_committed = fr->bytes_committed;
    stats.scratch_peak    = fr->scratch.peak;
    stats.unsaved         = fr->unsaved ? 1 : 0;
    stats.settled         = fr->turn.stage == FR_STAGE_IDLE &&
                            fr->fold.stage == FR_FOLD_IDLE &&
                            !fr_io_pending(fr, FR_IO_PURPOSE_TRANSCRIBE) &&
                            !fr_io_pending(fr, FR_IO_PURPOSE_CAPTION);

    /* Photographs are counted rather than kept: a photograph is a few
     * bytes of descriptor on a record, and nothing has ever needed the
     * number badly enough to maintain one. */
    for (i = 0; i < fr->memory_slots; ++i) {
        const Fr_Memory_Record *m = &fr->memories[i];
        if (!m->occupied) continue;
        fr_count_photos(fr, m->images, (i32)m->image_count, &stats);
        if (m->use_count == 0) stats.memories_unused += 1;
    }
    for (i = 0; i < friend_message_count(fr); ++i) {
        Fr_Scroll_View view;
        if (!fr_message_at(fr, i, &view)) break;
        fr_count_photos(fr, view.images, (i32)view.header.image_count,
                        &stats);
    }
    return stats;
}

/* ------------------------------------------------------------------ *
 *  The graph
 *
 *  Indices here are over occupied slots in slot order, which is stable
 *  as long as nothing is inserted or pruned. A frontend that wants to
 *  hold on to a memory holds its Friend_Memory_Id, which survives both.
 * ------------------------------------------------------------------ */

static i32 fr_nth_occupied_memory(Friend_Context *fr, i32 index)
{
    i32 slot;
    if (index < 0) return -1;
    for (slot = 0; slot < fr->memory_slots; ++slot) {
        if (!fr->memories[slot].occupied) continue;
        if (index-- == 0) return slot;
    }
    return -1;
}

i32 friend_memory_count(Friend_Context *fr)
{
    return fr ? fr->memory_count : 0;
}

Friend_Result friend_memory_at(Friend_Context *fr, i32 index,
                               Friend_Memory_Node *out)
{
    Fr_Memory_Record *m;
    i32               slot;
    i32               i;

    if (!fr || !out) return FRIEND_ERR_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    slot = fr_nth_occupied_memory(fr, index);
    if (slot < 0) return FRIEND_ERR_BAD_ARGUMENT;
    m = &fr->memories[slot];

    out->id.slot         = (u32)slot;
    out->id.generation   = m->generation;
    out->description     = fr_memory_description(m);
    out->created_unix_ms = m->created_unix_ms;
    out->use_count       = m->use_count;
    for (i = 0; i < fr->turn.recalled_count; ++i) {
        if (fr->turn.recalled[i] == (u32)slot) {
            out->recalled_now = 1;
            break;
        }
    }
    return FRIEND_OK;
}

i32 friend_memory_edge_count(Friend_Context *fr)
{
    return fr ? fr->edge_count : 0;
}

Friend_Result friend_memory_edge_at(Friend_Context *fr, i32 index,
                                    Friend_Memory_Edge *out)
{
    i32 slot;

    if (!fr || !out || index < 0) return FRIEND_ERR_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    for (slot = 0; slot < fr->edge_slots; ++slot) {
        Fr_Edge *e = &fr->edges[slot];
        if (!e->occupied) continue;
        if (index-- > 0) continue;
        out->from.slot       = e->from_slot;
        out->from.generation = fr->memories[e->from_slot].generation;
        out->to.slot         = e->to_slot;
        out->to.generation   = fr->memories[e->to_slot].generation;
        out->relation        = (Friend_Relation)e->relation;
        return FRIEND_OK;
    }
    return FRIEND_ERR_BAD_ARGUMENT;
}
