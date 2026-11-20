/* fr_platform_tests.c — the host layer every frontend shares, tested on
 * its own: the shelf of friends, the paths that do not fit, the photo
 * file, scratch files, and the transport starting up. There is no
 * network here; whatever would have gone to a model fails, the way it
 * does with a dead one. Run from the repository root. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fr_clock.h"
#include "fr_disk.h"
#include "fr_export.h"
#include "fr_net.h"
#include "fr_pump.h"
#include "fr_shelf.h"
#include "fr_virtual.h"

#include "../../base/fr_base.c"
#include "../../base/fr_test.h"

static char test_dir[512];

static i64 platform_file_size(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 ? (i64)info.st_size : -1;
}

static i32 platform_file_mode(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 ? (i32)(info.st_mode & 0777) : -1;
}

static void platform_put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static void platform_say_with_photo(Friend_Context *fr)
{
    static u8         jpeg[2048];
    Friend_Attachment a;

    memset(jpeg, 0xAB, sizeof(jpeg));
    jpeg[0] = 0xFF;
    jpeg[1] = 0xD8;
    memset(&a, 0, sizeof(a));
    a.jpeg.data = (const char *)jpeg;
    a.jpeg.len  = (i32)sizeof(jpeg);
    friend_tick(fr, fr_now_unix_ms());
    CHECK(friend_say(fr, str_lit("look at this"), &a, 1) == FRIEND_OK);
}

static b32 platform_current_is(const Fr_Shelf *s, const char *file)
{
    return s->current >= 0 && strcmp(s->entries[s->current].file, file) == 0;
}

static void test_net_begin(void)
{
    static long tiny[8];
    char        e[256];
    size_t      size  = fr_net_size(FR_NET_CONNECTIONS);
    void       *block = malloc(size);
    Fr_Net     *net;

    CHECK(!fr_net_begin(tiny, sizeof(tiny), FR_NET_CONNECTIONS, 0, e,
                        sizeof(e)));
    CHECK(strstr(e, "needs") != 0);
    CHECK(!fr_net_begin(block, size, FR_NET_CONNECTIONS, "/nowhere.pem", e,
                        sizeof(e)));
    CHECK(strstr(e, "CA bundle") != 0);
    net = fr_net_begin(block, size, FR_NET_CONNECTIONS, 0, e, sizeof(e));
    CHECK(net != 0 && e[0] == 0);
    if (net) fr_net_end(net);
    free(block);
}

static void test_disk(void)
{
    Friend_Config   config = friend_default_config();
    Fr_Virtual      space;
    Friend_Context *fr;
    Fr_Disk         disk;
    char            path[FR_PATH_MAX], other[FR_PATH_MAX];
    char            long_name[2000];
    char            got[8];
    int             fd;

    config.api_key = str_lit("offline-key");
    fr = fr_virtual_friend(&space, &config, 0, 0);
    CHECK(fr != 0);
    if (!fr) return;

    /* A path that does not fit is refused, not cut short. */
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    errno = 0;
    CHECK(!fr_disk_path(path, "%s/%s", test_dir, long_name));
    CHECK(errno == ENAMETOOLONG && path[0] == 0);
    errno = 0;
    CHECK(!fr_disk_write_atomic(long_name, "x", 1) && errno == ENAMETOOLONG);

    /* The photo file is made 0600 and never cut back. */
    {
        Friend_Io_Request req;
        snprintf(path, sizeof(path), "%s/photos.blob", test_dir);
        memset(&req, 0, sizeof(req));
        req.kind   = FRIEND_IO_FILE_WRITE;
        req.id     = 1; /* no such request: the friend ignores the answer */
        req.path   = str_from_cstr(path);
        req.offset = 4;
        req.body   = str_lit("WXYZ");
        fr_disk_request(fr, &req);
        CHECK(platform_file_size(path) == 8);
        CHECK(platform_file_mode(path) == 0600);
        req.offset = 0;
        req.body   = str_lit("ab");
        fr_disk_request(fr, &req);
        CHECK(platform_file_size(path) == 8);
        fd = open(path, O_RDONLY);
        CHECK(fd >= 0 && read(fd, got, 8) == 8);
        if (fd >= 0) close(fd);
        CHECK(memcmp(got, "ab\0\0WXYZ", 8) == 0);
    }

    /* An unzip never writes over a file that is already there, nor over
     * the zip, and cleans up only what it made. */
    CHECK(fr_disk_begin(&disk));
    snprintf(path, sizeof(path), "%s/export.zip", test_dir);
    snprintf(other, sizeof(other), "%s/export.zip.json", test_dir);
    platform_put(path, "PK\003\004 not really a zip");
    platform_put(other, "somebody's file");
    CHECK(!fr_disk_import(&disk, fr, path, other, 0));
    CHECK(platform_file_size(other) == 15);
    CHECK(!fr_disk_import(&disk, fr, path, path, 0));
    CHECK(platform_file_size(path) > 0);
    unlink(other);
    CHECK(!fr_disk_import(&disk, fr, path, other, 0));
    CHECK(platform_file_size(other) == -1);
    fr_disk_end(&disk);

    fr_virtual_release(&space);
}

