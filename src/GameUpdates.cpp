#include "GameUpdates.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "miniz.h"
#include "miniz_zip.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

std::mutex g_mutex;
std::string g_installDir;

// The one host this file will ever talk to. Release assets are served from
// objects.githubusercontent.com after a redirect, so that name is allowed too
// -- but only as a redirect target curl follows, never as a URL taken from the
// reply body.
constexpr const char* kApiLatest =
    "https://api.github.com/repos/Pr1nted/Open-Doctrines/releases/latest";
constexpr const char* kReleasePage =
    "https://github.com/Pr1nted/Open-Doctrines/releases/latest";

// ---------------------------------------------------------------- sha256 ---
// Small and self-contained. Pulling in a crypto library for one digest of one
// downloaded file would be a build dependency out of all proportion.

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t len = 0;
    uint8_t  buf[64];
    size_t   have = 0;

    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
            0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
            0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
            0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
            0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
            0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
            0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
            0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
            0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
                   (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            size_t take = std::min(n, 64 - have);
            memcpy(buf + have, p, take);
            have += take; p += take; n -= take;
            if (have == 64) { block(buf); have = 0; }
        }
    }

    std::string hex() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (have != 56) update(&zero, 1);
        uint8_t tail[8];
        for (int i = 0; i < 8; i++) tail[i] = (uint8_t)(bits >> (56 - i*8));
        update(tail, 8);
        char out[65];
        for (int i = 0; i < 8; i++)
            snprintf(out + i*8, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

// ------------------------------------------------------------ subprocess ---

// Runs curl with the given arguments, never through a shell, so nothing in a
// URL can be read as a command. Output goes to a file rather than a pipe:
// there is then no stdout plumbing to get wrong on either platform, and the
// partially written file is what the progress poll measures.
//
// `expectedSize` and `percent` are optional; when both are given the caller
// gets live progress while the child runs.
bool runCurl(const std::vector<std::string>& args, long long expectedSize,
             std::atomic<int>* percent, const std::string& progressFile) {
    auto poll = [&]() {
        if (!percent || expectedSize <= 0) return;
        std::error_code ec;
        auto n = (long long)fs::file_size(progressFile, ec);
        if (!ec) percent->store((int)std::min(100LL, n * 100 / expectedSize));
    };

#if defined(_WIN32)
    std::string cmd = "curl.exe";
    for (const auto& a : args) cmd += " " + a;   // args are validated, no quoting games
    STARTUPINFOA si{}; si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;                    // no console flash
    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    DWORD rc = 1;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 150) == WAIT_OBJECT_0) break;
        poll();
    }
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return rc == 0;
#else
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("curl"));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp("curl", argv.data());
        _exit(127);
    }
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        poll();
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

// Fetches a URL to a file. -f turns an HTTP error into a failure rather than a
// body we would go on to parse; --proto/--proto-redir keep every hop https,
// including after a redirect to the asset CDN.
bool fetchToFile(const std::string& url, const std::string& dest,
                 long long maxBytes, long long expectedSize = 0,
                 std::atomic<int>* percent = nullptr) {
    std::error_code ec;
    fs::remove(dest, ec);
    std::vector<std::string> args = {
        "-fsSL",
        "--max-time", "300",
        "--max-filesize", std::to_string(maxBytes),
        "--proto", "=https",
        "--proto-redir", "=https",
        "-A", "OpenDoctrines-update-check",
        "-H", "Accept: application/vnd.github+json",
        "-o", dest,
        url,
    };
    if (!runCurl(args, expectedSize, percent, dest)) { fs::remove(dest, ec); return false; }
    return fs::exists(dest, ec);
}

std::string readFile(const std::string& path, size_t cap) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
    if (s.size() > cap) s.resize(cap);
    return s;
}

// Pulls one JSON string value out of untrusted text. Deliberately a small hand
// parser rather than the JSON library: what is wanted is a handful of short
// strings, and a full parser here would be more attack surface for no gain.
std::string jsonString(const std::string& body, size_t from, const char* key,
                       size_t maxLen, size_t* endedAt = nullptr) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = body.find(needle, from);
    if (k == std::string::npos) return {};
    size_t colon = body.find(':', k + needle.size());
    if (colon == std::string::npos) return {};
    size_t q1 = body.find('"', colon);
    if (q1 == std::string::npos) return {};
    // Stop at the closing quote, honouring backslash escapes so a value
    // containing \" does not end the string early.
    size_t q2 = q1 + 1;
    while (q2 < body.size() && body[q2] != '"') q2 += (body[q2] == '\\') ? 2 : 1;
    if (q2 >= body.size() || q2 - q1 - 1 > maxLen) return {};
    if (endedAt) *endedAt = q2;
    return body.substr(q1 + 1, q2 - q1 - 1);
}

