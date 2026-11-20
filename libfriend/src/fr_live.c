/* fr_live.c — the realtime session: one audio model, listening and
 * talking.
 *
 * The other way of talking (fr_voice.c) hands a recording to a
 * transcriber, puts the words through the friend's own prompt and
 * model, and speaks the answer. This one holds a websocket open to a
 * model that does all three at once. It is faster — the gap between
 * your sentence and its first word is the model's, not three requests'
 * — and the prosody is a different thing entirely, because the model
 * hears how you said it rather than reading what you said.
 *
 * What it costs is that the friend is somewhere else for the duration.
 * The prompt goes up once, at the top, and the audio model runs the
 * conversation from there; the recall that would have happened per turn
 * happens once, at the start. The service ran it this way too, and
 * wrote a second system prompt for it — shorter sentences, no questions
 * out of habit, written for the ear. That prompt is in fr_prompt.c with
 * the other one.
 *
 * Two providers, the same session with different nouns:
 *
 *   Gemini Live   setup / clientContent / realtimeInput, and
 *                 serverContent coming back. 16 kHz in, 24 kHz out.
 *   OpenAI        session.update / conversation.item.create /
 *   realtime      input_audio_buffer.append, and a response.* event
 *                 for everything. 24 kHz both ways.
 *
 * Both are configured with their own speech detection switched OFF. The
 * friend already has one, it is the same one the other path uses, and
 * two detectors disagreeing about when a sentence ended is a friend
 * that talks over you.
 *
 * The turn is recorded into the conversation when it finishes — what
 * you said and what it said, from the transcripts both providers
 * return — so memory, consolidation and everything downstream work
 * exactly as they do for a typed message. That is the whole reason the
 * transcripts are asked for.
 *
 * The microphone and the speaker belong to fr_voice.c. This file tells
 * the voice what the session did and takes the microphone's audio from
 * it; it never decides what the voice is doing.
 */

#include "fr_live.h"
#include "fr_memory.h"
#include "fr_prompt.h"
#include "fr_provider.h"
#include "fr_recall.h"
#include "fr_voice.h"

/* What the OpenAI realtime API will accept and what it answers with:
 * the rate every provider speaks at, both ways, whatever the microphone
 * gave us. */
#define FR_LIVE_OPENAI_RATE FR_SPEAK_RATE

/* Gemini Live takes the microphone at FR_VOICE_RATE, as it is. */
#define FR_LIVE_QUOTE(x) #x
#define FR_LIVE_NUMBER(x) FR_LIVE_QUOTE(x)
#define FR_LIVE_GEMINI_AUDIO \
    "audio/pcm;rate=" FR_LIVE_NUMBER(FRIEND_VOICE_RATE)

/* How much microphone audio goes in one message. A tenth of a second
 * keeps the message rate at ten a second, which is nothing, and the
 * latency it adds is below what anybody can hear. A message carries up
 * to four of them when the socket has fallen behind. */
#define FR_LIVE_CHUNK (FR_VOICE_RATE / 10)
#define FR_LIVE_CHUNKS_PER_MESSAGE 4

/* One slot is the socket, for as long as it is open, and the rest of a
 * turn has to fit alongside it. Audio never takes the last two, so the
 * end of a turn is always sendable. */
#define FR_LIVE_SLOTS_MIN 6
#define FR_LIVE_SLOTS_KEPT 2

/* The service recycled a live session every two and a half minutes:
 * they get less stable the longer they run, as the context the model is
 * holding grows, and the gateway in front of them gave up at ten. Ours
 * has no gateway, but the first half of that is still true — and a
 * fresh session is also a fresh system prompt, with whatever the friend
 * has remembered since. */
#define FR_LIVE_RECYCLE_MS 150000u

/* Nothing at all from the far end for this long and the socket is
 * assumed dead rather than quiet. The providers send something during
 * any turn; between turns they are silent, so this is generous. */
#define FR_LIVE_QUIET_MS 90000u

/* ================================================================== *
 *  Sending
 * ================================================================== */

/* A message on the socket: built in its own slot's arena and gone once
 * the transport has sent it. */
static Fr_Io_Slot *fr_live_frame(Friend_Context *fr, Json_Writer *w)
{
    Fr_Io_Slot *slot;

    if (!fr->live.channel) return 0;
    slot = fr_io_acquire(fr, FR_IO_PURPOSE_LIVE_SESSION, FRIEND_IO_WS_SEND, 0);
    if (!slot) return 0;
    *w = jw_begin(&slot->arena);
    return slot;
}