static void test_shelf_photos(void)
{
    Friend_Config   config = friend_default_config();
    Fr_Virtual      space;
    Friend_Context *fr;
    Fr_Shelf        shelf;
    char            dir[FR_PATH_MAX], path[2 * FR_PATH_MAX];
    i32             index;

    config.api_key = str_lit("offline-key");
    fr = fr_virtual_friend(&space, &config, 0, 0);
    CHECK(fr != 0);
    if (!fr) return;

    /* Files the shelf cannot read are counted, not hidden. */
    snprintf(dir, sizeof(dir), "%s/photo_shelf", test_dir);
    mkdir(dir, 0700);
    snprintf(path, sizeof(path), "%s/junk.friend", dir);
    platform_put(path, "not a friend");
    CHECK(fr_shelf_begin(&shelf, dir));
    CHECK(strstr(fr_shelf_error(&shelf), "1 .friend file") != 0);
    unlink(path);

    /* Nothing in hand: a change with nowhere to go is an error. */
    CHECK(fr_shelf_save(&shelf, fr, 0));
    CHECK(friend_say(fr, str_lit("to nobody"), 0, 0) == FRIEND_OK);
    CHECK(!fr_shelf_save(&shelf, fr, 0));

    /* A photograph is written before the save that describes it... */
    CHECK(fr_shelf_create(&shelf, fr, 0, &config, str_lit("Sam")) >= 0);
    platform_say_with_photo(fr);
    CHECK(friend_stats(fr).photos_waiting > 0);
    CHECK(fr_shelf_save(&shelf, fr, 0));
    CHECK(friend_stats(fr).photos_waiting == 0);
    snprintf(path, sizeof(path), "%s/sam.blob", shelf.dir);
    CHECK(platform_file_size(path) > 0 && platform_file_mode(path) == 0600);

    /* ...and before the friend is put away for another. */
    platform_say_with_photo(fr);
    CHECK(friend_stats(fr).photos_waiting > 0);
    CHECK(fr_shelf_create(&shelf, fr, 0, &config, str_lit("Echo")) >= 0);
    index = fr_shelf_find(&shelf, "sam.friend", 10);
    CHECK(fr_shelf_open(&shelf, fr, 0, &config, index));
    CHECK(friend_stats(fr).photos == 2);
    CHECK(friend_stats(fr).photos_waiting == 0);

    /* Newest first, to the nanosecond. */
    CHECK(shelf.count == 2 &&
          strcmp(shelf.entries[0].file, "echo.friend") == 0);

    /* A photo file that will not take a write: said, kept behind, the
     * friend not let go of, and saved once the library retries. */
    if (getuid() != 0) {
        u64 began, until;
        snprintf(path, sizeof(path), "%s/sam.blob", shelf.dir);
        chmod(path, 0400);
        platform_say_with_photo(fr);
        began = fr_now_unix_ms();
        CHECK(!fr_shelf_save(&shelf, fr, 0));
        CHECK(fr_now_unix_ms() - began < 5000);
        CHECK(strstr(fr_shelf_error(&shelf), "photograph") != 0);
        CHECK(fr_shelf_behind(&shelf, fr));
        index = fr_shelf_find(&shelf, "echo.friend", 11);
        CHECK(!fr_shelf_open(&shelf, fr, 0, &config, index));
        CHECK(platform_current_is(&shelf, "sam.friend"));
        chmod(path, 0600);
        /* The friend says when it will try the write again; wait for
         * that, within reason. */
        until = fr_now_unix_ms() + 30000u;
        while (friend_stats(fr).photos_waiting > 0 &&
               fr_now_unix_ms() < until) {
            u32 wait = friend_next_deadline_ms(fr);
            friend_tick(fr, fr_now_unix_ms());
            fr_pump_io(fr, 0, wait > 1000u ? 1000u : wait);
        }
        CHECK(friend_stats(fr).photos_waiting == 0);
        CHECK(fr_shelf_save(&shelf, fr, 0));
        CHECK(!fr_shelf_behind(&shelf, fr));
    }

    /* A failed import puts back what was open, photo file and all. */
    {
        char before[FR_PATH_MAX];
        i32  photos = friend_stats(fr).photos;
        i32  full   = shelf.count;
        snprintf(before, sizeof(before), "%s", shelf.blob_path);
        CHECK(fr_shelf_import(&shelf, fr, 0, &config, "/nowhere/at/all.json",
                              0) == -1);
        CHECK(platform_current_is(&shelf, "sam.friend"));
        CHECK(strcmp(shelf.blob_path, before) == 0);
        CHECK(friend_stats(fr).photos == photos);
        snprintf(path, sizeof(path), "%s/.import.blob", shelf.dir);
        CHECK(platform_file_size(path) == -1);

        /* A full shelf refuses both ways in. */
        shelf.count = FR_SHELF_FRIENDS_MAX;
        CHECK(fr_shelf_create(&shelf, fr, 0, &config, str_lit("Z")) == -1);
        CHECK(fr_shelf_import(&shelf, fr, 0, &config, "libfriend/tests/export.json",
                              0) == -1);
        CHECK(strstr(fr_shelf_error(&shelf), "room") != 0);
        shelf.count = full;
    }

    /* Archiving: the photographs come back if the friend will not
     * follow them. */
    {
        char friend_file[2 * FR_PATH_MAX], held[FR_PATH_MAX];
        index = fr_shelf_find(&shelf, "sam.friend", 10);
        snprintf(friend_file, sizeof(friend_file), "%s/sam.friend", shelf.dir);
        snprintf(held, sizeof(held), "%s/held", test_dir);
        CHECK(rename(friend_file, held) == 0); /* the list still has it */
        CHECK(!fr_shelf_archive(&shelf, fr, 0, &config, index));
        snprintf(path, sizeof(path), "%s/sam.blob", shelf.dir);
        CHECK(platform_file_size(path) > 0);
        snprintf(path, sizeof(path), "%s/archived/sam.blob", shelf.dir);
        CHECK(platform_file_size(path) == -1);
        CHECK(rename(held, friend_file) == 0);
        CHECK(fr_shelf_archive(&shelf, fr, 0, &config, index));
        CHECK(platform_file_size(path) > 0);
        CHECK(shelf.current == -1 && shelf.blob_path[0] == 0);
    }

    fr_shelf_end(&shelf);
    fr_virtual_release(&space);
}

