/* fr_base.c — the parts of the base layer that are not worth inlining:
 * base64, and time formatted the way the friend's prompts expect it. */

#include "fr_base.h"

#ifndef FR_NO_ASSERT
#include <stdlib.h>

/* Overridable: a frontend that would rather log and keep running can
 * define FR_NO_ASSERT, and one that wants a breakpoint can replace this. */
FR_BASE void fr_assert_failed(const char *file, int line, const char *expr)
{
    fprintf(stderr, "friend: assertion failed: %s\n  at %s:%d\n", expr, file,
            line);
    abort();
}
#endif

/* ------------------------------------------------------------------ *
 *  Base64
 * ------------------------------------------------------------------ */

static const char fr_base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* -1 unknown, -2 whitespace (skipped), -3 padding. */
FR_BASE i32 fr_base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62; /* '-' is the URL-safe form */
    if (c == '/' || c == '_') return 63; /* '_' is the URL-safe form */
    if (c == '=') return -3;
    if (fr_is_space(c)) return -2;
    return -1;
}

FR_BASE size_t fr_base64_decoded_size(String encoded)
{
    size_t significant = 0;
    i32    i;
    for (i = 0; i < encoded.len; ++i) {
        if (fr_base64_value(encoded.data[i]) >= 0) ++significant;
    }
    return (significant / 4) * 3 + (significant % 4) * 3 / 4;
}

FR_BASE i64 fr_base64_decode(String encoded, u8 *out, size_t out_capacity)
{
    u32    accum     = 0;
    i32    bits      = 0;
    size_t written   = 0;
    i32    i;

    for (i = 0; i < encoded.len; ++i) {
        i32 value = fr_base64_value(encoded.data[i]);
        if (value == -2 || value == -3) continue;
        if (value < 0) return -1;

        accum = (accum << 6) | (u32)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= out_capacity) return -1;
            out[written++] = (u8)((accum >> bits) & 0xFFu);
        }
    }
    /* Leftover bits must be zero padding, not dropped data. */
    if (bits >= 6) return -1;
    if (bits > 0 && (accum & ((1u << bits) - 1u)) != 0) return -1;
    return (i64)written;
}

FR_BASE void fr_base64_encode(const u8 *a, size_t a_count, const u8 *b,
                      size_t b_count, char *out)
{
    size_t total = a_count + b_count;
    size_t i     = 0;
    size_t o     = 0;

    while (i < total) {
        u32    triple = 0;
        size_t have   = 0;
        size_t k;
        for (k = 0; k < 3; ++k) {
            u32 byte = 0;
            if (i + k < total) {
                size_t at = i + k;
                byte      = at < a_count ? a[at] : b[at - a_count];
                have += 1;
            }
            triple |= byte << (16 - 8 * k);
        }
        out[o++] = fr_base64_alphabet[(triple >> 18) & 63];
        out[o++] = fr_base64_alphabet[(triple >> 12) & 63];
        out[o++] = have > 1 ? fr_base64_alphabet[(triple >> 6) & 63] : '=';
        out[o++] = have > 2 ? fr_base64_alphabet[triple & 63] : '=';
        i += 3;
    }
}

FR_BASE size_t fr_base64_encoded_size(size_t byte_count)
{
    return ((byte_count + 2) / 3) * 4;
}

/* ------------------------------------------------------------------ *
 *  Durations
 *
 *  Reimplements the service's humanDuration exactly, because it is baked
 *  into every line of recalled memory the model reads: get it wrong and
 *  the friend's sense of time shifts.
 * ------------------------------------------------------------------ */

typedef struct Fr_Duration_Unit {
    const char *name;
    i64         ms;
} Fr_Duration_Unit;

static const Fr_Duration_Unit fr_duration_units[] = {
    { "year",   (i64)1000 * 60 * 60 * 24 * 365 },
    { "month",  (i64)1000 * 60 * 60 * 24 * 30 },
    { "week",   (i64)1000 * 60 * 60 * 24 * 7 },
    { "day",    (i64)1000 * 60 * 60 * 24 },
    { "hour",   (i64)1000 * 60 * 60 },
    { "minute", (i64)1000 * 60 },
    { "second", (i64)1000 },
};

