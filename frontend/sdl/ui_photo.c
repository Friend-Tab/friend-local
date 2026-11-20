/* ui_photo.c — pictures, in and out.
 *
 * Two directions and one decoder. Out: a photograph the friend holds
 * comes back from friend_message_attachment as JPEG bytes, which have to
 * become a texture before they can be a picture on a screen. In: a file
 * dropped on the window has to become the thing the library asks for —
 * "JPEG, already downscaled by you" — which is a decode, a scale, and an
 * encode.
 *
 * stb_image does the decoding and stb_image_write the encoding, because
 * neither a JPEG decoder nor an entropy coder is a thing to write for a
 * chat window. The scale in between is ours: it is a box filter over
 * RGBA, which is twenty lines and exactly right for making a photograph
 * smaller.
 *
 * A few decoded photographs are kept as textures, the ones drawn most
 * recently. A message's photograph is known by the message's place in
 * the whole conversation — its index plus however many messages the
 * friend has let go of, which does not move when old ones are dropped —
 * and one waiting to be sent by its own bytes.
 */

/* The long edge of what gets sent. The service sent about this, and it
 * is what the models are trained to read: past it the request gets
 * slower and the answer does not get better. */
#define UI_PHOTO_SEND_EDGE 1024
#define UI_PHOTO_SEND_QUALITY 85

/* How tall a photograph is drawn in the transcript. Fixed, so the
 * conversation does not reflow when one finishes decoding. */
#define UI_PHOTO_HEIGHT 184.0f
#define UI_PHOTOS_CACHED 8

/* Which photograph: a message's, by the message's ordinal in the whole
 * conversation and which of its attachments, or one waiting to be sent,
 * by where its JPEG is. */
typedef struct Ui_Photo_Key {
    u64         ordinal;
    i32         attachment;
    const void *pending;
} Ui_Photo_Key;

typedef struct Ui_Photo {
    SDL_Texture *texture;
    Ui_Photo_Key key;
    i32          width;
    i32          height;
    u64          drawn; /* frame number, for deciding which to drop */
} Ui_Photo;

typedef struct Ui_Photos {
    SDL_Renderer *renderer;
    Arena        *decode; /* stb's working memory; reset after each */
    Ui_Photo      cache[UI_PHOTOS_CACHED];
    u64           frame;
} Ui_Photos;

/* ------------------------------------------------------------------ *
 *  The decoder's allocator
 *
 *  stb_image allocates, and this program does not. The difference is
 *  settled by handing it an arena and resetting it when the decode is
 *  over: its frees are no-ops and its reallocs copy. Set up in
 *  ui_vendor.c, where the implementation is compiled.
 * ------------------------------------------------------------------ */

void ui_stb_use_arena(Arena *arena);

static Arena_Mark ui_decode_begin(Ui_Photos *photos)
{
    ui_stb_use_arena(photos->decode);
    return arena_mark(photos->decode);
}

static void ui_decode_end(Ui_Photos *photos, Arena_Mark mark)
{
    arena_reset_to(photos->decode, mark);
    ui_stb_use_arena(0);
}

/* ------------------------------------------------------------------ *
 *  Out: a photograph the friend holds
 * ------------------------------------------------------------------ */

static void ui_photos_init(Ui_Photos *photos, SDL_Renderer *renderer,
                           Arena *decode)
{
    memset(photos, 0, sizeof(*photos));
    photos->renderer = renderer;
    photos->decode   = decode;
}

static void ui_photo_drop(Ui_Photo *photo)
{
    if (photo->texture) SDL_DestroyTexture(photo->texture);
    memset(photo, 0, sizeof(*photo));
}

/* Everything decoded is forgotten: another friend is in hand, and its
 * ordinals are its own. */
static void ui_photos_forget(Ui_Photos *photos)
{
    i32 i;
    for (i = 0; i < UI_PHOTOS_CACHED; ++i) ui_photo_drop(&photos->cache[i]);
}