/* Writes libfriend/tests/export.json into a zip of the shape friend.com made, with
 * the system's own zip tool, so the importer reads a real one. */
static b32 make_zip(const char *json, const char *zip)
{
    char  command[2048];
    char  staging[1024];
    FILE *in, *out;
    char  buffer[4096];
    size_t n;

    snprintf(staging, sizeof(staging), "%s/zipwork", test_dir);
    mkdir(staging, 0700);
    snprintf(command, sizeof(command), "%s/friend_state.json", staging);
    in  = fopen(json, "rb");
    out = fopen(command, "wb");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return 0;
    }
    while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) fwrite(buffer, 1, n, out);
    fclose(in);
    fclose(out);
    snprintf(command, sizeof(command),
             "cd '%s' && python3 -c \"import zipfile;"
             "z=zipfile.ZipFile('%s','w',zipfile.ZIP_DEFLATED);"
             "z.write('friend_state.json');z.close()\"",
             staging, zip);
    return system(command) == 0;
}

/* ------------------------------------------------------------------ *
 *  The shelf on its own
 * ------------------------------------------------------------------ */

static Fr_Shelf shelf;

static b32 shelf_has(const char *leaf, i64 *out_size)
{
    char        path[2048];
    struct stat info;
    snprintf(path, sizeof(path), "%s/%s", shelf.dir, leaf);
    if (stat(path, &info) != 0) return 0;
    if (out_size) *out_size = (i64)info.st_size;
    return 1;
}

static void shelf_put(const char *leaf, const char *contents)
{
    char  path[2048];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", shelf.dir, leaf);
    f = fopen(path, "wb");
    if (!f) return;
    fputs(contents, f);
    fclose(f);
}

static b32 shelf_current_is(const char *file)
{
    return shelf.current >= 0 &&
           str_eq(str_make(shelf.entries[shelf.current].file,
                           shelf.entries[shelf.current].file_len),
                  str_from_cstr(file));
}

