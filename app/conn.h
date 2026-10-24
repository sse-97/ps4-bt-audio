#ifndef BTA_CONN_H
#define BTA_CONN_H
#include "bta_types.h"
#include "protocol.h"
/* The connection state machine, kept pure so a PC unit test can check every
 * transition, backoff step and fallback without a Bluetooth driver. The worker
 * holds one `enum conn_state`, performs the console-specific driver calls, and
 * asks the helpers here for every *decision*: which public state a UI must show,
 * when the next automatic attempt is due, how long to back off, whether to give
 * up, which SBC mode to try next, and whether the console just woke from rest
 * mode. None of these touch the driver, so all of them are tested on the host.
 *
 * States (each has one job and a single timed exit; the worker drives the exits
 * from driver observations):
 *
 *   IDLE         nothing wanted, or we gave up and wait for the user
 *   RECOVERING   wanted but not connected: waiting out the backoff before the
 *                next attempt (also the resting point between attempts)
 *   DISCOVERING  scanning because the target is not in the driver's cache yet
 *   CONNECTING   link requested, waiting for the driver to report it up
 *   PAIRING      the device asked for a PIN; waiting for the user to supply it
 *   CONFIGURING  link up, three-slot packet types requested, settling
 *   STREAMING    audio is flowing to the headphones
 */
enum conn_state {
    CONN_IDLE = 0,
    CONN_RECOVERING,
    CONN_DISCOVERING,
    CONN_CONNECTING,
    CONN_PAIRING,
    CONN_CONFIGURING,
    CONN_STREAMING,
};

/* SBC send mode per device. Joint stereo (driver send type 4) is the format
 * headphones agree to at connect and is the default; a few models stay silent
 * on it and only play dual channel (type 3), which the driver renegotiates. The
 * last mode that actually produced sound is remembered per device (profiles.h)
 * and, when a connected device refuses audio, the other mode is tried once
 * automatically — the step that used to need a manual reconnect. */
#define SBC_JOINT 0
#define SBC_DUAL  1
static int conn_send_type(int mode) { return mode == SBC_DUAL ? 3 : 4; }
static int conn_other_mode(int mode) { return mode == SBC_DUAL ? SBC_JOINT : SBC_DUAL; }

/* The driver's link state for a device (BT_LINK_STATE): 5 and 6 mean linked. */
static int conn_link_up(long btstate) { return btstate >= 5 && btstate <= 6; }

/* What a connect does with a link the driver already holds to the device. A link
 * this service run dialled itself is reused (a rebuilt audio stream keeps it).
 * Any other link, left by an earlier run or one just dropped that is still
 * closing, can carry stale stream state: the PS4 accepts every audio packet but
 * the device plays nothing. That one is dropped first and dialled fresh. */
enum conn_link_plan { CONN_DIAL = 0, CONN_REUSE, CONN_DROP_FIRST };
static enum conn_link_plan conn_plan_link(long btstate, int own) {
    if (!conn_link_up(btstate)) return CONN_DIAL;
    return own ? CONN_REUSE : CONN_DROP_FIRST;
}

/* Reconnect backoff after a failed attempt: 2, 4, 8, 16 s for attempts 1..4,
 * then 0, which means "stop retrying and ask the user". `attempt` counts
 * consecutive failures since the last successful stream (1 = the first). */
static u64 conn_backoff_us(u32 attempt) {
    if (attempt < 1 || attempt > 4) return 0;
    return (u64)2000000 << (attempt - 1);
}
/* True once the backoff schedule is exhausted (give up, wait for the user). */
static int conn_give_up(u32 attempt) { return attempt > 4; }

/* Rest-mode resume. The worker loop ticks well under a second apart; a gap
 * longer than this means the console slept and woke (or the process was frozen),
 * so the driver handle, the in-RAM firmware patch and every socket (rest mode
 * aborts them) must be re-established. conn_resumed watches the process clock
 * alone; on 13.52 that clock stops in rest mode (a connect attempt's 30 s timeout
 * spanned 12 minutes of sleep), so conn_slept is what catches a real suspend. */
#define CONN_RESUME_GAP_US 5000000UL
static int conn_resumed(u64 last_tick, u64 now, u64 gap) {
    return last_tick && now > last_tick && now - last_tick > gap;
}
/* The wall clock runs on while the process sleeps: a tick whose wall-clock gap
 * exceeds its process-clock gap by more than `gap` slept through rest mode. A
 * wall clock set backwards never counts. */
static int conn_slept(u64 proc_gap, u64 wall_gap, u64 gap) {
    return wall_gap > proc_gap && wall_gap - proc_gap > gap;
}

/* The one place the connection's public state is decided (shown by every UI,
 * never inferred from "selected" or "wanted"). `dropped` means the link was
 * lost mid-stream; `gave_up` means the backoff ran out. */
static int conn_link_state(enum conn_state s, int dropped, int gave_up) {
    switch (s) {
    case CONN_STREAMING:   return BT_LINK_STREAMING;
    case CONN_IDLE:        return gave_up ? BT_LINK_FAILED : BT_LINK_IDLE;
    case CONN_DISCOVERING: return BT_LINK_SEARCHING;
    case CONN_RECOVERING:  return dropped ? BT_LINK_LOST : BT_LINK_FAILED;
    default:               return BT_LINK_CONNECTING; /* CONNECTING/PAIRING/CONFIGURING */
    }
}

/* Milliseconds until the next automatic attempt, or 0 when none is pending
 * (connected or streaming, given up, or no retry scheduled). */
static int conn_retry_in_ms(enum conn_state s, int gave_up, u64 next_try, u64 now) {
    return (s == CONN_RECOVERING && !gave_up && next_try > now) ? (int)((next_try - now) / 1000) : 0;
}

/* While streaming, milliseconds since a packet was last accepted (clamped). */
static int conn_quiet_for_ms(enum conn_state s, u64 ok_at, u64 now) {
    if (s != CONN_STREAMING || now < ok_at) return 0;
    u64 ms = (now - ok_at) / 1000;
    return ms > 2000000000UL ? 2000000000 : (int)ms;
}

/* Short, stable name for the current state (status JSON and diagnostics). */
static const char *conn_state_name(enum conn_state s) {
    switch (s) {
    case CONN_IDLE:        return "idle";
    case CONN_RECOVERING:  return "recovering";
    case CONN_DISCOVERING: return "discovering";
    case CONN_CONNECTING:  return "connecting";
    case CONN_PAIRING:     return "pairing";
    case CONN_CONFIGURING: return "configuring";
    case CONN_STREAMING:   return "streaming";
    }
    return "idle";
}
#endif
