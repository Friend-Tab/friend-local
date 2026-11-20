/* ui_draw.c — rectangles, rectangles with the corners taken off, and the
 * one circle this program needs.
 *
 * SDL's renderer draws axis-aligned rectangles and textured quads, which
 * is everything a chat window needs except round corners. Those come from
 * two small antialiased discs generated once: a rounded rectangle is
 * three rectangles and four quadrants of the first, and a friend's avatar
 * is the first disc in their darker colour with the second laid over it
 * in their lighter one, its alpha falling away across the diagonal. Seven
 * draw calls and two, against a nine-patch to maintain, a shader to
 * compile, or corners that look like they were cut with scissors.
 *
 * The discs are generated at the largest radius anything asks for and
 * sampled down for smaller ones, so there is one texture rather than one
 * per corner size.
 */

#define UI_DISC_RADIUS 32
#define UI_DISC_SUPERSAMPLE 4

/* How deep the clips nest: the sheet, a field in it, and room to
 * spare. Deeper than this still unwinds in step, and is said. */
#define UI_CLIP_STACK 8

typedef struct Ui_Draw {
    SDL_Renderer *renderer;
    SDL_Texture  *disc;      /* a filled circle, 2R across          */
    SDL_Texture  *disc_fade; /* the same, fading out along x+y      */
    i32           radius;

    /* Clips nest, and a nested one ends by going back to the one it
     * was inside rather than to none. Doing that by hand is a bug
     * waiting to happen — it already happened once, and rows of the
     * settings sheet were drawn over its own buttons. */
    Ui_Rect clips[UI_CLIP_STACK];
    i32     clip_depth;
} Ui_Draw;

