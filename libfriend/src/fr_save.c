/* fr_save.c — the friend as bytes.
 *
 * The state is already position-independent: records refer to each other
 * by slot index, never by pointer, so saving is a walk rather than a
 * serialization pass with a pointer-fixup table. What it is not is a raw
 * memcpy of the block — the block is sized for the friend the user might
 * one day have, and a friend with ninety memories should not write out
 * three hundred megabytes of empty slots. So occupied records only, and
 * each one's variable-length text at its real length.
 *
 * Slot indices are written down and restored exactly, because the edges
 * are expressed in them and because a memory id a frontend wrote down
 * should still mean the same memory after a reload. Adjacency lists are
 * not written: they are a different arrangement of the same edges, so
 * they are rebuilt on load and cannot disagree with what was saved.
 *
 * One writer serves both friend_save_size and friend_save: with no
 * destination it only counts. The two cannot disagree about the size
 * because they are the same code. The header likewise has one writer and
 * one reader, and the reader serves both friend_load and friend_save_peek.
 */

#include "fr_state.h"

/* 2 adds the three numbers that say what the conversation has been
 * through: how much of it has been folded into memories, and how much
 * fell out of the scrollback. 3 adds how long the blob file is, and the
 * photo captions on a message. 4 writes down the three record shapes 3
 * left out: captions, name and monologue. Every older version still loads; it just
 * cannot say, and a friend that cannot say is taken to have folded
 * everything it is no longer holding and to have no photographs. */
#define FR_SAVE_VERSION 4u

static const u8 fr_save_magic[8] = { 'f', 'r', 'i', 'e', 'n', 'd', 0, 1 };

/* ------------------------------------------------------------------ *
 *  Bytes in, bytes out
 *
 *  No struct is ever written whole: padding and byte order are not
 *  things a file format should inherit from whatever compiled it.
 * ------------------------------------------------------------------ */

typedef struct Fr_Writer {
    u8    *at;   /* 0 to count without writing */
    u8    *end;
    size_t written;
    b32    overflow;
} Fr_Writer;

static void fw_bytes(Fr_Writer *w, const void *bytes, size_t count)
{
    if (w->at) {
        if ((size_t)(w->end - w->at) < count) {
            w->overflow = 1;
            return;
        }
        if (count > 0) memcpy(w->at, bytes, count);
        w->at += count;
    }
    w->written += count;
}

static void fw_u8(Fr_Writer *w, u8 value) { fw_bytes(w, &value, 1); }

static void fw_u16(Fr_Writer *w, u16 value)
{
    u8 b[2];
    fr_le_put16(b, value);
    fw_bytes(w, b, 2);
}

static void fw_u32(Fr_Writer *w, u32 value)
{
    u8 b[4];
    fr_le_put32(b, value);
    fw_bytes(w, b, 4);
}

static void fw_u64(Fr_Writer *w, u64 value)
{
    u8 b[8];
    fr_le_put64(b, value);
    fw_bytes(w, b, 8);
}

static void fw_f32(Fr_Writer *w, f32 value)
{
    u32 bits;
    memcpy(&bits, &value, 4);
    fw_u32(w, bits);
}

static void fw_string(Fr_Writer *w, String s)
{
    i32 len = s.len > 0 ? s.len : 0;
    fw_u32(w, (u32)len);
    fw_bytes(w, s.data, (size_t)len);
}

typedef struct Fr_Reader {
    const u8 *at;
    const u8 *end;
    b32       failed;  /* stop reading: either of the below */
    b32       damaged; /* ...because something in it cannot be right */
} Fr_Reader;

static b32 fr_take(Fr_Reader *r, void *out, size_t count)
{
    if (r->failed || (size_t)(r->end - r->at) < count) {
        r->failed = 1;
        return 0;
    }
    if (out && count > 0) memcpy(out, r->at, count);
    r->at += count;
    return 1;
}

static void fr_damaged(Fr_Reader *r)
{
    r->failed  = 1;
    r->damaged = 1;
}

static u8 fr_u8(Fr_Reader *r)
{
    u8 value = 0;
    fr_take(r, &value, 1);
    return value;
}

