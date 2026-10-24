/* Host tests for the QR encoder behind the phone-control panel (native/qr.h).
 * The encoder was checked against a real QR decoder when it was written; these
 * checks pin what a decoder relies on: the version per text length, the finder
 * and timing patterns, matching format bits in both copies, codewords that are
 * valid Reed-Solomon (all syndromes zero), and the text read back out of the
 * matrix after unmasking. */
#include "test.h"
#include "../native/qr.h"
#include <string.h>

static int fmt_bit(const struct qr *q, int x, int y) { return q->m[y][x]; }
/* The 15 format bits, from the first copy (second = 0) or the second (1). */
static int read_format(const struct qr *q, int second) {
    int n = q->size, bits = 0;
    for (int i = 0; i < 15; i++) {
        int b;
        if (second) b = i < 8 ? fmt_bit(q, n - 1 - i, 8) : fmt_bit(q, 8, n - 15 + i);
        else if (i <= 5) b = fmt_bit(q, 8, i);
        else if (i == 6) b = fmt_bit(q, 8, 7);
        else if (i == 7) b = fmt_bit(q, 8, 8);
        else if (i == 8) b = fmt_bit(q, 7, 8);
        else b = fmt_bit(q, 14 - i, 8);
        bits |= b << i;
    }
    return bits;
}
/* Undo the mask and read the codewords back in placement order. */
static int read_codewords(const struct qr *q, int mask, unsigned char *out, int count) {
    struct qr u = *q;
    int n = u.size, i = 0;
    qr_apply_mask(&u, mask);
    memset(out, 0, (size_t)count);
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < n; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j, up = ((right + 1) & 2) == 0, y = up ? n - 1 - vert : vert;
                if (!u.fn[y][x] && i < count * 8) { out[i >> 3] |= (unsigned char)(u.m[y][x] << (7 - (i & 7))); i++; }
            }
    }
    return i;
}
static void check_text(const char *text, int want_size) {
    struct qr q;
    CHECK(qr_encode(&q, text));
    CHECK_EQ(q.size, want_size);
    int n = q.size, ver = (n - 17) / 4;

    /* Finders (with separators) in three corners, timing between them, dark module. */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int dx = x - 3 < 0 ? 3 - x : x - 3, dy = y - 3 < 0 ? 3 - y : y - 3, d = dx > dy ? dx : dy;
            int want = d != 2 && d != 4;
            CHECK_EQ(q.m[y][x], want);
            CHECK_EQ(q.m[y][n - 1 - x], want);
            CHECK_EQ(q.m[n - 1 - y][x], want);
        }
    for (int i = 8; i < n - 8; i++) { CHECK_EQ(q.m[6][i], i % 2 == 0); CHECK_EQ(q.m[i][6], i % 2 == 0); }
    CHECK_EQ(q.m[n - 8][8], 1);

    /* Format: both copies agree, the BCH remainder checks out, level M (00). */
    int f = read_format(&q, 0);
    CHECK_EQ(f, read_format(&q, 1));
    int raw = f ^ 0x5412, data = raw >> 10, rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    CHECK_EQ(raw & 0x3ff, rem & 0x3ff);
    CHECK_EQ(data >> 3, 0);
    int mask = data & 7;

    /* Codewords: all syndromes zero, then byte mode, the length, the text. */
    int ndata = QR_DATA[ver], necc = QR_ECC[ver], total = ndata + necc;
    unsigned char cw[70];
    CHECK_EQ(read_codewords(&q, mask, cw, total), total * 8);
    unsigned alpha = 1;
    for (int j = 0; j < necc; j++) {
        unsigned s = 0;
        for (int i = 0; i < total; i++) s = qr_gmul(s, alpha) ^ cw[i];
        CHECK_EQ(s, 0);
        alpha = qr_gmul(alpha, 2);
    }
    int len = (int)strlen(text);
    CHECK_EQ(cw[0] >> 4, 4);
    CHECK_EQ(((cw[0] & 15) << 4) | (cw[1] >> 4), len);
    int same = 1;
    for (int i = 0; i < len; i++) same &= (((cw[1 + i] & 15) << 4) | (cw[2 + i] >> 4)) == (unsigned char)text[i];
    CHECK(same);
}

void test_qr(void) {
    check_text("A", 21);
    check_text("http://10.0.0.5:8097", 25);
    check_text("http://192.168.1.97:8097", 25);
    check_text("http://255.255.255.255:8097", 29);
    check_text("0123456789012345678901234567890123456789AB", 29); /* 42 bytes: the most version 3-M holds */

    struct qr q;
    CHECK(!qr_encode(&q, ""));
    CHECK_EQ(q.size, 0);
    CHECK(!qr_encode(&q, "0123456789012345678901234567890123456789ABC")); /* 43 bytes */
    CHECK_EQ(q.size, 0);
}
