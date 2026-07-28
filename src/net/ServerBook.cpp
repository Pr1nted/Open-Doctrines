#include "ServerBook.h"

#include "HttpClient.h"   // the JSON readers, shared rather than duplicated
#include "NetUrl.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {

constexpr size_t kMaxEntries   = 200;
constexpr size_t kMaxNameLen   = 48;
constexpr size_t kMaxIssuerLen = 256;
constexpr size_t kMaxFileBytes = 256 * 1024;

std::string clamp(const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n);
}

}  // namespace

bool ServerBook::validCode(const std::string& code) {
    // The human-typed form: uppercase letters and digits with optional dashes,
    // as net/src/util/crypto.ts humanCode() produces.
    if (code.size() < 4 || code.size() > 20) return false;
    bool alnum = false;
    for (char c : code) {
        if (c == '-') continue;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!ok) return false;
        alnum = true;
    }
    return alnum;
}

bool ServerBook::parseInvite(const std::string& text, const std::string& defaultIssuer,
                             std::string& issuerOut, std::string& codeOut) {
    std::string t = text;
    // Trim: a pasted invite almost always carries whitespace.
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    if (t.empty()) return false;

    // A bare code uses whatever service the game is configured against.
    if (t.find("://") == std::string::npos) {
        if (!validCode(t) || defaultIssuer.empty()) return false;
        issuerOut = defaultIssuer;
        codeOut = t;
        return true;
    }

    // A full URL carries both. Parsed with the shared parser rather than by
    // hand, so the same refusals apply -- no userinfo, no control characters.
    NetUrl url;
    if (!NetUrl::parse(t, url)) return false;
    if (!url.secure) return false;   // an invite over http is not one we follow

    // .../session/<CODE> or .../session/<CODE>/ws
    const std::string marker = "/session/";
    const size_t at = url.path.find(marker);
    if (at == std::string::npos) return false;
    std::string code = url.path.substr(at + marker.size());
    const size_t slash = code.find('/');
    if (slash != std::string::npos) code = code.substr(0, slash);
    const size_t query = code.find('?');
    if (query != std::string::npos) code = code.substr(0, query);
    if (!validCode(code)) return false;

    const bool defaultPort = url.port == 443;
    issuerOut = "https://" + url.host + (defaultPort ? "" : ":" + std::to_string(url.port));
    codeOut = code;
    return true;
}

void ServerBook::load(const std::string& path) {
    m_path = path;
    m_entries.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) return;   // no file yet is the normal first-run case, not an error
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() > kMaxFileBytes) return;

    // Hand-scanned rather than parsed, like every other reader here. The file
    // is ours, but it lives on disk where anything could have edited it, so a
    // malformed entry is skipped rather than trusted.
    size_t at = 0;
    while (m_entries.size() < kMaxEntries) {
        const size_t open = text.find('{', at);
        if (open == std::string::npos) break;
        const size_t close = text.find('}', open);
        if (close == std::string::npos) break;
        const std::string obj = text.substr(open, close - open + 1);
        at = close + 1;

        ServerEntry e;
        e.name         = clamp(httpJsonString(obj, "name", kMaxNameLen), kMaxNameLen);
        e.issuer       = clamp(httpJsonString(obj, "issuer", kMaxIssuerLen), kMaxIssuerLen);
        e.code         = httpJsonString(obj, "code", 32);
        e.lastJoined   = httpJsonNumber(obj, "lastJoined", 0);
        e.lastHostName = clamp(httpJsonString(obj, "lastHostName", 64), 64);

        if (!e.valid()) continue;
        if (!e.code.empty() && !validCode(e.code)) e.code.clear();
        m_entries.push_back(e);
    }
    sort();
}

bool ServerBook::save() const {
    if (m_path.empty()) return false;
    std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    out << "{\n  \"servers\": [\n";
    for (size_t i = 0; i < m_entries.size(); i++) {
        const auto& e = m_entries[i];
        out << "    {\"name\":\"" << httpJsonEscape(e.name)
            << "\",\"issuer\":\"" << httpJsonEscape(e.issuer)
            << "\",\"code\":\"" << httpJsonEscape(e.code)
            << "\",\"lastJoined\":" << e.lastJoined
            << ",\"lastHostName\":\"" << httpJsonEscape(e.lastHostName)
            << "\"}" << (i + 1 < m_entries.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    return static_cast<bool>(out);
}

void ServerBook::addOrUpdate(const ServerEntry& entry) {
    if (!entry.valid()) return;

    ServerEntry e = entry;
    e.name   = clamp(e.name, kMaxNameLen);
    e.issuer = clamp(e.issuer, kMaxIssuerLen);
    if (!e.code.empty() && !validCode(e.code)) e.code.clear();

    for (auto& existing : m_entries) {
        if (existing.issuer == e.issuer && existing.name == e.name) {
            // Keep what the caller did not supply, so pasting a code does not
            // wipe the last-joined time and drop the entry to the bottom.
            if (!e.code.empty()) existing.code = e.code;
            if (e.lastJoined) existing.lastJoined = e.lastJoined;
            if (!e.lastHostName.empty()) existing.lastHostName = e.lastHostName;
            return;
        }
    }
    if (m_entries.size() >= kMaxEntries) return;
    m_entries.push_back(e);
}

bool ServerBook::remove(size_t index) {
    if (index >= m_entries.size()) return false;
    m_entries.erase(m_entries.begin() + static_cast<long>(index));
    return true;
}

bool ServerBook::rename(size_t index, const std::string& name) {
    if (index >= m_entries.size() || name.empty()) return false;
    m_entries[index].name = clamp(name, kMaxNameLen);
    return true;
}

bool ServerBook::setCode(size_t index, const std::string& code) {
    if (index >= m_entries.size()) return false;
    if (!code.empty() && !validCode(code)) return false;
    m_entries[index].code = code;
    return true;
}

void ServerBook::markJoined(size_t index, const std::string& hostName, long long nowUnix) {
    if (index >= m_entries.size()) return;
    m_entries[index].lastJoined = nowUnix;
    if (!hostName.empty()) m_entries[index].lastHostName = clamp(hostName, 64);
}

void ServerBook::sort() {
    std::stable_sort(m_entries.begin(), m_entries.end(),
                     [](const ServerEntry& a, const ServerEntry& b) {
                         if (a.lastJoined != b.lastJoined) return a.lastJoined > b.lastJoined;
                         return a.name < b.name;
                     });
}
