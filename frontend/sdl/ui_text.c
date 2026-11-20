/* ui_text.c — shaping, wrapping, and glyphs on screen.
 *
 * Three libraries, each doing the one thing it is for:
 *
 *   kb_text_shape  turns text into positioned glyph ids. This is the part
 *                  nobody should write themselves: ligatures, marks,
 *                  cursive joining, bidirectional runs. A friend's chat
 *                  is whatever language its person speaks.
 *   kb_text_shape  also finds the places a line may be broken (UAX #14),
 *                  which is why wrapping here is not "break at spaces":
 *                  plenty of writing systems do not have any.
 *   stb_truetype   turns a glyph id into pixels.
 *
 * Both parse the same font file, which sounds wasteful and is not: the
 * shaper reads the OpenType layout tables, the rasterizer reads the
 * outlines, and they barely overlap. Advances come out of the shaper in
 * font units and are scaled by stb_truetype's em-to-pixel scale, so the
 * two agree about how big a pixel is.
 *
 * There is more than one face because the app has more than one: the
 * conversation is set in a grotesque and the friend's name is set in a
 * serif. They share the atlas and have a shaping context each.
 *
 * Sharing one context and choosing a face by pushing it looks tidier and
 * does not work. With two faces on the stack this shaper applied the
 * other one's substitutions while still naming the pushed face on the
 * run, so "Official" came back carrying the serif's ffi ligature id and
 * was rasterised out of the sans, where that id is some other letter
 * entirely. Popping a face after loading it is worse: the stack owns a
 * font pushed from memory, and using it afterwards takes the process
 * down. A context each is a few megabytes and leaves nothing to get
 * wrong — every glyph of a pass comes from the one face that context
 * holds.
 *
 * Nothing here allocates: the atlas and the caches are carved out of the
 * frontend's block at startup, and a layout's glyphs out of whatever
 * arena it is laid out into.
 */

#define UI_ATLAS_SIZE 2048
#define UI_GLYPH_SLOTS 8192
#define UI_ATLAS_PAD 1

/* Horizontal subpixel positions a glyph is rasterised at.
 *
 * Without this, a glyph whose pen lands at x=12.4 is drawn as a bitmap
 * baked for x=12 and sampled a tenth of a pixel off, which both softens
 * it and moves it — and since the error differs per glyph, the spacing
 * between letters wobbles. Rounding the pen instead would be crisp and
 * would throw away up to half a pixel per glyph, which is the same
 * wobble by another route. So each glyph is baked at four horizontal
 * offsets and the one matching its fractional position is drawn at a
 * whole pixel. Four is where the eye stops noticing. */
#define UI_SUBPIXEL 4

/* How far an icon is moved from the middle of its ink towards where
 * that ink balances. See ui_draw_icon. */
#define UI_OPTICAL_WEIGHT 0.5f
#define UI_FONTS_MAX 4

/* The faces that get a shaping context: the sans and the serif. The
 * shaper is handed its block whole and asks for no more. */
#define UI_SHAPED_FONTS_MAX 2
#define UI_SHAPE_PER_FONT FR_MB(4)
#define UI_SHAPE_BYTES (UI_SHAPED_FONTS_MAX * UI_SHAPE_PER_FONT)

/* The largest glyph rasterised, in pixels: a 256-pixel square is an
 * emoji at four times the size anything here is set. */
#define UI_RASTER_EDGE 256

typedef struct Ui_Glyph_Slot {
    u32 key; /* 0 is free; otherwise (size, phase, glyph) packed, plus 1 */
    i16 x, y, w, h;
    i16 bearing_x, bearing_y;
    /* Where the ink actually balances, in sixteenths of a pixel from the
     * slot's top-left. Not the middle of the box: an arrow carries its
     * weight in the head, so the two are a pixel or two apart and the
     * eye goes by the weight. */
    i16 balance_x, balance_y;
} Ui_Glyph_Slot;

typedef struct Ui_Glyph {
    u16 id;
    u8  font;
    u8  pad;
    i32 codepoint; /* which code point of the text it came from        */
    f32 pen;       /* where the pen was, from the left of its line     */
    f32 x;         /* the pen plus the shaper's offset                 */
    f32 baseline;  /* from the top of the block                        */
} Ui_Glyph;

