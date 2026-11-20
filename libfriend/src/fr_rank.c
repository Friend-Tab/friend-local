/* fr_rank.c — a recall run: embed, compare, rerank.
 *
 * See fr_rank.h. Every number here is the service's except the rerank
 * call itself, which replaced a hosted ranking service with a cheap
 * model asked for a list of numbers.
 */

#include "fr_rank.h"
#include "fr_prompt.h"
#include "fr_provider.h"
#include "fr_recall.h"

/* Reasoning rather than talking: a few numbers, no thinking, no
 * creativity. */
#define FR_RERANK_MAX_TOKENS 64
#define FR_RERANK_THINKING 0
#define FR_RERANK_TEMPERATURE 0.0f

/* Past this a run of digits is not a choice, and reading on would
 * overflow. */
#define FR_RERANK_NUMBER_MAX 1000000

/* The synthetic scores the service used when its reranker was down:
 * descending through [0.4, 0.6], deliberately under 1 so a memory from
 * five minutes ago can still outrank a rerank hit. They are used for a
 * successful rerank too, because what comes back is an order and not a
 * score. */
static f32 fr_rank_strength(i32 position, i32 count)
{
    i32 span = count - 1;
    if (span < 1) span = 1;
    return 0.6f - 0.2f * (f32)position / (f32)span;
}

static void fr_recall_add(Fr_Recall *run, u32 slot, f32 strength,
                          b32 from_reranker)
{
    Fr_Relevant *r;
    if (run->relevant_count >= FR_RELEVANT_MAX) return;
    r                = &run->relevant[run->relevant_count++];
    r->slot          = slot;
    r->strength      = strength;
    r->from_reranker = (u8)(from_reranker ? 1 : 0);
}

/* The embedding order, standing in for the reranker's answer. It is not
 * one, so nothing it picks counts as a use. */
static void fr_recall_fallback(Friend_Context *fr, Fr_Recall *run, i32 q)
{
    const Fr_Recall_Query *query = &run->queries[q];
    i32                    keep  = fr->tuning.recall_keep;
    i32                    i;

    if (keep > query->candidate_count) keep = query->candidate_count;
    for (i = 0; i < keep; ++i) {
        fr_recall_add(run, query->candidates[i], fr_rank_strength(i, keep),
                      0);
    }
}

/* ------------------------------------------------------------------ *
 *  Embedding
 * ------------------------------------------------------------------ */

static b32 fr_read_embedding(Friend_Context *fr, const Fr_Io_Slot *slot, i8 *out)
{
    f32 values[FR_EMBED_DIMS];

    if (fr_io_failed_badly(slot)) return 0;
    if (fr_provider_embedding(fr, fr_io_answer(slot), values, FR_EMBED_DIMS) !=
        FR_EMBED_DIMS) {
        return 0;
    }
    fr_quantize_embedding(values, FR_EMBED_DIMS, out);
    return 1;
}

static b32 fr_embed_memory(Friend_Context *fr, Fr_Io_Purpose purpose, i32 subject,
                    const Fr_Memory_Record *m)
{
    Fr_Io_Slot    *slot;
    Arena_Mark     mark;
    String         title, body, captions;
    String_Builder sb;
    b32            sent;

    slot = fr_io_acquire(fr, purpose, FRIEND_IO_HTTPS, subject);
    if (!slot) return 0;

    /* The document side of the asymmetric pair: the title is the time
     * the exchange ended, the text the description plus whatever the
     * friend saw in any photos. */
    mark = arena_mark(&fr->scratch);
    sb   = sb_begin(&fr->scratch);
    fr_append_time_address(&sb, m->dialog_end_unix_ms ? m->dialog_end_unix_ms
                                                      : m->created_unix_ms,
                           0);
    title = sb_done(&sb);

    sb = sb_begin(&slot->arena);
    sb_append(&sb, fr_memory_description(m));
    captions = fr_memory_captions(&fr->scratch, m);
    if (!str_is_empty(captions)) {
        sb_append_char(&sb, '\n');
        sb_append(&sb, captions);
    }
    body = sb_done(&sb);

    sent = fr_io_post(fr, slot,
                      fr_provider_embed(fr, &slot->arena,
                                        fr_embed_document_text(&slot->arena,
                                                               title, body)));
    arena_reset_to(&fr->scratch, mark);
    return sent;
}

