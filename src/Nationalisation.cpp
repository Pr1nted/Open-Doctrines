#include "Nationalisation.h"

#include <algorithm>
#include <cmath>

namespace odnat {

int capFor(float economic) {
    // NaN loses every comparison, so it must not reach the arithmetic: a
    // compass that somehow held one would otherwise produce a cap of INT_MIN
    // and a country that could nationalise nothing, for a reason nobody could
    // see on the screen.
    if (!(economic >= -100.0f)) economic = -100.0f;
    if (!(economic <=  100.0f)) economic =  100.0f;
    const int cap = (int)((100.0f - economic) / 40.0f);
    return std::clamp(cap, 0, 5);
}

float step(float ramp, bool held) {
    const float d = 1.0f / (float)kRampTurns;
    return std::clamp(ramp + (held ? d : -d), 0.0f, 1.0f);
}

float buildCostMul(float ramp) { return 1.0f + kCostMax   * std::clamp(ramp, 0.0f, 1.0f); }
float upkeepMul(float ramp)    { return 1.0f + kUpkeepMax * std::clamp(ramp, 0.0f, 1.0f); }
float outputMul(float ramp)    { return 1.0f + kOutputMax * std::clamp(ramp, 0.0f, 1.0f); }
float unrestPct(float ramp)    { return kUnrestMax * std::clamp(ramp, 0.0f, 1.0f); }

std::vector<std::string> overCap(const std::vector<Holding>& holdings, int cap) {
    std::vector<const Holding*> live;
    for (const Holding& h : holdings)
        if (h.held) live.push_back(&h);

    const int over = (int)live.size() - std::max(0, cap);
    if (over <= 0) return {};

    std::sort(live.begin(), live.end(), [](const Holding* a, const Holding* b) {
        if (a->ramp != b->ramp) return a->ramp < b->ramp;
        return a->resource < b->resource;
    });

    std::vector<std::string> out;
    for (int i = 0; i < over; ++i) out.push_back(live[i]->resource);
    return out;
}

}  // namespace odnat