/* A piece of text shaped and wrapped, ready to draw and to ask about.
 * Its glyphs live in the arena it was laid out into. edges[i] is where
 * the pen stood before code point i, within its line, and
 * edges[codepoints] where it stopped, so a caret, a selection and a
 * click all come from the same layout the text was drawn from. */
typedef struct Ui_Layout {
    Ui_Glyph *glyphs;
    i32       glyph_count;
    f32      *edges;
    i32       codepoints;
    i32       line_count;
    f32       width; /* the widest line */
    f32       height;
    i32       font;
    i32       size;  /* pixels per em */
} Ui_Layout;

typedef struct Ui_Font {
    u8                 *bytes;
    i32                 byte_count;
    stbtt_fontinfo      raster;
    kbts_shape_context *shaper;
    kbts_font          *shape_font;
    f32                 units_to_pixels; /* at one pixel per em */
    f32                 ascent, descent;
    Ui_Glyph_Slot      *slots;
} Ui_Font;

/* Limits that were reached, each said once rather than every frame. */
enum {
    UI_TEXT_ATLAS_FULL  = 1u << 0,
    UI_TEXT_SLOTS_FULL  = 1u << 1,
    UI_TEXT_GLYPH_LARGE = 1u << 2,
    UI_TEXT_GLYPHS_CUT  = 1u << 3,
    UI_TEXT_NO_ROOM     = 1u << 4
};

typedef struct Ui_Text {
    Arena  *shape_arena; /* carved up a context at a time, as faces load */
    Ui_Font fonts[UI_FONTS_MAX];
    i32     font_count;

    /* Break opportunities for the text being laid out. */
    kbts_break *breaks;
    i32         break_capacity;
    i32         break_count; /* what the last call wrote, so it can be cleared */
    u8         *break_bits;      /* indexed by codepoint */
    u8         *byte_break_bits; /* indexed by byte, as the shaper reports */
    i32         break_bits_capacity;

    /* The atlas: coverage in alpha, white everywhere else, so one texture
     * draws text in any colour through SDL's colour modulation. */
    u8          *atlas_pixels; /* RGBA */
    SDL_Texture *atlas;
    i32          pack_x, pack_y, row_height;
    b32          atlas_dirty;
    b32          atlas_full; /* start it again once this frame is drawn */
    i32          dirty_y0, dirty_y1;
    u8          *raster_scratch;

    u32 said;
} Ui_Text;

static void ui_text_complain(Ui_Text *t, u32 which, const char *what)
{
    if (t->said & which) return;
    t->said |= which;
    SDL_Log("text: %s", what);
}

/* ------------------------------------------------------------------ *
 *  Setup
 * ------------------------------------------------------------------ */

static b32 ui_text_init(Ui_Text *t, SDL_Renderer *renderer, Arena *arena,
                        Arena *shape_arena)
{
    memset(t, 0, sizeof(*t));
    t->shape_arena = shape_arena;

    /* About one break per character, over the longest message a record
     * can hold. */
    t->break_capacity      = 24000;
    t->breaks              = arena_push(arena, kbts_break, t->break_capacity);
    t->break_bits_capacity = FR_KB(16);
    t->break_bits          = arena_push(arena, u8, t->break_bits_capacity);
    t->byte_break_bits     = arena_push(arena, u8, t->break_bits_capacity);
    t->atlas_pixels = arena_push(arena, u8, UI_ATLAS_SIZE * UI_ATLAS_SIZE * 4);
    t->raster_scratch = arena_push(arena, u8, UI_RASTER_EDGE * UI_RASTER_EDGE);
    if (!t->breaks || !t->break_bits || !t->byte_break_bits ||
        !t->atlas_pixels || !t->raster_scratch) {
        return 0;
    }

    t->atlas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                 SDL_TEXTUREACCESS_STATIC, UI_ATLAS_SIZE,
                                 UI_ATLAS_SIZE);
    if (!t->atlas) return 0;
    SDL_SetTextureBlendMode(t->atlas, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t->atlas, SDL_SCALEMODE_NEAREST);

    t->pack_x   = UI_ATLAS_PAD;
    t->pack_y   = UI_ATLAS_PAD;
    t->dirty_y0 = UI_ATLAS_SIZE;
    t->dirty_y1 = 0;
    return 1;
}

/* Loads a face and returns its index, or -1. The first one loaded is the
 * one line heights are measured against, so that a line is the same
 * height whichever face happens to fill it.
 *
 * `shaping` buys the face a shaping context, which is most of what a
 * face costs. A face of icons does not need one: an icon is one
 * codepoint drawn straight from the cmap, with no ligature, no mark and
 * no direction to work out. */
