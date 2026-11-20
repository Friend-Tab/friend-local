/* stdio.h — formatting only: there are no files in a browser. */
#ifndef FR_WEB_STDIO_H
#define FR_WEB_STDIO_H
#include <stdarg.h>
#include <stddef.h>
typedef struct FILE FILE;
extern FILE *stderr;
int vsnprintf(char *out, size_t size, const char *fmt, va_list args);
int snprintf(char *out, size_t size, const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int printf(const char *fmt, ...); /* the tests only */
#endif
