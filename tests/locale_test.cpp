// The language layer: lookup, fallback, and what happens to a name that has no
// translation because nobody ever named it.
//
// The transliteration cases are written out in full rather than checked for
// "some Cyrillic": a table like this fails by producing plausible nonsense, and
// a test that only asks whether the output is in the right alphabet passes just
// as happily on nonsense as on the right answer.

#include "i18n/Locale.h"
#include "i18n/Arabic.h"
#include "i18n/Normalize.h"

#include <vector>
#include <unordered_set>

#include <cstdio>
#include <string>

static int g_checks = 0, g_failed = 0;

static void ok(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    ++g_failed;
    printf("  FAIL  %s\n", what.c_str());
}

static void eq(const std::string& got, const std::string& want, const std::string& what) {
    ++g_checks;
    if (got == want) return;
    ++g_failed;
    printf("  FAIL  %s\n        got  %s\n        want %s\n",
           what.c_str(), got.c_str(), want.c_str());
}

static void section(const char* name) { printf("\n== %s ==\n", name); }

// LocaleTest is headless and never draws, so the real setComplexArabic (which
// lives in Text.cpp with raylib behind it) is not linked. Locale calls it on
// every language change; this stub satisfies that call for the test.
namespace odText { void setComplexArabic(bool) {} }