static i32 ui_text_add_font(Ui_Text *t, Arena *arena, String path,
                            b32 shaping)
{
    Ui_Font      *font;
    SDL_IOStream *io;
    Sint64        size;
    size_t        read;
    i32           ascent = 0, descent = 0, line_gap = 0;
    char          name[512];

    if (t->font_count >= UI_FONTS_MAX) return -1;
    if (path.len <= 0 || path.len >= (i32)sizeof(name)) return -1;
    memcpy(name, path.data, (size_t)path.len);
    name[path.len] = 0;

    font = &t->fonts[t->font_count];
    memset(font, 0, sizeof(*font));

    /* Read whole: both libraries keep pointers into it for as long as
     * they live. */
    io = SDL_IOFromFile(name, "rb");
    if (!io) return -1;
    size = SDL_GetIOSize(io);
    if (size <= 0 || size > (Sint64)arena_remaining(arena)) {
        SDL_Log("%s is %lld bytes, and the window set aside %llu for text",
                name, (long long)size,
                (unsigned long long)arena_remaining(arena));
        SDL_CloseIO(io);
        return -1;
    }
    font->bytes = arena_push_nz(arena, u8, (size_t)size);
    read        = font->bytes ? SDL_ReadIO(io, font->bytes, (size_t)size) : 0;
    SDL_CloseIO(io);
    if (read != (size_t)size) return -1;
    font->byte_count = (i32)read;

    /* A collection (.ttc, which is how macOS ships Helvetica) holds
     * several faces; the first is the regular one. A plain .ttf says 0. */
    {
        int offset = stbtt_GetFontOffsetForIndex(font->bytes, 0);
        if (offset < 0 ||
            !stbtt_InitFont(&font->raster, font->bytes, offset)) {
            return -1;
        }
    }
    stbtt_GetFontVMetrics(&font->raster, &ascent, &descent, &line_gap);

    font->units_to_pixels =
        stbtt_ScaleForMappingEmToPixels(&font->raster, 1.0f);
    font->ascent  = (f32)ascent * font->units_to_pixels;
    font->descent = (f32)descent * font->units_to_pixels;

    if (shaping) {
        Arena sub = arena_sub(t->shape_arena, UI_SHAPE_PER_FONT, 16);
        /* The shaper is handed the block and writes into it on its own
         * terms, so the whole slice has to be real before it starts:
         * the arena will not see those writes coming. */
        if (!sub.base || !arena_commit_all(&sub)) return -1;
        font->shaper =
            kbts_PlaceShapeContextFixedMemory(sub.base, (int)sub.size);
        if (!font->shaper) return -1;
        font->shape_font = kbts_ShapePushFontFromMemory(
            font->shaper, font->bytes, font->byte_count, 0);
        if (!font->shape_font) return -1;
    }

    font->slots = arena_push(arena, Ui_Glyph_Slot, UI_GLYPH_SLOTS);
    if (!font->slots) return -1;
    return t->font_count++;
}

/* The site sets its messages at leading-relaxed, which is airier than
 * the font's own metrics would give, so the line box is a multiple of
 * the size and the text is centred in it rather than hung from the
 * font's ascent. */
#define UI_LINE_HEIGHT 1.5f

static f32 ui_line_height(f32 size) { return size * UI_LINE_HEIGHT; }

/* Where the baseline sits inside that box. */
static f32 ui_baseline_in_line(Ui_Text *t, f32 size)
{
    Ui_Font *font   = &t->fonts[0];
    f32      text_h = (font->ascent - font->descent) * size;
    f32      pad    = (ui_line_height(size) - text_h) * 0.5f;
    return pad + font->ascent * size;
}

/* ------------------------------------------------------------------ *
 *  The atlas
 * ------------------------------------------------------------------ */

static Ui_Glyph_Slot *ui_glyph_blank(Ui_Glyph_Slot *slot, u32 key)
{
    memset(slot, 0, sizeof(*slot));
    slot->key = key;
    return slot;
}

