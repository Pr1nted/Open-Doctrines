#include "NeuralNet.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Optional vectorised BLAS for the forward pass. Enabled by CMake on macOS
// only (OD_USE_ACCELERATE); everywhere else the portable scalar loop below is
// compiled instead, so the build has no hard dependency on any BLAS.
#if defined(OD_USE_ACCELERATE) && !defined(OD_FORCE_SCALAR)
#include <Accelerate/Accelerate.h>
#endif

NeuralNet::NeuralNet(const std::vector<int>& layerSizes, uint32_t seed) {
    m_sizes = layerSizes;
    if (m_sizes.size() < 2) return;
    std::mt19937 rng(seed);
    m_layers.resize(m_sizes.size() - 1);
    for (size_t l = 0; l < m_layers.size(); ++l) {
        Layer& L = m_layers[l];
        L.in = m_sizes[l];
        L.out = m_sizes[l + 1];
        // Xavier/Glorot uniform
        float lim = std::sqrt(6.0f / (float)(L.in + L.out));
        std::uniform_real_distribution<float> d(-lim, lim);
        L.w.resize((size_t)L.out * L.in);
        for (auto& v : L.w) v = d(rng);
        L.b.assign(L.out, 0.0f);
        L.mw.assign(L.w.size(), 0.0f);
        L.vw.assign(L.w.size(), 0.0f);
        L.mb.assign(L.out, 0.0f);
        L.vb.assign(L.out, 0.0f);
    }
    m_acts.resize(m_sizes.size());
    m_grads.resize(m_sizes.size());
    for (size_t i = 0; i < m_sizes.size(); ++i) {
        m_acts[i].assign(m_sizes[i], 0.0f);
        m_grads[i].assign(m_sizes[i], 0.0f);
    }
}

const std::vector<float>& NeuralNet::forward(const std::vector<float>& in) {
    if (!valid() || (int)in.size() != m_sizes.front()) {
        static std::vector<float> bad;
        return bad;
    }
    m_acts[0] = in;
    for (size_t l = 0; l < m_layers.size(); ++l) {
        const Layer& L = m_layers[l];
        const std::vector<float>& x = m_acts[l];
        std::vector<float>& y = m_acts[l + 1];
        bool hidden = (l + 1 < m_layers.size()) || m_tanhOutput;
#if defined(OD_USE_ACCELERATE) && !defined(OD_FORCE_SCALAR)
        // y = W*x + b. W is (out x in) row-major, so this is a plain sgemv with
        // the bias preloaded into y and beta = 1. Mathematically identical to
        // the scalar loop below; BLAS may sum the dot products in a different
        // order, so results can differ in the last bit or two of float
        // precision — harmless for the policy, but it does mean a run is not
        // bit-reproducible across the two code paths.
        //
        // AND NOT ALWAYS REPRODUCIBLE AGAINST ITSELF. Measured 2026-08-03:
        // at 96 inputs three identical --eval-ai runs returned 1.08x every
        // time; at 104 and 112 the SAME binary and model returned 0.83x, 1.34x,
        // 0.87x. Accelerate evidently selects a different (and internally
        // non-deterministic) kernel at some widths. A last-bit difference is
        // harmless to one decision and fatal to a benchmark: it flips an
        // occasional sampled action and the two games diverge from there --
        // traced to turn 4, identical province ownership and treasury, army
        // counts already differing.
        //
        // -DOD_FORCE_SCALAR=1 compiles the portable loop instead, which is
        // bit-reproducible at every width. That is the build to measure with
        // when the feature count is anything other than a width Accelerate
        // happens to be stable on.
        std::copy(L.b.begin(), L.b.end(), y.begin());
        cblas_sgemv(CblasRowMajor, CblasNoTrans, L.out, L.in,
                    1.0f, L.w.data(), L.in, x.data(), 1, 1.0f, y.data(), 1);
        if (hidden)
            for (int o = 0; o < L.out; ++o) y[o] = std::tanh(y[o]);
#else
        for (int o = 0; o < L.out; ++o) {
            const float* wr = &L.w[(size_t)o * L.in];
            float s = L.b[o];
            for (int i = 0; i < L.in; ++i) s += wr[i] * x[i];
            y[o] = hidden ? std::tanh(s) : s; // hidden: tanh, output: linear
        }
#endif
    }
    return m_acts.back();
}

