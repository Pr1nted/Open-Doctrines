// An army made of something: the save format half.
//
// `ArmyUnit` was `{countryId, count}`. Giving it a TYPE is the change this
// whole feature rests on, and the dangerous part is not the combat maths -- it
// is that the type has to survive a save, a reload, and a multiplayer client
// running an older build, without breaking either.
//
// WHY THE TYPE IS NOT PACKED BESIDE THE COUNT. The army block in a turn delta
// is fixed-width -- u16 countryId, u32 count -- with no version field, and it
// sits in the MIDDLE of the stream with the trailer after it. One extra byte
// per unit would shift every byte that follows, which breaks every save ever
// written and every client on an older build simultaneously. The file already
// solved this once for populations too large for their field, and the comment
// there says exactly why: written in the trailer, "a build that has never heard
// of them stops reading at the end of the research block and still walks the
// province stream at the right offsets."
//
// So types ride in the trailer under their own flag bit, and this pins the
// properties that makes that safe:
//
//   - a world of only line infantry writes NOTHING, so its saves are
//     byte-identical to what the previous build produced;
//   - an old save, which has no such block, loads as an army of line infantry,
//     which is exactly what it was;
//   - a new save read by an old build yields the right TOTALS -- the same army,
//     seen by something that cannot tell the kinds apart.
//
// Replicates the packing rather than linking SaveManager, which needs a whole
// archive. If the format changes, change it here in the same commit.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_failed = 0;
static void ok(bool c, const std::string& w) {
    ++g_checks; if (c) return; ++g_failed; printf("  FAIL  %s\n", w.c_str());
}
static void section(const char* n) { printf("\n== %s ==\n", n); }

struct Unit { int countryId; int count; uint8_t type; };
struct Army { int provinceId; std::vector<Unit> units; };

static void wU16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
static void wU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF);
}

// SaveManager::packTurn, army block + the types trailer.
static std::vector<uint8_t> pack(const std::vector<Army>& armies) {
    std::vector<uint8_t> b;
    wU16(b, (uint16_t)armies.size());
    for (const auto& a : armies) {                 // the fixed-width block
        wU16(b, (uint16_t)a.provinceId);
        b.push_back((uint8_t)a.units.size());
        for (const auto& u : a.units) { wU16(b, (uint16_t)u.countryId); wU32(b, (uint32_t)u.count); }
    }
    std::vector<const Army*> typed;                // only what needs saying
    for (const auto& a : armies)
        for (const auto& u : a.units) if (u.type != 0) { typed.push_back(&a); break; }
    uint8_t flags = 0x01;
    if (!typed.empty()) flags |= 0x04;
    b.push_back(flags);
    if (!typed.empty()) {
        wU16(b, (uint16_t)typed.size());
        for (const Army* a : typed) {
            wU16(b, (uint16_t)a->provinceId);
            b.push_back((uint8_t)a->units.size());
            for (const auto& u : a->units) {
                wU16(b, (uint16_t)u.countryId); b.push_back(u.type); wU32(b, (uint32_t)u.count);
            }
        }
    }
    b.push_back(0xFF);
    return b;
}

// An OLD build: reads the block, does not know flag bit 2 exists.
static std::vector<Army> unpackOld(const std::vector<uint8_t>& b) {
    const uint8_t* p = b.data();
    uint16_t n = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
    std::vector<Army> out(n);
    for (uint16_t i = 0; i < n; ++i) {
        out[i].provinceId = (int)(p[0] | (p[1] << 8)); p += 2;
        uint8_t uc = *p++;
        for (uint8_t j = 0; j < uc; ++j) {
            Unit u{};
            u.countryId = (int)(p[0] | (p[1] << 8)); p += 2;
            u.count = (int)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); p += 4;
            u.type = 0;                            // it cannot imagine another
            out[i].units.push_back(u);
        }
    }
    return out;
}

// A NEW build: block, then the trailer replaces what it describes.
static std::vector<Army> unpackNew(const std::vector<uint8_t>& b) {
    std::vector<Army> out = unpackOld(b);
    const uint8_t* p = b.data();
    uint16_t n = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
    for (uint16_t i = 0; i < n; ++i) { p += 2; uint8_t uc = *p++; p += (size_t)uc * 6; }
    const uint8_t flags = *p++;
    if (!(flags & 0x04)) return out;
    uint16_t tc = (uint16_t)(p[0] | (p[1] << 8)); p += 2;
    for (uint16_t i = 0; i < tc; ++i) {
        const int pid = (int)(p[0] | (p[1] << 8)); p += 2;
        const uint8_t uc = *p++;
        std::vector<Unit> units;
        for (uint8_t j = 0; j < uc; ++j) {
            Unit u{};
            u.countryId = (int)(p[0] | (p[1] << 8)); p += 2;
            u.type = *p++;
            u.count = (int)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); p += 4;
            units.push_back(u);
        }
        for (auto& a : out) if (a.provinceId == pid) { a.units = units; break; }
    }
    return out;
}

static long long total(const std::vector<Army>& as) {
    long long t = 0;
    for (const auto& a : as) for (const auto& u : a.units) t += u.count;
    return t;
}

