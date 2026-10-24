/* Bluetooth Audio service — orchestration.
 *
 * This file wires the pieces together and owns everything that must talk to the
 * PS4: the Bluetooth and HDMI drivers, the audio capture ring, the local HTTP
 * control server and the real-time sender thread. The logic that does not need
 * the console lives in small, unit-tested headers beside it:
 *
 *   json.h     bounded JSON writer for the status and timing responses
 *   strfmt.h   message/MAC/path string helpers
 *   dsp.h      float capture -> 16-bit PCM, the test tone and the ring maths
 *   conn.h     the connection state machine: link state, backoff and SBC mode
 *   profiles.h remembered-device table and selection policy
 *   pacing.h   clock-paced sending with a bounded latency buffer
 *   timing.h   per-second send statistics (GET /api/timing)
 *   auth.h     LAN pairing and token checks for phone control
 *
 * The firmware-specific driver helpers (bt_devices.c, bt_accept_patch.c) and
 * the single firmware table (firmware.h) sit behind bt_list / bt_prepare /
 * bta_apply_patch, so no firmware offset appears here. */
#include "runtime.h"
#include "web_page.h"
#include "protocol.h"
#include "json.h"
#include "auth.h"
#include "strfmt.h"
#include "dsp.h"
#include "conn.h"
#include "profiles.h"
#include "timing.h"
#include "pacing.h"
int bta_apply_patch(void);

/* FreeBSD syscall numbers used directly through sc3/sc5. */
#define SYS_read    3
#define SYS_write   4
#define SYS_open    5
#define SYS_unlink  10
#define SYS_chmod   15
#define SYS_accept  30
#define SYS_getsockname 32
#define SYS_ioctl   54
#define SYS_socket  97
#define SYS_connect 98
#define SYS_setsockopt 105
#define SYS_gettimeofday 116
#define SYS_rename  128
#define SYS_mkdir   136

/* Bluetooth driver ioctls on /dev/bt. */
#define BT_SCAN_START  0x20046204
#define BT_SCAN_STOP   0x20046205
#define BT_LINK_STATE  0x80086203
#define BT_CONNECT     0x8008620a
#define BT_DROP_LINK   0x8008620b
#define BT_PAIR_ACCEPT 0x80106208
#define BT_PIN_REPLY   0x80186207
#define BT_SEND_AUDIO  0x80206216
/* HDMI driver ioctl on /dev/hdmi: set the mute flag. */
#define HDMI_SET_MUTE  0xc0048d05

/* Keep HDMI unmuted for this long after streaming starts so the system audio
 * mix that feeds /shm_acap.full actually activates, THEN apply the user's
 * mute preference. Starting muted leaves the capture ring empty (silence). */
#define PRIME_US 2000000
/* Automatic recovery threshold: the PS4 refused every packet for this long. */
#define REFUSED_US 4000000
/* After BTA_MAX_PAIR_FAILS wrong pairing codes, reject further attempts for this
 * long. Rate-limits a LAN brute force without a permanent lockout. */
#define PAIR_COOLDOWN_US 30000000

struct audio_req { u64 address; void *samples; u64 length; u32 mode, pad; };
/* A response is `total` bytes: the first `head` come from output (the header, and
 * the body when it fits), the rest straight from `tail`, a constant body such as
 * the page, which is larger than output. */
struct client { long fd; u64 deadline; u32 used, sent, head, total; const char *tail; int loopback; char input[2048]; char output[20000]; };
struct app {
    int (*sleep)(unsigned);
    u64 (*clock)(void);
    int (*shm)(const char *, int, int);
    void *(*map)(void *, u64, int, int, int, u64);
    int (*unmap)(void *, u64);
    long listener, bt, hdmi, shmfd;
    u8 *ring;
    u32 readpos;
    int wanted, streaming, tv, muted, quit;
    enum conn_state state;
    long patch_rc, packet_rc, connect_rc, last_rc, hdmi_rc, btstate;
    u64 next_try, deadline, scan_at, heartbeat, poll_at, net_at, packets, errors, resyncs;
    unsigned level;
    int volume;
    char message[128];
    /* Per-second send timing (see timing.h). */
    struct bta_second secs[128], cur;
    u32 sec_seq;
    u64 sec_start, last_send;
    int mode;            /* current SBC send mode: SBC_JOINT / SBC_DUAL */
    u8 modes_tried;      /* modes attempted this stream (for one-shot fallback) */
    int confirmed;       /* a packet was accepted this stream (record the mode once) */
    /* Clock-paced sending (see pacing.h). latency_mode picks the backlog target;
     * drops/underruns count trims and starved slots for the diagnostics. */
    struct bta_pace pace;
    int latency_mode;
    u64 drops, underruns;
    u64 refused_since;
    u32 relinks, relinks_total;
    u64 stream_at, test_until, test_next;
    u32 test_phase;
    int snap; /* on the next capture read, drop to the live write cursor */
    void *mutex;
    int (*mlock)(void **);
    int (*munlock)(void **);
    volatile int sender_run, sender_alive;
    int sender_prio;
    long thread_rc, prio_rc, aff_rc, pi_rc;
    /* Truthful link state (see conn.h / link_state). dropped: the link was lost
     * while streaming. give_up: the backoff schedule ran out, so we wait for the
     * user. announced: a "disconnected" notice was shown. session: this
     * connection was announced. */
    int dropped, give_up, announced, session, meter;
    u32 attempts;        /* consecutive failed attempts (drives the backoff) */
    u64 ok_at, meter_at, notify_at[3];
    unsigned meter_peak;
    struct bt_device target;
    u64 own_link;        /* address of the link this run dialled; 0 once dropped (see conn_plan_link) */
    u64 redial_at;       /* a stale link was just dropped: dial afresh at this time */
    struct bta_profiles profiles;
    int profile_dirty;   /* profiles changed in memory, persist on the next beat */
    u64 last_tick;       /* worker-loop time, for rest-mode resume detection */
    u64 last_wall;       /* wall-clock time of that tick (see conn_slept) */
    u64 listen_at;       /* earliest next re-open of the listening socket */
    u32 accept_errs;     /* accept failures in a row (a broken listener) */
    struct bt_devices devices;
    int scanning, pin_pending;
    u64 scan_end, list_at;
    /* Searching and dialling never overlap (conn.h "Sharing the radio").
     * page_at: when the outstanding dial was sent (0: none), page_end: when the
     * driver is surely done with it; page_before: the link state read just before
     * it, page_last: the last one logged, page_seen: a reading showed the attempt.
     * hold: what the radio waits for (hold_addr's dial or link, since hold_at);
     * radio_quiet_at: no search or dial starts before this. */
    u64 page_at, page_end;
    long page_before, page_last;
    int page_seen;
    enum conn_hold hold;
    u64 hold_at, hold_addr, hold_poll, radio_quiet_at;
    /* Add Device (conn.h): who opened it; resume: reconnect the device when it
     * ends; scan_wait: a search is due once the radio is free; console_at: the
     * console app's latest request; add_idle: since when nothing was searched. */
    enum conn_add adding;
    int resume, scan_wait;
    u64 console_at, add_idle;
    u64 event_at;        /* driver events logged since then (log_event) */
    u32 event_lines, stream_events;
    struct client clients[4];
    /* LAN access control (see auth.h). The console reaches the service over the
     * loopback address and always has full access; LAN clients must pair. */
    struct bta_auth auth;
    int (*getrandom)(void *, u64);
    u64 entropy; /* stirred by network timing; seeds the RNG fallback */
    /* A lan-on/off request sets these under the lock; network_tick runs the
     * resulting file write and TV notice after releasing it, so the real-time
     * sender is never blocked on flash I/O while toggling phone control. */
    int lan_act;
    char lan_code[BTA_CODE_DIGITS + 1];
    /* The console's LAN address while phone control is on (0 if unknown), shown
     * with the code so a phone knows where to go. Worker thread only. */
    u32 ip;
    u64 ip_at;
    /* Service log (see log_text): lines wait here under the lock until the
     * heartbeat appends them to SERVICE_LOG outside it. */
    char log[1024];
    u32 log_len;
    u64 log_t0, log_size;
    u64 stats_at, stream_packets, stream_resyncs; /* audio summary every 10 min (log_stats) */
    char http_seen[4][96]; /* recent request lines, so a retrying page logs each once per 10 min */
    u64 http_at[4];
    u64 status_at;       /* next status.json write (every 10 s) */
};

/* Fold an observation into the entropy pool. Cheap, called on network events so
 * the fallback RNG below depends on real, attacker-unobservable runtime timing. */
static void stir_entropy(struct app *a, u64 x) {
    a->entropy = (a->entropy ^ x) * 0x100000001b3UL; /* FNV-style mix */
    a->entropy ^= a->entropy >> 29;
}
/* Fill buf with n random bytes. Uses the kernel RNG when available; otherwise an
 * xorshift seeded from the entropy pool and fresh clock reads. The fallback is a
 * defence in depth — sceKernelGetRandomNumber is expected to resolve on 13.52. */
static void fill_random(struct app *a, u8 *buf, unsigned n) {
    if (a->getrandom && n <= 64 && a->getrandom(buf, n) == 0) return;
    u64 x = a->entropy ^ a->clock() ^ ((u64)(unsigned long)buf) ^ 0x9e3779b97f4a7c15UL;
    for (unsigned i = 0; i < n; i++) {
        x += a->clock();
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        buf[i] = (u8)(x >> 24);
    }
    a->entropy ^= x;
}
/* Reconcile the in-memory auth with the LAN_ON flag `on`. Memory only: on a
 * transition it reports what the caller must persist, so the file write (and the
 * TV notification) happen outside the state lock. Returns 1 when phone control
 * just turned on (new code copied to `code`), -1 when it just turned off, else 0. */
static int sync_lan(struct app *a, int on, char code[BTA_CODE_DIGITS + 1]) {
    code[0] = 0;
    if (on && !a->auth.enabled) {
        u8 r[4];
        fill_random(a, r, sizeof(r));
        bta_auth_enable(&a->auth, r);
        for (int i = 0; i <= BTA_CODE_DIGITS; i++) code[i] = a->auth.code[i];
        return 1;
    }
    if (!on && a->auth.enabled) { bta_auth_disable(&a->auth); return -1; }
    return 0;
}
/* The console's LAN address: the source address the network stack picks for an
 * outside destination (a UDP connect sends nothing). 0 without a network, or
 * when it isn't a private LAN address, the only kind phones are accepted from
 * (see network_tick). */
static u32 lan_address(void) {
    long fd = sc3(SYS_socket, 2, 2, 0); /* AF_INET, SOCK_DGRAM */
    if (fd < 0) return 0;
    struct sockaddr4 to = {16, 2, 0x3500, 0x08080808, {0}}; /* 8.8.8.8, port 53 */
    struct sockaddr4 me;
    u32 len = sizeof(me), ip = 0;
    memset(&me, 0, sizeof(me));
    if (sc3(SYS_connect, fd, (long)&to, sizeof(to)) >= 0 && sc3(SYS_getsockname, fd, (long)&me, (long)&len) >= 0) ip = me.addr;
    closefd(fd);
    const u8 *b = (const u8 *)&ip;
    return local_ip(b) && b[0] != 127 ? ip : 0;
}
/* Act on what sync_lan reported, with no lock held: write or remove the code
 * file, look up the console's address and announce both on the TV. */