void NeuralNet::backprop(const std::vector<float>& outputGrad, float lr) {
    if (!valid()) return;
    m_grads.back() = outputGrad;
    m_adamT++;
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    // Bias-corrected step size
    float alpha = lr * std::sqrt(1.0f - std::pow(b2, (float)m_adamT)) /
                  (1.0f - std::pow(b1, (float)m_adamT));

    for (int l = (int)m_layers.size() - 1; l >= 0; --l) {
        Layer& L = m_layers[l];
        const std::vector<float>& x = m_acts[l];
        std::vector<float>& gy = m_grads[l + 1];
        std::vector<float>& gx = m_grads[l];
        std::fill(gx.begin(), gx.end(), 0.0f);

        for (int o = 0; o < L.out; ++o) {
            float g = gy[o];
            if (g == 0.0f) continue;
            // Per-element gradient clip. Saturated weights (|w|~1e6 after past
            // NaN poisoning) blow the backward chain up to inf within a layer
            // or two, and inf/inf in the Adam step mints fresh NaN weights.
            // Clipping here keeps every downstream product finite.
            if (!std::isfinite(g)) continue;
            g = std::clamp(g, -10.0f, 10.0f);
            float* wr = &L.w[(size_t)o * L.in];
            float* mwr = &L.mw[(size_t)o * L.in];
            float* vwr = &L.vw[(size_t)o * L.in];
            for (int i = 0; i < L.in; ++i) {
                float gw = g * x[i];
                gx[i] += wr[i] * g;
                mwr[i] = b1 * mwr[i] + (1 - b1) * gw;
                vwr[i] = b2 * vwr[i] + (1 - b2) * gw * gw;
                wr[i] -= alpha * mwr[i] / (std::sqrt(vwr[i]) + eps);
            }
            L.mb[o] = b1 * L.mb[o] + (1 - b1) * g;
            L.vb[o] = b2 * L.vb[o] + (1 - b2) * g * g;
            L.b[o] -= alpha * L.mb[o] / (std::sqrt(L.vb[o]) + eps);
        }
        // Through the tanh of the layer below (unless we've reached the input)
        if (l > 0) {
            const std::vector<float>& a = m_acts[l];
            for (int i = 0; i < (int)gx.size(); ++i)
                gx[i] *= (1.0f - a[i] * a[i]); // d/dx tanh = 1 - tanh^2
        }
    }
    m_updates++;
}

void NeuralNet::policyGradientUpdate(int action, float advantage, float lr) {
    if (!valid() || action < 0 || action >= outputSize()) return;
    // grad of -advantage * log softmax(logits)[action] wrt logits is
    // advantage * (softmax - onehot). We DESCEND on that.
    const std::vector<float>& logits = m_acts.back();
    std::vector<float> probs;
    softmax(logits, 1.0f, probs);
    std::vector<float> g(probs.size());
    for (size_t i = 0; i < probs.size(); ++i)
        g[i] = advantage * (probs[i] - (i == (size_t)action ? 1.0f : 0.0f));
    backprop(g, lr);
}

void NeuralNet::crossEntropyUpdate(int target, float lr,
                                   const std::vector<uint8_t>* validMask) {
    if (!valid() || target < 0 || target >= outputSize()) return;
    const std::vector<float>& logits = m_acts.back();
    std::vector<float> probs;
    if (validMask && !validMask->empty()) {
        std::vector<float> ml(logits);
        for (size_t i = 0; i < ml.size(); ++i)
            if (i >= validMask->size() || !(*validMask)[i]) ml[i] = -1e9f;
        softmax(ml, 1.0f, probs);
    } else {
        softmax(logits, 1.0f, probs);
    }
    std::vector<float> g(probs.size());
    for (size_t i = 0; i < probs.size(); ++i)
        g[i] = probs[i] - (i == (size_t)target ? 1.0f : 0.0f);
    backprop(g, lr);
}

// ─── Batched gradient accumulation ───────────────────────

void NeuralNet::backpropAccumulate(const std::vector<float>& outputGrad) {
    if (!valid()) return;
    if (m_batch.size() != m_layers.size()) {
        m_batch.resize(m_layers.size());
        for (size_t l = 0; l < m_layers.size(); ++l) {
            m_batch[l].w.assign(m_layers[l].w.size(), 0.0f);
            m_batch[l].b.assign(m_layers[l].b.size(), 0.0f);
        }
    }
    m_grads.back() = outputGrad;
    for (int l = (int)m_layers.size() - 1; l >= 0; --l) {
        Layer& L = m_layers[l];
        LayerGrad& G = m_batch[l];
        const std::vector<float>& x = m_acts[l];
        std::vector<float>& gy = m_grads[l + 1];
        std::vector<float>& gx = m_grads[l];
        std::fill(gx.begin(), gx.end(), 0.0f);
        for (int o = 0; o < L.out; ++o) {
            float g = gy[o];
            if (g == 0.0f || !std::isfinite(g)) continue;
            g = std::clamp(g, -10.0f, 10.0f);
            const float* wr = &L.w[(size_t)o * L.in];
            float* gwr = &G.w[(size_t)o * L.in];
            for (int i = 0; i < L.in; ++i) {
                gwr[i] += g * x[i];
                gx[i] += wr[i] * g;
            }
            G.b[o] += g;
        }
        if (l > 0) {
            const std::vector<float>& a = m_acts[l];
            for (int i = 0; i < (int)gx.size(); ++i)
                gx[i] *= (1.0f - a[i] * a[i]);
        }
    }
    m_batchN++;
}

void NeuralNet::accumulatePolicyGradient(int action, float advantage) {
    if (!valid() || action < 0 || action >= outputSize()) return;
    const std::vector<float>& logits = m_acts.back();
    std::vector<float> probs;
    softmax(logits, 1.0f, probs);
    std::vector<float> g(probs.size());
    for (size_t i = 0; i < probs.size(); ++i)
        g[i] = advantage * (probs[i] - (i == (size_t)action ? 1.0f : 0.0f));
    backpropAccumulate(g);
}

