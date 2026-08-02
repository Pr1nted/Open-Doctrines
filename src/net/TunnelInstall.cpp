// Fetching cloudflared, verified, on request. See the notes in Tunnel.h --
// this is the most dangerous thing the game does and the comments there are
// the reasoning, not decoration.

#include "Tunnel.h"

#include "HttpClient.h"
#include "util/Sha256.h"
#include "util/RunCurl.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

// THE ONLY PLACE A DOWNLOAD CAN COME FROM. Hard-coded, https, and no part of
// it is ever taken from a reply -- a URL read out of downloaded JSON is a URL
// somebody else chose.
const char* kReleaseApi =
    "https://api.github.com/repos/cloudflare/cloudflared/releases/latest";
const char* kAllowedAssetHost = "https://github.com/cloudflare/cloudflared/releases/download/";

/** A cloudflared release is tens of megabytes; anything larger is not one. */
constexpr long long kMaxAssetBytes = 120LL * 1024 * 1024;

/** The asset this platform needs, and whether it arrives inside an archive. */
struct PlatformAsset {
    const char* name = nullptr;
    bool        archived = false;   // .tgz that must be unpacked
};

PlatformAsset platformAsset() {
#if defined(__APPLE__)
  #if defined(__aarch64__) || defined(__arm64__)
    return {"cloudflared-darwin-arm64.tgz", true};
  #else
    return {"cloudflared-darwin-amd64.tgz", true};
  #endif
#elif defined(__linux__)
  #if defined(__aarch64__)
    return {"cloudflared-linux-arm64", false};
  #else
    return {"cloudflared-linux-amd64", false};
  #endif
#elif defined(_WIN32)
    return {"cloudflared-windows-amd64.exe", false};
#else
    return {nullptr, false};
#endif
}

const char* binaryName() {
#ifdef _WIN32
    return "cloudflared.exe";
#else
    return "cloudflared";
#endif
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

struct TunnelInstaller::Impl {
    mutable std::mutex  mutex;
    TunnelInstallStatus status;
    std::thread         worker;
    std::atomic<bool>   running{false};

    void set(TunnelInstallStatus::Phase p, const std::string& msg, int pct = -1,
             long long size = -1) {
        std::lock_guard<std::mutex> lock(mutex);
        status.phase = p;
        status.message = msg;
        if (pct >= 0) status.percent = pct;
        if (size >= 0) status.sizeBytes = size;
    }
};

TunnelInstaller::TunnelInstaller() : m_impl(std::make_unique<Impl>()) {}
TunnelInstaller::~TunnelInstaller() { shutdown(); }

bool TunnelInstaller::supported() { return platformAsset().name != nullptr; }

std::string TunnelInstaller::describe() {
    const PlatformAsset a = platformAsset();
    if (!a.name) {
        return "There is no cloudflared build this game knows how to install for "
               "this system. Installing it by hand still works.";
    }
    return std::string("Downloads ") + a.name + " (around 20-55 MB) from "
           "Cloudflare's official releases on GitHub, checks it against the "
           "SHA-256 GitHub publishes for it, and puts it beside the game's data. "
           "Nothing is installed system-wide and no administrator rights are "
           "asked for. It is only ever run to open a tunnel you asked for.";
}

std::string TunnelInstaller::installedPath(const std::string& toolsDir) {
    const fs::path p = fs::path(toolsDir) / binaryName();
    std::error_code ec;
    if (fs::exists(p, ec)) return p.string();
    return {};
}

TunnelInstallStatus TunnelInstaller::status() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->status;
}

void TunnelInstaller::shutdown() {
    if (m_impl && m_impl->worker.joinable()) m_impl->worker.join();
}