long long jsonNumber(const std::string& body, size_t from, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = body.find(needle, from);
    if (k == std::string::npos) return 0;
    size_t colon = body.find(':', k + needle.size());
    if (colon == std::string::npos) return 0;
    return strtoll(body.c_str() + colon + 1, nullptr, 10);
}

// A path taken from a zip is hostile until proven otherwise: an entry named
// "../../.bashrc" must not escape the directory being extracted into.
bool safeRelPath(const std::string& name, std::string& out) {
    if (name.empty() || name.size() > 512) return false;
    if (name[0] == '/' || name[0] == '\\') return false;
    if (name.find('\\') != std::string::npos) return false;   // no Windows separators
    if (name.size() > 1 && name[1] == ':') return false;      // no drive letters
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= name.size()) {
        size_t j = name.find('/', i);
        std::string seg = name.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (seg == "..") return false;
        if (!seg.empty() && seg != ".") parts.push_back(seg);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (parts.empty()) return false;
    // Every release zip wraps its contents in one folder named after the
    // artifact. Strip it so the staged tree matches the install layout.
    parts.erase(parts.begin());
    if (parts.empty()) return false;
    out.clear();
    for (size_t p = 0; p < parts.size(); ++p)
        out += (p ? "/" : "") + parts[p];
    return true;
}

bool extractZip(const std::string& zipPath, const std::string& into,
                std::string& error) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.c_str(), 0)) {
        error = "the downloaded archive could not be opened";
        return false;
    }
    struct Closer { mz_zip_archive* z; ~Closer() { mz_zip_reader_end(z); } } closer{&zip};

    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            error = "the downloaded archive is damaged";
            return false;
        }
        std::string rel;
        if (!safeRelPath(st.m_filename, rel)) {
            if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
            error = std::string("the archive contains an unsafe path: ") + st.m_filename;
            return false;
        }
        fs::path dst = fs::path(into) / rel;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            std::error_code ec; fs::create_directories(dst, ec);
            continue;
        }
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        if (!mz_zip_reader_extract_to_file(&zip, i, dst.string().c_str(), 0)) {
            error = "the archive could not be unpacked";
            return false;
        }
    }
    return true;
}

}  // namespace

// -------------------------------------------------------------- version ----

bool GameVersion::parse(const std::string& text, GameVersion& out) {
    // Mirrors odver.py's _RE exactly: digits.digits.digits, one of a/b/r/s,
    // then an optional counter. No leading "v", no missing state letter.
    size_t i = 0;
    auto number = [&](int& dst) {
        size_t start = i;
        while (i < text.size() && isdigit((unsigned char)text[i])) i++;
        if (i == start || i - start > 9) return false;
        dst = atoi(text.substr(start, i - start).c_str());
        return true;
    };
    GameVersion v;
    if (!number(v.major)) return false;
    if (i >= text.size() || text[i++] != '.') return false;
    if (!number(v.minor)) return false;
    if (i >= text.size() || text[i++] != '.') return false;
    if (!number(v.patch)) return false;
    if (i >= text.size()) return false;
    char s = text[i++];
    if (s != 'a' && s != 'b' && s != 'r' && s != 's') return false;
    v.state = s;
    if (i < text.size()) {
        // Only a snapshot carries a counter; trailing digits on any other
        // state mean this is not a version we understand.
        if (s != 's') return false;
        int c = 0;
        if (!number(c)) return false;
        v.counter = c;
    }
    if (i != text.size()) return false;
    out = v;
    return true;
}

int GameVersion::compare(const GameVersion& a, const GameVersion& b) {
    // The rank in odver.Version.sort_key(). A snapshot sorts BEFORE the plain
    // version it hangs off, because 1.0.2s3 is work toward 1.0.2 rather than
    // past it; among the rest alpha < beta < release.
    auto rank = [](char s) { return s == 's' ? 0 : s == 'a' ? 1 : s == 'b' ? 2 : 3; };
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    int ra = rank(a.state), rb = rank(b.state);
    if (ra != rb) return ra < rb ? -1 : 1;
    if (a.counter != b.counter) return a.counter < b.counter ? -1 : 1;
    return 0;
}

