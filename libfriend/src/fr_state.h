/* fr_state.h — what a friend is, in memory.
 *
 * Everything here lives inside the single block the caller handed to
 * friend_init, carved up once by fr_layout(). There are no pointers
 * between records: a memory refers to another by slot index, so the whole
 * state is position-independent and saving it is a walk over the
 * records, with no pointers to fix up.
 *
 * Records are fixed size. A memory is one 5-and-a-bit KiB slot whether it
 * holds three words or three paragraphs. That is the trade that makes the
 * budget honest: pruning a memory returns its slot to a free list, and
 * there is no pool to compact, no fragmentation to measure, and no moment
 * where the program has to go looking for room it does not have.
 */

#ifndef FR_STATE_H
#define FR_STATE_H

#include "../../base/fr_base.h"

/* The embedding geometry is not ours to choose: it is what the service
 * stored, so an imported friend's vectors stay comparable. 768 dimensions,
 * one signed byte each, round(unit_value * 127). */
#define FR_EMBED_DIMS 768
#define FR_EMBED_SCALE 127.0f
/* The model that made the export's vectors: they are reused byte for byte
 * only while the configured embedding model is this one. */
#define FR_EXPORT_EMBED_MODEL str_lit("gemini-embedding-2")

/* Measured against a real export: descriptions run to about 600 bytes at
 * the top end, so 512 would have quietly cut one memory in twenty. These
 * are what recall reads, so losing their tails is not a small thing. */
#define FR_DESCRIPTION_MAX 768  /* a memory's description */
#define FR_DIALOG_MAX 4096      /* the dialogue that formed it, rendered */
#define FR_MEMORY_IMAGES_MAX 6  /* photos carried on one memory, as many as
                                 * the service embedded in one request */
#define FR_CHAT_TEXT_MAX 8192   /* one message; longer is truncated */
#define FR_CHAT_IMAGES_MAX FRIEND_ATTACHMENTS_MAX
/* What a vision model made of the photos on one message, newline
 * separated, in image order. A caption in the export runs to about 250
 * bytes, so five of them fit with room over; the rest is cut. Kept on
 * the message rather than beside it because a caption is only ever read
 * with the photo it belongs to, and because this way it survives into
 * the scrollback and the save file without a second thing to keep in
 * step. */
#define FR_CAPTIONS_MAX 1536
/* A message's uuid, which the save format still has room for and writes
 * as zeros: the service used it to recognise a resend, and nothing here
 * resends. */
#define FR_UUID_BYTES 16

#define FR_SLOT_NONE 0xFFFFFFFFu

#define FR_NAME_MAX FRIEND_NAME_MAX
#define FR_INTERIORITY_MAX 2048

/* ------------------------------------------------------------------ *
 *  Dialogue
 *
 *  A memory keeps the exchange that formed it, because the relation pass
 *  is shown it when deciding how two memories connect. It cannot be kept
 *  pre-rendered: every line is stamped with how long ago it happened, so
 *  it has to be rebuilt against the clock each time it is used.
 *
 *  So the dialogue is a packed stream of turns inside the record's fixed
 *  4 KiB: an 8-byte time, a kind, a 2-byte length, then the text. Eleven
 *  bytes a turn and no turn limit, against 16 bytes a turn and a fixed
 *  limit if these were an array of descriptors.
 * ------------------------------------------------------------------ */

typedef enum Fr_Turn_Kind {
    FR_TURN_USER = 0,
    FR_TURN_FRIEND,
    FR_TURN_AMBIENT,   /* a summary of audio the friend overheard */
    FR_TURN_OVERHEARD, /* a raw transcription of it */
    FR_TURN_CAPTION    /* what a vision model saw in an attached photo */
} Fr_Turn_Kind;

#define FR_TURN_HEADER_BYTES 11

/* A photo, wherever it lives. The bytes are in the blob file; only this
 * descriptor is in the budget, which is why photo count does not move the
 * memory number.
 *
 * blob_offset and blob_size are the photo itself. thumb_offset, when it
 * is not zero, is where a smaller copy starts relative to blob_offset —
 * the copy that goes to a model, while the full one is for the person
 * looking at it. Zero means there is only one copy and it serves both.
 *
 * A photo is readable when the blob file is known to hold that range:
 * blob_offset + blob_size <= Friend_Context.blob_written. There is no
 * per-photo "is it there yet" flag because a photo's descriptor is
 * copied around — into the scrollback, into a fold — and a flag that has
 * to be set later would only be set on one of the copies. One number on
 * the context, compared, cannot go out of step with itself.
 *
 * No caption here. A photo's caption belongs to the memory that formed
 * around it, stored in its dialogue as an FR_TURN_CAPTION turn, in the
 * same order as the images — which is how the service held them too (two
 * parallel arrays), and keeps captions out of records that never use
 * them. */
#define FR_IMAGE_PENDING 0x1u /* described only: there are no pixels and
                               * none are coming */

typedef struct Fr_Image_Ref {
    u64 blob_offset;
    u32 blob_size;    /* the full JPEG */
    u32 thumb_offset; /* relative to blob_offset; 0 if none yet */
    u32 thumb_size;   /* the <=256px preview kept on memories */
    u32 flags;
} Fr_Image_Ref;

/* ------------------------------------------------------------------ *
 *  A memory
 * ------------------------------------------------------------------ */