void NeuralNet::accumulateValueGradient(float target) {
    if (!valid() || outputSize() < 1) return;
    std::vector<float> g(outputSize(), 0.0f);
    g[0] = (m_acts.back()[0] - target);
    backpropAccumulate(g);
}

// ─── Thread-private accumulation ─────────────────────────

void NeuralNet::initScratch(Scratch& s) const {
    s.acts.resize(m_sizes.size());
    s.grads.resize(m_sizes.size());
    for (size_t i = 0; i < m_sizes.size(); ++i) {
        s.acts[i].assign(m_sizes[i], 0.0f);
        s.grads[i].assign(m_sizes[i], 0.0f);
    }
    s.gw.resize(m_layers.size());
    s.gb.resize(m_layers.size());
    for (size_t l = 0; l < m_layers.size(); ++l) {
        s.gw[l].assign(m_layers[l].w.size(), 0.0f);
        s.gb[l].assign(m_layers[l].b.size(), 0.0f);
    }
    s.n = 0;
}

const std::vector<float>& NeuralNet::forwardInto(Scratch& s, const std::vector<float>& in) const {
    if (!valid() || (int)in.size() != m_sizes.front() || s.acts.size() != m_sizes.size()) {
        static const std::vector<float> bad;
        return bad;
    }
    s.acts[0] = in;
    for (size_t l = 0; l < m_layers.size(); ++l) {
        const Layer& L = m_layers[l];
        const std::vector<float>& x = s.acts[l];
        std::vector<float>& y = s.acts[l + 1];
        const bool hidden = (l + 1 < m_layers.size()) || m_tanhOutput;
#if defined(OD_USE_ACCELERATE) && !defined(OD_FORCE_SCALAR)
        std::copy(L.b.begin(), L.b.end(), y.begin());
        cblas_sgemv(CblasRowMajor, CblasNoTrans, L.out, L.in,
                    1.0f, L.w.data(), L.in, x.data(), 1, 1.0f, y.data(), 1);
        if (hidden)
            for (int o = 0; o < L.out; ++o) y[o] = std::tanh(y[o]);
#else
        for (int o = 0; o < L.out; ++o) {
            const float* wr = &L.w[(size_t)o * L.in];
            float acc = L.b[o];
            for (int i = 0; i < L.in; ++i) acc += wr[i] * x[i];
            y[o] = hidden ? std::tanh(acc) : acc;
        }
#endif
    }
    return s.acts.back();
}

// Backward pass into a Scratch. Mirrors backpropAccumulate exactly; the only
// difference is where the state lives.
static void backpropInto(const std::vector<int>& sizes, NeuralNet::Scratch& s,
                         const std::vector<float>& outputGrad,
                         const std::vector<const float*>& weights,
                         const std::vector<std::pair<int,int>>& dims,
                         bool tanhOutput = false) {
    s.grads.back() = outputGrad;
    // An activated output layer is squashed on the way forward, so the
    // gradient arriving on it must be squashed on the way back. Without this
    // the trunk would be trained as if its last layer were linear.
    if (tanhOutput) {
        const std::vector<float>& a = s.acts.back();
        for (size_t i = 0; i < s.grads.back().size() && i < a.size(); ++i)
            s.grads.back()[i] *= (1.0f - a[i] * a[i]);
    }
    for (int l = (int)dims.size() - 1; l >= 0; --l) {
        const int in = dims[l].first, out = dims[l].second;
        const float* w = weights[l];
        const std::vector<float>& x = s.acts[l];
        std::vector<float>& gy = s.grads[l + 1];
        std::vector<float>& gx = s.grads[l];
        std::fill(gx.begin(), gx.end(), 0.0f);
        float* gwr0 = s.gw[l].data();
        float* gbr = s.gb[l].data();
        for (int o = 0; o < out; ++o) {
            float g = gy[o];
            if (g == 0.0f || !std::isfinite(g)) continue;
            g = std::clamp(g, -10.0f, 10.0f);
            const float* wr = w + (size_t)o * in;
            float* gwr = gwr0 + (size_t)o * in;
            for (int i = 0; i < in; ++i) {
                gwr[i] += g * x[i];
                gx[i] += wr[i] * g;
            }
            gbr[o] += g;
        }
        if (l > 0) {
            const std::vector<float>& a = s.acts[l];
            for (int i = 0; i < (int)gx.size(); ++i)
                gx[i] *= (1.0f - a[i] * a[i]);
        }
    }
    (void)sizes;
    s.n++;
}

void NeuralNet::accumulatePolicyInto(Scratch& s, int action, float advantage) const {
    if (!valid() || action < 0 || action >= outputSize() || s.gw.size() != m_layers.size()) return;
    const std::vector<float>& logits = s.acts.back();
    std::vector<float> probs;
    softmax(logits, 1.0f, probs);
    std::vector<float> g(probs.size());
    for (size_t i = 0; i < probs.size(); ++i)
        g[i] = advantage * (probs[i] - (i == (size_t)action ? 1.0f : 0.0f));
    std::vector<const float*> w; std::vector<std::pair<int,int>> d;
    for (const Layer& L : m_layers) { w.push_back(L.w.data()); d.push_back({L.in, L.out}); }
    backpropInto(m_sizes, s, g, w, d, m_tanhOutput);
}

