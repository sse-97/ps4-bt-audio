/* Firmware adapter: the temporary 13.52 Bluetooth service-acceptance patch.
 * It flips one in-RAM instruction so the stock allowlist also admits audio-class
 * devices, after fingerprinting the exact bytes it expects. It changes no flash
 * or firmware files, reads the result back, and builds separately as a rollback
 * (-DROLLBACK) that restores the original bytes. Every 13.52-specific value
 * comes from firmware.h. */
#include "../app/firmware.h"
typedef unsigned long u64;
__attribute__((noinline)) static long sc3(long n, long a, long b, long c) {
    long out;
    __asm__ volatile("syscall; jnc 1f; neg %%rax; 1:" : "=a"(out), "+d"(c), "+D"(a), "+S"(b) : "a"(n) : "rcx", "r8", "r9", "r10", "r11", "cc", "memory");
    return out;
}
struct sw { u64 size; char text[28]; unsigned version; };
struct result { u64 before, after; };
struct args { void *fn; struct result *out; };

static int patch(void *td, struct args *a) {
    (void)td;
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(BTA_FW_MSR_LSTAR));
    u64 base = (((u64)hi << 32) | lo) - BTA_FW_KERNEL_BASE_DELTA;
    if ((base >> 48) != 0xffff || (base & 0x3fff)) return 22;
    volatile u64 *site = (void *)(base + BTA_FW_OFF_PATCH_SITE);
    u64 original = BTA_FW_ACCEPT_ORIGINAL, replacement = BTA_FW_ACCEPT_REPLACEMENT;
#ifdef ROLLBACK
    u64 swap = original; original = replacement; replacement = swap;
#endif
    struct result out = {*site, *site};
    int rc = 0;
    if (out.before == original) {
        u64 flags, cr0, expected = original;
        __asm__ volatile("pushfq; popq %0; cli; mov %%cr0,%1" : "=r"(flags), "=r"(cr0) :: "memory");
        u64 writable = cr0 & ~0x10000UL;
        __asm__ volatile("mov %0,%%cr0" :: "r"(writable) : "memory");
        __asm__ volatile("lock cmpxchgq %2,(%1)" : "+a"(expected) : "r"(site), "r"(replacement) : "cc", "memory");
        __asm__ volatile("mov %0,%%cr0; pushq %1; popfq" :: "r"(cr0), "r"(flags) : "cc", "memory");
        out.after = *site;
        if (expected != original || out.after != replacement) rc = 5;
    } else if (out.before != replacement) rc = 22;
    int (*copyout)(const void *, void *, u64) = (void *)(base + BTA_FW_OFF_COPYOUT);
    int copied = copyout(&out, a->out, sizeof(out));
    return copied ? copied : rc;
}

__attribute__((section(".text.start"))) int _start(void) {
    struct sw v = {.size = sizeof(v)};
    int (*getver)(void *) = 0;
    if (sc3(591, 0x2001, (long)"sceKernelGetSystemSwVersion", (long)&getver) || !getver ||
        getver(&v) || v.version != BTA_FW_VERSION) return 1;
    struct { long status; struct result result; } r = {0};
    r.status = sc3(11, (long)patch, (long)&r.result, 0);
    long fd = sc3(5, (long)"/data/bt_accept_patch_result.bin", 0x601, 0644);
    if (fd >= 0) { sc3(4, fd, (long)&r, sizeof(r)); sc3(6, fd, 0, 0); }
    return (int)r.status;
}
