/* ui_edit.c — a line of text somebody is typing.
 *
 * The bytes, a caret, and the other end of a selection, all byte offsets
 * that only ever land on code point boundaries: a caret inside a
 * character would draw in the wrong place and delete half of one. Every
 * place text is typed in this window — the composer, a friend's name,
 * each field of the settings sheet — is one of these over a buffer of
 * its own, and the keyboard does the same things to all of them.
 */

enum {
    UI_EDIT_DIGITS     = 1u << 0, /* takes 0-9 and nothing else          */
    UI_EDIT_TRIM_PASTE = 1u << 1  /* a pasted key or name loses the line
                                     ending whatever copied it put there */
};

typedef struct Ui_Text_Edit {
    char *buf;
    i32   len;
    i32   max;
    i32   caret;
    i32   anchor; /* the other end of the selection; == caret for none */
    u32   flags;
} Ui_Text_Edit;

static Ui_Text_Edit ui_edit_make(char *buf, i32 max, u32 flags)
{
    Ui_Text_Edit e;
    memset(&e, 0, sizeof(e));
    e.buf   = buf;
    e.max   = max;
    e.flags = flags;
    return e;
}

static String ui_edit_text(const Ui_Text_Edit *e)
{
    return str_make(e->buf, e->len);
}

static i32 ui_edit_low(const Ui_Text_Edit *e)
{
    return e->caret < e->anchor ? e->caret : e->anchor;
}

static i32 ui_edit_high(const Ui_Text_Edit *e)
{
    return e->caret > e->anchor ? e->caret : e->anchor;
}

/* The caret goes to `to`; the anchor follows it unless the selection is
 * being extended. */
static void ui_edit_go(Ui_Text_Edit *e, i32 to, b32 extend)
{
    to       = fr_clamp(0, to, e->len);
    e->caret = to;
    if (!extend) e->anchor = to;
}

/* Everything replaced, the caret at the end. */
static void ui_edit_set(Ui_Text_Edit *e, String s)
{
    i32 take = fr_truncate_utf8(s, s.len < e->max ? s.len : e->max).len;
    if (take > 0) memmove(e->buf, s.data, (size_t)take);
    e->len = take;
    ui_edit_go(e, take, 0);
}

static void ui_edit_clear(Ui_Text_Edit *e) { ui_edit_set(e, str_nil); }

/* ------------------------------------------------------------------ *
 *  Code points
 * ------------------------------------------------------------------ */

static b32 ui_utf8_continues(char c) { return ((u8)c & 0xC0u) == 0x80u; }

static i32 ui_step_left(const char *text, i32 at)
{
    if (at <= 0) return 0;
    --at;
    while (at > 0 && ui_utf8_continues(text[at])) --at;
    return at;
}

static i32 ui_step_right(const char *text, i32 len, i32 at)
{
    if (at >= len) return len;
    ++at;
    while (at < len && ui_utf8_continues(text[at])) ++at;
    return at;
}

/* How many code points start before byte `at`. */
static i32 ui_utf8_index(String s, i32 at)
{
    i32 i, n = 0;
    for (i = 0; i < at && i < s.len; ++i) {
        if (!ui_utf8_continues(s.data[i])) ++n;
    }
    return n;
}

/* The byte the code point numbered `index` starts at. */
static i32 ui_utf8_offset(String s, i32 index)
{
    i32 at = 0;
    while (index-- > 0 && at < s.len) at = ui_step_right(s.data, s.len, at);
    return at;
}

/* What ctrl-arrow and ctrl-backspace step over. Anything that is not a
 * space or punctuation counts, bytes over 0x7F included, so a word in a
 * language this program has never heard of is still a word. */
static b32 ui_is_word_byte(char c)
{
    u8 b = (u8)c;
    if (b >= 0x80u) return 1;
    if (b >= '0' && b <= '9') return 1;
    if (b >= 'A' && b <= 'Z') return 1;
    if (b >= 'a' && b <= 'z') return 1;
    return b == '_' || b == '\'';
}

static i32 ui_word_left(const char *text, i32 at)
{
    while (at > 0 && !ui_is_word_byte(text[at - 1])) --at;
    while (at > 0 && ui_is_word_byte(text[at - 1])) --at;
    return at;
}

static i32 ui_word_right(const char *text, i32 len, i32 at)
{
    while (at < len && !ui_is_word_byte(text[at])) ++at;
    while (at < len && ui_is_word_byte(text[at])) ++at;
    return at;
}

/* ------------------------------------------------------------------ *
 *  Changing it
 * ------------------------------------------------------------------ */

static void ui_edit_cut_range(Ui_Text_Edit *e, i32 from, i32 to)
{
    if (to <= from) return;
    memmove(e->buf + from, e->buf + to, (size_t)(e->len - to));
    e->len -= to - from;
    ui_edit_go(e, from, 0);
}