float NeuralNet::logProbOf(const std::vector<float>& logits, int action) {
    if (logits.empty() || action < 0 || action >= (int)logits.size()) return 0.0f;
    std::vector<float> probs;
    softmax(logits, 1.0f, probs);
    return std::log(std::max(1e-8f, probs[(size_t)action]));
}

void NeuralNet::accumulateCrossEntropyInto(Scratch& s, int target, float weight,
                                           const std::vector<uint8_t>* validMask) const {
    if (!valid() || target < 0 || target >= outputSize() ||
        s.gw.size() != m_layers.size()) return;
    const std::vector<float>& logits = s.acts.back();
    std::vector<float> probs;
    if (validMask && !validMask->empty()) {
        std::vector<float> ml(logits);
        for (size_t i = 0; i < ml.size(); ++i)
            if (i >= validMask->size() || !(*validMask)[i]) ml[i] = -1e9f;
        softmax(ml, 1.0f, probs);
    } else {
        softmax(logits, 1.0f, probs);
    }
    // d(-log p_target)/dz_i = p_i - [i == target]
    std::vector<float> g(probs.size(), 0.0f);
    for (size_t i = 0; i < probs.size(); ++i)
        g[i] = weight * (probs[i] - (i == (size_t)target ? 1.0f : 0.0f));

    std::vector<const float*> w; std::vector<std::pair<int,int>> d;
    for (const Layer& L : m_layers) { w.push_back(L.w.data()); d.push_back({L.in, L.out}); }
    backpropInto(m_sizes, s, g, w, d, m_tanhOutput);
}

void NeuralNet::accumulatePPOInto(Scratch& s, int action, float advantage,
                                  float oldLogProb, float clipEps,
                                  float entropyCoef,
                                  const std::vector<uint8_t>* validMask,
                                  float mixScale, float mixFloor,
                                  PPOStats* out) const {
    if (!valid() || action < 0 || action >= outputSize() || s.gw.size() != m_layers.size()) return;
    const std::vector<float>& logits = s.acts.back();
    std::vector<float> probs;
    if (validMask && !validMask->empty()) {
        // The same support the decision was made over. See the header note.
        std::vector<float> ml(logits);
        for (size_t i = 0; i < ml.size(); ++i)
            if (i >= validMask->size() || !(*validMask)[i]) ml[i] = -1e9f;
        softmax(ml, 1.0f, probs);
    } else {
        softmax(logits, 1.0f, probs);
    }

    const float p = std::max(1e-8f, probs[(size_t)action]);
    // The behaviour probability THIS policy would now produce, under the same
    // mixture the sample was drawn from. See the header note.
    const float pBeh = std::max(1e-8f, mixScale * p + mixFloor);
    // Clamped before exp: a stale sample can put this ratio far enough out that
    // exp() overflows to inf, and one inf in a gradient poisons every weight it
    // touches -- permanently, because the model is then saved to disk.
    const float ratio = std::exp(std::clamp(std::log(pBeh) - oldLogProb, -20.0f, 20.0f));

    // Beyond the clip, in the direction the advantage is pushing, the objective
    // is FLAT: no gradient at all. That flatness is the mechanism, not a
    // rounding of it -- it is what makes a too-large step worth nothing and so
    // stops the update taking it.
    // DUAL CLIP. Standard PPO bounds how far an update may CHASE an advantage
    // but not how hard it may punish one: with a negative advantage and a ratio
    // above 1 the surrogate is unclipped, so a single stale sample whose ratio
    // has drifted to e^20 can carry a gradient five orders of magnitude larger
    // than its neighbours -- enough to stomp a rare action's probability to the
    // floor in one batch. Rare actions are exactly where ratios drift most,
    // which is how "declare war" and "ceasefire" kept arriving at 0% or 100%
    // depending on which sign got lucky first. Beyond DUAL_CLIP the objective
    // goes flat, the same mechanism the ordinary clip already uses.
    static constexpr float DUAL_CLIP = 4.0f;
    const bool clipped = (advantage > 0.0f && ratio > 1.0f + clipEps) ||
                         (advantage < 0.0f && ratio < 1.0f - clipEps) ||
                         (advantage < 0.0f && ratio > DUAL_CLIP);

    std::vector<float> g(probs.size(), 0.0f);
    if (!clipped) {
        // d(-A*ratio)/dz = A*ratio*(p - onehot), the plain policy gradient with
        // the importance weight folded in.
        for (size_t i = 0; i < probs.size(); ++i)
            g[i] = advantage * ratio * (probs[i] - (i == (size_t)action ? 1.0f : 0.0f));
    }

    // Computed unconditionally, not just when the bonus is on: this is the
    // number the caller's collapse guard reads, and a guard that only sees the
    // policy while the bonus is already running cannot tell it to start.
    float H = 0.0f;
    for (float q : probs) if (q > 1e-8f) H -= q * std::log(q);

    if (entropyCoef > 0.0f) {
        // Loss carries -entropyCoef * H, so the gradient carries
        // +entropyCoef * p_i * (log p_i + H). Added even when the surrogate is
        // clipped: exploration should not switch off just because this
        // particular sample's step was too big.
        for (size_t i = 0; i < probs.size(); ++i) {
            const float q = std::max(1e-8f, probs[i]);
            g[i] += entropyCoef * q * (std::log(q) + H);
        }
    }

    if (out) {
        out->entropy = H;
        out->clipped = clipped;
        // Schulman's k3: (r - 1) - log r. Unbiased, and unlike -log r it is
        // never negative, so a mean over a batch cannot cancel itself to zero
        // while individual samples are far out.
        const float lr_ = std::clamp(std::log(pBeh) - oldLogProb, -20.0f, 20.0f);
        out->kl = (ratio - 1.0f) - lr_;
        int sup = 0;
        if (validMask && !validMask->empty()) {
            for (size_t i = 0; i < probs.size(); ++i)
                if (i < validMask->size() && (*validMask)[i]) ++sup;
        } else {
            sup = (int)probs.size();
        }
        out->support = sup;
    }

    std::vector<const float*> w; std::vector<std::pair<int,int>> d;
    for (const Layer& L : m_layers) { w.push_back(L.w.data()); d.push_back({L.in, L.out}); }
    backpropInto(m_sizes, s, g, w, d, m_tanhOutput);
}

