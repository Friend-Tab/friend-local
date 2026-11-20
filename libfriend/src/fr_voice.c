/* fr_voice.c — talking to it out loud.
 *
 * A spoken turn is the typed turn with one thing in front of it and one
 * behind: what you said becomes text before it reaches the friend, and
 * what the friend says becomes sound on the way back. Nothing in
 * between changes. Same prompt, same model, same recall, same
 * consolidation, same coin flip at the end of a sentence — which is the
 * whole point, because a friend you talk to and a friend you type to
 * should not be two different friends.
 *
 * So this file is three small machines:
 *
 *   the microphone    resample to 16 kHz, find the edges of speech
 *   the transcriber   one request, the audio as a WAV file
 *   the voice         the reply cut into sentences as it streams, each
 *                     one spoken the moment it is whole
 *
 * The last one is where the latency went. Waiting for the reply to
 * finish before speaking any of it costs the whole generation; sending
 * the first sentence the instant it has a full stop costs one sentence,
 * and the rest is synthesized while the speaker is still busy with it.
 * The thresholds (twenty characters for the first piece, eighty after)
 * and the sentence rules are the service's, from the same file that did
 * this on its own side.
 *
 * The realtime session (fr_live.c) replaces the middle of that with one
 * model on a websocket. The microphone and the speaker are still these:
 * the session takes the microphone's audio from here and says what it
 * played and when its turn was over, and the state a frontend sees is
 * decided here and nowhere else.
 */

#include "fr_provider.h"
#include "fr_voice.h"
#include "fr_live.h"

#include <math.h>

/* The service's live bridge: a press with less speech in it than this
 * is a cough, a chair, or a button pressed by accident. */
#define FR_VOICE_MIN_SPEECH_MS 150

/* Speech has to hold for this long before the friend believes a turn
 * has started. Below it a door closing starts a conversation. */
#define FR_VOICE_ONSET_MS 60

/* Kept at the end of a turn, so a transcriber is not handed a sentence
 * that stops the instant the last word does. */
#define FR_VOICE_TAIL_MS 200

/* Added to the length of the audio handed out before the microphone is
 * believed to be hearing the room again rather than the speaker: a
 * device buffers a little before the first sample is audible. */
#define FR_SPEAK_TAIL_MS 500

/* How much speech it takes to interrupt the friend, when interrupting by
 * talking is switched on at all. Higher than the onset above, because
 * being cut off by a cough is worse than having to speak up. */
#define FR_VOICE_BARGE_MS 240

/* Where the speech check's floor starts when the microphone opens,
 * before it has heard enough of the room to follow it. RMS, of 16-bit
 * samples. */
#define FR_VOICE_NOISE_START 150

/* The frame the speech check works in, FR_VAD_FRAME samples long. */
#define FR_VAD_FRAME_MS ((FR_VAD_FRAME * 1000) / FR_VOICE_RATE)

/* The service's ttsSegmenter: the first piece goes early and alone so
 * the friend starts talking, unless it is so short ("Oh!") that it
 * would be spoken as a disconnected blip. Later pieces are batched,
 * because fewer and longer requests read more naturally and they are
 * being spoken over the audio already playing anyway. */
#define FR_SPEAK_FIRST_MIN_CHARS 20
#define FR_SPEAK_MIN_CHARS 80

/* ================================================================== *
 *  Which voice
 *
 *  The service chose between its voices with FNV-1a over the friend's
 *  identifier, modulo the pool. The identifier is not in the export — it
 *  lives in the client's key material — so the name is hashed instead.
 *  It is stable, and it is the only thing left to hash, which is also
 *  how the avatar colour is decided. A friend that had a voice you
 *  remember can be given it back by name in the configuration.
 * ================================================================== */

/* Every name the clients' friend-name generators could produce, so a
 * friend that was named by one of them always resolves to a gendered
 * voice, and the common given names under them for a friend that was
 * named by hand. A name in both lists, or in neither, reads as neither:
 * the point is a voice that stays the same, not a perfect guess. */
static const char fr_pool_male_names[] =
    " aarav ahanu akira alex alexei ali andres andrew ari arlo atlas"
    " august azizi bao beck biko bjorn blaine blake bowie cameron casey"
    " charlie chen cheng corey daehyun daiki devin diego dimitri elliot"
    " ellis erik esteban everett feng flynn hassan haruto hayden hiro"
    " hollis hunter hyun ibrahim ishaan ivan jabari jamal jasper javier"
    " jayden jesse jian jiho jimin johan joon jordan jun kai kaito kenji"
    " kiran kofi kwame kyung lars leif lior logan lorenzo luca lysander"
    " maddox magnus malik mateo mikhail milan minsuk misu nasir nikos"
    " noel ocean om omar omari onyx orion parker phoenix rafael rami"
    " ravi remy ren riku river rohan rowan sam sanjay santiago sawyer"
    " selim seojun shawn shin stefan sterling sung sven tae takoda"
    " takumi taro tariq taye thor toby vihaan vivek wei west xiang"
    " yonah yuma yuri zade zayd zephyr zuberi ";