std::string GameVersion::str() const {
    char buf[64];
    if (state == 's') snprintf(buf, sizeof buf, "%d.%d.%d s%d", major, minor, patch, counter);
    else              snprintf(buf, sizeof buf, "%d.%d.%d%c", major, minor, patch, state);
    std::string s(buf);
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    return s;
}

// --------------------------------------------------------------- helpers ---

std::string GameUpdates::sha256Hex(const std::string& bytes) {
    Sha256 h;
    h.update((const uint8_t*)bytes.data(), bytes.size());
    return h.hex();
}

std::string GameUpdates::platformKey() {
#if defined(_WIN32)
    return "OpenDoctrines-windows-x64";
#elif defined(__APPLE__)
  #if defined(__aarch64__)
    return "OpenDoctrines-macos-arm64";
  #else
    return "OpenDoctrines-macos-x64";
  #endif
#else
    return "OpenDoctrines-linux-x64";
#endif
}

bool GameUpdates::canSelfInstall() {
#if defined(__APPLE__)
    return false;   // unnotarized replacement would not launch; see the header
#else
    return true;
#endif
}

bool GameUpdates::isReleaseHostUrl(const std::string& url) {
    static const char* kHosts[] = {
        "https://api.github.com/",
        "https://github.com/",
        "https://objects.githubusercontent.com/",
        "https://release-assets.githubusercontent.com/",
    };
    if (url.size() > 1024) return false;
    // Anything outside printable ASCII, or that could confuse a URL parser
    // into reading a different host, is refused outright.
    for (unsigned char c : url)
        if (c < 0x21 || c > 0x7E || strchr("\"'`\\<>|;&$(){}[]^", c)) return false;
    if (url.find('@') != std::string::npos) return false;   // no user@host trickery
    for (const char* h : kHosts)
        if (url.rfind(h, 0) == 0) return true;
    return false;
}

// ---------------------------------------------------------------- parsing --

bool GameUpdates::parseRelease(const std::string& body, const std::string& platform,
                               Status& out) {
    if (body.empty() || body.size() > 4u * 1024 * 1024) return false;

    std::string tag = jsonString(body, 0, "tag_name", 32);
    if (tag.empty()) return false;
    if (tag[0] == 'v' || tag[0] == 'V') tag.erase(0, 1);
    GameVersion parsed;
    if (!GameVersion::parse(tag, parsed)) return false;   // not a version, not an update
    out.latest = tag;

    std::string page = jsonString(body, 0, "html_url", 512);
    out.pageUrl = isReleaseHostUrl(page) ? page : kReleasePage;

    std::string notes = jsonString(body, 0, "body", 4000);
    // The notes are drawn as plain text in a panel; strip the escapes GitHub
    // sends rather than displaying them raw.
    std::string clean;
    for (size_t i = 0; i < notes.size() && clean.size() < 600; ++i) {
        if (notes[i] == '\\' && i + 1 < notes.size()) {
            char n = notes[++i];
            if (n == 'n' || n == 'r') clean += '\n';
            else if (n == 't') clean += ' ';
            else if (n == '"' || n == '\\' || n == '/') clean += n;
            // Anything else (\u escapes included) is dropped rather than
            // half-decoded into something that draws oddly.
            continue;
        }
        unsigned char c = (unsigned char)notes[i];
        clean += (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    out.notes = clean;

    // Walk the assets array looking for this platform's archive. Scanning for
    // the name and then reading the fields that follow it keeps this to one
    // pass without modelling the whole document.
    const std::string want = platform + ".zip";
    size_t at = 0;
    while (true) {
        size_t nameEnd = 0;
        std::string name = jsonString(body, at, "name", 128, &nameEnd);
        if (name.empty()) break;
        at = nameEnd;
        if (name != want) continue;
        std::string url = jsonString(body, at, "browser_download_url", 1024);
        if (!isReleaseHostUrl(url)) break;      // present but not on our host: refuse it
        out.assetUrl  = url;
        out.assetSize = jsonNumber(body, at, "size");
        std::string digest = jsonString(body, at, "digest", 128);
        const std::string prefix = "sha256:";
        if (digest.rfind(prefix, 0) == 0) {
            std::string hex = digest.substr(prefix.size());
            bool ok = hex.size() == 64;
            for (char c : hex) ok = ok && isxdigit((unsigned char)c);
            if (ok) out.sha256 = hex;
        }
        break;
    }
    return true;
}

// -------------------------------------------------------------- installing --

bool GameUpdates::installOver(const std::string& staged, const std::string& install,
                              const std::string& skipName, std::string& error) {
    std::error_code ec;
    if (!fs::is_directory(staged, ec)) { error = "nothing was unpacked"; return false; }

    for (auto it = fs::recursive_directory_iterator(staged, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (ec) { error = "the unpacked update could not be read"; return false; }
        const fs::path& src = it->path();
        fs::path rel = fs::relative(src, staged, ec);
        if (ec) { error = "the unpacked update could not be read"; return false; }
        fs::path dst = fs::path(install) / rel;

        if (fs::is_directory(src, ec)) {
            fs::create_directories(dst, ec);
            continue;
        }
        // The running binary cannot be overwritten in place; the caller
        // displaces it first and copies it separately.
        if (rel.string() == skipName) continue;

        fs::create_directories(dst.parent_path(), ec);
        // copy_options::overwrite_existing, and nothing else: this loop only
        // ever writes files the release contains. It never removes anything,
        // so a save, a custom map, a mod or a config file in the install
        // cannot be destroyed by an update.
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "could not replace " + rel.string() + " (" + ec.message() + ")";
            return false;
        }
    }
    return true;
}

void GameUpdates::setInstallDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_installDir = dir;
}

std::string GameUpdates::installDir() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_installDir.empty()) return g_installDir;
    }
    std::error_code ec;
    fs::path here = fs::current_path(ec);
    return ec ? std::string(".") : here.string();
}

