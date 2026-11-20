/* fr_json.h — JSON without a document model.
 *
 * Every JSON we touch has a schema we already know: a provider's response,
 * or the official friend export. So there is no tree, no token array, and
 * nothing proportional to the document — you walk the text and pull out
 * the fields you want. The export can be a gigabyte; a tokenizer like jsmn
 * would need a token array sized to it, which is exactly the kind of
 * allocation this library does not have.
 *
 * A parser that fails stays failed. Every reader returns a harmless zero
 * after that, so a long chain of pulls needs one check at the end rather
 * than one per field.
 *
 * protojson quirks are handled where they are not optional: 64-bit ints
 * arrive as strings, bytes arrive as base64, and timestamps arrive as
 * RFC 3339 text.
 */

#ifndef FR_JSON_H
#define FR_JSON_H

#include "fr_base.h"

typedef enum Json_Kind {
    JSON_INVALID = 0,
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_NUMBER,
    JSON_BOOL,
    JSON_NULL
} Json_Kind;

typedef struct Json_Parser {
    String text;
    i32    at;
    b32    failed;
    String error;
} Json_Parser;

FR_BASE Json_Parser json_begin(String text);

/* What the next value is, without consuming it. */
FR_BASE Json_Kind json_peek(Json_Parser *p);

/* Containers. The _next calls consume the closing bracket and return 0
 * when the container is done, so the idiom is:
 *
 *     String key;
 *     json_object_begin(&p);
 *     while (json_object_next(&p, &key)) {
 *         if (str_eq(key, str_lit("friendName"))) name = json_string(&p, a);
 *         else json_skip(&p);
 *     }
 *
 * Every branch must consume exactly one value, which is why the else is
 * json_skip and not nothing. */
FR_BASE b32 json_object_begin(Json_Parser *p);
FR_BASE b32 json_object_next(Json_Parser *p, String *out_key);
FR_BASE b32 json_array_begin(Json_Parser *p);
FR_BASE b32 json_array_next(Json_Parser *p);

/* Values. Each consumes exactly one. */

/* The bytes between the quotes, escapes left alone and no copy made. For
 * base64 payloads (images, embeddings) this is what you want: decode
 * straight out of the document. */
FR_BASE String json_string_raw(Json_Parser *p);

/* Unescaped into arena, including \uXXXX surrogate pairs as UTF-8. When
 * there is nothing to unescape this is the raw view and no copy is made,
 * so the result may point into the document rather than the arena. */
FR_BASE String json_string(Json_Parser *p, Arena *arena);

/* The same, appended onto a builder that is already open. For a reader
 * assembling one text out of many JSON strings: it cannot allocate in
 * the builder's arena, and it does not have to. */
FR_BASE void json_string_append(Json_Parser *p, String_Builder *sb);

/* Accepts a bare number or a quoted one, because protojson quotes every
 * 64-bit integer. */
FR_BASE i64 json_i64(Json_Parser *p);
FR_BASE f64 json_f64(Json_Parser *p);
FR_BASE b32 json_bool(Json_Parser *p);

/* A protojson Timestamp: "2026-11-02T13:17:04.512Z", with optional
 * fractional seconds and an optional numeric offset. Anything else fails
 * the parser; a time before 1970 reads as 0. */
FR_BASE u64 json_timestamp_ms(Json_Parser *p);

/* Decode a base64 string value into out. Returns bytes written, or -1 on
 * malformed input or insufficient room. */
FR_BASE i64 json_base64(Json_Parser *p, u8 *out, size_t out_capacity);

/* Consume and discard one value of any shape. Iterative: a pathological
 * document cannot blow the stack. */
FR_BASE void json_skip(Json_Parser *p);

/* ------------------------------------------------------------------ *
 *  Writing
 * ------------------------------------------------------------------ */

#define JSON_WRITER_MAX_DEPTH 32

typedef struct Json_Writer {
    String_Builder sb;
    i32            depth;
    u32            has_item;  /* bit per depth: does this container need a comma */
    b32            after_key; /* the next value belongs to the key just written */
    b32            failed;   /* depth overflow, or the arena ran out */
} Json_Writer;

FR_BASE Json_Writer jw_begin(Arena *arena);

FR_BASE void jw_object_begin(Json_Writer *w);
FR_BASE void jw_object_end(Json_Writer *w);
FR_BASE void jw_array_begin(Json_Writer *w);
FR_BASE void jw_array_end(Json_Writer *w);

FR_BASE void jw_key(Json_Writer *w, String key);
FR_BASE void jw_string(Json_Writer *w, String value);
FR_BASE void jw_i64(Json_Writer *w, i64 value);
FR_BASE void jw_f64(Json_Writer *w, f64 value);
FR_BASE void jw_bool(Json_Writer *w, b32 value);
FR_BASE void jw_null(Json_Writer *w);
FR_BASE void jw_base64(Json_Writer *w, const u8 *bytes, size_t byte_count);
/* Two buffers as one base64 string: a file header and its contents. */
FR_BASE void jw_base64_join(Json_Writer *w, const u8 *a, size_t a_count, const u8 *b,
                    size_t b_count);
FR_BASE void jw_base64_prefixed(Json_Writer *w, String prefix, const u8 *bytes,
                        size_t byte_count);

FR_BASE void jw_field_string(Json_Writer *w, String key, String value);
FR_BASE void jw_field_i64(Json_Writer *w, String key, i64 value);
FR_BASE void jw_field_f64(Json_Writer *w, String key, f64 value);
FR_BASE void jw_field_bool(Json_Writer *w, String key, b32 value);

FR_BASE String jw_done(Json_Writer *w);

FR_STATIC_ASSERT(JSON_WRITER_MAX_DEPTH <= 32, json_depth_fits_has_item_bits);

#endif /* FR_JSON_H */