static const char fr_pool_female_names[] =
    " ahyoka aisha alva amani amara anastasia anika aoi arden arya aspen"
    " astrid avery azra bellamy carmen chenoa clover dakota dalia devi"
    " echo elena elin elu emerson emily eun fatima finley frankie freya"
    " hana hania harley hui hye indigo ingrid isla jamie jessie jisoo"
    " jules juno kari katerina kaya kendall layla leighton lennon li"
    " lily ling lucia luna maka marisol marley meera mei mika min monroe"
    " morgan naomi neha nia nokomis noor nova oakley olga petra peyton"
    " priya qing quinn reese riley rina robin sade sage sakari samira"
    " sasha sigrid sita skyler sofia solveig sooyoung sora sunny sydney"
    " tala tatiana tatum tayen taylor terra willow winter wren ximena"
    " xiao yan yuan yuki yuna zhen zoe zuri valeria ";

static const char fr_male_names[] =
    " aaron adam adrian ahmed aiden alan albert alexander ali andre"
    " andrew andy anthony antonio arthur austin ben benjamin bill billy"
    " bob bobby brad bradley brandon brian bruce bryan caleb carl carlos"
    " carter cedric charles chris christian christopher cody colin"
    " connor cooper craig curtis dale dan daniel danny dave david dean"
    " dennis derek diego dominic don donald douglas dylan ed eddie"
    " edward eli elijah eric ethan eugene evan ezra felix frank franklin"
    " fred gabriel garrett gary george gerald grant greg gregory grayson"
    " harold harry hassan henry hudson hugo ian isaac ivan jack jackson"
    " jacob jake james jason javier jeff jeffrey jeremy jerry jesse jim"
    " jimmy joe joel john johnny jonathan jorge jose joseph josh joshua"
    " juan julian justin keith ken kenneth kevin kyle larry lawrence leo"
    " leon levi liam logan louis luca lucas luis luke marco marcus mark"
    " martin mason mateo matt matthew max michael miguel mike mohammed"
    " muhammad nathan nathaniel nick nicholas noah oliver omar oscar"
    " owen patrick paul pedro peter philip phillip ralph randy raymond"
    " ricardo richard rick rob robert roger ron ronald roy russell ryan"
    " samuel scott sean sebastian sergio seth simon stephen steve steven"
    " ted theodore thomas tim timothy todd tom tommy tony travis trevor"
    " troy tyler victor vincent walter wayne wesley will william wyatt"
    " xavier zachary zach zack ";

static const char fr_female_names[] =
    " abigail adeline aisha alice alicia allison amanda amber amelia amy"
    " andrea angela anita ann anna anne annie anya april aria ariana"
    " ashley astrid audrey aurora autumn ava barbara beatrice becky bella"
    " beth bethany betty beverly bonnie brenda brittany brooke caroline"
    " carol carolyn catherine charlotte chelsea cheryl chloe christina"
    " christine cindy claire clara claudia colleen connie crystal cynthia"
    " daisy danielle dawn deborah debra denise diana diane donna doris"
    " dorothy eleanor elena elizabeth ella ellen elsa emily emma erica"
    " erin esme eva evelyn faith fatima fiona frances freya gabriella"
    " gina gloria grace greta hannah hazel heather heidi helen holly"
    " imogen ingrid irene irina isabel isabella isla ivy jacqueline jane"
    " janet janice jasmine jean jenna jennifer jenny jessica jill joan"
    " joanna joyce judith judy julia julie karen katherine kathleen"
    " kathryn kathy katie katya kayla kelly kimberly kristen kristin"
    " laura lauren layla leah lena lila lily linda lisa lori lucy luna"
    " lydia madeline madison maggie margaret maria marie marilyn martha"
    " mary maya megan mei melanie melissa mia michelle mila molly monica"
    " nancy naomi natalie natasha nicole nina nora olga olivia paige"
    " pamela patricia paula pauline penelope phoebe piper poppy priya"
    " rachel rebecca renee rita rose ruby ruth sabrina sadie sakura sally"
    " samantha sandra sara sarah savannah scarlett shannon sharon sheila"
    " shirley sofia sophia sophie stella stephanie susan suzanne sylvia"
    " tammy tara teresa tiffany tina tracy valerie vanessa vera veronica"
    " victoria violet virginia vivian wendy willow yolanda yuki zoe zoey ";

/* The first run of letters in a name, lowercased: "Dr. Emma Watson" is
 * "emma", because the honorific's trailing dot is not a word. */
static i32 fr_first_name_word(String name, char *out, i32 capacity)
{
    i32 at  = 0;
    i32 len = 0;

    for (;;) {
        while (at < name.len && (name.data[at] < 'A' ||
                                 (name.data[at] > 'Z' &&
                                  (name.data[at] < 'a' ||
                                   name.data[at] > 'z')))) {
            ++at;
        }
        if (at >= name.len) return 0;
        len = 0;
        while (at < name.len && len < capacity &&
               ((name.data[at] >= 'A' && name.data[at] <= 'Z') ||
                (name.data[at] >= 'a' && name.data[at] <= 'z'))) {
            out[len++] = fr_lower_char(name.data[at]);
            ++at;
        }
        /* A word that runs straight into a dot is an honorific. */
        if (at < name.len && name.data[at] == '.') {
            ++at;
            continue;
        }
        return len;
    }
}

static b32 fr_name_listed(const char *table, size_t table_len,
                          const char *word, i32 word_len)
{
    char  needle[FR_NAME_MAX + 2];
    i32   i;

    if (word_len <= 0 || word_len > FR_NAME_MAX) return 0;
    needle[0] = ' ';
    for (i = 0; i < word_len; ++i) needle[i + 1] = word[i];
    needle[word_len + 1] = ' ';
    return str_index_of(str_make(table, (i32)table_len - 1),
                        str_make(needle, word_len + 2)) >= 0;
}

