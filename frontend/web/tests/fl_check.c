/* fl_check.c — the desktop's .friendlocal code, for tests/web_export.mjs
 * to hold the page's against.
 *
 *   fl_check write <friend> <photos|-> <out> <unix_ms>
 *   fl_check read <file>
 *
 * read prints "ok <memories> <friend bytes> <photo bytes>" or
 * "refused <why>". */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fr_export.h"

int main(int argc, char **argv)
{
    Fr_Disk disk;
    if (!fr_disk_begin(&disk)) return 2;
    if (argc == 6 && strcmp(argv[1], "write") == 0) {
        const char *photos = strcmp(argv[3], "-") == 0 ? 0 : argv[3];
        uint64_t    when   = strtoull(argv[5], 0, 10);
        if (!fr_export_write(&disk, argv[2], photos, argv[4], when)) {
            printf("refused %s\n", fr_disk_error(&disk));
            return 1;
        }
        printf("ok\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "read") == 0) {
        Fr_Export e;
        if (!fr_export_open(&disk, argv[2], &e)) {
            printf("refused %s\n", fr_disk_error(&disk));
            return 1;
        }
        printf("ok %d %zu %zu\n", (int)e.info.memories, e.friend_size,
               e.photos_size);
        fr_export_close(&e);
        return 0;
    }
    fprintf(stderr, "fl_check write <friend> <photos|-> <out> <ms>\n"
                    "fl_check read <file>\n");
    return 2;
}
