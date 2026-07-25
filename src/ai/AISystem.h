#pragma once
#include "NeuralNet.h"
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Game;

// Per-country neural-net AI with online reinforcement learning.
//
// Architecture: FOUR policy modules — economy, politics, war, navy — each a
// small MLP mapping a shared 48-float feature vector (everything the player
// can read off the UI: income breakdown, army/navy strength, relations,
// unrest, compass, research, frontier pressure...) to a menu of discrete
// actions. A fifth tiny net answers incoming diplomacy (accept/reject).
// Every module also has a value head used as the policy-gradient baseline.
//
// ONE shared model: all countries evaluate the same weights, so experience
// from every country trains the same brain (hundreds of samples per turn).
// Each country still thinks for itself — its own features, its own sampled
// action, its own reward.
//
// Difficulty is applied at ACTION SELECTION only, never to the model:
//   easy    high softmax temperature + 35% fully random actions
//   normal  T=1.0, 10% random
//   hard    T=0.35, 2% random
//   insane  argmax — the model's best known move, no noise
// A confident net keeps huge logit gaps, so temperature alone cannot make it
// play badly — the epsilon-random component is what makes easy genuinely dumb.
//
// Learning: REINFORCE with a learned per-module value baseline. Rewards come
// from turn-over-turn deltas (provinces, treasury, army, ships, rebellions)
// with per-module shaping. Invalid actions are masked to -inf BEFORE
// sampling, so the net never wastes probability mass on impossible moves and
// execution never has to reject a choice.
//
// Debuggability: deterministic seeded RNG, a ring buffer of every decision
// (country, module, chosen action, top alternatives, advantage on update),
// per-country summaries for the in-game overlay (config.aiDebug), and [AI]
// console logging.
class AISystem {
public:
    enum Module { MOD_ECONOMY = 0, MOD_POLITICS, MOD_WAR, MOD_NAVY, MOD_COUNT };
    static constexpr int FEATURE_COUNT = 96;
    // Economy: 0 save, 1 industry, 2 fort, 3 port, 4 specialize, 5 destroyer,
    //          6 carrier, 7 research fund up, 8 research fund down,
    //          9 research focus buildings, 10 focus army, 11 focus navy
    static constexpr int ECON_ACTIONS = 12;
    // Politics: 0 hold, 1 enact policy, 2 pac up, 3 pac down, 4 cancel policy,
    //           5 propose alliance, 6 propose NAP, 7 propose guarantee
    static constexpr int POL_ACTIONS  = 8;
    // War: 0 hold, 1 recruit, 2 reinforce, 3 attack, 4 declare war,
    //      5 artillery, 6 offer ceasefire
    static constexpr int WAR_ACTIONS  = 7;
    static constexpr int NAVY_ACTIONS = 5;
    static constexpr int DIPLO_ACTIONS = 2; // 0=reject 1=accept

    AISystem(Game* game, const std::string& modelPath);
    ~AISystem();

    // Once per turn, before any country thinks: single-pass world aggregates
    // (army totals, province counts, frontiers, ships) so per-country feature
    // extraction never rescans the map.
    void beginTurn();
    // Decide + enqueue this country's orders through the same pending-order
    // queues the player uses. Call from processCountryTurn, player excluded.
    void takeTurn(int cid);
    // After the turn resolves: compute rewards from state deltas, run the
    // policy-gradient updates, periodically persist the model.
    void endTurn();
    // Incoming diplomacy aimed at an AI country: accept or reject.
    bool decideDiplomacy(int targetCid, const std::string& action, const std::string& sourceIso);
    // "They said no" — back the pair off hard rather than re-asking as soon as
    // the ordinary cooldown lapses.
    void noteDiploRejected(int sourceCid, int targetCid);

