/* friend.h — the friend. Memory, recall, personality, voice.
 *
 * C99. No dynamic allocation: you hand it one block of memory at startup
 * and it never asks for more. No platform I/O: it tells you what calls to
 * make and you hand the bytes back, so the same library runs behind a
 * desktop GUI, a terminal, a phone app, or a replay harness in a test.
 *
 * The whole API is this header. Everything else in the repo is either an
 * implementation of it or an optional helper you may ignore.
 *
 * Lifetimes: every Friend_String and every pointer this API returns points
 * into your block and stays valid until the next friend_tick(). Copy out
 * anything you need to keep. Nothing returned is null-terminated; use the
 * length.
 *
 * Threading: one context, one thread. The context is never touched from
 * inside your transport. Two contexts in two threads never interact.
 */

#ifndef FRIEND_H
#define FRIEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Friend_Context Friend_Context;

/* The longest name a friend can have, in bytes. Stated here because a
 * frontend that lists friends has to put them somewhere. */
#define FRIEND_NAME_MAX 64

/* The most photographs one message carries, which is what the service
 * allowed. friend_say keeps the first this many and drops the rest. */
#define FRIEND_ATTACHMENTS_MAX 5

/* The rate the friend listens at, mono: what it hands a transcriber, and
 * what a frontend whose device can be asked for a rate should ask for.
 * friend_voice_push resamples anything else to it. */
#define FRIEND_VOICE_RATE 16000

/* A view into memory you do not own. len is bytes, never a code point count. */
typedef struct Friend_String {
    const char *data;
    int32_t     len;
} Friend_String;

#define FRIEND_LIT(s) ((Friend_String){ (s), (int32_t)(sizeof(s) - 1) })

typedef enum Friend_Result {
    FRIEND_OK = 0,
    FRIEND_ERR_BAD_ARGUMENT,
    FRIEND_ERR_TOO_SMALL,       /* the block you gave is smaller than required */
    FRIEND_ERR_FULL,            /* a fixed capacity in Friend_Limits is exhausted */
    FRIEND_ERR_BAD_STATE,       /* operation does not apply right now */
    FRIEND_ERR_BAD_DATA,        /* a save file or import was malformed */
    FRIEND_ERR_PROVIDER,        /* the model provider refused or misbehaved */
    FRIEND_ERR_IO              /* you reported a transport failure */
} Friend_Result;

/* Last error in detail, for logs and for showing the user. Valid until the
 * next call that returns a Friend_Result. */
Friend_String friend_last_error(Friend_Context *fr);

/* ------------------------------------------------------------------ *
 *  Memory
 * ------------------------------------------------------------------ */

typedef struct Friend_Limits {
    int32_t max_memories;        /* graph nodes.  default 131072 (~730 MiB) */
    int32_t max_connections;     /* graph edges.  default 524288            */
    int32_t max_active_events;   /* chat window the model sees. default 64  */
    int32_t max_scrollback;      /* scrollback entries.         default 262144 */
    int32_t scrollback_bytes;    /* their text, packed.         default 64 MiB */
    int32_t max_inflight_io;     /* outstanding requests.       default 32  */
    int32_t io_bytes;            /* request+response arena.     default 64 MiB */
    int32_t scratch_bytes;       /* reset every tick.           default 16 MiB */
    int32_t image_page_bytes;    /* one photo at a time.        default 2 MiB  */
    int32_t image_staging_bytes; /* photos not yet on disk.     default 64 MiB */
    /* The longest thing you can say in one go, in milliseconds. It is
     * what the microphone buffer is sized for, and it is bounded by what
     * one request can carry: the audio goes to a transcriber in one
     * piece, and a request has to fit a single I/O slot (io_bytes
     * divided by max_inflight_io). A spoken turn that reaches it is
     * ended there and answered rather than lost. default 20000 */
    int32_t voice_press_ms;
} Friend_Limits;

Friend_Limits friend_default_limits(void);

/* Exact size of the block friend_init needs for these limits. Pure.
 *
 * The defaults ask for about 950 megabytes, which is a number
 * to reserve rather than a number to allocate: see Friend_Config.commit,
 * and Friend_Stats.bytes_committed for what a running friend really
 * occupies. A friend of ordinary size touches a few megabytes of it and
 * the rest is addresses nobody paid for. */
size_t friend_required_memory(Friend_Limits limits);