bool TunnelInstaller::begin(const std::string& toolsDir) {
    if (m_impl->running.load()) return false;
    if (!supported()) {
        m_impl->set(TunnelInstallStatus::Phase::Failed, describe());
        return false;
    }
    if (m_impl->worker.joinable()) m_impl->worker.join();

    m_impl->running.store(true);
    m_impl->set(TunnelInstallStatus::Phase::Asking,
                "Asking Cloudflare which version is current...", 0, 0);

    Impl* impl = m_impl.get();
    m_impl->worker = std::thread([impl, toolsDir] {
        const PlatformAsset asset = platformAsset();

        // 1. Which release, and what is its digest? Over https to a fixed host.
        HttpRequest req;
        req.url = kReleaseApi;
        req.maxResponseBytes = 4 * 1024 * 1024;
        req.timeoutMs = 30000;
        const HttpResponse res = httpRequest(req);
        if (!res.ok()) {
            impl->set(TunnelInstallStatus::Phase::Failed,
                      res.error.empty() ? "Could not reach Cloudflare's release list."
                                        : res.error);
            impl->running.store(false);
            return;
        }

        // Scoped to the entry whose name matches this platform, so the digest
        // and the URL are read from the SAME asset rather than the first of
        // each that happens to appear.
        const std::string body = res.body;
        const std::string needle = std::string("\"name\":\"") + asset.name + "\"";
        const size_t at = body.find(needle);
        if (at == std::string::npos) {
            impl->set(TunnelInstallStatus::Phase::Failed,
                      std::string("Cloudflare's current release has no ") + asset.name +
                          ". Installing it by hand still works.");
            impl->running.store(false);
            return;
        }
        const size_t end = body.find("browser_download_url", at);
        const std::string scope = body.substr(at, end == std::string::npos
                                                  ? 2048 : end - at + 256);

        const std::string digest = httpJsonString(scope, "digest", 128);
        const long long size = httpJsonNumber(scope, "size", 0);
        const std::string url = httpJsonString(scope, "browser_download_url", 512);

        // The digest is what makes this safe; without one there is nothing to
        // check the bytes against and the download must not happen at all.
        if (digest.rfind("sha256:", 0) != 0 || digest.size() != 71) {
            impl->set(TunnelInstallStatus::Phase::Failed,
                      "Cloudflare's release did not come with a checksum, so this "
                      "download cannot be verified and will not be run. Install "
                      "cloudflared by hand instead.");
            impl->running.store(false);
            return;
        }
        // And the URL must be on the one host this is allowed to fetch from,
        // no matter what the reply said.
        if (url.rfind(kAllowedAssetHost, 0) != 0) {
            impl->set(TunnelInstallStatus::Phase::Failed,
                      "That release points somewhere unexpected, so nothing was "
                      "downloaded.");
            impl->running.store(false);
            return;
        }
        if (size <= 0 || size > kMaxAssetBytes) {
            impl->set(TunnelInstallStatus::Phase::Failed,
                      "That release is an unexpected size, so nothing was downloaded.");
            impl->running.store(false);
            return;
        }

        // 2. Fetch it.
        std::error_code ec;
        fs::create_directories(toolsDir, ec);
        const fs::path staged = fs::path(toolsDir) / "cloudflared.download";
        fs::remove(staged, ec);

        impl->set(TunnelInstallStatus::Phase::Downloading,
                  "Downloading cloudflared from Cloudflare...", 0, size);

        // curl, not HttpClient: a GitHub release URL redirects to a CDN, and
        // HttpClient refuses redirects on purpose (it carries bearer tokens and
        // following one would hand them to a host the caller never named).
        // Nothing here carries a credential, so following is safe -- but only
        // over https, both for the first hop and every redirect after it, which
        // is what --proto and --proto-redir pin down. This is the same approach
        // the game's own updater uses to fetch its releases.
        {
            // Separate argv entries, never a shell string. This was
            // std::system() with single-quoted arguments, which is wrong twice:
            // single quotes do not quote in cmd.exe -- they are ordinary
            // characters -- so it could never have worked on Windows at all,
            // and `url` comes from GitHub's API, so interpolating it into a
            // shell command is the shape of a command injection. The host
            // prefix is checked above, but that check does not reject quotes.
            //
            // odproc::runCurl is the same helper the game updater uses, which
            // is where this approach was already written down.
            const std::vector<std::string> args = {
                "-fsSL",
                "--proto",       "=https",
                "--proto-redir", "=https",
                "--max-time",    "300",
                "--max-filesize", std::to_string(kMaxAssetBytes),
                "-o",            staged.string(),
                url,
            };
            if (!odproc::runCurl(args)) {
                fs::remove(staged, ec);
                impl->set(TunnelInstallStatus::Phase::Failed,
                          "The download did not complete. Check your connection and "
                          "try again.");
                impl->running.store(false);
                return;
            }
        }

        const std::string payload = readFile(staged.string());
        if (payload.empty() || (long long)payload.size() != size) {
            fs::remove(staged, ec);
            impl->set(TunnelInstallStatus::Phase::Failed,
                      "The download finished at the wrong size, so it was discarded.");
            impl->running.store(false);
            return;
        }

        // 3. VERIFY, BEFORE ANYTHING IS WRITTEN WHERE IT COULD BE RUN.
        impl->set(TunnelInstallStatus::Phase::Verifying,
                  "Checking what was downloaded...", 100);
        const std::string actual = sha256Hex(payload);
        const std::string expected = digest.substr(7);
        if (actual != expected) {
            // Deleted, not kept "just in case". Something that failed its
            // checksum is the one thing that must never end up somewhere it
            // could later be executed.
            fs::remove(staged, ec);
            impl->set(TunnelInstallStatus::Phase::Failed,
                      "What arrived did not match Cloudflare's checksum, so it was "
                      "thrown away and nothing was installed.");
            impl->running.store(false);
            return;
        }

        // 4. Unpack, if it came in an archive, and put it in place.
        impl->set(TunnelInstallStatus::Phase::Installing, "Installing...");
        const fs::path finalPath = fs::path(toolsDir) / binaryName();
        fs::remove(finalPath, ec);

        if (asset.archived) {
#if !defined(_WIN32)
            // tar is on every macOS and Linux machine. The archive contains a
            // single `cloudflared`, and it is extracted into the tools
            // directory rather than anywhere the archive gets to choose.
            const std::string cmd = "tar -xzf '" + staged.string() + "' -C '" +
                                    std::string(toolsDir) + "' cloudflared 2>/dev/null";
            if (std::system(cmd.c_str()) != 0) {
                impl->set(TunnelInstallStatus::Phase::Failed,
                          "The download could not be unpacked.");
                fs::remove(staged, ec);
                impl->running.store(false);
                return;
            }
#endif
        } else {
            fs::rename(staged, finalPath, ec);
            if (ec) fs::copy_file(staged, finalPath,
                                  fs::copy_options::overwrite_existing, ec);
        }
        fs::remove(staged, ec);

        if (!fs::exists(finalPath, ec)) {
            impl->set(TunnelInstallStatus::Phase::Failed,
                      "The download finished but the program was not where it was "
                      "expected. Nothing was installed.");
            impl->running.store(false);
            return;
        }

        // Guard has to match the one on <sys/stat.h> at the top of this file,
        // not just exclude Windows: emscripten has no chmod to declare, so a
        // call site that only checked _WIN32 broke the web build and nothing
        // else. There is nothing to make executable in a browser regardless --
        // a tunnel is installed to be RUN, and the web build runs nothing.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        ::chmod(finalPath.string().c_str(), 0755);
#endif

        impl->set(TunnelInstallStatus::Phase::Done,
                  "cloudflared is installed. Hosting will use it from now on.", 100);
        impl->running.store(false);
    });
    return true;
}
