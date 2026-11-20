/* fr_provider.c — the two wire shapes.
 *
 * Request building is a JSON writer and a URL. Response reading is a
 * pull parser that walks to the one field it wants. Neither holds a
 * document, which matters most for the streaming case: a reply arrives
 * as a few hundred small JSON objects, and parsing each one in place
 * costs nothing and keeps nothing.
 *
 * The API key goes in a header, never in the URL. The service put it in
 * the query string, which is how keys end up in proxy logs.
 */

#include "fr_provider.h"

static b32 fr_is_gemini(Friend_Context *fr)
{
    return fr->provider == FRIEND_PROVIDER_GEMINI;
}

static String fr_base_url(Friend_Context *fr)
{
    if (!str_is_empty(fr->base_url)) {
        String base = fr->base_url;
        while (base.len > 0 && base.data[base.len - 1] == '/') --base.len;
        return base;
    }
    return fr_is_gemini(fr)
               ? str_lit("https://generativelanguage.googleapis.com")
               : str_lit("https://api.openai.com/v1");
}

/* Gemini names the model in the path and the verb after a colon; the
 * OpenAI shape has one path per kind of call and the model in the
 * body. */
static String fr_endpoint(Friend_Context *fr, Arena *arena, String model,
                          String gemini_verb, String openai_path)
{
    String_Builder sb = sb_begin(arena);
    sb_append(&sb, fr_base_url(fr));
    if (fr_is_gemini(fr)) {
        sb_append(&sb, str_lit("/v1beta/models/"));
        sb_append(&sb, model);
        sb_append_char(&sb, ':');
        sb_append(&sb, gemini_verb);
    } else {
        sb_append(&sb, openai_path);
    }
    return sb_done(&sb);
}

/* A websocket's upgrade gets no content type: it carries no body, and a
 * Content-Type on it is a header the server has to forgive. */
static String fr_headers(Friend_Context *fr, Arena *arena, String content_type)
{
    String_Builder sb = sb_begin(arena);
    if (!str_is_empty(content_type)) {
        sb_append(&sb, str_lit("Content-Type: "));
        sb_append(&sb, content_type);
        sb_append(&sb, str_lit("\r\n"));
    }
    if (fr_is_gemini(fr)) {
        sb_append(&sb, str_lit("x-goog-api-key: "));
    } else {
        sb_append(&sb, str_lit("Authorization: Bearer "));
    }
    sb_append(&sb, fr->api_key);
    return sb_done(&sb);
}

static String fr_json_headers(Friend_Context *fr, Arena *arena)
{
    return fr_headers(fr, arena, str_lit("application/json"));
}

static void fr_finish_request(Fr_Request *req, Json_Writer *w)
{
    req->failed = w->failed || w->sb.overflowed;
    req->body   = jw_done(w);
    if (str_is_empty(req->url) || str_is_empty(req->body)) req->failed = 1;
}

/* ================================================================== *
 *  Gemini's pieces
 * ================================================================== */

static void fr_gemini_text_parts(Json_Writer *w, String text)
{
    jw_key(w, str_lit("parts"));
    jw_array_begin(w);
    jw_object_begin(w);
    jw_field_string(w, str_lit("text"), text);
    jw_object_end(w);
    jw_array_end(w);
}

static void fr_gemini_system(Json_Writer *w, String text)
{
    jw_key(w, str_lit("systemInstruction"));
    jw_object_begin(w);
    fr_gemini_text_parts(w, text);
    jw_object_end(w);
}

static void fr_gemini_thinking(Json_Writer *w, i32 budget)
{
    jw_key(w, str_lit("thinkingConfig"));
    jw_object_begin(w);
    jw_field_i64(w, str_lit("thinkingBudget"), budget);
    jw_object_end(w);
}

static void fr_gemini_speech(Json_Writer *w, String voice)
{
    jw_key(w, str_lit("responseModalities"));
    jw_array_begin(w);
    jw_string(w, str_lit("AUDIO"));
    jw_array_end(w);
    jw_key(w, str_lit("speechConfig"));
    jw_object_begin(w);
    jw_key(w, str_lit("voiceConfig"));
    jw_object_begin(w);
    jw_key(w, str_lit("prebuiltVoiceConfig"));
    jw_object_begin(w);
    jw_field_string(w, str_lit("voiceName"), voice);
    jw_object_end(w);
    jw_object_end(w);
    jw_object_end(w);
}

/* ================================================================== *
 *  Requests
 * ================================================================== */