/* Hand friend_init a block of reserved address space and a function that
 * backs part of it with real pages, and the friend commits as it grows
 * into the block instead of all at once. Memory, not I/O: it is called
 * from inside the library, synchronously, and must either succeed or
 * return 0 — in which case whatever the friend was doing fails the way
 * running out of a fixed capacity fails, with an error and no crash.
 *
 * Must be idempotent, and must leave the contents of pages that are
 * already committed alone. platform/fr_virtual.h is this in both forms
 * that exist (mprotect, VirtualAlloc); the `user` pointer is there for
 * anyone whose allocator needs it. NULL means the block is memory you
 * already own, all of it, which is what malloc hands you. */
typedef int32_t (*Friend_Commit_Fn)(void *user, void *at, size_t bytes);

/* ------------------------------------------------------------------ *
 *  Configuration
 * ------------------------------------------------------------------ */

typedef enum Friend_Provider {
    FRIEND_PROVIDER_GEMINI = 0,  /* generativelanguage.googleapis.com         */
    FRIEND_PROVIDER_OPENAI_COMPATIBLE /* OpenAI, OpenRouter, llama.cpp, Ollama */
} Friend_Provider;

/* Which model does what. Leave a field empty for the provider's default,
 * which friend_default_models says. The kinds of call do not want the
 * same model: replying is latency-critical and cheap, extraction and
 * interiority want reasoning, embedding and reranking want to be fast. */
typedef struct Friend_Models {
    Friend_String chat;        /* the reply.                 gemini-3-flash-preview   */
    Friend_String extract;     /* events out of dialogue.    gemini-3-flash-preview   */
    Friend_String relate;      /* pairwise edge classify.    gemini-2.5-flash         */
    Friend_String interiority; /* the interior monologue.    gemini-3-flash-preview   */
    Friend_String rerank;      /* 16 candidates down to 3.   gemini-2.5-flash         */
    Friend_String embed;       /* 768-dim.                   gemini-embedding-2       */
    Friend_String vision;      /* photo captions.            gemini-3-flash-preview   */
    Friend_String transcribe;  /* what you said.             gemini-2.5-flash         */
    Friend_String speak;       /* what it says back.   gemini-2.5-flash-preview-tts    */
    Friend_String live;        /* the realtime session. gemini-2.5-flash-native-audio-latest */
} Friend_Models;

typedef struct Friend_Config {
    Friend_Limits   limits;
    Friend_Provider provider;
    Friend_String   api_key;      /* copied in. never read from the environment */
    Friend_String   base_url;     /* empty for the provider's own endpoint      */
    Friend_Models   models;

    /* What the user calls it, read once by friend_init. After that the
     * name belongs to the friend — friend_begin, friend_load and
     * friend_import_export give it one — and friend_reconfigure leaves
     * it alone. */
    Friend_String   friend_name;
    Friend_String   user_name;    /* what it calls the user; empty is
                                   * "the user"                            */

    /* Where photo bytes are kept. The friend appends to this file
     * through the I/O pump and reads a photo back out of it when
     * somebody wants to look at one; a message or a memory holds only an
     * offset and a length into it, which is why saving a friend with a
     * thousand photographs is still a small file.
     *
     * Leave it empty and photographs are described but not kept: an
     * import brings their captions and sizes across and throws the
     * pixels away, which is the right behaviour for a tool that is only
     * reading a friend. The file is written append-only and is never
     * rewritten, so it is safe to copy while the friend is running. */
    Friend_String   blob_path;
    int32_t         utc_offset_minutes;

    /* Personality. Empty prompts fall back to the embedded interiority2
     * defaults, so a fresh install needs no files at all. */
    Friend_String   prompt_override;   /* whole prompt config, JSON             */
    int32_t         embodied;          /* 1 when a pendant is connected: keeps
                                        * the physical-pendant language in the
                                        * system prompt. 0 swaps it out.        */

    /* Features. Images off, photos are kept but nobody is asked what
     * is in them; voice off, the friend neither listens nor speaks. The
     * room for either is laid out regardless, which is address space and
     * not memory until something is written into it. */
    int32_t enable_images;
    int32_t enable_voice;

    /* The voice it speaks in: one of the provider's prebuilt voices,
     * "Kore" or "cedar" or whatever the provider calls them. Leave it
     * empty and one is chosen from the friend's name, which is what the
     * service did — so a friend keeps the voice it had without anybody
     * writing the name down. */
    Friend_String voice;

    /* Speak the replies to typed messages too. Off, a reply is spoken
     * only when the thing it answers was spoken, which is how the
     * service decided it (ShouldGetEmotiveResponse was set for
     * transcribed audio and nothing else). */
    int32_t speak_replies;

    /* A Friend_Voice_Mode: which of the two ways of talking. */
    int32_t voice_mode;

    /* Let speech interrupt the friend mid-sentence. Off by default,
     * because on speakers the microphone hears the friend and the friend
     * interrupts itself: it wants headphones, or a room with an echo
     * canceller in it. Off, the friend is deaf while it talks and the
     * frontend interrupts it by asking (friend_retract_reply). */
    int32_t voice_barge_in;

    /* Everything below is a knob the service had hardcoded.
     * friend_default_config fills in the value the service shipped, given
     * in the comment, and zero means the same thing. */
    int32_t consolidate_at;        /* 50 active events triggers consolidation */
    int32_t consolidate_leaving;   /* 20 left behind                          */
    int32_t consolidate_max;       /* 40 folded at once                       */
    int32_t recall_candidates;     /* 16 from embeddings                      */
    int32_t recall_keep;           /* 3 after reranking                       */
    int32_t recall_recent;         /* 3 newest memories always considered     */
    int32_t max_timelines;         /* 10 chains rendered into the prompt      */
    int32_t prune_min_memories;    /* 50: never prune a graph smaller         */
    int32_t prune_epoch_min;       /* 300 replies minimum epoch               */
    int32_t voice_endpoint_ms;     /* 600 of silence ends a spoken turn       */
    int32_t voice_preroll_ms;      /* 600 kept from before you started        */

    uint64_t random_seed;          /* fix it and a session replays exactly    */

    /* Set these and the block you pass friend_init is treated as reserved
     * address space rather than memory, committed as the friend grows
     * into it. Leave them zero and the block is used exactly as before.
     * They cannot be changed by friend_reconfigure: the block is already
     * being lived in by then. */
    Friend_Commit_Fn commit;
    void            *commit_user;
} Friend_Config;

