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
 *   core.h     the truthful link-state derivation a UI must show
 *   timing.h   per-second send statistics (GET /api/timing)
 *
 * The firmware-specific driver helpers (bt_devices.c, bt_accept_patch.c) and
 * the single firmware table (firmware.h) sit behind bt_list / bt_prepare /
 * bta_apply_patch, so no firmware offset appears here. */
#include "runtime.h"
#include "web_page.h"
#include "protocol.h"
#include "json.h"
#include "strfmt.h"
#include "dsp.h"
#include "core.h"
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
#define SYS_ioctl   54
#define SYS_setsockopt 105
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

struct audio_req { u64 address; void *samples; u64 length; u32 mode, pad; };
struct client { long fd; u64 deadline; u32 used, sent, total; char input[2048]; char output[20000]; };
struct app {
    int (*sleep)(unsigned);
    u64 (*clock)(void);
    int (*shm)(const char *, int, int);
    void *(*map)(void *, u64, int, int, int, u64);
    int (*unmap)(void *, u64);
    long listener, bt, hdmi, shmfd;
    u8 *ring;
    u32 readpos;
    int wanted, streaming, tv, muted, quit, phase;
    long patch_rc, packet_rc, connect_rc, last_rc, hdmi_rc, btstate;
    u64 next_try, deadline, scan_at, heartbeat, poll_at, net_at, packets, errors, resyncs;
    unsigned level;
    int volume;
    char message[128];
    /* Per-second send timing (see timing.h). */
    struct bta_second secs[128], cur;
    u32 sec_seq;
    u64 sec_start, last_send;
    int joint;
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
    /* Truthful link state (see core.h / link_state). dropped: the link was lost
     * while streaming. failed: the last attempt failed. announced: a
     * "disconnected" notice was shown. session: this connection was announced. */
    int dropped, failed, announced, session, meter;
    u32 attempts;
    u64 ok_at, meter_at, notify_at[3];
    unsigned meter_peak;
    struct bt_device target;
    struct bt_devices devices;
    int scanning, pin_pending;
    u64 scan_end, list_at;
    struct client clients[4];
};

static void lock_state(struct app *a) { if (a->mlock) a->mlock(&a->mutex); }
static void unlock_state(struct app *a) { if (a->munlock) a->munlock(&a->mutex); }