static b32 fr_live_send(Friend_Context *fr, Fr_Io_Slot *slot, Json_Writer *w)
{
    Fr_Request req;

    memset(&req, 0, sizeof(req));
    req.channel = fr->live.channel;
    req.body    = jw_done(w);
    req.failed = w->failed || w->sb.overflowed || str_is_empty(req.body);
    return fr_io_post(fr, slot, req);
}

/* ------------------------------------------------------------------ *
 *  Gemini Live
 * ------------------------------------------------------------------ */

/* {"realtimeInput": {"activityStart": {}}} and its siblings. */
static b32 fr_live_gemini_activity(Friend_Context *fr, String which)
{
    Json_Writer w;
    Fr_Io_Slot *slot = fr_live_frame(fr, &w);

    if (!slot) return 0;
    jw_object_begin(&w);
    jw_key(&w, str_lit("realtimeInput"));
    jw_object_begin(&w);
    jw_key(&w, which);
    jw_object_begin(&w);
    jw_object_end(&w);
    jw_object_end(&w);
    jw_object_end(&w);
    fr_live_send(fr, slot, &w);
    return 1;
}

static void fr_live_gemini_setup(Friend_Context *fr, Json_Writer *w,
                                 String system)
{
    jw_key(w, str_lit("setup"));
    jw_object_begin(w);
    {
        /* In the scratch, not the slot the writer is open on. */
        String_Builder sb = sb_begin(&fr->scratch);
        sb_append(&sb, str_lit("models/"));
        sb_append(&sb, fr->models.live);
        jw_field_string(w, str_lit("model"), sb_done(&sb));
    }
    jw_key(w, str_lit("generationConfig"));
    jw_object_begin(w);
    /* No thinking. The audio models will happily write a paragraph of
     * "**Considering Weekend Plans**" to themselves before they say
     * anything, and it costs two seconds of silence while the person
     * who asked waits. */
    fr_gemini_thinking(w, 0);
    fr_gemini_speech(w, friend_voice(fr));
    jw_object_end(w);

    fr_gemini_system(w, system);

    jw_key(w, str_lit("realtimeInputConfig"));
    jw_object_begin(w);
    jw_key(w, str_lit("automaticActivityDetection"));
    jw_object_begin(w);
    jw_field_bool(w, str_lit("disabled"), 1);
    jw_object_end(w);
    jw_field_string(w, str_lit("turnCoverage"),
                    str_lit("TURN_INCLUDES_ONLY_ACTIVITY"));
    jw_object_end(w);

    jw_key(w, str_lit("inputAudioTranscription"));
    jw_object_begin(w);
    jw_object_end(w);
    jw_key(w, str_lit("outputAudioTranscription"));
    jw_object_begin(w);
    jw_object_end(w);
    jw_object_end(w);
}

static void fr_live_gemini_recap(Json_Writer *w, String recap)
{
    jw_key(w, str_lit("clientContent"));
    jw_object_begin(w);
    jw_key(w, str_lit("turns"));
    jw_array_begin(w);
    jw_object_begin(w);
    jw_field_string(w, str_lit("role"), str_lit("user"));
    fr_gemini_text_parts(w, recap);
    jw_object_end(w);
    jw_array_end(w);
    /* History, not a question: nothing is generated from this. */
    jw_field_bool(w, str_lit("turnComplete"), 0);
    jw_object_end(w);
}

static b32 fr_live_gemini_audio(Friend_Context *fr, const i16 *samples,
                                i32 count)
{
    Json_Writer w;
    Fr_Io_Slot *slot = fr_live_frame(fr, &w);

    if (!slot) return 0;
    jw_object_begin(&w);
    jw_key(&w, str_lit("realtimeInput"));
    jw_object_begin(&w);
    jw_key(&w, str_lit("audio"));
    jw_object_begin(&w);
    jw_field_string(&w, str_lit("mimeType"), str_lit(FR_LIVE_GEMINI_AUDIO));
    jw_key(&w, str_lit("data"));
    jw_base64(&w, (const u8 *)samples, (size_t)count * sizeof(i16));
    jw_object_end(&w);
    jw_object_end(&w);
    jw_object_end(&w);
    fr_live_send(fr, slot, &w);
    return 1;
}

static void fr_live_owe(Friend_Context *fr, Fr_Live_Frame frame);

static void fr_live_gemini_open_turn(Friend_Context *fr)
{
    fr_live_owe(fr, FR_LIVE_SEND_ACTIVITY_START);
}

static void fr_live_gemini_close_turn(Friend_Context *fr)
{
    fr_live_owe(fr, FR_LIVE_SEND_ACTIVITY_END);
}

/* Gemini's interrupt is opening an activity and not closing it. */
static void fr_live_gemini_interrupt(Friend_Context *fr)
{
    fr_live_owe(fr, FR_LIVE_SEND_ACTIVITY_START);
    fr->live.activity = 1;
}