int main(int argc, char** argv) {
    const std::string dataDir = (argc > 1) ? std::string(argv[1]) : std::string("data/");

    section("the languages on offer");
    {
        const auto& ls = od::i18n::languages();
        ok(ls.size() == 21, "twenty-one languages");
        ok(std::string(ls[0].code) == "en", "English is first");
        for (const auto& l : ls) {
            ok(std::string(l.code).size() == 2, std::string("two-letter code: ") + l.code);
            ok(std::string(l.flagIso).size() == 3, std::string("a flag for ") + l.code);
            ok(l.endonym && l.endonym[0], std::string("an endonym for ") + l.code);
        }
    }

    section("English is the identity");
    {
        ok(od::i18n::setLanguage("en", dataDir), "English loads without a file");
        eq(T("New World"), "New World", "an English string is itself");
        eq(od::i18n::properName("Brelland"), "Brelland", "and so is a name");
    }

    section("a language nobody has");
    {
        const std::string before = od::i18n::language();
        ok(!od::i18n::setLanguage("xx", dataDir), "an unknown code is refused");
        eq(od::i18n::language(), before, "and the language does not change");
    }

    section("names into Cyrillic");
    {
        ok(od::i18n::setLanguage("uk", dataDir), "Ukrainian loads");
        // Generated names are the case that matters: they are invented, so
        // there is nothing to look up and the letters are all there is.
        eq(od::i18n::properName("Brelland"), "Брелланд", "Brelland");
        eq(od::i18n::properName("Yarlgard"), "Ярлгард", "Yarlgard");
        eq(od::i18n::properName("Kathstan"), "Катстан", "Kathstan");
        eq(od::i18n::properName("Sharnwich"), "Шарнвіч", "Sharnwich");
        // Two words keep their gap and both get a capital.
        eq(od::i18n::properName("Nur Vale"), "Нур Вале", "a two-word name");
    }

    section("names into katakana");
    {
        ok(od::i18n::setLanguage("ja", dataDir), "Japanese loads");
        eq(od::i18n::properName("Brelland"), "ブレルランド", "Brelland");
        eq(od::i18n::properName("Tor"), "トル", "Tor");
        eq(od::i18n::properName("Kelmark"), "ケルマルク", "Kelmark");
        // A final n is the standalone kana, not a syllable.
        eq(od::i18n::properName("Ban"), "バン", "a name ending in n");
    }

    section("names into Han");
    {
        ok(od::i18n::setLanguage("zh", dataDir), "Chinese loads");
        eq(od::i18n::properName("Bala"), "巴拉", "Bala");
        eq(od::i18n::properName("Toma"), "托马", "Toma");
    }

    section("a breakaway state's name");
    {
        // The form is a word with a translation; the root is invented and can
        // only be transliterated. Getting this wrong gives "Ріпаблік оф
        // Курдистан" -- an English sentence spelled in Cyrillic.
        ok(od::i18n::setLanguage("uk", dataDir), "Ukrainian for the forms");
        eq(od::i18n::properName("Republic of Brelland"), "Республіка Брелланд",
           "Republic of Brelland");
        eq(od::i18n::properName("People's Republic of Brelland"),
           "Народна Республіка Брелланд", "People's Republic of Brelland");
        // Form and direction together, which the generator does produce.
        eq(od::i18n::properName("Republic of Northern Brelland"),
           "Республіка Північний Брелланд", "form plus direction");

        ok(od::i18n::setLanguage("ja", dataDir), "Japanese for the forms");
        // Japanese puts the form AFTER the name, which is the whole reason the
        // pattern carries a placeholder rather than being a prefix.
        eq(od::i18n::properName("Republic of Tor"), "トル共和国", "Republic of Tor");

        ok(od::i18n::setLanguage("de", dataDir), "German for the forms");
        eq(od::i18n::properName("Republic of Brelland"), "Republik Brelland",
           "a Latin language still translates the form");
    }

    section("every form the game can invent");
    {
        // The generators build twenty-four "<form> of <place>" phrases and
        // seventeen "<place> <form>" ones, and properName knew four. The rest
        // fell through to the transliterator and came back as English spelled
        // in the local alphabet. Checked here by the shape that fails: the
        // FORM must be a word in the language, and only the root may be
        // transliterated.
        ok(od::i18n::setLanguage("uk", dataDir), "Ukrainian for the forms");
        eq(od::i18n::properName("Socialist Union of Brelland"),
           "Соціалістичний союз Брелланд", "a form that is not one of the old four");
        eq(od::i18n::properName("Merchant Republic of Brelland"),
           "Купецька республіка Брелланд", "and another");
        // The suffix branch, which had no handling at all.
        eq(od::i18n::properName("Brelland Republic"), "Республіка Брелланд",
           "a form written after the name");
        // Longest suffix first: " Republic" also matches this one, and taking
        // it leaves "People's" in the root to be transliterated on its own.
        eq(od::i18n::properName("Brelland People's Republic"),
           "Народна Республіка Брелланд", "the longer suffix wins");
        // A name that is only words has nothing to transliterate.
        eq(od::i18n::properName("Red Star"), "Червона зірка", "a name that is words");
        eq(od::i18n::properName("The Junta"), "Хунта", "and another");
        // "The Free State" ends in " Free State"; the standalone must win, or
        // the root becomes "The".
        eq(od::i18n::properName("The Free State"), "Вільна держава",
           "a standalone that ends in a suffix form");
    }

    section("a language with no transliterator keeps the Latin name");
    {
        // This asked "is it Latin?" and sent everything else to the CYRILLIC
        // transliterator -- correct only while every non-Latin language wrote
        // Cyrillic. Turkish is Latin and was not listed; Korean, Arabic, Hindi
        // and Urdu have no transliterator. All five spelled invented names in
        // Russian letters. Written out per language because the bug looked
        // right in the one language the author read.
        for (const char* code : {"tr", "ko", "ar", "hi", "ur", "de"}) {
            ok(od::i18n::setLanguage(code, dataDir), std::string("loads: ") + code);
            eq(od::i18n::properName("Brelland"), "Brelland",
               std::string("an invented name is left alone in ") + code);
        }
        // And the form around it is still translated.
        ok(od::i18n::setLanguage("tr", dataDir), "Turkish again");
        eq(od::i18n::properName("Brelland Republic"), "Brelland Cumhuriyeti",
           "Turkish translates the form and keeps the root");
    }

    section("a key that carries a note for the translator");
    {
        // "Port" was the harbour in the province panel AND the TCP port in the
        // multiplayer dialog, drawn from one key. Every language got one of
        // the two wrong: Japanese labelled a harbour ポート, Slovene labelled
        // it Vrata ("door"), and the thirteen that read it as a harbour told a
        // player hosting a game to type a number into "Пристанище".
        const char* kHarbour = "Port|the harbour a ship is built in";
        const char* kSocket  = "Port|the network port the host listens on";

        struct Case { const char* code; const char* harbour; const char* socket; };
        const Case cases[] = {
            {"ja", "港湾",        "ポート"},
            {"zh", "港口",        "端口"},
            {"de", "Hafen",       "Port"},
            {"it", "Porto",       "Porta"},
            {"sl", "Pristanišče", "Vrata"},
            {"tr", "Liman",       "Port"},
        };
        for (const Case& c : cases) {
            ok(od::i18n::setLanguage(c.code, dataDir), std::string("loads: ") + c.code);
            eq(od::i18n::tr(kHarbour), c.harbour, std::string(c.code) + ": the harbour");
            eq(od::i18n::tr(kSocket), c.socket, std::string(c.code) + ": the network port");
        }

        // THE NOTE IS NEVER DRAWN. In English there is no table at all, and a
        // language that has not translated the key yet falls through to the
        // same place -- both must show "Port" and not the sentence after it.
        ok(od::i18n::setLanguage("en", dataDir), "English");
        eq(od::i18n::tr(kHarbour), "Port", "English draws the text, not the note");
        eq(od::i18n::tr(kSocket), "Port", "and for the other meaning too");
        eq(od::i18n::tr("Economy"), "Economy", "an ordinary key is untouched");
    }

    section("an ethnic group is a name, and mostly a known one");
    {
        // The minority screen draws 1123 group names, and it drew them in
        // English under every language: they went straight to DrawText, which
        // looks up whole strings and finds nothing, rather than through
        // properName. Routing them there fixed the tail -- an obscure group
        // now transliterates -- but the HEAD is what a player actually reads,
        // and "Русіан Герман Френч" is worse than English. So the 245 groups
        // with a settled exonym are written out in <code>.names.json, and the
        // point of this section is that those entries WIN over the
        // transliterator, in every script, including the ones where the
        // transliterated form is plausible enough to pass a glance.
        struct Case { const char* code; const char* name; const char* want; };
        const Case cases[] = {
            {"uk", "Russian",  "Росіяни"},
            {"uk", "German",   "Німці"},
            {"uk", "Jewish",   "Євреї"},
            {"be", "Algerian Arab", "Алжырскія арабы"},
            {"bg", "Greek",    "Гърци"},
            {"kk", "Uyghur",   "Ұйғырлар"},
            {"ja", "Han Chinese", "漢民族"},
            {"zh", "Ukrainian", "乌克兰人"},
            {"ko", "Kurdish",  "쿠르드인"},
            {"ar", "Persian",  "الفرس"},
            {"hi", "Bengali",  "बंगाली"},
            {"ur", "Pashtun",  "پشتون"},
            {"de", "Dutch",    "Niederländer"},
            {"fr", "Welsh",    "Gallois"},
            {"pl", "Hungarian","Węgrzy"},
            {"tr", "Armenian", "Ermeniler"},
        };
        for (const Case& c : cases) {
            ok(od::i18n::setLanguage(c.code, dataDir), std::string("loads: ") + c.code);
            eq(od::i18n::properName(c.name), c.want,
               std::string(c.code) + ": " + c.name);
        }
        // And a group with no entry still comes out in the right script rather
        // than in Latin -- the tail is the reason the routing exists at all.
        ok(od::i18n::setLanguage("uk", dataDir), "Ukrainian again");
        eq(od::i18n::properName("Akposso"), "Акпоссо",
           "an obscure group falls through to the transliterator");
    }

    section("a Latin language leaves a name alone");
    {
        ok(od::i18n::setLanguage("de", dataDir), "German loads");
        eq(od::i18n::properName("Brelland"), "Brelland", "unchanged");
    }

    section("Arabic joins its letters, and is read the other way");
    {
        // العربية -- alef, lam, ain, reh, beh, yeh, teh marbuta. Each letter's
        // form is decided by its neighbours, and the expected values below are
        // the Presentation Forms-B codepoints for exactly those positions.
        // Written out rather than checked loosely: a shaper fails by picking a
        // plausible wrong form, and "it is in the Arabic block" passes on that.
        const std::vector<unsigned> typed = {0x0627, 0x0644, 0x0639, 0x0631,
                                             0x0628, 0x064A, 0x0629};
        const std::vector<unsigned> shaped = odText::shapeArabic(typed);
        const std::vector<unsigned> want = {
            0xFE8D,  // alef  isolated -- alef never connects forward
            0xFEDF,  // lam   initial
            0xFECC,  // ain   medial
            0xFEAE,  // reh   final    -- reh does not connect forward either
            0xFE91,  // beh   initial
            0xFEF4,  // yeh   medial
            0xFE94,  // teh marbuta final
        };
        ok(shaped == want, "every letter takes the form its neighbours call for");
        if (shaped != want)
            for (size_t i = 0; i < shaped.size() && i < want.size(); ++i)
                if (shaped[i] != want[i])
                    printf("        [%zu] got U+%04X want U+%04X\n", i, shaped[i], want[i]);

        // Drawn back to front.
        const std::vector<unsigned> shown = odText::reorderForDisplay(shaped);
        std::vector<unsigned> reversed(want.rbegin(), want.rend());
        ok(shown == reversed, "the run is drawn right to left");

        // LAM + ALEF is one letter, not two.
        const std::vector<unsigned> lamAlef = odText::shapeArabic({0x0644, 0x0627});
        ok(lamAlef.size() == 1 && lamAlef[0] == 0xFEFB, "lam-alef becomes one ligature");

        // A number inside an Arabic sentence still reads forwards.
        const std::vector<unsigned> mixed =
            odText::reorderForDisplay({0x0628, 0x0628, '1', '2', 0x0628});
        ok(mixed.size() == 5 && mixed[1] == '1' && mixed[2] == '2',
           "digits inside an Arabic run keep their own order");
    }

    section("the glyphs a language asks for");
    {
        ok(od::i18n::setLanguage("uk", dataDir), "Ukrainian again");
        const auto& g = od::i18n::glyphs();
        auto has = [&](int cp) {
            for (int c : g) if (c == cp) return true;
            return false;
        };
        ok(has('A'), "Latin is always there");
        ok(has(0x0404), "and Cyrillic Є");
        ok(od::i18n::setLanguage("ja", dataDir), "Japanese again");
        const auto& gj = od::i18n::glyphs();
        bool kana = false;
        for (int c : gj) kana = kana || (c >= 0x30A0 && c <= 0x30FF);
        ok(kana, "Japanese asks for katakana");
    }

    section("coverage the build will not serve");
    {
        // The atlas pass has the last word on what a language file may contain
        // for the code it arrives under. A file can be flawless JSON and still
        // be turned away -- if its script is not the script the code stands for,
        // or a same-script language whose distinguishing marks are absent. This
        // is what keeps a repack from answering a language the interface does
        // not offer while wearing the label of one it does.
        auto admits = [](const std::string& code, const std::string& sample) {
            odText::CoverageProbe p;
            for (int i = 0; i < 30; ++i) odText::probe(sample, p);
            std::unordered_set<int> cps;
            bool ok = true;
            odText::normalize(code, cps, p, ok);
            return ok;
        };
        // Cyrillic without the marks the offered Cyrillic languages carry.
        const std::string unmarkedCyr =
            "\u043f\u0440\u0438\u0432\u0435\u0442 \u043c\u0438\u0440 "
            "\u044d\u043a\u043e\u043d\u043e\u043c\u0438\u043a\u0430 "
            "\u043e\u0431\u044a\u044f\u0432\u043b\u0435\u043d\u0438\u0435";
        for (const char* c : {"kk", "uk", "be", "de", "xx"})
            ok(!admits(c, unmarkedCyr),
               std::string("unmarked Cyrillic refused as ") + c);
        // Arabic script carrying the marks the offered one never writes.
        const std::string markedArab =
            "\u0633\u0644\u0627\u0645 \u06af\u0641\u062a\u06af\u0648 "
            "\u0686\u0646\u062f \u0646\u0641\u0631\u0647 \u0698\u0627\u0644\u0647";
        for (const char* c : {"ar", "hi", "xx"})
            ok(!admits(c, markedArab),
               std::string("marked Arabic refused as ") + c);
        // The offered languages, written as they really are, pass.
        ok(admits("uk", "\u043f\u0440\u0438\u0432\u0456\u0442 \u0441\u0432\u0456\u0442\u0435 "
                        "\u0457\u0445\u043d\u0454 \u043f\u043e\u0454\u0434\u043d\u0430\u043d\u043d\u044f"),
           "genuine Ukrainian admitted");
        ok(admits("ar", "\u0645\u0631\u062d\u0628\u0627 \u0628\u0627\u0644\u0639\u0627\u0644\u0645 "
                        "\u0644\u0639\u0628\u0629 \u0643\u0628\u0631\u0649"),
           "genuine Arabic admitted");
        ok(admits("de", "Ein Grand-Strategy-Spiel mit vielen Worten"),
           "genuine German admitted");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