#define FR_LISTED(table, word, len) \
    fr_name_listed(table, sizeof(table), word, len)

static u32 fr_fnv1a(String s)
{
    u32 hash = 2166136261u;
    i32 i;
    for (i = 0; i < s.len; ++i) {
        hash ^= (u8)s.data[i];
        hash *= 16777619u;
    }
    return hash;
}
static String fr_voice_for_name(Friend_Context *fr, String name)
{
    char            word[FR_NAME_MAX];
    i32             word_len = fr_first_name_word(name, word, (i32)sizeof(word));
    b32             male     = FR_LISTED(fr_pool_male_names, word, word_len);
    b32             female   = FR_LISTED(fr_pool_female_names, word, word_len);
    Fr_Voice_Gender gender   = FR_VOICE_EITHER;

    if (!male && !female) {
        male   = FR_LISTED(fr_male_names, word, word_len);
        female = FR_LISTED(fr_female_names, word, word_len);
    }
    if (male && !female) gender = FR_VOICE_MALE;
    if (female && !male) gender = FR_VOICE_FEMALE;
    return fr_provider_voice(fr, fr_fnv1a(name), gender);
}

Friend_String friend_voice(Friend_Context *fr)
{
    if (!fr) return str_nil;
    if (!str_is_empty(fr->voice_name)) return fr->voice_name;
    return fr_voice_for_name(fr, friend_name(fr));
}

/* ================================================================== *
 *  The microphone
 * ================================================================== */

static i32 fr_voice_preroll_samples(Friend_Context *fr)
{
    i64 samples = ((i64)fr->tuning.voice_preroll_ms * FR_VOICE_RATE) / 1000;
    if (samples > fr->audio_capacity / 2) samples = fr->audio_capacity / 2;
    return (i32)samples;
}

/* While nobody is talking the buffer keeps only the last pre-roll of
 * audio and throws the rest away: by the time speech is noticed the
 * speaker is already most of a word into it, and that word is in here. */
static void fr_voice_compact(Friend_Context *fr)
{
    Fr_Voice *v       = &fr->voice;
    i32       preroll = fr_voice_preroll_samples(fr);

    if (v->heard || v->pressed) return;
    if (v->captured < preroll * 2) return;
    memmove(fr->audio_in, fr->audio_in + (v->captured - preroll),
            (size_t)preroll * sizeof(i16));
    v->captured = preroll;
}

/* Energy against a floor that follows the quiet parts. Not a model:
 * Silero is an ONNX runtime, which is a dynamic allocator the size of
 * this library, and the service ran it server-side next to the hosted
 * transcriber it also had. This is what is left when both are gone, and
 * it is honest about what it is — it hears a voice, not a speaker. */
static void fr_vad_frame(Friend_Context *fr, const i16 *frame)
{
    Fr_Voice *v = &fr->voice;
    i64       energy = 0;
    i32       rms;
    i32       onset;
    i32       carry;
    b32       speech;
    i32       i;

    for (i = 0; i < FR_VAD_FRAME; ++i) {
        i32 sample = frame[i];
        energy += (i64)sample * (i64)sample;
    }
    rms = (i32)sqrt((f64)(energy / FR_VAD_FRAME));

    /* Two thresholds rather than one. Starting to talk has to be
     * clearly louder than the room — three times it, and never less
     * than a floor, because three times nothing is nothing. Carrying on
     * only has to be louder than the room, because speech dips at the
     * end of a word and a gate with a single threshold hears that dip
     * as the end of a sentence. Measured against recordings: with one
     * threshold both of them were cut off two seconds in. */
    onset  = v->noise * 3 + 200;
    carry  = v->noise + v->noise / 2 + 100;
    speech = rms > (v->heard ? carry : onset);

    /* The floor follows the room while nobody is talking, and is left
     * alone once somebody is: a pause inside a sentence is not the
     * room, and letting it raise the floor would leave the next word
     * quieter than the gate. */
    if (!speech && !v->heard) v->noise += (rms - v->noise) / 16;

    {
        f32 target = (f32)rms / 8000.0f;
        if (target > 1.0f) target = 1.0f;
        v->level += (target - v->level) * 0.3f;
    }

    if (speech) {
        v->speech_ms += FR_VAD_FRAME_MS;
        v->run_ms += FR_VAD_FRAME_MS;
        v->silence_ms = 0;
        if (v->run_ms >= FR_VOICE_ONSET_MS) v->heard = 1;
    } else {
        v->run_ms = 0;
        if (v->heard) v->silence_ms += FR_VAD_FRAME_MS;
    }
}

static void fr_voice_reset_turn(Friend_Context *fr)
{
    Fr_Voice *v = &fr->voice;
    v->captured   = 0;
    v->full       = 0;
    v->frame_fill = 0;
    v->speech_ms  = 0;
    v->run_ms     = 0;
    v->silence_ms = 0;
    v->heard      = 0;
    v->level      = 0.0f;
    v->phase      = 0.0f;
}

static void fr_voice_idle(Friend_Context *fr)
{
    Fr_Voice *v = &fr->voice;
    fr_voice_reset_turn(fr);
    v->state = v->open ? FRIEND_VOICE_LISTENING : FRIEND_VOICE_OFF;
}

