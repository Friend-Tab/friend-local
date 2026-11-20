/* fr_init.c — the one allocation, and the shape it takes.
 *
 * fr_layout() decides where everything in the block goes, as offsets and
 * sizes, and both friend_required_memory() and friend_init() go through
 * it. There is no second description of the layout to drift out of
 * agreement with the first. */

#include "fr_io.h"
#include "fr_memory.h"
#include "fr_voice.h"

/* Big enough for a whole prompt config (the shipped one is ~33 KB of JSON)
 * plus keys and names, with room to spare. Not a knob: a caller cannot
 * sensibly reason about it. */
#define FR_CONFIG_ARENA_BYTES FR_KB(256)

/* Room to keep the last prompt so friend_last_prompt can hand it back
 * after the reply arrived. A long conversation with long messages can
 * outgrow it, in which case friend_last_prompt says so. */
#define FR_PROMPT_PAGE_BYTES FR_KB(512)

/* ------------------------------------------------------------------ *
 *  Limits and layout
 * ------------------------------------------------------------------ */

/* Each limit, what shipped, and the floor below which the code would
 * stop working.
 *
 * The defaults are what the friend might one day need, not what it needs
 * today: with a commit function they cost address space until something
 * reaches them, and without one they cost nothing until something writes
 * to them either, because the block is never cleared whole. So they are
 * set where somebody using this for years never meets them. A quarter of
 * a million messages is a decade of talking every day; a hundred and
 * thirty thousand memories is more than the consolidation rate can
 * produce in that decade, and pruning keeps the graph smaller than that
 * anyway.
 *
 * The floors: a friend with four memories is a legitimate thing to test
 * with; one with no room for a reply is not. The scrollback's count is
 * floored again below, at the window it takes messages from. */
#define FR_LIMIT_FIELDS(X)                                                 \
    X(max_memories, 131072, 4)                                             \
    X(max_connections, 524288, 8)                                          \
    X(max_active_events, 64, 8)                                            \
    X(max_scrollback, 262144, 8)                                           \
    X(scrollback_bytes, FR_MB(64), FR_SCROLL_BYTES_MIN)                    \
    X(max_inflight_io, 32, 4)                                              \
    X(io_bytes, FR_MB(64), FR_MB(1))                                       \
    X(scratch_bytes, FR_MB(16), FR_MB(1))                                  \
    X(image_page_bytes, FR_MB(2), FR_KB(64))                               \
    X(image_staging_bytes, FR_MB(64), FR_KB(64))                           \
    X(voice_press_ms, 20000, 1)

Friend_Limits friend_default_limits(void)
{
    Friend_Limits limits;
#define FR_LIMIT_DEFAULT(field, shipped, floor) limits.field = (i32)(shipped);
    FR_LIMIT_FIELDS(FR_LIMIT_DEFAULT)
#undef FR_LIMIT_DEFAULT
    return limits;
}

/* A zero means "whatever shipped", like everywhere else in the config;
 * anything else is taken seriously but floored. */
static Friend_Limits fr_normalize_limits(Friend_Limits limits)
{
#define FR_LIMIT_NORMALIZE(field, shipped, floor)                          \
    if (limits.field <= 0) limits.field = (i32)(shipped);                  \
    limits.field = fr_max(limits.field, (i32)(floor));
    FR_LIMIT_FIELDS(FR_LIMIT_NORMALIZE)
#undef FR_LIMIT_NORMALIZE
    limits.max_scrollback =
        fr_max(limits.max_scrollback, limits.max_active_events);
    return limits;
}

static Fr_Region fr_reserve(size_t *at, size_t size, size_t align)
{
    Fr_Region region;
    region.offset = fr_align_up(*at, align);
    region.size   = size;
    *at           = region.offset + size;
    return region;
}

#define FR_RESERVE_ARRAY(at, T, count) \
    fr_reserve((at), sizeof(T) * (size_t)(count), FR_ALIGNOF(T))

