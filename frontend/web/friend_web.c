/* friend_web.c — libfriend as a WebAssembly module, for friend-local.html.
 *
 * The library does no I/O of its own: it says what it wants fetched and
 * written and takes the bytes back. So the page does the fetching, with
 * fetch(), and the keeping, in the browser's storage, and this file is
 * only the narrow waist between the two: the library and a few functions
 * the page can call with numbers.
 *
 * Everything crosses as i32 and f64. A pointer is an offset into the
 * module's memory; a 64-bit id or time is an f64, which holds every
 * integer the library will hand out. Anything with more than one field
 * comes back as a pointer to a small table of u32 (web_u32) and f64
 * (web_f64) that the page reads straight out of memory, valid until the
 * next call that fills it.
 *
 * There is no C library. What the library uses of one — memcpy and
 * friends, sqrt, one %.9g — is libc/libc.c, and libc/ holds the headers
 * it includes.
 *
 * Memory: the module's own data and stack, then the friend's block at
 * __heap_base, sized for the friend in hand and grown by the page before
 * friend_init. Past the block is the page's: a zip or a save being read
 * in sits there for the length of one call.
 */

#include "../../libfriend/src/friend_unity.c"
#include "libc/libc.c"

#define WEB_EXPORT(name) __attribute__((export_name(#name)))


/* ------------------------------------------------------------------ *
 *  State
 * ------------------------------------------------------------------ */

#define WEB_REQUESTS 16
#define WEB_INPUT_BYTES (8u * 1024u * 1024u)
#define WEB_CONFIG_BYTES (256u * 1024u)

static Friend_Context *web_fr;
static Friend_Config   web_config;

/* What the page writes before a call: text, a photo, some samples. */
static uint8_t web_input_bytes[WEB_INPUT_BYTES] __attribute__((aligned(16)));

/* The configuration's strings, which have to outlive the call that set
 * them: each field has its own room, so changing one leaves the others
 * where they are. */
enum {
    WEB_STR_API_KEY = 0,
    WEB_STR_BASE_URL,
    WEB_STR_FRIEND_NAME,
    WEB_STR_USER_NAME,
    WEB_STR_BLOB_PATH,
    WEB_STR_VOICE,
    WEB_STR_PROMPT,
    WEB_STR_MODEL_CHAT,
    WEB_STR_MODEL_EXTRACT,
    WEB_STR_MODEL_RELATE,
    WEB_STR_MODEL_INTERIORITY,
    WEB_STR_MODEL_RERANK,
    WEB_STR_MODEL_EMBED,
    WEB_STR_MODEL_VISION,
    WEB_STR_MODEL_TRANSCRIBE,
    WEB_STR_MODEL_SPEAK,
    WEB_STR_MODEL_LIVE,
    WEB_STR_COUNT
};
static char    web_config_room[WEB_STR_COUNT][WEB_CONFIG_BYTES / WEB_STR_COUNT];
static int32_t web_config_len[WEB_STR_COUNT];

enum {
    WEB_INT_PROVIDER = 0,
    WEB_INT_UTC_OFFSET,
    WEB_INT_IMAGES,
    WEB_INT_VOICE,
    WEB_INT_SPEAK_REPLIES,
    WEB_INT_VOICE_MODE,
    WEB_INT_BARGE_IN,
    WEB_INT_EMBODIED
};

static Friend_Io_Request web_requests[WEB_REQUESTS];
static uint64_t          web_abandoned_ids[WEB_REQUESTS];

/* The tables a call fills for the page to read. */
static uint32_t web_u32_out[32];
static double   web_f64_out[8];

static uint8_t      web_unzip_memory[64u * 1024u];
static Friend_Unzip *web_uz;

static Friend_String web_s(const char *p, int32_t len)
{
    Friend_String s;
    s.data = p;
    s.len  = len < 0 ? 0 : len;
    return s;
}

static uint32_t web_ptr(const void *p)
{
    return (uint32_t)(uintptr_t)p;
}

static void web_put_string(int32_t at, Friend_String s)
{
    web_u32_out[at]     = web_ptr(s.data);
    web_u32_out[at + 1] = (uint32_t)(s.len < 0 ? 0 : s.len);
}

static uint64_t web_u64(double v)
{
    return v <= 0 ? 0 : (uint64_t)v;
}

/* ------------------------------------------------------------------ *
 *  Where things are
 * ------------------------------------------------------------------ */

extern unsigned char __heap_base;

