// Achievement grants, checked against signatures this codebase did not make.
//
// The vectors in tests/data/achievement_grants.json come from Node's Ed25519,
// in the format net/src/achievements/grant.ts writes -- so a verifier that
// merely agreed with itself would fail here. Every refusal the game depends on
// is pinned: a forged payload, a stranger's key, the wrong audience, the wrong
// issuer. Then the .odstate rule: an archive cannot drop a grants file in
// place, only offer one to be verified.

#include "achievements/GrantVerify.h"
#include "OdState.h"
#include "json.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
int g_checks = 0, g_failed = 0;
void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    ++g_failed;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}
std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
void spit(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
}  // namespace

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::path(".");
    const json v = json::parse(slurp(root / "tests" / "data" / "achievement_grants.json"));
    const std::string iss = v["issuer"];
    const auto keys = odach::parseGrantKeys(v["key"].get<std::string>());
    const auto both = odach::parseGrantKeys(v["otherKey"].get<std::string>() + "," + v["key"].get<std::string>());
    ok(keys.size() == 1, "one key parses");
    ok(both.size() == 2, "a key list parses");

    odach::GrantFields f;
    ok(odach::verifyGrantToken(v["good"], iss, keys, f), "a good grant verifies");
    ok(f.sub == "acct_123" && f.ach == "first_steps" && f.iat == 1790000000 && !f.modded, "and its fields read back");
    ok(odach::verifyGrantToken(v["modded"], iss, keys, f) && f.modded && f.ach == "nuke", "the modified-rules mark reads back");
    ok(odach::verifyGrantToken(v["good"], "", keys, f), "no configured issuer: the key list decides");
    ok(odach::verifyGrantToken(v["good"], iss, both, f), "an older key in the list still verifies");

    ok(!odach::verifyGrantToken(v["tampered"], iss, keys, f), "an edited payload is refused");
    ok(!odach::verifyGrantToken(v["otherKey_signed"], iss, keys, f), "a stranger's key is refused");
    ok(!odach::verifyGrantToken(v["wrongAud"], iss, keys, f), "a session token is not a grant");
    ok(!odach::verifyGrantToken(v["wrongIss"], iss, keys, f), "another issuer is refused");
    ok(!odach::verifyGrantToken("oda1.e30.AAAA", iss, keys, f), "garbage is refused");
    ok(!odach::verifyGrantToken(v["good"], iss, {}, f), "no keys, no grants");
    // Signed and valid: GrantVerify accepts it, and Tracker refuses it because
    // the catalog has no such id. Here only the first half is checked.
    ok(odach::verifyGrantToken(v["unknownAch"], iss, keys, f), "catalog membership is the tracker's check, not this one");

    // ---- a .odstate cannot place a grants file ----
    const fs::path tmp = fs::temp_directory_path() / "od_ach_test";
    fs::remove_all(tmp);
    const fs::path src = tmp / "src", dst = tmp / "dst";
    spit(src / "achievements" / "grants.json", R"({"grants":["forged"]})");
    spit(src / "achievements" / "progress.json", R"({"counters":{"turns_ended":99999}})");
    spit(src / "achievements" / "evil.bin", "x");
    spit(src / "config.json", "{}");
    std::string err;
    ok(OdState::save(src.string() + "/", (tmp / "a.odstate").string(), err), "archive written: " + err);
    spit(dst / "achievements" / "progress.json", R"({"counters":{"turns_ended":3}})");
    spit(dst / "achievements" / "grants.json", R"({"grants":[]})");
    OdState::load(dst.string() + "/", (tmp / "a.odstate").string(), err);
    ok(slurp(dst / "achievements" / "grants.json") == R"({"grants":[]})", "the real grants file is never overwritten");
    ok(fs::exists(dst / "achievements" / "grants.import.json"), "the archive's grants wait to be verified");
    ok(slurp(dst / "achievements" / "progress.json").find("99999") == std::string::npos,
       "an install's own progress is kept over the archive's");
    ok(!fs::exists(dst / "achievements" / "evil.bin"), "nothing else is written under achievements/");
    ok(fs::exists(dst / "config.json"), "everything else restores as before");
    fs::remove_all(tmp);

    std::printf("achievements: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
