/* Firmware adapter: read the driver's Classic device cache and request the
 * normal three-slot HCI packet types for a link, exactly as the proven BTA
 * bridge did. Every 13.52-specific value comes from firmware.h; this file holds
 * only the logic that walks the kernel structures. Device selection is runtime
 * data, and the audio-service check in the caller stays enforced. */
#include "protocol.h"
#include "firmware.h"
typedef unsigned long u64;
__attribute__((noinline)) static long sc3(long n, long a, long b, long c) {
    long o;
    __asm__ volatile("syscall; jnc 1f; neg %%rax; 1:" : "=a"(o), "+d"(c), "+D"(a), "+S"(b) : "a"(n) : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return o;
}

struct input { u64 address; int mode; struct bt_devices *out; };
struct args { void *fn; struct input *input; };

/* Mode 0 copies the whole device cache out; mode 1 asks the link for one
 * device to agree the three-slot packet types. Runs on the calling worker's
 * private stack via syscall 11, like the other kernel helpers here. */
static int callback(void *td, struct args *a) {
    (void)td;
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(BTA_FW_MSR_LSTAR));
    u64 base = (((u64)hi << 32) | lo) - BTA_FW_KERNEL_BASE_DELTA;
    if ((base >> 48) != 0xffff || (base & 0x3fff)) return 22;

    /* Fingerprint the two routines we rely on before touching anything. */
    if (*(u64 *)(base + BTA_FW_OFF_FP_CACHE) != BTA_FW_PROLOGUE ||
        *(u64 *)(base + BTA_FW_OFF_FP_COMMAND) != BTA_FW_PROLOGUE) return 22;

    u64 address = a->input->address;
    int mode = a->input->mode;
    struct bt_devices *destination = a->input->out;
    if (mode != 0 && mode != 1) return 22;
    if (mode == 1 && (!address || address >> 48)) return 22;

    struct bt_devices out;
    for (unsigned i = 0; i < sizeof(out); i++) ((volatile unsigned char *)&out)[i] = 0;

    int (*lock)(void) = (void *)(base + BTA_FW_OFF_LOCK);
    int (*unlock)(int) = (void *)(base + BTA_FW_OFF_UNLOCK);
    int rc = lock();
    if (rc) return rc;
    unsigned char *ctx = *(void **)(base + BTA_FW_OFF_CTX_PTR);
    if (!ctx || ((u64)ctx >> 48) != 0xffff) { unlock(0); return 6; }

    if (mode == 0) {
        for (unsigned off = BTA_FW_CACHE_FIRST; off < BTA_FW_CACHE_END; off += BTA_FW_CACHE_STRIDE) {
            unsigned char *d = ctx + off;
            u64 mac = *(u64 *)d;
            if (!mac || mac >> 48) continue;
            struct bt_device *v = &out.items[out.count++];
            v->address = mac;
            v->cod = *(unsigned *)(d + BTA_FW_DEV_COD);
            v->services = *(unsigned *)(d + BTA_FW_DEV_SERVICES);
            v->flags = *(unsigned *)(d + BTA_FW_DEV_FLAGS);
            for (unsigned i = 0; i < sizeof(v->name) - 1; i++) {
                unsigned char c = d[BTA_FW_DEV_NAME + i];
                if (!c) break;
                v->name[i] = (c >= 32 && c < 127) ? c : '?';
            }
            if (out.count == BT_DEVICE_LIMIT) break;
        }
    } else {
        rc = 2;
        for (unsigned off = BTA_FW_LINK_FIRST; off < BTA_FW_LINK_END; off += BTA_FW_LINK_STRIDE) {
            unsigned char *c = ctx + off;
            if (*(u64 *)(c + BTA_FW_LINK_ADDRESS) != address) continue;
            unsigned char *d = *(void **)(c + BTA_FW_LINK_INFO);
            if (!d || ((u64)d >> 48) != 0xffff || !(*(unsigned *)(d + BTA_FW_DEV_SERVICES) & 8)) { rc = 93; break; }
            if (*(unsigned *)(c + BTA_FW_LINK_PKT_COUNT) >= 3) { rc = 0; break; }
            int (*command)(void *, unsigned, const char *, ...) = (void *)(base + BTA_FW_OFF_COMMAND);
            rc = command(ctx, BTA_FW_CMD_SET_PKT_TYPE, (const char *)(base + BTA_FW_OFF_FMT_STRING),
                         (unsigned)*(unsigned short *)(c + BTA_FW_LINK_HANDLE), BTA_FW_PKT_TYPES_3SLOT);
            if (rc >= 0) { *(unsigned *)(c + BTA_FW_LINK_PKT_TYPES) = BTA_FW_PKT_TYPES_3SLOT; rc = 0; }
            break;
        }
    }
    unlock(0);
    if (mode == 0) {
        int (*copyout)(const void *, void *, u64) = (void *)(base + BTA_FW_OFF_COPYOUT);
        return copyout(&out, destination, sizeof(out));
    }
    return rc;
}

static int invoke(struct input *in) {
    struct { u64 size; char text[28]; unsigned version; } v = {.size = 40};
    int (*getver)(void *) = 0;
    if (sc3(591, 0x2001, (long)"sceKernelGetSystemSwVersion", (long)&getver) || !getver ||
        getver(&v) || v.version != BTA_FW_VERSION) return 22;
    return (int)sc3(11, (long)callback, (long)in, 0);
}

int bt_list(struct bt_devices *out) { struct input in = {.mode = 0, .out = out}; return invoke(&in); }
int bt_prepare(u64 address) { struct input in = {.address = address, .mode = 1}; return invoke(&in); }
