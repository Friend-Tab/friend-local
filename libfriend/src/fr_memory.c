/* fr_memory.c — writing the graph.
 *
 * See fr_memory.h for the shape of the pipeline, which is the service's.
 * The graph operations it calls are in fr_graph.c.
 */

#include "fr_memory.h"
#include "fr_prompt.h"
#include "fr_provider.h"
#include "fr_rank.h"
#include "fr_recall.h"

/* The service's numbers, from ExtractEventsFromMessages and the
 * interiority pass. */
#define FR_EXTRACT_MAX_TOKENS 3072
#define FR_EXTRACT_THINKING 10240
#define FR_RELATE_MAX_TOKENS 10240
#define FR_RELATE_THINKING 0
#define FR_INTERIORITY_MAX_TOKENS 2048
#define FR_INTERIORITY_THINKING 4096
#define FR_INTERIORITY_MEMORIES 40
#define FR_INTERIORITY_EVENTS 60
#define FR_INTERIORITY_STATEMENTS 8

/* ================================================================== *
 *  Memories, formed and forgotten
 * ================================================================== */

/* A memory in a slot but not yet in the graph: described, and waiting
 * for its dialogue and a vector. FR_SLOT_NONE when there is no room. */
static u32 fr_memory_stage(Friend_Context *fr, String description,
                           u64 created_unix_ms)
{
    u32               slot = fr_memory_alloc(fr);
    Fr_Memory_Record *m;

    if (slot == FR_SLOT_NONE) return FR_SLOT_NONE;
    m = &fr->memories[slot];
    fr_set_description(m, str_trim(description));
    m->created_unix_ms    = created_unix_ms;
    m->dialog_end_unix_ms = 0;
    m->staged          = 1;
    m->needs_embedding = 1;
    /* Memories formed mid-epoch sit out the rest of it, so nothing is
     * pruned for being unused during a stretch it did not exist for. */
    m->in_this_epoch = 0;
    return slot;
}

Friend_Result friend_forget(Friend_Context *fr, Friend_Memory_Id id)
{
    Fr_Memory_Record *m;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    m = fr_memory(fr, id.slot);
    if (!m) {
        return fr_error(fr, FRIEND_ERR_BAD_ARGUMENT,
                        str_lit("there is no memory in that slot"));
    }
    if (m->generation != id.generation) {
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("that memory is gone: the slot has been "
                                "reused since you were given its id"));
    }
    fr_drop_memory(fr, id.slot);
    fr_push_memories_changed(fr);
    return FRIEND_OK;
}

static i32 fr_prune_unused(Friend_Context *fr)
{
    i32 removed = 0;
    i32 slot;

    if (!fr || !fr->epoch_active) return 0;
    if (fr->memory_count < fr->tuning.prune_min_memories) return 0;

    if (fr->epoch_samples < fr_epoch_length(fr)) return 0;

    for (slot = 0; slot < fr->memory_slots; ++slot) {
        Fr_Memory_Record *m = &fr->memories[slot];
        i32               in_count = 0, out_count = 0;
        u32               edge;

        if (!fr_memory_comparable(m)) continue;
        if (!m->in_this_epoch || m->uses_this_epoch != 0) continue;

        for (edge = m->first_in; edge != FR_SLOT_NONE;
             edge = fr->edges[edge].next_to) {
            ++in_count;
        }
        for (edge = m->first_out; edge != FR_SLOT_NONE;
             edge = fr->edges[edge].next_from) {
            ++out_count;
        }
        if (in_count * out_count > FR_PRUNE_MAX_FANOUT) continue; /* a junction */

        fr_drop_memory(fr, (u32)slot);
        ++removed;
    }

    fr_epoch_open(fr);

    if (removed > 0) fr_push_memories_changed(fr);
    return removed;
}

/* ================================================================== *
 *  The stretch being folded
 *
 *  Consolidation takes its own copy of the messages it is about, at the
 *  moment it starts, and works from that for the whole pass. Nothing
 *  downstream has to care about a message arriving mid-pass, and — the
 *  reason it is a copy rather than a span of the conversation — nothing
 *  that happens to the scrollback can take the stretch away from the
 *  fold halfway through. This is the path a message's content takes into
 *  memory; it does not get to depend on how much scrollback the user
 *  configured.
 * ================================================================== */

static const Fr_Chat_Event *fr_fold_event(Friend_Context *fr, i32 i)
{
    if (i < 0 || i >= fr->fold.event_count) return 0;
    return &fr->fold_events[i];
}

