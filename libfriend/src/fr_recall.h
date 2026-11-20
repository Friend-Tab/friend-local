/* fr_recall.h — what the friend remembers right now.
 *
 * Recall is four steps, and the shape of them is the service's:
 *
 *   1. Two windows over the conversation — the last 3 messages and the
 *      last 10 — are embedded as search queries. One asks "what is this
 *      about"; the other asks "what are we on about".
 *   2. Each query takes the 16 nearest memories by cosine distance over
 *      the int8 vectors.
 *   3. A model picks the 3 of those 16 that are worth having in mind.
 *   4. The three newest memories are added regardless, weighted so that
 *      something from five minutes ago outranks a rerank hit and
 *      something from last week does not.
 *
 * Then the graph is walked: every chain of connected memories that
 * passes through a recalled one is a candidate timeline, and the ten
 * with the least overlap go into the prompt. That is what makes this a
 * memory graph rather than a list — you remember the thing you were
 * asked about, and then you remember what happened around it.
 *
 * Nothing here does I/O. The embedding and the rerank are requests
 * fr_rank.c makes; these are the pure halves either side of them.
 */

#ifndef FR_RECALL_H
#define FR_RECALL_H

#include "fr_state.h"

/* ------------------------------------------------------------------ *
 *  Vectors
 * ------------------------------------------------------------------ */

/* round(v * 127), clamped. What the service stored, so an imported
 * friend's vectors stay comparable with ones we make. */
static void fr_quantize_embedding(const f32 *values, i32 count, i8 *out);

/* Cosine similarity over two int8 vectors. 0 when either is all zero. */
static f32 fr_cosine_i8(const i8 *a, const i8 *b, i32 count);

/* The k nearest memories to a query vector, best first. Memories still
 * waiting for an embedding are not candidates: they have no vector to
 * compare, and a staged memory being related to the graph must not find
 * itself. Returns how many were written. */
static i32 fr_nearest_memories(Friend_Context *fr, const i8 *query, i32 k,
                        u32 *out_slots, f32 *out_scores);

/* ------------------------------------------------------------------ *
 *  The conversation, as a query and as a prompt
 * ------------------------------------------------------------------ */

/* Which kind of dialogue turn a message is, when it is kept on a memory
 * or rendered as dialogue. Only for what fr_event_for_model allows. */
static Fr_Turn_Kind fr_event_turn_kind(const Fr_Chat_Event *e);

/* formatDialogToString over chat events: one line per turn, each stamped
 * with an absolute time address, and optionally with how long ago it
 * was. Only what fr_event_for_model allows. */
static void fr_append_chat_dialog(String_Builder *sb, Friend_Context *fr,
                           const Fr_Chat_Event *events, i32 count,
                           b32 relative_ago, i32 utc_offset_minutes);

/* The recall query for the last `messages_back` messages. Rendered in
 * UTC with no relative times, which makes it a pure function of the
 * conversation — the same window embeds to the same vector whenever it
 * is asked for, so it can be rebuilt rather than kept. */
static String fr_recall_query(Friend_Context *fr, Arena *arena, i32 messages_back);

/* "task: search result | query: ..." and "title: ... | text: ...", the
 * two halves of the asymmetric pair gemini-embedding-2 wants. */
static String fr_embed_query_text(Arena *arena, String conversation);
static String fr_embed_document_text(Arena *arena, String title, String text);

/* ------------------------------------------------------------------ *
 *  Timelines
 * ------------------------------------------------------------------ */

#define FR_TIMELINE_ELEMENTS_MAX 24
#define FR_TIMELINES_FOUND_MAX 256
#define FR_TIMELINE_PATHS_PER_MEMORY 48

typedef struct Fr_Timeline {
    u32 slots[FR_TIMELINE_ELEMENTS_MAX];
    u8  relations[FR_TIMELINE_ELEMENTS_MAX]; /* how each links to the next */
    i32 count;
    i32 most_relevant; /* index of the memory recall actually asked for */
    f32 match_strength;
} Fr_Timeline;