static void persist_lan(struct app *a, int act, const char *code) {
    if (act == 1) {
        put(PAIR_CODE, code, BTA_CODE_DIGITS);
        a->ip = lan_address();
        a->ip_at = a->clock() + 30000000;
        char m[160];
        if (a->ip) {
            char ip[16];
            struct writer w = {m, 0, sizeof(m)};
            ip_text(a->ip, ip);
            append(&w, "Phone control is on. On your phone, open http://");
            append(&w, ip);
            append(&w, ":8097 and enter code ");
            append(&w, code);
            append(&w, ".");
        } else compose(m, sizeof(m), "Phone control is on. Pairing code: ", code, ". Enter it in your phone's browser.");
        notify(m);
    } else if (act == -1) {
        sc3(SYS_unlink, (long)PAIR_CODE, 0, 0);
    }
}

static void lock_state(struct app *a) { if (a->mlock) a->mlock(&a->mutex); }
static void unlock_state(struct app *a) { if (a->munlock) a->munlock(&a->mutex); }

/* Wall-clock microseconds (0 if unavailable): unlike the process clock (a->clock),
 * it runs on while the console is in rest mode. */
static u64 wall_us(void) {
    struct { long sec, usec; } tv = {0, 0};
    if (sc3(SYS_gettimeofday, (long)&tv, 0, 0) < 0 || tv.sec <= 0) return 0;
    return (u64)tv.sec * 1000000 + (u64)tv.usec;
}

/* Service log: every new status message, connection steps, failed requests and an
 * audio summary, each stamped with the UTC time of day, so what happened on the
 * console can be read back over FTP and lined up with the app's log and KLog.
 * Each start, and each SERVICE_LOG_CAP bytes, moves the log to SERVICE_LOG_OLD:
 * at most two files, and the newest lines are always kept. */
#define SERVICE_LOG "/data/bta_audio/service.log"
#define SERVICE_LOG_OLD "/data/bta_audio/service-previous.log"
#define SERVICE_LOG_CAP 262144
static void log_put(struct app *a, const char *s) {
    for (; *s && a->log_len + 2 < sizeof(a->log); s++) a->log[a->log_len++] = *s;
}
static void log_text(struct app *a, const char *s) {
    char t[24];
    u64 w = wall_us();
    if (w) { /* "[13:44:02] " */
        t[0] = '[';
        clock_text(w / 1000000, t + 1);
        t[9] = ']'; t[10] = ' '; t[11] = 0;
        log_put(a, t);
    } else { /* no wall clock: "[+123.4] ", seconds since the service started */
        u64 ds = (a->clock() - a->log_t0) / 100000; /* tenths of a second */
        unsigned i = sizeof(t) - 1;
        t[i] = 0;
        t[--i] = ' '; t[--i] = ']'; t[--i] = (char)('0' + ds % 10); t[--i] = '.';
        ds /= 10;
        do { t[--i] = (char)('0' + ds % 10); ds /= 10; } while (ds && i > 2);
        t[--i] = '+'; t[--i] = '[';
        log_put(a, t + i);
    }
    log_put(a, s);
    if (a->log_len + 1 < sizeof(a->log)) a->log[a->log_len++] = '\n';
}
static void log_num(struct app *a, const char *label, long v) {
    char b[96];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, label);
    number(&w, v);
    log_text(a, b);
}
/* "<pre><milliseconds> ms<post>" */
static void log_ms(struct app *a, const char *pre, u64 us, const char *post) {
    char b[96];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, pre);
    number(&w, (long)(us / 1000));
    append(&w, " ms");
    append(&w, post);
    log_text(a, b);
}
/* This stream so far: minutes, packets, and what the pacer had to fix. */
static void log_stats(struct app *a, const char *label) {
    char b[128];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, label);
    number(&w, (long)((a->clock() - a->stream_at) / 60000000));
    append(&w, " min, ");
    number(&w, (long)(a->packets - a->stream_packets));
    append(&w, " packets, ");
    number(&w, (long)a->drops);
    append(&w, " drops, ");
    number(&w, (long)a->underruns);
    append(&w, " underruns, ");
    number(&w, (long)(a->resyncs - a->stream_resyncs));
    append(&w, " resyncs");
    log_text(a, b);
}
/* Under the lock: hand over the pending lines. */
static u32 take_log(struct app *a, char *out) {
    u32 n = a->log_len;
    memcpy(out, a->log, n);
    a->log_len = 0;
    return n;
}
/* Outside the lock: append them to the file. */
static void write_log(struct app *a, const char *b, u32 n) {
    if (!n) return;
    if (a->log_size + n > SERVICE_LOG_CAP) { sc3(SYS_rename, (long)SERVICE_LOG, (long)SERVICE_LOG_OLD, 0); a->log_size = 0; }
    long f = sc3(SYS_open, (long)SERVICE_LOG, 0x209, 0644); /* write, create, append */
    if (f < 0) return;
    sc3(SYS_write, f, (long)b, n);
    closefd(f);
    a->log_size += n;
}

static void message(struct app *a, const char *s) {
    if (!equal(a->message, s)) log_text(a, s);
    memset(a->message, 0, sizeof(a->message));
    u64 n = length(s);
    if (n > 127) n = 127;
    memcpy(a->message, s, n);
}
static const char *target_name(struct app *a) {
    return a->target.name[0] ? a->target.name : "your audio device";
}
static void say(struct app *a, const char *pre, const char *suf) {
    char b[128];
    compose(b, sizeof(b), pre, target_name(a), suf);
    message(a, b);
}
/* On-screen notices only on real transitions; a flapping link must not spam them. */
static int may_announce(struct app *a, int kind) {
    u64 now = a->clock();
    if (a->notify_at[kind] && now - a->notify_at[kind] < 10000000) return 0;
    a->notify_at[kind] = now;
    return 1;
}
static void mute(struct app *a, int on) {
    if (a->hdmi < 0 || a->muted == on) return;
    int v = on;
    a->hdmi_rc = sc3(SYS_ioctl, a->hdmi, HDMI_SET_MUTE, (long)&v);
    if (!a->hdmi_rc) a->muted = on;
    long result[4] = {a->hdmi, a->hdmi_rc, on, v};
    put("/data/bt_hdmi_mute_status.bin", result, sizeof(result));
}
/* Stop a running search; the radio rests before the next dial. */
static void end_scan(struct app *a) {
    if (a->scanning) {
        if (a->bt >= 0) sc3(SYS_ioctl, a->bt, BT_SCAN_STOP, 0);
        a->radio_quiet_at = a->clock() + CONN_SCAN_QUIET_US;
        a->add_idle = a->clock();
    }
    a->scanning = 0;
}
static void stop_audio(struct app *a) {
    if (a->state == CONN_DISCOVERING) end_scan(a);
    if (a->streaming) {
        log_stats(a, "audio stopped after ");
        short silence[384];
        memset(silence, 0, sizeof(silence));
        struct audio_req q = {a->target.address, silence, sizeof(silence), 0, 0};
        sc3(SYS_ioctl, a->bt, BT_SEND_AUDIO, (long)&q);
    }
    a->streaming = 0; a->level = 0; a->meter = 0; a->meter_peak = 0;
    a->pin_pending = 0; a->test_until = 0; mute(a, 0);
    if (a->ring) { a->unmap(a->ring, 0x10000); a->ring = 0; }
    closefd(a->shmfd); a->shmfd = -1;
}
/* Schedule the next automatic attempt from the backoff schedule (2, 4, 8, 16 s),
 * or give up and wait for the user once it is exhausted. Driven by a->attempts,
 * the count of consecutive failures. */
static void schedule_backoff(struct app *a) {
    u32 n = a->attempts ? a->attempts : 1;
    if (conn_give_up(n)) {
        a->give_up = 1;
        bta_profile_record_fail(&a->profiles, a->target.address);
        a->profile_dirty = 1;
        return;
    }
    a->next_try = a->clock() + conn_backoff_us(n);
}
/* The outstanding dial is over (it failed or ran out its window): the radio rests,
 * then may search or dial again. */
static void page_done(struct app *a) {
    if (!a->page_at) return;
    a->page_at = 0;
    u64 quiet = a->clock() + CONN_SCAN_QUIET_US;
    if (a->radio_quiet_at < quiet) a->radio_quiet_at = quiet;
}
/* A failed attempt: stop, explain, and back off before trying again. */
static void fail(struct app *a, const char *s) {
    page_done(a);
    stop_audio(a); message(a, s); a->dropped = 0; a->state = CONN_RECOVERING;
    schedule_backoff(a);
}
static void fail_named(struct app *a, const char *pre, const char *suf) {
    char m[128];
    compose(m, sizeof(m), pre, target_name(a), suf);
    fail(a, m);
}
/* The PS4 audio stream was rebuilt (e.g. a game started). The headphone link is
 * fine, so this is a quiet reconnect of the audio path, not a failure or a drop. */
static void restart_audio(struct app *a, const char *s) {
    stop_audio(a); message(a, s); a->dropped = 0; a->give_up = 0;
    a->state = CONN_RECOVERING; a->next_try = a->clock() + 2000000;
}
/* The Bluetooth link is up but the PS4 audio ring is not ready yet (e.g. right
 * after boot). Keep trying promptly; this is not a connection failure, so it
 * never counts toward the backoff or the give-up. */
static void audio_wait(struct app *a, const char *s) {
    stop_audio(a); message(a, s); a->dropped = 0; a->give_up = 0; a->attempts = 0;
    a->state = CONN_RECOVERING; a->next_try = a->clock() + 3000000;
}
/* Drop the Bluetooth link itself, not just the audio. The driver keeps per-link
 * stream state (e.g. a stuck format renegotiation) that survives an audio stop. */
static void drop_link(struct app *a) {
    if (a->bt >= 0 && a->target.address) {
        u64 address = a->target.address;
        sc3(SYS_ioctl, a->bt, BT_DROP_LINK, (long)&address);
        log_text(a, "link: dropped");
    }
    a->own_link = 0;
}
/* Last-resort recovery once the automatic format fallback has not helped: drop
 * the link and reconnect from scratch, restarting the backoff. Any accepted
 * packet resets the relink count. */
static void relink(struct app *a) {
    a->relinks++; a->relinks_total++; stop_audio(a); drop_link(a);
    a->state = CONN_RECOVERING; a->dropped = 1; a->give_up = 0; a->attempts = 0;
    a->next_try = a->clock() + 2000000; a->refused_since = 0;
    say(a, "The PS4 stopped accepting audio for ", ". Reconnecting...");
}
/* The headphones went away mid-stream, detected only by consecutive audio-send
 * failures (never by polling the driver while streaming: that starved the
 * controller). stop_audio restores TV sound. */