/* ------------------------------------------------------------------ *
 *  OpenAI realtime
 * ------------------------------------------------------------------ */

/* {"type": ...}: an event with no payload, which is most of them. */
static b32 fr_live_openai_event(Friend_Context *fr, String type)
{
    Json_Writer w;
    Fr_Io_Slot *slot = fr_live_frame(fr, &w);

    if (!slot) return 0;
    jw_object_begin(&w);
    jw_field_string(&w, str_lit("type"), type);
    jw_object_end(&w);
    fr_live_send(fr, slot, &w);
    return 1;
}

static void fr_live_openai_format(Json_Writer *w)
{
    jw_key(w, str_lit("format"));
    jw_object_begin(w);
    jw_field_string(w, str_lit("type"), str_lit("audio/pcm"));
    jw_field_i64(w, str_lit("rate"), FR_LIVE_OPENAI_RATE);
    jw_object_end(w);
}

static void fr_live_openai_setup(Friend_Context *fr, Json_Writer *w,
                                 String system)
{
    jw_field_string(w, str_lit("type"), str_lit("session.update"));
    jw_key(w, str_lit("session"));
    jw_object_begin(w);
    jw_field_string(w, str_lit("type"), str_lit("realtime"));
    jw_field_string(w, str_lit("instructions"), system);
    jw_key(w, str_lit("output_modalities"));
    jw_array_begin(w);
    jw_string(w, str_lit("audio"));
    jw_array_end(w);
    /* The service pinned the reasoning effort rather than leaving a
     * voice turn's latency to a server default that can move. */
    jw_key(w, str_lit("reasoning"));
    jw_object_begin(w);
    jw_field_string(w, str_lit("effort"), str_lit("low"));
    jw_object_end(w);

    jw_key(w, str_lit("audio"));
    jw_object_begin(w);
    jw_key(w, str_lit("input"));
    jw_object_begin(w);
    fr_live_openai_format(w);
    jw_key(w, str_lit("turn_detection"));
    jw_null(w);
    jw_key(w, str_lit("transcription"));
    jw_object_begin(w);
    jw_field_string(w, str_lit("model"), fr->models.transcribe);
    jw_object_end(w);
    jw_object_end(w);

    jw_key(w, str_lit("output"));
    jw_object_begin(w);
    fr_live_openai_format(w);
    jw_field_string(w, str_lit("voice"), friend_voice(fr));
    jw_object_end(w);
    jw_object_end(w);
    jw_object_end(w);
}

static void fr_live_openai_recap(Json_Writer *w, String recap)
{
    jw_field_string(w, str_lit("type"), str_lit("conversation.item.create"));
    jw_key(w, str_lit("item"));
    jw_object_begin(w);
    jw_field_string(w, str_lit("type"), str_lit("message"));
    jw_field_string(w, str_lit("role"), str_lit("user"));
    jw_key(w, str_lit("content"));
    jw_array_begin(w);
    jw_object_begin(w);
    jw_field_string(w, str_lit("type"), str_lit("input_text"));
    jw_field_string(w, str_lit("text"), recap);
    jw_object_end(w);
    jw_array_end(w);
    jw_object_end(w);
}

/* 16 kHz into 24: three samples where there were two. The OpenAI
 * realtime API refuses anything else, and refuses it at setup rather
 * than politely resampling. */
static i32 fr_live_upsample(const i16 *in, i32 count, i16 *out)
{
    i32 written = 0;
    i32 i;

    for (i = 0; i < count; ++i) {
        i32 a = in[i];
        i32 b = (i + 1 < count) ? in[i + 1] : in[i];
        out[written++] = (i16)a;
        if ((i & 1) == 0) {
            out[written++] = (i16)((a + b) / 2);
        }
    }
    return written;
}

static b32 fr_live_openai_audio(Friend_Context *fr, const i16 *samples,
                                i32 count)
{
    Arena_Mark  mark = arena_mark(&fr->scratch);
    i16        *up   = arena_push(&fr->scratch, i16, (size_t)count * 2);
    Json_Writer w;
    Fr_Io_Slot *slot;
    i32         up_count;

    if (!up) return 0;
    up_count = fr_live_upsample(samples, count, up);
    slot     = fr_live_frame(fr, &w);
    if (slot) {
        jw_object_begin(&w);
        jw_field_string(&w, str_lit("type"),
                        str_lit("input_audio_buffer.append"));
        jw_key(&w, str_lit("audio"));
        jw_base64(&w, (const u8 *)up, (size_t)up_count * sizeof(i16));
        jw_object_end(&w);
        fr_live_send(fr, slot, &w);
    }
    arena_reset_to(&fr->scratch, mark);
    return slot != 0;
}