static u16 fr_u16(Fr_Reader *r)
{
    u8 b[2] = { 0, 0 };
    fr_take(r, b, 2);
    return fr_le_get16(b);
}

static u32 fr_u32(Fr_Reader *r)
{
    u8 b[4] = { 0, 0, 0, 0 };
    fr_take(r, b, 4);
    return fr_le_get32(b);
}

static u64 fr_u64(Fr_Reader *r)
{
    u8 b[8];
    memset(b, 0, sizeof(b));
    fr_take(r, b, 8);
    return fr_le_get64(b);
}

static f32 fr_f32(Fr_Reader *r)
{
    u32 bits = fr_u32(r);
    f32 value;
    memcpy(&value, &bits, 4);
    return value;
}

/* A view into the buffer being read, not a copy: the caller copies into
 * whatever fixed room the record has. Every string in the file was cut
 * to that room before it was written, so one that is longer than it is a
 * damaged file, and is refused rather than cut again or quietly dropped. */
static String fr_view(Fr_Reader *r, i32 max)
{
    u32       len = fr_u32(r);
    const u8 *start;
    if (r->failed) return str_nil;
    if (len > (u32)max) {
        fr_damaged(r);
        return str_nil;
    }
    start = r->at;
    if (!fr_take(r, 0, len)) return str_nil;
    return str_make((const char *)start, (i32)len);
}

/* ------------------------------------------------------------------ *
 *  The header
 *
 *  Everything before the memories: what a picker needs, and the numbers
 *  that are not on any record.
 * ------------------------------------------------------------------ */

/* Every field, in the order it is written, and the version it first
 * appeared in. A field a file is too old to have is left zero, apart
 * from folded_through, below. The history count was once the number of
 * events written to a history log that was never built; it is written
 * as zero so the layout stays what every version has read. */
#define FR_SAVE_HEADER_FIELDS(X)                                           \
    X(U64, created_unix_ms, 1)                                             \
    X(U64, total_event_count, 1)                                           \
    X(U64, history_count, 1)                                               \
    X(U64, replies_total, 1)                                               \
    X(U64, epoch_samples, 1)                                               \
    X(U32, epoch_active, 1)                                                \
    X(U64, folded_through, 2)                                              \
    X(U64, scroll_lost, 2)                                                 \
    X(U64, scroll_unremembered, 2)                                         \
    X(U64, blob_bytes, 3)                                                  \
    X(U64, blob_written, 3)                                                \
    X(NAME, name, 1)                                                       \
    X(INTERIORITY, interiority, 1)                                         \
    X(U32, memory_count, 1)

#define FR_SAVE_TYPE_U64 u64
#define FR_SAVE_TYPE_U32 u32
#define FR_SAVE_TYPE_NAME String
#define FR_SAVE_TYPE_INTERIORITY String
#define FR_SAVE_PUT_U64(w, v) fw_u64((w), (v))
#define FR_SAVE_PUT_U32(w, v) fw_u32((w), (v))
#define FR_SAVE_PUT_NAME(w, v) fw_string((w), (v))
#define FR_SAVE_PUT_INTERIORITY(w, v) fw_string((w), (v))
#define FR_SAVE_GET_U64(r) fr_u64(r)
#define FR_SAVE_GET_U32(r) fr_u32(r)
#define FR_SAVE_GET_NAME(r) fr_view((r), FR_NAME_MAX)
#define FR_SAVE_GET_INTERIORITY(r) fr_view((r), FR_INTERIORITY_MAX)

typedef struct Fr_Save_Header {
    u32 version;
#define FR_SAVE_MEMBER(type, name, since) FR_SAVE_TYPE_##type name;
    FR_SAVE_HEADER_FIELDS(FR_SAVE_MEMBER)
#undef FR_SAVE_MEMBER
} Fr_Save_Header;

/* The shapes a record was compiled with, and the version that started
 * writing each down. A build whose records are a different size cannot
 * read this file's records, and finding that out from the header beats
 * finding it out from a memory that looks almost right. */