FR_BASE void fr_append_count_unit(String_Builder *sb, i64 count,
                                 const char *unit)
{
    if (count == 1) {
        sb_append_cstr(sb, "1 ");
        sb_append_cstr(sb, unit);
    } else {
        sb_append_i64(sb, count);
        sb_append_char(sb, ' ');
        sb_append_cstr(sb, unit);
        sb_append_char(sb, 's');
    }
}

FR_BASE f64 fr_fabs(f64 x) { return x < 0 ? -x : x; }

FR_BASE i64 fr_round_i64(f64 x)
{
    return x < 0 ? -(i64)(-x + 0.5) : (i64)(x + 0.5);
}

FR_BASE void fr_append_human_duration(String_Builder *sb, i64 milliseconds)
{
    const Fr_Duration_Unit *primary = 0;
    f64                     count   = 0;
    i32                     i;
    i64                     remaining;
    i32                     parts = 0;

    if (milliseconds < 1000) {
        sb_append_cstr(sb, "less than a second");
        return;
    }

    for (i = 0; i < fr_array_count(fr_duration_units); ++i) {
        count = (f64)milliseconds / (f64)fr_duration_units[i].ms;
        if (count >= 0.95) {
            primary = &fr_duration_units[i];
            break;
        }
    }
    if (!primary) {
        primary = &fr_duration_units[fr_array_count(fr_duration_units) - 1];
        count   = (f64)milliseconds / (f64)primary->ms;
    }

    if (count > 0) {
        i64 rounded = fr_round_i64(count);
        if (fr_fabs(count - (f64)rounded) < 0.1) {
            fr_append_count_unit(sb, rounded, primary->name);
            return;
        }
    }

    /* Otherwise the two largest whole units: "2 hours and 14 minutes". */
    remaining = milliseconds;
    for (i = 0; i < fr_array_count(fr_duration_units); ++i) {
        const Fr_Duration_Unit *unit = &fr_duration_units[i];
        if (remaining >= unit->ms) {
            i64 whole = remaining / unit->ms;
            remaining -= whole * unit->ms;
            if (parts > 0) sb_append_cstr(sb, " and ");
            fr_append_count_unit(sb, whole, unit->name);
            ++parts;
            if (parts == 2 || remaining == 0) break;
        }
    }
    if (parts == 0) sb_append_cstr(sb, "less than a second");
}

FR_BASE String fr_human_duration(Arena *arena, i64 milliseconds)
{
    String_Builder sb = sb_begin(arena);
    fr_append_human_duration(&sb, milliseconds);
    return sb_done(&sb);
}

/* ------------------------------------------------------------------ *
 *  Civil calendar
 *
 *  No libc time zones. The caller passes the offset it wants, which is the
 *  only honest thing to do when the state file may have moved machines.
 * ------------------------------------------------------------------ */

FR_BASE Fr_Date_Time fr_date_time(u64 unix_ms, i32 utc_offset_minutes)
{
    Fr_Date_Time dt;
    i64 total_seconds = (i64)(unix_ms / 1000u) + (i64)utc_offset_minutes * 60;
    i64 days          = total_seconds / 86400;
    i64 seconds_of_day = total_seconds % 86400;
    i64 era, year_of_era, day_of_year, month_prime, day, month, year;
    u64 era_day;

    if (seconds_of_day < 0) {
        seconds_of_day += 86400;
        days -= 1;
    }

    dt.hour   = (i32)(seconds_of_day / 3600);
    dt.minute = (i32)((seconds_of_day % 3600) / 60);
    dt.second = (i32)(seconds_of_day % 60);

    /* 1970-01-01 was a Thursday. */
    dt.weekday = (i32)(((days % 7) + 11) % 7);

    /* Hinnant's civil_from_days, shifted to a March-based year. */
    days += 719468;
    era         = (days >= 0 ? days : days - 146096) / 146097;
    era_day     = (u64)(days - era * 146097);
    year_of_era = (i64)((era_day - era_day / 1460 + era_day / 36524 - era_day / 146096) / 365);
    year        = year_of_era + era * 400;
    day_of_year = (i64)era_day - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    month_prime = (5 * day_of_year + 2) / 153;
    day         = day_of_year - (153 * month_prime + 2) / 5 + 1;
    month       = month_prime + (month_prime < 10 ? 3 : -9);
    if (month <= 2) year += 1;

    dt.year  = (i32)year;
    dt.month = (i32)month;
    dt.day   = (i32)day;
    return dt;
}

