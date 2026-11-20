/* fr_scroll.c — the conversation, after the model is done with it.
 *
 * A ring of bytes and a ring of positions over it. The bytes hold one
 * record per message: a header, the photo references if there are any,
 * then the text. The positions are stream positions — bytes ever written
 * — rather than offsets, so a position names a message for as long as the
 * message exists and the arithmetic for "has this fallen off the front"
 * is a subtraction rather than a case analysis of which side of the wrap
 * everything is on.
 *
 * Records are padded to eight bytes and never straddle the wrap: when one
 * will not fit in what is left of the ring, the stream skips to the start
 * instead and the skipped tail is dead space for one lap. That costs at
 * most one message's worth of room and buys a contiguous record, which is
 * what lets friend_message hand out a view of the text rather than a copy
 * of it.
 *
 * Nothing here can block the conversation. When the ring is full the
 * oldest message goes, and if it leaves before any memory was formed from
 * it, that is counted and said out loud once rather than silently.
 */

#include "fr_state.h"

#define FR_SCROLL_ALIGN 8

/* A record is as long as its text, and the photo references if it has
 * any. The count is clamped here rather than trusted, because the size
 * of the record and the reader's idea of where the text starts are the
 * same number twice and they must not be able to disagree. */
static i32 fr_scroll_images(const Fr_Chat_Event *event)
{
    i32 count = (i32)event->image_count;
    return count > FR_CHAT_IMAGES_MAX ? FR_CHAT_IMAGES_MAX : count;
}

static size_t fr_scroll_record_bytes(const Fr_Chat_Event *event)
{
    size_t bytes = sizeof(Fr_Message_Header);
    bytes += sizeof(Fr_Image_Ref) * (size_t)fr_scroll_images(event);
    bytes += (size_t)event->text_len;
    bytes += (size_t)event->captions_len;
    return fr_align_up(bytes, FR_SCROLL_ALIGN);
}

static u64 *fr_scroll_slot(Friend_Context *fr, i32 slot)
{
    return (u64 *)fr->scroll.positions.base + slot;
}

static u8 *fr_scroll_bytes_at(Friend_Context *fr, u64 position)
{
    return fr->scroll.bytes.base + (size_t)(position % fr->scroll.bytes.size);
}

/* One message is no longer in the conversation the user can scroll
 * through. Whether it was pushed off the front or never got in, it is
 * counted the same way, and the one case that costs the friend something
 * is counted apart. */
static void fr_scroll_lost(Friend_Context *fr, u64 event_index)
{
    Fr_Scrollback *s = &fr->scroll;

    s->lost += 1;
    fr_mark_dirty(fr);
    if (event_index >= fr->folded_through) {
        /* Nothing was ever remembered from this one. With the shipped
         * limits it takes a ring four orders of magnitude smaller than
         * the default to reach this, but it is the one case where
         * dropping a message costs the friend something, so it is
         * counted apart and reported. */
        s->unremembered += 1;
        if (!s->reported_loss) {
            s->reported_loss = 1;
            fr_error(fr, FRIEND_ERR_FULL,
                     str_lit("the scrollback filled before the oldest "
                             "messages had been folded into memories, so "
                             "they are gone: raise limits.max_scrollback "
                             "or limits.scrollback_bytes"));
            fr_push_event_text(fr, FRIEND_EVENT_ERROR, friend_last_error(fr));
        }
    }
}

static void fr_scroll_drop_oldest(Friend_Context *fr)
{
    Fr_Scrollback    *s = &fr->scroll;
    Fr_Message_Header header;

    if (s->count <= 0) return;
    memcpy(&header, fr_scroll_bytes_at(fr, *fr_scroll_slot(fr, s->first)),
           sizeof(header));
    fr_scroll_lost(fr, header.index);
    s->first = (s->first + 1) % s->capacity;
    s->count -= 1;
}

