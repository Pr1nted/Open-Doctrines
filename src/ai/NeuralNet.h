#pragma once
#include <cstdint>
#include <random>
#include <string>
#include <vector>

// Minimal self-contained MLP for the country AI. Vendored (no deps) to match
// the project's stb/miniz approach — the game must build anywhere.
//
// Design goals, in order: fast forward passes (hundreds of countries think
// every turn), debuggable (deterministic seeding, inspectable activations,
// stable binary serialization), and just enough training machinery for
// REINFORCE-style policy-gradient updates with an Adam optimizer.
//
// The net is a plain dense stack: input -> hidden(tanh) ... -> output(linear).
// Policy heads apply softmax externally (see softmax/samplePolicy) so the same
// net can also be used as a value/baseline head with a linear output.
class NeuralNet {
public:
    NeuralNet() = default;
    // layerSizes e.g. {64, 48, 12}: 64 inputs, one 48-wide hidden layer,
    // 12 outputs. Weights are Xavier-initialized from `seed` so runs are
    // reproducible when debugging.
    NeuralNet(const std::vector<int>& layerSizes, uint32_t seed);

    // Forward pass. `in` must have layerSizes.front() elements. Returns the
    // raw output (logits / value). No allocations after the first call.
    const std::vector<float>& forward(const std::vector<float>& in);

    // REINFORCE update for a policy head: pushes the log-probability of
    // `action` up (advantage > 0) or down (advantage < 0). Call forward()
    // first with the same input — the update uses the cached activations.
    // Softmax is applied internally to the cached logits.
    void policyGradientUpdate(int action, float advantage, float lr);

    // Squared-error update for a value head (output size 1): moves output
    // toward `target`. Call forward() first.
    void valueUpdate(float target, float lr);

    // Serialization: binary blob with magic + architecture; load fails (returns
    // false) on any mismatch rather than half-loading.
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    void serialize(std::vector<uint8_t>& out) const;
    bool deserialize(const uint8_t* data, size_t size);

    const std::vector<int>& architecture() const { return m_sizes; }
    int inputSize() const { return m_sizes.empty() ? 0 : m_sizes.front(); }
    int outputSize() const { return m_sizes.empty() ? 0 : m_sizes.back(); }
    bool valid() const { return m_sizes.size() >= 2; }

    // Introspection for the AI debug overlay.
    const std::vector<float>& lastOutput() const { return m_acts.empty() ? m_empty : m_acts.back(); }
    uint64_t updateCount() const { return m_updates; }

    // ── Helpers used by the AI system ──
    static void softmax(const std::vector<float>& logits, float temperature,
                        std::vector<float>& probsOut);
    // Samples an action index from softmaxed logits at `temperature`;
    // temperature <= ~0.05 collapses to argmax (the "insane" end).
    static int samplePolicy(const std::vector<float>& logits, float temperature,
                            std::mt19937& rng);

private:
    struct Layer {
        int in = 0, out = 0;
        std::vector<float> w;      // out x in, row-major
        std::vector<float> b;      // out
        // Adam state
        std::vector<float> mw, vw, mb, vb;
    };

    std::vector<int> m_sizes;
    std::vector<Layer> m_layers;
    std::vector<std::vector<float>> m_acts;   // activations per layer (incl. input copy)
    std::vector<std::vector<float>> m_grads;  // scratch for backprop
    std::vector<float> m_empty;
    uint64_t m_updates = 0;
    int m_adamT = 0;

    void backprop(const std::vector<float>& outputGrad, float lr);
};