/* The same stretch as plain dialogue, which is what the recall query and
 * the interiority pass are shown. */
static String fr_fold_dialog(Friend_Context *fr, Arena *arena, b32 relative,
                             i32 utc_offset, i32 last_n)
{
    String_Builder sb = sb_begin(arena);
    i32            first = 0;

    if (last_n > 0 && fr->fold.event_count > last_n) {
        first = fr->fold.event_count - last_n;
    }
    fr_append_chat_dialog(&sb, fr, fr->fold_events + first,
                          fr->fold.event_count - first, relative, utc_offset);
    return sb_done(&sb);
}

/* A few-shot conversation with the real turn on the end of it. */
static Fr_Prompt_Message *fr_with_shots(Arena *arena, const Fr_Shot *shots,
                                        i32 shot_count, String turn)
{
    Fr_Prompt_Message *messages =
        arena_push(arena, Fr_Prompt_Message, (size_t)shot_count + 1);
    i32 i;

    if (!messages) return 0;
    for (i = 0; i <= shot_count; ++i) {
        messages[i].is_user     = i < shot_count ? shots[i].is_user : 1;
        messages[i].text        = i < shot_count ? shots[i].text : turn;
        messages[i].images      = 0;
        messages[i].image_count = 0;
    }
    return messages;
}

/* ================================================================== *
 *  Starting
 * ================================================================== */

/* The stretch is accounted for: every message in it has had whatever
 * memory it was going to get. Only ever forward — a later fold can be
 * about an older stretch than the last one, and the watermark is the
 * point the scrollback is allowed to drop things before. */
static void fr_fold_accounted(Friend_Context *fr)
{
    u64 through = fr->fold.first_event_index + (u64)fr->fold.event_count;
    if (through <= fr->folded_through) return;
    fr->folded_through = through;
    fr_mark_dirty(fr);
}

static void fr_fold_reset(Friend_Context *fr)
{
    memset(&fr->fold, 0, sizeof(fr->fold));
}

static void fr_memory_reset(Friend_Context *fr)
{
    fr_fold_reset(fr);
    fr_retry_worked(&fr->backfill_retry);
    fr->folded_through     = 0;
    fr->fold_attempt_index = 0;
    fr->fold_attempts      = 0;
    fr->interiority_len    = 0;
    fr->replies_total      = 0;
    fr->epoch_samples      = 0;
    fr->epoch_active       = 0;
}

/* Three goes at one stretch. Fewer and a provider blip costs the friend
 * a conversation; more and a provider that is simply refusing this
 * stretch would be asked about it after every reply forever. */
#define FR_FOLD_ATTEMPTS_MAX 3

static void fr_fold_abandon(Friend_Context *fr, String why)
{
    /* The events are already out of the window and the staged memories
     * have descriptions but no place in the graph. Dropping them is the
     * honest outcome: a half-formed memory is worse than none, and the
     * conversation they came from is still in the scrollback, so the
     * next consolidation picks the same stretch up again. */
    i32 i;
    if (fr->fold_attempts >= FR_FOLD_ATTEMPTS_MAX) fr_fold_accounted(fr);
    for (i = 0; i < fr->fold.staged_count; ++i) {
        if (fr_memory(fr, fr->fold.staged[i])) {
            fr_memory_free(fr, fr->fold.staged[i]);
        }
    }
    fr_fold_reset(fr);
    if (!str_is_empty(why)) {
        fr_error(fr, FRIEND_ERR_PROVIDER, why);
        fr_push_event_text(fr, FRIEND_EVENT_ERROR, friend_last_error(fr));
    }
}

/* The whole stretch, trimmed to what the model will read. The service
 * measured the embedding's share in runes against an 8192-token limit;
 * the tail is the part that matters, so the tail is what is kept. */
#define FR_FOLD_QUERY_BYTES 28000
#define FR_FOLD_RERANK_BYTES 8000

static String fr_fold_query(Friend_Context *fr, Arena *arena, i32 q,
                            b32 for_rerank)
{
    String dialog = fr_fold_dialog(fr, arena, 0, 0, 0);
    i32    keep   = for_rerank ? FR_FOLD_RERANK_BYTES : FR_FOLD_QUERY_BYTES;
    fr_unused(q);
    if (dialog.len > keep) dialog = str_tail(dialog, keep);
    return dialog;
}

