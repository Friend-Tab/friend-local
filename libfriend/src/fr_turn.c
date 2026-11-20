/* fr_turn.c — the pump, and what a reply is made of.
 *
 * A reply is a recall run and then the reply itself, with model calls
 * between every step:
 *
 *   say -> embed the conversation -> rank what came back -> reply
 *
 * None of those stages blocks. friend_say starts the first one and
 * returns; friend_pending_io hands the frontend the requests to make;
 * friend_tick advances whatever has arrived since the last one. Every
 * piece of state the pipeline needs between ticks is in Fr_Reply_Turn,
 * at a fixed size, so starting a turn allocates nothing.
 *
 * The one surprising behaviour in here is deliberate. The service cut
 * the friend off mid-reply: from the second sentence on, every sentence
 * end had a coin-flip chance of being the end of the message. It is why
 * the friend answers in one or two sentences instead of a paragraph, and
 * it is reproduced rather than tidied up, because a friend who writes
 * paragraphs is a different friend.
 */

#include "fr_provider.h"
#include "fr_rank.h"
#include "fr_recall.h"
#include "fr_prompt.h"
#include "fr_memory.h"
#include "fr_voice.h"
#include "fr_live.h"

/* The service's numbers, from streamChatReply: hot, short, and barely
 * thinking. Temperature two sounds wrong and is not: it is what the
 * personality was tuned against. */
#define FR_REPLY_TEMPERATURE 2.0f
#define FR_REPLY_MAX_TOKENS 1024
#define FR_REPLY_THINKING 128

/* A caption is a sentence or two per photo. */
#define FR_CAPTION_MAX_TOKENS 512

/* ================================================================== *
 *  Messages
 * ================================================================== */

static void fr_turn_reset(Friend_Context *fr)
{
    fr->active_count      = 0;
    fr->total_event_count = 0;
    fr_scroll_reset(fr);
    memset(&fr->turn, 0, sizeof(fr->turn));
    fr->turn.reply_index = -1;
    fr->last_prompt      = str_nil;
    arena_reset(&fr->prompt_page);
}

static void fr_retire_active(Friend_Context *fr, i32 count)
{
    i32 i;

    if (count <= 0) return;
    if (count > fr->active_count) count = fr->active_count;

    for (i = 0; i < count; ++i) {
        /* The scrollback decides what it has room for and says so if
         * something had to go. Nothing here can fail in a way that
         * stops the conversation. */
        fr_scroll_push(fr, &fr->active_events[i]);
    }

    for (i = count; i < fr->active_count; ++i) {
        fr->active_events[i - count] = fr->active_events[i];
    }
    fr->active_count -= count;
}

static Fr_Chat_Event *fr_commit_event(Friend_Context *fr, Friend_Message_Kind kind,
                               String text, b32 was_transcribed)
{
    Fr_Chat_Event *e;
    String         kept;

    if (fr->active_count >= fr->active_capacity) {
        fr_retire_active(fr, 1 + fr->active_count - fr->active_capacity);
    }
    e = &fr->active_events[fr->active_count];
    memset(e, 0, sizeof(*e));

    kept = fr_truncate_utf8(text, FR_CHAT_TEXT_MAX);
    if (kept.len > 0) memcpy(e->text, kept.data, (size_t)kept.len);
    e->text_len         = (u16)kept.len;
    e->kind             = (u8)kind;
    e->was_transcribed  = was_transcribed ? 1 : 0;
    e->occurred_unix_ms = fr->now_unix_ms;
    e->index            = fr->total_event_count++;

    fr->active_count += 1;
    fr_mark_dirty(fr);
    return e;
}

/* Where the reply being filled in is now, or -1. */
static i32 fr_reply_position(Friend_Context *fr)
{
    if (fr->turn.reply_index < 0) return -1;
    return fr_active_find(fr, (u64)fr->turn.reply_index);
}