WEB_EXPORT(web_heap_base) uint32_t web_heap_base(void)
{
    return (web_ptr(&__heap_base) + 65535u) & ~65535u;
}

WEB_EXPORT(web_input) uint32_t web_input(void)
{
    return web_ptr(web_input_bytes);
}

WEB_EXPORT(web_input_size) uint32_t web_input_size(void)
{
    return WEB_INPUT_BYTES;
}

WEB_EXPORT(web_u32) uint32_t web_u32(void)
{
    return web_ptr(web_u32_out);
}

WEB_EXPORT(web_f64) uint32_t web_f64(void)
{
    return web_ptr(web_f64_out);
}

/* ------------------------------------------------------------------ *
 *  Configuration
 * ------------------------------------------------------------------ */

static Friend_String web_config_string(int32_t field)
{
    return web_s(web_config_room[field], web_config_len[field]);
}

static void web_config_apply(void)
{
    Friend_Models *m = &web_config.models;
    web_config.api_key         = web_config_string(WEB_STR_API_KEY);
    web_config.base_url        = web_config_string(WEB_STR_BASE_URL);
    web_config.friend_name     = web_config_string(WEB_STR_FRIEND_NAME);
    web_config.user_name       = web_config_string(WEB_STR_USER_NAME);
    web_config.blob_path       = web_config_string(WEB_STR_BLOB_PATH);
    web_config.voice           = web_config_string(WEB_STR_VOICE);
    web_config.prompt_override = web_config_string(WEB_STR_PROMPT);
    m->chat        = web_config_string(WEB_STR_MODEL_CHAT);
    m->extract     = web_config_string(WEB_STR_MODEL_EXTRACT);
    m->relate      = web_config_string(WEB_STR_MODEL_RELATE);
    m->interiority = web_config_string(WEB_STR_MODEL_INTERIORITY);
    m->rerank      = web_config_string(WEB_STR_MODEL_RERANK);
    m->embed       = web_config_string(WEB_STR_MODEL_EMBED);
    m->vision      = web_config_string(WEB_STR_MODEL_VISION);
    m->transcribe  = web_config_string(WEB_STR_MODEL_TRANSCRIBE);
    m->speak       = web_config_string(WEB_STR_MODEL_SPEAK);
    m->live        = web_config_string(WEB_STR_MODEL_LIVE);
}

WEB_EXPORT(web_config_reset) void web_config_reset(void)
{
    Friend_Limits limits = web_config.limits;
    int32_t       i;
    web_config = friend_default_config();
    if (limits.max_memories > 0) web_config.limits = limits;
    for (i = 0; i < WEB_STR_COUNT; ++i) web_config_len[i] = 0;
    web_config_apply();
}

/* Returns 0 when the field is unknown or the text does not fit. */
WEB_EXPORT(web_config_str)
int32_t web_config_str(int32_t field, const char *text, int32_t len)
{
    if (field < 0 || field >= WEB_STR_COUNT || len < 0 ||
        (size_t)len > sizeof(web_config_room[0])) {
        return 0;
    }
    memcpy(web_config_room[field], text, (size_t)len);
    web_config_len[field] = len;
    web_config_apply();
    return 1;
}

WEB_EXPORT(web_config_int) int32_t web_config_int(int32_t field, int32_t value)
{
    switch (field) {
    case WEB_INT_PROVIDER:
        web_config.provider = value ? FRIEND_PROVIDER_OPENAI_COMPATIBLE
                                    : FRIEND_PROVIDER_GEMINI;
        return 1;
    case WEB_INT_UTC_OFFSET: web_config.utc_offset_minutes = value; return 1;
    case WEB_INT_IMAGES: web_config.enable_images = value; return 1;
    case WEB_INT_VOICE: web_config.enable_voice = value; return 1;
    case WEB_INT_SPEAK_REPLIES: web_config.speak_replies = value; return 1;
    case WEB_INT_VOICE_MODE: web_config.voice_mode = value; return 1;
    case WEB_INT_BARGE_IN: web_config.voice_barge_in = value; return 1;
    case WEB_INT_EMBODIED: web_config.embodied = value; return 1;
    default: return 0;
    }
}

/* Limits for a friend of this size, with room to grow into, and what a
 * block for them is. A browser tab is not a desktop: the defaults lay out
 * room for a decade of talking, which costs a desktop address space and
 * costs a phone the tab. These are sized to the friend instead, and the
 * page lays them out again, bigger, when the friend outgrows them.
 * Returns the bytes the block needs. */
