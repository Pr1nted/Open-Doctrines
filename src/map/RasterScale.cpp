#include "RasterScale.h"

#include <cstdlib>

#ifndef OD_MAP_DIVISOR
#define OD_MAP_DIVISOR 1
#endif

namespace odmap {
namespace {

int sanitise(int d) { return (d == 2 || d == 4 || d == 8) ? d : 1; }

int g_divisor = [] {
    if (const char* e = std::getenv("OD_MAP_DIVISOR")) return sanitise(std::atoi(e));
    return sanitise(OD_MAP_DIVISOR);
}();

}  // namespace

int rasterDivisor() { return g_divisor; }
void setRasterDivisor(int divisor) { g_divisor = sanitise(divisor); }

}  // namespace odmap
