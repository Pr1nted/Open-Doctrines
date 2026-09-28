// PLAY BY PASTE: the turn block a human copies out of one game and into another.
//
// Long-form is the mode with no infrastructure at all. The host resolves a turn
// while everyone is away, the game shows a block of text, and somebody puts it
// in an email. A player pastes it, plays, and sends a block back. Four stores
// exist; this is the one that works when none of them do, and it is the one a
// player falls back to when something else breaks -- so it has to survive being
// carried by hand.
//
// WHAT THAT MEANS IN PRACTICE, and it is not the cryptography (tests/net_seal_test.cpp
// covers that): text does not arrive the way it left. It goes through mail
// clients, chat windows and browsers, which rewrite line endings, add quoting
// and wrap what they think is prose. A block that only decodes when it is
// byte-identical to what was produced is a feature that works on one machine.
//
// Build target: LongformPasteTest.

#include "net/TurnStore.h"
#include "net/TurnSeal.h"

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

/// A turn's worth of bytes: not text, and not short.
static std::vector<uint8_t> somePayload(size_t n = 700) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)((i * 31 + (i >> 3) * 7) & 0xFF);
    return v;
}

/// What a mail client does to a block on the way through.
static std::string toCRLF(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\n') o += '\r'; o += c; }
    return o;
}

