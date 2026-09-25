// The on-screen keyboard's layout: what a tap at a place produces.
//
// Android has no keyboard this game can reach, so every text field there was
// unusable -- including the one an invite code goes in, which is how a player
// joins a game at all. This is the half of the answer that can be checked
// without a screen: where the keys are, what they type, and that every
// character a player needs to type is reachable.
//
// What it cannot check is how it looks or whether a thumb can hit it. The
// screenshot tour photographs it (shot "keyboard"), and a phone is the only
// real answer to the second.

#include "TouchKeyboard.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(const char* what, bool ok, const std::string& detail = {}) {
    printf("  %-64s %s", what, ok ? "ok" : "FAILED");
    if (!ok && !detail.empty()) printf("  --  %s", detail.c_str());
    printf("\n");
    g_checks++;
    if (!ok) g_failures++;
}

/** The character a tap at the centre of the key holding `wanted` produces. */
const osk::Key* find(char wanted, bool shifted, bool symbols) {
    for (const osk::Key& k : osk::layout(shifted, symbols))
        if (k.ch == wanted) return &k;
    return nullptr;
}

const osk::Key* findSpecial(osk::Special s, bool shifted, bool symbols) {
    for (const osk::Key& k : osk::layout(shifted, symbols))
        if (k.special == s) return &k;
    return nullptr;
}

}  // namespace

int main() {
    printf("The keyboard the game draws for itself\n");

    printf("\n== every key is somewhere, and only one key is anywhere ==\n");
    for (bool symbols : {false, true}) {
        for (bool shifted : {false, true}) {
            const auto& keys = osk::layout(shifted, symbols);
            check("the layout has keys", !keys.empty());

            bool inside = true, overlap = false;
            for (size_t i = 0; i < keys.size(); ++i) {
                const osk::Key& a = keys[i];
                if (a.x < 0 || a.x + a.w > osk::kGridWidth + 0.001f) inside = false;
                if (a.row < 0 || a.row >= osk::kGridRows) inside = false;
                for (size_t j = i + 1; j < keys.size(); ++j) {
                    const osk::Key& b = keys[j];
                    if (a.row != b.row) continue;
                    if (a.x < b.x + b.w - 0.001f && b.x < a.x + a.w - 0.001f) overlap = true;
                }
            }
            check("every key is on the board", inside);
            check("and no two keys are in the same place", !overlap);
        }
    }

    printf("\n== a tap lands on the key it looks like it landed on ==\n");
    {
        const osk::Key* a = find('A', true, false);
        check("there is an A when shifted", a != nullptr);
        if (a) {
            const osk::Key* hit = osk::keyAt(a->x + a->w / 2, a->row + 0.5f, true, false);
            check("tapping the middle of it types A", hit && hit->ch == 'A');
            // The edges belong to it too: a key you can only hit in the centre
            // is a key a thumb misses.
            const osk::Key* left = osk::keyAt(a->x + 0.02f, a->row + 0.02f, true, false);
            const osk::Key* right = osk::keyAt(a->x + a->w - 0.02f, a->row + 0.98f, true, false);
            check("and so does its top-left corner", left && left->ch == 'A');
            check("and its bottom-right", right && right->ch == 'A');
        }
        check("a tap below the last row hits nothing",
              osk::keyAt(1.0f, osk::kGridRows + 0.5f, true, false) == nullptr);
        check("and one past the right edge hits nothing",
              osk::keyAt(osk::kGridWidth + 0.5f, 0.5f, true, false) == nullptr);
    }

    printf("\n== what a player has to be able to type ==\n");
    {
        // An invite code: four, a dash, four, from the unambiguous alphabet.
        // If any of these is unreachable, a player cannot join a game at all.
        const std::string code = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
        bool allThere = true;
        std::string missing;
        for (char c : code) {
            if (!find(c, true, false)) { allThere = false; missing += c; }
        }
        check("every character an invite code can contain is on the letters board",
              allThere, "missing: " + missing);

        // A tunnel address, which is what a host hands out when there is no
        // relay: wss://some-words-here.trycloudflare.com
        const std::string address = ":/.-";
        bool addressable = true;
        std::string absent;
        for (char c : address) {
            if (!find(c, false, true)) { addressable = false; absent += c; }
        }
        check("and every one a tunnel address needs is on the symbols board",
              addressable, "missing: " + absent);

        bool digits = true;
        for (char c = '0'; c <= '9'; ++c) if (!find(c, false, true)) digits = false;
        check("as are the digits", digits);
    }

    printf("\n== the keys that are not letters ==\n");
    {
        for (auto [what, special] : {
                 std::pair<const char*, osk::Special>{"backspace", osk::Special::Backspace},
                 {"enter", osk::Special::Enter},
                 {"shift", osk::Special::Shift},
                 {"the symbols switch", osk::Special::Symbols},
                 {"space", osk::Special::Space},
                 {"a way to put it away", osk::Special::Close},
             }) {
            check((std::string("there is ") + what).c_str(),
                  findSpecial(special, true, false) != nullptr);
        }
        const osk::Key* space = findSpecial(osk::Special::Space, true, false);
        check("space types a space", space && space->ch == ' ');
        const osk::Key* back = findSpecial(osk::Special::Backspace, true, false);
        check("backspace types nothing", back && back->ch == 0);
    }

    printf("\n== shift is the same board in the other case ==\n");
    {
        const auto& lower = osk::layout(false, false);
        const auto& upper = osk::layout(true, false);
        check("the two cases have the same keys in the same places",
              lower.size() == upper.size());
        bool sameShape = lower.size() == upper.size();
        bool casesDiffer = false;
        for (size_t i = 0; sameShape && i < lower.size(); ++i) {
            if (lower[i].x != upper[i].x || lower[i].row != upper[i].row ||
                lower[i].w != upper[i].w || lower[i].special != upper[i].special)
                sameShape = false;
            if (lower[i].ch >= 'a' && lower[i].ch <= 'z' &&
                upper[i].ch == lower[i].ch - 'a' + 'A')
                casesDiffer = true;
        }
        check("nothing moves when shift is pressed", sameShape);
        check("but the letters change case", casesDiffer);
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