Friend_Config friend_default_config(void);

/* What an empty field in Friend_Models becomes, for this provider. */
Friend_Models friend_default_models(Friend_Provider provider);

/* Places the context inside your block. Does not take ownership: free the
 * block when you are done and nothing leaks, because nothing else was ever
 * allocated. Returns NULL only on a bad argument or FRIEND_ERR_TOO_SMALL,
 * with the reason written to *out_result when you pass one. */
Friend_Context *friend_init(void *block, size_t block_size,
                            const Friend_Config *config,
                            Friend_Result *out_result);

/* Change what can be changed without rebuilding the state: keys, model
 * names, personality, feature flags, the knobs. Limits cannot change. */
Friend_Result friend_reconfigure(Friend_Context *fr, const Friend_Config *config);

/* ------------------------------------------------------------------ *
 *  Saying things
 * ------------------------------------------------------------------ */

/* A photograph on its way to the friend. JPEG, already downscaled by
 * you: the friend keeps what it is given and shows the model the same
 * bytes, so a twelve-megapixel original costs twelve megapixels of
 * request. Around a thousand pixels on the long edge is what the service
 * sent and what the models are trained to read. */
typedef struct Friend_Attachment {
    Friend_String jpeg;        /* bytes, already downscaled by you */
} Friend_Attachment;

/* Commit a typed message and start a reply. The reply arrives as
 * FRIEND_EVENT_REPLY_DELTA events and ends with FRIEND_EVENT_REPLY_DONE. */
Friend_Result friend_say(Friend_Context *fr, Friend_String text,
                         const Friend_Attachment *attachments, int32_t count);

/* Push the reply the friend is generating off the end of the conversation —
 * the user interrupted and did not hear it. fraction_heard in [0,1] is
 * remembered so a later session knows it was cut off. */
Friend_Result friend_retract_reply(Friend_Context *fr, float fraction_heard);

/* Voice.
 *
 * One call carries microphone audio in — mono int16, whatever rate your
 * device gives, resampled here — and the rest say what it means.
 *
 * Push-to-talk is begin, push, end: you decide where the turn starts and
 * stops, which is what a button is for. Hands-free is open, push, push,
 * push: the friend finds the edges of what you say and answers when you
 * stop, and goes on listening afterwards. Either way what you said is
 * transcribed, answered by the same prompt and the same model a typed
 * message gets, and spoken back — so talking to a friend out loud and
 * typing to it are the same friend, not two of them.
 *
 * What it says arrives as FRIEND_EVENT_AUDIO_OUT, mono int16 at
 * sample_rate, in the order it should be played. What it heard arrives
 * as FRIEND_EVENT_TRANSCRIPT before the reply starts. A spoken turn is a
 * message like any other: it joins the conversation, it is remembered,
 * and friend_message says it was spoken.
 *
 * A turn with no speech in it is dropped without being sent anywhere,
 * because a press that caught a cough is not a question. */