WEB_EXPORT(web_limits)
double web_limits(int32_t memories, double events, double save_bytes)
{
    Friend_Limits l = friend_default_limits();
    int32_t       want_memories = memories * 2 + 2048;
    double        want_events   = events * 2 + 8192;
    double        want_text     = save_bytes * 2 + 8.0 * 1024 * 1024;

    if (want_memories < 4096) want_memories = 4096;
    if (want_memories > l.max_memories) want_memories = l.max_memories;
    if (want_events > l.max_scrollback) want_events = l.max_scrollback;
    if (want_text > l.scrollback_bytes) want_text = l.scrollback_bytes;

    l.max_memories        = want_memories;
    l.max_connections     = want_memories * 4;
    l.max_scrollback      = (int32_t)want_events;
    l.scrollback_bytes    = (int32_t)want_text;
    l.max_inflight_io     = 16;
    l.io_bytes            = 32 * 1024 * 1024;
    l.scratch_bytes       = 16 * 1024 * 1024;
    /* Photographs on their way into storage keep the desktop's room,
     * because an import stages every one of them at once and whatever
     * does not fit comes across without its pixels. Untouched, it is
     * pages the browser never hands out. */
    web_config.limits     = l;
    return (double)friend_required_memory(l);
}

WEB_EXPORT(web_max_memories) int32_t web_max_memories(void)
{
    return web_config.limits.max_memories;
}

/* ------------------------------------------------------------------ *
 *  Life
 * ------------------------------------------------------------------ */

WEB_EXPORT(web_init) int32_t web_init(uint8_t *block, double size)
{
    Friend_Result result = FRIEND_OK;
    web_config_apply();
    web_fr = friend_init(block, (size_t)size, &web_config, &result);
    return web_fr ? FRIEND_OK : (int32_t)result;
}

WEB_EXPORT(web_reconfigure) int32_t web_reconfigure(void)
{
    web_config_apply();
    return (int32_t)friend_reconfigure(web_fr, &web_config);
}

WEB_EXPORT(web_tick) int32_t web_tick(double now_unix_ms)
{
    return (int32_t)friend_tick(web_fr, web_u64(now_unix_ms));
}

WEB_EXPORT(web_deadline) double web_deadline(void)
{
    uint32_t ms = friend_next_deadline_ms(web_fr);
    return ms == UINT32_MAX ? -1.0 : (double)ms;
}

/* [ptr, len] of the last error. */
WEB_EXPORT(web_last_error) uint32_t web_last_error(void)
{
    web_put_string(0, friend_last_error(web_fr));
    return web_u32();
}

WEB_EXPORT(web_name) uint32_t web_name(void)
{
    web_put_string(0, friend_name(web_fr));
    web_f64_out[0] = (double)friend_created_unix_ms(web_fr);
    return web_u32();
}

WEB_EXPORT(web_interiority) uint32_t web_interiority(void)
{
    web_put_string(0, friend_interiority(web_fr));
    return web_u32();
}

WEB_EXPORT(web_voice_name) uint32_t web_voice_name(void)
{
    web_put_string(0, friend_voice(web_fr));
    return web_u32();
}

/* ------------------------------------------------------------------ *
 *  The pump
 * ------------------------------------------------------------------ */

WEB_EXPORT(web_pending) int32_t web_pending(void)
{
    return friend_pending_io(web_fr, web_requests, WEB_REQUESTS);
}

/* One request from the last web_pending, laid out as
 *   0 kind  1 flags  2,3 method  4,5 url  6,7 headers  8,9 body
 *   10,11 path  12 length  13 recv  14 recv_capacity
 * with id, offset and channel in f64 0, 1 and 2. */
WEB_EXPORT(web_request) uint32_t web_request(int32_t i)
{
    const Friend_Io_Request *r = &web_requests[i];
    web_u32_out[0] = (uint32_t)r->kind;
    web_u32_out[1] = r->flags;
    web_put_string(2, r->method);
    web_put_string(4, r->url);
    web_put_string(6, r->headers);
    web_put_string(8, r->body);
    web_put_string(10, r->path);
    web_u32_out[12] = r->length;
    web_u32_out[13] = web_ptr(r->recv);
    web_u32_out[14] = r->recv_capacity;
    web_f64_out[0]  = (double)r->id;
    web_f64_out[1]  = (double)r->offset;
    web_f64_out[2]  = (double)r->channel;
    return web_u32();
}

WEB_EXPORT(web_io_progress) int32_t web_io_progress(double id, uint32_t appended)
{
    return (int32_t)friend_io_progress(web_fr, web_u64(id), appended);
}