typedef struct Fr_Memory_Record {
    /* Identity. generation is bumped every time the slot is freed, so a
     * Friend_Memory_Id held by a frontend across a prune is detectably
     * stale rather than quietly pointing at someone else's memory. */
    u32 generation;
    u32 next_free;

    u8  occupied;
    u8  in_this_epoch;    /* only in-epoch memories are prune candidates */
    u8  image_count;
    u8  needs_embedding;  /* no vector yet, or one from another model */
    /* Formed by a consolidation that has not finished: in a slot but not
     * yet in the graph, so nothing reads it, saves it or prunes it until
     * the fold has joined it or given it up. */
    u8  staged;
    u16 description_len;
    u16 dialog_len;

    u64 created_unix_ms;
    u64 dialog_end_unix_ms; /* the time address used when embedding it */

    /* Usage tracking, the basis for pruning. A memory counts as used when
     * the reranker picked it and it reached the prompt. */
    u64 use_count;
    u64 uses_this_epoch;

    /* Intrusive adjacency: no edge list is ever sorted or rebuilt when the
     * graph mutates, which the service did on every insert. */
    u32 first_out;
    u32 first_in;

    Fr_Image_Ref images[FR_MEMORY_IMAGES_MAX];

    i8   embedding[FR_EMBED_DIMS];
    char description[FR_DESCRIPTION_MAX];
    char dialog[FR_DIALOG_MAX];
} Fr_Memory_Record;

typedef struct Fr_Edge {
    u32 from_slot;
    u32 to_slot;
    u32 next_from; /* next edge out of from_slot */
    u32 next_to;   /* next edge into to_slot */
    u8  relation;  /* Friend_Relation */
    u8  occupied;
    u16 reserved0;
} Fr_Edge;

/* ------------------------------------------------------------------ *
 *  A message
 *
 *  Everything about a message but its contents is one field list, kept
 *  in two places: inline at the front of a Fr_Chat_Event, and packed
 *  ahead of the text in the scrollback's stream. Moving a message between
 *  the two copies the header whole, so a field added here reaches both
 *  without anybody remembering to. The save file writes it field by
 *  field, in fr_save.c.
 *
 *  A list rather than a member, so a message's fields read e->index and
 *  not e->header.index. The asserts below hold the two layouts together.
 * ------------------------------------------------------------------ */

#define FR_MESSAGE_HEADER_FIELDS                                            \
    u64 index; /* monotonic for the life of the friend; gaps are fine */    \
    u64 occurred_unix_ms;                                                   \
    f32 interrupted_fraction; /* >0: the user cut this reply off */         \
    u16 text_len;                                                           \
    u16 captions_len;                                                       \
    u8  kind;               /* Friend_Message_Kind */                       \
    u8  was_transcribed;    /* so the friend can allow for mishearing */    \
    /* An ambient event is either a summary the friend wrote for itself  \
     * or a raw transcription it overheard. The model is told which, in  \
     * different words, so the distinction has to survive. */            \
    u8  ambient_raw;                                                        \
    u8  image_count;                                                        \
    u8  reserved[4];

typedef struct Fr_Message_Header {
    FR_MESSAGE_HEADER_FIELDS
} Fr_Message_Header;

typedef struct Fr_Chat_Event {
    FR_MESSAGE_HEADER_FIELDS
    /* Not saved: a reply that was still arriving when the program closed
     * is not a message anybody sent. */
    u8 streaming;

    Fr_Image_Ref images[FR_CHAT_IMAGES_MAX];
    char         text[FR_CHAT_TEXT_MAX];
    char         captions[FR_CAPTIONS_MAX];
} Fr_Chat_Event;

/* The longest message there is, as the scrollback packs it, and the
 * smallest ring limits.scrollback_bytes is allowed to ask for: one that
 * holds that message several times over, because a scrollback that cannot
 * hold one record drops a message as it takes it. */
#define FR_SCROLL_RECORD_MAX                                               \
    (sizeof(Fr_Message_Header) + sizeof(Fr_Image_Ref) * FR_CHAT_IMAGES_MAX + \
     FR_CHAT_TEXT_MAX + FR_CAPTIONS_MAX)
#define FR_SCROLL_BYTES_MIN FR_KB(64)
FR_STATIC_ASSERT(FR_SCROLL_BYTES_MIN >= 4 * FR_SCROLL_RECORD_MAX,
                 scrollback_floor_holds_the_longest_message);

FR_STATIC_ASSERT(sizeof(Fr_Message_Header) == 32, message_header_unpadded);
FR_STATIC_ASSERT(offsetof(Fr_Chat_Event, reserved) ==
                     offsetof(Fr_Message_Header, reserved),
                 message_header_is_event_prefix);
FR_STATIC_ASSERT(offsetof(Fr_Chat_Event, streaming) ==
                     sizeof(Fr_Message_Header),
                 message_header_ends_where_event_goes_on);

static inline void fr_message_header_of(const Fr_Chat_Event *e,
                                        Fr_Message_Header *out)
{
    memcpy(out, e, sizeof(*out));
}

static inline void fr_message_header_into(Fr_Chat_Event *e,
                                          const Fr_Message_Header *header)
{
    memcpy(e, header, sizeof(*header));
}

/* The caption of the nth photo on a message, or nothing. */
static String fr_caption_at(String captions, i32 index);
static void   fr_set_captions(Fr_Chat_Event *e, const String *captions, i32 count);

static inline String fr_chat_captions(const Fr_Chat_Event *e)
{
    return str_make(e->captions, (i32)e->captions_len);
}

/* ------------------------------------------------------------------ *
 *  The scrollback
 *
 *  Messages that have left the window the model sees, kept so the user
 *  can scroll through their conversation. Fixed slots are right for a
 *  memory, whose dialogue is read back and re-rendered; they are wrong
 *  here, because this is the one thing in the friend that grows with how
 *  long somebody has been talking and a typed message is two hundred
 *  bytes in an eight kilobyte box. A thousand of those filled the ring
 *  the first version had, which is a week of talking.
 *
 *  So the scrollback is a packed byte stream with an index over it: a
 *  header and the text, one after another, in a ring, and a second ring
 *  of stream positions so a frontend can still ask for message 4,812
 *  without walking. Thirty-two bytes and the text, against eight
 *  kilobytes: the same memory holds forty times the conversation, and
 *  the budget it comes out of is reserved rather than resident, so the
 *  default is a quarter of a million messages.
 *
 *  Records never straddle the wrap — the tail is skipped instead — so
 *  every message's text is contiguous and friend_message can hand back a
 *  view into it like everything else here.
 * ------------------------------------------------------------------ */

