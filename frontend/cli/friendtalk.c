/* friendtalk.c — say something to a friend out of a file.
 *
 * The voice turn without a microphone or a speaker: a WAV file goes in,
 * the friend hears it, and what it says back comes out as another WAV
 * file. It exists for the same reason friendchat does — the interesting
 * half of this project is the library, and the shortest path to seeing
 * whether the voice side works is a loop around friend_voice_push and
 * the pump — and it is also the only way to test this part against the
 * real providers without standing in a quiet room talking to a laptop.
 *
 *     friendtalk echo.friend said.wav              hear it, answer it
 *     friendtalk echo.friend said.wav --out it.wav where to put the reply
 *     friendtalk echo.friend said.wav --press      a held button, not
 *                                                  hands-free
 *     friendtalk echo.friend said.wav --realtime    the other way of
 *                                                   talking: one model,
 *                                                   over a socket
 *
 * The friend can be a saved one, an export (the zip or its JSON), or a
 * path where there is nothing yet, which starts a new friend called Em.
 *
 * It prints what it heard, what it said, and how long each part of that
 * took, which is the number that matters: the time from the end of your
 * sentence to the first sound back.
 *
 * Any 16-bit PCM WAV will do, at any rate, mono or stereo — the same
 * thing a microphone hands a frontend. It does not save the friend:
 * this is a tool for watching a turn happen, and friendchat is the one
 * that keeps the conversation. Keys and the rest come from the
 * environment; see platform/fr_env.h.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fr_disk.h"
#include "fr_pump.h"
#include "fr_tool.h"
#include "fr_virtual.h"

/* 20 ms at 48 kHz: one buffer's worth, so the library is handed audio in
 * the sizes a device would hand it rather than in one lump. */
#define TALK_CHUNK 960

static Fr_Disk talk_disk;

static void talk_print(const char *label, Friend_String s)
{
    printf("%s%.*s\n", label, (int)s.len, s.data ? s.data : "");
}

/* ------------------------------------------------------------------ *
 *  WAV in, WAV out
 *
 *  Enough of RIFF to read what a recorder writes and to write something
 *  a player will open. Forty-four bytes and a chunk walk.
 * ------------------------------------------------------------------ */

static uint32_t talk_le32(const unsigned char *at)
{
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) |
           ((uint32_t)at[3] << 24);
}

static uint16_t talk_le16(const unsigned char *at)
{
    return (uint16_t)((uint32_t)at[0] | ((uint32_t)at[1] << 8));
}

static void talk_put32(unsigned char *at, uint32_t value)
{
    at[0] = (unsigned char)(value & 0xFF);
    at[1] = (unsigned char)((value >> 8) & 0xFF);
    at[2] = (unsigned char)((value >> 16) & 0xFF);
    at[3] = (unsigned char)((value >> 24) & 0xFF);
}

static void talk_put16(unsigned char *at, uint16_t value)
{
    at[0] = (unsigned char)(value & 0xFF);
    at[1] = (unsigned char)((value >> 8) & 0xFF);
}

/* One channel of it, as int16. Returns the samples, or 0. */
static int16_t *talk_read_wav(const char *path, int32_t *out_count,
                              int32_t *out_rate)
{
    Fr_Mapped            file  = fr_disk_map(path);
    const unsigned char *bytes = (const unsigned char *)file.data;
    size_t               size  = file.size;
    size_t               at    = 12;
    int32_t              rate = 0, channels = 0, bits = 0;
    int16_t             *samples = 0;

    *out_count = 0;
    *out_rate  = 0;
    if (!bytes) {
        fprintf(stderr, "cannot read %s\n", path);
        return 0;
    }
    if (size < 44 || memcmp(bytes, "RIFF", 4) != 0 ||
        memcmp(bytes + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "%s is not a WAV file\n", path);
        fr_disk_unmap(file);
        return 0;
    }

    while (at + 8 <= size) {
        const unsigned char *id     = bytes + at;
        uint32_t             length = talk_le32(bytes + at + 4);
        size_t               body   = at + 8;

        if (body + length > size) length = (uint32_t)(size - body);
        if (memcmp(id, "fmt ", 4) == 0 && length >= 16) {
            channels = talk_le16(bytes + body + 2);
            rate     = (int32_t)talk_le32(bytes + body + 4);
            bits     = talk_le16(bytes + body + 14);
        } else if (memcmp(id, "data", 4) == 0) {
            if (bits != 16 || channels < 1) {
                fprintf(stderr,
                        "%s is %d-bit with %d channels; 16-bit PCM only\n",
                        path, bits, channels);
                break;
            }
            {
                int32_t frames = (int32_t)(length / (uint32_t)(2 * channels));
                int32_t i;
                samples = (int16_t *)malloc(sizeof(int16_t) *
                                            (size_t)(frames > 0 ? frames : 1));
                if (!samples) break;
                /* The left channel. A friend has one microphone. */
                for (i = 0; i < frames; ++i) {
                    const unsigned char *frame =
                        bytes + body + (size_t)i * (size_t)(2 * channels);
                    samples[i] = (int16_t)talk_le16(frame);
                }
                *out_count = frames;
                *out_rate  = rate;
            }
            break;
        }
        at = body + length + (length & 1u);
    }

    fr_disk_unmap(file);
    return samples;
}