static b32 fr_scroll_push(Friend_Context *fr, const Fr_Chat_Event *event)
{
    Fr_Scrollback    *s = &fr->scroll;
    Fr_Message_Header header;
    size_t            need;
    u64               position;
    size_t            offset;
    u8               *at;
    i32               slot;
    i32               images;

    if (!fr || !event) return 0;
    if (s->capacity <= 0 || s->bytes.size == 0) {
        fr_scroll_lost(fr, event->index);
        return 0;
    }

    images = fr_scroll_images(event);
    need   = fr_scroll_record_bytes(event);
    if (need > s->bytes.size) { /* ruled out by FR_SCROLL_BYTES_MIN */
        fr_scroll_lost(fr, event->index);
        return 0;
    }

    position = (s->written + (FR_SCROLL_ALIGN - 1)) &
               ~(u64)(FR_SCROLL_ALIGN - 1);
    offset = (size_t)(position % s->bytes.size);
    if (offset + need > s->bytes.size) {
        position += s->bytes.size - offset;
        offset = 0;
    }

    while (s->count > 0 &&
           (position + need) - *fr_scroll_slot(fr, s->first) >
               (u64)s->bytes.size) {
        fr_scroll_drop_oldest(fr);
    }
    while (s->count >= s->capacity) fr_scroll_drop_oldest(fr);

    /* The machine would not give us the pages. The message is as gone
     * as one that fell off the front, and is counted as such. */
    if (!fr_pages_need(&s->bytes, offset + need)) {
        fr_scroll_lost(fr, event->index);
        return 0;
    }
    slot = (s->first + s->count) % s->capacity;
    if (!fr_pages_need(&s->positions, sizeof(u64) * ((size_t)slot + 1))) {
        fr_scroll_lost(fr, event->index);
        return 0;
    }

    fr_message_header_of(event, &header);
    header.image_count = (u8)images;

    at = s->bytes.base + offset;
    memcpy(at, &header, sizeof(header));
    at += sizeof(header);
    if (images > 0) {
        memcpy(at, event->images, sizeof(Fr_Image_Ref) * (size_t)images);
        at += sizeof(Fr_Image_Ref) * (size_t)images;
    }
    if (event->text_len > 0) {
        memcpy(at, event->text, (size_t)event->text_len);
        at += event->text_len;
    }
    if (event->captions_len > 0) {
        memcpy(at, event->captions, (size_t)event->captions_len);
    }

    *fr_scroll_slot(fr, slot) = position;
    s->count += 1;
    s->written = position + need;
    return 1;
}

static b32 fr_scroll_view(Friend_Context *fr, i32 index, Fr_Scroll_View *out)
{
    Fr_Scrollback *s = &fr->scroll;
    const u8      *at;
    i32            slot;

    if (!fr || !out || index < 0 || index >= s->count) return 0;

    slot = (s->first + index) % s->capacity;
    at   = fr_scroll_bytes_at(fr, *fr_scroll_slot(fr, slot));

    memset(out, 0, sizeof(*out));
    memcpy(&out->header, at, sizeof(out->header));
    at += sizeof(out->header);
    if (out->header.image_count > FR_CHAT_IMAGES_MAX) {
        out->header.image_count = FR_CHAT_IMAGES_MAX;
    }
    if (out->header.image_count > 0) {
        memcpy(out->images, at,
               sizeof(Fr_Image_Ref) * (size_t)out->header.image_count);
        at += sizeof(Fr_Image_Ref) * (size_t)out->header.image_count;
    }
    out->text = str_make((const char *)at, (i32)out->header.text_len);
    at += out->header.text_len;
    out->captions = str_make((const char *)at, (i32)out->header.captions_len);
    return 1;
}

static void fr_event_view(const Fr_Chat_Event *event, Fr_Scroll_View *out)
{
    memset(out, 0, sizeof(*out));
    fr_message_header_of(event, &out->header);
    out->header.image_count = (u8)fr_scroll_images(event);
    out->streaming          = event->streaming;
    memcpy(out->images, event->images,
           sizeof(Fr_Image_Ref) * (size_t)out->header.image_count);
    out->text     = fr_chat_text(event);
    out->captions = fr_chat_captions(event);
}

static void fr_scroll_to_event(const Fr_Scroll_View *view, Fr_Chat_Event *out)
{
    memset(out, 0, sizeof(*out));
    fr_message_header_into(out, &view->header);
    out->streaming = (u8)(view->streaming ? 1 : 0);
    memcpy(out->images, view->images,
           sizeof(Fr_Image_Ref) * (size_t)view->header.image_count);
    if (view->text.len > 0) {
        memcpy(out->text, view->text.data, (size_t)view->text.len);
    }
    if (view->captions.len > 0) {
        memcpy(out->captions, view->captions.data,
               (size_t)view->captions.len);
    }
}

static i32 fr_scroll_find(Friend_Context *fr, u64 event_index)
{
    i32 i;
    for (i = fr->scroll.count - 1; i >= 0; --i) {
        Fr_Scroll_View view;
        if (!fr_scroll_view(fr, i, &view)) break;
        if (view.header.index == event_index) return i;
        if (view.header.index < event_index) break; /* indices only rise */
    }
    return -1;
}

static void fr_scroll_reset(Friend_Context *fr)
{
    Fr_Scrollback *s = &fr->scroll;
    s->count         = 0;
    s->first         = 0;
    s->written       = 0;
    s->lost          = 0;
    s->unremembered  = 0;
    s->reported_loss = 0;
}

static i32 fr_unfolded_in_scrollback(Friend_Context *fr)
{
    i32 i;
    i32 unfolded = 0;

    for (i = fr->scroll.count - 1; i >= 0; --i) {
        Fr_Scroll_View view;
        if (!fr_scroll_view(fr, i, &view)) break;
        if (view.header.index < fr->folded_through) break;
        ++unfolded;
    }
    return unfolded;
}
