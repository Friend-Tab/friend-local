/* fr_memory.h — how a conversation becomes memories.
 *
 * Consolidation is the other half of the memory system. Recall reads the
 * graph; this writes it, and it is the slower, stranger half:
 *
 *   1. When the active window grows past fifty messages, everything but
 *      the newest twenty comes out of it, forty at most.
 *   2. That stretch is recalled against, so the model naming events
 *      knows what the friend already remembers and does not say it
 *      again.
 *   3. A model reads the stretch and answers with lines of the form
 *      "Event [3-9]: my User did such and such" — a description, and
 *      the span of dialogue it came from.
 *   4. Each becomes a memory: described, embedded, and then joined to
 *      the three memories nearest it by a second model call that says
 *      how they relate — this caused that, that changed into this, they
 *      are about the same thing, or nothing at all.
 *   5. Finally the friend rewrites its own interior monologue from the
 *      memories it just formed.
 *
 * An edge is only added to the most recent memory of each connected
 * component the neighbours fall into, which is what keeps the graph a
 * set of chains rather than a hairball — and chains are what recall
 * walks to say "and then this happened".
 *
 * None of this blocks a reply. The user can talk through all of it.
 */

#ifndef FR_MEMORY_H
#define FR_MEMORY_H

#include "fr_state.h"

/* No fold, no monologue, no usage epoch: for fr_state_clear. */
static void fr_memory_reset(Friend_Context *fr);

/* Start folding if the window has grown past the threshold and nothing
 * else is folding. Called when a reply finishes. */
static void fr_maybe_consolidate(Friend_Context *fr);

/* A completion of one of consolidation's purposes. The pump routes by
 * purpose and this decides what it meant. */
static void fr_fold_route(Friend_Context *fr, Fr_Io_Slot *slot);

/* Memories that arrived without a usable vector get one, a few at a
 * time, whenever nothing else is happening; the second is where the
 * vector comes back. */
static void fr_backfill_embeddings(Friend_Context *fr);
static void fr_backfill_route(Friend_Context *fr, Fr_Io_Slot *slot);

/* ------------------------------------------------------------------ *
 *  The graph, pruned
 * ------------------------------------------------------------------ */

/* In-epoch memories that went a whole epoch unused. Opens a new epoch
 * over the survivors. Does nothing on a graph too small to have
 * evidence either way. */
static i32 fr_prune_unused(Friend_Context *fr);

#endif /* FR_MEMORY_H */
