/* ui_settings.c — the keys, the models, the knobs, and where they live.
 *
 * Everything in Friend_Config that a person might reasonably want to
 * change while the program is running, held as text and switches,
 * written to one JSON file, and handed back as a Friend_Config for
 * friend_reconfigure. One table says what each setting is: what the file
 * calls it, where in the configuration it goes, and how the sheet shows
 * it. Loading, saving, configuring and drawing all walk that table.
 *
 * About the key. A key in an environment variable is not in a file
 * anybody can copy, which makes the environment the better place for
 * one. But a window that can only be configured by relaunching it from a
 * shell with the right variables set is a window for people who were
 * already going to use the terminal frontend. So, as platform/fr_env.h
 * decides it for every program here:
 *
 *   - The environment seeds and what is typed here wins. A key typed
 *     into the window is the key; with the field empty, GEMINI_KEY or
 *     OPENAI_KEY is. The same goes for the base URL and the realtime
 *     model. Nothing copies an environment key into a field or a file.
 *   - A key typed into the window is a key on disk, in the clear, in
 *     settings.json beside the friends. The file is 0600 and the panel
 *     says as much, because the alternative to saying it is somebody
 *     finding out later.
 *   - Not keeping it is one switch away, and then it lasts as long as
 *     the program does.
 *
 * There is no keychain here. Doing that properly is three platforms of
 * code for a program whose whole point is that it is small, and doing
 * it improperly is the appearance of safety, which is worse than the
 * sentence in the panel.
 */

#include <stddef.h>
#include <stdio.h>

#include "../../base/fr_json.h"

/* A key, a URL, a model name, or a number. The longest of those is an
 * API key, and no provider's is close to this. */
#define UI_SETTING_MAX 256

/* In the order the sheet shows them, which is also the order tab walks
 * them. The key and the base URL come in pairs because somebody with
 * both accounts should not lose one by looking at the other; the model
 * names do not, because an empty one means the provider's default and
 * the defaults follow the provider on their own. */
enum {
    UI_SETTING_NONE = 0,
    UI_SETTING_PROVIDER,
    UI_SETTING_KEY_GEMINI,
    UI_SETTING_KEY_OPENAI,
    UI_SETTING_BASE_GEMINI,
    UI_SETTING_BASE_OPENAI,
    UI_SETTING_KEEP_KEY,
    UI_SETTING_MODEL_CHAT,
    UI_SETTING_MODEL_EXTRACT,
    UI_SETTING_MODEL_RELATE,
    UI_SETTING_MODEL_INTERIORITY,
    UI_SETTING_MODEL_RERANK,
    UI_SETTING_MODEL_EMBED,
    UI_SETTING_MODEL_VISION,
    UI_SETTING_USER_NAME,
    UI_SETTING_IMAGES,
    UI_SETTING_EMBODIED,
    UI_SETTING_VOICE,
    UI_SETTING_REALTIME,
    UI_SETTING_SPEAK_REPLIES,
    UI_SETTING_BARGE_IN,
    UI_SETTING_VOICE_NAME,
    UI_SETTING_MODEL_TRANSCRIBE,
    UI_SETTING_MODEL_SPEAK,
    UI_SETTING_MODEL_LIVE,
    UI_SETTING_CONSOLIDATE_AT,
    UI_SETTING_CONSOLIDATE_LEAVING,
    UI_SETTING_CONSOLIDATE_MAX,
    UI_SETTING_RECALL_CANDIDATES,
    UI_SETTING_RECALL_KEEP,
    UI_SETTING_RECALL_RECENT,
    UI_SETTING_MAX_TIMELINES,
    UI_SETTING_COUNT
};

typedef enum Ui_Setting_Kind {
    UI_KIND_NONE = 0,
    UI_KIND_PROVIDER, /* which of the two                                */
    UI_KIND_KEY,      /* the chosen provider's key                       */
    UI_KIND_BASE_URL, /* the chosen provider's address                   */
    UI_KIND_TEXT,     /* a string in Friend_Config                       */
    UI_KIND_MODEL,    /* a string in Friend_Models; empty is the default */
    UI_KIND_SWITCH,   /* an int32_t in Friend_Config, 0 or 1             */
    UI_KIND_NUMBER    /* an int32_t in Friend_Config; empty is the default */
} Ui_Setting_Kind;

/* Where in the file: flat, or nested under "models" or "memory" so that
 * the file reads like what it is. */