/* Gemini takes a list of parts, so text and images sit side by side. */
static void fr_gemini_parts(Json_Writer *w, const Fr_Prompt_Message *msg)
{
    i32 i;

    jw_key(w, str_lit("parts"));
    jw_array_begin(w);
    jw_object_begin(w);
    jw_field_string(w, str_lit("text"), msg->text);
    jw_object_end(w);
    for (i = 0; i < msg->image_count; ++i) {
        if (str_is_empty(msg->images[i])) continue;
        jw_object_begin(w);
        jw_key(w, str_lit("inlineData"));
        jw_object_begin(w);
        jw_field_string(w, str_lit("mimeType"), str_lit("image/jpeg"));
        jw_key(w, str_lit("data"));
        jw_base64(w, (const u8 *)msg->images[i].data,
                  (size_t)msg->images[i].len);
        jw_object_end(w);
        jw_object_end(w);
    }
    jw_array_end(w);
}

static void fr_chat_body_gemini(Json_Writer *w, const Fr_Model_Call *call)
{
    i32 i;

    jw_object_begin(w);
    if (!str_is_empty(call->system)) fr_gemini_system(w, call->system);

    jw_key(w, str_lit("contents"));
    jw_array_begin(w);
    for (i = 0; i < call->message_count; ++i) {
        const Fr_Prompt_Message *m = &call->messages[i];
        jw_object_begin(w);
        jw_field_string(w, str_lit("role"),
                        m->is_user ? str_lit("user") : str_lit("model"));
        fr_gemini_parts(w, m);
        jw_object_end(w);
    }
    jw_array_end(w);

    jw_key(w, str_lit("generationConfig"));
    jw_object_begin(w);
    if (call->temperature >= 0.0f) {
        jw_field_f64(w, str_lit("temperature"), (f64)call->temperature);
    }
    if (call->max_output_tokens > 0) {
        jw_field_i64(w, str_lit("maxOutputTokens"), call->max_output_tokens);
    }
    jw_field_i64(w, str_lit("candidateCount"), 1);
    fr_gemini_thinking(w, call->thinking_budget);
    jw_object_end(w);

    jw_object_end(w);
}

static void fr_chat_body_openai(Json_Writer *w, const Fr_Model_Call *call)
{
    i32 i;

    jw_object_begin(w);
    jw_field_string(w, str_lit("model"), call->model);
    jw_key(w, str_lit("messages"));
    jw_array_begin(w);
    if (!str_is_empty(call->system)) {
        jw_object_begin(w);
        jw_field_string(w, str_lit("role"), str_lit("system"));
        jw_field_string(w, str_lit("content"), call->system);
        jw_object_end(w);
    }
    for (i = 0; i < call->message_count; ++i) {
        const Fr_Prompt_Message *m = &call->messages[i];
        jw_object_begin(w);
        jw_field_string(w, str_lit("role"),
                        m->is_user ? str_lit("user") : str_lit("assistant"));
        if (m->image_count > 0) {
            /* With a photograph the content stops being a string and
             * becomes a list of parts, and the photograph goes in as a
             * data URL rather than as bytes — the same picture, spelled
             * the way this shape spells it. */
            i32 j;
            jw_key(w, str_lit("content"));
            jw_array_begin(w);
            jw_object_begin(w);
            jw_field_string(w, str_lit("type"), str_lit("text"));
            jw_field_string(w, str_lit("text"), m->text);
            jw_object_end(w);
            for (j = 0; j < m->image_count; ++j) {
                if (str_is_empty(m->images[j])) continue;
                jw_object_begin(w);
                jw_field_string(w, str_lit("type"), str_lit("image_url"));
                jw_key(w, str_lit("image_url"));
                jw_object_begin(w);
                jw_key(w, str_lit("url"));
                jw_base64_prefixed(w, str_lit("data:image/jpeg;base64,"),
                                   (const u8 *)m->images[j].data,
                                   (size_t)m->images[j].len);
                jw_object_end(w);
                jw_object_end(w);
            }
            jw_array_end(w);
        } else {
            jw_field_string(w, str_lit("content"), m->text);
        }
        jw_object_end(w);
    }
    jw_array_end(w);
    if (call->temperature >= 0.0f) {
        /* Both scales run to 2, and they do not mean the same thing.
         * The friend's temperature of 2 is a Gemini number — lively.
         * Handed to an OpenAI-shaped model it produces word salad, in
         * the literal sense: the sentences stop being sentences a
         * paragraph in. Capped at 1, which is the liveliest this shape
         * stays coherent at. */
        f64 temperature = (f64)call->temperature;
        if (temperature > 1.0) temperature = 1.0;
        jw_field_f64(w, str_lit("temperature"), temperature);
    }
    if (call->max_output_tokens > 0) {
        /* max_tokens, not max_completion_tokens: it is the field every
         * server that speaks this shape understands. */
        jw_field_i64(w, str_lit("max_tokens"), call->max_output_tokens);
    }
    if (call->stream) jw_field_bool(w, str_lit("stream"), 1);
    jw_object_end(w);
}

