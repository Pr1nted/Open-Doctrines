// Tests for the in-game updater.
//
// Three things are worth testing here and they are all things that go wrong
// quietly rather than loudly:
//
//   1. Version ordering. If it disagrees with tools/odver.py, the game either
//      nags players who are up to date or never tells anyone about a release.
//      So the ordering is not just asserted here -- it is compared against the
//      Python, which is the version scheme's actual definition.
//   2. Parsing a release reply. The input comes off the network, so the tests
//      feed it malformed, oversized and hostile documents and require that
//      none of them produce an update.
//   3. Installing. The rule is that an update never deletes anything, because
//      the thing it would delete is somebody's save. That is checked against a
//      real directory tree rather than by reading the code.
//
// Build target: GameUpdatesTest.

#include "GameUpdates.h"

#include <cstdio>

// MSVC has popen/pclose, spelled with a leading underscore. Aliasing here keeps
// the one call site below readable rather than sprouting an #ifdef around it.
#ifdef _WIN32
  #define OD_POPEN  _popen
  #define OD_PCLOSE _pclose
#else
  #define OD_POPEN  popen
  #define OD_PCLOSE pclose
#endif
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(const std::string& what, bool cond, const std::string& detail = "") {
    g_checks++;
    if (cond) { printf("  ok    %s\n", what.c_str()); return; }
    printf("  FAIL  %s%s%s\n", what.c_str(), detail.empty() ? "" : " — ",
           detail.c_str());
    g_failures++;
}

int cmp(const char* a, const char* b) {
    GameVersion va, vb;
    if (!GameVersion::parse(a, va) || !GameVersion::parse(b, vb)) return -99;
    return GameVersion::compare(va, vb);
}

void write(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << body;
}

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "<missing>";
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------------------

void testVersionParsing() {
    printf("\n=== version parsing ===\n");
    GameVersion v;
    check("1.0.2a parses", GameVersion::parse("1.0.2a", v) &&
          v.major == 1 && v.minor == 0 && v.patch == 2 && v.state == 'a');
    check("1.0.2s3 keeps its counter", GameVersion::parse("1.0.2s3", v) &&
          v.state == 's' && v.counter == 3);
    check("2.11.40r parses", GameVersion::parse("2.11.40r", v) &&
          v.minor == 11 && v.patch == 40 && v.state == 'r');

    // Everything odver.py's regex refuses, refused here for the same reasons.
    // A version the game cannot parse must never be treated as newer, or a
    // junk reply from the server becomes an update prompt.
    const char* junk[] = {"1.0.2", "1.0.2x", "1.0", "", "v1.0.2a", "1.0.2a1",
                          "1.0.2ar", " 1.0.2a", "1.0.2a ", "a.b.cd",
                          "1.0.2r3", "-1.0.2a", "1.0.2.3a"};
    bool allRejected = true;
    for (const char* j : junk)
        if (GameVersion::parse(j, v)) { allRejected = false;
            printf("        accepted %s\n", j); }
    check("malformed versions are all rejected", allRejected);
}