static Fr_Layout fr_layout(Friend_Limits limits)
{
    Fr_Layout layout;
    size_t    at = 0;

    memset(&layout, 0, sizeof(layout));
    limits        = fr_normalize_limits(limits);
    layout.limits = limits;

    /* One event queue slot per in-flight request plus headroom for the
     * deltas of a reply streaming in between two drains. */
    layout.max_events_queued = limits.max_inflight_io * 4 + 64;
    /* One slice of the io arena per request slot, decided here and never
     * revisited. A request that does not fit in its slice is refused
     * with a reason rather than quietly exhausting a shared arena. */
    layout.io_slot_bytes = ((size_t)limits.io_bytes /
                            (size_t)limits.max_inflight_io) & ~(size_t)15;
    /* A spoken turn goes to a transcriber in one request, as base64 of a
     * WAV file, and a request has to fit its slot. So the microphone
     * buffer is the smaller of what the caller asked for and what can
     * actually be sent: half the slot for the request, which leaves the
     * other half for the answer, and base64 costs a third on top. */
    layout.voice_samples =
        (i32)(((i64)limits.voice_press_ms * FR_VOICE_RATE) / 1000);
    layout.voice_samples =
        fr_min(layout.voice_samples,
               (i32)((layout.io_slot_bytes / 2) * 3 / 4 / sizeof(i16)));
    layout.voice_samples = fr_max(layout.voice_samples, FR_VAD_FRAME);

    /* What friend_init itself writes comes first, in one run, so that a
     * block of reserved address space needs exactly one commit before
     * the friend can start: the context, the window the model sees,
     * consolidation's copy of what it is folding, and the two queues. */
    layout.context       = FR_RESERVE_ARRAY(&at, Friend_Context, 1);
    layout.active_events =
        FR_RESERVE_ARRAY(&at, Fr_Chat_Event, limits.max_active_events);
    layout.fold_events =
        FR_RESERVE_ARRAY(&at, Fr_Chat_Event, FR_FOLD_EVENTS_MAX);
    layout.io_slots =
        FR_RESERVE_ARRAY(&at, Fr_Io_Slot, limits.max_inflight_io);
    layout.events =
        FR_RESERVE_ARRAY(&at, Fr_Queued_Event, layout.max_events_queued);
    layout.eager = fr_align_up(at, 16);
    at           = layout.eager;

    /* Everything past here is backed as it is reached. The arenas do it
     * when they are pushed into, the graph when a slot is handed out,
     * the scrollback when the stream gets that far. */
    layout.config_arena = fr_reserve(&at, FR_CONFIG_ARENA_BYTES, 16);
    layout.prompt_page  = fr_reserve(&at, FR_PROMPT_PAGE_BYTES, 16);
    layout.scroll_positions =
        FR_RESERVE_ARRAY(&at, u64, limits.max_scrollback);
    layout.scroll_bytes = fr_reserve(
        &at, fr_align_up((size_t)limits.scrollback_bytes, 16), 16);
    layout.edges = FR_RESERVE_ARRAY(&at, Fr_Edge, limits.max_connections);
    layout.memories =
        FR_RESERVE_ARRAY(&at, Fr_Memory_Record, limits.max_memories);
    layout.io_arena      = fr_reserve(&at, (size_t)limits.io_bytes, 16);
    layout.scratch_arena = fr_reserve(&at, (size_t)limits.scratch_bytes, 16);
    layout.image_page = fr_reserve(&at, (size_t)limits.image_page_bytes, 16);
    layout.image_staging =
        fr_reserve(&at, (size_t)limits.image_staging_bytes, 16);
    layout.audio_in = FR_RESERVE_ARRAY(&at, i16, layout.voice_samples);

    layout.total = fr_align_up(at, 16);
    return layout;
}

size_t friend_required_memory(Friend_Limits limits)
{
    return fr_layout(limits).total;
}

/* ------------------------------------------------------------------ *
 *  Configuration
 * ------------------------------------------------------------------ */

/* The models each provider is asked for when a field is left empty. The
 * Gemini column is what the service was running when it stopped; the
 * other is the cheapest thing at OpenAI that does every job, and somebody
 * pointing this at their own llama.cpp types what they are serving. */
Friend_Models friend_default_models(Friend_Provider provider)
{
    Friend_Models m;
    if (provider == FRIEND_PROVIDER_OPENAI_COMPATIBLE) {
        m.chat        = str_lit("gpt-4o-mini");
        m.extract     = str_lit("gpt-4o-mini");
        m.relate      = str_lit("gpt-4o-mini");
        m.interiority = str_lit("gpt-4o-mini");
        m.rerank      = str_lit("gpt-4o-mini");
        m.embed       = str_lit("text-embedding-3-small");
        m.vision      = str_lit("gpt-4o-mini");
        m.transcribe  = str_lit("whisper-1");
        m.speak       = str_lit("gpt-4o-mini-tts");
        m.live        = str_lit("gpt-realtime-2.1");
        return m;
    }
    m.chat        = str_lit("gemini-3-flash-preview");
    m.extract     = str_lit("gemini-3-flash-preview");
    m.relate      = str_lit("gemini-2.5-flash");
    m.interiority = str_lit("gemini-3-flash-preview");
    m.rerank      = str_lit("gemini-2.5-flash");
    m.embed       = FR_EXPORT_EMBED_MODEL;
    m.vision      = str_lit("gemini-3-flash-preview");
    m.transcribe  = str_lit("gemini-2.5-flash");
    m.speak       = str_lit("gemini-2.5-flash-preview-tts");
    m.live        = str_lit("gemini-2.5-flash-native-audio-latest");
    return m;
}

