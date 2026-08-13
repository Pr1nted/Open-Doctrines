#include "PoliticalIdentity.h"

#include <cmath>

namespace politid {

namespace {

/**
 * What a restyled palette moves toward, and the solid colour that goes with it.
 *
 * The HSL reasoning behind these fields lives on FlagRecolor in
 * FlagRenderer.h -- the transform itself is the renderer's, because most
 * countries fly a raster image and there is no palette here to rewrite.
 */
struct Restyle { FlagRecolor recolor; Color solid; };

//                          active shiftHue   hue    sat  light  shadeSat shadeLight weight
// Red keeps a colour at its dark end: a maroon-and-scarlet flag is a flag, and
// the two read against each other.
constexpr Restyle RED_LEFT   = { { true, true,   2.0f, 0.68f, 0.46f, 0.50f, 0.20f, 0.0f }, {158, 32, 32, 255} };
// Gold does NOT. Gold at low lightness is olive, so the dark end of a gold
// restyle goes almost neutral and the gold itself only appears where the flag
// was already light. That is the difference between a charcoal-and-gold Union
// Jack and a wet field.
constexpr Restyle GOLD_LIB   = { { true, true,  45.0f, 0.70f, 0.58f, 0.10f, 0.17f, 0.0f }, {194, 152, 44, 255} };
// Nationalism reads as a DARKENING of a country's own colours, not as a move
// toward a hue -- charcoal has no meaningful hue to move to, and forcing one
// turns every flag the same grey. This target keeps each colour where it is on
// the wheel and takes the light and the saturation out of it.
constexpr Restyle DARK_RIGHT = { { true, false,  0.0f, 0.28f, 0.34f, 0.10f, 0.15f, 0.0f }, { 38, 40, 48, 255} };

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
    "Merchant Republic", "Workers' Republic", "Free Territories",
    "Federation", "Confederation", "Commonwealth", "Principality",
    "Protectorate", "Sultanate", "Caliphate", "Shogunate", "Emirate",
    "Imamate", "Khanate", "Tsardom", "Dominion", "Mandate", "Cooperative",
    "Autocracy", "Kingdom", "Republic", "Emirates", "Empire", "Duchy",
    "Union", "Reich", "SFSR", "SSR",
    // Plurals and abbreviations before their singulars, and before the bare
    // words they contain: "United States" must not fall through to "State" and
    // come out as "United State".
    "Territories", "Territory", "States", "State", "Areas", "Rep.", "Is.",
};

// Words that qualify a form rather than name a place. They are stripped from
// the core after the form comes off, because they belonged to the government
// that just fell: the "Second" in "Second Polish Republic" numbered THAT
// republic, and carrying it forward produced the "Second Polish Socialist
// Union" -- a state naming itself after its predecessor's ordinal.
const char* const LEADING_QUALIFIERS[] = {
    "First", "Second", "Third", "Fourth", "Fifth",
    "Allied", "Occupied", "Provisional", "Greater", "Grand", "Imperial",
    "Royal", "Sublime", "Federal", "Federated", "Democratic", "Socialist",
    "People's", "Communist", "National", "Dem.", "Rep.", "FR",
};
const char* const TRAILING_QUALIFIERS[] = {
    "Democratic", "Socialist", "People's", "Communist", "Federal", "National",
};

// A "core" that is one of these is not a place. "United Kingdom" reduces to
// "United", "Soviet Union" to "Soviet", "Blocked Territory" to "Blocked" --
// none of which can carry a new form, and "People's Republic of United" is
// worse than leaving the name alone. A country whose name has no geographic
// noun in it keeps that name; only its flag changes.
const char* const NOT_A_PLACE[] = {
    "United", "Soviet", "Allied", "Occupied", "Blocked", "Free", "Democratic",
    "Socialist", "People's", "Communist", "Federal", "National", "Imperial",
    "Royal", "Provisional",
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

/** Takes the fallen government's qualifiers off a core, front and back. */
std::string stripQualifiers(std::string core) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (const char* q : LEADING_QUALIFIERS) {
            const std::string w = std::string(q) + " ";
            if (startsWith(core, w) && core.size() > w.size()) {
                core = trim(core.substr(w.size()));
                changed = true;
                break;
            }
        }
        for (const char* q : TRAILING_QUALIFIERS) {
            const std::string w = " " + std::string(q);
            if (endsWith(core, w) && core.size() > w.size()) {
                core = trim(core.substr(0, core.size() - w.size()));
                changed = true;
                break;
            }
        }
    }
    return core;
}

