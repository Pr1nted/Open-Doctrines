#include "ToolRelease.h"

#include "HttpClient.h"

#include <algorithm>
#include <cctype>

namespace odtool {

Choice chooseAsset(const std::string& releaseJson, const Recipe& recipe,
                   const std::string& toolLabel) {
    Choice out;

    // ── Find THIS asset, and read everything from inside its own entry ──
    //
    // A release lists many assets. Searching the whole reply for "digest" and
    // "browser_download_url" separately would happily pair one asset's checksum
    // with another asset's URL, which is a verification that always passes and
    // means nothing. The window below starts at the matched name and ends just
    // past that entry's download URL.
    const std::string needle = "\"name\":\"" + recipe.assetName + "\"";
    const size_t at = releaseJson.find(needle);
    if (at == std::string::npos) {
        out.refusal = "The current " + toolLabel + " release has no " +
                      recipe.assetName + ". Installing it by hand still works.";
        return out;
    }
    const size_t end = releaseJson.find("browser_download_url", at);
    const std::string scope = releaseJson.substr(
        at, end == std::string::npos ? 2048 : end - at + 256);

    const std::string digest = httpJsonString(scope, "digest", 128);
    const long long size = httpJsonNumber(scope, "size", 0);
    const std::string url = httpJsonString(scope, "browser_download_url", 512);

    // ── A digest, in the right shape, or nothing happens ──
    //
    // "sha256:" plus 64 hex characters is 71. Length alone is not enough: a
    // digest of the right length made of the wrong alphabet would compare
    // against a hex string and never match, which is a confusing failure much
    // later rather than a clear one here.
    if (digest.rfind("sha256:", 0) != 0 || digest.size() != 71) {
        out.refusal = "That " + toolLabel + " release did not come with a checksum, "
                      "so the download cannot be verified and will not be run. "
                      "Install it by hand instead.";
        return out;
    }
    const std::string hex = digest.substr(7);
    const bool hexOnly = std::all_of(hex.begin(), hex.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
    if (!hexOnly) {
        out.refusal = "That " + toolLabel + " release's checksum is malformed, so "
                      "nothing was downloaded.";
        return out;
    }

    // ── The one host we will fetch from, whatever the reply says ──
    if (recipe.allowedAssetHost.empty() ||
        url.rfind(recipe.allowedAssetHost, 0) != 0) {
        out.refusal = "That release points somewhere unexpected, so nothing was "
                      "downloaded.";
        return out;
    }

    if (size <= 0 || size > recipe.maxAssetBytes) {
        out.refusal = "That release is an unexpected size, so nothing was downloaded.";
        return out;
    }

    out.url = url;
    // Lowercased so the comparison against a computed digest is a plain string
    // equality and cannot fail on case alone.
    out.sha256.resize(hex.size());
    std::transform(hex.begin(), hex.end(), out.sha256.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    out.size = size;
    return out;
}

}  // namespace odtool
