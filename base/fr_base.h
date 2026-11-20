/* fr_base.h — the floor everything else stands on.
 *
 * Strings carry their length. Nothing in this library is null-terminated
 * and nothing calls strlen on its own data: a String is a view, so slicing
 * is free and a substring is not a copy. char* appears only where libc
 * forces it.
 *
 * Memory comes from arenas carved out of the one block the caller gave us
 * at startup. An arena cannot grow, so pushing can fail, and every push
 * site has to mean something sensible when it does.
 *
 * This is not part of the library's interface. The library compiles it
 * in, and so does any program that wants the same strings and arenas
 * (the SDL window, the tests): each gets its own copy, with internal
 * linkage, so nothing links against another program's internals. The
 * one thing it shares with the library is friend.h, so that a String
 * and a Friend_String are the same type.
 */

#ifndef FR_BASE_H
#define FR_BASE_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>  /* vsnprintf */
#include <string.h> /* memcpy, memset, memcmp */

#include "../libfriend/include/friend.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;
typedef i32      b32;

#define fr_array_count(a) ((i32)(sizeof(a) / sizeof((a)[0])))
#define fr_min(a, b) ((a) < (b) ? (a) : (b))
#define fr_max(a, b) ((a) > (b) ? (a) : (b))
#define fr_clamp(lo, x, hi) fr_min(fr_max(x, lo), hi)
#define fr_unused(x) ((void)(x))

/* Every function here. Each program uses some of them, so the ones it
 * does not use are not a warning. */
#if defined(__GNUC__) || defined(__clang__)
#define FR_BASE static __attribute__((unused))
#else
#define FR_BASE static
#endif

/* C99 has no alignof. sizeof a struct that pads T after a char is the
 * alignment of T. */
#define FR_ALIGNOF(T) ((size_t)(sizeof(struct { char c_; T t_; }) - sizeof(T)))

#define FR_KB(n) ((size_t)(n) * 1024u)
#define FR_MB(n) (FR_KB(n) * 1024u)

/* C99 has no static_assert. A negative array size is a compile error. */
#define FR_STATIC_ASSERT(cond, name) typedef char fr_assert_##name[(cond) ? 1 : -1]

/* Assertions are for our own bugs, never for bad input from a model, a
 * file, or a user: those get a Friend_Result. */