/* The ones waiting to be sent, whose bytes are about to be reused. */
static void ui_photos_forget_pending(Ui_Photos *photos)
{
    i32 i;
    for (i = 0; i < UI_PHOTOS_CACHED; ++i) {
        if (photos->cache[i].key.pending) ui_photo_drop(&photos->cache[i]);
    }
}

static b32 ui_photo_key_eq(Ui_Photo_Key a, Ui_Photo_Key b)
{
    return a.pending == b.pending && a.ordinal == b.ordinal &&
           a.attachment == b.attachment;
}

static SDL_Texture *ui_photo_upload(Ui_Photos *photos, const u8 *rgba,
                                    i32 width, i32 height)
{
    SDL_Texture *texture =
        SDL_CreateTexture(photos->renderer, SDL_PIXELFORMAT_RGBA32,
                          SDL_TEXTUREACCESS_STATIC, width, height);
    if (!texture) return 0;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    /* Photographs are drawn smaller than they are, always: the smooth
     * sampler is the difference between a photograph and a mosaic. */
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    SDL_UpdateTexture(texture, 0, rgba, width * 4);
    return texture;
}

/* A photograph the cache knows by its key, decoding the bytes if it
 * does not know it yet. */
static Ui_Photo *ui_photo_decoded(Ui_Photos *photos, Ui_Photo_Key key,
                                  Friend_String jpeg)
{
    Ui_Photo  *slot;
    Arena_Mark mark;
    u8        *rgba;
    int        width = 0, height = 0, channels = 0;
    i32        i;

    for (i = 0; i < UI_PHOTOS_CACHED; ++i) {
        Ui_Photo *photo = &photos->cache[i];
        if (!photo->texture || !ui_photo_key_eq(photo->key, key)) continue;
        photo->drawn = photos->frame;
        return photo;
    }
    if (jpeg.len <= 0) return 0;

    /* The least recently drawn one makes way. */
    slot = &photos->cache[0];
    for (i = 1; i < UI_PHOTOS_CACHED; ++i) {
        if (!photos->cache[i].texture) { slot = &photos->cache[i]; break; }
        if (photos->cache[i].drawn < slot->drawn) slot = &photos->cache[i];
    }
    ui_photo_drop(slot);

    mark = ui_decode_begin(photos);
    rgba = stbi_load_from_memory((const stbi_uc *)jpeg.data, (int)jpeg.len,
                                 &width, &height, &channels, 4);
    if (rgba && width > 0 && height > 0) {
        slot->texture = ui_photo_upload(photos, rgba, width, height);
        slot->key     = key;
        slot->width   = width;
        slot->height  = height;
        slot->drawn   = photos->frame;
    }
    ui_decode_end(photos, mark);
    return slot->texture ? slot : 0;
}

/* The photograph on one message, when the friend can hand over the
 * bytes. Returns 0 while a read is still in flight — the pump will
 * finish it and the next frame can ask again — and for a photograph
 * whose pixels were never kept, which draws as a placeholder. */
static Ui_Photo *ui_photo_for(Ui_Photos *photos, Friend_Context *fr,
                              i32 message, i32 attachment)
{
    Friend_String jpeg, caption;
    Ui_Photo_Key  key;
    Ui_Photo     *known;

    memset(&key, 0, sizeof(key));
    key.ordinal    = friend_stats(fr).messages_lost + (u64)message;
    key.attachment = attachment;
    known          = ui_photo_decoded(photos, key, str_nil);
    if (known) return known;
    if (friend_message_attachment(fr, message, attachment, &jpeg, &caption) !=
        FRIEND_OK) {
        return 0;
    }
    return ui_photo_decoded(photos, key, jpeg);
}

/* One of the photographs waiting to go with the next message. */
static Ui_Photo *ui_photo_pending(Ui_Photos *photos, Friend_String jpeg)
{
    Ui_Photo_Key key;
    memset(&key, 0, sizeof(key));
    key.pending = jpeg.data;
    return ui_photo_decoded(photos, key, jpeg);
}