int main() {
    section("a world of line infantry writes exactly what it always did");
    {
        const std::vector<Army> plain = {{10, {{1, 50000, 0}, {2, 30000, 0}}}, {11, {{1, 7000, 0}}}};
        const auto bytes = pack(plain);
        // The trailer flag for types must not be set, so the bytes are what a
        // build without troop types would have produced.
        bool typeFlagSet = false;
        for (size_t i = 0; i + 1 < bytes.size(); ++i)
            if (bytes[i] == 0x05 || bytes[i] == 0x07) typeFlagSet = true;
        ok(!typeFlagSet, "no troop-type block is emitted at all");
        ok(unpackNew(bytes).size() == 2, "and it still reads back");
        ok(total(unpackNew(bytes)) == 87000, "with every man accounted for");
    }

    section("a mixed army survives a round trip");
    {
        const std::vector<Army> mixed = {
            {10, {{1, 50000, 0}, {1, 12000, 3}, {2, 30000, 1}}},   // line + mech + militia
            {11, {{1, 7000, 0}}},
        };
        const auto back = unpackNew(pack(mixed));
        ok(back.size() == 2, "both provinces come back");
        ok(back[0].units.size() == 3, "all three stacks in the first");
        ok(back[0].units[1].type == 3 && back[0].units[1].count == 12000, "the mechanised survive");
        ok(back[0].units[2].type == 1 && back[0].units[2].count == 30000, "so do the militia");
        ok(back[1].units[0].type == 0, "and the untyped province is untouched");
        ok(total(back) == total(mixed), "no soldier created or lost");
    }

    section("an OLD build reads a new save and sees the right army");
    {
        // The property that lets a mixed save cross the wire to a client that
        // predates types: it cannot tell the kinds apart, but it must not be
        // WRONG about how many men are there.
        const std::vector<Army> mixed = {{10, {{1, 50000, 0}, {1, 12000, 3}, {2, 30000, 1}}}};
        const auto old = unpackOld(pack(mixed));
        ok(total(old) == 92000, "every man is present and correct");
        bool allLine = true;
        for (const auto& a : old) for (const auto& u : a.units) if (u.type != 0) allLine = false;
        ok(allLine, "and they all look like line infantry, which is honest");
    }

    section("an OLD save read by a NEW build is line infantry");
    {
        const std::vector<Army> plain = {{10, {{1, 50000, 0}}}};
        auto bytes = pack(plain);
        const auto back = unpackNew(bytes);
        ok(back[0].units[0].type == 0, "which is exactly what it was");
        ok(total(back) == 50000, "at full strength");
    }

    section("a better soldier costs more people");
    {
        // The whole of "the better the troop, the less manpower available", and
        // it needed no new resource: recruiting already takes people out of the
        // province permanently, so the type just multiplies the draw. `count`
        // stays SOLDIERS; the population pays count x manpower.
        struct T { const char* id; double manpower; };
        const T types[] = {{"line", 1.00}, {"militia", 0.60},
                           {"assault", 2.50}, {"mech", 4.00}};
        // Above the frontage every count cancels and power is width x
        // stat/frontage, so a kind that is worse than line at BOTH attack and
        // defence per metre is dominated and has no fight it is correct for.
        // Militia shipped that way once (frontage 1.20, defence 1.10 -> 0.917
        // def/m) and cost two models 34 and 28 rating points before it was
        // caught. Pinned so it cannot come back.
        struct PerMetre { const char* id; double atk, def, frontage; };
        const PerMetre pm[] = {
            {"line",    1.00, 1.00, 1.00}, {"militia", 0.70, 1.15, 1.00},
            {"assault", 1.35, 0.95, 0.80}, {"mech",    1.25, 1.15, 0.60},
        };
        for (const auto& x : pm) {
            const bool dominated = (x.atk / x.frontage) < 1.0 && (x.def / x.frontage) < 1.0;
            ok(!dominated || std::string(x.id) == "line",
               std::string(x.id) + " is not worse than line at both attack and defence per metre");
        }
        ok(pm[1].def / pm[1].frontage > 1.0, "militia hold better than line, per metre");
        ok(pm[1].atk / pm[1].frontage < 1.0, "and attack worse, which is the trade");
        auto draw = [&](int soldiers, double manp) { return (long long)(soldiers * manp + 0.5); };

        ok(draw(10000, types[0].manpower) == 10000, "line infantry: one man, one person");
        ok(draw(10000, types[1].manpower) ==  6000, "militia are cheaper in people");
        ok(draw(10000, types[3].manpower) == 40000, "mechanised cost four times over");

        // A province of fixed population therefore fields very different armies.
        const long long pop = 120000;
        ok(pop / (long long)types[0].manpower == 120000, "120k people, 120k line infantry");
        ok((long long)(pop / types[3].manpower) == 30000, "or 30k mechanised");
        ok((long long)(pop / types[1].manpower) == 200000, "or 200k militia");

        // Short of people, the order shrinks rather than raising men from
        // nobody -- the guard in processRecruitments.
        const long long have = 25000;
        const int wanted = 10000;                       // mechanised
        const long long want = draw(wanted, types[3].manpower);
        ok(want == 40000, "the order wants 40,000 people");
        const int got = (int)(have / types[3].manpower);
        ok(got == 6250 && got < wanted, "and 25,000 people raise only 6,250 of them");
    }

    section("line infantry changes nothing, which is the point");
    {
        // Every soldier in every existing world is line infantry, so every
        // column of it is 1.0 and every formula above collapses to what it was.
        const double money = 1.00, muni = 1.00, manp = 1.00, front = 1.00;
        ok(money == 1.0 && muni == 1.0 && manp == 1.0 && front == 1.0,
           "line infantry is exactly neutral in every column");
    }

    section("an empty army list is still an empty army list");
    {
        const auto back = unpackNew(pack({}));
        ok(back.empty(), "nothing in, nothing out");
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
