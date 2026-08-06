// The wire format, and what it does with input it should not have been sent.
//
// This is the layer that parses bytes from strangers, so most of what is below
// is not "does it round-trip" but "does it refuse". A decoder that merely
// happens not to crash on the cases someone thought of is not the same as one
// that fails closed on everything it does not understand.
//
// Build target: NetProtocolTest. Run it; non-zero exit means a case failed.

#include "net/NetProtocol.h"
#include "net/WorldSync.h"
#include "net/WebSocket.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(const char* what, bool ok, const std::string& got = {}) {
    g_checks++;
    if (ok) { printf("  ok    %s\n", what); return; }
    g_failures++;
    printf("  FAIL  %s%s%s\n", what, got.empty() ? "" : "  --  ", got.c_str());
}

std::vector<uint8_t> noise(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t s = seed | 1u;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        v[i] = static_cast<uint8_t>(s >> 24);
    }
    return v;
}

// ------------------------------------------------------- reader/writer ----

void testPrimitives() {
    printf("\n=== primitives ===\n");

    NetWriter w;
    w.u8(0xAB);
    w.u16(0xBEEF);
    w.u32(0xDEADBEEF);
    w.u64(0x0123456789ABCDEFull);
    w.i32(-42);
    w.f32(3.5f);
    w.f64(-1.25);
    w.str("hello");
    w.blob({1, 2, 3});

    NetReader r(w.data());
    check("u8  round-trips", r.u8() == 0xAB);
    check("u16 round-trips", r.u16() == 0xBEEF);
    check("u32 round-trips", r.u32() == 0xDEADBEEF);
    check("u64 round-trips", r.u64() == 0x0123456789ABCDEFull);
    check("i32 round-trips negative", r.i32() == -42);
    check("f32 round-trips", r.f32() == 3.5f);
    check("f64 round-trips", r.f64() == -1.25);
    check("str round-trips", r.str(16) == "hello");
    check("blob round-trips", r.blob(16) == std::vector<uint8_t>({1, 2, 3}));
    check("and the message is fully consumed", r.done());

    // Little-endian, explicitly: the wire format must not depend on the host.
    NetWriter e;
    e.u32(0x01020304);
    check("u32 is little-endian on the wire",
          e.data()[0] == 0x04 && e.data()[1] == 0x03 &&
          e.data()[2] == 0x02 && e.data()[3] == 0x01);
}

void testReaderFailsClosed() {
    printf("\n=== the reader fails closed ===\n");

    {
        const uint8_t two[2] = {1, 2};
        NetReader r(two, sizeof(two));
        r.u32();
        check("reading past the end fails", !r.ok());
    }
    {
        // The first failure latches, so a decoder can read a whole message
        // straight through and check once. Code that must remember to check
        // after every field will eventually forget.
        const uint8_t one[1] = {7};
        NetReader r(one, sizeof(one));
        r.u32();
        check("and every read after it returns zero", r.u8() == 0);
        check("and ok() stays false", !r.ok());
    }
    {
        NetReader r(nullptr, 0);
        check("an empty message is not done() after a failed read",
              (r.u8(), !r.done()));
    }
    {
        NetWriter w;
        w.u32(8);
        w.bytes("abcdefgh", 8);
        NetReader r(w.data());
        r.str(4);
        check("a string longer than its stated ceiling is refused", !r.ok());
    }
    {
        // The ceiling is checked before the bounds check, so this costs a
        // comparison rather than an attempted 4 GiB allocation.
        NetWriter w;
        w.u32(0xFFFFFFFFu);
        NetReader r(w.data());
        r.str(1024);
        check("an absurd declared length is refused without allocating", !r.ok());
    }
    {
        NetWriter w;
        w.u32(0xFFFFFFFFu);
        NetReader r(w.data());
        r.blob(1024);
        check("the same for a blob", !r.ok());
    }
    {
        NetWriter w;
        w.u16(1);
        w.u8(9);          // one byte more than the reader below will consume
        NetReader r(w.data());
        r.u16();
        check("trailing bytes make done() false", !r.done());
    }
}

// ---------------------------------------------------------------- frame ----