/* ================================================================== *
 *  Photographs in a request
 *
 *  A photo the friend has the bytes of goes to the model as bytes. One
 *  it does not — it was sent in another session and is sitting in the
 *  blob file, and a request cannot wait on a disk read — goes in words.
 * ================================================================== */

static void fr_attach_photos(Friend_Context *fr, Arena *arena,
                             const Fr_Chat_Event *e, Fr_Prompt_Message *out)
{
    String *images;
    i32     count = (i32)e->image_count;
    i32     i;
    i32     kept = 0;

    out->images      = 0;
    out->image_count = 0;
    if (count <= 0) return;

    images = arena_push(arena, String, (size_t)count);
    if (!images) return;

    for (i = 0; i < count; ++i) {
        String bytes = fr_blob_for_model(fr, &e->images[i]);
        if (str_is_empty(bytes)) continue;
        images[kept++] = bytes;
    }
    if (kept > 0) {
        out->images      = images;
        out->image_count = kept;
    }

    if (kept < count) {
        String_Builder sb = sb_begin(arena);
        sb_append(&sb, out->text);
        for (i = 0; i < count; ++i) {
            if (!str_is_empty(fr_blob_for_model(fr, &e->images[i]))) continue;
            fr_append_unseen_photo(&sb, fr_caption_at(fr_chat_captions(e), i),
                                   (e->images[i].flags & FR_IMAGE_PENDING) != 0);
        }
        out->text = sb_done(&sb);
    }
}

/* ================================================================== *
 *  Recall
 * ================================================================== */

static void fr_start_reply(Friend_Context *fr);

/* The service's two windows: the last three messages and the last ten.
 * One asks what this is about; the other what we are on about. */
static const i32 fr_reply_windows[FR_RECALL_QUERIES] = { 3, 10 };

static String fr_reply_query(Friend_Context *fr, Arena *arena, i32 q,
                             b32 for_rerank)
{
    fr_unused(for_rerank);
    return fr_recall_query(fr, arena, fr_reply_windows[q]);
}

static const Fr_Recall_Spec fr_reply_recall = {
    FR_IO_PURPOSE_EMBED_QUERY, FR_IO_PURPOSE_RERANK, FR_RECALL_QUERIES,
    fr_reply_query
};

static void fr_turn_fail(Friend_Context *fr, String why)
{
    i32 at = fr_reply_position(fr);

    /* Nothing is going to be said out loud either. */
    fr_voice_silence(fr);

    /* A reply that never arrived is not a message. The half-filled event
     * comes back out of the window so the conversation is not left with
     * a blank from the friend. */
    if (at >= 0) {
        i32 i;
        for (i = at + 1; i < fr->active_count; ++i) {
            fr->active_events[i - 1] = fr->active_events[i];
        }
        fr->active_count -= 1;
    }
    fr->turn.reply_index = -1;
    fr->turn.stage       = FR_STAGE_IDLE;
    fr_error(fr, FRIEND_ERR_PROVIDER, why);
    fr_push_event_text(fr, FRIEND_EVENT_ERROR, friend_last_error(fr));
}

/* ================================================================== *
 *  The reply
 * ================================================================== */

/* The prompt, kept where it can be read back after the request that
 * carried it has been released. */
static void fr_keep_prompt(Friend_Context *fr, String system,
                           const Fr_Prompt_Message *messages, i32 count)
{
    String_Builder sb;
    i32            i;

    arena_reset(&fr->prompt_page);
    sb = sb_begin(&fr->prompt_page);
    sb_append(&sb, str_lit("=== system ===\n"));
    sb_append(&sb, system);
    sb_append(&sb, str_lit("\n\n=== conversation ===\n"));
    for (i = 0; i < count; ++i) {
        sb_append(&sb, messages[i].is_user ? str_lit("[user] ")
                                           : str_lit("[friend] "));
        sb_append(&sb, messages[i].text);
        sb_append(&sb, str_lit("\n\n"));
    }
    if (sb.overflowed) {
        fr->last_prompt = str_lit("(the prompt was longer than the page kept "
                                  "for it; raise FR_PROMPT_PAGE_BYTES)");
        return;
    }
    fr->last_prompt = sb_done(&sb);
}