bool namesAPlace(const std::string& core) {
    if (core.empty()) return false;
    for (const char* w : NOT_A_PLACE)
        if (core == w) return false;

    // A core left dangling on a connective means the form came off the wrong
    // end of the name: "S. Geo. and the Is." matched the "Is." form and left
    // "S. Geo. and the", which went on to produce the "S. Geo. and the
    // People's Republic". If the remainder does not finish, it is not a name.
    const std::string::size_type sp = core.rfind(' ');
    const std::string last = (sp == std::string::npos) ? core : core.substr(sp + 1);
    for (const char* w : {"and", "the", "of", "de", "da", "del", "and the", "&"})
        if (last == w) return false;
    return true;
}

NameCore geographicCore(const std::string& name) {
    // "<anything containing a form> of <Core>" -- catches "Imperial State of
    // Iran" and "Grand Duchy of X", where the form word is not at the start and
    // so is missed by the exact prefixes below.
    const std::string::size_type ofPos = name.rfind(" of ");
    if (ofPos != std::string::npos) {
        const std::string head = name.substr(0, ofPos);
        for (const char* f : FORMS)
            if (head.find(f) != std::string::npos)
                return { stripQualifiers(trim(name.substr(ofPos + 4))), false };
    }
    for (const char* f : FORMS) {
        const std::string form(f);
        // "<Form> of <Core>" -- what follows "of" is a noun.
        if (startsWith(name, form + " of "))
            return { stripQualifiers(trim(name.substr(form.size() + 4))), false };
        // "<Core> <Form>" -- what precedes a form is an adjective.
        if (endsWith(name, " " + form))
            return { stripQualifiers(trim(name.substr(0, name.size() - form.size() - 1))), true };
    }
    // No form at all. The name is the place -- but a qualifier can still be
    // sitting in front of it ("Occupied Japan", "Dem. Rep. Congo"), and those
    // describe a situation rather than a country. "New Zealand" survives this
    // because "New" is not one of them; it is not a zealand that is new.
    return { stripQualifiers(name), false };
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
    // Falls back to the WHOLE name, not to the extracted fragment, when the
    // fragment does not name anywhere. The breakaway namer builds on this too,
    // and a secession from the United Kingdom must not call itself the
    // Democratic Alliance of United.
    const NameCore core = geographicCore(name);
    return namesAPlace(core.text) ? core.text : name;
}

std::string applyName(const std::string& rootName, const PoliticalIdentity& id) {
    if (!id.expressed() || rootName.empty()) return rootName;

    const NameCore core = geographicCore(rootName);
    // Not every name has a place in it. "United Kingdom" reduces to "United"
    // and "Soviet Union" to "Soviet"; neither can carry a form, and inventing
    // one produces a country that does not sound like anywhere. Those keep
    // their name and change only their flag, which is still the visible event.
    if (!namesAPlace(core.text)) return rootName;

    const FormWords w = formFor(id);
    if (!w.phrase[0]) return rootName;

    return core.adjective ? core.text + " " + w.noun
                          : std::string(w.phrase) + " of " + core.text;
}

uint32_t seedFromName(const std::string& name) {
    uint32_t h = 2166136261u;                    // FNV-1a, 32-bit
    for (unsigned char c : name) { h ^= c; h *= 16777619u; }
    return h;
}