static Ui_Glyph_Slot *ui_glyph_slot(Ui_Text *t, Ui_Font *font, u16 glyph_id,
                                    i32 size, i32 phase)
{
    u32 key = (((u32)size << 18) | ((u32)phase << 16) | (u32)glyph_id) + 1u;
    u32 at  = (key * 2654435761u) & (UI_GLYPH_SLOTS - 1u);
    i32 probe;
    i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0, w, h, row;
    i32 trim_x = 0, trim_y = 0, scratch_pitch;
    f32 scale, shift;
    f32 weighted_x = 0.0f, weighted_y = 0.0f, total_weight = 0.0f;

    for (probe = 0; probe < UI_GLYPH_SLOTS; ++probe) {
        Ui_Glyph_Slot *slot = &font->slots[at];
        if (slot->key == key) return slot;
        if (slot->key == 0) break;
        at = (at + 1) & (UI_GLYPH_SLOTS - 1u);
    }
    if (probe >= UI_GLYPH_SLOTS) {
        ui_text_complain(t, UI_TEXT_SLOTS_FULL,
                         "the glyph cache is full; starting it again");
        t->atlas_full = 1;
    }
    if (t->atlas_full) return 0;

    scale = (f32)size * font->units_to_pixels;
    shift = (f32)phase / (f32)UI_SUBPIXEL;
    stbtt_GetGlyphBitmapBoxSubpixel(&font->raster, glyph_id, scale, scale,
                                    shift, 0.0f, &x0, &y0, &x1, &y1);
    w = x1 - x0;
    h = y1 - y0;
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    /* A blank glyph — a space — still earns a slot, so we stop asking. */
    if (w == 0 || h == 0) return ui_glyph_blank(&font->slots[at], key);
    if (w > UI_RASTER_EDGE || h > UI_RASTER_EDGE) {
        ui_text_complain(t, UI_TEXT_GLYPH_LARGE,
                         "a glyph larger than the rasteriser's square was "
                         "left blank");
        return ui_glyph_blank(&font->slots[at], key);
    }

    scratch_pitch = w;
    stbtt_MakeGlyphBitmapSubpixel(&font->raster, t->raster_scratch, w, h, w,
                                  scale, scale, shift, 0.0f, glyph_id);

    /* Trim to the ink. The box a face reports can be looser than what it
     * draws — Phosphor's icons carry their design square, so an arrow
     * whose ink stops four columns short of the right edge of its box
     * sits two pixels right of centre in a button if you trust the box.
     * Text does not notice, because the bearing moves with the trim;
     * anything that centres a glyph needs it. */
    {
        i32 left = w, right = -1, top = h, bottom = -1, column;
        for (row = 0; row < h; ++row) {
            const u8 *source = t->raster_scratch + (size_t)row * (size_t)w;
            for (column = 0; column < w; ++column) {
                if (!source[column]) continue;
                if (column < left) left = column;
                if (column > right) right = column;
                if (row < top) top = row;
                if (row > bottom) bottom = row;
            }
        }
        if (right < left || bottom < top) {
            return ui_glyph_blank(&font->slots[at], key);
        }
        x0 += left;
        y0 += top;
        trim_x = left;
        trim_y = top;
        w      = right - left + 1;
        h      = bottom - top + 1;
    }

    /* Shelf packing: fill a row, then start a taller one below it. */
    if (t->pack_x + w + UI_ATLAS_PAD > UI_ATLAS_SIZE) {
        t->pack_x = UI_ATLAS_PAD;
        t->pack_y += t->row_height + UI_ATLAS_PAD;
        t->row_height = 0;
    }
    if (t->pack_y + h + UI_ATLAS_PAD > UI_ATLAS_SIZE) {
        ui_text_complain(t, UI_TEXT_ATLAS_FULL,
                         "the glyph atlas is full; starting it again");
        t->atlas_full = 1;
        return 0;
    }

    for (row = 0; row < h; ++row) {
        u8 *dest = t->atlas_pixels +
                   ((size_t)(t->pack_y + row) * UI_ATLAS_SIZE +
                    (size_t)t->pack_x) * 4;
        const u8 *source = t->raster_scratch +
                           (size_t)(row + trim_y) * (size_t)scratch_pitch +
                           (size_t)trim_x;
        i32 column;
        for (column = 0; column < w; ++column) {
            f32 coverage = (f32)source[column];
            dest[column * 4 + 0] = 255;
            dest[column * 4 + 1] = 255;
            dest[column * 4 + 2] = 255;
            dest[column * 4 + 3] = source[column];
            /* Centre of mass, accumulated while the pixels are to hand. */
            weighted_x += coverage * ((f32)column + 0.5f);
            weighted_y += coverage * ((f32)row + 0.5f);
            total_weight += coverage;
        }
    }
    if (total_weight <= 0.0f) {
        weighted_x   = (f32)w * 0.5f;
        weighted_y   = (f32)h * 0.5f;
        total_weight = 1.0f;
    }

    {
        Ui_Glyph_Slot *slot = &font->slots[at];
        slot->key       = key;
        slot->x         = (i16)t->pack_x;
        slot->y         = (i16)t->pack_y;
        slot->w         = (i16)w;
        slot->h         = (i16)h;
        slot->bearing_x = (i16)x0;
        slot->bearing_y = (i16)y0;
        slot->balance_x = (i16)(weighted_x * 16.0f / total_weight + 0.5f);
        slot->balance_y = (i16)(weighted_y * 16.0f / total_weight + 0.5f);

        t->pack_x += w + UI_ATLAS_PAD;
        if (h > t->row_height) t->row_height = h;
        t->atlas_dirty = 1;
        if (slot->y < t->dirty_y0) t->dirty_y0 = slot->y;
        if (slot->y + h > t->dirty_y1) t->dirty_y1 = slot->y + h;
        return slot;
    }
}