static void fr_start_reply(Friend_Context *fr)
{
    Arena_Mark         mark   = arena_mark(&fr->scratch);
    Fr_Recall         *recall = &fr->turn.recall;
    Fr_Recall_Text     recalled;
    String             system;
    Fr_Prompt_Message *messages;
    Fr_Io_Slot        *slot;
    Fr_Model_Call      call;
    Fr_Chat_Event     *reply;
    i32                count = 0;
    i32                i;

    recalled = fr_render_recall(fr, &fr->scratch, recall, 0,
                                fr->turn.recalled, FR_RECALLED_MAX);
    fr->turn.recalled_count = recalled.rendered_count;
    fr_record_memory_usage(fr, recall->relevant, recall->relevant_count,
                           fr->turn.recalled, fr->turn.recalled_count);

    slot = fr_io_acquire(fr, FR_IO_PURPOSE_REPLY, FRIEND_IO_HTTPS, 0);
    if (!slot) {
        arena_reset_to(&fr->scratch, mark);
        fr_turn_fail(fr, str_lit("no room to send the reply request: every "
                                 "I/O slot is in use"));
        return;
    }

    system = fr_build_system_prompt(fr, &slot->arena, recalled.described,
                                    recalled.recently);

    messages = arena_push(&slot->arena, Fr_Prompt_Message,
                          (size_t)fr->active_count + 1);
    if (!messages) {
        fr_io_release(fr, slot);
        arena_reset_to(&fr->scratch, mark);
        fr_turn_fail(fr, str_lit("the conversation did not fit in the "
                                 "request's share of the I/O budget"));
        return;
    }

    for (i = 0; i < fr->active_count; ++i) {
        const Fr_Chat_Event *e = &fr->active_events[i];
        Fr_Prompt_Message   *m = &messages[count];

        if (!fr_event_for_model(e)) continue;

        m->images      = 0;
        m->image_count = 0;
        m->is_user     = e->kind != FRIEND_MESSAGE_FRIEND;
        if (e->kind == FRIEND_MESSAGE_AMBIENT) {
            m->text = fr_build_ambient_turn(fr, &slot->arena, e);
        } else if (e->kind == FRIEND_MESSAGE_USER) {
            m->text = fr_build_user_turn(fr, &slot->arena, fr_chat_text(e),
                                         e->occurred_unix_ms,
                                         e->was_transcribed);
            fr_attach_photos(fr, &slot->arena, e, m);
        } else {
            m->text = fr_chat_text(e);
        }
        ++count;
    }

    memset(&call, 0, sizeof(call));
    call.model             = fr->models.chat;
    call.system            = system;
    call.messages          = messages;
    call.message_count     = count;
    call.max_output_tokens = FR_REPLY_MAX_TOKENS;
    call.thinking_budget   = FR_REPLY_THINKING;
    call.temperature       = FR_REPLY_TEMPERATURE;
    call.stream            = 1;

    fr_keep_prompt(fr, system, messages, count);

    if (!fr_io_post(fr, slot, fr_provider_chat(fr, &slot->arena, &call))) {
        arena_reset_to(&fr->scratch, mark);
        fr_turn_fail(fr, friend_last_error(fr));
        return;
    }
    arena_reset_to(&fr->scratch, mark);

    /* The friend's message joins the conversation now, empty and marked
     * as still arriving, so a frontend can draw it filling in. */
    reply = fr_commit_event(fr, FRIEND_MESSAGE_FRIEND, str_nil, 0);
    reply->streaming = 1;

    fr->turn.reply_index      = (i64)reply->index;
    fr->turn.stage            = FR_STAGE_REPLYING;
    fr->turn.punctuation_seen = 0;
    fr->turn.cut_short        = 0;
}

