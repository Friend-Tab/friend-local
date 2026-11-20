/* fr_export.c — see fr_export.h. */

#include "fr_export.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ *
 *  Bytes
 * ------------------------------------------------------------------ */

static void fr_export_put32(uint8_t *at, uint32_t v)
{
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8);
    at[2] = (uint8_t)(v >> 16);
    at[3] = (uint8_t)(v >> 24);
}

static void fr_export_put64(uint8_t *at, uint64_t v)
{
    fr_export_put32(at, (uint32_t)v);
    fr_export_put32(at + 4, (uint32_t)(v >> 32));
}

static uint32_t fr_export_get32(const uint8_t *at)
{
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) |
           ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static uint64_t fr_export_get64(const uint8_t *at)
{
    return (uint64_t)fr_export_get32(at) |
           ((uint64_t)fr_export_get32(at + 4) << 32);
}

uint32_t fr_export_crc32(uint32_t crc, const void *bytes, size_t size)
{
    static uint32_t table[256];
    static int32_t  made;
    const uint8_t  *at = (const uint8_t *)bytes;
    size_t          i;

    if (!made) {
        uint32_t n, k, c;
        for (n = 0; n < 256u; ++n) {
            c = n;
            for (k = 0; k < 8u; ++k) {
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        made = 1;
    }
    crc = ~crc;
    for (i = 0; i < size; ++i) crc = table[(crc ^ at[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

static size_t fr_export_pad(size_t size) { return (8u - (size & 7u)) & 7u; }

/* The header's checksum: the header with its own checksum field read as
 * zero, then the table. */
static uint32_t fr_export_header_crc(const uint8_t *header,
                                     uint32_t header_bytes,
                                     const uint8_t *table, size_t table_bytes)
{
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    uint32_t             crc     = 0;
    crc = fr_export_crc32(crc, header, 48u);
    crc = fr_export_crc32(crc, zero, 4u);
    crc = fr_export_crc32(crc, header + 52, header_bytes - 52u);
    return fr_export_crc32(crc, table, table_bytes);
}

static int32_t fr_export_same_file(const char *a, const char *b)
{
    struct stat x, y;
    if (strcmp(a, b) == 0) return 1;
    if (stat(a, &x) != 0 || stat(b, &y) != 0) return 0;
    return x.st_dev == y.st_dev && x.st_ino == y.st_ino;
}

int32_t fr_export_looks_like(const void *bytes, size_t size)
{
    return size >= FR_EXPORT_MAGIC_BYTES &&
           memcmp(bytes, FR_EXPORT_MAGIC, FR_EXPORT_MAGIC_BYTES) == 0;
}

/* ------------------------------------------------------------------ *
 *  Writing
 * ------------------------------------------------------------------ */

int32_t fr_export_write(Fr_Disk *disk, const char *friend_path,
                        const char *photos_path, const char *out_path,
                        uint64_t now_unix_ms)
{
    static const uint8_t zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t              head[FR_EXPORT_HEADER_BYTES +
                 2u * FR_EXPORT_ENTRY_BYTES];
    Fr_Disk_Part         parts[5];
    Friend_Save_Info     info;
    Fr_Mapped            friend_file, photos;
    struct stat          status;
    uint32_t             count;
    size_t               table_end, at;
    int32_t              n = 0, ok = 0;

    if (fr_export_same_file(out_path, friend_path) ||
        (photos_path && fr_export_same_file(out_path, photos_path))) {
        fr_disk_say(disk, "%s is the friend's own file, and an export "
                          "cannot be written over it", out_path);
        return 0;
    }
    friend_file = fr_disk_map(friend_path);
    if (!friend_file.data) {
        fr_disk_say(disk, "cannot read %s", friend_path);
        return 0;
    }
    if (friend_save_peek(friend_file.data, friend_file.size, &info) !=
        FRIEND_OK) {
        fr_disk_say(disk, "%s is not a friend this can export", friend_path);
        fr_disk_unmap(friend_file);
        return 0;
    }
    photos.data = 0;
    photos.size = 0;
    if (photos_path) {
        photos = fr_disk_map(photos_path);
        /* No photo file, or an empty one, is a friend with no pictures;
         * one that is there and will not open is not. */
        if (!photos.data && stat(photos_path, &status) == 0 &&
            status.st_size > 0) {
            fr_disk_say(disk, "cannot read %s", photos_path);
            fr_disk_unmap(friend_file);
            return 0;
        }
    }

    count     = photos.data ? 2u : 1u;
    table_end = FR_EXPORT_HEADER_BYTES + count * FR_EXPORT_ENTRY_BYTES;
    memset(head, 0, sizeof(head));
    memcpy(head, FR_EXPORT_MAGIC, FR_EXPORT_MAGIC_BYTES);
    fr_export_put32(head + 12, FR_EXPORT_HEADER_BYTES);
    fr_export_put32(head + 16, FR_EXPORT_VERSION);
    fr_export_put32(head + 20, FR_EXPORT_OLDEST_READER);
    fr_export_put32(head + 24, count);
    fr_export_put64(head + 32, now_unix_ms);

    /* The friend, then the photographs, each on an 8-byte boundary. */
    at = table_end;
    {
        uint8_t *e = head + FR_EXPORT_HEADER_BYTES;
        fr_export_put32(e, FR_EXPORT_FRIEND);
        fr_export_put32(e + 4, FR_EXPORT_REQUIRED);
        fr_export_put64(e + 8, at);
        fr_export_put64(e + 16, friend_file.size);
        fr_export_put32(e + 24, fr_export_crc32(0, friend_file.data,
                                                friend_file.size));
        at += friend_file.size + fr_export_pad(friend_file.size);
    }
    if (photos.data) {
        uint8_t *e = head + FR_EXPORT_HEADER_BYTES + FR_EXPORT_ENTRY_BYTES;
        fr_export_put32(e, FR_EXPORT_PHOTOS);
        fr_export_put32(e + 4, FR_EXPORT_REQUIRED);
        fr_export_put64(e + 8, at);
        fr_export_put64(e + 16, photos.size);
        fr_export_put32(e + 24, fr_export_crc32(0, photos.data, photos.size));
        at += photos.size + fr_export_pad(photos.size);
    }
    fr_export_put64(head + 40, at);
    fr_export_put32(head + 48,
                    fr_export_header_crc(head, FR_EXPORT_HEADER_BYTES,
                                         head + FR_EXPORT_HEADER_BYTES,
                                         table_end - FR_EXPORT_HEADER_BYTES));

    parts[n].bytes   = head;
    parts[n++].size  = table_end;
    parts[n].bytes   = friend_file.data;
    parts[n++].size  = friend_file.size;
    parts[n].bytes   = zero;
    parts[n++].size  = fr_export_pad(friend_file.size);
    if (photos.data) {
        parts[n].bytes  = photos.data;
        parts[n++].size = photos.size;
        parts[n].bytes  = zero;
        parts[n++].size = fr_export_pad(photos.size);
    }
    if (!fr_disk_write_parts(out_path, parts, n)) {
        fr_disk_say(disk, "cannot write %s: %s", out_path, strerror(errno));
    } else {
        ok = 1;
    }
    fr_disk_unmap(photos);
    fr_disk_unmap(friend_file);
    return ok;
}

/* ------------------------------------------------------------------ *
 *  Reading
 * ------------------------------------------------------------------ */

static int32_t fr_export_refuse(Fr_Disk *disk, Fr_Export *e)
{
    (void)disk;
    fr_disk_unmap(e->mapped);
    memset(e, 0, sizeof(*e));
    return 0;
}

int32_t fr_export_open(Fr_Disk *disk, const char *path, Fr_Export *out)
{
    const uint8_t *file;
    uint64_t       size, file_bytes, table_end;
    uint32_t       header_bytes, count, i;
    int32_t        have_friend = 0, have_photos = 0;

    memset(out, 0, sizeof(*out));
    out->mapped = fr_disk_map(path);
    if (!out->mapped.data) {
        fr_disk_say(disk, "there is nothing to read in %s", path);
        return 0;
    }
    file = (const uint8_t *)out->mapped.data;
    size = out->mapped.size;

    if (!fr_export_looks_like(file, (size_t)size)) {
        fr_disk_say(disk, "%s is not a friend-local export", path);
        return fr_export_refuse(disk, out);
    }
    if (size < FR_EXPORT_HEADER_BYTES) {
        fr_disk_say(disk, "%s is cut short: it ends inside its header", path);
        return fr_export_refuse(disk, out);
    }
    header_bytes       = fr_export_get32(file + 12);
    out->version       = fr_export_get32(file + 16);
    out->oldest_reader = fr_export_get32(file + 20);
    count              = fr_export_get32(file + 24);
    out->exported_unix_ms = fr_export_get64(file + 32);
    file_bytes         = fr_export_get64(file + 40);

    /* Whether this build may read it at all comes before anything else
     * about it: a newer layout may put everything else elsewhere. */
    if (out->oldest_reader > FR_EXPORT_VERSION) {
        fr_disk_say(disk,
                    "%s was exported by a newer friend-local (export "
                    "format %u, readable from format %u on); this one "
                    "reads format %u, so update it to open this friend",
                    path, (unsigned)out->version,
                    (unsigned)out->oldest_reader,
                    (unsigned)FR_EXPORT_VERSION);
        return fr_export_refuse(disk, out);
    }
    if (out->version == 0 || out->oldest_reader == 0 ||
        out->oldest_reader > out->version ||
        header_bytes < FR_EXPORT_HEADER_BYTES || header_bytes > size ||
        count == 0 || count > FR_EXPORT_SECTIONS_MAX) {
        fr_disk_say(disk, "%s is damaged: its header does not make sense",
                    path);
        return fr_export_refuse(disk, out);
    }
    if (file_bytes != size) {
        fr_disk_say(disk,
                    "%s is %llu bytes and was written as %llu: it was %s on "
                    "the way here",
                    path, (unsigned long long)size,
                    (unsigned long long)file_bytes,
                    size < file_bytes ? "cut short" : "added to");
        return fr_export_refuse(disk, out);
    }
    table_end = (uint64_t)header_bytes + (uint64_t)count * FR_EXPORT_ENTRY_BYTES;
    if (table_end > size) {
        fr_disk_say(disk, "%s is damaged: its table of contents runs past "
                          "its end", path);
        return fr_export_refuse(disk, out);
    }
    if (fr_export_get32(file + 48) !=
        fr_export_header_crc(file, header_bytes, file + header_bytes,
                             (size_t)(table_end - header_bytes))) {
        fr_disk_say(disk, "%s is damaged: its header does not match its "
                          "checksum", path);
        return fr_export_refuse(disk, out);
    }

    for (i = 0; i < count; ++i) {
        const uint8_t *entry  = file + header_bytes + i * FR_EXPORT_ENTRY_BYTES;
        uint32_t       tag    = fr_export_get32(entry);
        uint32_t       flags  = fr_export_get32(entry + 4);
        uint64_t       offset = fr_export_get64(entry + 8);
        uint64_t       length = fr_export_get64(entry + 16);
        uint32_t       crc    = fr_export_get32(entry + 24);
        int32_t        known  = tag == FR_EXPORT_FRIEND ||
                         tag == FR_EXPORT_PHOTOS;

        if (!known) {
            if (flags & FR_EXPORT_REQUIRED) {
                fr_disk_say(disk,
                            "%s holds a part this friend-local does not know "
                            "(%c%c%c%c) and cannot do without; update it to "
                            "open this friend",
                            path, (char)(tag & 0xFFu),
                            (char)((tag >> 8) & 0xFFu),
                            (char)((tag >> 16) & 0xFFu),
                            (char)((tag >> 24) & 0xFFu));
                return fr_export_refuse(disk, out);
            }
            continue; /* written by a newer build, and safe to pass by */
        }
        if (offset < table_end || offset > size || length > size - offset) {
            fr_disk_say(disk, "%s is damaged: a part of it runs past its end",
                        path);
            return fr_export_refuse(disk, out);
        }
        if (fr_export_crc32(0, file + offset, (size_t)length) != crc) {
            fr_disk_say(disk, "%s is damaged: its %s do not match their "
                              "checksum",
                        path, tag == FR_EXPORT_FRIEND ? "memories and messages"
                                                      : "photographs");
            return fr_export_refuse(disk, out);
        }
        if (tag == FR_EXPORT_FRIEND) {
            if (have_friend++) break;
            out->friend_bytes = file + offset;
            out->friend_size  = (size_t)length;
        } else {
            if (have_photos++) break;
            out->photos      = length > 0 ? file + offset : 0;
            out->photos_size = (size_t)length;
        }
    }
    if (have_friend != 1 || have_photos > 1) {
        fr_disk_say(disk, "%s is damaged: it holds %s", path,
                    have_friend == 0 ? "no friend" : "a part twice");
        return fr_export_refuse(disk, out);
    }
    if (friend_save_peek(out->friend_bytes, out->friend_size, &out->info) !=
        FRIEND_OK) {
        fr_disk_say(disk,
                    "the friend in %s was saved by a friend-local this one "
                    "cannot read; update it to open this friend",
                    path);
        return fr_export_refuse(disk, out);
    }
    return 1;
}

void fr_export_close(Fr_Export *e)
{
    fr_disk_unmap(e->mapped);
    memset(e, 0, sizeof(*e));
}