#define FR_MODEL_FIELDS(X)                                                \
    X(chat) X(extract) X(relate) X(interiority) X(rerank) X(embed)        \
    X(vision) X(transcribe) X(speak) X(live)

/* What the friend calls somebody who never said. */
#define FR_USER_NAME_DEFAULT str_lit("the user")

Friend_Config friend_default_config(void)
{
    Friend_Config config;
    memset(&config, 0, sizeof(config));
    config.limits   = friend_default_limits();
    config.provider = FRIEND_PROVIDER_GEMINI;
#define FR_DEFAULT_KNOB(f, shipped) config.f = (shipped);
    FR_TUNING_FIELDS(FR_DEFAULT_KNOB)
#undef FR_DEFAULT_KNOB

    config.embodied      = 0;
    config.enable_images = 1;
    /* On, because a friend you can talk to is the thing being
     * reproduced. It costs the microphone buffer and nothing else until
     * somebody opens the microphone. */
    config.enable_voice  = 1;
    config.user_name     = FR_USER_NAME_DEFAULT;
    return config;
}

static Fr_Tuning fr_tuning_from_config(const Friend_Config *config)
{
    Fr_Tuning t;
#define FR_KNOB_OR_SHIPPED(f, shipped) \
    t.f = config->f > 0 ? config->f : (shipped);
    FR_TUNING_FIELDS(FR_KNOB_OR_SHIPPED)
#undef FR_KNOB_OR_SHIPPED

    /* Consolidation has to make progress and has to leave the window
     * valid, whatever the caller asked for. */
    t.consolidate_leaving = fr_min(t.consolidate_leaving, t.consolidate_at - 1);
    t.consolidate_leaving = fr_max(t.consolidate_leaving, 1);
    /* One pass cannot be about more messages than consolidation has
     * room to copy, because it works from a copy. */
    t.consolidate_max     = fr_clamp(1, t.consolidate_max,
                                     FR_FOLD_EVENTS_MAX);
    /* Recall's working room is fixed; asking for more than it holds
     * gets what it holds. */
    t.recall_candidates   = fr_min(t.recall_candidates, FR_CANDIDATES_MAX);
    t.recall_recent       = fr_min(t.recall_recent, FR_RECENT_MAX);
    t.recall_keep         = fr_min(t.recall_keep, t.recall_candidates);
    return t;
}

/* Copies src into the config arena. Reports failure rather than quietly
 * handing back an empty string, which is the difference between "the user
 * set no base URL" and "the base URL did not fit". */
static b32 fr_copy_config_string(Arena *a, String src, String *dest)
{
    *dest = str_push_copy(a, src);
    return src.len <= 0 || dest->len == src.len;
}

static Friend_Result fr_apply_config(Friend_Context *fr,
                                     const Friend_Config *config)
{
    Arena *a  = &fr->config_arena;
    b32    ok = 1;

    arena_reset(a);

    fr->provider           = config->provider;
    fr->utc_offset_minutes = config->utc_offset_minutes;
    fr->embodied           = config->embodied ? 1 : 0;
    fr->enable_images      = config->enable_images ? 1 : 0;
    fr->enable_voice       = config->enable_voice ? 1 : 0;
    fr->speak_replies      = config->speak_replies ? 1 : 0;
    fr->voice_barge_in     = config->voice_barge_in ? 1 : 0;
    fr->voice_mode         = config->voice_mode;
    fr->tuning             = fr_tuning_from_config(config);

    if (fr->random_seed != config->random_seed || fr->random_state == 0) {
        fr->random_seed = config->random_seed;
        fr_random_seed(fr);
    }

    ok &= fr_copy_config_string(a, config->api_key, &fr->api_key);
    ok &= fr_copy_config_string(a, config->base_url, &fr->base_url);
    ok &= fr_copy_config_string(a,
                                config->user_name.len > 0
                                    ? config->user_name
                                    : FR_USER_NAME_DEFAULT,
                                &fr->user_name);
    ok &= fr_copy_config_string(a, config->blob_path, &fr->blob_path);
    ok &= fr_copy_config_string(a, config->prompt_override,
                                &fr->prompt_override);
    ok &= fr_copy_config_string(a, config->voice, &fr->voice_name);
    {
        Friend_Models fallback = friend_default_models(config->provider);
#define FR_COPY_MODEL(f)                                                  \
        ok &= fr_copy_config_string(                                      \
            a, config->models.f.len > 0 ? config->models.f : fallback.f,  \
            &fr->models.f);
        FR_MODEL_FIELDS(FR_COPY_MODEL)
#undef FR_COPY_MODEL
    }

    if (!ok) {
        return fr_error(fr, FRIEND_ERR_FULL,
                        str_lit("configuration does not fit: its strings "
                                "are too large"));
    }
    return FRIEND_OK;
}