int main() {
    const std::vector<uint8_t> turnBytes = somePayload();

    section("a turn block survives being copied");
    {
        const std::string block = turnStoreEncodeText("turn", 7, turnBytes);
        std::string what; uint32_t turn = 0; std::vector<uint8_t> back;
        ok(turnStoreDecodeText(block, what, turn, back), "it decodes at all");
        ok(what == "turn", "and says what it is", what);
        ok(turn == 7, "and which turn it belongs to", std::to_string(turn));
        ok(back == turnBytes, "and the bytes are the bytes");
    }

    section("...including through the things that carry it");
    {
        const std::string block = turnStoreEncodeText("turn", 7, turnBytes);
        std::string what; uint32_t turn = 0; std::vector<uint8_t> back;

        // A MAIL CLIENT, A BROWSER, OR WINDOWS. Any of them may hand back CRLF,
        // and a player on Windows pasting a block from a Mac host is the
        // ordinary case rather than the exotic one.
        ok(turnStoreDecodeText(toCRLF(block), what, turn, back) && back == turnBytes,
           "with Windows line endings");

        // Somebody types a sentence above it before sending.
        ok(turnStoreDecodeText("here you go, sorry for the delay\n\n" + block,
                               what, turn, back) && back == turnBytes,
           "with a covering note above it");

        // And a signature below.
        ok(turnStoreDecodeText(block + "\n-- \nsent from my phone\n",
                               what, turn, back) && back == turnBytes,
           "with a signature below it");

        // Selected with the mouse, which usually takes the trailing newline and
        // often takes a leading space.
        ok(turnStoreDecodeText("  " + block, what, turn, back) && back == turnBytes,
           "with the selection starting a little early");
    }

    section("and it refuses what it should");
    {
        const std::string block = turnStoreEncodeText("turn", 7, turnBytes);
        std::string what; uint32_t turn = 0; std::vector<uint8_t> back;

        // THE COMMONEST HUMAN ERROR: selecting the body and missing a --- line.
        const size_t cut = block.find('\n') + 1;
        ok(!turnStoreDecodeText(block.substr(cut), what, turn, back),
           "a block pasted without its header is refused");
        ok(!turnStoreDecodeText(block.substr(0, block.size() - 14), what, turn, back),
           "a block pasted without its footer is refused");
        ok(!turnStoreDecodeText("good luck out there", what, turn, back),
           "an ordinary sentence is refused");
        ok(!turnStoreDecodeText("", what, turn, back), "and so is nothing at all");
    }

    section("the whole exchange, host to player and back");
    {
        // 1. the host resolves a turn and shows a block
        const std::string turnBlock = turnStoreEncodeText("turn", 12, turnBytes);

        // 2. the player pastes it and gets the turn
        std::string what; uint32_t turn = 0; std::vector<uint8_t> got;
        ok(turnStoreDecodeText(toCRLF(turnBlock), what, turn, got) &&
               what == "turn" && turn == 12 && got == turnBytes,
           "the player receives the turn the host published");

        if (turnSealAvailable()) {
            // 3. the player seals their orders for that turn and shows a block
            TurnSealKey key{};
            ok(turnSealKeyGenerate(key), "the game has a key to seal with");
            const std::vector<uint8_t> orders = somePayload(120);
            const std::string psid = "p_7f3a91";
            std::vector<uint8_t> sealed;
            ok(turnSeal(key, 12, psid, orders, sealed), "the orders seal");
            const std::string orderBlock =
                turnStoreEncodeText(("orders " + psid).c_str(), 12, sealed);

            // 4. the host pastes it, and gets the orders back out
            std::string w2; uint32_t t2 = 0; std::vector<uint8_t> carried;
            ok(turnStoreDecodeText(toCRLF(orderBlock), w2, t2, carried),
               "the host receives the block");
            ok(w2 == "orders " + psid, "which says whose orders they are", w2);
            std::vector<uint8_t> opened;
            ok(turnOpen(key, t2, psid, carried, opened) && opened == orders,
               "and they open to exactly what the player sent");

            // AND THE ROUND TRIP DOES NOT LAUNDER A REPLAY. The text block is
            // not a security boundary -- the seal is -- so what matters is that
            // carrying orders by hand cannot turn last turn's into this turn's.
            std::vector<uint8_t> wrongTurn;
            ok(!turnOpen(key, 13, psid, carried, wrongTurn),
               "last turn's block, pasted again, does not open on this one");
        } else {
            printf("  skip  sealing is unavailable in this build\n");
        }
    }

    section("every long-form game gets a key, including the pasted one");
    {
        // THE BUG THIS EXISTS TO STOP COMING BACK. The host minted the order
        // key only when the store was NOT Manual -- and the same branch was
        // where it recorded which store it had chosen. So picking "paste" left
        // the host believing it was on the default store: it never showed a
        // block to copy, the player could not seal orders without a key, and
        // the host could not have opened them if they had. The mode did not
        // work in either direction, and nothing said so.
        //
        // Manual changes how the bytes are CARRIED, not what they are: they go
        // through the same seal as every other store, and a block pasted into a
        // chat window has a larger audience than a bucket with a URL, not a
        // smaller one.
        ok(longFormNeedsSealKey(0, TurnStoreKind::Manual),
           "a paste game needs a key, exactly like every other long-form game");
        ok(longFormNeedsSealKey(0, TurnStoreKind::DurableObject),
           "so does the default store");
        ok(longFormNeedsSealKey(0, TurnStoreKind::JsonBlob), "and jsonblob");
        ok(longFormNeedsSealKey(0, TurnStoreKind::R2), "and R2");

        // A timed game does not: the orders go down a live connection that is
        // already encrypted, to a host that is sitting there.
        ok(!longFormNeedsSealKey(120, TurnStoreKind::Manual),
           "a timed game needs none");
        ok(!longFormNeedsSealKey(86400, TurnStoreKind::DurableObject),
           "however long its turns are");
    }

    section("what a pasted block is routed to");
    {
        // Manual mode is the one where a PERSON is holding the bytes, so nearly
        // every outcome is a mistake somebody is about to make. Each wants a
        // different sentence, and the ORDER of the checks decides whether they
        // get a useful one.
        const bool HOST = true, PLAYER = false, KNOWN = true, STRANGER = false;

        ok(classifyPaste(false, "", 0, PLAYER, 4, KNOWN) == PasteAction::NotABlock,
           "something that did not decode is not a block");

        // The player's ordinary Monday.
        ok(classifyPaste(true, "turn", 5, PLAYER, 4, KNOWN) == PasteAction::ApplyTurn,
           "the turn after ours is the one we apply");
        ok(classifyPaste(true, "turn", 4, PLAYER, 4, KNOWN) == PasteAction::TurnNotNext,
           "the turn we already played is refused");
        ok(classifyPaste(true, "turn", 9, PLAYER, 4, KNOWN) == PasteAction::TurnNotNext,
           "and so is one from the future");

        // WHO BEFORE WHICH. A host pasting its own block back wants to be told
        // that, not that the number is wrong -- the number IS wrong, for a
        // reason that would only confuse them, because the host is always a
        // turn ahead of the block it just produced.
        ok(classifyPaste(true, "turn", 5, HOST, 4, KNOWN) == PasteAction::TurnIsForPlayers,
           "a host pasting its own turn block is told whose it is");
        ok(classifyPaste(true, "turn", 4, HOST, 4, KNOWN) == PasteAction::TurnIsForPlayers,
           "and told the same thing whatever the number");

        // Orders, which only ever go one way.
        ok(classifyPaste(true, "orders p_1", 5, HOST, 4, KNOWN) == PasteAction::OpenOrders,
           "orders from somebody in the game are opened");
        ok(classifyPaste(true, "orders p_1", 5, HOST, 4, STRANGER) ==
               PasteAction::OrdersFromStranger,
           "orders from a pseudonym we do not know are refused");
        ok(classifyPaste(true, "orders p_1", 5, PLAYER, 4, KNOWN) ==
               PasteAction::OrdersAreForHost,
           "a player pasting somebody's orders is told only the host does that");

        // A host is not stopped from taking orders for a turn other than the
        // one in progress, and does not need to be: the seal binds them to
        // their turn, and the lobby counts a submission only when
        // submittedTurn == turnNumber. Pinned because it looks like a missing
        // check and is not one.
        ok(classifyPaste(true, "orders p_1", 2, HOST, 9, KNOWN) == PasteAction::OpenOrders,
           "stale orders reach the seal rather than being guessed at here");

        ok(classifyPaste(true, "greetings", 5, HOST, 4, KNOWN) == PasteAction::Unknown,
           "a block this build does not know is refused");

        ok(pasteOrdersPsid("orders p_7f3a91") == "p_7f3a91", "the sender is read off");
        ok(pasteOrdersPsid("turn").empty(), "and a turn block has no sender");
    }

    section("a campaign, played entirely by pasting");
    {
        // Two turns, both directions, through the real codec and the real seal:
        // the host publishes, the player applies and answers, the host opens.
        // Nothing here is a mock -- what is missing is only the lobby and the
        // window, and those decide nothing.
        if (!turnSealAvailable()) {
            printf("  skip  sealing is unavailable in this build\n");
        } else {
            TurnSealKey key{};
            turnSealKeyGenerate(key);
            const std::string psid = "p_a10c44";
            int playerTurn = 0;          // where the player's game has got to
            bool wholeCampaign = true;

            for (uint32_t t = 1; t <= 2; ++t) {
                // the host resolves turn t and shows a block
                const std::vector<uint8_t> delta = somePayload(300 + t);
                const std::string block = turnStoreEncodeText("turn", t, delta);

                // the player pastes it -- through a mail client, as ever
                std::string what; uint32_t bt = 0; std::vector<uint8_t> got;
                const bool dec = turnStoreDecodeText(toCRLF(block), what, bt, got);
                if (classifyPaste(dec, what, bt, false, (uint32_t)playerTurn,
                                  false) != PasteAction::ApplyTurn ||
                    got != delta) { wholeCampaign = false; break; }
                playerTurn = (int)bt;

                // the player answers with orders for that turn
                const std::vector<uint8_t> orders = somePayload(60 + t);
                std::vector<uint8_t> sealed;
                if (!turnSeal(key, t, psid, orders, sealed)) { wholeCampaign = false; break; }
                const std::string reply =
                    turnStoreEncodeText(("orders " + psid).c_str(), t, sealed);

                // and the host reads them
                std::string w2; uint32_t t2 = 0; std::vector<uint8_t> carried;
                const bool dec2 = turnStoreDecodeText(toCRLF(reply), w2, t2, carried);
                if (classifyPaste(dec2, w2, t2, true, t, true) !=
                    PasteAction::OpenOrders) { wholeCampaign = false; break; }
                std::vector<uint8_t> opened;
                if (!turnOpen(key, t2, pasteOrdersPsid(w2), carried, opened) ||
                    opened != orders) { wholeCampaign = false; break; }
            }
            ok(wholeCampaign, "two turns, host to player and back, by paste alone");
            ok(playerTurn == 2, "and the player's game advanced both times",
               std::to_string(playerTurn));
        }
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
