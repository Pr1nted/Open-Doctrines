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
    //           5 propose alliance, 6 propose NAP, 7 propose guarantee,
    //           8 enact a policy aimed at calming the country,
    //           9 conciliate a minority, 10 repress a minority
    static constexpr int POL_ACTIONS  = 11;
    // War: 0 hold, 1 recruit, 2 reinforce, 3 attack, 4 declare war,
    //      5 artillery, 6 offer ceasefire, 7 stage troops in allied territory
    static constexpr int WAR_ACTIONS  = 8;
    // Navy: 0 hold, 1 move fleet, 2 bombard, 3 embark, 4 disembark/unload,
    //       5 scrap a ship the country is paying for and not using
    static constexpr int NAVY_ACTIONS = 6;
    static constexpr int DIPLO_ACTIONS = 2; // 0=reject 1=accept

    // ── How willing the AI is to start a war ────────────────────────────
    //
    // Tuning knobs for "peaceful, but still expansionist". Every one of them is
    // about wars the AI CHOOSES to start; none of them stop it defending
    // itself, honouring an alliance, or finishing a war already under way.
    //
    // The split that matters is claimed versus unclaimed land. Retaking
    // territory it claims is the AI's war goal and stays cheap, because that is
    // the conquest the game is about. Attacking a neighbour it has no argument
    // with is what got throttled: the bar used to be 1.05, meaning "very
    // slightly ahead", and the result was a map permanently at war.
    //
    // Multiplier on the enemy's army the AI must exceed before it will attack.
    static constexpr double AI_WAR_BAR_CLAIMED         = 0.85; // reconquest: unchanged
    static constexpr double AI_WAR_BAR_UNCLAIMED       = 2.50; // was 2.00, was 1.05
    static constexpr double AI_WAR_BAR_UNCLAIMED_NAVAL = 2.75; // was 2.20, was 1.30
    // Added to the bar when already fighting someone. One front at a time
    // unless the second is genuinely easy.
    static constexpr double AI_WAR_BAR_SECOND_FRONT    = 0.50;
    // ONE WAR AT A TIME. Measured: at 2 this gate almost never fired, because
    // an AI rarely chose a third war anyway -- the reduction came only from the
    // superiority bar, and total declarations fell just 11%. Finishing a war
    // before starting another is the rule that actually changes behaviour, and
    // it is the one a player can see and plan around. It restrains only wars
    // the AI CHOOSES: being attacked, honouring a guarantee and answering a
    // call to arms all still pile on regardless, so coalitions still happen.
    static constexpr int    AI_MAX_CONCURRENT_WARS     = 1;
    // Nor while the home front is this unhappy (Game::WAR_WEARINESS_MAX is 20).
    // 12 was most of the way to maximum unrest -- a country that far gone has
    // worse problems than picking its next war, so the check never mattered.
    // 6 is roughly one answered call to arms (CALL_TO_ARMS_UNREST is 7), which
    // is the point at which a country should be settling its existing
    // commitments rather than adding to them.
    static constexpr float  AI_WAR_WEARINESS_BLOCK     = 6.0f;

    // ── Answering a call to arms ────────────────────────────────────────
    //
    // Marching because an ally asked is the single most expensive thing an AI
    // can agree to: an immediate war it did not choose, CALL_TO_ARMS_UNREST
    // (7 of a maximum 20) added to the home front, and no war goal of its own
    // at the end of it. Refusing costs the alliance, which is a real price but
    // a one-off one.
    //
    // The diplomacy net decides, but it decides inside these bounds. The gates
    // below are conditions under which no answer but "no" makes sense, and they
    // are heuristics on purpose, for the same reason the war bars are: the model
    // ships trained, so restraint cannot wait for a retrain, and a gate the
    // policy cannot talk its way past is the only kind that holds.
    //
    // Wars we are already fighting, at or above which we have nothing to spare.
    //
    // This was 1, which reads reasonably and behaved terribly: on a live map
    // most allies worth calling are already fighting somebody, so a gate on
    // "any war at all" refused nearly every call and alliances stopped meaning
    // anything. Two fronts is the point at which a country genuinely has
    // nothing left to give, and it is also AI_MAX_CONCURRENT_WARS plus the one
    // war it did not choose — which is exactly what answering a call is.
    static constexpr int    AI_CALL_MAX_OWN_WARS       = 2;
    // Weariness at or above which the home front comes first.
    static constexpr float  AI_CALL_WEARINESS_BLOCK    = 5.0f;
    // How much stronger the aggressor may be than us before joining is simply
    // volunteering to be invaded. Measured against our army plus the calling
    // ally's, since that is the side we would be fighting on.
    static constexpr double AI_CALL_MAX_ENEMY_ODDS     = 1.50;
    // Standing reluctance, in logits, subtracted from "accept" before sampling.
    // One logit is roughly a 2.7x odds shift at temperature 1.
    //
    // WAS 1.20, AND THAT WAS TOO MUCH — but the bias was the smaller half of
    // the problem. The reward had made refusing almost free: accepting was
    // charged the full war weariness it takes on, while breaking an alliance
    // cost a fraction of one term. Pointed at an asymmetry that large, the
    // policy did the obvious thing and refused everything, and no amount of
    // tuning a sampling bias fixes a reward that says refusal is correct.
    //
    // So the reward now charges for the alliance a refusal destroys (see the
    // diplomacy reward in endTurn), and this drops to a nudge: enough that a
    // marginal call gets declined, not enough to decide the answer on its own.
    static constexpr float  AI_CALL_RELUCTANCE         = 0.35f;
    // The mirror of it. A non-aggression pact is the one agreement that costs
    // nothing and buys peace outright: it does not drag its holder into
    // anybody's war, and declareWar has to break it first, which gives both
    // sides a turn of warning. Nudging the answer toward yes is the cheapest
    // "more diplomatic" lever there is. Alliances and guarantees are left
    // neutral on purpose — those are the ones that come with a call to arms
    // attached, and the AI should want them, not be pushed into them.
    static constexpr float  AI_NAP_WILLINGNESS         = 0.80f;

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
    // `subjectIso` is the third party a request is ABOUT — the aggressor named
    // in a call to arms. Empty for requests that are only between the two.
    bool decideDiplomacy(int targetCid, const std::string& action,
                         const std::string& sourceIso,
                         const std::string& subjectIso = std::string());
    // "They said no" — back the pair off hard rather than re-asking as soon as
    // the ordinary cooldown lapses.
    void noteDiploRejected(int sourceCid, int targetCid);
    /** Coalition counters for the trainer dashboard. */
    // `cid` is the country the event belongs to: the caller for an issued
    // call, the ally deciding for an answer or a refusal. It selects which
    // cohort's counters move.
    void noteCallIssued(int cid)   { statsFor(cid).callsIssued++; }
    void noteCallAnswered(int cid) { statsFor(cid).callsAnswered++; }
    void noteCallRefused(int cid)  { statsFor(cid).callsRefused++; }

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

    /**
     * The map is over without being decided: the turn cap ran out, or nothing
     * strategic moved for long enough that the trainer rotated it.
     *
     * noteVictory covers the one map in a session that ends with a single
     * country standing. Every other map ends here, and until this existed it
     * ended with every open reward window dropped on the floor — the last
     * N_STEP turns trained nothing, and, worse, a run of frozen maps produced
     * no terminal signal at all, so "I ended the map holding a third of the
     * world" and "I ended it holding one province" were worth exactly the same.
     *
     * Each surviving country's final window is settled against its share of the
     * map, scaled to the same range the win/loss terminals use. It is a weaker
     * statement than winning, and deliberately so: finishing large is evidence,
     * not proof.
     */
    void noteMapEnd();

    // ── Parallel training across processes ──────────────────────────────
    //
    // The only way to raise learning speed is more independent experience per
    // second, and that means more worlds. Threads were not the route: raylib
    // allows one window per process, map loading touches the GL context, and a
    // world measured at 3 GB resident leaves room for three of them on a 16 GB
    // machine — so the limit is memory long before it is the ten cores.
    //
    // So worlds live in separate PROCESSES. Each worker owns a model file, plays
    // its own maps against its own copy, and periodically pulls part of the way
    // toward the average of its peers. Nobody blocks on anybody; a worker that
    // dies takes only its own progress with it; and the merge is a file read, so
    // there is no shared memory to get wrong.
    //
    // The cost is that the copies drift between syncs and averaging them is an
    // approximation rather than a true summed gradient. At a sync interval of a
    // couple of minutes against a learning rate this small, the drift is far
    // smaller than the noise the extra experience removes.
    /** Blend this model `alpha` of the way toward the mean of `peerPaths`. */
    int syncWithPeers(const std::vector<std::string>& peerPaths, float alpha);
    /** Average several model files into one. Backs `--merge-ai`. */
    static bool mergeModelFiles(const std::string& outPath,
                                const std::vector<std::string>& inPaths);

    void saveModel();
    // Observation mode: load the model and act on it, but never write it back.
    // Lets a normal game run alongside a training session without the two
    // processes fighting over data/ai/model.bin.
    static bool s_readOnlyModel;
    // Measurement mode (--eval-ai). The harness reuses the training loop's
    // headless shortcuts, so m_aiTraining is set — but a measurement must not
    // inherit the training exploration schedule, or what it reports is 12%
    // dice. With this set, sampling comes from the configured difficulty, the
    // same way a real game samples it.
    static bool s_evaluating;
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
        // Hulls paid off. Reads as "is the fleet a tool or a standing bill" —
        // a number that stays at zero on maps where navies matter is the
        // healthy case, and one that climbs on landlocked-ish maps means the
        // economy module has stopped buying ships it cannot use.
        long long shipsScrapped = 0;
        // Coalition behaviour. callsAnswered/callsRefused is the readout that
        // says whether alliances mean anything yet; stagingMoves says whether
        // anyone is using an ally's ground to reach a front.
        long long callsIssued = 0, callsAnswered = 0, callsRefused = 0;
        long long stagingMoves = 0;
        // Domestic government. calmingPolicies counts policies enacted to hold
        // the country together rather than to express its politics; the two
        // minority counters are the clearest read there is on what kind of
        // governments self-play is producing.
        long long calmingPolicies = 0;
        long long minorityConciliations = 0, minorityRepressions = 0;
        // Country-turns spent bankrupt, and austerity cuts made to avoid it.
        // The first should fall as the second rises; both staying high means
        // the cuts are not keeping up with the spending.
        long long bankruptTurns = 0, austerityCuts = 0;
    };
    const TrainStats& trainStats() const { return m_trainStats; }
    /**
     * The control group's counters, when --vs-random has split the map.
     *
     * Knowing the model holds less land than a coin flip says nothing about
     * WHY. These are the same counters kept separately for the random cohort,
     * so the two can be compared behaviour by behaviour: a model that never
     * declares war against a random side that declares constantly explains a
     * land deficit outright, and no amount of staring at the ratio would have
     * said so. Empty (all zero) when no cohort split is active, because then
     * every country's counters are in trainStats().
     */
    const TrainStats& randomStats() const { return m_randomStats; }
    // Mean raw reward per module per turn, last ~600 turns
    const std::deque<float>* rewardHistory() const { return m_rewardHistory; }

    // ── Model/hyperparameter introspection ──
    //
    // LEARNING RATES ARE SET FOR A BATCHED OPTIMISER, NOT A PER-SAMPLE ONE.
    //
    // These were tuned when every experience took its own Adam step. Batching
    // (NeuralNet::accumulate/flushBatch) now averages a whole turn — measured at
    // ~50 samples on a 50-country map — into ONE step, so at a fixed learning
    // rate the weights move roughly 50x less per unit of experience than they
    // used to. The gradient is also ~sqrt(50) ≈ 7x less noisy, which is the
    // whole point of batching and is what lets the rate go back up.
    //
    // 0.005 is a 2.5x rise, deliberately short of the ~7x that noise reduction
    // would justify: Adam plus the +/-10 gradient clamp in flushBatch makes a
    // too-high rate survivable but not free, and the model on disk has ~92M
    // samples of history behind it that is not worth risking for speed.
    static constexpr float LR_POLICY = 0.005f;  // was 0.002 (per-sample era)
    static constexpr float LR_VALUE  = 0.010f;  // was 0.005 (per-sample era)
    // The diplomacy head never left the per-sample era and must not be dragged
    // into the rise above. It learns only when somebody actually proposes
    // something — measured at ~0.66 experiences per turn against a policy
    // head's fifty — so its "batch" is usually a single sample and its gradient
    // carries none of the noise reduction that justifies a larger step.
    static constexpr float LR_DIPLO  = 0.002f;
    // Self-play exploration schedule. Training used to sit at a fixed 10%
    // random forever, which caps final play strength no matter how long the
    // run: see difficultyParams.
    //
    // MEASURED ON ONE POLICY HEAD, NOT ON totalUpdates().
    //
    // The schedule used to be compared against the sum over all nine nets,
    // which quietly divided the horizon by nine and mixed in the diplomacy
    // head — a net that sees roughly one sample per two turns against a policy
    // head's fifty, so its count says nothing about how mature any policy is.
    // With four policy heads at ~92M samples each the old 4e7 horizon had been
    // exhausted many times over: training had been running at the 2% floor for
    // most of its life, which is a fine place to END but a bad place to sit
    // through a change of reward function or action set.
    //
    // 4e8 samples on one head is on the order of a day and a half of continuous
    // self-play at the measured ~3.1k samples/s, which is the right scale for
    // "explore hard early, exploit late" to describe an actual training run
    // rather than its first hour.
    static constexpr float EPSILON_START = 0.15f;
    static constexpr float EPSILON_FINAL = 0.02f;
    static constexpr double EPSILON_ANNEAL_SAMPLES = 4.0e8;
    long long paramCount() const;              // weights+biases across all nine nets
    unsigned long long totalUpdates() const;   // gradient updates across all nets
    size_t lastSaveBytes() const { return m_lastSaveBytes; }
    /** Bytes this model would occupy on disk, without writing it. */
    size_t serializedSize() const;
    void samplingParams(float& temperature, float& epsilon) const {
        difficultyParams(temperature, epsilon);
    }
    const float* rewardMeans() const { return m_rMean; }

    // ── Random-move control group ───────────────────────────────────────
    //
    // Countries named here ignore the model and pick uniformly at random from
    // whatever the validity masks allow. EVERYTHING else is identical: the same
    // masks, the same executors, the same reflexes, the same restraint
    // constants. The only difference is where the choice comes from, which is
    // what makes the comparison mean something — it measures the contribution
    // of the trained policy and nothing else.
    //
    // This is the yardstick the reward curves cannot be: reward is measured
    // against a moving reward function, while "did it beat a coin flip" is an
    // absolute question with an absolute answer. A model that cannot beat
    // random selection is not learning, however healthy its sparklines look.
    void setRandomCountries(std::unordered_set<int> cids) { m_randomCids = std::move(cids); }
    bool isRandomCountry(int cid) const { return m_randomCids.count(cid) > 0; }

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
        // Standing agreements of any kind (alliance, non-aggression,
        // guarantee), counted once per partner. What the politics module has
        // actually built, as opposed to what it has proposed.
        int pacts = 0;

        // ── Minorities ──
        // Alignment feeds getProvinceRebellionChance directly, and rebellions
        // are the largest single term in the politics reward — so this is the
        // module's most direct lever on its own score, and until minority
        // policy became a country's own (see Game.h) the AI could not touch it.
        int minorities = 0;            // distinct groups living in our provinces
        float meanAlignment = 50.0f;   // how they feel about us, 0-100
        float worstAlignment = 100.0f;
        std::string worstMinority;     // the least reconciled of them
        // Alignment points per turn our current option set is worth, summed
        // over categories and averaged over groups. Negative is a government
        // actively pushing its minorities away.
        float minorityTrend = 0.0f;
        float minorityCost = 0.0f;     // what that option set costs per turn
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
        // Troops put on a hostile shore within the window. The province a
        // landing wins may fall well outside N_STEP turns, so without this the
        // decision that mounted the invasion is scored on the crossing alone —
        // an army removed from the map and a bill for the hulls.
        int landings = 0;
        // Turns spent with an empty treasury inside the window. Bankruptcy is
        // now expensive in the game (BANKRUPTCY_UNREST_PCT) and has to be
        // expensive in the reward too, or the modules keep spending and let the
        // austerity reflex clean up after them.
        int bankruptTurns = 0;
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
        // War weariness before the decision. The politics reward already
        // charged for the LEVEL, which barely moves when a country agrees to
        // one more war; the DELTA over the window is the actual bill for
        // whatever it agreed to, and it is the only term that makes answering a
        // call to arms cost anything the diplomacy net can see.
        float weariness = 0;
        // Mean minority alignment before the decision. Minority policy pays off
        // through rebellions NOT happening, which is a rare event and a very
        // sparse teacher; the alignment it moves is the dense one.
        float minorityAlignment = 50.0f;
        // Standing agreements before the decision. Refusing a call to arms ends
        // an alliance on the spot, and that has to show up somewhere or the
        // diplomacy head is being asked to weigh a real cost against nothing.
        int pacts = 0;
        // This turn's war action was an unprovoked declaration: war chosen
        // against a country holding no land we claim. Wars of reconquest are
        // the AI's war goal and are not charged for; picking a fight with a
        // neighbour it has no argument with is what makes a map burn.
        bool aggressor = false;
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
    TrainStats m_randomStats;
    /** Whichever cohort `cid` belongs to. All one pool when no split is set. */
    TrainStats& statsFor(int cid) {
        return isRandomCountry(cid) ? m_randomStats : m_trainStats;
    }
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
    // Set by noteMapEnd: cid -> terminal reward for a map that ran out rather
    // than being won. Empty on an ordinary turn.
    std::unordered_map<int, float> m_finalStanding;
    // Written by execWar when it issues an unprovoked declaration, read by
    // takeTurn on the way back. execWar returns a label, not a verdict, and
    // threading one through every case for the benefit of one of them is worse
    // than a flag that lives for the length of a single call.
    bool m_declaredUnprovoked = false;

    // Bounds on getMinorityAlignmentTrend, i.e. what the most conciliatory and
    // most repressive option sets are worth per turn. Fixed for a given set of
    // categories, so they are computed once and reused: the validity mask needs
    // them every turn to answer "is there anywhere left to move".
    mutable float m_trendMin = 0.0f, m_trendMax = 0.0f;
    mutable bool m_trendBoundsReady = false;
    void ensureTrendBounds() const;

    // The control group, and whether the country currently thinking belongs to
    // it. pickAction has no country argument — it takes a net and a mask — and
    // threading one through every call site to answer a question that is
    // constant for the whole of takeTurn would be worse than a flag that is set
    // once at the top of it.
    std::unordered_set<int> m_randomCids;
    bool m_randomThisCountry = false;

    // Research is player-only, so AI countries would report level-0 build caps
    // forever; these give them a baseline capability (research still raises it).
    int industryCap(int cid) const;
    int fortCap(int cid) const;
    int portCap(int cid) const;

    void buildFeatures(int cid, std::vector<float>& out);
    /**
     * A self-play run that is generating training data, as opposed to a
     * measurement run that merely reuses the same headless loop. Everything
     * that should behave like a real game during evaluation — sampling, the
     * grave-action guard — asks this rather than m_aiTraining directly.
     */
    bool selfPlayLearning() const;
    void difficultyParams(float& temperature, float& epsilon) const;

    // `graveAction`, when >= 0, names an action that epsilon-random exploration
    // must not fire during normal play. See the note in pickAction.
    // `logitBias`, when given, is added to the logits before masking and
    // sampling — a standing preference the caller wants applied to this
    // decision without teaching the net anything (the learning step uses the
    // net's own unbiased activations). See AI_CALL_RELUCTANCE.
    int  pickAction(NeuralNet& net, const std::vector<float>& feats,
                    const std::vector<bool>& valid, float& scoreOut,
                    int graveAction = -1,
                    const std::vector<float>* logitBias = nullptr);
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
    // Peacetime housekeeping the policy should not be gambling on: shuffling
    // troops around inside your own borders, and paying off an army you
    // cannot afford. Both run unconditionally, like garrisonReflex.
    void redeployReflex(int cid);
    void manpowerReflex(int cid);

    /**
     * Live within the country's means.
     *
     * Running out of money is not a strategic trade-off a policy should be
     * exploring — it is an accounting failure, and now an expensive one: an
     * empty treasury adds BANKRUPTCY_UNREST_PCT to every province's rebellion
     * chance for as long as it lasts. Nor is it something the modules can
     * reliably head off between them: the bill is spread across four of them
     * (research and ships from economy, doctrines and minority settlements from
     * politics, the army from war) and each sees only its own share of it.
     *
     * So solvency is a reflex, and it cuts in the same order the bankruptcy
     * cascade does — budgets, doctrines, minority spending, then hulls — but
     * one step per turn and while there is still money in the bank, which is
     * the difference between trimming and a fire sale. The sampled actions that
     * do these things by choice are untouched; this only fires when the country
     * is heading for the wall.
     */
    void austerityReflex(int cid);
    /**
     * Turns of net loss the treasury can absorb before austerity starts.
     *
     * Cutting on the first bad turn would have a country cancelling a doctrine
     * because it built something last turn. Cutting only at zero would be the
     * bankruptcy cascade, which is the thing this exists to avoid.
     */
    static constexpr double AI_AUSTERITY_RUNWAY_TURNS = 8.0;

    /**
     * Once troops are at sea, sail them to a hostile shore and put them on it.
     *
     * Mounting an invasion is a decision; finishing one is not. The policy
     * picks ONE navy action per turn, so a crossing needed the module to sample
     * "move" several turns running and then "disembark" at exactly the right
     * moment, against five other actions competing for the same slot. Measured
     * over a two-map evaluation: 1,372 embarkations produced 121 landings. The
     * other 91% were troops subtracted from the land army, carried around at
     * sea, and eventually brought home again.
     *
     * So the embark action stays a decision — it is where the trade-off lives,
     * and the model still chooses whether to strip a port garrison — and
     * everything after it becomes doctrine, the same way holding a threatened
     * border is doctrine (see garrisonReflex).
     */
    void amphibiousReflex(int cid);
    /** Landings this country made this turn, for the reward window. */
    std::unordered_map<int, int> m_landingsThisTurn;

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
