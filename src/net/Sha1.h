#pragma once

// SHA-1, for the WebSocket handshake and nothing else.
//
// It is NOT doing security work here. RFC 6455 uses it as a fixed
// transformation so each side can prove it understood the other's request;
// what makes the connection trustworthy is TLS, or the tunnel terminating it.
// Do not reach for this for anything that needs a real hash -- util/Sha256.h
// is next door.
//
// Header-only because both halves of the handshake need it: the client
// computes the accept it expects, the server computes the one it sends.

#include <cstdint>
#include <cstring>

// SHA-1, needed only for the handshake's Sec-WebSocket-Accept. It is not doing
// any security work here -- the RFC uses it as a fixed transformation to prove
// the peer understood the request, and TLS is what makes the connection
// trustworthy.
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t len = 0;
    uint8_t  buf[64];
    size_t   have = 0;

    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
                   (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
            const uint32_t t = rol(a,5) + f + e + k + w[i];
            e = d; d = c; c = rol(b,30); b = a; a = t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }

    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            const size_t take = n < 64 - have ? n : 64 - have;
            memcpy(buf + have, p, take);
            have += take; p += take; n -= take;
            if (have == 64) { block(buf); have = 0; }
        }
    }

    void finish(uint8_t out[20]) {
        const uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (have != 56) update(&zero, 1);
        uint8_t tail[8];
        for (int i = 0; i < 8; i++) tail[i] = (uint8_t)(bits >> (56 - i*8));
        update(tail, 8);
        for (int i = 0; i < 5; i++) {
            out[i*4]   = (uint8_t)(h[i] >> 24);
            out[i*4+1] = (uint8_t)(h[i] >> 16);
            out[i*4+2] = (uint8_t)(h[i] >> 8);
            out[i*4+3] = (uint8_t)h[i];
        }
    }
};