/* The OpenAI shape has no activity to open; starting to talk over a
 * reply is a cancel and nothing else. */
static void fr_live_openai_open_turn(Friend_Context *fr)
{
    if (fr->live.responding) fr_live_owe(fr, FR_LIVE_SEND_CANCEL);
}

/* Manual turns: the buffer has to be committed and the response asked
 * for, because nothing at the far end is listening for a silence. */
static void fr_live_openai_close_turn(Friend_Context *fr)
{
    fr_live_owe(fr, FR_LIVE_SEND_COMMIT);
    fr_live_owe(fr, FR_LIVE_SEND_CREATE);
}

static void fr_live_openai_interrupt(Friend_Context *fr)
{
    fr_live_owe(fr, FR_LIVE_SEND_CANCEL);
}

/* ------------------------------------------------------------------ *
 *  Which
 * ------------------------------------------------------------------ */

static void fr_live_gemini_message(Friend_Context *fr, String message);
static void fr_live_openai_message(Friend_Context *fr, String message);

typedef struct Fr_Live_Wire {
    void (*setup)(Friend_Context *fr, Json_Writer *w, String system);
    void (*recap)(Json_Writer *w, String recap);
    b32 (*audio)(Friend_Context *fr, const i16 *samples, i32 count);
    void (*open_turn)(Friend_Context *fr);
    void (*close_turn)(Friend_Context *fr);
    void (*interrupt)(Friend_Context *fr);
    void (*message)(Friend_Context *fr, String message);
} Fr_Live_Wire;

static const Fr_Live_Wire fr_live_gemini = {
    fr_live_gemini_setup,      fr_live_gemini_recap,
    fr_live_gemini_audio,      fr_live_gemini_open_turn,
    fr_live_gemini_close_turn, fr_live_gemini_interrupt,
    fr_live_gemini_message
};

static const Fr_Live_Wire fr_live_openai = {
    fr_live_openai_setup,      fr_live_openai_recap,
    fr_live_openai_audio,      fr_live_openai_open_turn,
    fr_live_openai_close_turn, fr_live_openai_interrupt,
    fr_live_openai_message
};

static const Fr_Live_Wire *fr_live_wire(Friend_Context *fr)
{
    return fr->provider == FRIEND_PROVIDER_GEMINI ? &fr_live_gemini
                                                  : &fr_live_openai;
}

/* ================================================================== *
 *  Opening
 * ================================================================== */

/* The system prompt, with the recall that would otherwise happen once a
 * turn done once, here. It is frozen for the length of the session,
 * which is what the recycle is for.
 *
 * The recall is the shorter half of the real one. The service embedded
 * the conversation, took sixteen memories, reranked them to three and
 * added the newest three regardless, then walked the graph around all
 * of it — and did that before the session opened, because a hosted
 * bridge could afford three round trips while the pendant waited. This
 * takes the newest memories and walks the same graph around them. The
 * walk is most of what matters: a recent memory pulls in the chain it
 * belongs to, which is how a timeline from last month arrives under a
 * conversation from this morning. */
static String fr_live_system(Friend_Context *fr, Arena *arena)
{
    Arena_Mark     mark = arena_mark(&fr->scratch);
    Fr_Recall      none;
    Fr_Recall_Text recalled;
    String         system;

    memset(&none, 0, sizeof(none));
    recalled = fr_render_recall(fr, &fr->scratch, &none, 1, 0, 0);
    system   = fr_build_live_prompt(fr, arena, recalled.described,
                                    recalled.recently);
    arena_reset_to(&fr->scratch, mark);
    return system;
}

/* The setup, or the recap: 0 when there was no slot to say it in. The
 * text goes into the scratch first, because a writer owns the tail of
 * the arena it builds in and these are long. */
static b32 fr_live_send_opening(Friend_Context *fr, b32 recap)
{
    Json_Writer w;
    Arena_Mark  mark = arena_mark(&fr->scratch);
    String      text = recap ? fr_build_live_recap(fr, &fr->scratch)
                             : fr_live_system(fr, &fr->scratch);
    Fr_Io_Slot *slot;

    if (recap && str_is_empty(text)) {
        arena_reset_to(&fr->scratch, mark);
        return 1; /* nothing to seed */
    }
    slot = fr_live_frame(fr, &w);
    if (slot) {
        jw_object_begin(&w);
        if (recap) {
            fr_live_wire(fr)->recap(&w, text);
        } else {
            fr_live_wire(fr)->setup(fr, &w, text);
        }
        jw_object_end(&w);
        fr_live_send(fr, slot, &w);
    }
    arena_reset_to(&fr->scratch, mark);
    return slot != 0;
}