static int talk_write_wav(const char *path, const int16_t *samples,
                          int32_t count, int32_t rate)
{
    unsigned char header[44];
    uint32_t      data_bytes = (uint32_t)count * 2u;
    FILE         *f          = fopen(path, "wb");
    int           ok;

    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return 0;
    }
    memcpy(header, "RIFF", 4);
    talk_put32(header + 4, 36u + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    talk_put32(header + 16, 16u);
    talk_put16(header + 20, 1u);
    talk_put16(header + 22, 1u);
    talk_put32(header + 24, (uint32_t)rate);
    talk_put32(header + 28, (uint32_t)rate * 2u);
    talk_put16(header + 32, 2u);
    talk_put16(header + 34, 16u);
    memcpy(header + 36, "data", 4);
    talk_put32(header + 40, data_bytes);
    ok = fwrite(header, 1, sizeof(header), f) == sizeof(header) &&
         fwrite(samples, 2, (size_t)count, f) == (size_t)count;
    if (fclose(f) != 0) ok = 0;
    if (!ok) fprintf(stderr, "cannot write %s\n", path);
    return ok;
}

/* ------------------------------------------------------------------ *
 *  What came back
 * ------------------------------------------------------------------ */

typedef struct Talk {
    int      realtime;
    int16_t *samples;
    int32_t  count;
    int32_t  capacity;
    int32_t  rate;
    uint64_t transcribed_ms;
    uint64_t first_ms;
    uint64_t last_audio_ms;
    int      replied;
} Talk;

static void talk_collect(Talk *t, const int16_t *samples, int32_t count,
                         int32_t rate)
{
    if (t->count + count > t->capacity) {
        int32_t  want = (t->capacity ? t->capacity : 48000) * 2;
        int16_t *grown;
        while (want < t->count + count) want *= 2;
        grown = (int16_t *)realloc(t->samples, sizeof(int16_t) * (size_t)want);
        if (!grown) return;
        t->samples  = grown;
        t->capacity = want;
    }
    memcpy(t->samples + t->count, samples, sizeof(int16_t) * (size_t)count);
    t->count += count;
    t->rate = rate;
}

/* One turn of the pump: the clock, what happened, then the work. */
static void talk_step(Friend_Context *fr, Fr_Net *net, Talk *t,
                      uint32_t wait_ms)
{
    Friend_Event event;
    uint64_t     now = fr_now_unix_ms();

    friend_tick(fr, now);
    while (friend_next_event(fr, &event)) {
        switch (event.kind) {
        case FRIEND_EVENT_TRANSCRIPT:
            t->transcribed_ms = now;
            printf("heard: %.*s\n", (int)event.text.len, event.text.data);
            break;
        case FRIEND_EVENT_REPLY_DELTA:
            printf("%.*s", (int)event.text.len, event.text.data);
            fflush(stdout);
            break;
        case FRIEND_EVENT_REPLY_DONE:
            printf("\n");
            t->replied = 1;
            break;
        case FRIEND_EVENT_AUDIO_OUT:
            if (t->first_ms == 0) t->first_ms = now;
            t->last_audio_ms = now;
            talk_collect(t, event.samples, event.sample_count,
                         event.sample_rate);
            break;
        case FRIEND_EVENT_MESSAGE_COMMITTED:
            /* A realtime turn arrives as two finished messages rather
             * than as a reply streaming in. */
            if (t->realtime) {
                Friend_Message m;
                if (friend_message(fr, event.index, &m) == FRIEND_OK &&
                    m.kind == FRIEND_MESSAGE_FRIEND) {
                    printf("%.*s\n", (int)m.text.len, m.text.data);
                    t->replied = 1;
                }
            }
            break;
        case FRIEND_EVENT_ERROR:
            printf("\n[%.*s]\n", (int)event.text.len, event.text.data);
            break;
        default:
            break;
        }
    }
    fr_pump_io(fr, net, wait_ms);
}

