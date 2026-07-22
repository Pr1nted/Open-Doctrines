#pragma once
#include <string>
#include <cstdint>

struct Province {
    int id = 0;
    int countryId = 0;
    std::string name;
    std::string isoA3;

    uint8_t r = 0, g = 0, b = 0;

    static int colorToId(uint8_t r, uint8_t g, uint8_t b) {
        return (static_cast<int>(r) << 16) | (static_cast<int>(g) << 8) | b;
    }
};
