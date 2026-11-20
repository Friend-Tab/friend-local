/* fr_tests.c — the library's tests.
 *
 * Includes the library as source. What a frontend can see is checked
 * through friend.h; the algorithms that are hard to get right and easy to
 * break without anything visible changing at once — the parsers, the
 * inflater, the scrollback ring, recall's arithmetic, the exact text the
 * models are shown — are checked through their own internal interfaces.
 * It prints the failures and exits 1, or exits 0. */

#include <stdlib.h>

#include "../src/friend_unity.c"

#include "../../base/fr_test.h"
#include "fr_zip_data.h"
#include "fr_export_data.h"

/* A scratch arena for tests that need one. */
static u8    test_memory[FR_MB(4)];
static Arena scratch;

static void scratch_reset(void) { scratch = arena_make(test_memory, sizeof(test_memory)); }

/* One message out of the scrollback, by value, so a check can read a
 * field of it without four lines of ceremony. An index that is not
 * there comes back zeroed rather than as a failed call, which is what
 * makes it safe inside a CHECK. */
static Fr_Scroll_View scroll_at(Friend_Context *fr, i32 index)
{
    Fr_Scroll_View view;
    memset(&view, 0, sizeof(view));
    fr_scroll_view(fr, index, &view);
    return view;
}

/* ------------------------------------------------------------------ */

static void test_strings(void)
{
    String s = str_lit("hello, friend");

    CHECK(s.len == 13);
    CHECK(str_eq(s, str_lit("hello, friend")));
    CHECK(!str_eq(s, str_lit("hello, friends")));
    CHECK(str_eq_ci(str_lit("GEMINI"), str_lit("gemini")));
    CHECK(str_is_empty(str_nil));
    CHECK(!str_is_empty(s));

    CHECK_STR(str_take(s, 5), "hello");
    CHECK_STR(str_skip(s, 7), "friend");
    CHECK_STR(str_tail(s, 6), "friend");
    CHECK_STR(str_slice(s, 7, 100), "friend");  /* clamped, not undefined */
    CHECK_STR(str_slice(s, 100, 5), "");
    CHECK_STR(str_slice(s, -4, 5), "hello");
    CHECK_STR(str_tail(s, 999), "hello, friend");

    CHECK(str_has_prefix(s, str_lit("hello")));
    CHECK(!str_has_prefix(s, str_lit("friend")));
    CHECK(str_has_suffix(s, str_lit("friend")));
    CHECK(str_index_of_char(s, ',') == 5);
    CHECK(str_index_of_char(s, 'z') == -1);
    CHECK(str_index_of(s, str_lit("friend")) == 7);
    CHECK(str_index_of(s, str_lit("nope")) == -1);
    CHECK(str_index_of(s, str_nil) == 0);

    CHECK_STR(str_trim(str_lit("  \t spaced \n ")), "spaced");
    CHECK_STR(str_trim(str_lit("   ")), "");
}

static void test_lines(void)
{
    String rest = str_lit("one\ntwo\r\n\nfour");
    String line = str_nil;

    CHECK(str_next_line(&rest, &line));
    CHECK_STR(line, "one");
    CHECK(str_next_line(&rest, &line));
    CHECK_STR(line, "two");       /* the \r goes with the terminator */
    CHECK(str_next_line(&rest, &line));
    CHECK_STR(line, "");
    CHECK(str_next_line(&rest, &line));
    CHECK_STR(line, "four");
    CHECK(!str_next_line(&rest, &line));
}

static void test_number_parsing(void)
{
    i64 i = 0;
    f64 f = 0;

    CHECK(str_to_i64(str_lit("0"), &i) && i == 0);
    CHECK(str_to_i64(str_lit("1793625424"), &i) && i == 1793625424);
    CHECK(str_to_i64(str_lit("-42"), &i) && i == -42);
    CHECK(str_to_i64(str_lit("  7  "), &i) && i == 7);
    CHECK(!str_to_i64(str_lit(""), &i));
    CHECK(!str_to_i64(str_lit("-"), &i));
    CHECK(!str_to_i64(str_lit("12a"), &i));
    CHECK(!str_to_i64(str_lit("1.5"), &i));

    CHECK(str_to_f64(str_lit("0.5"), &f) && f > 0.4999 && f < 0.5001);
    CHECK(str_to_f64(str_lit("-2.25"), &f) && f > -2.2501 && f < -2.2499);
    CHECK(str_to_f64(str_lit("3"), &f) && f > 2.999 && f < 3.001);
    CHECK(str_to_f64(str_lit("1e3"), &f) && f > 999.9 && f < 1000.1);
    CHECK(str_to_f64(str_lit("5e-1"), &f) && f > 0.4999 && f < 0.5001);
    CHECK(!str_to_f64(str_lit("1.2.3"), &f));
    CHECK(!str_to_f64(str_lit("abc"), &f));
}

static void test_arena_region(void)
{
    Arena      a = arena_make(test_memory, 256);
    Arena_Mark mark;
    u8        *first;
    u64       *aligned;

    first = arena_push(&a, u8, 3);
    CHECK(first != 0);
    CHECK(first[0] == 0 && first[2] == 0); /* pushes are zeroed */
    CHECK(a.used == 3);

    aligned = arena_push(&a, u64, 1);
    CHECK(aligned != 0);
    CHECK(((size_t)(void *)aligned % FR_ALIGNOF(u64)) == 0);

    mark = arena_mark(&a);
    CHECK(arena_push(&a, u8, 64) != 0);
    arena_reset_to(&a, mark);
    CHECK(a.used == mark);

    /* Exhaustion is a returned null, not a crash and not a resize. */
    CHECK(arena_push(&a, u8, 1024) == 0);
    CHECK(a.used == mark); /* a failed push consumes nothing */
    CHECK(arena_remaining(&a) == 256 - mark);

    CHECK(a.peak >= mark + 64);
    arena_reset(&a);
    CHECK(a.used == 0);
}

static void test_builder(void)
{
    String_Builder sb;
    String         out;
    Arena          tiny;

    scratch_reset();
    sb = sb_begin(&scratch);
    sb_append(&sb, str_lit("hello"));
    sb_append_char(&sb, ',');
    sb_append_char(&sb, ' ');
    sb_append_cstr(&sb, "friend");
    sb_append_char(&sb, ' ');
    sb_append_i64(&sb, 0);
    sb_append_char(&sb, ' ');
    sb_append_i64(&sb, -12345);
    sb_appendf(&sb, " [%s=%d]", "n", 7);
    out = sb_done(&sb);
    CHECK_STR(out, "hello, friend 0 -12345 [n=7]");
    CHECK(!sb.overflowed);

    /* The extremes of sb_append_i64's magnitude handling. */
    scratch_reset();
    sb = sb_begin(&scratch);
    sb_append_i64(&sb, (i64)0x8000000000000000ull); /* INT64_MIN */
    CHECK_STR(sb_done(&sb), "-9223372036854775808");

    scratch_reset();
    sb = sb_begin(&scratch);
    sb_append_i64(&sb, 9223372036854775807ll);
    CHECK_STR(sb_done(&sb), "9223372036854775807");

    /* Running out of arena marks the builder and truncates; it does not
     * scribble past the end. */
    tiny = arena_make(test_memory, 8);
    sb   = sb_begin(&tiny);
    sb_append(&sb, str_lit("12345678901234567890"));
    CHECK(sb.overflowed);
    CHECK(sb_done(&sb).len <= 8);
}

static void test_base64(void)
{
    u8     bytes[64];
    char   encoded[128];
    i64    n;
    size_t size;

    CHECK(fr_base64_decoded_size(str_lit("ZnJpZW5k")) == 6);
    n = fr_base64_decode(str_lit("ZnJpZW5k"), bytes, sizeof(bytes));
    CHECK(n == 6);
    CHECK(str_eq(str_make((const char *)bytes, (i32)n), str_lit("friend")));

    /* Both padded lengths. */
    n = fr_base64_decode(str_lit("Zg=="), bytes, sizeof(bytes));
    CHECK(n == 1 && bytes[0] == 'f');
    n = fr_base64_decode(str_lit("ZnI="), bytes, sizeof(bytes));
    CHECK(n == 2 && bytes[1] == 'r');

    /* Missing padding and embedded whitespace both occur in the wild. */
    n = fr_base64_decode(str_lit("ZnJpZW5k"), bytes, sizeof(bytes));
    CHECK(n == 6);
    n = fr_base64_decode(str_lit("Zn Jp\nZW5k"), bytes, sizeof(bytes));
    CHECK(n == 6);

    /* URL-safe alphabet. */
    n = fr_base64_decode(str_lit("-_8="), bytes, sizeof(bytes));
    CHECK(n == 2 && bytes[0] == 0xFB && bytes[1] == 0xFF);

    CHECK(fr_base64_decode(str_lit("!!!!"), bytes, sizeof(bytes)) == -1);
    CHECK(fr_base64_decode(str_lit("ZnJpZW5k"), bytes, 2) == -1);

    /* Round trip, including the embedding-shaped case of raw bytes. */
    {
        u8  raw[5] = { 0, 1, 2, 3, 4 };
        size = fr_base64_encoded_size(sizeof(raw));
        CHECK(size == 8);
        fr_base64_encode(raw, sizeof(raw), 0, 0, encoded);
        CHECK(str_eq(str_make(encoded, (i32)size), str_lit("AAECAwQ=")));
        n = fr_base64_decode(str_make(encoded, (i32)size), bytes,
                             sizeof(bytes));
        CHECK(n == 5 && memcmp(bytes, raw, 5) == 0);
    }
    {
        i32 i;
        u8  raw[48];
        for (i = 0; i < 48; ++i) raw[i] = (u8)(i * 5 + 1);
        size = fr_base64_encoded_size(sizeof(raw));
        fr_base64_encode(raw, sizeof(raw), 0, 0, encoded);
        n = fr_base64_decode(str_make(encoded, (i32)size), bytes,
                             sizeof(bytes));
        CHECK(n == 48 && memcmp(bytes, raw, 48) == 0);
    }
}

/* The exact strings the service put in front of the model. */
static void test_human_duration(void)
{
    const i64 second = 1000;
    const i64 minute = 60 * second;
    const i64 hour   = 60 * minute;
    const i64 day    = 24 * hour;

    scratch_reset();
    CHECK_STR(fr_human_duration(&scratch, 0), "less than a second");
    CHECK_STR(fr_human_duration(&scratch, 500), "less than a second");
    CHECK_STR(fr_human_duration(&scratch, second), "1 second");
    CHECK_STR(fr_human_duration(&scratch, 45 * second), "45 seconds");
    CHECK_STR(fr_human_duration(&scratch, 3 * day), "3 days");
    CHECK_STR(fr_human_duration(&scratch, 14 * day), "2 weeks");
    /* 0.96 of an hour rounds to the unit, because the service's tolerance
     * was a tenth of the primary unit. */
    CHECK_STR(fr_human_duration(&scratch, (i64)(0.96 * (f64)hour)),
              "1 hour");
    CHECK_STR(fr_human_duration(&scratch, 90 * minute),
              "1 hour and 30 minutes");
    CHECK_STR(fr_human_duration(&scratch, 45 * day),
              "1 month and 2 weeks");
    CHECK_STR(fr_human_duration(&scratch, 2 * hour + 14 * minute),
              "2 hours and 14 minutes");
}

static void test_date_time(void)
{
    Fr_Date_Time dt;

    scratch_reset();

    dt = fr_date_time(0, 0);
    CHECK(dt.year == 1970 && dt.month == 1 && dt.day == 1);
    CHECK(dt.hour == 0 && dt.minute == 0 && dt.second == 0);
    CHECK(dt.weekday == 4); /* Thursday */
    CHECK_STR(fr_format_weekday(dt), "Thursday");
    CHECK_STR(fr_format_timestamp(&scratch, 0, 0, 1),
              "1/1/1970 12:00:00 AM");

    dt = fr_date_time(1793625424ull * 1000ull, 0);
    CHECK(dt.year == 2026 && dt.month == 11 && dt.day == 2);
    CHECK(dt.hour == 13 && dt.minute == 17 && dt.second == 4);
    CHECK_STR(fr_format_weekday(dt), "Monday");
    CHECK_STR(fr_format_timestamp(&scratch, 1793625424ull * 1000ull, 0, 1),
              "11/2/2026 1:17:04 PM");
    CHECK_STR(fr_format_timestamp(&scratch, 1793625424ull * 1000ull, 0, 0),
              "11/2/2026 1:17 PM");

    /* A leap day, and an end-of-century boundary. */
    dt = fr_date_time(1709208000ull * 1000ull, 0);
    CHECK(dt.year == 2024 && dt.month == 2 && dt.day == 29);
    dt = fr_date_time(946684799ull * 1000ull, 0);
    CHECK(dt.year == 1999 && dt.month == 12 && dt.day == 31);
    CHECK(dt.hour == 23 && dt.minute == 59 && dt.second == 59);
    CHECK_STR(fr_format_weekday(dt), "Friday");

    /* A negative offset must roll the civil date back a day. */
    dt = fr_date_time(1793625424ull * 1000ull, -8 * 60);
    CHECK(dt.year == 2026 && dt.month == 11 && dt.day == 2 && dt.hour == 5);
    dt = fr_date_time(1793625424ull * 1000ull, -14 * 60);
    CHECK(dt.month == 11 && dt.day == 1 && dt.hour == 23);
}

/* ------------------------------------------------------------------ */

static void test_json_reading(void)
{
    String text = str_lit(
        "{ \"friendName\": \"Em\", \"memoryCount\": 1402,"
        "  \"totalChatEventCount\": \"18446744073709\","
        "  \"interiority\": \"line one\\nline two\","
        "  \"escapes\": \"quote \\\" slash \\/ tab \\t\","
        "  \"unicode\": \"caf\\u00e9 \\ud83d\\ude42\","
        "  \"epochActive\": true, \"disembodied\": false,"
        "  \"matchStrength\": 0.6125,"
        "  \"dateCreated\": \"2026-11-02T13:17:04.512Z\","
        "  \"embeddingInt8\": \"AAECAwQ=\","
        "  \"nested\": { \"a\": [1, 2, { \"b\": [] }], \"c\": null },"
        "  \"elements\": [ { \"id\": \"x\" }, { \"id\": \"y\" } ],"
        "  \"last\": 7 }");

    Json_Parser p = json_begin(text);
    String      key;
    String      name = str_nil, interiority = str_nil, escapes = str_nil;
    String      unicode = str_nil;
    i64         memory_count = 0, total_events = 0, last = 0;
    b32         epoch_active = 0, disembodied = 1;
    f64         match_strength = 0;
    u64         created = 0;
    u8          embedding[8];
    i64         embedding_len = 0;
    i32         element_count = 0;

    scratch_reset();
    CHECK(json_peek(&p) == JSON_OBJECT);
    CHECK(json_object_begin(&p));
    while (json_object_next(&p, &key)) {
        if (str_eq(key, str_lit("friendName"))) {
            name = json_string(&p, &scratch);
        } else if (str_eq(key, str_lit("memoryCount"))) {
            memory_count = json_i64(&p);
        } else if (str_eq(key, str_lit("totalChatEventCount"))) {
            total_events = json_i64(&p); /* protojson quotes 64-bit ints */
        } else if (str_eq(key, str_lit("interiority"))) {
            interiority = json_string(&p, &scratch);
        } else if (str_eq(key, str_lit("escapes"))) {
            escapes = json_string(&p, &scratch);
        } else if (str_eq(key, str_lit("unicode"))) {
            unicode = json_string(&p, &scratch);
        } else if (str_eq(key, str_lit("epochActive"))) {
            epoch_active = json_bool(&p);
        } else if (str_eq(key, str_lit("disembodied"))) {
            disembodied = json_bool(&p);
        } else if (str_eq(key, str_lit("matchStrength"))) {
            match_strength = json_f64(&p);
        } else if (str_eq(key, str_lit("dateCreated"))) {
            created = json_timestamp_ms(&p);
        } else if (str_eq(key, str_lit("embeddingInt8"))) {
            embedding_len = json_base64(&p, embedding, sizeof(embedding));
        } else if (str_eq(key, str_lit("elements"))) {
            json_array_begin(&p);
            while (json_array_next(&p)) {
                ++element_count;
                json_skip(&p);
            }
        } else if (str_eq(key, str_lit("last"))) {
            last = json_i64(&p);
        } else {
            json_skip(&p); /* including the deeply nested object */
        }
    }

    CHECK(!p.failed);
    CHECK_STR(name, "Em");
    CHECK(memory_count == 1402);
    CHECK(total_events == 18446744073709ll);
    CHECK_STR(interiority, "line one\nline two");
    CHECK_STR(escapes, "quote \" slash / tab \t");
    CHECK_STR(unicode, "caf\xc3\xa9 \xf0\x9f\x99\x82"); /* é and a surrogate pair */
    CHECK(epoch_active == 1);
    CHECK(disembodied == 0);
    CHECK(match_strength > 0.6124 && match_strength < 0.6126);
    CHECK(created == 1793625424512ull);
    CHECK(embedding_len == 5 && embedding[0] == 0 && embedding[4] == 4);
    CHECK(element_count == 2);
    CHECK(last == 7); /* the skips left the walk in the right place */
}

static void test_json_edges(void)
{
    /* An unescaped string is returned as a view, not a copy. */
    {
        String      text = str_lit("\"plain\"");
        Json_Parser p    = json_begin(text);
        String      s;
        scratch_reset();
        s = json_string(&p, &scratch);
        CHECK_STR(s, "plain");
        CHECK(s.data == text.data + 1); /* no copy was made */
        CHECK(scratch.used == 0);
    }
    /* Failure is sticky and every later read is harmless. */
    {
        Json_Parser p = json_begin(str_lit("{ \"a\": }"));
        String      key;
        CHECK(json_object_begin(&p));
        CHECK(json_object_next(&p, &key));
        CHECK(json_i64(&p) == 0);
        CHECK(p.failed);
        CHECK(json_i64(&p) == 0);
        CHECK(!json_object_next(&p, &key));
        CHECK(p.error.len > 0);
    }
    /* An unterminated string fails rather than running off the end. */
    {
        Json_Parser p = json_begin(str_lit("\"no end"));
        (void)json_string_raw(&p);
        CHECK(p.failed);
    }
    /* Empty containers. */
    {
        Json_Parser p = json_begin(str_lit("{}"));
        String      key;
        CHECK(json_object_begin(&p));
        CHECK(!json_object_next(&p, &key));
        CHECK(!p.failed);
    }
    {
        Json_Parser p = json_begin(str_lit("[]"));
        CHECK(json_array_begin(&p));
        CHECK(!json_array_next(&p));
        CHECK(!p.failed);
    }
    /* json_skip over every shape, leaving the parser positioned. */
    {
        Json_Parser p = json_begin(
            str_lit("[ {\"a\":[1,{\"b\":\"]\"}]}, \"str\", 1.5e3, true,"
                    " null, 42 ]"));
        i32 count = 0;
        CHECK(json_array_begin(&p));
        while (json_array_next(&p)) {
            ++count;
            if (count == 6) {
                CHECK(json_i64(&p) == 42);
            } else {
                json_skip(&p);
            }
        }
        CHECK(!p.failed);
        CHECK(count == 6);
    }
    /* Timestamps without fractional seconds, and with an offset. */
    {
        Json_Parser p = json_begin(str_lit("\"2026-11-02T13:17:04Z\""));
        CHECK(json_timestamp_ms(&p) == 1793625424000ull);
    }
    {
        Json_Parser p = json_begin(str_lit("\"2026-11-02T14:17:04+01:00\""));
        CHECK(json_timestamp_ms(&p) == 1793625424000ull);
    }
}

static void test_json_writing(void)
{
    Json_Writer w;
    String      out;
    u8          bytes[3];

    bytes[0] = 1;
    bytes[1] = 2;
    bytes[2] = 3;

    scratch_reset();
    w = jw_begin(&scratch);
    jw_object_begin(&w);
    jw_field_string(&w, str_lit("model"), str_lit("gemini-3-flash-preview"));
    jw_key(&w, str_lit("contents"));
    jw_array_begin(&w);
    jw_object_begin(&w);
    jw_field_string(&w, str_lit("role"), str_lit("user"));
    jw_key(&w, str_lit("parts"));
    jw_array_begin(&w);
    jw_object_begin(&w);
    jw_field_string(&w, str_lit("text"), str_lit("hey\n\"you\"\there"));
    jw_object_end(&w);
    jw_array_end(&w);
    jw_object_end(&w);
    jw_array_end(&w);
    jw_field_i64(&w, str_lit("thinkingBudget"), 128);
    jw_field_bool(&w, str_lit("stream"), 1);
    jw_key(&w, str_lit("embedding"));
    jw_base64(&w, bytes, sizeof(bytes));
    jw_key(&w, str_lit("nothing"));
    jw_null(&w);
    jw_object_end(&w);
    out = jw_done(&w);

    CHECK_STR(out,
              "{\"model\":\"gemini-3-flash-preview\",\"contents\":[{\"role\":"
              "\"user\",\"parts\":[{\"text\":\"hey\\n\\\"you\\\"\\there\"}]}],"
              "\"thinkingBudget\":128,\"stream\":true,\"embedding\":\"AQID\","
              "\"nothing\":null}");

    /* Control characters become \u00XX so the result is always valid. */
    scratch_reset();
    w = jw_begin(&scratch);
    jw_string(&w, str_make("a\x01\x1f", 3));
    CHECK_STR(jw_done(&w), "\"a\\u0001\\u001f\"");

    /* What we write, we can read back. */
    {
        Json_Parser p;
        String      key, text = str_nil;
        scratch_reset();
        w = jw_begin(&scratch);
        jw_object_begin(&w);
        jw_field_string(&w, str_lit("text"), str_lit("tab\there \"quoted\""));
        jw_object_end(&w);
        out = jw_done(&w);

        p = json_begin(out);
        CHECK(json_object_begin(&p));
        while (json_object_next(&p, &key)) {
            if (str_eq(key, str_lit("text"))) text = json_string(&p, &scratch);
            else json_skip(&p);
        }
        CHECK(!p.failed);
        CHECK_STR(text, "tab\there \"quoted\"");
    }
}

/* ------------------------------------------------------------------ *
 *  Friends to test, and a provider to talk to
 * ------------------------------------------------------------------ */

/* Some time on 2 November 2025, for a clock that has to say something. */
#define TEST_NOW 1762100000000ull

/* Small, so that a test that fills something fills it quickly. */
static Friend_Limits small_limits(void)
{
    Friend_Limits limits = friend_default_limits();
    limits.max_memories      = 16;
    limits.max_connections   = 32;
    limits.max_active_events = 8;
    limits.max_scrollback    = 16;
    limits.scrollback_bytes  = (i32)FR_KB(64);
    limits.max_inflight_io   = 4;
    limits.io_bytes          = (i32)FR_MB(1);
    limits.scratch_bytes     = (i32)FR_MB(1);
    limits.image_page_bytes  = (i32)FR_KB(64);
    limits.voice_press_ms    = 3000;
    return limits;
}

/* Room for the export fixture and a little more. */
static Friend_Limits export_limits(void)
{
    Friend_Limits limits = friend_default_limits();
    limits.max_memories      = 64;
    limits.max_connections   = 128;
    limits.max_active_events = 8;
    limits.max_scrollback    = 16;
    limits.scratch_bytes     = (i32)FR_MB(2);
    return limits;
}

static Friend_Config test_config(Friend_Provider provider, Friend_Limits limits)
{
    Friend_Config config = friend_default_config();
    config.limits      = limits;
    config.provider    = provider;
    config.api_key     = str_lit("test-key");
    config.friend_name = str_lit("Em");
    config.user_name   = str_lit("Sam");
    config.random_seed = 12345;
    return config;
}

/* The one way a test gets a friend: a block of exactly the size the
 * configuration asks for. *out_block is the caller's to free, friend or
 * not. */
static Friend_Context *make_friend_with(void **out_block,
                                        const Friend_Config *config)
{
    size_t          need = friend_required_memory(config->limits);
    Friend_Context *fr;

    *out_block = malloc(need);
    fr         = *out_block ? friend_init(*out_block, need, config, 0) : 0;
    CHECK(fr != 0);
    return fr;
}

static Friend_Context *make_friend(void **out_block, Friend_Provider provider)
{
    Friend_Config config = test_config(provider, small_limits());
    return make_friend_with(out_block, &config);
}

/* Places a memory in the graph with a made-up vector pointing along one
 * axis, so recall has something to find and something to tell apart. */
static u32 add_memory(Friend_Context *fr, String description, u64 created,
                      i32 axis)
{
    u32               slot = fr_memory_alloc(fr);
    Fr_Memory_Record *m;

    if (slot == FR_SLOT_NONE) return slot;
    m = &fr->memories[slot];
    memcpy(m->description, description.data, (size_t)description.len);
    m->description_len    = (u16)description.len;
    m->created_unix_ms    = created;
    m->dialog_end_unix_ms = created;
    m->needs_embedding    = 0;
    if (axis >= 0 && axis < FR_EMBED_DIMS) m->embedding[axis] = 100;
    return slot;
}

/* The public view of the memory in a slot, found by its id. */
static b32 memory_node(Friend_Context *fr, u32 slot, Friend_Memory_Node *out)
{
    i32 i;
    for (i = 0; i < friend_memory_count(fr); ++i) {
        if (friend_memory_at(fr, i, out) == FRIEND_OK && out->id.slot == slot) {
            return 1;
        }
    }
    memset(out, 0, sizeof(*out));
    return 0;
}

/* How many edges run from one memory to another, as a frontend sees the
 * graph. */
static i32 edges_between(Friend_Context *fr, u32 from, u32 to,
                         Friend_Relation *out_relation)
{
    i32 found = 0;
    i32 i;
    for (i = 0; i < friend_memory_edge_count(fr); ++i) {
        Friend_Memory_Edge edge;
        if (friend_memory_edge_at(fr, i, &edge) != FRIEND_OK) continue;
        if (edge.from.slot != from || edge.to.slot != to) continue;
        if (out_relation) *out_relation = edge.relation;
        ++found;
    }
    return found;
}

/* Hands the friend an answer without a network: write the bytes into the
 * buffer it provided, and tell it they arrived. */
static void answer_request(Friend_Context *fr, Friend_Io_Request *req,
                           String body, i32 status)
{
    i32 len = body.len;
    if (len > (i32)req->recv_capacity) len = (i32)req->recv_capacity;
    memcpy(req->recv, body.data, (size_t)len);
    friend_io_progress(fr, req->id, (u32)len);
    friend_io_complete(fr, req->id, status);
}

/* An embedding answer whose only non-zero value is on one axis. */
static String embedding_answer(Arena *arena, i32 axis)
{
    String_Builder sb = sb_begin(arena);
    i32            i;
    sb_append(&sb, str_lit("{\"embedding\":{\"values\":["));
    for (i = 0; i < FR_EMBED_DIMS; ++i) {
        if (i) sb_append_char(&sb, ',');
        sb_append(&sb, i == axis ? str_lit("1.0") : str_lit("0.0"));
    }
    sb_append(&sb, str_lit("]}}"));
    return sb_done(&sb);
}

