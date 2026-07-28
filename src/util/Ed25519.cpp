#include "Ed25519.h"
#include "Sha512.h"

#include <cstring>

// Curve25519 field arithmetic in the TweetNaCl representation: an element is
// sixteen limbs of 16 bits, held in 64-bit signed words so that products and
// carries have room before reduction. See Ed25519.h for provenance; the shape
// of this code is the reference implementation's and is kept recognisable on
// purpose, so it can be diffed against it.

namespace {

using Field = int64_t[16];

// The group order L = 2^252 + 27742317777372353535851937790883648493,
// little-endian. Used to reduce the hash scalar and to reject a signature
// whose S is not already reduced.
const int64_t kL[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
};

const Field kGf0 = {0};
const Field kGf1 = {1};
// d = -121665/121666, the curve constant, and 2d.
const Field kD = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203,
};
const Field kD2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406,
};
// The base point B.
const Field kX = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169,
};
const Field kY = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
};
// sqrt(-1), for the square-root branch when recovering x from y.
const Field kI = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83,
};

void set(Field r, const Field a) { std::memcpy(r, a, sizeof(Field)); }

void carry(Field o) {
    for (int i = 0; i < 16; i++) {
        o[i] += 1LL << 16;
        const int64_t c = o[i] >> 16;
        // 2^256 = 38 mod (2^255 - 19), which is why the top limb folds back
        // into the bottom multiplied by 37 (+1 from the borrow).
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

/** Constant-time conditional swap. `b` must be 0 or 1. */
void cselect(Field p, Field q, int64_t b) {
    const int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        const int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack(uint8_t* o, const Field n) {
    Field m, t;
    set(t, n);
    carry(t); carry(t); carry(t);
    // Twice, because one conditional subtraction of p can leave a value that
    // still needs another.
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        int i;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        cselect(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i]     = static_cast<uint8_t>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<uint8_t>(t[i] >> 8);
    }
}

void unpack(Field o, const uint8_t* n) {
    for (int i = 0; i < 16; i++)
        o[i] = n[2 * i] + (static_cast<int64_t>(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;    // the top bit is the sign of x, not part of y
}

void add(Field o, const Field a, const Field b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

void sub(Field o, const Field a, const Field b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

void mul(Field o, const Field a, const Field b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    carry(o); carry(o);
}

void square(Field o, const Field a) { mul(o, a, a); }

/** o = 1/i, by Fermat: i^(p-2). The exponent's bit pattern is where the two
 *  skipped squarings come from. */
void invert(Field o, const Field i) {
    Field c;
    set(c, i);
    for (int a = 253; a >= 0; a--) {
        square(c, c);
        if (a != 2 && a != 4) mul(c, c, i);
    }
    set(o, c);
}

/** o = i^((p-5)/8), the candidate square root. */
void pow2523(Field o, const Field i) {
    Field c;
    set(c, i);
    for (int a = 250; a >= 0; a--) {
        square(c, c);
        if (a != 1) mul(c, c, i);
    }
    set(o, c);
}

/** Constant-time compare of two packed 32-byte values. 0 when equal. */
int diff32(const uint8_t* x, const uint8_t* y) {
    uint32_t d = 0;
    for (int i = 0; i < 32; i++) d |= static_cast<uint32_t>(x[i] ^ y[i]);
    return (1 & ((d - 1) >> 8)) - 1;
}

int notEqual(const Field a, const Field b) {
    uint8_t c[32], d[32];
    pack(c, a);
    pack(d, b);
    return diff32(c, d);
}

uint8_t parity(const Field a) {
    uint8_t d[32];
    pack(d, a);
    return d[0] & 1;
}

// A point in extended coordinates (x, y, z, t).
using Point = Field[4];

void pointAdd(Point p, const Point q) {
    Field a, b, c, d, t, e, f, g, h;
    sub(a, p[1], p[0]);
    sub(t, q[1], q[0]);
    mul(a, a, t);
    add(b, p[0], p[1]);
    add(t, q[0], q[1]);
    mul(b, b, t);
    mul(c, p[3], q[3]);
    mul(c, c, kD2);
    mul(d, p[2], q[2]);
    add(d, d, d);
    sub(e, b, a);
    sub(f, d, c);
    add(g, d, c);
    add(h, b, a);
    mul(p[0], e, f);
    mul(p[1], h, g);
    mul(p[2], g, f);
    mul(p[3], e, h);
}

void pointSwap(Point p, Point q, uint8_t b) {
    for (int i = 0; i < 4; i++) cselect(p[i], q[i], b);
}

void pointPack(uint8_t* r, const Point p) {
    Field tx, ty, zi;
    invert(zi, p[2]);
    mul(tx, p[0], zi);
    mul(ty, p[1], zi);
    pack(r, ty);
    r[31] ^= static_cast<uint8_t>(parity(tx) << 7);
}

/** p = s * q, a ladder over all 256 bits with no data-dependent branch. */
void scalarMul(Point p, Point q, const uint8_t* s) {
    set(p[0], kGf0);
    set(p[1], kGf1);
    set(p[2], kGf1);
    set(p[3], kGf0);
    for (int i = 255; i >= 0; i--) {
        const uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        pointSwap(p, q, b);
        pointAdd(q, p);
        pointAdd(p, p);
        pointSwap(p, q, b);
    }
}

/** p = s * B. */
void scalarBase(Point p, const uint8_t* s) {
    Point q;
    set(q[0], kX);
    set(q[1], kY);
    set(q[2], kGf1);
    mul(q[3], kX, kY);
    scalarMul(p, q, s);
}

/**
 * Decode a public key into -A.
 *
 * Negated because verification then becomes a single sum: h*(-A) + S*B, which
 * must equal R. False when the bytes are not a point on the curve at all.
 */
bool unpackNegative(Point r, const uint8_t p[32]) {
    Field t, chk, num, den, den2, den4, den6;
    set(r[2], kGf1);
    unpack(r[1], p);

    // Solve x^2 = (y^2 - 1) / (d*y^2 + 1).
    square(num, r[1]);
    mul(den, num, kD);
    sub(num, num, r[2]);
    add(den, r[2], den);

    square(den2, den);
    square(den4, den2);
    mul(den6, den4, den2);
    mul(t, den6, num);
    mul(t, t, den);

    pow2523(t, t);
    mul(t, t, num);
    mul(t, t, den);
    mul(t, t, den);
    mul(r[0], t, den);

    square(chk, r[0]);
    mul(chk, chk, den);
    if (notEqual(chk, num)) mul(r[0], r[0], kI);   // the other square root

    square(chk, r[0]);
    mul(chk, chk, den);
    if (notEqual(chk, num)) return false;          // no root: not on the curve

    if (parity(r[0]) == (p[31] >> 7)) sub(r[0], kGf0, r[0]);

    mul(r[3], r[0], r[1]);
    return true;
}

/** r = x mod L, with x a 64-byte little-endian value. Barrett-style folding. */
void modL(uint8_t* r, int64_t x[64]) {
    int64_t c;
    int j = 0;
    for (int i = 63; i >= 32; i--) {
        c = 0;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += c - 16 * x[i] * kL[j - (i - 32)];
            c = (x[j] + 128) >> 8;
            x[j] -= c << 8;
        }
        x[j] += c;
        x[i] = 0;
    }
    c = 0;
    for (j = 0; j < 32; j++) {
        x[j] += c - (x[31] >> 4) * kL[j];
        c = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++) x[j] -= c * kL[j];
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = static_cast<uint8_t>(x[i] & 255);
    }
}

void reduce(uint8_t r[64]) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = static_cast<int64_t>(r[i]);
    std::memset(r, 0, 64);
    modL(r, x);
}

/**
 * True when the 32-byte little-endian scalar is below the group order.
 *
 * RFC 8032 section 5.1.7 requires this and TweetNaCl does not do it. Without
 * it, S + L is a second valid signature over the same message -- signature
 * malleability. A ticket's `jti` is burned on first use, so a duplicate would
 * be refused anyway; this closes the door one step earlier, where the reasoning
 * does not depend on another component behaving.
 */
bool scalarIsReduced(const uint8_t s[32]) {
    for (int i = 31; i >= 0; i--) {
        if (s[i] < kL[i]) return true;
        if (s[i] > kL[i]) return false;
    }
    return false;   // exactly L is not reduced
}

}  // namespace

bool ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t msgLen,
                   const uint8_t pub[32]) {
    if (!scalarIsReduced(sig + 32)) return false;

    Point a;
    if (!unpackNegative(a, pub)) return false;

    // h = SHA-512(R || A || M), reduced mod L.
    Sha512 sha;
    sha.update(sig, 32);
    sha.update(pub, 32);
    if (msgLen) sha.update(msg, msgLen);
    uint8_t h[64];
    sha.finish(h);
    reduce(h);

    Point p, q;
    scalarMul(p, a, h);       // h * (-A)
    scalarBase(q, sig + 32);  // S * B
    pointAdd(p, q);

    uint8_t check[32];
    pointPack(check, p);
    return diff32(sig, check) == 0;
}