static void test_shelf(void)
{
    char            dir[1024];
    char            zip[1024];
    char            leaf[64];
    Friend_Config   config = friend_default_config();
    Fr_Virtual      space;
    Friend_Context *fr;
    i64             size = -1;
    i32             index, echo, i;

    /* A friend of the shelf's own, outside the app's. */
    config.limits.max_memories      = 64;
    config.limits.max_connections   = 128;
    config.limits.max_active_events = 8;
    config.limits.max_scrollback    = 16;
    config.api_key                  = str_lit("offline-key");
    fr = fr_virtual_friend(&space, &config, 0, 0);
    CHECK(fr != 0);
    if (!fr) return;

    snprintf(dir, sizeof(dir), "%s/shelf/", test_dir);
    CHECK(fr_shelf_begin(&shelf, dir));
    CHECK(shelf.count == 0);
    CHECK(shelf.dir[shelf.dir_len - 1] != '/');

    /* Names collide; files do not, and nor do the photographs. */
    echo = fr_shelf_create(&shelf, fr, 0, &config, str_lit("  Echo "));
    CHECK(echo >= 0 && shelf_current_is("echo.friend"));
    CHECK_STR(friend_name(fr), "Echo");
    index = fr_shelf_create(&shelf, fr, 0, &config, str_lit("Echo"));
    CHECK(index >= 0 && shelf_current_is("echo-2.friend"));
    CHECK(strstr(shelf.blob_path, "/echo-2.blob") != 0);
    CHECK(fr_shelf_create(&shelf, fr, 0, &config, str_lit(" ")) == -1);

    /* A photo file nobody owns still takes its name: whoever got that
     * name would be handed somebody else's pictures. */
    shelf_put("sam.blob", "not yours");
    CHECK(fr_shelf_create(&shelf, fr, 0, &config, str_lit("Sam")) >= 0);
    CHECK(shelf_current_is("sam-2.friend"));

    /* Every name taken is a refusal, not an overwrite of the last one. */
    shelf_put("x.friend", "");
    for (i = 2; i < 1000; ++i) {
        snprintf(leaf, sizeof(leaf), "x-%d.friend", (int)i);
        shelf_put(leaf, "");
    }
    CHECK(fr_shelf_create(&shelf, fr, 0, &config, str_lit("X")) == -1);
    CHECK(strstr(fr_shelf_error(&shelf), "taken") != 0);
    CHECK(shelf_has("x-999.friend", &size) && size == 0);

    /* Archiving twice under one name keeps both. */
    CHECK(fr_shelf_open(&shelf, fr, 0, &config,
                        fr_shelf_find(&shelf, "echo.friend", 11)));
    CHECK(shelf_current_is("echo.friend"));
    shelf_put("echo.blob", "pictures");
    CHECK(fr_shelf_archive(&shelf, fr, 0, &config, shelf.current));
    CHECK(shelf.current == -1);
    CHECK(fr_shelf_find(&shelf, "echo.friend", 11) == -1);
    CHECK(shelf_has("archived/echo.friend", 0));
    CHECK(shelf_has("archived/echo.blob", 0));
    CHECK(!shelf_has("echo.blob", 0));
    index = fr_shelf_create(&shelf, fr, 0, &config, str_lit("Echo"));
    CHECK(index >= 0 && shelf_current_is("echo.friend"));
    CHECK(fr_shelf_archive(&shelf, fr, 0, &config, fr_shelf_find(&shelf, "echo-2.friend", 13)));
    CHECK(shelf_current_is("echo.friend")); /* kept hold of through it */
    CHECK(fr_shelf_archive(&shelf, fr, 0, &config, shelf.current));
    CHECK(shelf_has("archived/echo-2.friend", 0));
    CHECK(shelf_has("archived/echo-3.friend", 0));
    CHECK(shelf_has("archived/echo.blob", &size) && size == 8);

    /* Imports, both shapes, leave nothing behind but the friend. */
    index = fr_shelf_import(&shelf, fr, 0, &config, "libfriend/tests/export.json", 0);
    CHECK(index >= 0 && shelf.current == index);
    CHECK(fr_disk_has_suffix(shelf.blob_path, ".blob") &&
          strstr(shelf.blob_path, ".import") == 0);
    CHECK(!shelf_has(".import.json", 0) && !shelf_has(".import.blob", 0));
    snprintf(zip, sizeof(zip), "%s/shelf_test.zip", test_dir);
    if (make_zip("libfriend/tests/export.json", zip)) {
        i32 before = shelf.count;
        CHECK(fr_shelf_import(&shelf, fr, 0, &config, zip, 0) >= 0);
        CHECK(shelf.count == before + 1);
        CHECK(!shelf_has(".import.json", 0) && !shelf_has(".import.blob", 0));
    }
    CHECK(fr_shelf_import(&shelf, fr, 0, &config, "/nowhere/at/all.zip", 0) ==
          -1);

    /* A save that fails is said once, kept behind, not tried again on
     * every frame, and tried when asked; and the friend it belongs to is
     * not closed. */
    if (getuid() != 0) {
        index = fr_shelf_find(&shelf, "sam-2.friend", 12);
        CHECK(fr_shelf_open(&shelf, fr, 0, &config, index));
        CHECK(!fr_shelf_behind(&shelf, fr));
        CHECK(friend_say(fr, str_lit("keep this"), 0, 0) == FRIEND_OK);
        /* Offline, the reply fails and the friend settles. */
        for (i = 0; i < 20 && !friend_stats(fr).settled; ++i) {
            fr_pump_io(fr, 0, 0);
            friend_tick(fr, fr_now_unix_ms());
        }
        CHECK(friend_stats(fr).settled);
        CHECK(fr_shelf_behind(&shelf, fr));
        chmod(shelf.dir, 0500);
        CHECK(!fr_shelf_save_when_quiet(&shelf, fr));
        CHECK(shelf.unwritten);
        CHECK(fr_shelf_save_when_quiet(&shelf, fr)); /* not again yet */
        CHECK(!fr_shelf_open(&shelf, fr, 0, &config, index == 0 ? 1 : 0));
        CHECK(shelf_current_is("sam-2.friend"));
        chmod(shelf.dir, 0700);
        CHECK(fr_shelf_save(&shelf, fr, 0));
        CHECK(!fr_shelf_behind(&shelf, fr));
        CHECK(fr_shelf_open(&shelf, fr, 0, &config, index == 0 ? 1 : 0));
        CHECK(fr_shelf_open(&shelf, fr, 0, &config,
                            fr_shelf_find(&shelf, "sam-2.friend", 12)));
        CHECK(friend_message_count(fr) >= 1);
    }
    fr_shelf_end(&shelf);
    fr_virtual_release(&space);
}