static b32 fr_live_send_frame(Friend_Context *fr, Fr_Live_Frame frame)
{
    switch (frame) {
    case FR_LIVE_SEND_SETUP:
        return fr_live_send_opening(fr, 0);
    case FR_LIVE_SEND_RECAP:
        return fr_live_send_opening(fr, 1);
    case FR_LIVE_SEND_ACTIVITY_START:
        return fr_live_gemini_activity(fr, str_lit("activityStart"));
    case FR_LIVE_SEND_ACTIVITY_END:
        return fr_live_gemini_activity(fr, str_lit("activityEnd"));
    case FR_LIVE_SEND_CANCEL:
        return fr_live_openai_event(fr, str_lit("response.cancel"));
    case FR_LIVE_SEND_COMMIT:
        return fr_live_openai_event(fr,
                                    str_lit("input_audio_buffer.commit"));
    case FR_LIVE_SEND_CREATE:
        return fr_live_openai_event(fr, str_lit("response.create"));
    }
    return 1;
}

/* Whatever is owed, in order, for as long as there are slots. */
static void fr_live_pay(Friend_Context *fr)
{
    Fr_Live *live = &fr->live;
    while (live->owed_count > 0 &&
           fr_live_send_frame(fr, (Fr_Live_Frame)live->owed[0])) {
        live->owed_count -= 1;
        memmove(live->owed, live->owed + 1, (size_t)live->owed_count);
    }
}

static void fr_live_owe(Friend_Context *fr, Fr_Live_Frame frame)
{
    Fr_Live *live = &fr->live;
    if (live->owed_count >= FR_LIVE_OWED_MAX) {
        /* The transport has not taken a frame in eight: the session is
         * not going anywhere, and it is closed rather than left half
         * steered. */
        fr_push_event_text(fr, FRIEND_EVENT_ERROR,
                           str_lit("the realtime session stopped taking "
                                   "messages and was closed"));
        fr_live_end(fr);
        fr_voice_live_reply_over(fr);
        return;
    }
    live->owed[live->owed_count++] = (u8)frame;
    fr_live_pay(fr);
}

/* The provider said yes: the conversation so far goes in, and the
 * microphone means something now. */
static void fr_live_ready(Friend_Context *fr)
{
    if (fr->live.stage != FR_LIVE_SETUP) return;
    fr->live.stage = FR_LIVE_READY;
    fr_live_owe(fr, FR_LIVE_SEND_RECAP);
    fr_voice_live_ready(fr);
}

static b32 fr_live_begin(Friend_Context *fr)
{
    Fr_Io_Slot *slot;

    if (fr->live.stage != FR_LIVE_OFF) return 1;
    if (fr->io_capacity < FR_LIVE_SLOTS_MIN) {
        fr_error(fr, FRIEND_ERR_TOO_SMALL,
                 str_lit("a realtime session needs at least "
                         FR_LIVE_NUMBER(FR_LIVE_SLOTS_MIN)
                         " in-flight requests: raise "
                         "limits.max_inflight_io"));
        return 0;
    }

    slot = fr_io_acquire(fr, FR_IO_PURPOSE_LIVE_SESSION, FRIEND_IO_WS_OPEN, 0);
    if (!slot) {
        fr_error(fr, FRIEND_ERR_FULL,
                 str_lit("there is no room to open a session"));
        return 0;
    }
    if (!fr_io_post(fr, slot, fr_provider_live_open(fr, &slot->arena))) {
        return 0;
    }

    memset(&fr->live, 0, sizeof(fr->live));
    fr->live.stage          = FR_LIVE_OPENING;
    fr->live.channel        = slot->req.id;
    fr->live.opened_unix_ms = fr->now_unix_ms;
    fr->live.heard_unix_ms  = fr->now_unix_ms;
    return 1;
}

static void fr_live_end(Friend_Context *fr)
{
    Fr_Io_Slot     *slot;
    Fr_Request close;

    if (fr->live.stage == FR_LIVE_OFF) return;
    slot = fr_io_acquire(fr, FR_IO_PURPOSE_LIVE_SESSION, FRIEND_IO_WS_CLOSE,
                         0);
    if (slot) {
        memset(&close, 0, sizeof(close));
        close.channel = fr->live.channel;
        fr_io_post(fr, slot, close);
    } else {
        /* No slot to say it in. The socket's own request is given up on
         * instead, which is the other way a transport is told to let a
         * connection go. */
        fr_io_abandon(fr, FR_IO_PURPOSE_LIVE_SESSION);
    }
    memset(&fr->live, 0, sizeof(fr->live));
}

static b32 fr_live_running(Friend_Context *fr)
{
    return fr->live.stage != FR_LIVE_OFF;
}

/* ================================================================== *
 *  The turn
 * ================================================================== */

