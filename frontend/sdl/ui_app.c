/* ui_app.c — the window.
 *
 * Laid out after the web and iOS clients, which agree with each other:
 * the whole conversation sits on a pale wash of the friend's own colour,
 * the friend's messages have no bubble at all and blend into it, and only
 * the user gets a white card. The friend's identity lives in the header
 * — avatar and name, the name set in a serif — rather than being
 * repeated beside every line they say. Getting that asymmetry right
 * matters more than any single colour: it is what the thing looked like.
 *
 * Immediate mode: there are no widget objects and no retained tree, just
 * a pass over the state drawing what it says and reading the mouse
 * against the rectangles as it goes. That suits a program with one
 * allocation: there is nothing to keep, so there is nothing to keep it
 * in. Each region is laid out by one function that hands back its
 * rectangles, and whoever needs to know how tall a thing is asks the
 * same function that places it for drawing.
 *
 * The one thing kept between frames is how tall each message is, because
 * knowing where the scrollbar is means knowing the height of a thousand
 * messages and shaping a thousand messages every frame to find out would
 * be absurd. They are measured once per friend, and again if the window
 * changes width or the display changes scale.
 */

/* How many messages the window keeps a measured height for. The library
 * will hold a quarter of a million, which is more than anybody scrolls
 * through and far more than is worth a megabyte of heights: past this
 * the window shows the newest UI_MESSAGES_MAX and says so at the top of
 * the scroll. Sixteen thousand is about a year of talking every day. */
#define UI_MESSAGES_MAX 16384

/* As much as the composer will hold. The library truncates a longer
 * message on a code point boundary, so this is not a limit so much as a
 * place to stop: it is already far more than anybody types at a friend. */
#define UI_INPUT_MAX 8192

/* How tall a photograph waiting to be sent is drawn. */
#define UI_PENDING_HEIGHT 54.0f

/* Sizes in points; everything is multiplied by the display scale.
 * text-base, text-xs and text-lg, as the site sets them. */
#define UI_BODY 16
#define UI_SMALL 12
#define UI_NAME 15
#define UI_TITLE 18
#define UI_SIDEBAR_WIDTH 300
#define UI_BUBBLE_RADIUS 18 /* the iOS corner; rounded-2xl on the web */
#define UI_BUBBLE_PAD_X 16  /* px-4 */
#define UI_BUBBLE_PAD_Y 8   /* py-2 */
#define UI_BUBBLE_SHARE 0.75f /* max-w-[75%] */
#define UI_MESSAGE_GAP 12   /* mb-3 */
#define UI_PHOTO_GAP 6
/* How far the conversation fades into the chrome at either end, and so
 * also how much empty room is kept there. */
#define UI_TRANSCRIPT_FADE 26.0f
#define UI_AVATAR 40
#define UI_LOGO 22 /* how tall the wordmark is drawn, in points */
/* The strip right of the conversation the scrollbar lives in, and where
 * in it. The bar is wider to the pointer than to the eye: a five-pixel
 * target is a dare. */
#define UI_GUTTER 14.0f
#define UI_SCROLLBAR_AT 8.0f
#define UI_SCROLLBAR 5.0f
#define UI_SCROLLBAR_REACH 7.0f
/* Text is never set smaller than this many pixels, whatever the scale. */
#define UI_PX_MIN 6
/* How far a line of the transcript or the sheet moves for an arrow key
 * or a notch of the wheel, in points, and for a page, in windows. */
#define UI_SCROLL_LINE 60.0f
#define UI_SCROLL_WHEEL 72.0f
#define UI_PAGE_TRANSCRIPT 0.8f
#define UI_PAGE_SHEET 0.6f
/* A caret that does not blink looks like a drawing of a caret, and one
 * that blinks while you type looks broken: it is reset by every edit, so
 * it only starts once you stop. */
#define UI_BLINK_MS 530u
/* What the window has to say stops being news after this long: a line
 * that never leaves stops being read. */
#define UI_SAY_MS 9000u

/* Where the keyboard is: the composer, the name of a friend being made,
 * or one field of the settings sheet, by its own UI_SETTING_*. */
#define UI_FOCUS_COMPOSER (-1)
#define UI_FOCUS_NAME (-2)

/* What has hold of the mouse while its button is down: nothing, the
 * scrollbar, or a field being dragged across, by its focus. */
#define UI_HELD_NONE 0
#define UI_HELD_SCROLLBAR 0x7FFF

/* Bottom up. Only the top one can be pointed at; anything under it is
 * a picture of a window. */
enum { UI_LAYER_WINDOW = 0, UI_LAYER_SHEET };

/* What the host saw this frame. */
typedef struct Ui_Input {
    f32 width, height; /* of the drawable, in pixels      */
    f32 scale;         /* display scale                   */
    f32 mouse_x, mouse_y;
    b32 pressed;       /* went down this frame            */
    b32 down;          /* is down now                     */
    f32 wheel;
} Ui_Input;

/* Something the window has to say, and when it started saying it. */
typedef struct Ui_Notice {
    char text[256];
    i32  len;
    b32  is_error;
    u64  ms;
} Ui_Notice;

/* The transcript's shape, measured and then reused. first is the
 * message heights[0] belongs to: everything older than that is in the
 * friend and off the top of this window. What it was measured for is
 * kept with it, so that a change to any of it measures again. */
typedef struct Ui_Heights {
    f32 height[UI_MESSAGES_MAX];
    i32 first;
    i32 count;
    f32 total;
    f32 width;
    f32 scale;
    u32 friend;
    u64 lost; /* what the friend had dropped: indices move when it does */
} Ui_Heights;

typedef struct Ui_App {
    SDL_Window     *window;
    SDL_Renderer   *renderer;
    Friend_Context *fr;
    Fr_Net         *net;
    Fr_Env          env;
    Fr_Shelf        shelf;

    /* The friend's configuration, pointed at the friend in hand's photo
     * file by the shelf. Everything in it points at the settings, the
     * shelf, a literal, or the environment, all of which outlive it. */
    Friend_Config config;

    Ui_Draw draw;
    Ui_Text text;
    Arena   frame; /* everything laid out this frame */

    i32 serif; /* font index, or 0 if no serif was found */
    i32 icons; /* font index, or 0 if the icon face is missing */

    Ui_Input in;
    f32      scale; /* in.scale, never zero */
    i32      layer; /* the one being drawn */
    i32      top_layer;
    b32      press_taken;
    i32      held;
    f32      scroll_grab; /* where on the thumb it was taken hold of */

    b32             have_friend;
    u32             friend_serial; /* moves whenever another is in hand */
    Ui_Friend_Color color;
    /* The same offset the friend was configured with, so a timestamp in
     * the transcript reads the same as one in the friend's prompts. */
    i32 utc_offset_minutes;

    Ui_Heights heights;
    f32        scroll;
    b32        stick_to_end;

    i32  focus;
    u64  caret_blinked_ms;
    char composer_bytes[UI_INPUT_MAX];
    Ui_Text_Edit composer;
    /* Naming a new friend, in the sidebar. */
    b32          naming;
    char         name_bytes[FRIEND_NAME_MAX];
    Ui_Text_Edit name;

    /* The wordmark, when the program can find it beside itself. Kept
     * at the size it is drawn, so the path and the scale it was last
     * built for are kept with it. */
    SDL_Texture *logo;
    i32          logo_w, logo_h;
    char         logo_path[FR_PATH_MAX];
    f32          logo_scale;

    /* Photographs: a few decoded and kept as textures, and the ones
     * waiting to go with the next message. A photograph on its way out
     * is JPEG bytes at the tail of an arena of its own, which is reset
     * the moment the friend has taken a copy. */
    Ui_Photos         photos;
    Arena             outgoing;
    Friend_Attachment pending[FRIEND_ATTACHMENTS_MAX];
    i32               pending_count;

    /* Waiting on the friend. Set when something is said and cleared when
     * the reply lands, so the composer can say so rather than looking
     * broken. */
    b32 awaiting;

    /* The last thing that went wrong, which typing again dismisses, and
     * the last thing that happened. */
    Ui_Notice trouble;
    Ui_Notice news;

    /* A path handed over by a drop or by the file dialog, which may land
     * on another thread, so the main loop picks it up rather than acting
     * on it. An import is done the frame after it is announced, so that
     * the announcement is on screen while it runs. */
    char          dropped[FR_PATH_MAX];
    SDL_AtomicInt dropped_ready;
    char          importing[FR_PATH_MAX];

    /* Where the save dialog said to export to, picked up the same way,
     * and which friend it was for — by file, since the list can be
     * reshuffled while the dialog is open. */
    char          export_to[FR_PATH_MAX];
    SDL_AtomicInt export_ready;
    char          export_file[FR_SHELF_FILE_MAX];
    char          export_start[FR_PATH_MAX];

    /* Settings: the sheet, and the values it edits. There is no widget
     * tree to keep them in — the sheet is drawn out of these every
     * frame and types straight back into them. */
    Ui_Settings settings;
    /* What was last applied, kept so that cancelling is exact: the
     * file is not the answer, because on a first run there is not one
     * yet and what is on screen came from the environment. */
    Ui_Settings settings_applied;
    b32         settings_open;
    b32         settings_show_key;
    f32         settings_scroll;
    f32         settings_content; /* how tall the rows were last frame */
    char        settings_path[FR_PATH_MAX];

    /* The microphone and the speaker. The library decides what a
     * spoken turn is; this is only the two devices. */
    Ui_Audio audio;

    /* Whether there is a key at all, typed or from the environment.
     * Worth saying where the window would otherwise just look broken. */
    b32 have_key;

    b32 quit;
} Ui_App;

/* ------------------------------------------------------------------ *
 *  Units, words, and the mouse
 * ------------------------------------------------------------------ */

static i32 ui_px(Ui_App *app, f32 points)
{
    i32 size = (i32)(points * app->scale + 0.5f);
    return size < UI_PX_MIN ? UI_PX_MIN : size;
}

static f32 ui_s(Ui_App *app, f32 points) { return points * app->scale; }

static f32 ui_line_h(Ui_App *app, f32 points)
{
    return ui_line_height((f32)ui_px(app, points));
}

static Ui_Layout ui_lay(Ui_App *app, i32 font, String s, f32 points,
                        f32 wrap)
{
    return ui_text_layout(&app->text, &app->frame, font, s,
                          ui_px(app, points), wrap);
}

static void ui_draw_layout(Ui_App *app, const Ui_Layout *layout, f32 x, f32 y,
                           Ui_Color color)
{
    ui_text_draw(&app->text, app->renderer, layout, x, y, color);
}

/* One line of text with its left edge at x, or its middle when align is
 * a half, or its right edge when it is one. Returns how wide it was. */
static f32 ui_label(Ui_App *app, i32 font, String s, f32 x, f32 y,
                    f32 points, Ui_Color color, f32 align)
{
    Ui_Layout layout = ui_lay(app, font, s, points, 0);
    ui_draw_layout(app, &layout, x - layout.width * align, y, color);
    return layout.width;
}

/* The same, centred on the line through the middle of r. */
static f32 ui_label_in(Ui_App *app, String s, Ui_Rect r, f32 points,
                       Ui_Color color, f32 align)
{
    return ui_label(app, 0, s, r.x + r.w * align,
                    r.y + (r.h - ui_line_h(app, points)) * 0.5f, points,
                    color, align);
}

static f32 ui_measure(Ui_App *app, String s, f32 points)
{
    return ui_lay(app, 0, s, points, 0).width;
}

static void ui_icon(Ui_App *app, u32 codepoint, f32 center_x, f32 center_y,
                    f32 points, Ui_Color color)
{
    ui_draw_icon(&app->text, app->renderer, app->icons, codepoint, center_x,
                 center_y, ui_px(app, points), color);
}

/* Whether the pointer is over r, on the layer that has it, and inside
 * whatever r is clipped to. */
static b32 ui_hover(Ui_App *app, Ui_Rect r)
{
    return app->layer == app->top_layer &&
           ui_clip_contains(&app->draw, app->in.mouse_x, app->in.mouse_y) &&
           ui_rect_contains(r, app->in.mouse_x, app->in.mouse_y);
}

/* A press on r, which nothing else this frame then gets. Whatever is
 * drawn on top asks first. */
static b32 ui_press(Ui_App *app, Ui_Rect r)
{
    if (!app->in.pressed || app->press_taken || !ui_hover(app, r)) return 0;
    app->press_taken = 1;
    return 1;
}

static void ui_notice(Ui_Notice *notice, b32 is_error, String text)
{
    i32 len = fr_truncate_utf8(text, (i32)sizeof(notice->text) - 1).len;
    if (len > 0) memcpy(notice->text, text.data, (size_t)len);
    notice->len      = len;
    notice->is_error = is_error;
    notice->ms       = SDL_GetTicks();
}

static void ui_say(Ui_App *app, b32 is_error, const char *format, ...)
{
    char    text[256];
    va_list args;
    int     written;
    va_start(args, format);
    written = SDL_vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (written < 0) written = 0;
    if (written >= (int)sizeof(text)) written = (int)sizeof(text) - 1;
    ui_notice(&app->news, is_error, str_make(text, (i32)written));
    SDL_Log("%s", text);
}

static void ui_trouble(Ui_App *app, String text)
{
    ui_notice(&app->trouble, 1, text);
}

