#include "TurnSeal.h"

#include <cstring>

#if !defined(__EMSCRIPTEN__) && defined(OD_ENABLE_NET)

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>

namespace {

constexpr size_t kNonceBytes = 12;   // 96 bits, the size GCM is defined for
constexpr size_t kTagBytes = 16;

/**
 * Associated data: what this blob is allowed to be.
 *
 * The turn number and the player are authenticated but not encrypted, so a
 * sealed submission opens only as the turn and player it was sealed for. Both
 * lengths are included so that ("12", "ab") and ("1", "2ab") cannot produce the
 * same bytes -- an ambiguity that would let one binding be traded for another.
 */
std::vector<uint8_t> associatedData(uint32_t turnNumber, const std::string& psid) {
    std::vector<uint8_t> ad;
    ad.reserve(8 + psid.size());
    for (int shift = 24; shift >= 0; shift -= 8)
        ad.push_back(static_cast<uint8_t>((turnNumber >> shift) & 0xFF));
    const uint32_t n = static_cast<uint32_t>(psid.size());
    for (int shift = 24; shift >= 0; shift -= 8)
        ad.push_back(static_cast<uint8_t>((n >> shift) & 0xFF));
    ad.insert(ad.end(), psid.begin(), psid.end());
    return ad;
}

/** base64url without padding, shared with the token format. */
std::string b64urlEncode(const uint8_t* data, size_t n) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((n * 4 + 2) / 3);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = i + 1 < n ? data[i + 1] : 0;
        const uint32_t c = i + 2 < n ? data[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        if (i + 1 < n) out += T[(v >> 6) & 63];
        if (i + 2 < n) out += T[v & 63];
    }
    return out;
}

bool b64urlDecodeInto(const std::string& in, uint8_t* out, size_t expected) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    size_t written = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        const int v = value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= expected) return false;
            out[written++] = static_cast<uint8_t>((acc >> bits) & 0xFF);
        }
    }
    return written == expected;
}

}  // namespace

bool TurnSealKey::valid() const {
    // An all-zero key is what an uninitialised one looks like, and sealing with
    // it would look like it worked.
    for (uint8_t b : bytes) if (b != 0) return true;
    return false;
}

std::string TurnSealKey::toText() const {
    return b64urlEncode(bytes, sizeof(bytes));
}

bool TurnSealKey::fromText(const std::string& text, TurnSealKey& out) {
    TurnSealKey k;
    if (!b64urlDecodeInto(text, k.bytes, sizeof(k.bytes))) return false;
    if (!k.valid()) return false;
    out = k;
    return true;
}

bool turnSealAvailable() { return true; }

bool turnSealKeyGenerate(TurnSealKey& out) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    const char* pers = "opendoctrines-turn-seal";
    bool ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                    reinterpret_cast<const unsigned char*>(pers),
                                    strlen(pers)) == 0;
    TurnSealKey k;
    if (ok) ok = mbedtls_ctr_drbg_random(&drbg, k.bytes, sizeof(k.bytes)) == 0;

    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);

    if (!ok || !k.valid()) return false;
    out = k;
    return true;
}

bool turnSeal(const TurnSealKey& key, uint32_t turnNumber, const std::string& psid,
              const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& out) {
    if (!key.valid()) return false;

    // A fresh nonce every time. Reusing one under the same key is the single
    // way to break GCM completely, so it is drawn from the CSPRNG rather than
    // from a counter that could restart with the process.
    uint8_t nonce[kNonceBytes];
    {
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context drbg;
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        const char* pers = "opendoctrines-turn-nonce";
        bool ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                        reinterpret_cast<const unsigned char*>(pers),
                                        strlen(pers)) == 0;
        if (ok) ok = mbedtls_ctr_drbg_random(&drbg, nonce, sizeof(nonce)) == 0;
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        if (!ok) return false;
    }

    const std::vector<uint8_t> ad = associatedData(turnNumber, psid);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key.bytes,
                                 sizeof(key.bytes) * 8) == 0;

    std::vector<uint8_t> cipher(plaintext.size());
    uint8_t tag[kTagBytes];
    if (ok) {
        ok = mbedtls_gcm_crypt_and_tag(
                 &gcm, MBEDTLS_GCM_ENCRYPT, plaintext.size(), nonce, sizeof(nonce),
                 ad.data(), ad.size(),
                 plaintext.empty() ? nullptr : plaintext.data(),
                 cipher.empty() ? nullptr : cipher.data(),
                 sizeof(tag), tag) == 0;
    }
    mbedtls_gcm_free(&gcm);
    if (!ok) return false;

    // nonce || ciphertext || tag
    out.clear();
    out.reserve(sizeof(nonce) + cipher.size() + sizeof(tag));
    out.insert(out.end(), nonce, nonce + sizeof(nonce));
    out.insert(out.end(), cipher.begin(), cipher.end());
    out.insert(out.end(), tag, tag + sizeof(tag));
    return true;
}

bool turnOpen(const TurnSealKey& key, uint32_t turnNumber, const std::string& psid,
              const std::vector<uint8_t>& sealed, std::vector<uint8_t>& out) {
    if (!key.valid()) return false;
    if (sealed.size() < kNonceBytes + kTagBytes) return false;

    const uint8_t* nonce = sealed.data();
    const size_t cipherLen = sealed.size() - kNonceBytes - kTagBytes;
    const uint8_t* cipher = sealed.data() + kNonceBytes;
    const uint8_t* tag = sealed.data() + kNonceBytes + cipherLen;

    const std::vector<uint8_t> ad = associatedData(turnNumber, psid);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key.bytes,
                                 sizeof(key.bytes) * 8) == 0;

    std::vector<uint8_t> plain(cipherLen);
    if (ok) {
        // auth_decrypt, not decrypt-then-check: it verifies the tag before
        // returning anything, so a forgery never becomes plaintext this code
        // has already started using.
        ok = mbedtls_gcm_auth_decrypt(
                 &gcm, cipherLen, nonce, kNonceBytes, ad.data(), ad.size(),
                 tag, kTagBytes,
                 cipherLen ? cipher : nullptr,
                 plain.empty() ? nullptr : plain.data()) == 0;
    }
    mbedtls_gcm_free(&gcm);
    if (!ok) return false;

    out = std::move(plain);
    return true;
}

#else   // no networking in this build

bool TurnSealKey::valid() const {
    for (uint8_t b : bytes) if (b != 0) return true;
    return false;
}
std::string TurnSealKey::toText() const { return {}; }
bool TurnSealKey::fromText(const std::string&, TurnSealKey&) { return false; }
bool turnSealAvailable() { return false; }
bool turnSealKeyGenerate(TurnSealKey&) { return false; }
bool turnSeal(const TurnSealKey&, uint32_t, const std::string&,
              const std::vector<uint8_t>&, std::vector<uint8_t>&) { return false; }
bool turnOpen(const TurnSealKey&, uint32_t, const std::string&,
              const std::vector<uint8_t>&, std::vector<uint8_t>&) { return false; }

#endif