/* One upload a frame, covering only the rows that changed. An atlas that
 * filled up is emptied once the frame is drawn, so the glyphs missing
 * from this one are back in the next. */
static void ui_text_flush_atlas(Ui_Text *t)
{
    SDL_Rect rect;
    if (t->atlas_dirty) {
        rect.x = 0;
        rect.y = t->dirty_y0;
        rect.w = UI_ATLAS_SIZE;
        rect.h = t->dirty_y1 - t->dirty_y0;
        if (rect.h > 0) {
            SDL_UpdateTexture(t->atlas, &rect,
                              t->atlas_pixels +
                                  (size_t)rect.y * UI_ATLAS_SIZE * 4,
                              UI_ATLAS_SIZE * 4);
        }
        t->atlas_dirty = 0;
        t->dirty_y0    = UI_ATLAS_SIZE;
        t->dirty_y1    = 0;
    }
    if (t->atlas_full) {
        i32 i;
        for (i = 0; i < t->font_count; ++i) {
            memset(t->fonts[i].slots, 0,
                   sizeof(Ui_Glyph_Slot) * UI_GLYPH_SLOTS);
        }
        t->pack_x     = UI_ATLAS_PAD;
        t->pack_y     = UI_ATLAS_PAD;
        t->row_height = 0;
        t->atlas_full = 0;
    }
}

/* ------------------------------------------------------------------ *
 *  Layout
 * ------------------------------------------------------------------ */

static i32 ui_floor_i32(f32 v)
{
    i32 truncated = (i32)v;
    return ((f32)truncated > v) ? truncated - 1 : truncated;
}

/* Takes the codepoint index a glyph came from, which is what the context
 * API puts in UserIdOrCodepointIndex. */
static b32 ui_can_break_before(Ui_Text *t, i32 codepoint)
{
    /* Out of range means we do not know, and not knowing has to read as
     * "no break here": a yes would let a line break anywhere at all,
     * which looks enough like wrapping to hide the mistake. */
    if (codepoint <= 0 || codepoint >= t->break_bits_capacity * 8) return 0;
    return (t->break_bits[codepoint >> 3] >> (codepoint & 7)) & 1u;
}

static i32 ui_utf8_length(u8 lead)
{
    if ((lead & 0x80u) == 0) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1; /* a stray continuation byte: step over it and carry on */
}

/* Where a line may end. The shaper reports positions as byte offsets into
 * the UTF-8 it was given, but a shaped glyph carries a *codepoint* index
 * — the context API says so plainly, whatever user-id mode you ask for —
 * so the two are converted here rather than confused later. They agree
 * exactly until the first character outside ASCII, which is the sort of
 * bug that looks like it works. Returns how many code points there are,
 * counted the way the shaper counts them. */
