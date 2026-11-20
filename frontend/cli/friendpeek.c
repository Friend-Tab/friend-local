/* friendpeek — read an export (or a saved friend) and say what is in it.
 *
 *     friendpeek friend_Name.zip
 *     friendpeek friend_state.json
 *     friendpeek saved.friend
 *     friendpeek friend_Name.zip out.friend photos.blob
 *     friendpeek --sizes
 *
 * This is the smallest useful frontend, and it is here because it is the
 * fastest way to point the library at a real export and see whether it
 * came out right. It walks the import path every other frontend walks
 * (platform/fr_disk.h): unzip to a file, map the file, import the map.
 * The zip is unzipped beside itself, and the JSON removed again.
 *
 * The mapping is the point. The export's JSON can be a gigabyte, and
 * neither the library nor a frontend with a fixed budget has room for
 * that — but the operating system will hand us a window onto the file for
 * nothing, demand-paged and read-only, and the importer never needs more
 * than one pass over it.
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fr_clock.h"
#include "fr_disk.h"
#include "fr_pump.h"
#include "fr_virtual.h"

static Fr_Disk peek_disk;

static void print_string(const char *label, Friend_String s)
{
    printf("%s%.*s\n", label, (int)s.len, s.data ? s.data : "");
}

static void print_messages(Friend_Context *fr, int count)
{
    int total = friend_message_count(fr);
    int first = total > count ? total - count : 0;
    int i;

    printf("\nlast %d of %d messages\n", total - first, total);
    for (i = first; i < total; ++i) {
        Friend_Message message;
        const char    *who = "?";
        if (friend_message(fr, i, &message) != FRIEND_OK) continue;
        switch (message.kind) {
        case FRIEND_MESSAGE_USER: who = "user  "; break;
        case FRIEND_MESSAGE_FRIEND: who = "friend"; break;
        case FRIEND_MESSAGE_AMBIENT: who = "heard "; break;
        case FRIEND_MESSAGE_NOTICE: who = "----  "; break;
        }
        printf("  %4d %s %.*s%s\n", i, who,
               (int)(message.text.len > 100 ? 100 : message.text.len),
               message.text.data ? message.text.data : "",
               message.text.len > 100 ? "..." : "");
    }
}

/* Works the pump until the friend has nothing left to do, or a few
 * seconds have gone by: a read that failed waits to be tried again, and
 * this is not going to wait with it. friendpeek has no network and wants
 * none, so everything but the file work fails at once. */
static void pump(Friend_Context *fr)
{
    uint64_t until = fr_now_unix_ms() + 5000u;
    do {
        fr_pump_io(fr, 0, 0);
        friend_tick(fr, fr_now_unix_ms());
    } while (friend_next_deadline_ms(fr) != 0xFFFFFFFFu &&
             fr_now_unix_ms() < until);
}

/* Every photograph the friend holds, read back out of the blob file
 * through the same call a window would use. The point is the last
 * column: those bytes came off the disk and they start the way a JPEG
 * starts. */
static void print_photos(Friend_Context *fr, int count)
{
    int total = friend_message_count(fr);
    int found = 0;
    int i;

    for (i = total - 1; i >= 0 && found < count; --i) {
        Friend_Message message;
        int            j;
        if (friend_message(fr, i, &message) != FRIEND_OK) continue;
        if (message.attachment_count == 0) continue;

        for (j = 0; j < message.attachment_count && found < count; ++j) {
            Friend_String jpeg, caption;
            Friend_Result result;

            if (found == 0) printf("\nphotographs\n");
            ++found;
            /* Not in memory yet is a read for the pump to do. */
            result = friend_message_attachment(fr, i, j, &jpeg, &caption);
            if (result == FRIEND_ERR_BAD_STATE) {
                pump(fr);
                result = friend_message_attachment(fr, i, j, &jpeg, &caption);
            }
            printf("  message %d photo %d: ", i, j);
            if (result != FRIEND_OK) {
                print_string("unreadable: ", friend_last_error(fr));
                continue;
            }
            printf("%d bytes, %s\n", (int)jpeg.len,
                   jpeg.len > 3 && (unsigned char)jpeg.data[0] == 0xFF &&
                           (unsigned char)jpeg.data[1] == 0xD8
                       ? "a JPEG"
                       : "NOT a JPEG");
            if (caption.len > 0) {
                printf("    %.*s\n", (int)(caption.len > 110 ? 110
                                                             : caption.len),
                       caption.data);
            }
        }
    }
}