static const Fr_Recall_Spec fr_fold_recall = {
    FR_IO_PURPOSE_FOLD_EMBED_QUERY, FR_IO_PURPOSE_FOLD_RERANK, 1,
    fr_fold_query
};

/* Everything the friend has not formed a memory from yet, oldest first,
 * copied into the fold's own room: first what is waiting in the
 * scrollback, then what is still in the window. Returns how many.
 *
 * The scrollback end is normally empty. It fills when the window had to
 * retire messages while a fold was already running, and those messages
 * would otherwise be the ones nothing is ever remembered from. */
static i32 fr_fold_collect(Friend_Context *fr, i32 want)
{
    i32 waiting = fr_unfolded_in_scrollback(fr);
    i32 taken   = 0;
    i32 i;

    if (want > FR_FOLD_EVENTS_MAX) want = FR_FOLD_EVENTS_MAX;

    for (i = fr->scroll.count - waiting; i < fr->scroll.count && taken < want;
         ++i) {
        Fr_Scroll_View view;
        if (!fr_scroll_view(fr, i, &view)) break;
        fr_scroll_to_event(&view, &fr->fold_events[taken]);
        ++taken;
    }
    for (i = 0; i < fr->active_count && taken < want; ++i) {
        fr->fold_events[taken] = fr->active_events[i];
        ++taken;
        /* It stays in the window too, until the retire below: the model
         * should go on seeing what it was just talking about. */
    }

    /* The ones that came out of the window leave it now, so that a
     * message arriving mid-pass cannot shift the stretch. */
    if (taken > waiting) fr_retire_active(fr, taken - waiting);
    return taken;
}

static void fr_maybe_consolidate(Friend_Context *fr)
{
    i32 fold_count;
    i32 waiting;

    if (!fr || fr->fold.stage != FR_FOLD_IDLE) return;

    waiting = fr_unfolded_in_scrollback(fr);
    if (fr->active_count <= fr->tuning.consolidate_at && waiting == 0) return;

    /* The service's arithmetic, with whatever is already waiting folded
     * in front of it: fold everything above what the window keeps, up to
     * the per-pass cap. */
    fold_count = fr->active_count - fr->tuning.consolidate_leaving;
    if (fold_count < 0) fold_count = 0;
    fold_count += waiting;
    if (fold_count > fr->tuning.consolidate_max) {
        fold_count = fr->tuning.consolidate_max;
    }
    /* Two messages are not a stretch of conversation. */
    if (fold_count <= 2) return;

    fr_fold_reset(fr);
    fold_count = fr_fold_collect(fr, fold_count);
    if (fold_count <= 2) {
        fr_fold_reset(fr);
        return;
    }
    fr->fold.event_count       = fold_count;
    fr->fold.first_event_index = fr->fold_events[0].index;

    /* Attempts are counted against the stretch, not against the friend:
     * a provider that is down gets the same stretch again after the next
     * reply, and a stretch that fails three times is written off rather
     * than retried for the rest of the conversation. */
    if (fr->fold_attempt_index != fr->fold.first_event_index) {
        fr->fold_attempt_index = fr->fold.first_event_index;
        fr->fold_attempts      = 0;
    }
    fr->fold_attempts += 1;

    fr->fold.stage = FR_FOLD_RECALLING;
    if (!fr_recall_start(fr, &fr->fold.recall, &fr_fold_recall)) {
        /* Nothing was sent, so nothing has to be unwound. It will be
         * tried again after the next reply. */
        fr_fold_reset(fr);
    }
}

/* ================================================================== *
 *  Naming the events
 * ================================================================== */

