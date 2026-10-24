/* Minimal QR Code encoder for the phone-control link (ISO/IEC 18004): byte mode,
 * error correction level M, versions 1-3, so up to 42 bytes, enough for
 * "http://255.255.255.255:8097". Each of these versions is a single Reed-Solomon
 * block (no interleaving) and carries no version-information area. The mask is
 * the one with the lowest penalty score, as the standard asks.
 *
 * Pure C with no console dependencies: the PC harness (native/host_ui.c) and the
 * host unit tests (tests/test_qr.c) run this exact code.
 */
#ifndef BTA_QR_H
#define BTA_QR_H
#include <string.h>

#define QR_MAX 29 /* modules per side at version 3 */
struct qr {
    int size;                          /* modules per side; 0 when encoding failed */
    unsigned char m[QR_MAX][QR_MAX];   /* [y][x], 1 = dark */
    unsigned char fn[QR_MAX][QR_MAX];  /* function modules: finders, timing, alignment, format */
};

/* Level M, versions 1-3: data and error-correction codewords of the one block. */
static const unsigned char QR_DATA[4] = {0, 16, 28, 44};
static const unsigned char QR_ECC[4] = {0, 10, 16, 26};

/* Multiplication in GF(2^8) modulo x^8 + x^4 + x^3 + x^2 + 1. */
static unsigned qr_gmul(unsigned x, unsigned y) {
    unsigned z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11d);
        z ^= ((y >> i) & 1) * x;
    }
    return z & 0xff;
}
/* Generator polynomial of the given degree, highest term (always 1) omitted. */
static void qr_rs_divisor(int degree, unsigned char *div) {
    memset(div, 0, (size_t)degree);
    div[degree - 1] = 1;
    unsigned root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            div[j] = (unsigned char)qr_gmul(div[j], root);
            if (j + 1 < degree) div[j] ^= div[j + 1];
        }
        root = qr_gmul(root, 2);
    }
}
/* Error-correction codewords: the remainder of data * x^degree by the generator. */
static void qr_rs_remainder(const unsigned char *data, int len, const unsigned char *div, int degree, unsigned char *out) {
    memset(out, 0, (size_t)degree);
    for (int i = 0; i < len; i++) {
        unsigned factor = data[i] ^ out[0];
        memmove(out, out + 1, (size_t)(degree - 1));
        out[degree - 1] = 0;
        for (int j = 0; j < degree; j++) out[j] ^= (unsigned char)qr_gmul(div[j], factor);
    }
}

static void qr_set(struct qr *q, int x, int y, int dark) { q->m[y][x] = (unsigned char)(dark != 0); q->fn[y][x] = 1; }
static int qr_cheb(int dx, int dy) {
    dx = dx < 0 ? -dx : dx; dy = dy < 0 ? -dy : dy;
    return dx > dy ? dx : dy;
}
/* Finder pattern centred at (cx, cy), with its light separator ring. */
static void qr_finder(struct qr *q, int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int x = cx + dx, y = cy + dy, d = qr_cheb(dx, dy);
            if (x >= 0 && y >= 0 && x < q->size && y < q->size) qr_set(q, x, y, d != 2 && d != 4);
        }
}
static void qr_alignment(struct qr *q, int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) qr_set(q, cx + dx, cy + dy, qr_cheb(dx, dy) != 1);
}
/* The 15 format bits (level M, mask) in both copies, plus the dark module. */
static void qr_format(struct qr *q, int mask) {
    int data = (0 << 3) | mask; /* level M is 00 */
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412, n = q->size;
    for (int i = 0; i <= 5; i++) qr_set(q, 8, i, (bits >> i) & 1);
    qr_set(q, 8, 7, (bits >> 6) & 1);
    qr_set(q, 8, 8, (bits >> 7) & 1);
    qr_set(q, 7, 8, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++) qr_set(q, 14 - i, 8, (bits >> i) & 1);
    for (int i = 0; i < 8; i++) qr_set(q, n - 1 - i, 8, (bits >> i) & 1);
    for (int i = 8; i < 15; i++) qr_set(q, 8, n - 15 + i, (bits >> i) & 1);
    qr_set(q, 8, n - 8, 1);
}
static int qr_mask_bit(int mask, int x, int y) {
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}
/* XOR the mask over every data module (applying it twice undoes it). */
static void qr_apply_mask(struct qr *q, int mask) {
    for (int y = 0; y < q->size; y++)
        for (int x = 0; x < q->size; x++)
            if (!q->fn[y][x] && qr_mask_bit(mask, x, y)) q->m[y][x] ^= 1;
}
/* The standard's penalty: long runs, 2x2 blocks, finder look-alikes, dark balance. */
static long qr_penalty(const struct qr *q) {
    static const unsigned char finder_like[2][11] = {{1,0,1,1,1,0,1,0,0,0,0}, {0,0,0,0,1,0,1,1,1,0,1}};
    int n = q->size, dark = 0;
    long p = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int a = 0; a < n; a++) {
            int run = 0, color = -1;
            for (int b = 0; b < n; b++) {
                int c = pass ? q->m[b][a] : q->m[a][b];
                if (c == color) run++;
                else { if (run >= 5) p += run - 2; color = c; run = 1; }
            }
            if (run >= 5) p += run - 2;
            for (int b = 0; b + 11 <= n; b++)
                for (int k = 0; k < 2; k++) {
                    int match = 1;
                    for (int i = 0; i < 11 && match; i++) match = (pass ? q->m[b + i][a] : q->m[a][b + i]) == finder_like[k][i];
                    if (match) p += 40;
                }
        }
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            dark += q->m[y][x];
            if (x + 1 < n && y + 1 < n) {
                int c = q->m[y][x];
                if (q->m[y][x + 1] == c && q->m[y + 1][x] == c && q->m[y + 1][x + 1] == c) p += 3;
            }
        }
    int total = n * n, diff = dark * 20 - total * 10;
    if (diff < 0) diff = -diff;
    p += (long)((diff + total - 1) / total - 1) * 10;
    return p;
}

