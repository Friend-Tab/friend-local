/* fr_blob.c — the photographs.
 *
 * Everything else the friend knows is small enough to hold: a memory is
 * five kilobytes, a message is a sentence. A photograph is not, and
 * there is no amount of fixed budget that is both enough for somebody
 * who sends their friend a picture every day and honest for somebody who
 * never sends one. So the pixels go in a file of their own and the
 * records keep an offset and a length.
 *
 * The file is append-only and never rewritten, which is what makes the
 * bookkeeping a pair of numbers rather than an allocator. A photo is
 * given its offset the moment it arrives, while it is still only in
 * memory; that offset is final, so the descriptor can be copied into a
 * message, into the scrollback, into a fold, without anything having to
 * go back and fix it up later. Whether the pixels are really on disk yet
 * is one comparison against one counter, which cannot disagree with
 * itself the way five copies of a flag would.
 *
 * Between arriving and being written, a photo lives in the staging
 * arena, and is read straight out of it — so a photo the person just
 * attached is on screen and in the next request immediately, rather than
 * after a round trip through the filesystem.
 *
 * One write is in flight at a time. The file's contents have to become
 * durable in the order they were given offsets, or blob_written would be
 * a lie about some range in the middle, and a lie in that direction
 * means showing somebody another photograph's bytes.
 */

#include "fr_io.h"

/* Big enough that an import's photographs usually go out in one write,
 * small enough that a write is not minutes of a frozen pump. */
#define FR_BLOB_WRITE_MAX FR_MB(8)

/* How long a failed read or write waits before it is tried again. A disk
 * that is full or a file that is gone does not fix itself in a tick, and
 * the pump should not spend every one finding that out again. */
#define FR_BLOB_RETRY_MS 10000u

static b32 fr_blob_have_file(const Friend_Context *fr)
{
    return !str_is_empty(fr->blob_path);
}

static b32 fr_blob_on_disk(const Friend_Context *fr, const Fr_Image_Ref *ref)
{
    return ref->blob_size > 0 &&
           ref->blob_offset + ref->blob_size <= fr->blob_written;
}

static b32 fr_blob_in_staging(const Friend_Context *fr, u64 offset, u32 size)
{
    u64 end = fr->staging_base + (u64)fr->image_staging.used;
    if (size == 0) return 0;
    if (offset < fr->staging_base) return 0;
    return offset + (u64)size <= end;
}

static String fr_blob_staged_bytes(Friend_Context *fr, u64 offset, u32 size)
{
    size_t at = (size_t)(offset - fr->staging_base);
    return str_make((const char *)fr->image_staging.base + at, (i32)size);
}

/* Photographs go with the friend that had them. The file they were in is
 * left alone — it is append-only and somebody else's state may still
 * point into it — and the next friend simply starts writing at the end of
 * whatever it is told it has. */
static void fr_blob_reset(Friend_Context *fr)
{
    arena_reset(&fr->image_staging);
    arena_reset(&fr->image_page);
    fr->blob_bytes            = 0;
    fr->blob_written          = 0;
    fr->staging_base          = 0;
    fr->staging_sent          = 0;
    fr->staging_writing       = 0;
    fr_retry_worked(&fr->blob_write_retry);
    fr->image_page_offset     = 0;
    fr->image_page_size       = 0;
    fr->image_page_ready      = 0;
    fr->image_page_failed     = 0;
    fr_retry_worked(&fr->image_read_retry);
    fr->image_read_id         = 0;
}

/* ------------------------------------------------------------------ *
 *  Arriving
 * ------------------------------------------------------------------ */

/* Describes a photo without keeping it: no blob_path to write to, no
 * room left in staging, or bytes that were not what they said. The record
 * still says a photograph was there and how big it was, which is what the
 * conversation needs; the pixels are what is gone. */
static void fr_blob_without_pixels(size_t jpeg, size_t thumb,
                                   Fr_Image_Ref *out)
{
    memset(out, 0, sizeof(*out));
    out->flags      = FR_IMAGE_PENDING;
    out->blob_size  = (u32)jpeg;
    out->thumb_size = (u32)thumb;
}