static void link_lost(struct app *a) {
    stop_audio(a); a->dropped = 1; a->give_up = 0; a->attempts = 0;
    a->own_link = 0; /* should the driver still list it, that link is dead: dial afresh */
    a->state = CONN_RECOVERING; a->next_try = a->clock() + 2000000;
    say(a, "", " disconnected. TV sound is back. Reconnecting...");
    if (may_announce(a, 1)) {
        char m[128];
        compose(m, sizeof(m), "", target_name(a), " disconnected. TV sound is back on. Reconnecting...");
        notify(m);
        a->announced = 1;
    }
}
static struct bt_device *device(struct app *a, u64 address) {
    for (unsigned i = 0; i < a->devices.count; i++)
        if (a->devices.items[i].address == address) return &a->devices.items[i];
    return 0;
}
static int ensure_bt(struct app *a) {
    if (a->bt >= 0) return 0;
    a->bt = sc3(SYS_open, (long)"/dev/bt", 6, 0);
    if (a->bt < 0) return -1;
    u64 mask = 0xffffff;
    sc3(SYS_write, a->bt, (long)&mask, 8);
    return 0;
}
static void refresh_devices(struct app *a) {
    struct bt_devices d;
    if (!bt_list(&d) && d.count <= BT_DEVICE_LIMIT) a->devices = d;
    struct bt_device *t = device(a, a->target.address);
    if (t) a->target = *t;
}
static int scan(struct app *a) {
    /* Never while a dial may still be running (conn.h "Sharing the radio"). */
    if (a->page_at || a->hold) { log_text(a, "scan: not started, a connection attempt holds the radio"); return -1; }
    if (ensure_bt(a)) return -1;
    long rc = sc3(SYS_ioctl, a->bt, BT_SCAN_START, 0);
    log_num(a, "scan: started, rc ", rc); /* the radio is busy for the next 16 s */
    if (rc < 0) return -1;
    a->scanning = 1; a->scan_end = a->clock() + 16000000; a->list_at = 0;
    return 0;
}
struct saved_device { u64 magic; struct bt_device device; };
static int put_profiles(const struct bta_profiles *p) {
    if (put("/data/bta_audio/profiles.tmp", p, sizeof(*p))) return -1;
    if (sc3(SYS_rename, (long)"/data/bta_audio/profiles.tmp", (long)"/data/bta_audio/profiles.bin", 0) < 0) return -1;
    return 0;
}
static int save_profiles(struct app *a) { return put_profiles(&a->profiles); }
/* Load the remembered devices, migrating the pre-refactor device.bin plus the
 * dual-<address> flag into one profile the first time it is seen. */
static void load_profiles(struct app *a) {
    bta_profiles_init(&a->profiles);
    long fd = sc3(SYS_open, (long)"/data/bta_audio/profiles.bin", 0, 0);
    if (fd >= 0) {
        struct bta_profiles p;
        long n = sc3(SYS_read, fd, (long)&p, sizeof(p));
        closefd(fd);
        if (n == (long)sizeof(p) && bta_profiles_valid(&p, sizeof(p))) { a->profiles = p; return; }
    }
    fd = sc3(SYS_open, (long)"/data/bta_audio/device.bin", 0, 0);
    if (fd < 0) return;
    struct saved_device s;
    long n = sc3(SYS_read, fd, (long)&s, sizeof(s));
    closefd(fd);
    if (n == (long)sizeof(s) && s.magic == 0x3254454752415442UL && s.device.address && !(s.device.address >> 48)) {
        s.device.name[95] = 0;
        int i = bta_profile_touch(&a->profiles, &s.device);
        char p[40];
        joint_path(s.device.address, p);
        a->profiles.items[i].mode = exists(p) ? SBC_DUAL : SBC_JOINT;
        a->profiles.items[i].last_ok_us = 1; /* previously used: prefer it on startup */
        save_profiles(a);
    }
}
/* Remember the user's selection and persist the table. */
static int select_device(struct app *a, const struct bt_device *d) {
    bta_profile_touch(&a->profiles, d);
    if (save_profiles(a)) return -1;
    a->target = *d;
    a->mode = bta_profile_mode(&a->profiles, d->address);
    return 0;
}
/* Make the most-recently-used remembered device the initial target so a
 * returning user auto-reconnects; begin_connect re-picks by what is in range. */
static void init_target(struct app *a) {
    int best = -1;
    for (unsigned i = 0; i < a->profiles.count; i++)
        if (best < 0 || a->profiles.items[i].last_ok_us > a->profiles.items[best].last_ok_us) best = (int)i;
    if (best < 0) return;
    bta_profile_device(&a->profiles.items[best], &a->target);
    a->mode = a->profiles.items[best].mode;
    a->wanted = 1;
}
static int save_volume(struct app *a, int volume) {
    if (volume < 0 || volume > 100) return -1;
    unsigned value = (unsigned)volume;
    if (put("/data/bta_audio/volume.tmp", &value, sizeof(value))) return -1;
    if (sc3(SYS_rename, (long)"/data/bta_audio/volume.tmp", (long)"/data/bta_audio/volume.bin", 0) < 0) return -1;
    a->volume = volume;
    return 0;
}
static void load_volume(struct app *a) {
    a->volume = 40;
    unsigned value = 101;
    long fd = sc3(SYS_open, (long)"/data/bta_audio/volume.bin", 0, 0);
    if (fd < 0) return;
    long n = sc3(SYS_read, fd, (long)&value, sizeof(value));
    closefd(fd);
    if (n == sizeof(value) && value <= 100) a->volume = (int)value;
}
/* Latency mode (BTA_LAT_*): the backlog the sender holds. Persisted so it
 * survives a restart, like the volume. Default balanced (12 ms). */
static void apply_latency(struct app *a, int mode) {
    if (mode < BTA_LAT_LOW || mode > BTA_LAT_STABLE) mode = BTA_LAT_BALANCED;
    u32 old_target = a->pace.target;
    a->latency_mode = mode;
    bta_pace_config(&a->pace, mode);
    /* A deeper target needs a deeper cushion; the steady pacer only trims down,
     * so re-enter prebuffering to grow it. Lowering the mode trims on the next
     * send and needs no refill. */
    if (a->pace.target > old_target) a->pace.filling = 1;
}
static int save_latency(struct app *a, int mode) {
    if (mode < BTA_LAT_LOW || mode > BTA_LAT_STABLE) return -1;
    unsigned value = (unsigned)mode;
    if (put("/data/bta_audio/latency.tmp", &value, sizeof(value))) return -1;
    if (sc3(SYS_rename, (long)"/data/bta_audio/latency.tmp", (long)"/data/bta_audio/latency.bin", 0) < 0) return -1;
    apply_latency(a, mode);
    return 0;
}
static void load_latency(struct app *a) {
    unsigned value = BTA_LAT_BALANCED;
    long fd = sc3(SYS_open, (long)"/data/bta_audio/latency.bin", 0, 0);
    if (fd >= 0) {
        long n = sc3(SYS_read, fd, (long)&value, sizeof(value));
        closefd(fd);
        if (n != sizeof(value)) value = BTA_LAT_BALANCED;
    }
    apply_latency(a, (int)value);
}
/* The radio is taken: a search runs or is due, a hold lasts, or it is resting. */
static int radio_busy(struct app *a, u64 now) {
    return a->scanning || a->scan_wait || a->hold || now < a->radio_quiet_at;
}
/* Ask the driver for a new link to the target. */
static void dial(struct app *a) {
    u64 address = a->target.address;
    sc3(SYS_ioctl, a->bt, BT_SCAN_STOP, 0);
    a->connect_rc = sc3(SYS_ioctl, a->bt, BT_CONNECT, (long)&address);
    log_num(a, "connect: dial rc ", a->connect_rc);
    /* An immediate driver error (e.g. 0x802F0204) means this device will not
     * connect right now; fail fast instead of waiting out the 30 s window. */
    if (a->connect_rc < 0) {
        fail_named(a, "Couldn't connect to ", ". Make sure it's on, nearby and not connected to another device.");
        return;
    }
    a->own_link = address;
    /* bluetooth_tick watches the dial; no search starts until it ends. */
    u64 now = a->clock();
    a->page_at = now; a->page_end = now + CONN_PAGE_END_US; a->deadline = now + CONN_DIAL_WINDOW_US;
    a->page_before = a->page_last = a->btstate; a->page_seen = 0;
}
static void begin_connect(struct app *a) {
    a->patch_rc = bta_apply_patch();
    if (a->patch_rc) { fail(a, "Bluetooth setup failed. Check the diagnostic status."); return; }
    if (ensure_bt(a)) { fail(a, "Bluetooth is not ready. Retrying shortly."); return; }
    refresh_devices(a);
    /* Connect to whichever remembered device is in range: keep the current
     * target if it is present, otherwise pick the best remembered one that is. */
    if (!a->target.address || !device(a, a->target.address)) {
        struct bt_device *p = device(a, bta_profile_pick(&a->profiles, &a->devices));
        if (p) a->target = *p;
    }
    struct bt_device *t = device(a, a->target.address);
    if (!t) {
        if (scan(a)) { fail(a, "Bluetooth scan could not start. Retrying shortly."); return; }
        a->state = CONN_DISCOVERING; a->deadline = a->clock() + 16000000;
        say(a, "Looking for ", "... Put it in pairing mode if needed.");
        return;
    }
    if (!bt_is_audio(t)) {
        a->wanted = 0; stop_audio(a); a->state = CONN_IDLE;
        message(a, "This device does not advertise Bluetooth audio playback.");
        return;
    }
    end_scan(a);
    a->mode = bta_profile_mode(&a->profiles, a->target.address);
    u64 address = a->target.address;
    a->btstate = sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address);
    a->state = CONN_CONNECTING; a->deadline = a->clock() + 30000000; a->redial_at = 0;
    say(a, "Connecting to ", "...");
    log_num(a, "connect: link state ", a->btstate);
    switch (conn_plan_link(a->btstate, a->own_link == address)) {
    case CONN_DIAL: /* right after a search the radio rests first: bluetooth_tick dials then */
        if (radio_busy(a, a->clock())) { a->redial_at = a->clock(); log_text(a, "connect: waiting for the radio"); }
        else dial(a);
        break;
    case CONN_REUSE: a->connect_rc = 0; break;
    case CONN_DROP_FIRST: /* bluetooth_tick dials once it has closed */
        drop_link(a);
        a->redial_at = a->clock() + 2000000;
        break;
    }
}
static void start_audio(struct app *a) {
    a->shmfd = a->shm("/shm_acap.full", 0, 0);
    if (a->shmfd < 0) { audio_wait(a, "Waiting for the PS4 audio service."); return; }
    void *p = a->map(0, 0x10000, 1, 1, a->shmfd, 0);
    if (!p || p == (void *)-1) { audio_wait(a, "Unable to open the game audio stream."); return; }
    a->ring = p;
    a->readpos = *(volatile u32 *)(a->ring + 12);
    if (*(volatile u32 *)a->ring != 0x10000 || a->readpos < 16 || a->readpos >= 0x10000 || (a->readpos & 7)) {
        audio_wait(a, "The PS4 audio stream has an unexpected format.");
        return;
    }
    a->streaming = 1; a->state = CONN_STREAMING; a->refused_since = 0; a->errors = 0;
    a->stream_at = a->clock(); a->test_until = 0; a->test_next = 0; a->test_phase = 0; a->snap = 0;
    a->stats_at = a->stream_at + 600000000; a->stream_packets = a->packets; a->stream_resyncs = a->resyncs;
    a->stream_events = 0;
    bta_pace_reset(&a->pace, a->stream_at); a->drops = 0; a->underruns = 0;
    int was_dropped = a->dropped, told = a->announced, fresh = !a->session;
    a->attempts = 0; a->give_up = 0; a->dropped = 0; a->announced = 0; a->session = 1;
    /* relinks persists across relink cycles (reset only when a packet is finally
     * accepted, in audio_tick) so the give-up cap can trigger. */
    a->confirmed = 0; a->modes_tried = (u8)(1 << a->mode);
    a->ok_at = a->stream_at; a->meter = 0; a->meter_peak = 0;
    say(a, "Connected to ", ".");
    if (was_dropped) {
        if (told && may_announce(a, 2)) {
            char m[96];
            compose(m, sizeof(m), "", target_name(a), " reconnected.");
            notify(m);
        }
    } else if (fresh && may_announce(a, 0)) {
        char m[128];
        compose(m, sizeof(m), "Connected to ", target_name(a), ".");
        notify(m);
    }
}
/* Let go of the device: drop its link, which also cancels a dial still running,
 * and hold the radio until the driver shows that done (radio_tick). */