void NeuralNet::accumulateValueInto(Scratch& s, float target) const {
    if (!valid() || outputSize() < 1 || s.gw.size() != m_layers.size()) return;
    std::vector<float> g(outputSize(), 0.0f);
    g[0] = (s.acts.back()[0] - target);
    std::vector<const float*> w; std::vector<std::pair<int,int>> d;
    for (const Layer& L : m_layers) { w.push_back(L.w.data()); d.push_back({L.in, L.out}); }
    backpropInto(m_sizes, s, g, w, d, m_tanhOutput);
}


void NeuralNet::attentionPool(const std::vector<std::vector<float>>& emb,
                              const std::vector<float>& scores,
                              std::vector<float>& pooled,
                              std::vector<float>& attnOut) {
    pooled.clear(); attnOut.clear();
    if (emb.empty() || emb.size() != scores.size()) return;
    const size_t d = emb[0].size();
    // Softmax, shifted by the max for numerical safety: a country with a
    // dominant neighbour produces large scores, and exp() of those overflows.
    float mx = scores[0];
    for (float v : scores) mx = std::max(mx, v);
    attnOut.assign(scores.size(), 0.0f);
    float sum = 0.0f;
    for (size_t i = 0; i < scores.size(); ++i) {
        attnOut[i] = std::exp(scores[i] - mx);
        sum += attnOut[i];
    }
    if (sum <= 0.0f || !std::isfinite(sum)) {
        attnOut.assign(scores.size(), 1.0f / (float)scores.size());
    } else {
        for (float& a : attnOut) a /= sum;
    }
    pooled.assign(d, 0.0f);
    for (size_t i = 0; i < emb.size(); ++i)
        for (size_t k = 0; k < d && k < emb[i].size(); ++k)
            pooled[k] += attnOut[i] * emb[i][k];
}

void NeuralNet::attentionPoolBackward(const std::vector<std::vector<float>>& emb,
                                      const std::vector<float>& attn,
                                      const std::vector<float>& gPooled,
                                      std::vector<std::vector<float>>& gEmb,
                                      std::vector<float>& gScores) {
    gEmb.clear(); gScores.clear();
    if (emb.empty() || emb.size() != attn.size()) return;
    const size_t d = gPooled.size();
    gEmb.assign(emb.size(), std::vector<float>(d, 0.0f));
    gScores.assign(emb.size(), 0.0f);
    // Direct path: d pooled / d e_i = a_i.
    for (size_t i = 0; i < emb.size(); ++i)
        for (size_t k = 0; k < d; ++k)
            gEmb[i][k] = attn[i] * gPooled[k];
    // Through the weights: dL/da_i = <gPooled, e_i>, then softmax's Jacobian
    // a_i (dL/da_i - sum_j a_j dL/da_j).
    std::vector<float> gA(emb.size(), 0.0f);
    for (size_t i = 0; i < emb.size(); ++i) {
        float dot = 0.0f;
        for (size_t k = 0; k < d && k < emb[i].size(); ++k) dot += gPooled[k] * emb[i][k];
        gA[i] = dot;
    }
    float weighted = 0.0f;
    for (size_t i = 0; i < emb.size(); ++i) weighted += attn[i] * gA[i];
    for (size_t i = 0; i < emb.size(); ++i)
        gScores[i] = attn[i] * (gA[i] - weighted);
}

void NeuralNet::accumulateVectorGradInto(Scratch& s,
                                         const std::vector<float>& gradOnOutputs) const {
    if (!valid() || s.gw.size() != m_layers.size()) return;
    if ((int)gradOnOutputs.size() != outputSize()) return;
    for (float g : gradOnOutputs) if (!std::isfinite(g)) return;
    std::vector<const float*> w; std::vector<std::pair<int,int>> d;
    for (const Layer& L : m_layers) { w.push_back(L.w.data()); d.push_back({L.in, L.out}); }
    backpropInto(m_sizes, s, gradOnOutputs, w, d, m_tanhOutput);
}

