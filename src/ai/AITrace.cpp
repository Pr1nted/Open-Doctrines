// The OD_AI_TRACE file writer. See AITrace.h and docs/ai/TRACE_FORMAT.md.
#include "AITrace.h"

#include <cmath>
#include <cstdio>

namespace {

// %.6g, with the two values JSON has no spelling for written as null rather
// than as a token every parser rejects. A NaN in an activation is exactly the
// thing somebody opens a trace to find, so the line must still parse.
void putFloat(std::string& b, float v) {
    if (!std::isfinite(v)) { b += "null"; return; }
    char tmp[32];
    std::snprintf(tmp, sizeof tmp, "%.6g", (double)v);
    b += tmp;
}

void putFloats(std::string& b, const std::vector<float>& v) {
    b += '[';
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) b += ',';
        putFloat(b, v[i]);
    }
    b += ']';
}

void putLayers(std::string& b, const std::vector<std::vector<float>>& layers) {
    b += '[';
    for (size_t i = 0; i < layers.size(); ++i) {
        if (i) b += ',';
        putFloats(b, layers[i]);
    }
    b += ']';
}

void putInt(std::string& b, long long v) {
    char tmp[32];
    std::snprintf(tmp, sizeof tmp, "%lld", v);
    b += tmp;
}

} // namespace

AITraceFileWriter::AITraceFileWriter(const std::string& path) {
    // Truncated, not appended: one run, one trace. A reader that finds two
    // runs' turns interleaved in a file cannot tell them apart.
    m_f = std::fopen(path.c_str(), "wb");
    if (!m_f)
        std::fprintf(stderr, "[AI] OD_AI_TRACE: cannot open %s for writing; tracing off\n",
                     path.c_str());
}

AITraceFileWriter::~AITraceFileWriter() {
    if (m_f) std::fclose(m_f);
}

void AITraceFileWriter::onDecision(const AIDecisionTrace& d) {
    if (!m_f) return;
    std::string& b = m_buf;
    b.clear();
    b += "{\"v\":1,\"turn\":"; putInt(b, d.turn);
    b += ",\"cid\":";    putInt(b, d.cid);
    b += ",\"module\":"; putInt(b, d.module);
    b += ",\"pick\":";   putInt(b, d.pick);
    b += ",\"action\":"; putInt(b, d.action);
    // `source` is one of a fixed set of literals (AITrace.h), none of which
    // needs escaping.
    b += ",\"source\":\""; b += d.source ? d.source : "policy"; b += '"';
    b += ",\"value\":";  putFloat(b, d.valueEstimate);
    b += ",\"valid\":[";
    for (size_t i = 0; i < d.valid.size(); ++i) {
        if (i) b += ',';
        b += d.valid[i] ? '1' : '0';
    }
    b += ']';
    b += ",\"logits\":";   putFloats(b, d.logits);
    b += ",\"probs\":";    putFloats(b, d.probs);
    b += ",\"features\":"; putFloats(b, d.features);
    b += ",\"trunk\":";    putLayers(b, d.trunkActs);
    b += ",\"policy\":";   putLayers(b, d.policyActs);
    b += "}\n";
    std::fwrite(b.data(), 1, b.size(), m_f);
}