Friend_Result friend_voice_begin(Friend_Context *fr);
Friend_Result friend_voice_push(Friend_Context *fr, const int16_t *samples,
                                int32_t sample_count, int32_t sample_rate);
Friend_Result friend_voice_end(Friend_Context *fr);

/* Which of the two ways of talking is in use.
 *
 * Spoken turns are a transcriber, the friend's own prompt and model,
 * and a speech model to say the answer: three requests, the same friend
 * you type to, and about three seconds before it starts talking.
 *
 * Realtime is one model with a websocket open to it, listening and
 * talking at once. It answers in well under a second and it hears how
 * you said something rather than reading what you said. What it costs
 * is that the friend is in that model for the length of the session:
 * its prompt goes up once at the start, with the memories recall found
 * then, and it does not consult the friend's own model again until the
 * session ends. The service ran both and wrote a second system prompt
 * for this one. */
typedef enum Friend_Voice_Mode {
    FRIEND_VOICE_SPOKEN_TURNS = 0, /* transcribe, answer, speak */
    FRIEND_VOICE_REALTIME          /* one model, listening and talking */
} Friend_Voice_Mode;

/* Hands-free: the microphone stays open and the friend decides when you
 * have finished saying something. Keep pushing audio either way. */
Friend_Result friend_voice_open(Friend_Context *fr);
Friend_Result friend_voice_close(Friend_Context *fr);

/* Say a message out loud that has already been said — the last reply
 * again, or one further up the conversation. The friend's own messages
 * only; FRIEND_ERR_BAD_STATE while it is already speaking. */
Friend_Result friend_speak(Friend_Context *fr, int32_t message_index);

/* What the voice side is doing, for a frontend that wants to show it. */
typedef enum Friend_Voice_State {
    FRIEND_VOICE_OFF = 0,     /* nothing is listening                    */
    FRIEND_VOICE_LISTENING,   /* the microphone is open, nobody talking  */
    FRIEND_VOICE_HEARING,     /* you are talking and it is being kept    */
    FRIEND_VOICE_WORKING,     /* it is being transcribed and answered    */
    FRIEND_VOICE_SPEAKING     /* the friend is talking                   */
} Friend_Voice_State;

Friend_Voice_State friend_voice_state(Friend_Context *fr);

/* The voice it speaks in: what the configuration asked for, or the one
 * its name chose. */
Friend_String friend_voice(Friend_Context *fr);

/* How loud it is right now, 0 to 1, smoothed enough to draw. Zero when
 * nothing is listening. */
float friend_voice_level(Friend_Context *fr);

/* ------------------------------------------------------------------ *
 *  The pump
 * ------------------------------------------------------------------ */

typedef enum Friend_Io_Kind {
    FRIEND_IO_HTTPS = 0,   /* one request, one response */
    FRIEND_IO_WS_OPEN,     /* open a websocket; stays open until WS_CLOSE */
    FRIEND_IO_WS_SEND,     /* send one message on an open websocket      */
    FRIEND_IO_WS_CLOSE,
    FRIEND_IO_FILE_READ,   /* state, blob, or history log */
    FRIEND_IO_FILE_WRITE
} Friend_Io_Kind;

enum {
    FRIEND_IO_FLAG_STREAM = 1u << 0, /* deliver the body as it arrives */
    FRIEND_IO_FLAG_BINARY = 1u << 1, /* websocket: binary frame, not text */
    /* On a message handed to friend_ws_recv: nothing was said, the
     * socket finished its handshake and the channel is live. A
     * websocket's open request stays outstanding for the life of the
     * socket — its buffer is where every message lands — so this is how
     * the transport says the thing it opened is ready. */
    FRIEND_IO_FLAG_OPENED = 1u << 2
};

typedef struct Friend_Io_Request {
    uint64_t       id;
    Friend_Io_Kind kind;
    uint32_t       flags;

    Friend_String  method;    /* "POST" */
    Friend_String  url;       /* absolute, scheme included */
    Friend_String  headers;   /* CRLF-joined block, ready to write */
    Friend_String  body;      /* valid until you complete or fail this id */

    Friend_String  path;      /* file requests only */
    uint64_t       offset;    /* file requests only */
    uint32_t       length;    /* file reads only: how much to read */

    uint64_t       channel;   /* websocket id for WS_SEND / WS_CLOSE */

    /* Where the response goes. The friend owns this buffer; append into it
     * and report how much you appended. Never write past recv_capacity. */
    uint8_t       *recv;
    uint32_t       recv_capacity;
} Friend_Io_Request;