/* Room in staging for one photo, and its place in the file. No photo at
 * all is described as nothing. */
static u8 *fr_blob_reserve(Friend_Context *fr, size_t jpeg, size_t thumb,
                           Fr_Image_Ref *out)
{
    u8 *at = 0;

    memset(out, 0, sizeof(*out));
    out->flags = FR_IMAGE_PENDING;
    if (jpeg == 0) return 0;
    if (fr_blob_have_file(fr)) {
        at = (u8 *)arena_push_size_nz(&fr->image_staging, jpeg + thumb, 1);
    }
    if (!at) {
        fr_blob_without_pixels(jpeg, thumb, out);
        return 0;
    }

    out->flags        = 0;
    out->blob_offset  = fr->blob_bytes;
    out->blob_size    = (u32)jpeg;
    out->thumb_offset = thumb > 0 ? (u32)jpeg : 0;
    out->thumb_size   = (u32)thumb;
    fr->blob_bytes += (u64)(jpeg + thumb);
    fr_mark_dirty(fr);
    return at;
}

static b32 fr_blob_stage(Friend_Context *fr, String jpeg, String thumb,
                  Fr_Image_Ref *out)
{
    size_t jpeg_len  = jpeg.len > 0 ? (size_t)jpeg.len : 0;
    size_t thumb_len = thumb.len > 0 ? (size_t)thumb.len : 0;
    u8    *at        = fr_blob_reserve(fr, jpeg_len, thumb_len, out);

    if (!at) return 0;
    memcpy(at, jpeg.data, jpeg_len);
    if (thumb_len > 0) memcpy(at + jpeg_len, thumb.data, thumb_len);
    return 1;
}

/* The same, for a photograph that is still base64 in somebody else's
 * document: decoded straight into its place in staging, so an import
 * never holds a second copy of a picture. */
