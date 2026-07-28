// Mod sides, grant masking, and the mod-set comparison two ends do on join.
//
// The point of most of these cases is not that a check exists but that it says
// the RIGHT thing: "you have 1.2 and they run 1.3" sends a player somewhere
// useful, "the bytes differ" sends them looking for a corrupted download that
// is not there.
//
// Build target: NetAttestTest. Run it; non-zero exit means a case failed.

#include "net/ModAttest.h"
#include "mods/ModPackage.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(const char* what, bool ok, const std::string& got = {}) {
    g_checks++;
    if (ok) { printf("  ok    %s\n", what); return; }
    g_failures++;
    printf("  FAIL  %s%s%s\n", what,
           got.empty() ? "" : "  --  got: ", got.c_str());
}

const char* kDigestA = "0000000000000000000000000000000000000000000000000000000000000001";
const char* kDigestB = "0000000000000000000000000000000000000000000000000000000000000002";

ModAttestEntry entry(const char* id, const char* version, const char* sha,
                     ModSide side = ModSide::Both) {
    return ModAttestEntry{id, version, sha, side};
}

// ---------------------------------------------------------------- sides ----

void testSides() {
    printf("\n=== side names ===\n");

    bool known = false;
    check("\"client\" parses", modSideFromName("client", known) == ModSide::Client && known);
    check("\"server\" parses", modSideFromName("server", known) == ModSide::Server && known);
    check("\"both\" parses", modSideFromName("both", known) == ModSide::Both && known);

    // A future release adding a fourth side must not stop today's game loading
    // a mod, and "both" is the fallback that cannot silently drop a mod the
    // server needed.
    ModSide fallback = modSideFromName("proxy", known);
    check("an unknown side falls back to both, and says it did not know it",
          fallback == ModSide::Both && !known);

    check("names round-trip", std::string(modSideName(ModSide::Client)) == "client" &&
                              std::string(modSideName(ModSide::Server)) == "server" &&
                              std::string(modSideName(ModSide::Both)) == "both");
}

void testGrantMask() {
    printf("\n=== grant masking ===\n");

    const uint32_t everything = MODULE_GAMESTATE_READ | MODULE_GAMESTATE_WRITE |
                                MODULE_GAMEPROCESS | MODULE_UI;

    // Singleplayer: side means nothing, because there is one process and it is
    // authoritative.
    check("outside multiplayer a client-side mod keeps everything",
          (everything & modSideGrantMask(ModSide::Client, false)) == everything);

    const uint32_t masked = everything & modSideGrantMask(ModSide::Client, true);
    check("in multiplayer a client-side mod loses GameState.Write",
          (masked & MODULE_GAMESTATE_WRITE) == 0);
    check("in multiplayer a client-side mod loses GameProcess",
          (masked & MODULE_GAMEPROCESS) == 0);
    // Reading and drawing are the whole point of a client-side mod; taking
    // those would make the category useless.
    check("but keeps GameState.Read", (masked & MODULE_GAMESTATE_READ) != 0);
    check("and keeps UI", (masked & MODULE_UI) != 0);

    check("a server-side mod is not masked",
          (everything & modSideGrantMask(ModSide::Server, true)) == everything);
    // Even a "both" mod keeps its grants: the client instance is presentation,
    // but that is enforced by the server never asking it to compute anything,
    // not by taking capabilities away.
    check("a both-side mod is not masked",
          (everything & modSideGrantMask(ModSide::Both, true)) == everything);
}

// ---------------------------------------------------------------- entry ----

void testEntryParsing() {
    printf("\n=== entry text form ===\n");

    ModAttestEntry e;
    check("a well-formed entry parses",
          ModAttestEntry::parse(std::string("com.a.b@1.2.3#") + kDigestA, e) &&
          e.id == "com.a.b" && e.version == "1.2.3" && e.sha256 == kDigestA);

    check("round-trips through toString",
          entry("com.a.b", "1.2.3", kDigestA).toString() ==
              std::string("com.a.b@1.2.3#") + kDigestA);

    // Everything below is text another machine sent.
    check("a missing digest is refused", !ModAttestEntry::parse("com.a.b@1.2.3", e));
    check("a short digest is refused",
          !ModAttestEntry::parse("com.a.b@1.2.3#abc", e));
    check("a non-hex digest is refused",
          !ModAttestEntry::parse(std::string("com.a.b@1.2.3#") +
                                 std::string(64, 'z'), e));
    check("an uppercase digest is refused, so one mod has one spelling",
          !ModAttestEntry::parse(std::string("com.a.b@1.2.3#") +
                                 std::string(64, 'A'), e));
    check("an empty id is refused",
          !ModAttestEntry::parse(std::string("@1.2.3#") + kDigestA, e));
    check("an empty version is refused",
          !ModAttestEntry::parse(std::string("com.a.b@#") + kDigestA, e));
    check("an absurd id is refused",
          !ModAttestEntry::parse(std::string(400, 'x') + "@1.0.0#" + kDigestA, e));

    // Splitting on the last separators means a '@' inside an id cannot shift
    // the field boundaries and smuggle a different version through.
    check("an id containing '@' does not shift the fields",
          ModAttestEntry::parse(std::string("com.a@b@1.2.3#") + kDigestA, e) &&
          e.id == "com.a@b" && e.version == "1.2.3");
}