static void release_link(struct app *a) {
    u64 address = a->target.address, now = a->clock();
    int dialling = a->page_at != 0;
    a->page_at = 0; a->redial_at = 0;
    if (a->bt < 0 || !address) { a->own_link = 0; return; }
    long st = sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address);
    sc3(SYS_ioctl, a->bt, BT_DROP_LINK, (long)&address);
    a->own_link = 0;
    enum conn_hold h = conn_link_up(st) ? CONN_HOLD_LINK : dialling ? CONN_HOLD_PAGE : CONN_HOLD_NONE;
    if (h == CONN_HOLD_LINK) log_text(a, "link: dropped");
    if (h == CONN_HOLD_PAGE) log_num(a, "radio: cancelling the dial, link state ", st);
    if (h && !a->hold) { a->hold = h; a->hold_at = now; a->hold_addr = address; a->hold_poll = now + 250000; a->page_last = st; }
}
/* Add Device (POST /api/scan): stop reconnecting, let go of the device (its audio
 * and link, or its dial), and search once the radio is free. Asked again while it
 * lasts: one more search, once the running one is over. */
static void begin_add(struct app *a, enum conn_add who) {
    a->add_idle = a->clock();
    if (a->adding) { if (!a->scanning) a->scan_wait = 1; return; }
    int playing = a->streaming, dialling = a->page_at != 0;
    a->adding = who;
    a->resume = a->wanted && !a->give_up;
    log_text(a, who == CONN_ADD_CONSOLE ? "add: Add Device opened" : "add: a phone asked for a search");
    if (a->state == CONN_DISCOVERING) { a->state = CONN_IDLE; log_text(a, "add: using the search already running"); }
    else if (a->wanted && !playing && a->state != CONN_IDLE) log_text(a, "add: reconnecting stopped");
    stop_audio(a);
    a->wanted = 0; a->pin_pending = 0; a->state = CONN_IDLE;
    a->dropped = a->give_up = a->announced = 0;
    release_link(a);
    if (playing) say(a, "", " is disconnected while you add a device. TV sound is back.");
    else if (dialling) say(a, "Stopped connecting to ", " while you add a device.");
    a->scan_wait = !a->scanning;
}
/* Add Device ends. resume: reconnect the device it paused (it was left, or the app
 * went away); otherwise the caller decides what happens next. */
static void end_add(struct app *a, const char *why, int resume) {
    if (!a->adding) return;
    log_text(a, why);
    end_scan(a);
    a->adding = CONN_ADD_NONE; a->scan_wait = 0;
    if (resume && a->resume && a->target.address) {
        a->wanted = 1; a->state = CONN_RECOVERING; a->next_try = 0; a->attempts = 0;
        a->dropped = a->give_up = a->announced = a->session = 0;
        say(a, "Reconnecting to ", "...");
    }
    a->resume = 0;
}
/* Every tick: settle a hold, start the waiting search, end Add Device when its
 * owner went away (conn.h "Sharing the radio"). */
static void radio_tick(struct app *a, u64 now) {
    if (a->hold && now >= a->hold_poll) {
        a->hold_poll = now + 250000;
        u64 address = a->hold_addr;
        long st = a->bt >= 0 ? sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address) : -1;
        if (a->hold == CONN_HOLD_PAGE && st != a->page_last) { log_num(a, "radio: link state ", st); a->page_last = st; }
        enum conn_hold_step step = conn_hold_next(a->hold, st, a->page_before, a->page_seen, now, a->hold_at, a->page_end);
        if (step == CONN_HOLD_LINKED) {
            if (a->bt >= 0) sc3(SYS_ioctl, a->bt, BT_DROP_LINK, (long)&address);
            log_text(a, "radio: the dial linked up anyway; link dropped");
            a->hold = CONN_HOLD_LINK; a->hold_at = now;
        } else if (step != CONN_HOLD_WAIT) {
            u64 waited = now - a->hold_at;
            if (step == CONN_HOLD_CANCELLED) log_ms(a, "radio: dial cancelled after ", waited, ", the driver confirmed it");
            else if (step == CONN_HOLD_WINDOW_OVER) log_ms(a, "radio: waited ", waited, " for the dial to end (no confirmation)");
            else if (step == CONN_HOLD_DOWN) log_ms(a, "radio: link down after ", waited, "");
            else {
                log_text(a, "radio: the link stayed up");
                if (a->scan_wait) { a->scan_wait = 0; say(a, "Couldn't disconnect ", ". Press Search Again to try again."); }
            }
            a->hold = CONN_HOLD_NONE;
            u64 quiet = now + CONN_HOLD_QUIET_US;
            if (a->radio_quiet_at < quiet) a->radio_quiet_at = quiet;
        }
    }
    if (a->scan_wait && !a->hold && now >= a->radio_quiet_at) {
        a->scan_wait = 0;
        if (!a->scanning && scan(a)) message(a, "Bluetooth search could not start. Press Search Again.");
    }
    if (a->adding && conn_add_over(a->adding, now, a->console_at, a->add_idle, a->scanning || a->scan_wait))
        end_add(a, a->adding == CONN_ADD_CONSOLE ? "add: ended, the app went away" : "add: ended, no device was chosen", 1);
}
/* One message from the driver (read from /dev/bt). */
struct bt_event { u8 type, subtype; u16 reserved; u32 data; u64 address; };
/* Driver events go to service.log: the service answers two kinds (3: PIN wanted,
 * 4: accept pairing), and what a device that won't connect or play is asking for
 * would be among the rest. At most 12 lines in 10 s, and while audio plays only
 * the stream's first 20, so a chatty link can't push everything else out. */
static void log_event(struct app *a, const struct bt_event *e, u64 now) {
    if (a->streaming && a->stream_events++ >= 20) return;
    if (now >= a->event_at + 10000000) { a->event_at = now; a->event_lines = 0; }
    if (a->event_lines++ >= 12) return;
    char b[96], mac[13];
    struct writer w = {b, 0, sizeof(b)};
    append(&w, "bt event: type ");
    number(&w, e->type);
    append(&w, ", subtype ");
    number(&w, e->subtype);
    append(&w, ", data ");
    number(&w, (long)e->data);
    mac_text(e->address, mac);
    append(&w, e->address == a->target.address ? ", the device " : ", from ");
    append(&w, mac);
    log_text(a, b);
}
static void bluetooth_tick(struct app *a, u64 now) {
    /* While audio is streaming, do NOT walk the driver cache: refresh_devices()
     * takes the kernel's Bluetooth link lock, and polling it mid-stream starves
     * the wireless controller's HID link. The proven BTA bridge never did this.
     * Device enumeration resumes as soon as streaming stops. */
    if (!a->streaming && now >= a->list_at) {
        refresh_devices(a);
        a->list_at = now + (a->scanning ? 500000 : 3000000);
    }
    if (a->scanning && now >= a->scan_end) end_scan(a);
    if (a->bt >= 0) {
        struct bt_event ev[16];
        long n = sc3(SYS_read, a->bt, (long)ev, sizeof(ev));
        if (n > 0 && n <= (long)sizeof(ev))
            for (unsigned i = 0; i < (unsigned)n / sizeof(ev[0]); i++) {
                log_event(a, &ev[i], now);
                /* Pairing only happens during connection setup. Ignoring these
                 * while streaming keeps the state == CONN_STREAMING invariant that
                 * link_state() relies on, and avoids a stray event knocking a live
                 * stream back into the forbidden link-state poll. */
                if (ev[i].address == a->target.address && a->wanted && !a->streaming) {
                    if (ev[i].type == 4) { u64 q[2] = {a->target.address, 1}; sc3(SYS_ioctl, a->bt, BT_PAIR_ACCEPT, (long)q); }
                    if (ev[i].type == 3) { a->pin_pending = 1; a->state = CONN_PAIRING; a->deadline = now + 60000000; message(a, "This device requires a pairing PIN."); }
                }
            }
    }
    radio_tick(a, now);
    if (!a->wanted) return;
    if (a->state == CONN_RECOVERING) {
        if (!a->give_up && now >= a->next_try && !radio_busy(a, now)) { a->attempts++; begin_connect(a); }
        return;
    }
    if (a->state == CONN_DISCOVERING) {
        if (device(a, a->target.address)) { end_scan(a); begin_connect(a); return; }
        struct bt_device *p = device(a, bta_profile_pick(&a->profiles, &a->devices));
        if (p) { a->target = *p; end_scan(a); begin_connect(a); return; }
        if (now >= a->deadline) fail_named(a, "Couldn't find ", ". Put it in pairing mode.");
        return;
    }
    if (a->state == CONN_CONNECTING || a->state == CONN_PAIRING) {
        u64 address = a->target.address;
        if (a->redial_at) {
            /* begin_connect dropped a stale link, or found the radio resting after a
             * search: dial once the link had time to close and the radio is free. */
            if (now < a->redial_at || radio_busy(a, now)) return;
            a->redial_at = 0;
            a->btstate = sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address);
            if (!conn_link_up(a->btstate)) { dial(a); return; }
            /* Up again already: the device linked back by itself, on a new link. */
            log_text(a, "connect: the device linked back by itself");
            a->own_link = address;
        }
        a->btstate = sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address);
        if (a->page_at) {
            /* Whether the driver shows the dial in its link state decides if a
             * cancel can ever be confirmed (radio_tick), so each change is logged. */
            if (a->btstate != a->page_last) { log_num(a, "connect: link state ", a->btstate); a->page_last = a->btstate; }
            if (conn_paging(a->btstate, a->page_before)) a->page_seen = 1;
        }
        if (conn_link_up(a->btstate)) {
            a->page_at = 0;
            log_text(a, "connect: link up");
            a->pin_pending = 0;
            if (exists(NO_WIDEN)) {
                a->packet_rc = 0; /* diagnostic: leave controller link params untouched */
            } else {
                a->packet_rc = bt_prepare(address);
                if (a->packet_rc) { fail(a, "No supported Bluetooth audio link. Retrying shortly."); return; }
            }
            a->state = CONN_CONFIGURING; a->deadline = now + 400000;
        } else if (a->state == CONN_CONNECTING && a->page_at && conn_dial_ended(a->btstate, a->page_before, a->page_seen)) {
            /* The driver gave up on it (after about 15 s): try again now rather
             * than wait out the rest of the window. */
            log_ms(a, "connect: the driver ended the dial after ", now - a->page_at, "");
            fail_named(a, "", " didn't respond. Is it on and nearby?");
        } else if (now >= a->deadline) {
            fail_named(a, "", " didn't respond. Is it on and nearby?");
        }
    } else if (a->state == CONN_CONFIGURING && now >= a->deadline) start_audio(a);
}
static void roll_second(struct app *a, u64 now) {
    bta_roll_second(a->secs, &a->cur, &a->sec_seq, &a->sec_start, now);
}
/* Send type 4 is joint stereo at full bitpool, the format headphones agree to at
 * connect; type 3 is dual channel, which the driver renegotiates. The mode is
 * remembered per device (profiles.h) and auto-switched on silence below. */