void testFraming() {
    printf("\n=== framing ===\n");

    const std::vector<uint8_t> payload = {1, 2, 3, 4};
    auto frame = netEncodeFrame(NetMsg::Delta, payload);

    NetMsg type;
    const uint8_t* body = nullptr;
    size_t bodySize = 0;
    check("a frame decodes to its type and payload",
          netDecodeFrame(frame.data(), frame.size(), type, body, bodySize) &&
          type == NetMsg::Delta && bodySize == payload.size() &&
          memcmp(body, payload.data(), bodySize) == 0);

    check("a truncated header is refused",
          !netDecodeFrame(frame.data(), 3, type, body, bodySize));
    check("an empty message is refused",
          !netDecodeFrame(frame.data(), 0, type, body, bodySize));

    // The transport already knows the length. A header that disagrees means
    // one of the two is wrong and there is no safe way to pick.
    auto lying = frame;
    lying[2] = 0xFF;
    check("a length that disagrees with the transport is refused",
          !netDecodeFrame(lying.data(), lying.size(), type, body, bodySize));

    auto shortened = frame;
    shortened.pop_back();
    check("a truncated payload is refused",
          !netDecodeFrame(shortened.data(), shortened.size(), type, body, bodySize));

    auto empty = netEncodeFrame(NetMsg::Ping, {});
    check("an empty payload is legal",
          netDecodeFrame(empty.data(), empty.size(), type, body, bodySize) &&
          type == NetMsg::Ping && bodySize == 0);

    // An unknown type decodes; the dispatcher decides what to do with it. A
    // newer server sending a message we do not know is not a protocol error.
    auto unknown = netEncodeFrame(static_cast<NetMsg>(9999), {1});
    check("an unknown type still decodes, for the dispatcher to ignore",
          netDecodeFrame(unknown.data(), unknown.size(), type, body, bodySize) &&
          static_cast<uint16_t>(type) == 9999);
}

// ------------------------------------------------------------- messages ----

void testMessages() {
    printf("\n=== messages ===\n");

    {
        NetHello in;
        in.gameVersion = "1.0.2a";
        in.ticket = "od1.payload.signature";
        in.modAttestation = "com.a.b@1.0.0#" + std::string(64, 'a') + "\n";
        auto bytes = in.encode();

        NetHello out;
        check("Hello round-trips",
              NetHello::decode(bytes.data(), bytes.size(), out) &&
              out.protocolVersion == kNetProtocolVersion &&
              out.gameVersion == in.gameVersion &&
              out.ticket == in.ticket &&
              out.modAttestation == in.modAttestation);
    }
    {
        NetWelcome in;
        in.peerId = 7;
        in.sessionName = "Friday game";
        in.turnSeconds = 90;
        in.turnNumber = 12;
        in.showBadges = false;
        in.issuer = "https://example.invalid";
        in.authNotice = "This server uses a third-party account provider.";
        in.requiredMods = "com.a.b@1.0.0#" + std::string(64, 'b') + "\n";
        in.roster.push_back(NetPeer{1, "psid-a", "Alice", "developer", true, 40, true});
        in.roster.push_back(NetPeer{2, "psid-b", "Bob", "", false, 0, false});
        auto bytes = in.encode();

        NetWelcome out;
        check("Welcome round-trips including its roster",
              NetWelcome::decode(bytes.data(), bytes.size(), out) &&
              out.peerId == 7 && out.turnSeconds == 90 && out.turnNumber == 12 &&
              !out.showBadges && out.authNotice == in.authNotice &&
              out.roster.size() == 2 &&
              out.roster[0].name == "Alice" && out.roster[0].officialIssuer &&
              out.roster[1].name == "Bob" && !out.roster[1].connected);
    }
    {
        NetRejectMsg in{NetReject::ModMismatch, "You are missing com.a.b."};
        auto bytes = in.encode();
        NetRejectMsg out;
        check("Reject round-trips",
              NetRejectMsg::decode(bytes.data(), bytes.size(), out) &&
              out.reason == NetReject::ModMismatch && out.text == in.text);
    }
    {
        // A newer server explaining itself in terms we do not know should
        // still get its sentence in front of the player.
        NetWriter w;
        w.u16(31337);
        w.str("Something new happened.");
        auto bytes = w.take();
        NetRejectMsg out;
        check("an unknown reject reason degrades to Unknown but keeps its text",
              NetRejectMsg::decode(bytes.data(), bytes.size(), out) &&
              out.reason == NetReject::Unknown &&
              out.text == "Something new happened.");
    }
    {
        NetWorld in;
        in.turnNumber = 41;
        in.payload = noise(4096, 3);
        auto bytes = in.encode();
        NetWorld out;
        check("a turn delta round-trips byte for byte",
              NetWorld::decode(bytes.data(), bytes.size(), out) &&
              out.turnNumber == 41 && out.payload == in.payload);
    }
    {
        NetOrdersMsg in;
        in.turnNumber = 5;
        in.payload = noise(128, 9);
        auto bytes = in.encode();
        NetOrdersMsg out;
        check("Orders round-trips",
              NetOrdersMsg::decode(bytes.data(), bytes.size(), out) &&
              out.turnNumber == 5 && out.payload == in.payload);
    }
    {
        NetTurnBegin in{99, 60000};
        auto bytes = in.encode();
        NetTurnBegin out;
        check("TurnBegin round-trips",
              NetTurnBegin::decode(bytes.data(), bytes.size(), out) &&
              out.turnNumber == 99 && out.deadlineMs == 60000);
    }
    {
        NetChat in{3, "hello everyone"};
        auto bytes = in.encode();
        NetChat out;
        check("Chat round-trips",
              NetChat::decode(bytes.data(), bytes.size(), out) &&
              out.fromPeerId == 3 && out.text == in.text);
    }
}

