#include "test.h"

/* Host unit tests for the parts of the service that need no console: the pure
 * modules (json, strfmt, dsp, conn, profiles, timing, pacing, auth, qr), the TV
 * app's status reader (model), and the kernel resolver (kernel). They build and run
 * on an ordinary PC with no PS4 toolchain:
 *   make test      (or: cc -I app -I tests tests/main.c tests/test_*.c -o tests)
 * The dsp suite carries a reference model of the audio loops and checks the
 * extracted helpers against it over randomised inputs, so a drift in clamping,
 * volume scaling or ring wraparound fails the build. */
struct test_counts g_test = {0, 0};

int main(void) {
    printf("json...\n");   test_json();
    printf("model...\n");  test_model();
    printf("auth...\n");   test_auth();
    printf("strfmt...\n"); test_strfmt();
    printf("dsp...\n");    test_dsp();
    printf("conn...\n");   test_conn();
    printf("profiles...\n"); test_profiles();
    printf("timing...\n"); test_timing();
    printf("pacing...\n"); test_pacing();
    printf("qr...\n");     test_qr();
    printf("kernel...\n"); test_kernel();
    printf("\n%d checks, %d failures\n", g_test.checks, g_test.failures);
    return g_test.failures ? 1 : 0;
}