/* ------------------------------------------------------------------ *
 *  Init
 * ------------------------------------------------------------------ */

static void *fr_at(void *block, Fr_Region region)
{
    return (u8 *)block + region.offset;
}

/* Every region commits through here, so what is really resident is one
 * number kept as it changes rather than a sum over the regions. */
static int32_t fr_commit(void *user, void *at, size_t bytes)
{
    Friend_Context *fr = (Friend_Context *)user;
    if (!fr->commit(fr->commit_user, at, bytes)) return 0;
    fr->bytes_committed += bytes;
    return 1;
}

static Arena fr_region_arena(Friend_Context *fr, Fr_Region region)
{
    return arena_make_reserved(fr_at(fr->block, region), region.size,
                               fr->commit ? fr_commit : 0, fr);
}

static Fr_Pages fr_region_pages(Friend_Context *fr, Fr_Region region)
{
    return fr_pages_make(fr_at(fr->block, region), region.size,
                         fr->commit ? fr_commit : 0, fr);
}

Friend_Context *friend_init(void *block, size_t block_size,
                            const Friend_Config *config,
                            Friend_Result *out_result)
{
    Friend_Context *fr;
    Fr_Layout       layout;
    Friend_Result   result;
    i32             i;

    if (out_result) *out_result = FRIEND_ERR_BAD_ARGUMENT;
    if (!block || !config) return 0;

    layout = fr_layout(config->limits);
    if (block_size < layout.total) {
        if (out_result) *out_result = FRIEND_ERR_TOO_SMALL;
        return 0;
    }

    /* The front of the block, which is everything written below, and the
     * microphone buffer — the only region nothing grows into on demand,
     * because the samples arrive from a device that is not going to take
     * no for an answer. */
    if (config->commit &&
        (!config->commit(config->commit_user, block, layout.eager) ||
         !config->commit(config->commit_user, fr_at(block, layout.audio_in),
                         layout.audio_in.size))) {
        if (out_result) *out_result = FRIEND_ERR_TOO_SMALL;
        return 0;
    }

    /* One memset, once, and only over the part of the block init writes.
     * Past that nothing in the library reads memory it did not write:
     * every region knows how far anything has reached into it, so there
     * is nothing to clear and nothing to page in. */
    memset(block, 0, layout.eager);

    fr              = (Friend_Context *)fr_at(block, layout.context);
    fr->layout      = layout;
    fr->block       = block;
    fr->block_size  = block_size;
    fr->commit      = config->commit;
    fr->commit_user = config->commit_user;
    fr->bytes_committed =
        config->commit ? layout.eager + layout.audio_in.size : layout.total;

    fr->memories         = (Fr_Memory_Record *)fr_at(block, layout.memories);
    fr->memory_pages     = fr_region_pages(fr, layout.memories);
    fr->memory_capacity  = layout.limits.max_memories;
    fr->memory_free_list = FR_SLOT_NONE;

    fr->edges          = (Fr_Edge *)fr_at(block, layout.edges);
    fr->edge_pages     = fr_region_pages(fr, layout.edges);
    fr->edge_capacity  = layout.limits.max_connections;
    fr->edge_free_list = FR_SLOT_NONE;

    fr->active_events   = (Fr_Chat_Event *)fr_at(block, layout.active_events);
    fr->active_capacity = layout.limits.max_active_events;
    fr->fold_events     = (Fr_Chat_Event *)fr_at(block, layout.fold_events);

    fr->scroll.bytes     = fr_region_pages(fr, layout.scroll_bytes);
    fr->scroll.positions = fr_region_pages(fr, layout.scroll_positions);
    fr->scroll.capacity  = layout.limits.max_scrollback;

    fr->io_slots    = (Fr_Io_Slot *)fr_at(block, layout.io_slots);
    fr->io_capacity = layout.limits.max_inflight_io;
    fr->next_io_id  = 1;
    fr->io_arena    = fr_region_arena(fr, layout.io_arena);
    for (i = 0; i < fr->io_capacity; ++i) {
        fr->io_slots[i].arena =
            arena_sub(&fr->io_arena, layout.io_slot_bytes, 16);
    }

    fr->event_queue    = (Fr_Queued_Event *)fr_at(block, layout.events);
    fr->event_capacity = layout.max_events_queued;

    fr->config_arena  = fr_region_arena(fr, layout.config_arena);
    fr->prompt_page   = fr_region_arena(fr, layout.prompt_page);
    fr->scratch       = fr_region_arena(fr, layout.scratch_arena);
    fr->image_page    = fr_region_arena(fr, layout.image_page);
    fr->image_staging = fr_region_arena(fr, layout.image_staging);

    fr->audio_in       = (i16 *)fr_at(block, layout.audio_in);
    fr->audio_capacity = layout.voice_samples;

    fr_state_clear(fr);

    result = fr_apply_config(fr, config);
    if (out_result) *out_result = result;
    if (result != FRIEND_OK) return 0;
    fr_set_name(fr, config->friend_name);
    return fr;
}

