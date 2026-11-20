/* fr_provider.h — talking to a model, without knowing how to talk.
 *
 * This builds request bytes and reads response bytes. It opens no
 * sockets, resolves no names and knows no TLS: the pump hands what this
 * produces to the frontend as a Friend_Io_Request, and hands back
 * whatever came off the wire. That is what makes the same library run
 * behind a window, a terminal, and a test that replays a recording.
 *
 * Two providers, because two shapes cover the field: Gemini, which is
 * what the service used and what the friend's prompts were tuned
 * against, and the OpenAI chat-completions shape, which is what
 * everything else speaks — OpenAI, OpenRouter, llama.cpp, Ollama, vLLM.
 *
 * Streaming is server-sent events in both cases. The reply has to arrive
 * a word at a time or a friend feels like a form submission.
 */

#ifndef FR_PROVIDER_H
#define FR_PROVIDER_H

#include "fr_io.h"
#include "../../base/fr_json.h"

/* One turn of a prompt. Images are the JPEG bytes, already sized by
 * whoever is sending them. */
typedef struct Fr_Prompt_Message {
    b32           is_user;
    String        text;
    const String *images;
    i32           image_count;
} Fr_Prompt_Message;

typedef struct Fr_Model_Call {
    String                   model;
    String                   system;
    const Fr_Prompt_Message *messages;
    i32                      message_count;

    i32 max_output_tokens;
    i32 thinking_budget; /* only Gemini has the dial; 0 asks for none */
    f32 temperature;     /* <0 leaves it to the provider */
    b32 stream;
} Fr_Model_Call;

/* ------------------------------------------------------------------ *
 *  Requests
 *
 *  Every string lands in the arena you pass and stays valid until it is
 *  reset — which is why the pump builds them in a slot's arena.
 * ------------------------------------------------------------------ */

static Fr_Request fr_provider_chat(Friend_Context *fr, Arena *arena,
                            const Fr_Model_Call *call);

/* One text, embedded. The friend never batches: the model the service
 * used accepts exactly one content per request, and the ones that
 * accept more are no slower for being asked one at a time by a program
 * with 32 requests in flight. */
static Fr_Request fr_provider_embed(Friend_Context *fr, Arena *arena,
                             String text);

/* One spoken turn, transcribed. The samples are mono 16-bit at
 * FR_VOICE_RATE and go up as a WAV file: base64 inside a JSON part for
 * Gemini, a multipart form for the OpenAI shape, which is how the two
 * spell the same upload.
 *
 * The service transcribed with Google's hosted speech API, which wants a
 * cloud project nobody running this locally has, so the call is made of
 * the model the friend already has a key for. The prompt is the
 * service's own — it had this written for Gemini and left it switched
 * off behind a constant. */
static Fr_Request fr_provider_transcribe(Friend_Context *fr, Arena *arena,
                                  const i16 *samples, i32 sample_count);

/* One sentence, spoken, in one of the provider's prebuilt voices. The
 * OpenAI request is the service's (gpt-4o-mini-tts, raw PCM, those
 * instructions); the Gemini one is ours, because the service only ever
 * spoke through its realtime session on that provider. A request that
 * comes back as raw samples is marked to stream, so a sentence starts
 * playing before it has finished arriving. */
static Fr_Request fr_provider_speak(Friend_Context *fr, Arena *arena,
                             String text, String voice);

/* The realtime session's socket: where it lives and what the upgrade
 * carries. */
static Fr_Request fr_provider_live_open(Friend_Context *fr, Arena *arena);

/* ------------------------------------------------------------------ *
 *  Gemini's pieces
 *
 *  The realtime session speaks the same nouns as a generateContent body
 *  in a different envelope, so the pieces are written once, here. Each
 *  writes its keys into an object the caller has open.
 * ------------------------------------------------------------------ */

static void fr_gemini_system(Json_Writer *w, String text);
static void fr_gemini_thinking(Json_Writer *w, i32 budget);
/* "responseModalities": ["AUDIO"] and the voice to say it in. */
static void fr_gemini_speech(Json_Writer *w, String voice);
/* "parts": [{"text": ...}] */
static void fr_gemini_text_parts(Json_Writer *w, String text);

/* ------------------------------------------------------------------ *
 *  Voices
 * ------------------------------------------------------------------ */

typedef enum Fr_Voice_Gender {
    FR_VOICE_EITHER = 0,
    FR_VOICE_MALE,
    FR_VOICE_FEMALE
} Fr_Voice_Gender;

/* One of the provider's prebuilt voices, chosen by a hash of the name.
 * A provider whose voices are not split by gender ignores the hint. */
static String fr_provider_voice(Friend_Context *fr, u32 hash, Fr_Voice_Gender gender);

/* ------------------------------------------------------------------ *
 *  Responses
 * ------------------------------------------------------------------ */

/* The whole text of a non-streamed reply, thinking parts left out. */
static String fr_provider_text(Friend_Context *fr, Arena *arena, String body);

/* 768 floats out of an embedding response. Returns how many, or -1. */
static i32 fr_provider_embedding(Friend_Context *fr, String body, f32 *out,
                          i32 capacity);

/* Pulls whatever complete server-sent events are at the front of a
 * streaming body. *consumed comes in as how much of `body` has already
 * been read and goes out advanced past every event that was whole; the
 * tail of a half-arrived event is left for the next call. The text of
 * those events is appended to the builder. Returns 1 once the provider
 * has said the stream is finished. */
static b32 fr_provider_stream(Friend_Context *fr, String body, i32 *consumed,
                       String_Builder *out_text);

/* What a transcriber heard. Empty when it heard nothing. */
static String fr_provider_transcript(Friend_Context *fr, Arena *arena, String body);

/* The samples out of a speech response that was not streamed, which is
 * one document with the audio inside it. Returns how many bytes were
 * written, or -1 when the body held no audio. */
static i64 fr_provider_speech_pcm(String body, u8 *out, size_t out_capacity);

/* What a provider said when it refused. Falls back to a plain reading
 * of the status when the body is not the error shape. */
static String fr_provider_error_text(Arena *arena, String body, i32 status);

/* Why a request that failed badly failed: the transport's reason, or
 * what the provider said when it refused. In the scratch. */
static String fr_provider_failure(Friend_Context *fr, const Fr_Io_Slot *slot);

#endif /* FR_PROVIDER_H */
