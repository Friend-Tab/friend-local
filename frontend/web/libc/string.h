/* string.h — the part of it the library uses, for a WebAssembly build
 * with no C library. See ../friend_web.c. */
#ifndef FR_WEB_STRING_H
#define FR_WEB_STRING_H
#include <stddef.h>
void  *memcpy(void *dest, const void *src, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
void  *memset(void *dest, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#endif