/* A friend from nothing. fr_state_clear is what load and import use to
 * make room for what they are about to put in; this is the same thing
 * with nothing following it. */
Friend_Result friend_begin(Friend_Context *fr, Friend_String name)
{
    char kept[FR_NAME_MAX];
    i32  kept_len;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    if (name.len <= 0) {
        kept_len = fr->friend_name.len;
        memcpy(kept, fr->friend_name.data ? fr->friend_name.data : "",
               (size_t)kept_len);
        name = str_make(kept, kept_len);
    }
    fr_state_clear(fr);
    fr_set_name(fr, name);
    /* The library's only clock is the one friend_tick is handed. */
    fr->created_unix_ms = fr->now_unix_ms;
    fr_mark_dirty(fr);
    return FRIEND_OK;
}

/* Everything a friend knows, forgotten; everything about how it runs,
 * kept. */
static void fr_state_clear(Friend_Context *fr)
{
    /* In this order. The voice first, because ending a realtime session
     * posts the socket's close; then every request, that close and the
     * socket included, is given up, so nothing that comes back for this
     * friend is routed anywhere; then the events nobody has drained,
     * which are about messages that are about to stop existing, once
     * nothing is left that could push another; and then what the
     * friend knows, module by module, in no order that matters. */
    fr_voice_reset(fr);
    fr_io_abandon_all(fr);
    fr_events_reset(fr);

    fr_graph_reset(fr);
    fr_turn_reset(fr);
    fr_memory_reset(fr);
    fr_blob_reset(fr);

    fr->friend_name     = str_nil;
    fr->created_unix_ms = 0;
    fr->unsaved         = 0;
}

Friend_Result friend_reconfigure(Friend_Context *fr,
                                 const Friend_Config *config)
{
    if (!fr || !config) return FRIEND_ERR_BAD_ARGUMENT;
    /* Limits are structural: they decided the layout of a block we are
     * already living in. Everything else is fair game. */
    return fr_apply_config(fr, config);
}

/* ------------------------------------------------------------------ *
 *  Errors and randomness
 * ------------------------------------------------------------------ */

static Friend_Result fr_error(Friend_Context *fr, Friend_Result result, String message)
{
    i32 len;
    if (!fr) return result;
    len = fr_min(message.len, FR_ERROR_MAX - 1);
    if (len > 0) memcpy(fr->error_buffer, message.data, (size_t)len);
    fr->error_buffer[len] = 0;
    fr->error_len         = len;
    return result;
}

Friend_String friend_last_error(Friend_Context *fr)
{
    if (!fr) return str_nil;
    return str_make(fr->error_buffer, fr->error_len);
}

static void fr_random_seed(Friend_Context *fr)
{
    /* splitmix64 over the clock: one multiply and two shifts, which is
     * plenty to turn a millisecond count into a seed that does not share
     * its low bits with the last session's. */
    u64 x = fr->now_unix_ms + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;

    if (fr->random_seed) {
        fr->random_state  = fr->random_seed;
        fr->random_seeded = 1;
        return;
    }
    /* Any nonzero state will do; xorshift cannot start at zero. Before
     * the first tick the clock says 1970, which is no seed at all, so
     * the tick seeds again. */
    fr->random_state  = x ? x : 0x9E3779B97F4A7C15ull;
    fr->random_seeded = fr->now_unix_ms != 0;
}

/* xorshift64*. Seeded from the config, so a fixed seed replays. */
static u32 fr_random_u32(Friend_Context *fr)
{
    u64 x = fr->random_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    fr->random_state = x;
    return (u32)((x * 0x2545F4914F6CDD1Dull) >> 32);
}