static void fr_voice_sample(Friend_Context *fr, i16 sample)
{
    Fr_Voice *v = &fr->voice;

    if (v->captured >= fr->audio_capacity) {
        if (v->heard) {
            v->full = 1;
            return;
        }
        fr_voice_compact(fr);
        if (v->captured >= fr->audio_capacity) v->captured = 0;
    }
    fr->audio_in[v->captured++] = sample;

    v->frame[v->frame_fill++] = sample;
    if (v->frame_fill == FR_VAD_FRAME) {
        v->frame_fill = 0;
        fr_vad_frame(fr, v->frame);
        if (!v->heard) fr_voice_compact(fr);
    }
}

static void fr_voice_finish_turn(Friend_Context *fr);

/* Microphone audio, resampled to FR_VOICE_RATE and measured for speech.
 * In hands-free this is also what ends a turn. */
static void fr_voice_feed(Friend_Context *fr, const i16 *samples, i32 count,
                          i32 rate)
{
    Fr_Voice *v = &fr->voice;
    i32       i;

    if (rate <= 0) rate = FR_VOICE_RATE;
    if (rate != v->in_rate) {
        v->in_rate = rate;
        v->phase   = 0.0f;
    }

    if (rate == FR_VOICE_RATE) {
        for (i = 0; i < count; ++i) fr_voice_sample(fr, samples[i]);
    } else {
        /* Linear, with the phase carried across calls so a buffer
         * boundary is not a click. The transcriber wants 16 kHz and the
         * device is not negotiating; a frontend whose audio layer can
         * convert should, because its resampler is better than this. */
        f32 step = (f32)rate / (f32)FR_VOICE_RATE;
        while (v->phase < (f32)count) {
            i32 index = (i32)v->phase;
            f32 frac  = v->phase - (f32)index;
            i32 a     = samples[index];
            i32 b     = index + 1 < count ? samples[index + 1]
                                          : samples[count - 1];
            fr_voice_sample(fr, (i16)((f32)a + ((f32)(b - a)) * frac));
            v->phase += step;
        }
        v->phase -= (f32)count;
        if (v->phase < 0.0f) v->phase = 0.0f;
    }

    if (v->state == FRIEND_VOICE_LISTENING && v->heard) {
        v->state = FRIEND_VOICE_HEARING;
        if (fr_live_running(fr)) fr_live_turn_begin(fr);
    }
    if (v->state == FRIEND_VOICE_HEARING) {
        /* Hands-free, the end of a sentence is the end of a turn; a
         * press ends when the button does. Either way a turn that has
         * run past what one request can carry is answered here rather
         * than lost. */
        b32 ended = !v->pressed && v->heard &&
                    v->silence_ms >= fr->tuning.voice_endpoint_ms;
        if (v->full || ended) fr_voice_finish_turn(fr);
    }
}

static i32 fr_voice_heard_since(Friend_Context *fr, i32 from, const i16 **out)
{
    i32 have = fr->voice.captured - from;
    if (from < 0 || have <= 0) return 0;
    *out = fr->audio_in + from;
    return have;
}

/* ================================================================== *
 *  Transcribing
 * ================================================================== */

/* A spoken turn is over: send what was heard to be transcribed, or drop
 * it when there was nothing in it. */
static void fr_voice_finish_turn(Friend_Context *fr)
{
    Fr_Voice   *v = &fr->voice;
    Fr_Io_Slot *slot;
    i32         samples;
    String      trouble = str_nil;

    if (v->state != FRIEND_VOICE_HEARING && v->state != FRIEND_VOICE_LISTENING) {
        return;
    }

    /* In a realtime session the audio has been going out as it
     * arrived; this is only the end of it. The short-press gate below
     * does not apply — a turn was opened at the far end the moment
     * speech started, and it has to be closed whatever was in it. */
    if (fr_live_running(fr)) {
        fr_live_turn_end(fr);
        v->pressed = 0;
        fr_voice_reset_turn(fr);
        v->state = FRIEND_VOICE_WORKING;
        return;
    }

    /* Nothing was said. A button pressed by accident is not a question,
     * and the service dropped these too rather than asking a model what
     * a cough meant. */
    if (v->speech_ms < FR_VOICE_MIN_SPEECH_MS) {
        if (v->pressed) trouble = str_lit("there was no speech in that");
        v->pressed = 0;
        fr_voice_idle(fr);
        if (!str_is_empty(trouble)) {
            fr_push_event_text(fr, FRIEND_EVENT_ERROR, trouble);
        }
        return;
    }

    /* The tail of a turn is silence by definition — that is what ended
     * it. Most of it is cut: it is a megabyte of nothing to upload, and
     * transcribers invent words to fill it. */
    samples = v->captured;
    if (v->silence_ms > FR_VOICE_TAIL_MS) {
        i32 drop = ((v->silence_ms - FR_VOICE_TAIL_MS) * FR_VOICE_RATE) / 1000;
        if (drop > 0 && drop < samples) samples -= drop;
    }

    v->pressed = 0;
    slot = fr_io_acquire(fr, FR_IO_PURPOSE_TRANSCRIBE, FRIEND_IO_HTTPS, 0);
    if (!slot) {
        trouble = str_lit("there is no room to send what you said");
    } else if (!fr_io_post(fr, slot,
                           fr_provider_transcribe(fr, &slot->arena,
                                                  fr->audio_in, samples))) {
        trouble = friend_last_error(fr);
    }
    if (!str_is_empty(trouble)) {
        fr_push_event_text(fr, FRIEND_EVENT_ERROR, trouble);
        fr_voice_idle(fr);
        return;
    }

    fr_voice_reset_turn(fr);
    v->state = FRIEND_VOICE_WORKING;
    fr_push_event_text(fr, FRIEND_EVENT_THINKING, str_nil);
}

