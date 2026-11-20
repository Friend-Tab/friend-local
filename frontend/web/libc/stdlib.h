/* stdlib.h — abort, which is all the library asks of it. */
#ifndef FR_WEB_STDLIB_H
#define FR_WEB_STDLIB_H
#include <stddef.h>
__attribute__((noreturn)) void abort(void);
void *malloc(size_t size); /* the tests only */
void  free(void *p);
#endif
