/* fr_json.c — see fr_json.h for why there is no document model. */

#include "fr_json.h"

/* ------------------------------------------------------------------ *
 *  Reading
 * ------------------------------------------------------------------ */

FR_BASE Json_Parser json_begin(String text)
{
    Json_Parser p;
    p.text   = text;
    p.at     = 0;
    p.failed = 0;
    p.error  = str_nil;
    return p;
}

/* First failure wins: later readers see a parser that is already dead and
 * return zero, so callers check once at the end. */
FR_BASE void json_fail(Json_Parser *p, String why)
{
    if (!p->failed) {
        p->failed = 1;
        p->error  = why;
    }
}

FR_BASE void json_skip_ws(Json_Parser *p)
{
    while (p->at < p->text.len && fr_is_space(p->text.data[p->at])) ++p->at;
}

FR_BASE char json_peek_char(Json_Parser *p)
{
    json_skip_ws(p);
    if (p->at >= p->text.len) return 0;
    return p->text.data[p->at];
}

FR_BASE b32 json_expect(Json_Parser *p, char c)
{
    if (json_peek_char(p) != c) {
        json_fail(p, str_lit("unexpected character in JSON"));
        return 0;
    }
    ++p->at;
    return 1;
}

FR_BASE Json_Kind json_peek(Json_Parser *p)
{
    char c;
    if (p->failed) return JSON_INVALID;
    c = json_peek_char(p);
    switch (c) {
    case '{': return JSON_OBJECT;
    case '[': return JSON_ARRAY;
    case '"': return JSON_STRING;
    case 't': case 'f': return JSON_BOOL;
    case 'n': return JSON_NULL;
    case 0: return JSON_INVALID;
    default:
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) return JSON_NUMBER;
        return JSON_INVALID;
    }
}

FR_BASE b32 json_object_begin(Json_Parser *p)
{
    if (p->failed) return 0;
    return json_expect(p, '{');
}

FR_BASE b32 json_array_begin(Json_Parser *p)
{
    if (p->failed) return 0;
    return json_expect(p, '[');
}

FR_BASE String json_string_raw(Json_Parser *p)
{
    i32 start;
    if (p->failed) return str_nil;
    if (!json_expect(p, '"')) return str_nil;
    start = p->at;
    while (p->at < p->text.len) {
        char c = p->text.data[p->at];
        if (c == '\\') {
            p->at += 2; /* whatever it escapes, it is not a closing quote */
            continue;
        }
        if (c == '"') {
            String out = str_slice(p->text, start, p->at - start);
            ++p->at;
            return out;
        }
        ++p->at;
    }
    json_fail(p, str_lit("unterminated JSON string"));
    return str_nil;
}

FR_BASE b32 json_object_next(Json_Parser *p, String *out_key)
{
    char c;
    if (p->failed) return 0;
    c = json_peek_char(p);
    if (c == ',') {
        ++p->at;
        c = json_peek_char(p);
    }
    if (c == '}') {
        ++p->at;
        return 0;
    }
    /* Keys in every schema we read are plain ASCII, so the raw bytes are
     * the key and no unescaping copy is needed. */
    *out_key = json_string_raw(p);
    if (p->failed) return 0;
    if (!json_expect(p, ':')) return 0;
    return 1;
}

FR_BASE b32 json_array_next(Json_Parser *p)
{
    char c;
    if (p->failed) return 0;
    c = json_peek_char(p);
    if (c == ',') {
        ++p->at;
        c = json_peek_char(p);
    }
    if (c == ']') {
        ++p->at;
        return 0;
    }
    if (c == 0) {
        json_fail(p, str_lit("unterminated JSON array"));
        return 0;
    }
    return 1;
}

FR_BASE b32 json_is_scalar_char(char c)
{
    return !(fr_is_space(c) || c == ',' || c == '}' || c == ']' || c == ':');
}

FR_BASE String json_scalar_token(Json_Parser *p)
{
    i32 start;
    json_skip_ws(p);
    start = p->at;
    while (p->at < p->text.len && json_is_scalar_char(p->text.data[p->at])) {
        ++p->at;
    }
    if (p->at == start) {
        json_fail(p, str_lit("expected a JSON value"));
        return str_nil;
    }
    return str_slice(p->text, start, p->at - start);
}

FR_BASE void json_skip(Json_Parser *p)
{
    i32 depth = 0;
    for (;;) {
        char c;
        if (p->failed) return;
        c = json_peek_char(p);
        if (c == 0) {
            if (depth != 0) json_fail(p, str_lit("truncated JSON"));
            return;
        }
        if (c == '{' || c == '[') {
            ++p->at;
            ++depth;
        } else if (c == '}' || c == ']') {
            ++p->at;
            if (depth > 0) --depth;
        } else if (c == '"') {
            (void)json_string_raw(p);
        } else if (c == ',' || c == ':') {
            ++p->at;
        } else {
            (void)json_scalar_token(p);
        }
        if (depth == 0) return;
    }
}