/* One message, read out of either place it can be. Everything is a copy
 * but the text and captions, which point into the record and stay valid
 * until that message falls off the front of the scrollback — the same
 * lifetime every other view in this library has. */
typedef struct Fr_Scroll_View {
    Fr_Message_Header header;
    b32               streaming;
    Fr_Image_Ref      images[FR_CHAT_IMAGES_MAX];
    String            text;
    String            captions;
} Fr_Scroll_View;

typedef struct Fr_Scrollback {
    Fr_Pages bytes;     /* the stream */
    Fr_Pages positions; /* u64 stream position per message */
    i32      capacity;  /* messages the index can hold */
    i32      count;
    i32      first;     /* oldest message's slot in the position ring */
    u64      written;   /* stream position: bytes ever written, wrap
                         * padding included, so a position is a name that
                         * is never reused */

    /* What fell off the front. Two numbers because they mean different
     * things to the person: messages they can no longer scroll back to,
     * and messages that left before anything was remembered from them,
     * which is the only one of the two that is a real loss. */
    u64 lost;
    u64 unremembered;
    b32 reported_loss;
} Fr_Scrollback;

/* Appends one. Only fails if the message cannot fit in an empty ring,
 * which normalize_limits rules out, or if committing the pages it needs
 * is refused. */
static b32 fr_scroll_push(Friend_Context *fr, const Fr_Chat_Event *event);

/* Reads one, oldest first. */
static b32 fr_scroll_view(Friend_Context *fr, i32 index, Fr_Scroll_View *out);

/* The same view of a message in the window the model sees. */
static void fr_event_view(const Fr_Chat_Event *event, Fr_Scroll_View *out);

/* The conversation as one list: the scrollback, oldest first, then the
 * window. 0 past the end. */
static b32 fr_message_at(Friend_Context *fr, i32 index, Fr_Scroll_View *out);

/* Back into a full record, for consolidation taking its own copy and for
 * a load putting messages back. */
static void fr_scroll_to_event(const Fr_Scroll_View *view, Fr_Chat_Event *out);

/* Where a known event index sits in the scrollback now, or -1 if it has
 * fallen off the front. Walks from the end a frontend is more likely to
 * be asking about. */
static i32 fr_scroll_find(Friend_Context *fr, u64 event_index);

static void fr_scroll_reset(Friend_Context *fr);

/* ------------------------------------------------------------------ *
 *  A turn in progress
 *
 *  One reply is a pipeline with model calls between the stages, so what
 *  it has worked out so far has to live somewhere across ticks. It lives
 *  here, in the context, at a fixed size: a recall run, which memories
 *  reached the prompt, and the reply being filled in. No allocation to
 *  start one.
 * ------------------------------------------------------------------ */

#define FR_RECALL_QUERIES 2
#define FR_CANDIDATES_MAX 32
#define FR_RELEVANT_MAX 64
#define FR_RECALLED_MAX 96

typedef struct Fr_Relevant {
    u32 slot;
    f32 strength;
    u8  from_reranker;
} Fr_Relevant;

/* One recall: query windows embedded, each one's nearest memories
 * reranked, and what survived. A reply and consolidation each run one;
 * fr_rank.c is the run. */
typedef enum Fr_Recall_Stage {
    FR_RECALL_IDLE = 0,
    FR_RECALL_EMBEDDING, /* the queries are out */
    FR_RECALL_RERANKING, /* a model is choosing among the candidates */
    FR_RECALL_DONE
} Fr_Recall_Stage;

typedef struct Fr_Recall_Query {
    i8  embedding[FR_EMBED_DIMS];
    u32 candidates[FR_CANDIDATES_MAX];
    i32 candidate_count;
} Fr_Recall_Query;

typedef struct Fr_Recall {
    Fr_Recall_Stage stage;
    i32             pending;  /* requests still out in this stage */
    u64             serial;   /* carried by its requests, so a late answer
                               * to an earlier run is not taken for this
                               * one's */

    Fr_Recall_Query queries[FR_RECALL_QUERIES];
    Fr_Relevant     relevant[FR_RELEVANT_MAX];
    i32             relevant_count;
} Fr_Recall;

typedef enum Fr_Turn_Stage {
    FR_STAGE_IDLE = 0,
    FR_STAGE_RECALLING, /* the recall run is out */
    FR_STAGE_REPLYING   /* the reply is streaming in */
} Fr_Turn_Stage;

typedef struct Fr_Reply_Turn {
    Fr_Turn_Stage stage;
    Fr_Recall     recall;

    /* What reached the prompt, for usage tracking and so a frontend can
     * show which memories are in mind. */
    u32 recalled[FR_RECALLED_MAX];
    i32 recalled_count;

    /* The reply being filled in, by its event index rather than where it
     * sits: the window shifts under it while it streams. -1 when none. */
    i64 reply_index;
    i32 punctuation_seen;  /* the service cut replies off at sentence ends */
    b32 cut_short;
} Fr_Reply_Turn;

/* ------------------------------------------------------------------ *
 *  Voice
 *
 *  Everything a spoken turn needs between ticks, at a fixed size like
 *  everything else here. The microphone audio itself lives in the block
 *  (audio_in); this is the state around it.
 * ------------------------------------------------------------------ */

/* What comes back from a speech model is not ours to pick: every
 * provider's speech output is 24 kHz mono. */
#define FR_VOICE_RATE FRIEND_VOICE_RATE
#define FR_SPEAK_RATE 24000

/* 20 ms at 16 kHz: the frame the speech check works in. */
#define FR_VAD_FRAME 320

