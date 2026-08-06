// Sealing orders for a long-form turn store.
//
// The store is a dumb bucket that anyone with a URL can read, and on some
// backends overwrite. So almost every case here is something that must NOT
// open: a different key, a different turn, a different player, a flipped bit.
//
// The property that matters most is the least obvious one -- a sealed
// submission must not be reusable AS SOMETHING ELSE. Orders that could be
// replayed onto another turn, or attributed to another player, would let
// somebody rewrite a game without ever forging a signature.
//
// Build target: NetSealTest. Run it; non-zero exit means a case failed.

#include "net/TurnSeal.h"

// memcmp. Needed explicitly: libc++ pulls <cstring> in transitively, so this
// compiled on macOS for as long as macOS was the only place it was built, and
// failed on the first Linux/libstdc++ build with "'memcmp' was not declared".
#include <cstring>

#include <cstdio>      // also ::remove, for the key-file case
#include <string>
#include <vector>

// To assert that the key file is not world-readable. Excludes emscripten as
// well as Windows: the web build has no stat to declare, and checking only
// _WIN32 is the mistake TunnelInstall.cpp records having made.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/stat.h>
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

void check(const char* what, bool ok, const std::string& got = {}) {
    g_checks++;
    if (ok) { printf("  ok    %s\n", what); return; }
    g_failures++;
    printf("  FAIL  %s%s%s\n", what, got.empty() ? "" : "  --  ", got.c_str());
}

std::vector<uint8_t> bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

const char* kPsid = "psid_player_0001______";
constexpr uint32_t kTurn = 7;

}  // namespace

