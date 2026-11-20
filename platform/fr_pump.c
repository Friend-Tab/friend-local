/* fr_pump.c — see fr_pump.h. */

#include "fr_pump.h"

#include <poll.h>

#include "fr_clock.h"
#include "fr_disk.h"

#define FR_PUMP_BATCH 16

static int32_t fr_pump_is_file(const Friend_Io_Request *request)
{
    return request->kind == FRIEND_IO_FILE_READ ||
           request->kind == FRIEND_IO_FILE_WRITE;
}

void fr_pump_io(Friend_Context *fr, Fr_Net *net, uint32_t wait_ms)
{
    Friend_Io_Request requests[FR_PUMP_BATCH];
    uint64_t          abandoned[FR_PUMP_BATCH];
    int32_t           n, i;

    if (!fr) return;

    do {
        n = friend_pending_io(fr, requests, FR_PUMP_BATCH);
        for (i = 0; i < n; ++i) {
            if (fr_pump_is_file(&requests[i])) {
                fr_disk_request(fr, &requests[i]);
            } else if (net) {
                fr_net_submit(net, fr, &requests[i]);
            } else {
                friend_io_failed(fr, requests[i].id,
                                 FRIEND_LIT("there is no network transport"));
            }
        }
    } while (n == FR_PUMP_BATCH);

    do {
        n = friend_abandoned_io(fr, abandoned, FR_PUMP_BATCH);
        for (i = 0; i < n && net; ++i) fr_net_abandon(net, abandoned[i]);
    } while (n == FR_PUMP_BATCH);

    if (net) {
        fr_net_poll(net, fr, wait_ms);
    } else if (wait_ms > 0) {
        poll(0, 0, (int)(wait_ms > 60000u ? 60000u : wait_ms));
    }
}

int32_t fr_pump_photos(Friend_Context *fr, Fr_Net *net, uint32_t limit_ms)
{
    uint64_t began = fr_now_unix_ms();
    int32_t  last  = -1;

    if (!fr) return 1;
    for (;;) {
        int32_t  waiting = friend_stats(fr).photos_waiting;
        uint64_t now     = fr_now_unix_ms();
        if (waiting == 0) return 1;
        if (now - began >= limit_ms) return 0;
        friend_tick(fr, now);
        /* No progress last time round is a write waiting out its retry,
         * which spinning will not hurry. */
        fr_pump_io(fr, net, waiting == last ? 10u : 0u);
        last = waiting;
    }
}