static void fr_voice_heard(Friend_Context *fr, Fr_Io_Slot *slot)
{
    String text;

    if (fr_io_failed_badly(slot)) {
        fr_push_event_text(fr, FRIEND_EVENT_ERROR,
                           fr_provider_failure(fr, slot));
        fr_voice_idle(fr);
        return;
    }

    text = fr_provider_transcript(fr, &fr->scratch, fr_io_answer(slot));
    if (str_is_empty(text)) {
        /* It listened and there was nothing there. Quietly back to
         * listening: the user does not need to be told that the room
         * was quiet. */
        fr_voice_idle(fr);
        return;
    }

    fr_push_event_text(fr, FRIEND_EVENT_TRANSCRIPT, text);
    fr->voice.state = FRIEND_VOICE_WORKING;
    if (fr_say(fr, text, 0, 0, 1) != FRIEND_OK) {
        fr_push_event_text(fr, FRIEND_EVENT_ERROR, friend_last_error(fr));
        fr_voice_idle(fr);
    }
}

/* ================================================================== *
 *  Speaking
 * ================================================================== */

/* The service's bracket-tag stripper, which the display path uses too:
 * a reply can carry [sighs] and the like, and they are stage directions
 * for the reader rather than words to say. One space after a closing
 * bracket is absorbed so "feel [sighs] fine" does not become "feel
 * fine" with two spaces in it. */
static void fr_speak_append(Friend_Context *fr, String text)
{
    Fr_Voice *v = &fr->voice;
    i32       i;

    for (i = 0; i < text.len; ++i) {
        char c = text.data[i];
        if (v->in_tag) {
            if (c == ']') {
                v->in_tag     = 0;
                v->closed_tag = 1;
            } else if (c == '\n') {
                /* An unterminated tag; the newline ends it and is kept,
                 * because it is also a sentence boundary. */
                v->in_tag     = 0;
                v->closed_tag = 0;
                if (v->segment_len < (i32)sizeof(v->segment)) {
                    v->segment[v->segment_len++] = c;
                }
            }
            continue;
        }
        if (c == '[') {
            v->in_tag     = 1;
            v->closed_tag = 0;
            continue;
        }
        if (v->closed_tag && (c == ' ' || c == '\t')) {
            v->closed_tag = 0;
            continue;
        }
        v->closed_tag = 0;
        if (v->segment_len < (i32)sizeof(v->segment)) {
            v->segment[v->segment_len++] = c;
            v->speak_through += 1;
        }
    }
}

static b32 fr_speak_abbreviation(String word)
{
    static const char *const words[] = {"mr", "mrs", "ms",  "dr", "st",
                                        "vs", "jr",  "sr",  "etc"};
    i32 i;
    for (i = 0; i < fr_array_count(words); ++i) {
        if (str_eq_ci(word, str_from_cstr(words[i]))) return 1;
    }
    return 0;
}

/* The index just past the first sentence end that leaves at least
 * min_chars behind it, or -1. A terminator only counts once the
 * character after it has arrived and is a space, so "3.5" and an
 * unfinished "..." do not split a sentence in half. */
static i32 fr_speak_boundary(String b, i32 min_chars)
{
    i32 i;

    for (i = 0; i < b.len; ++i) {
        char c = b.data[i];
        i32   end;

        if (c == '\n') {
            if (i + 1 >= min_chars) return i + 1;
            continue;
        }
        if (c != '.' && c != '!' && c != '?') continue;

        /* Closing quotes and brackets belong to the sentence. */
        end = i + 1;
        while (end < b.len &&
               (b.data[end] == '"' || b.data[end] == '\'' ||
                b.data[end] == ')' || b.data[end] == ']' ||
                b.data[end] == '*')) {
            ++end;
        }
        if (end >= b.len) continue;
        if (b.data[end] != ' ' && b.data[end] != '\n' &&
            b.data[end] != '\t') {
            continue;
        }
        if (end < min_chars) continue;
        if (c == '.') {
            i32 start = i;
            while (start > 0 && !fr_is_space(b.data[start - 1])) --start;
            if (fr_speak_abbreviation(str_make(b.data + start, i - start))) {
                continue;
            }
        }
        return end;
    }
    return -1;
}

/* Is there anything in here for a voice to say? A lone "..." or a row
 * of emoji is noise to a speech model, and some of them answer a
 * request like that with an error. */
static b32 fr_speak_worth_saying(String s)
{
    i32 i;
    for (i = 0; i < s.len; ++i) {
        u8 c = (u8)s.data[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            return 1;
        }
        /* Two and three byte sequences are letters in every alphabet
         * that has them; four byte ones are emoji and symbols. */
        if (c >= 0xC0 && c < 0xF0) return 1;
    }
    return 0;
}

/* The rest of this reply will not be said, and the friend says why. */
static void fr_speak_give_up(Friend_Context *fr, String why)
{
    fr_push_event_text(fr, FRIEND_EVENT_ERROR, why);
    fr->voice.speak_reply = 0;
    fr->voice.segment_len = 0;
}