static void audio_tick(struct app *a, u64 now) {
    if (!a->streaming) { a->last_send = 0; return; }
    roll_second(a, now);
    short pcm[384];
    unsigned peak = 0;
    unsigned backlog = 0;
    if (now < a->test_until) {
        /* Self-paced test tone (works even with no game audio): one 192-frame
         * packet every ~4 ms, independent of the capture ring. */
        if (now < a->test_next) return;
        a->test_next = now + 4000;
        bta_test_tone(pcm, &a->test_phase, a->volume, &a->level, &peak);
    } else {
        u32 writepos = (u32)(*(volatile u64 *)(a->ring + 8) >> 32);
        if (writepos < 16 || writepos >= 0x10000 || (writepos & 7)) {
            restart_audio(a, "Audio stream changed. Reconnecting...");
            return;
        }
        /* The test tone does not read the capture ring, so a long test laps it
         * and leaves readpos stale. On the first capture read after one, drop to
         * the live write cursor and prebuffer afresh instead of sending old or
         * lapped audio. */
        if (a->snap) { a->readpos = writepos; a->snap = 0; bta_pace_reset(&a->pace, now); }
        unsigned available = bta_ring_available(writepos, a->readpos);
        /* Clock-paced send with a bounded backlog (see pacing.h): prebuffer the
         * target cushion, then one packet per ~4 ms, dropping the oldest bytes
         * when the backlog runs ahead, instead of bursting and resyncing only at
         * ~128 ms. WAIT also covers the prebuffer, so it is not an underrun. */
        struct bta_pace_step step = bta_pace_run(&a->pace, now, available);
        if (step.kind == BTA_PACE_WAIT) return;
        if (step.kind == BTA_PACE_UNDERRUN) { a->underruns++; return; }
        if (step.skip) {
            a->readpos = bta_ring_advance(a->readpos, step.skip);
            if (step.kind == BTA_PACE_RESYNC) a->resyncs++; else a->drops++;
        }
        backlog = available - step.skip; /* audio actually held and sent, after any trim */
        a->readpos = bta_convert_capture(a->ring, a->readpos, a->volume, pcm, &a->level, &peak);
    }
    struct audio_req req = {a->target.address, pcm, sizeof(pcm), (u32)conn_send_type(a->mode), 0};
    a->last_rc = sc3(SYS_ioctl, a->bt, BT_SEND_AUDIO, (long)&req);
    {
        u64 done = a->clock();
        struct bta_second *s = &a->cur;
        s->pkts++;
        if (a->last_rc == 0) s->refused++;
        else if (a->last_rc < 0) s->failed++;
        if (a->last_send) {
            u64 gap = now - a->last_send;
            if (gap > s->maxgap_us) s->maxgap_us = (u32)(gap > 0xffffffffUL ? 0xffffffffUL : gap);
            if (gap > 8000) s->late8++;
            if (gap > 16000) s->late16++;
            if (gap > 50000) s->late50++;
        }
        if (done - now > s->maxsend_us) s->maxsend_us = (u32)(done - now);
        if (backlog / 384 > s->maxback_ms) s->maxback_ms = backlog / 384;
        if (peak > s->peak) s->peak = peak;
        a->last_send = now;
    }
    if (a->last_rc == 0) {
        /* Connected but the PS4 is refusing audio: the "connected yet silent"
         * case. First try the other SBC format once, keeping the link (the step
         * that used to need a manual reconnect); then fall back to a full relink,
         * and finally ask the user to power-cycle the headphones. */
        if (!a->refused_since) a->refused_since = now;
        else if (now - a->refused_since > REFUSED_US) {
            int other = conn_other_mode(a->mode);
            if (!(a->modes_tried & (1 << other))) {
                a->mode = other; a->modes_tried |= (u8)(1 << other); a->refused_since = 0;
                say(a, "No sound yet from ", ". Trying the other audio format...");
            } else if (a->relinks < 2) {
                relink(a); return;
            } else if (a->relinks == 2) {
                a->relinks++;
                say(a, "The PS4 keeps refusing audio for ", ". Turn the device off and on.");
            }
        }
    } else if (a->last_rc > 0) {
        a->refused_since = 0; a->relinks = 0;
        /* A packet was accepted: remember the mode that works for this device. */
        if (!a->confirmed) {
            a->confirmed = 1;
            bta_profile_record_ok(&a->profiles, a->target.address, a->mode, now);
            a->profile_dirty = 1;
        }
    }
    if (a->last_rc < 0) {
        if (++a->errors > 20) link_lost(a);
    } else {
        a->errors = 0; a->packets++; a->ok_at = now;
        if (peak > a->meter_peak) a->meter_peak = peak;
        /* Prime the mix: hold HDMI unmuted for PRIME_US after stream start, then mute. */
        int want = !a->tv;
        if (want && now - a->stream_at < PRIME_US) want = 0;
        if (a->muted != want) mute(a, want);
    }
}
/* The one place the connection's real state is decided (see conn.h). A UI must
 * show this, never infer it from "selected" or "wanted". */
static int link_state(struct app *a) {
    if (conn_due(a->state, a->dropped, a->give_up, a->next_try, a->clock())) return BT_LINK_CONNECTING;
    return conn_link_state(a->state, a->dropped, a->give_up);
}
static int retry_in_ms(struct app *a, u64 now) {
    return conn_retry_in_ms(a->state, a->give_up, a->next_try, now);
}
static int quiet_for_ms(struct app *a, u64 now) {
    return conn_quiet_for_ms(a->state, a->ok_at, now);
}
/* reveal: include the live pairing code. Set only for a loopback HTTP request,
 * so a future console app can read it; never for a LAN response, and never for
 * the on-disk status.json (pair_code.txt is the one file that holds the code). */
static unsigned status(struct app *a, char *out, unsigned cap, int reveal) {
    u64 now = a->clock();
    struct writer w = {out, 0, cap};
    append(&w, "{\"version\":\"2.8.0\",\"api\":");
    number(&w, BTA_API_VERSION);
    append(&w, ",\"message\":");
    quoted(&w, a->message);
    field(&w, "sender", a->sender_alive);
    field(&w, "sender_prio", a->sender_prio);
    field(&w, "thread_rc", a->thread_rc);
    field(&w, "prio_rc", a->prio_rc);
    field(&w, "aff_rc", a->aff_rc);
    field(&w, "pi_rc", a->pi_rc);
    field(&w, "volume", a->volume);
    field(&w, "wanted", a->wanted);
    field(&w, "streaming", a->streaming);
    field(&w, "tv_enabled", a->tv);
    field(&w, "joint", a->mode == SBC_JOINT);
    field(&w, "send_type", conn_send_type(a->mode));
    field(&w, "relinks", a->relinks_total);
    field(&w, "latency_mode", a->latency_mode);
    field(&w, "drops", a->drops);
    field(&w, "underruns", a->underruns);
    field(&w, "refused_ms", a->refused_since ? (long)((now - a->refused_since) / 1000) : 0);
    field(&w, "hdmi_muted", a->muted == 1);
    field(&w, "scanning", a->scanning || a->scan_wait); /* a search waiting for the radio counts */
    field(&w, "adding", a->adding != CONN_ADD_NONE);
    field(&w, "pin_pending", a->pin_pending);
    field(&w, "test", now < a->test_until);
    field(&w, "link", link_state(a));
    append(&w, ",\"state\":");
    quoted(&w, conn_state_name(a->state));
    field(&w, "attempts", a->attempts);
    field(&w, "retry_ms", retry_in_ms(a, now));
    field(&w, "meter", a->meter);
    field(&w, "quiet_ms", quiet_for_ms(a, now));
    field(&w, "level", a->level);
    field(&w, "packets", a->packets);
    field(&w, "errors", a->errors);
    field(&w, "resyncs", a->resyncs);
    field(&w, "patch_rc", a->patch_rc);
    field(&w, "packet_rc", a->packet_rc);
    field(&w, "connect_rc", a->connect_rc);
    field(&w, "audio_rc", a->last_rc);
    field(&w, "hdmi_rc", a->hdmi_rc);
    field(&w, "bt_state", a->btstate);
    field(&w, "no_widen", exists(NO_WIDEN));
    field(&w, "uptime_us", a->clock());
    field(&w, "lan", a->auth.enabled);
    if (a->auth.enabled && a->ip) {
        char ip[16];
        ip_text(a->ip, ip);
        append(&w, ",\"ip\":");
        quoted(&w, ip);
    }
    if (reveal && a->auth.enabled) {
        append(&w, ",\"pair_code\":");
        quoted(&w, a->auth.code);
    }
    char mac[13];
    mac_text(a->target.address, mac);
    append(&w, ",\"selected\":");
    quoted(&w, mac);
    append(&w, ",\"name\":");
    quoted(&w, a->target.name);
    append(&w, ",\"devices\":[");
    int first = 1;
    for (unsigned i = 0; i < a->devices.count; i++) {
        struct bt_device *d = &a->devices.items[i];
        if (!bt_is_audio(d) && d->address != a->target.address) continue;
        if (!first) append(&w, ",");
        first = 0;
        mac_text(d->address, mac);
        append(&w, "{\"address\":");
        quoted(&w, mac);
        append(&w, ",\"name\":");
        quoted(&w, d->name);
        field(&w, "audio", !!(d->services & 8));
        field(&w, "known", bta_profile_known(&a->profiles, d->address));
        append(&w, "}");
    }
    append(&w, "]}");
    return w.used;
}
static unsigned timing_json(struct app *a, char *out, unsigned cap) {
    return bta_timing_json(a->secs, a->sec_seq, (long)(a->clock() / 1000000), out, cap);
}
/* Write the response header for an n-byte body into output; returns its length. */
static unsigned response_head(struct client *c, const char *code, const char *type, unsigned n) {
    struct writer w = {c->output, 0, sizeof(c->output)};
    append(&w, "HTTP/1.1 ");
    append(&w, code);
    append(&w, "\r\nContent-Type: ");
    append(&w, type);
    append(&w, "\r\nContent-Length: ");
    number(&w, n);
    append(&w, "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'\r\n\r\n");
    return w.used;
}
static void response(struct client *c, const char *code, const char *type, const char *body, unsigned n) {
    unsigned h = response_head(c, code, type, n);
    /* Never drop a reply silently: the client would wait out the deadline and see
     * the connection closed with nothing. */
    if (h + n > sizeof(c->output)) { response(c, "500 Internal Server Error", "text/plain", "Response too large.", 19); return; }
    memcpy(c->output + h, body, n);
    c->head = c->total = h + n;
    c->tail = 0;
}
/* A constant body of any size (the page) goes out from where it lives, after the header. */
static void response_const(struct client *c, const char *code, const char *type, const char *body, unsigned n) {
    c->head = response_head(c, code, type, n);
    c->tail = body;
    c->total = c->head + n;
}
static char *header(char *request, const char *name) {
    char *p = find(request, "\r\n");
    if (!p) return 0;
    p += 2;
    while (*p && *p != '\r') {
        char *q = p;
        const char *k = name;
        while (*k) {
            char c = *q;
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != *k) break;
            q++; k++;
        }
        if (!*k && *q == ':') { q++; while (*q == ' ' || *q == '\t') q++; return q; }
        p = find(p, "\r\n");
        if (!p) return 0;
        p += 2;
    }
    return 0;
}
static int host_valid(char *request) {
    char *p = header(request, "host");
    if (!p) return 0;
    u8 ip[4];
    for (int i = 0; i < 4; i++) {
        unsigned n = 0, digits = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + *p++ - '0'; if (++digits > 3 || n > 255) return 0; }
        if (!digits) return 0;
        ip[i] = n;
        if (i < 3 && *p++ != '.') return 0;
    }
    return local_ip(ip) && starts(p, ":8097\r\n");
}
/* Copy the X-BTA-Token header value (hex, up to a token's length) into a
 * NUL-terminated buffer. The raw header value runs up to a CR, so it must be
 * bounded here before any string compare. */
