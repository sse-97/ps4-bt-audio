#include "test.h"
#include "core.h"

void test_core(void) {
    /* bta_link_state: each branch and the precedence between them.
     * Args: (streaming, wanted, phase, dropped, failed). */
    CHECK_EQ(bta_link_state(1, 1, 4, 1, 1), BT_LINK_STREAMING); /* streaming wins over all */
    CHECK_EQ(bta_link_state(0, 0, 4, 1, 1), BT_LINK_IDLE);      /* not wanted wins over phase/flags */
    CHECK_EQ(bta_link_state(0, 1, 4, 1, 1), BT_LINK_SEARCHING); /* scanning (phase 4) */
    CHECK_EQ(bta_link_state(0, 1, 1, 1, 0), BT_LINK_LOST);      /* dropped mid-stream */
    CHECK_EQ(bta_link_state(0, 1, 1, 0, 1), BT_LINK_FAILED);    /* last attempt failed */
    CHECK_EQ(bta_link_state(0, 1, 1, 0, 0), BT_LINK_CONNECTING);/* attempt in progress */
    CHECK_EQ(bta_link_state(0, 1, 0, 0, 0), BT_LINK_CONNECTING);

    /* bta_retry_in_ms: only pending when wanted, idle (phase 0), not streaming
     * and a future next_try. Reported in milliseconds. */
    CHECK_EQ(bta_retry_in_ms(1, 0, 0, 5000000, 1000000), (5000000 - 1000000) / 1000);
    CHECK_EQ(bta_retry_in_ms(0, 0, 0, 5000000, 1000000), 0); /* not wanted */
    CHECK_EQ(bta_retry_in_ms(1, 1, 0, 5000000, 1000000), 0); /* streaming */
    CHECK_EQ(bta_retry_in_ms(1, 0, 1, 5000000, 1000000), 0); /* mid-attempt (phase != 0) */
    CHECK_EQ(bta_retry_in_ms(1, 0, 0, 1000000, 5000000), 0); /* next_try already passed */

    /* bta_quiet_for_ms: 0 unless streaming and now >= ok_at; clamped. */
    CHECK_EQ(bta_quiet_for_ms(0, 1000000, 5000000), 0);        /* not streaming */
    CHECK_EQ(bta_quiet_for_ms(1, 5000000, 1000000), 0);        /* now before ok_at */
    CHECK_EQ(bta_quiet_for_ms(1, 1000000, 3000000), 2000);     /* (3s - 1s)/1000 us = 2000 ms */
    CHECK_EQ(bta_quiet_for_ms(1, 0, 3000000000000ULL), 2000000000); /* clamp */
}
