#pragma once
// Minimal population data still needed at compile time.
// Country populations and city databases are now fetched at runtime
// from the World Bank API and Natural Earth Populated Places shapefile.

#include <string>
#include <unordered_map>

// Name-to-ISO override for countries that Natural Earth returns as "-99"
inline const std::unordered_map<std::string, std::string>& getIsoOverrides() {
    static const std::unordered_map<std::string, std::string> overrides = {
        {"France", "FRA"},
        {"Norway", "NOR"},
        {"N. Cyprus", "XNC"},  // Northern Cyprus (unrecognised)
        {"Somaliland", "XSO"},  // Somaliland (unrecognised)
    };
    return overrides;
}