/* fr_virtual.h — address space is free; memory is not.
 *
 * The library wants one block at startup and never asks for more. That is
 * a good property and a bad reason to be stingy: if the block has to be
 * the friend's lifetime maximum, then either the limits are small enough
 * to be hit or the program is large enough to notice. Neither is
 * necessary. A 64-bit process has more address space than it can ever
 * use, so we reserve the maximum and let the operating system back it
 * with real pages only where the friend actually writes.
 *
 * Reserve once, commit as it grows. A friend with a hundred memories and
 * a thousand messages reserves a gigabyte and occupies a few megabytes of
 * it; the limits stop being a thing anybody has to tune.
 *
 * Two calls, and they are the same two calls everywhere: mmap(PROT_NONE)
 * then mprotect, or VirtualAlloc(MEM_RESERVE) then VirtualAlloc(MEM_COMMIT).
 */

#ifndef FR_VIRTUAL_H
#define FR_VIRTUAL_H

#include <stddef.h>
#include <stdint.h>

#include "friend.h"

typedef struct Fr_Virtual {
    void  *base;
    size_t reserved;
} Fr_Virtual;

/* Address space, not memory: nothing is resident afterwards and nothing
 * counts against the machine. Returns 0 if even the reservation is
 * refused, which on a 32-bit build is a real possibility and on a 64-bit
 * one means the size was absurd. */
int32_t fr_virtual_reserve(Fr_Virtual *v, size_t bytes);

/* Back [at, at + bytes) with real, zeroed pages. Idempotent: committing a
 * range that is already committed leaves what is in it alone, which
 * matters because a request rounds out to whole pages and the first of
 * those pages is usually half full of the friend already.
 *
 * Friend_Commit_Fn. Both operating systems commit by address, so user is
 * unused. */
int32_t fr_virtual_commit(void *user, void *at, size_t bytes);

/* Hands the pages under [at, at + bytes) back to the system while
 * keeping the addresses. What was in them is gone. */
void fr_virtual_decommit(void *at, size_t bytes);

void fr_virtual_release(Fr_Virtual *v);

/* A friend in reserved space: reserves what config->limits need, points
 * config->commit at it, and starts the friend there. Null when the
 * reservation is refused (space->base is then null too) or friend_init
 * is, with why in out_error, which may be null. */
Friend_Context *fr_virtual_friend(Fr_Virtual *space, Friend_Config *config,
                                  char *out_error, size_t error_size);

#endif /* FR_VIRTUAL_H */
