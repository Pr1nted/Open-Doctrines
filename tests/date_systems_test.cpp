// Pins the calendar conversions in src/DateSystems.cpp. Links only that .cpp --
// no raylib, no i18n, no world -- so the arithmetic and the script tables are
// tested in isolation. tr is passed as nullptr, so labels come back as their
// English/romanised base; the render layer substitutes the active language.
//
// Anchors checkable against outside knowledge:
//   * 1 AD is the sexagenary year 辛酉 (xīn yǒu), 1984 AD is 甲子 (jiǎzǐ);
//   * Rome's founding, 753 BC, is 1 AUC;
//   * the BC->AD boundary has no year zero, so 1 BC and 1 AD are adjacent.

#include "DateSystems.h"

#include <cstdio>
#include <string>

using od::DateSystem;
using od::Script;
using od::ParsedDate;
using od::parseMapDate;
using od::astronomicalYear;
using od::yearField;

static int g_fail = 0;

static void eq(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::printf("FAIL %-26s got \"%s\"  want \"%s\"\n", what, got.c_str(), want.c_str());
        ++g_fail;
    }
}
static void eqi(long long got, long long want, const char* what) {
    if (got != want) { std::printf("FAIL %-26s got %lld  want %lld\n", what, got, want); ++g_fail; }
}
static void istrue(bool c, const char* what) {
    if (!c) { std::printf("FAIL %-26s (expected true)\n", what); ++g_fail; }
}

static std::string field(const char* raw, DateSystem s, Script sc = Script::Latin) {
    return yearField(parseMapDate(raw), s, sc, nullptr);
}

int main() {
    // ── parsing ──────────────────────────────────────────────────────────
    istrue(parseMapDate("March 50 BC").valid,          "parse: March 50 BC");
    istrue(!parseMapDate("1 September 1939 AD").valid, "parse rejects leading day");
    istrue(!parseMapDate("Spring of the Third Age").valid, "parse rejects prose");
    istrue(!parseMapDate("March 2000 XY").valid,       "parse rejects bad era");

    // ── astronomical axis: no gap at zero ────────────────────────────────
    eqi(astronomicalYear(parseMapDate("January 1 BC")),  0, "astro 1 BC");
    eqi(astronomicalYear(parseMapDate("January 1 AD")),  1, "astro 1 AD");
    eqi(astronomicalYear(parseMapDate("January 50 BC")),-49, "astro 50 BC");

    // ── 50 BC, romanised/English base (tr == nullptr, Latin) ─────────────
    eq(field("March 50 BC", DateSystem::BC_AD),    "50 BC",     "50BC bc_ad");
    eq(field("March 50 BC", DateSystem::BCE_CE),   "50 BCE",    "50BC bce_ce");
    eq(field("March 50 BC", DateSystem::AUC),      "704 AUC",   "50BC auc");
    eq(field("March 50 BC", DateSystem::JAPANESE), "Kōki 611",  "50BC koki base");
    eq(field("March 50 BC", DateSystem::JUCHE),    "50 BC",     "50BC juche pre-epoch falls back");
    eq(field("March 50 BC", DateSystem::HEBREW),   "3711 AM",   "50BC hebrew");
    eq(field("March 50 BC", DateSystem::HOLOCENE), "9951 HE",   "50BC holocene");
    eq(field("March 50 BC", DateSystem::CHINESE),  "Xinwei",    "50BC sexagenary latin");

    // ── the sexagenary year 50 BC in each native script ──────────────────
    eq(field("March 50 BC", DateSystem::CHINESE, Script::Han),        "辛未年",   "50BC han");
    eq(field("March 50 BC", DateSystem::CHINESE, Script::Hangul),     "신미년",   "50BC hangul");
    eq(field("March 50 BC", DateSystem::CHINESE, Script::Vietnamese), "Tân Mùi", "50BC vi");

    // ── anchors checkable against outside knowledge ──────────────────────
    eq(field("January 753 BC", DateSystem::AUC),   "1 AUC",   "753BC == 1 AUC");
    eq(field("January 1 AD",   DateSystem::AUC),   "754 AUC", "1AD auc");
    eq(field("January 1 AD",   DateSystem::CHINESE, Script::Han), "辛酉年", "1AD == xin-you");
    eq(field("January 1984 AD",DateSystem::CHINESE, Script::Han), "甲子年", "1984 == jia-zi");
    eq(field("January 1 AD",   DateSystem::HOLOCENE), "10001 HE", "1AD holocene");
    eq(field("January 1912 AD",DateSystem::JUCHE), "Juche 1", "1912 == juche 1");
    eq(field("January 2000 AD",DateSystem::HEBREW), "5760 AM", "2000 hebrew");

    // ── Kōki native form comes from the LABEL (tr), not the script param ──
    //    With nullptr tr it stays romanised whatever the script.
    eq(field("January 1 AD", DateSystem::JAPANESE, Script::Han), "Kōki 661", "koki script-indep");

    // ── BC/AD and BCE/CE on an AD year ───────────────────────────────────
    eq(field("July 44 AD", DateSystem::BC_AD),  "44 AD", "44AD bc_ad");
    eq(field("July 44 AD", DateSystem::BCE_CE), "44 CE", "44AD bce_ce");

    // ── script chosen from the interface language ────────────────────────
    istrue(od::scriptForLanguage("zh") == Script::Han,        "zh -> Han");
    istrue(od::scriptForLanguage("ja") == Script::Han,        "ja -> Han");
    istrue(od::scriptForLanguage("ko") == Script::Hangul,     "ko -> Hangul");
    istrue(od::scriptForLanguage("vi") == Script::Vietnamese, "vi -> Vietnamese");
    istrue(od::scriptForLanguage("en") == Script::Latin,      "en -> Latin");
    istrue(od::scriptForLanguage("de") == Script::Latin,      "de -> Latin");

    // ── token round-trip (config stability) ──────────────────────────────
    for (int i = 0; i < (int)DateSystem::COUNT; ++i) {
        auto s = (DateSystem)i;
        istrue(od::systemFromToken(od::systemToken(s)) == s, "token round-trip");
    }
    istrue(od::systemFromToken("nonsense") == DateSystem::BC_AD, "unknown token -> bc_ad");

    if (g_fail == 0) std::printf("date systems: all checks passed\n");
    return g_fail == 0 ? 0 : 1;
}