typedef struct Fr_Voice {
    Friend_Voice_State state;
    b32 open;    /* hands-free: the microphone stays on between turns */
    b32 pressed; /* a button is being held down */

    /* What the microphone has given us for this turn, resampled to
     * FR_VOICE_RATE and written from the front of audio_in. */
    i32 captured;
    b32 full; /* it ran past what one request can carry */

    /* Resampling the device's rate to ours, carried across calls so a
     * buffer boundary is not a click. */
    i32 in_rate;
    f32 phase;

    /* The speech check: energy against an adaptive floor, in 20 ms
     * frames. Not a model — Silero is an ONNX runtime, which is a
     * dynamic allocator and a hosted dependency, and this has to run
     * inside a library that has neither. */
    i16 frame[FR_VAD_FRAME];
    i32 frame_fill;
    i32 noise;      /* the floor speech is measured against, RMS */
    i32 speech_ms;  /* how much speech this turn has heard */
    i32 run_ms;     /* consecutively, which is what starts a turn */
    i32 silence_ms; /* and how long since the last of it */
    b32 heard;      /* speech has started */
    f32 level;      /* smoothed, for a meter */

    /* Speaking. The reply is cut into sentences as it streams and each
     * one is sent to be spoken the moment it is whole, so the friend
     * starts talking while the rest of its answer is still arriving. */
    b32  speak_reply;    /* the reply in progress is to be spoken */
    i32  speak_through;  /* bytes of it already handed to the segmenter */
    i32  segment_len;
    i32  spoken_len;     /* of it, handed to the speech model */
    char segment[FR_CHAT_TEXT_MAX];
    b32  in_tag;     /* the bracket-tag stripper, across chunks */
    b32  closed_tag;
    b32  spoke_any;  /* the first sentence goes early and alone */
    b32  text_done;  /* no more text is coming: flush the tail */
    b32  requested;  /* a sentence is out at the speech model now */
    u8   carry;      /* half a sample left over by a chunk boundary */
    b32  have_carry;
    /* When the audio handed out so far will have finished playing. A
     * frontend plays samples at real time by definition, so the clock
     * is a good enough answer to "is it still talking" — and it has to
     * be answered, because the microphone must not be reopened while
     * the speaker is still going or the friend interrupts itself. */
    u64  speak_until_ms;
} Fr_Voice;

/* ------------------------------------------------------------------ *
 *  The realtime session
 *
 *  A websocket, a system prompt frozen when it opened, and the two
 *  halves of the turn in progress. The audio does not live here: it
 *  goes out as the voice's microphone fills and comes back as events, so
 *  the only thing worth keeping between ticks is what was said.
 * ------------------------------------------------------------------ */

typedef enum Fr_Live_Stage {
    FR_LIVE_OFF = 0,
    FR_LIVE_OPENING, /* the socket is being opened */
    FR_LIVE_SETUP,   /* the setup went out; the provider has not said yes */
    FR_LIVE_READY    /* seeded, listening, and talking */
} Fr_Live_Stage;

/* The messages that steer a session rather than carry audio. Each is
 * owed until a slot can take it, and they go in the order they were
 * owed: a turn that ends before it has started is not a turn. */
typedef enum Fr_Live_Frame {
    FR_LIVE_SEND_SETUP = 0,
    FR_LIVE_SEND_RECAP,
    FR_LIVE_SEND_ACTIVITY_START, /* Gemini */
    FR_LIVE_SEND_ACTIVITY_END,
    FR_LIVE_SEND_CANCEL,         /* OpenAI */
    FR_LIVE_SEND_COMMIT,
    FR_LIVE_SEND_CREATE
} Fr_Live_Frame;

#define FR_LIVE_OWED_MAX 8

typedef struct Fr_Live {
    Fr_Live_Stage stage;
    u64 channel;         /* the open request's id, which is the socket */
    u64 opened_unix_ms;
    u64 heard_unix_ms;   /* anything inbound, for noticing a dead socket */
    b32 activity;        /* a turn is open at the far end */
    b32 responding;      /* the friend is talking */
    b32 recycle;         /* start a fresh session once this one is idle */
    i32 sent_through;    /* samples of this turn already on the wire */
    u8  owed[FR_LIVE_OWED_MAX];
    i32 owed_count;

    /* The turn, both halves, as the transcripts arrive. */
    char heard[FR_CHAT_TEXT_MAX];
    i32  heard_len;
    char said[FR_CHAT_TEXT_MAX];
    i32  said_len;
} Fr_Live;

/* ------------------------------------------------------------------ *
 *  Consolidation in progress
 *
 *  Turning a stretch of conversation into memories is a longer pipeline
 *  than a reply and it runs in the background while the user carries on
 *  talking, so it gets its own state. The events it is folding are moved
 *  out of the active window the moment it starts, which is what keeps
 *  everything downstream from caring about a message arriving mid-pass.
 * ------------------------------------------------------------------ */

#define FR_STAGED_MAX 48
#define FR_RELATE_FANOUT 3

/* The most messages one fold can be about. The service folded forty; the
 * cap is what the copy below is sized for. */
#define FR_FOLD_EVENTS_MAX 48

typedef enum Fr_Fold_Stage {
    FR_FOLD_IDLE = 0,
    FR_FOLD_RECALLING,       /* so extraction knows what is known */
    FR_FOLD_EXTRACTING,      /* the model is naming the events */
    FR_FOLD_EMBEDDING,       /* each new memory is being embedded */
    FR_FOLD_RELATING,        /* each new memory is being joined to the graph */
    FR_FOLD_INTERIORITY      /* the friend is rewriting what it wants */
} Fr_Fold_Stage;

typedef struct Fr_Fold {
    Fr_Fold_Stage stage;
    i32           pending; /* requests still out in this stage */

    /* The events being folded, copied into the fold's own room when it
     * starts. A copy rather than a span of the scrollback because this
     * is the path a message's content takes into memory, and it must not
     * depend on how long the scrollback happens to hold things: once the
     * fold has them, nothing that happens to the conversation can lose
     * them. first_event_index is kept so the fold knows which part of
     * the conversation it is accounting for. */
    u64 first_event_index;
    i32 event_count;

    /* Recall for the extraction prompt. One window, not two: the
     * service embedded the whole stretch being folded. */
    Fr_Recall recall;

    /* Memories the extraction produced, each marked staged until it has
     * a vector and its edges. */
    u32 staged[FR_STAGED_MAX];
    i32 staged_count;

    /* Which staged memory is being joined, and what the model said
     * about each of its three nearest neighbours. */
    i32 relating_at;
    u32 relate_to[FR_RELATE_FANOUT];
    u8  relate_as[FR_RELATE_FANOUT];
    i32 relate_count;
} Fr_Fold;

