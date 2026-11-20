/* ui_base.h — the frontend's own small vocabulary, and the palette.
 *
 * Strings and arenas come from base/, the layer the library is built on,
 * so there is one String type in this repository and not two; the window
 * compiles its own copy (friend_sdl.c). Everything the frontend knows
 * about the friend goes through friend.h.
 *
 * The colours are not invented. They are the ones the website and the
 * iOS app already agree on, read out of website/src/Chat.tsx, the
 * Tailwind greys it uses, and the shared FriendColor table that both
 * clients hash a friend's identity into. Someone who used friend.com
 * should recognise this window, so the numbers are copied rather than
 * chosen, and the places where ours differ are marked.
 */

#ifndef UI_BASE_H
#define UI_BASE_H

#include "../../base/fr_base.h"

typedef struct Ui_Color {
    u8 r, g, b, a;
} Ui_Color;

static Ui_Color ui_rgb(u32 hex)
{
    Ui_Color c;
    c.r = (u8)((hex >> 16) & 0xFFu);
    c.g = (u8)((hex >> 8) & 0xFFu);
    c.b = (u8)(hex & 0xFFu);
    c.a = 255;
    return c;
}

static Ui_Color ui_rgba(u32 hex, f32 alpha)
{
    Ui_Color c = ui_rgb(hex);
    c.a        = (u8)(alpha * 255.0f + 0.5f);
    return c;
}

/* From Chat.tsx and the Tailwind greys around it. */
#define UI_TEXT 0x1F2937        /* MESSAGE_TEXT: every message, both sides */
#define UI_INK 0x111827         /* gray-900: names                        */
#define UI_TEXT_DIM 0x6B7280    /* gray-500: timestamps, notices          */
#define UI_TEXT_FAINT 0x9CA3AF  /* gray-400: placeholders, asides         */
#define UI_WHITE 0xFFFFFF       /* the user's bubble, the chrome          */
#define UI_PANEL 0xFFFFFF       /* sidebar and header                     */
#define UI_ROW_HOVER 0xF9FAFB   /* gray-50                                */
#define UI_SURFACE 0xF3F4F6     /* gray-100: quiet buttons                */
#define UI_SURFACE_HOVER 0xE5E7EB /* gray-200                             */
#define UI_LINE 0xF3F4F6        /* gray-100: row rules                    */
#define UI_LINE_STRONG 0xE5E7EB /* gray-200: panel edges                  */
#define UI_RULE 0xD1D5DB        /* gray-300: the notice separators        */
#define UI_BLACK 0x000000       /* the primary button, as on the site     */
#define UI_BLACK_HOVER 0x1F2937 /* gray-800: the primary button, hovered  */
#define UI_ACCENT 0x2563EB      /* blue-600: links                        */
#define UI_WARN 0xDC2626        /* red-600                                */
#define UI_NO_WASH 0xF5F6F8     /* ours: the window with no friend open   */

/* ------------------------------------------------------------------ *
 *  A friend's colour
 *
 *  Both clients tint the whole conversation with a pale wash of the
 *  friend's own colour and give them a gradient circle to match, picked
 *  by hashing the friend's id into a table of five. The hash and the
 *  table are reproduced here exactly, from
 *  website/src/components/FriendAvatar.tsx and the iOS FriendColor.
 *
 *  One thing we cannot reproduce: the id. The service's friend id lives
 *  in the client's key material, not in the export, so there is nothing
 *  in the file to hash. We hash the name instead, which gives a friend a
 *  stable colour here — just not necessarily the one they had.
 * ------------------------------------------------------------------ */

typedef struct Ui_Friend_Color {
    Ui_Color avatar_top;
    Ui_Color avatar_bottom;
    Ui_Color wash; /* the pale wash the conversation sits on */
} Ui_Friend_Color;

#define UI_FRIEND_COLORS 5