static void message(struct app *a, const char *s) {
    memset(a->message, 0, sizeof(a->message));
    u64 n = length(s);
    if (n > 127) n = 127;
    memcpy(a->message, s, n);
}
static const char *target_name(struct app *a) {
    return a->target.name[0] ? a->target.name : "your headphones";
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
static void stop_audio(struct app *a) {
    if (a->phase == 4 && a->bt >= 0) { sc3(SYS_ioctl, a->bt, BT_SCAN_STOP, 0); a->scanning = 0; }
    if (a->streaming) {
        short silence[384];
        memset(silence, 0, sizeof(silence));
        struct audio_req q = {a->target.address, silence, sizeof(silence), 0, 0};
        sc3(SYS_ioctl, a->bt, BT_SEND_AUDIO, (long)&q);
    }
    a->streaming = 0; a->phase = 0; a->level = 0; a->meter = 0; a->meter_peak = 0;
    a->pin_pending = 0; a->test_until = 0; mute(a, 0);
    if (a->ring) { a->unmap(a->ring, 0x10000); a->ring = 0; }
    closefd(a->shmfd); a->shmfd = -1;
}
/* A failed attempt: stop, explain, and try again after delay_us. */
static void retry_after(struct app *a, const char *s, u64 delay_us) {
    stop_audio(a); message(a, s); a->failed = 1; a->next_try = a->clock() + delay_us;
}
static void retry(struct app *a, const char *s) { retry_after(a, s, 10000000); }
static void retry_named(struct app *a, const char *pre, const char *suf, u64 delay_us) {
    char m[128];
    compose(m, sizeof(m), pre, target_name(a), suf);
    retry_after(a, m, delay_us);
}
/* The PS4 audio stream was rebuilt (e.g. a game started). The headphone link is
 * fine, so this is a quiet reconnect of the audio path, not a failure or a drop. */
static void restart_audio(struct app *a, const char *s) {
    stop_audio(a); message(a, s); a->next_try = a->clock() + 2000000;
}
/* Drop the Bluetooth link itself, not just the audio. The driver keeps per-link
 * stream state (e.g. a stuck format renegotiation) that survives an audio stop. */
static void drop_link(struct app *a) {
    if (a->bt >= 0 && a->target.address) {
        u64 address = a->target.address;
        sc3(SYS_ioctl, a->bt, BT_DROP_LINK, (long)&address);
    }
}
/* Automatic recovery: the PS4 refused every packet for REFUSED_US. Do what a
 * user would by hand: drop the link and reconnect. At most 3 times in a row;
 * any accepted packet resets the count. */
static void relink(struct app *a) {
    a->relinks++; a->relinks_total++; stop_audio(a); drop_link(a);
    a->phase = 0; a->next_try = a->clock() + 3000000; a->refused_since = 0;
    say(a, "The PS4 stopped accepting audio for ", ". Reconnecting...");
}
/* The headphones went away mid-stream, detected only by consecutive audio-send
 * failures (never by polling the driver while streaming: that starved the
 * controller). stop_audio restores TV sound. */
static void link_lost(struct app *a) {
    stop_audio(a); a->dropped = 1; a->failed = 0; a->next_try = a->clock() + 2000000;
    say(a, "", " disconnected. TV sound is back. Reconnecting...");
    if (may_announce(a, 1)) {
        notify("Bluetooth headphones disconnected. TV sound is back on. Reconnecting...");
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
static void end_scan(struct app *a) {
    if (a->scanning && a->bt >= 0) sc3(SYS_ioctl, a->bt, BT_SCAN_STOP, 0);
    a->scanning = 0;
}
static int scan(struct app *a) {
    if (ensure_bt(a)) return -1;
    long rc = sc3(SYS_ioctl, a->bt, BT_SCAN_START, 0);
    if (rc < 0) return -1;
    a->scanning = 1; a->scan_end = a->clock() + 16000000; a->list_at = 0;
    return 0;
}
struct saved_device { u64 magic; struct bt_device device; };
static int save_target(struct app *a, const struct bt_device *d) {
    struct saved_device s = {.magic = 0x3254454752415442UL, .device = *d};
    if (put("/data/bta_audio/device.tmp", &s, sizeof(s))) return -1;
    if (sc3(SYS_rename, (long)"/data/bta_audio/device.tmp", (long)"/data/bta_audio/device.bin", 0) < 0) return -1;
    a->target = *d;
    return 0;
}
static void load_target(struct app *a) {
    a->target.address = 0; a->target.services = 0;
    long fd = sc3(SYS_open, (long)"/data/bta_audio/device.bin", 0, 0);
    if (fd < 0) return;
    struct saved_device s;
    long n = sc3(SYS_read, fd, (long)&s, sizeof(s));
    closefd(fd);
    if (n == sizeof(s) && s.magic == 0x3254454752415442UL && s.device.address && !(s.device.address >> 48)) {
        s.device.name[95] = 0;
        a->target = s.device;
    }
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
static void begin_connect(struct app *a) {
    a->patch_rc = bta_apply_patch();
    if (a->patch_rc) { retry(a, "Bluetooth setup failed. Check the diagnostic status."); return; }
    if (ensure_bt(a)) { retry(a, "Bluetooth is not ready. Retrying shortly."); return; }
    refresh_devices(a);
    struct bt_device *t = device(a, a->target.address);
    if (!t) {
        if (scan(a)) { retry(a, "Bluetooth scan could not start. Retrying shortly."); return; }
        a->phase = 4; a->deadline = a->clock() + 16000000;
        say(a, "Looking for ", "... Put it in pairing mode if needed.");
        return;
    }
    if (!bt_is_audio(t)) { a->wanted = 0; retry(a, "This device does not advertise headphone audio support."); return; }
    end_scan(a);
    u64 address = a->target.address;
    a->btstate = sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address);
    a->phase = 1; a->deadline = a->clock() + 30000000;
    say(a, "Connecting to ", "...");
    if (a->btstate < 5 || a->btstate > 6) {
        sc3(SYS_ioctl, a->bt, BT_SCAN_STOP, 0);
        a->connect_rc = sc3(SYS_ioctl, a->bt, BT_CONNECT, (long)&address);
        /* An immediate driver error (e.g. 0x802F0204) means this device will not
         * connect right now; fail fast instead of waiting out the 30 s window. */
        if (a->connect_rc < 0) {
            retry_named(a, "Couldn't connect to ", ". Make sure it's on, nearby and not connected to another device.", 8000000);
            return;
        }
    } else a->connect_rc = 0;
}
static void start_audio(struct app *a) {
    a->shmfd = a->shm("/shm_acap.full", 0, 0);
    if (a->shmfd < 0) { retry(a, "Waiting for the PS4 audio service."); return; }
    void *p = a->map(0, 0x10000, 1, 1, a->shmfd, 0);
    if (!p || p == (void *)-1) { retry(a, "Unable to open the game audio stream."); return; }
    a->ring = p;
    a->readpos = *(volatile u32 *)(a->ring + 12);
    if (*(volatile u32 *)a->ring != 0x10000 || a->readpos < 16 || a->readpos >= 0x10000 || (a->readpos & 7)) {
        retry(a, "The PS4 audio stream has an unexpected format.");
        return;
    }
    a->streaming = 1; a->phase = 3; a->refused_since = 0; a->errors = 0;
    a->stream_at = a->clock(); a->test_until = 0; a->test_next = 0; a->test_phase = 0; a->snap = 0;
    bta_pace_reset(&a->pace, a->stream_at); a->drops = 0; a->underruns = 0;
    int was_dropped = a->dropped, told = a->announced, fresh = !a->session;
    a->attempts = 0; a->failed = 0; a->dropped = 0; a->announced = 0; a->session = 1;
    a->ok_at = a->stream_at; a->meter = 0; a->meter_peak = 0;
    say(a, "Connected to ", a->tv ? ". TV sound is on." : ". TV sound is muted.");
    if (was_dropped) {
        if (told && may_announce(a, 2)) notify("Bluetooth headphones reconnected.");
    } else if (fresh && may_announce(a, 0)) {
        char m[200];
        compose(m, sizeof(m), "Bluetooth headphones connected: ", target_name(a),
                a->tv ? ". TV sound stays on." : ". TV sound muted.");
        notify(m);
    }
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
        struct { u8 type, subtype; u16 reserved; u32 data; u64 address; } ev[16];
        long n = sc3(SYS_read, a->bt, (long)ev, sizeof(ev));
        if (n > 0 && n <= (long)sizeof(ev))
            for (unsigned i = 0; i < (unsigned)n / 16; i++)
                if (ev[i].address == a->target.address && a->wanted) {
                    if (ev[i].type == 4) { u64 q[2] = {a->target.address, 1}; sc3(SYS_ioctl, a->bt, BT_PAIR_ACCEPT, (long)q); }
                    if (ev[i].type == 3) { a->pin_pending = 1; a->deadline = now + 60000000; message(a, "These headphones require a pairing PIN."); }
                }
    }
    if (!a->wanted) return;
    if (a->phase == 0) {
        if (now >= a->next_try) { a->attempts++; a->failed = 0; begin_connect(a); }
        return;
    }
    if (a->phase == 4) {
        if (device(a, a->target.address)) { end_scan(a); a->phase = 0; begin_connect(a); return; }
        if (now >= a->deadline) retry_named(a, "Couldn't find ", ". Put it in pairing mode.", 8000000);
        return;
    }
    if (a->phase == 1) {
        u64 address = a->target.address;
        a->btstate = sc3(SYS_ioctl, a->bt, BT_LINK_STATE, (long)&address);
        if (a->btstate >= 5 && a->btstate <= 6) {
            a->pin_pending = 0;
            if (exists(NO_WIDEN)) {
                a->packet_rc = 0; /* diagnostic: leave controller link params untouched */
            } else {
                a->packet_rc = bt_prepare(address);
                if (a->packet_rc) { retry(a, "No supported Bluetooth audio link. Retrying shortly."); return; }
            }
            a->phase = 2; a->deadline = now + 400000;
        } else if (now >= a->deadline) {
            retry_named(a, "", " didn't respond. Is it on and nearby?", 8000000);
        }
    } else if (a->phase == 2 && now >= a->deadline) start_audio(a);
}
static void roll_second(struct app *a, u64 now) {
    bta_roll_second(a->secs, &a->cur, &a->sec_seq, &a->sec_start, now);
}
/* Joint stereo is the default. A headphone can opt out (old dual-channel format)
 * with an empty file /data/bta_audio/dual-<address>. Send type 3 makes the
 * driver encode dual-channel SBC at half bitpool per channel, a format
 * headphones did not agree to at connect, so it renegotiates the stream. Type 4
 * is joint stereo at full bitpool, the format headphones normally agree to. */
static void load_joint(struct app *a) {
    char p[40];
    joint_path(a->target.address, p);
    a->joint = !(a->target.address && exists(p));
}
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
    struct audio_req req = {a->target.address, pcm, sizeof(pcm), a->joint ? 4 : 3, 0};
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
        if (!a->refused_since) a->refused_since = now;
        else if (now - a->refused_since > REFUSED_US && a->relinks < 3) { relink(a); return; }
        else if (now - a->refused_since > REFUSED_US && a->relinks == 3) {
            a->relinks++;
            say(a, "The PS4 keeps refusing audio for ", ". Turn the headphones off and on.");
        }
    } else if (a->last_rc > 0) { a->refused_since = 0; a->relinks = 0; }
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
/* The one place the connection's real state is decided (see core.h). A UI must
 * show this, never infer it from "selected" or "wanted". */
static int link_state(struct app *a) {
    return bta_link_state(a->streaming, a->wanted, a->phase, a->dropped, a->failed);
}
static int retry_in_ms(struct app *a, u64 now) {
    return bta_retry_in_ms(a->wanted, a->streaming, a->phase, a->next_try, now);
}
static int quiet_for_ms(struct app *a, u64 now) {
    return bta_quiet_for_ms(a->streaming, a->ok_at, now);
}
static unsigned status(struct app *a, char *out, unsigned cap) {
    u64 now = a->clock();
    struct writer w = {out, 0, cap};
    append(&w, "{\"version\":\"2.7.5\",\"message\":");
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
    field(&w, "joint", a->joint);
    field(&w, "send_type", a->joint ? 4 : 3);
    field(&w, "relinks", a->relinks_total);
    field(&w, "latency_mode", a->latency_mode);
    field(&w, "drops", a->drops);
    field(&w, "underruns", a->underruns);
    field(&w, "refused_ms", a->refused_since ? (long)((now - a->refused_since) / 1000) : 0);
    field(&w, "hdmi_muted", a->muted == 1);
    field(&w, "scanning", a->scanning);
    field(&w, "pin_pending", a->pin_pending);
    field(&w, "test", now < a->test_until);
    field(&w, "link", link_state(a));
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
        append(&w, "}");
    }
    append(&w, "]}");
    return w.used;
}
static unsigned timing_json(struct app *a, char *out, unsigned cap) {
    return bta_timing_json(a->secs, a->sec_seq, (long)(a->clock() / 1000000), out, cap);
}
static unsigned native_status(struct app *a, char *out) {
    struct bt_native *n = (void *)out;
    memset(n, 0, sizeof(*n));
    n->magic = BT_NATIVE_MAGIC; n->version = 2; n->volume = a->volume;
    n->streaming = a->streaming; n->wanted = a->wanted; n->tv = a->tv;
    n->muted = a->muted == 1; n->scanning = a->scanning; n->pin_pending = a->pin_pending;
    n->selected = a->target.address;
    memcpy(n->message, a->message, sizeof(n->message));
    for (unsigned i = 0; i < a->devices.count; i++)
        if (bt_is_audio(&a->devices.items[i]) || a->devices.items[i].address == a->target.address)
            n->devices[n->count++] = a->devices.items[i];
    u64 now = a->clock();
    n->link = link_state(a); n->attempts = (int)a->attempts; n->retry_ms = retry_in_ms(a, now);
    n->meter = a->meter; n->quiet_ms = quiet_for_ms(a, now); n->test = now < a->test_until;
    n->connect_rc = (int)a->connect_rc; n->audio_rc = (int)a->last_rc;
    return sizeof(*n);
}
static void response(struct client *c, const char *code, const char *type, const char *body, unsigned n) {
    struct writer w = {c->output, 0, sizeof(c->output)};
    append(&w, "HTTP/1.1 ");
    append(&w, code);
    append(&w, "\r\nContent-Type: ");
    append(&w, type);
    append(&w, "\r\nContent-Length: ");
    number(&w, n);
    append(&w, "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'\r\n\r\n");
    if (w.used + n > sizeof(c->output)) { c->total = 0; return; }
    memcpy(c->output + w.used, body, n);
    c->total = w.used + n;
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
static void dispatch(struct app *a, struct client *c) {
    char body[12000];
    if (!host_valid(c->input)) { response(c, "403 Forbidden", "text/plain", "Use the PS4 IP address.", 23); return; }
    if (starts(c->input, "GET / HTTP/1.")) { response(c, "200 OK", "text/html; charset=utf-8", web_page, sizeof(web_page) - 1); return; }
    if (starts(c->input, "GET /api/native HTTP/1.")) { unsigned n = native_status(a, body); response(c, "200 OK", "application/octet-stream", body, n); return; }
    if (starts(c->input, "GET /api/status HTTP/1.")) { unsigned n = status(a, body, sizeof(body)); response(c, "200 OK", "application/json", body, n); return; }
    if (starts(c->input, "GET /api/timing HTTP/1.")) { unsigned n = timing_json(a, body, sizeof(body)); response(c, "200 OK", "application/json", body, n); return; }
    char *control = header(c->input, "x-bta-control");
    if (!starts(c->input, "POST ") || !control || !starts(control, "1\r\n")) {
        response(c, "403 Forbidden", "text/plain", "Control header required.", 24);
        return;
    }
    if (starts(c->input, "POST /api/scan HTTP/1.")) {
        if (a->streaming) { response(c, "409 Conflict", "text/plain", "Stop audio before scanning.", 27); return; }
        if (scan(a)) { response(c, "503 Service Unavailable", "text/plain", "Scan could not start", 20); return; }
    } else if (starts(c->input, "POST /api/select/")) {
        u64 mac = parse_mac(c->input + 17);
        refresh_devices(a);
        struct bt_device *d = device(a, mac);
        if (!mac || !d || !bt_is_audio(d)) { response(c, "400 Bad Request", "text/plain", "Select a listed audio device.", 29); return; }
        struct bt_device chosen = *d;
        u64 old = a->target.address;
        end_scan(a); stop_audio(a);
        if (save_target(a, &chosen)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save selection.", 22); return; }
        load_joint(a);
        if (old != mac && a->bt >= 0) sc3(SYS_ioctl, a->bt, BT_DROP_LINK, (long)&old);
        a->wanted = 1; a->next_try = 0;
        a->dropped = a->failed = a->announced = a->session = 0;
        a->attempts = 0; a->relinks = 0; a->refused_since = 0;
        sc3(SYS_unlink, (long)TV_ON, 0, 0); a->tv = 0;
        say(a, "Connecting to ", "...");
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
        message(a, "Pairing PIN submitted.");
    } else if (starts(c->input, "POST /api/volume/")) {
        const char *p = c->input + 17;
        unsigned value = 0, n = 0;
        while (p[n] >= '0' && p[n] <= '9' && n < 3) { value = value * 10 + (unsigned)(p[n] - '0'); n++; }
        if (!n || value > 100 || !starts(p + n, " HTTP/1.")) { response(c, "400 Bad Request", "text/plain", "Volume must be 0-100.", 21); return; }
        if (save_volume(a, (int)value)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save volume.", 19); return; }
    } else if (starts(c->input, "POST /api/test HTTP/1.")) {
        if (!a->streaming || a->bt < 0 || a->btstate < 5 || a->btstate > 6) { response(c, "409 Conflict", "text/plain", "Connect headphones first.", 25); return; }
        a->test_phase = 0; a->test_next = 0; a->test_until = a->clock() + 2000000; a->snap = 1;
        message(a, "Playing test tone.");
    } else if (starts(c->input, "POST /api/connect HTTP/1.")) {
        a->wanted = 1; a->relinks = 0;
        sc3(SYS_unlink, (long)TV_ON, 0, 0); a->tv = 0;
        if (!a->streaming) {
            end_scan(a); a->phase = 0; a->next_try = 0;
            a->dropped = a->failed = a->announced = a->session = 0;
            a->attempts = 0;
            say(a, "Connecting to ", "...");
        }
    } else if (starts(c->input, "POST /api/stop HTTP/1.")) {
        a->wanted = 0; end_scan(a); stop_audio(a); drop_link(a);
        a->relinks = 0; a->refused_since = 0;
        closefd(a->bt); a->bt = -1;
        a->dropped = a->failed = a->announced = a->session = 0;
        a->attempts = 0;
        message(a, "Earbud audio stopped. TV sound restored.");
    } else if (starts(c->input, "POST /api/joint-on HTTP/1.")) {
        if (!a->target.address) { response(c, "409 Conflict", "text/plain", "Select headphones first.", 24); return; }
        char p[40];
        joint_path(a->target.address, p);
        sc3(SYS_unlink, (long)p, 0, 0);
        a->joint = 1;
        message(a, "Joint stereo is on for these headphones.");
    } else if (starts(c->input, "POST /api/joint-off HTTP/1.")) {
        if (!a->target.address) { response(c, "409 Conflict", "text/plain", "Select headphones first.", 24); return; }
        char p[40];
        joint_path(a->target.address, p);
        if (put(p, "on\n", 3)) { response(c, "500 Internal Server Error", "text/plain", "Cannot save preference.", 23); return; }
        a->joint = 0;
        message(a, "Joint stereo is off for these headphones.");
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
        a->dropped = a->failed = a->announced = a->session = 0;
        a->attempts = 0;
        message(a, "Bluetooth Audio stopped. TV sound restored.");
    } else {
        response(c, "404 Not Found", "text/plain", "Not found", 9);
        return;
    }
    unsigned n = status(a, body, sizeof(body));
    response(c, "200 OK", "application/json", body, n);
}
static void network_tick(struct app *a, u64 now) {
    for (int i = 0; i < 4; i++) {
        struct client *c = &a->clients[i];
        if (c->fd < 0) {
            struct sockaddr4 peer;
            u32 len = sizeof(peer);
            long fd = sc3(SYS_accept, a->listener, (long)&peer, (long)&len);
            if (fd < 0) continue;
            if (len != 16 || !local_ip((u8 *)&peer.addr) || nonblock(fd) < 0) { closefd(fd); continue; }
            int one = 1;
            sc5(SYS_setsockopt, fd, 0xffff, 0x800, (long)&one, 4);
            c->fd = fd; c->used = c->sent = c->total = 0; c->input[0] = 0; c->deadline = now + 3000000;
        }
        if (now > c->deadline) { closefd(c->fd); c->fd = -1; continue; }
        if (!c->total) {
            long n = sc3(SYS_read, c->fd, (long)(c->input + c->used), sizeof(c->input) - c->used - 1);
            if (n == 0 || (n < 0 && n != -35)) { closefd(c->fd); c->fd = -1; continue; }
            if (n > 0) {
                c->used += (unsigned)n;
                c->input[c->used] = 0;
                if (find(c->input, "\r\n\r\n")) { lock_state(a); dispatch(a, c); unlock_state(a); }
                else if (c->used == sizeof(c->input) - 1) response(c, "431 Request Header Fields Too Large", "text/plain", "Request too large", 17);
            }
        }
        if (c->total) {
            long n = sc3(SYS_write, c->fd, (long)(c->output + c->sent), c->total - c->sent);
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
    sc3(SYS_unlink, (long)BTA_STOP, 0, 0);
    load_target(&a);
    load_joint(&a);
    load_volume(&a);
    load_latency(&a);
    a.tv = exists(TV_ON);
    a.hdmi = sc3(SYS_open, (long)"/dev/hdmi", 2, 0);
    message(&a, "Ready. Select your headphones and press Connect.");
    notify("Bluetooth Audio is ready and idle. Open the home-screen app, then press Connect. Web controls: http://127.0.0.1:8097/ (or your PS4 IP on port 8097).");
    init_lock(&a);
    start_sender(&a);
    while (!a.quit) {
        u64 now = a.clock();
        if (now >= a.net_at) { network_tick(&a, now); a.net_at = now + 4000; } /* locks around each request */
        if (a.quit) break;
        char json[12000];
        unsigned n = 0;
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
        if (beat) { n = status(&a, json, sizeof(json)); a.level = 0; a.heartbeat = now + 1000000; }
        unlock_state(&a);
        if (beat) {
            /* File I/O stays outside the lock: a slow write must never hold up the sender. */
            put("/data/bta_audio/status.json", json, n);
            if (exists(BTA_STOP)) break;
            int tv = exists(TV_ON);
            lock_state(&a);
            a.tv = tv;
            unlock_state(&a);
            /* Disconnect is detected by the audio send failing (see audio_tick),
             * as the proven bridge did. No per-second driver state poll while streaming. */
        }
        a.sleep(500);
    }
    stop_sender(&a);
    end_scan(&a);
    stop_audio(&a);
    a.wanted = 0;
    message(&a, "Bluetooth Audio is offline. TV sound restored.");
    char json[12000];
    unsigned n = status(&a, json, sizeof(json));
    put("/data/bta_audio/status.json", json, n);
    for (int i = 0; i < 4; i++) closefd(a.clients[i].fd);
    closefd(a.listener);
    closefd(a.bt);
    closefd(a.hdmi);
    return 0;
}