    void saveModel();
    // Observation mode: load the model and act on it, but never write it back.
    // Lets a normal game run alongside a training session without the two
    // processes fighting over data/ai/model.bin.
    static bool s_readOnlyModel;
    // Reward-scale calibration from a replayed save's turn history: seeds the
    // running reward statistics so advantages are sensibly scaled from turn
    // one instead of after dozens of live turns. (Actions are not recorded in
    // saves, so history cannot be used for imitation — only for calibration.)
    void calibrateRewardScale(const std::vector<float>& perTurnProvinceDeltas);

    // ── Debug interface ──
    struct Decision {
        int turn = 0, cid = 0, module = 0, action = 0;
        float score = 0;      // chosen action's logit
        float advantage = 0;  // filled in at the learning step
        std::string label;    // human-readable action description
    };
    const std::deque<Decision>& decisions() const { return m_log; }
    std::vector<std::string> debugLines(int maxLines) const;
    std::string countrySummary(int cid) const;
    int decisionsThisTurn() const { return m_decisionsThisTurn; }

    // ── Training progress feed (cheap, for the trainer dashboard) ──
    struct TrainStats {
        long long warsDeclared = 0, ceasefiresOffered = 0,
                  pactsProposed = 0, researchCompleted = 0;
        // Amphibious pipeline. Troops embarked but never landed means the fleet
        // is stuck at sea — the failure mode that leaves island maps frozen.
        // landings counts assaults on hostile shores only; unloadsHome counts
        // cargo brought back to our own ports. Conflating them would flatter the
        // invasion rate with what are really aborted crossings.
        long long embarks = 0, landings = 0, unloadsHome = 0;
    };
    const TrainStats& trainStats() const { return m_trainStats; }
    // Mean raw reward per module per turn, last ~600 turns
    const std::deque<float>* rewardHistory() const { return m_rewardHistory; }

    // ── Model/hyperparameter introspection ──
    static constexpr float LR_POLICY = 0.002f;
    static constexpr float LR_VALUE  = 0.005f;
    long long paramCount() const;              // weights+biases across all nine nets
    unsigned long long totalUpdates() const;   // gradient updates across all nets
    size_t lastSaveBytes() const { return m_lastSaveBytes; }
    void samplingParams(float& temperature, float& epsilon) const {
        difficultyParams(temperature, epsilon);
    }
    const float* rewardMeans() const { return m_rMean; }

private:
    struct CountryStat {
        int provinces = 0;
        long long population = 0;
        long long army = 0;
        int boats = 0, destroyers = 0, carriers = 0;
        int boatsWithCrew = 0;
        float industrySum = 0, fortSum = 0;
        int maxPort = 0;
        // Claims: exposure (my provinces others claim -> rebellion risk) and
        // ambitions (provinces I claim that others hold -> war goals).
        int claimsAgainstMe = 0;
        int myClaimsOutstanding = 0;
        // Overseas conquest: coastal enemy countries we could reach BY SEA
        // (they own a port, we own a port, no shared land border, not already
        // friendly/at-war). War is otherwise only declarable across a land
        // frontier, which left water-separated foes permanently un-attackable.
        int navalTargets = 0;
        // Coastal countries we are ALREADY at war with and could land on. Kept
        // apart from navalTargets (which counts only not-yet-engaged foes)
        // because embarking is only worth doing when one of the two is nonzero.
        int navalWarTargets = 0;
        struct Frontier { int pid; int enemyCid; };
        std::vector<Frontier> frontiers;
    };
    // Reward horizon: each decision is judged by the state change over the
    // NEXT N_STEP turns, not the same turn. One-turn deltas taught passivity —
    // spending money was punished instantly while the payoff (industry income,
    // conquered provinces, suppressed rebellions) landed many turns later,
    // credited to nothing.
    static constexpr int N_STEP = 12;

    struct Experience {
        std::vector<float> features;
        int action[MOD_COUNT + 1] = {-1, -1, -1, -1, -1}; // +1 = diplo slot
        bool acted[MOD_COUNT + 1] = {false, false, false, false, false};
        int age = 0;        // turns since the decision
        int rebellions = 0; // rebellions suffered within the window
        // Activations cached at decision time so the learning step can skip
        // the policy re-forward (~1/3 of the per-country net cost).
        std::vector<std::vector<float>> acts[MOD_COUNT];
        // pre-turn snapshot for reward deltas
        int provinces = 0;
        double treasury = 0;
        long long army = 0;
        int ships = 0;
        float netIncome = 0;
        float industrySum = 0;
        int researched = 0; // completed research nodes
    };

