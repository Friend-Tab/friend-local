/* libc.c — what the library uses of a C library, for WebAssembly, where
 * there is none. memcpy and friends, the few maths functions it calls,
 * and enough printf for its one %.9g; the page formats the doubles,
 * because it rounds them correctly and a printf written here would not.
 * Included once, by whatever is being built: friend_web.c, or the tests
 * when they are run in WebAssembly too. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEB_IMPORT(name) __attribute__((import_module("host"), import_name(#name)))

WEB_IMPORT(log) void js_log(const char *text, int32_t len);
WEB_IMPORT(format_number)
int32_t js_format_number(double value, int32_t precision, int32_t style,
                         char *out, int32_t cap);
WEB_IMPORT(pow) double js_pow(double x, double y);
WEB_IMPORT(exp) double js_exp(double x);
WEB_IMPORT(log_e) double js_log_e(double x);

void *memcpy(void *dest, const void *src, size_t n)
{
    return __builtin_memcpy(dest, src, n);
}

void *memmove(void *dest, const void *src, size_t n)
{
    return __builtin_memmove(dest, src, n);
}

void *memset(void *dest, int c, size_t n)
{
    return __builtin_memset(dest, c, n);
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    size_t         i;
    for (i = 0; i < n; ++i) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

double sqrt(double x) { return __builtin_sqrt(x); }
float  sqrtf(float x) { return __builtin_sqrtf(x); }
double floor(double x) { return __builtin_floor(x); }
double ceil(double x) { return __builtin_ceil(x); }
double fabs(double x) { return __builtin_fabs(x); }
float  fabsf(float x) { return __builtin_fabsf(x); }
double pow(double x, double y) { return js_pow(x, y); }
double exp(double x) { return js_exp(x); }
double log(double x) { return js_log_e(x); }

/* Halves away from zero, which is what C's round does and WebAssembly's
 * nearest (halves to even) does not. */
double round(double x)
{
    double t = __builtin_trunc(x);
    if (__builtin_fabs(x - t) >= 0.5) t += x < 0 ? -1.0 : 1.0;
    return t;
}

__attribute__((noreturn)) void abort(void)
{
    __builtin_trap();
}

struct FILE { int unused; };
static FILE web_stderr_file;
FILE       *stderr = &web_stderr_file;

/* Enough printf for what the library formats: %s %.*s %d %u %ld %lu
 * %lld %llu %zu %c %x %% and %e %f %g with a precision, the doubles
 * formatted by the page. */
typedef struct Web_Out {
    char  *at;
    size_t size;
    size_t len;
} Web_Out;

static void web_out(Web_Out *o, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (o->len + 1 < o->size) o->at[o->len] = s[i];
        o->len += 1;
    }
}

static void web_out_unsigned(Web_Out *o, uint64_t v, uint32_t base)
{
    char tmp[24];
    int  n = 0;
    do {
        uint32_t d = (uint32_t)(v % base);
        tmp[n++]   = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v && n < (int)sizeof(tmp));
    while (n-- > 0) web_out(o, &tmp[n], 1);
}

int vsnprintf(char *out, size_t size, const char *fmt, va_list args)
{
    Web_Out o;
    o.at   = out;
    o.size = size;
    o.len  = 0;
    while (*fmt) {
        int precision = -1, longs = 0, size_t_arg = 0;
        if (*fmt != '%') {
            web_out(&o, fmt++, 1);
            continue;
        }
        ++fmt;
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '0' ||
               *fmt == '#') {
            ++fmt;
        }
        while (*fmt >= '0' && *fmt <= '9') ++fmt;
        if (*fmt == '.') {
            ++fmt;
            if (*fmt == '*') {
                precision = va_arg(args, int);
                ++fmt;
            } else {
                precision = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt++ - '0');
                }
            }
        }
        while (*fmt == 'l') {
            ++longs;
            ++fmt;
        }
        if (*fmt == 'z') {
            size_t_arg = 1;
            ++fmt;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(args, const char *);
            size_t      n = 0;
            if (!s) s = "(null)";
            while (s[n] && (precision < 0 || n < (size_t)precision)) ++n;
            web_out(&o, s, n);
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            web_out(&o, &c, 1);
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = longs >= 2 ? va_arg(args, long long)
                        : longs    ? va_arg(args, long)
                        : size_t_arg ? (int64_t)va_arg(args, size_t)
                                     : va_arg(args, int);
            if (v < 0) {
                web_out(&o, "-", 1);
                web_out_unsigned(&o, (uint64_t)0 - (uint64_t)v, 10);
            } else {
                web_out_unsigned(&o, (uint64_t)v, 10);
            }
            break;
        }
        case 'u':
        case 'x': {
            uint64_t v = longs >= 2 ? va_arg(args, unsigned long long)
                         : longs    ? va_arg(args, unsigned long)
                         : size_t_arg ? va_arg(args, size_t)
                                      : va_arg(args, unsigned);
            web_out_unsigned(&o, v, *fmt == 'x' ? 16u : 10u);
            break;
        }
        case 'e':
        case 'f':
        case 'g': {
            char    tmp[64];
            double  v     = va_arg(args, double);
            int32_t style = *fmt == 'e' ? 0 : *fmt == 'f' ? 1 : 2;
            int32_t n     = js_format_number(v, precision < 0 ? 6 : precision,
                                             style, tmp, (int32_t)sizeof(tmp));
            web_out(&o, tmp, (size_t)(n < 0 ? 0 : n));
            break;
        }
        case '%': web_out(&o, "%", 1); break;
        default: break;
        }
        if (*fmt) ++fmt;
    }
    if (o.size) o.at[o.len < o.size ? o.len : o.size - 1] = 0;
    return (int)o.len;
}

int snprintf(char *out, size_t size, const char *fmt, ...)
{
    va_list args;
    int     n;
    va_start(args, fmt);
    n = vsnprintf(out, size, fmt, args);
    va_end(args);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    char    line[512];
    va_list args;
    int     n;
    (void)f;
    va_start(args, fmt);
    n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    js_log(line, n);
    return n;
}
