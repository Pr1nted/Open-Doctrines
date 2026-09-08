#pragma once

// Picking one asset out of a GitHub release, and refusing to.
//
// WHY THIS IS ITS OWN FILE
//
// Downloading and running a third-party program is the most dangerous thing
// this game does, and the decisions that make it safe are all made here:
//
//   * the asset is matched BY NAME, and the digest and the URL are read from
//     that asset's own entry rather than from the first of each in the reply;
//   * there must be a sha256 digest, in the right shape -- with nothing to
//     check the bytes against, the download must not happen at all;
//   * the URL must be on the one host we are willing to fetch from, whatever
//     the reply says;
//   * the size must be sane.
//
// It was written inline in TunnelInstall.cpp for cloudflared, and a second tool
// (the language-model runner) needs exactly the same decisions. Two copies of
// the most dangerous thing in the codebase is worse than one, and the one that
// was inline could not be tested without a network. Here it is a pure function
// over a string, so tests/tool_release_test.cpp can push every refusal through
// it -- which no test did before.
//
// WHAT IT DOES NOT PROTECT AGAINST, SAID PLAINLY
//
// The digest comes from the same service as the file. That defeats a corrupted
// transfer, a hostile mirror and a tampering CDN; it does not defeat GitHub
// itself serving a bad release. Pinning a hash into the binary would cover that
// too, at the cost of a maintainer re-pinning by hand for every release -- and
// a check nobody updates is a check that gets switched off.

#include <string>

namespace odtool {

/** What to look for, and what may be accepted. */
struct Recipe {
    /** The asset's exact filename in the release, e.g. "ollama-darwin.tgz". */
    std::string assetName;
    /** Every acceptable URL starts with this. Never taken from the reply. */
    std::string allowedAssetHost;
    long long   maxAssetBytes = 0;
};

/** Where to fetch from, once everything has been checked. */
struct Choice {
    std::string url;
    /** Lowercase hex, with the "sha256:" prefix already stripped. */
    std::string sha256;
    long long   size = 0;
    /** Empty when the choice may be acted on; a sentence for the player if not. */
    std::string refusal;

    bool ok() const { return refusal.empty() && !url.empty() && !sha256.empty(); }
};

/**
 * Read a GitHub "releases/latest" reply and decide whether anything in it may
 * be downloaded.
 *
 * `toolLabel` only appears in the refusal sentences, so a player is told which
 * program could not be installed.
 */
Choice chooseAsset(const std::string& releaseJson, const Recipe& recipe,
                   const std::string& toolLabel);

}  // namespace odtool