/* Would this character end a sentence? The guards are the service's: a
 * mark is not a sentence end when it is part of an ellipsis, when a
 * closing quote follows it, or when it is the dot in "vs." */
static b32 fr_is_sentence_end(String text, i32 at)
{
    char c = text.data[at];
    if (c != '.' && c != '!' && c != '?') return 0;
    if (at + 1 < text.len) {
        char next = text.data[at + 1];
        if (next == '"' || next == '.' || next == '\'') return 0;
    }
    if (at > 0 && text.data[at - 1] == '.') return 0;
    if (at > 1) {
        char a = text.data[at - 1];
        char b = text.data[at - 2];
        if ((a == 's' && b == 'v') || (a == 'S' && b == 'V')) return 0;
    }
    return 1;
}

static void fr_maybe_cut_reply(Friend_Context *fr, Fr_Chat_Event *reply,
                               i32 from)
{
    String text = fr_chat_text(reply);
    i32    i;

    for (i = from; i < text.len; ++i) {
        if (!fr_is_sentence_end(text, i)) continue;
        fr->turn.punctuation_seen += 1;
        if (fr->turn.punctuation_seen < 2) continue;
        if ((fr_random_u32(fr) & 0xFFFFu) >= 0x8000u) continue;

        /* Keep the mark, drop everything after it. */
        reply->text_len    = (u16)(i + 1);
        fr->turn.cut_short = 1;
        return;
    }
}

/* Appends whatever has arrived to the friend's message and tells the
 * frontend about it. Returns 1 when the provider says it is finished or
 * the reply was cut off. */
static b32 fr_drain_reply(Friend_Context *fr, Fr_Io_Slot *slot)
{
    i32            at = fr_reply_position(fr);
    Fr_Chat_Event *reply;
    Arena_Mark     mark;
    String_Builder sb;
    String         added;
    i32            consumed;
    b32            finished;
    i32            was;

    if (at < 0) return 1;
    reply = &fr->active_events[at];

    mark     = arena_mark(&fr->scratch);
    sb       = sb_begin(&fr->scratch);
    consumed = (i32)slot->consumed;
    finished = fr_provider_stream(fr, fr_io_answer(slot), &consumed, &sb);
    slot->consumed = (u32)consumed;
    added          = sb_done(&sb);

    was = reply->text_len;
    if (added.len > 0 && !fr->turn.cut_short) {
        i32 room = FR_CHAT_TEXT_MAX - reply->text_len;
        i32 take = added.len < room ? added.len : room;
        if (take > 0) {
            String delta;
            memcpy(reply->text + reply->text_len, added.data, (size_t)take);
            reply->text_len = (u16)(reply->text_len + take);
            fr_maybe_cut_reply(fr, reply, was);
            /* The delta points into the message itself, which outlives
             * every tick, so there is nothing to copy. */
            delta = str_make(reply->text + was, reply->text_len - was);
            fr_push_message_event(fr, FRIEND_EVENT_REPLY_DELTA, delta,
                                  reply->index);
            fr_voice_reply_text(fr, delta);
        }
    }
    arena_reset_to(&fr->scratch, mark);

    /* Once it has been cut off the rest of the stream is of no interest.
     * The transport is told so it can stop reading. */
    if (fr->turn.cut_short) slot->abandoned = 1;

    return finished || fr->turn.cut_short;
}

/* The reply is as finished as it is going to get: either the provider
 * said so, or it was cut off at a sentence end. */
static void fr_reply_succeeded(Friend_Context *fr)
{
    i32            at = fr_reply_position(fr);
    Fr_Chat_Event *reply;

    if (at < 0) {
        fr->turn.stage = FR_STAGE_IDLE;
        return;
    }
    reply = &fr->active_events[at];

    if (reply->text_len == 0) {
        fr_turn_fail(fr, str_lit("the model answered with no words at all"));
        return;
    }

    fr_voice_reply_done(fr);
    fr->turn.reply_index = -1;
    fr->turn.stage       = FR_STAGE_IDLE;
    fr_reply_finished(fr, reply);
}

