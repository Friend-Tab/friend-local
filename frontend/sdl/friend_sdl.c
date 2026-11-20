/* friend_sdl.c — the supplied frontend, as one translation unit.
 *
 * It owns a window and the allocations: the friend's block, which the
 * library carves up and grows into, and its own, which everything here
 * is carved out of. After startup neither asks for more.
 *
 * The friend is reached only through friend.h. Nothing in this file knows
 * how a memory is stored or what a prompt looks like, which is the point
 * of the split: replace this file entirely and the friend is the same
 * friend.
 */

#include <stdarg.h>

#include <SDL3/SDL.h>

#include "ui_base.h"
#include "ui_icons.h"

/* What the library deliberately does not have — sockets and TLS, the
 * clock, address space, files, a folder of friends, the environment —
 * supplied beside it in platform/. */
#include "fr_clock.h"
#include "fr_disk.h"
#include "fr_export.h"
#include "fr_env.h"
#include "fr_net.h"
#include "fr_pump.h"
#include "fr_shelf.h"
#include "fr_virtual.h"

/* Declarations only: the implementations are in ui_vendor.c, so their
 * warnings stay out of ours. kb_text_shape uses anonymous unions, which
 * C11 has and C99 does not; that is a thing to know about the header, not
 * a thing to fix in it. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "../../vendor/stb_truetype.h"
#include "../../vendor/kb_text_shape.h"
#define STBI_NO_STDIO
#include "../../vendor/stb_image.h"
#define STBI_WRITE_NO_STDIO
#include "../../vendor/stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* The base layer, compiled in here as well as into the library: the
 * window uses the same strings, arenas and JSON reader, but its own copy
 * of them, not the library's. */
#include "../../base/fr_base.c"
#include "../../base/fr_json.c"

#include "ui_text.c"
#include "ui_draw.c"
#include "ui_photo.c"
#include "ui_edit.c"
#include "ui_settings.c"
#include "ui_audio.c"
#include "ui_app.c"

/* ------------------------------------------------------------------ *
 *  The frontend's own budget
 *
 *  Stated here in one place, for the same reason the library states
 *  its: a program that can tell you what it needs before it starts is
 *  one you can reason about.
 * ------------------------------------------------------------------ */

#define UI_TEXT_BYTES FR_MB(32)    /* font files, glyph atlas             */
#define UI_MISC_BYTES FR_MB(1)     /* the rounded-corner disc             */
#define UI_PHOTO_BYTES FR_MB(96)   /* one photograph, decoded             */
#define UI_OUTGOING_BYTES FR_MB(8) /* photographs waiting to be sent      */
#define UI_FRAME_BYTES FR_MB(4)    /* what one frame lays out and says    */
#define UI_NET_BYTES FR_KB(768)    /* connections, and a request head each */

#define UI_BLOCK_BYTES                                                        \
    (UI_SHAPE_BYTES + UI_TEXT_BYTES + UI_MISC_BYTES + UI_PHOTO_BYTES +        \
     UI_OUTGOING_BYTES + UI_FRAME_BYTES + UI_NET_BYTES + FR_KB(64))

static Ui_App app;

/* The site sets its chrome in ABC Favorit and the friend's name in
 * Miller Text. Both are licensed faces we cannot ship, so the shapes are
 * matched rather than the files: a grotesque for everything and a serif
 * for the name. Point FRIEND_LOCAL_FONT and FRIEND_LOCAL_SERIF at the real
 * ones if you have them. */
static String ui_find_file(const char *from_env, const char *const *candidates,
                           i32 count)
{
    const char  *named = SDL_getenv(from_env);
    const char  *base;
    static char  guess[1024];
    i32          i;

    if (named && named[0]) return str_from_cstr(named);
    for (i = 0; i < count; ++i) {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(candidates[i], &info)) {
            return str_from_cstr(candidates[i]);
        }
    }
    /* And beside the executable, for a build run from anywhere at all:
     * in the folder it is in, which is how a release unpacks (and, in a
     * Mac app, SDL says the bundle's Resources), or one up, for build/.
     * The result is a C string either way, which is the one place in
     * this program that matters, because it goes to fopen. Only our own
     * files are looked for this way; a system path stays absolute. */
    base = SDL_GetBasePath();
    if (base) {
        for (i = 0; i < count; ++i) {
            SDL_PathInfo info;
            if (candidates[i][0] == '/' || candidates[i][1] == ':') continue;
            SDL_snprintf(guess, sizeof(guess), "%s%s", base, candidates[i]);
            if (SDL_GetPathInfo(guess, &info)) return str_from_cstr(guess);
            SDL_snprintf(guess, sizeof(guess), "%s../%s", base,
                         candidates[i]);
            if (SDL_GetPathInfo(guess, &info)) return str_from_cstr(guess);
        }
    }
    return str_nil;
}