static Fr_Request fr_provider_chat(Friend_Context *fr, Arena *arena,
                            const Fr_Model_Call *call)
{
    Fr_Request req;
    Json_Writer     w;

    memset(&req, 0, sizeof(req));
    if (!fr || !arena || !call) return req;

    req.method  = str_lit("POST");
    req.stream  = call->stream;
    req.url     = fr_endpoint(fr, arena, call->model,
                              call->stream
                                  ? str_lit("streamGenerateContent?alt=sse")
                                  : str_lit("generateContent"),
                              str_lit("/chat/completions"));
    req.headers = fr_json_headers(fr, arena);
    w           = jw_begin(arena);
    if (fr_is_gemini(fr)) {
        fr_chat_body_gemini(&w, call);
    } else {
        fr_chat_body_openai(&w, call);
    }
    fr_finish_request(&req, &w);
    return req;
}

/* ------------------------------------------------------------------ *
 *  Audio
 *
 *  One shape per provider, like everything else here: Gemini takes
 *  files as base64 inside the JSON, the OpenAI shape takes them as
 *  multipart uploads to endpoints of their own.
 * ------------------------------------------------------------------ */

/* The service's own words. It had the Gemini transcription written and
 * left it switched off behind a constant, using its hosted speech API
 * instead; this is that prompt, unedited, because a transcript that
 * reads differently is a different sentence for the friend to answer. */
#define FR_TRANSCRIBE_SYSTEM "You transcribe audio into text"
#define FR_TRANSCRIBE_PROMPT                                                 \
    "This audio was recorded from a small microphone attached to a "         \
    "person. Transcribe any speech in it for me, if there's no speech "      \
    "output [no speech detected]. Try to be as accurate as possible. Do "    \
    "not include anything that isn't speech"

/* What the service told the speech model about how a friend talks.
 * OpenAI takes it in a field of its own. */
#define FR_SPEAK_INSTRUCTIONS                                                \
    "Speak like a close friend in a relaxed, natural conversation: warm, "   \
    "casual, and expressive, at an easy pace."

/* Gemini has no such field: the style is the prompt, and the prompt has
 * to read as an instruction with the line to be read after a colon. The
 * wording above, as its own paragraph with the line underneath, made
 * the model answer the sentence instead of saying it — "Model tried to
 * generate text, but it should only be used for TTS", no audio at all,
 * and only for the replies that happened to end in a question. */
#define FR_SPEAK_GEMINI_LEAD                                                 \
    "Read this aloud, warm and casual and expressive, like a close friend "  \
    "in a relaxed conversation, at an easy pace: "

/* Long enough that no sample run can be mistaken for it. */
#define FR_FORM_BOUNDARY "friend9b2a7c1d4e6f8a0b"

#define FR_WAV_HEADER_BYTES 44


/* The forty-four bytes in front of mono 16-bit samples. Written by hand
 * because it is forty-four bytes and the alternative is a dependency. */