static void fr_live_turn_begin(Friend_Context *fr)
{
    if (fr->live.stage != FR_LIVE_READY) return;
    if (fr->live.activity) return;
    fr->live.activity     = 1;
    fr->live.sent_through = 0;
    fr_live_wire(fr)->open_turn(fr);
}

/* Whatever the microphone has that the socket has not. */
static void fr_live_flush_audio(Friend_Context *fr, b32 all)
{
    Fr_Live *live = &fr->live;

    if (live->stage != FR_LIVE_READY || !live->activity) return;
    /* Audio goes after whatever was owed before it. */
    if (live->owed_count > 0) return;
    /* Audio is the one thing here that can wait a tick, so it never
     * takes the last slots: the end of a turn has to be sendable even
     * when the transport is behind, or the session sits there with an
     * activity open and nobody talking. */
    if (fr->io_inflight + FR_LIVE_SLOTS_KEPT >= fr->io_capacity) return;
    for (;;) {
        const i16 *samples;
        i32        have = fr_voice_heard_since(fr, live->sent_through,
                                               &samples);
        i32        take;
        if (have <= 0) return;
        if (!all && have < FR_LIVE_CHUNK) return;
        take = have < FR_LIVE_CHUNK * FR_LIVE_CHUNKS_PER_MESSAGE
                   ? have
                   : FR_LIVE_CHUNK * FR_LIVE_CHUNKS_PER_MESSAGE;
        if (!fr_live_wire(fr)->audio(fr, samples, take)) return;
        live->sent_through += take;
    }
}

static void fr_live_turn_end(Friend_Context *fr)
{
    if (fr->live.stage != FR_LIVE_READY || !fr->live.activity) return;
    fr_live_flush_audio(fr, 1);
    fr_live_wire(fr)->close_turn(fr);
    fr->live.activity     = 0;
    fr->live.sent_through = 0;
    fr->live.responding   = 1;
    fr_push_event_text(fr, FRIEND_EVENT_THINKING, str_nil);
}

static void fr_live_interrupt(Friend_Context *fr)
{
    if (fr->live.stage != FR_LIVE_READY) return;
    if (!fr->live.responding) return;
    fr_live_wire(fr)->interrupt(fr);
    fr->live.responding = 0;
    fr->live.said_len   = 0;
    fr_push_event_text(fr, FRIEND_EVENT_REPLY_RETRACTED, str_nil);
}

/* ================================================================== *
 *  What came back
 * ================================================================== */

static void fr_live_keep(char *dest, i32 *len, i32 capacity, String text)
{
    i32 take = text.len;
    if (take > capacity - *len) take = capacity - *len;
    if (take <= 0) return;
    memcpy(dest + *len, text.data, (size_t)take);
    *len += take;
}

static void fr_live_heard(Friend_Context *fr, String text)
{
    fr_live_keep(fr->live.heard, &fr->live.heard_len,
                 (i32)sizeof(fr->live.heard), text);
}

static void fr_live_said(Friend_Context *fr, String text)
{
    fr_live_keep(fr->live.said, &fr->live.said_len,
                 (i32)sizeof(fr->live.said), text);
}

/* Samples out of a base64 string, into the scratch, where every other
 * view this library hands out also lives. Sized from the string: a
 * reply arrives as dozens of these in one tick, and taking a guess at
 * the room each one needs empties the arena halfway through the
 * sentence. */
static void fr_live_decode_audio(Friend_Context *fr, String base64, i32 rate)
{
    size_t room = fr_base64_decoded_size(base64);
    i16   *pcm;
    i64    written;

    if (room < sizeof(i16)) return;
    pcm = arena_push(&fr->scratch, i16, room / sizeof(i16) + 1);
    if (!pcm) return;
    written = fr_base64_decode(base64, (u8 *)pcm, room);
    if (written < (i64)sizeof(i16)) return;

    fr->live.responding = 1;
    fr_voice_play(fr, pcm, (i32)(written / (i64)sizeof(i16)), rate);
}

/* The turn is over: what was said, on both sides, becomes two messages
 * in the conversation like any other. A realtime session's turn is over
 * before the library hears about it — the model did the listening, the
 * answering and the talking — so there is nothing to start here, only
 * to record, and everything downstream then runs as it does for a typed
 * message. */
