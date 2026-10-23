#ifndef BTA_STRFMT_H
#define BTA_STRFMT_H
#include "bta_types.h"
/* Small, self-contained string helpers shared by the control API and the
 * status writer. None of them allocate; each writes into a caller buffer and
 * NUL-terminates within the given capacity. */

/* True when `s` begins with `p`. Local to this header so it can be unit-tested
 * without the payload runtime (runtime.h has an identical `starts`). */
static int bta_prefix(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

/* pre + device name (clipped to 40 bytes) + suf, always NUL-terminated within cap.
 * Returns the number of bytes written (excluding the terminator). */
static u64 compose(char *out, u64 cap, const char *pre, const char *name, const char *suf) {
    u64 n = 0;
    while (*pre && n + 1 < cap) out[n++] = *pre++;
    for (u64 k = 0; name[k] && k < 40 && n + 1 < cap; k++) out[n++] = name[k];
    while (*suf && n + 1 < cap) out[n++] = *suf++;
    out[n] = 0;
    return n;
}

/* Twelve uppercase hex digits of a 48-bit Bluetooth address. */
static void mac_text(u64 address, char out[13]) {
    const char digits[] = "0123456789ABCDEF";
    for (int i = 0; i < 12; i++) out[i] = digits[(address >> (44 - 4 * i)) & 15];
    out[12] = 0;
}

/* Parse twelve hex digits of a MAC that must be followed by " HTTP/1." (so a
 * truncated or malformed request line is rejected). Returns 0 on any error. */
static u64 parse_mac(const char *p) {
    u64 v = 0;
    for (int i = 0; i < 12; i++) {
        char c = p[i];
        unsigned n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else return 0;
        v = (v << 4) | n;
    }
    if (!bta_prefix(p + 12, " HTTP/1.")) return 0;
    return v;
}

/* Path of the per-device flag file that opts a headphone out of joint stereo:
 * /data/bta_audio/dual-<ADDRESS>. */
static void joint_path(u64 address, char out[40]) {
    const char *hex = "0123456789ABCDEF", *base = "/data/bta_audio/dual-";
    unsigned i = 0;
    while (base[i]) { out[i] = base[i]; i++; }
    for (int k = 0; k < 12; k++) out[i++] = hex[(address >> (44 - 4 * k)) & 15];
    out[i] = 0;
}
#endif
