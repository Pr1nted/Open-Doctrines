// THE WORLD YOU SAVED IS THE WORLD YOU GET BACK.
//
// save_delta_test.cpp covers the turn-delta codec; save_roundtrip_test.cpp
// covers writing one archive and reading it again. Neither covers what a
// PLAYER does to a save over an evening: append a turn, hit Save, append more,
// quit, come back, play on. Each of those rewrites the whole archive, and the
// turn history is carried across by hand every time.
//
// WHY THIS EXISTS. A player reported: "when a world is saved and exited,
// returning resets the world to how it was on the first day, undoing all
// in-game progress -- however, the politics and research tabs are not reset",
// and that a timelapse of the reset world was a static GIF. All three are one
// symptom. The world is rebuilt by REPLAYING turn deltas; politics and
// research come from state.json, which is written whole. So a save that loses
// its turns -- or merely loses COUNT of them -- comes back at day one with its
// research intact and no history to animate.
//
// The replay walks 1..metadata.turn_count. That number and the turns/ entries
// are maintained separately, in five different rewrite paths, and nothing
// checked they agreed. An archive can hold 200 turns and say 0, and the game
// would open it, quietly, as a new world.
//
// So the invariant every case below ends on is: the count and the entries
// agree, and every turn the count claims can actually be read back.

#include "SaveManager.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;
static void ok(bool cond, const std::string& what, const std::string& detail = "") {
    ++g_checks;
    printf("  %-4s  %s%s\n", cond ? "ok" : "FAIL", what.c_str(),
           detail.empty() ? "" : ("  [" + detail + "]").c_str());
    if (!cond) ++g_failed;
}
static void section(const char* n) { printf("\n== %s ==\n", n); }

/** A turn with something in it, so a dropped one is visible in the bytes. */
static TurnDelta aTurn(int n) {
    TurnDelta d;
    d.turnNumber = n;
    ProvinceDelta p;
    p.provinceId        = 100 + n;
    p.ownerChanged      = true;  p.newOwner      = n % 7 + 1;
    p.populationChanged = true;  p.newPopulation = 1000000LL + n;
    d.provinces.push_back(p);
    d.researchPoints = n * 10;
    return d;
}

/**
 * THE INVARIANT. What metadata claims, and what is actually in the archive.
 *
 * Returns false and says which way they disagree, because the two directions
 * are different bugs: a count too LOW silently discards history (the reported
 * one), a count too HIGH makes the replay ask for turns that are not there.
 */
static bool historyAgrees(const std::string& path, int expected, std::string& why) {
    const SaveMetadata meta = SaveManager::readMetadata(path);
    if (meta.turnCount != expected) {
        why = "turn_count is " + std::to_string(meta.turnCount) +
              ", expected " + std::to_string(expected);
        return false;
    }
    for (int t = 1; t <= expected; ++t) {
        const TurnDelta got = SaveManager::readTurn(path, t);
        if (got.turnNumber != t) {
            why = "turn " + std::to_string(t) + " is not readable back";
            return false;
        }
    }
    // And nothing beyond it, or the count is lying the other way.
    if (SaveManager::readTurn(path, expected + 1).turnNumber != 0) {
        why = "there is a turn " + std::to_string(expected + 1) + " the count does not admit to";
        return false;
    }
    return true;
}

static std::string makeSave(const std::string& path) {
    SaveMetadata meta;
    meta.saveName = "Durability";
    meta.version = "test";
    meta.created = "now";
    meta.turnCount = 0;
    meta.provinceCount = 3;
    SaveManager::createSave(path, "PRETEND-ODMAP-BYTES", meta);
    return path;
}