#define FR_SAVE_GEOMETRY(X)                                                \
    X(FR_EMBED_DIMS, 1)                                                    \
    X(FR_DESCRIPTION_MAX, 1)                                               \
    X(FR_DIALOG_MAX, 1)                                                    \
    X(FR_CHAT_TEXT_MAX, 1)                                                 \
    X(FR_MEMORY_IMAGES_MAX, 1)                                             \
    X(FR_CHAT_IMAGES_MAX, 1)                                               \
    X(FR_UUID_BYTES, 1)                                                    \
    X(FR_TURN_HEADER_BYTES, 1)                                             \
    X(FR_CAPTIONS_MAX, 4)                                                  \
    X(FR_NAME_MAX, 4)                                                      \
    X(FR_INTERIORITY_MAX, 4)

static void fr_put_header(Fr_Writer *w, const Fr_Save_Header *h)
{
    fw_bytes(w, fr_save_magic, sizeof(fr_save_magic));
    fw_u32(w, FR_SAVE_VERSION);
#define FR_SAVE_PUT_SHAPE(value, since) fw_u32(w, (u32)(value));
    FR_SAVE_GEOMETRY(FR_SAVE_PUT_SHAPE)
#undef FR_SAVE_PUT_SHAPE
#define FR_SAVE_PUT_FIELD(type, name, since) FR_SAVE_PUT_##type(w, h->name);
    FR_SAVE_HEADER_FIELDS(FR_SAVE_PUT_FIELD)
#undef FR_SAVE_PUT_FIELD
}

/* The most a header can be: the variable parts are the name and the
 * monologue, and both have a ceiling. Counted by the writer rather than
 * added up here, so it cannot disagree with it. */
static size_t fr_header_bytes_max(void)
{
    Fr_Writer      w;
    Fr_Save_Header h;
    memset(&w, 0, sizeof(w));
    memset(&h, 0, sizeof(h));
    h.name        = str_make(0, FR_NAME_MAX);
    h.interiority = str_make(0, FR_INTERIORITY_MAX);
    fr_put_header(&w, &h);
    return w.written;
}

/* FRIEND_OK, FRIEND_ERR_TOO_SMALL when the buffer ends inside the header,
 * or FRIEND_ERR_BAD_DATA with *why saying what is wrong. */
static Friend_Result fr_get_header(Fr_Reader *r, Fr_Save_Header *h,
                                   String *why)
{
    u8 magic[8];

    memset(h, 0, sizeof(*h));
    if (!fr_take(r, magic, sizeof(magic))) return FRIEND_ERR_TOO_SMALL;
    if (memcmp(magic, fr_save_magic, sizeof(magic)) != 0) {
        *why = str_lit("this is not a saved friend");
        return FRIEND_ERR_BAD_DATA;
    }
    h->version = fr_u32(r);
    if (r->failed) return FRIEND_ERR_TOO_SMALL;
    if (h->version < 1u || h->version > FR_SAVE_VERSION) {
        *why = str_lit("this friend was saved by a different version of "
                       "the library");
        return FRIEND_ERR_BAD_DATA;
    }
#define FR_SAVE_CHECK_SHAPE(value, since)                                  \
    if (h->version >= (since)) {                                           \
        u32 shape = fr_u32(r);                                             \
        if (r->failed) return FRIEND_ERR_TOO_SMALL;                        \
        if (shape != (u32)(value)) {                                       \
            *why = str_lit("this friend was saved by a build whose "       \
                           "records are a different shape");               \
            return FRIEND_ERR_BAD_DATA;                                    \
        }                                                                  \
    }
    FR_SAVE_GEOMETRY(FR_SAVE_CHECK_SHAPE)
#undef FR_SAVE_CHECK_SHAPE

#define FR_SAVE_GET_FIELD(type, name, since)                               \
    if (h->version >= (since)) h->name = FR_SAVE_GET_##type(r);
    FR_SAVE_HEADER_FIELDS(FR_SAVE_GET_FIELD)
#undef FR_SAVE_GET_FIELD
    /* A file from before folded_through was written down. Everything it
     * is not still holding counts as folded, which is the assumption
     * that does not re-fold a conversation the friend already has
     * memories of. */
    if (h->version < 2u) h->folded_through = h->total_event_count;

    if (r->damaged) {
        *why = str_lit("this friend's name or monologue is longer than "
                       "this build has room for");
        return FRIEND_ERR_BAD_DATA;
    }
    return r->failed ? FRIEND_ERR_TOO_SMALL : FRIEND_OK;
}