typedef enum Ui_Setting_Group {
    UI_GROUP_TOP = 0,
    UI_GROUP_MODELS,
    UI_GROUP_MEMORY
} Ui_Setting_Group;

/* For a setting that is not one field of the configuration. */
#define UI_NOWHERE 0xFFFFu

typedef struct Ui_Setting_Def {
    u8          kind;
    u8          group;
    u8          provider; /* a key or base URL: whose */
    u16         at;       /* offset into Friend_Config, or Friend_Models */
    const char *json;
    const char *heading;  /* this row starts a section of the sheet...   */
    const char *intro;    /* ...with this said under its title           */
    const char *label;
    const char *note;     /* said under the row                          */
} Ui_Setting_Def;

#define UI_CONFIG_AT(field) ((u16)offsetof(Friend_Config, field))
#define UI_MODEL_AT(field) ((u16)offsetof(Friend_Models, field))
#define UI_GEMINI ((u8)FRIEND_PROVIDER_GEMINI)
#define UI_OPENAI ((u8)FRIEND_PROVIDER_OPENAI_COMPATIBLE)

static const Ui_Setting_Def ui_setting_defs[UI_SETTING_COUNT] = {
    { UI_KIND_NONE, 0, 0, UI_NOWHERE, "", 0, 0, 0, 0 },
    { UI_KIND_PROVIDER, UI_GROUP_TOP, 0, UI_CONFIG_AT(provider), "provider",
      "Provider", 0, "Provider", 0 },
    { UI_KIND_KEY, UI_GROUP_TOP, UI_GEMINI, UI_CONFIG_AT(api_key),
      "geminiKey", 0, 0, "API key", 0 },
    { UI_KIND_KEY, UI_GROUP_TOP, UI_OPENAI, UI_CONFIG_AT(api_key),
      "openaiKey", 0, 0, "API key", 0 },
    { UI_KIND_BASE_URL, UI_GROUP_TOP, UI_GEMINI, UI_CONFIG_AT(base_url),
      "geminiBaseUrl", 0, 0, "Base URL", 0 },
    { UI_KIND_BASE_URL, UI_GROUP_TOP, UI_OPENAI, UI_CONFIG_AT(base_url),
      "openaiBaseUrl", 0, 0, "Base URL", 0 },
    { UI_KIND_SWITCH, UI_GROUP_TOP, 0, UI_NOWHERE, "keepKey", 0, 0,
      "Keep the key",
      "A key typed here is written to settings.json in the clear, readable "
      "only by you. Turn this off and it lasts until the window closes. A "
      "key from the environment is never written anywhere." },

    { UI_KIND_MODEL, UI_GROUP_MODELS, 0, UI_MODEL_AT(chat), "chat", "Models",
      "A friend makes several kinds of call and they do not want the same "
      "model. Empty is this provider's default, shown in grey.",
      "Replying", 0 },
    { UI_KIND_MODEL, UI_GROUP_MODELS, 0, UI_MODEL_AT(extract), "extract", 0, 0,
      "Extracting", 0 },
    { UI_KIND_MODEL, UI_GROUP_MODELS, 0, UI_MODEL_AT(relate), "relate", 0, 0,
      "Relating", 0 },
    { UI_KIND_MODEL, UI_GROUP_MODELS, 0, UI_MODEL_AT(interiority),
      "interiority", 0, 0, "Interior monologue", 0 },
    { UI_KIND_MODEL, UI_GROUP_MODELS, 0, UI_MODEL_AT(rerank), "rerank", 0, 0,
      "Reranking", 0 },
    { UI_KIND_MODEL, UI_GROUP_MODELS, 0, UI_MODEL_AT(embed), "embed", 0, 0,
      "Embedding", 0 },
    { UI_KIND_MODEL, UI_GROUP_MODELS, 0, UI_MODEL_AT(vision), "vision", 0, 0,
      "Looking at photographs", 0 },

    { UI_KIND_TEXT, UI_GROUP_TOP, 0, UI_CONFIG_AT(user_name), "userName",
      "The friend", 0, "It calls you", 0 },
    { UI_KIND_SWITCH, UI_GROUP_TOP, 0, UI_CONFIG_AT(enable_images),
      "photographs", 0, 0, "Photographs",
      "Off and no picture is sent, kept or looked at." },
    { UI_KIND_SWITCH, UI_GROUP_TOP, 0, UI_CONFIG_AT(embodied), "embodied", 0,
      0, "Wearing a pendant",
      "Keeps the physical-pendant language in the system prompt, which is "
      "what the service shipped. Off swaps it for a friend you talk to "
      "through a window." },

    /* A flat boolean, where "voiceName" under "models" is which voice it
     * speaks in. Different things, and the nesting tells them apart. */
    { UI_KIND_SWITCH, UI_GROUP_TOP, 0, UI_CONFIG_AT(enable_voice), "voice",
      "Voice",
      "There are two ways to talk to it. Spoken turns: what you say is "
      "transcribed and answered by the same prompt and the same model a "
      "typed message gets, and the answer is spoken back. Realtime: one "
      "audio model listens and talks at once.",
      "Talking", "Off and the microphone button goes away." },
    { UI_KIND_SWITCH, UI_GROUP_TOP, 0, UI_NOWHERE, "realtime", 0, 0,
      "Realtime",
      "One model, listening and talking at once, over a socket held open "
      "while the microphone is. It answers in well under a second and it "
      "hears how you said something rather than reading what you said. "
      "What it costs is that the friend is inside that model for the "
      "length of the session: its prompt and its memories go up once at "
      "the start. Off, what you say is transcribed and answered by the "
      "model you chose above, which is slower and is the same friend you "
      "type to." },
    { UI_KIND_SWITCH, UI_GROUP_TOP, 0, UI_CONFIG_AT(speak_replies),
      "speakReplies", 0, 0, "Speak typed replies too",
      "The service spoke a reply when the thing it answered had been "
      "spoken, and not otherwise. On, everything it says is said out "
      "loud." },
    { UI_KIND_SWITCH, UI_GROUP_TOP, 0, UI_CONFIG_AT(voice_barge_in),
      "bargeIn", 0, 0, "Interrupt it by talking",
      "Needs headphones: on speakers the microphone hears the friend and "
      "the friend interrupts itself. Off, the microphone is deaf while it "
      "talks and the button stops it." },
    /* Which voice, beside which models: it is the same kind of answer.
     * Empty is not a default but an instruction: one is picked from the
     * friend's name, which is how the service decided it. */
    { UI_KIND_TEXT, UI_GROUP_MODELS, 0, UI_CONFIG_AT(voice), "voiceName", 0,
      0, "Voice", 0 },
    { UI_KIND_MODEL, UI_GROUP_MODELS, 0, UI_MODEL_AT(transcribe),
      "transcribe", 0, 0, "Hearing you", 0 },
    { UI_KIND_MODEL, UI_GROUP_MODELS, 0, UI_MODEL_AT(speak), "speak", 0, 0,
      "Talking back", 0 },
    { UI_KIND_MODEL, UI_GROUP_MODELS, 0, UI_MODEL_AT(live), "live", 0, 0,
      "Realtime", 0 },

    { UI_KIND_NUMBER, UI_GROUP_MEMORY, 0, UI_CONFIG_AT(consolidate_at),
      "consolidateAt", "Memory",
      "The numbers the service had hardcoded. They are load bearing: the "
      "personality was tuned against them, so leaving them alone is the "
      "right answer until it is not.",
      "Consolidate at", 0 },
    { UI_KIND_NUMBER, UI_GROUP_MEMORY, 0, UI_CONFIG_AT(consolidate_leaving),
      "consolidateLeaving", 0, 0, "Leave behind", 0 },
    { UI_KIND_NUMBER, UI_GROUP_MEMORY, 0, UI_CONFIG_AT(consolidate_max),
      "consolidateMax", 0, 0, "Fold at most", 0 },
    { UI_KIND_NUMBER, UI_GROUP_MEMORY, 0, UI_CONFIG_AT(recall_candidates),
      "recallCandidates", 0, 0, "Recall candidates", 0 },
    { UI_KIND_NUMBER, UI_GROUP_MEMORY, 0, UI_CONFIG_AT(recall_keep),
      "recallKeep", 0, 0, "Keep after rerank", 0 },
    { UI_KIND_NUMBER, UI_GROUP_MEMORY, 0, UI_CONFIG_AT(recall_recent),
      "recallRecent", 0, 0, "Always recent", 0 },
    { UI_KIND_NUMBER, UI_GROUP_MEMORY, 0, UI_CONFIG_AT(max_timelines),
      "maxTimelines", 0, 0, "Timelines shown", 0 }
};

