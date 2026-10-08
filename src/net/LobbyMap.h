#pragma once

// A small political map of the world, for picking a country in the lobby.
//
// WHY THE HOST SENDS ONE
//
// A joining player has no world in the lobby. The world arrives with the
// snapshot when the game starts -- seconds to load and tens of megabytes held
// -- and a player who has not decided yet whether to stay should not pay for
// it. But choosing a country from a list of 180 names is choosing without
// seeing: nobody picks "Kingdom of the Serbs, Croats and Slovenes" by name.
//
// And the map they would load is the wrong one anyway. A resumed campaign has
// moved every border since turn one; only the host knows where they are now.
// So the host renders its own world, at a few hundred pixels across, as one
// country id per cell. Run-length encoded, a 512x256 world is tens of
// kilobytes, and clicking a cell names a country exactly -- no colour matching
// on the client, which is where two similar greens would have become one.
//
// Display only. A click becomes a ClaimCountry request, which the server
// decides like any other.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct LobbyMap {
    uint16_t width = 0;
    uint16_t height = 0;
    /** Country id per cell, row-major. 0 is sea or nobody's land. */
    std::vector<uint16_t> cells;

    struct Swatch {
        uint16_t countryId = 0;
        uint8_t r = 0, g = 0, b = 0;
    };
    /** Each country's colour, as the host draws it. */
    std::vector<Swatch> colors;

    /** Largest raster accepted from the wire, so a hostile one cannot balloon. */
    static constexpr uint16_t kMaxWidth = 1024;
    static constexpr uint16_t kMaxHeight = 512;

    bool empty() const { return width == 0 || height == 0 || cells.empty(); }

    /**
     * Sample a world into a map `width` x `height`.
     *
     * `countryAt(u, v)` is asked for the owner at normalised coordinates, one
     * call per cell, at the cell's centre. `colorOf(id)` gives each country's
     * colour as 0xRRGGBB, asked once per country that appears.
     */
    static LobbyMap build(uint16_t width, uint16_t height,
                          const std::function<uint16_t(float u, float v)>& countryAt,
                          const std::function<uint32_t(uint16_t id)>& colorOf);

    std::vector<uint8_t> encode() const;
    /** Fail-closed: anything malformed or over the bounds leaves `out` empty. */
    static bool decode(const uint8_t* data, size_t size, LobbyMap& out);

    /** The country at normalised (u, v), or 0 outside the map or on sea. */
    uint16_t countryAt(float u, float v) const;

    /** 0xRRGGBB for a country, or 0 when it has no swatch. */
    uint32_t colorOf(uint16_t countryId) const;

    /**
     * Where to write a country's name: the centre of its largest connected
     * piece, normalised. False when the country is not on the map at all.
     *
     * The mean of all its cells would put France's label in the Atlantic once
     * French Guiana is counted, so it is the homeland's centre only.
     */
    bool labelPoint(uint16_t countryId, float& u, float& v) const;
};

/**
 * Which of `names` match what was typed into the picker's search box, best
 * first: names that start with it, then names with a word that starts with it
 * ("Korea" finds "North Korea"), then names that merely contain it. Case does
 * not matter. An empty query keeps every name, in the order given.
 */
std::vector<size_t> lobbyCountryFilter(const std::vector<std::string>& names,
                                       const std::string& query);