/* A box filter: every destination pixel is the average of the source
 * pixels it covers. For making a photograph smaller this is what a
 * fancier filter would mostly agree with, and it is twenty lines. */
static void ui_photo_box_scale(const u8 *src, i32 src_w, i32 src_h, u8 *dst,
                               i32 dst_w, i32 dst_h)
{
    i32 y, x, c;
    for (y = 0; y < dst_h; ++y) {
        i32 y0 = (i32)((i64)y * src_h / dst_h);
        i32 y1 = (i32)(((i64)y + 1) * src_h / dst_h);
        if (y1 <= y0) y1 = y0 + 1;
        for (x = 0; x < dst_w; ++x) {
            i32 x0 = (i32)((i64)x * src_w / dst_w);
            i32 x1 = (i32)(((i64)x + 1) * src_w / dst_w);
            i32 sy;
            u32 sum[4] = { 0, 0, 0, 0 };
            u32 count  = 0;
            if (x1 <= x0) x1 = x0 + 1;
            for (sy = y0; sy < y1; ++sy) {
                i32 sx;
                for (sx = x0; sx < x1; ++sx) {
                    const u8 *p = src + ((size_t)sy * (size_t)src_w +
                                         (size_t)sx) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    sum[3] += p[3];
                    ++count;
                }
            }
            for (c = 0; c < 4; ++c) {
                dst[((size_t)y * (size_t)dst_w + (size_t)x) * 4 +
                    (size_t)c] =
                    (u8)(sum[c] / (count ? count : 1));
            }
        }
    }
}

/* A picture that ships with the program rather than with a friend: the
 * wordmark. Read once, scaled down to exactly the size it will be
 * drawn at, and kept as a texture until the display scale changes.
 *
 * Scaled here rather than by the renderer because the renderer's
 * bilinear sampler reads four texels whatever the ratio: taking a 532
 * pixel wordmark down to 72 that way skips seven pixels in eight, and
 * a serif face reduced like that comes apart — thin strokes drop in
 * and out and the result looks like a screenshot of a screenshot. The
 * box filter reads every pixel it is averaging, which is the whole
 * difference.
 *
 * The result is kept as an alpha mask with white underneath, so that a
 * colour modulation tints it exactly. That suits a one-colour mark and
 * nothing else.
 *
 * SDL_LoadFile is the one allocation here that is not out of the
 * block, and it is given back before this returns. */
static SDL_Texture *ui_picture_at_height(Ui_Photos *photos, const char *path,
                                         i32 want_h, i32 *out_w, i32 *out_h)
{
    Arena_Mark   mark;
    SDL_Texture *texture = 0;
    void        *bytes;
    size_t       len   = 0;
    u8          *rgba;
    u8          *small;
    int          width = 0, height = 0, channels = 0;
    i32          want_w;
    i32          i;

    if (want_h < 1) want_h = 1;
    bytes = SDL_LoadFile(path, &len);
    if (!bytes) return 0;

    mark = ui_decode_begin(photos);
    rgba = stbi_load_from_memory((const stbi_uc *)bytes, (int)len, &width,
                                 &height, &channels, 4);
    if (rgba && width > 0 && height > 0) {
        if (want_h > height) want_h = height;
        want_w = (i32)(((i64)width * want_h + height / 2) / height);
        if (want_w < 1) want_w = 1;

        small = arena_push_nz(photos->decode, u8,
                              (size_t)want_w * (size_t)want_h * 4);
        if (small) {
            ui_photo_box_scale(rgba, width, height, small, want_w, want_h);
            for (i = 0; i < want_w * want_h; ++i) {
                small[i * 4 + 0] = 255;
                small[i * 4 + 1] = 255;
                small[i * 4 + 2] = 255;
            }
            texture = ui_photo_upload(photos, small, want_w, want_h);
            if (out_w) *out_w = want_w;
            if (out_h) *out_h = want_h;
        }
    }
    ui_decode_end(photos, mark);
    SDL_free(bytes);
    return texture;
}