/* What the window has to say now, if anything. */
static b32 ui_said(Ui_App *app, String *out, b32 *is_error)
{
    u64              now   = SDL_GetTicks();
    const Ui_Notice *say[2];
    i32              i;
    say[0] = &app->trouble;
    say[1] = &app->news;
    for (i = 0; i < 2; ++i) {
        if (say[i]->len > 0 && now - say[i]->ms < UI_SAY_MS) {
            *out      = str_make(say[i]->text, say[i]->len);
            *is_error = say[i]->is_error;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Widgets
 * ------------------------------------------------------------------ */

/* The site's two buttons: the primary action is a black pill with white
 * text, and the quiet one, the thing you did not come here to do, is the
 * same shape in the greys. An icon goes ahead of the caption when there
 * is one to show. */
static b32 ui_button(Ui_App *app, Ui_Rect r, u32 icon, String caption,
                     f32 points, b32 primary)
{
    b32       hover = ui_hover(app, r);
    f32       glyph_pt = primary ? 18.0f : points + 2.0f;
    f32       glyph = icon && app->icons ? ui_s(app, glyph_pt) : 0;
    f32       gap   = glyph > 0 ? ui_s(app, primary ? 8.0f : 7.0f) : 0;
    Ui_Color  ink   = ui_rgb(primary ? UI_WHITE : UI_INK);
    Ui_Layout layout;
    f32       start;

    ui_fill_rounded(&app->draw, r, r.h * 0.5f,
                    ui_rgb(primary ? (hover ? UI_BLACK_HOVER : UI_BLACK)
                                   : (hover ? UI_SURFACE_HOVER : UI_SURFACE)));
    layout = ui_lay(app, 0, caption, points, r.w);
    start  = r.x + (r.w - (glyph + gap + layout.width)) * 0.5f;
    if (glyph > 0) {
        ui_icon(app, icon, start + glyph * 0.5f, r.y + r.h * 0.5f, glyph_pt,
                ink);
    }
    ui_draw_layout(app, &layout, start + glyph + gap,
                   r.y + (r.h - layout.height) * 0.5f, ink);
    return ui_press(app, r);
}

/* A small round icon button, for the things in the margins. Without an
 * icon font it is the caption as a quiet button, or, with no caption,
 * not there to be clicked, which is better than a mystery circle. */
static b32 ui_button_icon(Ui_App *app, Ui_Rect r, u32 icon, f32 points,
                          b32 quiet, String fallback)
{
    b32 hover = ui_hover(app, r);
    if (!app->icons) {
        return fallback.len > 0 &&
               ui_button(app, r, 0, fallback, UI_SMALL, 0);
    }
    if (hover) {
        ui_fill_rounded(&app->draw, r, r.h * 0.5f, ui_rgb(UI_SURFACE_HOVER));
    }
    ui_icon(app, icon, r.x + r.w * 0.5f, r.y + r.h * 0.5f, points,
            ui_rgb(hover ? UI_INK : (quiet ? UI_TEXT_FAINT : UI_TEXT_DIM)));
    return ui_press(app, r);
}

/* A rounded field with a one-pixel border, drawn as a filled rect with a
 * smaller one on top of it. */
static void ui_field(Ui_App *app, Ui_Rect r, f32 radius, Ui_Color fill,
                     Ui_Color border)
{
    f32 line = ui_s(app, 1.0f);
    ui_fill_rounded(&app->draw, r, radius, border);
    ui_fill_rounded(&app->draw, ui_inset(r, line, line), radius - line, fill);
}

/* A switch. Black when on, because that is what the primary action is
 * here, and the track has to be darker than the knob when it is off or
 * white on near-white disappears. */
static b32 ui_switch(Ui_App *app, Ui_Rect r, b32 *on)
{
    f32 inset = ui_s(app, 2.0f);
    f32 knob  = r.h - inset * 2;
    b32 hover = ui_hover(app, r);

    ui_fill_rounded(&app->draw, r, r.h * 0.5f,
                    ui_rgb(*on ? UI_BLACK
                               : (hover ? UI_TEXT_FAINT : UI_RULE)));
    ui_fill_rounded(&app->draw,
                    ui_rect(*on ? r.x + r.w - knob - inset : r.x + inset,
                            r.y + inset, knob, knob),
                    knob * 0.5f, ui_rgb(UI_WHITE));
    if (!ui_press(app, r)) return 0;
    *on = !*on;
    return 1;
}

/* ------------------------------------------------------------------ *
 *  The keyboard's focus, and the field it types into
 * ------------------------------------------------------------------ */

static Ui_Text_Edit *ui_edit_for(Ui_App *app, i32 focus)
{
    if (focus == UI_FOCUS_COMPOSER) return &app->composer;
    if (focus == UI_FOCUS_NAME) return &app->name;
    if (ui_setting_is_text(focus)) return &app->settings.text[focus];
    return 0;
}

static Ui_Text_Edit *ui_focused_edit(Ui_App *app)
{
    return ui_edit_for(app, app->focus);
}

static void ui_blink_restart(Ui_App *app) { app->caret_blinked_ms = 0; }

/* The keyboard goes to another field. A settings field is entered with
 * the caret at its end and nothing selected; the composer taking the
 * keyboard back from a friend's name is the end of naming one. */
static void ui_focus(Ui_App *app, i32 focus)
{
    Ui_Text_Edit *edit = ui_edit_for(app, focus);
    if (focus == UI_FOCUS_COMPOSER) app->naming = 0;
    if (app->focus == focus) return;
    app->focus = focus;
    if (edit && focus > 0) ui_edit_go(edit, edit->len, 0);
    ui_blink_restart(app);
}

/* A key, as bullets, one per code point, so that the caret and a click
 * land where they look like they should. */
static String ui_bulleted(Arena *arena, String s)
{
    String_Builder sb = sb_begin(arena);
    i32            i;
    for (i = 0; i < s.len; ++i) {
        if (ui_utf8_continues(s.data[i])) continue;
        sb_append_cstr(&sb, "\342\200\242");
    }
    return sb_done(&sb);
}

/* One line of editable text inside `inner`: what was typed, scrolled so
 * the caret stays in view, the selection behind it, and the caret. A
 * press on `hit` puts the caret under the pointer and takes the
 * keyboard; a drag that began there takes a selection. `hidden` shows
 * bullets instead, measured and clicked as bullets. */
static void ui_text_field(Ui_App *app, i32 focus, Ui_Text_Edit *edit,
                          Ui_Rect hit, Ui_Rect inner, f32 points,
                          String placeholder, b32 hidden)
{
    String    text    = ui_edit_text(edit);
    String    shown   = hidden ? ui_bulleted(&app->frame, text) : text;
    Ui_Layout layout  = ui_lay(app, 0, shown, points, 0);
    f32       line_h  = ui_line_h(app, points);
    f32       top     = inner.y + (inner.h - line_h) * 0.5f;
    b32       focused = app->focus == focus;
    f32       caret_x = ui_layout_x(&layout, ui_utf8_index(text, edit->caret));
    f32       shift   = focused && caret_x > inner.w ? caret_x - inner.w : 0;
    b32       pressed = ui_press(app, hit);

    if (pressed || (app->held == focus && app->in.down)) {
        i32 at = ui_utf8_offset(
            text, ui_layout_index_at(&layout,
                                     app->in.mouse_x - inner.x + shift));
        if (pressed) {
            ui_focus(app, focus);
            app->held    = focus;
            edit->anchor = at;
        }
        edit->caret = at;
        focused     = 1;
        ui_blink_restart(app);
        caret_x = ui_layout_x(&layout, ui_utf8_index(text, edit->caret));
        shift   = caret_x > inner.w ? caret_x - inner.w : 0;
    }

    /* The placeholder sits a little to the right of where the caret is,
     * so the two are not in the same place. */
    if (shown.len == 0) {
        ui_label(app, 0, placeholder,
                 inner.x + (focused ? ui_s(app, 9.0f) : 0.0f), top, points,
                 ui_rgb(UI_TEXT_FAINT), 0);
    }

    ui_clip(&app->draw, inner);
    if (focused && edit->anchor != edit->caret) {
        f32 x0 = ui_layout_x(&layout, ui_utf8_index(text, ui_edit_low(edit)));
        f32 x1 = ui_layout_x(&layout, ui_utf8_index(text, ui_edit_high(edit)));
        ui_fill(&app->draw, ui_rect(inner.x + x0 - shift, top, x1 - x0, line_h),
                ui_rgba(UI_ACCENT, 0.22f));
    }
    ui_draw_layout(app, &layout, inner.x - shift,
                   inner.y + (inner.h - layout.height) * 0.5f,
                   ui_rgb(UI_TEXT));
    if (focused) {
        u64 now = SDL_GetTicks();
        if (app->caret_blinked_ms == 0) app->caret_blinked_ms = now;
        if (((now - app->caret_blinked_ms) / UI_BLINK_MS) % 2u == 0u) {
            f32 caret_h = line_h * 0.85f;
            ui_fill(&app->draw,
                    ui_rect(inner.x + caret_x - shift,
                            inner.y + (inner.h - caret_h) * 0.5f,
                            ui_s(app, 1.5f), caret_h),
                    ui_rgb(UI_INK));
        }
    }
    ui_unclip(&app->draw);
}

/* ------------------------------------------------------------------ *
 *  A friend in hand
 * ------------------------------------------------------------------ */

/* Whatever the shelf now has in hand is what is on screen: its colour,
 * a transcript measured afresh, and nothing left over from the last
 * one. */
static void ui_adopt_friend(Ui_App *app)
{
    app->have_friend  = app->shelf.current >= 0;
    app->color        = ui_friend_color(friend_name(app->fr));
    app->friend_serial += 1;
    app->scroll       = 0;
    app->stick_to_end = 1;
    app->awaiting     = 0;
    ui_photos_forget(&app->photos);
}

static void ui_shelf_trouble(Ui_App *app)
{
    ui_say(app, 1, "%s", fr_shelf_error(&app->shelf));
}

static void ui_open_friend(Ui_App *app, i32 index)
{
    if (!fr_shelf_open(&app->shelf, app->fr, app->net, &app->config,
                       index)) {
        ui_shelf_trouble(app);
    }
    ui_adopt_friend(app);
}

static void ui_naming_begin(Ui_App *app)
{
    app->naming = 1;
    ui_edit_clear(&app->name);
    ui_focus(app, UI_FOCUS_NAME);
}

static void ui_naming_end(Ui_App *app) { ui_focus(app, UI_FOCUS_COMPOSER); }

/* A friend that never used the service: nothing in it but a name. */
static void ui_naming_commit(Ui_App *app)
{
    String name = ui_edit_text(&app->name);
    if (!app->naming) return;
    if (fr_shelf_create(&app->shelf, app->fr, app->net, &app->config,
                        name) < 0) {
        ui_shelf_trouble(app);
        return;
    }
    ui_say(app, 0, "%.*s is new here", (int)name.len, name.data);
    ui_adopt_friend(app);
    ui_naming_end(app);
}

/* An export from friend.com, written to a file of its own. */
static void ui_import(Ui_App *app, const char *path)
{
    Friend_Import_Report report;
    u64                  started = SDL_GetTicks();

    memset(&report, 0, sizeof(report));
    if (fr_shelf_import(&app->shelf, app->fr, app->net, &app->config, path,
                        &report) < 0) {
        ui_shelf_trouble(app);
    } else {
        ui_say(app, 0,
               "imported %d memories, %d connections and %d messages in "
               "%llu ms",
               report.memories, report.connections, report.messages,
               (unsigned long long)(SDL_GetTicks() - started));
    }
    ui_adopt_friend(app);
}

/* Out of the list, not off the disk. The friend in hand is written out
 * first, and the next one along takes its place. */
static void ui_archive(Ui_App *app, i32 index)
{
    b32 was_open = index == app->shelf.current;
    if (!fr_shelf_archive(&app->shelf, app->fr, app->net, &app->config,
                          index)) {
        ui_shelf_trouble(app);
        return;
    }
    ui_say(app, 0, "archived \342\200\224 the file moved to archived/, "
                   "nothing was deleted");
    if (!was_open) return;
    if (app->shelf.count > 0) {
        ui_open_friend(app, 0);
    } else {
        ui_adopt_friend(app);
    }
}

/* ------------------------------------------------------------------ *
 *  Photographs on their way out
 * ------------------------------------------------------------------ */

/* A file that turned out to be a picture: decoded, scaled to what the
 * friend wants, and held until the next message goes. */
static b32 ui_attach_photo(Ui_App *app, const void *bytes, size_t len)
{
    Friend_String jpeg;

    if (app->pending_count >= FRIEND_ATTACHMENTS_MAX) {
        ui_trouble(app, FRIEND_LIT("that is as many photographs as one "
                                   "message can carry"));
        return 0;
    }
    jpeg = ui_photo_for_sending(&app->photos, &app->outgoing, bytes, len);
    if (jpeg.len <= 0) {
        ui_trouble(app, FRIEND_LIT("that file is not a picture this can "
                                   "read, or there was no room to scale "
                                   "it"));
        return 0;
    }
    app->pending[app->pending_count++].jpeg = jpeg;
    return 1;
}

/* A dropped file is either a friend or a photograph, and which it is is
 * decided by what it is rather than by a mode the person has to be in.
 * Anything stb_image can read is a photograph; the rest goes to the
 * importer, which has its own opinion about what it was handed. */
static b32 ui_dropped_photo(Ui_App *app, const char *path)
{
    void  *bytes;
    size_t len = 0;

    if (!app->have_friend) return 0;
    /* SDL_LoadFile is the one allocation in this program that is not
     * out of the block: a file somebody dropped is whatever size it is,
     * and it is freed before the function returns. */
    bytes = SDL_LoadFile(path, &len);
    if (!bytes) return 0;
    if (!ui_photo_is_picture(&app->photos, bytes, len)) {
        SDL_free(bytes);
        return 0;
    }
    if (ui_attach_photo(app, bytes, len)) {
        ui_say(app, 0, "photograph attached");
        app->stick_to_end = 1;
    }
    SDL_free(bytes);
    return 1;
}

/* A path from a drop or the file dialog. Either can be on another
 * thread, so this only leaves a note. */
static void ui_app_drop(Ui_App *app, const char *path)
{
    if (!path || !path[0]) return;
    /* A path cut short would name some other file, or none. */
    if (SDL_strlen(path) >= sizeof(app->dropped)) return;
    SDL_snprintf(app->dropped, sizeof(app->dropped), "%s", path);
    SDL_SetAtomicInt(&app->dropped_ready, 1);
}

static void SDLCALL ui_dialog_chose(void *userdata,
                                    const char *const *filelist, int filter)
{
    (void)filter;
    if (filelist) ui_app_drop((Ui_App *)userdata, filelist[0]);
}

/* The system file picker. What a path turns out to be — a picture to
 * send or a friend to import — is decided by looking at the file
 * rather than by which button was pressed. */
static void ui_pick_file(Ui_App *app, const SDL_DialogFileFilter *filter)
{
    SDL_DialogFileFilter filters[2];
    filters[0]         = *filter;
    filters[1].name    = "All files";
    filters[1].pattern = "*";
    SDL_ShowOpenFileDialog(ui_dialog_chose, app, app->window, filters, 2, 0,
                           false);
}

static const SDL_DialogFileFilter ui_pick_export = {
    "Friends", "friendlocal;zip;json"
};
static const SDL_DialogFileFilter ui_pick_picture = { "Pictures",
                                                      "jpg;jpeg;png;bmp;gif" };

static void ui_take_dropped(Ui_App *app)
{
    if (app->importing[0]) {
        ui_import(app, app->importing);
        app->importing[0] = 0;
        return;
    }
    if (!SDL_GetAtomicInt(&app->dropped_ready)) return;
    SDL_SetAtomicInt(&app->dropped_ready, 0);
    if (!app->dropped[0] || ui_dropped_photo(app, app->dropped)) return;

    /* Said a frame before it is done: importing a big export takes long
     * enough that a frozen window would look like a crash. */
    ui_say(app, 0, "importing %s", app->dropped);
    SDL_snprintf(app->importing, sizeof(app->importing), "%s", app->dropped);
}

/* ------------------------------------------------------------------ *
 *  Exporting
 *
 *  A friend and its photographs as one .friendlocal file, to take to
 *  another machine and import there. The dialog answers on whatever
 *  thread it likes, so it only leaves a note, like a drop does.
 * ------------------------------------------------------------------ */

static void SDLCALL ui_dialog_export(void *userdata,
                                     const char *const *filelist, int filter)
{
    Ui_App *app = (Ui_App *)userdata;
    (void)filter;
    /* Null is a dialog that failed, an empty list one that was
     * cancelled; either way there is nothing to write. */
    if (!filelist || !filelist[0] || !filelist[0][0]) return;
    if (SDL_strlen(filelist[0]) >= sizeof(app->export_to)) return;
    SDL_snprintf(app->export_to, sizeof(app->export_to), "%s", filelist[0]);
    SDL_SetAtomicInt(&app->export_ready, 1);
}

static void ui_export_begin(Ui_App *app, i32 index)
{
    static const SDL_DialogFileFilter filter = { "friend-local export",
                                                 "friendlocal" };
    const Fr_Shelf_Entry *entry = &app->shelf.entries[index];
    const char           *folder = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    i32                   stem   = entry->file_len;

    if (fr_disk_has_suffix(entry->file, ".friend")) stem -= 7;
    if (!folder) folder = SDL_GetUserFolder(SDL_FOLDER_HOME);
    SDL_snprintf(app->export_file, sizeof(app->export_file), "%s",
                 entry->file);
    /* Starts where people keep documents, already named after the
     * friend; SDL wants it to outlast the dialog. */
    SDL_snprintf(app->export_start, sizeof(app->export_start), "%s%s%.*s%s",
                 folder ? folder : "",
                 folder && folder[0] &&
                         folder[SDL_strlen(folder) - 1] != '/'
                     ? "/"
                     : "",
                 (int)stem, entry->file, FR_EXPORT_SUFFIX);
    SDL_ShowSaveFileDialog(ui_dialog_export, app, app->window, &filter, 1,
                           app->export_start);
}

static void ui_take_export(Ui_App *app)
{
    char path[FR_PATH_MAX];
    i32  index;

    if (!SDL_GetAtomicInt(&app->export_ready)) return;
    SDL_SetAtomicInt(&app->export_ready, 0);
    /* A dialog that was told a bare name gets the suffix the importer
     * and the file picker look for. */
    if (fr_disk_has_suffix(app->export_to, FR_EXPORT_SUFFIX)) {
        SDL_snprintf(path, sizeof(path), "%s", app->export_to);
    } else if (SDL_strlen(app->export_to) + sizeof(FR_EXPORT_SUFFIX) <=
               sizeof(path)) {
        SDL_snprintf(path, sizeof(path), "%s%s", app->export_to,
                     FR_EXPORT_SUFFIX);
    } else {
        ui_trouble(app, FRIEND_LIT("that path is too long to export to"));
        return;
    }
    index = fr_shelf_find(&app->shelf, app->export_file,
                          (i32)SDL_strlen(app->export_file));
    if (index < 0) {
        ui_trouble(app, FRIEND_LIT("that friend is not in the list any "
                                   "more, so it was not exported"));
        return;
    }
    if (!fr_shelf_export(&app->shelf, app->fr, app->net, index, path)) {
        ui_shelf_trouble(app);
        return;
    }
    ui_say(app, 0, "exported %.*s to %s â import it on the other "
                   "machine",
           (int)app->shelf.entries[index].name_len,
           app->shelf.entries[index].name, path);
}

/* ------------------------------------------------------------------ *
 *  Talking
 * ------------------------------------------------------------------ */

static b32 ui_can_send(Ui_App *app)
{
    /* A photograph on its own is a thing to send; the service allowed
     * it and so does the library. */
    return app->have_friend && !app->awaiting &&
           (str_trim(ui_edit_text(&app->composer)).len > 0 ||
            app->pending_count > 0);
}

static void ui_send(Ui_App *app)
{
    if (!ui_can_send(app)) return;
    app->trouble.len = 0;
    if (friend_say(app->fr, ui_edit_text(&app->composer),
                   app->pending_count > 0 ? app->pending : 0,
                   app->pending_count) != FRIEND_OK) {
        ui_trouble(app, friend_last_error(app->fr));
        return;
    }
    /* The friend has its own copy of the photographs now. */
    app->pending_count = 0;
    arena_reset(&app->outgoing);
    ui_photos_forget_pending(&app->photos);
    ui_edit_clear(&app->composer);
    app->awaiting     = 1;
    app->stick_to_end = 1;
}

/* One button for the microphone, because what somebody wants from it is
 * always "whatever this is doing now, stop": off it opens, open it
 * closes, and while the friend is talking it interrupts — which is a
 * thing the friend remembers, with how much of the sentence was heard,
 * and the window is the only one that knows that number. */
static void ui_voice_toggle(Ui_App *app)
{
    Friend_Voice_State state = friend_voice_state(app->fr);

    if (state == FRIEND_VOICE_SPEAKING) {
        friend_retract_reply(app->fr, ui_audio_heard_fraction(&app->audio));
        ui_audio_hush(&app->audio);
        return;
    }
    if (state == FRIEND_VOICE_OFF) {
        if (friend_voice_open(app->fr) != FRIEND_OK) {
            ui_trouble(app, friend_last_error(app->fr));
            return;
        }
        if (!ui_audio_mic_on(&app->audio)) {
            friend_voice_close(app->fr);
            ui_trouble(app, str_lit("no microphone \342\200\224 this machine "
                                    "has none, or it will not open one"));
        }
        return;
    }
    friend_voice_close(app->fr);
    ui_audio_mic_off(&app->audio);
}

/* ------------------------------------------------------------------ *
 *  Settings
 *
 *  A sheet over the window holding everything in Friend_Config a
 *  person might want to change while the program is running, one row
 *  per entry of ui_setting_defs. It writes straight into app->settings,
 *  so applying it is one friend_reconfigure and one file.
 *
 *  Drawn the way everything else here is: a cursor down the page and a
 *  rectangle per row. The rows are measured as they are drawn and the
 *  total is used to clamp the scroll next frame, which is a frame
 *  behind and never visible.
 * ------------------------------------------------------------------ */

#define UI_SHEET_WIDTH 680.0f
#define UI_SHEET_LABEL 196.0f
#define UI_SHEET_ROW 32.0f
#define UI_SHEET_GAP 10.0f
#define UI_SHEET_NUMBER 92.0f

static void ui_settings_begin(Ui_App *app)
{
    ui_settings_copy(&app->settings_applied, &app->settings);
    app->settings_open     = 1;
    app->settings_scroll   = 0;
    app->settings_show_key = 0;
    ui_focus(app, ui_setting_key_id(&app->settings));
}

static void ui_settings_close(Ui_App *app)
{
    app->settings_open = 0;
    ui_focus(app, app->naming ? UI_FOCUS_NAME : UI_FOCUS_COMPOSER);
}

/* Whatever was typed and not saved is dropped, which is what cancelling
 * a sheet means everywhere else. */
static void ui_settings_cancel(Ui_App *app)
{
    ui_settings_copy(&app->settings, &app->settings_applied);
    ui_settings_close(app);
}

static void ui_settings_apply(Ui_App *app)
{
    Friend_Config config = app->config;

    ui_settings_into_config(&app->settings, &app->env, &config);
    config.utc_offset_minutes = fr_local_utc_offset_minutes();
    if (friend_reconfigure(app->fr, &config) != FRIEND_OK) {
        ui_trouble(app, friend_last_error(app->fr));
        return;
    }
    app->config             = config;
    app->utc_offset_minutes = config.utc_offset_minutes;
    app->have_key           = config.api_key.len > 0;
    ui_settings_copy(&app->settings_applied, &app->settings);
    /* Switching voice off while it is listening closes the device too:
     * a microphone nobody is reading is still a microphone that is on,
     * and the light on the machine says so. */
    if (!app->settings.on[UI_SETTING_VOICE] &&
        friend_voice_state(app->fr) != FRIEND_VOICE_OFF) {
        friend_voice_close(app->fr);
        ui_audio_mic_off(&app->audio);
    }
    app->trouble.len = 0;

    if (ui_settings_save(&app->settings, app->settings_path, &app->frame)) {
        ui_say(app, 0, "settings saved");
    } else {
        ui_say(app, 1, "settings applied, but %s could not be written",
               app->settings_path);
    }
    ui_settings_close(app);
}

/* Tab, and shift-tab, over the fields that are on screen. */
static void ui_settings_tab(Ui_App *app, i32 direction)
{
    i32 id = app->focus;
    i32 i;
    for (i = 0; i < UI_SETTING_COUNT; ++i) {
        id += direction;
        if (id <= UI_SETTING_NONE) id = UI_SETTING_COUNT - 1;
        if (id >= UI_SETTING_COUNT) id = UI_SETTING_NONE + 1;
        if (ui_setting_is_text(id) && ui_setting_visible(&app->settings, id)) {
            break;
        }
    }
    ui_focus(app, id);
}

typedef struct Ui_Sheet {
    Ui_App *app;
    f32     y; /* the cursor, in window coordinates */
    f32     x;
    f32     w;
} Ui_Sheet;

static void ui_sheet_heading(Ui_Sheet *sh, String title)
{
    Ui_App *app = sh->app;
    sh->y += ui_s(app, 18.0f);
    ui_label(app, 0, title, sh->x, sh->y, UI_NAME, ui_rgb(UI_INK), 0);
    sh->y += ui_line_h(app, UI_NAME) + ui_s(app, 6.0f);
    ui_fill(&app->draw, ui_rect(sh->x, sh->y, sh->w, ui_s(app, 1.0f)),
            ui_rgb(UI_LINE_STRONG));
    sh->y += ui_s(app, 10.0f);
}

/* An aside, in the voice the rest of the program uses: what the thing
 * means, or where it ends up. `indent` lines it up with the rows'
 * controls rather than their labels. */
static void ui_sheet_note(Ui_Sheet *sh, String text, f32 indent)
{
    Ui_App   *app = sh->app;
    Ui_Layout layout =
        ui_lay(app, 0, text, UI_SMALL, sh->w - indent - ui_s(app, 2.0f));
    ui_draw_layout(app, &layout, sh->x + indent, sh->y, ui_rgb(UI_TEXT_DIM));
    sh->y += layout.height + ui_s(app, 8.0f);
}

/* A row: its label, and the rectangle beside it the control goes in. */
static Ui_Rect ui_sheet_row(Ui_Sheet *sh, String label, f32 wide)
{
    Ui_App *app     = sh->app;
    f32     label_w = ui_s(app, UI_SHEET_LABEL);
    f32     row_h   = ui_s(app, UI_SHEET_ROW);
    ui_label_in(app, label, ui_rect(sh->x, sh->y, label_w, row_h), UI_BODY,
                ui_rgb(UI_TEXT), 0);
    return ui_rect(sh->x + label_w, sh->y,
                   wide > 0 ? wide : sh->w - label_w, row_h);
}

/* What an empty field means, in grey inside it. */
static String ui_sheet_placeholder(Ui_App *app, i32 id)
{
    Ui_Settings *s   = &app->settings;
    String       env = fr_env_key(&app->env, (Friend_Provider)s->provider);
    switch (ui_setting_def(id)->kind) {
    case UI_KIND_KEY:
        if (env.len <= 0) return str_lit("paste it here");
        return ui_settings_openai(s)
                   ? str_lit("set from OPENAI_KEY; one typed here is used "
                             "instead")
                   : str_lit("set from GEMINI_KEY; one typed here is used "
                             "instead");
    case UI_KIND_BASE_URL:
        if (app->env.base_url.len > 0) return app->env.base_url;
        return ui_settings_openai(s) ? str_lit("https://api.openai.com/v1")
                                     : str_lit("the provider's own endpoint");
    case UI_KIND_MODEL:
        if (id == UI_SETTING_MODEL_LIVE && app->env.live_model.len > 0) {
            return app->env.live_model;
        }
        return ui_setting_model_default(s->provider, id);
    case UI_KIND_NUMBER: {
        String_Builder sb = sb_begin(&app->frame);
        sb_append_i64(&sb, ui_setting_number_default(id));
        return sb_done(&sb);
    }
    default:
        return id == UI_SETTING_VOICE_NAME ? friend_voice(app->fr)
                                           : friend_default_config().user_name;
    }
}

/* One editable field. The key is shown as bullets until somebody asks,
 * with the asking drawn inside the field because there is nowhere else
 * for it to go. A knob is narrower, because none of them is wider than
 * three digits. */
static void ui_sheet_field(Ui_Sheet *sh, i32 id)
{
    Ui_App               *app    = sh->app;
    const Ui_Setting_Def *def    = ui_setting_def(id);
    b32                   secret = def->kind == UI_KIND_KEY;
    Ui_Rect               row    = ui_sheet_row(
        sh, str_from_cstr(def->label),
        def->kind == UI_KIND_NUMBER ? ui_s(app, UI_SHEET_NUMBER) : 0);
    f32     pad   = ui_s(app, 10.0f);
    Ui_Rect inner = ui_inset(row, pad, 0);

    ui_field(app, row, ui_s(app, 8.0f), ui_rgb(UI_WHITE),
             ui_rgb(app->focus == id ? UI_ACCENT : UI_LINE_STRONG));
    if (secret) {
        String  word   = app->settings_show_key ? str_lit("hide")
                                                : str_lit("show");
        f32     wide   = ui_measure(app, word, UI_SMALL) + pad;
        Ui_Rect reveal = ui_rect(row.x + row.w - wide, row.y, wide, row.h);
        inner.w -= wide;
        ui_label_in(app, word, reveal, UI_SMALL, ui_rgb(UI_ACCENT), 0);
        if (ui_press(app, reveal)) {
            app->settings_show_key = !app->settings_show_key;
        }
    }
    ui_text_field(app, id, &app->settings.text[id], row, inner, UI_BODY,
                  ui_sheet_placeholder(app, id),
                  secret && !app->settings_show_key);
    sh->y += row.h + ui_s(app, UI_SHEET_GAP);
}

static void ui_sheet_switch(Ui_Sheet *sh, i32 id)
{
    Ui_App               *app = sh->app;
    const Ui_Setting_Def *def = ui_setting_def(id);
    Ui_Rect               row = ui_sheet_row(sh, str_from_cstr(def->label),
                                             ui_s(app, 44.0f));
    ui_switch(app, ui_inset(row, 0, (row.h - ui_s(app, 24.0f)) * 0.5f),
              &app->settings.on[id]);
    sh->y += row.h + ui_s(app, 4.0f);
    if (def->note) {
        ui_sheet_note(sh, str_from_cstr(def->note), ui_s(app, UI_SHEET_LABEL));
    } else {
        sh->y += ui_s(app, UI_SHEET_GAP - 4.0f);
    }
}

/* Which provider, as the two it can be. Switching it moves the key and
 * the base URL to that provider's own, and empties any model name that
 * was the other one's default. */
static void ui_sheet_provider(Ui_Sheet *sh, i32 id)
{
    Ui_App *app   = sh->app;
    Ui_Rect track = ui_sheet_row(sh, str_from_cstr(ui_setting_def(id)->label),
                                 ui_s(app, 400.0f));
    i32     i;

    ui_fill_rounded(&app->draw, track, ui_s(app, 8.0f), ui_rgb(UI_SURFACE));
    for (i = 0; i < 2; ++i) {
        String  caption = i == 0 ? str_lit("Gemini")
                                 : str_lit("OpenAI-compatible");
        Ui_Rect half    = ui_rect(track.x + (f32)i * track.w * 0.5f, track.y,
                                  track.w * 0.5f, track.h);
        b32     chosen  = app->settings.provider == i;

        if (chosen) {
            f32 inset = ui_s(app, 3.0f);
            ui_fill_rounded(&app->draw, ui_inset(half, inset, inset),
                            ui_s(app, 6.0f), ui_rgb(UI_BLACK));
        }
        ui_label_in(app, caption, half, UI_NAME,
                    ui_rgb(chosen ? UI_WHITE : UI_TEXT), 0.5f);
        if (!chosen && ui_press(app, half)) {
            ui_settings_choose_provider(&app->settings, i);
            if (!ui_setting_visible(&app->settings, app->focus)) {
                ui_focus(app, ui_setting_key_id(&app->settings));
            }
        }
    }
    sh->y += track.h + ui_s(app, UI_SHEET_GAP);
}

/* A path, or anything else the window knows and the person cannot
 * change, in the dim grey the rest of the program uses for facts about
 * itself. */
static void ui_sheet_fact(Ui_Sheet *sh, String label, String value)
{
    Ui_App   *app     = sh->app;
    f32       label_w = ui_s(app, UI_SHEET_LABEL);
    Ui_Layout layout;

    ui_label(app, 0, label, sh->x, sh->y, UI_SMALL, ui_rgb(UI_TEXT), 0);
    layout = ui_lay(app, 0, value, UI_SMALL, sh->w - label_w);
    ui_draw_layout(app, &layout, sh->x + label_w, sh->y, ui_rgb(UI_TEXT_DIM));
    sh->y += layout.height + ui_s(app, 8.0f);
}

static void ui_sheet_rows(Ui_Sheet *sh)
{
    Ui_App *app = sh->app;
    i32     id;

    for (id = UI_SETTING_NONE + 1; id < UI_SETTING_COUNT; ++id) {
        const Ui_Setting_Def *def = ui_setting_def(id);
        if (def->heading) ui_sheet_heading(sh, str_from_cstr(def->heading));
        if (def->intro) ui_sheet_note(sh, str_from_cstr(def->intro), 0);
        if (!ui_setting_visible(&app->settings, id)) continue;
        switch (def->kind) {
        case UI_KIND_PROVIDER: ui_sheet_provider(sh, id); break;
        case UI_KIND_SWITCH: ui_sheet_switch(sh, id); break;
        default: ui_sheet_field(sh, id); break;
        }
    }

    ui_sheet_heading(sh, str_lit("On this machine"));
    {
        Friend_Stats   stats = friend_stats(app->fr);
        String_Builder sb;

        ui_sheet_fact(sh, str_lit("Friends"), str_from_cstr(app->shelf.dir));
        ui_sheet_fact(sh, str_lit("Settings"),
                      str_from_cstr(app->settings_path));
        ui_sheet_fact(sh, str_lit("Photographs"),
                      app->have_friend ? str_from_cstr(app->shelf.blob_path)
                                       : str_lit("no friend open"));
        sb = sb_begin(&app->frame);
        sb_append_i64(&sb, (i64)(stats.bytes_committed / FR_MB(1)));
        sb_append_cstr(&sb, " MiB in use of ");
        sb_append_i64(&sb, (i64)(friend_required_memory(app->config.limits) /
                                 FR_MB(1)));
        sb_append_cstr(&sb, " MiB reserved");
        ui_sheet_fact(sh, str_lit("Memory"), sb_done(&sb));
    }
    sh->y += ui_s(app, 8.0f);
}

static void ui_settings_panel(Ui_App *app)
{
    f32      pad    = ui_s(app, 26.0f);
    f32      wide   = ui_s(app, UI_SHEET_WIDTH);
    f32      margin = ui_s(app, 36.0f);
    f32      tall;
    Ui_Rect  card, body, foot;
    Ui_Sheet sh;
    f32      top;

    ui_fill(&app->draw, ui_rect(0, 0, app->in.width, app->in.height),
            ui_rgba(UI_BLACK, 0.34f));

    if (wide > app->in.width - margin * 2) wide = app->in.width - margin * 2;
    tall = app->in.height - margin * 2;
    if (tall > ui_s(app, 940.0f)) tall = ui_s(app, 940.0f);
    card = ui_rect((app->in.width - wide) * 0.5f,
                   (app->in.height - tall) * 0.5f, wide, tall);
    ui_shadow_rounded(&app->draw, card, ui_s(app, 16.0f), ui_s(app, 1.0f));
    ui_fill_rounded(&app->draw, card, ui_s(app, 16.0f), ui_rgb(UI_WHITE));

    top = card.y + pad;
    ui_label(app, app->serif, str_lit("Settings"), card.x + pad, top,
             UI_TITLE, ui_rgb(UI_INK), 0);
    top += ui_line_h(app, UI_TITLE) + ui_s(app, 2.0f);
    ui_label(app, 0,
             str_lit("Bring your own key. Nothing here is sent anywhere "
                     "but the provider you point it at."),
             card.x + pad, top, UI_SMALL, ui_rgb(UI_TEXT_DIM), 0);
    top += ui_line_h(app, UI_SMALL) + ui_s(app, 14.0f);

    body = ui_rect(card.x + pad, top, card.w - pad * 2,
                   card.y + card.h - ui_s(app, 72.0f) - top);
    ui_fill(&app->draw, ui_rect(body.x, body.y, body.w, ui_s(app, 1.0f)),
            ui_rgb(UI_LINE));

    /* Clamped against what last frame measured, so the rows are drawn
     * once and the scroll is right by the time anybody sees it. */
    app->settings_scroll =
        fr_clamp(0.0f, app->settings_scroll,
                 fr_max(0.0f, app->settings_content - body.h));

    memset(&sh, 0, sizeof(sh));
    sh.app = app;
    sh.x   = body.x;
    sh.w   = body.w - ui_s(app, 6.0f);
    sh.y   = body.y - app->settings_scroll;

    ui_clip(&app->draw, body);
    ui_sheet_rows(&sh);
    ui_unclip(&app->draw);
    app->settings_content = sh.y - (body.y - app->settings_scroll);

    /* The ends of the scroll faded into the card, the way the
     * transcript is faded into the chrome. */
    if (app->settings_scroll > 1.0f) {
        ui_fade(&app->draw, ui_rect(body.x, body.y, body.w, ui_s(app, 16.0f)),
                ui_rgb(UI_WHITE), 1);
    }
    if (app->settings_content - app->settings_scroll > body.h + 1.0f) {
        ui_fade(&app->draw,
                ui_rect(body.x, body.y + body.h - ui_s(app, 20.0f), body.w,
                        ui_s(app, 20.0f)),
                ui_rgb(UI_WHITE), 0);
    }

    /* The foot: Save on the right, Cancel beside it, and what is
     * missing, if anything, on the left. */
    foot = ui_rect(card.x + pad, card.y + card.h - pad - ui_s(app, 38.0f),
                   card.w - pad * 2, ui_s(app, 38.0f));
    {
        Ui_Rect save   = ui_cut_right(&foot, ui_s(app, 108.0f));
        Ui_Rect cancel = ui_cut_right(&foot, ui_s(app, 10.0f));
        cancel         = ui_cut_right(&foot, ui_s(app, 92.0f));
        if (!app->have_key) {
            ui_label_in(app,
                        str_lit("No key yet: the friend can remember, but "
                                "it cannot answer."),
                        foot, UI_SMALL, ui_rgb(UI_WARN), 0);
        }
        if (ui_button(app, cancel, 0, str_lit("Cancel"), UI_NAME, 0)) {
            ui_settings_cancel(app);
        } else if (ui_button(app, save, 0, str_lit("Save"), UI_NAME, 1)) {
            ui_settings_apply(app);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  The transcript's shape
 * ------------------------------------------------------------------ */

/* True when enough time passed between two messages to be worth saying
 * so. The friend's own prompts are full of how long ago things were; a
 * transcript that hides it reads worse than one that does not. */
static b32 ui_needs_time_mark(u64 previous_ms, u64 now_ms)
{
    if (previous_ms == 0) return 1;
    if (now_ms < previous_ms) return 0;
    return now_ms - previous_ms > 60u * 60u * 1000u;
}

/* One message, placed with its top at y in a column `area` wide. */
typedef struct Ui_Message_Box {
    Friend_Message message;
    f32            height;  /* all of it, the gap under it included    */
    b32            stamped; /* a time mark goes above it, in `stamp`   */
    Ui_Rect        stamp;
    Ui_Rect        body;    /* the bubble, or the notice's or the
                             * overheard line's row                    */
    Ui_Layout      text;
    f32            text_x;  /* where the text starts                   */
    f32            text_y;
    Ui_Rect        photo;   /* the first photograph's slot; the rest
                             * follow, a slot apart                    */
} Ui_Message_Box;

static Ui_Message_Box ui_message_layout(Ui_App *app, i32 index, Ui_Rect area,
                                        f32 y)
{
    Ui_Message_Box box;
    Friend_Message previous;
    f32            pad_x = ui_s(app, UI_BUBBLE_PAD_X);
    f32            pad_y = ui_s(app, UI_BUBBLE_PAD_Y);
    f32            widest = area.w * UI_BUBBLE_SHARE;
    f32            wrap  = widest - pad_x * 2;
    f32            top   = y;

    memset(&box, 0, sizeof(box));
    if (friend_message(app->fr, index, &box.message) != FRIEND_OK) return box;

    previous.unix_ms = 0;
    if (index > 0) friend_message(app->fr, index - 1, &previous);
    box.stamped = ui_needs_time_mark(index > 0 ? previous.unix_ms : 0,
                                     box.message.unix_ms);
    if (box.stamped) {
        box.stamp = ui_rect(area.x, y + ui_s(app, 6.0f), area.w,
                            ui_line_h(app, UI_SMALL));
        y += ui_line_h(app, UI_SMALL) + ui_s(app, 20.0f);
    }

    switch (box.message.kind) {
    case FRIEND_MESSAGE_NOTICE:
        /* A rule either side of a short grey line, which is how both
         * clients mark the point where the conversation was cut. */
        box.body = ui_rect(area.x, y, area.w,
                           ui_line_h(app, UI_SMALL) + ui_s(app, 24.0f));
        break;
    case FRIEND_MESSAGE_AMBIENT: {
        /* Audio the friend happened to overhear. Neither client shows
         * these in the timeline at all; this one is a reader, so it
         * shows them, quietly and in the middle where they cannot be
         * mistaken for something somebody said. */
        f32 glyph = app->icons ? ui_s(app, 14.0f) : 0;
        f32 gap   = glyph > 0 ? ui_s(app, 6.0f) : 0;
        box.text  = ui_lay(app, 0, box.message.text, UI_SMALL, wrap);
        box.body  = ui_rect(area.x + (area.w - (glyph + gap + box.text.width)) *
                                        0.5f,
                            y, glyph + gap + box.text.width,
                            box.text.height + pad_y * 2);
        box.text_x = box.body.x + glyph + gap;
        box.text_y = y + pad_y;
        break;
    }
    default: {
        b32 from_user = box.message.kind == FRIEND_MESSAGE_USER;
        box.text      = ui_lay(app, 0, box.message.text, UI_BODY, wrap);
        box.body = ui_rect(from_user ? area.x + area.w - box.text.width -
                                           pad_x * 2
                                     : area.x,
                           y, box.text.width + pad_x * 2,
                           box.text.height + pad_y * 2);
        box.text_x = box.body.x + pad_x;
        box.text_y = y + pad_y;
        /* A fixed slot per photograph, whatever shape it turns out to
         * be: the conversation must not reflow when one finishes
         * decoding, and it cannot be measured before it does. */
        box.photo = ui_rect(area.x, box.body.y + box.body.h +
                                        ui_s(app, UI_PHOTO_GAP),
                            widest, ui_s(app, UI_PHOTO_HEIGHT));
        y = box.body.y + box.body.h +
            (box.photo.h + ui_s(app, UI_PHOTO_GAP)) *
                (f32)box.message.attachment_count;
        box.height = y - top + ui_s(app, UI_MESSAGE_GAP);
        return box;
    }
    }
    box.height = box.body.y - top + box.body.h + ui_s(app, UI_MESSAGE_GAP);
    return box;
}

static f32 ui_message_height(Ui_App *app, i32 index, f32 width)
{
    Arena_Mark     mark = arena_mark(&app->frame);
    Ui_Message_Box box  = ui_message_layout(app, index,
                                            ui_rect(0, 0, width, 0), 0);
    arena_reset_to(&app->frame, mark);
    return box.height;
}

static void ui_measure_transcript(Ui_App *app, f32 width)
{
    Ui_Heights *h     = &app->heights;
    i32         count = friend_message_count(app->fr);
    i32         i;

    /* The newest, not the first: a conversation is read from the end. */
    h->first  = count > UI_MESSAGES_MAX ? count - UI_MESSAGES_MAX : 0;
    h->count  = count - h->first;
    h->lost   = friend_stats(app->fr).messages_lost;
    h->width  = width;
    h->scale  = app->scale;
    h->friend = app->friend_serial;
    h->total  = 0;
    for (i = 0; i < h->count; ++i) {
        h->height[i] = ui_message_height(app, h->first + i, width);
        h->total += h->height[i];
    }
}

/* A reply arriving changes the height of one message every frame, and a
 * new message adds one. Neither is a reason to shape the whole
 * conversation again. */
static void ui_measure_tail(Ui_App *app, f32 width)
{
    Ui_Heights *h     = &app->heights;
    i32         count = friend_message_count(app->fr);
    i32         have  = count - h->first;
    i32         i;

    if (h->width != width || h->scale != app->scale ||
        h->friend != app->friend_serial ||
        h->lost != friend_stats(app->fr).messages_lost ||
        have < h->count || have > UI_MESSAGES_MAX) {
        ui_measure_transcript(app, width);
        return;
    }
    for (i = h->count; i < have; ++i) {
        h->height[i] = ui_message_height(app, h->first + i, width);
        h->total += h->height[i];
    }
    h->count = have;

    /* The last message is measured again every frame, whether or not it
     * says it is still streaming: the delta that finishes a reply
     * arrives in the same tick that clears the flag, and a height that
     * is short by a line leaves that line under the fade at the bottom
     * of the window. One message a frame is nothing. */
    if (h->count > 0) {
        f32 height = ui_message_height(app, count - 1, width);
        h->total += height - h->height[h->count - 1];
        h->height[h->count - 1] = height;
    }
}

/* ------------------------------------------------------------------ *
 *  The transcript
 * ------------------------------------------------------------------ */

static void ui_draw_photo(Ui_App *app, Ui_Photo *photo, Ui_Rect frame,
                          f32 radius)
{
    if (photo) {
        SDL_FRect dest;
        dest.x = frame.x;
        dest.y = frame.y;
        dest.w = frame.w;
        dest.h = frame.h;
        SDL_RenderTexture(app->renderer, photo->texture, 0, &dest);
        return;
    }
    /* Not decoded yet, or there are no pixels to decode: a quiet
     * placeholder of the right size, which is what the layout reserved
     * anyway. */
    ui_fill_rounded(&app->draw, frame, radius, ui_rgba(UI_RULE, 0.35f));
}

static void ui_draw_message(Ui_App *app, i32 index, Ui_Rect area, f32 y)
{
    Ui_Message_Box box = ui_message_layout(app, index, area, y);
    b32            from_user = box.message.kind == FRIEND_MESSAGE_USER;
    i32            j;

    if (box.stamped) {
        Arena_Mark mark  = arena_mark(&app->frame);
        String     stamp = fr_format_timestamp(&app->frame,
                                               box.message.unix_ms,
                                               app->utc_offset_minutes, 0);
        ui_label(app, 0, stamp, box.stamp.x + box.stamp.w * 0.5f,
                 box.stamp.y, UI_SMALL, ui_rgb(UI_TEXT_DIM), 0.5f);
        arena_reset_to(&app->frame, mark);
    }

    switch (box.message.kind) {
    case FRIEND_MESSAGE_NOTICE: {
        f32 mid    = box.body.y + ui_s(app, 12.0f);
        f32 middle = box.body.x + box.body.w * 0.5f;
        f32 gutter = box.message.text.len > 0
                         ? ui_measure(app, box.message.text, UI_SMALL) * 0.5f +
                               ui_s(app, 12.0f)
                         : 0;
        Ui_Color rule = ui_rgba(UI_RULE, 0.5f);

        ui_fill(&app->draw,
                ui_rect(box.body.x, mid, box.body.w * 0.5f - gutter,
                        ui_s(app, 1.0f)),
                rule);
        ui_fill(&app->draw,
                ui_rect(middle + gutter, mid, box.body.w * 0.5f - gutter,
                        ui_s(app, 1.0f)),
                rule);
        ui_label(app, 0, box.message.text, middle,
                 mid - ui_line_h(app, UI_SMALL) * 0.5f, UI_SMALL,
                 ui_rgb(UI_TEXT_DIM), 0.5f);
        return;
    }
    case FRIEND_MESSAGE_AMBIENT:
        ui_icon(app, UI_ICON_EAR, box.body.x + ui_s(app, 7.0f),
                box.text_y + ui_line_h(app, UI_SMALL) * 0.5f, 14.0f,
                ui_rgb(UI_TEXT_FAINT));
        ui_draw_layout(app, &box.text, box.text_x, box.text_y,
                       ui_rgb(UI_TEXT_FAINT));
        return;
    default: break;
    }

    /* Only the user gets a bubble. The friend's words sit straight on
     * the wash — that asymmetry is the whole look. */
    if (from_user) {
        ui_shadow_rounded(&app->draw, box.body, ui_s(app, UI_BUBBLE_RADIUS),
                          ui_s(app, 1.0f));
        ui_fill_rounded(&app->draw, box.body, ui_s(app, UI_BUBBLE_RADIUS),
                        ui_rgb(UI_WHITE));
    }
    ui_draw_layout(app, &box.text, box.text_x, box.text_y, ui_rgb(UI_TEXT));

    for (j = 0; j < box.message.attachment_count; ++j) {
        Ui_Photo *photo = ui_photo_for(&app->photos, app->fr, index, j);
        Ui_Rect   slot  = box.photo;
        f32       wide  = slot.h;
        f32       tall  = slot.h;

        slot.y += (slot.h + ui_s(app, UI_PHOTO_GAP)) * (f32)j;
        if (photo && photo->height > 0) {
            wide = tall * (f32)photo->width / (f32)photo->height;
            if (wide > slot.w) {
                wide = slot.w;
                tall = wide * (f32)photo->height / (f32)photo->width;
            }
        }
        ui_draw_photo(app, photo,
                      ui_rect(from_user ? area.x + area.w - wide : area.x,
                              slot.y, wide, tall),
                      ui_s(app, UI_BUBBLE_RADIUS));
        if (!photo) {
            ui_icon(app, UI_ICON_IMAGE, (from_user ? area.x + area.w - wide
                                                   : area.x) +
                                            wide * 0.5f,
                    slot.y + tall * 0.5f, 18.0f, ui_rgb(UI_TEXT_FAINT));
        }
    }
}

/* What is above the top of the scroll, when anything is.
 *
 * Two different things can be: the friend no longer holds the oldest
 * messages, or it does and this window is only measuring the newest
 * sixteen thousand of them. Either way the person is looking at the
 * middle of a conversation, and a scroll that just stops is a lie about
 * where it began. */
static f32 ui_older(Ui_App *app, Ui_Rect area, f32 y, b32 draw)
{
    Friend_Stats   stats = friend_stats(app->fr);
    String_Builder sb;

    if (stats.messages_lost == 0 && app->heights.first == 0) return 0;
    if (draw) {
        sb = sb_begin(&app->frame);
        if (stats.messages_lost > 0) {
            sb_append_i64(&sb, (i64)stats.messages_lost);
            sb_append(&sb, str_lit(" earlier message"));
            if (stats.messages_lost != 1) sb_append_char(&sb, 's');
            sb_append(&sb, str_lit(" are no longer kept"));
            if (app->heights.first > 0) {
                sb_append(&sb, str_lit(", and "));
                sb_append_i64(&sb, app->heights.first);
                sb_append(&sb, str_lit(" more are above this window"));
            }
        } else {
            sb_append_i64(&sb, app->heights.first);
            sb_append(&sb, str_lit(" earlier messages are above this window"));
        }
        ui_label(app, 0, sb_done(&sb), area.x + area.w * 0.5f,
                 y + ui_s(app, 10.0f), UI_SMALL, ui_rgb(UI_TEXT_FAINT), 0.5f);
    }
    return ui_line_h(app, UI_SMALL) + ui_s(app, 28.0f);
}

/* Three dots where the friend's next line will be, while it is working
 * and has not started speaking. Once the reply begins arriving it is a
 * message like any other and says plenty for itself.
 *
 * A window that sits there doing nothing after you press return looks
 * broken, and this friend can take a few seconds: it has to decide what
 * it remembers before it decides what to say. */
static f32 ui_thinking(Ui_App *app, Ui_Rect area, f32 y, b32 draw)
{
    Friend_Message last;
    i32            count;
    f32            line_h = ui_line_h(app, UI_BODY);
    f32            pad_y  = ui_s(app, UI_BUBBLE_PAD_Y);

    if (!app->awaiting || !app->have_friend) return 0;
    count = friend_message_count(app->fr);
    if (count > 0 &&
        friend_message(app->fr, count - 1, &last) == FRIEND_OK &&
        last.kind == FRIEND_MESSAGE_FRIEND && last.text.len > 0) {
        return 0;
    }
    if (draw) {
        f32 dot = ui_s(app, 7.0f);
        f32 gap = ui_s(app, 6.0f);
        f32 mid = y + pad_y + line_h * 0.5f;
        u64 now = SDL_GetTicks();
        i32 i;
        for (i = 0; i < 3; ++i) {
            /* Each dot a step behind the last, so it reads as one thing
             * moving rather than three things flashing. A triangle
             * wave: there is no reason to link a trigonometric library
             * into a chat window. */
            u32 phase = (u32)((now + (u64)i * 160u) % 900u);
            f32 along = (f32)phase / 900.0f;
            f32 up    = along < 0.5f ? along * 2.0f : 2.0f - along * 2.0f;
            ui_fill_rounded(&app->draw,
                            ui_rect(area.x + ui_s(app, UI_BUBBLE_PAD_X) +
                                        (f32)i * (dot + gap),
                                    mid - dot * 0.5f - up * ui_s(app, 2.0f),
                                    dot, dot),
                            dot * 0.5f,
                            ui_rgba(UI_TEXT_DIM, 0.3f + 0.5f * up));
        }
    }
    return line_h + pad_y * 2 + ui_s(app, UI_MESSAGE_GAP);
}

/* Where the scroll may go, and where it sticks: at the end, until
 * somebody scrolls away from it, and again once they scroll back. */
static f32 ui_transcript_scroll(Ui_App *app, f32 content, f32 view)
{
    f32 most = fr_max(0.0f, content - view);
    if (app->stick_to_end) app->scroll = most;
    app->scroll       = fr_clamp(0.0f, app->scroll, most);
    app->stick_to_end = most - app->scroll < 1.0f;
    return most;
}

/* A scrollbar, because a thousand messages without one is a cliff — and
 * one you can take hold of, because a thousand messages is also a lot of
 * wheel. */
static void ui_scrollbar(Ui_App *app, Ui_Rect gutter, f32 content, f32 most)
{
    Ui_Rect track = ui_rect(gutter.x + ui_s(app, UI_SCROLLBAR_AT), gutter.y,
                            ui_s(app, UI_SCROLLBAR), gutter.h);
    Ui_Rect grab  = ui_inset(track, -ui_s(app, UI_SCROLLBAR_REACH), 0);
    f32     thumb_h = fr_max(track.h * (track.h / content), ui_s(app, 36.0f));
    f32     travel  = track.h - thumb_h;
    Ui_Rect thumb   = ui_rect(track.x,
                              track.y + (travel > 0
                                             ? travel * (app->scroll / most)
                                             : 0),
                              track.w, thumb_h);
    b32     over    = ui_hover(app, grab);
    b32     held;

    if (ui_press(app, grab)) {
        app->held = UI_HELD_SCROLLBAR;
        /* On the thumb it keeps hold where it was taken; on the empty
         * track the thumb comes to the pointer first. */
        app->scroll_grab =
            ui_rect_contains(ui_rect(grab.x, thumb.y, grab.w, thumb.h),
                             app->in.mouse_x, app->in.mouse_y)
                ? app->in.mouse_y - thumb.y
                : thumb.h * 0.5f;
    }
    held = app->held == UI_HELD_SCROLLBAR;
    if (held && travel > 0) {
        f32 at = app->in.mouse_y - app->scroll_grab - track.y;
        app->scroll       = fr_clamp(0.0f, (at / travel) * most, most);
        app->stick_to_end = 0;
        thumb.y           = track.y + travel * (app->scroll / most);
    }

    ui_fill_rounded(&app->draw, track, track.w * 0.5f,
                    ui_rgba(UI_RULE, over || held ? 0.35f : 0.0f));
    ui_fill_rounded(&app->draw, thumb, track.w * 0.5f,
                    held ? ui_rgba(UI_TEXT_DIM, 0.95f)
                         : ui_rgba(UI_RULE, over ? 1.0f : 0.85f));
}

/* `area` is where messages are laid out and scrolled; `visible` is what
 * may be painted, which reaches up behind the header and down behind the
 * composer so a message passes under them rather than stopping at an
 * edge. The chrome is drawn afterwards, over the top. */
static void ui_transcript(Ui_App *app, Ui_Rect area, Ui_Rect visible,
                          Ui_Rect gutter)
{
    Ui_Heights *h    = &app->heights;
    f32         fade = ui_s(app, UI_TRANSCRIPT_FADE);
    f32         cull = ui_s(app, 32.0f);
    f32         older_h, thinking_h, content, most, y;
    i32         i;

    ui_measure_tail(app, area.w);
    older_h    = ui_older(app, area, 0, 0);
    thinking_h = ui_thinking(app, area, 0, 0);
    /* Room at both ends for the fades to land on. Without it the first
     * and last lines of the conversation are drawn underneath them,
     * which looks like a message that got cut off rather than like the
     * edge of the window. */
    content = h->total + older_h + thinking_h + fade * 2.0f;
    most    = ui_transcript_scroll(app, content, area.h);

    ui_clip(&app->draw, visible);
    y = area.y + fade - app->scroll;
    if (older_h > 0 && y + older_h >= visible.y - cull) {
        ui_older(app, area, y, 1);
    }
    y += older_h;
    for (i = 0; i < h->count; ++i) {
        if (y + h->height[i] >= visible.y - cull &&
            y <= visible.y + visible.h + cull) {
            ui_draw_message(app, h->first + i, area, y);
        }
        y += h->height[i];
    }
    ui_thinking(app, area, y, 1);
    ui_unclip(&app->draw);

    /* Fade into the chrome at both ends. Without this a message is cut
     * level with the header, which reads as a bug rather than as more
     * conversation above. */
    ui_fade(&app->draw, ui_rect(visible.x, area.y, visible.w, fade),
            app->color.wash, 1);
    ui_fade(&app->draw,
            ui_rect(visible.x, visible.y + visible.h - fade, visible.w, fade),
            app->color.wash, 0);

    if (most > 0) {
        ui_scrollbar(app, gutter, content, most);
    } else if (app->held == UI_HELD_SCROLLBAR) {
        app->held = UI_HELD_NONE;
    }
}

/* ------------------------------------------------------------------ *
 *  The sidebar
 * ------------------------------------------------------------------ */

/* What the window has to say, in a bubble of its own in the sidebar,
 * where there is nothing underneath it to be in the way of. Red for
 * something that went wrong, grey for something that merely happened,
 * and an x either way because the person reading it is the one who
 * knows when they are done with it. */
typedef struct Ui_Say_Box {
    b32       shown;
    b32       bad;
    Ui_Rect   bubble;
    Ui_Layout text;
    Ui_Rect   close;
} Ui_Say_Box;

/* Laid out to sit with its bottom edge at `bottom`. */
static Ui_Say_Box ui_say_layout(Ui_App *app, f32 x, f32 w, f32 bottom)
{
    Ui_Say_Box box;
    String     say;
    f32        pad  = ui_s(app, 10.0f);
    f32        tuck = ui_s(app, 20.0f);

    memset(&box, 0, sizeof(box));
    if (!ui_said(app, &say, &box.bad)) return box;
    box.shown  = 1;
    box.text   = ui_lay(app, 0, say, UI_SMALL, w - pad * 2 - tuck);
    box.bubble = ui_rect(x, bottom - box.text.height - pad * 2, w,
                         box.text.height + pad * 2);
    box.close  = ui_rect(x + w - tuck - ui_s(app, 5.0f),
                         box.bubble.y + ui_s(app, 5.0f), tuck, tuck);
    return box;
}

static void ui_say_bubble(Ui_App *app, const Ui_Say_Box *box)
{
    f32 pad = ui_s(app, 10.0f);
    if (!box->shown) return;
    ui_fill_rounded(&app->draw, box->bubble, ui_s(app, 10.0f),
                    box->bad ? ui_rgba(UI_WARN, 0.10f) : ui_rgb(UI_SURFACE));
    ui_draw_layout(app, &box->text, box->bubble.x + pad, box->bubble.y + pad,
                   ui_rgb(box->bad ? UI_WARN : UI_TEXT_DIM));
    if (ui_button_icon(app, box->close, UI_ICON_X, 11.0f, 1, str_lit("x"))) {
        app->trouble.len = 0;
        app->news.len    = 0;
    }
}

/* The ways to get another friend, at the foot of the sidebar: naming
 * one, or importing one — the big black button only while there is
 * nothing here to talk to; after that it is one of the two ways to make
 * a friend. */
typedef struct Ui_Sidebar_Foot {
    f32     top; /* where the list has to stop */
    Ui_Rect field, create, cancel; /* naming */
    Ui_Rect import;                /* naming, or an empty shelf */
    Ui_Rect hint;                  /* an empty shelf */
    Ui_Rect fresh;                 /* not naming */
} Ui_Sidebar_Foot;

static Ui_Sidebar_Foot ui_sidebar_foot_layout(Ui_App *app, Ui_Rect column)
{
    Ui_Sidebar_Foot foot;
    f32             button_h = ui_s(app, 44.0f);
    f32             small_h  = ui_s(app, 38.0f);

    memset(&foot, 0, sizeof(foot));
    if (app->naming) {
        Ui_Rect row;
        foot.import = ui_cut_bottom(&column, small_h);
        ui_cut_bottom(&column, ui_s(app, 10.0f));
        row         = ui_cut_bottom(&column, small_h);
        foot.create = ui_cut_left(&row, (row.w - ui_s(app, 8.0f)) * 0.5f);
        ui_cut_left(&row, ui_s(app, 8.0f));
        foot.cancel = row;
        ui_cut_bottom(&column, ui_s(app, 10.0f));
        foot.field = ui_cut_bottom(&column, button_h);
    } else if (app->shelf.count == 0) {
        foot.import = ui_cut_bottom(&column, button_h);
        foot.hint   = ui_rect(column.x, foot.import.y - ui_s(app, 24.0f),
                              column.w, ui_line_h(app, UI_SMALL));
        ui_cut_bottom(&column, ui_s(app, 36.0f));
        foot.fresh = ui_cut_bottom(&column, small_h);
    } else {
        foot.fresh = ui_cut_bottom(&column, small_h);
    }
    foot.top = column.y + column.h;
    return foot;
}

static void ui_sidebar_foot(Ui_App *app, const Ui_Sidebar_Foot *foot,
                            Ui_Rect area)
{
    if (app->naming) {
        f32     pad   = ui_s(app, 12.0f);
        Ui_Rect inner = ui_inset(foot->field, pad, 0);
        ui_field(app, foot->field, ui_s(app, 10.0f), ui_rgb(UI_WHITE),
                 ui_rgb(UI_ACCENT));
        ui_text_field(app, UI_FOCUS_NAME, &app->name, inner, inner, UI_BODY,
                      str_lit("What are they called?"), 0);
        if (ui_button(app, foot->create, 0, str_lit("Create"), UI_NAME, 1)) {
            ui_naming_commit(app);
        }
        if (ui_button(app, foot->cancel, 0, str_lit("Cancel"), UI_NAME, 0)) {
            ui_naming_end(app);
        }
        if (ui_button(app, foot->import, UI_ICON_UPLOAD,
                      str_lit("Import a file instead"), UI_SMALL, 0)) {
            ui_naming_end(app);
            ui_pick_file(app, &ui_pick_export);
        }
        return;
    }
    if (app->shelf.count == 0) {
        if (ui_button(app, foot->import, UI_ICON_UPLOAD,
                      str_lit("Import a friend file"), UI_NAME, 1)) {
            ui_pick_file(app, &ui_pick_export);
        }
        ui_label(app, 0, str_lit("or drop a friend file here"),
                 area.x + area.w * 0.5f, foot->hint.y, UI_SMALL,
                 ui_rgb(UI_TEXT_FAINT), 0.5f);
    }
    if (ui_button(app, foot->fresh, UI_ICON_PLUS, str_lit("New friend"),
                  UI_NAME, 0)) {
        ui_naming_begin(app);
    }
}

/* The friends, and the ways to get another one. */
static void ui_sidebar(Ui_App *app, Ui_Rect area)
{
    f32             pad     = ui_s(app, 16.0f);
    f32             row_h   = ui_s(app, 64.0f);
    f32             avatar  = ui_s(app, UI_AVATAR);
    f32             line    = ui_s(app, 1.0f);
    f32             title_h = ui_line_h(app, UI_TITLE);
    f32             x       = area.x + pad;
    f32             w       = area.w - pad * 2 - line;
    Ui_Sidebar_Foot foot =
        ui_sidebar_foot_layout(app, ui_rect(x, area.y, w, area.h - pad));
    Ui_Say_Box      say = ui_say_layout(app, x, w, foot.top - ui_s(app, 12.0f));
    f32             bottom = (say.shown ? say.bubble.y : foot.top) -
                 ui_s(app, 12.0f);
    f32             y;
    i32             i;
    i32             archive_me = -1;
    i32             export_me  = -1;
    i32             open_me    = -1;

    ui_fill(&app->draw, area, ui_rgb(UI_PANEL));
    ui_fill(&app->draw, ui_rect(area.x + area.w - line, area.y, line, area.h),
            ui_rgb(UI_LINE_STRONG));

    /* The wordmark, which is the one place this window says whose
     * program it is. Drawn at exactly the size it was built for — one
     * texel to one pixel — because that is the point of having built
     * it at that size. Without the file beside it, the name in words. */
    if (app->logo && app->logo_h > 0) {
        Ui_Color  ink = ui_rgb(UI_INK);
        SDL_FRect dest;
        dest.x = x;
        dest.y = area.y + pad + (title_h - (f32)app->logo_h) * 0.5f;
        dest.w = (f32)app->logo_w;
        dest.h = (f32)app->logo_h;
        SDL_SetTextureColorMod(app->logo, ink.r, ink.g, ink.b);
        SDL_SetTextureAlphaMod(app->logo, 255);
        SDL_RenderTexture(app->renderer, app->logo, 0, &dest);
    } else {
        ui_label(app, 0, str_lit("friend-local"), x, area.y + pad, UI_TITLE,
                 ui_rgb(UI_INK), 0);
    }

    /* The key and the models live behind this, which is why it is on
     * the first row: somebody with no key has to find it before
     * anything else in this window works. */
    {
        f32     size = ui_s(app, 32.0f);
        Ui_Rect gear = ui_rect(area.x + area.w - pad - line - size,
                               area.y + pad + (title_h - size) * 0.5f, size,
                               size);
        if (ui_button_icon(app, gear, UI_ICON_GEAR, 20.0f, 0,
                           str_lit("Settings"))) {
            ui_settings_begin(app);
        }
    }

    ui_say_bubble(app, &say);

    y = area.y + pad + title_h + pad * 0.75f;
    for (i = 0; i < app->shelf.count && y + row_h < bottom; ++i) {
        Fr_Shelf_Entry *entry   = &app->shelf.entries[i];
        String          name    = str_make(entry->name, entry->name_len);
        Ui_Rect         row     = ui_rect(area.x, y, area.w - line, row_h);
        b32             hover   = ui_hover(app, row);
        b32             chosen  = i == app->shelf.current;
        f32             tuck    = ui_s(app, 30.0f);
        f32             text_x  = row.x + pad + avatar + ui_s(app, 12.0f);
        String_Builder  sb;

        if (chosen || hover) ui_fill(&app->draw, row, ui_rgb(UI_ROW_HOVER));
        ui_avatar(&app->draw,
                  ui_rect(row.x + pad, row.y + (row_h - avatar) * 0.5f,
                          avatar, avatar),
                  ui_friend_color(name));
        ui_label(app, 0, name, text_x, row.y + ui_s(app, 12.0f), UI_NAME,
                 ui_rgb(UI_INK), 0);

        sb = sb_begin(&app->frame);
        sb_append_i64(&sb, entry->memories);
        sb_append_cstr(&sb, " memories \302\267 ");
        sb_append_i64(&sb, (i64)entry->total_event_count);
        sb_append_cstr(&sb, " messages");
        ui_label(app, 0, sb_done(&sb), text_x, row.y + ui_s(app, 32.0f),
                 UI_SMALL, ui_rgb(UI_TEXT_DIM), 0);

        ui_fill(&app->draw,
                ui_rect(row.x + pad, row.y + row_h, row.w - pad * 2, line),
                ui_rgb(UI_LINE));

        /* Out of the list, not off the disk — and only offered on the
         * row the pointer is actually on, so it cannot be hit by
         * accident on the way past. */
        if (hover &&
            ui_button_icon(app,
                           ui_rect(row.x + row.w - pad - tuck,
                                   row.y + (row_h - tuck) * 0.5f, tuck, tuck),
                           UI_ICON_ARCHIVE, 17.0f, 1, str_nil)) {
            archive_me = i;
        }
        /* Beside it, the way to take this friend to another machine. */
        if (hover &&
            ui_button_icon(app,
                           ui_rect(row.x + row.w - pad - tuck * 2.0f -
                                       ui_s(app, 4.0f),
                                   row.y + (row_h - tuck) * 0.5f, tuck, tuck),
                           UI_ICON_DOWNLOAD, 17.0f, 1, str_nil)) {
            export_me = i;
        }
        if (ui_press(app, row) && !chosen) open_me = i;
        y += row_h + line;
    }

    ui_sidebar_foot(app, &foot, area);

    /* After the list rather than inside it: either one reshuffles every
     * index it was being drawn from. */
    if (export_me >= 0) ui_export_begin(app, export_me);
    if (archive_me >= 0) ui_archive(app, archive_me);
    if (open_me >= 0) ui_open_friend(app, open_me);
}

/* ------------------------------------------------------------------ *
 *  The main pane
 * ------------------------------------------------------------------ */

/* Avatar and name, centred, the name in a serif — which is where the
 * friend's identity lives in both clients, rather than beside every line
 * they say — and what it holds underneath. */
typedef struct Ui_Header_Box {
    Ui_Rect   area;
    Ui_Rect   avatar;
    Ui_Layout name;
    f32       name_x, name_y;
    f32       stats_y;
} Ui_Header_Box;

static Ui_Header_Box ui_header_layout(Ui_App *app, Ui_Rect pane)
{
    Ui_Header_Box box;
    f32           avatar = ui_s(app, 28.0f);
    f32           gap    = ui_s(app, 10.0f);
    f32           start;

    memset(&box, 0, sizeof(box));
    box.area   = ui_rect(pane.x, pane.y, pane.w,
                         ui_line_h(app, UI_TITLE) + ui_line_h(app, UI_SMALL) +
                             ui_s(app, 26.0f));
    box.name   = ui_lay(app, app->serif, friend_name(app->fr), UI_TITLE, 0);
    start      = pane.x + (pane.w - (avatar + gap + box.name.width)) * 0.5f;
    box.name_x = start + avatar + gap;
    box.name_y = pane.y + ui_s(app, 12.0f);
    box.avatar = ui_rect(start, box.name_y + ui_s(app, 2.0f), avatar, avatar);
    box.stats_y = box.name_y + ui_line_h(app, UI_TITLE);
    return box;
}

static void ui_header(Ui_App *app, const Ui_Header_Box *box)
{
    String_Builder sb    = sb_begin(&app->frame);
    Friend_Stats   stats = friend_stats(app->fr);
    f32            line  = ui_s(app, 1.0f);

    ui_fill(&app->draw, box->area, ui_rgb(UI_PANEL));
    ui_fill(&app->draw,
            ui_rect(box->area.x, box->area.y + box->area.h - line,
                    box->area.w, line),
            ui_rgb(UI_LINE_STRONG));
    ui_avatar(&app->draw, box->avatar, app->color);
    ui_draw_layout(app, &box->name, box->name_x, box->name_y, ui_rgb(UI_INK));

    sb_append_i64(&sb, stats.memories);
    sb_append_cstr(&sb, " memories \302\267 ");
    sb_append_i64(&sb, stats.connections);
    sb_append_cstr(&sb, " connections \302\267 ");
    sb_append_i64(&sb, friend_message_count(app->fr));
    sb_append_cstr(&sb, " messages held");
    if (friend_created_unix_ms(app->fr) > 0) {
        Fr_Date_Time dt = fr_date_time(friend_created_unix_ms(app->fr), 0);
        sb_append_cstr(&sb, " \302\267 since ");
        sb_append(&sb, fr_format_month(dt));
        sb_append_char(&sb, ' ');
        sb_append_i64(&sb, dt.year);
    }
    ui_label(app, 0, sb_done(&sb), box->area.x + box->area.w * 0.5f,
             box->stats_y, UI_SMALL, ui_rgb(UI_TEXT_DIM), 0.5f);
}

static void ui_empty_state(Ui_App *app, Ui_Rect area)
{
    f32 center_x = area.x + area.w * 0.5f;
    f32 y        = area.y + area.h * 0.32f;

    ui_icon(app, UI_ICON_CHAT, center_x, y - ui_s(app, 46.0f), 52.0f,
            ui_rgb(UI_LINE_STRONG));
    ui_label(app, app->serif, str_lit("No friend open."), center_x, y,
             UI_TITLE, ui_rgb(UI_INK), 0.5f);
    y += ui_line_h(app, UI_TITLE) + ui_s(app, 12.0f);
    ui_label(app, 0,
             str_lit("Drop the friend_*.zip the website gave you onto this "
                     "window,"),
             center_x, y, UI_BODY, ui_rgb(UI_TEXT_DIM), 0.5f);
    y += ui_line_h(app, UI_BODY);
    ui_label(app, 0,
             str_lit("or pick one with the button on the left. Everything "
                     "stays on this machine."),
             center_x, y, UI_BODY, ui_rgb(UI_TEXT_DIM), 0.5f);
    y += ui_line_h(app, UI_BODY) + ui_s(app, 28.0f);
    ui_label(app, 0, str_from_cstr(app->shelf.dir), center_x, y, UI_SMALL,
             ui_rgb(UI_TEXT_FAINT), 0.5f);
    if (!app->have_key) {
        y += ui_line_h(app, UI_SMALL) + ui_s(app, 10.0f);
        ui_label(app, 0,
                 str_lit("There is no API key yet, so nothing can answer: "
                         "Settings, top left."),
                 center_x, y, UI_SMALL, ui_rgb(UI_WARN), 0.5f);
    }
}

/* ------------------------------------------------------------------ *
 *  The composer
 * ------------------------------------------------------------------ */

/* It listens, it hears you, it is working it out, it is talking. The
 * ring around the button follows how loud the room is, so that a
 * microphone which is hearing nothing says so before the silence does.
 */
static void ui_voice_button(Ui_App *app, Ui_Rect mic)
{
    Friend_Voice_State state    = friend_voice_state(app->fr);
    b32                usable   = app->have_friend && app->have_key;
    b32                over     = usable && ui_hover(app, mic);
    f32                radius   = mic.w * 0.5f;
    f32                centre_x = mic.x + radius;
    f32                centre_y = mic.y + radius;
    b32                live     = state == FRIEND_VOICE_LISTENING ||
                   state == FRIEND_VOICE_HEARING;

    if (live) {
        f32      level = friend_voice_level(app->fr);
        f32      grown = radius * (1.0f + level * 0.55f);
        Ui_Color ring  = app->color.avatar_bottom;
        ring.a         = (u8)(40.0f + level * 150.0f);
        ui_fill_rounded(&app->draw,
                        ui_rect(centre_x - grown, centre_y - grown,
                                grown * 2.0f, grown * 2.0f),
                        grown, ring);
    }

    ui_fill_rounded(&app->draw, mic, radius,
                    live ? app->color.avatar_bottom
                         : ui_rgb(over ? UI_SURFACE_HOVER : UI_SURFACE));
    ui_icon(app,
            state == FRIEND_VOICE_SPEAKING ? UI_ICON_MIC_OFF : UI_ICON_MIC,
            centre_x, centre_y, 17.0f,
            live ? ui_rgb(UI_WHITE)
                 : ui_rgb(usable ? UI_INK : UI_TEXT_FAINT));
    if (usable && ui_press(app, mic)) ui_voice_toggle(app);
}

/* Photographs waiting to go, in a row above the composer, so that
 * dropping one on the window is visibly a thing that happened. Click one
 * to take it off again. */
static void ui_pending_strip(Ui_App *app, Ui_Rect strip)
{
    f32 tall = ui_s(app, UI_PENDING_HEIGHT);
    f32 gap  = ui_s(app, 8.0f);
    f32 x    = strip.x;
    f32 y    = strip.y + strip.h - gap - tall;
    i32 i;

    for (i = 0; i < app->pending_count; ++i) {
        Ui_Photo *photo = ui_photo_pending(&app->photos, app->pending[i].jpeg);
        f32       wide  = photo && photo->height > 0
                              ? tall * (f32)photo->width / (f32)photo->height
                              : tall;
        Ui_Rect   frame = ui_rect(x, y, wide, tall);

        ui_draw_photo(app, photo, frame, ui_s(app, 6.0f));
        if (ui_press(app, frame)) {
            memmove(&app->pending[i], &app->pending[i + 1],
                    sizeof(app->pending[0]) *
                        (size_t)(app->pending_count - i - 1));
            app->pending_count -= 1;
            break;
        }
        x += wide + gap;
    }
}

/* The site's composer: a white field, the text you are typing, and a
 * round send button that is grey until there is something to send —
 * with a way to a photograph on the left and the microphone beside the
 * send button. */
static void ui_footer(Ui_App *app, Ui_Rect area)
{
    f32     inset   = ui_s(app, 6.0f);
    f32     button  = area.h - inset * 2;
    b32     talking = app->settings.on[UI_SETTING_VOICE];
    b32     can_add = app->have_friend;
    Ui_Rect row     = area;
    Ui_Rect add, send, mic;
    String  placeholder;
    Friend_Voice_State voice = friend_voice_state(app->fr);

    ui_cut_left(&row, inset);
    add = ui_inset(ui_cut_left(&row, button), 0, inset);
    ui_cut_left(&row, ui_s(app, 8.0f));
    ui_cut_right(&row, inset);
    send = ui_inset(ui_cut_right(&row, button), 0, inset);
    mic  = send;
    if (talking) {
        ui_cut_right(&row, inset);
        mic = ui_inset(ui_cut_right(&row, button), 0, inset);
    }
    ui_cut_right(&row, ui_s(app, 14.0f));

    ui_field(app, area, ui_s(app, 10.0f), ui_rgb(UI_WHITE),
             ui_rgb(UI_LINE_STRONG));

    placeholder =
        !app->have_friend
            ? str_lit("Import a friend, or make one, to start talking.")
        : !app->have_key
            ? str_lit("No key yet \342\200\224 Settings, top left.")
        : app->awaiting ? str_lit("thinking...")
        : voice == FRIEND_VOICE_HEARING ? str_lit("listening...")
        : voice == FRIEND_VOICE_LISTENING
            ? str_lit("Say something, out loud or here.")
            : str_lit("Say something.");
    ui_text_field(app, UI_FOCUS_COMPOSER, &app->composer, row, row, UI_BODY,
                  placeholder, 0);

    ui_fill_rounded(&app->draw, add, button * 0.5f,
                    ui_rgb(can_add && ui_hover(app, add) ? UI_SURFACE_HOVER
                                                         : UI_SURFACE));
    ui_icon(app, UI_ICON_PLUS, add.x + add.w * 0.5f, add.y + add.h * 0.5f,
            17.0f, ui_rgb(can_add ? UI_INK : UI_TEXT_FAINT));
    if (can_add && ui_press(app, add)) ui_pick_file(app, &ui_pick_picture);

    if (talking) ui_voice_button(app, mic);

    ui_fill_rounded(&app->draw, send, button * 0.5f,
                    ui_rgb(ui_can_send(app) ? UI_BLACK : UI_TEXT_FAINT));
    ui_icon(app, UI_ICON_ARROW_RIGHT, send.x + send.w * 0.5f,
            send.y + send.h * 0.5f, 17.0f, ui_rgb(UI_WHITE));
    if (ui_can_send(app) && ui_press(app, send)) ui_send(app);
}

/* ------------------------------------------------------------------ *
 *  The frame
 * ------------------------------------------------------------------ */

/* The wordmark is a picture scaled to the size it is drawn at, so a
 * window that moves to a display of another scale needs another one. */
static void ui_logo_refresh(Ui_App *app)
{
    if (app->logo_scale == app->scale || app->logo_path[0] == 0) return;
    if (app->logo) SDL_DestroyTexture(app->logo);
    app->logo       = 0;
    app->logo_w     = 0;
    app->logo_h     = 0;
    app->logo_scale = app->scale;
    app->logo = ui_picture_at_height(&app->photos, app->logo_path,
                                     (i32)(UI_LOGO * app->scale + 0.5f),
                                     &app->logo_w, &app->logo_h);
}

/* The wheel moves whatever is on top. */
static void ui_scroll_by(Ui_App *app, f32 by)
{
    if (app->settings_open) {
        app->settings_scroll += by;
    } else {
        app->scroll += by;
        app->stick_to_end = 0;
    }
}

static void ui_frame(Ui_App *app)
{
    Ui_Rect  pane, column, footer, strip;
    f32      sidebar_w;
    f32      pad = 0;
    Ui_Color wash;

    app->scale       = app->in.scale > 0 ? app->in.scale : 1.0f;
    app->top_layer   = app->settings_open ? UI_LAYER_SHEET : UI_LAYER_WINDOW;
    app->layer       = UI_LAYER_WINDOW;
    app->press_taken = 0;
    if (!app->in.down) app->held = UI_HELD_NONE;
    if (app->in.wheel != 0.0f) {
        ui_scroll_by(app, -app->in.wheel * ui_s(app, UI_SCROLL_WHEEL));
    }

    wash = app->have_friend ? app->color.wash : ui_rgb(UI_NO_WASH);
    SDL_SetRenderDrawColor(app->renderer, wash.r, wash.g, wash.b, 255);
    SDL_RenderClear(app->renderer);
    app->photos.frame += 1;
    ui_logo_refresh(app);

    sidebar_w = ui_s(app, UI_SIDEBAR_WIDTH);
    pane      = ui_rect(sidebar_w, 0, app->in.width - sidebar_w,
                        app->in.height);
    pad       = ui_s(app, 24.0f);

    /* The composer at the bottom, the photographs waiting to be sent
     * above it — the conversation gives up the room for them rather
     * than having them drawn over it — and the conversation in what is
     * left, with the scrollbar's gutter down its right. */
    column = ui_inset(pane, pad, 0);
    ui_cut_bottom(&column, pad);
    footer = ui_cut_bottom(&column, ui_s(app, 44.0f));
    strip  = ui_cut_bottom(&column, app->pending_count > 0
                                        ? ui_s(app, UI_PENDING_HEIGHT + 10.0f)
                                        : 0.0f);
    ui_cut_bottom(&column, pad * 0.5f);

    if (app->have_friend) {
        Ui_Header_Box header = ui_header_layout(app, pane);
        Ui_Rect       gutter;
        Ui_Rect       visible;
        ui_cut_top(&column, header.area.h);
        gutter = ui_cut_right(&column, ui_s(app, UI_GUTTER));
        /* Up behind the header and down behind the composer. */
        visible = ui_rect(pane.x, pane.y, pane.w,
                          column.y + column.h - pane.y);
        ui_transcript(app, column, visible, gutter);
        ui_header(app, &header);
    } else {
        ui_empty_state(app, pane);
    }
    ui_pending_strip(app, strip);
    ui_footer(app, footer);
    ui_sidebar(app, ui_rect(0, 0, sidebar_w, app->in.height));

    if (app->settings_open) {
        app->layer = UI_LAYER_SHEET;
        ui_settings_panel(app);
    }

    ui_text_flush_atlas(&app->text);
    SDL_RenderPresent(app->renderer);
}

/* ------------------------------------------------------------------ *
 *  The keyboard
 * ------------------------------------------------------------------ */

static void ui_key_quit(Ui_App *app, SDL_Keymod mod)
{
    fr_unused(mod);
    app->quit = 1;
}

static void ui_key_enter(Ui_App *app, SDL_Keymod mod)
{
    fr_unused(mod);
    if (app->settings_open) ui_settings_apply(app);
    else if (app->naming) ui_naming_commit(app);
    else ui_send(app);
}

static void ui_key_escape(Ui_App *app, SDL_Keymod mod)
{
    fr_unused(mod);
    if (app->settings_open) ui_settings_cancel(app);
    else ui_naming_end(app);
}

static void ui_key_tab(Ui_App *app, SDL_Keymod mod)
{
    ui_settings_tab(app, (mod & SDL_KMOD_SHIFT) ? -1 : 1);
}

static void ui_key_settings(Ui_App *app, SDL_Keymod mod)
{
    fr_unused(mod);
    ui_settings_begin(app);
}

/* The microphone, without reaching for the mouse. The pendant had a
 * button on it; this is the nearest thing a window has. */
static void ui_key_voice(Ui_App *app, SDL_Keymod mod)
{
    fr_unused(mod);
    if (app->settings.on[UI_SETTING_VOICE]) ui_voice_toggle(app);
}

static f32 ui_page(Ui_App *app)
{
    return app->in.height *
           (app->settings_open ? UI_PAGE_SHEET : UI_PAGE_TRANSCRIPT);
}

static void ui_key_page_up(Ui_App *app, SDL_Keymod mod)
{
    fr_unused(mod);
    ui_scroll_by(app, -ui_page(app));
}

static void ui_key_page_down(Ui_App *app, SDL_Keymod mod)
{
    fr_unused(mod);
    ui_scroll_by(app, ui_page(app));
}

static void ui_key_up(Ui_App *app, SDL_Keymod mod)
{
    fr_unused(mod);
    ui_scroll_by(app, -ui_s(app, UI_SCROLL_LINE));
}

static void ui_key_down(Ui_App *app, SDL_Keymod mod)
{
    fr_unused(mod);
    ui_scroll_by(app, ui_s(app, UI_SCROLL_LINE));
}

/* Home and End reach the transcript only when there is nothing being
 * typed for them to belong to; the field has had first refusal. */
static void ui_key_home(Ui_App *app, SDL_Keymod mod)
{
    fr_unused(mod);
    app->scroll       = 0;
    app->stick_to_end = 0;
}

static void ui_key_end(Ui_App *app, SDL_Keymod mod)
{
    fr_unused(mod);
    app->stick_to_end = 1;
}

/* Where a binding applies: anywhere, only with the sheet closed, or only
 * with it open. */
enum { UI_KEYS_ANYWHERE, UI_KEYS_WINDOW, UI_KEYS_SHEET };

typedef struct Ui_Key_Binding {
    SDL_Keycode key;
    SDL_Keymod  needs; /* one of these held, or 0 for none needed */
    u8          where;
    void (*act)(Ui_App *app, SDL_Keymod mod);
} Ui_Key_Binding;

#define UI_COMMAND (SDL_KMOD_CTRL | SDL_KMOD_GUI)

static const Ui_Key_Binding ui_keys[] = {
    { SDLK_Q, SDL_KMOD_CTRL, UI_KEYS_ANYWHERE, ui_key_quit },
    { SDLK_RETURN, 0, UI_KEYS_ANYWHERE, ui_key_enter },
    { SDLK_KP_ENTER, 0, UI_KEYS_ANYWHERE, ui_key_enter },
    { SDLK_ESCAPE, 0, UI_KEYS_ANYWHERE, ui_key_escape },
    { SDLK_PAGEUP, 0, UI_KEYS_ANYWHERE, ui_key_page_up },
    { SDLK_PAGEDOWN, 0, UI_KEYS_ANYWHERE, ui_key_page_down },
    { SDLK_UP, 0, UI_KEYS_ANYWHERE, ui_key_up },
    { SDLK_DOWN, 0, UI_KEYS_ANYWHERE, ui_key_down },
    { SDLK_TAB, 0, UI_KEYS_SHEET, ui_key_tab },
    /* The shortcut every other program on the machine uses for this. */
    { SDLK_COMMA, UI_COMMAND, UI_KEYS_WINDOW, ui_key_settings },
    { SDLK_M, UI_COMMAND, UI_KEYS_WINDOW, ui_key_voice },
    { SDLK_HOME, 0, UI_KEYS_WINDOW, ui_key_home },
    { SDLK_END, 0, UI_KEYS_WINDOW, ui_key_end }
};

/* Whatever has the keyboard gets first refusal: carets, selections, word
 * motion, cut and paste are the same wherever text is being typed. What
 * is left over is the window's, or the sheet's. */
static void ui_app_key(Ui_App *app, SDL_Keycode key, SDL_Keymod mod)
{
    Ui_Text_Edit *edit  = ui_focused_edit(app);
    u8            where = app->settings_open ? UI_KEYS_SHEET : UI_KEYS_WINDOW;
    i32           i;

    if (edit && ui_edit_key(edit, key, mod)) {
        ui_blink_restart(app);
        return;
    }
    for (i = 0; i < fr_array_count(ui_keys); ++i) {
        const Ui_Key_Binding *b = &ui_keys[i];
        if (b->key != key) continue;
        if (b->needs && !(mod & b->needs)) continue;
        if (b->where != UI_KEYS_ANYWHERE && b->where != where) continue;
        b->act(app, mod);
        return;
    }
}

/* Typing, which is also the end of whatever went wrong last. */
static void ui_app_text(Ui_App *app, String text)
{
    Ui_Text_Edit *edit = ui_focused_edit(app);
    if (edit) ui_edit_insert(edit, text);
    ui_blink_restart(app);
    app->trouble.len = 0;
}

/* ------------------------------------------------------------------ *
 *  The friend's side: starting, pumping, stopping
 * ------------------------------------------------------------------ */

/* Once a frame: the friend's work done or handed to the network, the
 * microphone heard, the friend advanced, and what happened read off.
 * The window keeps drawing throughout, which is the whole point of the
 * friend never blocking. */
static void ui_pump(Ui_App *app)
{
    Friend_Event event;

    /* Zero wait: the frame is the clock. */
    fr_pump_io(app->fr, app->net, 0);
    /* The microphone before the tick, so what was said this frame is
     * part of this frame's turn rather than the next one's. */
    ui_audio_hear(&app->audio, app->fr);
    friend_tick(app->fr, fr_now_unix_ms());

    while (friend_next_event(app->fr, &event)) {
        switch (event.kind) {
        case FRIEND_EVENT_REPLY_DONE:
            app->awaiting = 0;
            break;
        case FRIEND_EVENT_REPLY_RETRACTED:
            app->awaiting = 0;
            /* It stopped mid-sentence, so the speaker stops too: the
             * rest of what it was saying has been taken back. */
            ui_audio_hush(&app->audio);
            break;
        case FRIEND_EVENT_ERROR:
            app->awaiting = 0;
            ui_trouble(app, event.text);
            break;
        case FRIEND_EVENT_THINKING:
            /* However the thing it is answering arrived. */
            app->awaiting = 1;
            break;
        case FRIEND_EVENT_AUDIO_OUT:
            ui_audio_play(&app->audio, event.samples, event.sample_count,
                          event.sample_rate);
            break;
        default:
            break;
        }
    }

    if (!fr_shelf_save_when_quiet(&app->shelf, app->fr)) {
        ui_shelf_trouble(app);
    }
}

/* Everything between one frame and the next that is not drawing. */
static void ui_app_step(Ui_App *app)
{
    ui_take_dropped(app);
    ui_take_export(app);
    ui_pump(app);
}

/* The settings, the friend they configure, the network, and the friend
 * most recently written to — so the window opens on the conversation
 * somebody was last having. The friend's block is reserved in
 * friend_space; the network's comes out of net_arena. */
static b32 ui_app_start(Ui_App *app, Fr_Virtual *friend_space,
                        Arena *net_arena)
{
    Friend_Config config = friend_default_config();
    size_t        net_bytes;
    void         *net_memory;
    char          dir[FR_PATH_MAX];
    char          why[256];

    app->composer = ui_edit_make(app->composer_bytes, UI_INPUT_MAX, 0);
    app->name     = ui_edit_make(app->name_bytes, FRIEND_NAME_MAX,
                                 UI_EDIT_TRIM_PASTE);
    app->focus    = UI_FOCUS_COMPOSER;
    app->scale    = 1.0f;
    app->stick_to_end = 1;

    app->env = fr_env_read();
    if (app->env.directory) {
        SDL_snprintf(dir, sizeof(dir), "%s", app->env.directory);
    } else {
        /* Where the platform says user data goes. SDL hands back heap
         * memory, which is copied out of and given straight back. */
        char *pref = SDL_GetPrefPath("friend-local", "friends");
        if (!pref) {
            SDL_Log("no folder for friends: %s", SDL_GetError());
            return 0;
        }
        SDL_snprintf(dir, sizeof(dir), "%s", pref);
        SDL_free(pref);
    }
    if (!fr_shelf_begin(&app->shelf, dir)) {
        SDL_Log("cannot keep friends in %s: %s", dir,
                fr_shelf_error(&app->shelf));
        return 0;
    }
    if (fr_shelf_error(&app->shelf)[0]) ui_shelf_trouble(app);
    SDL_Log("friends: %s, %d of them", app->shelf.dir, app->shelf.count);

    /* The settings file, and without one the environment's choice of
     * provider. */
    SDL_snprintf(app->settings_path, sizeof(app->settings_path),
                 "%s/settings.json", app->shelf.dir);
    ui_settings_defaults(&app->settings);
    ui_settings_defaults(&app->settings_applied);
    if (!ui_settings_load(&app->settings, app->settings_path, &app->frame)) {
        app->settings.provider = (i32)app->env.provider;
    }
    ui_settings_into_config(&app->settings, &app->env, &config);
    config.friend_name        = str_lit("friend");
    config.utc_offset_minutes = fr_local_utc_offset_minutes();
    app->utc_offset_minutes   = config.utc_offset_minutes;
    app->have_key             = config.api_key.len > 0;
    if (!app->have_key) {
        SDL_Log("no key: set GEMINI_KEY or OPENAI_KEY, or type one into "
                "Settings");
    }

    app->fr = fr_virtual_friend(friend_space, &config, why, sizeof(why));
    if (!app->fr) {
        SDL_Log("%s", why);
        return 0;
    }
    SDL_Log("friend: %.1f MiB reserved",
            (double)friend_space->reserved / (1024.0 * 1024.0));
    app->config = config;

    net_bytes  = fr_net_size(FR_NET_CONNECTIONS);
    net_memory = arena_push_size(net_arena, net_bytes, 16);
    app->net   = net_memory ? fr_net_begin(net_memory, net_bytes,
                                           FR_NET_CONNECTIONS,
                                           app->env.ca_bundle, why,
                                           sizeof(why))
                            : 0;
    if (!app->net) {
        /* No transport means no replies, which is worth saying once and
         * then carrying on: reading a friend still works, and so does
         * writing it. */
        SDL_Log("no transport: %s",
                net_memory ? why : "the window's budget has no room for it");
    }
    if (app->shelf.count > 0) ui_open_friend(app, 0);
    else ui_adopt_friend(app);
    return 1;
}

static void ui_app_end(Ui_App *app)
{
    if (!fr_shelf_save(&app->shelf, app->fr, app->net)) {
        SDL_Log("%s", fr_shelf_error(&app->shelf));
    }
    ui_audio_end(&app->audio);
    fr_net_end(app->net);
    fr_shelf_end(&app->shelf);
}
