/* Host tests for the kernel-routine signature matcher (app/sigscan.h) and the
 * 13.520.001 candidate signatures (app/fw_signatures.h).
 *
 * They prove the matcher is correct (unique match / none / ambiguous, wildcards
 * honoured), that each shipped signature locates its own bytes and not another
 * routine's, and that the fixed bytes agree with the fingerprints already in
 * firmware.h. They do NOT prove a signature is right on a different firmware - only
 * that firmware's own bytes can (see docs/FIRMWARE_SUPPORT.md). */
#include "test.h"
#include <string.h>
#include <stdlib.h>
#include "../app/fw_signatures.h"

/* Lay a signature's own bytes into buf at off. Wildcard slots get filler that the
 * matcher must ignore; fixed slots get the real byte. */
static void place(unsigned char *buf, unsigned long off, const struct bta_sig *s) {
    for (unsigned j = 0; j < s->len; j++)
        buf[off + j] = s->mask[j] ? s->bytes[j] : (unsigned char)(0xA0 + j);
}

static const struct bta_fw_sig *by_name(const char *name) {
    for (unsigned k = 0; k < BTA_FW_CODE_SIG_COUNT; k++)
        if (!strcmp(bta_fw_code_sigs[k].name, name)) return &bta_fw_code_sigs[k];
    return 0;
}

void test_sigscan(void) {
    unsigned char buf[256];

    /* --- matcher mechanics on a tiny hand-built signature (slot 1 wildcarded) --- */
    struct bta_sig t = { 4, {0xde, 0x00, 0xbe, 0xef}, {0xff, 0x00, 0xff, 0xff} };
    memset(buf, 0, sizeof(buf));
    buf[10] = 0xde; buf[11] = 0x99; buf[12] = 0xbe; buf[13] = 0xef;
    CHECK_EQ(bta_sig_scan(buf, sizeof(buf), &t), 10);
    buf[11] = 0x00;                                  /* wildcard slot: still matches */
    CHECK_EQ(bta_sig_scan(buf, sizeof(buf), &t), 10);
    buf[12] = 0x00;                                  /* break a fixed byte: gone */
    CHECK_EQ(bta_sig_scan(buf, sizeof(buf), &t), -1);

    memset(buf, 0, sizeof(buf));                     /* two matches -> ambiguous */
    buf[10] = 0xde; buf[11] = 0x11; buf[12] = 0xbe; buf[13] = 0xef;
    buf[40] = 0xde; buf[41] = 0x22; buf[42] = 0xbe; buf[43] = 0xef;
    CHECK_EQ(bta_sig_scan(buf, sizeof(buf), &t), -2);
    CHECK_EQ(bta_sig_scan(buf, 3, &t), -1);          /* buffer shorter than the sig */

    /* --- each shipped 13.52 signature locates its own bytes, uniquely --- */
    CHECK_EQ(BTA_FW_CODE_SIG_COUNT, 6);
    for (unsigned k = 0; k < BTA_FW_CODE_SIG_COUNT; k++) {
        const struct bta_sig *s = &bta_fw_code_sigs[k].sig;
        unsigned char big[1024];
        const unsigned long off = 300;
        CHECK(s->len >= 16u && s->len <= BTA_SIG_MAX);
        memset(big, 0, sizeof(big));
        place(big, off, s);
        CHECK_EQ(bta_sig_scan(big, sizeof(big), s), (long)off);
        big[off] = (unsigned char)(big[off] ^ 0xff);   /* first byte is fixed: break it */
        CHECK_EQ(bta_sig_scan(big, sizeof(big), s), -1);
        CHECK(bta_fw_code_sigs[k].known_offset != 0);  /* maps to a real firmware.h offset */
    }

    /* --- signatures are specific: one routine's bytes don't match another's sig,
     *     including command vs fp_cache, which share a prologue then diverge. --- */
    {
        const struct bta_fw_sig *lock = by_name("bt_lock"), *unlock = by_name("bt_unlock");
        const struct bta_fw_sig *cmd = by_name("command"), *cache = by_name("fp_cache");
        unsigned char b2[512];
        CHECK(lock && unlock && cmd && cache);
        memset(b2, 0, sizeof(b2)); place(b2, 100, &lock->sig);
        CHECK_EQ(bta_sig_scan(b2, sizeof(b2), &unlock->sig), -1);
        memset(b2, 0, sizeof(b2)); place(b2, 100, &cmd->sig);
        CHECK_EQ(bta_sig_scan(b2, sizeof(b2), &cache->sig), -1);
        memset(b2, 0, sizeof(b2)); place(b2, 100, &cache->sig);
        CHECK_EQ(bta_sig_scan(b2, sizeof(b2), &cmd->sig), -1);
    }

    /* --- fixed bytes agree with firmware.h's fingerprints --- */
    {
        const struct bta_fw_sig *patch = by_name("patch_site");
        const struct bta_fw_sig *cmd = by_name("command"), *cache = by_name("fp_cache");
        CHECK(patch && cmd && cache);
        /* patch_site opens with the exact accept check, fully fixed (not wildcarded). */
        for (unsigned j = 0; j < 8; j++) {
            CHECK_EQ(patch->sig.bytes[j], (unsigned char)(BTA_FW_ACCEPT_ORIGINAL >> (8 * j)));
            CHECK_EQ(patch->sig.mask[j], 0xff);
        }
        /* command and fp_cache open with the shared fingerprinted prologue. */
        for (unsigned j = 0; j < 8; j++) {
            CHECK_EQ(cmd->sig.bytes[j], (unsigned char)(BTA_FW_PROLOGUE >> (8 * j)));
            CHECK_EQ(cache->sig.bytes[j], (unsigned char)(BTA_FW_PROLOGUE >> (8 * j)));
        }
    }

    /* --- probe-style resolve: the six at their real 13.52 offsets in one buffer,
     *     each found uniquely at its own address (what fw_probe's scan does). --- */
    {
        const unsigned long size = 0x5b8000UL;   /* just past fp_cache (+0x5b7570) */
        unsigned char *k = calloc(size, 1);
        CHECK(k != 0);
        if (k) {
            for (unsigned i = 0; i < BTA_FW_CODE_SIG_COUNT; i++)
                place(k, bta_fw_code_sigs[i].known_offset, &bta_fw_code_sigs[i].sig);
            for (unsigned i = 0; i < BTA_FW_CODE_SIG_COUNT; i++)
                CHECK_EQ(bta_sig_scan(k, size, &bta_fw_code_sigs[i].sig),
                         (long)bta_fw_code_sigs[i].known_offset);
            free(k);
        }
    }
}