static const Ui_Setting_Def *ui_setting_def(i32 id)
{
    if (id <= UI_SETTING_NONE || id >= UI_SETTING_COUNT) {
        return &ui_setting_defs[0];
    }
    return &ui_setting_defs[id];
}

/* The settings that are typed rather than switched or chosen. */
static b32 ui_setting_is_text(i32 id)
{
    switch (ui_setting_def(id)->kind) {
    case UI_KIND_KEY:
    case UI_KIND_BASE_URL:
    case UI_KIND_TEXT:
    case UI_KIND_MODEL:
    case UI_KIND_NUMBER: return 1;
    default: return 0;
    }
}

static Friend_String *ui_config_string(Friend_Config *config, u16 at)
{
    return (Friend_String *)(void *)((char *)config + at);
}

static i32 *ui_config_int(Friend_Config *config, u16 at)
{
    return (i32 *)(void *)((char *)config + at);
}

static Friend_String *ui_model_string(Friend_Models *models, u16 at)
{
    return (Friend_String *)(void *)((char *)models + at);
}

/* ------------------------------------------------------------------ *
 *  The values
 * ------------------------------------------------------------------ */

typedef struct Ui_Settings {
    Ui_Text_Edit text[UI_SETTING_COUNT]; /* the typed ones, over bytes[] */
    b32          on[UI_SETTING_COUNT];   /* the switches                 */
    i32          provider;               /* Friend_Provider              */
    char         bytes[UI_SETTING_COUNT][UI_SETTING_MAX];
} Ui_Settings;