typedef struct Fr_Timelines {
    Fr_Timeline *at;
    i32          count;
} Fr_Timelines;

/* Walks the graph around each relevant memory and keeps the chains that
 * cover the most new ground, up to tuning.max_timelines. Built in the
 * arena you pass, which is expected to be scratch: the result is used
 * once, to render a prompt. */
static Fr_Timelines fr_build_timelines(Friend_Context *fr, Arena *arena,
                                const Fr_Relevant *relevant, i32 count);

/* The recall section of the system prompt: the memories recall asked
 * for, then every memory on every timeline in the order they happened.
 * Has a trailing newline, as the service's did. */
static String fr_describe_timelines(Friend_Context *fr, Arena *arena,
                             Fr_Timelines timelines, b32 absolute);

/* The tail of the graph that recall did not already cover, introduced
 * the way the service introduced it. Empty when recall covered it all. */
static String fr_recently_inserted(Friend_Context *fr, Arena *arena,
                            Fr_Timelines timelines, b32 absolute);

/* Every memory on them, once, in the order they first appear. focus, if
 * given, gets the ones recall asked for, under the same rule. */
static i32 fr_timeline_slots(Fr_Timelines timelines, u32 *out, i32 capacity,
                      u32 *focus, i32 *focus_count);

/* What a recall run found, as a prompt says it: the newest memories
 * are added to what the run chose, the graph is walked around all of it,
 * and the two recall sections of a system prompt come out, in the arena.
 * rendered, when given, gets every memory that reached the prompt.
 * absolute is as for fr_append_memory_string. */
typedef struct Fr_Recall_Text {
    String described; /* fr_describe_timelines */
    String recently;  /* fr_recently_inserted */
    i32    rendered_count;
} Fr_Recall_Text;

static Fr_Recall_Text fr_render_recall(Friend_Context *fr, Arena *arena,
                                Fr_Recall *run, b32 absolute, u32 *rendered,
                                i32 rendered_capacity);

/* "[3 days ago] description".
 *
 * `absolute` says it the other way: "[11/2/2026 11:26 PM]". A prompt
 * that is written once and then read for the next nine minutes cannot
 * say "ten minutes ago" and still be right at the end of them, so a
 * live session's memories are stamped with the clock instead. */
static void fr_append_memory_string(String_Builder *sb, Friend_Context *fr,
                             const Fr_Memory_Record *m, b32 absolute);

/* ------------------------------------------------------------------ *
 *  Scoring and usage
 * ------------------------------------------------------------------ */

/* The three newest memories, weighted by how recent: 0.7 at this moment,
 * a fifth of that a week later. Appended to the relevant list. */
static i32 fr_add_recent_memories(Friend_Context *fr, Fr_Relevant *out, i32 count,
                           i32 capacity);

/* The newest memories in the graph by creation time, newest first. The
 * graph is slot-ordered, not time-ordered, so this is a partial
 * selection rather than a sort. */
static i32 fr_newest_memories(Friend_Context *fr, i32 want, u32 *out);

/* Drops repeats, keeping the first mention — which is the stronger one,
 * because reranked memories are added before recent ones. */
static i32 fr_dedup_relevant(Fr_Relevant *at, i32 count);

/* Bumps use_count on the memories that were both chosen by the reranker
 * and rendered into the prompt, and counts the reply against the epoch.
 * Only those two together count as a use: a candidate that lost is not
 * evidence that the memory was worth keeping. */
static void fr_record_memory_usage(Friend_Context *fr, const Fr_Relevant *relevant,
                            i32 relevant_count, const u32 *rendered,
                            i32 rendered_count);

/* How many replies an epoch runs for: as many as there are memories,
 * with a floor, so a graph that grows mid-epoch extends its own epoch. */
static u64 fr_epoch_length(Friend_Context *fr);

/* A fresh usage epoch over every memory there is now. The first reply
 * opens the first; each prune pass opens the next. */
static void fr_epoch_open(Friend_Context *fr);

#endif /* FR_RECALL_H */