WEB_EXPORT(web_io_complete) int32_t web_io_complete(double id, int32_t status)
{
    return (int32_t)friend_io_complete(web_fr, web_u64(id), status);
}

WEB_EXPORT(web_io_failed)
int32_t web_io_failed(double id, const char *why, int32_t len)
{
    return (int32_t)friend_io_failed(web_fr, web_u64(id), web_s(why, len));
}

WEB_EXPORT(web_abandoned) int32_t web_abandoned(void)
{
    return friend_abandoned_io(web_fr, web_abandoned_ids, WEB_REQUESTS);
}

WEB_EXPORT(web_abandoned_id) double web_abandoned_id(int32_t i)
{
    return (double)web_abandoned_ids[i];
}

WEB_EXPORT(web_ws_recv)
int32_t web_ws_recv(double channel, const char *message, int32_t len,
                    uint32_t flags)
{
    return (int32_t)friend_ws_recv(web_fr, web_u64(channel),
                                   web_s(message, len), flags);
}

WEB_EXPORT(web_ws_closed) int32_t web_ws_closed(double channel)
{
    return (int32_t)friend_ws_closed(web_fr, web_u64(channel));
}

/* ------------------------------------------------------------------ *
 *  Events
 * ------------------------------------------------------------------ */

/* 0 when there are none; otherwise
 *   0 kind  1 index  2 count  3,4 text  5 samples  6 sample_count
 *   7 sample_rate */
WEB_EXPORT(web_next_event) uint32_t web_next_event(void)
{
    Friend_Event e;
    if (!friend_next_event(web_fr, &e)) return 0;
    web_u32_out[0] = (uint32_t)e.kind;
    web_u32_out[1] = (uint32_t)e.index;
    web_u32_out[2] = (uint32_t)e.count;
    web_put_string(3, e.text);
    web_u32_out[5] = web_ptr(e.samples);
    web_u32_out[6] = (uint32_t)e.sample_count;
    web_u32_out[7] = (uint32_t)e.sample_rate;
    return web_u32();
}

/* ------------------------------------------------------------------ *
 *  Saying things
 * ------------------------------------------------------------------ */

static Friend_Attachment web_attachments[FRIEND_ATTACHMENTS_MAX];
static int32_t           web_attachment_count;

WEB_EXPORT(web_attach) int32_t web_attach(const char *jpeg, int32_t len)
{
    if (web_attachment_count >= FRIEND_ATTACHMENTS_MAX) return 0;
    web_attachments[web_attachment_count++].jpeg = web_s(jpeg, len);
    return 1;
}

WEB_EXPORT(web_say) int32_t web_say(const char *text, int32_t len)
{
    Friend_Result r = friend_say(web_fr, web_s(text, len), web_attachments,
                                 web_attachment_count);
    web_attachment_count = 0;
    return (int32_t)r;
}

WEB_EXPORT(web_retract) int32_t web_retract(double fraction_heard)
{
    return (int32_t)friend_retract_reply(web_fr, (float)fraction_heard);
}

WEB_EXPORT(web_voice_begin) int32_t web_voice_begin(void)
{
    return (int32_t)friend_voice_begin(web_fr);
}

WEB_EXPORT(web_voice_push)
int32_t web_voice_push(const int16_t *samples, int32_t count, int32_t rate)
{
    return (int32_t)friend_voice_push(web_fr, samples, count, rate);
}

WEB_EXPORT(web_voice_end) int32_t web_voice_end(void)
{
    return (int32_t)friend_voice_end(web_fr);
}

WEB_EXPORT(web_voice_open) int32_t web_voice_open(void)
{
    return (int32_t)friend_voice_open(web_fr);
}

WEB_EXPORT(web_voice_close) int32_t web_voice_close(void)
{
    return (int32_t)friend_voice_close(web_fr);
}

WEB_EXPORT(web_voice_state) int32_t web_voice_state(void)
{
    return (int32_t)friend_voice_state(web_fr);
}

WEB_EXPORT(web_voice_level) double web_voice_level(void)
{
    return (double)friend_voice_level(web_fr);
}

WEB_EXPORT(web_speak) int32_t web_speak(int32_t message_index)
{
    return (int32_t)friend_speak(web_fr, message_index);
}

/* ------------------------------------------------------------------ *
 *  Reading the friend
 * ------------------------------------------------------------------ */

WEB_EXPORT(web_message_count) int32_t web_message_count(void)
{
    return friend_message_count(web_fr);
}