namespace {

/** What a flag is charged with: the device, how they are arranged, how many. */
struct Charge { SymbolType emblem; SymbolLayout layout; int count; };

// Every vocabulary below has exactly this many entries.
constexpr unsigned CHARGES_PER_IDENTITY = 4;

/**
 * The charges an identity can take, and the one this country takes.
 *
 * FOUR PER IDENTITY, NOT ONE.
 *
 * A single fixed emblem per identity is correct for one country and wrong for a
 * map: every communist government in the world reached for the same white star,
 * and with two hundred countries on the board that reads as one flag repeated
 * rather than as a world of them. Each identity now has a small vocabulary and
 * a country picks from it by name, so its choice is arbitrary but never random
 * -- the same country makes the same choice on the host, on every client, and
 * on reload, which is what stops a flag changing when nothing about the country
 * did.
 *
 * Between them these use twenty-three of the symbols in data/symbols/, against
 * the eleven a fixed mapping could reach.
 *
 * NOT among them: the swastika, and not the Reichsadler either. Both ship for
 * the historical flags that carry them and both are censorable; neither is
 * something a procedure should be able to invent for a country that never had
 * one. Anything a generated flag can wear, it will eventually wear.
 */
/** Symbols a generated flag must not invent for a country that never had one. */
bool isHateSymbol(SymbolType t) {
    return t == SymbolType::SWASTIKA;
}

Charge chargeFor(IdeologyQuadrant q, bool radical, uint32_t seed, bool avoidHateSymbols) {
    static const Charge COMMUNIST_C[] = {
        {SymbolType::STAR_5, SymbolLayout::SINGLE, 0},
        {SymbolType::STAR_5, SymbolLayout::ARC,    3},
        {SymbolType::TORCH,  SymbolLayout::SINGLE, 0},
        {SymbolType::HAMMER, SymbolLayout::SINGLE, 0},
    };
    static const Charge COMMUNIST_R[] = {
        {SymbolType::HAMMER_SICKLE, SymbolLayout::ESCORTED, 4},
        {SymbolType::HAMMER_SICKLE, SymbolLayout::SINGLE,   0},
        {SymbolType::STAR_5,        SymbolLayout::ESCORTED, 5},
        {SymbolType::HAMMER,        SymbolLayout::ESCORTED, 4},
    };
    static const Charge SOCIALIST_C[] = {
        {SymbolType::STAR_5, SymbolLayout::ROW,    3},
        {SymbolType::ROSE,   SymbolLayout::SINGLE, 0},
        {SymbolType::GEAR,   SymbolLayout::SINGLE, 0},
        {SymbolType::WREATH, SymbolLayout::SINGLE, 0},
    };
    static const Charge SOCIALIST_R[] = {
        {SymbolType::GEAR,   SymbolLayout::ESCORTED, 5},
        {SymbolType::ROSE,   SymbolLayout::ARC,      3},
        {SymbolType::TORCH,  SymbolLayout::ESCORTED, 4},
        {SymbolType::HAMMER, SymbolLayout::ROW,      3},
    };
    static const Charge NATIONALIST_C[] = {
        {SymbolType::CROSS_LATIN,  SymbolLayout::SINGLE, 0},
        {SymbolType::CROSS_PATTEE, SymbolLayout::SINGLE, 0},
        {SymbolType::STAR_4,       SymbolLayout::SINGLE, 0},
        {SymbolType::LIGHTNING,    SymbolLayout::SINGLE, 0},
    };
    // The swastika sits HERE and nowhere else, and only ever in the flag a
    // player has asked to see uncensored: the censored variant is built with
    // avoidHateSymbols and takes the next charge along instead, so switching
    // censoring on gives a clean flag rather than a mosaic of a dirty one.
    static const Charge NATIONALIST_R[] = {
        {SymbolType::CROSSED_SWORDS, SymbolLayout::SINGLE,   0},
        {SymbolType::FASCES,         SymbolLayout::SINGLE,   0},
        {SymbolType::SWASTIKA,       SymbolLayout::SINGLE,   0},
        {SymbolType::LIGHTNING,      SymbolLayout::ESCORTED, 4},
    };
    static const Charge LIBERAL_C[] = {
        {SymbolType::CIRCLE, SymbolLayout::SINGLE, 0},
        {SymbolType::STAR_4, SymbolLayout::SINGLE, 0},
        {SymbolType::TORCH,  SymbolLayout::SINGLE, 0},
        {SymbolType::WREATH, SymbolLayout::SINGLE, 0},
    };
    static const Charge LIBERAL_R[] = {
        {SymbolType::SUN_RAYS,      SymbolLayout::SINGLE, 0},
        {SymbolType::SUN,           SymbolLayout::SINGLE, 0},
        {SymbolType::TORCH,         SymbolLayout::ARC,    3},
        {SymbolType::SUN_SPLENDOUR, SymbolLayout::SINGLE, 0},
    };
    static const Charge AUTHORITARIAN_C[] = {
        {SymbolType::DIAMOND,      SymbolLayout::SINGLE, 0},
        {SymbolType::CROSS_PATTEE, SymbolLayout::SINGLE, 0},
        {SymbolType::ANCHOR,       SymbolLayout::SINGLE, 0},
        {SymbolType::CROSS_MALTESE, SymbolLayout::SINGLE, 0},
    };
    static const Charge AUTHORITARIAN_R[] = {
        {SymbolType::SWORD,          SymbolLayout::SINGLE, 0},
        {SymbolType::CROSSED_SWORDS, SymbolLayout::SINGLE, 0},
        {SymbolType::FASCES,         SymbolLayout::SINGLE, 0},
        {SymbolType::LIGHTNING,      SymbolLayout::SINGLE, 0},
    };
    // A liberty tree, not a disc: liberal-committed already has the disc and
    // they already share the gold, and two identities that come out as the same
    // picture are two identities the player cannot tell apart.
    static const Charge LIBERTARIAN_C[] = {
        {SymbolType::TREE,     SymbolLayout::SINGLE, 0},
        {SymbolType::MOUNTAIN, SymbolLayout::SINGLE, 0},
        {SymbolType::STAR_4,   SymbolLayout::SINGLE, 0},
        {SymbolType::WREATH,   SymbolLayout::SINGLE, 0},
    };
    // A ring of equals, with nothing at the centre of it, deliberately.
    static const Charge LIBERTARIAN_R[] = {
        {SymbolType::STAR_5, SymbolLayout::CIRCLE,   7},
        {SymbolType::TREE,   SymbolLayout::ARC,      3},
        {SymbolType::STAR_4, SymbolLayout::CIRCLE,   5},
        {SymbolType::TREE,   SymbolLayout::CIRCLE,   6},
    };
    static const Charge COLLECTIVIST_C[] = {
        {SymbolType::GEAR,     SymbolLayout::SINGLE, 0},
        {SymbolType::ANCHOR,   SymbolLayout::SINGLE, 0},
        {SymbolType::MOUNTAIN, SymbolLayout::SINGLE, 0},
        {SymbolType::HAMMER,   SymbolLayout::SINGLE, 0},
    };
    static const Charge COLLECTIVIST_R[] = {
        {SymbolType::GEAR,   SymbolLayout::ROW,      3},
        {SymbolType::ANCHOR, SymbolLayout::ESCORTED, 4},
        {SymbolType::GEAR,   SymbolLayout::ESCORTED, 4},
        {SymbolType::HAMMER, SymbolLayout::ROW,      3},
    };
    static const Charge CAPITALIST_C[] = {
        {SymbolType::DISC,   SymbolLayout::SINGLE, 0},
        {SymbolType::ANCHOR, SymbolLayout::SINGLE, 0},
        {SymbolType::STAR_4, SymbolLayout::SINGLE, 0},
        {SymbolType::WREATH, SymbolLayout::SINGLE, 0},
    };
    static const Charge CAPITALIST_R[] = {
        {SymbolType::STAR_5, SymbolLayout::ROW,    3},
        {SymbolType::SUN,    SymbolLayout::SINGLE, 0},
        {SymbolType::DISC,   SymbolLayout::ROW,    3},
        {SymbolType::SUN_SPLENDOUR, SymbolLayout::SINGLE, 0},
    };

    const Charge* set = COMMUNIST_C;
    switch (q) {
        case IdeologyQuadrant::Communist:     set = radical ? COMMUNIST_R     : COMMUNIST_C;     break;
        case IdeologyQuadrant::Socialist:     set = radical ? SOCIALIST_R     : SOCIALIST_C;     break;
        case IdeologyQuadrant::Nationalist:   set = radical ? NATIONALIST_R   : NATIONALIST_C;   break;
        case IdeologyQuadrant::Liberal:       set = radical ? LIBERAL_R       : LIBERAL_C;       break;
        case IdeologyQuadrant::Authoritarian: set = radical ? AUTHORITARIAN_R : AUTHORITARIAN_C; break;
        case IdeologyQuadrant::Libertarian:   set = radical ? LIBERTARIAN_R   : LIBERTARIAN_C;   break;
        case IdeologyQuadrant::Collectivist:  set = radical ? COLLECTIVIST_R  : COLLECTIVIST_C;  break;
        case IdeologyQuadrant::Capitalist:    set = radical ? CAPITALIST_R    : CAPITALIST_C;    break;
        default: break;
    }
    // Mixed, because the low bits of an FNV hash of two names differing in one
    // letter are not independent, and neighbouring countries would cluster on
    // the same choice.
    uint32_t h = seed;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
    unsigned pick = h % CHARGES_PER_IDENTITY;
    if (avoidHateSymbols && isHateSymbol(set[pick].emblem))
        pick = (pick + 1u) % CHARGES_PER_IDENTITY;
    return set[pick];
}

Restyle paletteFor(IdeologyQuadrant q) {
    switch (q) {
        case IdeologyQuadrant::Nationalist:
        case IdeologyQuadrant::Authoritarian:
            return DARK_RIGHT;
        case IdeologyQuadrant::Liberal:
        case IdeologyQuadrant::Libertarian:
        case IdeologyQuadrant::Capitalist:
            return GOLD_LIB;
        default:
            return RED_LEFT;      // communist, socialist, collectivist
    }
}

}  // namespace

