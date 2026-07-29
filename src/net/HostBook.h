#pragma once

// Who had which country, remembered across sessions.
//
// WHY THIS EXISTS
//
// A network game is one machine's view of a world somebody else is the
// authority for, and the lobby already knows how to give a returning player
// their seat back: `Lobby::admit()` treats a psid it has seen as a reconnect
// and keeps the country. That works beautifully within one session and is
// forgotten entirely when the host closes the game.
//
// A campaign played over weeks is exactly the case where that matters most, so
// the roster is written beside the save and read back when the host resumes.
// Then a player who returns three days later is, as far as the lobby is
// concerned, a player who reconnected -- the same code path, not a parallel one.
//
// A SIDECAR, NOT A SAVE FORMAT CHANGE
//
// This lives in `<save>.odhost` rather than inside the .odsv. The save
// format is shared with singleplayer and read by tools, and widening it for a
// multiplayer-only concern would make every reader carry the concept. The cost
// is that the two files can be separated -- and when that happens the game is
// still perfectly playable, everyone simply picks a country again. Losing this
// file loses convenience, never a world.
//
// WHAT IS DELIBERATELY NOT IN HERE
//
// No addresses, no tokens, no account identifiers. A `psid` is already the
// pairwise pseudonym -- meaningless to anyone but this server -- which is what
// makes it safe to write to disk beside a save a player might share.

#include <cstdint>
#include <string>
#include <vector>

struct SeatRecord {
    /** Pairwise pseudonymous id. Identity, and the only identity used here. */
    std::string psid;
    /** Display name, so the lobby can show who a held seat belongs to. */
    std::string name;
    uint16_t    countryId = 0;
};

/** How the table was set up. Restored so a resumed game plays as it did. */
struct HostSettings {
    uint32_t turnSeconds = 0;
    uint8_t  maxPlayers  = 8;
    uint8_t  lateJoin    = 0;   // 0 refuse, 1 spectate
    uint8_t  absent      = 0;   // 0 AI plays them, 1 their country idles
    uint8_t  assignment  = 0;   // 0 host assigns, 1 players pick
    bool     bindAll     = false;
    bool     listed      = false;
    uint16_t port        = 27015;
};

struct HostBook {
    /**
     * The map the save was built from.
     *
     * Kept because joiners are told a map NAME and resolve it locally -- the
     * host's paths mean nothing on their machine. A resumed game must announce
     * the same map the original did, and the save itself embeds the map data
     * without saying which catalogue entry it came from.
     */
    std::string mapId;
    uint32_t    turnNumber = 0;
    std::vector<SeatRecord> seats;

    /**
     * People barred from this game, by psid.
     *
     * On disk because a ban that evaporated when the host closed the game
     * would be no ban at all -- the person barred on Tuesday would walk back
     * in on Wednesday, and the host would have to remember and do it again.
     */
    std::vector<std::string> bans;

    /** The table's rules, so continuing a campaign continues its settings. */
    HostSettings settings;

    std::string encode() const;
    static bool decode(const std::string& json, HostBook& out);

    /** `<savePath>.odhost` -- beside the save it belongs to. */
    static std::string pathFor(const std::string& savePath);

    bool save(const std::string& savePath) const;
    static bool load(const std::string& savePath, HostBook& out);
};
