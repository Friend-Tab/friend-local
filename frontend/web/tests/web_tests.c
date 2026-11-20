/* web_tests.c — the library's own tests, run in WebAssembly against the
 * C library in ../libc, so that what the page runs is what was tested.
 * Built and run by ./build.sh web-test. */

#include "../../../libfriend/tests/fr_tests.c"
#include "../libc/libc.c"

/* The tests take blocks with malloc and give them back in the order they
 * took them, mostly: a bump allocator that steps back when the newest
 * block is freed, over memory grown as it is needed. */
extern unsigned char __heap_base;
static uintptr_t web_test_top;
static uintptr_t web_test_last;

void *malloc(size_t size)
{
    uintptr_t at, end;
    if (!web_test_top) web_test_top = ((uintptr_t)&__heap_base + 15u) & ~(uintptr_t)15u;
    at  = web_test_top;
    end = at + ((size + 15u) & ~(size_t)15u);
    if (end < at) return 0;
    while (end > (uintptr_t)__builtin_wasm_memory_size(0) * 65536u) {
        if (__builtin_wasm_memory_grow(0, 256) == (size_t)-1) return 0;
    }
    web_test_last = at;
    web_test_top  = end;
    return (void *)at;
}

void free(void *p)
{
    if (p && (uintptr_t)p == web_test_last) web_test_top = web_test_last;
}

int printf(const char *fmt, ...)
{
    char    line[1024];
    va_list args;
    int     n;
    va_start(args, fmt);
    n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    js_log(line, n);
    return n;
}