static void fr_reply_finished(Friend_Context *fr, Fr_Chat_Event *reply)
{
    reply->streaming = 0;
    fr_push_message_event(fr, FRIEND_EVENT_REPLY_DONE, fr_chat_text(reply),
                          reply->index);
    fr_mark_dirty(fr);
    /* A reply is also the moment the window might have grown past the
     * point where the oldest of it should become memories. */
    fr_maybe_consolidate(fr);
}


static void fr_finish_reply(Friend_Context *fr, Fr_Io_Slot *slot)
{
    if (fr_io_failed_badly(slot)) {
        fr_turn_fail(fr, fr_provider_failure(fr, slot));
        return;
    }
    fr_reply_succeeded(fr);
}

/* ================================================================== *
 *  Photographs arriving
 * ================================================================== */

/* The bytes the caller handed us, copied into staging and given their
 * place in the blob file. What does not fit is described — there is
 * nothing to describe it with yet, but the count is right and the
 * message still says a photograph was sent. */
static void fr_take_photos(Friend_Context *fr, Fr_Chat_Event *event,
                           const Friend_Attachment *attachments, i32 count)
{
    i32 i;

    if (count > FR_CHAT_IMAGES_MAX) count = FR_CHAT_IMAGES_MAX;
    for (i = 0; i < count; ++i) {
        if (str_is_empty(attachments[i].jpeg)) continue;
        /* One copy: the friend was handed what it should show a model,
         * so there is no smaller one to keep beside it. */
        fr_blob_stage(fr, attachments[i].jpeg, str_nil,
                      &event->images[event->image_count]);
        event->image_count += 1;
    }
}

/* A photo nobody described is one the friend later remembers only as "a
 * photograph", and a vision model that is misconfigured would otherwise
 * never be noticed. */
static void fr_caption_failed(Friend_Context *fr, String why)
{
    fr_push_event_text(fr, FRIEND_EVENT_ERROR, why);
}

/* What a vision model makes of them, which is what is left of a photo
 * once its bytes are in a file somewhere and a prompt has to be built
 * without them. One request for the message, however many photos it
 * carries; the answer is one line each, which is the order they went
 * in. */
static void fr_start_caption(Friend_Context *fr, u64 event_index)
{
    i32                  at = fr_active_find(fr, event_index);
    const Fr_Chat_Event *event;
    Fr_Io_Slot          *slot;
    Fr_Model_Call        call;
    Fr_Prompt_Message    turn;
    String              *images;
    i32                  i;
    i32                  kept = 0;

    if (!fr->enable_images || str_is_empty(fr->models.vision)) return;
    if (at < 0) return;
    event = &fr->active_events[at];
    if (event->image_count == 0) return;

    slot = fr_io_acquire(fr, FR_IO_PURPOSE_CAPTION, FRIEND_IO_HTTPS, 0);
    if (!slot) {
        fr_caption_failed(fr, str_lit("no room to ask what is in the photo: "
                                      "every I/O slot is in use"));
        return;
    }
    slot->subject_event = event_index;

    images = arena_push(&slot->arena, String, (size_t)event->image_count);
    for (i = 0; images && i < (i32)event->image_count; ++i) {
        String bytes = fr_blob_for_model(fr, &event->images[i]);
        if (str_is_empty(bytes)) continue;
        images[kept++] = bytes;
    }
    if (kept == 0) {
        fr_io_release(fr, slot);
        return;
    }

    turn.is_user     = 1;
    turn.text        = fr_caption_turn(kept);
    turn.images      = images;
    turn.image_count = kept;

    memset(&call, 0, sizeof(call));
    call.model             = fr->models.vision;
    call.system            = fr_caption_system();
    call.messages          = &turn;
    call.message_count     = 1;
    call.max_output_tokens = FR_CAPTION_MAX_TOKENS;
    call.temperature       = -1.0f;

    if (!fr_io_post(fr, slot, fr_provider_chat(fr, &slot->arena, &call))) {
        fr_caption_failed(fr, friend_last_error(fr));
    }
}

