/* friendchat.c — a friend in a terminal.
 *
 * The smallest thing that is actually a conversation: load a friend,
 * type, read what it says back. No window, no fonts, no audio. It exists
 * because the interesting half of this project is the library, and the
 * shortest path to seeing whether the library works is a loop around
 * friend_say and the pump.
 *
 *     friendchat friend_Echo.zip          import an export and chat
 *     friendchat echo.friend              carry on with a saved friend
 *     friendchat echo.friend --once "hi"  one message, for a script
 *
 * The folder it was given a file in is a shelf of friends, the same as
 * the window's: an import becomes <name>.friend there, with its
 * photographs beside it, and it is saved after every reply. A friend you
 * talked to and then lost would be a poor advert for running your own.
 *
 * Keys and the rest come from the environment; see platform/fr_env.h.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "fr_pump.h"
#include "fr_shelf.h"
#include "fr_tool.h"
#include "fr_virtual.h"

#define CHAT_LINE_MAX 8192

static Fr_Shelf chat_shelf;

static void chat_print(const char *label, Friend_String s)
{
    printf("%s%.*s\n", label, (int)s.len, s.data ? s.data : "");
}

static Friend_String chat_cstr(const char *c)
{
    Friend_String s;
    s.data = c;
    s.len  = (int32_t)strlen(c);
    return s;
}

/* ------------------------------------------------------------------ *
 *  The pump
 * ------------------------------------------------------------------ */

static void chat_events(Friend_Context *fr)
{
    Friend_Event event;

    while (friend_next_event(fr, &event)) {
        switch (event.kind) {
        case FRIEND_EVENT_REPLY_DELTA:
            printf("%.*s", (int)event.text.len, event.text.data);
            fflush(stdout);
            break;
        case FRIEND_EVENT_REPLY_DONE:
            printf("\n");
            break;
        case FRIEND_EVENT_ERROR:
            printf("\n[%.*s]\n", (int)event.text.len, event.text.data);
            break;
        case FRIEND_EVENT_MEMORIES_CHANGED:
            printf("\n[%d memories now]\n", event.count);
            break;
        default:
            break;
        }
    }
}

/* Runs the friend until it has nothing left to do, waiting on the
 * network or on the friend's own deadlines, whichever comes first. */
static void chat_pump(Friend_Context *fr, Fr_Net *net)
{
    for (;;) {
        uint32_t deadline;

        friend_tick(fr, fr_now_unix_ms());
        chat_events(fr);
        deadline = friend_next_deadline_ms(fr);
        if (deadline == 0xFFFFFFFFu && fr_net_active(net) == 0) break;
        fr_pump_io(fr, net, deadline > 1000u ? 1000u : deadline);
    }
}

/* ------------------------------------------------------------------ *
 *  Main
 * ------------------------------------------------------------------ */

/* The folder a path is in, into dir (FR_PATH_MAX), and its name within
 * that folder. */
static int chat_split(const char *path, char *dir, const char **out_leaf)
{
    const char *slash = strrchr(path, '/');
    *out_leaf         = slash ? slash + 1 : path;
    if (!slash) return fr_disk_path(dir, ".");
    return fr_disk_path(dir, "%.*s",
                        slash == path ? 1 : (int)(slash - path), path);
}