static const char *const ui_sans_candidates[] = {
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",           /* Arch      */
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",  /* Debian    */
    "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",    /* Fedora    */
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",      /* Ubuntu    */
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",    /* Fedora    */
    "/System/Library/Fonts/Helvetica.ttc",                  /* macOS     */
    "C:/Windows/Fonts/segoeui.ttf"
};

/* The icon face ships with us rather than coming from the system, so it
 * is looked for next to the binary and in the tree it was built from.
 * Without it the window still works; it simply has no pictures in it. */
static const char *const ui_icon_candidates[] = {
    "vendor/Phosphor.ttf",
    "../vendor/Phosphor.ttf",
    "../../vendor/Phosphor.ttf"
};

/* The wordmark. Same arrangement: ours, beside us, and the window
 * still works without it. */
static const char *const ui_logo_candidates[] = {
    "assets/friend-wordmark.png",
    "../assets/friend-wordmark.png",
    "../../assets/friend-wordmark.png"
};

static const char *const ui_serif_candidates[] = {
    "/usr/share/fonts/noto/NotoSerif-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSerif-Regular.ttf",
    "/usr/share/fonts/google-noto/NotoSerif-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationSerif-Regular.ttf",
    "/usr/share/fonts/TTF/LiberationSerif-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "/System/Library/Fonts/Supplemental/Georgia.ttf",       /* macOS 10.15+ */
    "/Library/Fonts/Georgia.ttf",
    "C:/Windows/Fonts/georgia.ttf"
};

/* What happened since the last frame, handed to the window. */
static void ui_read_input(Ui_App *self)
{
    SDL_Event event;
    Ui_Input *in      = &self->in;
    f32       mouse_x = 0, mouse_y = 0;
    i32       out_w = 0, out_h = 0, win_w = 0, win_h = 0;
    f32       render_scale;

    in->pressed = 0;
    in->wheel   = 0;

    SDL_GetRenderOutputSize(self->renderer, &out_w, &out_h);
    SDL_GetWindowSize(self->window, &win_w, &win_h);
    in->width    = (f32)out_w;
    in->height   = (f32)out_h;
    render_scale = win_w > 0 ? (f32)out_w / (f32)win_w : 1.0f;
    in->scale    = SDL_GetWindowDisplayScale(self->window);
    if (in->scale <= 0.0f) in->scale = render_scale;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT: self->quit = 1; break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT) in->pressed = 1;
            break;
        case SDL_EVENT_MOUSE_WHEEL: in->wheel += event.wheel.y; break;
        case SDL_EVENT_DROP_FILE: ui_app_drop(self, event.drop.data); break;
        case SDL_EVENT_TEXT_INPUT:
            if (event.text.text) {
                ui_app_text(self, str_from_cstr(event.text.text));
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            ui_app_key(self, event.key.key, event.key.mod);
            break;
        default: break;
        }
    }

    /* The button mask rather than the events: a press that began before
     * the window had focus still has to count as held, or a drag gets
     * stuck on. */
    in->down    = (SDL_GetMouseState(&mouse_x, &mouse_y) &
                   SDL_BUTTON_LMASK) != 0;
    in->mouse_x = mouse_x * render_scale;
    in->mouse_y = mouse_y * render_scale;
}

