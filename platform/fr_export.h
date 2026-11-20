/* fr_export.h — a friend in one file, to take to another machine.
 *
 * A friend on the shelf is two files: the .friend that friend_save wrote,
 * and the .blob its photographs are in. Copying one without the other
 * loses the pictures, so an export is both of them in a single
 * <name>.friendlocal file, and an import puts them back as a friend of
 * its own beside whatever is already there.
 *
 * The layout, little-endian throughout:
 *
 *     header     64 bytes (FR_EXPORT_HEADER_BYTES in version 1)
 *       0  12  magic "friend-local"
 *      12   4  header_bytes     how long this header is
 *      16   4  version          the format version that wrote the file
 *      20   4  oldest_reader    the lowest version that can read it
 *      24   4  section_count
 *      28   4  flags            none yet; written 0, ignored
 *      32   8  exported_unix_ms
 *      40   8  file_bytes       the whole file, so a short copy is caught
 *      48   4  crc32            of the header (this field as zero) and
 *                               the section table
 *      52  12  zero
 *     section table, section_count entries of 32 bytes
 *       0   4  tag              four ASCII letters
 *       4   4  flags            FR_EXPORT_REQUIRED: a reader that does
 *                               not know the tag must refuse the file
 *       8   8  offset           from the start of the file, 8-aligned
 *      16   8  size
 *      24   4  crc32            of the section's bytes
 *      28   4  zero
 *     sections
 *       FRND  required. Exactly what friend_save wrote, which carries its
 *             own version: friend_load says whether this build can read
 *             it, and the import asks before writing anything down.
 *       BLOB  required when present. The photo file, whole. A friend
 *             with no photographs has none.
 *
 * Versioning. Every change to what is written bumps FR_EXPORT_VERSION.
 * A change an older reader can live with — a new optional section, a
 * longer header — leaves oldest_reader where it was, so a file from a
 * newer build still opens in an older one, which reads the parts it
 * knows and skips the rest. A change it would misread bumps
 * oldest_reader too, and an older build then refuses the file and says
 * it needs updating, rather than guessing. A reader never needs the
 * newest header: header_bytes says where the table starts.
 *
 * Nothing is written anywhere until the whole file has been checked:
 * magic, versions, sizes, every checksum, and the friend inside. A file
 * that fails any of those is refused with the reason. POSIX.
 */

#ifndef FR_EXPORT_H
#define FR_EXPORT_H

#include <stddef.h>
#include <stdint.h>

#include "fr_disk.h"

#define FR_EXPORT_SUFFIX ".friendlocal"

/* What this build writes, and the oldest build that can read what it
 * writes. A file is readable here when its oldest_reader is at most
 * FR_EXPORT_VERSION. */
#define FR_EXPORT_VERSION 1u
#define FR_EXPORT_OLDEST_READER 1u

#define FR_EXPORT_MAGIC "friend-local"
#define FR_EXPORT_MAGIC_BYTES 12u
#define FR_EXPORT_HEADER_BYTES 64u
#define FR_EXPORT_ENTRY_BYTES 32u
/* More than this many sections is a damaged file, not a big friend. */
#define FR_EXPORT_SECTIONS_MAX 64u

#define FR_EXPORT_REQUIRED 1u

#define FR_EXPORT_TAG(a, b, c, d)                                          \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) |               \
     ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))
#define FR_EXPORT_FRIEND FR_EXPORT_TAG('F', 'R', 'N', 'D')
#define FR_EXPORT_PHOTOS FR_EXPORT_TAG('B', 'L', 'O', 'B')

/* An export, checked and read in place. The two views point into the
 * mapped file and last until fr_export_close. */
typedef struct Fr_Export {
    Fr_Mapped        mapped;
    uint32_t         version;
    uint32_t         oldest_reader;
    uint64_t         exported_unix_ms;
    const void      *friend_bytes;
    size_t           friend_size;
    const void      *photos; /* null when the friend has none */
    size_t           photos_size;
    Friend_Save_Info info; /* what the friend inside says about itself */
} Fr_Export;

/* Whether the front of a file says it is an export, for telling one
 * apart from a zip or a saved friend. It does not check the rest. */
int32_t fr_export_looks_like(const void *bytes, size_t size);

/* friend_path's friend, and photos_path's photographs if that file
 * exists, written to out_path as one file. Atomic, like a save: until it
 * has all been written and synced, out_path is whatever it was. */
int32_t fr_export_write(Fr_Disk *disk, const char *friend_path,
                        const char *photos_path, const char *out_path,
                        uint64_t now_unix_ms);

/* Maps path and checks all of it. Returns 1 with *out filled in, or 0
 * with the reason in disk's error and nothing to close. */
int32_t fr_export_open(Fr_Disk *disk, const char *path, Fr_Export *out);
void    fr_export_close(Fr_Export *e);

/* CRC-32 (the zip and PNG one), continued from crc; start at 0. */
uint32_t fr_export_crc32(uint32_t crc, const void *bytes, size_t size);

#endif /* FR_EXPORT_H */