/* One line per photo, in order. A model that answers with fewer lines
 * than there were photos leaves the rest uncaptioned, which is better
 * than pairing the wrong words with a picture. */
static void fr_finish_caption(Friend_Context *fr, Fr_Io_Slot *slot)
{
    i32            at = fr_active_find(fr, slot->subject_event);
    Fr_Chat_Event *event;
    String         rest, line;
    String         captions[FR_CHAT_IMAGES_MAX];
    i32            written = 0;

    if (fr_io_failed_badly(slot)) {
        fr_caption_failed(fr, fr_provider_failure(fr, slot));
        return;
    }
    if (at < 0) return;
    event = &fr->active_events[at];

    rest = fr_provider_text(fr, &fr->scratch, fr_io_answer(slot));
    while (written < (i32)event->image_count && str_next_line(&rest, &line)) {
        line = str_trim(line);
        if (line.len == 0) continue;
        /* Models like to number what they were asked to list. */
        if (line.len > 2 && line.data[0] >= '1' && line.data[0] <= '9' &&
            (line.data[1] == '.' || line.data[1] == ')')) {
            line = str_trim(str_skip(line, 2));
        }
        captions[written++] = line;
    }

    fr_set_captions(event, captions, written);
    fr_mark_dirty(fr);
}

/* ================================================================== *
 *  Saying things
 * ================================================================== */

Friend_Result friend_say(Friend_Context *fr, Friend_String text,
                         const Friend_Attachment *attachments, i32 count)
{
    return fr_say(fr, text, attachments, count, 0);
}

static Friend_Result fr_say(Friend_Context *fr, Friend_String text,
                     const Friend_Attachment *attachments, i32 count,
                     b32 was_spoken)
{
    Fr_Chat_Event *event;
    b32            spoken_reply;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    if (str_is_empty(str_trim(text)) && count <= 0) {
        return fr_error(fr, FRIEND_ERR_BAD_ARGUMENT,
                        str_lit("there is nothing to say"));
    }
    if (fr->turn.stage != FR_STAGE_IDLE) {
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("the friend is still answering the last "
                                "thing you said"));
    }
    if (str_is_empty(fr->api_key)) {
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("no API key is configured, so there is "
                                "nothing to ask"));
    }

    /* The service spoke its reply when the thing it was answering had
     * been spoken, and not otherwise — ShouldGetEmotiveResponse was set
     * for transcribed audio and nothing else. The configuration can ask
     * for typed replies to be spoken too. */
    spoken_reply = (was_spoken || fr->speak_replies) ? 1 : 0;

    event = fr_commit_event(fr, FRIEND_MESSAGE_USER, text, was_spoken);
    if (count > 0) fr_take_photos(fr, event, attachments, count);

    fr_voice_reply_begin(fr, spoken_reply && fr->enable_voice);
    fr_push_message_event(fr, FRIEND_EVENT_MESSAGE_COMMITTED, str_nil,
                          event->index);
    fr_push_event_text(fr, FRIEND_EVENT_THINKING, str_nil);
    if (count > 0) fr_start_caption(fr, event->index);

    memset(&fr->turn.recall, 0, sizeof(fr->turn.recall));
    fr->turn.recalled_count = 0;
    fr->turn.reply_index    = -1;
    fr->turn.stage          = FR_STAGE_RECALLING;

    /* A friend with nothing to remember has nothing to look up. */
    if (fr->memory_count == 0 ||
        !fr_recall_start(fr, &fr->turn.recall, &fr_reply_recall)) {
        fr_start_reply(fr);
    }
    return FRIEND_OK;
}