void testTurnStoreInfo() {
    printf("\n=== long-form store info ===\n");

    {
        NetTurnStoreInfo in;
        in.store       = 0;                       // the session's own storage
        in.sessionCode = "ABCD-EFGH";
        in.sealKey     = "F0Ur3Y0urEy3s0nly0000000000000000000000000AB";
        auto bytes = in.encode();

        NetTurnStoreInfo out;
        check("TurnStoreInfo round-trips",
              NetTurnStoreInfo::decode(bytes.data(), bytes.size(), out) &&
              out.store == 0 && out.sessionCode == in.sessionCode &&
              out.sealKey == in.sealKey);
    }
    {
        // Manual has no store to address and no key to distribute: the player
        // is the transport. Both empty must survive the trip rather than
        // becoming a decode failure.
        NetTurnStoreInfo in;
        in.store = 2;
        auto bytes = in.encode();

        NetTurnStoreInfo out;
        check("an empty code and key are legitimate",
              NetTurnStoreInfo::decode(bytes.data(), bytes.size(), out) &&
              out.store == 2 && out.sessionCode.empty() && out.sealKey.empty());
    }
    {
        // A host newer than this build. The byte survives UNCLAMPED so the
        // caller can tell the player their game is too old -- clamping it to
        // the default here would silently play on a store nobody chose.
        NetTurnStoreInfo in;
        in.store = 200;
        auto bytes = in.encode();

        NetTurnStoreInfo out;
        check("an unknown store arrives intact rather than clamped",
              NetTurnStoreInfo::decode(bytes.data(), bytes.size(), out) &&
              out.store == 200);
    }
    {
        NetTurnStoreInfo out;
        check("a truncated frame is refused",
              !NetTurnStoreInfo::decode(nullptr, 0, out));
    }
}