static b32 fr_blob_stage_base64(Friend_Context *fr, String jpeg, String thumb,
                         Fr_Image_Ref *out)
{
    size_t jpeg_len  = fr_base64_decoded_size(jpeg);
    size_t thumb_len = thumb.len > 0 ? fr_base64_decoded_size(thumb) : 0;
    u8    *at        = fr_blob_reserve(fr, jpeg_len, thumb_len, out);

    if (!at) return 0;
    if (fr_base64_decode(jpeg, at, jpeg_len) != (i64)jpeg_len ||
        (thumb_len > 0 &&
         fr_base64_decode(thumb, at + jpeg_len, thumb_len) !=
             (i64)thumb_len)) {
        /* Not base64 after all. The space it took keeps its place in the
         * file rather than being handed to the next photograph, because
         * the offsets this one was given are already out in a record. */
        memset(at, 0, jpeg_len + thumb_len);
        fr_blob_without_pixels(jpeg_len, thumb_len, out);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 *  Looking
 * ------------------------------------------------------------------ */

static String fr_blob_at(Friend_Context *fr, u64 offset, u32 size)
{
    if (size == 0) return str_nil;
    if (fr_blob_in_staging(fr, offset, size)) {
        return fr_blob_staged_bytes(fr, offset, size);
    }
    if (fr->image_page_ready && fr->image_page_offset == offset &&
        fr->image_page_size == size) {
        return str_make((const char *)fr->image_page.base, (i32)size);
    }
    return str_nil;
}

static String fr_blob_for_model(Friend_Context *fr, const Fr_Image_Ref *ref)
{
    if (!fr || !ref || (ref->flags & FR_IMAGE_PENDING)) return str_nil;
    if (ref->thumb_size > 0) {
        return fr_blob_at(fr, ref->blob_offset + ref->thumb_offset,
                          ref->thumb_size);
    }
    return fr_blob_at(fr, ref->blob_offset, ref->blob_size);
}

static b32 fr_blob_is_paged(const Friend_Context *fr, const Fr_Image_Ref *ref)
{
    return fr->image_page_offset == ref->blob_offset &&
           fr->image_page_size == ref->blob_size;
}

/* Starts the read that will put this photo in the image page, and says
 * where that leaves the caller: it is always "not yet". */
static Friend_Result fr_blob_start_read(Friend_Context *fr,
                                        const Fr_Image_Ref *ref)
{
    Fr_Io_Slot *slot;
    Fr_Request  read;

    if (fr->image_read_id != 0) {
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        fr_blob_is_paged(fr, ref)
                            ? str_lit("this photograph is being read from "
                                      "the file")
                            : str_lit("another photograph is being read "
                                      "from the file"));
    }
    if (fr->image_page_failed && fr_blob_is_paged(fr, ref) &&
        fr_retry_waiting(&fr->image_read_retry, fr->now_unix_ms)) {
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("this photograph could not be read from the "
                                "file; it will be tried again shortly"));
    }

    slot = fr_io_acquire(fr, FR_IO_PURPOSE_BLOB_READ, FRIEND_IO_FILE_READ, 0);
    if (!slot) {
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("every request slot is busy; the photograph "
                                "will be read when one is free"));
    }
    memset(&read, 0, sizeof(read));
    read.path   = fr->blob_path;
    read.offset = ref->blob_offset;
    read.length = ref->blob_size;

    /* Straight into the page it will be read out of: the transport
     * writes where the viewer will look, and nothing is copied. */
    arena_reset(&fr->image_page);
    read.recv = (u8 *)arena_push_size_nz(&fr->image_page,
                                         (size_t)ref->blob_size, 1);
    read.recv_capacity = ref->blob_size;
    if (!read.recv) {
        fr_io_release(fr, slot);
        return fr_error(fr, FRIEND_ERR_FULL,
                        str_lit("the machine would not give the page for "
                                "looking at a photograph its memory"));
    }
    if (!fr_io_post(fr, slot, read)) return FRIEND_ERR_FULL;

    fr->image_page_offset = ref->blob_offset;
    fr->image_page_size   = ref->blob_size;
    fr->image_page_ready  = 0;
    fr->image_page_failed = 0;
    fr->image_read_id     = slot->req.id;
    return fr_error(fr, FRIEND_ERR_BAD_STATE,
                    str_lit("this photograph is being read from the file"));
}

Friend_Result friend_message_attachment(Friend_Context *fr,
                                        i32 message_index,
                                        i32 attachment_index,
                                        Friend_String *out_jpeg,
                                        Friend_String *out_caption)
{
    Fr_Scroll_View view;
    Fr_Image_Ref   ref;
    String         bytes;

    if (out_jpeg) *out_jpeg = str_nil;
    if (out_caption) *out_caption = str_nil;
    if (!fr || attachment_index < 0 ||
        !fr_message_at(fr, message_index, &view) ||
        attachment_index >= (i32)view.header.image_count) {
        return FRIEND_ERR_BAD_ARGUMENT;
    }
    ref = view.images[attachment_index];
    if (out_caption) *out_caption = fr_caption_at(view.captions, attachment_index);

    if ((ref.flags & FR_IMAGE_PENDING) || ref.blob_size == 0) {
        return fr_error(fr, FRIEND_ERR_BAD_DATA,
                        str_lit("this photograph was described but its "
                                "pixels were never kept"));
    }

    bytes = fr_blob_at(fr, ref.blob_offset, ref.blob_size);
    if (!str_is_empty(bytes)) {
        if (out_jpeg) *out_jpeg = bytes;
        return FRIEND_OK;
    }

    if (!fr_blob_on_disk(fr, &ref)) {
        /* Written out but not yet confirmed, or lost with the session
         * that staged it. The first is worth waiting a tick for; the
         * second will keep saying this, which is the truth. */
        return fr_error(fr, FRIEND_ERR_BAD_STATE,
                        str_lit("this photograph is not in the file yet"));
    }
    if (!fr_blob_have_file(fr)) {
        return fr_error(fr, FRIEND_ERR_BAD_DATA,
                        str_lit("this friend has no photo file to read "
                                "from: set blob_path"));
    }
    if ((size_t)ref.blob_size > fr->image_page.size) {
        return fr_error(fr, FRIEND_ERR_BAD_DATA,
                        str_lit("this photograph is larger than the page "
                                "set aside to look at one: raise "
                                "limits.image_page_bytes"));
    }
    return fr_blob_start_read(fr, &ref);
}

