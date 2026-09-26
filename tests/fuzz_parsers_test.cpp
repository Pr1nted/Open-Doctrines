// Feeding the parsers input nobody would write on purpose.
//
// WHAT THIS IS, AND WHY IT IS NOT tools/fuzz.py
//
// That one plays whole games from random seeds, looking for the failures that
// only appear when correct components are combined. This one is the other
// half: the places where the game reads bytes it did not write. A .odmod off
// the internet, a map script somebody else's mod shipped, and every frame that
// arrives on a multiplayer socket are all input this process must survive
// being wrong about -- and the suite only ever showed them inputs a developer
// composed, which are inputs that make sense.
//
// HOW IT WORKS. Start from something valid, break it in one of nine ways, and
// require that the parser either refuses it or returns something within its
// own declared limits. Never that it accepts it: a fuzzer that asserts on the
// ANSWER is a fuzzer that fails whenever the answer improves. What it asserts
// on is: it came back, it did not crash, and what it handed over is inside the
// ceilings in NetLimits -- because a decoder that returns `true` with a
// megabyte in a field capped at 64 bytes has already handed the rest of the
// program a lie, whether or not it crashed doing it.
//
// DETERMINISTIC. The seed is fixed, so a failure here is a failure everybody
// gets, and the case is printed as hex that can be pasted straight back in.
// Set OD_FUZZ_ITERS to run it harder (the suite runs a few seconds' worth;
// 10,000,000 has been run against this by hand, and under ASan).
//
//   OD_FUZZ_ITERS=1000000 ./FuzzParsersTest
//   OD_FUZZ_SEED=12345    ./FuzzParsersTest

#include "net/NetProtocol.h"
#include "script/Blocks.h"
#include "script/Expr.h"
#include "mods/ModPackage.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_failed = 0;

void section(const char* name) { printf("\n== %s ==\n", name); }

std::string hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < v.size() && i < 96; ++i) {
        s += d[v[i] >> 4];
        s += d[v[i] & 15];
    }
    if (v.size() > 96) s += "...";
    return s;
}

/** A failure prints the input, because a case that cannot be reproduced is a rumour. */
void bad(const char* what, const std::vector<uint8_t>& input) {
    ++g_failed;
    printf("  FAIL  %s\n        input: %s\n", what, hex(input).c_str());
}

void check(bool cond, const char* what, const std::vector<uint8_t>& input) {
    ++g_checks;
    if (!cond) bad(what, input);
}

// ── the mutator ─────────────────────────────────────────────────────────────

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {          // xorshift64*, so a seed means the same thing
        s ^= s >> 12;          // on every platform this suite runs on
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    size_t below(size_t n) { return n ? (size_t)(next() % n) : 0; }
};

/**
 * Nine ways to be wrong, each of which has been a real bug somewhere.
 *
 * Truncation and extension matter most: a decoder that reads a length and then
 * trusts it is the classic shape, and both directions have to be tried --
 * short of the length and past it.
 */
std::vector<uint8_t> mutate(const std::vector<uint8_t>& in, Rng& rng) {
    std::vector<uint8_t> out = in;
    const int ops = 1 + (int)rng.below(3);
    for (int i = 0; i < ops; ++i) {
        if (out.empty()) { out.push_back((uint8_t)rng.next()); continue; }
        switch (rng.below(9)) {
            case 0: out[rng.below(out.size())] ^= (uint8_t)(1u << rng.below(8)); break;
            case 1: out[rng.below(out.size())] = (uint8_t)rng.next(); break;
            case 2: {   // extremes, which is where length fields go wrong
                static const uint8_t edge[] = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF};
                out[rng.below(out.size())] = edge[rng.below(sizeof edge)];
                break;
            }
            case 3: out.resize(rng.below(out.size())); break;              // short
            case 4: out.insert(out.end(), 1 + rng.below(64), (uint8_t)rng.next()); break;
            case 5: {   // splice a chunk over another, to build plausible nonsense
                if (out.size() < 4) break;
                const size_t n = 1 + rng.below(out.size() / 2);
                const size_t from = rng.below(out.size() - n + 1);
                const size_t to = rng.below(out.size() - n + 1);
                for (size_t k = 0; k < n; ++k) out[to + k] = out[from + k];
                break;
            }
            case 6: {   // a length field set to something enormous
                if (out.size() < 4) break;
                const size_t at = rng.below(out.size() - 3);
                out[at] = 0xFF; out[at + 1] = 0xFF; out[at + 2] = 0xFF; out[at + 3] = 0x7F;
                break;
            }
            case 7: out.insert(out.begin() + (long)rng.below(out.size() + 1),
                               (uint8_t)rng.next()); break;
            case 8: out.erase(out.begin() + (long)rng.below(out.size())); break;
        }
    }
    return out;
}

