/* ui_vendor.c — other people's code, compiled on its own.
 *
 * stb_truetype, stb_image, stb_image_write and kb_text_shape are
 * excellent and they are not written to this project's warning
 * settings, so they are built here with those settings off rather than
 * having the frontend turn them off for its own code too. Nothing else
 * lives in this translation unit except the thing stb_image needs that
 * this program does not otherwise have: an allocator.
 */

#include "../../base/fr_base.h"

/* stb_image allocates and this program does not. It gets an arena for
 * the length of one decode, and then the arena is reset — so a free is
 * nothing and a realloc is a copy. Each block carries its own size in
 * front of it, because a plain realloc is not told the old one.
 *
 * With no arena set, nothing can be decoded. That is deliberate: it is
 * better than quietly reaching for the heap. */
static Arena *ui_stb_arena;

void ui_stb_use_arena(Arena *arena) { ui_stb_arena = arena; }

#define UI_STB_HEADER 16

static void *ui_stb_malloc(size_t size)
{
    u8 *block;
    if (!ui_stb_arena) return 0;
    block = (u8 *)arena_push_size_nz(ui_stb_arena, size + UI_STB_HEADER, 16);
    if (!block) return 0;
    memcpy(block, &size, sizeof(size));
    return block + UI_STB_HEADER;
}

static void ui_stb_free(void *pointer) { (void)pointer; }

static void *ui_stb_realloc_sized(void *pointer, size_t old_size,
                                  size_t new_size)
{
    void *fresh = ui_stb_malloc(new_size);
    if (!fresh || !pointer) return fresh;
    memcpy(fresh, pointer, old_size < new_size ? old_size : new_size);
    return fresh;
}

static void *ui_stb_realloc(void *pointer, size_t new_size)
{
    size_t old_size = 0;
    if (pointer) {
        memcpy(&old_size, (u8 *)pointer - UI_STB_HEADER, sizeof(old_size));
    }
    return ui_stb_realloc_sized(pointer, old_size, new_size);
}

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../vendor/stb_truetype.h"

#define STBI_MALLOC(size) ui_stb_malloc(size)
#define STBI_REALLOC(pointer, size) ui_stb_realloc((pointer), (size))
#define STBI_REALLOC_SIZED(pointer, old_size, size)                           \
    ui_stb_realloc_sized((pointer), (old_size), (size))
#define STBI_FREE(pointer) ui_stb_free(pointer)
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "../../vendor/stb_image.h"

#define STBIW_MALLOC(size) ui_stb_malloc(size)
#define STBIW_REALLOC(pointer, size) ui_stb_realloc((pointer), (size))
#define STBIW_REALLOC_SIZED(pointer, old_size, size)                          \
    ui_stb_realloc_sized((pointer), (old_size), (size))
#define STBIW_FREE(pointer) ui_stb_free(pointer)
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../vendor/stb_image_write.h"

#define KB_TEXT_SHAPE_IMPLEMENTATION
#include "../../vendor/kb_text_shape.h"