/* Hinnant's days_from_civil, the inverse of the above. */
FR_BASE i64 fr_days_from_civil(i64 year, i64 month, i64 day)
{
    i64 era, year_of_era, day_of_year, day_of_era;
    year -= month <= 2;
    era         = (year >= 0 ? year : year - 399) / 400;
    year_of_era = year - era * 400;
    day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    day_of_era  = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
                 day_of_year;
    return era * 146097 + day_of_era - 719468;
}

FR_BASE String fr_format_weekday(Fr_Date_Time dt)
{
    switch (dt.weekday) {
    case 0: return str_lit("Sunday");
    case 1: return str_lit("Monday");
    case 2: return str_lit("Tuesday");
    case 3: return str_lit("Wednesday");
    case 4: return str_lit("Thursday");
    case 5: return str_lit("Friday");
    default: return str_lit("Saturday");
    }
}

FR_BASE String fr_format_month(Fr_Date_Time dt)
{
    switch (dt.month) {
    case 1: return str_lit("January");
    case 2: return str_lit("February");
    case 3: return str_lit("March");
    case 4: return str_lit("April");
    case 5: return str_lit("May");
    case 6: return str_lit("June");
    case 7: return str_lit("July");
    case 8: return str_lit("August");
    case 9: return str_lit("September");
    case 10: return str_lit("October");
    case 11: return str_lit("November");
    default: return str_lit("December");
    }
}

FR_BASE void fr_append_timestamp(String_Builder *sb, u64 unix_ms,
                         i32 utc_offset_minutes, b32 with_seconds)
{
    Fr_Date_Time dt     = fr_date_time(unix_ms, utc_offset_minutes);
    i32          hour12 = dt.hour % 12;
    if (hour12 == 0) hour12 = 12;

    sb_append_i64(sb, dt.month);
    sb_append_char(sb, '/');
    sb_append_i64(sb, dt.day);
    sb_append_char(sb, '/');
    sb_append_i64(sb, dt.year);
    sb_append_char(sb, ' ');
    sb_append_i64(sb, hour12);
    sb_append_char(sb, ':');
    if (dt.minute < 10) sb_append_char(sb, '0');
    sb_append_i64(sb, dt.minute);
    if (with_seconds) {
        sb_append_char(sb, ':');
        if (dt.second < 10) sb_append_char(sb, '0');
        sb_append_i64(sb, dt.second);
    }
    sb_append_cstr(sb, dt.hour < 12 ? " AM" : " PM");
}

FR_BASE String fr_format_timestamp(Arena *arena, u64 unix_ms, i32 utc_offset_minutes,
                           b32 with_seconds)
{
    String_Builder sb = sb_begin(arena);
    fr_append_timestamp(&sb, unix_ms, utc_offset_minutes, with_seconds);
    return sb_done(&sb);
}

FR_BASE void fr_append_time_address(String_Builder *sb, u64 unix_ms,
                            i32 utc_offset_minutes)
{
    Fr_Date_Time dt = fr_date_time(unix_ms, utc_offset_minutes);

    sb_append(sb, fr_format_month(dt));
    sb_append_cstr(sb, ", ");
    sb_append(sb, fr_format_weekday(dt));
    sb_append_cstr(sb, ", ");
    fr_append_timestamp(sb, unix_ms, utc_offset_minutes, 1);
    sb_append_cstr(sb, " UTC");

    /* The service named a real zone here because it had one. We have an
     * offset, so we say the offset rather than guess a name. */
    if (utc_offset_minutes != 0) {
        i32 magnitude = utc_offset_minutes < 0 ? -utc_offset_minutes
                                               : utc_offset_minutes;
        sb_append_char(sb, utc_offset_minutes < 0 ? '-' : '+');
        if (magnitude / 60 < 10) sb_append_char(sb, '0');
        sb_append_i64(sb, magnitude / 60);
        sb_append_char(sb, ':');
        if (magnitude % 60 < 10) sb_append_char(sb, '0');
        sb_append_i64(sb, magnitude % 60);
    }
}
