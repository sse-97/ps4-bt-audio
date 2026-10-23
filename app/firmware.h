#ifndef BTA_FIRMWARE_H
#define BTA_FIRMWARE_H
/* The firmware adapter: every value specific to PS4 firmware 13.520.001 lives
 * here, and nowhere else. The launcher, the device probe and the accept patch
 * all include this file instead of repeating a version gate or a kernel offset.
 * Supporting another firmware would mean adding a second table beside this one
 * and choosing between them by the live fingerprint; no other source file would
 * change, because no other source file names a firmware or an offset.
 *
 * The kernel offsets below are relative to the kernel text base, which is
 * recovered at runtime from the LSTAR MSR (the syscall entry point) minus a
 * fixed delta. Before any read or write, the live code is checked against the
 * byte fingerprints here, so the wrong firmware refuses cleanly rather than
 * corrupting memory. Every value was recovered from 13.520.001 and applies to
 * that firmware only. */

/* sceKernelGetSystemSwVersion reports the firmware as 0xMMMMNNNN. */
#define BTA_FW_VERSION            0x13520001u
#define BTA_FW_LABEL              "13.520.001"

/* Kernel text base recovery: read LSTAR, subtract the entry-stub delta. */
#define BTA_FW_MSR_LSTAR          0xc0000082u
#define BTA_FW_KERNEL_BASE_DELTA  0x1c0UL

/* Kernel-text-relative offsets (added to the recovered base). */
#define BTA_FW_OFF_COPYOUT        0x2bd6a0UL  /* int copyout(const void*,void*,u64) */
#define BTA_FW_OFF_LOCK           0x5a73f0UL  /* int bt_lock(void) */
#define BTA_FW_OFF_UNLOCK         0x5a7470UL  /* int bt_unlock(int) */
#define BTA_FW_OFF_COMMAND        0x5ae3b0UL  /* int command(ctx,op,fmt,...) */
#define BTA_FW_OFF_CTX_PTR        0x2612550UL /* void** holding the Bluetooth context */
#define BTA_FW_OFF_FMT_STRING     0xad7562UL  /* format string command() expects */
#define BTA_FW_OFF_PATCH_SITE     0x5a1334UL  /* the service-class accept check we flip */
#define BTA_FW_OFF_FP_CACHE       0x5b7570UL  /* prologue fingerprinted before a cache read */
#define BTA_FW_OFF_FP_COMMAND     0x5ae3b0UL  /* prologue fingerprinted before a command call */

/* Shared function prologue of the two fingerprinted routines:
 *   push rbp; mov rbp,rsp; push r15; push r14  (55 48 89 e5 41 57 41 56). */
#define BTA_FW_PROLOGUE           0x56415741e5894855UL

/* The accept check at the patch site, and its patched form. 0x40 = HID class,
 * 0x08 = audio class; the edit also admits audio without touching any other
 * allowlist path or connection-limit check:
 *   cmp dword [rax+0x20],0x40; jne reject  ->  test byte [rax+0x20],0x48; je reject */
#define BTA_FW_ACCEPT_ORIGINAL    0x8b492a7540207883UL
#define BTA_FW_ACCEPT_REPLACEMENT 0x8b492a74482040f6UL

/* Classic device cache: records in [CACHE_FIRST, CACHE_END) every CACHE_STRIDE
 * bytes, counted from the context pointer. */
#define BTA_FW_CACHE_FIRST        0x18da8u
#define BTA_FW_CACHE_END          0x20ea8u
#define BTA_FW_CACHE_STRIDE       0x408u
/* Field offsets inside a cache record (the device address is at offset 0). */
#define BTA_FW_DEV_COD            8u       /* class of device */
#define BTA_FW_DEV_SERVICES       0x20u    /* service bitmask (bit 3 = audio) */
#define BTA_FW_DEV_FLAGS          0x24u
#define BTA_FW_DEV_NAME           0x80u    /* NUL-terminated friendly name */

/* Per-link records: [LINK_FIRST, LINK_END) every LINK_STRIDE bytes. */
#define BTA_FW_LINK_FIRST         0x42a8u
#define BTA_FW_LINK_END           0xd960u
#define BTA_FW_LINK_STRIDE        0x1588u
#define BTA_FW_LINK_ADDRESS       0x30u    /* u64 remote address */
#define BTA_FW_LINK_INFO          0x638u   /* pointer to the service-info record */
#define BTA_FW_LINK_HANDLE        0x48u    /* u16 connection handle */
#define BTA_FW_LINK_PKT_COUNT     0x1560u  /* number of packet types already agreed */
#define BTA_FW_LINK_PKT_TYPES     0x1564u  /* agreed packet-type mask */

/* HCI "set packet types" opcode and the three-slot mask requested on connect. */
#define BTA_FW_CMD_SET_PKT_TYPE   0x40f
#define BTA_FW_PKT_TYPES_3SLOT    0x3c18
#endif