Friend_Result friend_retract_reply(Friend_Context *fr, float fraction_heard)
{
    Fr_Chat_Event *reply;
    i32            at;
    b32            arriving;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    at       = fr_reply_position(fr);
    arriving = at >= 0;
    if (!arriving) {
        /* Nothing is arriving. The last thing the friend said is what
         * the user is interrupting. */
        for (at = fr->active_count - 1; at >= 0; --at) {
            if (fr->active_events[at].kind == FRIEND_MESSAGE_FRIEND) break;
        }
        if (at < 0) {
            return fr_error(fr, FRIEND_ERR_BAD_STATE,
                            str_lit("the friend has not said anything to "
                                    "interrupt"));
        }
    }
    reply = &fr->active_events[at];

    if (fraction_heard < 0.0f) fraction_heard = 0.0f;
    if (fraction_heard > 1.0f) fraction_heard = 1.0f;
    reply->interrupted_fraction = fraction_heard;
    reply->streaming            = 0;

    if (arriving) {
        fr_io_abandon(fr, FR_IO_PURPOSE_REPLY);
        fr->turn.reply_index = -1;
        fr->turn.stage       = FR_STAGE_IDLE;
    }

    fr_mark_dirty(fr);
    fr_voice_silence(fr);
    fr_push_message_event(fr, FRIEND_EVENT_REPLY_RETRACTED, str_nil,
                          reply->index);
    return FRIEND_OK;
}

/* ================================================================== *
 *  The pump
 * ================================================================== */

static void fr_route_completion(Friend_Context *fr, Fr_Io_Slot *slot)
{
    switch (slot->purpose) {
    case FR_IO_PURPOSE_EMBED_QUERY:
    case FR_IO_PURPOSE_RERANK:
        if (fr->turn.stage == FR_STAGE_RECALLING &&
            fr_recall_route(fr, &fr->turn.recall, &fr_reply_recall, slot)) {
            fr_start_reply(fr);
        }
        break;

    case FR_IO_PURPOSE_REPLY:
        /* A reply that was already settled — cut off at a sentence end,
         * or retracted — is finished. Whatever the rest of the stream
         * said, and whatever status it ended on, is not news. */
        if (slot->abandoned || fr->turn.stage != FR_STAGE_REPLYING) break;
        fr_drain_reply(fr, slot);
        fr_finish_reply(fr, slot);
        break;

    case FR_IO_PURPOSE_CAPTION:
        fr_finish_caption(fr, slot);
        break;

    case FR_IO_PURPOSE_TRANSCRIBE:
    case FR_IO_PURPOSE_SPEAK:
        fr_voice_route(fr, slot);
        break;

    case FR_IO_PURPOSE_LIVE_SESSION:
        /* The open request is the socket: it is outstanding for as long
         * as the session lasts, so its completion is the session
         * ending, with the reason when the far end gave one. Sends and
         * the close complete on their own and say nothing. */
        if (slot->req.kind == FRIEND_IO_WS_OPEN) {
            fr_live_closed(fr, slot->req.id,
                           slot->failed ? slot->error : str_nil);
        }
        break;

    case FR_IO_PURPOSE_BLOB_READ:
    case FR_IO_PURPOSE_BLOB_WRITE:
        fr_blob_route(fr, slot);
        break;

    case FR_IO_PURPOSE_FOLD_EMBED_QUERY:
    case FR_IO_PURPOSE_FOLD_RERANK:
    case FR_IO_PURPOSE_EXTRACT_EVENTS:
    case FR_IO_PURPOSE_FOLD_EMBED:
    case FR_IO_PURPOSE_RELATE:
    case FR_IO_PURPOSE_INTERIORITY:
        fr_fold_route(fr, slot);
        break;

    case FR_IO_PURPOSE_EMBED_DOCUMENT:
        fr_backfill_route(fr, slot);
        break;

    case FR_IO_PURPOSE_NONE:
        break;
    }
}