void testWire() {
    printf("\n=== wire form ===\n");

    ModAttestation a;
    a.entries = {
        entry("com.z.last", "1.0.0", kDigestA),
        entry("com.a.first", "2.0.0", kDigestB),
        entry("com.c.clientonly", "1.0.0", kDigestA, ModSide::Client),
        entry("com.d.serveronly", "1.0.0", kDigestA, ModSide::Server),
    };

    // Only the shared set travels: a client's UI mods are its own business,
    // and a server's are not a client's.
    const std::string text = modAttestEncode(a);
    check("only both-side mods are sent",
          text.find("clientonly") == std::string::npos &&
          text.find("serveronly") == std::string::npos, text);

    // Sorted, so two installs that agree produce identical bytes regardless of
    // the order the mods happened to be scanned in.
    check("entries are emitted in id order",
          text.find("com.a.first") < text.find("com.z.last"), text);

    ModAttestation decoded;
    check("decodes what it encoded", modAttestDecode(text, decoded) &&
          decoded.entries.size() == 2);
    check("the digest survives the round trip", decoded.digest() == a.digest());

    ModAttestation reordered;
    reordered.entries = { a.entries[1], a.entries[0] };
    check("the digest does not depend on scan order",
          reordered.digest() == ModAttestation{{a.entries[0], a.entries[1]}}.digest());

    ModAttestation differs;
    differs.entries = { entry("com.a.first", "2.0.1", kDigestB), a.entries[0] };
    check("a version change changes the digest", differs.digest() != a.digest());

    ModAttestation out;
    check("an empty list is valid", modAttestDecode("", out) && out.entries.empty());
    // A partially understood list is worse than a refused one: it would let a
    // client drop an entry simply by malforming its line.
    check("one bad line fails the whole message",
          !modAttestDecode(std::string("com.a.b@1.0.0#") + kDigestA + "\ngarbage\n", out));
}

// -------------------------------------------------------------- compare ----

void testCompare() {
    printf("\n=== comparison ===\n");

    const std::vector<ModAttestEntry> required = {
        entry("com.a.rules", "1.2.3", kDigestA),
        entry("com.b.units", "0.9.0", kDigestB),
    };

    ModAttestation same;
    same.entries = required;
    check("an identical set is accepted", modAttestCompare(required, same).ok);

    ModAttestation missing;
    missing.entries = { required[0] };
    ModAttestResult r = modAttestCompare(required, missing);
    check("a missing mod is refused", !r.ok &&
          r.problems.size() == 1 &&
          r.problems[0].verdict == ModAttestVerdict::Missing);
    check("and the message names it",
          r.problems[0].detail.find("com.b.units") != std::string::npos,
          r.problems[0].detail);

    ModAttestation older;
    older.entries = { entry("com.a.rules", "1.2.2", kDigestA), required[1] };
    r = modAttestCompare(required, older);
    check("a version mismatch is reported as a version mismatch, not a bad file",
          !r.ok && r.problems[0].verdict == ModAttestVerdict::VersionDiffers);
    check("and the message names both versions",
          r.problems[0].detail.find("1.2.3") != std::string::npos &&
          r.problems[0].detail.find("1.2.2") != std::string::npos,
          r.problems[0].detail);

    ModAttestation tampered;
    tampered.entries = { entry("com.a.rules", "1.2.3", kDigestB), required[1] };
    r = modAttestCompare(required, tampered);
    check("same version, different bytes is reported separately",
          !r.ok && r.problems[0].verdict == ModAttestVerdict::BytesDiffer);

    ModAttestation extra;
    extra.entries = { required[0], required[1], entry("com.c.more", "1.0.0", kDigestA) };
    check("an unexpected shared mod is refused by default",
          !modAttestCompare(required, extra).ok);
    check("unless the server allows extras",
          modAttestCompare(required, extra, ModExtraPolicy::Allow).ok);

    // Client-side mods are invisible to this comparison by construction: they
    // are filtered out of shared(). That is the structural reason a server does
    // not care what UI mods a player runs.
    ModAttestation withClientMods;
    withClientMods.entries = {
        required[0], required[1],
        entry("com.ui.minimap", "3.0.0", kDigestA, ModSide::Client),
        entry("com.ui.tooltips", "1.0.0", kDigestB, ModSide::Client),
    };
    check("client-side mods never affect the verdict",
          modAttestCompare(required, withClientMods).ok);

    ModAttestation broken;
    broken.entries = { entry("com.a.rules", "1.2.2", kDigestA) };
    r = modAttestCompare(required, broken);
    check("several problems are summarised without hiding the count",
          r.summary().find("1 other") != std::string::npos, r.summary());
}

}  // namespace

int main() {
    printf("mod side and attestation tests\n");
    testSides();
    testGrantMask();
    testEntryParsing();
    testWire();
    testCompare();
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
