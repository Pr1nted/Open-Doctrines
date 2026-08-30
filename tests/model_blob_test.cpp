/**
 * The AI model container round-trips, or the model is gone.
 *
 * data/ai/model.bin is the only copy of every hour of training that has ever
 * gone into this game. It is now stored deinterleaved and deflated, and the
 * failure mode of getting that wrong is not a crash: it is a file that still
 * loads, still plays, and has had some of its weights quietly altered. That
 * shows up as a slow decline in play strength weeks later, with nothing left
 * to roll back to.
 *
 * So the properties checked here are the ones that would hide such a bug:
 * exact byte equality on the way back out, over inputs whose LENGTH is not a
 * multiple of four (the deinterleave has a tail), over pathological content
 * (all zeros, incompressible noise), and over a real ODAI file when one is
 * lying around. Plus the two compatibility rules the readers depend on: a
 * plain ODAI file must pass through unpack() untouched, and a damaged
 * container must be refused rather than half-decoded.
 *
 *     ModelBlobTest [path/to/model.bin]
 */

#include "ai/ModelBlob.h"

#include "miniz.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static int g_failures = 0;

static void check(bool ok, const std::string& what) {
    if (!ok) {
        printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

/// pack -> unpack must be the identity, and must actually be a container.
static void roundTrip(const std::vector<uint8_t>& plain, const std::string& what) {
    std::vector<uint8_t> packed = modelblob::pack(plain);
    if (packed.empty()) {
        check(false, what + ": pack returned nothing");
        return;
    }
    check(modelblob::isPacked(packed.data(), packed.size()), what + ": result is not ODAZ");

    std::vector<uint8_t> back = packed;
    check(modelblob::unpack(back), what + ": unpack failed");
    check(back == plain, what + ": bytes differ after the round trip");
}

/// Float payloads shaped like real weights: many values of similar magnitude.
static std::vector<uint8_t> weightsLike(size_t floats, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> d(0.0f, 0.05f);
    std::vector<uint8_t> out(floats * 4);
    for (size_t i = 0; i < floats; ++i) {
        float f = d(rng);
        memcpy(out.data() + i * 4, &f, 4);
    }
    return out;
}

int main(int argc, char** argv) {
    printf("model blob round trip\n");

    // Lengths either side of a four-byte boundary: the deinterleave copies the
    // remainder through verbatim, and an off-by-one there loses the tail of the
    // file -- which for an ODAI model is the reward normalisation statistics.
    for (size_t n = 1; n <= 40; ++n) {
        std::vector<uint8_t> v(n);
        for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)(i * 37 + 11);
        roundTrip(v, "length " + std::to_string(n));
    }

    // Content extremes: maximally compressible, and not compressible at all.
    roundTrip(std::vector<uint8_t>(1 << 20, 0), "one megabyte of zeros");
    {
        std::mt19937 rng(99);
        std::vector<uint8_t> noise(1 << 20);
        for (auto& b : noise) b = (uint8_t)rng();
        roundTrip(noise, "one megabyte of noise");
    }

    // What the file actually holds, at a size worth measuring.
    {
        const std::vector<uint8_t> w = weightsLike(1 << 19, 7);   // 2 MB of floats
        roundTrip(w, "two megabytes of weights");
        const size_t packed = modelblob::pack(w).size();
        // Deinterleaving is the whole point of the container, and the way it
        // would break is by quietly not happening -- the file would still
        // round-trip, just bigger. So the check is against what a plain deflate
        // pass over the same bytes costs, which is the thing it has to beat.
        mz_ulong flat = mz_compressBound((mz_ulong)w.size());
        std::vector<uint8_t> scratch(flat);
        check(mz_compress2(scratch.data(), &flat, w.data(), (mz_ulong)w.size(),
                           modelblob::SAVE_LEVEL) == MZ_OK, "plain deflate failed");
        printf("  weights: %zu -> %zu packed (%.1f%%), plain deflate %lu (%.1f%%)\n",
               w.size(), packed, 100.0 * (double)packed / (double)w.size(),
               (unsigned long)flat, 100.0 * (double)flat / (double)w.size());
        check(packed < (size_t)flat, "deinterleaving did not beat a plain deflate");
    }

    // A plain ODAI file must survive unpack() untouched -- that is what lets an
    // older model, or one a worker is midway through writing, still load.
    {
        std::vector<uint8_t> odai = {'O', 'D', 'A', 'I', 8, 21, 1, 2, 3, 4, 5};
        const std::vector<uint8_t> before = odai;
        check(modelblob::unpack(odai), "plain ODAI was rejected");
        check(odai == before, "plain ODAI was modified");
        check(!modelblob::isPacked(before.data(), before.size()), "ODAI reported as packed");
    }

    // Damage must be refused, not half-decoded into weights.
    {
        const std::vector<uint8_t> w = weightsLike(4096, 3);
        std::vector<uint8_t> packed = modelblob::pack(w);

        std::vector<uint8_t> truncated(packed.begin(), packed.end() - packed.size() / 3);
        check(!modelblob::unpack(truncated), "a truncated container was accepted");

        std::vector<uint8_t> flipped = packed;
        flipped[flipped.size() / 2] ^= 0xFF;
        std::vector<uint8_t> out = flipped;
        // A flipped bit either fails inflate or fails its Adler-32; either way
        // it must not come back as the original bytes.
        check(!modelblob::unpack(out) || out != w, "a corrupted container decoded as valid");

        std::vector<uint8_t> lied = packed;
        lied[6] = 0xFF; lied[7] = 0xFF; lied[8] = 0xFF; lied[9] = 0xFF;  // raw length
        check(!modelblob::unpack(lied), "a wrong length was accepted");
    }

    // The real file, when the caller points at one.
    if (argc > 1) {
        FILE* f = fopen(argv[1], "rb");
        if (!f) {
            printf("  (no model at %s, skipped)\n", argv[1]);
        } else {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            std::vector<uint8_t> buf((size_t)(n > 0 ? n : 0));
            const size_t rd = buf.empty() ? 0 : fread(buf.data(), 1, buf.size(), f);
            fclose(f);
            check(rd == buf.size(), "short read of the model file");

            std::vector<uint8_t> plain = buf;
            check(modelblob::unpack(plain), "the shipped model did not unpack");
            check(plain.size() >= 6 && memcmp(plain.data(), "ODAI", 4) == 0,
                  "the shipped model is not ODAI once unpacked");
            roundTrip(plain, "the shipped model");
            const size_t packed = modelblob::pack(plain).size();
            printf("  %s: %zu plain -> %zu packed (%.1f%%)\n", argv[1], plain.size(), packed,
                   100.0 * (double)packed / (double)plain.size());
            // A real model is float weights and Adam moments, and those pack to
            // a little under half. Well clear of the ~45% measured, so this
            // fires on the container silently degrading rather than on the
            // model happening to train into slightly noisier weights.
            check(packed < plain.size() * 3 / 5, "the shipped model barely compressed");
        }
    }

    if (g_failures) {
        printf("model blob: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("model blob: OK\n");
    return 0;
}