#ifndef FR_NO_ASSERT
FR_BASE void fr_assert_failed(const char *file, int line, const char *expr);
#define fr_assert(c)                                                          \
    do {                                                                      \
        if (!(c)) fr_assert_failed(__FILE__, __LINE__, #c);                   \
    } while (0)
#else
#define fr_assert(c) ((void)0)
#endif

/* ------------------------------------------------------------------ *
 *  String
 * ------------------------------------------------------------------ */

typedef Friend_String String;

#define str_lit(s) ((String){ (s), (i32)(sizeof(s) - 1) })

#define str_nil ((String){ 0, 0 })

static inline String str_make(const char *data, i32 len)
{
    String s;
    s.data = data;
    s.len  = len < 0 ? 0 : len;
    return s;
}

/* Only for literals and libc returns. */
static inline String str_from_cstr(const char *c)
{
    return str_make(c, c ? (i32)strlen(c) : 0);
}

static inline b32 str_is_empty(String s) { return s.len <= 0 || s.data == 0; }

static inline b32 str_eq(String a, String b)
{
    if (a.len != b.len) return 0;
    if (a.len == 0) return 1;
    return memcmp(a.data, b.data, (size_t)a.len) == 0;
}

static inline char fr_lower_char(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static inline b32 str_eq_ci(String a, String b)
{
    i32 i;
    if (a.len != b.len) return 0;
    for (i = 0; i < a.len; ++i) {
        if (fr_lower_char(a.data[i]) != fr_lower_char(b.data[i])) return 0;
    }
    return 1;
}

/* Clamped: a slice outside the string is empty, never undefined. */
static inline String str_slice(String s, i32 start, i32 len)
{
    if (start < 0) start = 0;
    if (start > s.len) start = s.len;
    if (len < 0) len = 0;
    if (start + len > s.len) len = s.len - start;
    return str_make(s.data + start, len);
}

static inline String str_skip(String s, i32 n) { return str_slice(s, n, s.len - n); }
static inline String str_take(String s, i32 n) { return str_slice(s, 0, n); }

/* Last n bytes. */
static inline String str_tail(String s, i32 n)
{
    if (n >= s.len) return s;
    return str_slice(s, s.len - n, n);
}

/* Cuts to at most max bytes without splitting a code point. A record has
 * a fixed amount of room and some descriptions will not fit it; half a
 * character is the one outcome that is worse than a short one. */
static inline String fr_truncate_utf8(String s, i32 max)
{
    i32 end = max;
    if (max <= 0) return str_nil;
    if (s.len <= max) return s;
    while (end > 0 && ((u8)s.data[end] & 0xC0u) == 0x80u) --end;
    return str_take(s, end);
}

static inline b32 str_has_prefix(String s, String prefix)
{
    return prefix.len <= s.len && str_eq(str_take(s, prefix.len), prefix);
}

static inline b32 str_has_suffix(String s, String suffix)
{
    return suffix.len <= s.len && str_eq(str_tail(s, suffix.len), suffix);
}

static inline i32 str_index_of_char(String s, char c)
{
    i32 i;
    for (i = 0; i < s.len; ++i) {
        if (s.data[i] == c) return i;
    }
    return -1;
}

static inline i32 str_index_of(String haystack, String needle)
{
    i32 i;
    if (needle.len == 0) return 0;
    if (needle.len > haystack.len) return -1;
    for (i = 0; i <= haystack.len - needle.len; ++i) {
        if (memcmp(haystack.data + i, needle.data, (size_t)needle.len) == 0) {
            return i;
        }
    }
    return -1;
}

static inline b32 fr_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static inline String str_trim(String s)
{
    i32 start = 0, end = s.len;
    while (start < end && fr_is_space(s.data[start])) ++start;
    while (end > start && fr_is_space(s.data[end - 1])) --end;
    return str_slice(s, start, end - start);
}

/* Line iterator. Consumes *rest and hands back the line without its
 * terminator; returns 0 when rest is exhausted. Handles CRLF. */
static inline b32 str_next_line(String *rest, String *out_line)
{
    i32 nl;
    if (rest->len <= 0) return 0;
    nl = str_index_of_char(*rest, '\n');
    if (nl < 0) {
        *out_line = *rest;
        *rest     = str_nil;
    } else {
        i32 end   = nl;
        if (end > 0 && rest->data[end - 1] == '\r') --end;
        *out_line = str_take(*rest, end);
        *rest     = str_skip(*rest, nl + 1);
    }
    return 1;
}

/* Parsers return 0 and leave *out alone on anything unparseable. Decimal
 * only; JSON exponents are handled by str_to_f64. */
static inline b32 str_to_i64(String s, i64 *out)
{
    i64 sign = 1, value = 0;
    i32 i = 0;
    s = str_trim(s);
    if (s.len == 0) return 0;
    if (s.data[0] == '-') { sign = -1; i = 1; }
    else if (s.data[0] == '+') { i = 1; }
    if (i >= s.len) return 0;
    for (; i < s.len; ++i) {
        char c = s.data[i];
        if (c < '0' || c > '9') return 0;
        value = value * 10 + (c - '0');
    }
    *out = sign * value;
    return 1;
}

static inline b32 str_to_f64(String s, f64 *out)
{
    f64 sign = 1.0, value = 0.0;
    i32 i = 0;
    s = str_trim(s);
    if (s.len == 0) return 0;
    if (s.data[0] == '-') { sign = -1.0; i = 1; }
    else if (s.data[0] == '+') { i = 1; }
    if (i >= s.len) return 0;
    while (i < s.len && s.data[i] >= '0' && s.data[i] <= '9') {
        value = value * 10.0 + (f64)(s.data[i] - '0');
        ++i;
    }
    if (i < s.len && s.data[i] == '.') {
        f64 scale = 0.1;
        ++i;
        while (i < s.len && s.data[i] >= '0' && s.data[i] <= '9') {
            value += (f64)(s.data[i] - '0') * scale;
            scale *= 0.1;
            ++i;
        }
    }
    if (i < s.len && (s.data[i] == 'e' || s.data[i] == 'E')) {
        i64 exp      = 0;
        f64 exp_sign = 1.0;
        ++i;
        if (i < s.len && (s.data[i] == '-' || s.data[i] == '+')) {
            if (s.data[i] == '-') exp_sign = -1.0;
            ++i;
        }
        if (i >= s.len) return 0;
        while (i < s.len && s.data[i] >= '0' && s.data[i] <= '9') {
            exp = exp * 10 + (s.data[i] - '0');
            ++i;
        }
        while (exp-- > 0) {
            if (exp_sign > 0) value *= 10.0;
            else value *= 0.1;
        }
    }
    if (i != s.len) return 0;
    *out = sign * value;
    return 1;
}

/* ------------------------------------------------------------------ *
 *  Little-endian bytes
 *
 *  For everything that lands in a file or a packed stream: byte order and
 *  alignment are not things a format should inherit from whatever
 *  compiled it.
 * ------------------------------------------------------------------ */

static inline void fr_le_put16(u8 *p, u16 value)
{
    p[0] = (u8)(value & 0xFFu);
    p[1] = (u8)(value >> 8);
}

static inline void fr_le_put32(u8 *p, u32 value)
{
    i32 i;
    for (i = 0; i < 4; ++i) p[i] = (u8)(value >> (i * 8));
}

static inline void fr_le_put64(u8 *p, u64 value)
{
    i32 i;
    for (i = 0; i < 8; ++i) p[i] = (u8)(value >> (i * 8));
}

static inline u16 fr_le_get16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static inline u32 fr_le_get32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static inline u64 fr_le_get64(const u8 *p)
{
    return (u64)fr_le_get32(p) | ((u64)fr_le_get32(p + 4) << 32);
}

/* ------------------------------------------------------------------ *
 *  Pages
 *
 *  Every region in the block is reserved up front and may or may not be
 *  real memory yet. When the caller gave us plain memory it is all real
 *  and none of this does anything. When the caller gave us reserved
 *  address space and a commit function, a region is backed as far as
 *  anything has reached into it and no further, which is what lets the
 *  default limits be generous.
 *
 *  Committing is in chunks, so growing by a record at a time does not
 *  mean a system call per record. 64 KiB is Windows' allocation
 *  granularity and sixteen pages everywhere else.
 * ------------------------------------------------------------------ */

#define FR_COMMIT_CHUNK FR_KB(64)

static inline size_t fr_align_up(size_t value, size_t align)
{
    size_t mask;
    if (align < 1) align = 1;
    mask = align - 1;
    return (value + mask) & ~mask;
}

/* Make sure the first `need` bytes from base are memory. *committed is
 * the watermark, which only ever moves forward. Returns 0 only when the
 * commit was refused, which means the machine is out and the caller has
 * to fail the way it fails when a capacity runs out. */
static inline b32 fr_pages_reach(Friend_Commit_Fn commit, void *user, u8 *base,
                                 size_t *committed, size_t size, size_t need)
{
    size_t want;
    if (need <= *committed) return 1;
    if (need > size) return 0;
    if (!commit) return 1; /* plain memory: all of it is already there */

    want = fr_align_up(need, FR_COMMIT_CHUNK);
    if (want > size) want = size;
    if (!commit(user, base + *committed, want - *committed)) return 0;
    *committed = want;
    return 1;
}

/* A region of records, indexed rather than pushed: the memory graph, the
 * edges, the scrollback. An arena is the other kind. */
typedef struct Fr_Pages {
    u8              *base;
    size_t           size;      /* reserved */
    size_t           committed; /* backed; == size for plain memory */
    Friend_Commit_Fn commit;
    void            *commit_user;
} Fr_Pages;

static inline Fr_Pages fr_pages_make(void *base, size_t size,
                                     Friend_Commit_Fn commit, void *user)
{
    Fr_Pages p;
    p.base        = (u8 *)base;
    p.size        = size;
    p.committed   = commit ? 0 : size;
    p.commit      = commit;
    p.commit_user = user;
    return p;
}

static inline b32 fr_pages_need(Fr_Pages *p, size_t bytes)
{
    return fr_pages_reach(p->commit, p->commit_user, p->base, &p->committed,
                          p->size, bytes);
}

/* ------------------------------------------------------------------ *
 *  Arena
 *
 *  A bump allocator over memory we were handed. No free; you reset to a
 *  mark, or you reset the whole thing. Sub-arenas carve a region out for
 *  good, which is how the fixed layout in fr_state.h is built.
 * ------------------------------------------------------------------ */

typedef struct Arena {
    u8    *base;
    size_t size;
    size_t used;
    size_t peak; /* high-water mark, reported through Friend_Stats */

    /* An arena over reserved space grows its backing as it is pushed
     * into; it never gives it back, because a reset is cheap and a
     * decommit is a system call and a fresh page fault for the next
     * tick that wanted the same kilobyte. */
    size_t           committed;
    Friend_Commit_Fn commit;
    void            *commit_user;
} Arena;

typedef size_t Arena_Mark;

static inline Arena arena_make(void *base, size_t size)
{
    Arena a;
    a.base        = (u8 *)base;
    a.size        = size;
    a.used        = 0;
    a.peak        = 0;
    a.committed   = size;
    a.commit      = 0;
    a.commit_user = 0;
    return a;
}

/* The same arena over space that is reserved and not yet real. */
static inline Arena arena_make_reserved(void *base, size_t size,
                                        Friend_Commit_Fn commit, void *user)
{
    Arena a = arena_make(base, size);
    if (commit) {
        a.committed   = 0;
        a.commit      = commit;
        a.commit_user = user;
    }
    return a;
}

/* Returns 0 when the arena is exhausted. Callers must check. */
static inline void *arena_push_size_nz(Arena *a, size_t size, size_t align)
{
    size_t start = fr_align_up(a->used, align);
    if (size > a->size || start > a->size - size) return 0;
    if (start + size > a->committed &&
        !fr_pages_reach(a->commit, a->commit_user, a->base, &a->committed,
                        a->size, start + size)) {
        return 0;
    }
    a->used = start + size;
    if (a->used > a->peak) a->peak = a->used;
    return a->base + start;
}

static inline void *arena_push_size(Arena *a, size_t size, size_t align)
{
    void *p = arena_push_size_nz(a, size, align);
    if (p) memset(p, 0, size);
    return p;
}

#define arena_push(a, T, n)                                                   \
    ((T *)arena_push_size((a), sizeof(T) * (size_t)(n), FR_ALIGNOF(T)))
#define arena_push_nz(a, T, n)                                                \
    ((T *)arena_push_size_nz((a), sizeof(T) * (size_t)(n), FR_ALIGNOF(T)))

static inline size_t arena_remaining(Arena *a) { return a->size - a->used; }

static inline Arena_Mark arena_mark(Arena *a) { return a->used; }

static inline void arena_reset_to(Arena *a, Arena_Mark mark)
{
    fr_assert(mark <= a->used);
    a->used = mark;
}

static inline void arena_reset(Arena *a) { a->used = 0; }

/* Permanently carve a region out of a. The parent cannot reclaim it.
 *
 * Addresses only: the slice is not touched, not zeroed, and not committed
 * here, so carving a 64 MiB arena into slices costs nothing and each
 * slice backs itself as it is used. Whatever the parent had already
 * committed over the slice's range stays committed and the child knows
 * it. The parent's own watermark stops meaning anything once it has
 * children, which is fine: an arena is either carved up or pushed into,
 * never both. */
static inline Arena arena_sub(Arena *a, size_t size, size_t align)
{
    size_t start = fr_align_up(a->used, align);
    Arena  out;

    if (size > a->size || start > a->size - size) return arena_make(0, 0);
    a->used = start + size;
    if (a->used > a->peak) a->peak = a->used;

    out = arena_make_reserved(a->base + start, size, a->commit,
                              a->commit_user);
    if (a->commit && a->committed > start) {
        size_t already = a->committed - start;
        out.committed  = already < size ? already : size;
    }
    return out;
}

/* Make the whole arena real at once. For memory that is going to be
 * handed to something that writes into it without pushing — a library
 * given a fixed-size block to live in — where the arena cannot see the
 * writes coming and so cannot back them. */
static inline b32 arena_commit_all(Arena *a)
{
    return fr_pages_reach(a->commit, a->commit_user, a->base, &a->committed,
                          a->size, a->size);
}

static inline String str_push_copy(Arena *a, String s)
{
    u8 *bytes;
    if (s.len <= 0) return str_nil;
    bytes = arena_push_nz(a, u8, s.len);
    if (!bytes) return str_nil;
    memcpy(bytes, s.data, (size_t)s.len);
    return str_make((const char *)bytes, s.len);
}

/* ------------------------------------------------------------------ *
 *  String_Builder
 *
 *  Builds in place at the tail of an arena, so the result is a String view
 *  with no copy at the end. Nothing else may push to that arena while a
 *  builder is open on it.
 * ------------------------------------------------------------------ */

typedef struct String_Builder {
    Arena *arena;
    u8    *start;
    i32    len;
    b32    overflowed; /* sticky: the result is a truncated prefix */
} String_Builder;

static inline String_Builder sb_begin(Arena *arena)
{
    String_Builder sb;
    sb.arena      = arena;
    sb.start      = arena->base + arena->used;
    sb.len        = 0;
    sb.overflowed = 0;
    return sb;
}

/* The next `count` bytes of the string, for a caller that writes them
 * itself — an encoder that produces straight into place. 0, and the
 * builder marked overflowed, when the arena has no room. */
static inline char *sb_reserve(String_Builder *sb, size_t count)
{
    char *dest;
    /* The rule above, enforced: a builder owns the tail of its arena,
     * and anything else pushed into that arena while one is open lands
     * in the middle of the string. Worth an assert because the symptom
     * is a request body that begins with half a header, two hundred
     * lines away from the cause. */
    fr_assert(sb->start + sb->len == sb->arena->base + sb->arena->used);
    if (count > 0x7FFFFFFFu - (size_t)sb->len) {
        sb->overflowed = 1;
        return 0;
    }
    dest = (char *)arena_push_size_nz(sb->arena, count, 1);
    if (!dest) {
        sb->overflowed = 1;
        return 0;
    }
    sb->len += (i32)count;
    return dest;
}

static inline void sb_append(String_Builder *sb, String s)
{
    char *dest;
    if (s.len <= 0) return;
    dest = sb_reserve(sb, (size_t)s.len);
    if (dest) memcpy(dest, s.data, (size_t)s.len);
}

static inline void sb_append_char(String_Builder *sb, char c)
{
    sb_append(sb, str_make(&c, 1));
}

static inline void sb_append_cstr(String_Builder *sb, const char *c)
{
    sb_append(sb, str_from_cstr(c));
}

static inline void sb_append_i64(String_Builder *sb, i64 value)
{
    char  tmp[24];
    i32   n = 0;
    u64   mag;
    b32   negative = value < 0;
    mag = negative ? (u64)(-(value + 1)) + 1u : (u64)value;
    do {
        tmp[n++] = (char)('0' + (mag % 10));
        mag /= 10;
    } while (mag > 0 && n < (i32)sizeof(tmp));
    if (negative) sb_append_char(sb, '-');
    while (n-- > 0) sb_append_char(sb, tmp[n]);
}

/* vsnprintf writes straight into the arena tail. Two passes so we never
 * guess a size: measure, then push exactly that much. */
static inline void sb_appendf(String_Builder *sb, const char *fmt, ...)
{
    va_list args, measure;
    int     needed;
    char   *dest;

    va_start(args, fmt);
    va_copy(measure, args);
    needed = vsnprintf(0, 0, fmt, measure);
    va_end(measure);

    if (needed <= 0) {
        va_end(args);
        return;
    }
    /* +1 for the null vsnprintf insists on writing, then we drop it. */
    dest = (char *)arena_push_size_nz(sb->arena, (size_t)needed + 1, 1);
    if (!dest) {
        sb->overflowed = 1;
        va_end(args);
        return;
    }
    vsnprintf(dest, (size_t)needed + 1, fmt, args);
    va_end(args);
    sb->arena->used -= 1; /* give back the null */
    sb->len += needed;
}

static inline String sb_done(String_Builder *sb)
{
    return str_make((const char *)sb->start, sb->len);
}

/* ------------------------------------------------------------------ *
 *  Base64
 *
 *  The export inlines images and embedding vectors as base64, so decoding
 *  is on the import path and encoding is on the request path.
 * ------------------------------------------------------------------ */

FR_BASE size_t fr_base64_decoded_size(String encoded);
/* Returns bytes written, or -1 if the input is not valid base64.
 * Accepts both standard and URL-safe alphabets, and missing padding. */
FR_BASE i64 fr_base64_decode(String encoded, u8 *out, size_t out_capacity);
FR_BASE size_t fr_base64_encoded_size(size_t byte_count);
/* Two buffers encoded as if they were one; b_count 0 for just the one. A
 * WAV file is a header and the samples, the header is 44 bytes, and 44 is
 * not a multiple of three — so encoding the two separately and joining
 * the results is wrong, and copying them together to avoid that is a
 * megabyte of memmove for nothing. */
FR_BASE void fr_base64_encode(const u8 *a, size_t a_count, const u8 *b,
                      size_t b_count, char *out);

/* ------------------------------------------------------------------ *
 *  Time, rendered the way the friend's prompts expect
 * ------------------------------------------------------------------ */

/* "3 days", "2 hours and 14 minutes", "less than a second" — the shape the
 * service used everywhere it told the model how long ago something was.
 * Appended, because a builder owns the tail of its arena and the duration
 * is always part of a longer line; the second form is that line alone. */
FR_BASE void   fr_append_human_duration(String_Builder *sb, i64 milliseconds);
FR_BASE String fr_human_duration(Arena *arena, i64 milliseconds);

typedef struct Fr_Date_Time {
    i32 year, month, day;    /* month 1-12, day 1-31 */
    i32 hour, minute, second; /* hour 0-23 */
    i32 weekday;             /* 0 = Sunday */
} Fr_Date_Time;

/* Civil calendar from a unix timestamp, with a fixed UTC offset. No libc
 * time zones: the caller supplies the offset it wants. */
FR_BASE Fr_Date_Time fr_date_time(u64 unix_ms, i32 utc_offset_minutes);

/* The other direction: days since 1970-01-01 for a civil date, month
 * 1-12. Negative before the epoch. */
FR_BASE i64 fr_days_from_civil(i64 year, i64 month, i64 day);

/* "1/2/2006 3:04:05 PM" and "Monday", the two formats the prompts use. */
FR_BASE void   fr_append_timestamp(String_Builder *sb, u64 unix_ms,
                           i32 utc_offset_minutes, b32 with_seconds);
FR_BASE String fr_format_timestamp(Arena *arena, u64 unix_ms, i32 utc_offset_minutes,
                           b32 with_seconds);
FR_BASE String fr_format_weekday(Fr_Date_Time dt);
FR_BASE String fr_format_month(Fr_Date_Time dt);

/* "November, Monday, 11/2/2026 1:17:04 PM UTC" — the time address the
 * service stamped on every line of dialogue it put in front of the model.
 * We only know an offset, not a zone, so the name is UTC or UTC+HH:MM. */
FR_BASE void fr_append_time_address(String_Builder *sb, u64 unix_ms,
                            i32 utc_offset_minutes);

#endif /* FR_BASE_H */