/* Encode `text` as the smallest version that holds it. Returns 1, or 0 (size 0)
 * when the text is empty or longer than 42 bytes. */
static int qr_encode(struct qr *q, const char *text) {
    int len = (int)strlen(text), ver = 0;
    memset(q, 0, sizeof(*q));
    for (int v = 1; v <= 3; v++) if (len <= (QR_DATA[v] * 8 - 12) / 8) { ver = v; break; }
    if (!len || !ver) return 0;
    int ndata = QR_DATA[ver], necc = QR_ECC[ver], n = 17 + 4 * ver;
    q->size = n;

    /* Codewords: mode 0100, 8-bit count, the bytes, terminator, then pad bytes. */
    unsigned char cw[70], div[26];
    int bit = 0;
    memset(cw, 0, sizeof(cw));
#define QR_PUT(value, count) for (int i_ = (count) - 1; i_ >= 0; i_--, bit++) cw[bit >> 3] |= (unsigned char)((((value) >> i_) & 1) << (7 - (bit & 7)))
    QR_PUT(4, 4);
    QR_PUT(len, 8);
    for (int i = 0; i < len; i++) QR_PUT((unsigned char)text[i], 8);
#undef QR_PUT
    bit += (ndata * 8 - bit < 4) ? ndata * 8 - bit : 4; /* terminator: up to four zero bits */
    int used = (bit + 7) / 8;
    for (int i = used, pad = 0xec; i < ndata; i++, pad ^= 0xec ^ 0x11) cw[i] = (unsigned char)pad;
    qr_rs_divisor(necc, div);
    qr_rs_remainder(cw, ndata, div, necc, cw + ndata);

    /* Function patterns, with the format area reserved before the data goes in. */
    for (int i = 0; i < n; i++) { qr_set(q, 6, i, i % 2 == 0); qr_set(q, i, 6, i % 2 == 0); }
    qr_finder(q, 3, 3);
    qr_finder(q, n - 4, 3);
    qr_finder(q, 3, n - 4);
    if (ver > 1) qr_alignment(q, n - 7, n - 7);
    qr_format(q, 0);

    /* Data: two-module columns from the right, zigzagging up and down, skipping the
     * vertical timing column. Leftover remainder modules stay light. */
    int total = (ndata + necc) * 8, i = 0;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < n; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j, up = ((right + 1) & 2) == 0, y = up ? n - 1 - vert : vert;
                if (!q->fn[y][x] && i < total) { q->m[y][x] = (cw[i >> 3] >> (7 - (i & 7))) & 1; i++; }
            }
    }

    int best = 0;
    long best_p = -1;
    for (int mask = 0; mask < 8; mask++) {
        qr_apply_mask(q, mask);
        qr_format(q, mask);
        long p = qr_penalty(q);
        if (best_p < 0 || p < best_p) { best = mask; best_p = p; }
        qr_apply_mask(q, mask);
    }
    qr_apply_mask(q, best);
    qr_format(q, best);
    return 1;
}
#endif
