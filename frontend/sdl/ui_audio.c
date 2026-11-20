/* ui_audio.c — the microphone and the speaker.
 *
 * The thinnest layer that can exist between a device and the library:
 * SDL converts whatever the hardware is doing into the one format the
 * friend wants (mono, 16-bit, FRIEND_VOICE_RATE in), the window hands it over
 * every frame, and what comes back goes straight out the other side.
 * There is no ring buffer here and no thread — the library already owns
 * a buffer for a spoken turn, and a frame is 16 milliseconds, which is
 * closer together than the friend needs to be fed.
 *
 * What the speaker queue is good for besides playing: it is the only
 * honest answer to "how much of that did they actually hear", which is
 * the number a friend wants when it is interrupted.
 */

/* A frame's worth several times over, so a stall in the window does not
 * drop audio the microphone has already captured. */
#define UI_MIC_CHUNK 4096

typedef struct Ui_Audio {
    SDL_AudioStream *mic;
    SDL_AudioStream *speaker;
    i32              speaker_rate;
    b32              broken; /* said once, not every frame */
    i64              given;  /* samples handed to the speaker since quiet */
    i16              buffer[UI_MIC_CHUNK];
} Ui_Audio;

/* Which device, when the default one is not the one you meant. A name
 * in the environment rather than a list in the settings sheet: the
 * right default is the system's own, and the people who need another
 * one are the people running this against a recording. The match is a
 * case-insensitive substring of what the driver calls the device. */
static SDL_AudioDeviceID ui_audio_named(const char *from_env, b32 recording)
{
    const char      *wanted = SDL_getenv(from_env);
    SDL_AudioDeviceID chosen =
        recording ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                  : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    SDL_AudioDeviceID *devices;
    int                count = 0, i;

    if (!wanted || !*wanted) return chosen;
    devices = recording ? SDL_GetAudioRecordingDevices(&count)
                        : SDL_GetAudioPlaybackDevices(&count);
    if (!devices) return chosen;
    for (i = 0; i < count; ++i) {
        const char *name = SDL_GetAudioDeviceName(devices[i]);
        if (name && SDL_strcasestr(name, wanted)) {
            chosen = devices[i];
            break;
        }
    }
    SDL_free(devices);
    return chosen;
}

static b32 ui_audio_mic_on(Ui_Audio *audio)
{
    SDL_AudioSpec spec;

    if (audio->mic) return 1;
    spec.format   = SDL_AUDIO_S16LE;
    spec.channels = 1;
    spec.freq     = FRIEND_VOICE_RATE;
    audio->mic    = SDL_OpenAudioDeviceStream(
        ui_audio_named("FRIEND_LOCAL_MIC", 1), &spec, 0, 0);
    if (!audio->mic) return 0;
    SDL_ResumeAudioStreamDevice(audio->mic);
    return 1;
}

static void ui_audio_mic_off(Ui_Audio *audio)
{
    if (!audio->mic) return;
    SDL_DestroyAudioStream(audio->mic);
    audio->mic = 0;
}

/* Everything the microphone has for us, in the sizes it has it. */
static void ui_audio_hear(Ui_Audio *audio, Friend_Context *fr)
{
    if (!audio->mic || !fr) return;
    for (;;) {
        int got = SDL_GetAudioStreamData(audio->mic, audio->buffer,
                                         (int)sizeof(audio->buffer));
        if (got <= 0) break;
        friend_voice_push(fr, audio->buffer, (i32)(got / 2),
                          FRIEND_VOICE_RATE);
        if (got < (int)sizeof(audio->buffer)) break;
    }
}

static void ui_audio_play(Ui_Audio *audio, const i16 *samples, i32 count,
                          i32 rate)
{
    if (count <= 0 || rate <= 0) return;

    /* The speaker is opened at the rate the friend is speaking at, and
     * reopened if that ever changes — which it does not today, but a
     * provider that answered at 48 kHz would otherwise play at half
     * speed and sound like a ghost. */
    if (audio->speaker && audio->speaker_rate != rate) {
        SDL_DestroyAudioStream(audio->speaker);
        audio->speaker = 0;
    }
    if (!audio->speaker) {
        SDL_AudioSpec spec;
        spec.format   = SDL_AUDIO_S16LE;
        spec.channels = 1;
        spec.freq     = rate;
        audio->speaker = SDL_OpenAudioDeviceStream(
            ui_audio_named("FRIEND_LOCAL_SPEAKER", 0), &spec, 0, 0);
        if (!audio->speaker) return;
        audio->speaker_rate = rate;
        SDL_ResumeAudioStreamDevice(audio->speaker);
    }
    if (SDL_GetAudioStreamQueued(audio->speaker) <= 0) audio->given = 0;
    SDL_PutAudioStreamData(audio->speaker, samples,
                           (int)((size_t)count * sizeof(i16)));
    audio->given += count;
}

/* Stop talking mid-word: what has not left the speaker is not going to
 * be heard, and keeping it would mean the friend carries on saying
 * something it has already taken back. */
static void ui_audio_hush(Ui_Audio *audio)
{
    if (audio->speaker) SDL_ClearAudioStream(audio->speaker);
    audio->given = 0;
}

/* How much of what the friend said has actually come out of the
 * speaker, 0 to 1. The one thing the window knows that the library
 * cannot. */
static f32 ui_audio_heard_fraction(Ui_Audio *audio)
{
    i64 queued;
    if (!audio->speaker || audio->given <= 0) return 0.0f;
    queued = (i64)SDL_GetAudioStreamQueued(audio->speaker) / 2;
    if (queued <= 0) return 1.0f;
    if (queued >= audio->given) return 0.0f;
    return (f32)(audio->given - queued) / (f32)audio->given;
}

static void ui_audio_end(Ui_Audio *audio)
{
    ui_audio_mic_off(audio);
    if (audio->speaker) SDL_DestroyAudioStream(audio->speaker);
    audio->speaker = 0;
}