/* Advance every state machine. Call it every frame, or whenever something
 * happened. now_unix_ms is the only clock the library has. Resets scratch,
 * which is what invalidates previously returned strings. */
Friend_Result friend_tick(Friend_Context *fr, uint64_t now_unix_ms);

/* Work the friend wants done. Fill your array, do the work, report back.
 * Requests stay outstanding until completed or failed; asking again does
 * not hand you the same request twice. */
int32_t friend_pending_io(Friend_Context *fr, Friend_Io_Request *out,
                          int32_t max_requests);

/* Bytes arrived: you appended `appended` bytes to request->recv. Safe to
 * call repeatedly while a response streams in. */
Friend_Result friend_io_progress(Friend_Context *fr, uint64_t id, uint32_t appended);

/* Finished. status is the HTTP status, or 0 for file and websocket work. */
Friend_Result friend_io_complete(Friend_Context *fr, uint64_t id, int32_t status);

/* Did not finish. The friend decides whether to retry, degrade, or give up:
 * a failed rerank falls back to embedding order, a failed reply surfaces to
 * the user, a failed consolidation is tried again later. */
Friend_Result friend_io_failed(Friend_Context *fr, uint64_t id, Friend_String why);

/* Requests whose answers the friend no longer wants: a reply it cut off
 * at a sentence end, one the user interrupted, or anything that was out
 * for a friend since replaced by a load or a begin. Each id is reported
 * once, and once reported it is the friend's again: close it, write
 * nothing more into its buffer, and report nothing further — a complete
 * or failure for it after this is ignored. If you never ask, complete
 * them normally and the friend discards what arrives. Returns how many
 * were written. */
int32_t friend_abandoned_io(Friend_Context *fr, uint64_t *out, int32_t max_ids);

/* A websocket you opened delivered a message, or — with
 * FRIEND_IO_FLAG_OPENED and nothing in it — finished opening. The
 * channel is the id of the WS_OPEN request, which stays outstanding
 * until the socket closes. */
Friend_Result friend_ws_recv(Friend_Context *fr, uint64_t channel,
                             Friend_String message, uint32_t flags);

/* The socket is gone. Saying so is optional and changes nothing: what
 * ends the session is the WS_OPEN request completing, or failing with
 * the reason, which a transport has to report either way. */
Friend_Result friend_ws_closed(Friend_Context *fr, uint64_t channel);

/* How long you may sleep before the next tick needs to happen, in
 * milliseconds. UINT32_MAX means nothing is pending: wait on input. */
uint32_t friend_next_deadline_ms(Friend_Context *fr);

/* ------------------------------------------------------------------ *
 *  Events out
 * ------------------------------------------------------------------ */

typedef enum Friend_Event_Kind {
    FRIEND_EVENT_NONE = 0,
    FRIEND_EVENT_MESSAGE_COMMITTED,  /* index valid: a message joined the chat */
    FRIEND_EVENT_REPLY_DELTA,        /* text valid: more of the reply          */
    FRIEND_EVENT_REPLY_DONE,         /* index and text valid                   */
    FRIEND_EVENT_REPLY_RETRACTED,
    FRIEND_EVENT_AUDIO_OUT,          /* samples valid: play these              */
    FRIEND_EVENT_TRANSCRIPT,         /* text valid: what the friend heard      */
    FRIEND_EVENT_THINKING,           /* the friend is working, show it         */
    FRIEND_EVENT_MEMORIES_CHANGED,   /* count valid: consolidation or pruning  */
    FRIEND_EVENT_INTERIORITY_CHANGED,
    FRIEND_EVENT_STATE_DIRTY,        /* Friend_Stats.unsaved became 1          */
    FRIEND_EVENT_ERROR               /* text valid                             */
} Friend_Event_Kind;

typedef struct Friend_Event {
    Friend_Event_Kind kind;
    /* The message the event is about, as an index friend_message reads:
     * the one committed, the reply arriving, finished or taken back. It
     * is worked out when friend_next_event hands the event over, so it
     * holds until a tick commits a new message, like any other index.
     * -1 for an event about no message, or about one that is no longer
     * in the conversation — a reply that failed is taken back out. */
    int32_t           index;
    int32_t           count;
    Friend_String     text;
    const int16_t    *samples;
    int32_t           sample_count;
    int32_t           sample_rate;
} Friend_Event;