int main(int argc, char **argv)
{
    Fr_Env            env    = fr_env_read();
    Friend_Config     config = friend_default_config();
    Friend_Context   *fr;
    Friend_Result     result;
    Fr_Virtual        reserved;
    void             *net_block;
    Fr_Net           *net;
    char              dir[FR_PATH_MAX];
    char              save_path[FR_PATH_MAX];
    const char       *leaf;
    Fr_Mapped         photos[FRIEND_ATTACHMENTS_MAX];
    Friend_Attachment pending[FRIEND_ATTACHMENTS_MAX];
    int               pending_count = 0;
    char              line[CHAT_LINE_MAX];
    const char       *once = 0;
    char              why[256];
    int32_t           index;
    int               i;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <friend.zip | friend_state.json | x.friend> "
                "[--once \"message\"]\n",
                argv[0]);
        return 2;
    }
    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--once") == 0 && i + 1 < argc) {
            once = argv[++i];
        }
    }

    config.embodied = 0;
    if (!tool_configure(&env, &config)) return 2;
    fr = fr_virtual_friend(&reserved, &config, why, sizeof(why));
    if (!fr) {
        fprintf(stderr, "%s\n", why);
        return 1;
    }
    net = tool_net_begin(&env, &net_block);
    if (!net) return 1;

    if (!chat_split(argv[1], dir, &leaf)) {
        fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    if (!fr_shelf_begin(&chat_shelf, dir)) {
        fprintf(stderr, "%s\n", fr_shelf_error(&chat_shelf));
        return 1;
    }
    index = fr_shelf_find(&chat_shelf, leaf, (int32_t)strlen(leaf));
    if (index >= 0) {
        if (!fr_shelf_open(&chat_shelf, fr, net, &config, index)) {
            fprintf(stderr, "%s\n", fr_shelf_error(&chat_shelf));
            return 1;
        }
    } else {
        Friend_Import_Report report;
        index = fr_shelf_import(&chat_shelf, fr, net, &config, argv[1],
                                &report);
        if (index < 0) {
            fprintf(stderr, "could not import: %s\n",
                    fr_shelf_error(&chat_shelf));
            return 1;
        }
        printf("imported %d memories, %d connections, %d messages, "
               "%d photos\n",
               report.memories, report.connections, report.messages,
               report.images);
        if (report.memories_dropped || report.messages_dropped ||
            report.truncated) {
            printf("  (dropped %d memories, %d messages; %d fields "
                   "truncated)\n",
                   report.memories_dropped, report.messages_dropped,
                   report.truncated);
        }
    }
    fr_shelf_path(&chat_shelf, chat_shelf.current, save_path,
                  sizeof(save_path));

    {
        Friend_Stats  stats = friend_stats(fr);
        Friend_String name  = friend_name(fr);
        printf("%.*s — %d memories, %d connections, %d messages. "
               "%.0f MiB of %.0f MiB.\n",
               (int)name.len, name.data, stats.memories, stats.connections,
               friend_message_count(fr),
               (double)stats.bytes_committed / (1024.0 * 1024.0),
               (double)reserved.reserved / (1024.0 * 1024.0));
        printf("saving to %s. /prompt, /inner, /graph, /mem, /photo, /quit.\n\n",
               save_path);
    }

    for (;;) {
        if (once) {
            snprintf(line, sizeof(line), "%s", once);
        } else {
            printf("you: ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin)) break;
            {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' ||
                                   line[len - 1] == '\r')) {
                    line[--len] = 0;
                }
            }
            if (line[0] == 0) continue;
            if (strcmp(line, "/quit") == 0) break;
            if (strcmp(line, "/prompt") == 0) {
                Friend_String prompt = friend_last_prompt(fr);
                printf("%.*s\n", (int)prompt.len, prompt.data);
                continue;
            }
            if (strcmp(line, "/inner") == 0) {
                chat_print("", friend_interiority(fr));
                continue;
            }
            if (strcmp(line, "/graph") == 0) {
                int32_t n = friend_memory_count(fr);
                int32_t k;
                for (k = 0; k < n; ++k) {
                    Friend_Memory_Node node;
                    if (friend_memory_at(fr, k, &node) != FRIEND_OK) continue;
                    printf("%4d %s used %llu  %.*s\n", k,
                           node.recalled_now ? "*" : " ",
                           (unsigned long long)node.use_count,
                           (int)node.description.len, node.description.data);
                }
                continue;
            }
            if (strncmp(line, "/photo ", 7) == 0) {
                /* The file as it is: this tool does not decode or
                 * scale anything, so it wants a JPEG somebody has
                 * already made small enough to send. */
                Fr_Mapped photo = fr_disk_map(line + 7);
                if (!photo.data) {
                    printf("cannot read %s\n", line + 7);
                } else if (pending_count >= FRIEND_ATTACHMENTS_MAX) {
                    printf("that is as many as one message carries\n");
                    fr_disk_unmap(photo);
                } else if (photo.size < 4 ||
                           (unsigned char)photo.data[0] != 0xFF ||
                           (unsigned char)photo.data[1] != 0xD8) {
                    printf("%s is not a JPEG\n", line + 7);
                    fr_disk_unmap(photo);
                } else {
                    photos[pending_count]       = photo;
                    pending[pending_count].jpeg.data = photo.data;
                    pending[pending_count].jpeg.len  = (int32_t)photo.size;
                    ++pending_count;
                    printf("attached %s (%.0f KiB) — it goes with the next "
                           "thing you say\n",
                           line + 7, (double)photo.size / 1024.0);
                }
                continue;
            }
            if (strcmp(line, "/mem") == 0) {
                Friend_Stats stats = friend_stats(fr);
                printf("%d memories (%d never used), %d connections, "
                       "%d in the window, %llu replies\n",
                       stats.memories, stats.memories_unused,
                       stats.connections, stats.active_events,
                       (unsigned long long)stats.replies_total);
                printf("%d in the scrollback, %llu lost (%llu of them "
                       "before any memory formed)\n",
                       stats.scrollback,
                       (unsigned long long)stats.messages_lost,
                       (unsigned long long)stats.messages_unremembered);
                printf("%.1f MiB committed of %.1f MiB reserved\n",
                       (double)stats.bytes_committed / (1024.0 * 1024.0),
                       (double)stats.bytes_total / (1024.0 * 1024.0));
                continue;
            }
        }

        friend_tick(fr, fr_now_unix_ms());
        result = friend_say(fr, chat_cstr(line),
                            pending_count > 0 ? pending : 0, pending_count);
        if (result != FRIEND_OK) {
            chat_print("", friend_last_error(fr));
            if (once) break;
            continue;
        }
        /* The friend has its own copy now. */
        while (pending_count > 0) fr_disk_unmap(photos[--pending_count]);

        {
            Friend_String name = friend_name(fr);
            printf("%.*s: ", (int)name.len, name.data);
            fflush(stdout);
        }
        chat_pump(fr, net);
        if (!fr_shelf_save(&chat_shelf, fr, net)) {
            fprintf(stderr, "%s\n", fr_shelf_error(&chat_shelf));
        }
        if (once) break;
    }

    while (pending_count > 0) fr_disk_unmap(photos[--pending_count]);
    tool_net_end(net, net_block);
    fr_shelf_end(&chat_shelf);
    fr_virtual_release(&reserved);
    return 0;
}