static void fr_fold_start_extract(Friend_Context *fr)
{
    Arena_Mark         mark = arena_mark(&fr->scratch);
    String             described, turn;
    Fr_Io_Slot        *slot;
    Fr_Model_Call      call;
    Fr_Prompt_Message *messages;
    const Fr_Shot     *shots;
    i32                shot_count = fr_extract_shots(&shots);

    described = fr_render_recall(fr, &fr->scratch, &fr->fold.recall, 0, 0, 0)
                    .described;

    slot = fr_io_acquire(fr, FR_IO_PURPOSE_EXTRACT_EVENTS, FRIEND_IO_HTTPS, 0);
    if (!slot) {
        arena_reset_to(&fr->scratch, mark);
        fr_fold_abandon(fr, str_lit("no room to ask what happened"));
        return;
    }

    turn = fr_build_extract_turn(
        &slot->arena,
        fr_build_extract_conversation(fr, &slot->arena, fr->fold_events,
                                      fr->fold.event_count),
        described);
    messages = fr_with_shots(&slot->arena, shots, shot_count, turn);
    if (!messages) {
        fr_io_release(fr, slot);
        arena_reset_to(&fr->scratch, mark);
        fr_fold_abandon(fr, str_lit("the extraction prompt did not fit"));
        return;
    }

    memset(&call, 0, sizeof(call));
    call.model             = fr->models.extract;
    call.system            = fr_build_extract_system(fr, &slot->arena);
    call.messages          = messages;
    call.message_count     = shot_count + 1;
    call.max_output_tokens = FR_EXTRACT_MAX_TOKENS;
    call.thinking_budget   = FR_EXTRACT_THINKING;
    call.temperature       = -1.0f;

    arena_reset_to(&fr->scratch, mark);
    if (!fr_io_post(fr, slot, fr_provider_chat(fr, &slot->arena, &call))) {
        fr_fold_abandon(fr, friend_last_error(fr));
        return;
    }
    fr->fold.stage   = FR_FOLD_EXTRACTING;
    fr->fold.pending = 1;
}

/* "Event [3-9]: my User did such and such". Anything else on the line is
 * the model not following instructions, and is skipped rather than
 * failing the pass: one unparseable line should not cost the others. */
static b32 fr_parse_event_line(String line, i32 *out_from, i32 *out_to,
                               String *out_description)
{
    i32 at = 0;
    i64 from = 0, to = 0;
    i32 digits;

    line = str_trim(line);
    if (!str_has_prefix(line, str_lit("Event ["))) return 0;
    at = 7;

    digits = 0;
    while (at < line.len && line.data[at] >= '0' && line.data[at] <= '9') {
        from = from * 10 + (line.data[at++] - '0');
        ++digits;
    }
    if (!digits || at >= line.len || line.data[at] != '-') return 0;
    ++at;

    digits = 0;
    while (at < line.len && line.data[at] >= '0' && line.data[at] <= '9') {
        to = to * 10 + (line.data[at++] - '0');
        ++digits;
    }
    if (!digits || at + 2 > line.len) return 0;
    if (line.data[at] != ']' || line.data[at + 1] != ':') return 0;
    at += 2;

    *out_from        = (i32)from;
    *out_to          = (i32)to;
    *out_description = str_trim(str_skip(line, at));
    return !str_is_empty(*out_description);
}

/* What the model named, staged: in a slot, with the dialogue it came
 * from, but not yet in the graph. */
static void fr_fold_finish_extract(Friend_Context *fr, Fr_Io_Slot *slot)
{
    String rest;
    String line;

    rest = fr_provider_text(fr, &fr->scratch, fr_io_answer(slot));
    while (str_next_line(&rest, &line)) {
        i32               from = 0, to = 0;
        String            description;
        u32               staged;
        Fr_Memory_Record *m;
        i32               i;

        if (fr->fold.staged_count >= FR_STAGED_MAX) break;
        if (!fr_parse_event_line(line, &from, &to, &description)) continue;

        /* The model answers with one-based line numbers into the
         * conversation it was shown. A span that runs off the end is
         * clamped rather than dropped: the description is still true. */
        if (from < 1) from = 1;
        if (to > fr->fold.event_count) to = fr->fold.event_count;
        if (to < from) to = from;

        staged = fr_memory_stage(fr, description, fr->now_unix_ms);
        if (staged == FR_SLOT_NONE) {
            fr_error(fr, FRIEND_ERR_FULL,
                     str_lit("no room for another memory: the graph is at "
                             "max_memories"));
            break;
        }
        m = &fr->memories[staged];

        /* The dialogue it came from, which is what the relation pass is
         * shown when deciding how two memories connect. */
        for (i = from - 1; i < to; ++i) {
            const Fr_Chat_Event *e = fr_fold_event(fr, i);
            if (!e) break;
            if (!fr_event_for_model(e)) continue;
            fr_dialog_append(m, e->occurred_unix_ms, fr_event_turn_kind(e),
                             fr_chat_text(e));
            m->dialog_end_unix_ms = e->occurred_unix_ms;
        }
        if (m->dialog_end_unix_ms == 0) m->dialog_end_unix_ms = fr->now_unix_ms;
        fr->fold.staged[fr->fold.staged_count++] = staged;
    }
}

