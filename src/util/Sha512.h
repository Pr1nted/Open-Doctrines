#pragma once

// SHA-512, FIPS 180-4.
//
// Here for one reason: Ed25519 is defined in terms of it (RFC 8032), and the
// host has to verify join tickets itself now that game connections do not pass
// through the account service. Nothing else in the game uses it -- mod archives
// and updates use SHA-256, in util/Sha256.h.
//
// The round constants below were GENERATED from their definition (the first 64
// bits of the fractional parts of the cube roots of the first 80 primes) rather
// than typed out, because eighty 64-bit literals is eighty chances to transpose
// a digit, and a wrong constant produces a wrong digest that still looks like a
// digest. The published test vectors in tests/net_crypto_test.cpp are what
// actually prove it, and they come from outside this codebase.

#include <cstddef>
#include <cstdint>
#include <cstring>

struct Sha512 {
    static constexpr size_t kDigestSize = 64;

    Sha512() { reset(); }

    void reset() {
        // The first 64 bits of the fractional parts of the square roots of the
        // first eight primes.
        h[0] = 0x6a09e667f3bcc908ULL; h[1] = 0xbb67ae8584caa73bULL;
        h[2] = 0x3c6ef372fe94f82bULL; h[3] = 0xa54ff53a5f1d36f1ULL;
        h[4] = 0x510e527fade682d1ULL; h[5] = 0x9b05688c2b3e6c1fULL;
        h[6] = 0x1f83d9abfb41bd6bULL; h[7] = 0x5be0cd19137e2179ULL;
        total = 0;
        used = 0;
    }

    void update(const uint8_t* data, size_t n) {
        total += n;
        while (n) {
            const size_t take = (128 - used) < n ? (128 - used) : n;
            std::memcpy(buf + used, data, take);
            used += take;
            data += take;
            n -= take;
            if (used == 128) { block(buf); used = 0; }
        }
    }

    void finish(uint8_t out[kDigestSize]) {
        // A 128-bit length field. The high half is always zero here: it would
        // take an exabyte of input to matter, and the game hashes tokens.
        const uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0x00;
        while (used != 112) update(&pad, 1);

        uint8_t tail[16] = {0};
        for (int i = 0; i < 8; i++)
            tail[15 - static_cast<size_t>(i)] =
                static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
        // update() would add these to `total`, which no longer matters.
        std::memcpy(buf + used, tail, 16);
        block(buf);
        used = 0;

        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 8; j++)
                out[static_cast<size_t>(i) * 8 + static_cast<size_t>(j)] =
                    static_cast<uint8_t>((h[i] >> (56 - 8 * j)) & 0xFF);
    }

    /** One-shot, for the common case. */
    static void hash(const uint8_t* data, size_t n, uint8_t out[kDigestSize]) {
        Sha512 s;
        s.update(data, n);
        s.finish(out);
    }

private:
    uint64_t h[8]{};
    uint64_t total = 0;
    uint8_t  buf[128]{};
    size_t   used = 0;

    static uint64_t ror(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

    void block(const uint8_t* p) {
        static const uint64_t K[80] = {
            0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
            0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
            0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
            0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
            0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
            0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
            0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
            0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
            0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
            0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
            0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
            0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
            0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
            0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
            0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
            0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
            0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
            0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
            0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
            0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
            0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
            0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
            0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
            0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
            0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
            0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
            0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
            0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
            0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
            0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
            0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
            0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
            0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
            0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
            0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
            0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
            0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
            0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
            0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
            0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
        };

        uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = 0;
            for (int j = 0; j < 8; j++)
                w[i] = (w[i] << 8) | p[static_cast<size_t>(i) * 8 + static_cast<size_t>(j)];
        }
        for (int i = 16; i < 80; i++) {
            const uint64_t s0 = ror(w[i - 15], 1) ^ ror(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const uint64_t s1 = ror(w[i - 2], 19) ^ ror(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint64_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint64_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 80; i++) {
            const uint64_t S1 = ror(e, 14) ^ ror(e, 18) ^ ror(e, 41);
            const uint64_t ch = (e & f) ^ (~e & g);
            const uint64_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint64_t S0 = ror(a, 28) ^ ror(a, 34) ^ ror(a, 39);
            const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint64_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
};