void testMessagesRefuseGarbage() {
    printf("\n=== messages refuse garbage ===\n");

    {
        NetHello out;
        check("Hello refuses an empty payload",
              !NetHello::decode(nullptr, 0, out));
    }
    {
        // Trailing bytes mean the sender and this build disagree about the
        // shape. Guessing which is right is how a parser becomes a confused
        // deputy.
        NetHello in;
        in.gameVersion = "1.0.0";
        auto bytes = in.encode();
        bytes.push_back(0);
        NetHello out;
        check("Hello refuses trailing bytes",
              !NetHello::decode(bytes.data(), bytes.size(), out));
    }
    {
        NetHello in;
        in.gameVersion = "1.0.0";
        auto bytes = in.encode();
        bytes.pop_back();
        NetHello out;
        check("Hello refuses a truncated payload",
              !NetHello::decode(bytes.data(), bytes.size(), out));
    }
    {
        // A count taken from the wire and used to size a loop is the classic
        // remote memory exhaustion. Capped before the loop runs.
        NetWriter w;
        w.u16(1); w.str(""); w.u16(0); w.u32(0); w.u8(1);
        w.str(""); w.str(""); w.str("");
        w.u32(0xFFFFFFFFu);       // roster count
        auto bytes = w.take();
        NetWelcome out;
        check("Welcome refuses an absurd roster count without allocating",
              !NetWelcome::decode(bytes.data(), bytes.size(), out));
    }
    {
        NetWriter w;
        w.u32(1000);              // claims 1000 peers, provides none
        auto bytes = w.take();
        NetRosterMsg out;
        check("Roster refuses a count it cannot satisfy",
              !NetRosterMsg::decode(bytes.data(), bytes.size(), out));
    }
    {
        NetWriter w;
        w.u16(0);
        w.u32(NetLimits::kChat + 1);
        auto bytes = w.take();
        NetChat out;
        check("Chat refuses an over-long message", !NetChat::decode(bytes.data(), bytes.size(), out));
    }

    // Fuzz-ish: every decoder, over every prefix of a valid message, plus
    // noise. None of them may read out of bounds; ASan on CI is what actually
    // proves that, and this is what gives it something to look at.
    NetWelcome sample;
    sample.sessionName = "s";
    sample.roster.push_back(NetPeer{1, "p", "n", "b", true, 2, true});
    auto valid = sample.encode();

    for (size_t n = 0; n <= valid.size(); n++) {
        NetWelcome out;
        NetWelcome::decode(valid.data(), n, out);      // must not read out of bounds
    }
    for (uint32_t seed = 1; seed <= 200; seed++) {
        auto junk = noise(seed % 97, seed);
        NetWelcome w1; NetHello h1; NetRosterMsg r1; NetWorld d1; NetChat c1;
        NetWelcome::decode(junk.data(), junk.size(), w1);
        NetHello::decode(junk.data(), junk.size(), h1);
        NetRosterMsg::decode(junk.data(), junk.size(), r1);
        NetWorld::decode(junk.data(), junk.size(), d1);
        NetChat::decode(junk.data(), junk.size(), c1);
    }
    // Reaching this line at all is the assertion: anything out of bounds
    // above would have been a crash, or an ASan report on CI.
    check("every prefix and 200 random inputs decode without reading out of bounds", true);
}


// ------------------------------------------------------------------ url ----

void testUrlParsing() {
    printf("\n=== websocket url parsing ===\n");

    NetUrl u;
    check("wss with a path parses",
          NetUrl::parse("wss://relay.example/session/ABCD/ws", u) &&
          u.secure && u.host == "relay.example" && u.port == 443 &&
          u.path == "/session/ABCD/ws");

    check("an explicit port is taken",
          NetUrl::parse("wss://relay.example:8443/x", u) && u.port == 8443);

    check("ws defaults to port 80",
          NetUrl::parse("ws://localhost/x", u) && !u.secure && u.port == 80);

    check("a missing path becomes /",
          NetUrl::parse("wss://relay.example", u) && u.path == "/");

    check("a query string stays on the path",
          NetUrl::parse("wss://a.example/x?role=host", u) && u.path == "/x?role=host");

    check("a bracketed IPv6 literal parses",
          NetUrl::parse("wss://[2001:db8::1]:9000/x", u) &&
          u.host == "2001:db8::1" && u.port == 9000);

    // Everything below is refused rather than guessed at. A URL this is wrong
    // about is a connection to somewhere unintended.
    // The parser is shared with the HTTPS client, so it accepts all four
    // schemes and records which. Whether plaintext is ACCEPTABLE is a policy
    // question, answered in WebSocket::connect, not here.
    check("https parses as secure",
          NetUrl::parse("https://api.example/ticket", u) && u.secure && u.port == 443);
    check("http parses as insecure",
          NetUrl::parse("http://127.0.0.1:8787/x", u) && !u.secure && u.port == 8787);

    check("an unknown scheme is refused", !NetUrl::parse("ftp://a.example/x", u));
    check("a bare host is refused", !NetUrl::parse("relay.example/x", u));
    check("an empty authority is refused", !NetUrl::parse("wss:///x", u));

    // The oldest trick for making a URL look like it points somewhere else.
    check("userinfo is refused outright",
          !NetUrl::parse("wss://relay.example@evil.example/x", u));

    check("a non-numeric port is refused", !NetUrl::parse("wss://a.example:80x/y", u));
    check("port 0 is refused", !NetUrl::parse("wss://a.example:0/y", u));
    check("a port above 65535 is refused", !NetUrl::parse("wss://a.example:70000/y", u));
    check("an unterminated IPv6 literal is refused", !NetUrl::parse("wss://[2001:db8::1/x", u));

    // The path goes verbatim into the request line, so a space or a control
    // character in it would let a caller inject a second header.
    check("a space in the path is refused", !NetUrl::parse("wss://a.example/x y", u));
    check("a newline in the path is refused",
          !NetUrl::parse("wss://a.example/x\r\nX-Evil: 1", u));
    check("a control character in the host is refused",
          !NetUrl::parse("wss://a\texample/x", u));
}