static void fr_wav_header(u8 *out, i32 sample_rate, i32 sample_count)
{
    u32 data_bytes = (u32)sample_count * 2u;

    memcpy(out + 0, "RIFF", 4);
    fr_le_put32(out + 4, 36u + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    fr_le_put32(out + 16, 16u);              /* the fmt chunk's size   */
    fr_le_put16(out + 20, 1u);               /* PCM, uncompressed      */
    fr_le_put16(out + 22, 1u);               /* mono                   */
    fr_le_put32(out + 24, (u32)sample_rate);
    fr_le_put32(out + 28, (u32)sample_rate * 2u); /* bytes per second  */
    fr_le_put16(out + 32, 2u);               /* bytes per frame        */
    fr_le_put16(out + 34, 16u);              /* bits per sample        */
    memcpy(out + 36, "data", 4);
    fr_le_put32(out + 40, data_bytes);
}

static Fr_Request fr_provider_transcribe(Friend_Context *fr, Arena *arena,
                                  const i16 *samples, i32 sample_count)
{
    Fr_Request req;
    u8              header[FR_WAV_HEADER_BYTES];
    size_t          audio_bytes;

    memset(&req, 0, sizeof(req));
    if (!fr || !arena || !samples || sample_count <= 0) return req;

    audio_bytes = (size_t)sample_count * sizeof(i16);
    fr_wav_header(header, FR_VOICE_RATE, sample_count);
    req.method = str_lit("POST");
    req.url    = fr_endpoint(fr, arena, fr->models.transcribe,
                             str_lit("generateContent"),
                             str_lit("/audio/transcriptions"));

    if (fr_is_gemini(fr)) {
        /* The headers first: a writer owns the tail of the arena it is
         * building in, so anything else pushed into it while the writer
         * is open would land inside the request body. */
        Json_Writer w;
        req.headers = fr_json_headers(fr, arena);
        w           = jw_begin(arena);
        jw_object_begin(&w);
        fr_gemini_system(&w, str_lit(FR_TRANSCRIBE_SYSTEM));

        jw_key(&w, str_lit("contents"));
        jw_array_begin(&w);
        jw_object_begin(&w);
        jw_field_string(&w, str_lit("role"), str_lit("user"));
        jw_key(&w, str_lit("parts"));
        jw_array_begin(&w);
        jw_object_begin(&w);
        jw_field_string(&w, str_lit("text"), str_lit(FR_TRANSCRIBE_PROMPT));
        jw_object_end(&w);
        jw_object_begin(&w);
        jw_key(&w, str_lit("inlineData"));
        jw_object_begin(&w);
        jw_field_string(&w, str_lit("mimeType"), str_lit("audio/wav"));
        jw_key(&w, str_lit("data"));
        jw_base64_join(&w, header, sizeof(header), (const u8 *)samples,
                       audio_bytes);
        jw_object_end(&w);
        jw_object_end(&w);
        jw_array_end(&w);
        jw_object_end(&w);
        jw_array_end(&w);

        /* No thinking and no creativity: it is a transcript. */
        jw_key(&w, str_lit("generationConfig"));
        jw_object_begin(&w);
        jw_field_f64(&w, str_lit("temperature"), 0.0);
        jw_field_i64(&w, str_lit("candidateCount"), 1);
        fr_gemini_thinking(&w, 0);
        jw_object_end(&w);
        jw_object_end(&w);
        fr_finish_request(&req, &w);
    } else {
        String_Builder sb;
        req.headers = fr_headers(
            fr, arena,
            str_lit("multipart/form-data; boundary=" FR_FORM_BOUNDARY));
        sb = sb_begin(arena);
        sb_append(&sb, str_lit("--" FR_FORM_BOUNDARY "\r\n"
                               "Content-Disposition: form-data; "
                               "name=\"model\"\r\n\r\n"));
        sb_append(&sb, fr->models.transcribe);
        sb_append(&sb, str_lit("\r\n--" FR_FORM_BOUNDARY "\r\n"
                               "Content-Disposition: form-data; "
                               "name=\"file\"; filename=\"turn.wav\"\r\n"
                               "Content-Type: audio/wav\r\n\r\n"));
        sb_append(&sb, str_make((const char *)header, (i32)sizeof(header)));
        sb_append(&sb, str_make((const char *)samples, (i32)audio_bytes));
        sb_append(&sb, str_lit("\r\n--" FR_FORM_BOUNDARY "--\r\n"));
        req.failed = sb.overflowed;
        req.body   = sb_done(&sb);
        if (str_is_empty(req.url) || str_is_empty(req.body)) req.failed = 1;
    }
    return req;
}

static Fr_Request fr_provider_speak(Friend_Context *fr, Arena *arena,
                             String text, String voice)
{
    Fr_Request req;
    Json_Writer     w;
    String          directed = text;

    memset(&req, 0, sizeof(req));
    if (!fr || !arena || str_is_empty(text)) return req;

    if (fr_is_gemini(fr)) {
        /* Joined here, before the writer starts, because two things
         * appending to one bump arena interleave. */
        String_Builder sb = sb_begin(arena);
        sb_append(&sb, str_lit(FR_SPEAK_GEMINI_LEAD));
        sb_append(&sb, text);
        directed = sb_done(&sb);
    }

    req.method  = str_lit("POST");
    req.url     = fr_endpoint(fr, arena, fr->models.speak,
                              str_lit("generateContent"),
                              str_lit("/audio/speech"));
    req.headers = fr_json_headers(fr, arena);

    w = jw_begin(arena);
    jw_object_begin(&w);
    if (fr_is_gemini(fr)) {
        jw_key(&w, str_lit("contents"));
        jw_array_begin(&w);
        jw_object_begin(&w);
        fr_gemini_text_parts(&w, directed);
        jw_object_end(&w);
        jw_array_end(&w);

        jw_key(&w, str_lit("generationConfig"));
        jw_object_begin(&w);
        fr_gemini_speech(&w, voice);
        jw_object_end(&w);
    } else {
        jw_field_string(&w, str_lit("model"), fr->models.speak);
        jw_field_string(&w, str_lit("voice"), voice);
        jw_field_string(&w, str_lit("input"), text);
        /* The older tts-1 models refuse a request that carries
         * instructions rather than ignoring it. */
        if (!str_has_prefix(fr->models.speak, str_lit("tts-"))) {
            jw_field_string(&w, str_lit("instructions"),
                            str_lit(FR_SPEAK_INSTRUCTIONS));
        }
        /* Raw samples, so there is no container to parse and no decoder
         * to carry: 24 kHz mono, signed 16-bit, little-endian. They are
         * the answer itself, so they can be played as they arrive. */
        jw_field_string(&w, str_lit("response_format"), str_lit("pcm"));
        req.stream = 1;
    }
    jw_object_end(&w);
    fr_finish_request(&req, &w);
    return req;
}

static Fr_Request fr_provider_embed(Friend_Context *fr, Arena *arena, String text)
{
    Fr_Request req;
    Json_Writer     w;

    memset(&req, 0, sizeof(req));
    if (!fr || !arena) return req;

    req.method  = str_lit("POST");
    req.url     = fr_endpoint(fr, arena, fr->models.embed,
                              str_lit("embedContent"), str_lit("/embeddings"));
    req.headers = fr_json_headers(fr, arena);

    w = jw_begin(arena);
    jw_object_begin(&w);
    if (fr_is_gemini(fr)) {
        jw_key(&w, str_lit("content"));
        jw_object_begin(&w);
        fr_gemini_text_parts(&w, text);
        jw_object_end(&w);
        jw_field_i64(&w, str_lit("outputDimensionality"), FR_EMBED_DIMS);
    } else {
        jw_field_string(&w, str_lit("model"), fr->models.embed);
        jw_field_string(&w, str_lit("input"), text);
        jw_field_i64(&w, str_lit("dimensions"), FR_EMBED_DIMS);
    }
    jw_object_end(&w);
    fr_finish_request(&req, &w);
    return req;
}

/* The documented endpoints, and the key in a header: the query-string
 * form every Gemini example uses puts it in the logs of everything
 * between here and there. */
static Fr_Request fr_provider_live_open(Friend_Context *fr, Arena *arena)
{
    Fr_Request req;
    String          base;
    String_Builder  sb;

    memset(&req, 0, sizeof(req));
    if (!fr || !arena) return req;

    base = fr_base_url(fr);
    sb   = sb_begin(arena);
    if (str_has_prefix(base, str_lit("https://"))) {
        sb_append(&sb, str_lit("wss://"));
        sb_append(&sb, str_skip(base, 8));
    } else {
        sb_append(&sb, base);
    }
    if (fr_is_gemini(fr)) {
        sb_append(&sb, str_lit("/ws/google.ai.generativelanguage.v1beta."
                               "GenerativeService.BidiGenerateContent"));
    } else {
        sb_append(&sb, str_lit("/realtime?model="));
        sb_append(&sb, fr->models.live);
    }
    req.failed  = sb.overflowed;
    req.url     = sb_done(&sb);
    req.method  = str_lit("GET");
    req.headers = fr_headers(fr, arena, str_nil);
    if (str_is_empty(req.url)) req.failed = 1;
    return req;
}

/* ------------------------------------------------------------------ *
 *  Voices
 *
 *  The voices the service's friends spoke in. OpenAI's are split by the
 *  voice's gender so a friend whose name reads as male or female gets a
 *  matching one; "marin" and "cedar" are the two the provider
 *  recommends. A name that reads as neither fans out across everything,
 *  male pool first, the way the service joined the lists.
 * ------------------------------------------------------------------ */

static const char *const fr_gemini_voices[] = {
    "Kore", "Charon", "Fenrir", "Leda", "Enceladus", "Puck", "Zephyr",
    "Aoede"
};
static const char *const fr_openai_male_voices[]   = {"cedar", "ash", "echo"};
static const char *const fr_openai_female_voices[] = {"marin", "coral",
                                                      "sage"};

static String fr_provider_voice(Friend_Context *fr, u32 hash, Fr_Voice_Gender gender)
{
    u32 male   = (u32)fr_array_count(fr_openai_male_voices);
    u32 female = (u32)fr_array_count(fr_openai_female_voices);
    u32 at;

    if (fr_is_gemini(fr)) {
        return str_from_cstr(
            fr_gemini_voices[hash % (u32)fr_array_count(fr_gemini_voices)]);
    }
    if (gender == FR_VOICE_MALE) {
        return str_from_cstr(fr_openai_male_voices[hash % male]);
    }
    if (gender == FR_VOICE_FEMALE) {
        return str_from_cstr(fr_openai_female_voices[hash % female]);
    }
    at = hash % (male + female);
    return str_from_cstr(at < male ? fr_openai_male_voices[at]
                                   : fr_openai_female_voices[at - male]);
}

/* ================================================================== *
 *  Responses
 * ================================================================== */

/* candidates[0].content.parts[*].text, skipping any part the model
 * flagged as thinking. Shared by the streaming and whole-body readers,
 * because a Gemini stream event is one of these objects.
 *
 * The text is appended straight onto the caller's builder, which is the
 * only way to assemble a reply out of a few hundred events without a
 * copy per event. A part that turns out to have been thinking is rolled
 * back off the tail, because the flag can arrive after the text. */
static void fr_append_gemini_text(Json_Parser *p, String_Builder *out,
                                  b32 *out_finished)
{
    String key;

    if (!json_object_begin(p)) return;
    while (json_object_next(p, &key)) {
        if (!str_eq(key, str_lit("candidates"))) {
            json_skip(p);
            continue;
        }
        if (!json_array_begin(p)) break;
        while (json_array_next(p)) {
            String ckey;
            if (!json_object_begin(p)) break;
            while (json_object_next(p, &ckey)) {
                if (str_eq(ckey, str_lit("finishReason"))) {
                    json_skip(p);
                    if (out_finished) *out_finished = 1;
                    continue;
                }
                if (!str_eq(ckey, str_lit("content"))) {
                    json_skip(p);
                    continue;
                }
                if (!json_object_begin(p)) break;
                {
                    String contentkey;
                    while (json_object_next(p, &contentkey)) {
                        if (!str_eq(contentkey, str_lit("parts"))) {
                            json_skip(p);
                            continue;
                        }
                        if (!json_array_begin(p)) break;
                        while (json_array_next(p)) {
                            String     partkey;
                            Arena_Mark mark = arena_mark(out->arena);
                            i32        len  = out->len;
                            b32        thought = 0;
                            if (!json_object_begin(p)) break;
                            while (json_object_next(p, &partkey)) {
                                if (str_eq(partkey, str_lit("text")) &&
                                    json_peek(p) == JSON_STRING) {
                                    json_string_append(p, out);
                                } else if (str_eq(partkey,
                                                  str_lit("thought"))) {
                                    thought = json_bool(p);
                                } else {
                                    json_skip(p);
                                }
                            }
                            if (thought) {
                                arena_reset_to(out->arena, mark);
                                out->len = len;
                            }
                        }
                    }
                }
            }
        }
    }
}

/* choices[0].delta.content for a stream, choices[0].message.content for
 * a whole body. */
static void fr_append_openai_text(Json_Parser *p, String_Builder *out,
                                  b32 *out_finished)
{
    String key;

    if (!json_object_begin(p)) return;
    while (json_object_next(p, &key)) {
        if (!str_eq(key, str_lit("choices"))) {
            json_skip(p);
            continue;
        }
        if (!json_array_begin(p)) break;
        while (json_array_next(p)) {
            String ckey;
            if (!json_object_begin(p)) break;
            while (json_object_next(p, &ckey)) {
                b32 is_delta   = str_eq(ckey, str_lit("delta"));
                b32 is_message = str_eq(ckey, str_lit("message"));
                if (str_eq(ckey, str_lit("finish_reason"))) {
                    if (json_peek(p) == JSON_STRING && out_finished) {
                        *out_finished = 1;
                    }
                    json_skip(p);
                    continue;
                }
                if (!is_delta && !is_message) {
                    json_skip(p);
                    continue;
                }
                if (!json_object_begin(p)) break;
                {
                    String dkey;
                    while (json_object_next(p, &dkey)) {
                        if (str_eq(dkey, str_lit("content")) &&
                            json_peek(p) == JSON_STRING) {
                            json_string_append(p, out);
                        } else {
                            json_skip(p);
                        }
                    }
                }
            }
        }
    }
}

static String fr_provider_text(Friend_Context *fr, Arena *arena, String body)
{
    String_Builder sb;
    Json_Parser    p;

    if (!fr || !arena) return str_nil;
    sb = sb_begin(arena);
    p  = json_begin(body);
    if (fr_is_gemini(fr)) {
        fr_append_gemini_text(&p, &sb, 0);
    } else {
        fr_append_openai_text(&p, &sb, 0);
    }
    return sb_done(&sb);
}

static i32 fr_provider_embedding(Friend_Context *fr, String body, f32 *out,
                          i32 capacity)
{
    Json_Parser p;
    String      key;
    i32         count = -1;

    if (!fr || !out || capacity <= 0) return -1;
    p = json_begin(body);
    if (!json_object_begin(&p)) return -1;

    while (json_object_next(&p, &key)) {
        b32 gemini = str_eq(key, str_lit("embedding"));
        b32 openai = str_eq(key, str_lit("data"));

        if (!gemini && !openai) {
            json_skip(&p);
            continue;
        }

        if (openai) {
            /* data: [ { "embedding": [ ... ] } ] */
            if (!json_array_begin(&p)) break;
            if (!json_array_next(&p)) break;
            if (!json_object_begin(&p)) break;
            {
                String dkey;
                while (json_object_next(&p, &dkey)) {
                    if (!str_eq(dkey, str_lit("embedding"))) {
                        json_skip(&p);
                        continue;
                    }
                    count = 0;
                    if (!json_array_begin(&p)) break;
                    while (json_array_next(&p)) {
                        f64 v = json_f64(&p);
                        if (count < capacity) out[count] = (f32)v;
                        ++count;
                    }
                }
            }
            while (json_array_next(&p)) json_skip(&p);
            continue;
        }

        /* embedding: { "values": [ ... ] } */
        if (!json_object_begin(&p)) break;
        {
            String ekey;
            while (json_object_next(&p, &ekey)) {
                if (!str_eq(ekey, str_lit("values"))) {
                    json_skip(&p);
                    continue;
                }
                count = 0;
                if (!json_array_begin(&p)) break;
                while (json_array_next(&p)) {
                    f64 v = json_f64(&p);
                    if (count < capacity) out[count] = (f32)v;
                    ++count;
                }
            }
        }
    }

    if (p.failed) return -1;
    return count;
}

static b32 fr_provider_stream(Friend_Context *fr, String body, i32 *consumed,
                       String_Builder *out_text)
{
    b32 finished = 0;
    i32 at;

    if (!fr || !consumed || !out_text) return 0;
    at = *consumed;

    for (;;) {
        String line;
        i32    end = at;
        i32    i;

        while (end < body.len && body.data[end] != '\n') ++end;
        if (end >= body.len) break; /* a half-arrived line: wait */

        line = str_slice(body, at, end - at);
        at   = end + 1;
        *consumed = at;

        if (line.len > 0 && line.data[line.len - 1] == '\r') --line.len;
        if (line.len == 0) continue;

        /* Server-sent events: we only care about data lines. Comments
         * (": keep-alive") and event/id lines are not ours. */
        if (!str_has_prefix(line, str_lit("data:"))) continue;
        line = str_skip(line, 5);
        while (line.len > 0 && line.data[0] == ' ') line = str_skip(line, 1);

        /* OpenAI ends with a sentinel; Gemini just stops. */
        if (str_eq(line, str_lit("[DONE]"))) {
            finished = 1;
            continue;
        }

        /* Some servers wrap an error in the stream. Nothing to extract:
         * the pump notices no text arrived and the status says why. */
        for (i = 0; i < line.len; ++i) {
            if (line.data[i] == '{') break;
        }
        if (i >= line.len) continue;

        {
            Json_Parser p = json_begin(str_skip(line, i));
            if (fr_is_gemini(fr)) {
                fr_append_gemini_text(&p, out_text, &finished);
            } else {
                fr_append_openai_text(&p, out_text, &finished);
            }
        }
    }
    return finished;
}

/* ------------------------------------------------------------------ *
 *  Audio coming back
 * ------------------------------------------------------------------ */

static String fr_provider_transcript(Friend_Context *fr, Arena *arena, String body)
{
    String text;

    if (!fr || !arena) return str_nil;
    if (fr_is_gemini(fr)) {
        /* A transcript is a reply like any other on this provider. */
        text = str_trim(fr_provider_text(fr, arena, body));
    } else {
        Json_Parser p = json_begin(body);
        String      key;
        text = str_nil;
        if (!json_object_begin(&p)) return str_nil;
        while (json_object_next(&p, &key)) {
            if (!str_eq(key, str_lit("text"))) {
                json_skip(&p);
                continue;
            }
            text = str_trim(json_string(&p, arena));
        }
        if (p.failed) return str_nil;
    }

    /* What the prompt asks it to say when there was nothing to hear.
     * The friend is not told the room was quiet; the turn is dropped.
     *
     * Matched loosely, because the models do not say it back exactly as
     * they were asked to: "[no speech detected]", "No speech
     * detected.", and "no speech detected" are all the same answer, and
     * the one that got through as a message read as the user saying the
     * words out loud. */
    {
        String bare = text;
        while (bare.len > 0 && (bare.data[0] == '[' || bare.data[0] == '(')) {
            bare = str_skip(bare, 1);
        }
        while (bare.len > 0 && (bare.data[bare.len - 1] == ']' ||
                                bare.data[bare.len - 1] == ')' ||
                                bare.data[bare.len - 1] == '.' ||
                                bare.data[bare.len - 1] == '!' ||
                                fr_is_space(bare.data[bare.len - 1]))) {
            bare.len -= 1;
        }
        if (str_eq_ci(bare, str_lit("no speech detected"))) return str_nil;
    }
    return text;
}

/* Only Gemini answers this way: the OpenAI shape's speech is raw samples
 * and was streamed. */
static i64 fr_provider_speech_pcm(String body, u8 *out, size_t out_capacity)
{
    Json_Parser p;
    String      key;
    i64         written = -1;

    if (!out) return -1;
    p = json_begin(body);
    if (!json_object_begin(&p)) return -1;

    /* candidates[0].content.parts[*].inlineData.data */
    while (json_object_next(&p, &key)) {
        if (!str_eq(key, str_lit("candidates"))) {
            json_skip(&p);
            continue;
        }
        if (!json_array_begin(&p)) break;
        if (json_array_next(&p)) {
            String ckey;
            if (!json_object_begin(&p)) break;
            while (json_object_next(&p, &ckey)) {
                String pkey;
                if (!str_eq(ckey, str_lit("content"))) {
                    json_skip(&p);
                    continue;
                }
                if (!json_object_begin(&p)) break;
                while (json_object_next(&p, &pkey)) {
                    if (!str_eq(pkey, str_lit("parts"))) {
                        json_skip(&p);
                        continue;
                    }
                    if (!json_array_begin(&p)) break;
                    while (json_array_next(&p)) {
                        String part_key;
                        if (!json_object_begin(&p)) break;
                        while (json_object_next(&p, &part_key)) {
                            String data_key;
                            if (!str_eq(part_key, str_lit("inlineData"))) {
                                json_skip(&p);
                                continue;
                            }
                            if (!json_object_begin(&p)) break;
                            while (json_object_next(&p, &data_key)) {
                                i64 n;
                                if (!str_eq(data_key, str_lit("data"))) {
                                    json_skip(&p);
                                    continue;
                                }
                                if (written < 0) written = 0;
                                n = json_base64(&p, out + written,
                                                out_capacity -
                                                    (size_t)written);
                                if (n < 0) return -1;
                                written += n;
                            }
                        }
                    }
                }
            }
        }
        while (json_array_next(&p)) json_skip(&p);
    }

    if (p.failed) return -1;
    return written;
}

static String fr_provider_error_text(Arena *arena, String body, i32 status)
{
    Json_Parser p;
    String      key;
    String      message = str_nil;

    if (!arena) return str_nil;

    /* Both shapes nest it the same way: { "error": { "message": ... } } */
    p = json_begin(body);
    if (json_object_begin(&p)) {
        while (json_object_next(&p, &key)) {
            if (!str_eq(key, str_lit("error"))) {
                json_skip(&p);
                continue;
            }
            if (json_peek(&p) == JSON_STRING) {
                message = json_string(&p, arena);
                continue;
            }
            if (!json_object_begin(&p)) break;
            {
                String ekey;
                while (json_object_next(&p, &ekey)) {
                    if (str_eq(ekey, str_lit("message")) &&
                        json_peek(&p) == JSON_STRING) {
                        message = json_string(&p, arena);
                    } else {
                        json_skip(&p);
                    }
                }
            }
        }
    }

    if (!str_is_empty(message)) return message;

    {
        String_Builder sb = sb_begin(arena);
        sb_append(&sb, str_lit("the provider answered with status "));
        sb_append_i64(&sb, status);
        if (body.len > 0) {
            sb_append(&sb, str_lit(": "));
            sb_append(&sb, fr_truncate_utf8(body, 200));
        }
        return sb_done(&sb);
    }
}

static String fr_provider_failure(Friend_Context *fr, const Fr_Io_Slot *slot)
{
    if (slot->failed) return slot->error;
    return fr_provider_error_text(&fr->scratch, fr_io_answer(slot),
                                  slot->status);
}
