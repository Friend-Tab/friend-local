/* fr_shelf.c — see fr_shelf.h. */

#include "fr_shelf.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fr_clock.h"
#include "fr_export.h"
#include "fr_pump.h"

#define FR_SHELF_SUFFIX ".friend"
#define FR_SHELF_SUFFIX_LEN 7
#define FR_SHELF_PHOTOS ".blob"
#define FR_SHELF_STEM_MAX (FR_SHELF_FILE_MAX - 16)
#define FR_SHELF_ARCHIVE "archived"

/* Where an import unzips to, and where its photographs go before it is
 * known what the friend is called. Dotted, so a scan passes them by. */
#define FR_SHELF_SCRATCH_STEM ".import"

/* The front of a save file a listing reads first: the header is
 * bounded, and its variable parts — the name and the monologue — come to
 * a little over two kilobytes at most. friend_save_peek says if it
 * needed more. */
#define FR_SHELF_PEEK_BYTES 4096u

/* How long writing the friend out for good waits for its photographs.
 * They are local writes, so a few seconds is room for hundreds of
 * pictures; a phone going to the background has about five. */
#define FR_SHELF_DRAIN_MS 3000u

/* ------------------------------------------------------------------ *
 *  Names
 * ------------------------------------------------------------------ */

/* folder/stem+suffix into out, which holds FR_PATH_MAX. */
static int32_t fr_shelf_named(Fr_Shelf *shelf, char *out, const char *folder,
                              const char *stem, const char *suffix)
{
    if (fr_disk_path(out, "%s/%s%s", folder, stem, suffix)) return 1;
    fr_disk_say(&shelf->disk, "%s/%s%s: %s", folder, stem, suffix,
                strerror(ENAMETOOLONG));
    return 0;
}

static int32_t fr_shelf_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

