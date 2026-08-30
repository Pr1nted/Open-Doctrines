// Gradient checks for the AI's differentiable pieces.
//
// WHY THESE EXIST. Every one of these paths is hand-written backpropagation,
// and every one of them fails SILENTLY when it is wrong: the net trains, the
// update counter climbs, the loss even moves, and the only symptom is a model
// that learns worse than it should for reasons nobody can see. This project has
// already shipped that bug twice -- the Q heads accumulated gradients into a
// scratch nobody merged and sat at zero updates for weeks looking like a
// feature waiting to warm up, and the stance head did the same thing on its
// first day from an edit that silently matched nothing.
//
// A finite-difference check cannot be fooled that way. It compares the analytic
// gradient against the loss actually moving when an input is nudged, so a
// missing term shows up as a number that does not match rather than as a model
// that is quietly mediocre.
//
// The three cases mirror the three ways gradients flow in AISystem:
//   1. into a net's INPUT          (NeuralNet::inputGrad)
//   2. through a trunk into a head (the shared encoder)
//   3. through attention pooling   (the relational observation)
#include "ai/NeuralNet.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <vector>

static int g_checks = 0, g_failed = 0;

static void ok(bool cond, const char* what, double worst) {
    g_checks++;
    if (cond) {
        printf("  ok    %s (worst %.2g)\n", what, worst);
    } else {
        printf("  FAIL  %s (worst %.2g)\n", what, worst);
        g_failed++;
    }
}

// Loss = sum(outputs), so the gradient arriving on every output is 1. Simple on
// purpose: the point is to exercise the backward path, not the loss.
static const float EPS = 1e-3f;
static const double TOL = 1e-2;

// ── 1. Gradient on a net's input ─────────────────────────
static void checkInputGrad() {
    NeuralNet net({6, 5, 4}, 7);
    std::vector<float> x = {0.3f, -0.2f, 0.5f, 0.1f, -0.7f, 0.4f};
    NeuralNet::Scratch s;
    net.initScratch(s);
    const std::vector<float>& y = net.forwardInto(s, x);
    net.accumulateVectorGradInto(s, std::vector<float>(y.size(), 1.0f));
    const std::vector<float> g = NeuralNet::inputGrad(s);   // copy: forward() clobbers

    double worst = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        std::vector<float> xp = x, xm = x;
        xp[i] += EPS; xm[i] -= EPS;
        double sp = 0, sm = 0;
        for (float v : net.forward(xp)) sp += v;
        for (float v : net.forward(xm)) sm += v;
        worst = std::max(worst, std::fabs((sp - sm) / (2.0 * EPS) - g[i]));
    }
    ok(worst < TOL, "input gradient matches finite differences", worst);
}

// ── 1b. Cross-entropy (behavioural cloning) ──────────────
// The claim is that accumulateCrossEntropyInto puts (p - onehot) on the logits,
// i.e. the exact derivative of -log p(target). Asserted against finite
// differences of that loss rather than trusted: a sign slip here trains the
// policy AWAY from the teacher and still looks like it is learning.
static void checkCrossEntropyGrad() {
    NeuralNet net({5, 6, 4}, 11);
    std::vector<float> x = {0.4f, -0.3f, 0.2f, 0.9f, -0.5f};
    const int target = 2;
    // Masked: action 3 is illegal, so it must receive no probability and the
    // loss must be computed over the remaining three.
    std::vector<uint8_t> mask = {1, 1, 1, 0};

    auto loss = [&](const std::vector<float>& xx) {
        std::vector<float> lg = net.forward(xx);
        for (size_t i = 0; i < lg.size(); ++i) if (!mask[i]) lg[i] = -1e9f;
        std::vector<float> p;
        NeuralNet::softmax(lg, 1.0f, p);
        return -std::log(std::max(1e-12f, p[(size_t)target]));
    };

    NeuralNet::Scratch s;
    net.initScratch(s);
    net.forwardInto(s, x);
    net.accumulateCrossEntropyInto(s, target, 1.0f, &mask);
    const std::vector<float> g = NeuralNet::inputGrad(s);

    double worst = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        std::vector<float> xp = x, xm = x;
        xp[i] += EPS; xm[i] -= EPS;
        worst = std::max(worst, std::fabs((loss(xp) - loss(xm)) / (2.0 * EPS) - g[i]));
    }
    ok(worst < TOL, "cross-entropy gradient matches finite differences", worst);

    // ...and it must point TOWARD the teacher: one descent step on the output
    // layer has to raise the target's probability, not lower it.
    auto probOfTarget = [&]() {
        std::vector<float> lg = net.forward(x);
        for (size_t i = 0; i < lg.size(); ++i) if (!mask[i]) lg[i] = -1e9f;
        std::vector<float> p; NeuralNet::softmax(lg, 1.0f, p);
        return p[(size_t)target];
    };
    const float before = probOfTarget();
    for (int step = 0; step < 40; ++step) {
        net.forward(x);
        net.crossEntropyUpdate(target, 0.05f, &mask);
    }
    const float after = probOfTarget();
    ok(after > before, "a cloning step raises the teacher's action", after - before);
}

