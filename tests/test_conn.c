#include "test.h"
#include "conn.h"

void test_conn(void) {
    /* Backoff schedule: 2, 4, 8, 16 s for the first four failures, then 0
     * ("give up and ask the user"). */
    CHECK_EQ(conn_backoff_us(1), 2000000);
    CHECK_EQ(conn_backoff_us(2), 4000000);
    CHECK_EQ(conn_backoff_us(3), 8000000);
    CHECK_EQ(conn_backoff_us(4), 16000000);
    CHECK_EQ(conn_backoff_us(5), 0);
    CHECK_EQ(conn_backoff_us(0), 0);
    CHECK_EQ(conn_give_up(4), 0);
    CHECK_EQ(conn_give_up(5), 1);

    /* SBC mode mapping and the one-shot flip used for automatic fallback. */
    CHECK_EQ(conn_send_type(SBC_JOINT), 4);
    CHECK_EQ(conn_send_type(SBC_DUAL), 3);
    CHECK_EQ(conn_other_mode(SBC_JOINT), SBC_DUAL);
    CHECK_EQ(conn_other_mode(SBC_DUAL), SBC_JOINT);

    /* Rest-mode resume: a gap beyond the threshold means the console woke. */
    CHECK_EQ(conn_resumed(0, 9000000, CONN_RESUME_GAP_US), 0);          /* no prior tick */
    CHECK_EQ(conn_resumed(1000000, 3000000, CONN_RESUME_GAP_US), 0);    /* 2 s gap */
    CHECK_EQ(conn_resumed(1000000, 9000000, CONN_RESUME_GAP_US), 1);    /* 8 s gap */
    CHECK_EQ(conn_resumed(9000000, 1000000, CONN_RESUME_GAP_US), 0);    /* clock went back */

    /* Public link state from the explicit state, with the dropped/gave-up flags. */
    CHECK_EQ(conn_link_state(CONN_STREAMING, 0, 0), BT_LINK_STREAMING);
    CHECK_EQ(conn_link_state(CONN_STREAMING, 1, 1), BT_LINK_STREAMING); /* streaming wins */
    CHECK_EQ(conn_link_state(CONN_IDLE, 0, 0), BT_LINK_IDLE);
    CHECK_EQ(conn_link_state(CONN_IDLE, 0, 1), BT_LINK_FAILED);         /* gave up, waiting */
    CHECK_EQ(conn_link_state(CONN_DISCOVERING, 0, 0), BT_LINK_SEARCHING);
    CHECK_EQ(conn_link_state(CONN_RECOVERING, 1, 0), BT_LINK_LOST);     /* dropped mid-stream */
    CHECK_EQ(conn_link_state(CONN_RECOVERING, 0, 0), BT_LINK_FAILED);   /* a connect failed */
    CHECK_EQ(conn_link_state(CONN_CONNECTING, 0, 0), BT_LINK_CONNECTING);
    CHECK_EQ(conn_link_state(CONN_PAIRING, 0, 0), BT_LINK_CONNECTING);
    CHECK_EQ(conn_link_state(CONN_CONFIGURING, 0, 0), BT_LINK_CONNECTING);

    /* retry_in_ms: only pending while recovering, not given up, with a future
     * next_try; reported in milliseconds. */
    CHECK_EQ(conn_retry_in_ms(CONN_RECOVERING, 0, 5000000, 1000000), (5000000 - 1000000) / 1000);
    CHECK_EQ(conn_retry_in_ms(CONN_RECOVERING, 1, 5000000, 1000000), 0); /* gave up */
    CHECK_EQ(conn_retry_in_ms(CONN_CONNECTING, 0, 5000000, 1000000), 0); /* mid-attempt */
    CHECK_EQ(conn_retry_in_ms(CONN_STREAMING, 0, 5000000, 1000000), 0);  /* streaming */
    CHECK_EQ(conn_retry_in_ms(CONN_RECOVERING, 0, 1000000, 5000000), 0); /* already due */

    /* quiet_for_ms: 0 unless streaming and now >= ok_at; clamped. */
    CHECK_EQ(conn_quiet_for_ms(CONN_RECOVERING, 1000000, 5000000), 0);   /* not streaming */
    CHECK_EQ(conn_quiet_for_ms(CONN_STREAMING, 5000000, 1000000), 0);    /* now before ok_at */
    CHECK_EQ(conn_quiet_for_ms(CONN_STREAMING, 1000000, 3000000), 2000); /* (3s-1s)/1000 */
    CHECK_EQ(conn_quiet_for_ms(CONN_STREAMING, 0, 3000000000000ULL), 2000000000); /* clamp */

    /* plan_link: dial when there is no link; reuse only a link this run dialled;
     * any other live link (an earlier run's, or one still closing) is dropped and
     * dialled afresh, since it can play nothing while accepting audio. */
    CHECK_EQ(conn_plan_link(0, 0), CONN_DIAL);
    CHECK_EQ(conn_plan_link(4, 1), CONN_DIAL);
    CHECK_EQ(conn_plan_link(7, 1), CONN_DIAL);
    CHECK_EQ(conn_plan_link(-1, 0), CONN_DIAL);       /* driver error: just dial */
    CHECK_EQ(conn_plan_link(5, 1), CONN_REUSE);
    CHECK_EQ(conn_plan_link(6, 1), CONN_REUSE);
    CHECK_EQ(conn_plan_link(5, 0), CONN_DROP_FIRST);
    CHECK_EQ(conn_plan_link(6, 0), CONN_DROP_FIRST);
    CHECK(conn_link_up(5) && conn_link_up(6) && !conn_link_up(4) && !conn_link_up(7));
}