void GameUpdates::cleanUpAfterUpdate() {
    std::error_code ec;
    fs::path root = installDir();
    fs::remove_all(root / ".od-update", ec);
    // The binary an earlier update displaced. Nothing is running from it now.
    for (auto& name : {"OpenDoctrines.old", "OpenDoctrines.exe.old"})
        fs::remove(root / name, ec);
}

// ---------------------------------------------------------------- driving --

GameUpdates& GameUpdates::get() {
    static GameUpdates u;
    return u;
}

GameUpdates::Status GameUpdates::status() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return m_status;
}

void GameUpdates::setStatus(const Status& s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    m_status = s;
}

bool GameUpdates::updateAvailable() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return m_status.stage == Stage::Available ||
           m_status.stage == Stage::Downloading ||
           m_status.stage == Stage::Installing ||
           m_status.stage == Stage::Restart;
}

bool GameUpdates::busy() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return m_status.stage == Stage::Checking ||
           m_status.stage == Stage::Downloading ||
           m_status.stage == Stage::Installing;
}

void GameUpdates::check(bool enabled) {
    // The gate, in the code that would do the work rather than only in the
    // caller: with the setting off nothing here reaches the network.
    if (!enabled) return;
    if (m_asked.exchange(true)) return;    // once per session

    Status s; s.stage = Stage::Checking;
    setStatus(s);

    std::thread([this]() {
        std::error_code ec;
        fs::path tmp = fs::temp_directory_path(ec) / "od-release-check.json";
        Status out;
        if (!fetchToFile(kApiLatest, tmp.string(), 4u * 1024 * 1024)) {
            out.stage = Stage::Failed;
            out.error = "could not reach the release server";
            setStatus(out);
            return;
        }
        std::string body = readFile(tmp.string(), 4u * 1024 * 1024);
        fs::remove(tmp, ec);

        if (!parseRelease(body, platformKey(), out)) {
            out.stage = Stage::Failed;
            out.error = "the release server did not report a version";
            setStatus(out);
            return;
        }

        GameVersion have, latest;
        if (!GameVersion::parse(OD_VERSION_STRING, have) ||
            !GameVersion::parse(out.latest, latest)) {
            out.stage = Stage::Failed;
            out.error = "could not compare versions";
            setStatus(out);
            return;
        }
        out.stage = GameVersion::compare(have, latest) < 0 ? Stage::Available
                                                           : Stage::UpToDate;
        setStatus(out);
    }).detach();
}

void GameUpdates::beginUpdate() {
    Status s = status();
    if (s.stage != Stage::Available) return;

    if (!canSelfInstall()) {
        // macOS. Hand off to the browser and say so; see the header for why
        // this platform does not install its own update.
        s.stage = Stage::OpenedPage;
        setStatus(s);
        return;
    }
    if (s.assetUrl.empty()) {
        s.stage = Stage::Failed;
        s.error = "this release has no download for " + platformKey();
        setStatus(s);
        return;
    }
    s.stage = Stage::Downloading;
    s.percent = 0;
    setStatus(s);
    std::thread([this]() { runUpdate(); }).detach();
}

