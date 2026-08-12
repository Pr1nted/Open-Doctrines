#include "PoliticalIdentity.h"

#include <cmath>

namespace politid {

namespace {

// The ideological colours a restyled flag pulls toward. Not replacements --
// applyFlag() BLENDS toward these, so a country's own palette survives.
constexpr Color RED_LEFT   = {170,  30,  30, 255};   // communist / socialist
constexpr Color DARK_RIGHT = { 40,  42,  50, 255};   // nationalist
constexpr Color GOLD_LIB   = {210, 175,  60, 255};   // liberal

Color blend(Color a, Color b, float t) {
    auto mix = [&](unsigned char x, unsigned char y) {
        return (unsigned char)(x + (int)((y - x) * t));
    };
    return { mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), 255 };
}

bool usesEconAxis(IdeologyQuadrant q) {
    switch (q) {
        case IdeologyQuadrant::Communist: case IdeologyQuadrant::Socialist:
        case IdeologyQuadrant::Nationalist: case IdeologyQuadrant::Liberal:
        case IdeologyQuadrant::Collectivist: case IdeologyQuadrant::Capitalist:
            return true;
        default: return false;
    }
}
bool usesSocialAxis(IdeologyQuadrant q) {
    switch (q) {
        case IdeologyQuadrant::Communist: case IdeologyQuadrant::Socialist:
        case IdeologyQuadrant::Nationalist: case IdeologyQuadrant::Liberal:
        case IdeologyQuadrant::Authoritarian: case IdeologyQuadrant::Libertarian:
            return true;
        default: return false;
    }
}

IdeologyQuadrant quadrantOf(float econ, float soc, const PoliticalIdentity& current) {
    // social is +libertarian, so authoritarian is the negative half.
    //
    // An axis already being used holds on at a lower bar than it took to start
    // using it -- the same hysteresis the radius has, and needed just as badly.
    const float econBar = usesEconAxis(current.quadrant)   ? AXIS_MIN_EXIT : AXIS_MIN;
    const float socBar  = usesSocialAxis(current.quadrant) ? AXIS_MIN_EXIT : AXIS_MIN;
    const bool econCommitted = std::fabs(econ) >= econBar;
    const bool socCommitted  = std::fabs(soc)  >= socBar;

    // Only one axis has actually gone anywhere: name THAT, and say nothing
    // about the other. Deciding "communist" from econ = -5 because the sign
    // happened to be negative is a coin flip dressed as an ideology.
    if (socCommitted && !econCommitted)
        return soc < 0.0f ? IdeologyQuadrant::Authoritarian : IdeologyQuadrant::Libertarian;
    if (econCommitted && !socCommitted)
        return econ < 0.0f ? IdeologyQuadrant::Collectivist : IdeologyQuadrant::Capitalist;
    if (!econCommitted && !socCommitted)
        return IdeologyQuadrant::None;   // far from centre only diagonally; not a label

    const bool left = econ < 0.0f;
    const bool auth = soc  < 0.0f;
    if (left)  return auth ? IdeologyQuadrant::Communist   : IdeologyQuadrant::Socialist;
    return           auth ? IdeologyQuadrant::Nationalist : IdeologyQuadrant::Liberal;
}

}  // namespace

const char* quadrantName(IdeologyQuadrant q) {
    switch (q) {
        case IdeologyQuadrant::Communist:   return "communist";
        case IdeologyQuadrant::Socialist:   return "socialist";
        case IdeologyQuadrant::Nationalist: return "nationalist";
        case IdeologyQuadrant::Liberal:     return "liberal";
        case IdeologyQuadrant::Authoritarian: return "authoritarian";
        case IdeologyQuadrant::Libertarian:   return "libertarian";
        case IdeologyQuadrant::Collectivist:  return "collectivist";
        case IdeologyQuadrant::Capitalist:    return "free-market";
        default:                            return "centrist";
    }
}