/* ------------------------------------------------------------------ *
 *  Exports: a friend in one file, to another machine and back
 * ------------------------------------------------------------------ */

static u8 export_bytes[3][1 << 20];

static i64 export_read(const char *path, u8 *into, size_t cap)
{
    FILE  *f = fopen(path, "rb");
    size_t n;
    if (!f) return -1;
    n = fread(into, 1, cap, f);
    fclose(f);
    return (i64)n;
}

static void export_write(const char *path, const u8 *bytes, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(bytes, 1, size, f);
    fclose(f);
}

static i32 export_files_in(const char *dir)
{
    DIR           *d = opendir(dir);
    struct dirent *e;
    i32            n = 0;
    if (!d) return -1;
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) ++n;
    }
    closedir(d);
    return n;
}

static u32 export_get32(const u8 *at)
{
    return (u32)at[0] | ((u32)at[1] << 8) | ((u32)at[2] << 16) |
           ((u32)at[3] << 24);
}

static u64 export_get64(const u8 *at)
{
    return (u64)export_get32(at) | ((u64)export_get32(at + 4) << 32);
}

static void export_put32(u8 *at, u32 v)
{
    at[0] = (u8)v;
    at[1] = (u8)(v >> 8);
    at[2] = (u8)(v >> 16);
    at[3] = (u8)(v >> 24);
}

static void export_put64(u8 *at, u64 v)
{
    export_put32(at, (u32)v);
    export_put32(at + 4, (u32)(v >> 32));
}

/* A section of an export, found through its table. */
static b32 export_section(const u8 *file, u32 tag, const u8 **out,
                          size_t *out_size)
{
    u32 header = export_get32(file + 12), count = export_get32(file + 24), i;
    for (i = 0; i < count; ++i) {
        const u8 *e = file + header + i * FR_EXPORT_ENTRY_BYTES;
        if (export_get32(e) == tag) {
            *out      = file + export_get64(e + 8);
            *out_size = (size_t)export_get64(e + 16);
            return 1;
        }
    }
    return 0;
}

typedef struct Export_Part {
    u32       tag, flags;
    const u8 *bytes;
    size_t    size;
} Export_Part;

/* An export built by hand, the way a newer or an older build might have
 * written one: any version, any header length, any sections, with every
 * checksum right so that only what is being tested is wrong. */
static size_t export_build(u8 *out, u32 version, u32 oldest, u32 header_bytes,
                           const Export_Part *parts, u32 count)
{
    static const u8 zero[4] = { 0, 0, 0, 0 };
    size_t          at      = header_bytes + count * FR_EXPORT_ENTRY_BYTES;
    u32             i, crc;

    memset(out, 0, at);
    memcpy(out, FR_EXPORT_MAGIC, FR_EXPORT_MAGIC_BYTES);
    export_put32(out + 12, header_bytes);
    export_put32(out + 16, version);
    export_put32(out + 20, oldest);
    export_put32(out + 24, count);
    export_put64(out + 32, 1234);
    for (i = 0; i < count; ++i) {
        u8 *e = out + header_bytes + i * FR_EXPORT_ENTRY_BYTES;
        export_put32(e, parts[i].tag);
        export_put32(e + 4, parts[i].flags);
        export_put64(e + 8, at);
        export_put64(e + 16, parts[i].size);
        export_put32(e + 24, fr_export_crc32(0, parts[i].bytes, parts[i].size));
        memcpy(out + at, parts[i].bytes, parts[i].size);
        at += parts[i].size;
        while (at & 7u) out[at++] = 0;
    }
    export_put64(out + 40, at);
    crc = fr_export_crc32(0, out, 48);
    crc = fr_export_crc32(crc, zero, 4);
    crc = fr_export_crc32(crc, out + 52, header_bytes - 52u);
    crc = fr_export_crc32(crc, out + header_bytes,
                          count * FR_EXPORT_ENTRY_BYTES);
    export_put32(out + 48, crc);
    return at;
}

/* Settles a friend that has just been told something: offline, the
 * reply fails, and then nothing more is coming. */
static void export_settle(Friend_Context *fr)
{
    i32 i;
    for (i = 0; i < 50 && !friend_stats(fr).settled; ++i) {
        fr_pump_io(fr, 0, 0);
        friend_tick(fr, fr_now_unix_ms());
    }
    CHECK(friend_stats(fr).settled);
}

/* Whether every photograph the friend holds comes back with its pixels:
 * a read through the photo file, so it is the file on this shelf that
 * is being looked at. */
