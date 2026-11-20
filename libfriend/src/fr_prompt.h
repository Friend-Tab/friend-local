/* fr_prompt.h — the friend's personality, and the prompts around it.
 *
 * The personality is not configuration with a default; it is the thing
 * itself. These texts are the ones the service shipped as its last
 * personality revision (interiority2), embedded here verbatim so a friend
 * imported out of an export keeps talking the way it talked. A frontend
 * that wanted to change them would be building a different friend, which
 * is what prompt_override is for.
 *
 * Two kinds of change were unavoidable:
 *
 *   - The pendant. Four passages of the system prompt are about being a
 *     physical thing clipped to the user, and they are false for someone
 *     running this on a laptop with no pendant in the room. Those four
 *     passages are slots now, filled from one of two sets of words by
 *     config.embodied. Everything else is unchanged.
 *
 *   - The reranker. The service ranked recall candidates through Google's
 *     Discovery Engine, which needs a cloud project. Here a cheap model
 *     is asked to do the same job, with a prompt of our own.
 *
 * Every sentence a model is shown is written in fr_prompt.c. Nothing
 * here does I/O or allocates outside the arena it is handed.
 */

#ifndef FR_PROMPT_H
#define FR_PROMPT_H

#include "fr_state.h"

/* ------------------------------------------------------------------ *
 *  Templates
 *
 *  The texts carry {name} slots. An unknown slot is left as it was
 *  written rather than silently dropped, so a typo in an override shows
 *  up in the prompt instead of disappearing from it.
 * ------------------------------------------------------------------ */

typedef struct Fr_Slot {
    String name;
    String value;
} Fr_Slot;

static String fr_render(Arena *arena, String tmpl, const Fr_Slot *slots, i32 count);

/* ------------------------------------------------------------------ *
 *  A few-shot turn
 * ------------------------------------------------------------------ */

typedef struct Fr_Shot {
    b32    is_user;
    String text;
} Fr_Shot;

/* ------------------------------------------------------------------ *
 *  The prompts, built
 *
 *  Each of these renders one finished prompt into the arena. They take
 *  the context because every one of them needs the friend's name, the
 *  clock, or both, and because prompt_override is read from it.
 * ------------------------------------------------------------------ */

/* The system prompt behind a reply: who the friend is, what it
 * remembers, and what it currently wants. memory_timelines is what
 * fr_describe_timelines produced; recently_inserted is the tail of the
 * graph that recall did not already cover. */
static String fr_build_system_prompt(Friend_Context *fr, Arena *arena,
                              String memory_timelines,
                              String recently_inserted);

/* The system prompt behind a live session: the service's second main
 * prompt, the one for a friend that is being spoken to rather than
 * typed at. Frozen when the session opens, so the memory times in it
 * are absolute and it ends with the clock. */
static String fr_build_live_prompt(Friend_Context *fr, Arena *arena,
                            String memory_timelines,
                            String recently_inserted);

/* The conversation so far, seeded into a live session as history. Empty
 * when there is nothing to seed. */
static String fr_build_live_recap(Friend_Context *fr, Arena *arena);

/* One user turn as the model sees it: the service prefixed every message
 * with when it was sent, and told the friend to ignore the prefix. */
static String fr_build_user_turn(Friend_Context *fr, Arena *arena, String text,
                          u64 sent_unix_ms, b32 was_transcribed);

/* A summary of overheard audio, as a turn of the conversation. */
static String fr_build_ambient_turn(Friend_Context *fr, Arena *arena,
                             const Fr_Chat_Event *e);

/* A photo that cannot go to the model as pixels, said in words: what
 * was seen in it at the time, or that it is gone, or that it is only not
 * in hand right now. */
static void fr_append_unseen_photo(String_Builder *sb, String caption, b32 gone);

/* One line of the dialogue as recall and the relation pass are shown
 * it, stamped with a time address and optionally with how long ago. */
static void fr_append_dialog_line(String_Builder *sb, u64 now_unix_ms,
                           b32 relative_ago, u64 at_unix_ms,
                           i32 utc_offset_minutes, Fr_Turn_Kind kind,
                           String text);

/* The dialogue as the relation pass is shown it, stamped against now.
 * This is the service's formatDialogToString, which is why the wording is
 * not ours to improve. */
static String fr_render_dialog(Arena *arena, const Fr_Memory_Record *m,
                        u64 now_unix_ms, i32 utc_offset_minutes);

/* The extraction call: conversation in, "Event [a-b]: description" out. */
static String fr_build_extract_system(Friend_Context *fr, Arena *arena);
static String fr_build_extract_conversation(Friend_Context *fr, Arena *arena,
                                     const Fr_Chat_Event *events, i32 count);
static String fr_build_extract_turn(Arena *arena, String conversation_text,
                             String recall_description);
static i32    fr_extract_shots(const Fr_Shot **out);

/* The relation call: two memories and the dialogue behind each. */
static String fr_relate_system(void);
static i32    fr_relate_shots(const Fr_Shot **out);
static String fr_build_relate_turn(Arena *arena, String event_a, String dialog_a,
                            String event_b, String dialog_b);

/* The rerank call: ours, not the service's. */
static String fr_rerank_system(Arena *arena, i32 keep);
static String fr_build_rerank_turn(Arena *arena, String query, const String *choices,
                            i32 choice_count, i32 keep);

/* The interiority pass: the friend rewriting what it wants. */
static String fr_build_interiority_system(Friend_Context *fr, Arena *arena,
                                   String memories, String chat_history);
static String fr_interiority_turn(void);

/* Captions. Not the service's prompt — the service captioned photos
 * inside its own vision service and the export only kept the answers —
 * but written to produce the same thing those answers are: a plain,
 * specific description of what is in the photograph, in one or two
 * sentences, with no preamble. */
static String fr_caption_system(void);
static String fr_caption_turn(i32 photo_count);

#endif /* FR_PROMPT_H */