FlagPattern applyFlag(const FlagPattern& originalFlag, const PoliticalIdentity& id,
                      uint32_t seed, bool avoidHateSymbols) {
    FlagPattern f = originalFlag;
    if (!id.expressed()) return f;

    const bool radical = (id.intensity == IdeologyIntensity::Radical);
    // How far the saturation and lightness move toward the band. High for both,
    // because the band already carries the flag's own contrast: a half-hearted
    // move lands between the flag's colours and the ideology's, which is where
    // mud lives. What separates committed from radical is the charge and the
    // field, not a weaker colour.
    const float pull   = radical ? 1.00f : 0.85f;

    const Restyle toward = paletteFor(id.quadrant);
    const Charge  charge = chargeFor(id.quadrant, radical, seed, avoidHateSymbols);

    // The recolour goes to the renderer rather than being applied here, because
    // a country's flag is nearly always a raster IMAGE and there is no palette
    // at this level to rewrite. Handing over the transform instead of the
    // result is what lets the same rule reach the pixels of the Union Jack and
    // the three colours of a breakaway state's tricolour alike -- and it is
    // what makes an ideological change fundamental rather than a sticker.
    f.recolor        = toward.recolor;
    f.recolor.weight = pull;

    // Replaces any emblem a previous restyle added -- but applyFlag is always
    // called with the ORIGINAL, so in practice this is the first and only one.
    FlagSymbol sym;
    sym.type   = charge.emblem;
    sym.layout = charge.layout;
    sym.count  = charge.count;
    sym.colors = { radical ? Color{245, 225, 120, 255} : Color{240, 240, 240, 255} };
    // The canton is where a device goes, so it is what is ASKED for -- but this
    // one emblem is dropped onto two hundred flags nobody looked at, and on
    // Japan the canton is the edge of the hinomaru and on the United States it
    // is the middle of the union. The renderer can see the finished pixels and
    // this cannot, so it decides where the emblem lands, whether it needs an
    // outline, and -- on a flag with no room anywhere, which the American and
    // British flags genuinely are -- whether to give it a field of its own.
    sym.x            = 0.22f;
    sym.y            = 0.28f;
    sym.size         = radical ? 0.26f : 0.21f;
    sym.autoPlace    = true;
    sym.autoContrast = true;
    sym.field        = radical ? EmblemField::AUTO_HOIST : EmblemField::AUTO_CANTON;
    sym.fieldColor   = toward.solid;
    f.symbols.push_back(sym);
    return f;
}

}  // namespace politid