/* \uXXXX, as UTF-8, surrogate pairs joined. Returns bytes written. */
FR_BASE i32 json_encode_utf8(u32 code_point, u8 *out)
{
    if (code_point <= 0x7F) {
        out[0] = (u8)code_point;
        return 1;
    }
    if (code_point <= 0x7FF) {
        out[0] = (u8)(0xC0 | (code_point >> 6));
        out[1] = (u8)(0x80 | (code_point & 0x3F));
        return 2;
    }
    if (code_point <= 0xFFFF) {
        out[0] = (u8)(0xE0 | (code_point >> 12));
        out[1] = (u8)(0x80 | ((code_point >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (code_point & 0x3F));
        return 3;
    }
    out[0] = (u8)(0xF0 | (code_point >> 18));
    out[1] = (u8)(0x80 | ((code_point >> 12) & 0x3F));
    out[2] = (u8)(0x80 | ((code_point >> 6) & 0x3F));
    out[3] = (u8)(0x80 | (code_point & 0x3F));
    return 4;
}

FR_BASE b32 json_hex4(String s, i32 at, u32 *out)
{
    u32 value = 0;
    i32 i;
    if (at + 4 > s.len) return 0;
    for (i = 0; i < 4; ++i) {
        char c = s.data[at + i];
        u32  digit;
        if (c >= '0' && c <= '9') digit = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (u32)(c - 'A' + 10);
        else return 0;
        value = (value << 4) | digit;
    }
    *out = value;
    return 1;
}

/* The escape handling, writing into a builder the caller already has
 * open. Shared so that a reader assembling text out of many JSON
 * strings — a streamed reply, one delta per event — can append straight
 * onto its own builder instead of copying through an arena it is not
 * allowed to allocate in while the builder is open. */
FR_BASE void json_append_unescaped(Json_Parser *p, String raw,
                                  String_Builder *sb)
{
    i32 i;

    for (i = 0; i < raw.len; ++i) {
        char c = raw.data[i];
        if (c != '\\') {
            sb_append_char(sb, c);
            continue;
        }
        if (++i >= raw.len) break;
        switch (raw.data[i]) {
        case '"':  sb_append_char(sb, '"');  break;
        case '\\': sb_append_char(sb, '\\'); break;
        case '/':  sb_append_char(sb, '/');  break;
        case 'b':  sb_append_char(sb, '\b'); break;
        case 'f':  sb_append_char(sb, '\f'); break;
        case 'n':  sb_append_char(sb, '\n'); break;
        case 'r':  sb_append_char(sb, '\r'); break;
        case 't':  sb_append_char(sb, '\t'); break;
        case 'u': {
            u32 code = 0;
            u8  utf8[4];
            i32 n;
            if (!json_hex4(raw, i + 1, &code)) {
                json_fail(p, str_lit("bad \\u escape in JSON string"));
                return;
            }
            i += 4;
            /* High surrogate: join with the low one that must follow. */
            if (code >= 0xD800 && code <= 0xDBFF && i + 6 < raw.len &&
                raw.data[i + 1] == '\\' && raw.data[i + 2] == 'u') {
                u32 low = 0;
                if (json_hex4(raw, i + 3, &low) && low >= 0xDC00 &&
                    low <= 0xDFFF) {
                    code = 0x10000u + ((code - 0xD800u) << 10) +
                           (low - 0xDC00u);
                    i += 6;
                }
            }
            n = json_encode_utf8(code, utf8);
            sb_append(sb, str_make((const char *)utf8, n));
        } break;
        default:
            json_fail(p, str_lit("unknown escape in JSON string"));
            return;
        }
    }
}

FR_BASE String json_string(Json_Parser *p, Arena *arena)
{
    String         raw = json_string_raw(p);
    String_Builder sb;

    if (p->failed) return str_nil;
    if (str_index_of_char(raw, '\\') < 0) return raw;

    sb = sb_begin(arena);
    json_append_unescaped(p, raw, &sb);
    if (p->failed) return str_nil;
    return sb_done(&sb);
}

FR_BASE void json_string_append(Json_Parser *p, String_Builder *sb)
{
    String raw = json_string_raw(p);
    if (p->failed || !sb) return;
    if (str_index_of_char(raw, '\\') < 0) {
        sb_append(sb, raw);
        return;
    }
    json_append_unescaped(p, raw, sb);
}

FR_BASE i64 json_i64(Json_Parser *p)
{
    i64    value = 0;
    String token;
    if (p->failed) return 0;
    /* protojson writes every 64-bit integer as a quoted string. */
    token = (json_peek_char(p) == '"') ? json_string_raw(p)
                                       : json_scalar_token(p);
    if (p->failed) return 0;
    if (!str_to_i64(token, &value)) {
        /* A float where an integer was expected: truncate rather than fail,
         * which is what a provider sending 3.0 for a count means. */
        f64 as_f64 = 0;
        if (str_to_f64(token, &as_f64)) return (i64)as_f64;
        json_fail(p, str_lit("expected an integer"));
        return 0;
    }
    return value;
}

FR_BASE f64 json_f64(Json_Parser *p)
{
    f64    value = 0;
    String token;
    if (p->failed) return 0;
    token = (json_peek_char(p) == '"') ? json_string_raw(p)
                                       : json_scalar_token(p);
    if (p->failed) return 0;
    if (!str_to_f64(token, &value)) {
        json_fail(p, str_lit("expected a number"));
        return 0;
    }
    return value;
}

FR_BASE b32 json_bool(Json_Parser *p)
{
    String token;
    if (p->failed) return 0;
    token = json_scalar_token(p);
    if (str_eq(token, str_lit("true"))) return 1;
    if (str_eq(token, str_lit("false"))) return 0;
    json_fail(p, str_lit("expected true or false"));
    return 0;
}

FR_BASE i64 json_base64(Json_Parser *p, u8 *out, size_t out_capacity)
{
    String raw = json_string_raw(p);
    if (p->failed) return -1;
    return fr_base64_decode(raw, out, out_capacity);
}

FR_BASE u64 json_timestamp_ms(Json_Parser *p)
{
    String raw = json_string_raw(p);
    i64    year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    i64    ms = 0, offset_minutes = 0;
    i32    at = 0;
    i64    total;

    if (p->failed) return 0;

    /* YYYY-MM-DDTHH:MM:SS, fixed width, so this is just slicing. */
    if (raw.len < 19 || !str_to_i64(str_slice(raw, 0, 4), &year) ||
        !str_to_i64(str_slice(raw, 5, 2), &month) ||
        !str_to_i64(str_slice(raw, 8, 2), &day) ||
        !str_to_i64(str_slice(raw, 11, 2), &hour) ||
        !str_to_i64(str_slice(raw, 14, 2), &minute) ||
        !str_to_i64(str_slice(raw, 17, 2), &second)) {
        json_fail(p, str_lit("expected an RFC 3339 timestamp"));
        return 0;
    }
    at = 19;

    if (at < raw.len && raw.data[at] == '.') {
        i64 scale = 100; /* first digit is hundreds of milliseconds */
        ++at;
        while (at < raw.len && raw.data[at] >= '0' && raw.data[at] <= '9') {
            if (scale > 0) {
                ms += (i64)(raw.data[at] - '0') * scale;
                scale /= 10;
            }
            ++at;
        }
    }

    if (at < raw.len && (raw.data[at] == '+' || raw.data[at] == '-')) {
        i64 sign = raw.data[at] == '-' ? -1 : 1;
        i64 off_hours = 0, off_minutes = 0;
        if (at + 6 > raw.len ||
            !str_to_i64(str_slice(raw, at + 1, 2), &off_hours) ||
            !str_to_i64(str_slice(raw, at + 4, 2), &off_minutes)) {
            json_fail(p, str_lit("expected an RFC 3339 timestamp"));
            return 0;
        }
        offset_minutes = sign * (off_hours * 60 + off_minutes);
    }

    total = fr_days_from_civil(year, month, day) * 86400 + hour * 3600 +
            minute * 60 + second - offset_minutes * 60;
    if (total < 0) return 0; /* before 1970: there is no unix_ms for it */
    return (u64)total * 1000u + (u64)ms;
}

/* ------------------------------------------------------------------ *
 *  Writing
 * ------------------------------------------------------------------ */

FR_BASE Json_Writer jw_begin(Arena *arena)
{
    Json_Writer w;
    w.sb        = sb_begin(arena);
    w.depth     = 0;
    w.has_item  = 0;
    w.after_key = 0;
    w.failed    = 0;
    return w;
}

/* A value that follows a key is part of that key's item, so it takes no
 * comma of its own. */
FR_BASE void jw_separator(Json_Writer *w)
{
    u32 bit;
    if (w->after_key) {
        w->after_key = 0;
        return;
    }
    if (w->depth <= 0) return;
    bit = 1u << (w->depth - 1);
    if (w->has_item & bit) sb_append_char(&w->sb, ',');
    w->has_item |= bit;
}

FR_BASE void jw_open(Json_Writer *w, char bracket)
{
    jw_separator(w);
    if (w->depth >= JSON_WRITER_MAX_DEPTH) {
        w->failed = 1;
        return;
    }
    sb_append_char(&w->sb, bracket);
    ++w->depth;
    w->has_item &= ~(1u << (w->depth - 1));
}

FR_BASE void jw_close(Json_Writer *w, char bracket)
{
    if (w->depth <= 0) {
        w->failed = 1;
        return;
    }
    --w->depth;
    sb_append_char(&w->sb, bracket);
}

FR_BASE void jw_object_begin(Json_Writer *w) { jw_open(w, '{'); }
FR_BASE void jw_object_end(Json_Writer *w) { jw_close(w, '}'); }
FR_BASE void jw_array_begin(Json_Writer *w) { jw_open(w, '['); }
FR_BASE void jw_array_end(Json_Writer *w) { jw_close(w, ']'); }

FR_BASE void jw_escaped(Json_Writer *w, String value)
{
    i32 i, run_start = 0;
    sb_append_char(&w->sb, '"');
    for (i = 0; i < value.len; ++i) {
        u8          c = (u8)value.data[i];
        const char *escape = 0;
        char        buffer[7];

        switch (c) {
        case '"':  escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\n': escape = "\\n";  break;
        case '\r': escape = "\\r";  break;
        case '\t': escape = "\\t";  break;
        case '\b': escape = "\\b";  break;
        case '\f': escape = "\\f";  break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                buffer[0] = '\\';
                buffer[1] = 'u';
                buffer[2] = '0';
                buffer[3] = '0';
                buffer[4] = hex[(c >> 4) & 0xF];
                buffer[5] = hex[c & 0xF];
                buffer[6] = 0;
                escape    = buffer;
            }
            break;
        }
        if (escape) {
            sb_append(&w->sb, str_slice(value, run_start, i - run_start));
            sb_append_cstr(&w->sb, escape);
            run_start = i + 1;
        }
    }
    sb_append(&w->sb, str_slice(value, run_start, value.len - run_start));
    sb_append_char(&w->sb, '"');
}

FR_BASE void jw_key(Json_Writer *w, String key)
{
    jw_separator(w);
    jw_escaped(w, key);
    sb_append_char(&w->sb, ':');
    w->after_key = 1;
}

FR_BASE void jw_string(Json_Writer *w, String value)
{
    jw_separator(w);
    jw_escaped(w, value);
}

FR_BASE void jw_i64(Json_Writer *w, i64 value)
{
    jw_separator(w);
    sb_append_i64(&w->sb, value);
}

FR_BASE void jw_f64(Json_Writer *w, f64 value)
{
    jw_separator(w);
    sb_appendf(&w->sb, "%.9g", value);
}

FR_BASE void jw_bool(Json_Writer *w, b32 value)
{
    jw_separator(w);
    sb_append_cstr(&w->sb, value ? "true" : "false");
}

FR_BASE void jw_null(Json_Writer *w)
{
    jw_separator(w);
    sb_append_cstr(&w->sb, "null");
}

/* A base64 string of two buffers taken as one, with something in front
 * of it inside the quotes. The prefix is written as-is and must not need
 * escaping. */
FR_BASE void jw_base64_parts(Json_Writer *w, String prefix, const u8 *a,
                            size_t a_count, const u8 *b, size_t b_count)
{
    char *dest;

    jw_separator(w);
    sb_append_char(&w->sb, '"');
    sb_append(&w->sb, prefix);
    dest = sb_reserve(&w->sb, fr_base64_encoded_size(a_count + b_count));
    if (dest) fr_base64_encode(a, a_count, b, b_count, dest);
    sb_append_char(&w->sb, '"');
}

FR_BASE void jw_base64(Json_Writer *w, const u8 *bytes, size_t byte_count)
{
    jw_base64_parts(w, str_nil, bytes, byte_count, 0, 0);
}

FR_BASE void jw_base64_join(Json_Writer *w, const u8 *a, size_t a_count, const u8 *b,
                    size_t b_count)
{
    jw_base64_parts(w, str_nil, a, a_count, b, b_count);
}

FR_BASE void jw_base64_prefixed(Json_Writer *w, String prefix, const u8 *bytes,
                        size_t byte_count)
{
    jw_base64_parts(w, prefix, bytes, byte_count, 0, 0);
}

FR_BASE void jw_field_string(Json_Writer *w, String key, String value)
{
    jw_key(w, key);
    jw_string(w, value);
}

FR_BASE void jw_field_i64(Json_Writer *w, String key, i64 value)
{
    jw_key(w, key);
    jw_i64(w, value);
}

FR_BASE void jw_field_f64(Json_Writer *w, String key, f64 value)
{
    jw_key(w, key);
    jw_f64(w, value);
}

FR_BASE void jw_field_bool(Json_Writer *w, String key, b32 value)
{
    jw_key(w, key);
    jw_bool(w, value);
}

FR_BASE String jw_done(Json_Writer *w)
{
    if (w->failed || w->sb.overflowed) return str_nil;
    return sb_done(&w->sb);
}
