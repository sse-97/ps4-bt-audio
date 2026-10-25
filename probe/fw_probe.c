/* Read-only firmware probe. A standalone GoldHEN payload that recovers the kernel
 * text base from the LSTAR MSR (exactly as the accept patch does before it
 * fingerprints) and, on the firmware firmware.h describes, copies a short byte
 * window around each of its nine offsets so cross-firmware search signatures can
 * be built from the real bytes. It PATCHES NOTHING and WRITES NOTHING to the
 * kernel: it only reads kernel memory in ring 0 and hands the bytes back to a
 * userland buffer, which _start saves to /data/bta_audio/.
 *
 * On any other firmware the known offsets would point at unrelated bytes, so the
 * probe reads none of them: it records only the firmware version and whether the
 * base was recovered. That makes it safe to run on a console whose layout we do
 * not yet know — the worst case is a report that says "no table for this
 * firmware", never a wrong kernel access.
 *
 * Built standalone (its own _start at offset 0), sent with GoldHEN's payloader
 * like the audio service; it exits immediately after writing its report. */
#include "../app/firmware.h"
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

__attribute__((noinline)) static long sc3(long n, long a, long b, long c) {
    long out;
    __asm__ volatile("syscall; jnc 1f; neg %%rax; 1:" : "=a"(out), "+d"(c), "+D"(a), "+S"(b) : "a"(n) : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return out;
}

#define PROBE_VERSION "2.8.0"
#define TARGETS 9
#define BEFORE 16u             /* bytes captured before each offset */
#define WINDOW 64u             /* total bytes per window (offset byte at index BEFORE) */
#define PROBE_BIN "/data/bta_audio/fw_probe.bin"
#define PROBE_TXT "/data/bta_audio/fw_probe.txt"

/* What _start hands the kernel callback: where to copy the capture, and whether
 * this firmware is the one the offsets describe (only then are they read). */
struct capture {
    u64 magic;                 /* 'BTAPROBE' so the file is self-identifying */
    u64 base;                  /* recovered kernel text base (0 if invalid) */
    u32 version;               /* firmware as reported by the kernel path */
    u32 count;                 /* windows filled (0 unless base valid and known fw) */
    u64 offset[TARGETS];       /* the firmware.h offset each window was read at */
    u8  bytes[TARGETS][WINDOW];
};
struct req { u64 known; struct capture *out; };
/* GoldHEN's kexec syscall calls fn(td, uap); uap is the {fn, arg} pair we pass. */
struct args { void *fn; struct req *req; };

/* Copy one window, with the offset as a compile-time immediate: the kernel
 * callback must not read a payload global (only the syscall-arg pointer, as the
 * other kexec callbacks here do). The report prints the windows in this order. */
#define CAP(idx, off) do { \
    cap.offset[idx] = (u64)(off); \
    const volatile u8 *s = (const volatile u8 *)(base + (u64)(off) - BEFORE); \
    for (unsigned i = 0; i < WINDOW; i++) cap.bytes[idx][i] = s[i]; \
} while (0)

static int probe(void *td, struct args *a) {
    (void)td;
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(BTA_FW_MSR_LSTAR));
    u64 base = (((u64)hi << 32) | lo) - BTA_FW_KERNEL_BASE_DELTA;
    int valid = ((base >> 48) == 0xffff) && !(base & 0x3fff);

    /* Build the result on the kernel stack, then copy it out in one call. */
    struct capture cap;
    for (unsigned i = 0; i < sizeof(cap); i++) ((volatile u8 *)&cap)[i] = 0;
    cap.magic = 0x45424f5250415442UL; /* "BTAPROBE" */
    cap.base = valid ? base : 0;
    cap.version = BTA_FW_VERSION;

    if (!valid) return 22; /* no copyout without a base; _start reports base-fail */

    int (*copyout)(const void *, void *, u64) = (void *)(base + BTA_FW_OFF_COPYOUT);

    /* Only read the nine offsets when they describe THIS firmware. On another
     * firmware they are meaningless, so the windows stay zero and count stays 0. */
    if (a->req->known) {
        CAP(0, BTA_FW_OFF_COPYOUT);   CAP(1, BTA_FW_OFF_LOCK);
        CAP(2, BTA_FW_OFF_UNLOCK);    CAP(3, BTA_FW_OFF_COMMAND);
        CAP(4, BTA_FW_OFF_CTX_PTR);   CAP(5, BTA_FW_OFF_FMT_STRING);
        CAP(6, BTA_FW_OFF_PATCH_SITE);CAP(7, BTA_FW_OFF_FP_CACHE);
        CAP(8, BTA_FW_OFF_FP_COMMAND);
        cap.count = TARGETS;
    }
    return copyout(&cap, a->req->out, sizeof(cap));
}