/* ------------------------------------------------------------------ *
 *  Writing
 * ------------------------------------------------------------------ */

static void fr_blob_drain(Friend_Context *fr)
{
    Fr_Io_Slot *slot;
    Fr_Request  write;
    size_t      waiting;
    size_t      length;

    if (!fr || fr->staging_writing > 0 || !fr_blob_have_file(fr)) return;
    if (fr_retry_waiting(&fr->blob_write_retry, fr->now_unix_ms)) return;

    waiting = fr->image_staging.used - fr->staging_sent;
    if (waiting == 0) {
        /* Everything staged is on disk. Take the arena back for the next
         * photographs; the descriptors already point at the file. */
        if (fr->image_staging.used > 0 &&
            fr->blob_written >= fr->staging_base +
                                    (u64)fr->image_staging.used) {
            arena_reset(&fr->image_staging);
            fr->staging_sent = 0;
            fr->staging_base = fr->blob_bytes;
        }
        return;
    }

    length = waiting < FR_BLOB_WRITE_MAX ? waiting : FR_BLOB_WRITE_MAX;
    slot   = fr_io_acquire(fr, FR_IO_PURPOSE_BLOB_WRITE, FRIEND_IO_FILE_WRITE,
                           0);
    if (!slot) return; /* every slot is busy; next tick */

    memset(&write, 0, sizeof(write));
    write.path   = fr->blob_path;
    write.offset = fr->staging_base + (u64)fr->staging_sent;
    write.body   = str_make((const char *)fr->image_staging.base +
                                fr->staging_sent,
                            (i32)length);
    write.length = (u32)length;
    if (!fr_io_post(fr, slot, write)) return;

    fr->staging_writing = (u32)length;

}

static void fr_blob_route(Friend_Context *fr, Fr_Io_Slot *slot)
{
    if (slot->purpose == FR_IO_PURPOSE_BLOB_WRITE) {
        if (fr_io_failed_badly(slot)) {
            /* Nothing moves: the same bytes are offered again after a
             * wait. Said once per run of failures, which is enough for
             * the only error here a person can do something about. */
            fr->staging_writing     = 0;
            if (fr_retry_failed(&fr->blob_write_retry, fr->now_unix_ms,
                                FR_BLOB_RETRY_MS)) {
                fr_error(fr, FRIEND_ERR_IO,
                         str_is_empty(slot->error)
                             ? str_lit("the photographs could not be "
                                       "written")
                             : slot->error);
                fr_push_event_text(fr, FRIEND_EVENT_ERROR,
                                   friend_last_error(fr));
            }
            return;
        }
        fr_retry_worked(&fr->blob_write_retry);
        fr->staging_sent += fr->staging_writing;
        fr->blob_written = fr->staging_base + (u64)fr->staging_sent;
        fr->staging_writing = 0;
        /* Worth writing the friend out again: until it is, the state on
         * disk says these photographs are not in the file, and after a
         * restart it would be believed. */
        fr_mark_dirty(fr);
        return;
    }

    if (slot->purpose == FR_IO_PURPOSE_BLOB_READ) {
        fr->image_read_id = 0;
        if (fr_io_failed_badly(slot) || slot->recv_len < fr->image_page_size) {
            fr->image_page_ready    = 0;
            fr->image_page_failed   = 1;
            fr_retry_failed(&fr->image_read_retry, fr->now_unix_ms,
                            FR_BLOB_RETRY_MS);
            return;
        }
        fr->image_page_ready = 1;
    }
}