static i32 ui_find_breaks(Ui_Text *t, String text)
{
    i32 break_count = 0, flag_count = 0, i;
    i32 bytes = (text.len + 7) / 8 + 1;
    i32 byte, codepoint;

    if (bytes > t->break_bits_capacity) bytes = t->break_bits_capacity;
    memset(t->break_bits, 0, (size_t)bytes);
    memset(t->byte_break_bits, 0, (size_t)bytes);
    if (text.len <= 0) return 0;

    /* The shaper fills in Position on every break it reports but only
     * sets Flags on some of them, so entries it leaves alone still carry
     * the flags the last call put there. Clearing what the last call
     * wrote is what stops one message's break positions turning up in
     * the next one's — which reads as wrapping that works except where
     * it does not. */
    memset(t->breaks, 0, sizeof(kbts_break) * (size_t)t->break_count);

    kbts_BreakEntireStringUtf8(KBTS_DIRECTION_DONT_KNOW,
                               KBTS_JAPANESE_LINE_BREAK_STYLE_NORMAL, 0,
                               text.data, text.len, t->breaks,
                               t->break_capacity, &break_count, 0, 0,
                               &flag_count);
    if (break_count > t->break_capacity) break_count = t->break_capacity;
    t->break_count = break_count;

    for (i = 0; i < break_count; ++i) {
        i32 at = t->breaks[i].Position;
        if (!(t->breaks[i].Flags & KBTS_BREAK_FLAG_LINE)) continue;
        if (at <= 0 || at >= bytes * 8) continue;
        t->byte_break_bits[at >> 3] |= (u8)(1u << (at & 7));
    }

    for (byte = 0, codepoint = 0; byte < text.len; ++codepoint) {
        if ((t->byte_break_bits[byte >> 3] >> (byte & 7)) & 1u) {
            if (codepoint < bytes * 8) {
                t->break_bits[codepoint >> 3] |= (u8)(1u << (codepoint & 7));
            }
        }
        byte += ui_utf8_length((u8)text.data[byte]);
    }
    return codepoint;
}

/* Where the pen stood before each code point. A code point that made no
 * glyph of its own — the tail of a ligature — shares the edge of what
 * follows it. */
static void ui_layout_edges(Ui_Layout *layout, f32 end)
{
    i32 i;
    for (i = 0; i < layout->glyph_count; ++i) {
        i32 at = layout->glyphs[i].codepoint;
        if (at < 0 || at >= layout->codepoints) continue;
        if (layout->edges[at] < 0.0f) layout->edges[at] = layout->glyphs[i].pen;
    }
    layout->edges[layout->codepoints] = end;
    for (i = layout->codepoints - 1; i >= 0; --i) {
        if (layout->edges[i] < 0.0f) layout->edges[i] = layout->edges[i + 1];
    }
}

/* Shapes text at `size` pixels, wraps it to wrap_width, and leaves the
 * glyphs in `arena`, positioned relative to the top-left of the block.
 * wrap_width <= 0 means one line.
 *
 * The wrap is greedy and walks back to the last place the shaper said a
 * break was allowed. A word longer than the line is let overflow rather
 * than cut, because cutting inside a word is a worse lie than a long
 * line. */