static void fr_live_record_turn(Friend_Context *fr)
{
    Fr_Live *live  = &fr->live;
    String   heard = str_trim(str_make(live->heard, live->heard_len));
    String   said  = str_trim(str_make(live->said, live->said_len));

    if (!str_is_empty(heard)) {
        Fr_Chat_Event *event;
        fr_push_event_text(fr, FRIEND_EVENT_TRANSCRIPT, heard);
        event = fr_commit_event(fr, FRIEND_MESSAGE_USER, heard, 1);
        fr_push_message_event(fr, FRIEND_EVENT_MESSAGE_COMMITTED, str_nil,
                              event->index);
    }
    if (!str_is_empty(said)) {
        Fr_Chat_Event *reply =
            fr_commit_event(fr, FRIEND_MESSAGE_FRIEND, said, 0);
        fr_push_message_event(fr, FRIEND_EVENT_MESSAGE_COMMITTED, str_nil,
                              reply->index);
        /* The prompt the session opened with was in front of the model
         * again for this answer, which is what a reply is counted by.
         * Nothing in it was a reranker's choice, so no memory's use
         * count moves. */
        fr_record_memory_usage(fr, 0, 0, 0, 0);
        fr_reply_finished(fr, reply);
    } else if (!str_is_empty(heard)) {
        fr_maybe_consolidate(fr);
    }
    live->heard_len  = 0;
    live->said_len   = 0;
    live->responding = 0;
    fr_voice_live_reply_over(fr);
}

static void fr_live_gemini_content(Friend_Context *fr, Json_Parser *p)
{
    String key;

    if (!json_object_begin(p)) return;
    while (json_object_next(p, &key)) {
        if (str_eq(key, str_lit("modelTurn"))) {
            String turn_key;
            if (!json_object_begin(p)) break;
            while (json_object_next(p, &turn_key)) {
                if (!str_eq(turn_key, str_lit("parts"))) {
                    json_skip(p);
                    continue;
                }
                if (!json_array_begin(p)) break;
                while (json_array_next(p)) {
                    String part_key;
                    if (!json_object_begin(p)) break;
                    while (json_object_next(p, &part_key)) {
                        String data_key;
                        if (!str_eq(part_key, str_lit("inlineData"))) {
                            json_skip(p);
                            continue;
                        }
                        if (!json_object_begin(p)) break;
                        while (json_object_next(p, &data_key)) {
                            if (str_eq(data_key, str_lit("data"))) {
                                fr_live_decode_audio(fr, json_string_raw(p),
                                                     FR_SPEAK_RATE);
                            } else {
                                json_skip(p);
                            }
                        }
                    }
                }
            }
        } else if (str_eq(key, str_lit("inputTranscription")) ||
                   str_eq(key, str_lit("outputTranscription"))) {
            b32    input = str_eq(key, str_lit("inputTranscription"));
            String inner;
            if (!json_object_begin(p)) break;
            while (json_object_next(p, &inner)) {
                String text;
                if (!str_eq(inner, str_lit("text"))) {
                    json_skip(p);
                    continue;
                }
                text = json_string(p, &fr->scratch);
                if (input) {
                    fr_live_heard(fr, text);
                } else {
                    fr_live_said(fr, text);
                }
            }
        } else if (str_eq(key, str_lit("interrupted"))) {
            if (json_bool(p)) {
                fr->live.responding = 0;
                fr_push_event_text(fr, FRIEND_EVENT_REPLY_RETRACTED, str_nil);
            }
        } else if (str_eq(key, str_lit("turnComplete"))) {
            if (json_bool(p)) fr_live_record_turn(fr);
        } else {
            json_skip(p);
        }
    }
}

static void fr_live_gemini_message(Friend_Context *fr, String message)
{
    Json_Parser p = json_begin(message);
    String      key;

    if (!json_object_begin(&p)) return;
    while (json_object_next(&p, &key)) {
        if (str_eq(key, str_lit("setupComplete"))) {
            json_skip(&p);
            fr_live_ready(fr);
        } else if (str_eq(key, str_lit("serverContent"))) {
            fr_live_gemini_content(fr, &p);
        } else if (str_eq(key, str_lit("goAway"))) {
            json_skip(&p);
            /* The far end is about to hang up; start again at the next
             * quiet moment rather than in the middle of a sentence. */
            fr->live.recycle = 1;
        } else {
            json_skip(&p);
        }
    }
}

/* One object, at most one level of nesting that matters. */
static String fr_live_inner_string(Json_Parser *p, Arena *arena, String want)
{
    String inner;
    String found = str_nil;

    if (!json_object_begin(p)) return str_nil;
    while (json_object_next(p, &inner)) {
        if (str_eq(inner, want)) {
            found = json_string(p, arena);
        } else {
            json_skip(p);
        }
    }
    return found;
}

