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

    /**
     * Make the OUTPUT layer activated (tanh) like the hidden ones.
     *
     * A shared trunk needs this. Splitting {F,512,320,N} into a trunk
     * {F,512,320} and a head {320,N} reproduces the original topology exactly
     * -- except for one detail: 512->320 is a HIDDEN layer in the monolith, so
     * it is squashed, and the LAST layer of the trunk, so it would not be.
     * Without this the trunk emits raw pre-activations and the split is not the
     * same function it replaced.
     *
     * Set at construction and preserved across deserialize(), which rebuilds
     * the net at the target shape and would otherwise silently drop it.
     */
    void setTanhOutput(bool on) { m_tanhOutput = on; }
    bool tanhOutput() const { return m_tanhOutput; }

    // Forward pass. `in` must have layerSizes.front() elements. Returns the
    // raw output (logits / value). No allocations after the first call.
    const std::vector<float>& forward(const std::vector<float>& in);

    // REINFORCE update for a policy head: pushes the log-probability of
    // `action` up (advantage > 0) or down (advantage < 0). Call forward()
    // first with the same input — the update uses the cached activations.
    // Softmax is applied internally to the cached logits.
    void policyGradientUpdate(int action, float advantage, float lr);
    /** Immediate cross-entropy step toward `target`. Call forward() first.
     *  The batched equivalent is accumulateCrossEntropyInto. */
    void crossEntropyUpdate(int target, float lr, const std::vector<uint8_t>* validMask = nullptr);

    // ── Batched policy gradient ──
    //
    // REINFORCE with a batch of one is the noisiest estimator there is: every
    // Adam step chases a single sampled outcome, and the optimiser's moment
    // estimates spend most of their time undoing the previous sample. The AI
    // already produces hundreds of experiences per turn, all settling in the
    // same learning step, so averaging their gradients before taking ONE step
    // costs nothing and cuts the variance by roughly sqrt(batch).
    //
    // Usage: accumulate() with the cached activations restored for each sample,
    // then flushBatch() once. accumulate() does not touch the weights.
    void accumulatePolicyGradient(int action, float advantage);
    void accumulateValueGradient(float target);
    /** Applies the mean of everything accumulated. No-op on an empty batch. */
    void flushBatch(float lr);
    int batchSize() const { return m_batchN; }


    // ── Thread-private accumulation ──
    //
    // The learning step is hundreds of independent samples per turn, measured
    // at ~31% of turn time on a 40-country map. They are independent right up
    // to the point where their gradients are summed, which is exactly the
    // shape that parallelises — but the net itself is stateful (activations,
    // backprop scratch, accumulators), so concurrent callers would trample
    // each other. A Scratch is one worker's private copy of all of that; the
    // WEIGHTS stay shared and are only read during accumulation.
    //
    // This is also the shape a GPU port would need, so it is not throwaway
    // work if compute shaders ever become available (they are not on macOS —
    // Apple's OpenGL stops at 4.1 and compute needs 4.3).
    struct Scratch {
        std::vector<std::vector<float>> acts, grads;
        std::vector<std::vector<float>> gw, gb;  // per-layer gradient sums
        int n = 0;
    };
    void initScratch(Scratch& s) const;
    /** Forward pass into `s`. Does not touch the net's own activations. */
    const std::vector<float>& forwardInto(Scratch& s, const std::vector<float>& in) const;
    /** Overwrite `s.acts` with activations captured earlier by snapshotActs. */
    static void loadActs(Scratch& s, std::vector<std::vector<float>>&& acts) { s.acts = std::move(acts); }
    void accumulatePolicyInto(Scratch& s, int action, float advantage) const;
    void accumulateValueInto(Scratch& s, float target) const;
    /**
     * Regress ONE output toward a target, leaving the others alone.
     *
     * This is what an action-value head needs: a window teaches what the action
     * actually taken was worth and says nothing about the ones that were not,
     * so every other output must receive no gradient at all. Regressing the
     * whole vector toward the same number would train Q to be flat, which is
     * the one shape that makes it useless for choosing between actions.
     */
    void accumulateActionValueInto(Scratch& s, int action, float target) const;
    /**
     * The PPO clipped surrogate, in place of the plain policy gradient.
     *
     * accumulatePolicyInto assumes the weights that chose the action are the
     * weights being updated. Here they are not: a decision waits N_STEP turns
     * for its reward, and the policy has been updated hundreds of times by the
     * time it arrives. Every sample is therefore off-policy by an amount nobody
     * was measuring, and the plain gradient treats it as if it were fresh.
     *
     * `oldLogProb` is log pi(a|s) as it stood when the action was taken, so the
     * ratio pi_new/pi_old says exactly how far the policy has moved on this
     * decision. Weighting by it corrects the estimate; clipping it flattens the
     * objective once the move is large enough, which is what stops one stale
     * batch from dragging the policy somewhere it cannot come back from.
     *
     * `entropyCoef` adds a bonus for keeping the distribution spread out. A
     * policy that collapses onto one action stops exploring and cannot discover
     * that another was better -- which is the failure mode this project has
     * already met twice, as 0.00 declarations per thousand country-turns.
     */
    /**
     * `validMask`, when given, restricts the policy to the actions that were
     * legal when the sample was taken. The behaviour policy is a softmax over
     * MASKED logits (see AISystem::pickAction), so an update that softmaxes the
     * raw logits is measuring a ratio against a distribution that was never on
     * the table -- and, worse, the entropy bonus then spends itself pushing
     * probability mass onto actions the mask will delete anyway, which is how
     * a policy can collapse over its VALID actions while its raw entropy still
     * looks healthy. Null keeps the old behaviour for heads with no mask.
     */
    /**
     * `mixScale`/`mixFloor` describe the BEHAVIOUR policy the sample was drawn
     * from: p_behaviour = mixScale * p_policy + mixFloor. Training acts through
     * an epsilon-mixture, so for an action the policy has abandoned the two
     * differ enormously -- and a ratio of p_policy over p_behaviour then scales
     * that sample's gradient down by exactly the factor by which exploration
     * over-sampled it. Measured 2026-08-24: "declare war" carried the HIGHEST
     * advantage of any war action (+0.17 against recruit's +0.02) over 735
     * samples a map, and the policy still played it twice in 3,597 offers,
     * because its ratio sat at ~0.025 and shrank every step it did take.
     *
     * Measuring the ratio on the mixture at BOTH ends puts it back at ~1 for an
     * unchanged policy, which is what PPO's trust region assumes. The gradient
     * direction is unchanged -- still the plain policy gradient on the softmax
     * -- only its magnitude stops being crushed for rare actions. Defaults
     * reproduce the old behaviour exactly for heads with no exploration.
     */
    /**
     * SUPERVISED cross-entropy toward one target action -- behavioural cloning.
     *
     * Loss is -log p(target) over the MASKED softmax, so the gradient arriving
     * on the logits is (p - onehot). No advantage, no ratio, no clip: this is
     * not a policy-gradient step and must not be confused for one. It exists so
     * a head can be taught by the scripted player before it is turned loose on
     * a reward -- AlphaGo's first stage, and the reason it worked.
     *
     * `validMask` matters as much here as in the PPO path: cloning a teacher
     * whose choice was made over legal actions, against a softmax that includes
     * illegal ones, spends most of the gradient pushing mass off actions the
     * mask deletes anyway.
     */
    void accumulateCrossEntropyInto(Scratch& s, int target, float weight,
                                    const std::vector<uint8_t>* validMask = nullptr) const;

    /**
     * What one PPO sample did, so a caller can watch the policy's health
     * rather than infer it afterwards from a benchmark.
     *
     * Everything here is already computed inside the update; reporting it
     * costs a few stores. The entropy is the quantity that says a head is
     * collapsing WHILE it collapses -- by the time a bench prints
     * "shape war:hold 100%" the run is over and the model is ruined.
     */
    struct PPOStats {
        float entropy = 0.0f;  ///< H of the masked policy here, in nats
        float kl      = 0.0f;  ///< KL(behaviour || policy), Schulman's k3
        int   support = 0;     ///< legal actions, so entropy has a ceiling
        bool  clipped = false;
    };

    void accumulatePPOInto(Scratch& s, int action, float advantage,
                           float oldLogProb, float clipEps,
                           float entropyCoef,
                           const std::vector<uint8_t>* validMask = nullptr,
                           float mixScale = 1.0f, float mixFloor = 0.0f,
                           PPOStats* out = nullptr) const;
    /** log pi(a|s) at temperature 1 for the logits currently in `s`. */
    static float logProbOf(const std::vector<float>& logits, int action);
    /**
     * Backpropagate a gradient the caller computed, on a single-output net.
     *
     * For a policy over a set whose SIZE VARIES -- which neighbour to attack,
     * where the candidates differ every turn -- there is no fixed output layer
     * to softmax over. Each candidate is scored by its own forward pass, the
     * softmax is taken across those scores outside the net, and each pass then
     * needs the one derivative belonging to it. That is this.
     */
    /**
     * Backprop an arbitrary gradient on ALL outputs, and expose the gradient
     * this net produces on its INPUT.
     *
     * Together these are what lets one net feed another: a head backprops its
     * loss, hands inputGrad() to the trunk, and the trunk backprops that. The
     * arithmetic was already there -- backpropInto fills grads[0] on its way
     * past, and applies the tanh derivative only for layers above zero, so
     * grads[0] is the raw input gradient -- it simply had no way out.
     *
     * Kept separate from accumulateOutputGradInto (which takes a single
     * scalar, for the one-output heads) so neither has to guess which it is.
     */
    void accumulateVectorGradInto(Scratch& s, const std::vector<float>& gradOnOutputs) const;
    // ── Attention pooling over a variable-size set ──
    //
    // A country's neighbours are a SET, not a fixed-width vector: there may be
    // one or nine, in no meaningful order. Averaging them throws away which one
    // matters; concatenating them needs an ordering the game does not have.
    // Attention solves exactly this -- score each member, softmax the scores,
    // and return the weighted sum -- and it is differentiable, so the encoder
    // learns what to look at.
    //
    // Free functions rather than a net, because there are no weights here: the
    // learning lives in whatever produced `emb` and `scores`. Kept separate and
    // testable for the same reason the trunk chaining was.
    static void attentionPool(const std::vector<std::vector<float>>& emb,
                              const std::vector<float>& scores,
                              std::vector<float>& pooled,
                              std::vector<float>& attnOut);
    /**
     * Backward for attentionPool.
     *
     * pooled = sum_i a_i * e_i with a = softmax(s), so a member's embedding is
     * pulled two ways: directly (weight a_i) and through its own influence on
     * the weights. Both terms are needed -- keeping only the first trains the
     * encoder while leaving the scorer blind, which looks like it works.
     */
    static void attentionPoolBackward(const std::vector<std::vector<float>>& emb,
                                      const std::vector<float>& attn,
                                      const std::vector<float>& gPooled,
                                      std::vector<std::vector<float>>& gEmb,
                                      std::vector<float>& gScores);

    /** Gradient on this net's input from the last backprop into `s`. */
    static const std::vector<float>& inputGrad(const Scratch& s) { return s.grads.front(); }

    void accumulateOutputGradInto(Scratch& s, float gradOnOutput) const;
    /** Folds a worker's gradients into the shared batch and empties it. */
    void mergeScratch(Scratch& s);

    // Squared-error update for a value head (output size 1): moves output
    // toward `target`. Call forward() first.
    void valueUpdate(float target, float lr);

    /**
     * Move this net's weights `alpha` of the way toward `other`'s.
     *
     * The merge step for parallel training: several processes each play their
     * own worlds against their own copy of the model, and periodically pull
     * toward the average of their peers. At alpha 0 nothing happens; at 1 this
     * net becomes the other. Adam's moment estimates are blended too — leaving
     * them behind would pair freshly-averaged weights with a momentum vector
     * describing a trajectory those weights were never on.
     *
     * Returns false if the architectures differ, in which case nothing is
     * touched: a peer file from a different build must not be half-absorbed.
     */
    bool blendToward(const NeuralNet& other, float alpha);

    // Serialization: binary blob with magic + architecture; load fails (returns
    // false) on any mismatch rather than half-loading.
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    void serialize(std::vector<uint8_t>& out) const;
    bool deserialize(const uint8_t* data, size_t size);

    const std::vector<int>& architecture() const { return m_sizes; }
    size_t paramCount() const {
        size_t n = 0;
        for (const Layer& L : m_layers) n += L.w.size() + L.b.size();
        return n;
    }
    int inputSize() const { return m_sizes.empty() ? 0 : m_sizes.front(); }
    int outputSize() const { return m_sizes.empty() ? 0 : m_sizes.back(); }
    bool valid() const { return m_sizes.size() >= 2; }

    // Introspection for the AI debug overlay.
    const std::vector<float>& lastOutput() const { return m_acts.empty() ? m_empty : m_acts.back(); }
    uint64_t updateCount() const { return m_updates; }

    // Activation snapshot/restore: lets a caller run forward() once, stash the
    // activations, and later apply policyGradientUpdate() WITHOUT re-running
    // the forward pass (the update only needs the cached activations).
    void snapshotActs(std::vector<std::vector<float>>& out) const { out = m_acts; }
    void restoreActs(std::vector<std::vector<float>>&& in) { m_acts = std::move(in); }

    // ── Helpers used by the AI system ──
    static void softmax(const std::vector<float>& logits, float temperature,
                        std::vector<float>& probsOut);
    // Samples an action index from softmaxed logits at `temperature`;
    // temperature <= ~0.05 collapses to argmax (the "insane" end).
    static int samplePolicy(const std::vector<float>& logits, float temperature,
                            std::mt19937& rng);

private:
    bool m_tanhOutput = false;
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

    // Batch accumulators, same shape as the weights/biases. Not serialized:
    // a batch never spans a save.
    struct LayerGrad { std::vector<float> w, b; };
    std::vector<LayerGrad> m_batch;
    int m_batchN = 0;

    void backprop(const std::vector<float>& outputGrad, float lr);
    /** Backward pass that writes into m_batch instead of stepping the weights. */
    void backpropAccumulate(const std::vector<float>& outputGrad);
};