PoliticalIdentity classify(float economic, float social, const PoliticalIdentity& current) {
    const float r = std::sqrt(economic * economic + social * social);

    // Which threshold applies depends on where we already are. Somewhere
    // between EXIT and ENTER, a country that has arrived stays and a country
    // that has not does not yet -- which is the whole point of the pair.
    const float keepOrEnter = current.expressed() ? EXIT_RADIUS : ENTER_RADIUS;
    if (r < keepOrEnter) return PoliticalIdentity{};

    PoliticalIdentity out;
    out.quadrant  = quadrantOf(economic, social, current);
    if (out.quadrant == IdeologyQuadrant::None) return PoliticalIdentity{};
    out.intensity = (r >= RADICAL_RADIUS) ? IdeologyIntensity::Radical
                                          : IdeologyIntensity::Committed;
    return out;
}

namespace {

// Forms of government that already appear in shipped country names. A name is
// stripped down to its GEOGRAPHIC core before a new form is applied, because
// prefixing blindly is how "Empire of Japan" became "Empire of Empire of
// Japan" and "Kingdom of Italy" became "Empire of Kingdom of Italy".
//
// Longest first: "People's Republic" must be tried before "Republic", or the
// leftover "People's" survives into the result.
const char* const FORMS[] = {
    "Mongolian People's Republic", "People's Republic", "Socialist Union",
    "National State", "Free Republic", "Free Communes", "Free State",
    "Federation", "Commonwealth", "Principality", "Sultanate", "Emirate",
    "Dominion", "Kingdom", "Republic", "Empire", "Duchy", "Union", "State",
};

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
std::string trim(std::string s) {
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back()  == ' ') s.pop_back();
    return s;
}

/**
 * The geographic core of a name, and whether it is an adjective.
 *
 * The ORIGINAL name settles the grammar exactly, so no guessing is needed:
 *
 *   "Empire of Japan"   -> core "Japan",  a noun      (it followed "of")
 *   "Slovak Republic"   -> core "Slovak", an adjective (it preceded the form)
 *   "Nepal"             -> core "Nepal",  a noun      (no form at all)
 *
 * Guessing from the ending cannot do this. Japan, Iran, Oman, Taiwan and Sudan
 * all end in "-an" and are nouns; Slovak is an adjective and ends in neither
 * "-an" nor "-ian". The first version of this got both of those wrong in the
 * same test run.
 */
struct NameCore { std::string text; bool adjective = false; };

NameCore geographicCore(const std::string& name) {
    // "<anything containing a form> of <Core>" -- catches "Imperial State of
    // Iran" and "Grand Duchy of X", where the form word is not at the start and
    // so is missed by the exact prefixes below.
    const std::string::size_type ofPos = name.rfind(" of ");
    if (ofPos != std::string::npos) {
        const std::string head = name.substr(0, ofPos);
        for (const char* f : FORMS)
            if (head.find(f) != std::string::npos)
                return { trim(name.substr(ofPos + 4)), false };
    }
    for (const char* f : FORMS) {
        const std::string form(f);
        // "<Form> of <Core>" -- what follows "of" is a noun.
        if (startsWith(name, form + " of "))
            return { trim(name.substr(form.size() + 4)), false };
        // "<Core> <Form>" -- what precedes a form is an adjective.
        if (endsWith(name, " " + form))
            return { trim(name.substr(0, name.size() - form.size() - 1)), true };
    }
    return { name, false };
}

/** The two halves of a form: the phrase used with "of", and the bare noun. */
struct FormWords { const char* phrase; const char* noun; };

FormWords formFor(const PoliticalIdentity& id) {
    const bool rad = (id.intensity == IdeologyIntensity::Radical);
    switch (id.quadrant) {
        case IdeologyQuadrant::Communist:
            return rad ? FormWords{"People's Republic", "People's Republic"}
                       : FormWords{"Socialist Union",   "Socialist Union"};
        case IdeologyQuadrant::Socialist:
            return rad ? FormWords{"Free Communes",     "Free Communes"}
                       : FormWords{"Federation",        "Federation"};
        case IdeologyQuadrant::Nationalist:
            return rad ? FormWords{"Empire",            "Empire"}
                       : FormWords{"National State",    "National State"};
        case IdeologyQuadrant::Liberal:
            return rad ? FormWords{"Free Republic",     "Free Republic"}
                       : FormWords{"Republic",          "Republic"};
        case IdeologyQuadrant::Authoritarian:
            return rad ? FormWords{"Autocracy",         "Autocracy"}
                       : FormWords{"State",             "State"};
        case IdeologyQuadrant::Libertarian:
            return rad ? FormWords{"Free Territories",  "Free Territories"}
                       : FormWords{"Free State",        "Free State"};
        case IdeologyQuadrant::Collectivist:
            return rad ? FormWords{"Workers' Republic", "Workers' Republic"}
                       : FormWords{"Cooperative",       "Cooperative"};
        case IdeologyQuadrant::Capitalist:
            return rad ? FormWords{"Merchant Republic", "Merchant Republic"}
                       : FormWords{"Free Market",       "Free Market"};
        default:
            return FormWords{"", ""};
    }
}

}  // namespace

