/* fr_state.c — operations on the records themselves.
 *
 * Mostly the packed dialogue a memory carries: writing turns into it,
 * walking them back out, and rendering them the way the service did,
 * which is the form the relation pass is shown.
 */

#include "fr_state.h"

/* Returns 0 when there was nothing left to drop. */
static b32 fr_dialog_drop_oldest(Fr_Memory_Record *m)
{
    i32 len, size, remaining;
    if ((i32)m->dialog_len < FR_TURN_HEADER_BYTES) {
        m->dialog_len = 0;
        return 0;
    }
    len  = (i32)fr_le_get16((const u8 *)m->dialog + 9);
    size = FR_TURN_HEADER_BYTES + len;
    if (size > (i32)m->dialog_len) {
        /* Malformed, which means a bug in us. Start the stream over
         * rather than walk off the end of the record. */
        m->dialog_len = 0;
        return 0;
    }
    remaining = (i32)m->dialog_len - size;
    if (remaining > 0) {
        memmove(m->dialog, m->dialog + size, (size_t)remaining);
    }
    m->dialog_len = (u16)remaining;
    return 1;
}

static b32 fr_dialog_append(Fr_Memory_Record *m, u64 at_unix_ms, Fr_Turn_Kind kind,
                     String text)
{
    i32 widest = FR_DIALOG_MAX - FR_TURN_HEADER_BYTES;
    i32 need;
    u8 *p;

    if (!m) return 0;
    if (text.len < 0 || !text.data) text = str_nil;
    if (text.len > widest) text = fr_truncate_utf8(text, widest);
    need = FR_TURN_HEADER_BYTES + text.len;

    while ((i32)m->dialog_len + need > FR_DIALOG_MAX) {
        if (!fr_dialog_drop_oldest(m)) break;
    }
    if ((i32)m->dialog_len + need > FR_DIALOG_MAX) return 0;

    p = (u8 *)m->dialog + m->dialog_len;
    fr_le_put64(p, at_unix_ms);
    p[8] = (u8)kind;
    fr_le_put16(p + 9, (u16)text.len);
    if (text.len > 0) {
        memcpy(p + FR_TURN_HEADER_BYTES, text.data, (size_t)text.len);
    }
    m->dialog_len = (u16)((i32)m->dialog_len + need);
    return 1;
}

static b32 fr_dialog_next(String *cursor, u64 *out_at, Fr_Turn_Kind *out_kind,
                   String *out_text)
{
    const u8 *p;
    i32       len;

    if (!cursor || cursor->len < FR_TURN_HEADER_BYTES || !cursor->data) {
        return 0;
    }
    p   = (const u8 *)cursor->data;
    len = (i32)fr_le_get16(p + 9);
    if (FR_TURN_HEADER_BYTES + len > cursor->len) return 0;

    if (out_at) *out_at = fr_le_get64(p);
    if (out_kind) *out_kind = (Fr_Turn_Kind)p[8];
    if (out_text) {
        *out_text = str_make((const char *)p + FR_TURN_HEADER_BYTES, len);
    }
    *cursor = str_skip(*cursor, FR_TURN_HEADER_BYTES + len);
    return 1;
}


static void fr_set_description(Fr_Memory_Record *m, String text)
{
    String kept = fr_truncate_utf8(text, FR_DESCRIPTION_MAX);
    if (kept.len > 0) memcpy(m->description, kept.data, (size_t)kept.len);
    m->description_len = (u16)kept.len;
}

/* The captions for one message's photographs, in image order, one line
 * each. A newline inside a caption would make it two photographs'
 * worth, so it becomes a space; a photograph nobody described is an
 * empty line rather than a missing one, which is what keeps the nth
 * line the nth photograph. */
static void fr_set_captions(Fr_Chat_Event *e, const String *captions, i32 count)
{
    i32 at = 0;
    i32 i;

    e->captions_len = 0;
    for (i = 0; i < count; ++i) {
        String caption = str_trim(captions[i]);
        i32    j;
        if (i > 0) {
            if (at >= FR_CAPTIONS_MAX) break;
            e->captions[at++] = '\n';
        }
        caption = fr_truncate_utf8(caption, FR_CAPTIONS_MAX - at);
        for (j = 0; j < caption.len; ++j) {
            char c = caption.data[j];
            e->captions[at++] = (c == '\n' || c == '\r') ? ' ' : c;
        }
    }
    e->captions_len = (u16)at;
}

static String fr_caption_at(String captions, i32 index)
{
    String rest = captions;
    String line;
    i32    at   = 0;

    if (index < 0) return str_nil;
    while (str_next_line(&rest, &line)) {
        if (at++ == index) return str_trim(line);
    }
    return str_nil;
}

static String fr_memory_captions(Arena *arena, const Fr_Memory_Record *m)
{
    String_Builder sb     = sb_begin(arena);
    String         cursor = m ? fr_memory_dialog(m) : str_nil;
    u64            at     = 0;
    Fr_Turn_Kind   kind   = FR_TURN_USER;
    String         text   = str_nil;
    i32            written = 0;

    while (fr_dialog_next(&cursor, &at, &kind, &text)) {
        if (kind != FR_TURN_CAPTION) continue;
        text = str_trim(text);
        if (text.len == 0) continue;
        if (written > 0) sb_append_char(&sb, ' ');
        sb_append(&sb, text);
        ++written;
    }
    return sb_done(&sb);
}

/* ------------------------------------------------------------------ *
 *  The two strings a friend keeps outside the config arena
 *
 *  Both are replaced at runtime — the name by an import, the monologue
 *  after every consolidation — and a bump arena has no way to take the
 *  old one back, so each has a buffer of its own.
 * ------------------------------------------------------------------ */

static void fr_set_name(Friend_Context *fr, String name)
{
    String kept = fr_truncate_utf8(str_trim(name), FR_NAME_MAX);
    if (kept.len > 0) memcpy(fr->name_buffer, kept.data, (size_t)kept.len);
    fr->friend_name = str_make(fr->name_buffer, kept.len);
}

static void fr_set_interiority(Friend_Context *fr, String text)
{
    String kept = fr_truncate_utf8(text, FR_INTERIORITY_MAX);
    fr->interiority_len = kept.len;
    if (kept.len > 0) {
        memcpy(fr->interiority_buffer, kept.data, (size_t)kept.len);
    }
}