/* ------------------------------------------------------------------ *
 *  Records
 * ------------------------------------------------------------------ */

static void fr_put_images(Fr_Writer *w, const Fr_Image_Ref *images,
                          i32 count)
{
    i32 i;
    for (i = 0; i < count; ++i) {
        fw_u64(w, images[i].blob_offset);
        fw_u32(w, images[i].blob_size);
        fw_u32(w, images[i].thumb_offset);
        fw_u32(w, images[i].thumb_size);
        fw_u32(w, images[i].flags);
    }
}

static void fr_get_images(Fr_Reader *r, Fr_Image_Ref *images, i32 count)
{
    i32 i;
    for (i = 0; i < count; ++i) {
        images[i].blob_offset  = fr_u64(r);
        images[i].blob_size    = fr_u32(r);
        images[i].thumb_offset = fr_u32(r);
        images[i].thumb_size   = fr_u32(r);
        images[i].flags        = fr_u32(r);
    }
}

static void fr_put_memory(Fr_Writer *w, u32 slot, const Fr_Memory_Record *m)
{
    fw_u32(w, slot);
    fw_u32(w, m->generation);
    fw_u8(w, m->in_this_epoch);
    fw_u8(w, m->needs_embedding);
    fw_u8(w, m->image_count);
    fw_u8(w, 0);
    fw_u64(w, m->created_unix_ms);
    fw_u64(w, m->dialog_end_unix_ms);
    fw_u64(w, m->use_count);
    fw_u64(w, m->uses_this_epoch);
    fr_put_images(w, m->images, m->image_count);
    fw_bytes(w, m->embedding, sizeof(m->embedding));
    fw_string(w, fr_memory_description(m));
    fw_string(w, fr_memory_dialog(m));
}

/* Everything after the slot, into a record the graph has just handed
 * out at that slot. */
static void fr_get_memory(Fr_Reader *r, Fr_Memory_Record *m)
{
    String description, dialog;

    m->generation         = fr_u32(r);
    m->in_this_epoch      = fr_u8(r);
    m->needs_embedding    = fr_u8(r);
    m->image_count        = fr_u8(r);
    (void)fr_u8(r);
    m->created_unix_ms    = fr_u64(r);
    m->dialog_end_unix_ms = fr_u64(r);
    m->use_count          = fr_u64(r);
    m->uses_this_epoch    = fr_u64(r);
    if (m->image_count > FR_MEMORY_IMAGES_MAX) {
        m->image_count = 0;
        fr_damaged(r);
        return;
    }
    fr_get_images(r, m->images, m->image_count);
    fr_take(r, m->embedding, sizeof(m->embedding));
    description = fr_view(r, FR_DESCRIPTION_MAX);
    dialog      = fr_view(r, FR_DIALOG_MAX);
    if (r->failed) return;
    fr_set_description(m, description);
    m->dialog_len = (u16)dialog.len;
    if (dialog.len > 0) memcpy(m->dialog, dialog.data, (size_t)dialog.len);
}

/* One message, from whichever side of the conversation it is held on:
 * the file format does not know or care. */
static void fr_put_message(Fr_Writer *w, const Fr_Scroll_View *v)
{
    const Fr_Message_Header *h = &v->header;
    fw_u64(w, h->index);
    fw_u64(w, h->occurred_unix_ms);
    fw_u8(w, h->kind);
    fw_u8(w, h->was_transcribed);
    fw_u8(w, h->ambient_raw);
    fw_u8(w, 0); /* once whether a reply to it was to be spoken */
    fw_u8(w, h->image_count);
    fw_u8(w, 0);
    fw_u16(w, 0);
    fw_f32(w, h->interrupted_fraction);
    {
        static const u8 no_uuid[FR_UUID_BYTES];
        fw_bytes(w, no_uuid, sizeof(no_uuid));
    }
    fr_put_images(w, v->images, h->image_count);
    fw_string(w, v->text);
    fw_string(w, v->captions);
}