/* Drain the queue. Returns 0 when empty. */
int32_t friend_next_event(Friend_Context *fr, Friend_Event *out);

/* ------------------------------------------------------------------ *
 *  Reading the friend
 *
 *  Views, not copies. Draw straight from these.
 * ------------------------------------------------------------------ */

typedef enum Friend_Message_Kind {
    FRIEND_MESSAGE_USER = 0,
    FRIEND_MESSAGE_FRIEND,
    FRIEND_MESSAGE_AMBIENT,   /* overheard summary. hidden from the timeline */
    FRIEND_MESSAGE_NOTICE     /* a separator the frontend draws as a rule    */
} Friend_Message_Kind;

typedef struct Friend_Message {
    Friend_Message_Kind kind;
    Friend_String       text;
    uint64_t            unix_ms;
    int32_t             attachment_count;
    int32_t             was_spoken;     /* transcribed from voice */
    int32_t             is_streaming;   /* still arriving         */
} Friend_Message;

/* The conversation, oldest first. Indices below friend_message_count() are
 * stable until a tick commits a new message.
 *
 * It is the whole conversation up to max_scrollback messages and
 * scrollback_bytes of text, which at the defaults is a quarter of a
 * million messages — more than the service kept. Past that the oldest
 * fall off the front and index 0 is no longer the first thing anybody
 * said; Friend_Stats.messages_lost says how many are missing, which is
 * what a frontend needs to put a line at the top of the scroll rather
 * than silently pretend the conversation began there. Nothing the friend
 * remembers is lost with them: a message is folded into memory long
 * before it can reach the front. */
int32_t       friend_message_count(Friend_Context *fr);
Friend_Result friend_message(Friend_Context *fr, int32_t index, Friend_Message *out);

/* A photo on a message. JPEG bytes, and what a vision model made of it
 * when there is one.
 *
 * The bytes come from the blob file, which means a read through the I/O
 * pump: the first call starts it and returns FRIEND_ERR_BAD_STATE, and a
 * later tick has them. Ask again until it is FRIEND_OK, or until it is
 * FRIEND_ERR_BAD_DATA, which means this photo's pixels are not in the
 * file and never will be — an import that had no room for them, or a
 * friend configured without a blob_path. A photo the friend has only
 * just been handed is there immediately, because it has not been written
 * out yet.
 *
 * One photo is held at a time: the bytes are valid until the next call
 * for a different one. out_caption may be empty and is valid as long as
 * the message is. Either out pointer may be null. */
Friend_Result friend_message_attachment(Friend_Context *fr, int32_t message_index,
                                        int32_t attachment_index,
                                        Friend_String *out_jpeg,
                                        Friend_String *out_caption);

typedef struct Friend_Memory_Id { uint32_t slot; uint32_t generation; } Friend_Memory_Id;

typedef enum Friend_Relation {
    FRIEND_RELATION_NONE = 0,
    FRIEND_RELATION_CHANGED,      /* A became B                        */
    FRIEND_RELATION_CAUSE,        /* A caused B                        */
    FRIEND_RELATION_REASON,       /* A happened because of B           */
    FRIEND_RELATION_HINDERED_BY,
    FRIEND_RELATION_REACT,        /* B is how they felt about A        */
    FRIEND_RELATION_WANT,         /* A made them want B                */
    FRIEND_RELATION_SAME_TOPIC
} Friend_Relation;

typedef struct Friend_Memory_Node {
    Friend_Memory_Id id;
    Friend_String    description;
    uint64_t         created_unix_ms;
    uint64_t         use_count;
    int32_t          recalled_now;   /* in the prompt of the current reply */
} Friend_Memory_Node;

typedef struct Friend_Memory_Edge {
    Friend_Memory_Id from, to;
    Friend_Relation  relation;
} Friend_Memory_Edge;

/* The graph, for display and for anyone who wants to show a user what
 * their friend remembers. */
int32_t       friend_memory_count(Friend_Context *fr);
Friend_Result friend_memory_at(Friend_Context *fr, int32_t index, Friend_Memory_Node *out);
int32_t       friend_memory_edge_count(Friend_Context *fr);
Friend_Result friend_memory_edge_at(Friend_Context *fr, int32_t index, Friend_Memory_Edge *out);

/* What the friend is called, and when it was first made. A frontend
 * needs both to put a list of friends in front of someone. */
Friend_String friend_name(Friend_Context *fr);
uint64_t      friend_created_unix_ms(Friend_Context *fr);