/* ================================================================== *
 *  Embedding, then joining
 * ================================================================== */

static b32 fr_fold_start_embeddings(Friend_Context *fr)
{
    i32 i;

    fr->fold.stage   = FR_FOLD_EMBEDDING;
    fr->fold.pending = 0;
    for (i = 0; i < fr->fold.staged_count; ++i) {
        Fr_Memory_Record *m = fr_memory(fr, fr->fold.staged[i]);
        if (m && fr_embed_memory(fr, FR_IO_PURPOSE_FOLD_EMBED, i, m)) {
            fr->fold.pending += 1;
        }
    }
    return fr->fold.pending > 0;
}

static void fr_fold_finish_embedding(Friend_Context *fr, Fr_Io_Slot *slot)
{
    Fr_Memory_Record *m;

    if (slot->subject < 0 || slot->subject >= fr->fold.staged_count) return;
    m = fr_memory(fr, fr->fold.staged[slot->subject]);
    if (m && fr_read_embedding(fr, slot, m->embedding)) {
        m->needs_embedding = 0;
    }
}

static void fr_fold_start_interiority(Friend_Context *fr);

/* "Explanation: ...\nRelation: Cause" */
static u8 fr_parse_relation(String answer)
{
    String rest = answer;
    String line;
    String found = str_nil;

    while (str_next_line(&rest, &line)) {
        line = str_trim(line);
        if (!str_has_prefix(line, str_lit("Relation:"))) continue;
        found = str_trim(str_skip(line, 9));
    }
    if (str_is_empty(found)) return FRIEND_RELATION_NONE;
    if (str_eq(found, str_lit("Changed"))) return FRIEND_RELATION_CHANGED;
    if (str_eq(found, str_lit("Cause"))) return FRIEND_RELATION_CAUSE;
    if (str_eq(found, str_lit("Reason"))) return FRIEND_RELATION_REASON;
    if (str_eq(found, str_lit("HinderedBy"))) return FRIEND_RELATION_HINDERED_BY;
    if (str_eq(found, str_lit("React"))) return FRIEND_RELATION_REACT;
    if (str_eq(found, str_lit("Want"))) return FRIEND_RELATION_WANT;
    if (str_eq(found, str_lit("SameTopic"))) return FRIEND_RELATION_SAME_TOPIC;
    return FRIEND_RELATION_NONE;
}

/* One neighbour, sent off to be classified against the new memory. */
static b32 fr_fold_ask_relation(Friend_Context *fr, Fr_Memory_Record *m,
                                Fr_Memory_Record *other, i32 which)
{
    Fr_Io_Slot        *io;
    Fr_Model_Call      call;
    Fr_Prompt_Message *messages;
    const Fr_Shot     *shots;
    i32                shot_count = fr_relate_shots(&shots);
    Arena_Mark         mark;
    String             turn;

    io = fr_io_acquire(fr, FR_IO_PURPOSE_RELATE, FRIEND_IO_HTTPS, which);
    if (!io) return 0;

    mark = arena_mark(&fr->scratch);
    turn = fr_build_relate_turn(
        &io->arena, fr_memory_description(other),
        fr_render_dialog(&fr->scratch, other, fr->now_unix_ms, 0),
        fr_memory_description(m),
        fr_render_dialog(&fr->scratch, m, fr->now_unix_ms, 0));
    arena_reset_to(&fr->scratch, mark);
    messages = fr_with_shots(&io->arena, shots, shot_count, turn);
    if (!messages) {
        fr_io_release(fr, io);
        return 0;
    }

    memset(&call, 0, sizeof(call));
    call.model             = fr->models.relate;
    call.system            = fr_relate_system();
    call.messages          = messages;
    call.message_count     = shot_count + 1;
    call.max_output_tokens = FR_RELATE_MAX_TOKENS;
    call.thinking_budget   = FR_RELATE_THINKING;
    call.temperature       = -1.0f;
    return fr_io_post(fr, io, fr_provider_chat(fr, &io->arena, &call));
}

/* The next staged memory's three nearest neighbours, each sent off to be
 * classified. When there are none — the first memory a friend ever forms
 * — it simply joins. When every staged memory has joined, the friend
 * rewrites what it wants. */