static int32_t fr_shelf_same_file(const char *a, const char *b)
{
    struct stat x, y;
    if (strcmp(a, b) == 0) return 1;
    if (stat(a, &x) != 0 || stat(b, &y) != 0) return 0;
    return x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

/* "Echo (imported)" becomes "echo-imported". The name is the user's; the
 * file name has to survive a file system. */
static void fr_shelf_slug(Friend_String name, char *out, size_t out_size)
{
    int32_t n    = 0;
    int32_t dash = 0;
    int32_t i;
    for (i = 0; i < name.len && n + 1 < (int32_t)out_size; ++i) {
        char c = name.data[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[n++] = c;
            dash     = 0;
        } else if (c >= 'A' && c <= 'Z') {
            out[n++] = (char)(c - 'A' + 'a');
            dash     = 0;
        } else if (n > 0 && !dash) {
            out[n++] = '-';
            dash     = 1;
        }
    }
    while (n > 0 && out[n - 1] == '-') --n;
    out[n] = 0;
    if (n == 0) snprintf(out, out_size, "friend");
}

/* A file name with its .friend taken off. */
static void fr_shelf_stem(const Fr_Shelf_Entry *entry, char *out,
                          size_t out_size)
{
    int32_t len = entry->file_len;
    if (fr_disk_has_suffix(entry->file, FR_SHELF_SUFFIX)) {
        len -= FR_SHELF_SUFFIX_LEN;
    }
    snprintf(out, out_size, "%.*s", (int)len, entry->file);
}

static int32_t fr_shelf_entry_path(Fr_Shelf *shelf, int32_t index, char *out)
{
    return fr_shelf_named(shelf, out, shelf->dir, shelf->entries[index].file,
                          "");
}

/* The first of wanted, wanted-2, wanted-3 ... that neither a friend nor
 * a photo file in folder is using: two friends may be called the same
 * thing and the one that was there first keeps its files. */
static int32_t fr_shelf_free_stem(Fr_Shelf *shelf, const char *folder,
                                  const char *wanted, char *out,
                                  size_t out_size)
{
    char    path[FR_PATH_MAX];
    int32_t attempt;

    for (attempt = 1; attempt < 1000; ++attempt) {
        if (attempt == 1) snprintf(out, out_size, "%s", wanted);
        else snprintf(out, out_size, "%s-%d", wanted, (int)attempt);
        if (!fr_shelf_named(shelf, path, folder, out, FR_SHELF_SUFFIX)) {
            return 0;
        }
        if (fr_shelf_exists(path)) continue;
        if (!fr_shelf_named(shelf, path, folder, out, FR_SHELF_PHOTOS)) {
            return 0;
        }
        if (fr_shelf_exists(path)) continue;
        return 1;
    }
    fr_disk_say(&shelf->disk, "every file name for %s in %s is taken",
                wanted, folder);
    return 0;
}

/* Where the friend in hand keeps its photographs, told to the friend.
 * Empty for nowhere. */
static int32_t fr_shelf_point(Fr_Shelf *shelf, Friend_Context *fr,
                              Friend_Config *config, const char *path)
{
    snprintf(shelf->blob_path, sizeof(shelf->blob_path), "%s", path);
    config->blob_path.data = shelf->blob_path;
    config->blob_path.len  = (int32_t)strlen(shelf->blob_path);
    if (friend_reconfigure(fr, config) != FRIEND_OK) {
        Friend_String why = friend_last_error(fr);
        fr_disk_say(&shelf->disk, "%.*s", (int)why.len,
                    why.data ? why.data : "");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 *  The list
 * ------------------------------------------------------------------ */

static uint64_t fr_shelf_mtime_ns(const struct stat *info)
{
#if defined(__APPLE__) && \
    (!defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE))
    return (uint64_t)info->st_mtimespec.tv_sec * 1000000000u +
           (uint64_t)info->st_mtimespec.tv_nsec;
#elif defined(__APPLE__)
    return (uint64_t)info->st_mtime * 1000000000u +
           (uint64_t)info->st_mtimensec;
#else
    return (uint64_t)info->st_mtim.tv_sec * 1000000000u +
           (uint64_t)info->st_mtim.tv_nsec;
#endif
}

static size_t fr_shelf_read_front(int fd, uint8_t *into, size_t want)
{
    size_t got = 0;
    while (got < want) {
        ssize_t n = pread(fd, into + got, want - got, (off_t)got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    return got;
}

/* What the front of a save file says, and when it was written. info's
 * strings point into shelf->front. */
static int32_t fr_shelf_peek(Fr_Shelf *shelf, const char *path,
                             Friend_Save_Info *info, uint64_t *out_mtime_ns)
{
    struct stat   status;
    Friend_Result result = FRIEND_ERR_BAD_DATA;
    size_t        got;
    int           fd = open(path, O_RDONLY);

    if (fd < 0) return 0;
    if (fstat(fd, &status) == 0) {
        *out_mtime_ns = fr_shelf_mtime_ns(&status);
        got = fr_shelf_read_front(fd, shelf->front, FR_SHELF_PEEK_BYTES);
        if (got > 0) result = friend_save_peek(shelf->front, got, info);
        if (result == FRIEND_ERR_TOO_SMALL && got == FR_SHELF_PEEK_BYTES &&
            info->bytes_needed > got &&
            info->bytes_needed <= sizeof(shelf->front)) {
            got    = fr_shelf_read_front(fd, shelf->front, info->bytes_needed);
            result = friend_save_peek(shelf->front, got, info);
        }
    }
    close(fd);
    return result == FRIEND_OK;
}

/* Lists it again, keeping hold of the friend in hand wherever the sort
 * puts it. Returns how many .friend files it left out. */
static int32_t fr_shelf_scan(Fr_Shelf *shelf)
{
    char           keep[FR_SHELF_FILE_MAX];
    int32_t        keep_len = 0;
    uint64_t       order[FR_SHELF_FRIENDS_MAX];
    int32_t        skipped = 0;
    DIR           *dir;
    struct dirent *item;
    int32_t        i, j;

    if (shelf->current >= 0 && shelf->current < shelf->count) {
        keep_len = shelf->entries[shelf->current].file_len;
        memcpy(keep, shelf->entries[shelf->current].file, (size_t)keep_len);
    }
    shelf->count = 0;

    dir = opendir(shelf->dir);
    if (!dir) {
        fr_disk_say(&shelf->disk, "cannot read %s: %s", shelf->dir,
                    strerror(errno));
    }
    while (dir && (item = readdir(dir)) != 0) {
        Fr_Shelf_Entry  *entry;
        Friend_Save_Info info;
        char             full[FR_PATH_MAX];
        uint64_t         mtime_ns = 0;

        if (item->d_name[0] == '.') continue;
        if (!fr_disk_has_suffix(item->d_name, FR_SHELF_SUFFIX)) continue;
        if (shelf->count >= FR_SHELF_FRIENDS_MAX ||
            strlen(item->d_name) >= FR_SHELF_FILE_MAX ||
            !fr_disk_path(full, "%s/%s", shelf->dir, item->d_name) ||
            !fr_shelf_peek(shelf, full, &info, &mtime_ns)) {
            ++skipped;
            continue;
        }

        order[shelf->count] = mtime_ns;
        entry = &shelf->entries[shelf->count++];
        memset(entry, 0, sizeof(*entry));
        entry->name_len = info.name.len < FRIEND_NAME_MAX ? info.name.len
                                                          : FRIEND_NAME_MAX;
        if (entry->name_len > 0) {
            memcpy(entry->name, info.name.data, (size_t)entry->name_len);
        }
        entry->file_len = (int32_t)snprintf(entry->file, sizeof(entry->file),
                                            "%s", item->d_name);
        entry->created_unix_ms   = info.created_unix_ms;
        entry->total_event_count = info.total_event_count;
        entry->memories          = info.memories;
        entry->modified_unix_ms  = mtime_ns / 1000000u;
    }
    if (dir) closedir(dir);

    /* Most recently touched first, which is nearly always the one you
     * came back for. Insertion sort: this list is short by construction. */
    for (i = 1; i < shelf->count; ++i) {
        Fr_Shelf_Entry held     = shelf->entries[i];
        uint64_t       held_key = order[i];
        for (j = i; j > 0 && order[j - 1] < held_key; --j) {
            shelf->entries[j] = shelf->entries[j - 1];
            order[j]          = order[j - 1];
        }
        shelf->entries[j] = held;
        order[j]          = held_key;
    }

    shelf->current = keep_len > 0 ? fr_shelf_find(shelf, keep, keep_len) : -1;
    return skipped;
}

int32_t fr_shelf_begin(Fr_Shelf *shelf, const char *dir)
{
    size_t  len = strlen(dir);
    int32_t skipped;

    memset(shelf, 0, sizeof(*shelf));
    shelf->current = -1;
    if (!fr_disk_begin(&shelf->disk)) return 0;
    while (len > 1 && dir[len - 1] == '/') --len;
    /* Room for every path made in it: archived/, a file name, a suffix. */
    if (len == 0 || len + sizeof("/" FR_SHELF_ARCHIVE "/") +
                            FR_SHELF_FILE_MAX + 16 >=
                        FR_PATH_MAX) {
        fr_disk_say(&shelf->disk, "that is not a folder friends can be "
                                  "kept in%s",
                    len == 0 ? "" : ": its path is too long");
        return 0;
    }
    memcpy(shelf->dir, dir, len);
    shelf->dir[len] = 0;
    shelf->dir_len  = (int32_t)len;
    if (mkdir(shelf->dir, 0700) != 0 && errno != EEXIST) {
        fr_disk_say(&shelf->disk, "cannot make %s: %s", shelf->dir,
                    strerror(errno));
    }
    skipped = fr_shelf_scan(shelf);
    if (skipped > 0 && !fr_shelf_error(shelf)[0]) {
        fr_disk_say(&shelf->disk,
                    "%d .friend file%s in %s %s left out: unreadable, or "
                    "past the %d friends one folder holds",
                    (int)skipped, skipped == 1 ? "" : "s", shelf->dir,
                    skipped == 1 ? "was" : "were", FR_SHELF_FRIENDS_MAX);
    }
    return 1;
}

void fr_shelf_end(Fr_Shelf *shelf) { fr_disk_end(&shelf->disk); }

const char *fr_shelf_error(const Fr_Shelf *shelf)
{
    return fr_disk_error(&shelf->disk);
}

int32_t fr_shelf_find(const Fr_Shelf *shelf, const char *file, int32_t len)
{
    int32_t i;
    for (i = 0; i < shelf->count; ++i) {
        const Fr_Shelf_Entry *e = &shelf->entries[i];
        if (e->file_len == len && memcmp(e->file, file, (size_t)len) == 0) {
            return i;
        }
    }
    return -1;
}

void fr_shelf_path(const Fr_Shelf *shelf, int32_t index, char *out,
                   size_t out_size)
{
    if (index < 0 || index >= shelf->count) {
        if (out_size > 0) out[0] = 0;
        return;
    }
    snprintf(out, out_size, "%s/%s", shelf->dir, shelf->entries[index].file);
}

/* ------------------------------------------------------------------ *
 *  Writing
 * ------------------------------------------------------------------ */

int32_t fr_shelf_behind(const Fr_Shelf *shelf, Friend_Context *fr)
{
    Friend_Stats stats = friend_stats(fr);
    return shelf->unwritten || stats.unsaved || stats.photos_waiting > 0;
}

/* The friend in hand into its file, if it has changed. */
static int32_t fr_shelf_write(Fr_Shelf *shelf, Friend_Context *fr)
{
    char        path[FR_PATH_MAX];
    struct stat info;

    if (!fr_shelf_behind(shelf, fr)) return 1;
    if (shelf->current < 0) {
        fr_disk_say(&shelf->disk, "the friend has changed, and no friend's "
                                  "file is open to write it to");
        return 0;
    }
    if (!fr_shelf_entry_path(shelf, shelf->current, path) ||
        !fr_disk_save(&shelf->disk, fr, path)) {
        shelf->unwritten     = 1;
        shelf->retry_unix_ms = fr_now_unix_ms() + FR_SHELF_RETRY_MS;
        return 0;
    }
    if (stat(path, &info) == 0) {
        shelf->entries[shelf->current].modified_unix_ms =
            fr_shelf_mtime_ns(&info) / 1000000u;
    }
    shelf->unwritten = 0;
    return 1;
}

int32_t fr_shelf_save(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net)
{
    int32_t waiting = 0;

    if (shelf->current >= 0) fr_pump_photos(fr, net, FR_SHELF_DRAIN_MS);
    if (!fr_shelf_write(shelf, fr)) return 0;
    if (shelf->current >= 0) waiting = friend_stats(fr).photos_waiting;
    if (waiting > 0) {
        /* Each one that reaches the file later is a change of its own,
         * so the friend says so then and it is saved again. */
        fr_disk_say(&shelf->disk,
                    "%d photograph%s could not be written to %s, so %s not "
                    "saved yet",
                    (int)waiting, waiting == 1 ? "" : "s", shelf->blob_path,
                    waiting == 1 ? "it is" : "they are");
        return 0;
    }
    return 1;
}

int32_t fr_shelf_save_when_quiet(Fr_Shelf *shelf, Friend_Context *fr)
{
    if (shelf->current < 0 || !fr_shelf_behind(shelf, fr)) return 1;
    if (shelf->unwritten && fr_now_unix_ms() < shelf->retry_unix_ms) {
        return 1;
    }
    /* Photographs still on their way to the file are waited for here:
     * each one that lands is a change the friend reports, and saving
     * before then would only be saving again a moment later. */
    if (!friend_stats(fr).settled || friend_stats(fr).photos_waiting > 0) {
        return 1;
    }
    return fr_shelf_write(shelf, fr);
}

/* Before the friend in hand is replaced. */
static int32_t fr_shelf_put_away(Fr_Shelf *shelf, Friend_Context *fr,
                                 Fr_Net *net)
{
    char why[FR_DISK_ERROR_MAX];
    if (shelf->current < 0 || fr_shelf_save(shelf, fr, net)) return 1;
    snprintf(why, sizeof(why), "%s", fr_shelf_error(shelf));
    fr_disk_say(&shelf->disk, "the friend that is open could not be "
                              "written out, so it is still open: %s", why);
    return 0;
}

static int32_t fr_shelf_has_room(Fr_Shelf *shelf)
{
    if (shelf->count < FR_SHELF_FRIENDS_MAX) return 1;
    fr_disk_say(&shelf->disk, "there is room for %d friends in one folder",
                FR_SHELF_FRIENDS_MAX);
    return 0;
}

/* The file name of the friend in hand, or empty. */
static void fr_shelf_held(const Fr_Shelf *shelf, char *out, size_t out_size)
{
    if (shelf->current >= 0) {
        snprintf(out, out_size, "%s", shelf->entries[shelf->current].file);
    } else if (out_size > 0) {
        out[0] = 0;
    }
}

/* The friend in hand is now this entry. */
static int32_t fr_shelf_became(Fr_Shelf *shelf, Friend_Context *fr,
                               Friend_Config *config, int32_t index)
{
    char stem[FR_SHELF_FILE_MAX];
    char photos[FR_PATH_MAX];

    fr_shelf_stem(&shelf->entries[index], stem, sizeof(stem));
    if (!fr_shelf_named(shelf, photos, shelf->dir, stem, FR_SHELF_PHOTOS) ||
        !fr_shelf_point(shelf, fr, config, photos)) {
        return 0;
    }
    shelf->current   = index;
    shelf->unwritten = 0;
    return 1;
}

/* Nothing in hand, and nowhere for photographs to go: whatever the
 * context still holds is not any entry's, and must not write into one's
 * photo file. */
static void fr_shelf_let_go(Fr_Shelf *shelf, Friend_Context *fr,
                            Friend_Config *config)
{
    shelf->current   = -1;
    shelf->unwritten = 0;
    (void)fr_shelf_point(shelf, fr, config, "");
}

/* Something after the friend in hand was put away went wrong: it is
 * opened again from the file it was just written to, and the error stays
 * what went wrong in the first place. */
static void fr_shelf_restore(Fr_Shelf *shelf, Friend_Context *fr,
                             Friend_Config *config, const char *previous)
{
    char    why[FR_DISK_ERROR_MAX];
    char    again[FR_DISK_ERROR_MAX];
    char    path[FR_PATH_MAX];
    int32_t index = previous[0] ? fr_shelf_find(shelf, previous,
                                                (int32_t)strlen(previous))
                                : -1;

    snprintf(why, sizeof(why), "%s", fr_shelf_error(shelf));
    shelf->current = -1;
    if (index >= 0 && fr_shelf_entry_path(shelf, index, path) &&
        fr_disk_load(&shelf->disk, fr, path) &&
        fr_shelf_became(shelf, fr, config, index)) {
        fr_disk_say(&shelf->disk, "%s", why);
        return;
    }
    snprintf(again, sizeof(again), "%s", fr_shelf_error(shelf));
    fr_shelf_let_go(shelf, fr, config);
    if (index >= 0) {
        fr_disk_say(&shelf->disk, "%s; and %s would not open again: %s", why,
                    previous, again);
    } else {
        fr_disk_say(&shelf->disk, "%s", why);
    }
}

/* Whatever the friend now is, into a file nothing else is using, named
 * after it. Returns the new entry. */
static int32_t fr_shelf_write_new(Fr_Shelf *shelf, Friend_Context *fr,
                                  char *out_stem, size_t stem_size)
{
    char    slug[128];
    char    leaf[FR_SHELF_FILE_MAX];
    char    path[FR_PATH_MAX];
    int32_t index;

    if (!fr_shelf_has_room(shelf)) return -1;
    fr_shelf_slug(friend_name(fr), slug, sizeof(slug));
    if (!fr_shelf_free_stem(shelf, shelf->dir, slug, out_stem, stem_size) ||
        !fr_shelf_named(shelf, path, shelf->dir, out_stem, FR_SHELF_SUFFIX) ||
        !fr_disk_save(&shelf->disk, fr, path)) {
        return -1;
    }

    shelf->current = -1;
    fr_shelf_scan(shelf);
    snprintf(leaf, sizeof(leaf), "%s" FR_SHELF_SUFFIX, out_stem);
    index = fr_shelf_find(shelf, leaf, (int32_t)strlen(leaf));
    if (index < 0) {
        fr_disk_say(&shelf->disk, "%s was written and then could not be "
                                  "read back", path);
    }
    return index;
}

int32_t fr_shelf_open(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net,
                      Friend_Config *config, int32_t index)
{
    char previous[FR_SHELF_FILE_MAX];
    char path[FR_PATH_MAX];

    if (index < 0 || index >= shelf->count) {
        fr_disk_say(&shelf->disk, "there is no friend %d here", (int)index);
        return 0;
    }
    if (index == shelf->current) return 1;
    if (!fr_shelf_put_away(shelf, fr, net)) return 0;

    fr_shelf_held(shelf, previous, sizeof(previous));
    shelf->current = -1;
    if (fr_shelf_entry_path(shelf, index, path) &&
        fr_disk_load(&shelf->disk, fr, path) &&
        fr_shelf_became(shelf, fr, config, index)) {
        return 1;
    }
    fr_shelf_restore(shelf, fr, config, previous);
    return 0;
}

static Friend_String fr_shelf_trim(Friend_String s)
{
    while (s.len > 0 && (s.data[0] == ' ' || s.data[0] == '\t' ||
                         s.data[0] == '\n' || s.data[0] == '\r')) {
        ++s.data;
        --s.len;
    }
    while (s.len > 0 &&
           (s.data[s.len - 1] == ' ' || s.data[s.len - 1] == '\t' ||
            s.data[s.len - 1] == '\n' || s.data[s.len - 1] == '\r')) {
        --s.len;
    }
    return s;
}

int32_t fr_shelf_create(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net,
                        Friend_Config *config, Friend_String name)
{
    char    previous[FR_SHELF_FILE_MAX];
    char    stem[FR_SHELF_STEM_MAX];
    int32_t index;

    name = fr_shelf_trim(name);
    if (name.len <= 0) {
        fr_disk_say(&shelf->disk, "a friend needs a name");
        return -1;
    }
    if (!fr_shelf_has_room(shelf)) return -1;
    if (!fr_shelf_put_away(shelf, fr, net)) return -1;

    fr_shelf_held(shelf, previous, sizeof(previous));
    shelf->current = -1;
    /* A friend is made at the time the last tick was given. */
    friend_tick(fr, fr_now_unix_ms());
    if (friend_begin(fr, name) != FRIEND_OK) {
        Friend_String why = friend_last_error(fr);
        fr_disk_say(&shelf->disk, "%.*s", (int)why.len,
                    why.data ? why.data : "");
    } else {
        index = fr_shelf_write_new(shelf, fr, stem, sizeof(stem));
        if (index >= 0 && fr_shelf_became(shelf, fr, config, index)) {
            return index;
        }
    }
    fr_shelf_restore(shelf, fr, config, previous);
    return -1;
}

/* Whether path starts the way an export from fr_shelf_export does. */
static int32_t fr_shelf_is_export(const char *path)
{
    uint8_t front[FR_EXPORT_MAGIC_BYTES];
    size_t  got = 0;
    int     fd  = open(path, O_RDONLY);
    if (fd < 0) return 0;
    got = fr_shelf_read_front(fd, front, sizeof(front));
    close(fd);
    return fr_export_looks_like(front, got);
}

/* A .friendlocal: checked whole before anything is written, then its
 * photographs and its friend written beside the others under a name
 * nothing is using, and opened. Nothing is converted — the friend is the
 * bytes it was saved as — so it comes back exactly as it left. */
static int32_t fr_shelf_import_export(Fr_Shelf *shelf, Friend_Context *fr,
                                      Fr_Net *net, Friend_Config *config,
                                      const char *path,
                                      Friend_Import_Report *out_report)
{
    Fr_Export export_file;
    char      previous[FR_SHELF_FILE_MAX];
    char      slug[128];
    char      stem[FR_SHELF_STEM_MAX];
    char      leaf[FR_SHELF_FILE_MAX];
    char      friend_path[FR_PATH_MAX];
    char      photos_path[FR_PATH_MAX];
    int32_t   index;

    if (!fr_shelf_has_room(shelf)) return -1;
    if (!fr_export_open(&shelf->disk, path, &export_file)) return -1;
    if (!fr_shelf_put_away(shelf, fr, net)) {
        fr_export_close(&export_file);
        return -1;
    }

    fr_shelf_slug(export_file.info.name, slug, sizeof(slug));
    if (!fr_shelf_free_stem(shelf, shelf->dir, slug, stem, sizeof(stem)) ||
        !fr_shelf_named(shelf, friend_path, shelf->dir, stem,
                        FR_SHELF_SUFFIX) ||
        !fr_shelf_named(shelf, photos_path, shelf->dir, stem,
                        FR_SHELF_PHOTOS)) {
        fr_export_close(&export_file);
        return -1;
    }
    /* The photographs first, so the friend is never on the shelf without
     * them. */
    if (export_file.photos &&
        !fr_disk_write_atomic(photos_path, export_file.photos,
                              export_file.photos_size)) {
        fr_disk_say(&shelf->disk, "cannot write %s: %s", photos_path,
                    strerror(errno));
        fr_export_close(&export_file);
        return -1;
    }
    if (!fr_disk_write_atomic(friend_path, export_file.friend_bytes,
                              export_file.friend_size)) {
        fr_disk_say(&shelf->disk, "cannot write %s: %s", friend_path,
                    strerror(errno));
        unlink(photos_path);
        fr_export_close(&export_file);
        return -1;
    }
    fr_export_close(&export_file);

    fr_shelf_scan(shelf);
    snprintf(leaf, sizeof(leaf), "%s" FR_SHELF_SUFFIX, stem);
    index = fr_shelf_find(shelf, leaf, (int32_t)strlen(leaf));

    fr_shelf_held(shelf, previous, sizeof(previous));
    shelf->current = -1;
    if (index >= 0 && fr_disk_load(&shelf->disk, fr, friend_path) &&
        fr_shelf_became(shelf, fr, config, index)) {
        if (out_report) {
            Friend_Stats stats = friend_stats(fr);
            memset(out_report, 0, sizeof(*out_report));
            out_report->memories    = stats.memories;
            out_report->connections = stats.connections;
            out_report->messages    = friend_message_count(fr);
            out_report->images      = stats.photos;
        }
        return index;
    }
    if (index < 0) {
        fr_disk_say(&shelf->disk, "%s was written and then could not be "
                                  "read back", friend_path);
    }
    /* Not a friend this can open after all: it does not stay on the
     * shelf looking like one. */
    unlink(friend_path);
    unlink(photos_path);
    fr_shelf_scan(shelf);
    fr_shelf_restore(shelf, fr, config, previous);
    return -1;
}

int32_t fr_shelf_import(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net,
                        Friend_Config *config, const char *path,
                        Friend_Import_Report *out_report)
{
    char    previous[FR_SHELF_FILE_MAX];
    char    scratch_json[FR_PATH_MAX];
    char    scratch_photos[FR_PATH_MAX];
    char    photos[FR_PATH_MAX];
    char    stem[FR_SHELF_STEM_MAX];
    int32_t index;

    if (fr_shelf_is_export(path)) {
        return fr_shelf_import_export(shelf, fr, net, config, path,
                                      out_report);
    }
    if (!fr_shelf_has_room(shelf) ||
        !fr_shelf_named(shelf, scratch_json, shelf->dir,
                        FR_SHELF_SCRATCH_STEM, ".json") ||
        !fr_shelf_named(shelf, scratch_photos, shelf->dir,
                        FR_SHELF_SCRATCH_STEM, FR_SHELF_PHOTOS)) {
        return -1;
    }
    if (fr_shelf_same_file(path, scratch_json) ||
        fr_shelf_same_file(path, scratch_photos)) {
        fr_disk_say(&shelf->disk, "%s is where an import is unpacked, and "
                                  "cannot itself be imported", path);
        return -1;
    }
    if (!fr_shelf_put_away(shelf, fr, net)) return -1;

    fr_shelf_held(shelf, previous, sizeof(previous));
    shelf->current = -1;
    /* What an import that never finished left behind. */
    unlink(scratch_json);
    unlink(scratch_photos);

    /* The photographs need somewhere to go while the import runs, or it
     * throws their pixels away; the real name follows once the friend
     * has one. */
    if (!fr_shelf_point(shelf, fr, config, scratch_photos) ||
        !fr_disk_import(&shelf->disk, fr, path, scratch_json, out_report)) {
        goto failed;
    }
    /* Best effort: any still waiting go to the real file afterwards, at
     * offsets the friend knows, which do not change when the file
     * moves. */
    fr_pump_photos(fr, net, FR_SHELF_DRAIN_MS);
    index = fr_shelf_write_new(shelf, fr, stem, sizeof(stem));
    if (index < 0) goto failed;

    if (!fr_shelf_named(shelf, photos, shelf->dir, stem, FR_SHELF_PHOTOS) ||
        (!fr_disk_move(scratch_photos, photos) && errno != ENOENT)) {
        char new_path[FR_PATH_MAX];
        if (photos[0]) {
            fr_disk_say(&shelf->disk, "cannot move %s to %s: %s",
                        scratch_photos, photos, strerror(errno));
        }
        /* Without its photographs it is not the friend that was
         * imported. */
        if (fr_shelf_entry_path(shelf, index, new_path)) unlink(new_path);
        fr_shelf_scan(shelf);
        goto failed;
    }
    if (fr_shelf_became(shelf, fr, config, index)) return index;

failed:
    unlink(scratch_photos);
    fr_shelf_restore(shelf, fr, config, previous);
    return -1;
}

int32_t fr_shelf_export(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net,
                        int32_t index, const char *out_path)
{
    char stem[FR_SHELF_FILE_MAX];
    char friend_path[FR_PATH_MAX];
    char photos_path[FR_PATH_MAX];

    if (index < 0 || index >= shelf->count) {
        fr_disk_say(&shelf->disk, "there is no friend %d here", (int)index);
        return 0;
    }
    /* All of it as it is now, or nothing: an export missing what was
     * said a minute ago would be a quiet way to lose it. */
    if (index == shelf->current && !fr_shelf_save(shelf, fr, net)) {
        char why[FR_DISK_ERROR_MAX];
        snprintf(why, sizeof(why), "%s", fr_shelf_error(shelf));
        fr_disk_say(&shelf->disk, "the friend could not be saved, so it "
                                  "was not exported: %s", why);
        return 0;
    }
    fr_shelf_stem(&shelf->entries[index], stem, sizeof(stem));
    if (!fr_shelf_entry_path(shelf, index, friend_path) ||
        !fr_shelf_named(shelf, photos_path, shelf->dir, stem,
                        FR_SHELF_PHOTOS)) {
        return 0;
    }
    return fr_export_write(&shelf->disk, friend_path, photos_path, out_path,
                           fr_now_unix_ms());
}

int32_t fr_shelf_archive(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net,
                         Friend_Config *config, int32_t index)
{
    char    folder[FR_PATH_MAX];
    char    from[FR_PATH_MAX], to[FR_PATH_MAX];
    char    from_photos[FR_PATH_MAX], to_photos[FR_PATH_MAX];
    char    stem[FR_SHELF_FILE_MAX];
    char    target[FR_SHELF_FILE_MAX];
    int32_t was_open;
    int32_t moved_photos = 0;

    if (index < 0 || index >= shelf->count) {
        fr_disk_say(&shelf->disk, "there is no friend %d here", (int)index);
        return 0;
    }
    was_open = index == shelf->current;
    if (was_open && !fr_shelf_put_away(shelf, fr, net)) return 0;

    fr_shelf_stem(&shelf->entries[index], stem, sizeof(stem));
    if (!fr_shelf_named(shelf, folder, shelf->dir, FR_SHELF_ARCHIVE, "")) {
        return 0;
    }
    if (mkdir(folder, 0700) != 0 && errno != EEXIST) {
        fr_disk_say(&shelf->disk, "cannot make %s: %s", folder,
                    strerror(errno));
        return 0;
    }
    /* A friend archived earlier under the same name keeps its files. */
    if (!fr_shelf_free_stem(shelf, folder, stem, target, sizeof(target)) ||
        !fr_shelf_named(shelf, from, shelf->dir, stem, FR_SHELF_SUFFIX) ||
        !fr_shelf_named(shelf, to, folder, target, FR_SHELF_SUFFIX) ||
        !fr_shelf_named(shelf, from_photos, shelf->dir, stem,
                        FR_SHELF_PHOTOS) ||
        !fr_shelf_named(shelf, to_photos, folder, target, FR_SHELF_PHOTOS)) {
        return 0;
    }

    /* The photographs first, so that a friend can only ever be in
     * archived/ with them: if the friend will not follow, they go back.
     * A friend with none has no photo file, and that is not a failure. */
    if (fr_disk_move(from_photos, to_photos)) {
        moved_photos = 1;
    } else if (errno != ENOENT) {
        fr_disk_say(&shelf->disk, "cannot move %s: %s", from_photos,
                    strerror(errno));
        return 0;
    }
    if (!fr_disk_move(from, to)) {
        int error = errno;
        if (moved_photos && !fr_disk_move(to_photos, from_photos)) {
            fr_disk_say(&shelf->disk, "cannot move %s: %s; its photographs "
                                      "are in %s", from, strerror(error),
                        to_photos);
        } else {
            fr_disk_say(&shelf->disk, "cannot move %s: %s", from,
                        strerror(error));
        }
        return 0;
    }

    if (was_open) fr_shelf_let_go(shelf, fr, config);
    fr_shelf_scan(shelf);
    return 1;
}