/* ------------------------------------------------------------------ *
 *  The run
 * ------------------------------------------------------------------ */

static Fr_Io_Slot *fr_recall_slot(Friend_Context *fr, Fr_Recall *run,
                                  Fr_Io_Purpose purpose, i32 q)
{
    Fr_Io_Slot *slot = fr_io_acquire(fr, purpose, FRIEND_IO_HTTPS, q);
    if (slot) slot->subject_event = run->serial;
    return slot;
}

static b32 fr_recall_start(Friend_Context *fr, Fr_Recall *run,
                    const Fr_Recall_Spec *spec)
{
    i32 q;

    memset(run, 0, sizeof(*run));
    run->serial = fr->next_io_id;
    run->stage  = FR_RECALL_EMBEDDING;

    for (q = 0; q < spec->query_count && q < FR_RECALL_QUERIES; ++q) {
        Arena_Mark  mark  = arena_mark(&fr->scratch);
        String      query = spec->query(fr, &fr->scratch, q, 0);
        Fr_Io_Slot *slot;
        b32         sent;

        if (str_is_empty(query)) {
            arena_reset_to(&fr->scratch, mark);
            continue;
        }
        slot = fr_recall_slot(fr, run, spec->embed_purpose, q);
        if (!slot) {
            arena_reset_to(&fr->scratch, mark);
            break;
        }
        /* The request text lives in the slot; the query it was made
         * from was scratch and is not needed again. */
        sent = fr_io_post(fr, slot,
                          fr_provider_embed(fr, &slot->arena,
                                            fr_embed_query_text(&slot->arena,
                                                                query)));
        arena_reset_to(&fr->scratch, mark);
        if (!sent) break;
        run->pending += 1;
    }

    if (run->pending == 0) {
        run->stage = FR_RECALL_DONE;
        return 0;
    }
    return 1;
}

static b32 fr_recall_send_rerank(Friend_Context *fr, Fr_Recall *run,
                                 const Fr_Recall_Spec *spec, i32 q)
{
    const Fr_Recall_Query *query = &run->queries[q];
    i32                    keep  = fr->tuning.recall_keep;
    String                 choices[FR_CANDIDATES_MAX];
    Fr_Prompt_Message      turn;
    Fr_Model_Call          call;
    Fr_Io_Slot            *slot;
    Arena_Mark             mark;
    i32                    i;

    slot = fr_recall_slot(fr, run, spec->rerank_purpose, q);
    if (!slot) return 0;

    /* Both the conversation and the candidate lines are built in the
     * slot, because the request body quotes them and the body has to
     * outlive this tick. */
    for (i = 0; i < query->candidate_count; ++i) {
        Fr_Memory_Record *m = fr_memory(fr, query->candidates[i]);
        String_Builder    sb;
        if (!m) {
            choices[i] = str_nil;
            continue;
        }
        sb = sb_begin(&slot->arena);
        fr_append_time_address(&sb, m->created_unix_ms, 0);
        sb_append_char(&sb, ' ');
        sb_append(&sb, fr_memory_description(m));
        choices[i] = sb_done(&sb);
    }

    mark             = arena_mark(&fr->scratch);
    turn.is_user     = 1;
    turn.text        = fr_build_rerank_turn(&slot->arena,
                                            spec->query(fr, &fr->scratch, q, 1),
                                            choices, query->candidate_count,
                                            keep);
    turn.images      = 0;
    turn.image_count = 0;
    arena_reset_to(&fr->scratch, mark);

    memset(&call, 0, sizeof(call));
    call.model             = fr->models.rerank;
    call.system            = fr_rerank_system(&slot->arena, keep);
    call.messages          = &turn;
    call.message_count     = 1;
    call.max_output_tokens = FR_RERANK_MAX_TOKENS;
    call.thinking_budget   = FR_RERANK_THINKING;
    call.temperature       = FR_RERANK_TEMPERATURE;
    return fr_io_post(fr, slot, fr_provider_chat(fr, &slot->arena, &call));
}

