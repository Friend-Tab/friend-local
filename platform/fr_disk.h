/* fr_disk.h — a friend's files: reading, writing, importing.
 *
 * The library does not touch the filesystem; it hands out bytes and
 * takes them back. Every host needs the same few things done with them,
 * and they are here:
 *
 *   - a file mapped rather than read, so a gigabyte of export JSON costs
 *     address space rather than memory: the operating system pages it in
 *     behind the importer's one forward pass and drops it again after
 *   - a friend written beside its file and then over it, so a process
 *     killed halfway through a save leaves the friend as it was
 *   - the export zip streamed out to a file 64 KiB at a time, so it never
 *     has to fit anywhere:
 *
 *         zip  --(friend_unzip)-->  a file on disk  --(mmap)-->  import
 *
 *   - the reads and writes the friend asks for by path, which is where
 *     its photographs go
 *
 * Anything reported written is on the disk, not in a cache in front of
 * it: fsync, and on Apple F_FULLFSYNC, because there fsync only reaches
 * the drive's own cache. POSIX.
 */

#ifndef FR_DISK_H
#define FR_DISK_H

#include <stddef.h>
#include <stdint.h>

#include "friend.h"
#include "fr_virtual.h"

/* The longest path anything here builds or opens. A longer one is
 * refused with ENAMETOOLONG rather than cut short into some other file's
 * name. 1024 is PATH_MAX on Apple, so no path a Mac can use is turned
 * away. */
#define FR_PATH_MAX 1024

#define FR_DISK_CHUNK (64u * 1024u)
#define FR_DISK_UNZIP_BYTES (64u * 1024u)
#define FR_DISK_ERROR_MAX 512

/* What writing a friend out may need. Reserved, not taken: a friend with
 * a few hundred memories needs a megabyte of it, and the pages are
 * handed back after every save. */
#define FR_DISK_SAVE_MAX ((size_t)512u * 1024u * 1024u)

typedef struct Fr_Mapped {
    const char *data;
    size_t      size;
} Fr_Mapped;

/* The room the work is done in, and what went wrong last. No allocation
 * after fr_disk_begin. */
typedef struct Fr_Disk {
    Fr_Virtual save_space;
    uint8_t    in[FR_DISK_CHUNK];
    uint8_t    out[FR_DISK_CHUNK];
    uint8_t    unzip[FR_DISK_UNZIP_BYTES];
    char       error[FR_DISK_ERROR_MAX];
} Fr_Disk;

/* Returns 0 if the address space to save into was refused. */
int32_t fr_disk_begin(Fr_Disk *disk);
void    fr_disk_end(Fr_Disk *disk);

/* Why the last call that returned 0 failed, in a sentence. */
const char *fr_disk_error(const Fr_Disk *disk);
void        fr_disk_say(Fr_Disk *disk, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Read-only and demand-paged. data is null when the file is missing or
 * empty. */
Fr_Mapped fr_disk_map(const char *path);
void      fr_disk_unmap(Fr_Mapped mapped);

/* A path into out, which holds FR_PATH_MAX. Returns 0, with errno
 * ENAMETOOLONG, when it does not fit. */
int32_t fr_disk_path(char *out, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Into path.part, onto the disk, then over path. 0600: this is
 * somebody's private conversations. Returns 0 and leaves path as it was
 * on any failure, with errno saying why. */
int32_t fr_disk_write_atomic(const char *path, const void *bytes,
                             size_t size);

/* The same, for a file written in pieces that are each somewhere else
 * already — a header, and files mapped rather than copied. */
typedef struct Fr_Disk_Part {
    const void *bytes;
    size_t      size;
} Fr_Disk_Part;

int32_t fr_disk_write_parts(const char *path, const Fr_Disk_Part *parts,
                            int32_t count);

/* rename, and then the directories it changed written down, so that the
 * move survives the power going. Returns 0, with errno, only when the
 * rename did not happen: a caller undoing a move needs to know whether
 * there is one to undo. */
int32_t fr_disk_move(const char *from, const char *to);

/* Case-insensitively. */
int32_t fr_disk_has_suffix(const char *name, const char *suffix);

/* A FRIEND_IO_FILE_READ or FRIEND_IO_FILE_WRITE, done now, and the friend
 * told how it went before this returns. A write is to where the friend
 * says, into a file that is made 0600 if it is not there and never
 * shortened, and it is complete only once it is on the disk. */
void fr_disk_request(Friend_Context *fr, const Friend_Io_Request *request);

/* A saved friend, loaded. */
int32_t fr_disk_load(Fr_Disk *disk, Friend_Context *fr, const char *path);

/* The friend, written to path. */
int32_t fr_disk_save(Fr_Disk *disk, Friend_Context *fr, const char *path);

/* An export from friend.com — the zip, or the friend_state.json inside
 * it, told apart by what is in the file — imported. A zip is unzipped to
 * scratch_path first, which must not exist yet — whatever is there is
 * somebody's, and is left alone — and is removed again whatever happens.
 * out_report may be null. */
int32_t fr_disk_import(Fr_Disk *disk, Friend_Context *fr, const char *path,
                       const char *scratch_path,
                       Friend_Import_Report *out_report);

/* Whichever of the two path is, by what is in it: a saved friend is
 * loaded, anything else imported. *out_imported says which, and may be
 * null. */
int32_t fr_disk_open(Fr_Disk *disk, Friend_Context *fr, const char *path,
                     const char *scratch_path,
                     Friend_Import_Report *out_report, int32_t *out_imported);

#endif /* FR_DISK_H */