/* What one more of something in Friend_Limits costs: the difference
 * friend_required_memory sees between the default and half of it, per
 * entry. Measured rather than added up from the records, so it is what
 * the library really reserves, padding and all. */
static size_t cost_of_one(Friend_Limits limits, size_t field)
{
    int32_t *count = (int32_t *)(void *)((char *)&limits + field);
    int32_t  all   = *count;
    size_t   full  = friend_required_memory(limits);
    size_t   half;

    *count = all / 2;
    half   = friend_required_memory(limits);
    return (full - half) / (size_t)(all - all / 2);
}

static void print_cost(const char *what, int32_t count, size_t each)
{
    printf("  %-12s %7d x %5lu = %8.1f MiB\n", what, (int)count,
           (unsigned long)each,
           (double)each * (double)count / (1024.0 * 1024.0));
}

/* --sizes: what the default limits cost, and how much of it a friend
 * touches before it has done anything. */
static int print_sizes(void)
{
    Friend_Limits  limits = friend_default_limits();
    Friend_Config  config = friend_default_config();
    Friend_Context *fr;
    Fr_Virtual     reserved;
    char           why[256];

    printf("default budget\n");
    print_cost("memories", limits.max_memories,
               cost_of_one(limits, offsetof(Friend_Limits, max_memories)));
    print_cost("connections", limits.max_connections,
               cost_of_one(limits, offsetof(Friend_Limits, max_connections)));
    print_cost("window", limits.max_active_events,
               cost_of_one(limits,
                           offsetof(Friend_Limits, max_active_events)));
    print_cost("scrollback", limits.max_scrollback,
               cost_of_one(limits, offsetof(Friend_Limits, max_scrollback)));
    printf("  %-12s                 = %8.1f MiB\n", "...its text",
           (double)limits.scrollback_bytes / (1024.0 * 1024.0));
    printf("  RESERVED                     = %8.1f MiB\n",
           (double)friend_required_memory(limits) / (1024.0 * 1024.0));

    fr = fr_virtual_friend(&reserved, &config, why, sizeof(why));
    if (!fr) {
        fprintf(stderr, "%s\n", why);
        return 1;
    }
    printf("  committed at startup         = %8.1f MiB\n",
           (double)friend_stats(fr).bytes_committed / (1024.0 * 1024.0));
    fr_virtual_release(&reserved);
    return 0;
}