static i32 export_photos_readable(Friend_Context *fr)
{
    i32 m, a, n = 0, tries;
    for (m = 0; m < friend_message_count(fr); ++m) {
        Friend_Message msg;
        if (friend_message(fr, m, &msg) != FRIEND_OK) continue;
        for (a = 0; a < msg.attachment_count; ++a) {
            Friend_String jpeg;
            for (tries = 0; tries < 50; ++tries) {
                Friend_Result r =
                    friend_message_attachment(fr, m, a, &jpeg, 0);
                if (r == FRIEND_OK) {
                    if (jpeg.len == 2048 && (u8)jpeg.data[0] == 0xFF &&
                        (u8)jpeg.data[1000] == 0xAB) {
                        ++n;
                    }
                    break;
                }
                if (r != FRIEND_ERR_BAD_STATE) break;
                friend_tick(fr, fr_now_unix_ms());
                fr_pump_io(fr, 0, 0);
            }
        }
    }
    return n;
}

/* A file that must be refused: refused for the reason given, with the
 * friend that was open still open and nothing new on the shelf. */
static void export_refused(Fr_Shelf *s, Friend_Context *fr,
                           Friend_Config *config, const char *path,
                           const char *reason)
{
    char before[FR_SHELF_FILE_MAX];
    i32  files = export_files_in(s->dir);
    i32  count = s->count;
    i32  messages = friend_message_count(fr);

    snprintf(before, sizeof(before), "%s",
             s->current >= 0 ? s->entries[s->current].file : "");
    CHECK(fr_shelf_import(s, fr, 0, config, path, 0) == -1);
    if (!strstr(fr_shelf_error(s), reason)) {
        printf("    wanted \"%s\" in: %s\n", reason, fr_shelf_error(s));
        FAIL("the refusal says why");
    }
    CHECK(export_files_in(s->dir) == files);
    CHECK(s->count == count);
    CHECK(s->current >= 0 && strcmp(s->entries[s->current].file, before) == 0);
    CHECK(friend_message_count(fr) == messages);
}