static void fr_recall_start_reranks(Friend_Context *fr, Fr_Recall *run,
                                    const Fr_Recall_Spec *spec)
{
    i32 q;

    run->stage = FR_RECALL_RERANKING;
    for (q = 0; q < spec->query_count && q < FR_RECALL_QUERIES; ++q) {
        if (run->queries[q].candidate_count <= 0) continue;
        if (fr_recall_send_rerank(fr, run, spec, q)) {
            run->pending += 1;
        } else {
            fr_recall_fallback(fr, run, q);
        }
    }
    if (run->pending == 0) run->stage = FR_RECALL_DONE;
}

static void fr_recall_embedded(Friend_Context *fr, Fr_Recall *run,
                               Fr_Io_Slot *slot)
{
    Fr_Recall_Query *query = &run->queries[slot->subject];
    f32              scores[FR_CANDIDATES_MAX];
    i32              want  = fr->tuning.recall_candidates;

    /* Without a vector there is nothing to compare, so this window
     * contributes nothing. Another may still have landed, and the
     * newest memories are added regardless. */
    if (!fr_read_embedding(fr, slot, query->embedding)) {
        return;
    }
    query->candidate_count = fr_nearest_memories(fr, query->embedding, want,
                                                 query->candidates, scores);
}

static void fr_recall_reranked(Friend_Context *fr, Fr_Recall *run,
                               Fr_Io_Slot *slot)
{
    const Fr_Recall_Query *query = &run->queries[slot->subject];
    i32                    keep  = fr->tuning.recall_keep;
    String                 answer;
    i32                    at    = 0;
    i32                    taken = 0;

    if (fr_io_failed_badly(slot)) {
        fr_recall_fallback(fr, run, slot->subject);
        return;
    }

    /* "4, 1, 9", or "None", or — because models decorate — any of those
     * with words around them. Every run of digits is a choice. */
    answer = fr_provider_text(fr, &fr->scratch, fr_io_answer(slot));
    while (at < answer.len && taken < keep &&
           run->relevant_count < FR_RELEVANT_MAX) {
        i64 number = 0;
        i32 digits = 0;

        while (at < answer.len &&
               (answer.data[at] < '0' || answer.data[at] > '9')) {
            ++at;
        }
        while (at < answer.len && answer.data[at] >= '0' &&
               answer.data[at] <= '9') {
            number = number * 10 + (answer.data[at] - '0');
            ++at;
            ++digits;
            if (number > FR_RERANK_NUMBER_MAX) break;
        }
        if (digits == 0) break;

        /* One-based into the list that was sent. Anything else is the
         * model having a bad day; skip it rather than recall the wrong
         * thing. */
        if (number < 1 || number > query->candidate_count) continue;
        fr_recall_add(run, query->candidates[number - 1],
                      fr_rank_strength(taken, keep), 1);
        ++taken;
    }

    /* It answered, but with nothing usable. The embedding order is a
     * better guess than no memories at all. */
    if (taken == 0) {
        fr_recall_fallback(fr, run, slot->subject);
    }
}

static b32 fr_recall_route(Friend_Context *fr, Fr_Recall *run,
                    const Fr_Recall_Spec *spec, Fr_Io_Slot *slot)
{
    Fr_Recall_Stage expected = slot->purpose == spec->embed_purpose
                                   ? FR_RECALL_EMBEDDING
                                   : FR_RECALL_RERANKING;

    if (slot->subject_event != run->serial || run->stage != expected) return 0;
    if (slot->subject < 0 || slot->subject >= FR_RECALL_QUERIES) return 0;

    if (expected == FR_RECALL_EMBEDDING) {
        fr_recall_embedded(fr, run, slot);
    } else {
        fr_recall_reranked(fr, run, slot);
    }
    if (run->pending > 0) run->pending -= 1;
    if (run->pending > 0) return 0;

    if (expected == FR_RECALL_EMBEDDING) {
        fr_recall_start_reranks(fr, run, spec);
    } else {
        run->stage = FR_RECALL_DONE;
    }
    return run->stage == FR_RECALL_DONE;
}