void NeuralNet::accumulateOutputGradInto(Scratch& s, float gradOnOutput) const {
    if (!valid() || outputSize() < 1 || s.gw.size() != m_layers.size()) return;
    if (!std::isfinite(gradOnOutput)) return;
    std::vector<float> g(outputSize(), 0.0f);
    g[0] = gradOnOutput;
    std::vector<const float*> w; std::vector<std::pair<int,int>> d;
    for (const Layer& L : m_layers) { w.push_back(L.w.data()); d.push_back({L.in, L.out}); }
    backpropInto(m_sizes, s, g, w, d, m_tanhOutput);
}

void NeuralNet::accumulateActionValueInto(Scratch& s, int action, float target) const {
    if (!valid() || s.gw.size() != m_layers.size()) return;
    if (action < 0 || action >= outputSize()) return;
    std::vector<float> g(outputSize(), 0.0f);
    g[action] = (s.acts.back()[action] - target);
    std::vector<const float*> w; std::vector<std::pair<int,int>> d;
    for (const Layer& L : m_layers) { w.push_back(L.w.data()); d.push_back({L.in, L.out}); }
    backpropInto(m_sizes, s, g, w, d, m_tanhOutput);
}

void NeuralNet::mergeScratch(Scratch& s) {
    if (s.n <= 0 || s.gw.size() != m_layers.size()) { s.n = 0; return; }
    if (m_batch.size() != m_layers.size()) {
        m_batch.resize(m_layers.size());
        for (size_t l = 0; l < m_layers.size(); ++l) {
            m_batch[l].w.assign(m_layers[l].w.size(), 0.0f);
            m_batch[l].b.assign(m_layers[l].b.size(), 0.0f);
        }
    }
    for (size_t l = 0; l < m_layers.size(); ++l) {
        for (size_t i = 0; i < m_batch[l].w.size(); ++i) { m_batch[l].w[i] += s.gw[l][i]; s.gw[l][i] = 0.0f; }
        for (size_t i = 0; i < m_batch[l].b.size(); ++i) { m_batch[l].b[i] += s.gb[l][i]; s.gb[l][i] = 0.0f; }
    }
    m_batchN += s.n;
    s.n = 0;
}

void NeuralNet::flushBatch(float lr) {
    if (!valid() || m_batchN <= 0 || m_batch.size() != m_layers.size()) {
        m_batchN = 0;
        return;
    }
    const float inv = 1.0f / (float)m_batchN;
    m_adamT++;
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    const float alpha = lr * std::sqrt(1.0f - std::pow(b2, (float)m_adamT)) /
                        (1.0f - std::pow(b1, (float)m_adamT));
    for (size_t l = 0; l < m_layers.size(); ++l) {
        Layer& L = m_layers[l];
        LayerGrad& G = m_batch[l];
        for (size_t i = 0; i < L.w.size(); ++i) {
            float g = G.w[i] * inv;
            G.w[i] = 0.0f;
            if (!std::isfinite(g)) continue;
            g = std::clamp(g, -10.0f, 10.0f);
            L.mw[i] = b1 * L.mw[i] + (1 - b1) * g;
            L.vw[i] = b2 * L.vw[i] + (1 - b2) * g * g;
            L.w[i] -= alpha * L.mw[i] / (std::sqrt(L.vw[i]) + eps);
        }
        for (size_t o = 0; o < L.b.size(); ++o) {
            float g = G.b[o] * inv;
            G.b[o] = 0.0f;
            if (!std::isfinite(g)) continue;
            g = std::clamp(g, -10.0f, 10.0f);
            L.mb[o] = b1 * L.mb[o] + (1 - b1) * g;
            L.vb[o] = b2 * L.vb[o] + (1 - b2) * g * g;
            L.b[o] -= alpha * L.mb[o] / (std::sqrt(L.vb[o]) + eps);
        }
    }
    // One optimiser step per batch, but the update counter still reflects how
    // many experiences went into the model — the epsilon schedule and the
    // dashboard both read it as "how much has this thing seen".
    m_updates += (uint64_t)m_batchN;
    m_batchN = 0;
}

void NeuralNet::valueUpdate(float target, float lr) {
    if (!valid() || outputSize() < 1) return;
    std::vector<float> g(outputSize(), 0.0f);
    g[0] = (m_acts.back()[0] - target); // d/dy of 0.5*(y-target)^2
    backprop(g, lr);
}

// ─── Softmax / sampling ──────────────────────────────────

void NeuralNet::softmax(const std::vector<float>& logits, float temperature,
                        std::vector<float>& probsOut) {
    probsOut.resize(logits.size());
    if (logits.empty()) return;
    float t = std::max(0.05f, temperature);
    float mx = *std::max_element(logits.begin(), logits.end());
    double sum = 0.0;
    for (size_t i = 0; i < logits.size(); ++i) {
        probsOut[i] = std::exp((logits[i] - mx) / t);
        sum += probsOut[i];
    }
    if (sum <= 0) { // degenerate — uniform
        for (auto& p : probsOut) p = 1.0f / probsOut.size();
        return;
    }
    for (auto& p : probsOut) p = (float)(p / sum);
}