static b32 fr_speak_send(Friend_Context *fr, String text)
{
    Fr_Io_Slot *slot = fr_io_acquire(fr, FR_IO_PURPOSE_SPEAK, FRIEND_IO_HTTPS,
                                     0);
    if (!slot) return 0;
    if (!fr_io_post(fr, slot,
                    fr_provider_speak(fr, &slot->arena, text,
                                      friend_voice(fr)))) {
        return 0;
    }
    fr->voice.requested = 1;
    fr->voice.state     = FRIEND_VOICE_SPEAKING;
    return 1;
}

/* Hand out whatever is ready to be said, one request at a time. The
 * next sentence is asked for while the speaker is still busy with the
 * last, so the gap between them is a request's latency rather than a
 * request's duration. */
static void fr_speak_pump(Friend_Context *fr)
{
    Fr_Voice *v = &fr->voice;

    while (v->speak_reply && !v->requested) {
        i32 min_chars =
            v->spoke_any ? FR_SPEAK_MIN_CHARS : FR_SPEAK_FIRST_MIN_CHARS;
        i32    cut = fr_speak_boundary(str_make(v->segment, v->segment_len),
                                       min_chars);
        String piece;

        if (cut < 0) {
            if (!v->text_done) return;
            cut = v->segment_len;
        }
        if (cut <= 0) return;

        piece = str_trim(str_make(v->segment, cut));
        if (fr_speak_worth_saying(piece)) {
            if (!fr_speak_send(fr, piece)) {
                fr_speak_give_up(fr, friend_last_error(fr));
                return;
            }
            v->spoke_any = 1;
            v->spoken_len += piece.len;
        }
        memmove(v->segment, v->segment + cut, (size_t)(v->segment_len - cut));
        v->segment_len -= cut;
    }
}

static void fr_voice_reply_begin(Friend_Context *fr, b32 spoken)
{
    Fr_Voice *v = &fr->voice;

    v->speak_reply   = spoken ? 1 : 0;
    v->speak_through = 0;
    v->spoken_len    = 0;
    v->segment_len   = 0;
    v->in_tag        = 0;
    v->closed_tag    = 0;
    v->spoke_any     = 0;
    v->text_done     = 0;
}

static void fr_voice_reply_text(Friend_Context *fr, String added)
{
    if (!fr->voice.speak_reply || str_is_empty(added)) return;
    fr_speak_append(fr, added);
    fr_speak_pump(fr);
}

static void fr_voice_reply_done(Friend_Context *fr)
{
    if (!fr->voice.speak_reply) return;
    fr->voice.text_done = 1;
    fr_speak_pump(fr);
}

/* What the microphone has heard is left alone: the thing that stops a
 * friend mid-sentence is usually somebody talking over it, and that
 * audio is the next turn. */
static void fr_voice_silence(Friend_Context *fr)
{
    Fr_Voice *v = &fr->voice;

    v->speak_reply    = 0;
    v->segment_len    = 0;
    v->spoken_len     = 0;
    v->speak_through  = 0;
    v->text_done      = 1;
    v->have_carry     = 0;
    v->speak_until_ms = 0;
    v->requested      = 0;
    fr_io_abandon(fr, FR_IO_PURPOSE_SPEAK);
    if (v->state == FRIEND_VOICE_SPEAKING) {
        v->state = v->open ? FRIEND_VOICE_LISTENING : FRIEND_VOICE_OFF;
    }
}

/* How much of what it was saying had been said when it was cut off.
 * Characters handed to the speech model against characters in the
 * reply, which runs ahead of the speaker by whatever the frontend has
 * buffered — a frontend that owns the speaker knows better and can call
 * friend_retract_reply itself with the real number. */
static f32 fr_voice_fraction_spoken(Friend_Context *fr)
{
    Fr_Voice *v = &fr->voice;
    if (v->speak_through <= 0) return 0.0f;
    if (v->spoken_len >= v->speak_through) return 1.0f;
    return (f32)v->spoken_len / (f32)v->speak_through;
}

/* The user talked over the friend. On the spoken-turn path that is a
 * retraction, which the friend remembers along with how much of it was
 * heard; a realtime session is told to stop instead. Either way the
 * speaker stops. */
static void fr_voice_interrupt(Friend_Context *fr)
{
    if (fr_live_running(fr)) {
        fr_live_interrupt(fr);
    } else {
        friend_retract_reply(fr, fr_voice_fraction_spoken(fr));
    }
    fr_voice_silence(fr);
}

/* ================================================================== *
 *  Audio out
 * ================================================================== */

static void fr_voice_play(Friend_Context *fr, const i16 *samples, i32 count,
                   i32 rate)
{
    Fr_Voice    *v = &fr->voice;
    Friend_Event event;

    if (count <= 0 || rate <= 0) return;
    if (v->state != FRIEND_VOICE_SPEAKING) {
        /* A realtime session's reply starting: nothing was asked for
         * here, so the end of it is whatever the session says. */
        v->state     = FRIEND_VOICE_SPEAKING;
        v->text_done = 0;
    }

    memset(&event, 0, sizeof(event));
    event.kind         = FRIEND_EVENT_AUDIO_OUT;
    event.samples      = samples;
    event.sample_count = count;
    event.sample_rate  = rate;
    fr_push_event(fr, event);

    /* A frontend plays samples at real time, by definition, so this is
     * when the speaker will have caught up. */
    if (v->speak_until_ms < fr->now_unix_ms) {
        v->speak_until_ms = fr->now_unix_ms;
    }
    v->speak_until_ms += ((u64)count * 1000u) / (u64)rate;
}