/* 0 result  1 kind  2,3 text  4 attachments  5 spoken  6 streaming,
 * and the time in f64 0. */
WEB_EXPORT(web_message) uint32_t web_message(int32_t index)
{
    Friend_Message m;
    Friend_Result  r = friend_message(web_fr, index, &m);
    memset(web_u32_out, 0, sizeof(web_u32_out));
    web_u32_out[0] = (uint32_t)r;
    if (r == FRIEND_OK) {
        web_u32_out[1] = (uint32_t)m.kind;
        web_put_string(2, m.text);
        web_u32_out[4] = (uint32_t)m.attachment_count;
        web_u32_out[5] = (uint32_t)m.was_spoken;
        web_u32_out[6] = (uint32_t)m.is_streaming;
        web_f64_out[0] = (double)m.unix_ms;
    }
    return web_u32();
}

/* 0 result  1,2 jpeg  3,4 caption */
WEB_EXPORT(web_attachment) uint32_t web_attachment(int32_t message, int32_t i)
{
    Friend_String jpeg = { 0, 0 }, caption = { 0, 0 };
    Friend_Result r = friend_message_attachment(web_fr, message, i, &jpeg,
                                                &caption);
    web_u32_out[0] = (uint32_t)r;
    web_put_string(1, jpeg);
    web_put_string(3, caption);
    return web_u32();
}

WEB_EXPORT(web_memory_count) int32_t web_memory_count(void)
{
    return friend_memory_count(web_fr);
}

/* 0 result  1 slot  2 generation  3,4 description  5 recalled now,
 * and created and use count in f64 0 and 1. */
WEB_EXPORT(web_memory) uint32_t web_memory(int32_t index)
{
    Friend_Memory_Node n;
    Friend_Result      r = friend_memory_at(web_fr, index, &n);
    memset(web_u32_out, 0, sizeof(web_u32_out));
    web_u32_out[0] = (uint32_t)r;
    if (r == FRIEND_OK) {
        web_u32_out[1] = n.id.slot;
        web_u32_out[2] = n.id.generation;
        web_put_string(3, n.description);
        web_u32_out[5] = (uint32_t)n.recalled_now;
        web_f64_out[0] = (double)n.created_unix_ms;
        web_f64_out[1] = (double)n.use_count;
    }
    return web_u32();
}

WEB_EXPORT(web_forget) int32_t web_forget(uint32_t slot, uint32_t generation)
{
    Friend_Memory_Id id;
    id.slot       = slot;
    id.generation = generation;
    return (int32_t)friend_forget(web_fr, id);
}

/* 0 memories  1 connections  2 active  3 inflight  4 scrollback
 * 5 photos  6 photos_waiting  7 unsaved  8 settled, and in f64:
 * 0 replies  1 committed  2 lost  3 total. */
WEB_EXPORT(web_stats) uint32_t web_stats(void)
{
    Friend_Stats s = friend_stats(web_fr);
    web_u32_out[0] = (uint32_t)s.memories;
    web_u32_out[1] = (uint32_t)s.connections;
    web_u32_out[2] = (uint32_t)s.active_events;
    web_u32_out[3] = (uint32_t)s.io_inflight;
    web_u32_out[4] = (uint32_t)s.scrollback;
    web_u32_out[5] = (uint32_t)s.photos;
    web_u32_out[6] = (uint32_t)s.photos_waiting;
    web_u32_out[7] = (uint32_t)s.unsaved;
    web_u32_out[8] = (uint32_t)s.settled;
    web_f64_out[0] = (double)s.replies_total;
    web_f64_out[1] = (double)s.bytes_committed;
    web_f64_out[2] = (double)s.messages_lost;
    web_f64_out[3] = (double)s.bytes_total;
    return web_u32();
}

WEB_EXPORT(web_last_prompt) uint32_t web_last_prompt(void)
{
    web_put_string(0, friend_last_prompt(web_fr));
    return web_u32();
}

/* ------------------------------------------------------------------ *
 *  Keeping it
 * ------------------------------------------------------------------ */

WEB_EXPORT(web_begin) int32_t web_begin(const char *name, int32_t len)
{
    return (int32_t)friend_begin(web_fr, web_s(name, len));
}

WEB_EXPORT(web_save_size) double web_save_size(void)
{
    return (double)friend_save_size(web_fr);
}