void testLobbyMessages() {
    printf("\n=== lobby messages ===\n");

    {
        NetLobbyState in;
        in.state = NetSessionState::Lobby;
        in.assignment = NetAssignment::HostAssigns;
        in.roster.push_back(NetPeer{1, "p1", "Alice", "developer", true, 40, true, false, true});
        in.roster.push_back(NetPeer{2, "p2", "Bob", "", false, 0, false, true, false});
        auto bytes = in.encode();

        NetLobbyState out;
        check("LobbyState round-trips",
              NetLobbyState::decode(bytes.data(), bytes.size(), out) &&
              out.assignment == NetAssignment::HostAssigns &&
              out.roster.size() == 2 &&
              out.roster[0].countryId == 40 && out.roster[0].submitted &&
              out.roster[1].spectator && !out.roster[1].submitted);
    }
    {
        NetClaimCountry in{42};
        auto bytes = in.encode();
        NetClaimCountry out;
        check("ClaimCountry round-trips",
              NetClaimCountry::decode(bytes.data(), bytes.size(), out) && out.countryId == 42);
    }
    {
        NetSwap in{3, 7, true};
        auto bytes = in.encode();
        NetSwap out;
        check("Swap round-trips",
              NetSwap::decode(bytes.data(), bytes.size(), out) &&
              out.fromPeerId == 3 && out.toPeerId == 7 && out.accepted);
    }
    {
        NetNotice in{40, NetSubstitution::Malformed, "AI played France."};
        auto bytes = in.encode();
        NetNotice out;
        check("Notice round-trips",
              NetNotice::decode(bytes.data(), bytes.size(), out) &&
              out.countryId == 40 && out.reason == NetSubstitution::Malformed &&
              out.text == in.text);
        check("and every substitution reason has words",
              std::string(netSubstitutionReason(NetSubstitution::Malformed)).size() > 0 &&
              std::string(netSubstitutionReason(NetSubstitution::NotSubmitted)).size() > 0 &&
              std::string(netSubstitutionReason(NetSubstitution::Disconnected)).size() > 0);
    }
}

void testHostIdentity() {
    printf("\n=== host identity in WELCOME ===\n");

    NetWelcome in;
    in.state = NetSessionState::Game;
    in.assignment = NetAssignment::HostAssigns;
    in.lateJoin = NetLateJoin::Refuse;
    in.absent = NetAbsent::Idle;
    in.spectator = true;
    in.host.psid = "host-psid";
    in.host.name = "Hosty";
    in.host.badges = "developer";
    in.host.issuer = "https://official.example";
    in.host.verified = true;
    auto bytes = in.encode();

    NetWelcome out;
    check("Welcome carries the host and the session rules",
          NetWelcome::decode(bytes.data(), bytes.size(), out) &&
          out.state == NetSessionState::Game &&
          out.assignment == NetAssignment::HostAssigns &&
          out.lateJoin == NetLateJoin::Refuse &&
          out.absent == NetAbsent::Idle && out.spectator &&
          out.host.name == "Hosty" && out.host.verified);

    check("a declared host is recognised as declared", out.host.declared());

    NetWelcome anon;
    auto anonBytes = anon.encode();
    NetWelcome anonOut;
    check("a host that said nothing is NOT declared",
          NetWelcome::decode(anonBytes.data(), anonBytes.size(), anonOut) &&
          !anonOut.host.declared());
}