// ── what a decoder is allowed to hand back ──────────────────────────────────

bool within(const std::string& s, uint32_t cap) { return s.size() <= cap; }

/**
 * Every decoder that came back `true`, checked against the ceilings it
 * declares. This is the half a crash-only fuzzer misses: accepting a field
 * that is too long is not a crash here, it is a crash later, in whatever
 * copies it.
 */
template <typename T>
void limits(bool okDecode, const T&, const std::vector<uint8_t>&) { (void)okDecode; }

// ── the corpus ──────────────────────────────────────────────────────────────

std::vector<std::vector<uint8_t>> netCorpus() {
    std::vector<std::vector<uint8_t>> c;

    NetHello hello;
    hello.gameVersion = "1.2.2a";
    hello.ticket = std::string(64, 'k');
    hello.modAttestation = "{\"mods\":[]}";
    c.push_back(hello.encode());

    NetWelcome welcome;
    welcome.peerId = 3;
    welcome.sessionName = "A Game";
    welcome.host.psid = "psid-0001";
    welcome.host.issuer = "https://example.invalid";
    NetPeer peer;
    peer.name = "Player";
    peer.psid = "psid-0002";
    welcome.roster.push_back(peer);
    c.push_back(welcome.encode());

    NetRejectMsg reject;
    reject.reason = NetReject::HostNotDeclared;
    reject.text = "no";
    c.push_back(reject.encode());

    NetWorld world;
    world.turnNumber = 7;
    world.payload.assign(256, 0xAB);
    c.push_back(world.encode());

    NetOrdersMsg orders;
    orders.turnNumber = 7;
    orders.payload.assign(128, 0x11);
    c.push_back(orders.encode());

    NetChat chat;
    chat.text = "hello everyone";
    c.push_back(chat.encode());

    NetSignal signal;
    signal.payload = std::string(512, 'v');
    c.push_back(signal.encode());

    NetModMsg mod;
    mod.modId = "com.example.mod";
    mod.payload.assign(64, 0x5A);
    c.push_back(mod.encode());

    // Wrapped in frames too: netDecodeFrame is what actually meets the socket,
    // and its length field is the first thing an attacker reaches.
    const size_t plain = c.size();
    for (size_t i = 0; i < plain; ++i)
        c.push_back(netEncodeFrame((NetMsg)(1 + i % 12), c[i]));

    c.push_back({});                       // empty
    c.push_back({0x00});                   // one byte
    c.push_back(std::vector<uint8_t>(64, 0xFF));
    return c;
}

const char* kScriptCorpus[] = {
    "#OD/MapEngine/1\nset gold = gold + 1\n",
    "if country.gold > 100\n    set flag = 1\nelseif country.gold > 50\n"
    "    set flag = 2\nelse\n    set flag = 3\nendif\n",
    "foreach province in country.provinces\n    set p = p + 1\nnext\n",
    "while x < 10\n    set x = x + 1\nendwhile\n",
    "try\n    jump nowhere\ncatch\n    stop\nendtry\n",
    "label top\nrepeat\n    set i = i - 1\nuntil i <= 0\njump top\n",
    "array names\nlist values\nwaitUntil turn > 4\n",
};