int main(int argc, char **argv)
{
    Arena      block;
    Arena      shape_arena;
    Arena      text_arena;
    Arena      misc_arena;
    Arena      photo_arena;
    Arena      net_arena;
    Fr_Virtual ui_reserved;
    Fr_Virtual friend_reserved;
    String     font_path;
    String     serif_path;

    memset(&app, 0, sizeof(app));

    /* Reserved rather than allocated: most of this is room for a
     * photograph the size of a phone's camera, which most sessions
     * never ask for. The friend's block is reserved the same way, by
     * ui_app_start, sized for the friend somebody might have in ten
     * years. */
    if (!fr_virtual_reserve(&ui_reserved, UI_BLOCK_BYTES)) {
        SDL_Log("cannot reserve %.0f MiB for the window",
                (double)UI_BLOCK_BYTES / (1024.0 * 1024.0));
        return 1;
    }
    SDL_Log("window: %.1f MiB reserved",
            (double)UI_BLOCK_BYTES / (1024.0 * 1024.0));
    block        = arena_make_reserved(ui_reserved.base, UI_BLOCK_BYTES,
                                       fr_virtual_commit, 0);
    shape_arena  = arena_sub(&block, UI_SHAPE_BYTES, 16);
    text_arena   = arena_sub(&block, UI_TEXT_BYTES, 16);
    misc_arena   = arena_sub(&block, UI_MISC_BYTES, 16);
    photo_arena  = arena_sub(&block, UI_PHOTO_BYTES, 16);
    app.outgoing = arena_sub(&block, UI_OUTGOING_BYTES, 16);
    app.frame    = arena_sub(&block, UI_FRAME_BYTES, 16);
    net_arena    = arena_sub(&block, UI_NET_BYTES, 16);

    /* Audio as well as video: the microphone and the speaker are the
     * other half of talking to it. */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    if (!SDL_CreateWindowAndRenderer(
            "friend-local", 1180, 820,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &app.window,
            &app.renderer)) {
        SDL_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderDrawBlendMode(app.renderer, SDL_BLENDMODE_BLEND);
    ui_photos_init(&app.photos, app.renderer, &photo_arena);
    /* The frame is also the pump's clock, so there is no reason to draw
     * faster than the display. */
    SDL_SetRenderVSync(app.renderer, 1);
    /* Some field always has the keyboard — the composer, when nothing
     * else does — so text input is on for good. */
    SDL_StartTextInput(app.window);

    if (!ui_text_init(&app.text, app.renderer, &text_arena, &shape_arena)) {
        SDL_Log("cannot set up text");
        return 1;
    }
    font_path = ui_find_file("FRIEND_LOCAL_FONT", ui_sans_candidates,
                             fr_array_count(ui_sans_candidates));
    if (font_path.len <= 0 ||
        ui_text_add_font(&app.text, &text_arena, font_path, 1) != 0) {
        SDL_Log("no usable sans font; set FRIEND_LOCAL_FONT to a .ttf");
        return 1;
    }
    /* The serif is a nicety: without one the name is simply set in the
     * same face as everything else. */
    serif_path = ui_find_file("FRIEND_LOCAL_SERIF", ui_serif_candidates,
                              fr_array_count(ui_serif_candidates));
    if (serif_path.len > 0) {
        i32 index = ui_text_add_font(&app.text, &text_arena, serif_path, 1);
        if (index > 0) app.serif = index;
    }
    {
        /* One at a time: a path found beside the executable is in a
         * buffer the next lookup writes over, so each is used before the
         * next is asked for. */
        String icons = ui_find_file("FRIEND_LOCAL_ICONS", ui_icon_candidates,
                                    fr_array_count(ui_icon_candidates));
        String logo;
        if (icons.len > 0) {
            i32 index = ui_text_add_font(&app.text, &text_arena, icons, 0);
            if (index > 0) app.icons = index;
        }
        if (app.icons == 0) SDL_Log("no icon font found; running without");
        logo = ui_find_file("FRIEND_LOCAL_LOGO", ui_logo_candidates,
                            fr_array_count(ui_logo_candidates));
        /* Kept rather than loaded: the first frame knows what scale
         * the display is, and builds it for that. */
        if (logo.len > 0 && logo.len < (i32)sizeof(app.logo_path)) {
            memcpy(app.logo_path, logo.data, (size_t)logo.len);
            app.logo_path[logo.len] = 0;
        }
    }
    if (!ui_draw_init(&app.draw, app.renderer, &misc_arena)) {
        SDL_Log("cannot build the corner texture");
        return 1;
    }
    if (!ui_app_start(&app, &friend_reserved, &net_arena)) return 1;

    /* A path on the command line is the same as one dropped on the
     * window — an export to import, or a photograph to send — which
     * saves a user who already has the file a trip. */
    if (argc > 1) ui_app_drop(&app, argv[1]);

    while (!app.quit) {
        arena_reset(&app.frame);
        ui_read_input(&app);
        ui_app_step(&app);
        ui_frame(&app);
    }

    ui_app_end(&app);
    fr_virtual_release(&friend_reserved);
    fr_virtual_release(&ui_reserved);
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
