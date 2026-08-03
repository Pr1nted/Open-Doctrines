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
    checkTrunkChaining();
    checkAttention();
    checkInputWidening();
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