/* Raw bytes off the network, as samples. */
static void fr_voice_emit(Friend_Context *fr, const u8 *bytes, i32 count)
{
    Fr_Voice *v       = &fr->voice;
    i32       total   = count + (v->have_carry ? 1 : 0);
    i32       samples = total / 2;
    i16      *out;

    if (samples <= 0) {
        if (count == 1 && !v->have_carry) {
            v->carry      = bytes[0];
            v->have_carry = 1;
        }
        return;
    }

    /* Copied into the scratch rather than handed out where it landed:
     * the samples arrived as bytes at whatever offset the network chose,
     * and a frontend is owed an aligned array of them. A sample split
     * across two chunks waits in carry for its other half. */
    out = arena_push(&fr->scratch, i16, (size_t)samples);
    if (!out) return;
    {
        u8 *dest = (u8 *)out;
        i32 at   = 0;
        if (v->have_carry) {
            dest[0]       = v->carry;
            at            = 1;
            v->have_carry = 0;
        }
        memcpy(dest + at, bytes, (size_t)(samples * 2 - at));
    }
    if (total & 1) {
        v->carry      = bytes[count - 1];
        v->have_carry = 1;
    }
    fr_voice_play(fr, out, samples, FR_SPEAK_RATE);
}

static void fr_voice_drain_speech(Friend_Context *fr, Fr_Io_Slot *slot)
{
    if (slot->recv_len <= slot->consumed) return;
    fr_voice_emit(fr, slot->req.recv + slot->consumed,
                  (i32)(slot->recv_len - slot->consumed));
    slot->consumed = slot->recv_len;
}

static void fr_voice_spoke(Friend_Context *fr, Fr_Io_Slot *slot)
{
    fr->voice.requested = 0;
    if (slot->abandoned) return;

    if (fr_io_failed_badly(slot)) {
        fr_speak_give_up(fr, fr_provider_failure(fr, slot));
        return;
    }

    if (slot->req.flags & FRIEND_IO_FLAG_STREAM) {
        fr_voice_drain_speech(fr, slot);
    } else {
        /* One document with the samples inside it. Decoded into the
         * scratch, where it lives exactly as long as every other view
         * this library hands out. */
        size_t room = arena_remaining(&fr->scratch) / 2;
        u8    *pcm  = arena_push(&fr->scratch, u8, room);
        i64    written;
        if (!pcm) return;
        written = fr_provider_speech_pcm(fr_io_answer(slot), pcm, room);
        if (written <= 0) {
            fr_speak_give_up(fr, str_lit("the speech model answered with no "
                                         "audio"));
            return;
        }
        fr_voice_emit(fr, pcm, (i32)written);
    }
    fr_speak_pump(fr);
}

static void fr_voice_route(Friend_Context *fr, Fr_Io_Slot *slot)
{
    if (slot->purpose == FR_IO_PURPOSE_TRANSCRIBE) {
        fr_voice_heard(fr, slot);
    } else {
        fr_voice_spoke(fr, slot);
    }
}

static void fr_voice_reset(Friend_Context *fr)
{
    fr_live_end(fr);
    memset(&fr->voice, 0, sizeof(fr->voice));
}

static b32 fr_voice_busy(Friend_Context *fr)
{
    Fr_Voice *v = &fr->voice;
    if (fr_live_running(fr)) return 1;
    return v->state == FRIEND_VOICE_WORKING ||
           v->state == FRIEND_VOICE_SPEAKING ||
           v->state == FRIEND_VOICE_HEARING;
}

static void fr_voice_tick(Friend_Context *fr)
{
    Fr_Voice *v = &fr->voice;

    if (v->state != FRIEND_VOICE_SPEAKING) return;
    fr_speak_pump(fr);
    if (v->requested || v->segment_len > 0) return;
    if (!v->text_done) return;
    /* The last sentence has been handed over; the friend is still
     * talking until the speaker has caught up with it. */
    if (fr->now_unix_ms < v->speak_until_ms + FR_SPEAK_TAIL_MS) return;
    v->speak_reply = 0;
    fr_voice_idle(fr);
}

/* ================================================================== *
 *  What the realtime session tells the voice
 * ================================================================== */

static void fr_voice_live_ready(Friend_Context *fr)
{
    if (fr->voice.open && fr->voice.state == FRIEND_VOICE_WORKING) {
        fr->voice.state = FRIEND_VOICE_LISTENING;
    }
}

static void fr_voice_live_reply_over(Friend_Context *fr)
{
    Fr_Voice *v = &fr->voice;
    if (v->state == FRIEND_VOICE_SPEAKING) {
        v->text_done = 1; /* the tick idles once the speaker catches up */
    } else if (v->state == FRIEND_VOICE_WORKING) {
        fr_voice_idle(fr);
    }
}

/* ================================================================== *
 *  The public calls
 * ================================================================== */

static Friend_Result fr_voice_ready(Friend_Context *fr)
{
    if (!fr->enable_voice) {
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("voice is switched off in the "
                                "configuration"));
    }
    if (str_is_empty(fr->api_key)) {
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("no API key is configured, so there is "
                                "nothing to listen with"));
    }
    return FRIEND_OK;
}

