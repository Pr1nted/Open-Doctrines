#include "LobbyMap.h"

#include "NetProtocol.h"

#include <algorithm>
#include <unordered_map>

LobbyMap LobbyMap::build(uint16_t width, uint16_t height,
                         const std::function<uint16_t(float, float)>& countryAt,
                         const std::function<uint32_t(uint16_t)>& colorOf) {
    LobbyMap m;
    if (width == 0 || height == 0 || width > kMaxWidth || height > kMaxHeight) return m;
    m.width = width;
    m.height = height;
    m.cells.resize((size_t)width * height, 0);
    std::vector<uint16_t> seen;
    for (uint16_t y = 0; y < height; ++y) {
        for (uint16_t x = 0; x < width; ++x) {
            const uint16_t id = countryAt((x + 0.5f) / width, (y + 0.5f) / height);
            m.cells[(size_t)y * width + x] = id;
            if (id) seen.push_back(id);
        }
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    for (uint16_t id : seen) {
        const uint32_t c = colorOf ? colorOf(id) : 0x808080u;
        m.colors.push_back({id, (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c});
    }
    return m;
}

// Layout, version 1:
//   u8  version
//   u16 width, u16 height
//   u16 swatch count, then per swatch: u16 id, u8 r, u8 g, u8 b
//   u32 run count, then per run: u16 id, u16 length
std::vector<uint8_t> LobbyMap::encode() const {
    NetWriter w;
    w.u8(1);
    w.u16(width);
    w.u16(height);
    w.u16((uint16_t)std::min<size_t>(colors.size(), 0xFFFF));
    for (size_t i = 0; i < colors.size() && i < 0xFFFF; ++i) {
        w.u16(colors[i].countryId);
        w.u8(colors[i].r);
        w.u8(colors[i].g);
        w.u8(colors[i].b);
    }
    std::vector<std::pair<uint16_t, uint16_t>> runs;
    for (size_t i = 0; i < cells.size();) {
        const uint16_t id = cells[i];
        size_t n = 1;
        while (i + n < cells.size() && cells[i + n] == id && n < 0xFFFF) ++n;
        runs.push_back({id, (uint16_t)n});
        i += n;
    }
    w.u32((uint32_t)runs.size());
    for (const auto& r : runs) {
        w.u16(r.first);
        w.u16(r.second);
    }
    return w.take();
}

bool LobbyMap::decode(const uint8_t* data, size_t size, LobbyMap& out) {
    out = LobbyMap{};
    NetReader r(data, size);
    if (r.u8() != 1 || !r.ok()) return false;
    const uint16_t w = r.u16();
    const uint16_t h = r.u16();
    if (!r.ok() || w == 0 || h == 0 || w > kMaxWidth || h > kMaxHeight) return false;

    LobbyMap m;
    m.width = w;
    m.height = h;
    const uint16_t nColors = r.u16();
    if (!r.ok()) return false;
    m.colors.reserve(nColors);
    for (uint16_t i = 0; i < nColors; ++i) {
        Swatch s;
        s.countryId = r.u16();
        s.r = r.u8();
        s.g = r.u8();
        s.b = r.u8();
        if (!r.ok()) return false;
        m.colors.push_back(s);
    }

    const size_t total = (size_t)w * h;
    const uint32_t nRuns = r.u32();
    if (!r.ok() || nRuns > total) return false;
    m.cells.reserve(total);
    for (uint32_t i = 0; i < nRuns; ++i) {
        const uint16_t id = r.u16();
        const uint16_t len = r.u16();
        if (!r.ok() || len == 0 || m.cells.size() + len > total) return false;
        m.cells.insert(m.cells.end(), len, id);
    }
    // Every cell accounted for, nothing left over: a short map would draw as
    // a world with a missing south, and a padded one is not the format.
    if (m.cells.size() != total || !r.done()) return false;
    out = std::move(m);
    return true;
}

uint16_t LobbyMap::countryAt(float u, float v) const {
    if (empty() || !(u >= 0.0f) || !(v >= 0.0f) || u >= 1.0f || v >= 1.0f) return 0;
    const size_t x = std::min<size_t>((size_t)(u * width), width - 1);
    const size_t y = std::min<size_t>((size_t)(v * height), height - 1);
    return cells[y * width + x];
}

uint32_t LobbyMap::colorOf(uint16_t countryId) const {
    for (const Swatch& s : colors)
        if (s.countryId == countryId)
            return ((uint32_t)s.r << 16) | ((uint32_t)s.g << 8) | s.b;
    return 0;
}

bool LobbyMap::labelPoint(uint16_t countryId, float& u, float& v) const {
    if (empty() || countryId == 0) return false;
    // Flood the 4-connected components and keep the biggest: a country's
    // name belongs on its homeland, not on the average of its colonies.
    std::vector<uint8_t> done(cells.size(), 0);
    size_t bestSize = 0;
    double bestX = 0, bestY = 0;
    std::vector<size_t> stack;
    for (size_t start = 0; start < cells.size(); ++start) {
        if (done[start] || cells[start] != countryId) continue;
        size_t count = 0;
        double sx = 0, sy = 0;
        stack.push_back(start);
        done[start] = 1;
        while (!stack.empty()) {
            const size_t i = stack.back();
            stack.pop_back();
            const size_t x = i % width, y = i / width;
            ++count;
            sx += x;
            sy += y;
            auto visit = [&](size_t j) {
                if (!done[j] && cells[j] == countryId) { done[j] = 1; stack.push_back(j); }
            };
            if (x > 0) visit(i - 1);
            if (x + 1 < width) visit(i + 1);
            if (y > 0) visit(i - width);
            if (y + 1 < height) visit(i + width);
        }
        if (count > bestSize) { bestSize = count; bestX = sx / count; bestY = sy / count; }
    }
    if (bestSize == 0) return false;
    u = (float)((bestX + 0.5) / width);
    v = (float)((bestY + 0.5) / height);
    return true;
}

std::vector<size_t> lobbyCountryFilter(const std::vector<std::string>& names,
                                       const std::string& query) {
    auto lower = [](std::string t) {
        for (char& c : t) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return t;
    };
    std::string q = lower(query);
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    while (!q.empty() && q.back() == ' ') q.pop_back();
    std::vector<size_t> starts, words, contains;
    for (size_t i = 0; i < names.size(); ++i) {
        if (q.empty()) { starts.push_back(i); continue; }
        const std::string n = lower(names[i]);
        const size_t at = n.find(q);
        if (at == std::string::npos) continue;
        if (at == 0) { starts.push_back(i); continue; }
        // A later word starting with it: look at every occurrence, not just
        // the first, so "an" in "Japan and Andorra" still finds "Andorra".
        bool word = false;
        for (size_t k = at; k != std::string::npos; k = n.find(q, k + 1)) {
            const char before = n[k - 1];
            if (before == ' ' || before == '-' || before == '(' || before == '\'') {
                word = true;
                break;
            }
        }
        (word ? words : contains).push_back(i);
    }
    std::vector<size_t> out;
    out.reserve(starts.size() + words.size() + contains.size());
    out.insert(out.end(), starts.begin(), starts.end());
    out.insert(out.end(), words.begin(), words.end());
    out.insert(out.end(), contains.begin(), contains.end());
    return out;
}
