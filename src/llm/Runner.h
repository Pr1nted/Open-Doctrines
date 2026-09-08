#pragma once

// Getting a language model runner onto the player's machine, and off it again.
//
// WHAT THIS DOES
//
// Fetches Ollama's own release, checks it against the checksum GitHub publishes
// beside it, unpacks it under the game's data directory, and points the module
// at it. "Remove it" deletes that directory and leaves nothing behind.
//
// THIS DOWNLOADS AND RUNS A THIRD-PARTY BINARY, WHICH IS THE WHOLE PROBLEM
//
// It is the same shape as the cloudflared installer this game already has --
// deliberately, because that is the audited one. The checks that decide whether
// a download may happen are literally the same code: see net/ToolRelease.h.
//
//   1. ONE HOST, HARD-CODED. The release API and the asset host are written
//      here, and no part of either is ever read out of a reply. A URL taken
//      from downloaded JSON is a URL somebody else chose.
//   2. A CHECKSUM, OR NOTHING. The asset is matched by name and its digest read
//      from ITS OWN entry -- pairing one asset's checksum with another's URL is
//      a verification that always passes. Anything that fails is deleted, not
//      kept.
//   3. VERIFIED BEFORE IT IS PUT ANYWHERE IT COULD RUN, not after.
//   4. NOTHING WITHOUT A CLEAR YES. describeDownload() is shown first. A game
//      that quietly fetches an executable has acted on the player's behalf in a
//      way they never agreed to.
//   5. UNDER THE GAME'S OWN DIRECTORY. Not /usr/local, not the PATH, not a
//      system service. Uninstalling is deleting a folder, which is a promise
//      that can actually be kept.
//
// WHAT IT DOES NOT PROTECT AGAINST, SAID PLAINLY
//
// The checksum comes from the same service as the file. That defeats a
// corrupted transfer, a hostile mirror and a tampering CDN; it does not defeat
// GitHub serving a bad release. The alternative -- a hash compiled into the
// game -- covers that too but must be re-pinned by hand for every release, and
// a check nobody updates is a check that gets switched off.
//
// PULLING A MODEL IS A DIFFERENT AND SAFER THING
//
// The game can also fetch the weights, and that is much less dangerous than
// fetching the runner was: Ollama verifies every layer's digest itself, and a
// model is DATA -- nothing here ever executes it. So this does not repeat the
// checksum machinery; it asks Ollama to pull, and Ollama does the verifying.
//
// What it will not do is choose for you silently. Every model has a licence,
// and they differ -- Apache 2.0 on some, bespoke terms on others -- so the
// screen names the licence beside each one and pulling is a deliberate press.
// Sizes are quoted from Ollama's own registry rather than from memory.

#include <functional>
#include <string>
#include <vector>

namespace llm {

/** What came of an install attempt. `path` is empty when `error` is set. */
struct Install {
    std::string path;
    std::string error;
    bool ok() const { return !path.empty() && error.empty(); }
};

/** Told what is happening, so a 159 MB download is not a frozen screen. */
using ProgressFn = std::function<void(const char*)>;

/**
 * Whether this platform is one we offer to install on.
 *
 * macOS only, and deliberately. Ollama's macOS build is a 159 MB plain .tgz
 * that `tar -xzf` handles anywhere; its Linux and Windows builds are 1.4 GB,
 * bundle CUDA and ROCm runtimes, and come in .tar.zst and .zip. Fetching those
 * through a game would mean a gigabyte and a half and a dependency on zstd, to
 * arrive somewhere worse than the official installer reaches in one line. Where
 * we do not offer it, manualInstructions() says what to run instead.
 */
bool canInstall();

/** The one line to run where we do not offer to do it. */
const char* manualInstructions();

/** What the player is told BEFORE anything is fetched: what, and from where. */
std::string describeDownload();

/// Where an installed runner lives: <dataDir>/llm/. Nothing is written outside.
std::string installDir(const std::string& dataDir);

/// The installed binary, or empty.
std::string installedPath(const std::string& dataDir);

/// Whether a runner is installed and ready to start.
bool installed(const std::string& dataDir);

/**
 * Remove it. Deletes that directory and nothing else.
 *
 * False only if something could not be removed, in which case the module keeps
 * reporting itself as installed rather than claiming an uninstall it did not
 * perform.
 */
bool uninstall(const std::string& dataDir);

/**
 * Download, verify and unpack. BLOCKS -- call it off the game thread.
 *
 * Every decision about whether the download may happen at all is made by
 * odtool::chooseAsset, shared with the cloudflared installer and tested in
 * tests/tool_release_test.cpp. Nothing that fails its checksum is kept.
 */
Install fetch(const std::string& dataDir, const ProgressFn& progress);

/** One model the game offers to pull, with what it costs and what it is under. */
struct Model {
    const char* name;      ///< as Ollama names it, e.g. "gemma3:4b"
    const char* label;
    const char* size;      ///< from Ollama's registry, not from memory
    const char* licence;
    const char* note;
};

/// The shortlist. Any other model can still be typed in by hand.
const Model* offeredModels(int* count);

/**
 * Ask a running Ollama to pull a model, writing its progress stream to a file.
 *
 * BLOCKS -- call it off the game thread. `apiBase` is Ollama's own root (the
 * endpoint with any trailing "/v1" removed), because pulling is not part of the
 * OpenAI-compatible surface.
 *
 * Progress is read back from `streamFile` by pullProgress() rather than through
 * a callback: curl writes the stream to a file, which is the same shape the
 * cloudflared download already uses and needs no pipe plumbing on either
 * platform.
 */
bool pullModel(const std::string& apiBase, const std::string& model,
               const std::string& streamFile);

/** How far a pull has got, read from the file it is streaming into. */
struct PullProgress {
    long long completed = 0;
    long long total = 0;
    std::string status;
    bool done = false;
    std::string error;
};
PullProgress pullProgress(const std::string& streamFile);

/** Ollama's own API root for a configured endpoint: strips a trailing /v1. */
std::string apiRootOf(const std::string& endpoint);

/// Bytes hashed, for tests and for the verification step.
std::string sha256Hex(const std::vector<unsigned char>& bytes);

}  // namespace llm