/* --- userland report writing (freestanding, no libc) --- */
static void put_str(char **p, const char *s) { while (*s) *(*p)++ = *s++; }
static void put_hex(char **p, u64 v, int digits) {
    const char *h = "0123456789abcdef";
    put_str(p, "0x");
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) *(*p)++ = h[(v >> i) & 0xf];
}

__attribute__((section(".text.launcher"))) int _start(void) {
    /* Firmware version (for the report; the capture is gated on it matching). */
    struct { u64 size; char text[28]; u32 version; } v = {.size = 40};
    int (*getver)(void *) = 0;
    sc3(591, 0x2001, (long)"sceKernelGetSystemSwVersion", (long)&getver);
    u32 version = (getver && getver(&v) == 0) ? v.version : 0;

    struct capture cap;
    for (unsigned i = 0; i < sizeof(cap); i++) ((volatile u8 *)&cap)[i] = 0;
    struct req req = {version == BTA_FW_VERSION, &cap};
    long rc = sc3(11, (long)probe, (long)&req, 0);

    /* The raw capture, only when windows were read (known firmware, base valid). */
    if (cap.count) {
        long fd = sc3(5, (long)PROBE_BIN, 0x601, 0644);
        if (fd >= 0) { sc3(4, fd, (long)&cap, sizeof(cap)); sc3(6, fd, 0, 0); }
    }

    /* A human-readable summary alongside it, for any firmware. */
    char buf[512], *p = buf;
    put_str(&p, "Bluetooth Audio firmware probe " PROBE_VERSION "\n");
    put_str(&p, "built for firmware: "); put_hex(&p, BTA_FW_VERSION, 8); put_str(&p, " (" BTA_FW_LABEL ")\n");
    put_str(&p, "this console:       "); put_hex(&p, version, 8); put_str(&p, "\n");
    if (cap.base) {
        put_str(&p, "kernel base:        recovered\n");
        if (cap.count) {
            put_str(&p, "capture:            "); put_hex(&p, cap.count, 1);
            put_str(&p, " windows written to fw_probe.bin\n");
        } else {
            put_str(&p, "capture:            none (no offset table for this firmware yet)\n");
            put_str(&p, "                    send this file to the developer\n");
        }
    } else {
        put_str(&p, "kernel base:        NOT recovered (rc="); put_hex(&p, (u64)rc, 2); put_str(&p, ")\n");
    }
    long fd = sc3(5, (long)PROBE_TXT, 0x601, 0644);
    if (fd >= 0) { sc3(4, fd, (long)buf, (u64)(p - buf)); sc3(6, fd, 0, 0); }

    /* Tell the person on the TV where to look. */
    int (*send)(int, void *, u64, int) = 0;
    sc3(591, 0x2001, (long)"sceKernelSendNotificationRequest", (long)&send);
    if (send) {
        u8 note[0xc30];
        /* volatile so -O2 cannot fold this into a memset call (none is linked). */
        for (unsigned i = 0; i < sizeof(note); i++) ((volatile u8 *)note)[i] = 0;
        *(int *)(note + 0x10) = -1;
        const char *m = cap.count ? "Firmware probe done: fw_probe.bin and .txt written."
                                  : "Firmware probe done: see /data/bta_audio/fw_probe.txt.";
        char *d = (char *)note + 0x2d;
        while (*m) *d++ = *m++;
        send(0, note, sizeof(note), 0);
    }
    return (int)rc;
}