static void fr_fold_advance_relating(Friend_Context *fr)
{
    u32 nearest[FR_RELATE_FANOUT];
    f32 scores[FR_RELATE_FANOUT];
    i32 i;

    while (fr->fold.relating_at < fr->fold.staged_count) {
        Fr_Memory_Record *m = fr_memory(fr, fr->fold.staged[fr->fold.relating_at]);
        i32               found;

        if (!m) {
            ++fr->fold.relating_at;
            continue;
        }

        found = fr_nearest_memories(fr, m->embedding, FR_RELATE_FANOUT, nearest,
                                    scores);
        fr->fold.relate_count = found;
        fr->fold.pending      = 0;
        memset(fr->fold.relate_as, 0, sizeof(fr->fold.relate_as));
        for (i = 0; i < found; ++i) {
            Fr_Memory_Record *other = fr_memory(fr, nearest[i]);
            fr->fold.relate_to[i] = nearest[i];
            if (other && fr_fold_ask_relation(fr, m, other, i)) {
                fr->fold.pending += 1;
            }
        }
        if (fr->fold.pending > 0) return; /* wait for the answers */

        /* Nothing to relate to, or nothing could be sent: it joins the
         * graph unconnected rather than being lost. */
        m->staged = 0;
        ++fr->fold.relating_at;
    }

    fr_fold_start_interiority(fr);
}

/* Drops anything that never got a vector, then joins what is left. */
static void fr_fold_begin_relating(Friend_Context *fr)
{
    i32 kept = 0;
    i32 i;

    for (i = 0; i < fr->fold.staged_count; ++i) {
        u32               slot = fr->fold.staged[i];
        Fr_Memory_Record *m    = fr_memory(fr, slot);
        if (!m) continue;
        if (m->needs_embedding) {
            fr_memory_free(fr, slot);
            continue;
        }
        fr->fold.staged[kept++] = slot;
    }
    fr->fold.staged_count = kept;
    fr->fold.relating_at  = 0;
    fr->fold.stage        = FR_FOLD_RELATING;
    fr_fold_advance_relating(fr);
}

static void fr_fold_finish_relate(Friend_Context *fr, Fr_Io_Slot *slot)
{
    if (slot->subject < 0 || slot->subject >= FR_RELATE_FANOUT) return;
    fr->fold.relate_as[slot->subject] =
        fr_io_failed_badly(slot)
            ? (u8)FRIEND_RELATION_NONE
            : fr_parse_relation(
                  fr_provider_text(fr, &fr->scratch, fr_io_answer(slot)));
}

/* Every neighbour of the current memory has been classified: join it,
 * then move on to the next. */
static void fr_fold_join_current(Friend_Context *fr)
{
    u32               slot = fr->fold.staged[fr->fold.relating_at];
    Fr_Memory_Record *m    = fr_memory(fr, slot);

    if (m) {
        fr_connect_to_related(fr, slot, fr->fold.relate_to,
                              fr->fold.relate_as, fr->fold.relate_count);
        /* Now it is in the graph: recall can find it, and so can the
         * next staged memory looking for neighbours. */
        m->staged = 0;
        fr_mark_dirty(fr);
    }
    ++fr->fold.relating_at;
    fr_fold_advance_relating(fr);
}

/* ================================================================== *
 *  What the friend wants now
 * ================================================================== */

/* The tail of the graph by creation time, oldest first. Recency, not
 * relevance: the pass is asking what life has been like lately. */
static String fr_interiority_memories(Friend_Context *fr, Arena *arena)
{
    u32            newest[FR_INTERIORITY_MEMORIES];
    String_Builder sb;
    i32            found = fr_newest_memories(fr, FR_INTERIORITY_MEMORIES,
                                              newest);
    i32            i;

    if (found == 0) return str_nil;
    sb = sb_begin(arena);
    for (i = found - 1; i >= 0; --i) {
        Fr_Memory_Record *m = fr_memory(fr, newest[i]);
        if (!m) continue;
        fr_append_memory_string(&sb, fr, m, 0);
        if (i > 0) sb_append_char(&sb, '\n');
    }
    return sb_done(&sb);
}

static void fr_fold_finish(Friend_Context *fr);

/* The memories are formed and kept; only the monologue stays as it was,
 * and the friend says why rather than leaving it to be noticed. */
static void fr_interiority_skipped(Friend_Context *fr, String why)
{
    fr_push_event_text(fr, FRIEND_EVENT_ERROR, why);
    fr_fold_finish(fr);
}