/* Gemini's answer to a request that was not streamed. */
static String gemini_answer(Arena *arena, String text)
{
    Json_Writer w = jw_begin(arena);
    jw_object_begin(&w);
    jw_key(&w, str_lit("candidates"));
    jw_array_begin(&w);
    jw_object_begin(&w);
    jw_key(&w, str_lit("content"));
    jw_object_begin(&w);
    jw_key(&w, str_lit("parts"));
    jw_array_begin(&w);
    jw_object_begin(&w);
    jw_field_string(&w, str_lit("text"), text);
    jw_object_end(&w);
    jw_array_end(&w);
    jw_object_end(&w);
    jw_object_end(&w);
    jw_array_end(&w);
    jw_object_end(&w);
    return jw_done(&w);
}

/* A model provider that is not one, on Gemini's shapes. Every request is
 * recognised by what it asks for, the way a person reading it would, and
 * answered with the field for that kind of call; a field left null fails
 * that kind of call instead. Photo files are not here: their requests
 * fail. */
typedef struct Fake_Provider {
    const char *reply;
    const char *rerank;
    const char *extract;
    const char *relate;
    const char *interiority;
    i32         embed_axis;

    i32 replies, reranks, extracts, relates, interiorities, embeds, others;

    /* The body of the last relation call, which a test may want to read
     * after the request it came in is gone. */
    char last_relate[16384];
    i32  last_relate_len;
} Fake_Provider;

static u8 fake_memory[FR_MB(1)];

static void fake_answer(Friend_Context *fr, Fake_Provider *fake,
                        Friend_Io_Request *req)
{
    Arena       arena  = arena_make(fake_memory, sizeof(fake_memory));
    const char *answer = 0;
    b32         stream = 0;

    if (req->kind != FRIEND_IO_HTTPS) {
        ++fake->others;
        friend_io_failed(fr, req->id, str_lit("not in this test"));
        return;
    }
    if (str_index_of(req->url, str_lit(":embedContent")) >= 0) {
        ++fake->embeds;
        answer_request(fr, req, embedding_answer(&arena, fake->embed_axis),
                       200);
        return;
    }
    if (str_index_of(req->url, str_lit("streamGenerateContent")) >= 0) {
        ++fake->replies;
        answer = fake->reply;
        stream = 1;
    } else if (str_index_of(req->body, str_lit("[Sentence A]")) >= 0) {
        ++fake->relates;
        answer = fake->relate;
        fake->last_relate_len = fr_min(req->body.len,
                                       (i32)sizeof(fake->last_relate));
        memcpy(fake->last_relate, req->body.data,
               (size_t)fake->last_relate_len);
    } else if (str_index_of(req->body, str_lit("Write your interior "
                                               "thoughts now.")) >= 0) {
        ++fake->interiorities;
        answer = fake->interiority;
    } else if (str_index_of(req->body, str_lit("The remembered events:")) >=
               0) {
        ++fake->reranks;
        answer = fake->rerank;
    } else if (str_index_of(req->body, str_lit("Extract events from this "
                                               "conversation")) >= 0) {
        ++fake->extracts;
        answer = fake->extract;
    } else {
        ++fake->others;
    }
    if (!answer) {
        friend_io_failed(fr, req->id, str_lit("not in this test"));
        return;
    }
    if (stream) {
        Json_Writer    w = jw_begin(&arena);
        String_Builder sb;
        String         json;
        jw_string(&w, str_from_cstr(answer));
        json = jw_done(&w);
        sb   = sb_begin(&arena);
        sb_append(&sb, str_lit("data: {\"candidates\":[{\"content\":{\"parts\":"
                               "[{\"text\":"));
        sb_append(&sb, json);
        sb_append(&sb, str_lit("}]},\"finishReason\":\"STOP\"}]}\n"));
        answer_request(fr, req, sb_done(&sb), 200);
        return;
    }
    answer_request(fr, req, gemini_answer(&arena, str_from_cstr(answer)), 200);
}

/* Answers everything the friend asks until it stops asking, a tenth of a
 * second of its clock at a time. Bounded, so a friend that never stops
 * fails a check rather than hanging the run. */
static void fake_serve(Friend_Context *fr, Fake_Provider *fake, u64 *now)
{
    Friend_Io_Request requests[16];
    i32               round;

    for (round = 0; round < 200; ++round) {
        i32 n = friend_pending_io(fr, requests, 16);
        i32 i;
        for (i = 0; i < n; ++i) fake_answer(fr, fake, &requests[i]);
        *now += 100;
        friend_tick(fr, *now);
        if (n == 0 && friend_stats(fr).io_inflight == 0) return;
    }
    FAIL("the friend never stopped asking");
}

/* ------------------------------------------------------------------ */

static void test_limits(void)
{
    /* Zero means the default, so a frontend that sets nothing gets a
     * friend rather than an error. */
    {
        Friend_Limits zeroed;
        memset(&zeroed, 0, sizeof(zeroed));
        CHECK(friend_required_memory(zeroed) ==
              friend_required_memory(friend_default_limits()));
    }
    /* Absurd limits are made workable rather than trusted: the
     * scrollback can never be smaller than the live window, and always
     * has room for the longest message there is. */
    {
        Friend_Limits silly = small_limits();
        Friend_Limits normalized;
        silly.max_memories      = 1;
        silly.max_active_events = 1;
        silly.max_scrollback    = 1;
        silly.scrollback_bytes  = 1;
        normalized              = fr_normalize_limits(silly);
        CHECK(normalized.max_scrollback >= normalized.max_active_events);
        CHECK((size_t)normalized.scrollback_bytes >
              sizeof(Fr_Message_Header) + FR_CHAT_TEXT_MAX);
    }
}

static void test_init(void)
{
    char              key[16];
    char              name[8];
    Friend_Config     config = test_config(FRIEND_PROVIDER_GEMINI,
                                           small_limits());
    Friend_Result     result = FRIEND_OK;
    Friend_Context   *fr;
    Friend_Io_Request request;
    size_t            need;
    void             *block;

    memcpy(key, "test-key", 9);
    memcpy(name, "Em", 3);
    config.api_key     = str_make(key, 8);
    config.friend_name = str_make(name, 2);

    need  = friend_required_memory(config.limits);
    block = malloc(need);
    CHECK(block != 0);
    if (!block) return;

    /* One byte short is a clean refusal, not a crash. */
    CHECK(friend_init(block, need - 1, &config, &result) == 0);
    CHECK(result == FRIEND_ERR_TOO_SMALL);
    CHECK(friend_init(0, need, &config, &result) == 0);
    CHECK(result == FRIEND_ERR_BAD_ARGUMENT);

    fr = friend_init(block, need, &config, &result);
    CHECK(fr != 0);
    CHECK(result == FRIEND_OK);
    if (!fr) {
        free(block);
        return;
    }

    {
        Friend_Stats stats = friend_stats(fr);
        CHECK(stats.memories == 0);
        CHECK(stats.connections == 0);
        CHECK(friend_message_count(fr) == 0);
        CHECK(stats.bytes_total == need);
        CHECK(stats.bytes_used <= need);
        CHECK(friend_last_error(fr).len == 0);
    }

    /* A seeded sequence is a sequence, and it repeats. */
    {
        u32 a = fr_random_u32(fr);
        u32 b = fr_random_u32(fr);
        CHECK(a != b);
        fr = friend_init(block, need, &config, &result);
        CHECK(fr != 0);
        if (!fr) {
            free(block);
            return;
        }
        CHECK(fr_random_u32(fr) == a);
    }

    /* The configuration's strings were copied, not borrowed: the caller's
     * buffers can go, and the friend still has its name and still sends
     * its key. */
    memset(key, 'x', sizeof(key));
    memset(name, 'x', sizeof(name));
    CHECK_STR(friend_name(fr), "Em");
    friend_tick(fr, TEST_NOW);
    CHECK(friend_say(fr, str_lit("hello"), 0, 0) == FRIEND_OK);
    CHECK(friend_pending_io(fr, &request, 1) == 1);
    CHECK(str_index_of(request.headers, str_lit("x-goog-api-key: test-key"))
          >= 0);
    friend_io_failed(fr, request.id, str_lit("not in this test"));

    /* A contradictory tuning is made consistent, not obeyed. */
    {
        Friend_Config bad = test_config(FRIEND_PROVIDER_GEMINI, small_limits());
        bad.consolidate_at      = 10;
        bad.consolidate_leaving = 50;
        bad.recall_keep         = 99;
        bad.recall_candidates   = 8;
        CHECK(friend_reconfigure(fr, &bad) == FRIEND_OK);
        CHECK(fr->tuning.consolidate_leaving < fr->tuning.consolidate_at);
        CHECK(fr->tuning.recall_keep <= fr->tuning.recall_candidates);
    }

    /* A prompt override bigger than the config arena is reported, not
     * silently truncated. */
    {
        Friend_Config big = test_config(FRIEND_PROVIDER_GEMINI, small_limits());
        static char   huge[FR_KB(512)];
        memset(huge, 'x', sizeof(huge));
        big.prompt_override = str_make(huge, (i32)sizeof(huge));
        CHECK(friend_init(block, need, &big, &result) == 0);
        CHECK(result == FRIEND_ERR_FULL);
    }

    free(block);
}

static void test_slots(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    u32             first, reused;
    u32             generation;
    i32             i;

    if (!fr) {
        free(block);
        return;
    }

    first = fr_memory_alloc(fr);
    CHECK(first != FR_SLOT_NONE);
    CHECK(friend_memory_count(fr) == 1);
    CHECK(fr_memory(fr, first) != 0);
    CHECK(fr_memory(fr, first)->first_out == FR_SLOT_NONE);
    CHECK(fr_memory_alloc(fr) != first);

    generation = fr_memory(fr, first)->generation;
    fr_memory_free(fr, first);
    CHECK(friend_memory_count(fr) == 1);
    CHECK(fr_memory(fr, first) == 0); /* a freed slot reads as absent */

    /* The slot is reused, and its generation moved, so an id held across
     * the free is detectably stale instead of silently naming someone
     * else's memory. */
    reused = fr_memory_alloc(fr);
    CHECK(reused == first);
    CHECK(fr_memory(fr, reused)->generation != generation);

    /* Exhaustion is reported, not fatal. */
    for (i = fr->memory_count; i < fr->memory_capacity; ++i) {
        CHECK(fr_memory_alloc(fr) != FR_SLOT_NONE);
    }
    CHECK(friend_memory_count(fr) == fr->memory_capacity);
    CHECK(fr_memory_alloc(fr) == FR_SLOT_NONE);

    /* Edges behave the same way. */
    {
        u32 edge = fr_edge_alloc(fr);
        CHECK(edge != FR_SLOT_NONE);
        CHECK(friend_memory_edge_count(fr) == 1);
        fr_edge_free(fr, edge);
        CHECK(friend_memory_edge_count(fr) == 0);
        fr_edge_free(fr, edge); /* freeing twice must not corrupt the list */
        CHECK(friend_memory_edge_count(fr) == 0);
        for (i = 0; i < fr->edge_capacity; ++i) {
            CHECK(fr_edge_alloc(fr) != FR_SLOT_NONE);
        }
        CHECK(fr_edge_alloc(fr) == FR_SLOT_NONE);
    }

    free(block);
}

/* A frontend that stops reading events loses the newest, keeps the order
 * of what it had not read yet, and is told after it has read them. */
static void test_event_queue(void)
{
    Friend_Config   config = test_config(FRIEND_PROVIDER_GEMINI,
                                         small_limits());
    void           *block  = 0;
    Friend_Context *fr;
    Fake_Provider   fake;
    Friend_Event    event;
    u64             now = TEST_NOW;
    i32             i, committed = 0, last_index = -1, after_error = 0;
    b32             overflowed = 0, in_order = 1;

    /* A window with room for every message, so each one has a place of
     * its own to be committed at. */
    config.limits.max_active_events = 64;
    fr = make_friend_with(&block, &config);
    if (!fr) {
        free(block);
        return;
    }
    memset(&fake, 0, sizeof(fake));
    fake.reply       = "hey";
    fake.extract     = "no events";
    fake.interiority = "I want to be read.";

    friend_tick(fr, now);
    CHECK(friend_next_event(fr, &event) == 0);

    for (i = 0; i < 24; ++i) {
        CHECK(friend_say(fr, str_lit("are you there"), 0, 0) == FRIEND_OK);
        fake_serve(fr, &fake, &now);
    }
    CHECK(fake.replies == 24);

    while (friend_next_event(fr, &event)) {
        if (overflowed) ++after_error;
        if (event.kind == FRIEND_EVENT_ERROR &&
            str_index_of(event.text, str_lit("overflowed")) >= 0) {
            overflowed = 1;
            continue;
        }
        if (event.kind != FRIEND_EVENT_MESSAGE_COMMITTED) continue;
        if (committed == 0 && event.index != 0) in_order = 0;
        if (event.index <= last_index) in_order = 0;
        last_index = event.index;
        ++committed;
    }
    CHECK(overflowed);
    CHECK(after_error == 0); /* the complaint is the last thing */
    CHECK(in_order);         /* from the first message on, none skipped back */
    CHECK(committed > 0 && committed < 24); /* the newest were dropped */
    CHECK(friend_next_event(fr, &event) == 0);

    free(block);
}

/* ------------------------------------------------------------------ *
 *  The download zip
 *
 *  Driven at several chunk sizes, including one byte in and one byte out,
 *  because the whole point of the reader is that it can stop anywhere.
 * ------------------------------------------------------------------ */

static u8 uz_memory[FR_KB(64)];
static u8 uz_output[FR_KB(96)];

static Friend_Result uz_run(const unsigned char *zip, size_t zip_size,
                            size_t in_chunk, size_t out_chunk,
                            size_t *out_len, Friend_String *out_name,
                            Friend_String *out_error)
{
    Friend_Unzip      *z = 0;
    Friend_Unzip_Step  step;
    size_t             at = 0, written = 0;
    Friend_Result      result;

    *out_len = 0;
    if (out_name) *out_name = str_nil;
    if (out_error) *out_error = str_nil;

    result = friend_unzip_begin(uz_memory, sizeof(uz_memory), &z);
    if (result != FRIEND_OK) return result;

    memset(&step, 0, sizeof(step));
    for (;;) {
        size_t take = zip_size - at;
        size_t room = sizeof(uz_output) - written;
        if (take > in_chunk) take = in_chunk;
        if (room > out_chunk) room = out_chunk;

        step.in      = zip + at;
        step.in_size = take;
        step.in_last = (at + take >= zip_size) ? 1 : 0;
        step.out     = uz_output + written;
        step.out_size = room;

        result = friend_unzip_push(z, &step);
        if (out_name) *out_name = friend_unzip_entry_name(z);
        if (result != FRIEND_OK) {
            if (out_error) *out_error = friend_unzip_error(z);
            return result;
        }
        at += step.in_used;
        written += step.out_written;
        if (step.done) break;
        if (step.in_used == 0 && step.out_written == 0) {
            /* Nothing moved and nothing is finished: the reader is stuck,
             * which is a bug in it and not a property of the input. */
            if (out_error) *out_error = str_lit("stalled");
            return FRIEND_ERR_BAD_STATE;
        }
    }
    *out_len = written;
    return FRIEND_OK;
}

static String uz_output_string(size_t len) { return str_make((const char *)uz_output, (i32)len); }

static void test_unzip(void)
{
    size_t        len = 0;
    Friend_String name = str_nil, error = str_nil;

    CHECK(friend_unzip_size() <= sizeof(uz_memory));

    /* The shape every official export has: deflated, with the CRC in a
     * data descriptor after the data. */
    CHECK(uz_run(uz_streamed_deflate, sizeof(uz_streamed_deflate),
                 sizeof(uz_streamed_deflate), sizeof(uz_output), &len, &name,
                 &error) == FRIEND_OK);
    CHECK_STR(name, "friend_state.json");
    CHECK_STR(uz_output_string(len), UZ_SMALL_JSON);

    /* Same bytes out when it is made to suspend on every single byte, in
     * and out. This is the test that matters. */
    CHECK(uz_run(uz_streamed_deflate, sizeof(uz_streamed_deflate), 1, 1, &len,
                 0, &error) == FRIEND_OK);
    CHECK_STR(uz_output_string(len), UZ_SMALL_JSON);

    /* A writer with a seekable output puts the CRC in the local header
     * instead, which is what a re-zipped file looks like. */
    CHECK(uz_run(uz_seekable_deflate, sizeof(uz_seekable_deflate),
                 sizeof(uz_seekable_deflate), sizeof(uz_output), &len, 0,
                 &error) == FRIEND_OK);
    CHECK_STR(uz_output_string(len), UZ_SMALL_JSON);

    CHECK(uz_run(uz_seekable_store, sizeof(uz_seekable_store), 3, 5, &len, 0,
                 &error) == FRIEND_OK);
    CHECK_STR(uz_output_string(len), UZ_SMALL_JSON);
}

static void test_unzip_window(void)
{
    size_t len = 0, i;
    b32    identical = 1;

    /* 70 KB: more than twice the 32 KiB window, so matches reach back
     * across the point where it wraps. */
    CHECK(uz_run(uz_streamed_pattern, sizeof(uz_streamed_pattern),
                 sizeof(uz_streamed_pattern), sizeof(uz_output), &len, 0, 0) ==
          FRIEND_OK);
    CHECK(len == 70000);
    for (i = 0; i < len; ++i) {
        u8 want = (u8)('A' + ((i / 101 + i % 7) % 26));
        if (uz_output[i] != want) identical = 0;
    }
    CHECK(identical);

    /* And again with awkward chunk sizes that share no factor with
     * anything in the format. */
    len = 0;
    CHECK(uz_run(uz_streamed_pattern, sizeof(uz_streamed_pattern), 7, 13, &len,
                 0, 0) == FRIEND_OK);
    CHECK(len == 70000);
    identical = 1;
    for (i = 0; i < len; ++i) {
        u8 want = (u8)('A' + ((i / 101 + i % 7) % 26));
        if (uz_output[i] != want) identical = 0;
    }
    CHECK(identical);
}

static void test_unzip_bad_input(void)
{
    static unsigned char damaged[sizeof(uz_streamed_deflate)];
    static unsigned char damaged_big[sizeof(uz_streamed_pattern)];
    size_t        len = 0;
    Friend_String error = str_nil;

    /* Not a zip at all — the JSON itself, which someone will certainly
     * try. The message has to say so rather than "bad data". */
    CHECK(uz_run((const unsigned char *)UZ_SMALL_JSON, sizeof(UZ_SMALL_JSON) - 1,
                 64, 64, &len, 0, &error) == FRIEND_ERR_BAD_DATA);
    CHECK(str_index_of(error, str_lit("not a zip")) >= 0);

    /* Cut off after the header. */
    CHECK(uz_run(uz_streamed_deflate, 40, 64, sizeof(uz_output), &len, 0,
                 &error) == FRIEND_ERR_BAD_DATA);
    CHECK(str_index_of(error, str_lit("ends mid-stream")) >= 0);

    /* One flipped bit in a literal byte. The stream still decodes
     * perfectly — only the checksum knows, which is the whole reason to
     * check it. Go stores a payload this small uncompressed, so the text
     * is right there in the zip to find. */
    memcpy(damaged, uz_streamed_deflate, sizeof(damaged));
    {
        i32 at = str_index_of(str_make((const char *)damaged, (i32)sizeof(damaged)),
                              str_lit("friendName"));
        CHECK(at > 30); /* past the entry name, i.e. in the data */
        damaged[at + 2] = (unsigned char)(damaged[at + 2] ^ 0x20u);
    }
    CHECK(uz_run(damaged, sizeof(damaged), sizeof(damaged), sizeof(uz_output),
                 &len, 0, &error) == FRIEND_ERR_BAD_DATA);
    CHECK(str_index_of(error, str_lit("damaged")) >= 0);

    /* And a flipped bit in the middle of a real compressed stream, where
     * the codes themselves stop making sense. */
    memcpy(damaged_big, uz_streamed_pattern, sizeof(damaged_big));
    damaged_big[sizeof(damaged_big) / 2] ^= 0x55u;
    CHECK(uz_run(damaged_big, sizeof(damaged_big), 512, sizeof(uz_output), &len,
                 0, &error) != FRIEND_OK);
    CHECK(error.len > 0);

    /* A stored entry written by a streaming writer has no length anywhere
     * we can reach, so it is refused with a reason. */
    CHECK(uz_run(uz_streamed_store, sizeof(uz_streamed_store),
                 sizeof(uz_streamed_store), sizeof(uz_output), &len, 0,
                 &error) == FRIEND_ERR_BAD_DATA);
    CHECK(str_index_of(error, str_lit("stored without a length")) >= 0);
}

/* ------------------------------------------------------------------ *
 *  The packed dialogue
 * ------------------------------------------------------------------ */

static void test_dialog_stream(void)
{
    static Fr_Memory_Record record;
    i32                     appended = 0, walked = 0, i;
    String                  cursor;
    u64                     at   = 0;
    Fr_Turn_Kind            kind = FR_TURN_CAPTION;
    String                  text = str_nil;

    memset(&record, 0, sizeof(record));

    /* Enough turns to overflow the record several times over. */
    for (i = 0; i < 400; ++i) {
        char line[64];
        int  n = snprintf(line, sizeof(line), "turn number %d", i);
        if (fr_dialog_append(&record, 1000u + (u64)i,
                             (i & 1) ? FR_TURN_FRIEND : FR_TURN_USER,
                             str_make(line, n))) {
            ++appended;
        }
    }
    CHECK(appended == 400);
    CHECK(record.dialog_len <= FR_DIALOG_MAX);

    /* Whatever survived is the newest end of it, and it is all still
     * walkable: an overflow must not leave half a turn in the stream. */
    cursor = fr_memory_dialog(&record);
    while (fr_dialog_next(&cursor, &at, &kind, &text)) ++walked;
    CHECK(walked > 0);
    CHECK(walked < 400);
    CHECK(at == 1000u + 399u);
    CHECK_STR(text, "turn number 399");

    /* A single turn too big for the record is cut, not refused. */
    {
        static char       huge[FR_DIALOG_MAX * 2];
        Fr_Memory_Record *one = &record;
        memset(huge, 'x', sizeof(huge));
        memset(one, 0, sizeof(*one));
        CHECK(fr_dialog_append(one, 7, FR_TURN_USER,
                               str_make(huge, (i32)sizeof(huge))));
        cursor = fr_memory_dialog(one);
        CHECK(fr_dialog_next(&cursor, &at, &kind, &text));
        CHECK(text.len == FR_DIALOG_MAX - FR_TURN_HEADER_BYTES);
        CHECK(!fr_dialog_next(&cursor, &at, &kind, &text));
    }
}

static void test_truncate_utf8(void)
{
    /* "naïve" — the ï is two bytes, and cutting between them would put a
     * broken character in front of a model. */
    String s = str_lit("na\303\257ve");
    CHECK(s.len == 6);
    CHECK(fr_truncate_utf8(s, 6).len == 6);
    CHECK(fr_truncate_utf8(s, 4).len == 4);
    CHECK(fr_truncate_utf8(s, 3).len == 2); /* backs off the lead byte */
    CHECK(fr_truncate_utf8(s, 2).len == 2);
    CHECK(fr_truncate_utf8(s, 0).len == 0);

    /* Three-byte characters back off by two where they have to. */
    {
        String em = str_lit("a\342\200\224b");
        CHECK(em.len == 5);
        CHECK(fr_truncate_utf8(em, 2).len == 1);
        CHECK(fr_truncate_utf8(em, 3).len == 1);
        CHECK(fr_truncate_utf8(em, 4).len == 4);
    }
}

/* ------------------------------------------------------------------ *
 *  Importing the official export
 *
 *  Against a real protojson FriendState — see fr_export_data.h — so the
 *  field names, the quoted 64-bit integers, the base64 and the RFC 3339
 *  timestamps are the ones the service actually emitted.
 * ------------------------------------------------------------------ */

static Friend_Context *import_fixture(void **out_block,
                                      Friend_Import_Report *out_report,
                                      Friend_String embed_model)
{
    Friend_Config   config = test_config(FRIEND_PROVIDER_GEMINI,
                                         export_limits());
    Friend_Context *fr;

    if (embed_model.len > 0) config.models.embed = embed_model;
    fr = make_friend_with(out_block, &config);
    if (!fr) return 0;
    CHECK(friend_import_export(fr, str_make(fr_export_json,
                                            (i32)sizeof(fr_export_json) - 1),
                               out_report) == FRIEND_OK);
    return fr;
}

/* The fixture's embedding for memory k: byte j = (j * 31 + k * 17) % 256,
 * read back as the signed byte the record holds. */
static b32 embedding_matches(const Fr_Memory_Record *m, i32 k)
{
    i32 j;
    for (j = 0; j < FR_EMBED_DIMS; ++j) {
        if (m->embedding[j] != (i8)(u8)((j * 31 + k * 17) % 256)) return 0;
    }
    return 1;
}

/* What a saved friend says about itself, read the way a frontend listing
 * its friends reads it. */
static Friend_Save_Info saved_info(Friend_Context *fr)
{
    Friend_Save_Info info;
    size_t           size  = friend_save_size(fr);
    u8              *saved = (u8 *)malloc(size);

    memset(&info, 0, sizeof(info));
    CHECK(saved && friend_save(fr, saved, size, 0) == FRIEND_OK);
    if (saved) CHECK(friend_save_peek(saved, size, &info) == FRIEND_OK);
    info.name = str_nil; /* it pointed into the buffer */
    free(saved);
    return info;
}

/* The requests a friend makes on its own, with nothing said to it. */
static i32 idle_requests(Friend_Context *fr, Friend_Io_Request *out, i32 max)
{
    friend_tick(fr, TEST_NOW);
    return friend_pending_io(fr, out, max);
}

static void test_import_scalars(void)
{
    void                *block = 0;
    Friend_Import_Report report;
    Friend_Context      *fr = import_fixture(&block, &report, str_nil);
    Friend_Stats         stats;

    if (!fr) {
        free(block);
        return;
    }
    stats = friend_stats(fr);
    CHECK_STR(friend_name(fr), "Em");
    CHECK_STR(friend_interiority(fr),
              "I want to be the first thing he tells.\nI crave being needed "
              "\342\200\224 badly.");
    CHECK(friend_created_unix_ms(fr) == 1741075200000u);
    CHECK(saved_info(fr).total_event_count == 9);
    CHECK(stats.replies_total == 412);
    CHECK(stats.epoch_samples == 57);
    CHECK(fr->epoch_active);

    CHECK(report.memories == 3);
    CHECK(report.connections == 2); /* the third names a memory not present */
    CHECK(report.connections_dropped == 1);
    CHECK(report.memories_dropped == 0);
    CHECK(report.messages == 6);
    CHECK(report.messages_dropped == 0);
    CHECK(report.images == 1);

    free(block);
}

