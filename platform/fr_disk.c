/* fr_disk.c — see fr_disk.h. */

#include "fr_disk.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int32_t fr_disk_begin(Fr_Disk *disk)
{
    disk->error[0] = 0;
    if (!fr_virtual_reserve(&disk->save_space, FR_DISK_SAVE_MAX)) {
        fr_disk_say(disk, "could not reserve %u MiB to write friends out of",
                    (unsigned)(FR_DISK_SAVE_MAX / (1024u * 1024u)));
        return 0;
    }
    return 1;
}

void fr_disk_end(Fr_Disk *disk) { fr_virtual_release(&disk->save_space); }

const char *fr_disk_error(const Fr_Disk *disk) { return disk->error; }

void fr_disk_say(Fr_Disk *disk, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(disk->error, sizeof(disk->error), format, args);
    va_end(args);
}

static void fr_disk_say_library(Fr_Disk *disk, Friend_Context *fr)
{
    Friend_String why = friend_last_error(fr);
    fr_disk_say(disk, "%.*s", (int)why.len, why.data ? why.data : "");
}

Fr_Mapped fr_disk_map(const char *path)
{
    Fr_Mapped   mapped;
    struct stat info;
    int         fd;
    void       *at;

    mapped.data = 0;
    mapped.size = 0;
    fd          = open(path, O_RDONLY);
    if (fd < 0) return mapped;
    if (fstat(fd, &info) != 0 || info.st_size <= 0) {
        close(fd);
        return mapped;
    }
    at = mmap(0, (size_t)info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (at == MAP_FAILED) return mapped;
    mapped.data = (const char *)at;
    mapped.size = (size_t)info.st_size;
    return mapped;
}

void fr_disk_unmap(Fr_Mapped mapped)
{
    if (mapped.data) munmap((void *)mapped.data, mapped.size);
}

int32_t fr_disk_path(char *out, const char *format, ...)
{
    va_list args;
    int     n;
    va_start(args, format);
    n = vsnprintf(out, FR_PATH_MAX, format, args);
    va_end(args);
    if (n < 0 || n >= FR_PATH_MAX) {
        out[0] = 0;
        errno  = ENAMETOOLONG;
        return 0;
    }
    return 1;
}

static int32_t fr_disk_sync(int fd)
{
#if defined(__APPLE__)
    /* Not every file system Apple mounts has it; those have fsync. */
    if (fcntl(fd, F_FULLFSYNC) == 0) return 1;
#endif
    return fsync(fd) == 0;
}

/* The directory a path is in, written down, so that a name made or
 * moved in it is there after a crash too. Some file systems cannot sync
 * a directory at all and say EINVAL, which leaves nothing to do. */
static int32_t fr_disk_sync_folder(const char *path)
{
    char        folder[FR_PATH_MAX];
    const char *slash = strrchr(path, '/');
    int         fd, saved;
    int32_t     ok;

    if (!slash) {
        folder[0] = '.';
        folder[1] = 0;
    } else if (!fr_disk_path(folder, "%.*s",
                             slash == path ? 1 : (int)(slash - path), path)) {
        return 0;
    }
    fd = open(folder, O_RDONLY);
    if (fd < 0) return 0;
    ok    = fr_disk_sync(fd) || errno == EINVAL;
    saved = errno;
    close(fd);
    errno = saved;
    return ok;
}

int32_t fr_disk_move(const char *from, const char *to)
{
    if (rename(from, to) != 0) return 0;
    (void)fr_disk_sync_folder(to);
    (void)fr_disk_sync_folder(from);
    return 1;
}

int32_t fr_disk_write_atomic(const char *path, const void *bytes, size_t size)
{
    Fr_Disk_Part part;
    part.bytes = bytes;
    part.size  = size;
    return fr_disk_write_parts(path, &part, 1);
}

int32_t fr_disk_write_parts(const char *path, const Fr_Disk_Part *parts,
                            int32_t count)
{
    char    temp[FR_PATH_MAX];
    int     fd, saved;
    int32_t i;

    if (!fr_disk_path(temp, "%s.part", path)) return 0;
    fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 0;
    for (i = 0; i < count; ++i) {
        const uint8_t *at   = (const uint8_t *)parts[i].bytes;
        size_t         left = parts[i].size;
        while (left > 0) {
            ssize_t did = write(fd, at, left);
            if (did < 0 && errno == EINTR) continue;
            if (did <= 0) {
                if (did == 0) errno = ENOSPC;
                saved = errno;
                close(fd);
                unlink(temp);
                errno = saved;
                return 0;
            }
            at += did;
            left -= (size_t)did;
        }
    }
    if (!fr_disk_sync(fd)) {
        saved = errno;
        close(fd);
        unlink(temp);
        errno = saved;
        return 0;
    }
    if (close(fd) != 0 || rename(temp, path) != 0) {
        saved = errno;
        unlink(temp);
        errno = saved;
        return 0;
    }
    return fr_disk_sync_folder(path);
}

/* ------------------------------------------------------------------ *
 *  What the friend asks for
 * ------------------------------------------------------------------ */

static void fr_disk_request_failed(Friend_Context *fr, uint64_t id,
                                   const char *what, int error)
{
    char          why[256];
    Friend_String text;
    int           n = snprintf(why, sizeof(why), "%s: %s", what,
                               strerror(error));
    text.data = why;
    text.len  = n < 0 ? 0 : n >= (int)sizeof(why) ? (int)sizeof(why) - 1 : n;
    friend_io_failed(fr, id, text);
}

static void fr_disk_read_request(Friend_Context *fr,
                                 const Friend_Io_Request *request,
                                 const char *path)
{
    uint32_t want  = request->length < request->recv_capacity
                         ? request->length
                         : request->recv_capacity;
    uint32_t did   = 0;
    int      error = 0;
    int      fd    = open(path, O_RDONLY);

    if (fd < 0) {
        fr_disk_request_failed(fr, request->id, "that file cannot be read",
                               errno);
        return;
    }
    while (did < want) {
        ssize_t got = pread(fd, request->recv + did, (size_t)(want - did),
                            (off_t)(request->offset + did));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) error = errno;
        if (got <= 0) break;
        did += (uint32_t)got;
    }
    close(fd);
    if (did > 0) friend_io_progress(fr, request->id, did);
    if (error) {
        fr_disk_request_failed(fr, request->id, "that file cannot be read",
                               error);
    } else if (did < want) {
        friend_io_failed(fr, request->id,
                         FRIEND_LIT("that file ends before the thing being "
                                    "read out of it"));
    } else {
        friend_io_complete(fr, request->id, 0);
    }
}

