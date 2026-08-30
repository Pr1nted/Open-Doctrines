/**
 * Convert an AI model file between the plain and the compressed container.
 *
 *     ModelPack pack   <in.bin> <out.bin>   # ODAI -> ODAZ
 *     ModelPack unpack <in.bin> <out.bin>   # ODAZ -> ODAI
 *     ModelPack info   <file.bin>           # which one it is, and how big
 *
 * The game writes ODAZ on every save, so this exists for the files it does not
 * write: a model produced by an older build, one restored from a backup, or the
 * shipped data/ai/model.bin the first time it is converted. Linking the game's
 * own src/ai/ModelBlob.cpp rather than reimplementing the format is the point
 * -- a packer that drifted from the reader would produce a file that only the
 * packer can read.
 *
 * `pack` refuses to write unless the bytes it is about to produce decode back
 * to the exact input. See tests/model_blob_test.cpp for the rest of that
 * argument.
 */

#include "ai/ModelBlob.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool readFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); fprintf(stderr, "%s is empty\n", path); return false; }
    out.resize((size_t)n);
    const size_t rd = fread(out.data(), 1, out.size(), f);
    fclose(f);
    if (rd != out.size()) { fprintf(stderr, "short read of %s\n", path); return false; }
    return true;
}

/// Through a temp file and a rename, so an interrupted run cannot leave a
/// half-written model where a whole one used to be.
static bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", tmp.c_str()); return false; }
    const size_t w = fwrite(data.data(), 1, data.size(), f);
    const bool ok = (fclose(f) == 0) && w == data.size();
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        fprintf(stderr, "failed to write %s\n", path.c_str());
        return false;
    }
    return true;
}

static const char* kindOf(const std::vector<uint8_t>& b) {
    if (modelblob::isPacked(b.data(), b.size())) return "ODAZ (compressed)";
    if (b.size() >= 4 && memcmp(b.data(), "ODAI", 4) == 0) return "ODAI (plain)";
    if (b.size() >= 4 && memcmp(b.data(), "ODLG", 4) == 0) return "ODLG (league checkpoint)";
    return "unrecognised";
}

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";

    if (cmd == "info" && argc == 3) {
        std::vector<uint8_t> buf;
        if (!readFile(argv[2], buf)) return 1;
        printf("%s: %s, %zu bytes\n", argv[2], kindOf(buf), buf.size());
        if (modelblob::isPacked(buf.data(), buf.size())) {
            std::vector<uint8_t> plain = buf;
            if (!modelblob::unpack(plain)) { fprintf(stderr, "  container is corrupt\n"); return 1; }
            printf("  unpacks to %zu bytes (%.1f%%)\n", plain.size(),
                   100.0 * (double)buf.size() / (double)plain.size());
        }
        return 0;
    }

    if (cmd == "pack" && argc == 4) {
        std::vector<uint8_t> buf;
        if (!readFile(argv[2], buf)) return 1;
        std::vector<uint8_t> plain = buf;
        if (!modelblob::unpack(plain)) { fprintf(stderr, "%s is corrupt\n", argv[2]); return 1; }
        if (plain.size() < 6 || memcmp(plain.data(), "ODAI", 4) != 0) {
            fprintf(stderr, "%s is %s, not a model\n", argv[2], kindOf(plain));
            return 1;
        }
        std::vector<uint8_t> packed = modelblob::pack(plain);
        if (packed.empty()) { fprintf(stderr, "compression failed\n"); return 1; }
        std::vector<uint8_t> check = packed;
        if (!modelblob::unpack(check) || check != plain) {
            fprintf(stderr, "REFUSING TO WRITE: the packed bytes do not decode back to the "
                            "input. %s is unchanged.\n", argv[2]);
            return 1;
        }
        if (!writeFile(argv[3], packed)) return 1;
        printf("%s -> %s: %zu -> %zu bytes (%.1f%%), verified lossless\n", argv[2], argv[3],
               plain.size(), packed.size(), 100.0 * (double)packed.size() / (double)plain.size());
        return 0;
    }

    if (cmd == "unpack" && argc == 4) {
        std::vector<uint8_t> buf;
        if (!readFile(argv[2], buf)) return 1;
        std::vector<uint8_t> plain = buf;
        if (!modelblob::unpack(plain)) { fprintf(stderr, "%s is corrupt\n", argv[2]); return 1; }
        if (!writeFile(argv[3], plain)) return 1;
        printf("%s -> %s: %zu -> %zu bytes\n", argv[2], argv[3], buf.size(), plain.size());
        return 0;
    }

    fprintf(stderr,
            "usage: ModelPack pack   <in> <out>\n"
            "       ModelPack unpack <in> <out>\n"
            "       ModelPack info   <file>\n");
    return 2;
}