/* Work that failed and is tried again after a wait rather than every
 * tick, saying so once per run of failures. at is when it may go again,
 * which friend_next_deadline_ms counts as something to wake for. */
typedef struct Fr_Retry {
    u64 at;
    b32 reported;
} Fr_Retry;

/* It failed: wait. 1 when this is the first failure since it last
 * worked, which is the one worth telling anybody about. */
static inline b32 fr_retry_failed(Fr_Retry *r, u64 now_unix_ms, u64 wait_ms)
{
    b32 first   = !r->reported;
    r->at       = now_unix_ms + wait_ms;
    r->reported = 1;
    return first;
}

static inline void fr_retry_worked(Fr_Retry *r)
{
    r->at       = 0;
    r->reported = 0;
}

static inline b32 fr_retry_waiting(const Fr_Retry *r, u64 now_unix_ms)
{
    return now_unix_ms < r->at;
}

/* ------------------------------------------------------------------ *
 *  In-flight I/O
 * ------------------------------------------------------------------ */

typedef enum Fr_Io_Purpose {
    FR_IO_PURPOSE_NONE = 0,
    FR_IO_PURPOSE_REPLY,
    FR_IO_PURPOSE_EMBED_QUERY,
    FR_IO_PURPOSE_EMBED_DOCUMENT, /* a memory that arrived without a vector */
    FR_IO_PURPOSE_RERANK,
    /* Consolidation runs the same three kinds of call as a reply, but
     * its own answers land somewhere else, so they are told apart by
     * purpose rather than by asking what stage the friend is in. */
    FR_IO_PURPOSE_FOLD_EMBED_QUERY,
    FR_IO_PURPOSE_FOLD_RERANK,
    FR_IO_PURPOSE_FOLD_EMBED,
    FR_IO_PURPOSE_EXTRACT_EVENTS,
    FR_IO_PURPOSE_RELATE,
    FR_IO_PURPOSE_INTERIORITY,
    FR_IO_PURPOSE_CAPTION,
    FR_IO_PURPOSE_TRANSCRIBE,
    FR_IO_PURPOSE_SPEAK,
    FR_IO_PURPOSE_LIVE_SESSION,
    FR_IO_PURPOSE_BLOB_READ,
    FR_IO_PURPOSE_BLOB_WRITE
} Fr_Io_Purpose;

typedef enum Fr_Io_State {
    FR_IO_STATE_FREE = 0,
    FR_IO_STATE_READY,     /* waiting to be handed out by friend_pending_io */
    FR_IO_STATE_OUTSTANDING,
    FR_IO_STATE_DONE,      /* complete or failed; waiting to be consumed */
} Fr_Io_State;

typedef struct Fr_Io_Slot {
    /* What friend_pending_io hands the transport, as it will be handed:
     * every string in it is in the slot's own arena, put there by
     * fr_io_post, so nothing the request points at can change under it
     * between being built and being answered. */
    Friend_Io_Request req;
    Fr_Io_State       state;
    Fr_Io_Purpose     purpose;

    /* Every slot owns a fixed slice of the io arena. A bump arena cannot
     * give memory back out of order, and requests finish in whatever
     * order the network feels like, so there is one arena per slot
     * rather than one shared one with marks into it. The request is
     * built at the front of it and the receive buffer gets the rest. */
    Arena arena;

    /* How much of req.recv the transport has filled. */
    u32 recv_len;
    u32 consumed; /* how much of recv a streaming parser has already read */

    i32    status;
    b32    failed;
    String error;

    /* The friend stopped wanting this answer — the reply was cut off at
     * a sentence end, or the user interrupted. The slot stays alive
     * because the transport may still be writing into recv, until
     * friend_abandoned_io has told the transport, which releases it. */
    b32 abandoned;

    /* Which piece of work this belongs to, so a completion can be routed
     * without searching: a slot index, a memory slot, a candidate index.
     * subject_event is a number that outlives positions: a message's
     * event index, because the window shifts under anything that takes
     * a few seconds, or the serial of the recall run that asked. */
    i32 subject;
    u64 subject_event;

} Fr_Io_Slot;

/* ------------------------------------------------------------------ *
 *  Events waiting to be drained
 * ------------------------------------------------------------------ */

typedef struct Fr_Queued_Event {
    Friend_Event event;
    u64          message; /* event index + 1 of the message it is about; 0
                           * for none */
} Fr_Queued_Event;

/* ------------------------------------------------------------------ *
 *  Tuning
 *
 *  The service hardcoded these. They are knobs here because running
 *  locally changes what is reasonable, but the defaults are exactly what
 *  shipped, so an untouched config behaves like the friend people had.
 * ------------------------------------------------------------------ */

/* Each knob and what the service shipped, which is load bearing: it is
 * what the personality was tuned against. The names are Friend_Config's.
 *
 * The live bridge's number for the pre-roll: by the time anything notices
 * speech the speaker is already 600 ms into it. Its number for the
 * endpoint was 400 ms, and that one is not kept. It was measured against
 * a neural detector that can tell quiet speech from silence; the one in
 * here is energy against a floor and cannot, so at 400 ms it ended a turn
 * in the middle of both recordings it was tested on. 600 is where they
 * survive. The difference is a fifth of a second against a reply that
 * takes three, and being cut off mid-sentence is the worse of the two. */
#define FR_TUNING_FIELDS(X)                                               \
    X(consolidate_at, 50)                                                 \
    X(consolidate_leaving, 20)                                            \
    X(consolidate_max, 40)                                                \
    X(recall_candidates, 16)                                              \
    X(recall_keep, 3)                                                     \
    X(recall_recent, 3)                                                   \
    X(max_timelines, 10)                                                  \
    X(prune_min_memories, 50)                                             \
    X(prune_epoch_min, 300)                                               \
    X(voice_endpoint_ms, 600)                                             \
    X(voice_preroll_ms, 600)