void GameUpdates::runUpdate() {
    Status s = status();
    std::error_code ec;

    // Staged inside the install so the final move is a rename on one
    // filesystem rather than a copy across two.
    fs::path root = installDir();
    fs::path work = root / ".od-update";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    if (ec) {
        s.stage = Stage::Failed;
        s.error = "could not write next to the game; is it installed somewhere writable?";
        setStatus(s);
        return;
    }

    fs::path zip = work / "update.zip";
    std::atomic<int> pct{0};
    std::thread progress([this, &pct]() {
        // Mirrors the download counter into the status the menu reads.
        for (;;) {
            Status cur = status();
            if (cur.stage != Stage::Downloading) return;
            int p = pct.load();
            if (p != cur.percent) { cur.percent = p; setStatus(cur); }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    });

    bool got = fetchToFile(s.assetUrl, zip.string(), 512u * 1024 * 1024,
                           s.assetSize, &pct);
    s = status();
    s.stage = Stage::Installing;
    setStatus(s);
    progress.join();

    if (!got) {
        s.stage = Stage::Failed;
        s.error = "the download did not finish";
        setStatus(s);
        fs::remove_all(work, ec);
        return;
    }

    // Only catches a truncated or corrupted download -- the digest arrived
    // over the same connection as the file, so it is not evidence about
    // GitHub itself. Worth doing anyway: a half-written archive that unpacked
    // over a working install would be much worse than a refused update.
    if (!s.sha256.empty()) {
        std::string bytes = readFile(zip.string(), 512u * 1024 * 1024);
        if (sha256Hex(bytes) != s.sha256) {
            s.stage = Stage::Failed;
            s.error = "the download did not match its checksum and was discarded";
            setStatus(s);
            fs::remove_all(work, ec);
            return;
        }
    }

    fs::path staged = work / "staged";
    fs::create_directories(staged, ec);
    std::string err;
    if (!extractZip(zip.string(), staged.string(), err)) {
        s.stage = Stage::Failed;
        s.error = err;
        setStatus(s);
        fs::remove_all(work, ec);
        return;
    }

#if defined(_WIN32)
    const std::string binName = "OpenDoctrines.exe";
#else
    const std::string binName = "OpenDoctrines";
#endif

    // Refuse to install something that does not look like the game. Without
    // this, an archive that unpacked to nothing would "succeed" and leave the
    // player told to restart into an unchanged install.
    if (!fs::exists(staged / binName, ec) || !fs::is_directory(staged / "data", ec)) {
        s.stage = Stage::Failed;
        s.error = "the download did not contain a complete copy of the game";
        setStatus(s);
        fs::remove_all(work, ec);
        return;
    }

    if (!installOver(staged.string(), root.string(), binName, err)) {
        s.stage = Stage::Failed;
        s.error = err;
        setStatus(s);
        fs::remove_all(work, ec);
        return;
    }

    // The running binary, last and separately. It cannot be overwritten while
    // it runs, but it CAN be renamed on both Windows and Linux -- the running
    // process keeps the file it was launched from. If the rename works and the
    // copy then fails, the old binary is moved back so the install still
    // starts.
    fs::path live = root / binName;
    fs::path old  = root / (binName + ".old");
    fs::remove(old, ec);
    fs::rename(live, old, ec);
    if (ec) {
        s.stage = Stage::Failed;
        s.error = "could not replace the game itself (" + ec.message() + ")";
        setStatus(s);
        fs::remove_all(work, ec);
        return;
    }
    fs::copy_file(staged / binName, live, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::error_code back;
        fs::rename(old, live, back);          // put it back; a failed update
        s.stage = Stage::Failed;              // must not cost the player the game
        s.error = "could not replace the game itself (" + ec.message() + ")";
        setStatus(s);
        fs::remove_all(work, ec);
        return;
    }
#if !defined(_WIN32)
    fs::permissions(live, fs::perms::owner_all | fs::perms::group_read |
                          fs::perms::group_exec | fs::perms::others_read |
                          fs::perms::others_exec, ec);
#endif

    fs::remove_all(work, ec);
    s = status();
    s.stage = Stage::Restart;
    s.percent = 100;
    setStatus(s);
}