int main() {
    const std::string dir = "/tmp/od-save-durability/";
    system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    std::string why;

    section("an evening of play");
    {
        const std::string p = makeSave(dir + "evening.odsv");
        ok(historyAgrees(p, 0, why), "a new world has no history", why);

        for (int t = 1; t <= 5; ++t) SaveManager::appendTurn(p, aTurn(t));
        ok(historyAgrees(p, 5, why), "five turns played, five turns kept", why);

        // The player hits Save. This is trySaveGame(): metadata, then state.
        SaveMetadata m = SaveManager::readMetadata(p);
        SaveManager::updateLastPlayed(p, &m);
        SaveManager::writeState(p, "{\"research\":\"kept\"}", {});
        ok(historyAgrees(p, 5, why), "and hitting Save keeps all five", why);

        // ...and plays on afterwards.
        for (int t = 6; t <= 8; ++t) SaveManager::appendTurn(p, aTurn(t));
        ok(historyAgrees(p, 8, why), "playing on after a Save keeps counting", why);

        // Save again, twice, because a rewrite that loses history usually
        // loses it on the SECOND pass rather than the first.
        for (int i = 0; i < 2; ++i) {
            SaveMetadata m2 = SaveManager::readMetadata(p);
            SaveManager::updateLastPlayed(p, &m2);
            SaveManager::writeState(p, "{\"research\":\"still here\"}", {});
        }
        ok(historyAgrees(p, 8, why), "and saving repeatedly keeps it too", why);
    }

    section("what the player would have seen");
    {
        // The reported shape, asserted directly: state survives AND history
        // survives. Either alone is the bug.
        const std::string p = makeSave(dir + "reported.odsv");
        for (int t = 1; t <= 12; ++t) SaveManager::appendTurn(p, aTurn(t));
        SaveManager::writeState(p, "{\"politics\":\"enacted\",\"research\":\"ind4\"}", {});

        ok(SaveManager::readState(p) == "{\"politics\":\"enacted\",\"research\":\"ind4\"}",
           "politics and research come back");
        ok(historyAgrees(p, 12, why), "AND so does every turn that made them", why);
    }

    section("carrying the rest of the archive");
    {
        const std::string p = makeSave(dir + "extras.odsv");
        for (int t = 1; t <= 3; ++t) SaveManager::appendTurn(p, aTurn(t));

        std::vector<std::pair<std::string, std::string>> rebels = {
            {"rebels.json", "[{\"cid\":900}]"},
            {"rebellion/900.svg", "<svg/>"},
        };
        SaveManager::writeState(p, "{\"s\":1}", rebels);
        ok(historyAgrees(p, 3, why), "an archive with rebel files keeps its turns", why);

        // Written again with DIFFERENT contents under the same names: a zip
        // lookup answers with the first match, so a duplicate entry means the
        // stale copy wins for ever.
        rebels[0].second = "[{\"cid\":900},{\"cid\":901}]";
        SaveManager::writeState(p, "{\"s\":2}", rebels);
        ok(historyAgrees(p, 3, why), "and keeps them when those files change", why);
        ok(SaveManager::readState(p) == "{\"s\":2}", "the newest state is the one that reads back");
    }

    section("a long game");
    {
        // 60 rewrites, because the carry-across is the part that degrades: it
        // copies every previous turn on every append.
        const std::string p = makeSave(dir + "long.odsv");
        for (int t = 1; t <= 60; ++t) SaveManager::appendTurn(p, aTurn(t));
        ok(historyAgrees(p, 60, why), "sixty turns survive sixty rewrites", why);
        const TurnDelta first = SaveManager::readTurn(p, 1);
        ok(first.turnNumber == 1 && first.provinces.size() == 1 &&
               first.provinces[0].newPopulation == 1000001LL &&
               first.provinces[0].provinceId == 101 && first.researchPoints == 10,
           "and turn one still says what it said");
    }

    section("damage is reported, not guessed at");
    {
        const std::string p = makeSave(dir + "damaged.odsv");
        for (int t = 1; t <= 3; ++t) SaveManager::appendTurn(p, aTurn(t));
        ok(SaveManager::readTurn(p, 99).turnNumber == 0,
           "a turn that was never played reads as no turn");
        ok(SaveManager::readMetadata(dir + "does-not-exist.odsv").saveName.empty(),
           "a save that is not there names itself as nothing");
        ok(!SaveManager::appendTurn(dir + "does-not-exist.odsv", aTurn(1)),
           "and appending to it fails rather than pretending");

        // A FILE THAT IS NOT A SAVE. Refused, and -- the part that matters --
        // refused by RETURNING FALSE, because every caller in the game
        // discards that value and a silent failure here is a lost evening.
        FILE* f = fopen((dir + "notazip.odsv").c_str(), "wb");
        if (f) { fputs("this is not a zip", f); fclose(f); }
        ok(!SaveManager::appendTurn(dir + "notazip.odsv", aTurn(1)),
           "appending to a file that is not a save fails");
        ok(!SaveManager::writeState(dir + "notazip.odsv", "{}", {}),
           "and so does writing state into one");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