typedef struct Fr_Tuning {
#define FR_TUNING_MEMBER(name, shipped) i32 name;
    FR_TUNING_FIELDS(FR_TUNING_MEMBER)
#undef FR_TUNING_MEMBER
} Fr_Tuning;

/* The most newest memories recall adds on its own. */
#define FR_RECENT_MAX 8

/* ------------------------------------------------------------------ *
 *  Layout
 *
 *  Computed as offsets, not pointers, so friend_required_memory() can ask
 *  for the total without a block to point into and the two can never
 *  disagree about the shape.
 * ------------------------------------------------------------------ */

typedef struct Fr_Region {
    size_t offset;
    size_t size;
} Fr_Region;

typedef struct Fr_Layout {
    Friend_Limits limits; /* normalized: what the layout was made for */
    size_t        total;

    /* Everything below this offset is written during friend_init, so it
     * is the part of the block that has to be real before anything
     * happens. The rest is reserved and backed as the friend grows into
     * it, which is the whole reason the limits above can be generous. */
    size_t eager;

    Fr_Region context;
    Fr_Region active_events;
    Fr_Region fold_events;
    Fr_Region io_slots;
    Fr_Region events;
    Fr_Region config_arena;
    Fr_Region prompt_page;
    Fr_Region scroll_positions;
    Fr_Region scroll_bytes;
    Fr_Region edges;
    Fr_Region memories;
    Fr_Region io_arena;
    Fr_Region scratch_arena;
    Fr_Region image_page;
    Fr_Region image_staging;
    Fr_Region audio_in;

    i32    max_events_queued;
    i32    voice_samples; /* one spoken turn, mono 16 kHz */
    size_t io_slot_bytes; /* each request slot's slice of io_arena */
} Fr_Layout;

static Friend_Limits fr_normalize_limits(Friend_Limits limits);
static Fr_Layout     fr_layout(Friend_Limits limits);

/* ------------------------------------------------------------------ *
 *  The context
 * ------------------------------------------------------------------ */

#define FR_ERROR_MAX 512

/* friend.h already declares the typedef; this is the definition. */
struct Friend_Context {
    Fr_Layout     layout;
    Fr_Tuning     tuning;

    /* The caller's block, for save/load and for reporting usage. */
    void  *block;
    size_t block_size;

    /* Configuration, with its strings copied into config_arena so the
     * caller's buffers need not outlive the call. */
    Friend_Provider provider;
    String          api_key;
    String          base_url;
    String          friend_name;
    String          user_name;
    String          prompt_override;
    String          blob_path;
    Friend_Models   models;
    i32             utc_offset_minutes;
    b32             embodied;
    b32             enable_images;
    b32             enable_voice;
    String          voice_name;
    b32             speak_replies;
    b32             voice_barge_in;
    i32             voice_mode; /* Friend_Voice_Mode */
    u64             random_seed;
    u64             random_state;
    /* A seed of zero means "do not fix it", so the state is taken from
     * the clock. Without that, every session would flip the same coins
     * in the same order and the friend would cut its replies off in
     * exactly the same places every time. */
    b32             random_seeded;

    Arena config_arena;
    Arena io_arena;
    /* Where the last prompt is kept so it can be read back after the
     * reply it produced has arrived. Separate because the request it was
     * sent in has long since been released. */
    Arena prompt_page;
    Arena scratch;       /* reset by every friend_tick */
    Arena image_page;    /* one photo at a time, reset on each use */

    /* ------------------------------------------------------------- *
     *  Photographs
     *
     *  The pixels are in a file; the records hold offsets into it. A
     *  photo arrives — from an import, or from the person attaching one
     *  — as bytes in the staging arena, and is given its place in the
     *  file at that moment, before anything has been written. That is
     *  what lets the descriptor be copied around freely: its offset is
     *  final the instant it exists.
     *
     *  blob_bytes is where the next photo will go. blob_written is how
     *  much of the file is really there, which only moves when the
     *  transport says a write landed, and only forwards in one run, so
     *  a photo is readable exactly when the file reaches past its end.
     *  Everything between the two is in staging, and only in staging:
     *  it is where a photo lives for the few ticks between arriving and
     *  being written, and where it is read from in the meantime.
     * ------------------------------------------------------------- */
    Arena image_staging;
    u64   blob_bytes;    /* the append cursor: offsets are handed out here */
    u64   blob_written;  /* what the file is known to hold */
    u64   staging_base;  /* the blob offset staging starts at */
    size_t staging_sent; /* bytes of staging already written */
    u32   staging_writing; /* bytes in the write that is out now, 0 if none */
    Fr_Retry blob_write_retry;

    /* The one photo paged in for looking at, and which it is. A read
     * that failed is remembered against that photo for a while, so
     * asking again every frame does not read again every frame. */
    u64 image_page_offset;
    u32 image_page_size;
    b32 image_page_ready;
    b32      image_page_failed;
    Fr_Retry image_read_retry;
    u64      image_read_id;

    /* How the block grows. Zero when the caller handed us plain memory,
     * in which case every region is real from the start and none of this
     * does anything. Every region commits through fr_commit, which adds
     * up what it has been given in bytes_committed. */
    Friend_Commit_Fn commit;
    void            *commit_user;
    size_t           bytes_committed;

    /* Memories.
     *
     * Slots are handed out by bumping a watermark and reusing holes in
     * what is below it, rather than from a free list threaded through
     * the whole capacity at startup. That is what keeps a capacity of a
     * hundred thousand from costing anything: nothing walks or writes a
     * slot the friend has not reached, so nothing commits its pages
     * either, and every loop over the graph stops at the watermark. */
    Fr_Memory_Record *memories;
    Fr_Pages          memory_pages;
    i32               memory_capacity;
    i32               memory_slots;      /* watermark: slots in use */
    i32               memory_slots_ever; /* ...and ever written, which is
                                          * where a generation count can
                                          * still be read */
    i32               memory_count; /* occupied slots */
    u32               memory_free_list;