static b32 fr_get_message(Fr_Reader *r, Fr_Chat_Event *e, u32 version)
{
    Fr_Message_Header h;
    String            text;
    String            captions = str_nil;

    memset(&h, 0, sizeof(h));
    h.index              = fr_u64(r);
    h.occurred_unix_ms   = fr_u64(r);
    h.kind               = fr_u8(r);
    h.was_transcribed    = fr_u8(r);
    h.ambient_raw        = fr_u8(r);
    (void)fr_u8(r);
    h.image_count        = fr_u8(r);
    (void)fr_u8(r);
    (void)fr_u16(r);
    h.interrupted_fraction = fr_f32(r);
    fr_take(r, 0, FR_UUID_BYTES);
    if (h.image_count > FR_CHAT_IMAGES_MAX) {
        fr_damaged(r);
        return 0;
    }

    memset(e, 0, sizeof(*e));
    fr_get_images(r, e->images, h.image_count);
    text = fr_view(r, FR_CHAT_TEXT_MAX);
    if (version >= 3u) captions = fr_view(r, FR_CAPTIONS_MAX);
    if (r->failed) return 0;

    h.text_len     = (u16)text.len;
    h.captions_len = (u16)captions.len;
    fr_message_header_into(e, &h);
    if (text.len > 0) memcpy(e->text, text.data, (size_t)text.len);
    if (captions.len > 0) {
        memcpy(e->captions, captions.data, (size_t)captions.len);
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 *  The whole thing
 * ------------------------------------------------------------------ */

static i32 fr_streaming_count(Friend_Context *fr)
{
    i32 count = 0;
    i32 i;
    for (i = 0; i < fr->active_count; ++i) {
        count += fr->active_events[i].streaming ? 1 : 0;
    }
    return count;
}

/* A memory a consolidation has not finished forming is left out, and the
 * stretch it came from is folded again after a load: folded_through has
 * not moved past it. */
static b32 fr_memory_saved(const Fr_Memory_Record *m)
{
    return m->occupied && !m->staged;
}

static void fr_put_all(Friend_Context *fr, Fr_Writer *w)
{
    Fr_Save_Header h;
    i32            i;

    memset(&h, 0, sizeof(h));
    h.created_unix_ms     = fr->created_unix_ms;
    h.total_event_count   = fr->total_event_count;
    h.replies_total       = fr->replies_total;
    h.epoch_samples       = fr->epoch_samples;
    h.epoch_active        = fr->epoch_active ? 1u : 0u;
    h.folded_through      = fr->folded_through;
    h.scroll_lost         = fr->scroll.lost;
    h.scroll_unremembered = fr->scroll.unremembered;
    /* Both numbers. The cursor, so that offsets handed out before the
     * program closed are never handed out again — a photograph lost in
     * staging is a photograph that cannot be read, which is a small
     * thing, while two photographs sharing an offset would show one of
     * them in place of the other, which is not. And what the file is
     * known to hold, so that the ones in between are reported as
     * missing rather than read out of a hole. */
    h.blob_bytes   = fr->blob_bytes;
    h.blob_written = fr->blob_written;
    h.name         = fr->friend_name;
    h.interiority  = friend_interiority(fr);
    for (i = 0; i < fr->memory_slots; ++i) {
        h.memory_count += fr_memory_saved(&fr->memories[i]) ? 1u : 0u;
    }
    fr_put_header(w, &h);

    for (i = 0; i < fr->memory_slots; ++i) {
        if (fr_memory_saved(&fr->memories[i])) {
            fr_put_memory(w, (u32)i, &fr->memories[i]);
        }
    }


    fw_u32(w, (u32)fr->edge_count);
    for (i = 0; i < fr->edge_slots; ++i) {
        if (fr->edges[i].occupied) {
            fw_u32(w, fr->edges[i].from_slot);
            fw_u32(w, fr->edges[i].to_slot);
            fw_u8(w, fr->edges[i].relation);
        }
    }

    /* The scrollback goes out oldest first, then the window, so a load
     * can lay the conversation down in order and forget that it was ever
     * a ring. */
    fw_u32(w, (u32)fr->scroll.count);
    for (i = 0; i < fr->scroll.count; ++i) {
        Fr_Scroll_View view;
        if (!fr_scroll_view(fr, i, &view)) break;
        fr_put_message(w, &view);
    }
    /* A reply still arriving is not a message anybody sent. */
    fw_u32(w, (u32)(fr->active_count - fr_streaming_count(fr)));
    for (i = 0; i < fr->active_count; ++i) {
        Fr_Scroll_View view;
        if (fr->active_events[i].streaming) continue;
        fr_event_view(&fr->active_events[i], &view);
        fr_put_message(w, &view);
    }
}

size_t friend_save_size(Friend_Context *fr)
{
    Fr_Writer w;
    if (!fr) return 0;
    memset(&w, 0, sizeof(w));
    fr_put_all(fr, &w);
    return w.written;
}

Friend_Result friend_save(Friend_Context *fr, void *buffer, size_t size,
                          size_t *out_written)
{
    Fr_Writer w;
    if (out_written) *out_written = 0;
    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    if (!buffer && size > 0) return FRIEND_ERR_BAD_ARGUMENT;

    memset(&w, 0, sizeof(w));
    w.at  = (u8 *)buffer;
    w.end = (u8 *)buffer + size;
    fr_put_all(fr, &w);

    if (w.overflow) {
        return fr_error(fr, FRIEND_ERR_TOO_SMALL,
                        str_lit("the buffer is smaller than "
                                "friend_save_size() asked for"));
    }
    if (out_written) *out_written = w.written;
    if (buffer) fr->unsaved = 0;
    return FRIEND_OK;
}

static Friend_Result fr_load_failed(Friend_Context *fr, const Fr_Reader *r,
                                    String why)
{
    fr_state_clear(fr);
    if (r->damaged) {
        why = str_lit("this friend's file is damaged: a record in it is "
                      "larger than a record can be");
    }
    return fr_error(fr, FRIEND_ERR_BAD_DATA, why);
}

Friend_Result friend_load(Friend_Context *fr, const void *buffer, size_t size)
{
    Fr_Reader      r;
    Fr_Save_Header h;
    Friend_Result  result;
    String         why = str_lit("the file ends early");
    u32            count, i, overflow;

    if (!fr) return FRIEND_ERR_BAD_ARGUMENT;
    if (!buffer || size == 0) {
        return fr_error(fr, FRIEND_ERR_BAD_ARGUMENT,
                        str_lit("there is nothing to load"));
    }

    memset(&r, 0, sizeof(r));
    r.at   = (const u8 *)buffer;
    r.end  = (const u8 *)buffer + size;
    result = fr_get_header(&r, &h, &why);
    if (result != FRIEND_OK) {
        /* Before anything is cleared: a file that is not a friend leaves
         * the one that was open alone. */
        return fr_error(fr, FRIEND_ERR_BAD_DATA, why);
    }

    fr_state_clear(fr);
    fr->created_unix_ms     = h.created_unix_ms;
    fr->total_event_count   = h.total_event_count;
    fr->replies_total       = h.replies_total;
    fr->epoch_samples       = h.epoch_samples;
    fr->epoch_active        = h.epoch_active != 0;
    fr->folded_through      = h.folded_through;
    fr->scroll.lost         = h.scroll_lost;
    fr->scroll.unremembered = h.scroll_unremembered;
    fr->blob_written        = h.blob_written;
    fr->blob_bytes          = fr_max(h.blob_bytes, h.blob_written);
    fr->staging_base        = fr->blob_bytes;
    fr_set_name(fr, h.name);
    fr_set_interiority(fr, h.interiority);

    for (i = 0; i < h.memory_count && !r.failed; ++i) {
        u32               slot = fr_u32(&r);
        Fr_Memory_Record *m;

        if (r.failed) break;
        m = fr_memory_claim(fr, slot);
        if (!m) {
            return fr_load_failed(
                fr, &r,
                slot < (u32)fr->memory_capacity && fr_memory(fr, slot)
                    ? str_lit("two memories in the file claim one slot")
                    : str_lit("this friend has more memories than this one "
                              "has room for"));
        }
        fr_get_memory(&r, m);
    }
    if (r.failed) {
        return fr_load_failed(fr, &r, str_lit("the memories end early"));
    }

    count = fr_u32(&r);
    for (i = 0; i < count && !r.failed; ++i) {
        u32 from     = fr_u32(&r);
        u32 to       = fr_u32(&r);
        u8  relation = fr_u8(&r);

        if (r.failed) break;
        if (from == to || !fr_memory(fr, from) || !fr_memory(fr, to)) {
            return fr_load_failed(fr, &r,
                                  str_lit("a connection in the file is not "
                                          "between two of its memories"));
        }
        if (!fr_edge_add(fr, from, to, relation)) {
            return fr_load_failed(fr, &r,
                                  str_lit("this friend has more connections "
                                          "than this one has room for"));
        }
    }
    if (r.failed) {
        return fr_load_failed(fr, &r, str_lit("the connections end early"));
    }

    /* The conversation, laid back down oldest first: the scrollback, then
     * the window. A window bigger than this build's goes into the
     * scrollback from its oldest end, and a file that holds more than
     * this build's limits leave room for loses the front of it, which the
     * scrollback counts and says out loud. */
    count = fr_u32(&r);
    for (i = 0; i < count && !r.failed; ++i) {
        Fr_Chat_Event event;
        if (!fr_get_message(&r, &event, h.version)) break;
        fr_scroll_push(fr, &event);
    }
    count    = fr_u32(&r);
    overflow = count > (u32)fr->active_capacity
                   ? count - (u32)fr->active_capacity
                   : 0;
    for (i = 0; i < count && !r.failed; ++i) {
        if (i < overflow) {
            Fr_Chat_Event event;
            if (!fr_get_message(&r, &event, h.version)) break;
            fr_scroll_push(fr, &event);
        } else if (fr_get_message(&r, &fr->active_events[fr->active_count],
                                  h.version)) {
            fr->active_count += 1;
        }
    }
    if (r.failed) {
        return fr_load_failed(fr, &r, str_lit("the conversation ends early"));
    }

    /* Occupied slots are placed where they were saved, so the free list
     * has to be built around them rather than from scratch. */
    fr_rebuild_free_lists(fr);
    fr->unsaved = 0;
    return FRIEND_OK;
}

/* ------------------------------------------------------------------ *
 *  Looking without loading
 *
 *  A picker wants a name and a couple of numbers per file, and loading a
 *  friend to get them would mean a block each. Everything it needs is in
 *  the header, so this reads that much and stops.
 * ------------------------------------------------------------------ */

Friend_Result friend_save_peek(const void *buffer, size_t size,
                               Friend_Save_Info *out)
{
    Fr_Reader      r;
    Fr_Save_Header h;
    String         why = str_nil;
    Friend_Result  result;

    if (!out) return FRIEND_ERR_BAD_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!buffer || size == 0) return FRIEND_ERR_BAD_ARGUMENT;

    memset(&r, 0, sizeof(r));
    r.at   = (const u8 *)buffer;
    r.end  = (const u8 *)buffer + size;
    result = fr_get_header(&r, &h, &why);
    if (result == FRIEND_ERR_TOO_SMALL) {
        out->bytes_needed = fr_header_bytes_max();
        return FRIEND_ERR_TOO_SMALL;
    }
    if (result != FRIEND_OK) return result;

    out->name              = h.name;
    out->created_unix_ms   = h.created_unix_ms;
    out->total_event_count = h.total_event_count;
    out->memories          = (i32)h.memory_count;
    return FRIEND_OK;
}