static void fr_fold_start_interiority(Friend_Context *fr)
{
    Fr_Io_Slot       *slot;
    Fr_Model_Call     call;
    Fr_Prompt_Message turn;
    String            memories, history;

    fr->fold.stage   = FR_FOLD_INTERIORITY;
    fr->fold.pending = 0;
    slot = fr_io_acquire(fr, FR_IO_PURPOSE_INTERIORITY, FRIEND_IO_HTTPS, 0);
    if (!slot) {
        fr_interiority_skipped(fr, str_lit("no room to ask what the friend "
                                           "wants now: every I/O slot is "
                                           "in use"));
        return;
    }

    memories = fr_interiority_memories(fr, &slot->arena);
    history  = fr_fold_dialog(fr, &slot->arena, 1, fr->utc_offset_minutes,
                              FR_INTERIORITY_EVENTS);

    turn.is_user     = 1;
    turn.text        = fr_interiority_turn();
    turn.images      = 0;
    turn.image_count = 0;

    memset(&call, 0, sizeof(call));
    call.model  = fr->models.interiority;
    call.system = fr_build_interiority_system(fr, &slot->arena, memories,
                                              str_trim(history));
    call.messages          = &turn;
    call.message_count     = 1;
    call.max_output_tokens = FR_INTERIORITY_MAX_TOKENS;
    call.thinking_budget   = FR_INTERIORITY_THINKING;
    call.temperature       = -1.0f;

    if (!fr_io_post(fr, slot, fr_provider_chat(fr, &slot->arena, &call))) {
        fr_interiority_skipped(fr, friend_last_error(fr));
        return;
    }
    fr->fold.pending = 1;
}

/* One first-person statement per line. The prompt asks for bare "I"
 * lines and models decorate, so bullets and quotes come off and anything
 * that is not an "I" statement is dropped rather than let into the
 * system prompt. */
static void fr_fold_finish_interiority(Friend_Context *fr, Fr_Io_Slot *slot)
{
    Arena_Mark     mark;
    String         rest, line;
    String_Builder sb;
    i32            statements = 0;

    if (fr_io_failed_badly(slot)) return; /* keep the stream it had */

    mark = arena_mark(&fr->scratch);
    rest = fr_provider_text(fr, &fr->scratch, fr_io_answer(slot));
    sb   = sb_begin(&fr->scratch);
    while (str_next_line(&rest, &line) &&
           statements < FR_INTERIORITY_STATEMENTS) {
        line = str_trim(line);
        while (line.len > 0) {
            char c = line.data[0];
            if (c == '-' || c == '*' || c == '#' || c == '.' || c == ')' ||
                c == ' ' || c == '\t' || (c >= '0' && c <= '9')) {
                line = str_skip(line, 1);
                continue;
            }
            break;
        }
        while (line.len > 0 &&
               (line.data[0] == '"' || line.data[0] == '\'')) {
            line = str_skip(line, 1);
        }
        while (line.len > 0 && (line.data[line.len - 1] == '"' ||
                                line.data[line.len - 1] == '\'')) {
            --line.len;
        }
        line = str_trim(line);

        /* An "I" statement, not merely a word starting with i. The third
         * test is the first byte of a curly apostrophe. */
        if (line.len < 2 || line.data[0] != 'I') continue;
        if (line.data[1] != ' ' && line.data[1] != '\'' &&
            (u8)line.data[1] != 0xE2) {
            continue;
        }

        if (statements > 0) sb_append_char(&sb, '\n');
        sb_append(&sb, line);
        ++statements;
    }

    if (statements > 0) {
        fr_set_interiority(fr, sb_done(&sb));
        fr_mark_dirty(fr);
        fr_push_event_text(fr, FRIEND_EVENT_INTERIORITY_CHANGED,
                           friend_interiority(fr));
    }
    arena_reset_to(&fr->scratch, mark);
}

static void fr_fold_finish(Friend_Context *fr)
{
    fr_fold_accounted(fr);
    fr_push_memories_changed(fr);
    fr_mark_dirty(fr);
    fr_fold_reset(fr);

    /* An epoch that has run its length is settled now rather than in
     * the middle of a reply. */
    fr_prune_unused(fr);
}

/* ================================================================== *
 *  Routing
 * ================================================================== */

/* One of the stage's requests came back. 1 when it was the last. */
static b32 fr_fold_landed(Friend_Context *fr)
{
    if (fr->fold.pending > 0) fr->fold.pending -= 1;
    return fr->fold.pending == 0;
}