    Fr_Edge *edges;
    Fr_Pages edge_pages;
    i32      edge_capacity;
    i32      edge_slots;
    i32      edge_count;
    u32      edge_free_list;

    /* Conversation. active_events is what the model sees; the scrollback
     * is everything that has left it. */
    Fr_Chat_Event *active_events;
    i32            active_capacity;
    i32            active_count;

    Fr_Scrollback scroll;

    /* Consolidation's own copy of the stretch it is folding. */
    Fr_Chat_Event *fold_events;

    u64 total_event_count; /* next event index */

    /* One past the last event any memory has been formed from. What the
     * scrollback checks before dropping a message, and where the next
     * fold starts looking. */
    u64 folded_through;
    /* Consolidation can fail; the stretch it failed on is tried again
     * rather than written off, but not forever. */
    u64 fold_attempt_index;
    i32 fold_attempts;

    /* The friend's interior monologue, rewritten after consolidation, and
     * the name an import brought with it. Both have a buffer of their own
     * rather than a slice of the config arena, because they are replaced
     * at runtime and a bump arena has no way to take one back. */
    char interiority_buffer[FR_INTERIORITY_MAX];
    i32  interiority_len;
    char name_buffer[FR_NAME_MAX];

    u64 created_unix_ms; /* when the friend was first made */

    /* Usage epochs for pruning. */
    u64 replies_total;
    u64 epoch_samples;
    b32 epoch_active;

    /* I/O */
    Fr_Io_Slot *io_slots;
    i32         io_capacity;
    i32         io_inflight;
    u64         next_io_id;

    /* Events out, a ring drained by friend_next_event. */
    Fr_Queued_Event *event_queue;
    i32           event_capacity;
    i32           event_count;
    i32           event_first;
    b32           event_overflowed;

    /* One spoken turn of microphone audio, and the state around it. */
    i16     *audio_in;
    i32      audio_capacity;
    Fr_Voice voice;
    Fr_Live  live;

    /* The reply in progress, and the prompt it was built from, kept so
     * friend_last_prompt can hand it back. */
    Fr_Reply_Turn turn;
    Fr_Fold       fold;
    Fr_Retry      backfill_retry; /* memories that came without a vector */
    String        last_prompt;

    u64 now_unix_ms;
    /* Changed since the last friend_save or friend_load: Friend_Stats.unsaved. */
    b32 unsaved;

    /* Last error, copied so it survives the scratch reset. */
    char error_buffer[FR_ERROR_MAX];
    i32  error_len;
};

/* Appends one turn. When the 4 KiB is full the oldest turns go first,
 * which is what the dialogue is for: context for the description, not a
 * transcript of record. Returns 0 only when the text cannot fit at all.
 * Image captions are turns too, so appending may renumber nothing — the
 * captions are found by walking, never by offset. */
static b32 fr_dialog_append(Fr_Memory_Record *m, u64 at_unix_ms, Fr_Turn_Kind kind,
                     String text);

/* Walks it. Start with cursor = fr_memory_dialog(m); returns 0 at the
 * end, and stops rather than guessing if the stream is malformed. */
static b32 fr_dialog_next(String *cursor, u64 *out_at, Fr_Turn_Kind *out_kind,
                   String *out_text);

/* Truncated to the record's room, on a code point boundary. */
static void fr_set_description(Fr_Memory_Record *m, String text);

/* The photo captions on a memory, joined with a space, in image order. */
static String fr_memory_captions(Arena *arena, const Fr_Memory_Record *m);

/* Shared internals. */

/* Back to an empty friend, keeping the configuration and the block. What
 * friend_begin, friend_load and friend_import_export all start with.
 * Requests still out for the friend being replaced are abandoned, and so
 * is a voice or realtime turn: their answers belong to somebody else. */
static void fr_state_clear(Friend_Context *fr);

/* From random_seed, or from the clock when it is zero. */
static void fr_random_seed(Friend_Context *fr);
static u32  fr_random_u32(Friend_Context *fr);

/* Both truncate to the record's room, on a code point boundary. */
static void fr_set_name(Friend_Context *fr, String name);
static void fr_set_interiority(Friend_Context *fr, String text);

/* A message from the user and the reply to it. friend_say is this with
 * was_spoken 0; the voice side is this with 1, which is the only
 * difference between talking to the friend and typing to it. */
static Friend_Result fr_say(Friend_Context *fr, Friend_String text,
                     const Friend_Attachment *attachments, i32 count,
                     b32 was_spoken);

/* No conversation and nothing being answered, for fr_state_clear. */
static void fr_turn_reset(Friend_Context *fr);

/* A reply is finished — written by the friend's own model, or said by a
 * realtime one — and is a message like any other: the frontend is told,
 * the friend is worth saving, and the window may have grown past where
 * its oldest should become memories. */
static void fr_reply_finished(Friend_Context *fr, Fr_Chat_Event *reply);

/* A message joins the end of the window, which retires its oldest when
 * it is full. Never fails. */
static Fr_Chat_Event *fr_commit_event(Friend_Context *fr, Friend_Message_Kind kind,
                               String text, b32 was_transcribed);

/* Where the message with this event index sits in the window now, or -1
 * if it has left it. */
static i32 fr_active_find(Friend_Context *fr, u64 event_index);

/* Where it sits in the conversation friend_message reads, or -1 if it is
 * no longer in it. */
static i32 fr_message_position(Friend_Context *fr, u64 event_index);

static Friend_Result fr_error(Friend_Context *fr, Friend_Result result, String message);

/* ------------------------------------------------------------------ *
 *  Events (fr_events.c)
 * ------------------------------------------------------------------ */

static void fr_events_reset(Friend_Context *fr);
static void fr_push_event(Friend_Context *fr, Friend_Event event);
static void fr_push_event_text(Friend_Context *fr, Friend_Event_Kind kind,
                        String text);
/* About the message with this event index, which friend_next_event
 * turns into the position friend_message reads it at. */
static void fr_push_message_event(Friend_Context *fr, Friend_Event_Kind kind,
                           String text, u64 message);