/* ------------------------------------------------------------------ *
 *  In: a file on its way to the friend
 * ------------------------------------------------------------------ */

/* Is this file a picture at all? stb_image decides, which means it
 * needs the arena: even reading a header allocates in there. */
static b32 ui_photo_is_picture(Ui_Photos *photos, const void *bytes,
                               size_t len)
{
    Arena_Mark mark;
    int        width = 0, height = 0, channels = 0;
    int        ok;

    if (len < 4) return 0;
    mark = ui_decode_begin(photos);
    ok   = stbi_info_from_memory((const stbi_uc *)bytes, (int)len, &width,
                                 &height, &channels);
    ui_decode_end(photos, mark);
    return ok ? 1 : 0;
}

typedef struct Ui_Jpeg_Sink {
    Arena *arena;
    i32    len;
    b32    failed;
} Ui_Jpeg_Sink;

static void ui_jpeg_write(void *context, void *data, int size)
{
    Ui_Jpeg_Sink *sink = (Ui_Jpeg_Sink *)context;
    void         *at;
    if (size <= 0 || sink->failed) return;
    at = arena_push_size_nz(sink->arena, (size_t)size, 1);
    if (!at) {
        sink->failed = 1;
        return;
    }
    memcpy(at, data, (size_t)size);
    sink->len += size;
}

/* Whatever the file was — JPEG, PNG, anything stb_image reads — comes
 * back as a JPEG no bigger than the friend wants, built at the tail of
 * `out`. Empty when the file was not a picture. */
static Friend_String ui_photo_for_sending(Ui_Photos *photos, Arena *out,
                                          const void *file_bytes, size_t len)
{
    Arena_Mark   mark = ui_decode_begin(photos);
    Ui_Jpeg_Sink sink;
    u8          *rgba;
    u8          *scaled;
    const u8    *source;
    int          width = 0, height = 0, channels = 0;
    i32          dst_w, dst_h;
    const u8    *start;

    rgba = stbi_load_from_memory((const stbi_uc *)file_bytes, (int)len, &width,
                                 &height, &channels, 4);
    if (!rgba || width <= 0 || height <= 0) {
        ui_decode_end(photos, mark);
        return str_nil;
    }

    dst_w = width;
    dst_h = height;
    if (width > UI_PHOTO_SEND_EDGE || height > UI_PHOTO_SEND_EDGE) {
        if (width >= height) {
            dst_w = UI_PHOTO_SEND_EDGE;
            dst_h = (i32)((i64)height * UI_PHOTO_SEND_EDGE / width);
        } else {
            dst_h = UI_PHOTO_SEND_EDGE;
            dst_w = (i32)((i64)width * UI_PHOTO_SEND_EDGE / height);
        }
        if (dst_w < 1) dst_w = 1;
        if (dst_h < 1) dst_h = 1;
    }

    source = rgba;
    if (dst_w != width || dst_h != height) {
        scaled = arena_push_nz(photos->decode, u8,
                               (size_t)dst_w * (size_t)dst_h * 4);
        if (!scaled) {
            ui_decode_end(photos, mark);
            return str_nil;
        }
        ui_photo_box_scale(rgba, width, height, scaled, dst_w, dst_h);
        source = scaled;
    }

    /* Encoded at the tail of the caller's arena, which is where it will
     * stay until the message it belongs to has been sent. */
    memset(&sink, 0, sizeof(sink));
    sink.arena = out;
    start      = (const u8 *)out->base + out->used;
    stbi_write_jpg_to_func(ui_jpeg_write, &sink, dst_w, dst_h, 4, source,
                           UI_PHOTO_SEND_QUALITY);

    ui_decode_end(photos, mark);
    if (sink.failed || sink.len <= 0) return str_nil;
    return str_make((const char *)start, sink.len);
}
