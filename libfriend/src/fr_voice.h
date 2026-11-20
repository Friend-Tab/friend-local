/* fr_voice.h — what the rest of the library needs from the voice side.
 *
 * The public calls are in friend.h. These are the seams: a reply has to
 * be told to the voice as it streams, the requests the voice makes come
 * back through the same slots as everything else, and a realtime
 * session reports what it did here rather than deciding for itself what
 * the voice is doing.
 */

#ifndef FR_VOICE_H
#define FR_VOICE_H

#include "fr_io.h"

/* The voice a friend of this name speaks in, when the configuration
 * does not name one. */
static String fr_voice_for_name(Friend_Context *fr, String name);

/* The reply the friend is writing. `spoken` decides whether it is to be
 * said out loud; the text arrives in whatever pieces the provider sends
 * and the end of it has to be announced, because the last sentence has
 * no following space to prove it is finished. */
static void fr_voice_reply_begin(Friend_Context *fr, b32 spoken);
static void fr_voice_reply_text(Friend_Context *fr, String added);
static void fr_voice_reply_done(Friend_Context *fr);

/* Stop talking now, and forget what was going to be said. */
static void fr_voice_silence(Friend_Context *fr);

/* Per tick: hand out the next sentence when the last one is done, and
 * notice when the speaker has caught up with the friend. */
static void fr_voice_tick(Friend_Context *fr);

/* A transcription or a sentence of speech came back. */
static void fr_voice_route(Friend_Context *fr, Fr_Io_Slot *slot);

/* Speech that arrives as raw samples is handed on as it comes in, so a
 * sentence starts playing before the request that carries it is done. */
static void fr_voice_drain_speech(Friend_Context *fr, Fr_Io_Slot *slot);

/* Back to nothing: no session, no microphone, nothing being said. For a
 * friend that is being replaced. */
static void fr_voice_reset(Friend_Context *fr);

/* Is there anything the voice side needs a tick for? */
static b32 fr_voice_busy(Friend_Context *fr);

/* ------------------------------------------------------------------ *
 *  The realtime session's side
 * ------------------------------------------------------------------ */

/* The session is up: the microphone means something now. */
static void fr_voice_live_ready(Friend_Context *fr);

/* The friend said this; it goes to the speaker, and the friend is
 * talking until the speaker has played it. */
static void fr_voice_play(Friend_Context *fr, const i16 *samples, i32 count,
                   i32 rate);

/* The session's turn is over, or the session is: nothing more is
 * coming to be played. */
static void fr_voice_live_reply_over(Friend_Context *fr);

/* The microphone's audio for this turn from sample `from` on, at
 * FR_VOICE_RATE. Returns how many samples there are. */
static i32 fr_voice_heard_since(Friend_Context *fr, i32 from, const i16 **out);

#endif /* FR_VOICE_H */