static void ui_settings_defaults(Ui_Settings *s)
{
    i32 id;
    memset(s, 0, sizeof(*s));
    for (id = 0; id < UI_SETTING_COUNT; ++id) {
        u32 flags = ui_setting_def(id)->kind == UI_KIND_NUMBER
                        ? UI_EDIT_DIGITS
                        : UI_EDIT_TRIM_PASTE;
        s->text[id] = ui_edit_make(s->bytes[id], UI_SETTING_MAX, flags);
    }
    s->provider                   = (i32)FRIEND_PROVIDER_GEMINI;
    s->on[UI_SETTING_KEEP_KEY]    = 1;
    s->on[UI_SETTING_IMAGES]      = 1;
    s->on[UI_SETTING_VOICE]       = 1;
}

/* The values, and not the edits' pointers into the bytes, which belong
 * to the struct they are in. */
static void ui_settings_copy(Ui_Settings *to, const Ui_Settings *from)
{
    i32 id;
    memcpy(to->bytes, from->bytes, sizeof(to->bytes));
    memcpy(to->on, from->on, sizeof(to->on));
    to->provider = from->provider;
    for (id = 0; id < UI_SETTING_COUNT; ++id) {
        to->text[id].len = from->text[id].len;
        ui_edit_go(&to->text[id], from->text[id].len, 0);
    }
}

static String ui_setting_text(const Ui_Settings *s, i32 id)
{
    if (!ui_setting_is_text(id)) return str_nil;
    return ui_edit_text(&s->text[id]);
}

static b32 ui_settings_openai(const Ui_Settings *s)
{
    return s->provider == (i32)FRIEND_PROVIDER_OPENAI_COMPATIBLE;
}

/* What a knob field means as a number. Zero for anything that is not
 * one, which is exactly what Friend_Config wants to hear. */
static i32 ui_setting_number(const Ui_Settings *s, i32 id)
{
    i64    value = 0;
    String text  = str_trim(ui_setting_text(s, id));
    if (text.len <= 0 || !str_to_i64(text, &value)) return 0;
    if (value < 0 || value > 1000000) return 0;
    return (i32)value;
}

/* The key and the base URL of the provider nobody has selected are not
 * drawn, and tabbing does not stop on them. */
static b32 ui_setting_visible(const Ui_Settings *s, i32 id)
{
    const Ui_Setting_Def *def = ui_setting_def(id);
    switch (def->kind) {
    case UI_KIND_NONE: return 0;
    case UI_KIND_KEY:
    case UI_KIND_BASE_URL: return def->provider == (u8)s->provider;
    default: return 1;
    }
}

static i32 ui_setting_key_id(const Ui_Settings *s)
{
    return ui_settings_openai(s) ? UI_SETTING_KEY_OPENAI
                                 : UI_SETTING_KEY_GEMINI;
}