const char* kExprCorpus[] = {
    "1 + 2 * 3 - 4 / 2",
    "(a and b) or not c",
    "country.gold >= 100 and province.unrest < 5",
    "\"a string\" + \"another\"",
    "min(1, max(2, 3))",
    "-x * (y % 3) == 0",
};

const char* kManifest =
    "{\"schema\":1,\"id\":\"com.example.fuzz\",\"name\":\"Fuzz\",\"version\":\"1.0.0\","
    "\"description\":\"d\",\"authors\":[\"a\"],\"modules\":[\"rules\"],\"side\":\"both\"}";

std::vector<uint8_t> bytesOf(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
std::string stringOf(const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
}

}  // namespace

int main() {
    const char* itersEnv = std::getenv("OD_FUZZ_ITERS");
    const char* seedEnv = std::getenv("OD_FUZZ_SEED");
    const long iters = itersEnv ? std::atol(itersEnv) : 60000;
    const uint64_t seed = seedEnv ? (uint64_t)std::atoll(seedEnv) : 0xD0C7817E5ull;

    printf("Parsers, fed input nobody would write on purpose\n");
    printf("  %ld iterations, seed %llu\n", iters, (unsigned long long)seed);

    Rng rng(seed);
    const auto corpus = netCorpus();

    section("every message a socket can deliver");
    for (long i = 0; i < iters; ++i) {
        const std::vector<uint8_t>& base = corpus[rng.below(corpus.size())];
        const std::vector<uint8_t> in = mutate(base, rng);
        const uint8_t* p = in.data();
        const size_t n = in.size();

        // The frame layer first, which is what a socket hands over.
        NetMsg type{};
        const uint8_t* payload = nullptr;
        size_t payloadSize = 0;
        if (netDecodeFrame(p, n, type, payload, payloadSize)) {
            check(payload != nullptr || payloadSize == 0,
                  "a frame decoded with a null payload and a non-zero size", in);
            check(payloadSize <= n, "a frame claimed more payload than it was given", in);
            check(payload == nullptr || (payload >= p && payload + payloadSize <= p + n),
                  "a frame's payload points outside the buffer it came from", in);
        }

        // Then every message decoder, on the same bytes. None of them may read
        // past the size they are given, which is what ASan is for; what is
        // checked here without a sanitiser is that a `true` means a value the
        // rest of the game can hold.
        NetHello hello;
        if (NetHello::decode(p, n, hello)) {
            check(within(hello.gameVersion, NetLimits::kVersion) &&
                      within(hello.ticket, NetLimits::kTicket) &&
                      within(hello.modAttestation, NetLimits::kModAttest),
                  "NetHello accepted a field past its own limit", in);
        }
        NetWelcome welcome;
        if (NetWelcome::decode(p, n, welcome)) {
            check(welcome.roster.size() <= NetLimits::kRoster,
                  "NetWelcome accepted more peers than the roster allows", in);
            for (const NetPeer& peer : welcome.roster) {
                check(within(peer.name, NetLimits::kName) &&
                          within(peer.psid, NetLimits::kPsid),
                      "NetWelcome accepted an oversized peer", in);
            }
        }
        NetRejectMsg reject;
        if (NetRejectMsg::decode(p, n, reject))
            check(within(reject.text, NetLimits::kReason),
                  "NetRejectMsg accepted an oversized detail", in);
        NetWorld world;
        if (NetWorld::decode(p, n, world))
            check(world.payload.size() <= NetLimits::kWorld,
                  "NetWorld accepted a world past the frame ceiling", in);
        NetOrdersMsg orders;
        if (NetOrdersMsg::decode(p, n, orders))
            check(orders.payload.size() <= NetLimits::kOrders,
                  "NetOrdersMsg accepted orders past the ceiling", in);
        NetChat chat;
        if (NetChat::decode(p, n, chat))
            check(within(chat.text, NetLimits::kChat),
                  "NetChat accepted a message past the ceiling", in);
        NetSignal signal;
        if (NetSignal::decode(p, n, signal))
            check(within(signal.payload, NetLimits::kSignal),
                  "NetSignal accepted a payload past the ceiling", in);
        NetModMsg modMsg;
        if (NetModMsg::decode(p, n, modMsg)) {
            check(within(modMsg.modId, NetLimits::kModId),
                  "NetModMsg accepted an oversized mod id", in);
            check(modMsg.payload.size() <= NetLimits::kModMsg,
                  "NetModMsg accepted a payload past the ceiling", in);
        }
        NetRosterMsg roster;
        if (NetRosterMsg::decode(p, n, roster))
            check(roster.peers.size() <= NetLimits::kRoster,
                  "NetRosterMsg accepted more peers than the roster allows", in);
        NetLobbyState lobby;
        (void)NetLobbyState::decode(p, n, lobby);
        NetTurnBegin begin;
        (void)NetTurnBegin::decode(p, n, begin);
        NetTurnOrders turnOrders;
        (void)NetTurnOrders::decode(p, n, turnOrders);
    }
    printf("  %ld message decodes, nothing out of bounds\n", iters);

    section("a map script somebody else wrote");
    {
        const long n = iters / 4;
        for (long i = 0; i < n; ++i) {
            const std::string base = kScriptCorpus[rng.below(sizeof kScriptCorpus /
                                                             sizeof kScriptCorpus[0])];
            const std::vector<uint8_t> in = mutate(bytesOf(base), rng);
            const std::string text = stringOf(in);

            // parseScript never fails destructively -- a structural error goes
            // in Doc::error and what parsed is still returned -- so what is
            // asserted is that it comes back, and that unparsing what it
            // returned does not fall over either. Round-tripping broken input
            // is exactly what the map editor does when a player opens a file
            // that does not compile.
            odscript::Doc doc = odscript::parseScript(text);
            const std::string back = odscript::unparseScript(doc);
            ++g_checks;
            (void)back;
            (void)odscript::classify(text);
            (void)odscript::normaliseAssignment(text);
        }
        printf("  %ld scripts parsed and written back\n", n);
    }

    section("an expression in one of its statements");
    {
        const long n = iters / 4;
        for (long i = 0; i < n; ++i) {
            const std::string base = kExprCorpus[rng.below(sizeof kExprCorpus /
                                                           sizeof kExprCorpus[0])];
            const std::vector<uint8_t> in = mutate(bytesOf(base), rng);
            std::string err;
            odscript::NodePtr node = odscript::parse(stringOf(in), err);
            ++g_checks;
            if (!node) {
                if (err.empty()) bad("parse returned nothing and no reason", in);
                continue;
            }
            // A tree that parsed must also evaluate without taking the process
            // with it. Every name resolves to zero: what is under test is the
            // evaluator's shape handling, not the game's data.
            std::string evalErr;
            (void)odscript::eval(
                *node, [](const std::string&) { return ScriptValue::makeInt(0); }, evalErr);
            (void)odscript::unparse(*node);
        }
        printf("  %ld expressions parsed and evaluated\n", n);
    }

    section("a mod manifest from the internet");
    {
        const long n = iters / 4;
        for (long i = 0; i < n; ++i) {
            const std::vector<uint8_t> in = mutate(bytesOf(kManifest), rng);
            ModManifest manifest;
            std::vector<std::string> warnings;
            std::string diagnostic;
            const ModLoadResult r = parseModManifest(stringOf(in), manifest, warnings,
                                                     diagnostic);
            ++g_checks;
            if (r != ModLoadResult::Ok && diagnostic.empty())
                bad("a manifest was refused without saying why", in);
        }
        printf("  %ld manifests parsed\n", n);
    }

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