static void test_export(void)
{
    static Fr_Shelf home, away;
    Friend_Config   config = friend_default_config();
    Fr_Virtual      space;
    Friend_Context *fr;
    char            dir[FR_PATH_MAX], path[2 * FR_PATH_MAX];
    char            first[FR_PATH_MAX], second[FR_PATH_MAX];
    char            again[FR_PATH_MAX], made[FR_PATH_MAX];
    const u8       *friend_part, *photo_part;
    size_t          friend_size = 0, photo_size = 0;
    i64             size;
    i32             echo, at_export, photos_at_export, index;

    config.api_key = str_lit("offline-key");
    fr = fr_virtual_friend(&space, &config, 0, 0);
    CHECK(fr != 0);
    if (!fr) return;

    snprintf(dir, sizeof(dir), "%s/export_home", test_dir);
    CHECK(fr_shelf_begin(&home, dir));
    snprintf(dir, sizeof(dir), "%s/export_away", test_dir);
    CHECK(fr_shelf_begin(&away, dir));
    snprintf(first, sizeof(first), "%s/echo.friendlocal", test_dir);
    snprintf(second, sizeof(second), "%s/echo-back.friendlocal", test_dir);
    snprintf(again, sizeof(again), "%s/echo-again.friendlocal", test_dir);
    snprintf(made, sizeof(made), "%s/made.friendlocal", test_dir);

    /* A friend that has been talked to for a while, photographs and
     * all. Two of them are still in memory when the export is asked
     * for: it saves them first. */
    echo = fr_shelf_create(&home, fr, 0, &config, str_lit("Echo"));
    CHECK(echo >= 0);
    platform_say_with_photo(fr);
    export_settle(fr);
    CHECK(fr_shelf_save(&home, fr, 0));
    platform_say_with_photo(fr);
    export_settle(fr);
    CHECK(friend_say(fr, str_lit("and one more thing"), 0, 0) == FRIEND_OK);
    export_settle(fr);
    CHECK(friend_stats(fr).photos_waiting > 0 || fr_shelf_behind(&home, fr));
    at_export        = friend_message_count(fr);
    photos_at_export = friend_stats(fr).photos;
    CHECK(photos_at_export == 2);

    CHECK(fr_shelf_export(&home, fr, 0, home.current, first));
    CHECK(!fr_shelf_behind(&home, fr));
    CHECK(platform_file_mode(first) == 0600);
    size = export_read(first, export_bytes[0], sizeof(export_bytes[0]));
    CHECK(size > 0 && memcmp(export_bytes[0], "friend-local", 12) == 0);
    CHECK(export_get32(export_bytes[0] + 16) == FR_EXPORT_VERSION);
    CHECK(export_get32(export_bytes[0] + 20) == FR_EXPORT_OLDEST_READER);
    CHECK(export_get64(export_bytes[0] + 40) == (u64)size);
    CHECK(export_section(export_bytes[0], FR_EXPORT_FRIEND, &friend_part,
                         &friend_size));
    CHECK(export_section(export_bytes[0], FR_EXPORT_PHOTOS, &photo_part,
                         &photo_size));
    CHECK(photo_size >= 2 * 2048);

    /* Talking on after the export changes the friend at home, not the
     * file that was already written. */
    CHECK(friend_say(fr, str_lit("said after the export"), 0, 0) ==
          FRIEND_OK);
    export_settle(fr);
    CHECK(friend_message_count(fr) > at_export);
    CHECK(fr_shelf_save(&home, fr, 0));

    /* On the other machine: the friend as it was when it left. Something
     * else is open there first, and stays safe. */
    CHECK(fr_shelf_create(&away, fr, 0, &config, str_lit("Someone")) >= 0);
    index = fr_shelf_import(&away, fr, 0, &config, first, 0);
    CHECK(index >= 0 && away.current == index);
    CHECK(strcmp(away.entries[index].file, "echo.friend") == 0);
    CHECK_STR(friend_name(fr), "Echo");
    CHECK(friend_message_count(fr) == at_export);
    CHECK(friend_stats(fr).photos == photos_at_export);
    CHECK(export_photos_readable(fr) == photos_at_export);
    CHECK(strstr(away.blob_path, "/export_away/echo.blob") != 0);
    CHECK(fr_shelf_find(&away, "someone.friend", 14) >= 0);

    /* Exported again untouched, it is the same friend byte for byte:
     * nothing was converted on the way in. */
    CHECK(fr_shelf_export(&away, fr, 0, away.current, again));
    size = export_read(again, export_bytes[1], sizeof(export_bytes[1]));
    {
        const u8 *f2, *p2;
        size_t    f2_size = 0, p2_size = 0;
        CHECK(export_section(export_bytes[1], FR_EXPORT_FRIEND, &f2, &f2_size));
        CHECK(export_section(export_bytes[1], FR_EXPORT_PHOTOS, &p2, &p2_size));
        CHECK(f2_size == friend_size && memcmp(f2, friend_part, f2_size) == 0);
        CHECK(p2_size == photo_size && memcmp(p2, photo_part, p2_size) == 0);
    }

    /* Talked to over there — a photograph too — and brought home, where
     * a friend of that name is already: it arrives beside it, and the
     * one at home is untouched. */
    platform_say_with_photo(fr);
    export_settle(fr);
    CHECK(friend_say(fr, str_lit("said on the other machine"), 0, 0) ==
          FRIEND_OK);
    export_settle(fr);
    {
        i32 away_count = friend_message_count(fr);
        CHECK(fr_shelf_export(&away, fr, 0, away.current, second));
        index = fr_shelf_import(&home, fr, 0, &config, second, 0);
        CHECK(index >= 0 &&
              strcmp(home.entries[index].file, "echo-2.friend") == 0);
        CHECK(friend_message_count(fr) == away_count);
        CHECK(export_photos_readable(fr) == photos_at_export + 1);
        CHECK(fr_shelf_open(&home, fr, 0, &config,
                            fr_shelf_find(&home, "echo.friend", 11)));
        CHECK(friend_message_count(fr) > at_export);
        CHECK(friend_message_count(fr) != away_count);
        CHECK(export_photos_readable(fr) == photos_at_export);
    }

    /* A friend that was never shown a photograph has no photo file to
     * carry, and arrives without one. An entry that is not open is
     * exported from its file. */
    {
        i32 plain = fr_shelf_create(&home, fr, 0, &config, str_lit("Plain"));
        CHECK(plain >= 0);
        CHECK(fr_shelf_open(&home, fr, 0, &config,
                            fr_shelf_find(&home, "echo.friend", 11)));
        snprintf(path, sizeof(path), "%s/plain.friendlocal", test_dir);
        CHECK(fr_shelf_export(&home, fr, 0,
                              fr_shelf_find(&home, "plain.friend", 12), path));
        size = export_read(path, export_bytes[1], sizeof(export_bytes[1]));
        CHECK(size > 0 && export_get32(export_bytes[1] + 24) == 1);
        CHECK(fr_shelf_import(&away, fr, 0, &config, path, 0) >= 0);
        CHECK_STR(friend_name(fr), "Plain");
        snprintf(path, sizeof(path), "%s/plain.blob", away.dir);
        CHECK(platform_file_size(path) == -1);
    }

    /* An export is never written over the friend it came from. */
    fr_shelf_path(&home, home.current, path, sizeof(path));
    CHECK(!fr_shelf_export(&home, fr, 0, home.current, path));
    CHECK(strstr(fr_shelf_error(&home), "own file") != 0);
    CHECK(platform_file_size(path) > 0);

    /* Versions. A newer build that only added things a reader can pass
     * by — a longer header, a section it does not know but does not
     * need — still opens here. */
    {
        Export_Part parts[3];
        size_t      n;
        parts[0].tag   = FR_EXPORT_FRIEND;
        parts[0].flags = FR_EXPORT_REQUIRED;
        parts[0].bytes = friend_part;
        parts[0].size  = friend_size;
        parts[1].tag   = FR_EXPORT_TAG('N', 'O', 'T', 'E');
        parts[1].flags = 0;
        parts[1].bytes = (const u8 *)"from the future";
        parts[1].size  = 15;
        parts[2].tag   = FR_EXPORT_PHOTOS;
        parts[2].flags = FR_EXPORT_REQUIRED;
        parts[2].bytes = photo_part;
        parts[2].size  = photo_size;
        n = export_build(export_bytes[2], FR_EXPORT_VERSION + 1,
                         FR_EXPORT_OLDEST_READER, 96, parts, 3);
        export_write(made, export_bytes[2], n);
        index = fr_shelf_import(&away, fr, 0, &config, made, 0);
        CHECK(index >= 0);
        CHECK(friend_message_count(fr) == at_export);
        CHECK(export_photos_readable(fr) == photos_at_export);

        /* One that says this build is too old to read it is refused,
         * and says to update, before anything about it is believed. */
        n = export_build(export_bytes[2], FR_EXPORT_VERSION + 1,
                         FR_EXPORT_VERSION + 1, 64, parts, 3);
        export_write(made, export_bytes[2], n);
        export_refused(&away, fr, &config, made, "newer friend-local");
        CHECK(strstr(fr_shelf_error(&away), "update") != 0);

        /* So is one that needs a part this build does not know. */
        parts[1].flags = FR_EXPORT_REQUIRED;
        n = export_build(export_bytes[2], FR_EXPORT_VERSION + 1,
                         FR_EXPORT_OLDEST_READER, 64, parts, 3);
        export_write(made, export_bytes[2], n);
        export_refused(&away, fr, &config, made, "does not know (NOTE)");

        /* And one whose friend was saved by a newer library, which the
         * library itself would refuse: the export says so first. */
        memcpy(export_bytes[1], friend_part, friend_size);
        export_put32(export_bytes[1] + 8, 999u);
        parts[0].bytes = export_bytes[1];
        n = export_build(export_bytes[2], FR_EXPORT_VERSION,
                         FR_EXPORT_OLDEST_READER, 64, parts, 1);
        export_write(made, export_bytes[2], n);
        export_refused(&away, fr, &config, made, "cannot read; update");
        parts[0].bytes = friend_part;

        /* No friend in it at all. */
        n = export_build(export_bytes[2], FR_EXPORT_VERSION,
                         FR_EXPORT_OLDEST_READER, 64, parts + 2, 1);
        export_write(made, export_bytes[2], n);
        export_refused(&away, fr, &config, made, "no friend");
    }

    /* Damage. A photograph's byte, a header byte, a copy cut short, one
     * with something on the end: each refused, each for what it is. */
    size = export_read(first, export_bytes[1], sizeof(export_bytes[1]));
    {
        size_t photo_at = (size_t)(photo_part - export_bytes[0]);
        memcpy(export_bytes[2], export_bytes[1], (size_t)size);
        export_bytes[2][photo_at + 100] ^= 0x01;
        export_write(made, export_bytes[2], (size_t)size);
        export_refused(&away, fr, &config, made, "photographs do not match");

        memcpy(export_bytes[2], export_bytes[1], (size_t)size);
        export_bytes[2][33] ^= 0x01; /* the export time */
        export_write(made, export_bytes[2], (size_t)size);
        export_refused(&away, fr, &config, made, "header does not match");

        export_write(made, export_bytes[1], (size_t)size - 10);
        export_refused(&away, fr, &config, made, "cut short");

        memcpy(export_bytes[2], export_bytes[1], (size_t)size);
        memcpy(export_bytes[2] + size, "extra", 5);
        export_write(made, export_bytes[2], (size_t)size + 5);
        export_refused(&away, fr, &config, made, "added to");

        export_write(made, export_bytes[1], 20);
        export_refused(&away, fr, &config, made, "ends inside its header");

        /* Every byte of the header and its table is under the checksum
         * or checked before it: flip any one of them and the file is
         * refused, never half-read. */
        {
            size_t table_end = FR_EXPORT_HEADER_BYTES +
                               export_get32(export_bytes[1] + 24) *
                                   FR_EXPORT_ENTRY_BYTES;
            size_t i;
            i32    refused = 0, files = export_files_in(away.dir);
            for (i = 0; i < table_end; ++i) {
                memcpy(export_bytes[2], export_bytes[1], (size_t)size);
                export_bytes[2][i] ^= 0x10;
                export_write(made, export_bytes[2], (size_t)size);
                refused += fr_shelf_import(&away, fr, 0, &config, made, 0) == -1;
            }
            CHECK(refused == (i32)table_end);
            CHECK(export_files_in(away.dir) == files);
        }
    }

    /* Nothing left half-written anywhere. */
    snprintf(path, sizeof(path), "%s/echo.friendlocal.part", test_dir);
    CHECK(platform_file_size(path) == -1);

    fr_shelf_end(&away);
    fr_shelf_end(&home);
    fr_virtual_release(&space);
}

int main(void)
{
    char command[600];
    int  status;

    snprintf(test_dir, sizeof(test_dir), "/tmp/fr_platform_tests_%d",
             (int)getpid());
    mkdir(test_dir, 0700);

    run("shelf", test_shelf);
    run("net begin", test_net_begin);
    run("disk", test_disk);
    run("shelf photos", test_shelf_photos);
    run("export", test_export);

    status = test_finish();
    snprintf(command, sizeof(command), "rm -rf '%s'", test_dir);
    if (system(command) != 0) printf("  could not remove %s\n", test_dir);
    return status;
}