static void fr_live_openai_message(Friend_Context *fr, String message)
{
    Json_Parser p = json_begin(message);
    String      key;
    String      type       = str_nil;
    String      delta      = str_nil;
    String      transcript = str_nil;
    String      status     = str_nil;
    String      error      = str_nil;

    if (!json_object_begin(&p)) return;
    while (json_object_next(&p, &key)) {
        if (str_eq(key, str_lit("type"))) {
            type = json_string(&p, &fr->scratch);
        } else if (str_eq(key, str_lit("delta"))) {
            /* Either base64 audio or a piece of transcript; which one
             * the type says, which may not have arrived yet. Base64 has
             * nothing to unescape, so reading it as text costs
             * nothing. */
            delta = json_string(&p, &fr->scratch);
        } else if (str_eq(key, str_lit("transcript"))) {
            transcript = json_string(&p, &fr->scratch);
        } else if (str_eq(key, str_lit("response"))) {
            status = fr_live_inner_string(&p, &fr->scratch, str_lit("status"));
        } else if (str_eq(key, str_lit("error"))) {
            error = fr_live_inner_string(&p, &fr->scratch, str_lit("message"));
        } else {
            json_skip(&p);
        }
    }
    if (p.failed) return;

    if (str_eq(type, str_lit("session.updated"))) {
        fr_live_ready(fr);
    } else if (str_eq(type, str_lit("response.output_audio.delta"))) {
        fr_live_decode_audio(fr, delta, FR_LIVE_OPENAI_RATE);
    } else if (str_eq(type,
                      str_lit("response.output_audio_transcript.delta"))) {
        fr_live_said(fr, delta);
    } else if (str_eq(type,
                      str_lit("conversation.item.input_audio_transcription."
                              "completed"))) {
        fr->live.heard_len = 0;
        fr_live_heard(fr, transcript);
    } else if (str_eq(type, str_lit("response.done"))) {
        /* Only a finished response is a turn. A cancelled one was the
         * user cutting in, and committing it would write half a
         * sentence into the conversation as if it had been said. */
        if (str_is_empty(status) || str_eq(status, str_lit("completed"))) {
            fr_live_record_turn(fr);
        } else {
            fr->live.said_len   = 0;
            fr->live.responding = 0;
        }
    } else if (str_eq(type, str_lit("error")) && !str_is_empty(error)) {
        fr_push_event_text(fr, FRIEND_EVENT_ERROR, error);
    }
}

/* ================================================================== *
 *  From the pump
 * ================================================================== */

static void fr_live_opened(Friend_Context *fr, u64 channel)
{
    if (fr->live.channel != channel) return;
    fr->live.heard_unix_ms = fr->now_unix_ms;
    if (fr->live.stage != FR_LIVE_OPENING) return;
    /* Said to be sent from here on: the answer to it cannot come before
     * it goes. */
    fr->live.stage = FR_LIVE_SETUP;
    fr_live_owe(fr, FR_LIVE_SEND_SETUP);
}

static void fr_live_message(Friend_Context *fr, u64 channel, String message)
{
    if (fr->live.channel != channel) return;
    fr->live.heard_unix_ms = fr->now_unix_ms;
    if (str_is_empty(message)) return;
    fr_live_wire(fr)->message(fr, message);
}

static void fr_live_closed(Friend_Context *fr, u64 channel, String why)
{
    b32 was_open;

    if (fr->live.channel != channel) return;
    was_open = fr->live.stage == FR_LIVE_READY;

    /* Whatever was said before it went is still something that
     * happened. */
    fr_live_record_turn(fr);
    memset(&fr->live, 0, sizeof(fr->live));

    if (!str_is_empty(why)) {
        fr_push_event_text(fr, FRIEND_EVENT_ERROR, why);
    } else if (!was_open) {
        fr_push_event_text(fr, FRIEND_EVENT_ERROR,
                           str_lit("the realtime session closed before it "
                                   "was ready"));
    }
}

static void fr_live_tick(Friend_Context *fr)
{
    Fr_Live *live = &fr->live;

    if (live->stage == FR_LIVE_OFF) return;

    fr_live_pay(fr);
    fr_live_flush_audio(fr, 0);

    if (fr->now_unix_ms - live->heard_unix_ms > FR_LIVE_QUIET_MS) {
        /* Whatever was said before it went quiet is still something
         * that happened. */
        fr_live_record_turn(fr);
        fr_live_end(fr);
        fr_push_event_text(fr, FRIEND_EVENT_ERROR,
                           str_lit("the realtime session went quiet and was "
                                   "closed"));
        return;
    }

    /* A session is recycled between turns, never during one. */
    if (live->stage == FR_LIVE_READY && !live->activity && !live->responding) {
        if (live->recycle ||
            fr->now_unix_ms - live->opened_unix_ms > FR_LIVE_RECYCLE_MS) {
            fr_live_end(fr);
            if (!fr_live_begin(fr)) {
                fr_push_event_text(fr, FRIEND_EVENT_ERROR,
                                   friend_last_error(fr));
            }
        }
    }
}