static b32 ui_edit_drop_selection(Ui_Text_Edit *e)
{
    if (e->caret == e->anchor) return 0;
    ui_edit_cut_range(e, ui_edit_low(e), ui_edit_high(e));
    return 1;
}

static void ui_edit_insert_bytes(Ui_Text_Edit *e, String text)
{
    i32 room, take;

    ui_edit_drop_selection(e);
    room = e->max - e->len;
    take = fr_truncate_utf8(text, text.len < room ? text.len : room).len;
    if (take <= 0) return;
    memmove(e->buf + e->caret + take, e->buf + e->caret,
            (size_t)(e->len - e->caret));
    memcpy(e->buf + e->caret, text.data, (size_t)take);
    e->len += take;
    ui_edit_go(e, e->caret + take, 0);
}

/* Typed or pasted. A digits-only field keeps the digits, so that a field
 * somebody typed a word into cannot quietly mean "the default". */
static void ui_edit_insert(Ui_Text_Edit *e, String text)
{
    i32 i;
    if (!(e->flags & UI_EDIT_DIGITS)) {
        ui_edit_insert_bytes(e, text);
        return;
    }
    for (i = 0; i < text.len; ++i) {
        if (text.data[i] >= '0' && text.data[i] <= '9') {
            ui_edit_insert_bytes(e, str_make(text.data + i, 1));
        }
    }
}

/* The selection on the clipboard. An empty selection copies nothing
 * rather than everything. */
static void ui_edit_copy(Ui_Text_Edit *e, b32 and_cut)
{
    char out[1024];
    i32  lo   = ui_edit_low(e);
    i32  take = ui_edit_high(e) - lo;

    if (take <= 0) return;
    take = fr_truncate_utf8(str_make(e->buf + lo, take),
                            (i32)sizeof(out) - 1).len;
    memcpy(out, e->buf + lo, (size_t)take);
    out[take] = 0;
    SDL_SetClipboardText(out);
    if (and_cut) ui_edit_cut_range(e, lo, lo + take);
}

static void ui_edit_paste(Ui_Text_Edit *e)
{
    char  *clip = SDL_GetClipboardText();
    String text;
    if (!clip) return;
    text = str_from_cstr(clip);
    if (e->flags & UI_EDIT_TRIM_PASTE) text = str_trim(text);
    ui_edit_insert(e, text);
    SDL_free(clip);
}

/* Everything the keyboard does to a line of text. Returns whether the
 * key was taken: whoever asked still owns the ones it was not. */
static b32 ui_edit_key(Ui_Text_Edit *e, SDL_Keycode key, SDL_Keymod mod)
{
    b32 word    = (mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) != 0;
    b32 command = (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    b32 extend  = (mod & SDL_KMOD_SHIFT) != 0;
    b32 chosen  = e->caret != e->anchor;

    e->caret  = fr_clamp(0, e->caret, e->len);
    e->anchor = fr_clamp(0, e->anchor, e->len);

    switch (key) {
    case SDLK_LEFT:
        ui_edit_go(e,
                   word ? ui_word_left(e->buf, e->caret)
                        : (!extend && chosen ? ui_edit_low(e)
                                             : ui_step_left(e->buf, e->caret)),
                   extend);
        return 1;
    case SDLK_RIGHT:
        ui_edit_go(e,
                   word ? ui_word_right(e->buf, e->len, e->caret)
                        : (!extend && chosen
                               ? ui_edit_high(e)
                               : ui_step_right(e->buf, e->len, e->caret)),
                   extend);
        return 1;
    case SDLK_BACKSPACE:
        if (ui_edit_drop_selection(e)) return 1;
        ui_edit_cut_range(e,
                          word ? ui_word_left(e->buf, e->caret)
                               : ui_step_left(e->buf, e->caret),
                          e->caret);
        return 1;
    case SDLK_DELETE:
        if (ui_edit_drop_selection(e)) return 1;
        ui_edit_cut_range(e, e->caret,
                          word ? ui_word_right(e->buf, e->len, e->caret)
                               : ui_step_right(e->buf, e->len, e->caret));
        return 1;
    case SDLK_A:
        if (!command) return 0;
        e->anchor = 0;
        ui_edit_go(e, e->len, 1);
        return 1;
    case SDLK_C:
        if (!command) return 0;
        ui_edit_copy(e, 0);
        return 1;
    case SDLK_X:
        if (!command) return 0;
        ui_edit_copy(e, 1);
        return 1;
    case SDLK_V:
        if (!command) return 0;
        ui_edit_paste(e);
        return 1;
    case SDLK_HOME:
        /* Home and End belong to the line when there is something in it
         * to go to the start of, and to whatever is around it otherwise. */
        if (e->len <= 0) return 0;
        ui_edit_go(e, 0, extend);
        return 1;
    case SDLK_END:
        if (e->len <= 0) return 0;
        ui_edit_go(e, e->len, extend);
        return 1;
    default: return 0;
    }
}