// ── 1b. The collapse guard ───────────────────────────────
//
// A head CAN be driven onto one action, and the guard CAN pull it back off.
//
// The first half is not hypothetical. A training run collapsed the war head to
// `hold` 100.00% with every other action at exactly 0.00%, while PPO_ENTROPY
// was 0.01 throughout and its comment claimed that was "enough that a
// distribution cannot collapse to a point". It was not.
//
// The guard that replaced that claim rests on a fact about two gradients, and
// this is where the fact is checked rather than asserted:
//
//   entropy bonus:  coef * p_i * (log p_i + H).  As p -> 1 both log p and H go
//                   to zero, so its force VANISHES exactly at saturation. It
//                   can slow a collapse; it cannot reverse one.
//   uniform pull:   (p_i - 1/k).  At saturation that is (1 - 1/k) for the
//                   dominant action -- its LARGEST value. It does not vanish.
//
// So the test drives a head into the corner, confirms the entropy bonus cannot
// get it out, and then requires the pull to. Both halves matter: without the
// first, a passing second half would not show the guard was needed.
static void checkEntropyGuard() {
    // The guard's own constants (AISystem: ENTROPY_FLOOR_FRAC,
    // ENTROPY_GUARD_TARGET_MUL, ENTROPY_COEF_MAX, UNIFORM_PULL_K). Restated
    // rather than included: AISystem.h drags the whole game into a gradient
    // test. If they drift apart this test still tests the mechanism, which is
    // what it is for -- the values themselves are exercised by a real run.
    const float BASE_ENTROPY = 0.01f;
    const float MAX_ENTROPY  = 0.15f;
    const float FLOOR_FRAC   = 0.20f;
    const float TARGET_MUL   = 1.5f;
    const float PULL_K       = 1.0f;
    const int   K            = 4;      // legal actions

    const float ceiling = std::log((float)K);
    const float floorH  = FLOOR_FRAC * ceiling;
    const float target  = floorH * TARGET_MUL;
    const std::vector<float> x = {0.5f, -0.2f, 0.7f, 0.1f};
    const std::vector<uint8_t> mask(K, 1);

    NeuralNet net({4, 8, (int)K}, 7);
    NeuralNet::Scratch s;
    net.initScratch(s);

    auto entropyNow = [&]() {
        std::vector<float> p;
        NeuralNet::softmax(net.forward(x), 1.0f, p);
        float H = 0.0f;
        for (float q : p) if (q > 1e-8f) H -= q * std::log(q);
        return H;
    };

    // One batch under a relentless advantage on action 0 -- the shape of a
    // module with one profitable answer, which is what the war head met.
    // `guard` selects how hard the guard is allowed to push back.
    NeuralNet::PPOStats st;
    auto step = [&](float entropyCoef, float pullK) {
        std::vector<float> p;
        NeuralNet::softmax(net.forward(x), 1.0f, p);
        const float lp = std::log(std::max(1e-8f, p[0]));
        net.forwardInto(s, x);
        net.accumulatePPOInto(s, 0, 1.0f, lp, 0.2f, entropyCoef, &mask,
                              1.0f, 0.0f, &st);
        if (pullK > 0.0f) {
            float H = 0.0f;
            for (float q : p) if (q > 1e-8f) H -= q * std::log(q);
            const float deficit = std::clamp((target - H) / target, 0.0f, 1.0f);
            if (deficit > 0.0f)
                for (int a = 0; a < K; ++a)
                    net.accumulateCrossEntropyInto(s, a, pullK * deficit / (float)K, &mask);
        }
        // accumulate* writes into the SCRATCH; flushBatch drains the NET. The
        // merge between them is what connects the two, and leaving it out is
        // how the Q head once trained for zero updates -- see runLearningWork.
        net.mergeScratch(s);
        net.flushBatch(0.05f);
    };

    // ── it collapses, at the entropy coefficient that was supposed to stop it ──
    for (int i = 0; i < 4000; ++i) step(BASE_ENTROPY, 0.0f);
    const float collapsed = entropyNow();
    ok(collapsed < floorH,
       "the entropy bonus does not prevent a head collapsing", collapsed);
    ok(st.support == K, "PPO stats report the legal support", st.support);
    ok(std::fabs(st.entropy - collapsed) < 0.05,
       "PPO stats report the entropy the head actually has",
       std::fabs(st.entropy - collapsed));
    ok(st.kl >= 0.0f, "the KL estimator is never negative", st.kl);

    // ── and more entropy does not get it out again ──
    for (int i = 0; i < 4000; ++i) step(MAX_ENTROPY, 0.0f);
    const float entropyOnly = entropyNow();
    ok(entropyOnly < floorH,
       "...and the largest entropy bonus cannot climb back out either",
       entropyOnly);

    // ── the pull does, against the same advantage that put it there ──
    for (int i = 0; i < 4000; ++i) step(MAX_ENTROPY, PULL_K);
    const float recovered = entropyNow();
    ok(recovered > floorH,
       "the uniform pull restores a collapsed head above its floor",
       recovered - floorH);

    // ── and it is INERT on a healthy head: a deficit of zero adds nothing ──
    NeuralNet fresh({4, 8, (int)K}, 7);
    std::vector<float> before = fresh.forward(x);
    NeuralNet::Scratch fs;
    fresh.initScratch(fs);
    fresh.forwardInto(fs, x);
    {
        std::vector<float> p;
        NeuralNet::softmax(before, 1.0f, p);
        float H = 0.0f;
        for (float q : p) if (q > 1e-8f) H -= q * std::log(q);
        const float deficit = std::clamp((target - H) / target, 0.0f, 1.0f);
        ok(deficit == 0.0f,
           "a fresh head is above the guard's target, so the guard is inert",
           H - target);
    }
}

