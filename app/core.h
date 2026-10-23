#ifndef BTA_CORE_H
#define BTA_CORE_H
#include "bta_types.h"
#include "protocol.h"
/* Core link-state logic, kept pure so a PC unit test can check every branch
 * without a Bluetooth driver. These derive the truthful state a UI must show
 * from the service's own flags; a UI must never infer it from "selected" or
 * "wanted" (a selected device may be offline or retrying). The worker holds the
 * flags and calls these with their current values. */

/* The one place the connection's real state is decided. */
static int bta_link_state(int streaming, int wanted, int phase, int dropped, int failed) {
    if (streaming) return BT_LINK_STREAMING;
    if (!wanted) return BT_LINK_IDLE;
    if (phase == 4) return BT_LINK_SEARCHING;
    if (dropped) return BT_LINK_LOST;
    if (failed) return BT_LINK_FAILED;
    return BT_LINK_CONNECTING;
}

/* Milliseconds until the next automatic connect attempt, or 0 when none is
 * pending (streaming, mid-attempt, or no retry scheduled). */
static int bta_retry_in_ms(int wanted, int streaming, int phase, u64 next_try, u64 now) {
    return (wanted && !streaming && !phase && next_try > now) ? (int)((next_try - now) / 1000) : 0;
}

/* While streaming, milliseconds since a packet was last sent (clamped). */
static int bta_quiet_for_ms(int streaming, u64 ok_at, u64 now) {
    if (!streaming || now < ok_at) return 0;
    u64 ms = (now - ok_at) / 1000;
    return ms > 2000000000UL ? 2000000000 : (int)ms;
}
#endif