/* What an empty model field becomes for this provider. */
static String ui_setting_model_default(i32 provider, i32 id)
{
    Friend_Models models =
        friend_default_models((Friend_Provider)provider);
    if (ui_setting_def(id)->kind != UI_KIND_MODEL) return str_nil;
    return *ui_model_string(&models, ui_setting_def(id)->at);
}

/* What an empty knob field becomes. */
static i32 ui_setting_number_default(i32 id)
{
    Friend_Config shipped = friend_default_config();
    if (ui_setting_def(id)->kind != UI_KIND_NUMBER) return 0;
    return *ui_config_int(&shipped, ui_setting_def(id)->at);
}

/* Switching provider: a model field still holding the old provider's
 * default is emptied, so it picks up the new one rather than sending
 * gemini-3-flash-preview to OpenAI. A name somebody typed themselves is
 * theirs and stays. */
static void ui_settings_choose_provider(Ui_Settings *s, i32 provider)
{
    i32 was = s->provider;
    i32 id;
    if (was == provider) return;
    s->provider = provider;
    for (id = 1; id < UI_SETTING_COUNT; ++id) {
        if (ui_setting_def(id)->kind != UI_KIND_MODEL) continue;
        if (str_eq(ui_setting_text(s, id), ui_setting_model_default(was, id))) {
            ui_edit_clear(&s->text[id]);
        }
    }
}

/* ------------------------------------------------------------------ *
 *  The file
 * ------------------------------------------------------------------ */

static i32 ui_setting_by_json(Ui_Setting_Group group, String name)
{
    i32 id;
    for (id = 1; id < UI_SETTING_COUNT; ++id) {
        const Ui_Setting_Def *def = ui_setting_def(id);
        if (def->group == (u8)group && str_eq(name, str_from_cstr(def->json))) {
            return id;
        }
    }
    return UI_SETTING_NONE;
}

static void ui_settings_read_value(Ui_Settings *s, Json_Parser *p, i32 id,
                                   Arena *scratch)
{
    switch (ui_setting_def(id)->kind) {
    case UI_KIND_PROVIDER:
        s->provider = str_eq_ci(json_string(p, scratch), str_lit("openai"))
                          ? (i32)FRIEND_PROVIDER_OPENAI_COMPATIBLE
                          : (i32)FRIEND_PROVIDER_GEMINI;
        break;
    case UI_KIND_SWITCH: s->on[id] = json_bool(p); break;
    case UI_KIND_NUMBER: {
        char at[32];
        i64  value = json_i64(p);
        int  wrote = value > 0 ? snprintf(at, sizeof(at), "%lld",
                                          (long long)value)
                               : 0;
        ui_edit_set(&s->text[id], str_make(at, wrote > 0 ? (i32)wrote : 0));
        break;
    }
    case UI_KIND_NONE: json_skip(p); break;
    default: ui_edit_set(&s->text[id], json_string(p, scratch)); break;
    }
}

static void ui_settings_read_group(Ui_Settings *s, Json_Parser *p,
                                   Ui_Setting_Group group, Arena *scratch)
{
    String key;
    if (!json_object_begin(p)) return;
    while (json_object_next(p, &key)) {
        if (group == UI_GROUP_TOP && str_eq(key, str_lit("models"))) {
            ui_settings_read_group(s, p, UI_GROUP_MODELS, scratch);
        } else if (group == UI_GROUP_TOP && str_eq(key, str_lit("memory"))) {
            ui_settings_read_group(s, p, UI_GROUP_MEMORY, scratch);
        } else {
            ui_settings_read_value(s, p, ui_setting_by_json(group, key),
                                   scratch);
        }
    }
}

/* Returns 0 when there is no file to read, or not one this wrote. */
static b32 ui_settings_load(Ui_Settings *s, const char *path, Arena *scratch)
{
    Arena_Mark  mark   = arena_mark(scratch);
    Fr_Mapped   mapped = fr_disk_map(path);
    Json_Parser p;
    b32         ok;

    if (!mapped.data) return 0;
    p = json_begin(str_make(mapped.data, (i32)fr_min(mapped.size,
                                                      (size_t)FR_MB(1))));
    ok = json_peek(&p) == JSON_OBJECT;
    if (ok) ui_settings_read_group(s, &p, UI_GROUP_TOP, scratch);
    ok = ok && !p.failed;
    fr_disk_unmap(mapped);
    arena_reset_to(scratch, mark);
    return ok;
}