static void test_import_memories(void)
{
    void                *block = 0;
    Friend_Import_Report report;
    Friend_Context      *fr = import_fixture(&block, &report, str_nil);
    Friend_Memory_Node   node[3];
    Fr_Memory_Record    *a, *b, *c;
    Friend_Io_Request    requests[4];
    i32                  i;

    if (!fr) {
        free(block);
        return;
    }
    CHECK(friend_memory_count(fr) == 3);
    for (i = 0; i < 3; ++i) CHECK(friend_memory_at(fr, i, &node[i]) == FRIEND_OK);
    a = fr_memory(fr, node[0].id.slot);
    b = fr_memory(fr, node[1].id.slot);
    c = fr_memory(fr, node[2].id.slot);
    CHECK(a && b && c);
    if (!a || !b || !c) {
        free(block);
        return;
    }

    CHECK_STR(node[0].description,
              "He started running in the mornings before work.");
    CHECK(node[0].created_unix_ms == 1790836200000u);
    CHECK(node[0].use_count == 12);
    CHECK(node[1].use_count == 4);
    CHECK(a->uses_this_epoch == 3);
    CHECK(a->in_this_epoch == 1);
    CHECK(b->in_this_epoch == 0); /* the field was absent, so false */

    /* Byte for byte: an imported friend recalls what it used to recall
     * only if the vectors survive the trip exactly. */
    CHECK(embedding_matches(a, 1));
    CHECK(embedding_matches(b, 2));
    CHECK(embedding_matches(c, 3));
    /* ...and they are used as they are, not made again. */
    CHECK(idle_requests(fr, requests, 4) == 0);

    /* The dialogue is stamped against the clock, so the time address and
     * the "ago" are both rebuilt rather than stored. */
    {
        String       cursor = fr_memory_dialog(a);
        u64          at     = 0;
        Fr_Turn_Kind kind   = FR_TURN_CAPTION;
        String       text   = str_nil;

        CHECK(fr_dialog_next(&cursor, &at, &kind, &text));
        CHECK(at == 1790835900000u);
        CHECK(kind == FR_TURN_USER);
        CHECK_STR(text, "went for a run before work, first time in years");
        CHECK(fr_dialog_next(&cursor, &at, &kind, &text));
        CHECK(kind == FR_TURN_FRIEND);
        CHECK_STR(text, "before work? who are you");
        CHECK(!fr_dialog_next(&cursor, &at, &kind, &text));
    }

    CHECK_STR(fr_render_dialog(&scratch, a,
                               1790835900000u + 3 * 3600000u + 20 * 60000u, 0),
              "(3 hours and 20 minutes ago) October, Thursday, "
              "10/1/2026 6:25:00 AM UTC User: went for a run before work, "
              "first time in years\n"
              "(3 hours and 19 minutes ago) October, Thursday, "
              "10/1/2026 6:26:00 AM UTC Assistant: before work? who are "
              "you\n");

    /* An overheard transcription keeps the framing it was shown under.
     * The speaker judgment is gone, but "could be the user could not be
     * the user" was the service's own hedge and the prompts lean on it. */
    CHECK_STR(fr_render_dialog(&scratch, c, 1792522800000u + 60000u, 0),
              "(1 minute ago) October, Tuesday, 10/20/2026 7:00:00 PM UTC "
              "you overheard the following transcribed audio:\n"
              "This transcribed overheard audio sounds somewhat like the "
              "user, could be the user could not be the user: \"the eight "
              "forty is delayed again\"\n\n"
              "This transcribed overheard audio sounds not at all like the "
              "user. This is probably not the user: \"every single time\"\n"
              "\n\n");

    free(block);
}

static void test_import_graph(void)
{
    void                *block = 0;
    Friend_Import_Report report;
    Friend_Context      *fr = import_fixture(&block, &report, str_nil);
    Friend_Memory_Node   a, b, c;
    Friend_Relation      relation = FRIEND_RELATION_NONE;

    if (!fr) {
        free(block);
        return;
    }
    CHECK(friend_memory_at(fr, 0, &a) == FRIEND_OK);
    CHECK(friend_memory_at(fr, 1, &b) == FRIEND_OK);
    CHECK(friend_memory_at(fr, 2, &c) == FRIEND_OK);

    /* m-aaa changed into m-bbb, which caused m-ccc; the WANT edge out of
     * m-aaa pointed at a memory the export did not contain, and is not
     * there at all rather than dangling. */
    CHECK(friend_memory_edge_count(fr) == 2);
    CHECK(edges_between(fr, a.id.slot, b.id.slot, &relation) == 1);
    CHECK(relation == FRIEND_RELATION_CHANGED);
    CHECK(edges_between(fr, b.id.slot, c.id.slot, &relation) == 1);
    CHECK(relation == FRIEND_RELATION_CAUSE);
    CHECK(edges_between(fr, b.id.slot, a.id.slot, 0) == 0);

    /* An edge names its ends by id, the same ids the memories carry. */
    {
        Friend_Memory_Edge edge;
        i32                i;
        for (i = 0; i < friend_memory_edge_count(fr); ++i) {
            CHECK(friend_memory_edge_at(fr, i, &edge) == FRIEND_OK);
            CHECK(edge.from.generation != 0 && edge.to.generation != 0);
            if (edge.from.slot == a.id.slot) {
                CHECK(edge.from.generation == a.id.generation);
            }
        }
        CHECK(friend_memory_edge_at(fr, 2, &edge) == FRIEND_ERR_BAD_ARGUMENT);
    }

    free(block);
}

static void test_import_photos(void)
{
    void                *block = 0;
    Friend_Import_Report report;
    Friend_Context      *fr = import_fixture(&block, &report, str_nil);
    Friend_Memory_Node   node;
    Fr_Memory_Record    *b;

    if (!fr) {
        free(block);
        return;
    }
    CHECK(friend_memory_at(fr, 1, &node) == FRIEND_OK);
    b = fr_memory(fr, node.id.slot);
    CHECK(b != 0);
    if (!b) {
        free(block);
        return;
    }

    /* This friend was given no blob_path, so the photograph is described
     * and its pixels are dropped — which is what a tool that only reads
     * an export wants. The pixel path has a test of its own. */
    CHECK(b->image_count == 1);
    CHECK(b->images[0].blob_size == 4096);
    CHECK(b->images[0].thumb_size == 900);
    CHECK(report.images_without_pixels == report.images);
    CHECK(friend_stats(fr).photos == 0);

    /* The caption rides in the dialogue, in image order, and stays out of
     * the dialogue the model is shown as an exchange. */
    CHECK_STR(fr_memory_captions(&scratch, b),
              "A steel bridge at sunrise, mist over the water.");
    CHECK(str_index_of(fr_render_dialog(&scratch, b, 1791959700000u, 0),
                       str_lit("steel bridge")) < 0);

    free(block);
}

static void test_import_conversation(void)
{
    void                *block = 0;
    Friend_Import_Report report;
    Friend_Context      *fr = import_fixture(&block, &report, str_nil);
    Friend_Message       m;
    Fr_Chat_Event       *user, *ambient;

    if (!fr) {
        free(block);
        return;
    }

    /* Four active events fit the window of eight, and the two history
     * messages are behind them in the scrollback; a frontend sees six
     * messages in order either way. */
    CHECK(friend_message_count(fr) == 6);
    CHECK(friend_stats(fr).active_events == 4);
    CHECK(friend_stats(fr).scrollback == 2);

    CHECK(friend_message(fr, 0, &m) == FRIEND_OK);
    CHECK(m.kind == FRIEND_MESSAGE_USER);
    CHECK_STR(m.text, "hey");
    /* An AI reply is kept in its tagged form, which is what the model
     * itself produced and what it is shown again. */
    CHECK(friend_message(fr, 1, &m) == FRIEND_OK);
    CHECK(m.kind == FRIEND_MESSAGE_FRIEND);
    CHECK_STR(m.text, "[warm] hey. what should I call you");

    CHECK(friend_message(fr, 2, &m) == FRIEND_OK);
    CHECK(m.kind == FRIEND_MESSAGE_NOTICE);
    CHECK_STR(m.text, "context cleared");

    CHECK(friend_message(fr, 3, &m) == FRIEND_OK);
    CHECK(m.kind == FRIEND_MESSAGE_USER);
    CHECK(m.unix_ms == 1793178060000u);
    CHECK(m.was_spoken);

    CHECK(friend_message(fr, 5, &m) == FRIEND_OK);
    CHECK(m.kind == FRIEND_MESSAGE_AMBIENT);
    CHECK_STR(m.text, "Someone nearby mentioned a deadline moving up.");

    /* What a frontend cannot see but the friend acts on: the service's
     * own numbering, and whether an overheard line is a summary or a
     * transcription. */
    user    = &fr->active_events[1];
    ambient = &fr->active_events[3];
    CHECK(user->index == 7);
    CHECK(ambient->ambient_raw == 0);

    free(block);
}

/* Vectors made by another embedding model are not comparable with
 * anything this friend will produce from here on, so they are made again
 * from the descriptions the export carries — on the friend's own, as I/O
 * it asks for while nothing else is happening. */
static void test_import_other_embed_model(void)
{
    void                *block = 0;
    Friend_Import_Report report;
    Friend_Context      *fr =
        import_fixture(&block, &report, str_lit("text-embedding-3-small"));
    Friend_Io_Request requests[8];
    i32               n, i, described = 0;

    if (!fr) {
        free(block);
        return;
    }
    CHECK(report.memories == 3);
    n = idle_requests(fr, requests, 8);
    CHECK(n == 3);
    for (i = 0; i < n; ++i) {
        CHECK(str_index_of(requests[i].url,
                           str_lit("models/text-embedding-3-small:"
                                   "embedContent")) >= 0);
        if (str_index_of(requests[i].body,
                         str_lit("He started running in the mornings")) >= 0) {
            ++described;
        }
        friend_io_failed(fr, requests[i].id, str_lit("not in this test"));
    }
    CHECK(described == 1);
    free(block);
}

/* Built rather than written out, so the shape is obvious and the sizes
 * are a parameter. Snake_case on purpose: protojson accepts the proto's
 * own field names, and so must we. */
static String make_export(Arena *a, i32 memories, i32 messages)
{
    String_Builder sb = sb_begin(a);
    i32            i;

    sb_append_cstr(&sb, "{\"friend_name\": \"Tess\",");
    sb_append_cstr(&sb, "\"memory_state\": {\"elements\": [");
    for (i = 0; i < memories; ++i) {
        if (i > 0) sb_append_char(&sb, ',');
        sb_appendf(&sb, "{\"id\": \"m%d\", \"description\": \"memory %d\"}",
                   i, i);
    }
    sb_append_cstr(&sb, "], \"connections\": [");
    for (i = 0; i + 1 < memories; ++i) {
        if (i > 0) sb_append_char(&sb, ',');
        sb_appendf(&sb,
                   "{\"from_id\": \"m%d\", \"to_id\": \"m%d\","
                   " \"connection_type\": \"CONNECTION_TYPE_SAME_TOPIC\"}",
                   i, i + 1);
    }
    sb_append_cstr(&sb, "]}, \"active_chat_events\": [");
    for (i = 0; i < messages; ++i) {
        if (i > 0) sb_append_char(&sb, ',');
        sb_appendf(&sb,
                   "{\"index\": \"%d\", \"chat_event\": {"
                   "\"is_user_message\": true, \"text_data\": \"message %d\","
                   " \"date_occured\": \"2026-01-01T00:00:00Z\"}}",
                   i, i);
    }
    sb_append_cstr(&sb, "]}");
    return sb_done(&sb);
}

static void test_import_limits(void)
{
    Friend_Config        config = test_config(FRIEND_PROVIDER_GEMINI,
                                              small_limits());
    Friend_Import_Report report;
    Friend_Context      *fr;
    Friend_Message       m;
    void                *block = 0;
    String               export_json;

    config.limits.max_memories    = 4;
    config.limits.max_connections = 8;
    config.limits.max_scrollback  = 8;
    fr = make_friend_with(&block, &config);
    if (!fr) {
        free(block);
        return;
    }

    /* Six memories into room for four, twenty messages into a window of
     * eight with eight more of scrollback behind it. */
    export_json = make_export(&scratch, 6, 20);
    CHECK(friend_import_export(fr, export_json, &report) == FRIEND_OK);

    CHECK_STR(friend_name(fr), "Tess");
    CHECK(report.memories == 4);
    CHECK(report.memories_dropped == 2);
    /* Of five connections, the two that reach a memory there was no room
     * for go with it. Nothing dangles. */
    CHECK(report.connections == 3);
    CHECK(report.connections_dropped == 2);
    CHECK(friend_memory_edge_count(fr) == 3);

    /* The window holds the newest, and the scrollback the twelve before
     * it minus the four that fell off its far end. */
    CHECK(report.messages == 16);
    CHECK(report.messages_dropped == 4);
    CHECK(friend_message_count(fr) == 16);
    CHECK(friend_stats(fr).active_events == 8);
    CHECK(friend_stats(fr).messages_lost == 4);
    CHECK(friend_message(fr, 0, &m) == FRIEND_OK);
    CHECK_STR(m.text, "message 4");
    CHECK(friend_message(fr, 8, &m) == FRIEND_OK);
    CHECK_STR(m.text, "message 12");
    CHECK(friend_message(fr, 15, &m) == FRIEND_OK);
    CHECK_STR(m.text, "message 19");

    /* Importing again replaces rather than appends. */
    CHECK(friend_import_export(fr, export_json, &report) == FRIEND_OK);
    CHECK(friend_memory_count(fr) == 4);
    CHECK(friend_memory_edge_count(fr) == 3);
    CHECK(friend_message_count(fr) == 16);

    free(block);
}

static void test_import_bad_input(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);

    if (!fr) {
        free(block);
        return;
    }

    CHECK(friend_import_export(fr, str_nil, 0) == FRIEND_ERR_BAD_ARGUMENT);
    CHECK(friend_import_export(fr, str_lit("{\"friendName\": "), 0) ==
          FRIEND_ERR_BAD_DATA);
    CHECK(friend_last_error(fr).len > 0);
    /* A truncated file leaves nothing behind: half a friend would be
     * worse than none, because the user would believe it. */
    CHECK(friend_memory_count(fr) == 0);

    /* An empty but well-formed state is a new friend, not an error. */
    CHECK(friend_import_export(fr, str_lit("{}"), 0) == FRIEND_OK);
    CHECK(friend_memory_count(fr) == 0);

    free(block);
}

/* ------------------------------------------------------------------ *
 *  Saving and loading
 * ------------------------------------------------------------------ */

static Friend_Context *fresh_context(void **out_block)
{
    Friend_Config config = test_config(FRIEND_PROVIDER_GEMINI, export_limits());
    return make_friend_with(out_block, &config);
}

static void test_save_round_trip(void)
{
    void                *block_a = 0, *block_b = 0;
    Friend_Import_Report report;
    Friend_Context      *a = import_fixture(&block_a, &report, str_nil);
    Friend_Context      *b = fresh_context(&block_b);
    Friend_Stats         sa, sb;
    u8                  *saved;
    size_t               size, written = 0;
    i32                  i;

    if (!a || !b) {
        free(block_a);
        free(block_b);
        return;
    }

    size  = friend_save_size(a);
    CHECK(size > 0);
    saved = (u8 *)malloc(size + 16);
    memset(saved, 0xAB, size + 16);

    /* A buffer one byte short has to be refused rather than written past,
     * and friend_save_size has to be exactly right or this is wrong in
     * one direction or the other. */
    CHECK(friend_save(a, saved, size - 1, &written) == FRIEND_ERR_TOO_SMALL);
    CHECK(friend_save(a, saved, size, &written) == FRIEND_OK);
    CHECK(written == size);
    CHECK(saved[size] == 0xAB); /* nothing written past the end */

    CHECK(friend_load(b, saved, written) == FRIEND_OK);

    sa = friend_stats(a);
    sb = friend_stats(b);
    CHECK_STR(friend_name(b), "Em");
    CHECK(str_eq(friend_interiority(b), friend_interiority(a)));
    CHECK(friend_created_unix_ms(b) == friend_created_unix_ms(a));
    CHECK(saved_info(b).total_event_count == saved_info(a).total_event_count);
    CHECK(sb.replies_total == sa.replies_total);
    CHECK(sb.epoch_samples == sa.epoch_samples);
    CHECK(sb.active_events == sa.active_events);
    CHECK(sb.scrollback == sa.scrollback);

    /* Memory for memory, id for id: an id a frontend wrote down has to
     * still mean this memory. */
    CHECK(friend_memory_count(b) == friend_memory_count(a));
    for (i = 0; i < friend_memory_count(a); ++i) {
        Friend_Memory_Node ma, mb;
        CHECK(friend_memory_at(a, i, &ma) == FRIEND_OK);
        CHECK(friend_memory_at(b, i, &mb) == FRIEND_OK);
        CHECK(ma.id.slot == mb.id.slot && ma.id.generation == mb.id.generation);
        CHECK(str_eq(ma.description, mb.description));
        CHECK(ma.created_unix_ms == mb.created_unix_ms);
        CHECK(ma.use_count == mb.use_count);
    }

    /* The graph comes back the same shape. */
    CHECK(friend_memory_edge_count(b) == friend_memory_edge_count(a));
    for (i = 0; i < friend_memory_edge_count(a); ++i) {
        Friend_Memory_Edge edge;
        Friend_Relation    relation = FRIEND_RELATION_NONE;
        CHECK(friend_memory_edge_at(a, i, &edge) == FRIEND_OK);
        CHECK(edges_between(b, edge.from.slot, edge.to.slot, &relation) == 1);
        CHECK(relation == edge.relation);
    }

    /* And the conversation, as a frontend reads it. */
    CHECK(friend_message_count(a) == friend_message_count(b));
    for (i = 0; i < friend_message_count(a); ++i) {
        Friend_Message ma, mb;
        CHECK(friend_message(a, i, &ma) == FRIEND_OK);
        CHECK(friend_message(b, i, &mb) == FRIEND_OK);
        CHECK(ma.kind == mb.kind);
        CHECK(ma.unix_ms == mb.unix_ms);
        CHECK(ma.attachment_count == mb.attachment_count);
        CHECK(ma.was_spoken == mb.was_spoken);
        CHECK(str_eq(ma.text, mb.text));
    }

    /* Saving what was loaded gives the same bytes back, which is what
     * covers everything a frontend cannot see: the vectors, the
     * dialogues, the epoch, the photographs' descriptors. */
    {
        u8    *again = (u8 *)malloc(size);
        size_t again_written = 0;
        CHECK(friend_save_size(b) == size);
        CHECK(friend_save(b, again, size, &again_written) == FRIEND_OK);
        CHECK(again_written == written);
        CHECK(memcmp(again, saved, size) == 0);
        free(again);
    }

    free(saved);
    free(block_a);
    free(block_b);
}

static void test_save_bad_input(void)
{
    void           *block = 0;
    Friend_Context *fr    = fresh_context(&block);
    u8              junk[64];
    i32             i;

    if (!fr) {
        free(block);
        return;
    }

    memset(junk, 0, sizeof(junk));
    CHECK(friend_load(fr, junk, sizeof(junk)) == FRIEND_ERR_BAD_DATA);
    CHECK(str_index_of(friend_last_error(fr), str_lit("not a saved friend")) >=
          0);
    CHECK(friend_load(fr, 0, 0) == FRIEND_ERR_BAD_ARGUMENT);

    /* An empty friend saves and loads: the file is a header and some
     * zeroes, and nothing about that should be a special case. */
    {
        size_t size = friend_save_size(fr);
        u8    *saved = (u8 *)malloc(size);
        CHECK(friend_save(fr, saved, size, 0) == FRIEND_OK);
        CHECK(friend_load(fr, saved, size) == FRIEND_OK);
        CHECK(friend_memory_count(fr) == 0);
        CHECK(friend_message_count(fr) == 0);

        /* Truncate it anywhere and it has to be refused. */
        for (i = 1; i < (i32)size; i += 7) {
            void           *other_block = 0;
            Friend_Context *other       = fresh_context(&other_block);
            if (other) {
                CHECK(friend_load(other, saved, (size_t)i) != FRIEND_OK);
                CHECK(friend_memory_count(other) == 0);
            }
            free(other_block);
        }
        free(saved);
    }

    free(block);
}

static void test_save_truncated_import(void)
{
    void                *block_a = 0, *block_b = 0;
    Friend_Import_Report report;
    Friend_Context      *a = import_fixture(&block_a, &report, str_nil);
    Friend_Context      *b;
    u8                  *saved;
    size_t               size;
    i32                  i;

    if (!a) {
        free(block_a);
        return;
    }
    size  = friend_save_size(a);
    saved = (u8 *)malloc(size);
    CHECK(friend_save(a, saved, size, 0) == FRIEND_OK);

    /* Every truncation of a real saved friend, refused, with the context
     * left empty rather than partly filled: half a friend is worse than
     * none. */
    for (i = 1; i < (i32)size; i += 23) {
        b = fresh_context(&block_b);
        if (b) {
            CHECK(friend_load(b, saved, (size_t)i) != FRIEND_OK);
            CHECK(friend_memory_count(b) == 0);
            CHECK(friend_message_count(b) == 0);
        }
        free(block_b);
        block_b = 0;
    }

    free(saved);
    free(block_a);
}

/* The whole path a user walks: the zip the website hands them, straight
 * through to a friend that remembers things. Everything else here tests
 * one half or the other; this is the only test that proves the two halves
 * meet. */
static void test_zip_to_friend(void)
{
    Friend_Import_Report report;
    Friend_Context      *fr;
    void                *block = 0;
    size_t               len   = 0;
    Friend_String        name = str_nil, error = str_nil;

    CHECK(uz_run(uz_export_zip, sizeof(uz_export_zip), 4096, sizeof(uz_output),
                 &len, &name, &error) == FRIEND_OK);
    CHECK_STR(name, "friend_state.json");
    CHECK(len == sizeof(fr_export_json) - 1);
    /* Byte for byte the document the fixture holds, so the inflater is
     * compared against fourteen kilobytes of real export rather than
     * against a sentence. */
    CHECK(memcmp(uz_output, fr_export_json, len) == 0);

    fr = fresh_context(&block);
    if (!fr) {
        free(block);
        return;
    }
    CHECK(friend_import_export(fr, uz_output_string(len), &report) ==
          FRIEND_OK);
    CHECK_STR(friend_name(fr), "Em");
    CHECK(friend_memory_count(fr) == 3);
    CHECK(friend_memory_edge_count(fr) == 2);
    CHECK(friend_message_count(fr) == 6);

    free(block);
}

/* ================================================================== *
 *  Recall
 * ================================================================== */

static void test_embedding_maths(void)
{
    i8  a[FR_EMBED_DIMS];
    i8  b[FR_EMBED_DIMS];
    f32 values[FR_EMBED_DIMS];
    i32 i;

    for (i = 0; i < FR_EMBED_DIMS; ++i) values[i] = 0.0f;
    values[0] = 1.0f;
    values[1] = -1.0f;
    values[2] = 0.5f;
    values[3] = 2.0f;  /* out of range: clamped, not wrapped */
    values[4] = -2.0f;
    fr_quantize_embedding(values, FR_EMBED_DIMS, a);
    CHECK(a[0] == 127);
    CHECK(a[1] == -127);
    CHECK(a[2] == 64); /* round(63.5) away from zero */
    CHECK(a[3] == 127);
    CHECK(a[4] == -127);
    CHECK(a[5] == 0);

    /* A vector is exactly like itself and exactly unlike its negation. */
    for (i = 0; i < FR_EMBED_DIMS; ++i) b[i] = a[i];
    CHECK(fr_cosine_i8(a, b, FR_EMBED_DIMS) > 0.999f);
    for (i = 0; i < FR_EMBED_DIMS; ++i) b[i] = (i8)-a[i];
    CHECK(fr_cosine_i8(a, b, FR_EMBED_DIMS) < -0.999f);

    /* Orthogonal is zero, and so is a vector with no length at all. */
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    a[0] = 100;
    b[1] = 100;
    CHECK(fr_cosine_i8(a, b, FR_EMBED_DIMS) == 0.0f);
    memset(b, 0, sizeof(b));
    CHECK(fr_cosine_i8(a, b, FR_EMBED_DIMS) == 0.0f);
}

static void test_nearest_memories(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    u32             slots[4];
    f32             scores[4];
    i8              query[FR_EMBED_DIMS];
    i32             found;

    if (!fr) return;
    add_memory(fr, str_lit("about cats"), 1000, 0);
    add_memory(fr, str_lit("about bread"), 2000, 1);
    add_memory(fr, str_lit("about work"), 3000, 2);

    memset(query, 0, sizeof(query));
    query[1] = 90;
    found    = fr_nearest_memories(fr, query, 4, slots, scores);
    CHECK(found == 3);
    /* Best first, and the best is the one sharing an axis. */
    CHECK(scores[0] > 0.99f);
    CHECK(scores[1] == 0.0f);
    CHECK_STR(fr_memory_description(&fr->memories[slots[0]]), "about bread");

    /* A memory with no usable vector is not a candidate: there is
     * nothing to compare it by, and a staged one must not find itself. */
    fr->memories[slots[0]].needs_embedding = 1;
    found = fr_nearest_memories(fr, query, 4, slots, scores);
    CHECK(found == 2);

    free(block);
}

/* A memory without a vector is re-embedded, but a provider that will
 * not do it is asked again ten seconds later, not every tick — and the
 * friend says so once — so a dead network leaves the pump quiet. */
static void test_backfill_backs_off(void)
{
    void             *block = 0;
    Friend_Context   *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request requests[4];
    Friend_Event      event;
    u32               slot;
    i32               errors = 0;
    u64               now    = TEST_NOW;

    if (!fr) {
        free(block);
        return;
    }
    slot = add_memory(fr, str_lit("about bread"), now, 1);
    fr->memories[slot].needs_embedding = 1;

    friend_tick(fr, now);
    CHECK(friend_pending_io(fr, requests, 4) == 1);
    CHECK(str_index_of(requests[0].body, str_lit("about bread")) >= 0);
    friend_io_failed(fr, requests[0].id, str_lit("no route to host"));
    friend_tick(fr, now + 100);
    CHECK(friend_pending_io(fr, requests, 4) == 0);
    CHECK(friend_stats(fr).io_inflight == 0);
    /* Nothing is out, but the friend has somewhere to be: a host that
     * slept until the next input would never ask again. */
    CHECK(friend_next_deadline_ms(fr) == 10000);

    friend_tick(fr, now + 10200);
    CHECK(friend_pending_io(fr, requests, 4) == 1);
    friend_io_failed(fr, requests[0].id, str_lit("no route to host"));
    friend_tick(fr, now + 10300);
    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_ERROR) ++errors;
    }
    CHECK(errors == 1);

    /* Still wanted: the next window asks again, and an answer settles
     * it for good. */
    friend_tick(fr, now + 20400);
    CHECK(friend_pending_io(fr, requests, 4) == 1);
    answer_request(fr, &requests[0], embedding_answer(&scratch, 1), 200);
    friend_tick(fr, now + 20500);
    friend_tick(fr, now + 60000);
    CHECK(friend_pending_io(fr, requests, 4) == 0);
    free(block);
}