static b32 ui_draw_init(Ui_Draw *d, SDL_Renderer *renderer, Arena *arena)
{
    i32 radius = UI_DISC_RADIUS;
    i32 size   = radius * 2;
    u8 *solid;
    u8 *fade;
    i32 x, y;

    memset(d, 0, sizeof(*d));
    d->renderer = renderer;
    d->radius   = radius;

    solid = arena_push(arena, u8, (size_t)size * (size_t)size * 4);
    fade  = arena_push(arena, u8, (size_t)size * (size_t)size * 4);
    if (!solid || !fade) return 0;

    /* Coverage by supersampling, because the corners are the one place
     * the eye notices a stair step. */
    for (y = 0; y < size; ++y) {
        for (x = 0; x < size; ++x) {
            i32 inside = 0, sy, sx;
            for (sy = 0; sy < UI_DISC_SUPERSAMPLE; ++sy) {
                for (sx = 0; sx < UI_DISC_SUPERSAMPLE; ++sx) {
                    f32 px = (f32)x + ((f32)sx + 0.5f) / UI_DISC_SUPERSAMPLE;
                    f32 py = (f32)y + ((f32)sy + 0.5f) / UI_DISC_SUPERSAMPLE;
                    f32 dx = px - (f32)radius;
                    f32 dy = py - (f32)radius;
                    if (dx * dx + dy * dy <= (f32)(radius * radius)) ++inside;
                }
            }
            {
                size_t at = ((size_t)y * (size_t)size + (size_t)x) * 4;
                f32    coverage =
                    (f32)inside / (UI_DISC_SUPERSAMPLE * UI_DISC_SUPERSAMPLE);
                /* Top-left to bottom-right, the way both clients run it. */
                f32 along = ((f32)x + (f32)y) / (f32)(size * 2 - 2);
                f32 lead  = 1.0f - along;

                solid[at + 0] = 255;
                solid[at + 1] = 255;
                solid[at + 2] = 255;
                solid[at + 3] = (u8)(coverage * 255.0f + 0.5f);

                fade[at + 0] = 255;
                fade[at + 1] = 255;
                fade[at + 2] = 255;
                fade[at + 3] = (u8)(coverage * lead * 255.0f + 0.5f);
            }
        }
    }

    d->disc = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STATIC, size, size);
    d->disc_fade = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STATIC, size, size);
    if (!d->disc || !d->disc_fade) return 0;
    SDL_UpdateTexture(d->disc, 0, solid, size * 4);
    SDL_UpdateTexture(d->disc_fade, 0, fade, size * 4);
    SDL_SetTextureBlendMode(d->disc, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(d->disc_fade, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(d->disc, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureScaleMode(d->disc_fade, SDL_SCALEMODE_LINEAR);
    return 1;
}

static void ui_fill(Ui_Draw *d, Ui_Rect r, Ui_Color c)
{
    SDL_FRect rect;
    if (r.w <= 0 || r.h <= 0 || c.a == 0) return;
    rect.x = r.x;
    rect.y = r.y;
    rect.w = r.w;
    rect.h = r.h;
    SDL_SetRenderDrawColor(d->renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(d->renderer, &rect);
}

static void ui_fill_rounded(Ui_Draw *d, Ui_Rect r, f32 radius, Ui_Color c)
{
    f32 source_radius = (f32)d->radius;
    i32 i;

    if (r.w <= 0 || r.h <= 0 || c.a == 0) return;
    if (radius > r.w * 0.5f) radius = r.w * 0.5f;
    if (radius > r.h * 0.5f) radius = r.h * 0.5f;
    if (radius < 0.5f) {
        ui_fill(d, r, c);
        return;
    }

    ui_fill(d, ui_rect(r.x + radius, r.y, r.w - radius * 2, r.h), c);
    ui_fill(d, ui_rect(r.x, r.y + radius, radius, r.h - radius * 2), c);
    ui_fill(d, ui_rect(r.x + r.w - radius, r.y + radius, radius,
                       r.h - radius * 2),
            c);

    SDL_SetTextureColorMod(d->disc, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(d->disc, c.a);
    for (i = 0; i < 4; ++i) {
        SDL_FRect source, dest;
        b32       right  = (i & 1) != 0;
        b32       bottom = (i & 2) != 0;
        source.x = right ? source_radius : 0;
        source.y = bottom ? source_radius : 0;
        source.w = source_radius;
        source.h = source_radius;
        dest.x   = right ? r.x + r.w - radius : r.x;
        dest.y   = bottom ? r.y + r.h - radius : r.y;
        dest.w   = radius;
        dest.h   = radius;
        SDL_RenderTexture(d->renderer, d->disc, &source, &dest);
    }
}

/* The site's shadow-sm under the user's bubble: barely there, and the
 * only thing separating white on near-white. Two offset passes, `step`
 * apart, stand in for the blur. */
static void ui_shadow_rounded(Ui_Draw *d, Ui_Rect r, f32 radius, f32 step)
{
    ui_fill_rounded(d, ui_rect(r.x, r.y + step * 2.0f, r.w, r.h), radius,
                    ui_rgba(UI_BLACK, 0.025f));
    ui_fill_rounded(d, ui_rect(r.x, r.y + step, r.w, r.h), radius,
                    ui_rgba(UI_BLACK, 0.035f));
}

/* A soft gradient circle in the friend's colour: the darker end filled,
 * the lighter one laid over it and falling away across the diagonal. */
static void ui_avatar(Ui_Draw *d, Ui_Rect r, Ui_Friend_Color color)
{
    SDL_FRect dest;
    if (r.w <= 0 || r.h <= 0) return;
    dest.x = r.x;
    dest.y = r.y;
    dest.w = r.w;
    dest.h = r.h;

    SDL_SetTextureColorMod(d->disc, color.avatar_bottom.r,
                           color.avatar_bottom.g, color.avatar_bottom.b);
    SDL_SetTextureAlphaMod(d->disc, 255);
    SDL_RenderTexture(d->renderer, d->disc, 0, &dest);

    SDL_SetTextureColorMod(d->disc_fade, color.avatar_top.r,
                           color.avatar_top.g, color.avatar_top.b);
    SDL_SetTextureAlphaMod(d->disc_fade, 255);
    SDL_RenderTexture(d->renderer, d->disc_fade, 0, &dest);
}

/* A vertical fade, drawn as one-pixel strips. Both clients fade the
 * conversation into the chrome at either end rather than letting it stop
 * at a hard line, and a message sliced level with the header edge is
 * exactly the thing that looks broken. */
static void ui_fade(Ui_Draw *d, Ui_Rect r, Ui_Color c, b32 opaque_at_top)
{
    i32 rows = (i32)r.h;
    i32 i;
    if (rows <= 0 || r.w <= 0) return;
    for (i = 0; i < rows; ++i) {
        f32      along = ((f32)i + 0.5f) / (f32)rows;
        f32      alpha = opaque_at_top ? 1.0f - along : along;
        Ui_Color strip = c;
        /* Squared, so the opaque end holds and the tail disappears
         * instead of leaving a visible band. */
        strip.a = (u8)(alpha * alpha * (f32)c.a + 0.5f);
        ui_fill(d, ui_rect(r.x, r.y + (f32)i, r.w, 1.0f), strip);
    }
}

/* Whole pixels covering the rectangle, rather than truncated at both
 * ends. */
static void ui_set_clip(Ui_Draw *d, Ui_Rect r)
{
    SDL_Rect rect;
    i32      x0 = ui_floor_i32(r.x), y0 = ui_floor_i32(r.y);
    i32      x1 = -ui_floor_i32(-(r.x + r.w));
    i32      y1 = -ui_floor_i32(-(r.y + r.h));
    rect.x = x0;
    rect.y = y0;
    rect.w = x1 - x0;
    rect.h = y1 - y0;
    SDL_SetRenderClipRect(d->renderer, &rect);
}

/* The clip in force. A stack deeper than it holds keeps the deepest one
 * it could. */
static Ui_Rect ui_clip_top(const Ui_Draw *d)
{
    i32 kept = d->clip_depth < UI_CLIP_STACK ? d->clip_depth : UI_CLIP_STACK;
    return d->clips[kept - 1];
}

/* What is clipped away cannot be pointed at either. */
static b32 ui_clip_contains(const Ui_Draw *d, f32 x, f32 y)
{
    return d->clip_depth == 0 || ui_rect_contains(ui_clip_top(d), x, y);
}

/* Draw only inside r, and inside whatever was already in force. */
static void ui_clip(Ui_Draw *d, Ui_Rect r)
{
    if (d->clip_depth > 0) r = ui_rect_intersect(ui_clip_top(d), r);
    if (d->clip_depth < UI_CLIP_STACK) {
        d->clips[d->clip_depth] = r;
    } else if (d->clip_depth == UI_CLIP_STACK) {
        SDL_Log("clips nested deeper than %d", UI_CLIP_STACK);
    }
    d->clip_depth += 1;
    ui_set_clip(d, ui_clip_top(d));
}

static void ui_unclip(Ui_Draw *d)
{
    if (d->clip_depth > 0) d->clip_depth -= 1;
    if (d->clip_depth > 0) ui_set_clip(d, ui_clip_top(d));
    else SDL_SetRenderClipRect(d->renderer, 0);
}
