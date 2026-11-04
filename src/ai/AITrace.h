#pragma once
// ─── THE VISUALISER'S HOOK ──────────────────────────────────
//
// Groundwork item 6 of docs/ai/REWORK_PLAN.md, and everything the 1.3.3b
// visualiser needs from the AI.
//
// One record per module decision -- the same granularity as OD_DECISION_HASH,
// and the same moment: after the action is final (book, search, policy, or a
// mod's override), before the executor runs it. It carries what produced the
// choice: the input features, every layer of the trunk and of the module's
// policy head as they fired, the raw logits, the mask, the distribution the
// policy actually sampled from, what was picked, and the value net's estimate.
//
// OPT-IN AND FREE WHEN OFF. AISystem holds a raw pointer; with nothing
// attached the whole thing is one null check per decision. When something IS
// attached, the record is built by re-running the nets through
// NeuralNet::forwardInto into a scratch of the tracer's own: that reads weights
// and writes nothing the AI reads, and draws no random numbers, so a traced
// game makes exactly the decisions an untraced one does. The decision hash is
// the proof, and docs/ai/TRACE_FORMAT.md says how to check it.
//
// The file writer below (OD_AI_TRACE=<path>) is the first sink. A test or an
// in-game panel can implement AITraceSink directly instead.

#include <cstdio>
#include <string>
#include <vector>
#include <deque>
#include <mutex>

struct AIDecisionTrace {
    int turn = 0;
    int cid = 0;
    int module = 0;          ///< AISystem::Module
    int pick = 0;            ///< 0 = the module's first pick this turn, then 1, 2...
    int action = 0;          ///< what was chosen, after every override
    /// Who chose: "policy", "book", "script", "random", "search", "league", "mod".
    const char* source = "policy";
    float valueEstimate = 0.0f;   ///< m_value[module] on `features`
    std::vector<float> features;  ///< the trunk's input
    /// Activations per layer, input copy first, output last. Empty when the
    /// net was not built (a model file without that head).
    std::vector<std::vector<float>> trunkActs;
    std::vector<std::vector<float>> policyActs;
    std::vector<float> logits;    ///< the policy head's raw output
    std::vector<bool>  valid;     ///< the legality mask
    /// The distribution the policy sampled from, after mask, bias and mixing.
    /// Empty when no net drove the pick (book, script, random).
    std::vector<float> probs;
    /// The steering added to each action's raw logit before masking: stance,
    /// war-staging, coalition, the LLM advisor's intent, the search's scores and
    /// the critic's Q, summed (see AISystem qBiasFor). This is "what moved the
    /// choice" away from the policy's own preference. Empty when the pick took no
    /// bias (book, script, random, search-visits) or none applied.
    std::vector<float> qbias;
};

class AITraceSink {
public:
    virtual ~AITraceSink() = default;
    virtual void onDecision(const AIDecisionTrace& d) = 0;
};

/**
 * JSON Lines, one decision per line. Format in docs/ai/TRACE_FORMAT.md.
 *
 * JSONL rather than binary because the visualiser is to be readable from a
 * browser page and from Python on a training server alike, and a trace is a
 * debugging artifact whose size is bounded by how long someone lets it run.
 * Floats are written with %.6g, which round-trips what a reader needs to draw.
 */
class AITraceFileWriter final : public AITraceSink {
public:
    explicit AITraceFileWriter(const std::string& path);
    ~AITraceFileWriter() override;
    AITraceFileWriter(const AITraceFileWriter&) = delete;
    AITraceFileWriter& operator=(const AITraceFileWriter&) = delete;
    bool ok() const { return m_f != nullptr; }
    void onDecision(const AIDecisionTrace& d) override;
private:
    std::FILE* m_f = nullptr;
    std::string m_buf;
};

/**
 * Keeps the most recent decision traces in memory for an in-game panel to draw
 * -- the "Decision timeline" view of the 1.3.4b visualiser (REWORK_PLAN.md).
 *
 * Free when not attached (AISystem tests one null pointer per decision). The
 * game attaches it only while the AI-debug overlay is on, so a normal session
 * and every bench/training run -- which attach nothing, or the file writer --
 * are untouched and the decision hash is unchanged. Mutex-guarded because a
 * headless or training run can drive the AI off the render thread; normal play
 * decides and draws on the one thread, where the lock is uncontended.
 */
class AITraceRing final : public AITraceSink {
public:
    void onDecision(const AIDecisionTrace& d) override;
    /// The most recent `n` decisions, newest first (a copy, so the caller can
    /// read them without holding the lock while it draws).
    std::vector<AIDecisionTrace> recent(int n) const;
    void clear();
private:
    mutable std::mutex m_mu;
    std::deque<AIDecisionTrace> m_buf;
    static constexpr size_t kCap = 64;
};