static Ui_Layout ui_text_layout(Ui_Text *t, Arena *arena, i32 font_index,
                                String text, i32 size, f32 wrap_width)
{
    Ui_Layout           layout;
    kbts_run            run;
    f32                 line_h     = ui_line_height((f32)size);
    f32                 pen        = 0;
    i32                 line       = 0;
    i32                 count      = 0;
    i32                 capacity;
    i32                 line_start = 0;  /* first glyph of the line being filled */
    i32                 break_at   = -1; /* glyph index a break is allowed before */
    f32                 break_pen  = 0;  /* pen position at that glyph */
    f32                 ascent;
    kbts_shape_context *shaper;

    memset(&layout, 0, sizeof(layout));
    if (font_index < 0 || font_index >= t->font_count ||
        !t->fonts[font_index].shaper) {
        font_index = 0; /* a face with no shaper is a face of icons */
    }
    layout.font = font_index;
    layout.size = size;
    if (text.len < 0) text.len = 0;

    /* Never more glyphs than bytes, give or take the dotted circle a
     * shaper puts in front of a mark with nothing to sit on. */
    capacity      = text.len + 16;
    layout.glyphs = arena_push_nz(arena, Ui_Glyph, capacity);
    layout.edges  = arena_push_nz(arena, f32, text.len + 1);
    if (!layout.glyphs || !layout.edges) {
        ui_text_complain(t, UI_TEXT_NO_ROOM,
                         "a frame had more text than room to lay it out");
        memset(&layout, 0, sizeof(layout));
        return layout;
    }
    layout.edges[0] = 0;
    if (t->font_count == 0 || text.len == 0) return layout;

    ascent            = ui_baseline_in_line(t, (f32)size);
    layout.codepoints = ui_find_breaks(t, text);
    {
        i32 i;
        for (i = 0; i <= layout.codepoints; ++i) layout.edges[i] = -1.0f;
    }

    shaper = t->fonts[font_index].shaper;
    kbts_ShapeBegin(shaper, KBTS_DIRECTION_DONT_KNOW,
                    KBTS_LANGUAGE_DONT_KNOW);
    /* Codepoint indices, which is what the context API returns whatever
     * is asked for. Starting them at zero explicitly matters: the plain
     * kbts_ShapeUtf8 continues the context's own counter, which climbs
     * for the life of the program, and indices that walk off the end of
     * the break table are the kind of bug that still looks like wrapping. */
    kbts_ShapeUtf8WithUserId(shaper, text.data, text.len, 0,
                             KBTS_USER_ID_GENERATION_MODE_CODEPOINT_INDEX);
    kbts_ShapeEnd(shaper);

    while (kbts_ShapeRun(shaper, &run)) {
        kbts_glyph *glyph;
        f32         scale = (f32)size * t->fonts[font_index].units_to_pixels;

        while (kbts_GlyphIteratorNext(&run.Glyphs, &glyph)) {
            f32       advance      = (f32)glyph->AdvanceX * scale;
            i32       at_codepoint = glyph->UserIdOrCodepointIndex;
            Ui_Glyph *placed;

            if (count >= capacity) {
                ui_text_complain(t, UI_TEXT_GLYPHS_CUT,
                                 "a piece of text shaped into more glyphs "
                                 "than it has bytes; the rest was dropped");
                break;
            }

            /* A hard break in the text itself. The newline glyph carries
             * no ink, so it is not placed; its edge is the end of its
             * line. */
            if (glyph->Codepoint == '\n') {
                if (at_codepoint >= 0 && at_codepoint < layout.codepoints) {
                    layout.edges[at_codepoint] = pen;
                }
                ++line;
                pen        = 0;
                line_start = count;
                break_at   = -1;
                continue;
            }

            if (ui_can_break_before(t, at_codepoint) && count > line_start) {
                break_at  = count;
                break_pen = pen;
            }

            if (wrap_width > 0 && pen + advance > wrap_width &&
                count > line_start) {
                if (break_at > line_start) {
                    /* Move everything since the break down a line. */
                    i32 i;
                    f32 shift = break_pen;
                    ++line;
                    for (i = break_at; i < count; ++i) {
                        layout.glyphs[i].pen -= shift;
                        layout.glyphs[i].x -= shift;
                        layout.glyphs[i].baseline += line_h;
                    }
                    pen -= shift;
                    line_start = break_at;
                    break_at   = -1;
                } else {
                    /* Nowhere to break: start a new line here. */
                    ++line;
                    pen        = 0;
                    line_start = count;
                    break_at   = -1;
                }
            }

            placed            = &layout.glyphs[count++];
            placed->id        = glyph->Id;
            placed->font      = (u8)font_index;
            placed->pad       = 0;
            placed->codepoint = at_codepoint;
            placed->pen       = pen;
            placed->x         = pen + (f32)glyph->OffsetX * scale;
            placed->baseline =
                (f32)line * line_h + ascent - (f32)glyph->OffsetY * scale;

            pen += advance;
            if (pen > layout.width) layout.width = pen;
        }
    }
    layout.glyph_count = count;
    layout.line_count  = line + 1;
    layout.height      = (f32)layout.line_count * line_h;
    ui_layout_edges(&layout, pen);
    if (wrap_width > 0 && layout.width > wrap_width) layout.width = wrap_width;
    return layout;
}

/* Where the pen stood before code point `index`. */
static f32 ui_layout_x(const Ui_Layout *layout, i32 index)
{
    if (!layout->edges || index <= 0) return 0;
    if (index > layout->codepoints) index = layout->codepoints;
    return layout->edges[index];
}

/* The code point boundary nearest x. */
static i32 ui_layout_index_at(const Ui_Layout *layout, f32 x)
{
    i32 best = 0, i;
    f32 best_away = 1.0e30f;
    if (!layout->edges) return 0;
    for (i = 0; i <= layout->codepoints; ++i) {
        f32 away = layout->edges[i] - x;
        if (away < 0) away = -away;
        if (away < best_away) {
            best_away = away;
            best      = i;
        }
    }
    return best;
}