void testEnumsAreClamped() {
    printf("\n=== enums off the wire are clamped, not cast ===\n");

    // An out-of-range enum would otherwise become a switch that matches no
    // case, and every downstream `if (state == Lobby)` would silently be false.
    NetWriter w;
    w.u8(0); w.u8(0);                     // state, assignment
    w.u32(0);                             // empty roster
    auto good = w.take();
    NetLobbyState ok;
    check("a valid enum decodes", NetLobbyState::decode(good.data(), good.size(), ok));

    NetWriter bad;
    bad.u8(200); bad.u8(200);
    bad.u32(0);
    auto garbage = bad.take();
    NetLobbyState out;
    check("an out-of-range enum falls back to a real value",
          NetLobbyState::decode(garbage.data(), garbage.size(), out) &&
          out.state == NetSessionState::Lobby &&
          out.assignment == NetAssignment::PlayersPick);

    NetWriter n;
    n.u16(1); n.u8(99); n.str("x");
    auto noticeBytes = n.take();
    NetNotice notice;
    check("an unknown substitution reason becomes None",
          NetNotice::decode(noticeBytes.data(), noticeBytes.size(), notice) &&
          notice.reason == NetSubstitution::None);
}


void testWorldSync() {
    printf("\n=== world snapshots ===\n");

    NetWorldSnapshot s;
    s.mapName = "STDmaps/map";
    s.turnNumber = 7;
    s.stateJson = "{\"claims\":[1,2,3]}";
    s.turns.push_back(NetTurnDelta{1, {0xDE, 0xAD}});
    s.turns.push_back(NetTurnDelta{2, {0xBE, 0xEF, 0x00}});

    const std::vector<uint8_t> wire = s.encode();
    NetWorldSnapshot back;
    check("a snapshot round-trips",
          NetWorldSnapshot::decode(wire.data(), wire.size(), back) &&
          back.mapName == s.mapName && back.turnNumber == 7 &&
          back.stateJson == s.stateJson && back.turns.size() == 2 &&
          back.turns[1].turn == 2 && back.turns[1].packed.size() == 3);

    // A game that has not started carries no deltas, and that is not an error:
    // loading the named map already gives the world as it starts.
    NetWorldSnapshot fresh;
    fresh.mapName = "STDmaps/map";
    const std::vector<uint8_t> freshWire = fresh.encode();
    NetWorldSnapshot freshBack;
    check("a snapshot with no turns is valid",
          NetWorldSnapshot::decode(freshWire.data(), freshWire.size(), freshBack) &&
          freshBack.turns.empty());

    // Without a map there is nothing to load, so it is not a snapshot.
    NetWorldSnapshot nameless;
    const std::vector<uint8_t> namelessWire = nameless.encode();
    NetWorldSnapshot out;
    check("a snapshot with no map is refused",
          !NetWorldSnapshot::decode(namelessWire.data(), namelessWire.size(), out));

    check("an empty payload is refused",
          !NetWorldSnapshot::decode(nullptr, 0, out));

    // Truncation at every length must fail rather than half-load a world.
    bool everyTruncationRefused = true;
    for (size_t n = 0; n < wire.size(); n++) {
        NetWorldSnapshot partial;
        if (NetWorldSnapshot::decode(wire.data(), n, partial)) {
            everyTruncationRefused = false;
            printf("        accepted a %zu-byte prefix\n", n);
        }
    }
    check("every truncated prefix is refused", everyTruncationRefused);

    // Trailing bytes mean the sender and this build disagree about the shape.
    {
        std::vector<uint8_t> extra = wire;
        extra.push_back(0x00);
        NetWorldSnapshot t;
        check("trailing bytes are refused",
              !NetWorldSnapshot::decode(extra.data(), extra.size(), t));
    }

    // A count field is a claim, not a fact. Reserving on it before reading is
    // how a length becomes an allocation.
    {
        NetWriter w;
        w.str("STDmaps/map");
        w.u32(1);
        w.str("{}");
        w.u32(0xFFFFFFFFu);          // "four billion turns follow"
        const std::vector<uint8_t> lying = w.data();
        NetWorldSnapshot t;
        check("an impossible turn count is refused",
              !NetWorldSnapshot::decode(lying.data(), lying.size(), t));
    }
}