static void read_token(char *request, char out[BTA_TOKEN_HEX + 1]) {
    out[0] = 0;
    char *v = header(request, "x-bta-token");
    if (!v) return;
    unsigned n = 0;
    while (n < BTA_TOKEN_HEX && ((v[n] >= '0' && v[n] <= '9') || (v[n] >= 'a' && v[n] <= 'f'))) { out[n] = v[n]; n++; }
    out[n] = 0;
}
/* A LAN client with no (or a stale) token: tell it, in JSON, whether pairing is
 * even possible so the page can prompt for the code or ask the owner to turn
 * phone control on at the console. */
static void needs_pairing(struct app *a, struct client *c) {
    char body[48];
    struct writer w = {body, 0, sizeof(body)};
    append(&w, "{\"api\":");
    number(&w, BTA_API_VERSION);
    append(&w, ",\"paired\":false");
    field(&w, "lan", a->auth.enabled);
    append(&w, "}");
    response(c, "401 Unauthorized", "application/json", body, w.used);
}
static void dispatch(struct app *a, struct client *c) {
    char body[12000];
    if (!host_valid(c->input)) { response(c, "403 Forbidden", "text/plain", "Use the PS4 IP address.", 23); return; }
    /* The page itself carries no state and must load so a LAN client can see the
     * pairing prompt; everything under /api is gated. */
    if (starts(c->input, "GET / HTTP/1.")) { response_const(c, "200 OK", "text/html; charset=utf-8", web_page, sizeof(web_page) - 1); return; }
    char token_hdr[BTA_TOKEN_HEX + 1];
    read_token(c->input, token_hdr);
    int authed = bta_auth_request_ok(&a->auth, c->loopback, token_hdr);
    if (c->loopback) a->console_at = a->clock(); /* the console app is still there (conn_add_over) */
    /* Pairing: a LAN client posts the code shown on the TV and is handed a token.
     * Open to unpaired clients by design; still needs the anti-CSRF header. */
    if (starts(c->input, "POST /api/pair/")) {
        char *control = header(c->input, "x-bta-control");
        if (!control || !starts(control, "1\r\n")) { response(c, "403 Forbidden", "text/plain", "Control header required.", 24); return; }
        char code[BTA_CODE_DIGITS + 1];
        const char *p = c->input + 15;
        for (int i = 0; i < BTA_CODE_DIGITS; i++) {
            if (p[i] < '0' || p[i] > '9') { response(c, "400 Bad Request", "text/plain", "Enter the six-digit code.", 25); return; }
            code[i] = p[i];
        }
        code[BTA_CODE_DIGITS] = 0;
        if (!starts(p + BTA_CODE_DIGITS, " HTTP/1.")) { response(c, "400 Bad Request", "text/plain", "Enter the six-digit code.", 25); return; }
        u8 rnd[16];
        char token[BTA_TOKEN_HEX + 1], issued[BTA_TOKEN_HEX + 1];
        fill_random(a, rnd, sizeof(rnd));
        bta_auth_make_token(issued, rnd);
        int rc = bta_auth_pair(&a->auth, code, issued, token, a->clock(), PAIR_COOLDOWN_US);
        if (rc < 0) { response(c, "403 Forbidden", "text/plain", "Phone control is off or locked. Enable it on the console.", 57); return; }
        if (rc == 0) { response(c, "403 Forbidden", "text/plain", "Wrong code. Check the code on the TV.", 37); return; }
        struct writer w = {body, 0, sizeof(body)};
        append(&w, "{\"api\":");
        number(&w, BTA_API_VERSION);
        append(&w, ",\"paired\":true,\"token\":");
        quoted(&w, token);
        append(&w, "}");
        response(c, "200 OK", "application/json", body, w.used);
        return;
    }
    if (starts(c->input, "GET /api/status HTTP/1.")) { if (!authed) { needs_pairing(a, c); return; } unsigned n = status(a, body, sizeof(body), c->loopback); response(c, "200 OK", "application/json", body, n); return; }
    if (starts(c->input, "GET /api/timing HTTP/1.")) { if (!authed) { needs_pairing(a, c); return; } unsigned n = timing_json(a, body, sizeof(body)); response(c, "200 OK", "application/json", body, n); return; }
    char *control = header(c->input, "x-bta-control");
    if (!starts(c->input, "POST ") || !control || !starts(control, "1\r\n")) {
        response(c, "403 Forbidden", "text/plain", "Control header required.", 24);
        return;
    }
    if (!authed) { needs_pairing(a, c); return; }
    /* Changing who can reach the service is a console-only action: a paired
     * phone must not be able to widen access or lock the owner out. */
    if (starts(c->input, "POST /api/lan-on HTTP/1.") || starts(c->input, "POST /api/lan-off HTTP/1.")) {
        if (!c->loopback) { response(c, "403 Forbidden", "text/plain", "Change phone control at the console.", 36); return; }
        if (starts(c->input, "POST /api/lan-on HTTP/1.")) {
            if (put(LAN_ON, "on\n", 3)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return; }
        } else if (exists(LAN_ON) && sc3(SYS_unlink, (long)LAN_ON, 0, 0) < 0) {
            response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return;
        }
        /* In-memory only here; network_tick persists it after the lock drops. */
        a->lan_act = sync_lan(a, exists(LAN_ON), a->lan_code);
        message(a, a->auth.enabled ? "Phone control is on. Enter the code shown on the TV." : "Phone control is off.");
    } else if (starts(c->input, "POST /api/scan HTTP/1.")) {
        /* Add Device: stop reconnecting, let go of the device, then search once
         * the radio is free (begin_add). One search at a time: each runs the radio
         * the controller shares for 16 s, so a repeat lets the running one finish. */
        if (ensure_bt(a)) { response(c, "503 Service Unavailable", "text/plain", "Bluetooth is not ready.", 23); return; }
        begin_add(a, c->loopback ? CONN_ADD_CONSOLE : CONN_ADD_PHONE);
    } else if (starts(c->input, "POST /api/scan-done HTTP/1.")) {
        end_add(a, "add: Add Device closed", 1);
    } else if (starts(c->input, "POST /api/select/")) {
        u64 mac = parse_mac(c->input + 17);
        refresh_devices(a);
        struct bt_device *d = device(a, mac);
        if (!mac || !d || !bt_is_audio(d)) { response(c, "400 Bad Request", "text/plain", "Select a listed audio device.", 29); return; }
        struct bt_device chosen = *d;
        u64 old = a->target.address;
        end_add(a, "add: ended, a device was chosen", 0);
        /* Already dialling this one: that attempt carries on. */
        if (!(old == mac && a->wanted && a->page_at)) {
            end_scan(a); stop_audio(a);
            if (old != mac) release_link(a); /* the old device's link or dial: the new dial waits for it */
            if (select_device(a, &chosen)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save selection.", 22); return; }
            a->wanted = 1; a->state = CONN_RECOVERING; a->next_try = 0;
            a->dropped = a->give_up = a->announced = a->session = 0;
            a->attempts = 0; a->relinks = 0; a->refused_since = 0;
            sc3(SYS_unlink, (long)TV_ON, 0, 0); a->tv = 0;
            say(a, "Connecting to ", "...");
        }
    } else if (starts(c->input, "POST /api/pin/")) {
        char pin[17];
        unsigned n = 0;
        const char *p = c->input + 14;
        while (p[n] >= '0' && p[n] <= '9' && n < 16) { pin[n] = p[n]; n++; }
        if (!a->pin_pending || !n || !starts(p + n, " HTTP/1.")) { response(c, "400 Bad Request", "text/plain", "Invalid pairing PIN.", 20); return; }
        pin[n] = 0;
        struct { u64 address; char *pin; unsigned length, pad; } q = {a->target.address, pin, n, 0};
        long rc = sc3(SYS_ioctl, a->bt, BT_PIN_REPLY, (long)&q);
        if (rc) { response(c, "500 Internal Server Error", "text/plain", "PIN submission failed.", 22); return; }
        a->pin_pending = 0;
        a->state = CONN_CONNECTING; a->deadline = a->clock() + 30000000;
        message(a, "Pairing PIN submitted.");
    } else if (starts(c->input, "POST /api/volume/")) {
        const char *p = c->input + 17;
        unsigned value = 0, n = 0;
        while (p[n] >= '0' && p[n] <= '9' && n < 3) { value = value * 10 + (unsigned)(p[n] - '0'); n++; }
        if (!n || value > 100 || !starts(p + n, " HTTP/1.")) { response(c, "400 Bad Request", "text/plain", "Volume must be 0-100.", 21); return; }
        if (save_volume(a, (int)value)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save volume.", 19); return; }
    } else if (starts(c->input, "POST /api/test HTTP/1.")) {
        if (!a->streaming || a->bt < 0 || a->btstate < 5 || a->btstate > 6) { response(c, "409 Conflict", "text/plain", "Connect a device first.", 23); return; }
        a->test_phase = 0; a->test_next = 0; a->test_until = a->clock() + 2000000; a->snap = 1;
        message(a, "Playing test tone.");
    } else if (starts(c->input, "POST /api/connect HTTP/1.")) {
        end_add(a, "add: ended, connect", 0);
        a->wanted = 1; a->relinks = 0;
        sc3(SYS_unlink, (long)TV_ON, 0, 0); a->tv = 0;
        if (!a->streaming && !a->page_at) { /* a dial already running carries on */
            end_scan(a); a->state = CONN_RECOVERING; a->next_try = 0;
            a->dropped = a->give_up = a->announced = a->session = 0;
            a->attempts = 0;
            say(a, "Connecting to ", "...");
        }
    } else if (starts(c->input, "POST /api/stop HTTP/1.")) {
        end_add(a, "add: ended, disconnect", 0);
        /* release_link holds the radio for a dial it cancels: a search right after
         * a Disconnect waits for that, too. */
        a->wanted = 0; end_scan(a); stop_audio(a); release_link(a);
        a->state = CONN_IDLE; a->relinks = 0; a->refused_since = 0;
        closefd(a->bt); a->bt = -1;
        a->dropped = a->give_up = a->announced = a->session = 0;
        a->attempts = 0;
        message(a, "Audio stopped. TV sound restored.");
    } else if (starts(c->input, "POST /api/joint-on HTTP/1.")) {
        if (!a->target.address) { response(c, "409 Conflict", "text/plain", "Select a device first.", 22); return; }
        bta_profile_touch(&a->profiles, &a->target);
        bta_profile_set_mode(&a->profiles, a->target.address, SBC_JOINT);
        if (save_profiles(a)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return; }
        a->mode = SBC_JOINT;
        message(a, "Joint stereo is on for this device.");
    } else if (starts(c->input, "POST /api/joint-off HTTP/1.")) {
        if (!a->target.address) { response(c, "409 Conflict", "text/plain", "Select a device first.", 22); return; }
        bta_profile_touch(&a->profiles, &a->target);
        bta_profile_set_mode(&a->profiles, a->target.address, SBC_DUAL);
        if (save_profiles(a)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return; }
        a->mode = SBC_DUAL;
        message(a, "Joint stereo is off for this device.");
    } else if (starts(c->input, "POST /api/latency/")) {
        const char *p = c->input + 18;
        if (p[0] < '0' || p[0] > '2' || !starts(p + 1, " HTTP/1.")) { response(c, "400 Bad Request", "text/plain", "Latency mode must be 0-2.", 25); return; }
        if (save_latency(a, p[0] - '0')) { response(c, "500 Internal Server Error", "text/plain", "Cannot save latency mode.", 25); return; }
        message(a, a->latency_mode == BTA_LAT_LOW ? "Latency mode: lowest." : a->latency_mode == BTA_LAT_STABLE ? "Latency mode: most stable." : "Latency mode: balanced.");
    } else if (starts(c->input, "POST /api/tv-on HTTP/1.")) {
        if (put(TV_ON, "on\n", 3)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return; }
        a->tv = 1; mute(a, 0);
    } else if (starts(c->input, "POST /api/tv-off HTTP/1.")) {
        if (exists(TV_ON) && sc3(SYS_unlink, (long)TV_ON, 0, 0) < 0) { response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return; }
        a->tv = 0;
        if (a->streaming) mute(a, 1);
    } else if (starts(c->input, "POST /api/quit HTTP/1.")) {
        a->wanted = 0; stop_audio(a); a->quit = 1;
        a->state = CONN_IDLE; a->dropped = a->give_up = a->announced = a->session = 0;
        a->attempts = 0;
        message(a, "Bluetooth Audio stopped. TV sound restored.");
    } else {
        response(c, "404 Not Found", "text/plain", "Not found", 9);
        return;
    }
    unsigned n = status(a, body, sizeof(body), c->loopback);
    response(c, "200 OK", "application/json", body, n);
}
/* Log one line per notable request, never the routine polls (the app asks for the
 * status twice a second): failures, pairings, and https:// attempts the plain-HTTP
 * server can't answer. A path that carries a secret (the pairing code, a PIN) is
 * cut before it. Each of the last four distinct lines is logged at most once per
 * 10 minutes (a forgotten phone tab polling the status, a button pressed again and
 * again), so retries can't fill the log. Called with the lock held. */
static void log_request(struct app *a, struct client *c, const char *what) {
    char line[96], req[48];
    struct writer w = {line, 0, sizeof(line)};
    append(&w, "http: ");
    if (what) append(&w, what);
    else {
        if (c->head < 12) return;
        const char *code = c->output + 9;
        int pair = starts(c->input, "POST /api/pair/");
        if (code[0] == '2' && !pair) return;
        unsigned i = 0;
        for (; c->input[i] && c->input[i] != '\r' && i < sizeof(req) - 1; i++)
            req[i] = c->input[i] >= 32 && c->input[i] < 127 ? c->input[i] : '?';
        req[i] = 0;
        char *p = find(req, " HTTP/");
        if (p) *p = 0;
        if ((p = find(req, "/api/pair/"))) p[9] = 0;
        if ((p = find(req, "/api/pin/"))) p[8] = 0;
        char status[4] = {code[0], code[1], code[2], 0};
        if (code[0] == '2') append(&w, "a phone paired");
        else { append(&w, status); append(&w, " "); append(&w, req); }
    }
    append(&w, c->loopback ? " (console)" : " (network)");
    u64 now = a->clock();
    int slot = 0;
    for (int i = 0; i < 4; i++) {
        if (equal(line, a->http_seen[i])) {
            if (now - a->http_at[i] < 600000000) return;
            slot = i;
            break;
        }
        if (a->http_at[i] < a->http_at[slot]) slot = i; /* else reuse the oldest */
    }
    memcpy(a->http_seen[slot], line, w.used + 1);
    a->http_at[slot] = now;
    log_text(a, line);
}
/* Rest mode aborts every socket. The aborted listener still completes a connection
 * and then resets it, so the app and the phone see the service as gone while audio
 * plays on. Open a fresh one (at most every 2 s) and drop the client connections,
 * which are dead too. `why` is the accept error, or 0 after a wake or a retry. */
static void reopen_listener(struct app *a, u64 now, long why) {
    for (int i = 0; i < 4; i++) { closefd(a->clients[i].fd); a->clients[i].fd = -1; }
    closefd(a->listener);
    a->listener = bta_listen();
    a->listen_at = now + 2000000;
    a->accept_errs = 0;
    lock_state(a);
    log_num(a, a->listener >= 0 ? "http: listening again; cause " : "http: port 8097 still busy; cause ", why);
    unlock_state(a);
}
static void network_tick(struct app *a, u64 now) {
    if (a->listener < 0 && now >= a->listen_at) reopen_listener(a, now, 0);
    for (int i = 0; i < 4; i++) {
        struct client *c = &a->clients[i];
        if (c->fd < 0) {
            if (a->listener < 0) continue;
            struct sockaddr4 peer;
            u32 len = sizeof(peer);
            long fd = sc3(SYS_accept, a->listener, (long)&peer, (long)&len);
            if (fd < 0) {
                /* -35: nobody is waiting. Other errors in a row mean the listening
                 * socket itself is broken. */
                if (fd != -35 && ++a->accept_errs >= 3 && now >= a->listen_at) reopen_listener(a, now, fd);
                continue;
            }
            a->accept_errs = 0;
            if (len != 16 || !local_ip((u8 *)&peer.addr) || nonblock(fd) < 0) { closefd(fd); continue; }
            int one = 1;
            sc5(SYS_setsockopt, fd, 0xffff, 0x800, (long)&one, 4);
            c->fd = fd; c->used = c->sent = c->head = c->total = 0; c->tail = 0; c->input[0] = 0; c->deadline = now + 3000000;
            c->loopback = (((u8 *)&peer.addr)[0] == 127); /* the console reaches us here */
            stir_entropy(a, a->clock() ^ ((u64)peer.addr << 16) ^ (u64)fd); /* feed the RNG fallback */
        }
        if (now > c->deadline) { closefd(c->fd); c->fd = -1; continue; }
        if (!c->total) {
            long n = sc3(SYS_read, c->fd, (long)(c->input + c->used), sizeof(c->input) - c->used - 1);
            if (n == 0 || (n < 0 && n != -35)) { closefd(c->fd); c->fd = -1; continue; }
            if (n > 0) {
                c->used += (unsigned)n;
                c->input[c->used] = 0;
                /* 0x16 opens a TLS handshake: the browser asked for https://, which
                 * this plain-HTTP server can't answer. Close, and say so in the log. */
                if ((u8)c->input[0] == 0x16) {
                    lock_state(a); log_request(a, c, "an https:// request; the page is http:// only"); unlock_state(a);
                    closefd(c->fd); c->fd = -1; continue;
                }
                if (find(c->input, "\r\n\r\n")) {
                    lock_state(a); dispatch(a, c); log_request(a, c, 0); unlock_state(a);
                    /* A phone-control toggle leaves its file write and TV notice
                     * here, outside the lock, so flash I/O can't stall the sender. */
                    if (a->lan_act) { persist_lan(a, a->lan_act, a->lan_code); a->lan_act = 0; }
                }
                else if (c->used == sizeof(c->input) - 1) response(c, "431 Request Header Fields Too Large", "text/plain", "Request too large", 17);
            }
        }
        if (c->total) {
            int in_head = c->sent < c->head;
            const char *from = in_head ? c->output + c->sent : c->tail + (c->sent - c->head);
            long n = sc3(SYS_write, c->fd, (long)from, (in_head ? c->head : c->total) - c->sent);
            if (n > 0) c->sent += (unsigned)n;
            if (c->sent == c->total || (n < 0 && n != -35)) { closefd(c->fd); c->fd = -1; }
        }
    }
}
/* Feed the driver the way the PS4's own audio daemon does. orbis_audiod creates
 * its Bluetooth senders as dedicated threads with explicit scheduling:
 * SCHED_FIFO, priority 0x132 (306) and CPU mask 0xc0 (cores 6-7), away from game
 * threads. Our sender does nothing but capture ring -> driver; the web page,
 * Bluetooth management and status file stay on the worker thread. One mutex
 * guards the shared state. If the console refuses the scheduling attributes, the
 * sender runs with defaults; if no thread can be created, the worker loop sends
 * audio itself, as before. */
static void init_lock(struct app *a) {
    /* Priority inheritance: while the sender waits for the lock, its holder runs
     * at the sender's priority, so a paused housekeeping thread cannot stall the
     * audio. */
    int (*init)(void **, void **, const char *) = 0, (*ainit)(void **) = 0, (*proto)(void **, int) = 0, (*adestroy)(void **) = 0;
    void *attr = 0;
    void **ap = 0;
    a->pi_rc = -1;
    if (!resolve("scePthreadMutexattrInit", &ainit) && ainit && !ainit(&attr)) {
        ap = &attr;
        if (!resolve("scePthreadMutexattrSetprotocol", &proto) && proto) a->pi_rc = proto(ap, 1);
    }
    if (resolve("scePthreadMutexInit", &init) || resolve("scePthreadMutexLock", &a->mlock) || resolve("scePthreadMutexUnlock", &a->munlock) || !init || !a->mlock || !a->munlock)
        a->mlock = a->munlock = 0;
    else if (init(&a->mutex, ap, "Bluetooth Audio state")) {
        a->pi_rc = -2;
        if (init(&a->mutex, 0, "Bluetooth Audio state")) a->mlock = a->munlock = 0;
    }
    if (ap && !resolve("scePthreadMutexattrDestroy", &adestroy) && adestroy) adestroy(ap);
}
static void *sender(void *arg) {
    struct app *a = arg;
    void *(*self)(void) = 0;
    int (*getprio)(void *, int *) = 0;
    if (!resolve("scePthreadSelf", &self) && !resolve("scePthreadGetprio", &getprio) && self && getprio) {
        int p = 0;
        if (!getprio(self(), &p)) a->sender_prio = p;
    }
    while (a->sender_run) {
        u64 now = a->clock();
        lock_state(a);
        audio_tick(a, now);
        unlock_state(a);
        a->sleep(500);
    }
    a->sender_alive = 0;
    return 0;
}
static void start_sender(struct app *a) {
    if (!a->mlock) return; /* no lock, no second thread */
    int (*create)(void **, void **, void *(*)(void *), void *, const char *) = 0;
    int (*detach)(void *) = 0;
    if (resolve("scePthreadCreate", &create) || resolve("scePthreadDetach", &detach) || !create || !detach) { a->thread_rc = -1; return; }
    int (*init)(void **) = 0, (*inherit)(void **, int) = 0, (*policy)(void **, int) = 0, (*param)(void **, const int *) = 0, (*affinity)(void **, u64) = 0, (*destroy)(void **) = 0;
    void *attr = 0;
    void **ap = 0;
    if (!resolve("scePthreadAttrInit", &init) && init && !init(&attr)) {
        ap = &attr;
        int prio = 0x132;
        if (!resolve("scePthreadAttrSetinheritsched", &inherit) && inherit) inherit(ap, 0);
        if (!resolve("scePthreadAttrSetschedpolicy", &policy) && policy) policy(ap, 1);
        a->prio_rc = (!resolve("scePthreadAttrSetschedparam", &param) && param) ? param(ap, &prio) : -1;
        a->aff_rc = (!resolve("scePthreadAttrSetaffinity", &affinity) && affinity) ? affinity(ap, 0xc0) : -1;
    }
    void *thread = 0;
    a->sender_run = 1;
    a->sender_alive = 1;
    a->thread_rc = create(&thread, ap, sender, a, "Bluetooth Audio sender");
    if (a->thread_rc && ap) {
        a->prio_rc = a->aff_rc = -2;
        a->thread_rc = create(&thread, 0, sender, a, "Bluetooth Audio sender");
    }
    if (a->thread_rc) { a->sender_run = 0; a->sender_alive = 0; } else detach(thread);
    if (ap && !resolve("scePthreadAttrDestroy", &destroy) && destroy) destroy(ap);
}
/* The sender uses this stack frame's state: wait for it to leave before returning. */
static void stop_sender(struct app *a) { a->sender_run = 0; while (a->sender_alive) a->sleep(1000); }

void *bta_worker(void *arg) {
    struct app a;
    memset(&a, 0, sizeof(a));
    a.listener = (long)arg;
    a.bt = a.hdmi = a.shmfd = -1;
    a.muted = -1;
    a.wanted = 0;
    for (int i = 0; i < 4; i++) a.clients[i].fd = -1;
    if (resolve("sceKernelUsleep", &a.sleep) || resolve("sceKernelGetProcessTime", &a.clock) || resolve("shm_open", &a.shm) || resolve("mmap", &a.map) || resolve("munmap", &a.unmap) || !a.sleep || !a.clock || !a.shm || !a.map || !a.unmap) {
        closefd(a.listener);
        return 0;
    }
    sc3(SYS_mkdir, (long)"/data/bta_audio", 0777, 0);
    sc3(SYS_chmod, (long)"/data/bta_audio", 0777, 0);
    sc3(SYS_rename, (long)SERVICE_LOG, (long)SERVICE_LOG_OLD, 0);
    a.log_t0 = a.clock();
    { char m[64], d[11];
      u64 w = wall_us();
      date_text(w / 1000000, d);
      compose(m, sizeof(m), "Bluetooth Audio service 2.8.0 started ", w ? d : "(no clock)", ", times UTC");
      log_text(&a, m); }
    sc3(SYS_unlink, (long)BTA_STOP, 0, 0);
    resolve("sceKernelGetRandomNumber", &a.getrandom); /* optional; fill_random falls back */
    load_profiles(&a);
    init_target(&a);
    load_volume(&a);
    load_latency(&a);
    a.tv = exists(TV_ON);
    sc3(SYS_unlink, (long)PAIR_CODE, 0, 0); /* a code never outlives the service session */
    { char code[BTA_CODE_DIGITS + 1]; /* phone control stays off unless the flag file is present */
      persist_lan(&a, sync_lan(&a, exists(LAN_ON), code), code); }
    a.hdmi = sc3(SYS_open, (long)"/dev/hdmi", 2, 0);
    if (a.wanted) {
        /* A remembered device exists: reconnect to whichever one is turned on. */
        a.state = CONN_RECOVERING; a.next_try = 0;
        say(&a, "Reconnecting to ", "...");
        notify("Bluetooth Audio is reconnecting to your last audio device. Open the app to change the device or settings.");
    } else {
        message(&a, "Ready. Select an audio device to connect.");
        notify("Bluetooth Audio is ready. Open the app to connect an audio device. Web controls: http://127.0.0.1:8097/ (or your PS4 IP on port 8097).");
    }
    init_lock(&a);
    start_sender(&a);
    while (!a.quit) {
        u64 now = a.clock(), wall = wall_us();
        /* Rest-mode resume (see conn_slept): the driver handle, the in-RAM firmware
         * patch and the sockets may be stale. Tear down, drop any half-done connect
         * (it may have started as the console went to sleep), reopen the listener
         * and let the normal connect path (which re-applies the patch and reopens
         * /dev/bt) reconnect from scratch. The process-clock check alone skips the
         * case where audio is still flowing (a recently accepted packet): then the
         * gap was only this normal-priority worker thread being starved while the
         * real-time sender kept streaming, not a real suspend. */
        u64 slept = a.last_wall && wall > a.last_wall ? wall - a.last_wall : 0;
        if (conn_slept(now - a.last_tick, slept, CONN_RESUME_GAP_US) ||
            (conn_resumed(a.last_tick, now, CONN_RESUME_GAP_US) &&
             !(a.streaming && now >= a.ok_at && now - a.ok_at < CONN_RESUME_GAP_US))) {
            lock_state(&a);
            log_num(&a, "wake: seconds asleep ", (long)(slept / 1000000));
            stop_audio(&a);
            closefd(a.bt); a.bt = -1;
            a.own_link = 0; a.redial_at = 0; /* a link that outlived the sleep is not trusted: dial afresh */
            /* No dial or search outlived it either; the radio rests before the next. */
            a.page_at = 0; a.hold = CONN_HOLD_NONE; a.scanning = 0; a.scan_wait = 0;
            a.radio_quiet_at = now + CONN_SCAN_QUIET_US;
            if (a.wanted) {
                a.state = CONN_RECOVERING; a.dropped = a.give_up = 0; a.attempts = 0;
                a.next_try = now + 2000000; /* let the console's own Bluetooth come back first */
                message(&a, "Woke from rest mode. Reconnecting...");
            } else a.state = CONN_IDLE;
            unlock_state(&a);
            reopen_listener(&a, now, 0);
        }
        a.last_tick = now;
        a.last_wall = wall;
        if (now >= a.net_at) { network_tick(&a, now); a.net_at = now + 4000; } /* locks around each request */
        if (a.quit) break;
        char json[12000], logbuf[sizeof(a.log)];
        unsigned n = 0;
        u32 logn = 0;
        int beat = now >= a.heartbeat;
        /* Take the lock only when there is shared-state work (~35 times a second). */
        if (now < a.poll_at && a.sender_alive && now < a.meter_at && !beat) { a.sleep(500); continue; }
        lock_state(&a);
        if (now >= a.poll_at) { bluetooth_tick(&a, now); a.poll_at = now + 50000; }
        if (!a.sender_alive) audio_tick(&a, now); /* fallback: no sender thread */
        /* Output meter: peak of the last ~80 ms, with a quick decay so a client
         * that polls a few times a second still sees movement. */
        if (now >= a.meter_at) {
            unsigned p = a.meter_peak * 100 / 32767;
            if (p > 100) p = 100;
            unsigned hold = (unsigned)a.meter * 7 / 10;
            a.meter = (int)(p > hold ? p : hold);
            a.meter_peak = 0;
            a.meter_at = now + 80000;
        }
        if (beat) {
            /* status.json is only for reading over FTP (clients use /api/status):
             * every 10 s spares the disk a write a second. reveal=0: the code
             * never lands in it. */
            if (now >= a.status_at) { n = status(&a, json, sizeof(json), 0); a.status_at = now + 10000000; }
            if (a.streaming && now >= a.stats_at) { log_stats(&a, "audio: "); a.stats_at = now + 600000000; }
            /* The PS4 sets HDMI up again, which clears its mute, whenever a game or the
             * home screen comes to the front (KLog: "Hdmi Setup" on each switch), and
             * mute() only acts on a change it knows about. Put the mute back each
             * second while it should hold. */
            if (a.streaming && a.muted == 1 && a.hdmi >= 0) { int v = 1; a.hdmi_rc = sc3(SYS_ioctl, a.hdmi, HDMI_SET_MUTE, (long)&v); }
            a.level = 0; a.heartbeat = now + 1000000; logn = take_log(&a, logbuf);
        }
        unlock_state(&a);
        if (beat) {
            /* File I/O stays outside the lock: a slow write must never hold up the sender. */
            if (n) put("/data/bta_audio/status.json", json, n);
            write_log(&a, logbuf, logn);
            if (a.profile_dirty) {
                lock_state(&a);
                struct bta_profiles snap = a.profiles; a.profile_dirty = 0;
                unlock_state(&a);
                put_profiles(&snap);
            }
            if (exists(BTA_STOP)) break;
            int tv = exists(TV_ON);
            int lan = exists(LAN_ON);
            char lancode[BTA_CODE_DIGITS + 1];
            int lanact;
            lock_state(&a);
            a.tv = tv;
            lanact = sync_lan(&a, lan, lancode); /* owner may toggle phone control via the flag file or the app */
            unlock_state(&a);
            persist_lan(&a, lanact, lancode); /* file write and TV notice stay outside the lock */
            /* The address can arrive late (network still coming up) or change (DHCP). */
            if (a.auth.enabled && now >= a.ip_at) { a.ip = lan_address(); a.ip_at = now + (a.ip ? 30000000 : 5000000); }
            /* Disconnect is detected by the audio send failing (see audio_tick),
             * as the proven bridge did. No per-second driver state poll while streaming. */
        }
        a.sleep(500);
    }
    stop_sender(&a);
    end_scan(&a);
    stop_audio(&a);
    drop_link(&a); /* leave no idle link behind: the next run would find it stale (see conn_plan_link) */
    a.wanted = 0;
    message(&a, "Bluetooth Audio is offline. TV sound restored.");
    char json[12000];
    unsigned n = status(&a, json, sizeof(json), 0);
    put("/data/bta_audio/status.json", json, n);
    write_log(&a, a.log, a.log_len); /* the sender has stopped: no lock, no copy */
    for (int i = 0; i < 4; i++) closefd(a.clients[i].fd);
    closefd(a.listener);
    closefd(a.bt);
    closefd(a.hdmi);
    return 0;
}
