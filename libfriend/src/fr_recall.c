/* fr_recall.c — the memory graph, read.
 *
 * See fr_recall.h for what recall is. This file is the arithmetic and
 * the graph walk; the model calls that sit between them are in
 * fr_rank.c.
 *
 * Everything here is a pure function of the state and the clock. That is
 * worth protecting: it means a recall can be run twice and compared, and
 * a disagreement is a bug rather than the weather.
 */

#include "fr_recall.h"
#include "fr_prompt.h"
#include <math.h>

/* ================================================================== *
 *  Vectors
 * ================================================================== */

static void fr_quantize_embedding(const f32 *values, i32 count, i8 *out)
{
    i32 i;
    if (!values || !out) return;
    if (count > FR_EMBED_DIMS) count = FR_EMBED_DIMS;
    for (i = 0; i < count; ++i) {
        f32 scaled = values[i] * FR_EMBED_SCALE;
        i32 q      = (i32)(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
        if (q > 127) q = 127;
        if (q < -127) q = -127;
        out[i] = (i8)q;
    }
    for (; i < FR_EMBED_DIMS; ++i) out[i] = 0;
}

static f32 fr_cosine_i8(const i8 *a, const i8 *b, i32 count)
{
    i64 dot = 0, norm_a = 0, norm_b = 0;
    i32 i;

    if (!a || !b || count <= 0) return 0.0f;
    for (i = 0; i < count; ++i) {
        i64 av = a[i];
        i64 bv = b[i];
        dot += av * bv;
        norm_a += av * av;
        norm_b += bv * bv;
    }
    if (norm_a == 0 || norm_b == 0) return 0.0f;
    return (f32)((f64)dot / (sqrt((f64)norm_a) * sqrt((f64)norm_b)));
}

static i32 fr_nearest_memories(Friend_Context *fr, const i8 *query, i32 k,
                        u32 *out_slots, f32 *out_scores)
{
    i32 found = 0;
    i32 slot;

    if (!fr || !query || !out_slots || k <= 0) return 0;

    /* An insertion sort over k, not a sort of the whole graph. k is 16
     * and the graph is tens of thousands: one pass, sixteen compares. */
    for (slot = 0; slot < fr->memory_slots; ++slot) {
        Fr_Memory_Record *m = &fr->memories[slot];
        f32               score;
        i32               at;

        if (!fr_memory_comparable(m)) continue;
        score = fr_cosine_i8(m->embedding, query, FR_EMBED_DIMS);

        if (found == k && score <= out_scores[found - 1]) continue;
        at = (found < k) ? found : k - 1;
        while (at > 0 && out_scores[at - 1] < score) {
            out_slots[at]  = out_slots[at - 1];
            out_scores[at] = out_scores[at - 1];
            --at;
        }
        out_slots[at]  = (u32)slot;
        out_scores[at] = score;
        if (found < k) ++found;
    }
    return found;
}

/* ================================================================== *
 *  The conversation as text
 * ================================================================== */

static Fr_Turn_Kind fr_event_turn_kind(const Fr_Chat_Event *e)
{
    if (e->kind == FRIEND_MESSAGE_USER) return FR_TURN_USER;
    if (e->kind == FRIEND_MESSAGE_FRIEND) return FR_TURN_FRIEND;
    return FR_TURN_AMBIENT;
}

static void fr_append_chat_dialog(String_Builder *sb, Friend_Context *fr,
                           const Fr_Chat_Event *events, i32 count,
                           b32 relative_ago, i32 utc_offset_minutes)
{
    i32 i;

    for (i = 0; i < count; ++i) {
        const Fr_Chat_Event *e = &events[i];
        if (!fr_event_for_model(e)) continue;
        fr_append_dialog_line(sb, fr->now_unix_ms, relative_ago,
                              e->occurred_unix_ms, utc_offset_minutes,
                              fr_event_turn_kind(e), fr_chat_text(e));
    }
}

static String fr_recall_query(Friend_Context *fr, Arena *arena, i32 messages_back)
{
    String_Builder sb;
    i32            start;

    if (!fr || !arena) return str_nil;
    start = fr->active_count - messages_back;
    if (start < 0) start = 0;

    sb = sb_begin(arena);
    /* UTC, no relative times: the query has to embed to the same vector
     * every time it is asked for, or recall would drift with the clock. */
    fr_append_chat_dialog(&sb, fr, fr->active_events + start,
                          fr->active_count - start, 0, 0);
    return sb_done(&sb);
}

static String fr_embed_query_text(Arena *arena, String conversation)
{
    String_Builder sb = sb_begin(arena);
    sb_append(&sb, str_lit("task: search result | query: "));
    sb_append(&sb, conversation);
    return sb_done(&sb);
}

static String fr_embed_document_text(Arena *arena, String title, String text)
{
    String_Builder sb = sb_begin(arena);
    sb_append(&sb, str_lit("title: "));
    sb_append(&sb, str_is_empty(title) ? str_lit("none") : title);
    sb_append(&sb, str_lit(" | text: "));
    sb_append(&sb, text);
    return sb_done(&sb);
}

/* ================================================================== *
 *  Memories as prompt lines
 * ================================================================== */

static void fr_append_memory_string(String_Builder *sb, Friend_Context *fr,
                             const Fr_Memory_Record *m, b32 absolute)
{
    sb_append_char(sb, '[');
    if (absolute) {
        fr_append_timestamp(sb, m->created_unix_ms, fr->utc_offset_minutes, 1);
        sb_append(sb, str_lit("] "));
    } else {
        fr_append_human_duration(sb,
                                 (i64)(fr->now_unix_ms - m->created_unix_ms));
        sb_append(sb, str_lit(" ago] "));
    }
    sb_append(sb, fr_memory_description(m));
}

/* The same with the captions of any photos the memory formed around. */
static void fr_append_recalled_memory_string(String_Builder *sb,
                                             Friend_Context *fr,
                                             const Fr_Memory_Record *m,
                                             b32 absolute)
{
    String cursor = fr_memory_dialog(m);
    u64    at;
    Fr_Turn_Kind kind;
    String turn_text;
    b32    first = 1;

    fr_append_memory_string(sb, fr, m, absolute);

    /* The captions of the photos this memory formed around, so a
     * text-only recall still carries what the friend saw. They are
     * dialogue turns, so they are found by walking. */
    while (fr_dialog_next(&cursor, &at, &kind, &turn_text)) {
        if (kind != FR_TURN_CAPTION || str_is_empty(turn_text)) continue;
        if (first) {
            sb_append(sb, str_lit(" [image(s) you saw at the time: "));
            first = 0;
        } else {
            sb_append_char(sb, ' ');
        }
        sb_append(sb, turn_text);
    }
    if (!first) sb_append_char(sb, ']');
}

/* ================================================================== *
 *  Scoring
 * ================================================================== */

static i32 fr_newest_memories(Friend_Context *fr, i32 want, u32 *out)
{
    i32 found = 0;
    i32 slot;

    for (slot = 0; slot < fr->memory_slots; ++slot) {
        Fr_Memory_Record *m = &fr->memories[slot];
        u64               created;
        i32               at;

        if (!fr_memory_comparable(m)) continue;
        created = m->created_unix_ms;

        if (found == want &&
            created <= fr->memories[out[found - 1]].created_unix_ms) {
            continue;
        }
        at = (found < want) ? found : want - 1;
        while (at > 0 && fr->memories[out[at - 1]].created_unix_ms < created) {
            out[at] = out[at - 1];
            --at;
        }
        out[at] = (u32)slot;
        if (found < want) ++found;
    }
    return found;
}

static i32 fr_add_recent_memories(Friend_Context *fr, Fr_Relevant *out, i32 count,
                           i32 capacity)
{
    u32 newest[FR_RECENT_MAX];
    i32 want = fr->tuning.recall_recent;
    i32 found, i;

    if (want <= 0) return count;
    found = fr_newest_memories(fr, want, newest);

    /* Oldest of the three first, so that when the list is deduplicated
     * the stronger (newer) entry is the one that survives a tie with a
     * rerank hit further down. */
    for (i = found - 1; i >= 0 && count < capacity; --i) {
        Fr_Memory_Record *m = &fr->memories[newest[i]];
        f64 hours = 0.0;
        f64 strength;

        if (fr->now_unix_ms > m->created_unix_ms) {
            hours = (f64)(fr->now_unix_ms - m->created_unix_ms) / 3600000.0;
        }
        /* 0.7 now, 0.55 a day later, 0.13 a week later. */
        strength = pow(1.01, -hours) * 0.7;

        out[count].slot          = newest[i];
        out[count].strength      = (f32)strength;
        out[count].from_reranker = 0;
        ++count;
    }
    return count;
}

static b32 fr_slots_contain(const u32 *slots, i32 count, u32 slot)
{
    i32 i;
    for (i = 0; i < count; ++i) {
        if (slots[i] == slot) return 1;
    }
    return 0;
}

static i32 fr_dedup_relevant(Fr_Relevant *at, i32 count)
{
    i32 kept = 0;
    i32 i, j;

    for (i = 0; i < count; ++i) {
        b32 seen = 0;
        for (j = 0; j < kept && !seen; ++j) seen = at[j].slot == at[i].slot;
        if (seen) continue;
        at[kept++] = at[i];
    }
    return kept;
}

static u64 fr_epoch_length(Friend_Context *fr)
{
    return (u64)fr_max(fr->memory_count, fr->tuning.prune_epoch_min);
}

static void fr_epoch_open(Friend_Context *fr)
{
    i32 slot;
    for (slot = 0; slot < fr->memory_slots; ++slot) {
        if (!fr->memories[slot].occupied) continue;
        fr->memories[slot].in_this_epoch   = 1;
        fr->memories[slot].uses_this_epoch = 0;
    }
    fr->epoch_samples = 0;
    fr->epoch_active  = 1;
    fr_mark_dirty(fr);
}

static void fr_record_memory_usage(Friend_Context *fr, const Fr_Relevant *relevant,
                            i32 relevant_count, const u32 *rendered,
                            i32 rendered_count)
{
    i32 i;

    if (!fr) return;
    /* The first tracked reply opens the first epoch; later ones are
     * opened by the prune pass that closes the one before. */
    if (!fr->epoch_active) fr_epoch_open(fr);

    for (i = 0; i < relevant_count; ++i) {
        Fr_Memory_Record *m;

        if (!relevant[i].from_reranker) continue;
        if (!fr_slots_contain(rendered, rendered_count, relevant[i].slot)) {
            continue;
        }

        m = fr_memory(fr, relevant[i].slot);
        if (!m) continue;
        ++m->use_count;
        ++m->uses_this_epoch;
    }

    ++fr->replies_total;
    ++fr->epoch_samples;
    fr_mark_dirty(fr);
}

/* ================================================================== *
 *  Timelines
 *
 *  A timeline is a maximal chain through the graph: start at a memory
 *  nothing points to, follow the connections, end where they run out.
 *  Every chain that passes through a recalled memory is a candidate, and
 *  there can be many of them, so the enumeration is capped in three
 *  places — path length, paths per memory, and candidates in total. A
 *  graph that branched enough to hit those caps would produce timelines
 *  nobody could read anyway.
 * ================================================================== */

typedef struct Fr_Path {
    u32 slots[FR_TIMELINE_ELEMENTS_MAX];
    u8  relations[FR_TIMELINE_ELEMENTS_MAX];
    i32 count;
} Fr_Path;

#define FR_PATH_STACK_MAX 256

static b32 fr_path_contains(const Fr_Path *p, u32 slot)
{
    return fr_slots_contain(p->slots, p->count, slot);
}

/* Every chain that ends at `from`, walking incoming edges. Each result
 * is oldest-first and ends at `from`. */
static i32 fr_paths_backward(Friend_Context *fr, Arena *arena, u32 from,
                             Fr_Path **out, i32 max_paths)
{
    Fr_Path *stack = arena_push(arena, Fr_Path, FR_PATH_STACK_MAX);
    Fr_Path *paths = arena_push(arena, Fr_Path, max_paths);
    i32      depth = 0, found = 0;

    if (!stack || !paths) return 0;
    *out = paths;

    stack[0].count       = 1;
    stack[0].slots[0]    = from;
    stack[0].relations[0] = FRIEND_RELATION_NONE;
    depth                = 1;

    while (depth > 0 && found < max_paths) {
        Fr_Path path = stack[--depth];
        u32     head = path.slots[0];
        u32     edge = fr->memories[head].first_in;
        b32     any  = 0;

        for (; edge != FR_SLOT_NONE; edge = fr->edges[edge].next_to) {
            Fr_Edge *e = &fr->edges[edge];
            Fr_Path *next;
            i32      i;

            if (!e->occupied) continue;
            if (!fr_memory(fr, e->from_slot)) continue;
            if (fr_path_contains(&path, e->from_slot)) continue; /* cycle */
            if (path.count >= FR_TIMELINE_ELEMENTS_MAX) continue;
            if (depth >= FR_PATH_STACK_MAX) break;

            any  = 1;
            next = &stack[depth++];
            next->count = path.count + 1;
            for (i = path.count - 1; i >= 0; --i) {
                next->slots[i + 1]     = path.slots[i];
                next->relations[i + 1] = path.relations[i];
            }
            next->slots[0]     = e->from_slot;
            next->relations[0] = e->relation;
        }

        /* Nothing points at the head, or nothing we could follow: this
         * chain is as far back as it goes. */
        if (!any) paths[found++] = path;
    }
    return found;
}

/* Every chain that starts at `from`, walking outgoing edges. */
static i32 fr_paths_forward(Friend_Context *fr, Arena *arena, u32 from,
                            Fr_Path **out, i32 max_paths)
{
    Fr_Path *stack = arena_push(arena, Fr_Path, FR_PATH_STACK_MAX);
    Fr_Path *paths = arena_push(arena, Fr_Path, max_paths);
    i32      depth = 0, found = 0;

    if (!stack || !paths) return 0;
    *out = paths;

    stack[0].count        = 1;
    stack[0].slots[0]     = from;
    stack[0].relations[0] = FRIEND_RELATION_NONE;
    depth                 = 1;

    while (depth > 0 && found < max_paths) {
        Fr_Path path = stack[--depth];
        u32     tail = path.slots[path.count - 1];
        u32     edge = fr->memories[tail].first_out;
        b32     any  = 0;

        for (; edge != FR_SLOT_NONE; edge = fr->edges[edge].next_from) {
            Fr_Edge *e = &fr->edges[edge];
            Fr_Path *next;

            if (!e->occupied) continue;
            if (!fr_memory(fr, e->to_slot)) continue;
            if (fr_path_contains(&path, e->to_slot)) continue;
            if (path.count >= FR_TIMELINE_ELEMENTS_MAX) continue;
            if (depth >= FR_PATH_STACK_MAX) break;

            any   = 1;
            next  = &stack[depth++];
            *next = path;
            next->relations[next->count - 1] = e->relation;
            next->slots[next->count]         = e->to_slot;
            next->relations[next->count]     = FRIEND_RELATION_NONE;
            ++next->count;
        }

        if (!any) paths[found++] = path;
    }
    return found;
}

static Fr_Timelines fr_build_timelines(Friend_Context *fr, Arena *arena,
                                const Fr_Relevant *relevant, i32 count)
{
    Fr_Timelines result;
    Fr_Timeline *found;
    Fr_Timeline *chosen;
    i32          found_count = 0;
    i32          chosen_count = 0;
    i32          max_timelines;
    i32          r;
    u32         *added;
    i32          added_count = 0;

    result.at    = 0;
    result.count = 0;
    if (!fr || !arena || count <= 0) return result;

    max_timelines = fr->tuning.max_timelines;
    if (max_timelines <= 0) return result;

    found  = arena_push(arena, Fr_Timeline, FR_TIMELINES_FOUND_MAX);
    chosen = arena_push(arena, Fr_Timeline, max_timelines);
    added  = arena_push(arena, u32, max_timelines * FR_TIMELINE_ELEMENTS_MAX);
    if (!found || !chosen || !added) return result;

    for (r = 0; r < count && found_count < FR_TIMELINES_FOUND_MAX; ++r) {
        Arena_Mark mark    = arena_mark(arena);
        Fr_Path   *back    = 0;
        Fr_Path   *forward = 0;
        i32        back_count, forward_count;
        i32        b;

        if (!fr_memory(fr, relevant[r].slot)) continue;

        back_count    = fr_paths_backward(fr, arena, relevant[r].slot, &back,
                                          FR_TIMELINE_PATHS_PER_MEMORY);
        forward_count = fr_paths_forward(fr, arena, relevant[r].slot,
                                         &forward,
                                         FR_TIMELINE_PATHS_PER_MEMORY);

        for (b = 0; b < back_count && found_count < FR_TIMELINES_FOUND_MAX;
             ++b) {
            i32 f;
            for (f = 0;
                 f < forward_count && found_count < FR_TIMELINES_FOUND_MAX;
                 ++f) {
                Fr_Timeline *t     = &found[found_count];
                i32          focus = back[b].count - 1;
                i32          total = back[b].count + forward[f].count - 1;
                i32          i;

                if (total > FR_TIMELINE_ELEMENTS_MAX) continue;

                for (i = 0; i < back[b].count; ++i) {
                    t->slots[i]     = back[b].slots[i];
                    t->relations[i] = back[b].relations[i];
                }
                /* The two halves share the recalled memory: the forward
                 * half starts where the backward half ended. */
                for (i = 1; i < forward[f].count; ++i) {
                    t->slots[focus + i]     = forward[f].slots[i];
                    t->relations[focus + i] = forward[f].relations[i];
                }
                t->relations[focus] = forward[f].relations[0];
                t->count            = total;
                t->most_relevant    = focus;
                t->match_strength   = relevant[r].strength;
                ++found_count;
            }
        }
        arena_reset_to(arena, mark);
    }

    /* Keep the chains that cover the most new ground. Greedy, because
     * the alternative is a set-cover and this is a prompt. */
    while (chosen_count < max_timelines) {
        i32 best = -1;
        f32 best_overlap = 2.0f;
        i32 i, j;

        for (i = 0; i < found_count; ++i) {
            i32 already = 0;
            f32 overlap;

            if (found[i].count <= 0) continue;
            for (j = 0; j < found[i].count; ++j) {
                already += fr_slots_contain(added, added_count,
                                            found[i].slots[j]);
            }
            overlap = (f32)already / (f32)found[i].count;
            if (overlap < best_overlap) {
                best_overlap = overlap;
                best         = i;
            }
        }

        /* Everything left is already covered. */
        if (best < 0 || best_overlap >= 1.0f) break;

        chosen[chosen_count++] = found[best];
        for (j = 0; j < found[best].count; ++j) {
            if (!fr_slots_contain(added, added_count, found[best].slots[j])) {
                added[added_count++] = found[best].slots[j];
            }
        }
        found[best].count = 0; /* taken */
    }

    /* Strongest match first, stably. */
    {
        i32 i, j;
        for (i = 1; i < chosen_count; ++i) {
            Fr_Timeline hold = chosen[i];
            j = i;
            while (j > 0 && chosen[j - 1].match_strength < hold.match_strength) {
                chosen[j] = chosen[j - 1];
                --j;
            }
            chosen[j] = hold;
        }
    }

    result.at    = chosen;
    result.count = chosen_count;
    return result;
}

static b32 fr_timelines_contain(Fr_Timelines timelines, u32 slot)
{
    i32 t;
    for (t = 0; t < timelines.count; ++t) {
        if (fr_slots_contain(timelines.at[t].slots, timelines.at[t].count,
                             slot)) {
            return 1;
        }
    }
    return 0;
}

static i32 fr_timeline_slots(Fr_Timelines timelines, u32 *out, i32 capacity,
                      u32 *focus, i32 *focus_count)
{
    i32 count = 0;
    i32 t, i;

    if (focus_count) *focus_count = 0;
    for (t = 0; t < timelines.count; ++t) {
        const Fr_Timeline *tl = &timelines.at[t];
        for (i = 0; i < tl->count && count < capacity; ++i) {
            if (fr_slots_contain(out, count, tl->slots[i])) continue;
            out[count++] = tl->slots[i];
            if (focus && focus_count && i == tl->most_relevant) {
                focus[(*focus_count)++] = tl->slots[i];
            }
        }
    }
    return count;
}

static String fr_describe_timelines(Friend_Context *fr, Arena *arena,
                             Fr_Timelines timelines, b32 absolute)
{
    String_Builder sb;
    u32           *all;
    u32           *focus;
    i32            all_count, focus_count;
    i32            capacity;
    i32            i, j;

    if (!fr || !arena) return str_nil;
    if (timelines.count <= 0) {
        return str_lit("(no memories found for this conversation)");
    }

    capacity = timelines.count * FR_TIMELINE_ELEMENTS_MAX;
    all      = arena_push(arena, u32, capacity);
    focus    = arena_push(arena, u32, capacity);
    if (!all || !focus) return str_nil;

    /* First mention wins, including for the "most relevant" list: a
     * memory that already turned up as somebody else's context does not
     * get introduced again as the thing we were looking for. */
    all_count = fr_timeline_slots(timelines, all, capacity, focus,
                                  &focus_count);

    /* The full list reads in the order things happened. */
    for (i = 1; i < all_count; ++i) {
        u32 hold = all[i];
        u64 at   = fr->memories[hold].created_unix_ms;
        j = i;
        while (j > 0 && fr->memories[all[j - 1]].created_unix_ms > at) {
            all[j] = all[j - 1];
            --j;
        }
        all[j] = hold;
    }

    sb = sb_begin(arena);
    sb_append(&sb, str_lit("The most relevant memories:\n"));
    for (i = 0; i < focus_count; ++i) {
        Fr_Memory_Record *m = fr_memory(fr, focus[i]);
        if (!m) continue;
        fr_append_recalled_memory_string(&sb, fr, m, absolute);
        sb_append_char(&sb, '\n');
    }
    sb_append(&sb, str_lit("\n\nThe full timeline of memories you remember "
                           "right now:\n"));
    for (i = 0; i < all_count; ++i) {
        Fr_Memory_Record *m = fr_memory(fr, all[i]);
        if (!m) continue;
        fr_append_recalled_memory_string(&sb, fr, m, absolute);
        sb_append_char(&sb, '\n');
    }
    return sb_done(&sb);
}

static String fr_recently_inserted(Friend_Context *fr, Arena *arena,
                            Fr_Timelines timelines, b32 absolute)
{
    String_Builder sb;
    u32            newest[16];
    i32            want, found, i;
    b32            wrote_intro = 0;

    if (!fr || !arena) return str_nil;

    /* The service looked at the last ten, but asked for one fewer than
     * the graph holds — so a friend with a single memory is told about
     * none of them. Kept, because it is what shipped. */
    want = fr->memory_count - 1;
    if (want > 10) want = 10;
    if (want <= 0) return str_nil;

    found = fr_newest_memories(fr, want, newest);
    sb    = sb_begin(arena);

    /* Oldest of the tail first. */
    for (i = found - 1; i >= 0; --i) {
        Fr_Memory_Record *m = fr_memory(fr, newest[i]);

        if (!m) continue;
        if (fr_timelines_contain(timelines, newest[i])) continue;

        if (!wrote_intro) {
            sb_append(&sb, str_lit("Some recent memories that weren't present "
                                   "in your recalled timelines:\n"));
            wrote_intro = 1;
        }
        fr_append_memory_string(&sb, fr, m, absolute);
        sb_append_char(&sb, '\n');
    }
    return sb_done(&sb);
}

static Fr_Recall_Text fr_render_recall(Friend_Context *fr, Arena *arena,
                                Fr_Recall *run, b32 absolute, u32 *rendered,
                                i32 rendered_capacity)
{
    Fr_Recall_Text text;
    Fr_Timelines   timelines;

    run->relevant_count = fr_add_recent_memories(
        fr, run->relevant, run->relevant_count, FR_RELEVANT_MAX);
    run->relevant_count = fr_dedup_relevant(run->relevant, run->relevant_count);

    timelines = fr_build_timelines(fr, arena, run->relevant,
                                   run->relevant_count);
    text.rendered_count =
        rendered ? fr_timeline_slots(timelines, rendered, rendered_capacity,
                                     0, 0)
                 : 0;
    text.described = fr_describe_timelines(fr, arena, timelines, absolute);
    text.recently  = fr_recently_inserted(fr, arena, timelines, absolute);
    return text;
}
