#include "NetZip.h"

#include "miniz.h"

std::vector<uint8_t> netDeflate(const std::vector<uint8_t>& in) {
    if (in.empty()) return {};
    mz_ulong bound = mz_compressBound((mz_ulong)in.size());
    std::vector<uint8_t> out(bound);
    // Level 6: most of the saving for a fraction of level 9's time, and this
    // runs on the host's frame thread when somebody joins.
    if (mz_compress2(out.data(), &bound, in.data(), (mz_ulong)in.size(), 6) != MZ_OK)
        return {};
    out.resize(bound);
    return out;
}

bool netInflate(const uint8_t* data, size_t size, size_t rawSize, size_t maxRaw,
                std::vector<uint8_t>& out) {
    out.clear();
    if (!data || size == 0 || rawSize == 0 || rawSize > maxRaw) return false;
    out.resize(rawSize);
    mz_ulong got = (mz_ulong)rawSize;
    if (mz_uncompress(out.data(), &got, data, (mz_ulong)size) != MZ_OK || got != rawSize) {
        out.clear();
        return false;
    }
    return true;
}