/* The friend only ever appends, but it says where, so the write goes at
 * the offset it names, and a file longer than the friend thinks keeps
 * what is past the end rather than being cut back to fit. */
static void fr_disk_write_request(Friend_Context *fr,
                                  const Friend_Io_Request *request,
                                  const char *path)
{
    const uint8_t *at      = (const uint8_t *)request->body.data;
    size_t         left    = request->body.len > 0
                                 ? (size_t)request->body.len
                                 : 0;
    uint64_t       offset  = request->offset;
    int32_t        created = 0;
    int            fd      = open(path, O_RDWR);

    if (fd < 0 && errno == ENOENT) {
        fd      = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        created = 1;
    }
    if (fd < 0) {
        fr_disk_request_failed(fr, request->id,
                               "that file cannot be opened to write to",
                               errno);
        return;
    }
    while (left > 0) {
        ssize_t did = pwrite(fd, at, left, (off_t)offset);
        if (did < 0 && errno == EINTR) continue;
        if (did <= 0) {
            int error = did == 0 ? ENOSPC : errno;
            close(fd);
            fr_disk_request_failed(fr, request->id,
                                   "that file cannot be written to", error);
            return;
        }
        at += did;
        offset += (uint64_t)did;
        left -= (size_t)did;
    }
    if (!fr_disk_sync(fd)) {
        int error = errno;
        close(fd);
        fr_disk_request_failed(fr, request->id,
                               "that file cannot be written to", error);
        return;
    }
    if (close(fd) != 0 || (created && !fr_disk_sync_folder(path))) {
        fr_disk_request_failed(fr, request->id,
                               "that file cannot be written to", errno);
        return;
    }
    friend_io_complete(fr, request->id, 0);
}