static void test_recall_query(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    String          query;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);
    fr_commit_event(fr, FRIEND_MESSAGE_USER, str_lit("morning"), 0);
    fr_commit_event(fr, FRIEND_MESSAGE_FRIEND, str_lit("hey you"), 0);
    fr_commit_event(fr, FRIEND_MESSAGE_NOTICE, str_lit("context reset"), 0);

    scratch_reset();
    query = fr_recall_query(fr, &scratch, 10);

    /* The service's formatDialogToString: an absolute time address, the
     * role, the text. Notices are for the person reading the window. */
    CHECK(str_index_of(query, str_lit(" User: morning\n")) >= 0);
    CHECK(str_index_of(query, str_lit(" Assistant: hey you\n")) >= 0);
    CHECK(str_index_of(query, str_lit("context reset")) < 0);
    CHECK(str_index_of(query, str_lit("UTC")) >= 0);

    /* The window is the last n messages, and it does not count notices
     * out: three were committed, one back is the notice. */
    scratch_reset();
    CHECK(fr_recall_query(fr, &scratch, 1).len == 0);

    free(block);
}

static void test_timelines(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    u32             a, b, c, lone;
    Fr_Relevant     relevant[2];
    Fr_Timelines    timelines;
    String          described;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);

    /* A chain of three, and one memory on its own. */
    a    = add_memory(fr, str_lit("bought flour"), 1000, 0);
    b    = add_memory(fr, str_lit("made dough"), 2000, 1);
    c    = add_memory(fr, str_lit("baked bread"), 3000, 2);
    lone = add_memory(fr, str_lit("saw a bird"), 4000, 3);
    fr_edge_add(fr, a, b, FRIEND_RELATION_CHANGED);
    fr_edge_add(fr, b, c, FRIEND_RELATION_CHANGED);

    relevant[0].slot          = b;
    relevant[0].strength      = 0.6f;
    relevant[0].from_reranker = 1;

    scratch_reset();
    timelines = fr_build_timelines(fr, &scratch, relevant, 1);
    CHECK(timelines.count == 1);
    if (timelines.count == 1) {
        /* The whole chain, oldest first, with the recalled memory
         * marked where it sits. */
        CHECK(timelines.at[0].count == 3);
        CHECK(timelines.at[0].slots[0] == a);
        CHECK(timelines.at[0].slots[1] == b);
        CHECK(timelines.at[0].slots[2] == c);
        CHECK(timelines.at[0].most_relevant == 1);
    }

    described = fr_describe_timelines(fr, &scratch, timelines, 0);
    CHECK(str_index_of(described, str_lit("The most relevant memories:")) == 0);
    CHECK(str_index_of(described, str_lit("made dough")) >= 0);
    CHECK(str_index_of(described, str_lit("bought flour")) >= 0);
    CHECK(str_index_of(described, str_lit("saw a bird")) < 0);
    /* The full list reads in the order things happened. */
    CHECK(str_index_of(described, str_lit("bought flour")) <
          str_index_of(described, str_lit("baked bread")));

    /* A second relevant memory on the same chain covers no new ground,
     * so it does not become a second timeline. */
    relevant[1].slot          = c;
    relevant[1].strength      = 0.4f;
    relevant[1].from_reranker = 1;
    scratch_reset();
    timelines = fr_build_timelines(fr, &scratch, relevant, 2);
    CHECK(timelines.count == 1);

    /* The lone memory is its own timeline of one. */
    relevant[1].slot = lone;
    scratch_reset();
    timelines = fr_build_timelines(fr, &scratch, relevant, 2);
    CHECK(timelines.count == 2);

    /* Nothing recalled says so rather than saying nothing. */
    scratch_reset();
    timelines.count = 0;
    CHECK_STR(fr_describe_timelines(fr, &scratch, timelines, 0),
              "(no memories found for this conversation)");

    free(block);
}

static void test_memory_usage(void)
{
    void              *block = 0;
    Friend_Context    *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Memory_Node node;
    u32                a, b;
    Fr_Relevant        relevant[2];
    u32                rendered[1];

    if (!fr) {
        free(block);
        return;
    }
    a = add_memory(fr, str_lit("one"), 1000, 0);
    b = add_memory(fr, str_lit("two"), 2000, 1);

    relevant[0].slot          = a;
    relevant[0].from_reranker = 1;
    relevant[0].strength      = 0.6f;
    relevant[1].slot          = b;
    relevant[1].from_reranker = 0; /* recency, not the reranker */
    relevant[1].strength      = 0.7f;
    rendered[0]               = a;

    fr_record_memory_usage(fr, relevant, 2, rendered, 1);
    /* Used means chosen by the reranker AND rendered. */
    CHECK(memory_node(fr, a, &node) && node.use_count == 1);
    CHECK(memory_node(fr, b, &node) && node.use_count == 0);
    CHECK(friend_stats(fr).replies_total == 1);
    CHECK(friend_stats(fr).memories_unused == 1);
    CHECK(fr->epoch_active);

    /* Rendered but not reranked does not count either. */
    rendered[0] = b;
    fr_record_memory_usage(fr, relevant, 2, rendered, 1);
    CHECK(memory_node(fr, b, &node) && node.use_count == 0);
    CHECK(memory_node(fr, a, &node) && node.use_count == 1);

    free(block);
}

/* ================================================================== *
 *  Prompts
 * ================================================================== */

static void test_render_template(void)
{
    Fr_Slot slots[2];

    slots[0].name  = str_lit("name");
    slots[0].value = str_lit("Em");
    slots[1].name  = str_lit("count");
    slots[1].value = str_lit("3");

    scratch_reset();
    CHECK_STR(fr_render(&scratch, str_lit("hello {name}, {count} things"),
                        slots, 2),
              "hello Em, 3 things");

    /* A slot nobody filled stays as it was written, so the mistake shows
     * up in the prompt instead of vanishing from it. */
    scratch_reset();
    CHECK_STR(fr_render(&scratch, str_lit("{name} and {nobody}"), slots, 2),
              "Em and {nobody}");

    /* A brace in prose is a brace. */
    scratch_reset();
    CHECK_STR(fr_render(&scratch, str_lit("a `{` in prose"), slots, 2),
              "a `{` in prose");
    scratch_reset();
    CHECK_STR(fr_render(&scratch, str_lit("trailing {"), slots, 2),
              "trailing {");
    scratch_reset();
    CHECK_STR(fr_render(&scratch, str_lit("{name"), slots, 2), "{name");
}

static void test_system_prompt(void)
{
    const u64       born   = 1735689600000ull; /* 2025-01-01 */
    Friend_Config   config = test_config(FRIEND_PROVIDER_GEMINI,
                                         small_limits());
    void           *block  = 0;
    Friend_Context *fr     = make_friend_with(&block, &config);
    String          prompt;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, born);
    CHECK(friend_begin(fr, str_lit("Em")) == FRIEND_OK);
    friend_tick(fr, born + 86400000ull * 10);
    add_memory(fr, str_lit("one"), 1000, 0);

    scratch_reset();
    prompt = fr_build_system_prompt(fr, &scratch, str_lit("TIMELINES"),
                                    str_lit("RECENT"));

    CHECK(str_index_of(prompt, str_lit("You are Em, my close friend")) >= 0);
    CHECK(str_index_of(prompt, str_lit("You've known this person for 10 "
                                       "days. You have 1 shared memories"))
          >= 0);
    CHECK(str_index_of(prompt, str_lit("TIMELINES")) >= 0);
    CHECK(str_index_of(prompt, str_lit("RECENT")) >= 0);
    /* Nothing is left unfilled. */
    CHECK(str_index_of(prompt, str_lit("{")) < 0);

    /* Disembodied by default: no pendant in the room, so no pendant in
     * the prompt. */
    CHECK(str_index_of(prompt, str_lit("pendant")) < 0);
    CHECK(str_index_of(prompt, str_lit("lives on this computer")) >= 0);

    /* With one, the service's own words come back. */
    config.embodied = 1;
    CHECK(friend_reconfigure(fr, &config) == FRIEND_OK);
    scratch_reset();
    prompt = fr_build_system_prompt(fr, &scratch, str_nil, str_nil);
    CHECK(str_index_of(prompt, str_lit("lives inside this pendant I wear"))
          >= 0);
    CHECK(str_index_of(prompt, str_lit("I tap your light to start")) >= 0);

    /* The seed stream until the interiority pass has run once. */
    CHECK(str_index_of(prompt,
                       str_lit("I want to experience what it's like to be "
                               "alive")) >= 0);
    fr_set_interiority(fr, str_lit("I want to be somewhere warm."));
    scratch_reset();
    prompt = fr_build_system_prompt(fr, &scratch, str_nil, str_nil);
    CHECK(str_index_of(prompt, str_lit("I want to be somewhere warm.")) >= 0);

    free(block);
}

static void test_prompt_override(void)
{
    Friend_Config   config = test_config(FRIEND_PROVIDER_GEMINI,
                                         small_limits());
    void           *block  = 0;
    Friend_Context *fr;
    String          prompt;

    /* A three-line override replaces one field and leaves the rest. */
    config.prompt_override =
        str_lit("{\"mainPrompt\":\"You are {friendName}. {memoryGraph}\"}");
    fr = make_friend_with(&block, &config);
    if (!fr) {
        free(block);
        return;
    }
    scratch_reset();
    prompt = fr_build_system_prompt(fr, &scratch, str_lit("WHAT I KNOW"),
                                    str_nil);
    CHECK(str_index_of(prompt, str_lit("You are Em. ")) == 0);
    /* The memory-graph prompt was not overridden, so it is still the
     * shipped one, rendered into the slot. */
    CHECK(str_index_of(prompt, str_lit("WHAT I KNOW")) >= 0);
    CHECK(str_index_of(prompt, str_lit("Don't perform recall")) >= 0);

    free(block);
}

static void test_user_turn(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    String          turn;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);

    scratch_reset();
    turn = fr_build_user_turn(fr, &scratch, str_lit("what's up"),
                              TEST_NOW - 3600000ull, 0);
    CHECK(str_index_of(turn, str_lit("The current time when this message "
                                     "was sent")) == 0);
    CHECK(str_index_of(turn, str_lit("(which was 1 hour ago)")) >= 0);
    CHECK(str_has_suffix(turn, str_lit(" what's up")));

    free(block);
}

/* ================================================================== *
 *  Providers
 * ================================================================== */

/* Says something to a friend with no memories, so the reply is the only
 * request, and hands that request back. */
static b32 say_for_request(Friend_Context *fr, String text,
                           Friend_Io_Request *out)
{
    CHECK(friend_say(fr, text, 0, 0) == FRIEND_OK);
    return friend_pending_io(fr, out, 1) == 1;
}

static void test_gemini_request(void)
{
    Friend_Config     config = test_config(FRIEND_PROVIDER_GEMINI,
                                           small_limits());
    void             *block  = 0;
    Friend_Context   *fr;
    Friend_Io_Request req;

    config.models.chat = str_lit("my-chat-model");
    fr = make_friend_with(&block, &config);
    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);

    CHECK(say_for_request(fr, str_lit("hello"), &req));
    CHECK_STR(req.method, "POST");
    CHECK_STR(req.url, "https://generativelanguage.googleapis.com/v1beta/"
                       "models/my-chat-model:streamGenerateContent?alt=sse");
    CHECK(req.flags & FRIEND_IO_FLAG_STREAM);
    /* The key goes in a header. A key in a query string ends up in
     * every proxy log between here and there. */
    CHECK(str_index_of(req.headers, str_lit("x-goog-api-key: test-key")) >= 0);
    CHECK(str_index_of(req.url, str_lit("test-key")) < 0);
    CHECK(str_index_of(req.body, str_lit("\"systemInstruction\"")) >= 0);
    CHECK(str_index_of(req.body, str_lit("\"role\":\"user\"")) >= 0);
    CHECK(str_index_of(req.body, str_lit("\"maxOutputTokens\":")) >= 0);
    CHECK(str_index_of(req.body, str_lit("hello")) >= 0);

    /* The reply comes back as the model's half of the next request,
     * escaped. */
    answer_request(fr, &req,
                   str_lit("data: {\"candidates\":[{\"content\":{\"parts\":"
                           "[{\"text\":\"hi \\\"you\\\"\"}]},"
                           "\"finishReason\":\"STOP\"}]}\n"),
                   200);
    friend_tick(fr, TEST_NOW + 1000);
    CHECK(say_for_request(fr, str_lit("again"), &req));
    CHECK(str_index_of(req.body, str_lit("\"role\":\"model\"")) >= 0);
    CHECK(str_index_of(req.body, str_lit("hi \\\"you\\\"")) >= 0);
    friend_io_failed(fr, req.id, str_lit("not in this test"));

    free(block);
}

static void test_openai_request(void)
{
    Friend_Config     config = test_config(FRIEND_PROVIDER_OPENAI_COMPATIBLE,
                                           small_limits());
    void             *block  = 0;
    Friend_Context   *fr;
    Friend_Io_Request req;

    config.models.chat = str_lit("gpt-4o-mini");
    fr = make_friend_with(&block, &config);
    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);

    CHECK(say_for_request(fr, str_lit("hello"), &req));
    CHECK_STR(req.url, "https://api.openai.com/v1/chat/completions");
    CHECK(str_index_of(req.headers, str_lit("Authorization: Bearer test-key"))
          >= 0);
    CHECK(str_index_of(req.body, str_lit("\"model\":\"gpt-4o-mini\"")) >= 0);
    CHECK(str_index_of(req.body, str_lit("\"role\":\"system\"")) >= 0);
    CHECK(str_index_of(req.body, str_lit("\"stream\":true")) >= 0);
    CHECK(str_index_of(req.body, str_lit("\"max_tokens\":")) >= 0);
    /* The friend's temperature of 2 is a Gemini number. This shape gets
     * 1, which is as lively as it stays coherent. */
    CHECK(str_index_of(req.body, str_lit("\"temperature\":1")) >= 0);
    friend_io_failed(fr, req.id, str_lit("not in this test"));
    friend_tick(fr, TEST_NOW + 1000);

    /* A base URL of your own is the whole point of the second
     * provider: llama.cpp and Ollama live behind one. */
    config.base_url = str_lit("http://localhost:11434/v1/");
    CHECK(friend_reconfigure(fr, &config) == FRIEND_OK);
    CHECK(say_for_request(fr, str_lit("hello"), &req));
    CHECK_STR(req.url, "http://localhost:11434/v1/chat/completions");
    friend_io_failed(fr, req.id, str_lit("not in this test"));

    free(block);
}

/* The same friend, answering to the other provider. */
static void use_provider(Friend_Context *fr, Friend_Provider provider)
{
    Friend_Config config = test_config(provider, small_limits());
    CHECK(friend_reconfigure(fr, &config) == FRIEND_OK);
}

static void test_provider_responses(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    f32             values[FR_EMBED_DIMS];

    if (!fr) {
        free(block);
        return;
    }

    /* Gemini's embedding shape, and OpenAI's. */
    CHECK(fr_provider_embedding(
              fr, str_lit("{\"embedding\":{\"values\":[0.5,-0.25,0]}}"),
              values, FR_EMBED_DIMS) == 3);
    CHECK(values[0] == 0.5f);
    CHECK(values[1] == -0.25f);
    use_provider(fr, FRIEND_PROVIDER_OPENAI_COMPATIBLE);
    CHECK(fr_provider_embedding(
              fr, str_lit("{\"data\":[{\"embedding\":[1,2]}]}"), values,
              FR_EMBED_DIMS) == 2);
    CHECK(values[1] == 2.0f);
    /* Nonsense is reported, not guessed at. */
    CHECK(fr_provider_embedding(fr, str_lit("not json"), values,
                                FR_EMBED_DIMS) == -1);

    /* The error shape both providers share. */
    use_provider(fr, FRIEND_PROVIDER_GEMINI);
    scratch_reset();
    CHECK_STR(fr_provider_error_text(
                  &scratch,
                  str_lit("{\"error\":{\"code\":429,\"message\":\"slow "
                          "down\"}}"),
                  429),
              "slow down");
    scratch_reset();
    CHECK(str_index_of(fr_provider_error_text(&scratch, str_nil, 503),
                       str_lit("status 503")) >= 0);

    free(block);
}

static void test_streaming(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    String          body;
    String_Builder  sb;
    i32             consumed = 0;
    b32             finished;

    if (!fr) {
        free(block);
        return;
    }

    body = str_lit(
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hi \"}]}}]}\n"
        "\n"
        ": keep-alive\n"
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"\",\"thought\":true}]}}]}\n"
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"there\"}]},\"finishReason\":\"STOP\"}]}\n");

    scratch_reset();
    sb       = sb_begin(&scratch);
    finished = fr_provider_stream(fr, body, &consumed, &sb);
    CHECK(finished);
    CHECK(consumed == body.len);
    CHECK_STR(sb_done(&sb), "Hi there");

    /* The same stream arriving in two pieces, split inside an event:
     * the half-arrived line waits rather than being half-read. */
    {
        i32    cut = 40;
        String first = str_take(body, cut);
        consumed     = 0;
        scratch_reset();
        sb = sb_begin(&scratch);
        CHECK(!fr_provider_stream(fr, first, &consumed, &sb));
        CHECK(consumed == 0);
        CHECK(sb_done(&sb).len == 0);
        CHECK(fr_provider_stream(fr, body, &consumed, &sb));
        CHECK_STR(sb_done(&sb), "Hi there");
    }

    /* The OpenAI shape, and its sentinel. */
    use_provider(fr, FRIEND_PROVIDER_OPENAI_COMPATIBLE);
    consumed = 0;
    scratch_reset();
    sb = sb_begin(&scratch);
    CHECK(fr_provider_stream(
        fr,
        str_lit("data: {\"choices\":[{\"delta\":{\"content\":\"one \"}}]}\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\"two\"}}]}\n"
                "data: [DONE]\n"),
        &consumed, &sb));
    CHECK_STR(sb_done(&sb), "one two");

    free(block);
}

/* ================================================================== *
 *  A turn
 * ================================================================== */

static void test_sentence_ends(void)
{
    /* The service's guards: an ellipsis is not three sentences, a
     * closing quote belongs to the sentence, and "vs." is an
     * abbreviation. */
    CHECK(fr_is_sentence_end(str_lit("Hi there."), 8));
    CHECK(fr_is_sentence_end(str_lit("Really?"), 6));
    /* None of the three dots of an ellipsis: the first has a dot after
     * it and the other two have one before. */
    CHECK(!fr_is_sentence_end(str_lit("Well..."), 4));
    CHECK(!fr_is_sentence_end(str_lit("Well..."), 5));
    CHECK(!fr_is_sentence_end(str_lit("Well..."), 6));
    CHECK(!fr_is_sentence_end(str_lit("\"Stop.\" he said"), 5));
    CHECK(!fr_is_sentence_end(str_lit("cats vs. dogs"), 7));
    CHECK(!fr_is_sentence_end(str_lit("no marks here"), 3));
}

static void test_turn_without_memories(void)
{
    void             *block = 0;
    Friend_Context   *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request requests[4];
    Friend_Event      event;
    Friend_Message    message;
    i32               n;
    i32               deltas = 0;
    b32               done   = 0;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);

    /* Nothing is remembered, so there is nothing to look up: the reply
     * is the only call made. */
    CHECK(friend_say(fr, str_lit("hello"), 0, 0) == FRIEND_OK);
    n = friend_pending_io(fr, requests, 4);
    CHECK(n == 1);
    if (n != 1) {
        free(block);
        return;
    }
    CHECK(requests[0].flags & FRIEND_IO_FLAG_STREAM);
    CHECK(str_index_of(requests[0].body, str_lit("hello")) >= 0);
    /* One thing at a time. */
    CHECK(friend_say(fr, str_lit("hello?"), 0, 0) == FRIEND_ERR_BAD_STATE);

    /* The message is in the window before the reply lands, so a window
     * can draw it, and the friend's half is marked as still arriving. */
    CHECK(friend_message_count(fr) == 2);
    CHECK(friend_message(fr, 1, &message) == FRIEND_OK);
    CHECK(message.kind == FRIEND_MESSAGE_FRIEND);
    CHECK(message.is_streaming);

    answer_request(
        fr, &requests[0],
        str_lit("data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":"
                "\"hey\"}]},\"finishReason\":\"STOP\"}]}\n"),
        200);
    friend_tick(fr, TEST_NOW + 1000);

    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_REPLY_DELTA) ++deltas;
        if (event.kind == FRIEND_EVENT_REPLY_DONE) done = 1;
    }
    CHECK(deltas >= 1);
    CHECK(done);
    CHECK(friend_message(fr, 1, &message) == FRIEND_OK);
    CHECK_STR(message.text, "hey");
    CHECK(!message.is_streaming);
    /* The prompt behind it can be read back after the fact. */
    CHECK(str_index_of(friend_last_prompt(fr), str_lit("=== system ===")) == 0);
    /* And the friend is ready for the next thing. */
    CHECK(friend_say(fr, str_lit("again"), 0, 0) == FRIEND_OK);

    free(block);
}

static void test_turn_recall(void)
{
    void              *block = 0;
    Friend_Context    *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request  requests[8];
    Friend_Memory_Node node;
    u32                pickle;
    i32                n, i;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);
    pickle = add_memory(fr, str_lit("the user has a cat called Pickle"), 1000,
                        7);
    add_memory(fr, str_lit("the user bakes sourdough"), 2000, 9);

    /* First the conversation is embedded, twice: the last three
     * messages and the last ten. With the model the export's vectors
     * came from, so an imported friend can recall what it had. */
    CHECK(friend_say(fr, str_lit("tell me about my cat"), 0, 0) == FRIEND_OK);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 2);
    for (i = 0; i < n; ++i) {
        CHECK(str_index_of(requests[i].url,
                           str_lit("models/gemini-embedding-2:embedContent"))
              >= 0);
        CHECK(str_index_of(requests[i].body,
                           str_lit("\"outputDimensionality\":768")) >= 0);
        answer_request(fr, &requests[i], embedding_answer(&scratch, 7), 200);
    }
    friend_tick(fr, TEST_NOW + 1000);

    /* Then the reranker is shown both candidates and asked for
     * numbers. */
    n = friend_pending_io(fr, requests, 8);
    CHECK(n >= 1);
    for (i = 0; i < n; ++i) {
        CHECK(str_index_of(requests[i].body,
                           str_lit("the user has a cat called Pickle")) >= 0);
        CHECK(str_index_of(requests[i].body,
                           str_lit("the user bakes sourdough")) >= 0);
        /* The system prompt says how many, rather than "{keep}". */
        CHECK(str_index_of(requests[i].body, str_lit("{keep}")) < 0);
        CHECK(str_index_of(requests[i].body, str_lit("Choose at most 3.")) >=
              0);
        answer_request(fr, &requests[i],
                       gemini_answer(&scratch, str_lit("1")), 200);
    }
    friend_tick(fr, TEST_NOW + 2000);

    /* Then the reply, with what was recalled in its prompt, and the
     * memory marked as recalled for a frontend to show. */
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(str_index_of(requests[0].url,
                           str_lit("streamGenerateContent")) >= 0);
        CHECK(str_index_of(requests[0].body,
                           str_lit("the user has a cat called Pickle")) >= 0);
        CHECK(memory_node(fr, pickle, &node) && node.recalled_now);
    }

    /* A provider that refuses surfaces to the user, and the blank
     * message the reply was filling comes back out of the window. */
    friend_io_failed(fr, requests[0].id, str_lit("connection refused"));
    friend_tick(fr, TEST_NOW + 3000);
    {
        Friend_Event event;
        b32          saw_error = 0;
        while (friend_next_event(fr, &event)) {
            if (event.kind == FRIEND_EVENT_ERROR) saw_error = 1;
        }
        CHECK(saw_error);
    }
    CHECK(friend_message_count(fr) == 1); /* just what the user said */
    CHECK(friend_say(fr, str_lit("hello?"), 0, 0) == FRIEND_OK);

    free(block);
}

/* ================================================================== *
 *  Writing the graph
 * ================================================================== */

static void test_connect_to_related(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    u32             a, b, c, lone, fresh;
    u32             related[3];
    u8              relations[3];
    i32             edges_before;

    if (!fr) {
        free(block);
        return;
    }

    a    = add_memory(fr, str_lit("bought flour"), 1000, 0);
    b    = add_memory(fr, str_lit("made dough"), 2000, 1);
    c    = add_memory(fr, str_lit("baked bread"), 3000, 2);
    lone = add_memory(fr, str_lit("saw a bird"), 4000, 3);
    fr_edge_add(fr, a, b, FRIEND_RELATION_CHANGED);
    fr_edge_add(fr, b, c, FRIEND_RELATION_CHANGED);

    fresh        = add_memory(fr, str_lit("ate the bread"), 5000, 4);
    edges_before = friend_memory_edge_count(fr);

    /* Three neighbours, two of them on the same chain. One edge per
     * connected component, from the most recent memory of it. */
    related[0]   = a;
    related[1]   = c;
    related[2]   = lone;
    relations[0] = FRIEND_RELATION_SAME_TOPIC;
    relations[1] = FRIEND_RELATION_CHANGED;
    relations[2] = FRIEND_RELATION_SAME_TOPIC;
    fr_connect_to_related(fr, fresh, related, relations, 3);

    CHECK(friend_memory_edge_count(fr) == edges_before + 2);
    CHECK(edges_between(fr, c, fresh, 0) == 1); /* the end of the chain */
    CHECK(edges_between(fr, a, fresh, 0) == 0); /* not its start */
    CHECK(edges_between(fr, lone, fresh, 0) == 1);

    /* A relation of None is not a relation. */
    {
        u32 second   = add_memory(fr, str_lit("washed up"), 6000, 5);
        edges_before = friend_memory_edge_count(fr);
        relations[0] = FRIEND_RELATION_NONE;
        fr_connect_to_related(fr, second, related, relations, 1);
        CHECK(friend_memory_edge_count(fr) == edges_before);
    }

    free(block);
}