Friend_Result friend_voice_open(Friend_Context *fr)
{
    Friend_Result result;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    result = fr_voice_ready(fr);
    if (result != FRIEND_OK) return result;
    if (fr->voice.open) return FRIEND_OK;

    if (fr->voice_mode == FRIEND_VOICE_REALTIME && !fr_live_begin(fr)) {
        return FRIEND_ERR_BAD_STATE;
    }
    fr->voice.open  = 1;
    fr->voice.noise = FR_VOICE_NOISE_START;
    fr_voice_reset_turn(fr);
    if (fr->voice.state == FRIEND_VOICE_OFF) {
        /* A realtime session is not listening until the socket is open
         * and the friend has been loaded into it, which takes a second
         * or two. Saying it is working rather than listening is the
         * difference between a pause and a word that went nowhere. */
        fr->voice.state = fr_live_running(fr) ? FRIEND_VOICE_WORKING
                                              : FRIEND_VOICE_LISTENING;
    }
    return FRIEND_OK;
}

Friend_Result friend_voice_close(Friend_Context *fr)
{
    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    fr_live_end(fr);
    fr->voice.open    = 0;
    fr->voice.pressed = 0;
    fr_voice_reset_turn(fr);
    if (fr->voice.state == FRIEND_VOICE_LISTENING ||
        fr->voice.state == FRIEND_VOICE_HEARING) {
        fr->voice.state = FRIEND_VOICE_OFF;
    }
    return FRIEND_OK;
}

Friend_Result friend_voice_begin(Friend_Context *fr)
{
    Friend_Result result;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    result = fr_voice_ready(fr);
    if (result != FRIEND_OK) return result;
    if (fr->voice.state == FRIEND_VOICE_WORKING) {
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("the friend is still working out what you "
                                "said last time"));
    }

    /* Talking over the friend is an interruption, which is a thing the
     * friend remembers happening. */
    if (fr->voice.state == FRIEND_VOICE_SPEAKING) fr_voice_interrupt(fr);

    if (fr->voice_mode == FRIEND_VOICE_REALTIME && !fr_live_running(fr) &&
        !fr_live_begin(fr)) {
        return FRIEND_ERR_BAD_STATE;
    }
    fr->voice.pressed = 1;
    if (fr->voice.noise == 0) fr->voice.noise = FR_VOICE_NOISE_START;
    fr_voice_reset_turn(fr);
    fr->voice.state = FRIEND_VOICE_HEARING;
    if (fr_live_running(fr)) fr_live_turn_begin(fr);
    return FRIEND_OK;
}

Friend_Result friend_voice_push(Friend_Context *fr, const i16 *samples,
                                i32 sample_count, i32 sample_rate)
{
    Fr_Voice *v;

    if (!fr || !samples || sample_count <= 0) return FRIEND_ERR_BAD_ARGUMENT;
    v = &fr->voice;

    if (v->state == FRIEND_VOICE_OFF || v->state == FRIEND_VOICE_WORKING) {
        return FRIEND_OK;
    }

    /* While the friend is talking the microphone is hearing the friend.
     * On headphones it is not, and then speech can interrupt — which is
     * what the knob is for, because getting this wrong means a friend
     * that talks over itself. */
    if (v->state == FRIEND_VOICE_SPEAKING) {
        if (!fr->voice_barge_in) {
            v->level = 0.0f;
            return FRIEND_OK;
        }
        fr_voice_feed(fr, samples, sample_count, sample_rate);
        if (v->speech_ms >= FR_VOICE_BARGE_MS) {
            fr_voice_interrupt(fr);
            v->state = FRIEND_VOICE_HEARING;
            if (fr_live_running(fr)) fr_live_turn_begin(fr);
        }
        return FRIEND_OK;
    }

    fr_voice_feed(fr, samples, sample_count, sample_rate);
    return FRIEND_OK;
}

Friend_Result friend_voice_end(Friend_Context *fr)
{
    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    if (!fr->voice.pressed) {
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("no spoken turn is open"));
    }
    fr_voice_finish_turn(fr);
    return FRIEND_OK;
}

Friend_Result friend_speak(Friend_Context *fr, i32 message_index)
{
    Friend_Message message;
    Friend_Result  result;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    result = fr_voice_ready(fr);
    if (result != FRIEND_OK) return result;
    if (fr->voice.state == FRIEND_VOICE_SPEAKING) {
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("the friend is already talking"));
    }
    result = friend_message(fr, message_index, &message);
    if (result != FRIEND_OK) return result;
    if (message.kind != FRIEND_MESSAGE_FRIEND) {
        return fr_error(fr, FRIEND_ERR_BAD_ARGUMENT,
                        str_lit("that is not something the friend said"));
    }

    fr_voice_reply_begin(fr, 1);
    fr_speak_append(fr, message.text);
    fr->voice.text_done = 1;
    fr_speak_pump(fr);
    if (!fr->voice.requested) {
        fr->voice.speak_reply = 0;
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("there is nothing in that message to say"));
    }
    return FRIEND_OK;
}

Friend_Voice_State friend_voice_state(Friend_Context *fr)
{
    if (!fr) return FRIEND_VOICE_OFF;
    return fr->voice.state;
}

float friend_voice_level(Friend_Context *fr)
{
    if (!fr) return 0.0f;
    if (fr->voice.state == FRIEND_VOICE_OFF) return 0.0f;
    return fr->voice.level;
}
