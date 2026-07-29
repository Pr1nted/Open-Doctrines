#pragma once
#include "NeuralNet.h"
#include <chrono>
#include <deque>
#include <random>
#include <thread>
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
    //      5 artillery, 6 offer ceasefire, 7 stage troops in allied territory
    static constexpr int WAR_ACTIONS  = 8;
    static constexpr int NAVY_ACTIONS = 5;
    static constexpr int DIPLO_ACTIONS = 2; // 0=reject 1=accept

    AISystem(Game* game, const std::string& modelPath);
    ~AISystem();

    // Once per turn, before any country thinks: single-pass world aggregates
    // (army totals, province counts, frontiers, ships) so per-country feature
    // extraction never rescans the map.
    void beginTurn();
private:
    /** The world aggregates, without the per-turn bookkeeping. */
    void refreshStats();
public:
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
    /** Coalition counters for the trainer dashboard. */
    void noteCallIssued()   { m_trainStats.callsIssued++; }
    void noteCallAnswered() { m_trainStats.callsAnswered++; }
    void noteCallRefused()  { m_trainStats.callsRefused++; }

    /**
     * The map is decided: `cid` won it.
     *
     * Flushes every open reward window with a terminal win/loss, which is the
     * only way the outcome the run is optimising for ever reaches the weights —
     * map rotation otherwise destroys the AISystem with those windows still
     * pending, so the last N_STEP turns of every map, including the decisive
     * ones, trained nothing at all.
     */
    void noteVictory(int cid);

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
        // Coalition behaviour. callsAnswered/callsRefused is the readout that
        // says whether alliances mean anything yet; stagingMoves says whether
        // anyone is using an ally's ground to reach a front.
        long long callsIssued = 0, callsAnswered = 0, callsRefused = 0;
        long long stagingMoves = 0;
    };
    const TrainStats& trainStats() const { return m_trainStats; }
    // Mean raw reward per module per turn, last ~600 turns
    const std::deque<float>* rewardHistory() const { return m_rewardHistory; }

    // ── Model/hyperparameter introspection ──
    static constexpr float LR_POLICY = 0.002f;
    static constexpr float LR_VALUE  = 0.005f;
    // Self-play exploration schedule. Training used to sit at a fixed 10%
    // random forever, which caps final play strength no matter how long the
    // run: see difficultyParams.
    static constexpr float EPSILON_START = 0.15f;
    static constexpr float EPSILON_FINAL = 0.02f;
    static constexpr double EPSILON_ANNEAL_UPDATES = 4.0e7;
    long long paramCount() const;              // weights+biases across all nine nets
    unsigned long long totalUpdates() const;   // gradient updates across all nets
    size_t lastSaveBytes() const { return m_lastSaveBytes; }
    /** Bytes this model would occupy on disk, without writing it. */
    size_t serializedSize() const;
    void samplingParams(float& temperature, float& epsilon) const {
        difficultyParams(temperature, epsilon);
    }
    const float* rewardMeans() const { return m_rMean; }

    // Observe-only view for the Neural capability. buildFeatures is private
    // because nothing outside should be constructing training input; this
    // hands out a copy of what the model would see, and cannot alter it.
    void modObserveFeatures(int cid, std::vector<float>& out) { buildFeatures(cid, out); }

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

        // ── Staging in allied territory ──
        // An ally's province that borders a country we are at war with, paired
        // with one of OUR provinces next to it. Moving troops along that pair
        // puts an army on a front we have no border of our own on — the whole
        // point of an alliance, and something the AI simply had no
        // representation for: `frontiers` only ever held our own provinces, so
        // no action could name a foreign one.
        struct Staging { int fromPid; int allyPid; int enemyCid; };
        std::vector<Staging> staging;
        // Troops we already have parked on allied soil, and where. Without the
        // pid list a staged army was a dead end: the attack action only ever
        // launched from provinces we owned, so troops walked into an ally and
        // stood there for the rest of the game.
        long long armyAbroad = 0;
        std::vector<int> abroadPids;

        // ── Defensive posture ──
        // A frontier facing a live enemy STACK, not merely a hostile country.
        // Everything the war module knew about being attacked was "am I at war
        // with somebody", which is not a fact you can build a frontline from.
        int threatenedProvinces = 0;    // own provinces with hostile troops adjacent
        long long enemyAdjArmy = 0;     // hostile troops standing on those borders
        long long defenderArmy = 0;     // our troops in those same provinces
        int worstThreatPid = -1;        // the biggest single deficit
        long long worstDeficit = 0;
        int provincesLost = 0;          // since last turn
        // ── Coalition ──
        long long allyAdjArmy = 0;      // allied troops adjacent to our territory
        int coBelligerents = 0;         // allies at war with one of our enemies
        int idleAllies = 0;             // allies at peace while we are at war
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
        // Was this country under attack when it decided? Ground lost while
        // nobody was pressing us is map churn; ground lost with enemy stacks on
        // the border is a defensive failure, and only the second should be
        // charged to the war module.
        int threatened = 0;
        // At war with anyone when the decision was taken. An army is worth
        // paying for when there is a war to fight and dead weight otherwise,
        // and the reward could not tell those apart without this.
        bool atWar = false;
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
    // Relation graph as integer sets, rebuilt in beginTurn. The ISO-keyed
    // m_relations is fine for the odd query but not for the tens of thousands
    // the neighbour walks make every turn.
    std::unordered_map<int, std::unordered_set<int>> m_warWith, m_alliedWith;
    // Province count at the END of last turn, so "did I just lose ground?" is
    // answerable from a single turn's stats.
    std::unordered_map<int, int> m_prevProvinces;
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

    // ─── Parallel learning step ──────────────────
    //
    // One deferred gradient computation. endTurn splits into a cheap serial
    // phase (rewards, running statistics — order-dependent) that fills this
    // list, and an expensive parallel phase that drains it. Measured at ~31%
    // of turn time on a 40-country map, and every item is independent of every
    // other until the gradients are summed.
    struct WorkItem {
        int module = 0;      // MOD_* , or MOD_COUNT for the diplomacy net
        int action = -1;
        float norm = 0;      // normalised reward
        float advantage = 0; // filled in by the worker, for the debug log
        int cid = 0;
        std::vector<float> features;
        std::vector<std::vector<float>> acts; // cached policy activations
    };
    std::vector<WorkItem> m_work;
    struct WorkerScratch {
        NeuralNet::Scratch policy[MOD_COUNT];
        NeuralNet::Scratch value[MOD_COUNT];
        NeuralNet::Scratch diplo;
        bool ready = false;
    };
    std::vector<WorkerScratch> m_scratch;
    /** Worker count: cores minus one, scaled by the resource limiter. */
    int learningThreads() const;
    /** Drains m_work across `learningThreads()` workers and merges gradients. */
    void runLearningWork();

    std::deque<Decision> m_log;
    TrainStats m_trainStats;
    std::deque<float> m_rewardHistory[MOD_COUNT];
    size_t m_lastSaveBytes = 0;
    // Checkpoint pacing. Losing at most a minute of self-play is a fine trade
    // for not rewriting a 12 MB file twice a second — see endTurn.
    static constexpr double SAVE_INTERVAL_SECONDS = 60.0;
    std::chrono::steady_clock::time_point m_lastSave = std::chrono::steady_clock::now();
    bool m_modelDirReady = false;
    // Set by the trainer when a map is decided, so the last window of the
    // winner's decisions is scored as a win rather than as an ordinary turn.
    int m_victorCid = -1;

    // Research is player-only, so AI countries would report level-0 build caps
    // forever; these give them a baseline capability (research still raises it).
    int industryCap(int cid) const;
    int fortCap(int cid) const;
    int portCap(int cid) const;

    void buildFeatures(int cid, std::vector<float>& out);
    void difficultyParams(float& temperature, float& epsilon) const;

    // `graveAction`, when >= 0, names an action that epsilon-random exploration
    // must not fire during normal play. See the note in pickAction.
    int  pickAction(NeuralNet& net, const std::vector<float>& feats,
                    const std::vector<bool>& valid, float& scoreOut,
                    int graveAction = -1);
    void logDecision(int cid, int module, int action, float score, const std::string& label);

    // Defence is issued as a batch, not one province a turn: a country invaded
    // on six borders needs six answers. Capped so a single country cannot fill
    // the move queue on its own.
    static constexpr int MAX_REINFORCE_ORDERS = 4;
    static constexpr int MAX_GARRISON_ORDERS  = 3;
    /** Move half the strongest adjacent friendly garrison into `dstPid`. */
    bool reinforceProvince(int cid, int dstPid);
    /** Unsampled defensive doctrine — see the note on the definition. */
    void garrisonReflex(int cid);

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
