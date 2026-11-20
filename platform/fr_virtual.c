/* fr_virtual.c — see fr_virtual.h. */

#include "fr_virtual.h"

#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static size_t fr_virtual_page_size(void)
{
    long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? (size_t)size : 4096u;
}

int32_t fr_virtual_reserve(Fr_Virtual *v, size_t bytes)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    if (!v) return 0;
    v->base     = 0;
    v->reserved = 0;
    if (bytes == 0) return 0;

#ifdef MAP_NORESERVE
    /* Do not charge this against commit limits or swap: we are asking
     * for addresses, and we will say when we want pages. */
    flags |= MAP_NORESERVE;
#endif
    v->base = mmap(0, bytes, PROT_NONE, flags, -1, 0);
    if (v->base == MAP_FAILED) v->base = 0;
    if (!v->base) return 0;
    v->reserved = bytes;
    return 1;
}

int32_t fr_virtual_commit(void *user, void *at, size_t bytes)
{
    size_t page = fr_virtual_page_size();
    size_t mask = page - 1;
    size_t from = (size_t)at & ~mask;
    size_t to   = ((size_t)at + bytes + mask) & ~mask;

    (void)user;
    if (!at || bytes == 0) return 1;
    return mprotect((void *)from, to - from, PROT_READ | PROT_WRITE) == 0;
}

void fr_virtual_decommit(void *at, size_t bytes)
{
    if (!at || bytes == 0) return;
#if defined(__APPLE__) && defined(MADV_FREE_REUSABLE)
    madvise(at, bytes, MADV_FREE_REUSABLE);
#elif defined(MADV_DONTNEED)
    madvise(at, bytes, MADV_DONTNEED);
#endif
}

void fr_virtual_release(Fr_Virtual *v)
{
    if (!v || !v->base) return;
    munmap(v->base, v->reserved);
    v->base     = 0;
    v->reserved = 0;
}

Friend_Context *fr_virtual_friend(Fr_Virtual *space, Friend_Config *config,
                                  char *out_error, size_t error_size)
{
    size_t          bytes  = friend_required_memory(config->limits);
    Friend_Result   result = FRIEND_ERR_TOO_SMALL;
    Friend_Context *fr     = 0;

    if (fr_virtual_reserve(space, bytes)) {
        config->commit      = fr_virtual_commit;
        config->commit_user = 0;
        fr = friend_init(space->base, bytes, config, &result);
        if (!fr) fr_virtual_release(space);
    }
    if (!fr && out_error && error_size > 0) {
        snprintf(out_error, error_size,
                 "the friend would not start in %.0f MiB of address space "
                 "(%d)",
                 (double)bytes / (1024.0 * 1024.0), (int)result);
    }
    return fr;
}