static f32 ui_hue_channel(f32 p, f32 q, f32 t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

/* CSS hsl(), so the numbers from the stylesheet can be used as written. */
static Ui_Color ui_hsl(f32 hue_degrees, f32 saturation, f32 lightness)
{
    f32      h = hue_degrees / 360.0f;
    f32      q = lightness < 0.5f ? lightness * (1.0f + saturation)
                                  : lightness + saturation - lightness * saturation;
    f32      p = 2.0f * lightness - q;
    Ui_Color c;
    if (saturation <= 0.0f) {
        c.r = c.g = c.b = (u8)(lightness * 255.0f + 0.5f);
        c.a             = 255;
        return c;
    }
    c.r = (u8)(ui_hue_channel(p, q, h + 1.0f / 3.0f) * 255.0f + 0.5f);
    c.g = (u8)(ui_hue_channel(p, q, h) * 255.0f + 0.5f);
    c.b = (u8)(ui_hue_channel(p, q, h - 1.0f / 3.0f) * 255.0f + 0.5f);
    c.a = 255;
    return c;
}

/* One hue run through the three ramps every surface shares. */
static Ui_Friend_Color ui_hued(f32 hue)
{
    Ui_Friend_Color c;
    c.avatar_top    = ui_hsl(hue, 0.83f, 0.82f);
    c.avatar_bottom = ui_hsl(hue, 0.54f, 0.61f);
    c.wash          = ui_hsl(hue, 0.32f, 0.93f);
    return c;
}

/* Order matters: the hash indexes into this list and the other clients
 * list the same five in the same order. */
static Ui_Friend_Color ui_friend_color_at(i32 index)
{
    switch (index) {
    case 0: return ui_hued(335.0f); /* pink   */
    case 1: return ui_hued(212.0f); /* blue   */
    case 2: return ui_hued(145.0f); /* green  */
    case 3: return ui_hued(48.0f);  /* yellow */
    default: {
        /* `clear` is the colourless one. Its greys are written out
         * rather than run through ui_hued at zero saturation, because
         * HSL lightness only lines up with iOS's HSB brightness while
         * there is some saturation to shift. */
        Ui_Friend_Color c;
        c.avatar_top    = ui_hsl(0.0f, 0.0f, 0.97f);
        c.avatar_bottom = ui_hsl(0.0f, 0.0f, 0.82f);
        c.wash          = ui_hsl(0.0f, 0.0f, 0.97f);
        return c;
    }
    }
}

/* djb2 over the utf8 bytes, exactly as both clients do it. */
static Ui_Friend_Color ui_friend_color(String identity)
{
    u64 hash = 5381u;
    i32 i;
    for (i = 0; i < identity.len; ++i) {
        hash = (hash * 33u) ^ (u64)(u8)identity.data[i];
    }
    return ui_friend_color_at((i32)(hash % UI_FRIEND_COLORS));
}

/* ------------------------------------------------------------------ *
 *  Rectangles
 * ------------------------------------------------------------------ */

typedef struct Ui_Rect {
    f32 x, y, w, h;
} Ui_Rect;

static Ui_Rect ui_rect(f32 x, f32 y, f32 w, f32 h)
{
    Ui_Rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

/* The part of a that is also inside b. Empty rather than negative when
 * they do not meet, so a clip set to one shows nothing. */
static Ui_Rect ui_rect_intersect(Ui_Rect a, Ui_Rect b)
{
    f32 x0 = a.x > b.x ? a.x : b.x;
    f32 y0 = a.y > b.y ? a.y : b.y;
    f32 x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    f32 y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return ui_rect(x0, y0, x1 - x0, y1 - y0);
}

static b32 ui_rect_contains(Ui_Rect r, f32 x, f32 y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

/* Taking a strip off one side of a rectangle and handing it back: a row
 * of things laid out as what each one takes from what is left. */
static Ui_Rect ui_cut_left(Ui_Rect *r, f32 w)
{
    Ui_Rect out = ui_rect(r->x, r->y, w, r->h);
    r->x += w;
    r->w -= w;
    return out;
}

static Ui_Rect ui_cut_right(Ui_Rect *r, f32 w)
{
    r->w -= w;
    return ui_rect(r->x + r->w, r->y, w, r->h);
}

static Ui_Rect ui_cut_top(Ui_Rect *r, f32 h)
{
    Ui_Rect out = ui_rect(r->x, r->y, r->w, h);
    r->y += h;
    r->h -= h;
    return out;
}

static Ui_Rect ui_cut_bottom(Ui_Rect *r, f32 h)
{
    r->h -= h;
    return ui_rect(r->x, r->y + r->h, r->w, h);
}

static Ui_Rect ui_inset(Ui_Rect r, f32 dx, f32 dy)
{
    return ui_rect(r.x + dx, r.y + dy, r.w - dx * 2, r.h - dy * 2);
}

#endif /* UI_BASE_H */
