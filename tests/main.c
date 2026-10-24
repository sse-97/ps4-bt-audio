#include "test.h"

/* Host unit tests for the pure service modules (json, strfmt, dsp, conn,
 * profiles, timing). They build and run on an ordinary PC with no PS4 toolchain:
 *   make test      (or: cc -I app -I tests tests/main.c tests/test_*.c -lm ...)
 * The dsp and strfmt suites include reference copies of the pre-refactor
 * worker.c logic and check the extracted helpers against them over randomised
 * inputs, so a behaviour change in the split would fail the build. */
struct test_counts g_test = {0, 0};

int main(void) {
    printf("json...\n");   test_json();
    printf("auth...\n");   test_auth();
    printf("strfmt...\n"); test_strfmt();
    printf("dsp...\n");    test_dsp();
    printf("conn...\n");   test_conn();
    printf("profiles...\n"); test_profiles();
    printf("timing...\n"); test_timing();
    printf("pacing...\n"); test_pacing();
    printf("\n%d checks, %d failures\n", g_test.checks, g_test.failures);
    return g_test.failures ? 1 : 0;
}