static void ui_settings_write_group(const Ui_Settings *s, Json_Writer *w,
                                    Ui_Setting_Group group)
{
    i32 id;
    for (id = 1; id < UI_SETTING_COUNT; ++id) {
        const Ui_Setting_Def *def  = ui_setting_def(id);
        String                name = str_from_cstr(def->json);
        if (def->group != (u8)group) continue;
        switch (def->kind) {
        case UI_KIND_PROVIDER:
            jw_field_string(w, name, ui_settings_openai(s) ? str_lit("openai")
                                                           : str_lit("gemini"));
            break;
        case UI_KIND_SWITCH: jw_field_bool(w, name, s->on[id]); break;
        case UI_KIND_NUMBER:
            if (ui_setting_number(s, id) > 0) {
                jw_field_i64(w, name, ui_setting_number(s, id));
            }
            break;
        case UI_KIND_KEY:
            if (!s->on[UI_SETTING_KEEP_KEY]) break;
            /* fall through */
        default:
            if (ui_setting_text(s, id).len > 0) {
                jw_field_string(w, name, ui_setting_text(s, id));
            }
            break;
        }
    }
}

/* Written beside itself and then over itself, 0600 from the moment it
 * exists: a key that was briefly world readable was world readable, and
 * a key half written is no key at all. */
static b32 ui_settings_save(const Ui_Settings *s, const char *path,
                            Arena *scratch)
{
    Arena_Mark  mark = arena_mark(scratch);
    Json_Writer w    = jw_begin(scratch);
    String      text;
    b32         ok;

    jw_object_begin(&w);
    jw_field_i64(&w, str_lit("version"), 1);
    ui_settings_write_group(s, &w, UI_GROUP_TOP);
    jw_key(&w, str_lit("models"));
    jw_object_begin(&w);
    ui_settings_write_group(s, &w, UI_GROUP_MODELS);
    jw_object_end(&w);
    jw_key(&w, str_lit("memory"));
    jw_object_begin(&w);
    ui_settings_write_group(s, &w, UI_GROUP_MEMORY);
    jw_object_end(&w);
    jw_object_end(&w);

    text = jw_done(&w);
    ok   = text.len > 0 &&
         fr_disk_write_atomic(path, text.data, (size_t)text.len);
    arena_reset_to(scratch, mark);
    return ok;
}

/* ------------------------------------------------------------------ *
 *  Into a Friend_Config
 * ------------------------------------------------------------------ */

/* Everything here points into the settings, which live as long as the
 * window does, or into the environment, which lives as long as the
 * process. What is typed wins, and the environment fills in what is
 * not; anything still empty is the library's own default. */
static void ui_settings_into_config(const Ui_Settings *s, const Fr_Env *env,
                                    Friend_Config *config)
{
    i32 id;

    config->provider = (Friend_Provider)s->provider;
    config->api_key  = fr_env_key(env, config->provider);
    config->base_url = env->base_url;
    memset(&config->models, 0, sizeof(config->models));
    for (id = 1; id < UI_SETTING_COUNT; ++id) {
        const Ui_Setting_Def *def   = ui_setting_def(id);
        String                typed = ui_setting_text(s, id);
        switch (def->kind) {
        case UI_KIND_KEY:
        case UI_KIND_BASE_URL:
            if (def->provider == (u8)s->provider && typed.len > 0) {
                *ui_config_string(config, def->at) = typed;
            }
            break;
        case UI_KIND_TEXT: *ui_config_string(config, def->at) = typed; break;
        case UI_KIND_MODEL:
            if (typed.len > 0) *ui_model_string(&config->models, def->at) = typed;
            break;
        case UI_KIND_SWITCH:
            if (def->at != UI_NOWHERE) {
                *ui_config_int(config, def->at) = s->on[id] ? 1 : 0;
            }
            break;
        case UI_KIND_NUMBER:
            *ui_config_int(config, def->at) = ui_setting_number(s, id);
            break;
        default: break;
        }
    }
    if (ui_setting_text(s, UI_SETTING_MODEL_LIVE).len == 0 &&
        env->live_model.len > 0) {
        config->models.live = env->live_model;
    }
    config->voice_mode = s->on[UI_SETTING_REALTIME]
                             ? (i32)FRIEND_VOICE_REALTIME
                             : (i32)FRIEND_VOICE_SPOKEN_TURNS;
}
