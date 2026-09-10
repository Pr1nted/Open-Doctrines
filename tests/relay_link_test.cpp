// The relay's framing, which is the half of multiplayer that has no host in
// it to notice a mistake.
//
// A wrong byte here does not fail loudly. A peer id assembled little-endian at
// one end and big-endian at the other drops nobody's connection -- it delivers
// one player's orders to a different player, in a game where that is the whole
// point. So the endianness, the header length and the URL are pinned here
// against what net/src/lobby/LobbyDO.ts actually does.

#include "net/RelayLink.h"

#include <cstdio>
#include <string>

static int checks = 0, fails = 0;
static void ok(bool c, const std::string& what) {
    ++checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++fails;
}
static void section(const char* t) { printf("\n== %s ==\n", t); }

int main() {
    printf("Relay framing\n");

    section("relay -> host");
    {
        // kind=Data(1), peer 0x0102 little-endian, then "hi"
        const uint8_t f[] = {1, 0x02, 0x01, 'h', 'i'};
        netrelay::Inbound in;
        ok(netrelay::decodeToHost(f, sizeof f, in), "a data frame decodes");
        ok(in.kind == netrelay::ToHost::Data, "with its kind");
        ok(in.peerId == 0x0102, "and its peer id read LITTLE-endian");
        ok(in.payload.size() == 2 && in.payload[0] == 'h' && in.payload[1] == 'i',
           "and the payload after the three-byte header");
    }
    {
        const uint8_t f[] = {3, 7, 0};
        netrelay::Inbound in;
        ok(netrelay::decodeToHost(f, sizeof f, in) &&
           in.kind == netrelay::ToHost::PeerLeft && in.peerId == 7 && in.payload.empty(),
           "a departure carries no payload");
    }
    {
        netrelay::Inbound in;
        const uint8_t two[] = {1, 0};
        ok(!netrelay::decodeToHost(two, sizeof two, in), "a frame shorter than the header is refused");
        ok(!netrelay::decodeToHost(nullptr, 0, in), "so is nothing at all");
        // An unknown tag is a NEWER RELAY, not an attack: refused so the caller
        // drops the frame, and deliberately not fatal.
        const uint8_t odd[] = {99, 1, 0, 'x'};
        ok(!netrelay::decodeToHost(odd, sizeof odd, in), "an unknown kind is refused");
    }

    section("host -> relay");
    {
        const uint8_t body[] = {'a', 'b'};
        const auto f = netrelay::encodeFromHost(netrelay::FromHost::ToPeer, 0x0102, body, 2);
        ok(f.size() == 5, "three bytes of header and the payload");
        ok(f[0] == 1, "the kind");
        ok(f[1] == 0x02 && f[2] == 0x01, "the peer id, little-endian, as the relay reads it");
        ok(f[3] == 'a' && f[4] == 'b', "then the payload");
    }
    {
        const auto f = netrelay::encodeFromHost(netrelay::FromHost::Broadcast, 0, nullptr, 0);
        ok(f.size() == 3 && f[0] == 2, "a broadcast is a header and nothing else");
    }
    {
        // Round trip: what the host encodes for a peer is what the relay would
        // read back out, which is the property that actually matters.
        const uint8_t body[] = {9, 8, 7};
        const auto f = netrelay::encodeFromHost(netrelay::FromHost::ToPeer, 65535, body, 3);
        netrelay::Inbound in;
        ok(netrelay::decodeToHost(f.data(), f.size(), in) && in.peerId == 65535,
           "the highest peer id survives a round trip");
    }

    section("the hello, and its answer");
    {
        const std::string h = netrelay::helloFrame("abc.def-ghi_jkl");
        ok(h == "{\"ticket\":\"abc.def-ghi_jkl\"}", "a ticket goes out as json text");
        // A ticket is base64url, so there is nothing legitimate to escape --
        // and anything else is dropped rather than quoted into the document.
        ok(netrelay::helloFrame("bad\"}{x").find('"') != std::string::npos &&
           netrelay::helloFrame("bad\"}{x") == "{\"ticket\":\"badx\"}",
           "characters that could break out of the json are dropped");
    }
    {
        uint16_t id = 0; std::string role;
        ok(netrelay::parseHelloReply("{\"ok\":true,\"peerId\":42,\"role\":\"player\"}", id, role),
           "the relay's acceptance parses");
        ok(id == 42, "with the peer id it assigned");
        ok(role == "player", "and the role it granted");

        ok(!netrelay::parseHelloReply("{\"ok\":false}", id, role), "a refusal is not an acceptance");
        ok(!netrelay::parseHelloReply("", id, role), "nor is nothing");
        ok(!netrelay::parseHelloReply("{\"ok\":true}", id, role), "nor an acceptance with no peer id");
    }

    section("THE URL IS BUILT, NEVER READ OUT OF A REPLY");
    {
        const std::string iss = "https://opendoctrines-net.opendoctrines.workers.dev";
        ok(netrelay::relayUrl(iss, "ABCD-2345", "player") ==
           "wss://opendoctrines-net.opendoctrines.workers.dev/session/ABCD-2345/ws?role=player",
           "https becomes wss");
        ok(netrelay::relayUrl("http://localhost:8787", "ABCD-2345", "host") ==
           "ws://localhost:8787/session/ABCD-2345/ws?role=host",
           "and http becomes ws, so a local service still works");
        ok(netrelay::relayUrl(iss + "/", "ABCD-2345", "player").find("dev/session/") != std::string::npos,
           "a trailing slash does not double up");

        ok(netrelay::relayUrl(iss, "nope", "player").empty(), "a code that is not a code gets no url");
        ok(netrelay::relayUrl("ftp://elsewhere", "ABCD-2345", "player").empty(),
           "a scheme this does not understand gets no url");
        ok(netrelay::relayUrl("", "ABCD-2345", "player").empty(), "and neither does no issuer");
        // The refusals that matter: anything that could make one URL into
        // another once an attacker controls part of the issuer setting.
        ok(netrelay::relayUrl("https://host/?x=", "ABCD-2345", "player").empty(),
           "a query in the issuer is refused");
        ok(netrelay::relayUrl("https://host/#f", "ABCD-2345", "player").empty(),
           "so is a fragment");
        ok(netrelay::relayUrl("https://host x", "ABCD-2345", "player").empty(),
           "so is whitespace");
        ok(netrelay::relayUrl(iss, "ABCD-2345", "pla yer").empty(),
           "and a role that is not a bare word");
    }

    section("codes people type, paste and read off a stream");
    {
        ok(netrelay::normaliseCode("abcd-2345") == "ABCD-2345", "lower case is lifted");
        ok(netrelay::normaliseCode("ABCD2345")  == "ABCD-2345", "a missing dash is put back");
        ok(netrelay::normaliseCode(" abcd 2345 ") == "ABCD-2345", "spaces anywhere are dropped");
        ok(netrelay::normaliseCode("ABCD-2345") == "ABCD-2345", "an already-correct code is unchanged");

        // NOTHING IS GUESSED. I, L, O, 0 and 1 are absent from the alphabet so
        // that nothing is ambiguous; a code containing one is not a code.
        // Repairing it would send somebody to a stranger's game.
        ok(netrelay::normaliseCode("ABCI-2345").empty(), "a letter outside the alphabet is refused, not repaired");
        ok(netrelay::normaliseCode("ABC0-2345").empty(), "and so is a zero");
        ok(netrelay::normaliseCode("ABCD-234").empty(),  "too short is refused");
        ok(netrelay::normaliseCode("ABCD-23456").empty(), "too long is refused");
        ok(netrelay::normaliseCode("").empty(), "and empty is not a code");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