/* The path is not null-terminated — no string the library hands out
 * is — so it is copied on the way to open. */
void fr_disk_request(Friend_Context *fr, const Friend_Io_Request *request)
{
    char path[FR_PATH_MAX];

    if (request->path.len <= 0) {
        friend_io_failed(fr, request->id,
                         FRIEND_LIT("that request names no file"));
        return;
    }
    if (request->path.len >= FR_PATH_MAX) {
        fr_disk_request_failed(fr, request->id, "that file cannot be opened",
                               ENAMETOOLONG);
        return;
    }
    memcpy(path, request->path.data, (size_t)request->path.len);
    path[request->path.len] = 0;
    if (request->kind == FRIEND_IO_FILE_READ) {
        fr_disk_read_request(fr, request, path);
    } else {
        fr_disk_write_request(fr, request, path);
    }
}

int32_t fr_disk_has_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name), s = strlen(suffix), i;
    if (n < s) return 0;
    for (i = 0; i < s; ++i) {
        char a = name[n - s + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* The zip's one entry, streamed out to out_path, which this makes and
 * removes again if it fails. */
static int32_t fr_disk_unzip(Fr_Disk *disk, const char *zip_path,
                             const char *out_path)
{
    Friend_Unzip     *uz = 0;
    Friend_Unzip_Step step;
    size_t            pending = 0, at = 0;
    int               zip, out;
    int32_t           ok = 0;

    if (friend_unzip_size() > sizeof(disk->unzip) ||
        friend_unzip_begin(disk->unzip, sizeof(disk->unzip), &uz) !=
            FRIEND_OK) {
        fr_disk_say(disk, "the unzip reader needs more room than it was "
                          "given");
        return 0;
    }
    zip = open(zip_path, O_RDONLY);
    if (zip < 0) {
        fr_disk_say(disk, "cannot open %s: %s", zip_path, strerror(errno));
        return 0;
    }
    out = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out < 0) {
        fr_disk_say(disk, "cannot unzip into %s: %s", out_path,
                    strerror(errno));
        close(zip);
        return 0;
    }

    memset(&step, 0, sizeof(step));
    for (;;) {
        Friend_Result result;

        if (at >= pending) {
            ssize_t got = read(zip, disk->in, sizeof(disk->in));
            if (got < 0) {
                fr_disk_say(disk, "cannot read %s: %s", zip_path,
                            strerror(errno));
                break;
            }
            pending = (size_t)got;
            at      = 0;
        }
        step.in       = disk->in + at;
        step.in_size  = pending - at;
        step.in_last  = pending == 0 ? 1 : 0;
        step.out      = disk->out;
        step.out_size = sizeof(disk->out);

        result = friend_unzip_push(uz, &step);
        if (step.out_written > 0 &&
            write(out, disk->out, step.out_written) !=
                (ssize_t)step.out_written) {
            fr_disk_say(disk, "cannot write %s: the disk may be full",
                        out_path);
            break;
        }
        if (result != FRIEND_OK) {
            Friend_String why = friend_unzip_error(uz);
            fr_disk_say(disk, "%.*s", (int)why.len, why.data ? why.data : "");
            break;
        }
        at += step.in_used;
        if (step.done) {
            ok = 1;
            break;
        }
        if (step.in_used == 0 && step.out_written == 0 && pending == 0) {
            fr_disk_say(disk, "the zip ends before the friend does");
            break;
        }
    }
    if (close(out) != 0 && ok) {
        fr_disk_say(disk, "cannot write %s: the disk may be full", out_path);
        ok = 0;
    }
    close(zip);
    if (!ok) unlink(out_path);
    return ok;
}

static int32_t fr_disk_load_mapped(Fr_Disk *disk, Friend_Context *fr,
                                   Fr_Mapped mapped)
{
    if (friend_load(fr, mapped.data, mapped.size) != FRIEND_OK) {
        fr_disk_say_library(disk, fr);
        return 0;
    }
    return 1;
}

int32_t fr_disk_load(Fr_Disk *disk, Friend_Context *fr, const char *path)
{
    Fr_Mapped mapped = fr_disk_map(path);
    int32_t   ok;
    if (!mapped.data) {
        fr_disk_say(disk, "cannot open %s", path);
        return 0;
    }
    ok = fr_disk_load_mapped(disk, fr, mapped);
    fr_disk_unmap(mapped);
    return ok;
}

int32_t fr_disk_save(Fr_Disk *disk, Friend_Context *fr, const char *path)
{
    size_t  needed  = friend_save_size(fr);
    size_t  written = 0;
    int32_t ok      = 0;

    if (needed > disk->save_space.reserved) {
        fr_disk_say(disk,
                    "this friend needs %.0f MiB to write out and %.0f MiB "
                    "was set aside",
                    (double)needed / (1024.0 * 1024.0),
                    (double)disk->save_space.reserved / (1024.0 * 1024.0));
        return 0;
    }
    if (!fr_virtual_commit(0, disk->save_space.base, needed)) {
        fr_disk_say(disk, "there is not enough memory to write the friend "
                          "out");
        return 0;
    }
    if (friend_save(fr, disk->save_space.base, needed, &written) !=
        FRIEND_OK) {
        fr_disk_say_library(disk, fr);
    } else if (!fr_disk_write_atomic(path, disk->save_space.base, written)) {
        fr_disk_say(disk, "cannot write %s: %s", path, strerror(errno));
    } else {
        ok = 1;
    }
    fr_virtual_decommit(disk->save_space.base, needed);
    return ok;
}

static int32_t fr_disk_is_zip(Fr_Mapped mapped)
{
    return mapped.size >= 4 && memcmp(mapped.data, "PK\003\004", 4) == 0;
}

int32_t fr_disk_import(Fr_Disk *disk, Friend_Context *fr, const char *path,
                       const char *scratch_path,
                       Friend_Import_Report *out_report)
{
    Friend_Import_Report report;
    Friend_String        json;
    Fr_Mapped            mapped = fr_disk_map(path);
    const char          *unzipped = 0;
    int32_t              ok       = 0;

    if (!mapped.data) {
        fr_disk_say(disk, "there is nothing to read in %s", path);
        return 0;
    }
    if (fr_disk_is_zip(mapped)) {
        fr_disk_unmap(mapped);
        if (!scratch_path) {
            fr_disk_say(disk, "%s is a zip, and there is nowhere to unzip it",
                        path);
            return 0;
        }
        if (strcmp(scratch_path, path) == 0) {
            fr_disk_say(disk, "%s cannot be unzipped over itself", path);
            return 0;
        }
        mapped.data = 0;
        if (fr_disk_unzip(disk, path, scratch_path)) {
            unzipped = scratch_path;
            mapped   = fr_disk_map(scratch_path);
            if (!mapped.data) {
                fr_disk_say(disk, "there is nothing in the zip %s", path);
            }
        }
    }

    if (mapped.data && mapped.size > 0x7FFFFFFFu) {
        fr_disk_say(disk, "that export is larger than two gigabytes, which "
                          "is more than one friend can be");
    } else if (mapped.data) {
        json.data = mapped.data;
        json.len  = (int32_t)mapped.size;
        if (friend_import_export(fr, json, &report) != FRIEND_OK) {
            fr_disk_say_library(disk, fr);
        } else {
            if (out_report) *out_report = report;
            ok = 1;
        }
    }
    fr_disk_unmap(mapped);
    if (unzipped) unlink(unzipped);
    return ok;
}

int32_t fr_disk_open(Fr_Disk *disk, Friend_Context *fr, const char *path,
                     const char *scratch_path,
                     Friend_Import_Report *out_report, int32_t *out_imported)
{
    Fr_Mapped        mapped = fr_disk_map(path);
    Friend_Save_Info info;
    int32_t          ok;

    if (out_imported) *out_imported = 0;
    if (!mapped.data) {
        fr_disk_say(disk, "cannot open %s", path);
        return 0;
    }
    if (friend_save_peek(mapped.data, mapped.size, &info) == FRIEND_OK) {
        ok = fr_disk_load_mapped(disk, fr, mapped);
        fr_disk_unmap(mapped);
        if (ok && out_report) memset(out_report, 0, sizeof(*out_report));
        return ok;
    }
    fr_disk_unmap(mapped);
    if (out_imported) *out_imported = 1;
    return fr_disk_import(disk, fr, path, scratch_path, out_report);
}