static void ui_text_draw(Ui_Text *t, SDL_Renderer *renderer,
                         const Ui_Layout *layout, f32 x, f32 y,
                         Ui_Color color)
{
    i32 i;
    if (t->font_count == 0 || layout->glyph_count <= 0) return;

    SDL_SetTextureColorMod(t->atlas, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(t->atlas, color.a);

    for (i = 0; i < layout->glyph_count; ++i) {
        const Ui_Glyph *glyph = &layout->glyphs[i];
        f32             pen_x = x + glyph->x;
        f32             pen_y = y + glyph->baseline;
        i32             whole = ui_floor_i32(pen_x);
        i32             phase = (i32)((pen_x - (f32)whole) * UI_SUBPIXEL);
        Ui_Glyph_Slot  *slot;
        SDL_FRect       source, dest;

        if (phase < 0) phase = 0;
        if (phase >= UI_SUBPIXEL) phase = UI_SUBPIXEL - 1;
        slot = ui_glyph_slot(t, &t->fonts[glyph->font], glyph->id,
                             layout->size, phase);
        if (!slot || slot->w == 0) continue;

        source.x = (f32)slot->x;
        source.y = (f32)slot->y;
        source.w = (f32)slot->w;
        source.h = (f32)slot->h;
        /* Whole pixels, so the atlas is sampled one texel to one pixel
         * and the glyph comes out as crisp as it was baked. */
        dest.x = (f32)(whole + slot->bearing_x);
        dest.y = (f32)(ui_floor_i32(pen_y + 0.5f) + slot->bearing_y);
        dest.w = (f32)slot->w;
        dest.h = (f32)slot->h;
        SDL_RenderTexture(renderer, t->atlas, &source, &dest);
    }
}

/* An icon is one codepoint from a face that holds nothing but icons, so
 * it skips shaping entirely — there is nothing to shape — and is placed
 * by its ink rather than its baseline, which is what "centred in this
 * button" means for a picture. */
static void ui_draw_icon(Ui_Text *t, SDL_Renderer *renderer, i32 font_index,
                         u32 codepoint, f32 center_x, f32 center_y, i32 size,
                         Ui_Color color)
{
    Ui_Font       *font;
    Ui_Glyph_Slot *slot;
    SDL_FRect      source, dest;
    i32            glyph;

    if (font_index <= 0 || font_index >= t->font_count) return;
    font  = &t->fonts[font_index];
    glyph = stbtt_FindGlyphIndex(&font->raster, (int)codepoint);
    if (glyph <= 0) return;

    slot = ui_glyph_slot(t, font, (u16)glyph, size, 0);
    if (!slot || slot->w == 0) return;

    SDL_SetTextureColorMod(t->atlas, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(t->atlas, color.a);

    source.x = (f32)slot->x;
    source.y = (f32)slot->y;
    source.w = (f32)slot->w;
    source.h = (f32)slot->h;
    /* Between the middle of the ink and where it balances.
     *
     * Neither alone is right. The middle of the box leans the way an
     * arrow points, because all the weight is in the head. The balance
     * point over-corrects the other way, because the shaft reaches a
     * long way left while weighing almost nothing, and the eye counts
     * reach as well as weight. Halfway between is the usual answer and
     * it is a judgement, not a derivation — this is the number to turn
     * if an icon ever looks off. For a symmetric icon the two agree and
     * the weight changes nothing. */
    {
        f32 box_x  = (f32)slot->w * 0.5f;
        f32 box_y  = (f32)slot->h * 0.5f;
        f32 mass_x = (f32)slot->balance_x / 16.0f;
        f32 mass_y = (f32)slot->balance_y / 16.0f;
        f32 at_x   = box_x + (mass_x - box_x) * UI_OPTICAL_WEIGHT;
        f32 at_y   = box_y + (mass_y - box_y) * UI_OPTICAL_WEIGHT;
        dest.x = (f32)ui_floor_i32(center_x - at_x + 0.5f);
        dest.y = (f32)ui_floor_i32(center_y - at_y + 0.5f);
    }
    dest.w = (f32)slot->w;
    dest.h = (f32)slot->h;
    SDL_RenderTexture(renderer, t->atlas, &source, &dest);
}