/* The friend's current interior monologue. */
Friend_String friend_interiority(Friend_Context *fr);

/* Forget one memory, because the user asked. Drops it and bridges its
 * predecessors to its successors so the chains through it survive. */
Friend_Result friend_forget(Friend_Context *fr, Friend_Memory_Id id);

/* ------------------------------------------------------------------ *
 *  Persistence
 *
 *  The library does not touch the filesystem; it hands you bytes. Blob and
 *  history files it does read and write, through FRIEND_IO_FILE_* requests
 *  against the paths you configure.
 * ------------------------------------------------------------------ */

/* What is at the front of a saved friend, without loading it: enough for
 * a frontend to put a directory of them in front of someone. The name
 * points into your buffer. A few kilobytes of the file is plenty — it
 * says how much it needed, so a short read can be retried longer. */
typedef struct Friend_Save_Info {
    Friend_String name;
    uint64_t      created_unix_ms;
    uint64_t      total_event_count;
    int32_t       memories;
    size_t        bytes_needed;
} Friend_Save_Info;

Friend_Result friend_save_peek(const void *buffer, size_t size,
                               Friend_Save_Info *out);

/* A friend with nothing in it, called `name`. Everything the context
 * held is forgotten the way loading over the top of it would forget it,
 * and the configuration — keys, models, personality — is untouched,
 * because it belongs to the program rather than to the friend.
 *
 * This is the other way to get a friend besides loading one and
 * importing one: somebody who never used the service still has to be
 * able to start. The name is what the user calls it, truncated at
 * FRIEND_NAME_MAX on a code point boundary; empty keeps the current
 * one. Made at whatever time the last friend_tick was given, so tick
 * before you call this or it will think it was born in 1970. */
Friend_Result friend_begin(Friend_Context *fr, Friend_String name);

/* A load replaces everything the context held, the way friend_begin
 * does: requests still out for the friend that was open are given up
 * (friend_abandoned_io names them, and their answers are ignored), and a
 * spoken turn or realtime session is ended. A file that is not a friend,
 * or is from a build that cannot read it, is refused before anything is
 * replaced. The same goes for friend_import_export. */
size_t        friend_save_size(Friend_Context *fr);
Friend_Result friend_save(Friend_Context *fr, void *buffer, size_t size, size_t *out_written);
Friend_Result friend_load(Friend_Context *fr, const void *buffer, size_t size);

/* What an import brought across, for telling the user. Counts are of
 * things that landed, except the two that say otherwise. */
typedef struct Friend_Import_Report {
    int32_t memories;
    int32_t connections;
    int32_t messages;
    int32_t images;             /* photos referenced by what came across  */
    int32_t images_without_pixels; /* ...of those, descriptor only: no
                                    * blob_path, or more photographs than
                                    * image_staging_bytes had room for  */
    int32_t memories_dropped;   /* over max_memories                      */
    int32_t connections_dropped;
    int32_t messages_dropped;
    int32_t truncated;          /* fields longer than a record holds      */
} Friend_Import_Report;

/* Import a friend out of the official export: the contents of
 * friend_state.json from the download zip. Memories, embeddings,
 * connections, interiority, use counts and full history come across.
 *
 * The JSON is read in place and never copied whole, so hand it a mapped
 * view of the file rather than a buffer you had to find room for. It is
 * only read during the call. Replaces everything in the context.
 *
 * Embeddings are reused byte-for-byte when the configured embedding model
 * is the one that produced them, and re-derived from descriptions
 * otherwise — which takes a while and shows up as I/O work.
 *
 * out_report may be null. */
Friend_Result friend_import_export(Friend_Context *fr, Friend_String friend_state_json,
                                   Friend_Import_Report *out_report);

/* ------------------------------------------------------------------ *
 *  Reading the download zip
 *
 *  The export arrives as friend_<name>.zip holding one deflated entry,
 *  friend_state.json. This turns the one into the other a chunk at a
 *  time, so neither of you ever holds both: push bytes you read from the
 *  file in, take plain bytes out, put them where you like. It allocates
 *  nothing and does no I/O — every byte of its state is in the memory you
 *  pass it, which is about 36 KiB and reusable between entries.
 *
 *      uint8_t  *mem = malloc(friend_unzip_size());
 *      Friend_Unzip *uz;
 *      friend_unzip_begin(mem, friend_unzip_size(), &uz);
 *
 *      Friend_Unzip_Step step = {0};
 *      for (;;) {
 *          if (step.in_used == step.in_size) {
 *              n = fread(in_buf, 1, sizeof in_buf, zip);
 *              step.in = in_buf; step.in_size = n; step.in_used = 0;
 *              step.in_last = (n == 0);
 *          }
 *          step.out = out_buf; step.out_size = sizeof out_buf;
 *          if (friend_unzip_push(uz, &step) != FRIEND_OK) break;
 *          fwrite(out_buf, 1, step.out_written, json);
 *          if (step.done) break;
 *      }
 *
 *  If you already have the JSON — you unzipped it yourself, or the file
 *  was never zipped — skip all this and hand it straight to
 *  friend_import_export.
 * ------------------------------------------------------------------ */