static void fr_push_memories_changed(Friend_Context *fr);

/* Something a save would write has changed. The first change since the
 * friend was last saved or loaded says so with FRIEND_EVENT_STATE_DIRTY;
 * every change after it is already covered. */
static void fr_mark_dirty(Friend_Context *fr);

/* The oldest `count` messages leave the window the model sees and join
 * the scrollback the frontend scrolls through. Consolidation does this
 * when it starts; the window does it on its own when it fills, because a
 * full window must not stop the conversation. */
static void fr_retire_active(Friend_Context *fr, i32 count);

/* How many of the messages the friend has not formed any memory from yet
 * — the ones from folded_through onwards — are in the scrollback rather
 * than the window. */
static i32 fr_unfolded_in_scrollback(Friend_Context *fr);

/* ------------------------------------------------------------------ *
 *  Photographs
 * ------------------------------------------------------------------ */

/* No photographs: staging empty, nothing written, nothing paged in. */
static void fr_blob_reset(Friend_Context *fr);

/* Takes a copy of the bytes, gives them a place in the blob file, and
 * fills in the descriptor. Fails — leaving the descriptor marked
 * FR_IMAGE_PENDING, which means described with no pixels — when there is
 * no blob_path to write to or no room left in staging. */
static b32 fr_blob_stage(Friend_Context *fr, String jpeg, String thumb,
                  Fr_Image_Ref *out);

/* The same, from base64 in a document we are reading: decoded in place
 * rather than through a copy. */
static b32 fr_blob_stage_base64(Friend_Context *fr, String jpeg, String thumb,
                         Fr_Image_Ref *out);

/* What a model should be shown of a photo: the small copy when there is
 * one. Empty when the pixels are not in hand. */
static String fr_blob_for_model(Friend_Context *fr, const Fr_Image_Ref *ref);

/* Called from the pump: start the next write if there is one, and
 * account for a write or a read that came back. */
static void fr_blob_drain(Friend_Context *fr);
static void fr_blob_route(Friend_Context *fr, Fr_Io_Slot *slot);

/* The file is known to hold this photo's bytes. */
static b32 fr_blob_on_disk(const Friend_Context *fr, const Fr_Image_Ref *ref);

/* ------------------------------------------------------------------ *
 *  The graph (fr_graph.c)
 * ------------------------------------------------------------------ */

/* An empty graph, for fr_state_clear. */
static void fr_graph_reset(Friend_Context *fr);

/* Both return FR_SLOT_NONE when full, or when the machine would not give
 * us the pages for one more. A memory comes back occupied, with a fresh
 * generation and nothing else in it. */
static u32  fr_memory_alloc(Friend_Context *fr);
static void fr_memory_free(Friend_Context *fr, u32 slot);
static u32  fr_edge_alloc(Friend_Context *fr);
static void fr_edge_free(Friend_Context *fr, u32 slot);

/* The same as fr_memory_alloc, but at one particular slot, for a load
 * that is putting records back where they were saved. The generation is
 * the caller's to set. Returns 0 if the slot is out of range, already
 * taken, or its pages cannot be had. */
static Fr_Memory_Record *fr_memory_claim(Friend_Context *fr, u32 slot);

/* An edge from one memory to another, threaded onto both ends' lists.
 * Newest first: nothing depends on the order, and prepending is O(1).
 * Returns 0 if either end is not a memory, they are the same one, or
 * there is no room. */
static b32 fr_edge_add(Friend_Context *fr, u32 from, u32 to, u8 relation);

/* Joins a staged memory to the graph: one edge per connected component
 * the related memories fall into, from the most recent memory of that
 * component to the new one. The service's connectToRelated. */
static void fr_connect_to_related(Friend_Context *fr, u32 new_slot, const u32 *related,
                           const u8 *relations, i32 count);

/* Removes a memory and bridges its predecessors to its successors, so
 * the chains that ran through it still run. */
static void fr_drop_memory(Friend_Context *fr, u32 slot);

/* Both free lists, built around whatever is occupied. For a load, which
 * places records at the slots they were saved at rather than taking the
 * next one offered. */
static void fr_rebuild_free_lists(Friend_Context *fr);

/* Against the watermark, not the capacity: a slot above the watermark has
 * never been written, so reading its occupied flag would be reading
 * whatever the operating system handed us. */
static inline Fr_Memory_Record *fr_memory(Friend_Context *fr, u32 slot)
{
    if (slot >= (u32)fr->memory_slots) return 0;
    if (!fr->memories[slot].occupied) return 0;
    return &fr->memories[slot];
}

static inline Fr_Edge *fr_edge(Friend_Context *fr, u32 slot)
{
    if (slot >= (u32)fr->edge_slots) return 0;
    if (!fr->edges[slot].occupied) return 0;
    return &fr->edges[slot];
}

/* A memory recall can find: in the graph, with a vector to compare. */
static inline b32 fr_memory_comparable(const Fr_Memory_Record *m)
{
    return m->occupied && !m->staged && !m->needs_embedding;
}

static inline String fr_memory_description(const Fr_Memory_Record *m)

{
    return str_make(m->description, (i32)m->description_len);
}

static inline String fr_memory_dialog(const Fr_Memory_Record *m)
{
    return str_make(m->dialog, (i32)m->dialog_len);
}

/* Whether a model is ever shown this message. Notices are for the
 * person reading the window. A raw overheard transcript is not: only the
 * summary the friend wrote for itself is, which friend.h promises. And a
 * reply still arriving is not yet something anybody said. */
static inline b32 fr_event_for_model(const Fr_Chat_Event *e)
{
    if (e->kind == FRIEND_MESSAGE_NOTICE) return 0;
    if (e->kind == FRIEND_MESSAGE_AMBIENT && e->ambient_raw) return 0;
    return !e->streaming;
}

static inline String fr_chat_text(const Fr_Chat_Event *e)
{
    return str_make(e->text, (i32)e->text_len);
}

#endif /* FR_STATE_H */