// ── 2. Trunk -> head chaining ────────────────────────────
// The trunk's output layer is a HIDDEN layer of the nets it replaced, so it is
// squashed (setTanhOutput). If that derivative is dropped on the way back the
// encoder trains as though its last layer were linear, which is wrong by a
// factor that depends on the activation -- invisible in any loss curve.
static void checkTrunkChaining() {
    NeuralNet trunk({6, 5}, 11);
    trunk.setTanhOutput(true);
    NeuralNet head({5, 4}, 12);
    std::vector<float> x = {0.3f, -0.2f, 0.5f, 0.1f, -0.7f, 0.4f};

    NeuralNet::Scratch ts, hs;
    trunk.initScratch(ts);
    head.initScratch(hs);

    auto forward = [&](const std::vector<float>& in) {
        const std::vector<float> h = trunk.forwardInto(ts, in);
        double sum = 0;
        for (float v : head.forwardInto(hs, h)) sum += v;
        return sum;
    };

    const std::vector<float> h = trunk.forwardInto(ts, x);
    const std::vector<float>& y = head.forwardInto(hs, h);
    head.accumulateVectorGradInto(hs, std::vector<float>(y.size(), 1.0f));
    trunk.accumulateVectorGradInto(ts, NeuralNet::inputGrad(hs));
    const std::vector<float> g = NeuralNet::inputGrad(ts);

    double worst = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        std::vector<float> xp = x, xm = x;
        xp[i] += EPS; xm[i] -= EPS;
        worst = std::max(worst, std::fabs((forward(xp) - forward(xm)) / (2.0 * EPS) - g[i]));
    }
    ok(worst < TOL, "trunk-to-head chaining matches finite differences", worst);
}