void testSignalling() {
    printf("\n=== peer-to-peer signalling ===\n");

    NetSignal s;
    s.kind = NetSignal::Kind::Offer;
    s.peerId = 42;
    s.payload = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n";

    const std::vector<uint8_t> wire = s.encode();
    NetSignal back;
    check("a signal round-trips",
          NetSignal::decode(wire.data(), wire.size(), back) &&
          back.kind == NetSignal::Kind::Offer && back.peerId == 42 &&
          back.payload == s.payload);

    // The payload is opaque here on purpose -- SDP and ICE candidates belong to
    // whatever implementation sits underneath, not to this layer.
    {
        NetSignal cand;
        cand.kind = NetSignal::Kind::Candidate;
        cand.payload = "candidate:1 1 UDP 2130706431 192.0.2.1 54321 typ host";
        const std::vector<uint8_t> w = cand.encode();
        NetSignal b;
        check("a candidate round-trips",
              NetSignal::decode(w.data(), w.size(), b) &&
              b.kind == NetSignal::Kind::Candidate && b.payload == cand.payload);
    }

    // An unknown kind must clamp rather than becoming a value nothing handles.
    {
        NetWriter w;
        w.u8(200);
        w.u16(1);
        w.str("x");
        const std::vector<uint8_t> v = w.data();
        NetSignal b;
        check("an unknown kind is clamped, not cast",
              NetSignal::decode(v.data(), v.size(), b) &&
              b.kind == NetSignal::Kind::Offer);
    }

    check("an empty payload is refused", !NetSignal::decode(nullptr, 0, back));
    {
        std::vector<uint8_t> extra = wire;
        extra.push_back(0);
        check("trailing bytes are refused",
              !NetSignal::decode(extra.data(), extra.size(), back));
    }
    {
        bool allRefused = true;
        for (size_t n = 0; n < wire.size(); n++) {
            NetSignal b;
            if (NetSignal::decode(wire.data(), n, b)) allRefused = false;
        }
        check("every truncation is refused", allRefused);
    }
}

// A mod message survives the wire, keeps binary payloads intact, and refuses
// anything oversized or truncated.
void testModMessages() {
    {
        NetModMsg m;
        m.modId = "od.example";
        m.peerId = 7;
        // Deliberately binary, NUL included: this payload is opaque bytes, and
        // a codec that stops at a NUL would silently truncate mods' data.
        m.payload = std::string("\x01\x00\xff\x00hi", 6);

        const std::vector<uint8_t> wire = m.encode();
        NetModMsg back;
        check("mod message round-trips",
              NetModMsg::decode(wire.data(), wire.size(), back));
        check("mod id survives", back.modId == "od.example");
        check("peer survives", back.peerId == 7);
        check("binary payload survives whole", back.payload == m.payload);
        check("a NUL does not end the payload", back.payload.size() == 6);

        bool allRefused = true;
        for (size_t n = 0; n < wire.size(); n++) {
            NetModMsg b;
            if (NetModMsg::decode(wire.data(), n, b)) allRefused = false;
        }
        check("every mod-message truncation is refused", allRefused);
    }
    {
        // The broadcast marker must not collide with a real peer id, or a
        // message meant for one player would go to everyone.
        check("broadcast is not a plausible peer",
              NetModMsg::kBroadcast == 0xFFFF);
    }
    {
        NetModMsg big;
        big.modId = "od.example";
        big.payload = std::string(NetLimits::kModMsg + 1, 'x');
        const std::vector<uint8_t> wire = big.encode();
        NetModMsg back;
        check("an oversized mod payload is refused, not clipped",
              !NetModMsg::decode(wire.data(), wire.size(), back));
    }
}

}  // namespace

int main() {
    printf("net protocol\n");
    testPrimitives();
    testReaderFailsClosed();
    testFraming();
    testMessages();
    testTurnStoreInfo();
    testMessagesRefuseGarbage();
    testLobbyMessages();
    testHostIdentity();
    testEnumsAreClamped();
    testUrlParsing();
    testWorldSync();
    testSignalling();
    testModMessages();
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