typedef struct Friend_Unzip Friend_Unzip;

/* Bytes to hand friend_unzip_begin. Includes room to align. */
size_t        friend_unzip_size(void);
Friend_Result friend_unzip_begin(void *memory, size_t size, Friend_Unzip **out);

typedef struct Friend_Unzip_Step {
    /* In: the next compressed bytes, and whether the file ends here.
     * in_used comes back set; leave the unconsumed tail for next time. */
    const void *in;
    size_t      in_size;
    size_t      in_used;
    int32_t     in_last;

    /* Out: where plain bytes go. Must have room, or nothing progresses. */
    void   *out;
    size_t  out_size;
    size_t  out_written;

    int32_t done; /* 1 when the entry is complete and verified */
} Friend_Unzip_Step;

/* Consumes what it can of in and produces what fits in out, which may be
 * neither all of one nor any of the other: call it until done. in_used and
 * out_written are set on every call, including the failing one. */
Friend_Result friend_unzip_push(Friend_Unzip *uz, Friend_Unzip_Step *step);

/* The entry's name, valid once the first push has returned. The official
 * export holds exactly one entry, called friend_state.json. */
Friend_String friend_unzip_entry_name(Friend_Unzip *uz);

/* Why the last push failed. */
Friend_String friend_unzip_error(Friend_Unzip *uz);

/* ------------------------------------------------------------------ *
 *  Introspection
 *
 *  The old service grew a diagnostics API because you cannot debug a
 *  friend you cannot see. Here it is free: it is all in the block already.
 * ------------------------------------------------------------------ */

typedef struct Friend_Stats {
    int32_t  memories;
    int32_t  memories_unused;
    int32_t  connections;
    int32_t  active_events;
    uint64_t replies_total;
    uint64_t epoch_samples;
    uint64_t epoch_length;
    int32_t  io_inflight;
    int32_t  scrollback;       /* messages held out of the model's window */
    int32_t  photos;           /* photographs the friend has bytes for    */
    int32_t  photos_waiting;   /* ...of those, not yet written to disk    */
    uint64_t messages_lost;    /* fell out of the scrollback entirely     */
    uint64_t messages_unremembered; /* ...before they reached a memory    */
    size_t   bytes_used;       /* of the block the layout needs */
    size_t   bytes_total;
    size_t   bytes_committed;  /* ...and of that, what is really there */
    size_t   scratch_peak;

    /* What a frontend checks before saving.
     *
     * unsaved is 1 when friend_save would write something the last
     * friend_save or friend_load did not: it goes to 1 with
     * FRIEND_EVENT_STATE_DIRTY at the first change, and back to 0 when
     * friend_save writes into a buffer. A friend made by friend_begin or
     * friend_import_export has never been written, so it starts at 1.
     * A save taken while work is out is a true picture of that moment,
     * and the work landing later sets unsaved again.
     *
     * settled is 1 when a save taken now is not half of something: no
     * reply is being recalled for or written, no consolidation is
     * folding, nothing said is being transcribed and no photo captioned.
     * A realtime session's open socket and a speaker still playing do
     * not count; what they produce is recorded when it is finished. A
     * save that is not settled is still a sound file — a reply arriving
     * is left out of it, and a fold in progress is done again after a
     * load — but it is one that will have to be taken again.
     *
     * photos_waiting, above, is the third: a photo not yet in the blob
     * file is one a save would point at and a restart could not read.
     * A frontend about to exit pumps until it is 0. */
    int32_t  unsaved;
    int32_t  settled;
} Friend_Stats;

Friend_Stats friend_stats(Friend_Context *fr);

/* The prompt behind the most recent reply: system text, rendered memory
 * timelines, and the conversation as the model saw it. Kept in a page of
 * its own; a prompt longer than that page is not kept, and this says so
 * instead. */
Friend_String friend_last_prompt(Friend_Context *fr);

#ifdef __cplusplus
}
#endif

#endif /* FRIEND_H */
