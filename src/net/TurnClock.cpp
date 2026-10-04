#include "TurnClock.h"

#include <chrono>
#include <cstdio>

namespace turnclock {

namespace {
constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;

/** Floor division that rounds toward minus infinity, for times before 1970. */
int64_t floorDiv(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
}  // namespace

int64_t nowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int parseAnchor(const std::string& text) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 2) return -1;
    if (text.size() != colon + 3) return -1;
    int h = 0, m = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == colon) continue;
        if (text[i] < '0' || text[i] > '9') return -1;
    }
    h = std::stoi(text.substr(0, colon));
    m = std::stoi(text.substr(colon + 1));
    if (h > 23 || m > 59) return -1;
    return h * 60 + m;
}

std::string formatAnchor(int minutes) {
    if (minutes < 0) return "";
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", (minutes / 60) % 24, minutes % 60);
    return buf;
}

int64_t nextDeadline(int64_t nowMs, uint32_t turnSeconds, int anchorMinutes) {
    if (turnSeconds == 0) return 0;
    const int64_t stepMs = (int64_t)turnSeconds * 1000;
    if (anchorMinutes < 0) return nowMs + stepMs;

    // The grid passes through today's anchor and repeats every interval, so
    // the first point at or after `earliest` is found by stepping from it.
    const int64_t earliest = nowMs + stepMs / 2;
    const int64_t gridOrigin = floorDiv(nowMs, kDayMs) * kDayMs +
                               (int64_t)anchorMinutes * 60 * 1000;
    const int64_t k = floorDiv(earliest - gridOrigin + stepMs - 1, stepMs);
    return gridOrigin + k * stepMs;
}

int64_t resumeRemainingMs(int64_t savedMs, int64_t nowMs, int64_t graceMs) {
    if (savedMs <= 0) return -1;
    const int64_t left = savedMs - nowMs;
    return left > 0 ? left : (graceMs > 0 ? graceMs : 0);
}

uint32_t clampForWire(int64_t ms) {
    if (ms <= 0) return 0;
    if (ms > 0xFFFFFFFFLL) return 0xFFFFFFFFu;
    return (uint32_t)ms;
}

std::string describe(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t s = ms / 1000;
    const int64_t d = s / 86400; s %= 86400;
    const int64_t h = s / 3600;  s %= 3600;
    const int64_t m = s / 60;    s %= 60;
    char buf[48];
    if (d > 0)      std::snprintf(buf, sizeof buf, "%lldd %lldh", (long long)d, (long long)h);
    else if (h > 0) std::snprintf(buf, sizeof buf, "%lldh %lldm", (long long)h, (long long)m);
    else if (m > 0) std::snprintf(buf, sizeof buf, "%lldm %llds", (long long)m, (long long)s);
    else            std::snprintf(buf, sizeof buf, "%llds", (long long)s);
    return buf;
}

}  // namespace turnclock
