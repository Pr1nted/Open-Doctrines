#include "Runner.h"

#include "../net/HttpClient.h"
#include "../net/ToolRelease.h"
#include "../util/RunCurl.h"
#include "../util/Sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  #include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace llm {

namespace {

// THE ONLY PLACE A RUNNER CAN COME FROM. Hard-coded, https, and no part of it
// is ever read out of a reply -- a URL taken from downloaded JSON is a URL
// somebody else chose. Same shape as the cloudflared installer, and the checks
// in between are literally the same code: see net/ToolRelease.h.
const char* kReleaseApi = "https://api.github.com/repos/ollama/ollama/releases/latest";
const char* kAllowedAssetHost = "https://github.com/ollama/ollama/releases/download/";

/**
 * Which asset this platform gets, and whether we offer to fetch it at all.
 *
 * WE OFFER THIS ON macOS AND NOWHERE ELSE, and that is a judgement rather than
 * an oversight. Ollama's macOS build is a 159 MB plain .tgz that `tar -xzf`
 * unpacks on any machine. Its Linux and Windows builds are 1.4 GB and bundle
 * CUDA and ROCm runtimes, in .tar.zst and .zip -- so an in-game installer there
 * would mean a gigabyte and a half over a game's progress bar, plus a
 * dependency on zstd or an unzip that may not be present, to arrive at a worse
 * result than the official installer gives in one line.
 *
 * Where we do not offer it, the screen says so and gives that line. A refusal
 * that explains itself is better than a button that downloads 1.4 GB and then
 * fails to unpack.
 */
struct PlatformAsset {
    const char* name = nullptr;      ///< null means "not offered here"
    const char* binaryInArchive = nullptr;
    long long   maxBytes = 0;
    const char* manualHint = nullptr;
};

PlatformAsset platformAsset() {
#if defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    // 159 MB today; the ceiling leaves room to grow without accepting a
    // release that is obviously not this.
    return {"ollama-darwin.tgz", "ollama", 400LL * 1024 * 1024, nullptr};
#elif defined(__linux__)
    return {nullptr, nullptr, 0,
            "Install it with:  curl -fsSL https://ollama.com/install.sh | sh"};
#elif defined(_WIN32)
    return {nullptr, nullptr, 0,
            "Download the installer from https://ollama.com/download"};
#else
    return {nullptr, nullptr, 0, "See https://ollama.com/download"};
#endif
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

}  // namespace

bool canInstall() { return platformAsset().name != nullptr; }

const char* manualInstructions() {
    const PlatformAsset a = platformAsset();
    return a.manualHint ? a.manualHint : "See https://ollama.com/download";
}

std::string installDir(const std::string& dataDir) {
    std::string d = dataDir;
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d + "llm";
}

std::string installedPath(const std::string& dataDir) {
    std::error_code ec;
    const fs::path p = fs::path(installDir(dataDir)) / "ollama";
    return fs::exists(p, ec) ? p.string() : std::string();
}

bool installed(const std::string& dataDir) {
    return !installedPath(dataDir).empty();
}

bool uninstall(const std::string& dataDir) {
    std::error_code ec;
    const fs::path dir = installDir(dataDir);
    if (!fs::exists(dir, ec)) return true;
    fs::remove_all(dir, ec);
    if (ec) {
        // Reported rather than swallowed: claiming a clean uninstall that did
        // not happen leaves an executable on disk the player believes is gone,
        // which is the one outcome this must never produce.
        fprintf(stderr, "[LLM] could not remove %s: %s\n",
                dir.string().c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

std::string sha256Hex(const std::vector<unsigned char>& bytes) {
    return ::sha256Hex(std::string(bytes.begin(), bytes.end()));
}

std::string describeDownload() {
    const PlatformAsset a = platformAsset();
    if (!a.name) return manualInstructions();
    return std::string("Ollama's own release of ") + a.name + ", from " +
           kAllowedAssetHost + ", checked against the checksum GitHub publishes "
           "for it before anything is run.";
}

/**
 * Is this machine online at all?
 *
 * Asked only to CHOOSE THE MESSAGE, never to gate the attempt: a probe that
 * decides whether to try would turn its own false negative -- a proxy, a
 * captive portal, a blocked HEAD -- into a refusal to install on a machine
 * that is perfectly able to. So the download runs regardless, and this only
 * decides whether the failure is reported as "no internet" or as "try again".
 *
 * Deliberately short: this runs after something has already failed, and a
 * player waiting on a verdict should not wait another half minute for it.
 */
/// Where to throw a response away, per platform.
const char* nullDevice() {
#if defined(_WIN32)
    return "NUL";
#else
    return "/dev/null";
#endif
}

bool reachable() {
    // CURL, NOT httpRequest, and the difference is the whole point of this
    // function. HttpRequest::timeoutMs bounds the READ loop -- it starts
    // counting once there is a connection to read from -- so a host that
    // blackholes packets waits out the operating system's own connect timeout,
    // about 75 seconds on macOS, whatever the field says. A "is there internet"
    // probe that can take a minute and a quarter to say no is not a probe.
    //
    // (That is a live limitation of net/HttpClient for every caller, not just
    // this one. Not changed here: it is the shared transport behind accounts
    // and multiplayer, and it deserves its own look rather than a drive-by
    // edit from a feature branch.)
    return odproc::runCurl({
        "-fsS",
        "--proto",           "=https",
        "--connect-timeout", "3",
        "--max-time",        "4",
        "-o",                nullDevice(),
        kReleaseApi,
    });
}

Install fetch(const std::string& dataDir, const ProgressFn& progress) {
    Install out;
    const PlatformAsset asset = platformAsset();
    if (!asset.name) {
        out.error = manualInstructions();
        return out;
    }

    auto say = [&](const char* what) { if (progress) progress(what); };

    // 1. Which release, and what is its checksum?
    say("Asking Ollama which version is current...");
    HttpRequest req;
    req.url = kReleaseApi;
    req.maxResponseBytes = 4 * 1024 * 1024;
    req.timeoutMs = 30000;
    const HttpResponse res = httpRequest(req);
    if (!res.ok()) {
        // Said as a connection problem when it is one. "Could not reach
        // Ollama's release list" reads like the service is down, and the
        // commonest cause by far is that this machine is not online.
        out.error = reachable()
            ? (res.error.empty() ? "Could not reach Ollama's release list. "
                                   "It may be temporarily unavailable."
                                 : res.error)
            : "No internet connection. Installing the runner needs one.";
        return out;
    }

    // Every decision about whether this may be downloaded at all is made in one
    // shared, tested place. See net/ToolRelease.h for what it refuses and why.
    const odtool::Recipe recipe{asset.name, kAllowedAssetHost, asset.maxBytes};
    const odtool::Choice choice = odtool::chooseAsset(res.body, recipe, "Ollama's");
    if (!choice.ok()) {
        out.error = choice.refusal;
        return out;
    }

    // 2. Fetch it.
    std::error_code ec;
    const fs::path dir = installDir(dataDir);
    fs::create_directories(dir, ec);
    const fs::path staged = dir / "ollama.download";
    fs::remove(staged, ec);

    say("Downloading Ollama...");
    {
        // curl, and argv rather than a shell string, for the reasons written
        // out in TunnelInstall.cpp: a release URL redirects to a CDN, so
        // redirects must be followed but ONLY over https, and a URL that came
        // from a reply must never be interpolated into a command line.
        // ── WHY THERE ARE THREE TIMEOUTS AND NOT ONE ──
        //
        // --max-time alone was 1800, and that is the ONLY one that used to
        // exist: on a connection that could not reach the CDN at all, the
        // button said "Installing..." for half an hour and then failed. From
        // the player's side that is indistinguishable from a hang, and there is
        // no cancel.
        //
        //   --connect-timeout  a route that goes nowhere fails in 20s, not 30
        //                      minutes. Broken IPv6 with no working fallback is
        //                      the common case and it presents as a stall.
        //   --speed-time/limit a transfer that STARTS and then dies is not
        //                      covered by connect-timeout at all -- it is a
        //                      live socket delivering nothing. Under 2 KB/s for
        //                      60s is a dead download, not a slow one; the
        //                      asset is 160 MB, so a link genuinely that slow
        //                      would need seven hours anyway.
        //   --max-time         still the backstop for the pathological case.
        const std::vector<std::string> args = {
            "-fsSL",
            "--proto",        "=https",
            "--proto-redir",  "=https",
            "--connect-timeout", "20",
            "--speed-time",   "60",
            "--speed-limit",  "2048",
            "--max-time",     "1800",
            "--max-filesize", std::to_string(asset.maxBytes),
            "-o",             staged.string(),
            choice.url,
        };
        if (!odproc::runCurl(args)) {
            fs::remove(staged, ec);
            out.error = reachable()
                ? "The download did not complete. It may have been interrupted -- try again."
                : "No internet connection. The runner is a 160 MB download and "
                  "cannot be installed offline.";
            return out;
        }
    }

    const std::string payload = readFile(staged.string());
    if (payload.empty() || (long long)payload.size() != choice.size) {
        fs::remove(staged, ec);
        out.error = "The download finished at the wrong size, so it was discarded.";
        return out;
    }

    // 3. VERIFY, BEFORE ANYTHING IS PUT WHERE IT COULD BE RUN.
    say("Checking what was downloaded...");
    if (::sha256Hex(payload) != choice.sha256) {
        // Deleted, not kept. Something that failed its checksum is the one
        // thing that must never end up somewhere it could later be executed.
        fs::remove(staged, ec);
        out.error = "What arrived did not match Ollama's checksum, so it was thrown "
                    "away and nothing was installed.";
        return out;
    }

    // 4. Unpack into our own directory, and nowhere the archive may choose.
    say("Installing...");
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    // THE WHOLE ARCHIVE, not just the binary named in it.
    //
    // This asked for the single member "ollama" at first, which extracted a
    // 68 MB program that could not run anything: the archive is 57 entries and
    // the other 56 are llama-server and the ggml libraries it loads. Ollama
    // started, accepted a pull, and then answered every completion with
    // "llama-server binary not found" -- a failure that looks like a broken
    // model rather than a broken install.
    //
    // Extracting everything is safe here for two reasons that both have to
    // hold: the archive's checksum has already been checked against the digest
    // Ollama published, and tar strips leading slashes and refuses ".." members
    // by default, so a hostile archive still could not write outside -C.
    const std::vector<std::string> tarArgs = {
        "-xzf", staged.string(),
        "-C",   dir.string(),
    };
    if (!odproc::runTool("tar", tarArgs)) {
        fs::remove(staged, ec);
        out.error = "The download could not be unpacked.";
        return out;
    }
#endif
    fs::remove(staged, ec);

    const fs::path finalPath = dir / "ollama";
    if (!fs::exists(finalPath, ec)) {
        out.error = "The download finished but the program was not where it was "
                    "expected. Nothing was installed.";
        return out;
    }
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    ::chmod(finalPath.string().c_str(), 0755);
#endif

    out.path = finalPath.string();
    return out;
}

// ─────────────────────────────────────────────────── pulling a model ────

namespace {

// Sizes are Ollama's registry's own figures, read from the manifests rather
// than remembered. Licences differ and are named because pulling one is
// accepting it, and the player should see which before they press.
constexpr Model kModels[] = {
    {"gemma3:4b",  "Gemma 3 4B",  "3.3 GB", "Gemma Terms of Use",
     "A good default. Strong in many languages, runs on most machines."},
    {"gemma3:1b",  "Gemma 3 1B",  "0.8 GB", "Gemma Terms of Use",
     "For a machine that struggles with the above. Blunter letters."},
    {"qwen2.5:7b", "Qwen 2.5 7B", "4.7 GB", "Apache 2.0",
     "Better prose, and a permissive licence. Wants more memory."},
    {"llama3.1:8b", "Llama 3.1 8B", "4.9 GB", "Llama 3.1 Community License",
     "Widely used. Weaker outside English than the other two."},
};

}  // namespace

const Model* offeredModels(int* count) {
    if (count) *count = (int)(sizeof(kModels) / sizeof(kModels[0]));
    return kModels;
}

std::string apiRootOf(const std::string& endpoint) {
    std::string s = endpoint;
    while (!s.empty() && s.back() == '/') s.pop_back();
    // The configured endpoint is the OpenAI-compatible surface; pulling lives
    // on Ollama's own root, one level up.
    const std::string suffix = "/v1";
    if (s.size() >= suffix.size() &&
        s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0) {
        s.resize(s.size() - suffix.size());
    }
    return s;
}

bool pullModel(const std::string& apiBase, const std::string& model,
               const std::string& streamFile) {
    std::error_code ec;
    fs::remove(streamFile, ec);

    // The model name goes in a JSON body as a curl argument, never into a shell
    // string. It is typed by the player, so it is exactly the sort of value
    // that must not be able to become part of a command.
    std::string escaped;
    for (char c : model) {
        if (c == '"' || c == '\\') escaped += '\\';
        if ((unsigned char)c >= 0x20) escaped += c;
    }
    const std::string body = "{\"model\":\"" + escaped + "\",\"stream\":true}";

    const std::vector<std::string> args = {
        "-fsS",
        // Loopback by default, so http is expected here -- but if somebody has
        // pointed this at a remote Ollama, redirects must still not downgrade.
        "--proto-redir", "=https,http",
        // This one talks to the LOCAL runner, so a connect failure means the
        // runner is not up -- worth failing in seconds rather than 90 minutes.
        // No --speed-limit here: the stream is progress lines, not the weights,
        // and it is legitimately silent for long stretches while a layer
        // downloads. Ollama's own error text is what names a network failure,
        // and it is surfaced verbatim.
        "--connect-timeout", "10",
        "--max-time",    "5400",          // a 5 GB pull on a slow line
        "-H",            "content-type: application/json",
        "-d",            body,
        "-o",            streamFile,
        apiBase + "/api/pull",
    };
    return odproc::runCurl(args);
}

std::string localEndpoint() { return "http://127.0.0.1:11434/v1"; }

long long startServer(const std::string& dataDir) {
    const std::string exe = installedPath(dataDir);
    if (exe.empty()) return 0;
    // OLLAMA_HOST is how ollama is told where to listen; loopback only.
#if defined(_WIN32)
    _putenv_s("OLLAMA_HOST", "127.0.0.1:11434");
#else
    setenv("OLLAMA_HOST", "127.0.0.1:11434", 1);
#endif
    // And its models go in the game's own folder, so "Remove it" really does
    // leave nothing behind -- otherwise a pulled model sits in the player's
    // home directory forever and the game never mentions it.
    const std::string models = installDir(dataDir) + "/models";
#if defined(_WIN32)
    _putenv_s("OLLAMA_MODELS", models.c_str());
#else
    setenv("OLLAMA_MODELS", models.c_str(), 1);
#endif
    return odproc::startDetached(exe, {"serve"});
}

bool stopServer(long long pid) { return odproc::stopDetached(pid); }
bool serverAlive(long long pid) { return odproc::detachedAlive(pid); }

PullProgress pullProgress(const std::string& streamFile) {
    PullProgress out;
    const std::string text = readFile(streamFile);
    if (text.empty()) return out;

    // Ollama streams one JSON object per line. The last COMPLETE line is the
    // current state; a partial trailing line is what a file being written to
    // looks like, and parsing it would produce nonsense every other frame.
    size_t end = text.find_last_of('\n');
    if (end == std::string::npos) return out;
    size_t start = text.find_last_of('\n', end - 1);
    start = (start == std::string::npos) ? 0 : start + 1;
    const std::string line = text.substr(start, end - start);
    if (line.empty()) return out;

    out.status = httpJsonString(line, "status", 200);
    out.completed = httpJsonNumber(line, "completed", 0);
    out.total = httpJsonNumber(line, "total", 0);
    const std::string err = httpJsonString(line, "error", 300);
    if (!err.empty()) out.error = err;
    if (out.status == "success") out.done = true;
    return out;
}

}  // namespace llm