Friend_Result friend_tick(Friend_Context *fr, u64 now_unix_ms)
{
    i32 i;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    arena_reset(&fr->scratch);
    fr->now_unix_ms = now_unix_ms;

    if (!fr->random_seeded) fr_random_seed(fr);

    /* Deltas first, so a reply that finished in the same tick it last
     * streamed into still reaches the frontend in order. */
    for (i = 0; i < fr->io_capacity; ++i) {
        Fr_Io_Slot *slot = &fr->io_slots[i];
        if (slot->state != FR_IO_STATE_OUTSTANDING) continue;
        if (slot->purpose != FR_IO_PURPOSE_REPLY) continue;
        if (slot->abandoned) continue;
        if (slot->recv_len <= slot->consumed) continue;
        /* Cut off at a sentence end: the message is finished even though
         * the request is not, and its eventual completion is ignored. */
        if (fr_drain_reply(fr, slot) && fr->turn.cut_short) {
            fr_reply_succeeded(fr);
        }
    }

    for (i = 0; i < fr->io_capacity; ++i) {
        Fr_Io_Slot *slot = &fr->io_slots[i];
        if (slot->state != FR_IO_STATE_DONE) continue;
        fr_route_completion(fr, slot);
        fr_io_release(fr, slot);
    }

    fr_live_tick(fr);

    /* Speech that is still arriving is handed on as it comes, so a
     * sentence starts playing before the request carrying it is done. */
    for (i = 0; i < fr->io_capacity; ++i) {
        Fr_Io_Slot *slot = &fr->io_slots[i];
        if (slot->state != FR_IO_STATE_OUTSTANDING) continue;
        if (slot->purpose != FR_IO_PURPOSE_SPEAK) continue;
        if (slot->abandoned) continue;
        if (!(slot->req.flags & FRIEND_IO_FLAG_STREAM)) continue;
        fr_voice_drain_speech(fr, slot);
    }
    fr_voice_tick(fr);

    /* Photographs that are waiting on the disk rather than on a model. */
    fr_blob_drain(fr);

    /* Nothing else to do: catch up on any memory that came across
     * without a vector we can compare. */
    if (fr->turn.stage == FR_STAGE_IDLE) fr_backfill_embeddings(fr);

    return FRIEND_OK;
}

/* The sooner of a wait and a retry that is due after now. */
static u32 fr_sooner(u32 wait, const Fr_Retry *retry, u64 now_unix_ms)
{
    u64 left;
    if (!fr_retry_waiting(retry, now_unix_ms)) return wait;
    left = retry->at - now_unix_ms;
    return left < (u64)wait ? (u32)left : wait;
}

u32 friend_next_deadline_ms(Friend_Context *fr)
{
    u32 wait;

    if (!fr) return 0xFFFFFFFFu;
    wait = fr_io_wait_ms(fr);
    /* A spoken turn is on a clock the network knows nothing about: the
     * speaker is still playing what has already arrived. */
    if (fr_voice_busy(fr) && wait > FR_IO_POLL_MS) wait = FR_IO_POLL_MS;
    /* Work that failed is tried again at a time nobody will announce. */
    wait = fr_sooner(wait, &fr->blob_write_retry, fr->now_unix_ms);
    wait = fr_sooner(wait, &fr->backfill_retry, fr->now_unix_ms);
    wait = fr_sooner(wait, &fr->image_read_retry, fr->now_unix_ms);
    return wait;
}

/* ================================================================== *
 *  The realtime session's socket
 * ================================================================== */

Friend_Result friend_ws_recv(Friend_Context *fr, u64 channel,
                             Friend_String message, u32 flags)
{
    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    if (flags & FRIEND_IO_FLAG_OPENED) {
        fr_live_opened(fr, channel);
        return FRIEND_OK;
    }
    fr_live_message(fr, channel, message);
    return FRIEND_OK;
}

Friend_Result friend_ws_closed(Friend_Context *fr, u64 channel)
{
    fr_unused(channel);
    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    return FRIEND_OK;
}