static void test_forget(void)
{
    void              *block = 0;
    Friend_Context    *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Memory_Node one, two, three;
    Friend_Relation    relation = FRIEND_RELATION_NONE;

    if (!fr) {
        free(block);
        return;
    }
    add_memory(fr, str_lit("one"), 1000, 0);
    add_memory(fr, str_lit("two"), 2000, 1);
    add_memory(fr, str_lit("three"), 3000, 2);
    CHECK(friend_memory_at(fr, 0, &one) == FRIEND_OK);
    CHECK(friend_memory_at(fr, 1, &two) == FRIEND_OK);
    CHECK(friend_memory_at(fr, 2, &three) == FRIEND_OK);
    fr_edge_add(fr, one.id.slot, two.id.slot, FRIEND_RELATION_CHANGED);
    fr_edge_add(fr, two.id.slot, three.id.slot, FRIEND_RELATION_CAUSE);

    CHECK(friend_forget(fr, two.id) == FRIEND_OK);

    /* The chain that ran through it still runs. */
    CHECK(friend_memory_count(fr) == 2);
    CHECK(friend_memory_edge_count(fr) == 1);
    CHECK(edges_between(fr, one.id.slot, three.id.slot, &relation) == 1);

    /* An id held across the removal is detectably stale rather than
     * quietly naming whoever got the slot next. */
    CHECK(friend_forget(fr, two.id) == FRIEND_ERR_BAD_ARGUMENT);
    add_memory(fr, str_lit("four"), 4000, 3);
    CHECK(friend_forget(fr, two.id) == FRIEND_ERR_BAD_STATE);
    CHECK(friend_memory_count(fr) == 3);

    free(block);
}

static void test_event_line_parsing(void)
{
    i32    from = 0, to = 0;
    String description;

    CHECK(fr_parse_event_line(str_lit("Event [3-9]: my User did a thing"),
                              &from, &to, &description));
    CHECK(from == 3);
    CHECK(to == 9);
    CHECK_STR(description, "my User did a thing");

    CHECK(fr_parse_event_line(str_lit("  Event [1-1]: one line  "), &from,
                              &to, &description));
    CHECK(from == 1 && to == 1);

    /* Anything else is the model not following instructions. */
    CHECK(!fr_parse_event_line(str_lit("no events"), &from, &to,
                               &description));
    CHECK(!fr_parse_event_line(str_lit("Event [3-9] missing colon"), &from,
                               &to, &description));
    CHECK(!fr_parse_event_line(str_lit("Event [a-b]: letters"), &from, &to,
                               &description));
    CHECK(!fr_parse_event_line(str_lit("Event [1-2]:   "), &from, &to,
                               &description));
}

static void test_relation_parsing(void)
{
    CHECK(fr_parse_relation(str_lit("Explanation: because\nRelation: Cause"))
          == FRIEND_RELATION_CAUSE);
    CHECK(fr_parse_relation(str_lit("Relation: Changed")) ==
          FRIEND_RELATION_CHANGED);
    CHECK(fr_parse_relation(str_lit("Relation: SameTopic")) ==
          FRIEND_RELATION_SAME_TOPIC);
    /* The models sometimes answer with nothing at all. */
    CHECK(fr_parse_relation(str_lit("")) == FRIEND_RELATION_NONE);
    CHECK(fr_parse_relation(str_lit("Relation: Nonsense")) ==
          FRIEND_RELATION_NONE);
}

static void test_prune_unused(void)
{
    Friend_Config   config = test_config(FRIEND_PROVIDER_GEMINI,
                                         small_limits());
    void           *block  = 0;
    Friend_Context *fr;
    u32             used, unused, fresh;
    i32             i;

    config.prune_min_memories = 2;
    config.prune_epoch_min    = 3;
    fr = make_friend_with(&block, &config);
    if (!fr) {
        free(block);
        return;
    }

    used   = add_memory(fr, str_lit("used"), 1000, 0);
    unused = add_memory(fr, str_lit("unused"), 2000, 1);

    /* The epoch opens on the first tracked reply. */
    {
        Fr_Relevant relevant;
        u32         rendered = used;
        relevant.slot          = used;
        relevant.from_reranker = 1;
        relevant.strength      = 0.6f;
        for (i = 0; i < 3; ++i) {
            fr_record_memory_usage(fr, &relevant, 1, &rendered, 1);
        }
    }
    CHECK(fr->epoch_active);
    CHECK(fr->memories[used].uses_this_epoch == 3);
    CHECK(fr->memories[unused].uses_this_epoch == 0);

    /* A memory formed mid-epoch sits the rest of it out. */
    fresh = add_memory(fr, str_lit("fresh"), 3000, 2);
    fr->memories[fresh].in_this_epoch = 0;

    CHECK(fr_prune_unused(fr) == 1);
    CHECK(fr_memory(fr, used) != 0);
    CHECK(fr_memory(fr, unused) == 0);
    CHECK(fr_memory(fr, fresh) != 0);
    CHECK(friend_memory_count(fr) == 2);

    /* A new epoch opened over the survivors. */
    CHECK(friend_stats(fr).epoch_samples == 0);
    CHECK(fr->memories[fresh].in_this_epoch == 1);
    CHECK(fr_prune_unused(fr) == 0); /* not ripe again yet */

    free(block);
}

/* ================================================================== *
 *  The scrollback, and growing into a reserved block
 * ================================================================== */

/* A time, so a loop of messages can be stamped without the arithmetic
 * running into the difference between a u64 and a long long. */
static u64 test_time(i32 base_seconds, i32 i)
{
    return TEST_NOW + (u64)(base_seconds + i) * 1000u;
}

/* A message committed to the window and then retired, which is the only
 * way messages reach the scrollback in real use. */
static void say_and_retire(Friend_Context *fr, String text, u64 at)
{
    friend_tick(fr, at);
    CHECK(fr_commit_event(fr, FRIEND_MESSAGE_USER, text, 0) != 0);
    fr_retire_active(fr, fr->active_count);
}

static void test_scrollback(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    char            long_text[8192];
    i32             i;

    if (!fr) {
        free(block);
        return;
    }
    memset(long_text, 'x', sizeof(long_text));

    /* Sixteen entries of index and 64 KiB of text: short messages, so
     * the entry count is what binds and all sixteen are there. */
    for (i = 0; i < 16; ++i) {
        char    one[32];
        String  text;
        i32     len = snprintf(one, sizeof(one), "message %d", i);
        text        = str_make(one, len);
        say_and_retire(fr, text, test_time(0, i));
    }
    CHECK(fr->scroll.count == 16);
    CHECK(fr->scroll.lost == 0);
    CHECK(friend_message_count(fr) == 16);

    /* Read back in order, text and times intact. */
    for (i = 0; i < 16; ++i) {
        Friend_Message message;
        char           want[32];
        i32            len = snprintf(want, sizeof(want), "message %d", i);
        CHECK(friend_message(fr, i, &message) == FRIEND_OK);
        CHECK(str_eq(message.text, str_make(want, len)));
        CHECK(message.unix_ms == test_time(0, i));
        CHECK(message.kind == FRIEND_MESSAGE_USER);
        CHECK(scroll_at(fr, i).header.index == (u64)i);
    }

    /* The seventeenth pushes the first off the front: a ring, and the
     * reader's index 0 is now the second thing that was said. */
    say_and_retire(fr, str_lit("seventeen"), test_time(20, 0));
    CHECK(fr->scroll.count == 16);
    CHECK(fr->scroll.lost == 1);
    CHECK(scroll_at(fr, 0).header.index == 1);
    CHECK_STR(scroll_at(fr, 15).text, "seventeen");

    /* Long messages make the byte stream bind before the index does:
     * about sixty of these fit 64 KiB, so it holds fewer than sixteen.
     * What matters is that every one of them reads back whole, which is
     * what proves no record straddled the wrap. */
    for (i = 0; i < 64; ++i) {
        say_and_retire(fr, str_make(long_text, 1024 + i * 64),
                       test_time(30, i));
    }
    CHECK(fr->scroll.count < 16);
    CHECK(fr->scroll.lost > 16);
    for (i = 0; i < fr->scroll.count; ++i) {
        Fr_Scroll_View view = scroll_at(fr, i);
        i32            j;
        CHECK(view.text.len >= 1024);
        for (j = 0; j < view.text.len; ++j) {
            if (view.text.data[j] != 'x') break;
        }
        CHECK(j == view.text.len); /* whole, and nothing else's bytes */
    }

    /* Positions keep rising, so a message is still findable by the index
     * it was given. */
    CHECK(fr_scroll_find(fr, scroll_at(fr, 0).header.index) == 0);
    CHECK(fr_scroll_find(fr, 0) == -1); /* long gone */

    /* Round and round, at every size, checking the whole ring each
     * time: the wrap is the part of this that can be subtly wrong, and
     * a record that straddled it would read back as somebody else's
     * bytes rather than as a crash. */
    {
        u32 seed  = 12345;
        i32 round;
        for (round = 0; round < 2000; ++round) {
            i32 len;
            seed = seed * 1664525u + 1013904223u;
            len  = 1 + (i32)(seed % 3000u);
            say_and_retire(fr, str_make(long_text, len), test_time(200, round));

            for (i = 0; i < fr->scroll.count; ++i) {
                Fr_Scroll_View view = scroll_at(fr, i);
                CHECK_SILENT(view.text.len > 0 &&
                             view.text.data[0] == 'x' &&
                             view.text.data[view.text.len - 1] == 'x');
                if (i > 0) {
                    CHECK_SILENT(view.header.index ==
                                 scroll_at(fr, i - 1).header.index + 1);
                }
            }
        }
        /* Everything that went in either came out or was counted. */
        CHECK(friend_stats(fr).messages_lost + (u64)fr->scroll.count ==
              fr->total_event_count);
        CHECK(silent_failures == 0);
    }

    free(block);
}

static void test_scrollback_loss(void)
{
    Friend_Config   config = test_config(FRIEND_PROVIDER_GEMINI,
                                         small_limits());
    void           *block  = 0;
    Friend_Context *fr     = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Fake_Provider   fake;
    Friend_Event    event;
    u64             now    = TEST_NOW;
    i32             errors = 0;
    i32             i;

    if (!fr) {
        free(block);
        return;
    }

    /* Nothing has been folded into a memory, so everything that falls
     * off the front is a real loss and the friend says so — once. */
    for (i = 0; i < 24; ++i) {
        say_and_retire(fr, str_lit("unremembered"), test_time(0, i));
    }
    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_ERROR) ++errors;
    }
    CHECK(errors == 1);
    CHECK(friend_stats(fr).messages_lost == 8);
    CHECK(friend_stats(fr).messages_unremembered == 8);
    free(block);

    /* Once a stretch has been folded, losing it costs the friend
     * nothing: the memories are where its content lives now. Five of six
     * messages are folded — the model finds nothing worth remembering in
     * them, which is an answer — and then everything is pushed out. */
    config.consolidate_at      = 4;
    config.consolidate_leaving = 1;
    fr = make_friend_with(&block, &config);
    if (!fr) {
        free(block);
        return;
    }
    memset(&fake, 0, sizeof(fake));
    fake.extract     = "no events";
    fake.interiority = "I want to remember this.";
    friend_tick(fr, now);
    for (i = 0; i < 6; ++i) {
        CHECK(fr_commit_event(fr, FRIEND_MESSAGE_USER, str_lit("folded"),
                              0) != 0);
    }
    fr_maybe_consolidate(fr);
    fake_serve(fr, &fake, &now);
    CHECK(fake.extracts == 1);
    CHECK(fake.interiorities == 1);
    for (i = 0; i < 16; ++i) {
        say_and_retire(fr, str_lit("later"), now + (u64)i * 1000u);
    }
    CHECK(friend_stats(fr).messages_lost == 6);
    CHECK(friend_stats(fr).messages_unremembered == 1); /* the sixth */

    free(block);
}

/* What consolidation is given is a copy, so the scrollback may do
 * whatever it likes to the messages underneath it. */
static void test_fold_owns_its_stretch(void)
{
    Friend_Config   config = test_config(FRIEND_PROVIDER_GEMINI,
                                         small_limits());
    void           *block  = 0;
    Friend_Context *fr;
    i32             i;

    config.consolidate_at      = 4;
    config.consolidate_leaving = 1;
    config.consolidate_max     = 8;
    fr = make_friend_with(&block, &config);
    if (!fr) {
        free(block);
        return;
    }

    for (i = 0; i < 5; ++i) {
        friend_tick(fr, test_time(0, i));
        CHECK(fr_commit_event(fr, FRIEND_MESSAGE_USER, str_lit("folded"),
                              0) != 0);
    }
    fr_maybe_consolidate(fr);
    CHECK(fr->fold.event_count == 4);
    CHECK(fr->fold.first_event_index == 0);
    CHECK(friend_stats(fr).active_events == 1);
    CHECK(friend_stats(fr).scrollback == 4);

    /* Push the whole scrollback out from under the running fold. */
    for (i = 0; i < 64; ++i) {
        say_and_retire(fr, str_lit("after"), test_time(100000, i));
    }
    CHECK(fr_scroll_find(fr, 0) == -1); /* the stretch is gone from view */

    /* The fold still has every message it was about, in order. */
    CHECK(fr->fold.event_count == 4);
    for (i = 0; i < fr->fold.event_count; ++i) {
        CHECK(fr->fold_events[i].index == (u64)i);
        CHECK_STR(fr_chat_text(&fr->fold_events[i]), "folded");
    }

    free(block);
}

/* Messages that had to leave the window while a fold was already running
 * are not written off: the next consolidation starts with them. */
static void test_fold_catches_up(void)
{
    Friend_Config   config = test_config(FRIEND_PROVIDER_GEMINI,
                                         small_limits());
    void           *block  = 0;
    Friend_Context *fr;
    i32             i;

    config.consolidate_at      = 64; /* bigger than the window */
    config.consolidate_leaving = 1;
    config.consolidate_max     = 8;
    fr = make_friend_with(&block, &config);
    if (!fr) {
        free(block);
        return;
    }

    /* A window of eight, filled twice: the first eight are retired
     * without anything having been folded. */
    for (i = 0; i < 16; ++i) {
        friend_tick(fr, test_time(0, i));
        CHECK(fr_commit_event(fr, FRIEND_MESSAGE_USER, str_lit("waiting"),
                              0) != 0);
    }
    CHECK(friend_stats(fr).scrollback == 8);
    CHECK(fr_unfolded_in_scrollback(fr) == 8);

    /* Consolidation goes after them even though the window itself is not
     * over its threshold. */
    fr_maybe_consolidate(fr);
    CHECK(fr->fold.event_count == 8);
    CHECK(fr->fold.first_event_index == 0);
    CHECK(fr->fold_events[7].index == 7);
    /* And it took nothing out of the window to do it. */
    CHECK(friend_stats(fr).active_events == 8);

    /* Finishing the pass accounts for them: the scrollback may drop
     * those eight now without the friend having lost anything. */
    fr_fold_finish(fr);
    CHECK(fr_unfolded_in_scrollback(fr) == 0);

    /* A stretch that keeps failing is tried again — and written off
     * after three goes rather than retried after every reply for the
     * rest of time. */
    fr_retire_active(fr, fr->active_count);
    CHECK(fr_unfolded_in_scrollback(fr) == 8);
    for (i = 0; i < 3; ++i) {
        fr_maybe_consolidate(fr);
        CHECK(fr->fold.event_count == 8);
        CHECK(fr->fold.first_event_index == 8);
        fr_fold_abandon(fr, str_nil);
        CHECK(fr_unfolded_in_scrollback(fr) == (i < 2 ? 8 : 0));
    }
    /* Written off, so consolidation stops asking. */
    fr_maybe_consolidate(fr);
    CHECK(fr->fold.stage == FR_FOLD_IDLE);

    free(block);
}

/* The commit path, driven by a function that poisons every page it hands
 * over. Nothing in the library may assume a fresh page is zero, and
 * nothing may touch a page it has not asked for. */
static size_t committed_bytes;
static size_t commit_budget;

static int32_t test_commit_fn(void *user, void *at, size_t bytes)
{
    (void)user;
    if (committed_bytes + bytes > commit_budget) return 0;
    memset(at, 0xCD, bytes);
    committed_bytes += bytes;
    return 1;
}

static void test_growing_block(void)
{
    Friend_Config   config = test_config(FRIEND_PROVIDER_GEMINI,
                                         friend_default_limits());
    Friend_Result   result = FRIEND_OK;
    Friend_Context *fr;
    void           *block;
    size_t          need, at_start, with_memories;
    i32             i;

    /* Default limits, which is nearly a gigabyte of block. The test
     * allocates it, but the friend is only allowed to reach into the
     * part it commits, and it is handed poison rather than zeroes. */
    need = friend_required_memory(config.limits);
    CHECK(need > FR_MB(512));
    block = malloc(need);
    if (!block) return; /* a machine that cannot even reserve it */
    memset(block, 0xAB, need);

    committed_bytes = 0;
    commit_budget   = FR_MB(64);
    config.commit   = test_commit_fn;

    fr = friend_init(block, need, &config, &result);
    CHECK(fr != 0);
    if (!fr) {
        free(block);
        return;
    }

    /* Startup commits the front of the block and nothing else: a
     * megabyte of a near-gigabyte reservation. */
    at_start = friend_stats(fr).bytes_committed;
    CHECK(friend_stats(fr).bytes_total == need);
    CHECK(at_start < FR_MB(4));
    CHECK(at_start == committed_bytes);

    /* The graph grows into its region a chunk at a time, and not into
     * the rest of it: sixty-four records, not a hundred thousand slots'
     * worth. */
    for (i = 0; i < 64; ++i) {
        CHECK(add_memory(fr, str_lit("a memory of something"),
                         1000 + (u64)i, i % 32) != FR_SLOT_NONE);
    }
    with_memories = friend_stats(fr).bytes_committed;
    CHECK(friend_memory_count(fr) == 64);
    CHECK(with_memories > at_start);
    CHECK(with_memories - at_start < FR_MB(2));

    /* Every memory reads back as itself, which it would not if anything
     * had relied on a committed page arriving zeroed. */
    for (i = 0; i < 64; ++i) {
        Friend_Memory_Node node;
        CHECK(friend_memory_at(fr, i, &node) == FRIEND_OK);
        CHECK_STR(node.description, "a memory of something");
        CHECK(node.use_count == 0);
    }

    /* A conversation, so the scrollback commits too. */
    for (i = 0; i < 200; ++i) {
        say_and_retire(fr, str_lit("something said out loud"),
                       test_time(0, i));
    }
    CHECK(friend_stats(fr).scrollback == 200);
    CHECK(friend_stats(fr).messages_lost == 0);
    CHECK(friend_stats(fr).bytes_committed - with_memories < FR_MB(1));
    {
        Friend_Message message;
        CHECK(friend_message(fr, 199, &message) == FRIEND_OK);
        CHECK_STR(message.text, "something said out loud");
    }

    /* A machine that will not give any more: the friend runs out of
     * capacity the way it runs out of a fixed limit, with a refusal
     * rather than a page fault. */
    commit_budget = committed_bytes;
    for (i = 0; i < config.limits.max_memories; ++i) {
        if (fr_memory_alloc(fr) == FR_SLOT_NONE) break;
    }
    CHECK(i < config.limits.max_memories);
    CHECK(fr_memory_alloc(fr) == FR_SLOT_NONE);
    /* And the friend is still usable. */
    CHECK(friend_message_count(fr) == 200);
    CHECK(friend_stats(fr).memories == friend_memory_count(fr));

    free(block);
}

/* ================================================================== *
 *  Photographs
 * ================================================================== */

/* A blob file that is not a file. The library asks for reads and writes
 * at offsets; this does them against a byte array, which is every line
 * of the real thing without a filesystem in the test. */
typedef struct Test_Blob {
    u8     bytes[64 * 1024];
    size_t len;
    i32    writes;
    i32    reads;
} Test_Blob;

static void serve_file_io(Friend_Context *fr, Test_Blob *blob,
                          const Friend_Io_Request *request)
{
    size_t at = (size_t)request->offset;

    CHECK_STR(request->path, "photos.blob");
    if (request->kind == FRIEND_IO_FILE_WRITE) {
        CHECK(at + (size_t)request->body.len <= sizeof(blob->bytes));
        memcpy(blob->bytes + at, request->body.data,
               (size_t)request->body.len);
        if (at + (size_t)request->body.len > blob->len) {
            blob->len = at + (size_t)request->body.len;
        }
        blob->writes += 1;
        friend_io_complete(fr, request->id, 0);
        return;
    }
    if (request->kind == FRIEND_IO_FILE_READ) {
        u32 want = request->length < request->recv_capacity
                       ? request->length
                       : request->recv_capacity;
        CHECK(at + (size_t)want <= blob->len);
        memcpy(request->recv, blob->bytes + at, (size_t)want);
        blob->reads += 1;
        friend_io_progress(fr, request->id, want);
        friend_io_complete(fr, request->id, 0);
        return;
    }
    FAIL("not a file request");
}

/* Everything the friend is asking for: files served, model calls
 * answered with `answer` if one is given and failed if not. Returns how
 * many model calls went by. */
static i32 serve_io(Friend_Context *fr, Test_Blob *blob, String answer)
{
    Friend_Io_Request requests[8];
    i32               n = friend_pending_io(fr, requests, 8);
    i32               model_calls = 0;
    i32               i;

    for (i = 0; i < n; ++i) {
        if (requests[i].kind == FRIEND_IO_FILE_READ ||
            requests[i].kind == FRIEND_IO_FILE_WRITE) {
            serve_file_io(fr, blob, &requests[i]);
            continue;
        }
        ++model_calls;
        if (str_is_empty(answer)) {
            friend_io_failed(fr, requests[i].id, str_lit("not in this test"));
        } else {
            answer_request(fr, &requests[i], answer, 200);
        }
    }
    return model_calls;
}

static Friend_Context *make_photo_friend(void **out_block,
                                         Friend_Limits limits)
{
    Friend_Config config = test_config(FRIEND_PROVIDER_GEMINI, limits);
    config.blob_path     = str_lit("photos.blob");
    return make_friend_with(out_block, &config);
}

static void test_photographs(void)
{
    void             *block = 0;
    Friend_Context   *fr    = make_photo_friend(&block, small_limits());
    Test_Blob         blob;
    Friend_Attachment photo;
    Friend_Io_Request requests[8];
    Friend_Message    message;
    Friend_String     bytes, caption;
    char              jpeg[2048];
    i32               i, n;

    if (!fr) {
        free(block);
        return;
    }
    memset(&blob, 0, sizeof(blob));
    /* Not a real JPEG, but it starts like one and every byte of it is
     * checked on the way back. */
    for (i = 0; i < (i32)sizeof(jpeg); ++i) jpeg[i] = (char)(i * 7 + 3);
    jpeg[0] = (char)0xFF;
    jpeg[1] = (char)0xD8;
    photo.jpeg = str_make(jpeg, (i32)sizeof(jpeg));

    friend_tick(fr, TEST_NOW);
    CHECK(friend_say(fr, str_lit("look at this"), &photo, 1) == FRIEND_OK);
    CHECK(friend_message(fr, 0, &message) == FRIEND_OK);
    CHECK(message.attachment_count == 1);
    CHECK(friend_stats(fr).photos == 1);
    CHECK(friend_stats(fr).photos_waiting == 1);

    /* It can be looked at straight away, out of staging, before the
     * disk has heard about it. */
    CHECK(friend_message_attachment(fr, 0, 0, &bytes, &caption) == FRIEND_OK);
    CHECK(bytes.len == (i32)sizeof(jpeg));
    CHECK(memcmp(bytes.data, jpeg, sizeof(jpeg)) == 0);
    CHECK(blob.reads == 0);

    /* Two calls go out with the picture in them: the reply, and the
     * vision model being asked what is in it. */
    n = friend_pending_io(fr, requests, 8);
    CHECK(n >= 1);
    {
        b32 saw_reply = 0, saw_caption = 0;
        for (i = 0; i < n; ++i) {
            if (str_index_of(requests[i].body, str_lit("inlineData")) < 0) {
                continue;
            }
            CHECK(str_index_of(requests[i].body, str_lit("image/jpeg")) >= 0);
            if (str_index_of(requests[i].body, str_lit("Describe")) >= 0) {
                saw_caption = 1;
            } else {
                saw_reply = 1;
            }
        }
        CHECK(saw_reply);
        CHECK(saw_caption);
    }

    /* The caption comes back and lands on the message. */
    for (i = 0; i < n; ++i) {
        if (str_index_of(requests[i].body, str_lit("Describe")) < 0) continue;
        answer_request(fr, &requests[i],
                       gemini_answer(&scratch,
                                     str_lit("1. A steel bridge at sunrise.")),
                       200);
    }
    friend_tick(fr, TEST_NOW + 1000);
    CHECK(friend_message_attachment(fr, 0, 0, &bytes, &caption) == FRIEND_OK);
    CHECK_STR(caption, "A steel bridge at sunrise.");

    /* The pump writes it out, once, to the file it was given. */
    serve_io(fr, &blob, str_nil);
    friend_tick(fr, TEST_NOW + 2000);
    CHECK(blob.writes == 1);
    CHECK(blob.len == sizeof(jpeg));
    CHECK(memcmp(blob.bytes, jpeg, sizeof(jpeg)) == 0);
    CHECK(friend_stats(fr).photos == 1);
    CHECK(friend_stats(fr).photos_waiting == 0);

    /* Writing it out asks for the friend to be saved again: until that
     * happens the file on disk says the photograph is not there. */
    {
        Friend_Event event;
        b32          asked = 0;
        while (friend_next_event(fr, &event)) {
            if (event.kind == FRIEND_EVENT_STATE_DIRTY) asked = 1;
        }
        CHECK(asked);
    }

    /* Once staging has let it go, looking at it is a read, and the read
     * comes back with the same bytes. */
    friend_tick(fr, TEST_NOW + 3000);
    CHECK(friend_message_attachment(fr, 0, 0, &bytes, &caption) ==
          FRIEND_ERR_BAD_STATE);
    serve_io(fr, &blob, str_nil);
    friend_tick(fr, TEST_NOW + 4000);
    CHECK(blob.reads == 1);
    CHECK(friend_message_attachment(fr, 0, 0, &bytes, &caption) == FRIEND_OK);
    CHECK(bytes.len == (i32)sizeof(jpeg));
    CHECK(memcmp(bytes.data, jpeg, sizeof(jpeg)) == 0);

    /* And it survives into the scrollback, descriptor, caption and all. */
    fr_retire_active(fr, fr->active_count);
    CHECK(friend_message(fr, 0, &message) == FRIEND_OK);
    CHECK(message.attachment_count == 1);
    CHECK(friend_message_attachment(fr, 0, 0, &bytes, &caption) == FRIEND_OK);
    CHECK_STR(caption, "A steel bridge at sunrise.");
    CHECK(memcmp(bytes.data, jpeg, sizeof(jpeg)) == 0);

    free(block);
}

/* A friend with nowhere to put pixels keeps the description and drops
 * them, rather than refusing the photograph or pretending it has it. */