void testVersionOrdering() {
    printf("\n=== version ordering ===\n");
    check("a newer patch is newer", cmp("1.0.2a", "1.0.3a") < 0);
    check("a newer minor is newer", cmp("1.0.9a", "1.1.0a") < 0);
    check("a newer major is newer", cmp("1.9.9r", "2.0.0a") < 0);
    check("release supersedes the beta of the same number",
          cmp("1.0.2b", "1.0.2r") < 0);
    check("beta supersedes the alpha of the same number",
          cmp("1.0.2a", "1.0.2b") < 0);
    check("a snapshot sorts before the version it hangs off",
          cmp("1.0.2s3", "1.0.2a") < 0);
    check("snapshots count up", cmp("1.0.2s1", "1.0.2s2") < 0);
    check("equal is equal", cmp("1.0.2r", "1.0.2r") == 0);
    check("the same version is not an update", cmp("1.0.2r", "1.0.2r") >= 0);

    // The real check: agree with the Python, which is where the scheme is
    // actually defined. Asserting the ordering twice in two languages is how
    // they drift; comparing them is how they cannot.
    const std::vector<std::string> table = {
        "1.0.2s1", "1.0.2s2", "1.0.2a", "1.0.2b", "1.0.2r",
        "1.0.3a", "1.1.0a", "2.0.0a", "0.9.9r", "1.0.10a", "1.0.2s10",
    };
    std::string cmd = "python3 -c \""
        "import sys; sys.path.insert(0, 'tools'); import odver; "
        "vs = sys.argv[1].split(','); "
        "print(','.join(sorted(vs, key=lambda s: odver.Version.parse(s).sort_key())))\" ";
    std::string joined;
    for (size_t i = 0; i < table.size(); ++i) joined += (i ? "," : "") + table[i];
    // 2>NUL on Windows: popen there goes through cmd.exe, which has no
    // /dev/null and would take it as a filename to redirect into.
#ifdef _WIN32
    cmd += joined + " 2>NUL";
#else
    cmd += joined + " 2>/dev/null";
#endif

    FILE* p = OD_POPEN(cmd.c_str(), "r");
    std::string pyOut;
    if (p) {
        char buf[512];
        while (fgets(buf, sizeof buf, p)) pyOut += buf;
        OD_PCLOSE(p);
    }
    while (!pyOut.empty() && (pyOut.back() == '\n' || pyOut.back() == '\r'))
        pyOut.pop_back();

    if (pyOut.empty()) {
        // Reported rather than skipped silently: a cross-check that stopped
        // running is a cross-check that stopped protecting anything.
        check("odver.py cross-check ran", false,
              "python3 produced nothing; run from the repository root");
        return;
    }

    std::vector<std::string> mine = table;
    std::sort(mine.begin(), mine.end(), [](const std::string& a, const std::string& b) {
        return cmp(a.c_str(), b.c_str()) < 0;
    });
    std::string cppOut;
    for (size_t i = 0; i < mine.size(); ++i) cppOut += (i ? "," : "") + mine[i];

    check("C++ ordering matches tools/odver.py", cppOut == pyOut,
          cppOut + " vs " + pyOut);
}

void testParsingAReleaseReply() {
    printf("\n=== parsing a release reply ===\n");
    const std::string plat = "OpenDoctrines-linux-x64";

    std::string good = R"({
      "tag_name": "v1.4.0r",
      "html_url": "https://github.com/Pr1nted/Open-Doctrines/releases/tag/v1.4.0r",
      "body": "Fixed the thing.\nAnd the other thing.",
      "assets": [
        {"name": "OpenDoctrines-macos-arm64.zip",
         "size": 111,
         "browser_download_url": "https://github.com/Pr1nted/Open-Doctrines/releases/download/v1.4.0r/OpenDoctrines-macos-arm64.zip"},
        {"name": "OpenDoctrines-linux-x64.zip",
         "size": 4242,
         "digest": "sha256:0000000000000000000000000000000000000000000000000000000000000042",
         "browser_download_url": "https://github.com/Pr1nted/Open-Doctrines/releases/download/v1.4.0r/OpenDoctrines-linux-x64.zip"}
      ]
    })";

    GameUpdates::Status s;
    check("a well-formed reply parses", GameUpdates::parseRelease(good, plat, s));
    check("the leading v is stripped from the tag", s.latest == "1.4.0r", s.latest);
    check("this platform's asset is the one chosen",
          s.assetUrl.find("linux-x64") != std::string::npos, s.assetUrl);
    check("the asset size is read", s.assetSize == 4242);
    check("the digest is read", s.sha256.size() == 64, s.sha256);
    check("escapes in the notes become real newlines",
          s.notes.find("Fixed the thing.\nAnd") != std::string::npos, s.notes);

    // A reply for a platform with no build must not offer a download. The
    // release is still reported, so the player is told it exists.
    GameUpdates::Status w;
    check("a platform with no asset still reports the release",
          GameUpdates::parseRelease(good, "OpenDoctrines-solaris-sparc", w));
    check("...but offers no download", w.assetUrl.empty(), w.assetUrl);

    // Hostile and broken input. None of it may produce a usable update.
    struct { const char* what; std::string body; } bad[] = {
        {"an empty reply", ""},
        {"a reply with no tag", R"({"html_url": "https://github.com/x"})"},
        {"a tag that is not a version", R"({"tag_name": "latest"})"},
        {"a tag that is prose", R"({"tag_name": "the newest one ever"})"},
        {"an HTML error page", "<html><body>404</body></html>"},
    };
    for (auto& b : bad) {
        GameUpdates::Status out;
        check(std::string(b.what) + " is refused",
              !GameUpdates::parseRelease(b.body, plat, out));
    }

    // The download URL is the dangerous field: it is where the game would go
    // to fetch code. A reply that points somewhere else must lose the URL, not
    // be followed.
    std::string offHost = R"({
      "tag_name": "v9.9.9r",
      "html_url": "https://evil.example.com/pwn",
      "assets": [{"name": "OpenDoctrines-linux-x64.zip", "size": 1,
                  "browser_download_url": "https://evil.example.com/payload.zip"}]
    })";
    GameUpdates::Status e;
    check("a release on a different host still parses",
          GameUpdates::parseRelease(offHost, plat, e));
    check("...but its download URL is refused", e.assetUrl.empty(), e.assetUrl);
    check("...and its page falls back to the real release page",
          e.pageUrl.rfind("https://github.com/Pr1nted/", 0) == 0, e.pageUrl);

    check("http is not a release URL",
          !GameUpdates::isReleaseHostUrl("http://github.com/Pr1nted/x"));
    check("a lookalike host is not a release URL",
          !GameUpdates::isReleaseHostUrl("https://github.com.evil.example/x"));
    check("a userinfo trick is not a release URL",
          !GameUpdates::isReleaseHostUrl("https://github.com@evil.example/x"));
    check("a javascript: url is not a release URL",
          !GameUpdates::isReleaseHostUrl("javascript:alert(1)"));
    check("a url with a shell metacharacter is refused",
          !GameUpdates::isReleaseHostUrl("https://github.com/x;rm -rf /"));
    check("the real asset host is accepted",
          GameUpdates::isReleaseHostUrl(
              "https://objects.githubusercontent.com/foo"));
}

