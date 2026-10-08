// The row-by-row PNG decoder agrees with stb_image, byte for byte.
//
// src/util/PngRows lets a dedicated server read an 8192x4096 map layer without
// ever holding it whole -- see the header for why that matters on a 512 MB
// host. Anything the server computes from those rows (which province each
// pixel is, which pixels are land) has to equal what the full decode gave, or
// the server would quietly play a different map. So this decodes every
// shipped map's layers both ways and compares every byte, then does the same
// for small images of every colour type stb_image_write can produce.

#include "util/PngRows.h"

#include "miniz.h"
#include "miniz_zip.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {
int g_checks = 0, g_failures = 0;
void check(const std::string& what, bool ok, const std::string& got = {}) {
    g_checks++;
    if (ok) { printf("  ok    %s\n", what.c_str()); return; }
    g_failures++;
    printf("  FAIL  %s%s%s\n", what.c_str(), got.empty() ? "" : "  --  ", got.c_str());
}

/** Both decodes, compared. Returns a description of the first difference. */
std::string compare(const std::vector<uint8_t>& png) {
    int w = 0, h = 0, ch = 0;
    unsigned char* ref = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &ch, 4);
    if (!ref) return "stb could not decode it";
    int sw = 0, sh = 0;
    std::string why, diff;
    const bool ok = pngForEachRow(png.data(), png.size(),
        [&](int ww, int hh) { sw = ww; sh = hh; return true; },
        [&](int y, const uint8_t* rgba) {
            if (!diff.empty() || y >= h) return;
            if (std::memcmp(rgba, ref + (size_t)y * w * 4, (size_t)w * 4) != 0) {
                for (int x = 0; x < w; ++x) {
                    if (std::memcmp(rgba + x * 4, ref + ((size_t)y * w + x) * 4, 4) != 0) {
                        diff = "first difference at (" + std::to_string(x) + "," +
                               std::to_string(y) + ")";
                        break;
                    }
                }
            }
        }, &why);
    stbi_image_free(ref);
    if (!ok) return "streaming decode failed: " + why;
    if (sw != w || sh != h) return "size differs";
    return diff;
}

std::vector<uint8_t> zipEntry(const std::string& zipPath, const char* name) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof zip);
    std::vector<uint8_t> out;
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0)) return out;
    size_t n = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&zip, name, &n, 0);
    if (p) { out.assign((uint8_t*)p, (uint8_t*)p + n); mz_free(p); }
    mz_zip_reader_end(&zip);
    return out;
}

void writeTo(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}
}  // namespace

int main(int argc, char** argv) {
    const std::string dataDir = argc > 1 ? argv[1] : "data/";

    printf("=== small images, every colour type stb_image_write makes ===\n");
    for (int comp = 1; comp <= 4; ++comp) {
        const int w = 37, h = 23;           // odd sizes: no row is a round number of bytes
        std::vector<uint8_t> px((size_t)w * h * comp);
        for (size_t i = 0; i < px.size(); ++i) px[i] = (uint8_t)((i * 131 + i / 7) ^ (i >> 3));
        std::vector<uint8_t> png;
        stbi_write_png_to_func(writeTo, &png, w, h, comp, px.data(), w * comp);
        const std::string d = compare(png);
        check(std::to_string(comp) + " channel(s) decode identically", d.empty(), d);
    }

    printf("\n=== hostile input is refused, not crashed on ===\n");
    {
        std::vector<uint8_t> png;
        const uint8_t px[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        stbi_write_png_to_func(writeTo, &png, 2, 2, 3, px, 6);
        std::string why;
        for (size_t cut : {png.size() / 2, png.size() - 20, (size_t)40}) {
            std::vector<uint8_t> shortPng(png.begin(), png.begin() + (long)cut);
            const bool ok = pngForEachRow(shortPng.data(), shortPng.size(), nullptr,
                                          [](int, const uint8_t*) {}, &why);
            check("a PNG cut to " + std::to_string(cut) + " bytes is refused", !ok, why);
        }
        const uint8_t junk[64] = {137, 80, 78, 71, 13, 10, 26, 10};
        check("a signature with nothing after it is refused",
              !pngForEachRow(junk, sizeof junk, nullptr, [](int, const uint8_t*) {}, &why));
    }

    printf("\n=== every shipped map, both layers ===\n");
    std::error_code ec;
    int maps = 0;
    for (const auto& e : std::filesystem::directory_iterator(dataDir + "STDmaps", ec)) {
        if (e.path().extension() != ".odmap") continue;
        for (const char* layer : {"provinces.png", "land_sea.png"}) {
            const std::vector<uint8_t> png = zipEntry(e.path().string(), layer);
            if (png.empty()) continue;
            const std::string d = compare(png);
            check(e.path().stem().string() + " " + layer, d.empty(), d);
        }
        ++maps;
    }
    check("found the shipped maps", maps > 0, dataDir);

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