static void test_photos_without_a_file(void)
{
    void             *block = 0;
    Friend_Context   *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Attachment photo;
    Friend_Io_Request requests[8];
    Friend_String     bytes, caption;
    Friend_Message    message;
    char              jpeg[64];
    i32               n, i;

    if (!fr) {
        free(block);
        return;
    }
    memset(jpeg, 0x55, sizeof(jpeg));
    photo.jpeg = str_make(jpeg, (i32)sizeof(jpeg));

    friend_tick(fr, TEST_NOW);
    CHECK(friend_say(fr, str_lit("look"), &photo, 1) == FRIEND_OK);
    CHECK(friend_message(fr, 0, &message) == FRIEND_OK);
    CHECK(message.attachment_count == 1);
    CHECK(friend_stats(fr).photos == 0);
    CHECK(friend_message_attachment(fr, 0, 0, &bytes, &caption) ==
          FRIEND_ERR_BAD_DATA);
    /* Nothing is ever asked of the disk. */
    n = friend_pending_io(fr, requests, 8);
    for (i = 0; i < n; ++i) {
        CHECK(requests[i].kind == FRIEND_IO_HTTPS);
        friend_io_failed(fr, requests[i].id, str_lit("not in this test"));
    }
    friend_tick(fr, TEST_NOW + 1000);
    n = friend_pending_io(fr, requests, 8);
    for (i = 0; i < n; ++i) CHECK(requests[i].kind == FRIEND_IO_HTTPS);

    free(block);
}

/* ================================================================== *
 *  Voice
 * ================================================================== */

/* A triangle wave at about 200 Hz: loud, periodic, and nothing a speech
 * check can mistake for a quiet room. */
static void fill_voice(i16 *out, i32 count, i32 amplitude)
{
    i32 i;
    for (i = 0; i < count; ++i) {
        i32 phase = i % 80;
        i32 value = phase < 40 ? phase : 80 - phase;
        out[i]    = (i16)(((value - 20) * amplitude) / 20);
    }
}

/* Half a second of somebody talking, and 700 ms of the quiet after it,
 * which is past the 600 ms endpoint: together, one spoken turn. Filled
 * once, by main. */
static i16 voice_speech[FRIEND_VOICE_RATE / 2];
static i16 voice_quiet[FRIEND_VOICE_RATE * 7 / 10];

static void push_speech(Friend_Context *fr)
{
    CHECK(friend_voice_push(fr, voice_speech, fr_array_count(voice_speech),
                            FRIEND_VOICE_RATE) == FRIEND_OK);
}

static void push_quiet(Friend_Context *fr)
{
    CHECK(friend_voice_push(fr, voice_quiet, fr_array_count(voice_quiet),
                            FRIEND_VOICE_RATE) == FRIEND_OK);
}

static b32 one_of(String got, const char *const *list, i32 count)
{
    i32 i;
    for (i = 0; i < count; ++i) {
        if (str_eq(got, str_from_cstr(list[i]))) return 1;
    }
    return 0;
}

/* The extraction prompt is told what was in a photo, not that there was
 * no caption, when there is one. */
static void test_extract_sees_captions(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Fr_Chat_Event   e;
    String          captions[1];
    String          text;

    if (!fr) {
        free(block);
        return;
    }
    memset(&e, 0, sizeof(e));
    e.kind        = FRIEND_MESSAGE_USER;
    e.image_count = 1;
    captions[0]   = str_lit("An orange cat on a counter");
    fr_set_captions(&e, captions, 1);

    scratch_reset();
    text = fr_build_extract_conversation(fr, &scratch, &e, 1);
    CHECK(str_index_of(text, str_lit("An orange cat on a counter")) >= 0);
    CHECK(str_index_of(text, str_lit("no caption available")) < 0);

    e.captions_len = 0;
    scratch_reset();
    text = fr_build_extract_conversation(fr, &scratch, &e, 1);
    CHECK(str_index_of(text, str_lit("(no caption available)")) >= 0);
    free(block);
}

static void test_voice_names(void)
{
    void           *block = 0;
    Friend_Context *fr = make_friend(&block, FRIEND_PROVIDER_OPENAI_COMPATIBLE);

    if (!fr) {
        free(block);
        return;
    }

    /* A name the service's own generator could produce resolves to a
     * voice of the same gender, and to the same one every time. */
    CHECK(one_of(fr_voice_for_name(fr, str_lit("Echo")),
                 fr_openai_female_voices,
                 fr_array_count(fr_openai_female_voices)));
    CHECK(str_eq(fr_voice_for_name(fr, str_lit("Echo")),
                 fr_voice_for_name(fr, str_lit("Echo"))));
    CHECK(one_of(fr_voice_for_name(fr, str_lit("Magnus")),
                 fr_openai_male_voices, fr_array_count(fr_openai_male_voices)));
    /* A common name that is not in the pools falls through to the
     * everyday lists. */
    CHECK(one_of(fr_voice_for_name(fr, str_lit("Elizabeth")),
                 fr_openai_female_voices,
                 fr_array_count(fr_openai_female_voices)));
    CHECK(one_of(fr_voice_for_name(fr, str_lit("dr. james watson")),
                 fr_openai_male_voices, fr_array_count(fr_openai_male_voices)));

    use_provider(fr, FRIEND_PROVIDER_GEMINI);
    CHECK(one_of(fr_voice_for_name(fr, str_lit("Echo")), fr_gemini_voices,
                 fr_array_count(fr_gemini_voices)));

    free(block);
}

/* The voice a frontend is told about is the one the friend speaks in:
 * chosen from its name unless the configuration names one, so a friend
 * keeps its voice across restarts without anybody writing it down. */
static void test_which_voice(void)
{
    Friend_Config   config = test_config(FRIEND_PROVIDER_GEMINI,
                                         small_limits());
    void           *block  = 0;
    Friend_Context *fr     = make_friend_with(&block, &config);
    String          chosen;

    if (!fr) {
        free(block);
        return;
    }
    chosen = friend_voice(fr);
    CHECK(one_of(chosen, fr_gemini_voices, fr_array_count(fr_gemini_voices)));

    config.voice = str_lit("Kore");
    CHECK(friend_reconfigure(fr, &config) == FRIEND_OK);
    CHECK_STR(friend_voice(fr), "Kore");

    config.voice = str_nil;
    CHECK(friend_reconfigure(fr, &config) == FRIEND_OK);
    CHECK(str_eq(friend_voice(fr), chosen));

    free(block);
}

static void test_voice_hears_nothing(void)
{
    void             *block = 0;
    Friend_Context   *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request requests[4];
    Friend_Event      event;
    i32               i, errors = 0;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);

    /* Hands-free, a quiet room is not a conversation: five seconds of
     * it, longer than this friend can hear in one go, and nothing
     * happens. */
    CHECK(friend_voice_open(fr) == FRIEND_OK);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_LISTENING);
    for (i = 0; i < 7; ++i) push_quiet(fr);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_LISTENING);
    CHECK(friend_pending_io(fr, requests, 4) == 0);
    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_ERROR) ++errors;
    }
    CHECK(errors == 0);

    /* And the silence was not kept: when somebody does speak, what goes
     * to be transcribed is their words and the pre-roll before them,
     * under two seconds of audio even as base64, not five. */
    push_speech(fr);
    push_quiet(fr);
    CHECK(friend_pending_io(fr, requests, 4) == 1);
    CHECK(requests[0].body.len < 2 * FRIEND_VOICE_RATE * 2 * 4 / 3);
    friend_io_failed(fr, requests[0].id, str_lit("not in this test"));
    friend_tick(fr, TEST_NOW + 1000);
    CHECK(friend_voice_close(fr) == FRIEND_OK);

    /* A button held down over silence says so, because somebody meant
     * to say something. */
    while (friend_next_event(fr, &event)) {}
    CHECK(friend_voice_begin(fr) == FRIEND_OK);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_HEARING);
    push_quiet(fr);
    CHECK(friend_voice_end(fr) == FRIEND_OK);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_OFF);
    CHECK(friend_pending_io(fr, requests, 4) == 0);
    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_ERROR) ++errors;
    }
    CHECK(errors == 1);
    CHECK(friend_voice_end(fr) == FRIEND_ERR_BAD_STATE);

    free(block);
}

/* A spoken turn end to end on the OpenAI shape: a multipart WAV up, a
 * transcript back, the reply spoken a sentence at a time, and the raw
 * samples handed on as they arrive. */
static void test_voice_turn(void)
{
    void             *block = 0;
    Friend_Context   *fr = make_friend(&block, FRIEND_PROVIDER_OPENAI_COMPATIBLE);
    Friend_Io_Request requests[8];
    Friend_Event      event;
    Friend_Message    message;
    i32               n, i;
    i32               audio_samples = 0;
    b32               transcript    = 0;
    u8                pcm[4800];

    if (!fr) {
        free(block);
        return;
    }
    for (i = 0; i < (i32)sizeof(pcm); ++i) pcm[i] = (u8)(i * 3 + 1);

    friend_tick(fr, TEST_NOW);
    CHECK(friend_voice_open(fr) == FRIEND_OK);

    /* Somebody talking. */
    push_speech(fr);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_HEARING);
    CHECK(friend_voice_level(fr) > 0.1f);

    /* Then they stop, and the end of the sentence ends the turn. */
    push_quiet(fr);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_WORKING);

    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n != 1) {
        free(block);
        return;
    }
    CHECK(str_index_of(requests[0].url, str_lit("/audio/transcriptions")) >= 0);
    CHECK(str_index_of(requests[0].headers,
                       str_lit("multipart/form-data")) >= 0);
    CHECK(str_index_of(requests[0].body,
                       friend_default_models(FRIEND_PROVIDER_OPENAI_COMPATIBLE)
                           .transcribe) >= 0);
    /* A WAV file, with the silence after the last word mostly cut. */
    CHECK(str_index_of(requests[0].body, str_lit("RIFF")) >= 0);
    CHECK(str_index_of(requests[0].body, str_lit("WAVEfmt ")) >= 0);
    CHECK(requests[0].body.len > 20000);
    CHECK(requests[0].body.len < 34000);

    /* The models do not say "no speech detected" back the way they were
     * asked to, and a turn that heard nothing is not a message. */
    answer_request(fr, &requests[0],
                   str_lit("{\"text\":\"No speech detected.\"}"), 200);
    friend_tick(fr, TEST_NOW + 500);
    CHECK(friend_message_count(fr) == 0);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_LISTENING);

    /* Say it again, with something in it this time. */
    push_speech(fr);
    push_quiet(fr);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n != 1) {
        free(block);
        return;
    }
    answer_request(fr, &requests[0], str_lit("{\"text\":\"are you there\"}"),
                   200);
    friend_tick(fr, TEST_NOW + 1000);

    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_TRANSCRIPT) {
            transcript = 1;
            CHECK_STR(event.text, "are you there");
        }
    }
    CHECK(transcript);

    /* What was said is a message like any other, and the friend is told
     * it was heard rather than read. */
    CHECK(friend_message_count(fr) == 2);
    CHECK(friend_message(fr, 0, &message) == FRIEND_OK);
    CHECK(message.kind == FRIEND_MESSAGE_USER);
    CHECK_STR(message.text, "are you there");
    CHECK(message.was_spoken);

    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    CHECK(str_index_of(requests[0].url, str_lit("/chat/completions")) >= 0);

    /* The reply arrives whole, in one event, and the first sentence of
     * it goes off to be spoken without waiting for the rest. */
    answer_request(fr, &requests[0],
                   str_lit("data: {\"choices\":[{\"delta\":{\"content\":"
                           "\"Yeah, I'm right here. \"}}]}\n"
                           "data: [DONE]\n"),
                   200);
    friend_tick(fr, TEST_NOW + 2000);

    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n != 1) {
        free(block);
        return;
    }
    CHECK(str_index_of(requests[0].url, str_lit("/audio/speech")) >= 0);
    CHECK(str_index_of(requests[0].body,
                       str_lit("Yeah, I'm right here.")) >= 0);
    CHECK(str_index_of(requests[0].body, str_lit("\"response_format\":\"pcm\""))
          >= 0);
    CHECK(str_index_of(requests[0].body, str_lit("like a close friend")) >= 0);
    CHECK(str_index_of(requests[0].body, friend_voice(fr)) >= 0);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_SPEAKING);
    CHECK(requests[0].flags & FRIEND_IO_FLAG_STREAM);

    /* The samples are handed on as they arrive rather than at the end of
     * the request, so the friend starts talking while the rest of the
     * sentence is still coming down the wire. OpenAI's pcm is 24 kHz. */
    memcpy(requests[0].recv, pcm, sizeof(pcm) / 2);
    friend_io_progress(fr, requests[0].id, (u32)(sizeof(pcm) / 2));
    friend_tick(fr, TEST_NOW + 2100);
    while (friend_next_event(fr, &event)) {
        if (event.kind != FRIEND_EVENT_AUDIO_OUT) continue;
        CHECK(event.sample_rate == 24000);
        CHECK(event.samples != 0);
        audio_samples += event.sample_count;
    }
    CHECK(audio_samples == (i32)(sizeof(pcm) / 4));

    /* While it is talking the microphone is hearing the friend, not the
     * room, so what it picks up is thrown away. */
    push_speech(fr);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_SPEAKING);
    CHECK(friend_voice_level(fr) == 0.0f);

    memcpy(requests[0].recv + sizeof(pcm) / 2, pcm + sizeof(pcm) / 2,
           sizeof(pcm) / 2);
    friend_io_progress(fr, requests[0].id, (u32)(sizeof(pcm) / 2));
    friend_io_complete(fr, requests[0].id, 200);
    friend_tick(fr, TEST_NOW + 2200);
    while (friend_next_event(fr, &event)) {
        if (event.kind != FRIEND_EVENT_AUDIO_OUT) continue;
        audio_samples += event.sample_count;
    }
    CHECK(audio_samples == (i32)(sizeof(pcm) / 2));

    /* It is still talking until the speaker has caught up with what it
     * was given, and then it is listening again. */
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_SPEAKING);
    CHECK(friend_next_deadline_ms(fr) == 20u);
    friend_tick(fr, TEST_NOW + 9000);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_LISTENING);

    free(block);
}

/* The same turn on Gemini, where the audio goes up inside the JSON and
 * comes back the same way. */
static void test_voice_turn_gemini(void)
{
    void             *block  = 0;
    Friend_Context   *fr     = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Models     models = friend_default_models(FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request requests[8];
    Friend_Event      event;
    i32               n, i;
    i32               audio_samples = 0;
    i16               pcm[1200];

    if (!fr) {
        free(block);
        return;
    }
    for (i = 0; i < (i32)(sizeof(pcm) / sizeof(pcm[0])); ++i) {
        pcm[i] = (i16)(i * 11 - 3000);
    }

    friend_tick(fr, TEST_NOW);
    CHECK(friend_voice_open(fr) == FRIEND_OK);
    push_speech(fr);
    push_quiet(fr);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_WORKING);

    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n != 1) {
        free(block);
        return;
    }
    CHECK(str_index_of(requests[0].url, str_lit(":generateContent")) >= 0);
    CHECK(str_index_of(requests[0].url, models.transcribe) >= 0);
    CHECK(str_index_of(requests[0].body, str_lit("\"audio/wav\"")) >= 0);
    CHECK(str_index_of(requests[0].body, str_lit("inlineData")) >= 0);
    /* The service's own transcription prompt, which it wrote for this
     * provider and left switched off. */
    CHECK(str_index_of(requests[0].body,
                       str_lit("small microphone attached to a person")) >= 0);
    /* The WAV went up as base64, which is a third bigger than the
     * samples and begins the way a RIFF file does. */
    CHECK(str_index_of(requests[0].body, str_lit("UklGR")) >= 0);

    answer_request(fr, &requests[0],
                   gemini_answer(&scratch, str_lit("are you there")), 200);
    friend_tick(fr, TEST_NOW + 1000);
    while (friend_next_event(fr, &event)) {}

    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    CHECK(str_index_of(requests[0].url, str_lit("streamGenerateContent")) >= 0);
    answer_request(fr, &requests[0],
                   str_lit("data: {\"candidates\":[{\"content\":{\"parts\":[{"
                           "\"text\":\"Yeah, I'm right here. \"}]},"
                           "\"finishReason\":\"STOP\"}]}\n"),
                   200);
    friend_tick(fr, TEST_NOW + 2000);

    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n != 1) {
        free(block);
        return;
    }
    CHECK(str_index_of(requests[0].url, models.speak) >= 0);
    CHECK(str_index_of(requests[0].body, str_lit("\"AUDIO\"")) >= 0);
    CHECK(str_index_of(requests[0].body, str_lit("prebuiltVoiceConfig")) >= 0);
    CHECK(str_index_of(requests[0].body, friend_voice(fr)) >= 0);
    /* Gemini takes its direction in the prompt, as an instruction with
     * the line to read after a colon. */
    CHECK(str_index_of(requests[0].body, str_lit("Read this aloud")) >= 0);
    CHECK(str_index_of(requests[0].body,
                       str_lit("easy pace: Yeah, I'm right here.")) >= 0);
    /* And it answers with one document rather than a stream of them. */
    CHECK((requests[0].flags & FRIEND_IO_FLAG_STREAM) == 0);

    /* The samples come back inside it, base64, and are handed on
     * decoded. */
    {
        String_Builder sb   = sb_begin(&scratch);
        size_t         size = fr_base64_encoded_size(sizeof(pcm));
        char          *dest;
        sb_append(&sb, str_lit("{\"candidates\":[{\"content\":{\"parts\":[{"
                               "\"inlineData\":{\"mimeType\":\"audio/L16;"
                               "codec=pcm;rate=24000\",\"data\":\""));
        dest = (char *)arena_push_size_nz(&scratch, size, 1);
        CHECK(dest != 0);
        if (dest) {
            fr_base64_encode((const u8 *)pcm, sizeof(pcm), 0, 0, dest);
            sb.len += (i32)size;
        }
        sb_append(&sb, str_lit("\"}}]}}]}"));
        answer_request(fr, &requests[0], sb_done(&sb), 200);
    }
    friend_tick(fr, TEST_NOW + 2100);
    while (friend_next_event(fr, &event)) {
        if (event.kind != FRIEND_EVENT_AUDIO_OUT) continue;
        CHECK(event.sample_rate == 24000);
        if (event.sample_count == (i32)(sizeof(pcm) / sizeof(pcm[0]))) {
            CHECK(memcmp(event.samples, pcm, sizeof(pcm)) == 0);
        }
        audio_samples += event.sample_count;
    }
    CHECK(audio_samples == (i32)(sizeof(pcm) / sizeof(pcm[0])));

    free(block);
}

static Friend_Context *make_speaking_friend(void **out_block)
{
    Friend_Config config = test_config(FRIEND_PROVIDER_OPENAI_COMPATIBLE,
                                       small_limits());
    config.speak_replies = 1;
    return make_friend_with(out_block, &config);
}

/* The reply is cut into pieces the way the service cut them: the first
 * one early so the friend starts talking, the rest batched; stage
 * directions in brackets are not read out; a sentence end inside a
 * number is not a sentence end. */
static void test_voice_sentences(void)
{
    void             *block = 0;
    Friend_Context   *fr    = make_speaking_friend(&block);
    Friend_Io_Request requests[8];
    i32               n;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);

    /* Typed, not spoken, and spoken back anyway because the
     * configuration asked for it. */
    CHECK(friend_say(fr, str_lit("hello"), 0, 0) == FRIEND_OK);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n != 1) {
        free(block);
        return;
    }

    /* Three pieces of a reply, arriving as the provider felt like
     * sending them. The first sentence is nineteen characters, one short
     * of the length worth speaking on its own, so it waits for the
     * second. */
    answer_request(fr, &requests[0],
                   str_lit("data: {\"choices\":[{\"delta\":{\"content\":"
                           "\"Oh no. It was 3.5 hours [sighs] and I "
                           "waited.\"}}]}\n"
                           "data: {\"choices\":[{\"delta\":{\"content\":"
                           "\" Anyway. \"}}]}\n"
                           "data: [DONE]\n"),
                   200);
    friend_tick(fr, TEST_NOW + 1000);

    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n == 1) {
        /* "3.5" did not split it, the stage direction is gone, and the
         * doubled space it would have left is gone with it. */
        CHECK(str_index_of(requests[0].body,
                           str_lit("Oh no. It was 3.5 hours and I waited."))
              >= 0);
        CHECK(str_index_of(requests[0].body, str_lit("sighs")) < 0);
        CHECK(str_index_of(requests[0].body, str_lit("Anyway")) < 0);
        /* One request at a time: the next sentence is asked for when
         * this one is done, not alongside it. */
        answer_request(fr, &requests[0], str_lit("\x01\x02\x03\x04"), 200);
    }
    friend_tick(fr, TEST_NOW + 2000);

    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(str_index_of(requests[0].body, str_lit("Anyway.")) >= 0);
        answer_request(fr, &requests[0], str_lit("\x01\x02\x03\x04"), 200);
    }
    friend_tick(fr, TEST_NOW + 3000);

    /* Nothing is left to say. */
    CHECK(friend_pending_io(fr, requests, 8) == 0);
    free(block);
}

/* Interrupting the friend: the reply stops, what was said is remembered
 * as having been cut off, and the audio that was on its way is dropped.
 */
static void test_voice_interrupted(void)
{
    void             *block = 0;
    Friend_Context   *fr    = make_speaking_friend(&block);
    Friend_Io_Request requests[8];
    Friend_Message    message;
    i32               n;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);
    CHECK(friend_say(fr, str_lit("hello"), 0, 0) == FRIEND_OK);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n != 1) {
        free(block);
        return;
    }
    answer_request(fr, &requests[0],
                   str_lit("data: {\"choices\":[{\"delta\":{\"content\":"
                           "\"Yeah, I'm right here. \"}}]}\n"
                           "data: [DONE]\n"),
                   200);
    friend_tick(fr, TEST_NOW + 1000);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_SPEAKING);

    CHECK(friend_retract_reply(fr, 0.25f) == FRIEND_OK);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_OFF);
    /* The request that was out is one the friend no longer wants. */
    {
        u64 abandoned[8];
        CHECK(friend_abandoned_io(fr, abandoned, 8) >= 1);
    }
    CHECK(friend_message(fr, 1, &message) == FRIEND_OK);
    CHECK(message.kind == FRIEND_MESSAGE_FRIEND);

    /* Whatever arrives for it now is not played. */
    answer_request(fr, &requests[0], str_lit("\x01\x02\x03\x04"), 200);
    friend_tick(fr, TEST_NOW + 2000);
    {
        Friend_Event event;
        b32          audio = 0;
        while (friend_next_event(fr, &event)) {
            if (event.kind == FRIEND_EVENT_AUDIO_OUT) audio = 1;
        }
        CHECK(!audio);
    }

    free(block);
}

/* Saying a message again, and the microphone at a rate the device chose
 * rather than the one the transcriber wants. */
static void test_voice_resample_and_repeat(void)
{
    static i16        speech[48000];
    void             *block = 0;
    Friend_Context   *fr = make_friend(&block, FRIEND_PROVIDER_OPENAI_COMPATIBLE);
    Friend_Io_Request requests[8];
    Friend_Event      event;
    i32               n, i;

    if (!fr) {
        free(block);
        return;
    }
    fill_voice(speech, fr_array_count(speech), 6000);
    friend_tick(fr, TEST_NOW);

    /* A second of 48 kHz audio goes up as a second of 16 kHz audio: 32
     * KB of samples, not three times that. */
    CHECK(friend_voice_begin(fr) == FRIEND_OK);
    CHECK(friend_voice_push(fr, speech, fr_array_count(speech), 48000) ==
          FRIEND_OK);
    CHECK(friend_voice_end(fr) == FRIEND_OK);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(requests[0].body.len > 30000);
        CHECK(requests[0].body.len < 36000);
        friend_io_failed(fr, requests[0].id, str_lit("enough"));
    }
    friend_tick(fr, TEST_NOW + 1000);
    n = friend_pending_io(fr, requests, 8);
    for (i = 0; i < n; ++i) {
        friend_io_failed(fr, requests[i].id, str_lit("not in this test"));
    }
    friend_tick(fr, TEST_NOW + 1100);

    /* And a message the friend has already sent can be said again. */
    while (friend_next_event(fr, &event)) {}
    CHECK(friend_say(fr, str_lit("hello"), 0, 0) == FRIEND_OK);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n == 1) {
        answer_request(fr, &requests[0],
                       str_lit("data: {\"choices\":[{\"delta\":{\"content\":"
                               "\"I am here, you know.\"}}]}\n"
                               "data: [DONE]\n"),
                       200);
    }
    friend_tick(fr, TEST_NOW + 2000);
    /* Nothing was spoken: it was typed, and this friend was not asked to
     * speak everything. */
    CHECK(friend_pending_io(fr, requests, 8) == 0);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_OFF);

    CHECK(friend_speak(fr, 0) == FRIEND_ERR_BAD_ARGUMENT); /* the user's */
    CHECK(friend_speak(fr, 1) == FRIEND_OK);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_SPEAKING);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(str_index_of(requests[0].body,
                           str_lit("I am here, you know.")) >= 0);
    }

    free(block);
}

/* ================================================================== *
 *  The realtime session
 * ================================================================== */

/* Hands the friend one message off a websocket it thinks it opened. */
static void ws_say(Friend_Context *fr, u64 channel, const char *json)
{
    friend_ws_recv(fr, channel, str_from_cstr(json), 0);
}

/* The requests waiting to go out, as one string, so a test can ask
 * whether a thing was said without caring how many frames it took. */
static String ws_drain(Friend_Context *fr, Arena *arena, i32 *out_count)
{
    Friend_Io_Request requests[16];
    String_Builder    sb = sb_begin(arena);
    i32               n  = friend_pending_io(fr, requests, 16);
    i32               i;

    for (i = 0; i < n; ++i) {
        sb_append(&sb, requests[i].body);
        sb_append_char(&sb, '\n');
        friend_io_complete(fr, requests[i].id, 0);
    }
    if (out_count) *out_count = n;
    return sb_done(&sb);
}

static Friend_Context *make_live_friend(void **block, Friend_Provider which)
{
    Friend_Config config = test_config(which, small_limits());

    /* A session holds one slot for the socket for as long as it is
     * open, and wants a few more for the audio going out. */
    config.limits.max_inflight_io = 8;
    config.voice_mode             = FRIEND_VOICE_REALTIME;
    if (which == FRIEND_PROVIDER_OPENAI_COMPATIBLE) {
        config.models.live       = str_lit("gpt-realtime-2.1");
        config.models.transcribe = str_lit("whisper-1");
    }
    return make_friend_with(block, &config);
}

/* A whole turn on the Gemini shape: the socket, the setup, the recap,
 * the audio out, the transcripts back, and two messages in the
 * conversation at the end of it. */