std::string geographicCoreOf(const std::string& name) {
    return geographicCore(name).text;
}

std::string applyName(const std::string& rootName, const PoliticalIdentity& id) {
    if (!id.expressed() || rootName.empty()) return rootName;

    const NameCore core = geographicCore(rootName);
    if (core.text.empty()) return rootName;

    const FormWords w = formFor(id);
    if (!w.phrase[0]) return rootName;

    return core.adjective ? core.text + " " + w.noun
                          : std::string(w.phrase) + " of " + core.text;
}

FlagPattern applyFlag(const FlagPattern& originalFlag, const PoliticalIdentity& id) {
    FlagPattern f = originalFlag;
    if (!id.expressed()) return f;

    // A flag loaded from a file is an IMAGE, and there is nothing to restyle --
    // recolouring is a pattern operation. Rather than throw the country's real
    // flag away, the emblem is added over it and the image is kept.
    const bool isImage = !f.imagePath.empty();

    const bool radical = (id.intensity == IdeologyIntensity::Radical);
    const float pull   = radical ? 0.55f : 0.30f;   // how far the palette moves

    Color toward = RED_LEFT;
    SymbolType emblem = SymbolType::STAR_5;
    switch (id.quadrant) {
        case IdeologyQuadrant::Communist:
            toward = RED_LEFT;
            emblem = radical ? SymbolType::HAMMER_SICKLE : SymbolType::STAR_5;
            break;
        case IdeologyQuadrant::Socialist:
            toward = RED_LEFT;
            emblem = radical ? SymbolType::GEAR : SymbolType::STAR_5;
            break;
        case IdeologyQuadrant::Nationalist:
            toward = DARK_RIGHT;
            // Deliberately NOT the swastika, which exists in the vocabulary for
            // historical flags that shipped with it and is censorable. A
            // generated flag must not manufacture one.
            emblem = radical ? SymbolType::CROSSED_SWORDS : SymbolType::CROSS_LATIN;
            break;
        case IdeologyQuadrant::Liberal:
            toward = GOLD_LIB;
            emblem = radical ? SymbolType::SUN_RAYS : SymbolType::CIRCLE;
            break;
        // The single-axis identities. Their palettes pull less far than the
        // committed quadrants above, because a country that has gone a long way
        // on one axis and nowhere on the other is making a weaker statement.
        case IdeologyQuadrant::Authoritarian:
            toward = DARK_RIGHT;
            emblem = radical ? SymbolType::SWORD : SymbolType::DIAMOND;
            break;
        case IdeologyQuadrant::Libertarian:
            toward = GOLD_LIB;
            emblem = SymbolType::CIRCLE;
            break;
        case IdeologyQuadrant::Collectivist:
            toward = RED_LEFT;
            emblem = SymbolType::GEAR;
            break;
        case IdeologyQuadrant::Capitalist:
            toward = GOLD_LIB;
            emblem = radical ? SymbolType::STAR_5 : SymbolType::DISC;
            break;
        default:
            return f;
    }

    if (!isImage)
        for (Color& c : f.colors) c = blend(c, toward, pull);

    // One emblem, in the canton, at a size that reads at 256x128 and does not
    // swallow the flag. Replaces any emblem a previous restyle added -- but
    // applyFlag is always called with the ORIGINAL, so in practice this is the
    // first and only one.
    FlagSymbol sym;
    sym.type   = emblem;
    sym.colors = { radical ? Color{245, 225, 120, 255} : Color{240, 240, 240, 255} };
    sym.x      = 0.22f;
    sym.y      = 0.30f;
    sym.size   = radical ? 0.28f : 0.22f;
    f.symbols.push_back(sym);
    return f;
}

}  // namespace politid
