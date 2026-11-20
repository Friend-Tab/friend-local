/* platform_unity.c — everything in platform/, as one translation unit.
 *
 * What the library deliberately does not have — sockets and TLS, the
 * clock, address space, files, a directory of friends, the environment —
 * supplied beside it rather than inside it, so a frontend that would
 * rather bring its own can. Build it the way the library is built: this
 * one file, with platform/ and mbedTLS's headers on the include path.
 */

/* madvise and its flags are outside strict POSIX on Linux; Apple shows
 * them unless a strict standard is asked for. */
#if !defined(__APPLE__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif

#include "fr_clock.c"
#include "fr_virtual.c"
#include "fr_net.c"
#include "fr_pump.c"
#include "fr_disk.c"
#include "fr_export.c"
#include "fr_shelf.c"
#include "fr_env.c"