int main() {
    printf("sealing turn submissions\n");

    if (!turnSealAvailable()) {
        printf("\nthis build has no networking, so there is nothing to seal\n");
        return 0;
    }

    printf("\n=== keys ===\n");
    TurnSealKey key;
    check("a key can be generated", turnSealKeyGenerate(key));
    check("it is not all zeroes", key.valid());

    TurnSealKey other;
    check("a second key is generated", turnSealKeyGenerate(other));
    {
        bool same = true;
        for (int i = 0; i < 32; i++) if (key.bytes[i] != other.bytes[i]) same = false;
        check("two keys differ", !same);
    }

    // The key travels to players as text over the lobby connection.
    {
        TurnSealKey round;
        check("a key survives the round trip to text",
              TurnSealKey::fromText(key.toText(), round) &&
              memcmp(round.bytes, key.bytes, 32) == 0);
        TurnSealKey junk;
        check("junk is not a key", !TurnSealKey::fromText("not a key", junk));
        check("a truncated key is refused", !TurnSealKey::fromText(key.toText().substr(0, 20), junk));
        check("an all-zero key is refused",
              !TurnSealKey::fromText(std::string(43, 'A'), junk));
    }

    printf("\n=== sealing and opening ===\n");
    const std::vector<uint8_t> orders = bytes("{\"pendingMoveOrders\":[{\"from\":1,\"to\":2}]}");

    std::vector<uint8_t> sealed;
    check("orders seal", turnSeal(key, kTurn, kPsid, orders, sealed));
    check("the sealed form is larger than the plaintext",
          sealed.size() == orders.size() + 12 + 16,
          std::to_string(sealed.size()));

    // Confidentiality: whoever runs the store must not be able to read this.
    {
        const std::string asText(sealed.begin(), sealed.end());
        check("the plaintext is not visible in the sealed bytes",
              asText.find("pendingMoveOrders") == std::string::npos);
    }

    std::vector<uint8_t> opened;
    check("they open again", turnOpen(key, kTurn, kPsid, sealed, opened));
    check("and are unchanged", opened == orders);

    // A fresh nonce every time, or GCM breaks completely.
    {
        std::vector<uint8_t> again;
        turnSeal(key, kTurn, kPsid, orders, again);
        check("sealing twice does not produce the same bytes", again != sealed);
        std::vector<uint8_t> openedAgain;
        check("and both still open",
              turnOpen(key, kTurn, kPsid, again, openedAgain) && openedAgain == orders);
    }

    check("empty orders seal and open", [&] {
        std::vector<uint8_t> s, o;
        return turnSeal(key, kTurn, kPsid, {}, s) &&
               turnOpen(key, kTurn, kPsid, s, o) && o.empty();
    }());

    printf("\n=== what must not open ===\n");

    {
        std::vector<uint8_t> o;
        check("a different key does not open it",
              !turnOpen(other, kTurn, kPsid, sealed, o));
    }
    {
        // Replay onto another turn. This is the attack the binding exists for.
        std::vector<uint8_t> o;
        check("it does not open as a different turn",
              !turnOpen(key, kTurn + 1, kPsid, sealed, o));
    }
    {
        // Attribution to another player, likewise.
        std::vector<uint8_t> o;
        check("it does not open as a different player",
              !turnOpen(key, kTurn, "psid_player_0002______", sealed, o));
    }
    {
        // The length prefixes in the associated data exist so that these two
        // splits cannot collide into the same authenticated bytes.
        std::vector<uint8_t> a, b, o;
        turnSeal(key, 12, "ab", orders, a);
        check("a turn/psid split cannot be traded for another",
              !turnOpen(key, 1, "2ab", a, o));
    }

    // Every single-bit flip must be caught, anywhere in the blob -- nonce,
    // ciphertext or tag.
    {
        bool allCaught = true;
        for (size_t i = 0; i < sealed.size(); i++) {
            std::vector<uint8_t> t = sealed;
            t[i] ^= 0x01;
            std::vector<uint8_t> o;
            if (turnOpen(key, kTurn, kPsid, t, o)) {
                allCaught = false;
                printf("        a flip at byte %zu was not caught\n", i);
            }
        }
        check("every single-bit change is caught", allCaught);
    }

    // Truncation at every length, which is what a half-written blob looks like.
    {
        bool allRefused = true;
        for (size_t n = 0; n < sealed.size(); n++) {
            std::vector<uint8_t> t(sealed.begin(), sealed.begin() + (long)n);
            std::vector<uint8_t> o;
            if (turnOpen(key, kTurn, kPsid, t, o)) {
                allRefused = false;
                printf("        a %zu-byte prefix opened\n", n);
            }
        }
        check("every truncation is refused", allRefused);
    }

    {
        std::vector<uint8_t> o;
        check("empty input is refused", !turnOpen(key, kTurn, kPsid, {}, o));
        check("an unset key seals nothing",
              !turnSeal(TurnSealKey{}, kTurn, kPsid, orders, o));
        check("an unset key opens nothing",
              !turnOpen(TurnSealKey{}, kTurn, kPsid, sealed, o));
    }

    // ------------------------------------------------ the host's copy ----
    //
    // A long-form host closes the game between turns, so a key that did not
    // survive the process would mean every submission made while the host was
    // away failed to open -- which the turn logic would faithfully report as
    // every player having sent nothing.
    printf("\n=== the key on disk ===\n");
    {
        // Relative, so this works the same on the Windows runner as on a Mac:
        // there is no portable temp directory here without <filesystem>, and
        // the test already runs from a build directory it may write to.
        const std::string save = "od_seal_test_save.odsv";
        ::remove(turnSealKeyPathFor(save).c_str());

        check("the path sits beside the save, in its own file",
              turnSealKeyPathFor(save) == save + ".odkey");

        TurnSealKey loaded;
        check("a key that was never written does not load",
              !turnSealKeyLoad(save, loaded));

        check("a key can be written", turnSealKeySave(save, key));
        check("and read back", turnSealKeyLoad(save, loaded));
        check("byte for byte",
              memcmp(loaded.bytes, key.bytes, sizeof(key.bytes)) == 0);

        // The point of persisting it at all: orders sealed before the host
        // closed must still open after it comes back.
        {
            std::vector<uint8_t> opened;
            check("orders sealed earlier still open under the reloaded key",
                  turnOpen(loaded, kTurn, kPsid, sealed, opened) &&
                  opened == orders);
        }

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        {
            // The reason this file exists separately from `.odhost` at all: it
            // is the one secret in the long-form design, and anyone who reads
            // it can open every player's orders for every turn.
            struct stat st{};
            const bool statted = ::stat(turnSealKeyPathFor(save).c_str(), &st) == 0;
            check("the key file was created", statted);
            check("and no other user on this machine can read it",
                  statted && (st.st_mode & (S_IRWXG | S_IRWXO)) == 0);
        }
#endif

        check("an unset key is not written", !turnSealKeySave(save, TurnSealKey{}));
        check("and no path means no write", !turnSealKeySave("", key));

        ::remove(turnSealKeyPathFor(save).c_str());
        check("once removed it no longer loads", !turnSealKeyLoad(save, loaded));
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
