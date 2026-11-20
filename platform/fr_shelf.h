/* fr_shelf.h — a directory of friends.
 *
 * One .friend file per friend, each one what friend_save produced, and
 * beside it the friend's photographs: echo.friend keeps its pictures in
 * echo.blob. Named after the file rather than the friend, because two
 * friends can share a name, and two friends sharing one photo file
 * would be writing over each other's pictures.
 *
 * Listing reads a few kilobytes from the front of each file rather than
 * loading any of them: friend_save_peek exists for exactly this, because
 * a picker that had to load every friend to draw their names would need
 * a block each.
 *
 * The shelf knows which entry the friend in hand is, keeps its
 * photographs pointed at the right file, and writes it back when it has
 * changed. The host keeps the Friend_Config; the shelf sets its
 * blob_path, which points into the shelf, and reconfigures.
 *
 * A friend's newest photographs are in the library's memory until the
 * pump has written them to its photo file, so anything here that writes
 * the friend out for good or puts it away pumps first, through the
 * transport it is given (null fails network work that comes due). It
 * waits a few seconds at most; past that it says so, and the friend
 * stays changed.
 */

#ifndef FR_SHELF_H
#define FR_SHELF_H

#include <stdint.h>

#include "friend.h"
#include "fr_disk.h"
#include "fr_net.h"

#define FR_SHELF_FRIENDS_MAX 256
#define FR_SHELF_FILE_MAX 192

/* The most of a file's front a listing will read to find its header. */
#define FR_SHELF_FRONT_MAX (64u * 1024u)

typedef struct Fr_Shelf_Entry {
    char     name[FRIEND_NAME_MAX];
    int32_t  name_len;
    char     file[FR_SHELF_FILE_MAX]; /* its file name, which is its identity */
    int32_t  file_len;
    uint64_t created_unix_ms;
    uint64_t modified_unix_ms;
    uint64_t total_event_count;
    int32_t  memories;
} Fr_Shelf_Entry;

typedef struct Fr_Shelf {
    Fr_Disk disk;

    char    dir[FR_PATH_MAX];
    int32_t dir_len;

    /* Most recently written first. Indices hold until the next scan. */
    Fr_Shelf_Entry entries[FR_SHELF_FRIENDS_MAX];
    int32_t        count;

    /* The entry the friend in hand is, or -1. */
    int32_t current;
    /* The last write of it to its file failed, so the file is behind
     * whatever the friend says about unsaved changes, and when it may
     * be tried again without being asked. */
    int32_t  unwritten;
    uint64_t retry_unix_ms;

    char blob_path[FR_PATH_MAX];

    uint8_t front[FR_SHELF_FRONT_MAX];
} Fr_Shelf;

/* How long a save that failed waits before fr_shelf_save_when_quiet
 * tries it again. */
#define FR_SHELF_RETRY_MS 10000u

/* Makes the directory if it is not there (0700) and lists it. Returns 0
 * only when the shelf could not set itself up at all; a directory that
 * cannot be read, or holds files that are not friends, is said in the
 * error. */
int32_t fr_shelf_begin(Fr_Shelf *shelf, const char *dir);
void    fr_shelf_end(Fr_Shelf *shelf);

/* Why the last call that failed did. */
const char *fr_shelf_error(const Fr_Shelf *shelf);

/* The entry with this file name, or -1. */
int32_t fr_shelf_find(const Fr_Shelf *shelf, const char *file, int32_t len);

/* The whole path of an entry's file. */
void fr_shelf_path(const Fr_Shelf *shelf, int32_t index, char *out,
                   size_t out_size);

/* Each of these writes out the friend in hand first if it has changed,
 * and refuses — leaving it in hand — if that fails, rather than lose it.
 * Then the friend becomes the entry, its photographs go beside it, and
 * config is reconfigured to say so. If that goes wrong after the friend
 * in hand was put away, it is opened again. */

/* Loads an entry. Returns 1 or 0. */
int32_t fr_shelf_open(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net,
                      Friend_Config *config, int32_t index);

/* A friend with nothing in it but a name, in a file of its own from the
 * start so that the first thing it is told survives. Returns its index,
 * or -1. */
int32_t fr_shelf_create(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net,
                        Friend_Config *config, Friend_String name);

/* A friend from somewhere else, written to a file of its own and opened:
 * an export from friend.com (the zip, or the JSON inside it), or a
 * .friendlocal that fr_shelf_export wrote here or on another machine,
 * told apart by what is in the file. Returns its index, or -1.
 * out_report may be null; for a .friendlocal it says what the friend
 * holds, since nothing was converted. */
int32_t fr_shelf_import(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net,
                        Friend_Config *config, const char *path,
                        Friend_Import_Report *out_report);

/* An entry, photographs and all, written to out_path as one .friendlocal
 * file (see fr_export.h). The friend in hand is saved first, its
 * photographs with it, so the export is all of it as it is now; if that
 * save fails, nothing is exported. Returns 1 or 0. */
int32_t fr_shelf_export(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net,
                        int32_t index, const char *out_path);

/* Out of the list, not off the disk: the file and its photographs move
 * into archived/ together, where nothing looks for them and anybody can
 * move them back. The friend in hand is written out first and then is
 * not in hand any more. Returns 1 or 0. */
int32_t fr_shelf_archive(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net,
                         Friend_Config *config, int32_t index);

/* Whether the friend in hand has changes its file does not: the
 * friend's own Friend_Stats.unsaved, photographs not yet in the photo
 * file, or a write of it that failed. */
int32_t fr_shelf_behind(const Fr_Shelf *shelf, Friend_Context *fr);

/* Writes the friend in hand if it has changed, for when the program is
 * going away: its photographs first. Returns 1, or 0 with the reason —
 * a change with no friend in hand to write it to is one. */
int32_t fr_shelf_save(Fr_Shelf *shelf, Friend_Context *fr, Fr_Net *net);

/* The same without waiting for photographs — while any are on their
 * way it does nothing — and only when the friend is settled
 * (Friend_Stats.settled), and a save that failed is not
 * tried again for FR_SHELF_RETRY_MS: call it as often as you like.
 * Returns 0 only when a save was tried and failed. */
int32_t fr_shelf_save_when_quiet(Fr_Shelf *shelf, Friend_Context *fr);

#endif /* FR_SHELF_H */
