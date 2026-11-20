/* fr_test.h — the harness both test programs share.
 *
 * No framework: CHECK counts and reports, run() names a test and says
 * whether it passed, and test_finish() prints the total and gives main
 * its exit status. Include it after the code under test, which supplies
 * str_eq, str_lit and the integer types. */

#ifndef FR_TEST_H
#define FR_TEST_H

#include <stdio.h>
#include "fr_base.h"

static i32         checks_run;
static i32         checks_failed;
static const char *current_test;

/* Runs before every test, for a program that has per-test state. */
static void (*test_setup)(void);

static void check_impl(b32 condition, const char *expression, int line)
{
    ++checks_run;
    if (!condition) {
        ++checks_failed;
        printf("  FAIL %s:%d  %s\n", current_test, line, expression);
    }
}

#define CHECK(expr) check_impl((expr) ? 1 : 0, #expr, __LINE__)
/* A check that has failed by being reached, and says what that means. */
#define FAIL(why) check_impl(0, why, __LINE__)

/* For a check inside a loop that runs tens of thousands of times:
 * counting every one would drown the total and reporting every failure
 * would drown the screen. The loop's test checks silent_failures once. */
static i32 silent_failures;
#define CHECK_SILENT(expr)                                                    \
    do {                                                                      \
        if (!(expr)) ++silent_failures;                                       \
    } while (0)

static void check_str_impl(String got, String want, const char *expression,
                           int line)
{
    ++checks_run;
    if (!str_eq(got, want)) {
        ++checks_failed;
        printf("  FAIL %s:%d  %s\n        got  \"%.*s\"\n        want \"%.*s\"\n",
               current_test, line, expression, (int)got.len,
               got.data ? got.data : "", (int)want.len,
               want.data ? want.data : "");
    }
}

#define CHECK_STR(got, want)                                                  \
    check_str_impl((got), str_lit(want), #got " == " want, __LINE__)

static void run(const char *name, void (*fn)(void))
{
    i32 before = checks_failed;
    current_test = name;
    if (test_setup) test_setup();
    fn();
    printf("%-34s %s\n", name, checks_failed == before ? "ok" : "FAILED");
}

static int test_finish(void)
{
    printf("\n%d checks, %d failed\n", (int)checks_run, (int)checks_failed);
    if (silent_failures) printf("and %d silent failures\n", (int)silent_failures);
    return checks_failed == 0 && silent_failures == 0 ? 0 : 1;
}

#endif /* FR_TEST_H */
