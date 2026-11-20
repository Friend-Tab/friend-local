/* fr_live.h — the realtime session, as the rest of the library sees it.
 *
 * The public calls are still friend_voice_*: which of the two ways of
 * talking is in use is a configuration, not a different API. These are
 * the seams — the socket's events arrive through the pump, and the
 * microphone's turn boundaries come from the same speech check the
 * other path uses.
 */

#ifndef FR_LIVE_H
#define FR_LIVE_H

#include "fr_io.h"

/* Open and close the socket. Opening is not instant: the session is
 * ready when the provider has acknowledged the setup and the
 * conversation so far has been seeded into it. */
static b32  fr_live_begin(Friend_Context *fr);
static void fr_live_end(Friend_Context *fr);

/* Is there a session, or the beginning of one? */
static b32 fr_live_running(Friend_Context *fr);

/* The turn, decided here as it is on the other path: by the speech
 * check, or by the button. */
static void fr_live_turn_begin(Friend_Context *fr);
static void fr_live_turn_end(Friend_Context *fr);

/* Stop it talking: the user cut in. */
static void fr_live_interrupt(Friend_Context *fr);

/* Per tick: microphone audio out, and the session's own clocks. */
static void fr_live_tick(Friend_Context *fr);

/* From the pump. */
static void fr_live_opened(Friend_Context *fr, u64 channel);
static void fr_live_message(Friend_Context *fr, u64 channel, String message);
static void fr_live_closed(Friend_Context *fr, u64 channel, String why);

#endif /* FR_LIVE_H */