/* Bytes written, or -1. */
WEB_EXPORT(web_save) double web_save(void *buffer, double size)
{
    size_t written = 0;
    if (friend_save(web_fr, buffer, (size_t)size, &written) != FRIEND_OK) {
        return -1;
    }
    return (double)written;
}

WEB_EXPORT(web_load) int32_t web_load(const void *buffer, double size)
{
    return (int32_t)friend_load(web_fr, buffer, (size_t)size);
}

/* 0 result  1,2 name  3 memories, and in f64 0 created  1 events
 * 2 bytes needed. */
WEB_EXPORT(web_peek) uint32_t web_peek(const void *buffer, double size)
{
    Friend_Save_Info info;
    Friend_Result    r = friend_save_peek(buffer, (size_t)size, &info);
    memset(web_u32_out, 0, sizeof(web_u32_out));
    web_u32_out[0] = (uint32_t)r;
    if (r == FRIEND_OK) {
        web_put_string(1, info.name);
        web_u32_out[3] = (uint32_t)info.memories;
        web_f64_out[0] = (double)info.created_unix_ms;
        web_f64_out[1] = (double)info.total_event_count;
        web_f64_out[2] = (double)info.bytes_needed;
    }
    return web_u32();
}

/* 0 result, then the report's nine counts in order. */
WEB_EXPORT(web_import) uint32_t web_import(const char *json, double size)
{
    Friend_Import_Report rep;
    Friend_Result        r;
    memset(&rep, 0, sizeof(rep));
    r = friend_import_export(web_fr, web_s(json, (int32_t)size), &rep);
    web_u32_out[0] = (uint32_t)r;
    web_u32_out[1] = (uint32_t)rep.memories;
    web_u32_out[2] = (uint32_t)rep.connections;
    web_u32_out[3] = (uint32_t)rep.messages;
    web_u32_out[4] = (uint32_t)rep.images;
    web_u32_out[5] = (uint32_t)rep.images_without_pixels;
    web_u32_out[6] = (uint32_t)rep.memories_dropped;
    web_u32_out[7] = (uint32_t)rep.connections_dropped;
    web_u32_out[8] = (uint32_t)rep.messages_dropped;
    web_u32_out[9] = (uint32_t)rep.truncated;
    return web_u32();
}

/* ------------------------------------------------------------------ *
 *  The download zip
 * ------------------------------------------------------------------ */

WEB_EXPORT(web_unzip_begin) int32_t web_unzip_begin(void)
{
    return (int32_t)friend_unzip_begin(web_unzip_memory,
                                       sizeof(web_unzip_memory), &web_uz);
}

/* 0 result  1 in_used  2 out_written  3 done */
WEB_EXPORT(web_unzip_push)
uint32_t web_unzip_push(const void *in, uint32_t in_size, int32_t in_last,
                        void *out, uint32_t out_size)
{
    Friend_Unzip_Step step;
    Friend_Result     r;
    memset(&step, 0, sizeof(step));
    step.in       = in;
    step.in_size  = in_size;
    step.in_last  = in_last;
    step.out      = out;
    step.out_size = out_size;
    r             = friend_unzip_push(web_uz, &step);
    web_u32_out[0] = (uint32_t)r;
    web_u32_out[1] = (uint32_t)step.in_used;
    web_u32_out[2] = (uint32_t)step.out_written;
    web_u32_out[3] = (uint32_t)step.done;
    return web_u32();
}

WEB_EXPORT(web_unzip_error) uint32_t web_unzip_error(void)
{
    web_put_string(0, friend_unzip_error(web_uz));
    return web_u32();
}

WEB_EXPORT(web_unzip_entry) uint32_t web_unzip_entry(void)
{
    web_put_string(0, friend_unzip_entry_name(web_uz));
    return web_u32();
}

/* [ptr, len] of what an empty model field becomes for this provider,
 * field numbered as the WEB_STR_MODEL_ ones are from chat. */
WEB_EXPORT(web_default_model)
uint32_t web_default_model(int32_t provider, int32_t field)
{
    Friend_Models m = friend_default_models(
        provider ? FRIEND_PROVIDER_OPENAI_COMPATIBLE : FRIEND_PROVIDER_GEMINI);
    const Friend_String all[] = { m.chat,   m.extract,    m.relate,
                                  m.interiority, m.rerank, m.embed,
                                  m.vision, m.transcribe, m.speak,
                                  m.live };
    Friend_String none = { 0, 0 };
    web_put_string(0, field >= 0 && field < (int32_t)(sizeof(all) / sizeof(all[0]))
                          ? all[field]
                          : none);
    return web_u32();
}