int NeuralNet::samplePolicy(const std::vector<float>& logits, float temperature,
                            std::mt19937& rng) {
    if (logits.empty()) return -1;
    if (temperature <= 0.05f) { // insane end: pure argmax
        return (int)(std::max_element(logits.begin(), logits.end()) - logits.begin());
    }
    std::vector<float> probs;
    softmax(logits, temperature, probs);
    std::uniform_real_distribution<float> d(0.0f, 1.0f);
    float r = d(rng), acc = 0.0f;
    for (size_t i = 0; i < probs.size(); ++i) {
        acc += probs[i];
        if (r <= acc) return (int)i;
    }
    return (int)probs.size() - 1;
}

// ─── Serialization ───────────────────────────────────────

static const uint32_t NN_MAGIC = 0x4F44414Eu; // "NADO"
// v2: Adam moments + step counter ride along with the weights, so resumed
// training keeps its optimizer momentum instead of cold-starting Adam.
static const uint32_t NN_VERSION = 2;

bool NeuralNet::blendToward(const NeuralNet& other, float alpha) {
    if (m_sizes != other.m_sizes || m_layers.size() != other.m_layers.size())
        return false;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    if (alpha <= 0.0f) return true;
    const float keep = 1.0f - alpha;
    auto mix = [&](std::vector<float>& a, const std::vector<float>& b) {
        if (a.size() != b.size()) return;
        for (size_t i = 0; i < a.size(); ++i) {
            const float v = keep * a[i] + alpha * b[i];
            // A peer that went non-finite must not take this one with it. The
            // model file is scrubbed on load for the same reason.
            a[i] = std::isfinite(v) ? v : a[i];
        }
    };
    for (size_t l = 0; l < m_layers.size(); ++l) {
        Layer& A = m_layers[l];
        const Layer& B = other.m_layers[l];
        mix(A.w, B.w);   mix(A.b, B.b);
        mix(A.mw, B.mw); mix(A.vw, B.vw);
        mix(A.mb, B.mb); mix(A.vb, B.vb);
    }
    // Experience is additive across workers: two processes that have each seen
    // a million samples have, between them, produced a model informed by two
    // million. The Adam step counter is NOT — it indexes the bias correction,
    // and inflating it would flatten the correction for everyone.
    m_updates = std::max(m_updates, other.m_updates);
    m_adamT = std::max(m_adamT, other.m_adamT);
    return true;
}

void NeuralNet::serialize(std::vector<uint8_t>& out) const {
    auto put32 = [&](uint32_t v) {
        out.push_back(v & 0xFF); out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF); out.push_back((v >> 24) & 0xFF);
    };
    auto putf = [&](float f) { uint32_t v; memcpy(&v, &f, 4); put32(v); };
    put32(NN_MAGIC);
    put32(NN_VERSION);
    put32((uint32_t)m_sizes.size());
    for (int s : m_sizes) put32((uint32_t)s);
    put32((uint32_t)(m_updates & 0xFFFFFFFFu));
    put32((uint32_t)m_adamT);
    for (const Layer& L : m_layers) {
        for (float f : L.w) putf(f);
        for (float f : L.b) putf(f);
        for (float f : L.mw) putf(f);
        for (float f : L.vw) putf(f);
        for (float f : L.mb) putf(f);
        for (float f : L.vb) putf(f);
    }
}