    Game* m_g = nullptr;
    std::string m_modelPath;
    NeuralNet m_policy[MOD_COUNT];
    NeuralNet m_value[MOD_COUNT];
    NeuralNet m_diplo;
    std::mt19937 m_rng{1337}; // fixed seed: identical state -> identical picks
    int m_turn = 0;
    int m_decisionsThisTurn = 0;

    std::unordered_map<int, CountryStat> m_stats;      // rebuilt in beginTurn
    // cid -> sliding window of decisions awaiting their N_STEP reward
    std::unordered_map<int, std::deque<Experience>> m_pending;
    std::unordered_map<int, int> m_lastResearchCount;  // for the completions counter
    // Overture cooldown, keyed on the UNORDERED pair. An ordered key let A and
    // B alternate proposals to each other every single turn, so the pair never
    // actually cooled down.
    std::unordered_map<long long, int> m_diploCooldownUntil;
    // Per-country budget. The pair cooldown alone still let a country with a
    // dozen neighbours fire one overture every turn for a dozen turns straight
    // — that, not the per-pair rate, is what flooded the log.
    std::unordered_map<int, int> m_diploNextTurn;
    static long long diploKey(int a, int b) {
        int lo = a < b ? a : b, hi = a < b ? b : a;
        return ((long long)lo << 24) | (long long)hi;
    }
    bool diploBudgetReady(int cid) const {
        auto it = m_diploNextTurn.find(cid);
        return it == m_diploNextTurn.end() || m_turn >= it->second;
    }
    bool diploReady(int sourceCid, int targetCid) const {
        if (!diploBudgetReady(sourceCid)) return false;
        auto it = m_diploCooldownUntil.find(diploKey(sourceCid, targetCid));
        return it == m_diploCooldownUntil.end() || m_turn >= it->second;
    }
    void diploCoolDown(int sourceCid, int targetCid) {
        m_diploCooldownUntil[diploKey(sourceCid, targetCid)] = m_turn + 25;
        m_diploNextTurn[sourceCid] = m_turn + 5;
    }
    long long m_worldArmy = 0;
    size_t m_worldPixels = 0;

    // Running reward normalisation (mean/var per module), so advantage scale
    // is stable across maps of very different sizes.
    float m_rMean[MOD_COUNT] = {0, 0, 0, 0};
    float m_rVar[MOD_COUNT] = {1, 1, 1, 1};

    std::deque<Decision> m_log;
    TrainStats m_trainStats;
    std::deque<float> m_rewardHistory[MOD_COUNT];
    size_t m_lastSaveBytes = 0;

    // Research is player-only, so AI countries would report level-0 build caps
    // forever; these give them a baseline capability (research still raises it).
    int industryCap(int cid) const;
    int fortCap(int cid) const;
    int portCap(int cid) const;

    void buildFeatures(int cid, std::vector<float>& out);
    void difficultyParams(float& temperature, float& epsilon) const;
    int pickAction(NeuralNet& net, const std::vector<float>& feats,
                   const std::vector<bool>& valid, float& scoreOut);
    void logDecision(int cid, int module, int action, float score, const std::string& label);

    // Action execution (mirrors player enqueue rules incl. treasury deduction)
    std::string execEconomy(int cid, int action);
    std::string execPolitics(int cid, int action);
    std::string execWar(int cid, int action);
    std::string execNavy(int cid, int action);
    void validEconomy(int cid, std::vector<bool>& out);
    void validPolitics(int cid, std::vector<bool>& out);
    void validWar(int cid, std::vector<bool>& out);
    void validNavy(int cid, std::vector<bool>& out);

    bool loadModel();
};
