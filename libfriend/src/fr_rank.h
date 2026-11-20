/* fr_rank.h — recall's model calls.
 *
 * fr_recall.h is the pure half of recall: vectors compared, the graph
 * walked. This is the half with requests in it. A recall run embeds one
 * or two windows of conversation as search queries, takes each one's
 * nearest memories, and asks a model which of those are worth having in
 * mind. A reply runs one before it answers and consolidation runs one
 * before it names events; they differ only in what they ask about, which
 * is the spec, so the numbers they use cannot drift apart.
 *
 * When the reranker cannot be asked, or answers with nothing usable, the
 * embedding order stands in for it: the ranking is a refinement, not a
 * dependency.
 */

#ifndef FR_RANK_H
#define FR_RANK_H

#include "fr_io.h"

typedef struct Fr_Recall_Spec {
    Fr_Io_Purpose embed_purpose;
    Fr_Io_Purpose rerank_purpose;
    i32           query_count; /* at most FR_RECALL_QUERIES */
    /* The conversation query q is about: what is embedded, and — with
     * for_rerank — what the reranker is shown beside the candidates. */
    String (*query)(Friend_Context *fr, Arena *arena, i32 q, b32 for_rerank);
} Fr_Recall_Spec;

/* Sends the query embeddings. Returns 0 when none could be sent, which
 * leaves the run finished with nothing in it. */
static b32 fr_recall_start(Friend_Context *fr, Fr_Recall *run,
                    const Fr_Recall_Spec *spec);

/* A completion of one of the spec's two purposes. Returns 1 when it was
 * the one that finished the run: run->relevant is then what recall
 * found. A completion from an earlier run is ignored. */
static b32 fr_recall_route(Friend_Context *fr, Fr_Recall *run,
                    const Fr_Recall_Spec *spec, Fr_Io_Slot *slot);

/* An embedding answer, quantized into out. 0 when the request failed or
 * the answer was not a vector of the right size. */
static b32 fr_read_embedding(Friend_Context *fr, const Fr_Io_Slot *slot, i8 *out);

/* Sends a memory to be embedded as a document: the time its exchange
 * ended, its description, and what was seen in its photos. */
static b32 fr_embed_memory(Friend_Context *fr, Fr_Io_Purpose purpose, i32 subject,
                    const Fr_Memory_Record *m);

#endif /* FR_RANK_H */