bool NeuralNet::deserialize(const uint8_t* data, size_t size) {
    size_t p = 0;
    auto get32 = [&](uint32_t& v) -> bool {
        if (p + 4 > size) return false;
        v = data[p] | (data[p+1] << 8) | (data[p+2] << 16) | ((uint32_t)data[p+3] << 24);
        p += 4; return true;
    };
    auto getf = [&](float& f) -> bool {
        uint32_t v; if (!get32(v)) return false; memcpy(&f, &v, 4); return true;
    };
    uint32_t magic, ver, n;
    if (!get32(magic) || magic != NN_MAGIC) return false;
    if (!get32(ver) || ver != NN_VERSION) return false;
    if (!get32(n) || n < 2 || n > 16) return false;
    std::vector<int> sizes(n);
    for (auto& s : sizes) { uint32_t v; if (!get32(v) || v == 0 || v > 4096) return false; s = (int)v; }
    // Architecture must match what the code expects — refuse otherwise so a
    // stale model file can't silently misbehave.
    //
    // ONE exception: a policy head that has GAINED actions. Adding an action to
    // a module (a new order the AI can give) changes only the output width, and
    // refusing the file over that would throw away every hour of training in it
    // — including the economy, politics and navy heads, which did not change at
    // all. The old outputs keep their learned weights and the new ones start
    // from their Xavier initialisation, which is exactly the right prior: an
    // action nothing has ever been learned about should start neutral.
    //
    // A SECOND exception, by the same argument from the other end: a net that
    // has gained INPUTS. Adding a feature -- a fact the AI can now see --
    // widens only the first layer, and refusing the file over it would discard
    // every net in the model over a change none of them had a chance to be
    // wrong about. The difference from the output case is what the new weights
    // start at: a new ACTION should start neutral, so Xavier is right, but a
    // new FEATURE must start IGNORED, or the model's behaviour changes the
    // instant it loads. The new columns are zeroed below, which makes the
    // widened net compute exactly what the narrow one did and leaves it to
    // learn what the feature is worth.
    bool grewOutputs = false, grewInputs = false;
    if (!m_sizes.empty() && sizes != m_sizes) {
        const bool sameShape = sizes.size() == m_sizes.size();
        const bool sameExceptLast =
            sameShape && std::equal(sizes.begin(), sizes.end() - 1, m_sizes.begin());
        const bool sameExceptFirst =
            sameShape && std::equal(sizes.begin() + 1, sizes.end(), m_sizes.begin() + 1);
        if (sameExceptLast && sizes.back() < m_sizes.back())        grewOutputs = true;
        else if (sameExceptFirst && sizes.front() < m_sizes.front()) grewInputs = true;
        else return false;
    }
    uint32_t updates, adamT;
    if (!get32(updates)) return false;
    if (!get32(adamT)) return false;

    // Build at the TARGET shape when migrating, so the extra output rows keep
    // their fresh initialisation, and read the file's (narrower) rows into it.
    const std::vector<int> target = (grewOutputs || grewInputs) ? m_sizes : sizes;
    const bool keepTanhOut = m_tanhOutput;
    *this = NeuralNet(target, 1234);
    m_tanhOutput = keepTanhOut;
    m_updates = updates;
    m_adamT = (int)adamT;
    for (size_t l = 0; l < m_layers.size(); ++l) {
        Layer& L = m_layers[l];
        const int fileOut = sizes[l + 1];
        const int fileIn = sizes[l];
        // Only the last layer can differ, and only in its output count.
        auto readInto = [&](std::vector<float>& dst, int rows, int cols) -> bool {
            for (int o = 0; o < rows; ++o)
                for (int i = 0; i < cols; ++i)
                    if (!getf(dst[(size_t)o * L.in + i])) return false;
            return true;
        };
        auto readVec = [&](std::vector<float>& dst, int n) -> bool {
            for (int o = 0; o < n; ++o) if (!getf(dst[o])) return false;
            return true;
        };
        if (!readInto(L.w,  fileOut, fileIn)) return false;
        if (!readVec (L.b,  fileOut))         return false;
        if (!readInto(L.mw, fileOut, fileIn)) return false;
        if (!readInto(L.vw, fileOut, fileIn)) return false;
        if (!readVec (L.mb, fileOut))         return false;
        if (!readVec (L.vb, fileOut))         return false;
    }
    if (grewOutputs)
        printf("[AI] Policy head widened %d -> %d actions; existing weights kept\n",
               sizes.back(), m_sizes.back());
    if (grewInputs) {
        // ZERO, not Xavier. Every weight reading a feature the file never saw
        // must contribute nothing, so the widened net reproduces the narrow
        // one's output bit for bit on its first forward pass. The Adam moments
        // go with them: a stale moment on a brand-new weight would take a large
        // first step in a direction nothing has evidence for.
        Layer& L0 = m_layers[0];
        const int oldIn = sizes.front();
        for (int o = 0; o < L0.out; ++o)
            for (int i = oldIn; i < L0.in; ++i) {
                const size_t k = (size_t)o * L0.in + i;
                L0.w[k] = 0.0f; L0.mw[k] = 0.0f; L0.vw[k] = 0.0f;
            }
        printf("[AI] Input widened %d -> %d features; new inputs start ignored\n",
               oldIn, L0.in);
    }
    // Self-heal models poisoned by earlier NaN/inf reward bugs: a single NaN
    // weight makes every forward pass NaN (0*NaN==NaN), and a NaN Adam moment
    // re-poisons the weight on its next update. Zero non-finite values, clamp
    // the survivors — weights of magnitude 1e6+ (past-poison fallout) overflow
    // the backward pass into inf and regenerate the NaNs — and keep second
    // moments non-negative (Adam divides by sqrt(v)).
    for (Layer& L : m_layers) {
        auto scrub = [](std::vector<float>& v, float lim) {
            for (float& f : v) {
                if (!std::isfinite(f)) f = 0.0f;
                else f = std::clamp(f, -lim, lim);
            }
        };
        scrub(L.w, 50.0f);   scrub(L.b, 50.0f);
        scrub(L.mw, 1e3f);   scrub(L.mb, 1e3f);
        auto scrubVar = [](std::vector<float>& v) {
            for (float& f : v) {
                if (!std::isfinite(f) || f < 0.0f) f = 0.0f;
                else if (f > 1e6f) f = 1e6f;
            }
        };
        scrubVar(L.vw); scrubVar(L.vb);
    }
    return true;
}

bool NeuralNet::save(const std::string& path) const {
    std::vector<uint8_t> buf;
    serialize(buf);
    // Write to a temporary beside the target, then rename into place. rename()
    // is atomic, so a reader always sees either the whole previous model or the
    // whole new one. Writing in place left an 11MB window on every save during
    // which the file was truncated or half-written: quitting, crashing, or
    // starting a second instance in that window destroyed the model. With tens
    // of millions of updates invested in it, that is not an acceptable risk.
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    size_t w = fwrite(buf.data(), 1, buf.size(), f);
    if (fclose(f) != 0 || w != buf.size()) { std::remove(tmp.c_str()); return false; }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
    return true;
}

bool NeuralNet::load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    std::vector<uint8_t> buf((size_t)n);
    size_t r = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (r != buf.size()) return false;
    return deserialize(buf.data(), buf.size());
}