// ── 3. Attention pooling ─────────────────────────────────
// Two paths, and the SECOND is the one that disappears quietly: a member's
// embedding moves the pooled result directly (weight a_i) and through its own
// influence on the weights (the softmax Jacobian). Keep only the direct term
// and the encoder still trains while the scorer learns nothing at all.
static void checkAttention() {
    std::vector<std::vector<float>> emb = {
        {0.3f, -0.2f, 0.5f}, {-0.1f, 0.7f, 0.2f}, {0.4f, 0.1f, -0.6f}};
    std::vector<float> scores = {0.5f, -0.3f, 0.8f};

    std::vector<float> pooled, attn;
    NeuralNet::attentionPool(emb, scores, pooled, attn);
    std::vector<std::vector<float>> gEmb;
    std::vector<float> gScores;
    NeuralNet::attentionPoolBackward(emb, attn, std::vector<float>(pooled.size(), 1.0f),
                                     gEmb, gScores);

    auto loss = [](const std::vector<std::vector<float>>& e, const std::vector<float>& s) {
        std::vector<float> p, a;
        NeuralNet::attentionPool(e, s, p, a);
        double t = 0;
        for (float v : p) t += v;
        return t;
    };

    double worstE = 0.0;
    for (size_t i = 0; i < emb.size(); ++i)
        for (size_t k = 0; k < emb[i].size(); ++k) {
            auto ep = emb, em = emb;
            ep[i][k] += EPS; em[i][k] -= EPS;
            worstE = std::max(worstE,
                              std::fabs((loss(ep, scores) - loss(em, scores)) / (2.0 * EPS)
                                        - gEmb[i][k]));
        }
    ok(worstE < TOL, "attention: gradient on the embeddings", worstE);

    double worstS = 0.0;
    for (size_t i = 0; i < scores.size(); ++i) {
        auto sp = scores, sm = scores;
        sp[i] += EPS; sm[i] -= EPS;
        worstS = std::max(worstS,
                          std::fabs((loss(emb, sp) - loss(emb, sm)) / (2.0 * EPS) - gScores[i]));
    }
    ok(worstS < TOL, "attention: gradient on the scores (softmax Jacobian)", worstS);

    // Weights must be a distribution, or "pooled" is not a weighted mean of
    // anything and the scale of the observation drifts with the neighbour count.
    double sum = 0.0;
    for (float a : attn) sum += a;
    ok(std::fabs(sum - 1.0) < 1e-5, "attention weights sum to one", std::fabs(sum - 1.0));
}

// ── 4. Migration, which is not a gradient but fails as quietly ──
// A model gaining INPUTS must keep its learned weights and start the new
// columns at ZERO -- a new feature has to be ignored until something is learned
// about it, or the widened net computes something different from the narrow one
// the instant it loads, and every stored measurement becomes incomparable.
static void checkInputWidening() {
    NeuralNet narrow({4, 5, 3}, 21);
    std::vector<uint8_t> blob;
    narrow.serialize(blob);

    std::vector<float> x = {0.3f, -0.2f, 0.5f, 0.1f};
    std::vector<float> before = narrow.forward(x);

    NeuralNet wide({6, 5, 3}, 22);           // two extra inputs
    const bool loaded = wide.deserialize(blob.data(), blob.size());
    ok(loaded, "a narrower model loads into a wider net", 0.0);

    std::vector<float> xw = x;
    xw.push_back(0.9f);                       // new features, arbitrary values
    xw.push_back(-0.4f);
    const std::vector<float>& after = wide.forward(xw);

    double worst = 0.0;
    for (size_t i = 0; i < before.size() && i < after.size(); ++i)
        worst = std::max(worst, (double)std::fabs(before[i] - after[i]));
    ok(worst < 1e-6, "widening does not change what the model computes", worst);
}

int main() {
    printf("neural net gradients\n\n");
    checkInputGrad();
    checkCrossEntropyGrad();
    checkEntropyGuard();
    checkTrunkChaining();
    checkAttention();
    checkInputWidening();
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
