// SHA-512 and Ed25519 verification.
//
// EVERY VECTOR HERE COMES FROM OUTSIDE THIS CODEBASE -- FIPS 180-4 for the
// hash, RFC 8032 section 7.1 for the signatures. That is the whole point of the
// file. A crypto implementation that agrees with itself proves nothing, and we
// have already been bitten once by exactly that: the WebSocket handshake had
// the RFC's magic constant mistyped identically in the client and the server,
// so the two halves agreed perfectly and neither could talk to anything else.
//
// The rejection cases matter as much as the acceptances. A verifier that
// returns true unconditionally passes every positive vector in this file.
//
// Build target: NetCryptoTest. Run it; non-zero exit means a case failed.

#include "util/Ed25519.h"
#include "util/Sha512.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(const char* what, bool ok, const std::string& got = {}) {
    g_checks++;
    if (ok) { printf("  ok    %s\n", what); return; }
    g_failures++;
    printf("  FAIL  %s%s%s\n", what, got.empty() ? "" : "  --  ", got.c_str());
}

std::vector<uint8_t> fromHex(const std::string& s) {
    std::vector<uint8_t> o;
    o.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        o.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    return o;
}

std::string toHex(const uint8_t* d, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += H[d[i] >> 4]; s += H[d[i] & 15]; }
    return s;
}

void testSha512() {
    printf("\n=== SHA-512 (FIPS 180-4) ===\n");

    struct { std::string in; const char* want; const char* label; } v[] = {
        {"", "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
             "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
         "the empty string"},
        {"abc", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
         "\"abc\""},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
         "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
         "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
         "a two-block message"},
        {std::string(1000000, 'a'),
         "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
         "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
         "one million 'a'"},
    };

    for (const auto& t : v) {
        uint8_t d[Sha512::kDigestSize];
        Sha512::hash(reinterpret_cast<const uint8_t*>(t.in.data()), t.in.size(), d);
        const std::string got = toHex(d, sizeof(d));
        check(t.label, got == t.want, got == t.want ? "" : got);
    }

    // Feeding the same bytes in awkward pieces must not change the answer;
    // that is where a buffered hash goes wrong, and only on some input sizes.
    {
        const std::string s(5000, 'x');
        uint8_t once[64], piecemeal[64];
        Sha512::hash(reinterpret_cast<const uint8_t*>(s.data()), s.size(), once);
        Sha512 st;
        for (size_t i = 0; i < s.size(); i += 7) {
            const size_t take = s.size() - i < 7 ? s.size() - i : 7;
            st.update(reinterpret_cast<const uint8_t*>(s.data()) + i, take);
        }
        st.finish(piecemeal);
        check("streaming in 7-byte pieces matches one shot",
              std::memcmp(once, piecemeal, 64) == 0);
    }

    // Exactly on and around the 128-byte block and the 112-byte length field,
    // which is where padding is easy to get wrong.
    for (size_t n : {111u, 112u, 113u, 127u, 128u, 129u}) {
        const std::string s(n, 'q');
        uint8_t a[64], b[64];
        Sha512::hash(reinterpret_cast<const uint8_t*>(s.data()), n, a);
        Sha512 st;
        st.update(reinterpret_cast<const uint8_t*>(s.data()), n / 2);
        st.update(reinterpret_cast<const uint8_t*>(s.data()) + n / 2, n - n / 2);
        st.finish(b);
        check(("a " + std::to_string(n) + "-byte message pads consistently").c_str(),
              std::memcmp(a, b, 64) == 0);
    }
}

struct Vector {
    const char* label;
    const char* pub;
    const char* msg;
    const char* sig;
};

// RFC 8032 section 7.1.
const Vector kRfc8032[] = {
    {"TEST 1 (empty message)",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f"
     "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"TEST 2 (one byte)",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da0"
     "85ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"TEST 3 (two bytes)",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac1"
     "8ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    {"TEST SHA(abc) (64-byte message)",
     "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
     "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
     "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
     "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"},
};

void testEd25519Accepts() {
    printf("\n=== Ed25519, the RFC 8032 vectors ===\n");
    for (const Vector& t : kRfc8032) {
        const auto pk = fromHex(t.pub), m = fromHex(t.msg), sg = fromHex(t.sig);
        check(t.label, ed25519Verify(sg.data(), m.data(), m.size(), pk.data()));
    }
}

void testEd25519Rejects() {
    printf("\n=== Ed25519, what it must refuse ===\n");

    // Without these, a function that just returns true passes the whole file.
    for (const Vector& t : kRfc8032) {
        const auto pk = fromHex(t.pub), m = fromHex(t.msg), sg = fromHex(t.sig);

        auto flippedSig = sg;
        flippedSig[10] ^= 0x01;
        check("a tampered signature is refused",
              !ed25519Verify(flippedSig.data(), m.data(), m.size(), pk.data()));

        auto flippedMsg = m;
        if (flippedMsg.empty()) flippedMsg.push_back(0x00);
        else flippedMsg[0] ^= 0x01;
        check("a tampered message is refused",
              !ed25519Verify(sg.data(), flippedMsg.data(), flippedMsg.size(), pk.data()));

        auto wrongKey = pk;
        wrongKey[0] ^= 0x01;
        check("a different public key is refused",
              !ed25519Verify(sg.data(), m.data(), m.size(), wrongKey.data()));

        // S + L is the classic malleability: a second valid-looking signature
        // over the same message. RFC 8032 5.1.7 requires refusing it.
        const uint8_t L[32] = {
            0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
            0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
        };
        auto malleable = sg;
        int carry = 0;
        for (int i = 0; i < 32; i++) {
            const int t2 = malleable[32 + static_cast<size_t>(i)] + L[i] + carry;
            malleable[32 + static_cast<size_t>(i)] = static_cast<uint8_t>(t2 & 0xff);
            carry = t2 >> 8;
        }
        if (!carry) {
            check("S + L is refused (no signature malleability)",
                  !ed25519Verify(malleable.data(), m.data(), m.size(), pk.data()));
        }
    }

    // An all-zero key is not a valid curve point encoding of anything useful,
    // and an all-zero signature must never verify against it.
    {
        const std::vector<uint8_t> zeroKey(32, 0), zeroSig(64, 0);
        const std::vector<uint8_t> msg{1, 2, 3};
        check("an all-zero key and signature are refused",
              !ed25519Verify(zeroSig.data(), msg.data(), msg.size(), zeroKey.data()));
    }

    // Random bytes are overwhelmingly not on the curve; this must fail
    // cleanly rather than misbehaving on a point that does not decode.
    {
        std::vector<uint8_t> junkKey(32), junkSig(64);
        for (size_t i = 0; i < 32; i++) junkKey[i] = static_cast<uint8_t>(i * 7 + 3);
        for (size_t i = 0; i < 64; i++) junkSig[i] = static_cast<uint8_t>(i * 5 + 1);
        const std::vector<uint8_t> msg{9, 9};
        check("undecodable key bytes are refused",
              !ed25519Verify(junkSig.data(), msg.data(), msg.size(), junkKey.data()));
    }
}

}  // namespace

int main() {
    printf("hashing and signature verification\n");
    testSha512();
    testEd25519Accepts();
    testEd25519Rejects();
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