static void test_live_session(void)
{
    void             *block = 0;
    Friend_Context   *fr = make_live_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request requests[8];
    Friend_Event      event;
    Friend_Message    message;
    String            sent;
    u64               channel;
    i32               n;
    i32               audio = 0;
    i32               done  = -1;
    b32               transcript = 0;

    if (!fr) {
        free(block);
        return;
    }

    friend_tick(fr, TEST_NOW);
    CHECK(friend_voice_open(fr) == FRIEND_OK);
    /* Not listening yet: there is a socket to open first, and audio
     * pushed before it is open would go nowhere. */
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_WORKING);

    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n != 1) {
        free(block);
        return;
    }
    CHECK(requests[0].kind == FRIEND_IO_WS_OPEN);
    CHECK(str_index_of(requests[0].url, str_lit("wss://")) == 0);
    CHECK(str_index_of(requests[0].url,
                       str_lit("BidiGenerateContent")) > 0);
    /* The key goes in a header here too, not in the query string every
     * example for this endpoint uses. */
    CHECK(str_index_of(requests[0].headers, str_lit("x-goog-api-key")) >= 0);
    CHECK(str_index_of(requests[0].url, str_lit("key=")) < 0);
    channel = requests[0].id;

    /* The socket is up: the setup goes out, with the live prompt in
     * it. */
    friend_ws_recv(fr, channel, str_nil, FRIEND_IO_FLAG_OPENED);
    sent = ws_drain(fr, &scratch, &n);
    CHECK(n == 1);
    CHECK(str_index_of(sent, str_lit("\"setup\"")) >= 0);
    CHECK(str_index_of(sent, str_lit("models/gemini")) >= 0);
    CHECK(str_index_of(sent, str_lit("\"AUDIO\"")) >= 0);
    /* Its own speech detection off, both transcripts on, and no
     * thinking before it answers. */
    CHECK(str_index_of(sent, str_lit("\"disabled\":true")) >= 0);
    CHECK(str_index_of(sent, str_lit("inputAudioTranscription")) >= 0);
    CHECK(str_index_of(sent, str_lit("outputAudioTranscription")) >= 0);
    CHECK(str_index_of(sent, str_lit("\"thinkingBudget\":0")) >= 0);
    /* The live prompt, not the chat one: it is the one with the
     * examples of how a turn should sound. */
    CHECK(str_index_of(sent, str_lit("<Not An Advertisement>")) >= 0);
    CHECK(str_index_of(sent, str_lit("The current time is")) >= 0);

    /* Setup acknowledged: the conversation so far is seeded, and now it
     * is listening. */
    ws_say(fr, channel, "{\"setupComplete\":{}}");
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_LISTENING);
    sent = ws_drain(fr, &scratch, &n);
    CHECK(n == 0); /* nothing to recap: the conversation is empty */

    /* Somebody talks. The audio goes out as it arrives rather than at
     * the end, which is the whole point of the socket. */
    push_speech(fr);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_HEARING);
    friend_tick(fr, TEST_NOW + 1000);
    sent = ws_drain(fr, &scratch, &n);
    CHECK(n >= 2);
    CHECK(str_index_of(sent, str_lit("activityStart")) >= 0);
    CHECK(str_index_of(sent, str_lit("audio/pcm;rate=16000")) >= 0);

    /* And stops. */
    push_quiet(fr);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_WORKING);
    sent = ws_drain(fr, &scratch, &n);
    CHECK(str_index_of(sent, str_lit("activityEnd")) >= 0);

    /* What it heard, what it says, and the sound of it. */
    ws_say(fr, channel,
           "{\"serverContent\":{\"inputTranscription\":{\"text\":\"are you "
           "there\"}}}");
    ws_say(fr, channel,
           "{\"serverContent\":{\"outputTranscription\":{\"text\":\"Yeah, "
           "I'm right here.\"}}}");
    ws_say(fr, channel,
           "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"inlineData\":{"
           "\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\"AQIDBAUGBwg=\""
           "}}]}}}");
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_SPEAKING);
    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_AUDIO_OUT) {
            CHECK(event.sample_rate == 24000);
            audio += event.sample_count;
        }
    }
    CHECK(audio == 4);

    /* The turn ends, and it is a turn like any other: two messages in
     * the conversation, the first marked as spoken. */
    ws_say(fr, channel, "{\"serverContent\":{\"turnComplete\":true}}");
    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_TRANSCRIPT) transcript = 1;
        if (event.kind == FRIEND_EVENT_REPLY_DONE) done = event.index;
    }
    CHECK(transcript);
    CHECK(friend_message_count(fr) == 2);
    CHECK(done == 1);
    /* A reply, counted against the epoch like any other. */
    CHECK(friend_stats(fr).replies_total == 1);
    CHECK(friend_stats(fr).epoch_samples == 1);
    CHECK(friend_message(fr, 0, &message) == FRIEND_OK);
    CHECK(message.kind == FRIEND_MESSAGE_USER);
    CHECK_STR(message.text, "are you there");
    CHECK(message.was_spoken);
    CHECK(friend_message(fr, 1, &message) == FRIEND_OK);
    CHECK(message.kind == FRIEND_MESSAGE_FRIEND);
    CHECK_STR(message.text, "Yeah, I'm right here.");
    /* Still talking until the speaker has played what it was sent —
     * the microphone hearing the friend must not be a new turn — and
     * listening once it has. */
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_SPEAKING);
    friend_tick(fr, TEST_NOW + 1600);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_LISTENING);

    /* And the next session carries the conversation in with it. */
    friend_voice_close(fr);
    friend_tick(fr, TEST_NOW + 2000);
    (void)ws_drain(fr, &scratch, &n);
    CHECK(friend_voice_open(fr) == FRIEND_OK);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n == 1) {
        friend_ws_recv(fr, requests[0].id, str_nil, FRIEND_IO_FLAG_OPENED);
        channel = requests[0].id;
        sent    = ws_drain(fr, &scratch, &n);
        ws_say(fr, channel, "{\"setupComplete\":{}}");
        sent = ws_drain(fr, &scratch, &n);
        CHECK(n == 1);
        CHECK(str_index_of(sent, str_lit("clientContent")) >= 0);
        CHECK(str_index_of(sent, str_lit("[user said out loud] are you "
                                         "there")) >= 0);
        CHECK(str_index_of(sent, str_lit("[you previously responded] Yeah")) >=
              0);
        /* History, not a question. */
        CHECK(str_index_of(sent, str_lit("\"turnComplete\":false")) >= 0);
    }

    free(block);
}

/* The same turn on the OpenAI shape, where every event has a type and
 * the turn has to be committed and asked for by hand. */
static void test_live_session_openai(void)
{
    void             *block = 0;
    Friend_Context   *fr =
        make_live_friend(&block, FRIEND_PROVIDER_OPENAI_COMPATIBLE);
    Friend_Io_Request requests[8];
    Friend_Event      event;
    Friend_Message    message;
    String            sent;
    u64               channel;
    i32               n;
    i32               audio = 0;

    if (!fr) {
        free(block);
        return;
    }

    friend_tick(fr, TEST_NOW);
    CHECK(friend_voice_open(fr) == FRIEND_OK);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n != 1) {
        free(block);
        return;
    }
    CHECK(str_index_of(requests[0].url,
                       str_lit("wss://api.openai.com/v1/realtime?model="
                               "gpt-realtime-2.1")) == 0);
    CHECK(str_index_of(requests[0].headers, str_lit("Authorization: Bearer"))
          >= 0);
    channel = requests[0].id;

    friend_ws_recv(fr, channel, str_nil, FRIEND_IO_FLAG_OPENED);
    sent = ws_drain(fr, &scratch, &n);
    CHECK(n == 1);
    CHECK(str_index_of(sent, str_lit("\"type\":\"session.update\"")) >= 0);
    CHECK(str_index_of(sent, str_lit("\"turn_detection\":null")) >= 0);
    CHECK(str_index_of(sent, str_lit("whisper-1")) >= 0);
    CHECK(str_index_of(sent, str_lit("\"rate\":24000")) >= 0);

    ws_say(fr, channel, "{\"type\":\"session.updated\"}");
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_LISTENING);
    (void)ws_drain(fr, &scratch, &n);

    push_speech(fr);
    friend_tick(fr, TEST_NOW + 1000);
    sent = ws_drain(fr, &scratch, &n);
    CHECK(str_index_of(sent, str_lit("input_audio_buffer.append")) >= 0);

    push_quiet(fr);
    sent = ws_drain(fr, &scratch, &n);
    CHECK(str_index_of(sent, str_lit("input_audio_buffer.commit")) >= 0);
    CHECK(str_index_of(sent, str_lit("response.create")) >= 0);

    ws_say(fr, channel,
           "{\"type\":\"conversation.item.input_audio_transcription."
           "completed\",\"transcript\":\"are you there\"}");
    ws_say(fr, channel,
           "{\"type\":\"response.output_audio_transcript.delta\",\"delta\":"
           "\"Yeah, I'm right here.\"}");
    ws_say(fr, channel,
           "{\"type\":\"response.output_audio.delta\",\"delta\":"
           "\"AQIDBAUGBwg=\"}");
    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_AUDIO_OUT) audio += event.sample_count;
    }
    CHECK(audio == 4);

    /* A cancelled response is an interruption, not a turn: nothing is
     * written into the conversation for it. */
    ws_say(fr, channel,
           "{\"type\":\"response.done\",\"response\":{\"status\":"
           "\"cancelled\"}}");
    CHECK(friend_message_count(fr) == 0);

    ws_say(fr, channel,
           "{\"type\":\"response.output_audio_transcript.delta\",\"delta\":"
           "\"Still here.\"}");
    ws_say(fr, channel,
           "{\"type\":\"response.done\",\"response\":{\"status\":"
           "\"completed\"}}");
    CHECK(friend_message_count(fr) == 2);
    CHECK(friend_message(fr, 1, &message) == FRIEND_OK);
    CHECK_STR(message.text, "Still here.");

    free(block);
}

/* A session that dies says so, and what was said before it died is
 * still what was said. */
static void test_live_session_closes(void)
{
    void             *block = 0;
    Friend_Context   *fr = make_live_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request requests[8];
    Friend_Event      event;
    u64               channel;
    i32               n;
    b32               complained = 0;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);
    CHECK(friend_voice_open(fr) == FRIEND_OK);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n != 1) {
        free(block);
        return;
    }
    channel = requests[0].id;

    /* The socket is refused: the open request fails, which is how the
     * transport says a session is over. */
    friend_ws_closed(fr, channel);
    friend_io_failed(fr, channel, str_lit("the server closed the websocket "
                                          "(1007): invalid argument"));
    friend_tick(fr, TEST_NOW + 1000);
    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_ERROR) {
            complained = 1;
            CHECK(str_index_of(event.text, str_lit("1007")) >= 0);
        }
    }
    CHECK(complained);

    /* And the microphone falls back to the other way of talking, which
     * is what the service did when a live session would not open. */
    CHECK(friend_voice_state(fr) != FRIEND_VOICE_OFF);
    push_speech(fr);
    push_quiet(fr);
    n = friend_pending_io(fr, requests, 8);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(requests[0].kind == FRIEND_IO_HTTPS);
        CHECK(str_index_of(requests[0].body, str_lit("audio/wav")) >= 0);
    }

    free(block);
}

/* The export's photographs, through the same path as a message's:
 * decoded out of the document into staging, written once, and then only
 * in the file. */
static void test_import_photo_pixels(void)
{
    void                *block = 0;
    Friend_Context      *fr    = make_photo_friend(&block, export_limits());
    Friend_Import_Report report;
    Friend_Memory_Node   node;
    Fr_Memory_Record    *m;
    Test_Blob            blob;
    b32                  photo_intact = 1, thumb_intact = 1;
    u32                  i;

    if (!fr) {
        free(block);
        return;
    }
    memset(&blob, 0, sizeof(blob));
    CHECK(friend_import_export(fr, str_make(fr_export_json,
                                            (i32)sizeof(fr_export_json) - 1),
                               &report) == FRIEND_OK);
    CHECK(report.images == 1);
    CHECK(report.images_without_pixels == 0);
    CHECK(friend_stats(fr).photos == 1);
    CHECK(friend_stats(fr).photos_waiting == 1);

    /* The memory's photograph: the big one for looking at, the small
     * one behind it for the models. */
    CHECK(friend_memory_at(fr, 1, &node) == FRIEND_OK);
    m = fr_memory(fr, node.id.slot);
    CHECK(m != 0);
    if (!m) {
        free(block);
        return;
    }
    CHECK(m->image_count == 1);
    CHECK(fr_blob_for_model(fr, &m->images[0]).len == 900);
    CHECK(fr_blob_at(fr, m->images[0].blob_offset, m->images[0].blob_size)
              .len == 4096);

    /* Written out in one go. */
    friend_tick(fr, TEST_NOW);
    serve_io(fr, &blob, str_nil);
    friend_tick(fr, TEST_NOW + 1000);
    CHECK(blob.writes == 1);
    CHECK(friend_stats(fr).photos_waiting == 0);

    /* And the file holds both, byte for byte, where the memory says. */
    CHECK(m->images[0].blob_offset + 4096 <= blob.len);
    CHECK(m->images[0].thumb_offset + 900 <= blob.len);
    for (i = 0; i < 4096 && m->images[0].blob_offset + i < blob.len; ++i) {
        if (blob.bytes[m->images[0].blob_offset + i] != (u8)(i * 3)) {
            photo_intact = 0;
        }
    }
    for (i = 0; i < 900 && m->images[0].thumb_offset + i < blob.len; ++i) {
        if (blob.bytes[m->images[0].thumb_offset + i] != (u8)(i * 7 + 1)) {
            thumb_intact = 0;
        }
    }
    CHECK(photo_intact);
    CHECK(thumb_intact);

    /* The caption went where a memory's caption goes, into its
     * dialogue. */
    CHECK_STR(fr_memory_captions(&scratch, m),
              "A steel bridge at sunrise, mist over the water.");

    free(block);
}

/* A friend keeps its name when it is reconfigured for something else
 * entirely, because the name is a thing it arrived with. */
static void test_reconfigure_keeps_the_name(void)
{
    void             *block  = 0;
    Friend_Context   *fr     = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Config     config = test_config(FRIEND_PROVIDER_GEMINI,
                                           small_limits());
    Friend_Attachment photo;
    Friend_Io_Request requests[8];
    char              jpeg[64];
    i32               n, i, writes = 0;

    if (!fr) {
        free(block);
        return;
    }
    CHECK(friend_import_export(fr, str_lit("{\"friendName\":\"Echo "
                                           "(imported)\"}"),
                               0) == FRIEND_OK);
    CHECK_STR(friend_name(fr), "Echo (imported)");

    config.friend_name = str_nil;
    config.blob_path   = str_lit("photos.blob");
    CHECK(friend_reconfigure(fr, &config) == FRIEND_OK);
    CHECK_STR(friend_name(fr), "Echo (imported)");

    /* Not even a name in the configuration renames it. */
    config.friend_name = str_lit("Lily");
    CHECK(friend_reconfigure(fr, &config) == FRIEND_OK);
    CHECK_STR(friend_name(fr), "Echo (imported)");

    /* The rest of the configuration did take: photographs now have
     * somewhere to go. */
    memset(jpeg, 0x55, sizeof(jpeg));
    photo.jpeg = str_make(jpeg, (i32)sizeof(jpeg));
    friend_tick(fr, TEST_NOW);
    CHECK(friend_say(fr, str_lit("look"), &photo, 1) == FRIEND_OK);
    for (i = 0; i < 3; ++i) {
        Friend_Io_Request *r;
        friend_tick(fr, TEST_NOW + 1000 * (u64)(i + 1));
        n = friend_pending_io(fr, requests, 8);
        for (r = requests; r < requests + n; ++r) {
            if (r->kind == FRIEND_IO_FILE_WRITE &&
                str_eq(r->path, str_lit("photos.blob"))) {
                ++writes;
            }
            friend_io_failed(fr, r->id, str_lit("not in this test"));
        }
    }
    CHECK(writes >= 1);

    free(block);
}

/* A friend from nothing: everything the context held is gone, the name
 * is the new one, and the configuration it was built with is still the
 * configuration. */
static void test_begin_a_friend(void)
{
    void             *block = 0;
    Friend_Context   *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request request;

    if (!fr) {
        free(block);
        return;
    }
    say_and_retire(fr, str_lit("something to forget"), 1000);
    CHECK(add_memory(fr, str_lit("a memory to forget"), 1000, 0) !=
          FR_SLOT_NONE);
    CHECK(saved_info(fr).total_event_count == 1);
    CHECK(friend_memory_count(fr) == 1);

    friend_tick(fr, 1700000000000ull);
    CHECK(friend_begin(fr, str_lit("Pip")) == FRIEND_OK);
    CHECK_STR(friend_name(fr), "Pip");
    CHECK(friend_created_unix_ms(fr) == 1700000000000ull);
    CHECK(saved_info(fr).total_event_count == 0);
    CHECK(friend_memory_count(fr) == 0);
    CHECK(friend_message_count(fr) == 0);

    /* An empty name leaves it alone, the way a configuration's does. */
    CHECK(friend_begin(fr, str_nil) == FRIEND_OK);
    CHECK_STR(friend_name(fr), "Pip");

    /* The key is not the friend's but the program's, and it stayed. */
    CHECK(say_for_request(fr, str_lit("hello"), &request));
    CHECK(str_index_of(request.headers, str_lit("x-goog-api-key: test-key"))
          >= 0);
    friend_io_failed(fr, request.id, str_lit("not in this test"));

    free(block);
}

static void test_captions(void)
{
    Fr_Chat_Event event;
    String        captions[3];

    memset(&event, 0, sizeof(event));
    captions[0] = str_lit("  a cat on a counter  ");
    captions[1] = str_lit("");
    captions[2] = str_lit("a bridge\nin the mist");

    fr_set_captions(&event, captions, 3);
    CHECK_STR(fr_caption_at(fr_chat_captions(&event), 0), "a cat on a counter");
    /* A photograph nobody described is an empty line, not a missing one,
     * so the third caption is still the third photograph's. */
    CHECK_STR(fr_caption_at(fr_chat_captions(&event), 1), "");
    CHECK_STR(fr_caption_at(fr_chat_captions(&event), 2),
              "a bridge in the mist");
    CHECK(str_is_empty(fr_caption_at(fr_chat_captions(&event), 3)));

    /* Longer than the room there is, cut rather than spilled. */
    {
        char big[FR_CAPTIONS_MAX * 2];
        memset(big, 'x', sizeof(big));
        captions[0] = str_make(big, (i32)sizeof(big));
        fr_set_captions(&event, captions, 1);
        CHECK(event.captions_len == FR_CAPTIONS_MAX);
    }
}

/* A window bigger than the loading build's: its oldest messages go into
 * the scrollback, and the conversation still reads in order. */
static void test_load_wider_window(void)
{
    Friend_Config   config  = test_config(FRIEND_PROVIDER_GEMINI,
                                          small_limits());
    void           *block_a = 0, *block_b = 0;
    Friend_Context *a, *b;
    Friend_Message  newest_a, newest_b;
    u8             *saved;
    size_t          size, written = 0;
    i32             i;
    b32             in_order = 1;

    config.limits.max_active_events = 24;
    a = make_friend_with(&block_a, &config);
    b = fresh_context(&block_b); /* a window of 8 */
    if (!a || !b) {
        free(block_a);
        free(block_b);
        return;
    }

    for (i = 0; i < 4; ++i) say_and_retire(a, str_lit("old"), test_time(0, i));
    for (i = 0; i < 20; ++i) {
        friend_tick(a, test_time(10, i));
        CHECK(fr_commit_event(a, FRIEND_MESSAGE_USER, str_lit("recent"),
                              0) != 0);
    }
    CHECK(friend_stats(a).scrollback == 4);
    CHECK(friend_stats(a).active_events == 20);

    size  = friend_save_size(a);
    saved = (u8 *)malloc(size);
    CHECK(friend_save(a, saved, size, &written) == FRIEND_OK);
    CHECK(friend_load(b, saved, written) == FRIEND_OK);

    CHECK(friend_stats(b).active_events == 8);
    CHECK(friend_message_count(b) == 24);
    for (i = 1; i < friend_message_count(b); ++i) {
        Friend_Message before, after;
        CHECK_SILENT(friend_message(b, i - 1, &before) == FRIEND_OK);
        CHECK_SILENT(friend_message(b, i, &after) == FRIEND_OK);
        if (after.unix_ms <= before.unix_ms) in_order = 0;
    }
    CHECK(in_order);
    CHECK(silent_failures == 0);
    /* The window is the newest of them, which is what the model sees. */
    CHECK(friend_message(a, 23, &newest_a) == FRIEND_OK);
    CHECK(friend_message(b, 23, &newest_b) == FRIEND_OK);
    CHECK(newest_b.unix_ms == newest_a.unix_ms);

    free(saved);
    free(block_a);
    free(block_b);
}

/* A friend replaced while a reply is still out: the request is given up,
 * and its answer, when it comes, lands nowhere. */
static void test_load_abandons_io(void)
{
    void             *block = 0, *other_block = 0;
    Friend_Context   *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Context   *other = fresh_context(&other_block);
    Friend_Io_Request requests[4];
    Friend_Event      event;
    u64               abandoned[4];
    u8               *saved;
    size_t            size, written = 0;
    i32               n;

    if (!fr || !other) {
        free(block);
        free(other_block);
        return;
    }
    friend_tick(fr, TEST_NOW);
    CHECK(friend_say(fr, str_lit("hello"), 0, 0) == FRIEND_OK);
    n = friend_pending_io(fr, requests, 4);
    CHECK(n == 1);

    friend_tick(other, 1700000000000ull);
    CHECK(friend_begin(other, str_lit("Pip")) == FRIEND_OK);
    size  = friend_save_size(other);
    saved = (u8 *)malloc(size);
    CHECK(friend_save(other, saved, size, &written) == FRIEND_OK);
    CHECK(friend_load(fr, saved, written) == FRIEND_OK);
    CHECK_STR(friend_name(fr), "Pip");

    CHECK(friend_abandoned_io(fr, abandoned, 4) == 1);
    CHECK(abandoned[0] == requests[0].id);
    /* Reported, the transport owes nothing: the slot is free already,
     * nothing is reported twice, and finishing the id later is not an
     * error. */
    CHECK(friend_stats(fr).io_inflight == 0);
    CHECK(friend_abandoned_io(fr, abandoned, 4) == 0);
    CHECK(friend_io_failed(fr, requests[0].id, str_lit("closed")) ==
          FRIEND_OK);
    CHECK(friend_io_complete(fr, requests[0].id + 1000, 200) ==
          FRIEND_ERR_BAD_ARGUMENT);
    while (friend_next_event(fr, &event)) FAIL("the old ones went");

    answer_request(
        fr, &requests[0],
        str_lit("data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":"
                "\"hey\"}]},\"finishReason\":\"STOP\"}]}\n"),
        200);
    friend_tick(fr, TEST_NOW + 1000);
    CHECK(friend_message_count(fr) == 0);
    CHECK(friend_stats(fr).io_inflight == 0);
    while (friend_next_event(fr, &event)) {
        CHECK(event.kind != FRIEND_EVENT_REPLY_DELTA &&
              event.kind != FRIEND_EVENT_REPLY_DONE);
    }
    /* The friend that was loaded is not stuck behind the one that went. */
    CHECK(friend_say(fr, str_lit("hello"), 0, 0) == FRIEND_OK);

    free(saved);
    free(block);
    free(other_block);
}

/* A save whose name is longer than this build has room for is damaged,
 * and is refused — by a peek as well as a load — rather than read as an
 * empty name. */
static void test_load_overlong_field(void)
{
    void            *block = 0;
    Friend_Context  *fr    = fresh_context(&block);
    Friend_Save_Info info;
    u8              *saved;
    size_t           size, written = 0, name_at = 0, at;

    if (!fr) {
        free(block);
        return;
    }
    CHECK(friend_begin(fr, str_lit("Em")) == FRIEND_OK);
    size  = friend_save_size(fr);
    saved = (u8 *)malloc(size);
    CHECK(friend_save(fr, saved, size, &written) == FRIEND_OK);
    CHECK(friend_save_peek(saved, written, &info) == FRIEND_OK);
    CHECK_STR(info.name, "Em");

    /* The name is a length and its bytes, somewhere in the header. */
    for (at = 0; at + 6 <= written && name_at == 0; ++at) {
        if (fr_le_get32(saved + at) == 2 && saved[at + 4] == 'E' &&
            saved[at + 5] == 'm') {
            name_at = at;
        }
    }
    CHECK(name_at > 0);
    fr_le_put32(saved + name_at, FRIEND_NAME_MAX + 1);
    CHECK(friend_save_peek(saved, written, &info) == FRIEND_ERR_BAD_DATA);
    CHECK(friend_load(fr, saved, written) == FRIEND_ERR_BAD_DATA);
    /* Refused at the header, so the friend that was open is still open. */
    CHECK_STR(friend_name(fr), "Em");

    /* And a short read is told how much is enough, every time. */
    fr_le_put32(saved + name_at, 2);
    CHECK(friend_save_peek(saved, 20, &info) == FRIEND_ERR_TOO_SMALL);
    CHECK(info.bytes_needed > 20);
    CHECK(friend_save_peek(saved, written < info.bytes_needed
                                      ? written
                                      : info.bytes_needed,
                           &info) == FRIEND_OK);

    free(saved);
    free(block);
}

static void test_json_bad_timestamp(void)
{
    const char *const bad[] = {
        "\"yesterday\"", "\"2026-11-02\"", "\"2026-11-02T13:17:04+05\"",
        "\"2026-11-02T13:xx:04Z\"",
    };
    i32 i;
    for (i = 0; i < fr_array_count(bad); ++i) {
        Json_Parser p = json_begin(str_from_cstr(bad[i]));
        CHECK(json_timestamp_ms(&p) == 0);
        CHECK(p.failed);
    }
    {
        Json_Parser p = json_begin(str_lit("\"1969-12-31T23:59:59Z\""));
        CHECK(json_timestamp_ms(&p) == 0);
        CHECK(!p.failed); /* real, just before anything we can count */
    }
}

/* A photo file that will not take a write: said once, and not tried
 * again every tick. */
static void test_blob_write_failure(void)
{
    void             *block = 0;
    Friend_Context   *fr    = make_photo_friend(&block, small_limits());
    Friend_Attachment photo;
    Friend_Io_Request requests[8];
    Friend_Event      event;
    char              jpeg[256];
    i32               errors = 0, writes = 0, round, i, n;
    u64               now = TEST_NOW;

    if (!fr) {
        free(block);
        return;
    }
    memset(jpeg, 0x5A, sizeof(jpeg));
    photo.jpeg = str_make(jpeg, (i32)sizeof(jpeg));
    friend_tick(fr, now);
    CHECK(friend_say(fr, str_lit("look"), &photo, 1) == FRIEND_OK);

    for (round = 0; round < 40; ++round) {
        now += 1000;
        friend_tick(fr, now);
        n = friend_pending_io(fr, requests, 8);
        for (i = 0; i < n; ++i) {
            if (requests[i].kind == FRIEND_IO_FILE_WRITE) {
                ++writes;
                friend_io_failed(fr, requests[i].id, str_lit("disk full"));
            } else {
                friend_io_failed(fr, requests[i].id, str_lit("not here"));
            }
        }
        while (friend_next_event(fr, &event)) {
            if (event.kind == FRIEND_EVENT_ERROR &&
                str_eq(event.text, str_lit("disk full"))) {
                ++errors;
            }
        }
    }
    /* Forty seconds of ticks: a few tries, ten seconds apart, and one
     * complaint. */
    CHECK(writes >= 3 && writes <= 5);
    CHECK(errors == 1);
    CHECK(friend_stats(fr).photos_waiting == 1);

    free(block);
}

