// Deciding whether a third-party program may be downloaded at all.
//
// This is the gate in front of the most dangerous thing the game does. It was
// written inline for cloudflared and had NO tests, because reaching it needed a
// network and a real release. As a pure function over a string, every refusal
// can be pushed through it -- and the refusals are the point: the happy path is
// one line, and the six ways it says no are what make it safe.
//
// The subtlest one is the last: a release lists many assets, and reading the
// checksum from one while reading the URL from another is a verification that
// always passes and means nothing.

#include "ToolRelease.h"

#include <cstdio>
#include <string>

static int checks = 0, fails = 0;
static void ok(bool c, const std::string& what) {
    ++checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++fails;
}
static void section(const char* t) { printf("\n== %s ==\n", t); }

static const char* kHost = "https://github.com/ollama/ollama/releases/download/";
static const std::string kGoodHex(64, 'a');

static odtool::Recipe recipe() {
    return {"ollama-darwin.tgz", kHost, 500LL * 1024 * 1024};
}

/// A release reply shaped like GitHub's, with the fields we read.
static std::string release(const std::string& name, const std::string& digest,
                           long long size, const std::string& url) {
    return std::string("{\"tag_name\":\"v0.1\",\"assets\":[")
         + "{\"name\":\"" + name + "\",\"digest\":\"" + digest + "\","
         + "\"size\":" + std::to_string(size) + ","
         + "\"browser_download_url\":\"" + url + "\"}]}";
}

int main() {
    printf("Tool release gate\n");
    const auto r = recipe();

    section("the one case where a download may happen");
    {
        const auto c = odtool::chooseAsset(
            release("ollama-darwin.tgz", "sha256:" + kGoodHex, 20000000,
                    std::string(kHost) + "v0.1/ollama-darwin.tgz"), r, "Ollama");
        ok(c.ok(), "a well-formed release is accepted");
        ok(c.sha256 == kGoodHex, "and the digest is handed back without its prefix");
        ok(c.size == 20000000, "with the size it declared");
        ok(c.refusal.empty(), "and nothing to tell the player");
    }
    {
        // Upper-case hex is valid hex. Lowercased here so the later comparison
        // against a computed digest cannot fail on case alone.
        const std::string upper(64, 'A');
        const auto c = odtool::chooseAsset(
            release("ollama-darwin.tgz", "sha256:" + upper, 1000,
                    std::string(kHost) + "v/ollama-darwin.tgz"), r, "Ollama");
        ok(c.ok() && c.sha256 == std::string(64, 'a'),
           "an upper-case checksum is accepted and folded down");
    }

    section("no checksum, no download");
    {
        const auto missing = odtool::chooseAsset(
            std::string("{\"assets\":[{\"name\":\"ollama-darwin.tgz\",\"size\":10,")
                + "\"browser_download_url\":\"" + kHost + "v/ollama-darwin.tgz\"}]}",
            r, "Ollama");
        ok(!missing.ok(), "a release with no digest at all is refused");
        ok(missing.refusal.find("checksum") != std::string::npos,
           "and the player is told why");

        const auto shortHex = odtool::chooseAsset(
            release("ollama-darwin.tgz", "sha256:abcd", 10,
                    std::string(kHost) + "v/ollama-darwin.tgz"), r, "Ollama");
        ok(!shortHex.ok(), "a truncated digest is refused");

        const auto wrongAlgo = odtool::chooseAsset(
            release("ollama-darwin.tgz", "md5:" + std::string(64, 'a'), 10,
                    std::string(kHost) + "v/ollama-darwin.tgz"), r, "Ollama");
        ok(!wrongAlgo.ok(), "so is a digest that is not sha256");

        // Right length, wrong alphabet. Length alone would let this through and
        // it would fail the comparison much later, as a confusing error.
        const auto notHex = odtool::chooseAsset(
            release("ollama-darwin.tgz", "sha256:" + std::string(64, 'z'), 10,
                    std::string(kHost) + "v/ollama-darwin.tgz"), r, "Ollama");
        ok(!notHex.ok(), "and a digest of the right length that is not hex");
    }

    section("only from the host we chose");
    {
        const auto elsewhere = odtool::chooseAsset(
            release("ollama-darwin.tgz", "sha256:" + kGoodHex, 10,
                    "https://evil.example/ollama-darwin.tgz"), r, "Ollama");
        ok(!elsewhere.ok(), "a URL on another host is refused");

        // The prefix check must not be fooled by a host that merely BEGINS the
        // same way once an attacker controls the reply.
        const auto lookalike = odtool::chooseAsset(
            release("ollama-darwin.tgz", "sha256:" + kGoodHex, 10,
                    "https://github.com.evil.example/ollama/ollama/releases/download/x"),
            r, "Ollama");
        ok(!lookalike.ok(), "and so is a host that only looks like it");

        const auto httpNotHttps = odtool::chooseAsset(
            release("ollama-darwin.tgz", "sha256:" + kGoodHex, 10,
                    "http://github.com/ollama/ollama/releases/download/x"), r, "Ollama");
        ok(!httpNotHttps.ok(), "plain http is not the allowed prefix either");
    }

    section("a size that makes sense");
    {
        const auto huge = odtool::chooseAsset(
            release("ollama-darwin.tgz", "sha256:" + kGoodHex, 900LL * 1024 * 1024,
                    std::string(kHost) + "v/ollama-darwin.tgz"), r, "Ollama");
        ok(!huge.ok(), "an asset far larger than the tool could be is refused");

        const auto zero = odtool::chooseAsset(
            release("ollama-darwin.tgz", "sha256:" + kGoodHex, 0,
                    std::string(kHost) + "v/ollama-darwin.tgz"), r, "Ollama");
        ok(!zero.ok(), "and so is one that claims to be empty");
    }

    section("nothing for this platform");
    {
        const auto other = odtool::chooseAsset(
            release("ollama-linux-amd64.tgz", "sha256:" + kGoodHex, 10,
                    std::string(kHost) + "v/ollama-linux-amd64.tgz"), r, "Ollama");
        ok(!other.ok(), "a release without our asset is refused");
        ok(other.refusal.find("by hand") != std::string::npos,
           "and the player is pointed at the manual route");
        ok(!odtool::chooseAsset("", r, "Ollama").ok(), "an empty reply is refused");
        ok(!odtool::chooseAsset("not json", r, "Ollama").ok(), "so is rubbish");
    }

    section("THE SUBTLE ONE: the digest and the URL must be the same asset");
    {
        // Two assets. Ours has a checksum; the other has a different one and a
        // URL. Reading the two fields independently across the whole reply
        // would pair our name with the other's URL -- a check that passes and
        // verifies nothing.
        const std::string two =
            std::string("{\"assets\":[")
            + "{\"name\":\"ollama-darwin.tgz\",\"digest\":\"sha256:" + kGoodHex + "\","
            + "\"size\":1000,\"browser_download_url\":\""
            + kHost + "v/ollama-darwin.tgz\"},"
            + "{\"name\":\"ollama-linux-amd64.tgz\",\"digest\":\"sha256:"
            + std::string(64, 'b') + "\",\"size\":2000,"
            + "\"browser_download_url\":\"" + kHost + "v/ollama-linux-amd64.tgz\"}]}";
        const auto c = odtool::chooseAsset(two, r, "Ollama");
        ok(c.ok(), "the right asset is found among several");
        ok(c.sha256 == kGoodHex, "with ITS digest, not the other one's");
        ok(c.url.find("darwin") != std::string::npos, "and ITS url");
        ok(c.size == 1000, "and ITS size");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