void testSha256() {
    printf("\n=== sha256 ===\n");
    // The published test vectors. A digest implementation that is subtly wrong
    // rejects every legitimate download, which would look like a network fault.
    check("empty string",
          GameUpdates::sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc",
          GameUpdates::sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("a 448-bit message",
          GameUpdates::sha256Hex(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string million(1000000, 'a');
    check("a million a's",
          GameUpdates::sha256Hex(million) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void testInstallNeverDeletes() {
    printf("\n=== installing never deletes ===\n");
    fs::path root = fs::temp_directory_path() / "od-update-test";
    fs::remove_all(root);

    fs::path install = root / "install";
    fs::path staged  = root / "staged";

    // An install with the player's own things in it, exactly the files a
    // careless update would destroy.
    write(install / "OpenDoctrines", "OLD BINARY");
    write(install / "data" / "policies.json", "old policies");
    write(install / "data" / "saves" / "My Campaign.odsv", "PRECIOUS");
    write(install / "data" / "custom_maps" / "My World.odmap", "PRECIOUS");
    write(install / "data" / "config.json", "{\"mine\": true}");
    write(install / "data" / "mods" / "mymod.odmod", "PRECIOUS");
    write(install / "data" / "mods.json", "{\"enabled\": [\"mymod\"]}");

    // What a release contains: the program, the shipped data, and the empty
    // directories package.py creates for the player's files.
    write(staged / "OpenDoctrines", "NEW BINARY");
    write(staged / "data" / "policies.json", "new policies");
    write(staged / "data" / "tips.json", "new tips");
    fs::create_directories(staged / "data" / "saves");
    fs::create_directories(staged / "data" / "custom_maps");
    fs::create_directories(staged / "data" / "mods");

    std::string err;
    bool ok = GameUpdates::installOver(staged.string(), install.string(),
                                       "OpenDoctrines", err);
    check("the install succeeds", ok, err);

    check("shipped content is replaced",
          read(install / "data" / "policies.json") == "new policies");
    check("new content arrives",
          read(install / "data" / "tips.json") == "new tips");
    check("the running binary is left for the caller to swap",
          read(install / "OpenDoctrines") == "OLD BINARY");

    check("a save survives",
          read(install / "data" / "saves" / "My Campaign.odsv") == "PRECIOUS");
    check("a custom map survives",
          read(install / "data" / "custom_maps" / "My World.odmap") == "PRECIOUS");
    check("an installed mod survives",
          read(install / "data" / "mods" / "mymod.odmod") == "PRECIOUS");
    check("the player's config survives",
          read(install / "data" / "config.json") == "{\"mine\": true}");
    check("which mods were enabled survives",
          read(install / "data" / "mods.json") == "{\"enabled\": [\"mymod\"]}");

    // A staged tree that is not there at all must fail rather than report a
    // successful install of nothing.
    std::string err2;
    check("installing from a missing directory fails",
          !GameUpdates::installOver((root / "nope").string(), install.string(),
                                    "OpenDoctrines", err2));

    fs::remove_all(root);
}

void testInstallDirIsExplicit() {
    printf("\n=== where an update installs ===\n");
    // Without this the updater would stage into whatever directory the OS gave
    // the process, which for a double-clicked app is not where the game lives.
    std::string beforeSet = GameUpdates::installDir();
    check("with nothing set, it falls back to the working directory",
          beforeSet == fs::current_path().string(), beforeSet);

    fs::path elsewhere = fs::temp_directory_path() / "od-install-dir-test";
    GameUpdates::setInstallDir(elsewhere.string());
    check("an explicit install directory wins",
          GameUpdates::installDir() == elsewhere.string(),
          GameUpdates::installDir());

    // cleanUpAfterUpdate must sweep the install directory, not the CWD.
    fs::create_directories(elsewhere / ".od-update" / "staged");
    write(elsewhere / "OpenDoctrines.old", "displaced");
    GameUpdates::cleanUpAfterUpdate();
    check("stale staging in the install directory is removed",
          !fs::exists(elsewhere / ".od-update"));
    check("a displaced old binary is removed",
          !fs::exists(elsewhere / "OpenDoctrines.old"));

    GameUpdates::setInstallDir(beforeSet);   // leave it as it was found
    fs::remove_all(elsewhere);
}

void testPlatformRules() {
    printf("\n=== platform rules ===\n");
    std::string key = GameUpdates::platformKey();
    check("this build knows its own artifact name",
          key.rfind("OpenDoctrines-", 0) == 0, key);
#if defined(__APPLE__)
    check("macOS does not install its own update (no Developer ID)",
          !GameUpdates::canSelfInstall());
#else
    check("this platform installs its own update",
          GameUpdates::canSelfInstall());
#endif
}


void testAgainstARealGitHubDocument() {
    printf("\n=== a real GitHub release document ===\n");
    // tests/fixtures/github_release.json is a genuine reply from the GitHub
    // releases API with the project's own names substituted in. It is here
    // because a hand-written fixture only proves the parser handles documents
    // shaped the way the author imagined: the real thing nests author and
    // uploader objects that carry their OWN html_url and name fields, in an
    // order nobody would think to invent.
    std::string body = read("tests/fixtures/github_release.json");
    if (body == "<missing>") {
        check("the recorded GitHub reply is readable", false,
              "run the test from the repository root");
        return;
    }

    GameUpdates::Status s;
    check("a real reply parses",
          GameUpdates::parseRelease(body, "OpenDoctrines-linux-x64", s));
    check("the release tag wins over the ones nested inside author objects",
          s.latest == "1.4.0r", s.latest);
    check("the release page wins over the author's profile URL",
          s.pageUrl == "https://github.com/Pr1nted/Open-Doctrines/releases/tag/v1.4.0r",
          s.pageUrl);
    check("the right one of four assets is chosen",
          s.assetUrl.size() > 0 &&
          s.assetUrl.find("OpenDoctrines-linux-x64.zip") != std::string::npos,
          s.assetUrl);
    check("its size comes from that asset, not another",
          s.assetSize == 87654321, std::to_string(s.assetSize));
    std::string wantDigest;
    for (int i = 0; i < 32; ++i) wantDigest += "ab";   // what the fixture carries
    check("its digest comes from that asset, not another",
          s.sha256 == wantDigest, s.sha256);
    check("CRLF in the notes does not survive as visible junk",
          s.notes.find("\\r") == std::string::npos &&
          s.notes.find("Conflict detection") != std::string::npos, s.notes);

    // The macOS builds are separate artifacts; picking the wrong one would
    // hand an arm64 player an x64 download.
    GameUpdates::Status m;
    GameUpdates::parseRelease(body, "OpenDoctrines-macos-arm64", m);
    check("arm64 gets the arm64 archive",
          m.assetUrl.find("macos-arm64.zip") != std::string::npos, m.assetUrl);
    check("...and not the x64 one",
          m.assetUrl.find("x64.zip") == std::string::npos ||
          m.assetUrl.find("macos-arm64") != std::string::npos, m.assetUrl);
}

}  // namespace

int main() {
    printf("=== GameUpdates ===\n");
    testVersionParsing();
    testVersionOrdering();
    testParsingAReleaseReply();
    testAgainstARealGitHubDocument();
    testSha256();
    testInstallNeverDeletes();
    testInstallDirIsExplicit();
    testPlatformRules();

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    if (g_failures) { printf("FAILED\n"); return 1; }
    printf("PASSED\n");
    return 0;
}