int main(int argc, char **argv)
{
    Friend_Config        config = friend_default_config();
    Friend_Import_Report report;
    Friend_Context      *fr;
    Friend_Stats         stats;
    Fr_Virtual           reserved;
    char                 why[256];
    Fr_Mapped            source;
    char                 scratch[FR_PATH_MAX];
    const char          *path;

    if (argc < 2) {
        fprintf(stderr, "usage: friendpeek <export.zip | state.json | "
                        "saved.friend> [out.friend] [photos.blob]\n"
                        "       friendpeek --sizes\n");
        return 2;
    }
    if (strcmp(argv[1], "--sizes") == 0) return print_sizes();
    path = argv[1];

    config.friend_name = FRIEND_LIT("friend");
    /* Photographs go somewhere, or they are described and dropped. The
     * third argument is where; without one this reads an export without
     * keeping any pixels, which is what it is mostly for. */
    if (argc > 3) {
        config.blob_path.data = argv[3];
        config.blob_path.len  = (int32_t)strlen(argv[3]);
    }

    fr = fr_virtual_friend(&reserved, &config, why, sizeof(why));
    if (!fr) {
        fprintf(stderr, "%s\n", why);
        return 1;
    }
    printf("block: %.1f MiB reserved\n",
           (double)reserved.reserved / (1024.0 * 1024.0));

    source = fr_disk_map(path);
    if (!source.data) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    printf("file: %.2f MiB\n", (double)source.size / (1024.0 * 1024.0));
    fr_disk_unmap(source);

    if (!fr_disk_path(scratch, "%s.json", path)) {
        fprintf(stderr, "%s.json: %s\n", path, strerror(errno));
        return 1;
    }
    if (!fr_disk_begin(&peek_disk) ||
        !fr_disk_open(&peek_disk, fr, path, scratch, &report, 0)) {
        fprintf(stderr, "%s\n", fr_disk_error(&peek_disk));
        return 1;
    }

    /* The photographs an import staged are written out here. */
    if (!fr_pump_photos(fr, 0, 5000u)) {
        fprintf(stderr, "%d photographs could not be written out\n",
                friend_stats(fr).photos_waiting);
    }

    print_string("name: ", friend_name(fr));
    print_string("interiority: ", friend_interiority(fr));
    printf("imported: %d memories, %d connections, %d messages, %d photos "
           "(%d without pixels)\n",
           report.memories, report.connections, report.messages,
           report.images, report.images_without_pixels);
    printf("dropped:  %d memories, %d connections, %d messages, %d truncated\n",
           report.memories_dropped, report.connections_dropped,
           report.messages_dropped, report.truncated);

    stats = friend_stats(fr);
    printf("stats: %d memories (%d unused), %d connections, %d active\n",
           stats.memories, stats.memories_unused, stats.connections,
           stats.active_events);
    printf("       %llu replies, epoch %llu of %llu\n",
           (unsigned long long)stats.replies_total,
           (unsigned long long)stats.epoch_samples,
           (unsigned long long)stats.epoch_length);
    printf("       %d messages in the scrollback, %llu lost (%llu before "
           "they were remembered)\n",
           stats.scrollback, (unsigned long long)stats.messages_lost,
           (unsigned long long)stats.messages_unremembered);
    printf("       %d photographs, %d still to be written\n",
           stats.photos, stats.photos_waiting);
    printf("memory: %.1f MiB committed of %.1f MiB reserved\n",
           (double)stats.bytes_committed / (1024.0 * 1024.0),
           (double)stats.bytes_total / (1024.0 * 1024.0));

    print_messages(fr, 12);
    print_photos(fr, 4);

    /* Save, reload into the same context, and make sure the second save
     * is the same bytes. If the format cannot survive that it cannot be
     * trusted with somebody's friend. */
    {
        size_t saved_size = friend_save_size(fr);
        void  *saved      = malloc(saved_size);
        void  *again      = malloc(saved_size);
        size_t written = 0, again_written = 0;

        printf("\nsave: %.2f MiB\n", (double)saved_size / (1024.0 * 1024.0));
        if (!saved || !again ||
            friend_save(fr, saved, saved_size, &written) != FRIEND_OK) {
            print_string("save failed: ", friend_last_error(fr));
            return 1;
        }
        if (friend_load(fr, saved, written) != FRIEND_OK) {
            print_string("reload failed: ", friend_last_error(fr));
            return 1;
        }
        if (friend_save(fr, again, saved_size, &again_written) != FRIEND_OK ||
            again_written != written ||
            memcmp(again, saved, written) != 0) {
            fprintf(stderr, "save/load/save is not stable\n");
            return 1;
        }
        printf("save/load/save: identical\n");

        if (argc > 2) {
            if (!fr_disk_write_atomic(argv[2], saved, written)) {
                fprintf(stderr, "cannot write %s\n", argv[2]);
                return 1;
            }
            printf("wrote %s\n", argv[2]);
        }
        free(saved);
        free(again);
    }
    fr_disk_end(&peek_disk);
    fr_virtual_release(&reserved);
    return 0;
}