static void fr_fold_route(Friend_Context *fr, Fr_Io_Slot *slot)
{
    switch (slot->purpose) {
    case FR_IO_PURPOSE_FOLD_EMBED_QUERY:
    case FR_IO_PURPOSE_FOLD_RERANK:
        if (fr->fold.stage == FR_FOLD_RECALLING &&
            fr_recall_route(fr, &fr->fold.recall, &fr_fold_recall, slot)) {
            fr_fold_start_extract(fr);
        }
        break;

    case FR_IO_PURPOSE_EXTRACT_EVENTS:
        if (fr->fold.stage != FR_FOLD_EXTRACTING) break;
        if (fr_io_failed_badly(slot)) {
            fr_fold_abandon(fr, fr_provider_failure(fr, slot));
            break;
        }
        fr_fold_landed(fr);
        fr_fold_finish_extract(fr, slot);
        if (fr->fold.staged_count == 0) {
            /* "no events" is a legitimate answer: a stretch of small talk
             * is not a thing to remember. */
            fr_fold_start_interiority(fr);
        } else if (!fr_fold_start_embeddings(fr)) {
            fr_fold_abandon(fr, str_lit("could not embed the new memories"));
        }
        break;

    case FR_IO_PURPOSE_FOLD_EMBED:
        if (fr->fold.stage != FR_FOLD_EMBEDDING) break;
        fr_fold_finish_embedding(fr, slot);
        if (fr_fold_landed(fr)) fr_fold_begin_relating(fr);
        break;

    case FR_IO_PURPOSE_RELATE:
        if (fr->fold.stage != FR_FOLD_RELATING) break;
        fr_fold_finish_relate(fr, slot);
        if (fr_fold_landed(fr)) fr_fold_join_current(fr);
        break;

    case FR_IO_PURPOSE_INTERIORITY:
        if (fr->fold.stage != FR_FOLD_INTERIORITY) break;
        fr_fold_finish_interiority(fr, slot);
        if (fr_fold_landed(fr)) fr_fold_finish(fr);
        break;

    default:
        break;
    }
}

/* ================================================================== *
 *  Catching up on embeddings
 *
 *  A friend imported out of an export whose vectors were made by a
 *  different embedding model has descriptions but nothing to compare
 *  them by, so recall cannot see those memories at all. friend.h
 *  promises they are re-derived and that it shows up as I/O work: this
 *  is that work, a few at a time whenever nothing else is happening.
 * ================================================================== */

#define FR_BACKFILL_AT_ONCE 4

/* How long to leave a provider that would not embed them before asking
 * again: asking every tick is a busy loop against a dead network. */
#define FR_BACKFILL_RETRY_MS 10000u

static void fr_backfill_route(Friend_Context *fr, Fr_Io_Slot *slot)
{
    Fr_Memory_Record *m = fr_memory(fr, (u32)slot->subject);

    if (!m) return;
    if (!fr_read_embedding(fr, slot, m->embedding)) {
        if (fr_retry_failed(&fr->backfill_retry, fr->now_unix_ms,
                            FR_BACKFILL_RETRY_MS)) {
            fr_push_event_text(fr, FRIEND_EVENT_ERROR,
                               fr_io_failed_badly(slot)
                                   ? fr_provider_failure(fr, slot)
                                   : str_lit("the embedding model answered "
                                             "with something that is not "
                                             "a vector"));
        }
        return;
    }
    fr_retry_worked(&fr->backfill_retry);
    m->needs_embedding = 0;
    fr_mark_dirty(fr);
}

static void fr_backfill_embeddings(Friend_Context *fr)
{
    i32 started = 0;
    i32 slot;

    if (!fr || fr->fold.stage != FR_FOLD_IDLE) return;
    if (str_is_empty(fr->api_key)) return;
    if (fr_retry_waiting(&fr->backfill_retry, fr->now_unix_ms)) return;

    for (slot = 0; slot < fr->memory_slots && started < FR_BACKFILL_AT_ONCE;
         ++slot) {
        Fr_Memory_Record *m = &fr->memories[slot];

        if (!m->occupied || m->staged || !m->needs_embedding) continue;

        if (fr_io_in_flight(fr, FR_IO_PURPOSE_EMBED_DOCUMENT, slot)) continue;
        if (!fr_embed_memory(fr, FR_IO_PURPOSE_EMBED_DOCUMENT, slot, m)) break;
        ++started;
    }
}