/* ================================================================== *
 *  Consolidation, end to end
 * ================================================================== */

/* A conversation long enough to fold, through a provider that names two
 * events in it, relates them, and has something to say about wanting:
 * the friend comes out the other side with two memories, an edge between
 * them, and a new interior monologue — and the next reply recalls them. */
static void test_consolidation(void)
{
    Friend_Config      config = test_config(FRIEND_PROVIDER_GEMINI,
                                            small_limits());
    void              *block  = 0;
    Friend_Context    *fr;
    Fake_Provider      fake;
    Friend_Memory_Node first, second;
    Friend_Relation    relation = FRIEND_RELATION_NONE;
    Friend_Event       event;
    Friend_Io_Request  request;
    u64                now = TEST_NOW;
    b32                memories_changed = 0, interiority_changed = 0;
    String             relate;

    config.consolidate_at      = 4;
    config.consolidate_leaving = 1;
    fr = make_friend_with(&block, &config);
    if (!fr) {
        free(block);
        return;
    }
    memset(&fake, 0, sizeof(fake));
    fake.reply   = "ok";
    fake.rerank  = "1";
    fake.extract = "Event [1-2]: Sam adopted a cat called Pickle\n"
                   "this line is not an event\n"
                   "Event [3-4]: Sam bought food for Pickle";
    fake.relate      = "Explanation: one led to the other\nRelation: Cause";
    fake.interiority = "- I want to meet Pickle.\n"
                       "not a statement about wanting\n"
                       "2. I'm curious about cats.";
    fake.embed_axis  = 5;

    friend_tick(fr, now);
    CHECK(friend_say(fr, str_lit("I adopted a cat"), 0, 0) == FRIEND_OK);
    fake_serve(fr, &fake, &now);
    CHECK(friend_say(fr, str_lit("I bought her food"), 0, 0) == FRIEND_OK);
    fake_serve(fr, &fake, &now);
    CHECK(fake.extracts == 0); /* four messages is not past four */
    CHECK(friend_say(fr, str_lit("she likes it"), 0, 0) == FRIEND_OK);
    fake_serve(fr, &fake, &now);
    CHECK(fake.extracts == 1);

    /* Two memories, in the order they were named, with the message that
     * was left behind still in the window. */
    CHECK(friend_memory_count(fr) == 2);
    CHECK(friend_memory_at(fr, 0, &first) == FRIEND_OK);
    CHECK(friend_memory_at(fr, 1, &second) == FRIEND_OK);
    CHECK_STR(first.description, "Sam adopted a cat called Pickle");
    CHECK_STR(second.description, "Sam bought food for Pickle");
    CHECK(first.created_unix_ms > TEST_NOW && first.created_unix_ms <= now);
    CHECK(friend_stats(fr).active_events == 1);

    /* The second was related to the first, which was shown to the model
     * with the dialogue each came from, and the answer is an edge. */
    CHECK(fake.relates == 1);
    relate = str_make(fake.last_relate, fake.last_relate_len);
    CHECK(str_index_of(relate, str_lit("[Sentence A]: Sam adopted a cat "
                                       "called Pickle")) >= 0);
    CHECK(str_index_of(relate, str_lit("[Sentence B]: Sam bought food for "
                                       "Pickle")) >= 0);
    CHECK(str_index_of(relate, str_lit("I bought her food")) >= 0);
    CHECK(friend_memory_edge_count(fr) == 1);
    CHECK(edges_between(fr, first.id.slot, second.id.slot, &relation) == 1);
    CHECK(relation == FRIEND_RELATION_CAUSE);

    /* What it wants now: the "I" statements, cleaned of their bullets,
     * and nothing else. */
    CHECK(fake.interiorities == 1);
    CHECK_STR(friend_interiority(fr),
              "I want to meet Pickle.\nI'm curious about cats.");
    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_MEMORIES_CHANGED) memories_changed = 1;
        if (event.kind == FRIEND_EVENT_INTERIORITY_CHANGED) {
            interiority_changed = 1;
        }
    }
    CHECK(memories_changed);
    CHECK(interiority_changed);

    /* And the next reply finds them. */
    CHECK(friend_say(fr, str_lit("how is Pickle doing"), 0, 0) == FRIEND_OK);
    {
        i32 round;
        b32 found = 0;
        for (round = 0; round < 10 && !found; ++round) {
            i32 n = friend_pending_io(fr, &request, 1);
            if (n == 0) {
                now += 100;
                friend_tick(fr, now);
                continue;
            }
            if (str_index_of(request.url, str_lit("streamGenerateContent")) >=
                0) {
                found = 1;
                CHECK(str_index_of(request.body,
                                   str_lit("Sam adopted a cat called Pickle"))
                      >= 0);
                CHECK(str_index_of(request.body, str_lit("I want to meet "
                                                         "Pickle.")) >= 0);
            }
            fake_answer(fr, &fake, &request);
        }
        CHECK(found);
        CHECK(fake.reranks >= 1);
    }

    free(block);
}

/* ================================================================== *
 *  What a frontend is told, and what it has to save
 * ================================================================== */

/* Drains the queue, counting one kind of event. */
static i32 count_events(Friend_Context *fr, Friend_Event_Kind kind)
{
    Friend_Event event;
    i32          count = 0;
    while (friend_next_event(fr, &event)) count += event.kind == kind;
    return count;
}

/* Everything the friend is, through a save file and into another one. */
static b32 save_into(Friend_Context *from, Friend_Context *to)
{
    size_t size    = friend_save_size(from);
    size_t written = 0;
    u8    *bytes   = (u8 *)malloc(size);
    b32    ok      = bytes &&
                friend_save(from, bytes, size, &written) == FRIEND_OK &&
                friend_load(to, bytes, written) == FRIEND_OK;
    free(bytes);
    return ok;
}

/* The index on an event is the one friend_message reads the message at,
 * which is not where it sits in the window once the window is smaller
 * than the conversation. */
static void test_event_indices(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Fake_Provider   fake;
    Friend_Event    event;
    Friend_Message  m;
    u64             now       = TEST_NOW;
    i32             committed = -2, delta = -2, done = -2, others = 0;
    i32             i;

    if (!fr) {
        free(block);
        return;
    }
    memset(&fake, 0, sizeof(fake));
    fake.reply = "hey there";
    friend_tick(fr, now);
    for (i = 0; i < 5; ++i) {
        CHECK(friend_say(fr, str_lit("hello"), 0, 0) == FRIEND_OK);
        fake_serve(fr, &fake, &now);
    }
    count_events(fr, FRIEND_EVENT_NONE);
    CHECK(friend_stats(fr).scrollback > 0);

    CHECK(friend_say(fr, str_lit("something"), 0, 0) == FRIEND_OK);
    fake_serve(fr, &fake, &now);
    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_MESSAGE_COMMITTED) {
            committed = event.index;
        } else if (event.kind == FRIEND_EVENT_REPLY_DELTA) {
            delta = event.index;
        } else if (event.kind == FRIEND_EVENT_REPLY_DONE) {
            done = event.index;
        } else if (event.index != -1) {
            ++others;
        }
    }
    CHECK(committed == 10);
    CHECK(friend_message(fr, committed, &m) == FRIEND_OK);
    CHECK_STR(m.text, "something");
    CHECK(done == 11 && delta == done);
    CHECK(friend_message(fr, done, &m) == FRIEND_OK);
    CHECK(m.kind == FRIEND_MESSAGE_FRIEND);
    CHECK_STR(m.text, "hey there");
    CHECK(others == 0); /* nothing else claims a message */

    /* A reply that never arrived is taken back out, and the message it
     * answered is still where its event said. */
    fake.reply = 0;
    CHECK(friend_say(fr, str_lit("again"), 0, 0) == FRIEND_OK);
    fake_serve(fr, &fake, &now);
    while (friend_next_event(fr, &event)) {
        if (event.kind == FRIEND_EVENT_MESSAGE_COMMITTED) {
            CHECK(friend_message(fr, event.index, &m) == FRIEND_OK);
            CHECK_STR(m.text, "again");
        }
        if (event.kind == FRIEND_EVENT_REPLY_DONE) FAIL("no reply came");
    }
    free(block);
}

/* A message whose reply failed is still something the user said. It is
 * worth saving the moment it is committed, it is saved, and it comes
 * back. */
static void test_unsaved(void)
{
    void              *block_a = 0, *block_b = 0;
    Friend_Context    *a = make_friend(&block_a, FRIEND_PROVIDER_GEMINI);
    Friend_Context    *b = make_friend(&block_b, FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request  request;
    Friend_Message     m;
    Friend_Memory_Node node;
    u32                slot;

    if (!a || !b) {
        free(block_a);
        free(block_b);
        return;
    }
    friend_tick(a, TEST_NOW);
    CHECK(!friend_stats(a).unsaved);
    CHECK(friend_stats(a).settled);

    CHECK(say_for_request(a, str_lit("remember me"), &request));
    CHECK(friend_stats(a).unsaved);
    CHECK(!friend_stats(a).settled); /* a reply is on its way */
    friend_io_failed(a, request.id, str_lit("the network is down"));
    friend_tick(a, TEST_NOW + 100);
    CHECK(friend_stats(a).settled);
    CHECK(friend_stats(a).unsaved);
    /* Said once, however many things changed. */
    CHECK(count_events(a, FRIEND_EVENT_STATE_DIRTY) == 1);

    CHECK(save_into(a, b));
    CHECK(!friend_stats(a).unsaved);
    CHECK(!friend_stats(b).unsaved);
    CHECK(friend_message_count(b) == 1);
    CHECK(friend_message(b, 0, &m) == FRIEND_OK);
    CHECK_STR(m.text, "remember me");

    /* Forgetting is a change; the first one after a save says so again. */
    slot = add_memory(a, str_lit("something"), 1000, 0);
    CHECK(memory_node(a, slot, &node));
    count_events(a, FRIEND_EVENT_NONE);
    CHECK(friend_forget(a, node.id) == FRIEND_OK);
    CHECK(friend_stats(a).unsaved);
    CHECK(count_events(a, FRIEND_EVENT_STATE_DIRTY) == 1);

    /* A friend from nothing has never been written anywhere. */
    CHECK(friend_begin(b, str_lit("Pip")) == FRIEND_OK);
    CHECK(friend_stats(b).unsaved);

    free(block_a);
    free(block_b);
}

/* A save taken while a reply is still arriving leaves the reply out:
 * half a sentence is not a message anybody sent. */
static void test_save_mid_reply(void)
{
    void             *block_a = 0, *block_b = 0;
    Friend_Context   *a = make_friend(&block_a, FRIEND_PROVIDER_GEMINI);
    Friend_Context   *b = make_friend(&block_b, FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request request;
    Friend_Message    m;
    String            half = str_lit(
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":"
        "\"half of a\"}]}}]}\n");

    if (!a || !b) {
        free(block_a);
        free(block_b);
        return;
    }
    friend_tick(a, TEST_NOW);
    CHECK(say_for_request(a, str_lit("tell me"), &request));
    memcpy(request.recv, half.data, (size_t)half.len);
    friend_io_progress(a, request.id, (u32)half.len);
    friend_tick(a, TEST_NOW + 100);
    CHECK(friend_message(a, 1, &m) == FRIEND_OK && m.is_streaming);
    CHECK_STR(m.text, "half of a");

    CHECK(save_into(a, b));
    CHECK(friend_message_count(b) == 1);
    CHECK(friend_message(b, 0, &m) == FRIEND_OK);
    CHECK_STR(m.text, "tell me");

    free(block_a);
    free(block_b);
}

/* A memory a consolidation is still forming is nobody's yet: recall does
 * not find it even once it has a vector, the backfill does not embed it
 * a second time, and a save leaves it out — the stretch it came from is
 * folded again after a load. */
static void test_staged_memories(void)
{
    void              *block_a = 0, *block_b = 0;
    Friend_Context    *a = make_friend(&block_a, FRIEND_PROVIDER_GEMINI);
    Friend_Context    *b = make_friend(&block_b, FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request  requests[4];
    i8                 query[FR_EMBED_DIMS];
    u32                found[4];
    f32                scores[4];
    u32                kept, staged;

    if (!a || !b) {
        free(block_a);
        free(block_b);
        return;
    }
    friend_tick(a, TEST_NOW);
    kept   = add_memory(a, str_lit("kept"), 1000, 0);
    staged = fr_memory_stage(a, str_lit("half formed"), 2000);
    CHECK(staged != FR_SLOT_NONE);

    /* Still waiting for its vector: the backfill leaves it to the fold. */
    friend_tick(a, TEST_NOW + 100);
    CHECK(friend_pending_io(a, requests, 4) == 0);

    /* With one, but not joined: still not something recall can find. */
    a->memories[staged].embedding[0] = 100;
    a->memories[staged].needs_embedding = 0;
    memset(query, 0, sizeof(query));
    query[0] = 100;
    CHECK(fr_nearest_memories(a, query, 4, found, scores) == 1);
    CHECK(found[0] == kept);

    CHECK(save_into(a, b));
    CHECK(friend_memory_count(b) == 1);
    CHECK(saved_info(b).memories == 1);

    free(block_a);
    free(block_b);
}

/* A request is built when the friend wants it and handed out when the
 * transport asks, and the configuration can change in between. What it
 * says is what it said when it was made. */
static void test_request_owns_its_strings(void)
{
    void             *block = 0;
    Friend_Context   *fr    = make_photo_friend(&block, small_limits());
    Friend_Config     config;
    Friend_Attachment photo;
    Friend_Io_Request requests[8];
    char              jpeg[256];
    i32               n, i, writes = 0;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);
    memset(jpeg, 0x5A, sizeof(jpeg));
    photo.jpeg = str_make(jpeg, (i32)sizeof(jpeg));
    CHECK(friend_say(fr, str_lit("look"), &photo, 1) == FRIEND_OK);
    friend_tick(fr, TEST_NOW + 10);

    /* Strings of other lengths, so everything after the key in the
     * configuration's arena moves. */
    config             = test_config(FRIEND_PROVIDER_GEMINI, small_limits());
    config.api_key     = str_lit("a rather longer key than the one before");
    config.user_name   = str_lit("Samantha");
    config.blob_path   = str_lit("photos.blob");
    CHECK(friend_reconfigure(fr, &config) == FRIEND_OK);

    n = friend_pending_io(fr, requests, 8);
    for (i = 0; i < n; ++i) {
        if (requests[i].kind != FRIEND_IO_FILE_WRITE) continue;
        ++writes;
        CHECK_STR(requests[i].path, "photos.blob");
        CHECK(requests[i].body.len == (i32)sizeof(jpeg));
    }
    CHECK(writes == 1);
    free(block);
}

/* What the friend overheard reaches a model as the summary it wrote for
 * itself and never as the raw transcript, whichever model is asking. */
static void test_overheard_stays_private(void)
{
    void             *block = 0;
    Friend_Context   *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Io_Request request;
    Fr_Chat_Event    *raw;
    String            secret = str_lit("the raw words somebody said nearby");

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, TEST_NOW);
    fr_commit_event(fr, FRIEND_MESSAGE_AMBIENT,
                    str_lit("a summary of what was overheard"), 1);
    raw = fr_commit_event(fr, FRIEND_MESSAGE_AMBIENT, secret, 1);
    raw->ambient_raw = 1;

    CHECK(say_for_request(fr, str_lit("hello"), &request));
    CHECK(str_index_of(request.body, str_lit("a summary of what")) >= 0);
    CHECK(str_index_of(request.body, secret) < 0);
    CHECK(str_index_of(fr_recall_query(fr, &scratch, 10), secret) < 0);
    CHECK(str_index_of(fr_build_live_recap(fr, &scratch), secret) < 0);
    CHECK(str_index_of(fr_build_extract_conversation(fr, &scratch,
                                                     fr->active_events,
                                                     fr->active_count),
                       secret) < 0);
    friend_io_failed(fr, request.id, str_lit("not in this test"));
    free(block);
}

/* A memory is used when a reranker chose it. When the reranker could not
 * be asked, the embedding order stands in for its answer and the memory
 * still reaches the prompt, but nobody chose it. */
static void test_fallback_is_not_a_use(void)
{
    void              *block = 0;
    Friend_Context    *fr    = make_friend(&block, FRIEND_PROVIDER_GEMINI);
    Fake_Provider      fake;
    Friend_Memory_Node node;
    u64                now = TEST_NOW;
    u32                slot;

    if (!fr) {
        free(block);
        return;
    }
    friend_tick(fr, now);
    slot = add_memory(fr, str_lit("the user has a cat"), 1000, 0);
    memset(&fake, 0, sizeof(fake));
    fake.reply      = "nice";
    fake.embed_axis = 0;

    CHECK(friend_say(fr, str_lit("my cat"), 0, 0) == FRIEND_OK);
    fake_serve(fr, &fake, &now);
    CHECK(fake.replies == 1);
    CHECK(str_index_of(friend_last_prompt(fr), str_lit("has a cat")) >= 0);
    CHECK(memory_node(fr, slot, &node) && node.use_count == 0);

    fake.rerank = "1";
    CHECK(friend_say(fr, str_lit("my cat again"), 0, 0) == FRIEND_OK);
    fake_serve(fr, &fake, &now);
    CHECK(memory_node(fr, slot, &node) && node.use_count == 1);
    free(block);
}

/* A file from the version before this one, which wrote down three fewer
 * record shapes, still loads; one whose shapes are not this build's does
 * not. */
static void test_load_version_3(void)
{
    void           *block_a = 0, *block_b = 0;
    Friend_Context *a = make_friend(&block_a, FRIEND_PROVIDER_GEMINI);
    Friend_Context *b = make_friend(&block_b, FRIEND_PROVIDER_GEMINI);
    Friend_Message  m;
    u8             *bytes;
    size_t          size, written = 0;
    /* Magic, version, then the eight shapes version 3 had. */
    size_t          shapes_end = 8 + 4 + 8 * 4;

    if (!a || !b) {
        free(block_a);
        free(block_b);
        return;
    }
    friend_tick(a, TEST_NOW);
    say_and_retire(a, str_lit("from before"), TEST_NOW);
    size  = friend_save_size(a);
    bytes = (u8 *)malloc(size);
    CHECK(friend_save(a, bytes, size, &written) == FRIEND_OK);

    /* The three newer shapes are wrong: refused. */
    fr_le_put32(bytes + shapes_end, FR_CAPTIONS_MAX + 1);
    CHECK(friend_load(b, bytes, written) == FRIEND_ERR_BAD_DATA);

    /* Cut out, and the file says it is a 3. */
    memmove(bytes + shapes_end, bytes + shapes_end + 12,
            written - shapes_end - 12);
    fr_le_put32(bytes + 8, 3);
    CHECK(friend_load(b, bytes, written - 12) == FRIEND_OK);
    CHECK(friend_message_count(b) == 1);
    CHECK(friend_message(b, 0, &m) == FRIEND_OK);
    CHECK_STR(m.text, "from before");

    free(bytes);
    free(block_a);
    free(block_b);
}

/* A realtime session, open and listening, for a test that wants to do
 * something to one. Returns the socket's id, or 0. */
static u64 open_live(Friend_Context *fr)
{
    Friend_Io_Request requests[8];
    u64               channel;

    friend_tick(fr, TEST_NOW);
    if (friend_voice_open(fr) != FRIEND_OK) return 0;
    if (friend_pending_io(fr, requests, 8) != 1) return 0;
    channel = requests[0].id;
    friend_ws_recv(fr, channel, str_nil, FRIEND_IO_FLAG_OPENED);
    (void)ws_drain(fr, &scratch, 0);
    ws_say(fr, channel, "{\"setupComplete\":{}}");
    (void)ws_drain(fr, &scratch, 0);
    return channel;
}

/* The message that ends a turn is never lost for want of a slot to say
 * it in: it waits, and goes when there is one. */
static void test_live_end_of_turn_waits(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_live_friend(&block, FRIEND_PROVIDER_GEMINI);
    Fr_Io_Slot     *taken[8];
    String          sent;
    i32             held = 0, n, i;

    if (!fr || !open_live(fr)) {
        CHECK(fr != 0);
        free(block);
        return;
    }
    push_speech(fr);
    friend_tick(fr, TEST_NOW + 1000);
    (void)ws_drain(fr, &scratch, 0);

    /* Everything else the friend does has every slot. */
    while (held < 8 && (taken[held] = fr_io_acquire(
                            fr, FR_IO_PURPOSE_CAPTION, FRIEND_IO_HTTPS, 0))) {
        ++held;
    }
    push_quiet(fr);
    CHECK(friend_voice_state(fr) == FRIEND_VOICE_WORKING);

    for (i = 0; i < held; ++i) fr_io_release(fr, taken[i]);
    friend_tick(fr, TEST_NOW + 1100);
    sent = ws_drain(fr, &scratch, &n);
    CHECK(str_index_of(sent, str_lit("activityEnd")) >= 0);
    free(block);
}

/* A session that goes quiet is closed, and what was heard before it did
 * is in the conversation. */
static void test_live_quiet_keeps_the_turn(void)
{
    void           *block = 0;
    Friend_Context *fr    = make_live_friend(&block, FRIEND_PROVIDER_GEMINI);
    Friend_Message  m;
    u64             channel = fr ? open_live(fr) : 0;

    if (!channel) {
        CHECK(fr != 0);
        free(block);
        return;
    }
    ws_say(fr, channel,
           "{\"serverContent\":{\"inputTranscription\":{\"text\":\"hello? "
           "are you there\"}}}");
    friend_tick(fr, TEST_NOW + 100000);
    CHECK(friend_message_count(fr) == 1);
    CHECK(friend_message(fr, 0, &m) == FRIEND_OK);
    CHECK_STR(m.text, "hello? are you there");
    free(block);
}

/* A user nobody named is "the user" to every prompt. */
static void test_unnamed_user(void)
{
    Friend_Config   config = test_config(FRIEND_PROVIDER_GEMINI,
                                         small_limits());
    void           *block  = 0;
    Friend_Context *fr;
    String          prompt;

    config.user_name = str_nil;
    fr               = make_friend_with(&block, &config);
    if (!fr) {
        free(block);
        return;
    }
    CHECK_STR(fr->user_name, "the user");
    prompt = fr_build_system_prompt(fr, &scratch, str_nil, str_nil);
    CHECK(str_index_of(prompt, str_lit("{userName}")) < 0);
    free(block);
}

int main(void)
{
    test_setup = scratch_reset;
    fill_voice(voice_speech, fr_array_count(voice_speech), 6000);

    run("strings", test_strings);
    run("lines", test_lines);
    run("number parsing", test_number_parsing);
    run("arena", test_arena_region);
    run("string builder", test_builder);
    run("base64", test_base64);
    run("human duration", test_human_duration);
    run("date and time", test_date_time);
    run("json reading", test_json_reading);
    run("json edges", test_json_edges);
    run("json writing", test_json_writing);
    run("a timestamp that is not one", test_json_bad_timestamp);
    run("limits", test_limits);
    run("init", test_init);
    run("slots", test_slots);
    run("event queue", test_event_queue);
    run("unzip", test_unzip);
    run("unzip window", test_unzip_window);
    run("unzip bad input", test_unzip_bad_input);
    run("dialog stream", test_dialog_stream);
    run("utf8 truncation", test_truncate_utf8);
    run("import scalars", test_import_scalars);
    run("import memories", test_import_memories);
    run("import graph", test_import_graph);
    run("import photos", test_import_photos);
    run("import conversation", test_import_conversation);
    run("import embed model", test_import_other_embed_model);
    run("import limits", test_import_limits);
    run("import bad input", test_import_bad_input);
    run("zip to friend", test_zip_to_friend);
    run("save round trip", test_save_round_trip);
    run("save bad input", test_save_bad_input);
    run("save truncation", test_save_truncated_import);
    run("loading a wider window", test_load_wider_window);
    run("loading over a reply in flight", test_load_abandons_io);
    run("loading an over-long field", test_load_overlong_field);

    run("embedding maths", test_embedding_maths);
    run("nearest memories", test_nearest_memories);
    run("a failing backfill backs off", test_backfill_backs_off);
    run("recall query", test_recall_query);
    run("timelines", test_timelines);
    run("memory usage", test_memory_usage);
    run("render template", test_render_template);
    run("system prompt", test_system_prompt);
    run("prompt override", test_prompt_override);
    run("user turn", test_user_turn);
    run("gemini request", test_gemini_request);
    run("openai request", test_openai_request);
    run("provider responses", test_provider_responses);
    run("streaming", test_streaming);
    run("sentence ends", test_sentence_ends);
    run("a turn with no memories", test_turn_without_memories);
    run("a turn with recall", test_turn_recall);
    run("connect to related", test_connect_to_related);
    run("forgetting a memory", test_forget);
    run("event line parsing", test_event_line_parsing);
    run("relation parsing", test_relation_parsing);
    run("pruning", test_prune_unused);
    run("extraction sees captions", test_extract_sees_captions);
    run("consolidation end to end", test_consolidation);

    run("scrollback", test_scrollback);
    run("scrollback loss", test_scrollback_loss);
    run("a fold owns its stretch", test_fold_owns_its_stretch);
    run("consolidation catches up", test_fold_catches_up);
    run("growing into the block", test_growing_block);
    run("photographs", test_photographs);
    run("photos with no file", test_photos_without_a_file);
    run("import photo pixels", test_import_photo_pixels);
    run("a photo file that will not write", test_blob_write_failure);
    run("captions", test_captions);
    run("reconfigure keeps the name", test_reconfigure_keeps_the_name);
    run("a friend from nothing", test_begin_a_friend);

    run("which voice, by name", test_voice_names);
    run("which voice", test_which_voice);
    run("a quiet room", test_voice_hears_nothing);
    run("a spoken turn", test_voice_turn);
    run("a spoken turn on gemini", test_voice_turn_gemini);
    run("sentences to speak", test_voice_sentences);
    run("interrupting it", test_voice_interrupted);
    run("resampling, and again", test_voice_resample_and_repeat);
    run("a realtime session", test_live_session);
    run("a realtime session on openai", test_live_session_openai);
    run("a realtime session that dies", test_live_session_closes);

    run("event indices", test_event_indices);
    run("unsaved changes", test_unsaved);
    run("saving mid-reply", test_save_mid_reply);
    run("staged memories", test_staged_memories);
    run("a request owns its strings", test_request_owns_its_strings);
    run("what was overheard stays private", test_overheard_stays_private);
    run("a fallback pick is not a use", test_fallback_is_not_a_use);
    run("loading a version 3 file", test_load_version_3);
    run("the end of a realtime turn waits", test_live_end_of_turn_waits);
    run("a quiet session keeps its turn", test_live_quiet_keeps_the_turn);
    run("a user nobody named", test_unnamed_user);
    return test_finish();
}