static double talk_seconds(uint64_t from, uint64_t to)
{
    return from && to ? (double)((int64_t)to - (int64_t)from) / 1000.0 : 0.0;
}

/* A voice call that failed, said; 1 if it did not. */
static int talk_ok(Friend_Context *fr, Friend_Result result, const char *what)
{
    if (result == FRIEND_OK) return 1;
    printf("%s: ", what);
    talk_print("", friend_last_error(fr));
    return 0;
}

/* All of it, in the sizes a device would hand it over. */
static int talk_push_all(Friend_Context *fr, const int16_t *said,
                         int32_t count, int32_t rate)
{
    int32_t at = 0;
    while (at < count) {
        int32_t take = count - at;
        if (take > TALK_CHUNK) take = TALK_CHUNK;
        if (!talk_ok(fr, friend_voice_push(fr, said + at, take, rate),
                     "cannot hear it")) {
            return 0;
        }
        at += take;
    }
    return 1;
}

int main(int argc, char **argv)
{
    Fr_Env          env    = fr_env_read();
    Friend_Config   config = friend_default_config();
    Friend_Context *fr;
    Fr_Virtual      reserved;
    Fr_Net         *net;
    void           *net_block;
    const char     *out_path = "reply.wav";
    char            why[256];
    int             press    = 0;
    Talk            t;
    int16_t        *said;
    int32_t         said_count = 0, said_rate = 0;
    uint64_t        start_ms, sent_ms = 0;
    struct stat     info;
    int             i;

    memset(&t, 0, sizeof(t));
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <x.friend | friend.zip | new.friend> <said.wav> "
                "[--out reply.wav] [--press | --realtime]\n",
                argv[0]);
        return 2;
    }
    for (i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--press") == 0) {
            press = 1;
        } else if (strcmp(argv[i], "--realtime") == 0) {
            t.realtime = 1;
        }
    }
    /* Line buffered: this prints as it goes, and what it printed
     * matters most when it is interrupted. */
    setvbuf(stdout, 0, _IOLBF, 0);

    said = talk_read_wav(argv[2], &said_count, &said_rate);
    if (!said) return 1;
    printf("%s: %.1f seconds at %d Hz\n", argv[2],
           (double)said_count / (double)(said_rate ? said_rate : 1),
           said_rate);

    config.voice_mode = t.realtime ? FRIEND_VOICE_REALTIME
                                   : FRIEND_VOICE_SPOKEN_TURNS;
    if (!tool_configure(&env, &config)) return 2;
    fr = fr_virtual_friend(&reserved, &config, why, sizeof(why));
    if (!fr) {
        fprintf(stderr, "%s\n", why);
        return 1;
    }

    if (stat(argv[1], &info) != 0) {
        /* A friend is made at the time the last tick was given. */
        friend_tick(fr, fr_now_unix_ms());
        if (friend_begin(fr, FRIEND_LIT("Em")) != FRIEND_OK) {
            talk_print("could not begin: ", friend_last_error(fr));
            return 1;
        }
    } else {
        char scratch[FR_PATH_MAX];
        if (!fr_disk_path(scratch, "%s.json", argv[1])) {
            fprintf(stderr, "%s.json: %s\n", argv[1], strerror(errno));
            return 1;
        }
        if (!fr_disk_begin(&talk_disk) ||
            !fr_disk_open(&talk_disk, fr, argv[1], scratch, 0, 0)) {
            fprintf(stderr, "%s\n", fr_disk_error(&talk_disk));
            return 1;
        }
    }
    printf("friend: %.*s, %d memories\n", (int)friend_name(fr).len,
           friend_name(fr).data, friend_memory_count(fr));
    printf("voice: %.*s\n", (int)friend_voice(fr).len, friend_voice(fr).data);

    net = tool_net_begin(&env, &net_block);
    if (!net) return 1;

    start_ms = fr_now_unix_ms();
    friend_tick(fr, start_ms);
    if (t.realtime) {
        /* The socket has to be open and the friend loaded into it
         * before anything said means anything. */
        if (!talk_ok(fr, friend_voice_open(fr), "cannot listen")) return 1;
        printf("opening the session...\n");
        while (friend_voice_state(fr) == FRIEND_VOICE_WORKING &&
               fr_now_unix_ms() - start_ms < 30000ull) {
            talk_step(fr, net, &t, 20);
        }
        if (friend_voice_state(fr) != FRIEND_VOICE_LISTENING) {
            printf("the session never opened\n");
            return 1;
        }
        printf("session open after %.2fs\n",
               (double)(fr_now_unix_ms() - start_ms) / 1000.0);
        start_ms = fr_now_unix_ms();
    } else if (!talk_ok(fr, press ? friend_voice_begin(fr)
                                  : friend_voice_open(fr),
                        "cannot listen")) {
        return 1;
    }

    /* The file, in the sizes a device would hand it over. */
    {
        int32_t  at    = 0;
        uint64_t began = fr_now_unix_ms();
        while (at < said_count) {
            int32_t take = said_count - at;
            if (take > TALK_CHUNK) take = TALK_CHUNK;
            if (!talk_ok(fr, friend_voice_push(fr, said + at, take,
                                               said_rate),
                         "cannot hear it")) {
                return 1;
            }
            at += take;
            if (friend_voice_state(fr) == FRIEND_VOICE_WORKING) break;
            if (t.realtime) {
                /* Pushed at the speed it was recorded at, because the
                 * far end is listening in real time and a session that
                 * is handed seven seconds of audio in one go answers
                 * before the sentence is over. */
                uint64_t rate   = (uint64_t)(said_rate > 0 ? said_rate : 1);
                uint64_t should = began + ((uint64_t)at * 1000u) / rate;
                uint64_t now;
                while ((now = fr_now_unix_ms()) < should) {
                    talk_step(fr, net, &t,
                              should - now < 5u ? (uint32_t)(should - now)
                                                : 5u);
                }
            }
        }
    }
    if (press && !talk_ok(fr, friend_voice_end(fr), "cannot finish")) {
        return 1;
    }
    /* Hands-free and the file ended while they were still talking: the
     * silence that would have ended the turn is not in the recording. */
    if (!t.realtime && friend_voice_state(fr) == FRIEND_VOICE_HEARING) {
        printf("(the recording ends mid-sentence; taking it as finished)\n");
        if (!talk_ok(fr, friend_voice_close(fr), "cannot stop listening") ||
            !talk_ok(fr, friend_voice_begin(fr), "cannot listen") ||
            !talk_push_all(fr, said, said_count, said_rate) ||
            !talk_ok(fr, friend_voice_end(fr), "cannot finish")) {
            return 1;
        }
    }
    if (friend_voice_state(fr) != FRIEND_VOICE_WORKING) {
        printf("nothing in that was speech\n");
    }
    sent_ms = fr_now_unix_ms();

    /* Until there is nothing left out and nothing left to say. The clock
     * is real, so a reply that is still being spoken is still pending. */
    for (;;) {
        uint64_t now = fr_now_unix_ms();
        uint32_t deadline;

        /* A live session keeps a socket open, so the transport is
         * never idle and cannot be what says the work is over. This is
         * a recording: it is over when the reply has arrived and the
         * audio has stopped. */
        if (t.realtime) {
            if (t.replied && t.last_audio_ms &&
                now - t.last_audio_ms > 1500ull) {
                talk_ok(fr, friend_voice_close(fr), "cannot close the session");
                break;
            }
            if (now - start_ms > 90000ull) break;
            talk_step(fr, net, &t, 20);
            continue;
        }
        if (now - start_ms > 180000ull) break;
        deadline = friend_next_deadline_ms(fr);
        if (deadline == 0xFFFFFFFFu && fr_net_active(net) == 0) break;
        talk_step(fr, net, &t, deadline > 50u ? 50u : deadline);
    }

    if (t.count > 0) {
        if (talk_write_wav(out_path, t.samples, t.count, t.rate)) {
            printf("%s: %.1f seconds at %d Hz\n", out_path,
                   (double)t.count / (double)t.rate, t.rate);
        }
    } else {
        printf("nothing came back to play\n");
    }

    printf("\n  listened %.2fs, transcribed %.2fs, first audio %.2fs\n",
           talk_seconds(start_ms, sent_ms),
           talk_seconds(sent_ms, t.transcribed_ms),
           talk_seconds(sent_ms, t.first_ms));
    if (!t.replied) printf("  (no reply arrived)\n");

    free(said);
    free(t.samples);
    tool_net_end(net, net_block);
    fr_disk_end(&talk_disk);
    fr_virtual_release(&reserved);
    return 0;
}
