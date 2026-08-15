#pragma once
#include "NeuralNet.h"

struct NavyShip;   // GameStructs.h; only referenced by pointer/reference here
#include <cstdio>
#include <cstdlib>
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
    /**
     * 96 -> 104: the last eight are TRENDS.
     *
     * Everything before them describes the country RIGHT NOW. A grand-strategy
     * position is not a snapshot: "my army is 40,000" means something very
     * different when it was 20,000 eight turns ago than when it was 80,000, and
     * the policy could not tell those apart. Nor could it see a neighbour
     * massing on its border until the stack arrived.
     *
     * EVERY SOURCE HERE IS ALREADY-COMPUTED STATE -- CountryStat, which
     * refreshStats() fills at the top of the turn, plus direct Country fields
     * and pure map lookups. The first attempt at this sampled
     * computeCountryIncome() from beginTurn, which reads an income cache that
     * processTurn has not refreshed yet, and the resulting garbage made the
     * whole evaluation NON-DETERMINISTIC: three identical runs returned 1.00,
     * 0.83 and 1.14. Nothing that has a validity window belongs in here.
     *
     * Safe on a trained model: NeuralNet::deserialize widens the input layer
     * and ZEROES the new columns, so a 96-feature file loads into this and
     * computes exactly what it did before until it learns otherwise.
     */
    /**
     * 143, WAS 140. The three new slots are the political-distance features at
     * the end; see CountryStat::compassGapMean.
     *
     * Widening is safe for an existing model: NeuralNet::deserialize grows a
     * net's FIRST layer and zeroes the new weights, so a v7 file loads and
     * computes exactly what it computed before until training moves them. The
     * new inputs are appended rather than dropped into the spare slots at 80-84
     * and 88-94 -- those belong to decideDiplomacy, which overwrites them on
     * every request, so anything else living there would be visible on ordinary
     * turns and invisible on the turns diplomacy is decided.
     */
    static constexpr int FEATURE_COUNT = 143;  // +24 for the relational slice, +3 compass
    /** Political distance past which a province counts as a long way off. */
    static constexpr float COMPASS_FAR = 0.35f;
    /** How far back a trend looks: long enough to show a build-up, short
     *  enough to still be about the current situation. */
    static constexpr int TREND_WINDOW = 8;
    /**
     * 104 -> 112: WORLD STATE. The cheap half of a centralised critic.
     *
     * Every country runs the same shared brain on its own local view, so from
     * any one country's seat the other few hundred are non-stationary noise --
     * and the value head, whose entire job is "how good is this position", was
     * estimating that without knowing whether the map around it was a stable
     * patchwork or already carved up by three empires. That is variance the
     * baseline cannot explain away, and it lands in every advantage the policy
     * trains on.
     *
     * Textbook CTDE hands the critic something the policy cannot see. This game
     * has no hidden information -- province ownership is on the map for
     * everyone -- so the gain here is purely conditioning on global context,
     * and these go in the shared observation rather than a separate critic
     * input. Cheaper, honest about what it is, and it costs no reset.
     */
    static constexpr int WORLD_FEATURES = 8;
    // Economy: 0 save, 1 industry, 2 fort, 3 port, 4 specialize, 5 destroyer,
    //          6 carrier, 7 research fund up, 8 research fund down,
    //          9 research focus buildings, 10 focus army, 11 focus navy
    static constexpr int ECON_ACTIONS = 12;
    // Politics: 0 hold, 1 enact policy, 2 pac up, 3 pac down, 4 cancel policy,
    //           5 propose alliance, 6 propose NAP, 7 propose guarantee,
    //           8 enact a policy aimed at calming the country,
    //           9 conciliate a minority, 10 repress a minority,
    //           11 propose a trade
    //
    // 11 was added after the trade system shipped, and widening this head is
    // what lets the AI ever OFFER one. Until it existed, feature 112 ("this is
    // a trade") was fed to the diplomacy net but could only ever be 1 when a
    // human proposed -- so in self-play it was always 0, the weight on it never
    // saw a gradient, and no amount of training could teach the AI anything
    // about trade. Growing a policy head is a supported migration: see
    // NeuralNet::deserialize, which keeps the ten learned outputs and starts
    // this one from its Xavier initialisation.
    static constexpr int POL_ACTIONS  = 12;
    // War: 0 hold, 1 recruit, 2 reinforce, 3 attack, 4 declare war,
    //      5 artillery, 6 offer ceasefire, 7 stage troops in allied territory
    static constexpr int WAR_ACTIONS  = 8;
    // Navy: 0 hold, 1 move fleet, 2 bombard, 3 embark, 4 disembark/unload,
    //       5 scrap a ship the country is paying for and not using
    // 0 hold, 1 move, 2 bombard, 3 embark, 4 land, 5 scrap, 6 engage.
    //
    // ENGAGE WAS MISSING ENTIRELY until 2026-08-06. Naval combat existed and
    // was resolved by processNavyCombat, but only the player and the network
    // could ever queue an engage order -- the AI had no such action, so an AI
    // fleet sailed past an enemy fleet without ever attacking it, could only be
    // attacked and never attack, and would not contest a sea lane or escort its
    // own transports. Widening the head is safe: NeuralNet::deserialize widens
    // a policy head and keeps existing weights, with the new action starting
    // contributing nothing.
    static constexpr int NAVY_ACTIONS = 7;
    static constexpr int DIPLO_ACTIONS = 2; // 0=reject 1=accept

    /**
     * Width of the shared trunk's output -- the embedding every head reads.
     *
     * The policy and Q nets were {FEATURE_COUNT, 512, 320, N} apiece: eight
     * separate encoders learning the same job from the same features, each
     * from its own module's gradient alone. They are now ONE encoder
     * {FEATURE_COUNT, 512, 320} feeding eight {320, N} heads, which is the same
     * function decomposed -- 512->320 was already a hidden layer, and the trunk
     * squashes its output (setTanhOutput) so it still is.
     *
     * What changes is who pays for it: the encoder now learns from every
     * module's gradient at once, which is the whole point. What does NOT share
     * it: the value heads (their own narrow FEATURE_COUNT->160->1 pathway --
     * a critic wants to be free to disagree with the actor's representation),
     * and the war-target head, which reads a different input space entirely
     * (own state PLUS one candidate) and could not consume this embedding
     * without redesigning how candidates are scored.
     */
    static constexpr int TRUNK_OUT = 320;

    /**
     * THE STANCE HEAD: temporal abstraction over the four modules.
     *
     * Every module decides afresh every turn, with nothing holding them to a
     * plan. That produced two measured pathologies: wars declared and then not
     * prosecuted, and modules working against each other -- the economy
     * defunding research while the war module recruited armies the treasury
     * could not carry. Neither is a bad decision in isolation; both are the
     * absence of a decision ABOVE them.
     *
     * A slow head picks a posture and holds it for STANCE_WINDOW turns. It is
     * fed back into the observation as a one-hot, so every module conditions on
     * it and the four of them are at least arguing about the same question.
     * The stance itself is trained on the shared reward, because it is the one
     * decision that genuinely owns the whole country's outcome.
     */
    static constexpr int REL_FEATURES = 8;
    static constexpr int REL_EMBED    = 24;
    static constexpr int REL_MAX      = 6;

    static constexpr int STANCE_COUNT  = 4;   // expand / consolidate / defend / develop
    static constexpr int STANCE_WINDOW = 10;

    /**
     * HOW HARD THE STANCE PUSHES, in logits.
     *
     * The paragraph above describes what the stance was meant to be. What it
     * actually was, for its whole life, is a memory bit: `stanceOf` was read in
     * exactly ONE place -- to set the one-hot in features 112-115 -- and gated
     * nothing, biased nothing and changed no executor. In principle the trunk
     * could learn any stance-conditional behaviour from that one-hot; in
     * practice one input among a hundred and forty, with nothing forcing the
     * association, is not a plan. It is a note the country leaves itself.
     *
     * So the posture now leans on the choice, through the logit-bias channel
     * pickAction already has for the critic. A BIAS and not a mask, deliberately
     * and in both directions: at the hard-difficulty temperature of 0.35 this
     * moves the odds of an action by about a factor of two and a half, which is
     * enough to make a country at war behave like one that has decided
     * something, and nowhere near enough to stop it defending itself because it
     * declared a building phase ten turns ago. Masking would do the latter.
     *
     * This is a hand-authored prior, which is the same kind of thing the
     * learned attack head exists to get rid of elsewhere -- worth saying
     * plainly. The difference that makes it acceptable here is that the policy
     * can overrule it: a logit bias shifts a preference, it does not remove an
     * option, and a module that has learned better keeps its own answer.
     */
    /**
     * WHAT A DIFFICULTY SETTING ACTUALLY CHANGES.
     *
     * It used to be two floats: temperature and epsilon. Easy was the best
     * policy the project has, told to ignore itself 35% of the time. That is
     * not a gentler opponent, it is an ERRATIC one -- the same country that
     * fortified its border last turn declares war on a great power this turn
     * because a coin came up heads, and a player reads that as the game being
     * broken rather than as themselves winning. The pickAction comment about
     * random declarations reading as "deranged rather than merely weak" is that
     * failure, already half-diagnosed.
     *
     * A skill ladder should take faculties away, not add noise. Each tier below
     * plays the SAME policy with fewer of the things that make it strong:
     *
     *   critic   the Q head's opinion blended into the choice
     *   aim      the learned target and attack heads -- whom to declare on and
     *            which province to take. Without them the old margin rule aims,
     *            which is exactly how this AI played before those heads existed
     *            and is a perfectly reasonable weaker opponent.
     *   posture  the stance bias, i.e. whether the country has a plan at all
     *
     * Epsilon stays, much smaller, and only as a little unpredictability so a
     * human cannot read the AI like a table. Easy is now a player who takes the
     * obvious move without a plan; Insane is the full apparatus at argmax.
     */
    struct DifficultyProfile {
        float temperature;
        float epsilon;
        bool  useCritic;
        bool  useLearnedAim;
        bool  usePosture;
    };
    static const DifficultyProfile DIFFICULTY[4];

    /**
     * How often a refusal comes with no explanation at all.
     *
     * Not a fallback -- a move. A country that always has an answer ready is as
     * readable as one that always tells the truth, and "declined, said nothing"
     * is a real thing for a player to be told. Low enough that a refusal is
     * usually informative, high enough that silence is not remarkable when it
     * happens.
     */
    static constexpr float REFUSAL_SILENCE_CHANCE = 0.15f;
    /**
     * How often a war is declared with no justification offered at all.
     *
     * Higher than the refusal figure on purpose. Refusing an offer in silence
     * is mildly rude; invading a neighbour without a word is a statement, and
     * one a player should meet often enough to recognise the countries that
     * bother with a pretext and the ones that do not.
     */
    static constexpr float WAR_GOAL_SILENCE_CHANCE = 0.25f;

    /**
     * How much being caught lying costs you at the next table.
     *
     * Applied as a logit bias on ACCEPT, scaled by how far the asker's word has
     * fallen: full credibility subtracts nothing, none subtracts all of this.
     * At the hard-difficulty temperature that is a swing of roughly four in the
     * odds -- enough that a reputation is worth keeping, not so much that a
     * country everybody distrusts can never sign anything again.
     *
     * A BIAS, like AI_NAP_WILLINGNESS and AI_CALL_RELUCTANCE beside it. Nothing
     * is blocked, nothing is greyed out, and the net can still say yes to
     * somebody it has every reason to doubt -- which is right, because
     * sometimes the deal is worth it anyway.
     */
    static constexpr float CREDIBILITY_WEIGHT = 0.5f;

    /**
     * ZERO, AND THE REASON IS A MEASUREMENT RATHER THAN A CHANGE OF MIND.
     *
     * Everything above is still what this is for. What it assumes is that the
     * stance head holds a DISTRIBUTION -- that a country's posture is sometimes
     * one thing and sometimes another, so the lean is situational. Measured
     * after the head was reset and retrained: consolidate 99.5% of
     * country-turns, the other three sharing the remaining half a percent.
     *
     * A posture that never changes is not a posture. Its row of the table
     * becomes a fixed offset added to every decision the country ever makes --
     * +0.35 on reinforce and -0.70 on declare war, forever, in every situation
     * -- which is not authority, it is a thumb permanently on the scale. The
     * war module duly collapsed onto reinforce at 100.0% while declarations sat
     * near zero, and two reward terms were rewritten chasing that before the
     * stance table was suspected.
     *
     * So the mechanism stays, wired and tested, and the weight is zero until
     * the stance head can be shown to hold more than one opinion. Raising it
     * again is one number; the thing to check first is the stance share in the
     * eval, which is reported for exactly this reason.
     */
    static constexpr float STANCE_BIAS = 0.0f;
    /**
     * Per-stance leanings, in units of STANCE_BIAS. Rows are stances in the
     * order above; the arrays are indexed by that module's action numbering
     * (see ECON_ACTIONS and friends). Zero means "no opinion", which is most of
     * it: a posture that has a view on every action is not a posture.
     */
    static const float STANCE_ECON[STANCE_COUNT][ECON_ACTIONS];
    static const float STANCE_POL [STANCE_COUNT][POL_ACTIONS];
    static const float STANCE_WAR [STANCE_COUNT][WAR_ACTIONS];
    static const float STANCE_NAVY[STANCE_COUNT][NAVY_ACTIONS];

    /**
     * What the target head is told about one candidate, beyond our own state.
     *
     * Relative rather than absolute wherever possible: "twice my army" is the
     * same decision at any scale, while "40,000 troops" is not, and the same
     * net has to work on a twelve-country map and a two-hundred-country one.
     */
    static constexpr int TARGET_FEATURES = 12;
    /** Most candidates ever scored. Beyond this the weakest are dropped. */
    static constexpr int TARGET_MAX_CANDIDATES = 12;
    /**
     * Updates the target head needs before it chooses anything.
     *
     * Until then the old rule picks, and the head is trained on what the rule
     * did -- so it starts from imitation of a sane policy rather than from
     * noise, and a fresh model does not open its first game by declaring war on
     * whoever a random net happened to score highest.
     */
    /**
     * LOWER THAN THE ATTACK HEAD'S, because it is fed far more slowly.
     *
     * Both heads warm up by watching the old rule choose, and both used to wait
     * for 300,000 updates. But an assault candidate set is built on most turns
     * a country is at war, while a DECLARATION candidate set is built only when
     * a war is actually declared -- and declarations are rare. Measured on one
     * worker: the attack head takes about 92,000 updates an hour and the target
     * head about 8,700. The same threshold therefore meant three hours for one
     * and thirty-four for the other, so in any run anybody actually performs,
     * the head that picks WHO to fight would never once have been consulted.
     *
     * ...AND 50,000 WAS STILL WRONG, because that rate was measured on short
     * maps. A rotation of 250-turn worlds is mostly opening turns, when there
     * are many countries and many declarations; the training pool runs
     * 10,000-turn maps, which are mostly late game, where the survivors are few
     * and already at war with each other. Measured in the pool itself: about
     * 750 target-head updates an hour, not 8,700. 50,000 would have been
     * sixty-six hours.
     *
     * 10,000 is around thirteen hours there. The lesson worth keeping is that a
     * threshold extrapolated from a benchmark's map mix says nothing about a
     * trainer's.
     */
    static constexpr uint64_t TARGET_WARMUP_UPDATES = 10000;

    /**
     * WHERE TO ATTACK, learned, on the same terms as whom to declare on.
     *
     * The war policy chooses a KIND of action -- hold, recruit, attack, sue for
     * peace. Everything about what that action then does was a fixed rule:
     * execWar case 3 walked the frontier, scored each adjacent enemy province
     * by attacker-to-defender ratio with a bonus for claims and a bigger one
     * for rebels, and took the best. So the model picked the verb and a
     * hand-written heuristic picked the noun -- and since the noun is what
     * decides whether the war is won, the ceiling on how well this AI plays was
     * the quality of that heuristic rather than anything training could reach.
     * A player experiencing "the AI attacked my weakest province again" was
     * experiencing forty lines of C++, not a policy.
     *
     * The head scores candidates exactly as m_target does for declarations,
     * with the same warmup arrangement and for the same reason: below the
     * threshold the OLD RULE still chooses and the head merely watches, so it
     * learns from a policy that already works instead of from noise. Without
     * that, nothing is recorded until it is good and it is never good because
     * nothing was recorded.
     *
     * WHAT IS STILL A RULE, deliberately: which candidates exist at all. A
     * province is only offered if the attack is winnable on the same arithmetic
     * as before. That is a mask, in the same sense validWar is a mask, and it
     * keeps the head choosing among sane options rather than free to throw
     * armies at fortresses -- the failure that would read to a player as the
     * change having made the AI worse.
     */
    static constexpr int ATTACK_FEATURES = 12;
    static constexpr int ATTACK_MAX_CANDIDATES = 16;
    static constexpr uint64_t ATTACK_WARMUP_UPDATES = 300000;

    /**
     * HOW MANY FRONTS ONE DECISION TO ATTACK COVERS.
     *
     * The war module is asked once per country per turn, and for its whole life
     * one "attack" produced exactly one move order. A country with fifteen
     * active fronts therefore pushed on one of them; a player pushes on all
     * fifteen. That is a cap on how competent this AI can ever look which no
     * amount of training reaches -- worse, training ADAPTS to it, because
     * pressing an attack you cannot follow up is genuinely worth less when you
     * only get one. Left in place, hours of self-play would tune a policy for a
     * game that is about to change.
     *
     * The fix is in the executor, not the decision: the policy still decides
     * ONCE that this is a turn for attacking, and the order goes out everywhere
     * it can. That is also how a player plays -- you resolve to go on the
     * offensive and then issue all your orders, you do not re-litigate it per
     * province.
     *
     * Capped rather than unlimited, and one order per launching province, so a
     * country cannot empty every garrison in a turn. Four is enough to make a
     * broad front behave like one and small enough that the order queue on a
     * 185-country map stays the size it was.
     */
    static constexpr int ATTACK_ORDERS_PER_TURN = 4;

    /**
     * One winnable assault: move from `fromPid` into `toPid`.
     *
     * Carries the garrisons and the province's fortification rather than
     * letting the feature builder look them up again. execWar has just computed
     * all three to decide the candidate was winnable at all, and `garrisonOf`
     * is a local lambda duplicated in two functions already -- a third copy to
     * recompute a number we are holding would be the wrong way round.
     */
    struct AttackCandidate {
        int fromPid = -1;
        int toPid = -1;
        int enemyCid = 0;
        float margin = 0.0f;   // attacker over defender, the old rule's score
        bool fromAlly = false; // launched from an ally's ground
        int myGarrison = 0;
        int theirGarrison = 0;
        int fortLevel = 0;
        int indLevel = 0;
    };

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
    // BACK TO 2.00 AND 2.20. Raising these to 2.50/2.75 was half of what
    // produced a policy that declared literally zero wars: measured against a
    // random-action control on the same maps, 0.00 declarations per thousand
    // country-turns against random's 3.84 and 4.32. A country that never fights
    // never takes ground, and the model held 36% of the world to random's 64%
    // as a direct result. The bar is still nearly twice the 1.05 it started at,
    // so wars of pure opportunism remain expensive -- they are simply possible
    // again.
    static constexpr double AI_WAR_BAR_UNCLAIMED       = 2.00; // 2.50 was too high
    static constexpr double AI_WAR_BAR_UNCLAIMED_NAVAL = 2.20; // 2.75 was too high
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
    // `statedReasonOut`, when given, receives what the country SAYS about a
    // refusal -- which is chosen separately from why it actually refused, and
    // need not match. REFUSE_NONE means it declined to explain itself.
    void noteTradeOutcome(int cid, float goldDelta) { m_tradeOutcome[cid] += goldDelta; }

    bool decideDiplomacy(int targetCid, const std::string& action,
                         const std::string& sourceIso,
                         const std::string& subjectIso = std::string(),
                         int* statedReasonOut = nullptr);
    /**
     * What to SAY, given why we actually refused.
     *
     * Three moves: say nothing, say the true thing, or say something else. The
     * rule below is a v1 heuristic and deliberately a simple one -- the choice
     * of when to lie is exactly the sort of thing that should eventually be
     * learned, and the counters in TrainStats are there so it can be judged
     * before it is.
     *
     * What it will not do is state anything `askerCid` can check against the
     * map. That is not a restriction on what the AI is ALLOWED to say -- the
     * player has the same open list, and so does the AI in principle -- it is
     * the AI playing well. An opponent caught in an excuse the map disproves
     * has told you it is not paying attention.
     */
    int chooseStatedRefusal(int selfCid, int askerCid, int trueReason);
    /**
     * The goal this country is really fighting `defenderCid` for. PRIVATE.
     *
     * Never shown, never serialised into the relation, and deliberately not
     * exposed to the UI. It is derived from what findWarTarget knew when it
     * picked the war, and it stays inside the AI because a war aim you can read
     * off a panel is not an aim, it is a label.
     *
     * It is not hidden in the sense of being inert, either -- the same claim
     * that makes a war one of reconquest is already what steers the attack head
     * toward those provinces and what the ceasefire composer trades around. So
     * the player CAN learn it, by watching which ground the AI goes for and
     * which ground it will not sell. That is the intended way to find out.
     */
    int trueWarGoal(int selfCid, int defenderCid) const;
    /**
     * ...and what it announces instead, which need not be the same thing.
     *
     * A country that simply wants land is in the awkward position that the only
     * unfalsifiable thing it can say is the truth. So it looks for a pretext
     * that happens to be true -- a claim it holds, a border it shares, a rival
     * grown too large -- and states the most legitimate one available. Failing
     * that it either admits to conquest or says nothing, which are both
     * perfectly good moves and read very differently to whoever it just
     * attacked.
     */
    int chooseStatedWarGoal(int selfCid, int defenderCid, int trueGoal);
    // "They said no" — back the pair off hard rather than re-asking as soon as
    // the ordinary cooldown lapses.
    void noteDiploRejected(int sourceCid, int targetCid);
    /**
     * `speakerCid` told `hearerCid` why it refused. Either may be the player.
     *
     * The receiving end of the statement channel, and deliberately separate
     * from noteDiploRejected: that one is about the refusal, this one is about
     * what was SAID about it, and a refusal with no explanation is a real thing
     * that still has to arrive. Today it counts what was heard and whether the
     * map contradicts it; that check is the hook credibility will hang on, and
     * it is written here rather than at the call sites so the player's
     * statements and the AI's are judged by the same rule.
     */
    void noteRefusalHeard(int speakerCid, int hearerCid, int statedReason);
    /** Coalition counters for the trainer dashboard. */
    // `cid` is the country the event belongs to: the caller for an issued
    // call, the ally deciding for an answer or a refusal. It selects which
    // cohort's counters move.
    void noteCallIssued(int cid)   { statsFor(cid).callsIssued++; }
    void noteCallAnswered(int cid) { statsFor(cid).callsAnswered++; }
    void noteCallRefused(int cid)  { statsFor(cid).callsRefused++; }
    /** One province changing hands, attributed to both sides by cause. */
    /**
     * A hull went down. attacker may be <= 0 when nobody owns the kill.
     * crew is what was aboard: 0 for a warship, the cargo for a transport.
     */
    void noteShipSunk(int attackerCid, int victimCid, int crew);
    void noteConquest(int winnerCid, int loserCid, bool contested);
    /** An assault that lost: the attacking stack died taking nothing. */
    void noteAssaultRepulsed(int attackerCid, int troopsLost);
    void noteRevolt(int loserCid) { statsFor(loserCid).provLostToRebel++; }
    // A funded-at-zero turn with a node still active. See researchStalls.
    void noteResearchStall(int cid) { statsFor(cid).researchStalls++; }
    void noteResearchFunded(int cid){ statsFor(cid).researchFundedTurns++; }
    /**
     * A node finished. Counted HERE, at the completion site, and not by
     * diffing the size of m_countryResearched from the experience loop.
     *
     * That loop lives in endTurn(), which returns early when aiLearning is
     * off -- so every evaluation reported "research 0.00 per 1k" no matter how
     * much research happened, and it happens constantly (385 nodes in one
     * 300-turn map). The control cohort was worse off still: random countries
     * return before m_pending is populated, so their figure was structurally
     * zero even during training.
     *
     * Counting at the source also drops the baseline bookkeeping the old
     * version needed: nodes GRANTED by the map at start never pass through
     * here, so they cannot be miscounted as completions.
     */
    void noteResearchDone(int cid)  { statsFor(cid).researchCompleted++; }
    /** Same reasoning: counted per country-turn in decide(), not in endTurn(). */
    void noteBankruptTurn(int cid)  { statsFor(cid).bankruptTurns++; }
    void noteTreatyTransfer(int toCid, int fromCid) {
        statsFor(toCid).provByTreaty++;
        statsFor(fromCid).provCededByTreaty++;
    }

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
    /**
     * Move every net `share` of the way toward `peer`'s. False if any pair has
     * incompatible shapes, in which case some may already have moved -- callers
     * discard the result rather than trying to unwind it.
     *
     * ONE list, used by both the peer sync and the final merge, because there
     * were two and they had drifted from the model. Both blended the trunk, the
     * four policy heads, the four value heads and the diplomacy net -- and
     * silently skipped the stance head, the war-target head, all four Q heads,
     * the relational encoder and scorer, and the diplomacy value head. Eight
     * nets out of fifteen. In a parallel pool that meant every worker's
     * learning on those eight was thrown away at the merge and replaced with
     * whatever the FIRST input file happened to hold, which is not a merge of
     * anything. Adding a net and forgetting one of two lists is how that
     * happened; there is now one list to forget.
     */
    bool blendAllToward(const AISystem& peer, float share);
    /** Average several model files into one. Backs `--merge-ai`. */
    static bool mergeModelFiles(const std::string& outPath,
                                const std::vector<std::string>& inPaths);
    /**
     * Throw away one module's learning and leave the rest alone.
     * Backs `--reset-ai-head`. `module` is a MOD_* index, or MOD_COUNT for the
     * diplomacy head (whose value baseline is m_diploValue and whose reward
     * statistics are POLITICS' and are therefore left alone).
     *
     * For when a reward function is corrected after the policy has already
     * converged on the old one. A converged softmax puts almost no mass on the
     * actions it has learned to avoid, so the corrected reward is never
     * sampled often enough to pay — the head has to be told, not persuaded.
     *
     * Resets the module's POLICY, its VALUE baseline and its reward
     * normalisation statistics together, because they only mean anything as a
     * set: a baseline fitted to the old policy's returns would score a fresh
     * policy's every move as a large surprise, and the running mean and
     * variance describe a reward distribution that no longer exists.
     */
    static bool resetModuleHead(const std::string& modelPath, int module);

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
    /**
     * The control cohort's brain, for `--eval-ai --vs-model <path>`.
     *
     * Empty means the control group picks uniformly at random, which is the
     * only reference --eval-ai has ever had. Random is a FLOOR, not a level: it
     * can say "better than a coin flip" and nothing else, so a model that has
     * cleared it has no yardstick left. Naming a model file here makes the
     * control group play THAT instead — same cohort split, same counters, same
     * report — so the question becomes "better than this specific opponent",
     * which is a question that keeps meaning something after parity.
     *
     * STATIC and a path rather than an argument, for the same reason
     * s_readOnlyModel is: --eval-ai destroys and rebuilds the AISystem on every
     * map, so the opponent has to be re-loaded per map from something that
     * outlives the object.
     */
    static std::string s_opponentModelPath;
    /**
     * RUNG ONE. The control cohort plays a hand-written competent policy.
     *
     * Random is a floor that never rises, so it answers "better than nothing"
     * and then stops meaning anything. A model opponent is a rung, but only
     * once you have a model worth pinning -- and until this project has one,
     * "as good as an intermediate player" has nothing to be measured against
     * at all.
     *
     * So: a player written down. It attacks what it can beat, keeps its books,
     * researches continuously, sues for peace when it is losing, answers its
     * allies and calms its own unrest. Nothing in it is clever and nothing in
     * it is learned; it is the standard of "somebody who has read the manual
     * and is paying attention", which is exactly the bar in question.
     *
     * It shares every reflex, mask and restraint constant with the model
     * cohort, like the random one does -- the only difference is where the
     * choice comes from.
     */
    static bool s_updTrace;
    static bool s_scriptedControl;
    /**
     * SCRIPT AGAINST SCRIPT: does aggression pay in this game at all?
     *
     * With this set, BOTH cohorts are hand-written and no network is consulted
     * anywhere. One side builds an army and attacks with it; the other builds
     * an army and never attacks, defending what it has and making peace when it
     * can. Same reflexes, same masks, same restraint constants, same matched
     * split -- the only difference between them is whether they ever go on the
     * offensive.
     *
     * It exists because a day of reward work kept arriving at the same place. A
     * policy trained to attack lost; a policy that recruited on 100% of turns
     * and barely fought won 1.645x against a competent aggressor; and every
     * reward edit that made attacking more attractive made the model worse. The
     * measured advantage on the mature model has ATTACK at -0.085, below doing
     * nothing.
     *
     * All of that is consistent with the AI having correctly learned that
     * aggression does not pay here -- which would be a fact about the game's
     * balance, not about the AI, and no amount of reward tuning would fix it.
     * This removes the networks from the question entirely so the game can
     * answer it on its own.
     */
    static bool s_scriptDuel;
    enum ScriptVariant { SCRIPT_AGGRESSOR = 0, SCRIPT_TURTLE = 1 };
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
                  pactsProposed = 0, researchCompleted = 0,
                  tradesOffered = 0;
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
        /**
         * HULLS LAID DOWN. The other end of the fleet, and it was not counted.
         *
         * The economy module builds them -- execEconomy cases 5 and 6 -- and
         * only scrapping was ever tallied, so a policy that built a navy it
         * could not crew or use looked identical in every number to one that
         * built none at all. The navy reward pays 0.5 x tanh(dShips) when there
         * is a crossing to make and charges 0.4 when there is not, and neither
         * side of that could be checked against what was actually laid down.
         *
         * Split by type because they are different decisions: a destroyer is 15
         * and needs a level-2 port, a carrier is 40 and needs a level-3, and a
         * module that has learned to build only the cheap one is doing
         * something specific rather than something vague.
         */
        long long destroyersBuilt = 0, carriersBuilt = 0;
        // Coalition behaviour. callsAnswered/callsRefused is the readout that
        // says whether alliances mean anything yet; stagingMoves says whether
        // anyone is using an ally's ground to reach a front.
        long long callsIssued = 0, callsAnswered = 0, callsRefused = 0;
        long long stagingMoves = 0;
        /**
         * EVERY diplomatic request this cohort was asked, and how many it said
         * yes to -- ceasefires, alliances, non-aggression pacts, guarantees and
         * calls to arms together.
         *
         * The calls counters above could not see the failure that made this
         * necessary. Every answer was an unconditional reject (see
         * decideDiplomacy), so no alliance ever formed; with no alliances
         * nobody could issue a call to arms; and with no calls issued the
         * coalition line read "0 of 0 answered" -- which is not a policy, it is
         * an empty denominator, and the blunder gate correctly declined to
         * judge it. The one number that would have shown it plainly was how
         * often anybody agreed to anything, and nothing counted that.
         *
         * Requests are counted where they ARRIVE, including the ones the
         * heuristic gates decline before the net is consulted: from the asking
         * country's side those are refusals like any other.
         */
        long long diploRequests = 0, diploAccepted = 0;
        /**
         * What it said when it said no. See RefusalReason.
         *
         * `refusalsLied` is the interesting one and `refusalsCaught` is an
         * INVARIANT: a lie the asker can check against the map should never be
         * chosen, so this must stay at zero. A non-zero reading means the
         * believability filter has stopped working, which would show up to a
         * player as an opponent whose excuses fall apart on inspection -- worse
         * than an opponent that says nothing at all.
         */
        long long refusalsSilent = 0, refusalsTrue = 0;
        long long refusalsLied   = 0, refusalsCaught = 0;
        /** ...and from the other side: what was said TO this cohort. */
        long long refusalsHeard = 0, refusalsHeardFalse = 0;
        /**
         * Declarations, by what was announced. `warGoalPretext` counts the ones
         * where the stated goal was not the real one, and `warGoalCaught` is
         * the same invariant the refusals have: a pretext the world can check
         * and disprove should never be chosen, so it must stay at zero.
         */
        long long warGoalSilent = 0, warGoalTrue = 0;
        long long warGoalPretext = 0, warGoalCaught = 0;
        /**
         * Provinces demanded at a ceasefire, and how many of them were land
         * this country actually claims.
         *
         * The share is the whole test of whether a war goal reaches the table.
         * Before the terms consulted it the demand was "the first border
         * province the iterator reached", so the claimed share was whatever
         * chance produced -- and a war fought for Danzig ended with a demand
         * for somewhere else, which is indistinguishable to a player from an AI
         * that has no war aims at all.
         */
        long long ceasefireProvsAsked = 0, ceasefireClaimedAsked = 0;
        /** Ceasefires offered while REFUSING to trade away the claim fought for. */
        long long ceasefireHeldClaim = 0;
        /**
         * Country-turns spent under each posture. Reported because the stance
         * now steers behaviour (see STANCE_BIAS) and a controller nothing
         * counts is a controller nobody can tell is stuck: a model that picks
         * "develop" on turn one and never revisits it would look, in every
         * other number here, exactly like one that thought about it.
         */
        long long stanceHeld[STANCE_COUNT] = {0};
        // WHERE THE LAND CAME FROM.
        //
        // "land held" says who ended up with the world and nothing about how.
        // A cohort that conquers its neighbours and one that quietly absorbs
        // provinces shed by other people's rebellions look identical in it, and
        // those are completely different strategies to have to beat.
        long long provTakenFromCountry = 0;  // conquered from a real country
        // Of those, how many were actually FOUGHT for. A province with no
        // defender is taken by walking into it, and a cohort that expands by
        // strolling into undefended land is playing a different game from one
        // that wins battles -- indistinguishable in any total.
        long long provTakenInBattle    = 0;
        long long provWalkedInto       = 0;
        long long provTakenFromRebel   = 0;  // taken off a rebel state
        long long provLostToCountry    = 0;  // conquered from us by a country
        long long provLostToRebel      = 0;  // seized by a rebel, or revolted
        long long provByTreaty         = 0;  // handed over at a ceasefire
        long long provCededByTreaty    = 0;
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
        // WHAT THE WAR MODULE ACTUALLY DOES, offered against chosen.
        //
        // The closed province ledger says the model shrinks because it wins far
        // fewer battles, not because it declares fewer wars -- so the question
        // is where the attacks go. Two very different answers look identical in
        // any outcome number: the MASK never offers attack (a bug, and fixable),
        // or the mask offers it and the POLICY declines (a learned preference,
        // and a reward problem). Counting both sides of that separates them.
        //
        // Chosen alone would not: an action picked 5% of the time is damning if
        // it was available every turn and unremarkable if it was available on
        // one turn in twenty.
        long long warOffered[WAR_ACTIONS] = {0};
        long long warChosen[WAR_ACTIONS]  = {0};
        /** The same, for the war module. See econProbMass. */
        double warProbMass[WAR_ACTIONS] = {0.0};
        long long warProbN[WAR_ACTIONS] = {0};
        long long warDecisions = 0;
        /**
         * DOES THE ADVANTAGE ACTUALLY DISCRIMINATE BETWEEN ACTIONS?
         *
         * The war module has now collapsed onto five different single actions
         * under four different reward configurations, the last of them onto
         * "hold". Each was diagnosed and fixed as a reward defect and each fix
         * moved the collapse somewhere else, which is the signature of a cause
         * upstream of any individual term.
         *
         * A policy gradient can only learn a preference if the advantage
         * differs by action. If it does not -- if the twelve-turn window's
         * outcome is dominated by the state and by the other eleven actions
         * taken inside it -- then the update is noise with respect to the
         * choice, and whichever action is sampled slightly more in good windows
         * gets reinforced until it takes everything. That failure looks exactly
         * like a reward bug and cannot be fixed by editing rewards.
         *
         * So: the mean advantage actually credited to each action, and how many
         * samples it came from. If the collapsed action's mean is clearly the
         * highest, the policy is right and the reward is what needs work. If
         * every action's mean sits on top of the others, the signal is not
         * there to learn from and no reward term will supply it.
         */
        double warAdvSum[WAR_ACTIONS] = {0.0};
        long long warAdvN[WAR_ACTIONS] = {0};
        /**
         * THE ADVANTAGE, SPLIT INTO ITS TWO HALVES.
         *
         * advantage = (immediate normalised reward) + BOOTSTRAP_DISCOUNT * V(end
         * of window) - V(start). The first half is what the window itself paid;
         * the second is what the window left the country WORTH, and it is the
         * only channel through which "I built an army I have not used yet" can
         * ever be credited.
         *
         * Recruiting earns +0.047 against attacking's +0.338 at every horizon
         * tested -- 6, 12 and 24 -- so the gap is not about how long the window
         * is. Splitting it says which half is missing: if the bootstrap barely
         * differs between the two, then the value head does not think an army
         * is worth anything, and no reward term on recruiting can fix that
         * because the term would only be paying for the army twice.
         */
        double warImmSum[WAR_ACTIONS]  = {0.0};   // immediate normalised reward
        double warBootSum[WAR_ACTIONS] = {0.0};   // discounted V(end of window)
        double warBaseSum[WAR_ACTIONS] = {0.0};   // V(start), the baseline
        // Same question for the economy module, which owns research. Countries
        // complete 0.00 research per 1k country-turns, and "never asks" and
        // "asks and is refused" need opposite fixes.
        long long econOffered[ECON_ACTIONS] = {0};
        long long econChosen[ECON_ACTIONS]  = {0};
        /**
         * THE SHAPE OF THE POLICY, as opposed to the shape of the sampling.
         *
         * A take rate -- chosen over offered -- is what came out of the dice
         * AFTER temperature and epsilon. Measurement runs at difficulty 2,
         * where temperature is 0.35, and at that setting a logit lead of about
         * one unit already becomes a take rate near 95%. So a module with a
         * MILD preference and one that has genuinely stopped choosing report
         * almost the same number, and the reward-term gates cannot tell them
         * apart -- which matters, because those two need opposite responses:
         * one needs nothing done, the other needs a reward corrected and a head
         * reset.
         *
         * These accumulate the policy's own probability for each action at a
         * NEUTRAL temperature of 1.0, over the masked logits, at every decision
         * the module made. Divided by the decision count they give the mean
         * P(action), which sums to 100% across a module and describes the
         * distribution the net actually holds rather than the one sampling
         * collapsed it into.
         *
         * Only accumulated for countries a policy net is actually driving --
         * not the random cohort, whose choices are dice, and not the scripted
         * rung, which never consults a net at all.
         */
        double econProbMass[ECON_ACTIONS] = {0.0};
        /** Matching denominator: offers on turns there was a CHOICE. */
        long long econProbN[ECON_ACTIONS] = {0};
        long long econDecisions = 0;
        // A node was picked and is sitting at the front of the queue, and the
        // turn resolver still made no progress on it because funding is zero.
        // Bankruptcy zeroes research allocation every turn it bites, and the
        // mask only offers "pick a node" while IDLE -- so a country that goes
        // bankrupt mid-node is locked out of research permanently: it cannot
        // fund the node and cannot abandon it.
        long long researchStalls  = 0;
        long long researchPicked  = 0;
        // Splitting the pick: an action that found a node and armed it, against
        // one that ran the whole tree and came back empty.
        long long researchArmed       = 0;
        long long researchNothingLeft = 0;
        // Funded turns that actually moved the needle on the active node.
        long long researchFundedTurns = 0;
        // WHY THE BATTLE COUNT DIFFERS.
        //
        // The model wins provinces in battle at a quarter of the control's
        // rate, and both cohorts run the SAME targeting code -- execWar's
        // attack case is not policy, it is a fixed rule, so the gap cannot be
        // combat skill. It is upstream, and there are only three places it can
        // hide: being at war less often, choosing attack less often, or
        // choosing it and finding nothing to hit.
        //
        // turnsAtWar is the denominator that matters. Battles won per
        // country-turn-at-war is the number that says whether the model
        // actually fights worse, or simply fights less.
        long long turnsAtWar        = 0;
        long long attackIssued      = 0;  // a move order went out
        // Of those, how many the LEARNED head chose rather than the margin
        // rule. Zero until ATTACK_WARMUP_UPDATES, and the only way to tell a
        // head that is steering badly from one that has not been let out yet --
        // two situations with opposite fixes and identical outcome numbers.
        long long attackSteered     = 0;
        long long attackNoTarget    = 0;  // nothing winnable adjacent
        long long attackPending     = 0;  // that province already had an order
        /**
         * ASSAULTS THAT LOST, and the men they cost.
         *
         * The funnel could count attacks issued and provinces won and had no
         * way to tell the difference between an attack still in progress and
         * one that was thrown away: `attackIssued - provTakenInBattle` lumps
         * them together. So "attacks into a fight it could not win" -- the
         * single most defining trait of a player who is NOT an intermediate --
         * was not merely un-gated, it was unmeasurable.
         *
         * Counted where the turn resolver kills the attacking stack, on both
         * routes into a province: over a land border and off a ship.
         */
        long long attacksRepulsed   = 0;
        long long troopsLostAttacking = 0;
        /**
         * MICROSECONDS SPENT THINKING, and country-turns spent thinking them.
         *
         * A playability number, and the only one here that is not about how
         * well the AI plays. The present-day scenario has 185 countries and
         * every one of them runs a trunk pass and four heads every turn; if
         * that adds a second to end-turn, the AI is bad in a way no ADVANTAGE
         * figure will ever report and every player will notice immediately.
         * Kept per cohort like everything else, so a change that makes the
         * model think harder shows its bill next to its benefit.
         */
        long long thinkMicros = 0;
        long long thinkCalls  = 0;
    };
    const TrainStats& trainStats() const { return m_trainStats; }
    /**
     * Mean and worst political distance across every live real country.
     *
     * Reported by the eval because a feature is only worth having if it is ever
     * non-zero: a generated map that carries no compass data would leave these
     * dead, and dead looks exactly like a world in perfect agreement with
     * itself. See CountryStat::compassGapMean.
     */
    void compassGap(float& meanOut, float& worstOut) const;
    /**
     * How many live wars are being fought against countries that hold no land.
     *
     * AN INVARIANT, and it must read zero. A conquered map country keeps its
     * entry so an amphibious landing can revive it, and for a long time it kept
     * its WAR relations too -- so refreshStats, which builds m_warWith straight
     * from those rows, counted a permanent war for anybody who had ever
     * finished someone off. atWar stayed true forever, warInWindow with it, and
     * the idleness charge that tests warInWindow could never fire for a
     * conqueror. It was invisible in every outcome number; this is what would
     * have shown it.
     */
    int warsWithTheDead() const;
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
    /**
     * Multiplies every learning rate, from OD_LR_SCALE. 1.0 is the shipped
     * behaviour. Exists because these rates are high for Adam -- 0.005 policy
     * and 0.010 value against a usual 3e-4..1e-3 -- and were RAISED when batched
     * updates arrived, which is the wrong direction: averaging a batch reduces
     * gradient noise, it does not license longer steps.
     */
    static float lrScale() {
        static const float s = [] {
            if (const char* e = std::getenv("OD_LR_SCALE")) {
                const float f = (float)atof(e);
                if (f > 0.0f && f <= 10.0f) {
                    printf("[AI] LR scaled by %.4f\n", f);
                    return f;
                }
            }
            return 1.0f;
        }();
        return s;
    }
    static constexpr float LR_VALUE  = 0.010f;  // was 0.005 (per-sample era)
    // The diplomacy head never left the per-sample era and must not be dragged
    // into the rise above. It learns only when somebody actually proposes
    // something — measured at ~0.66 experiences per turn against a policy
    // head's fifty — so its "batch" is usually a single sample and its gradient
    // carries none of the noise reduction that justifies a larger step.
    static constexpr float LR_DIPLO  = 0.002f;
    /**
     * Q is a regression onto the same target the value head fits, so it takes
     * the value rate. The target head is a policy over candidates, so it takes
     * the policy rate.
     *
     * Both exist because both nets need a flushBatch call of their own.
     * mergeScratch only sums gradients into a batch; flushBatch is what applies
     * one. Adding a net without adding both lines gives you a head that
     * accumulates gradients for hours and never moves -- which is exactly what
     * Q did until this was noticed, sitting at its initial weights while
     * looking, from every log and every test, like a feature waiting to warm up.
     */
    static constexpr float LR_Q      = 0.010f;
    static constexpr float LR_TARGET = 0.005f;
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
    // Out of line because the cohort is not always dice: with an opponent model
    // loaded these same countries play it instead. See s_opponentModelPath.
    void setRandomCountries(std::unordered_set<int> cids);
    bool isRandomCountry(int cid) const { return m_randomCids.count(cid) > 0; }
    /**
     * True when s_opponentModelPath named a file and it loaded.
     *
     * The caller MUST check this before running: a mistyped path that silently
     * fell back to random selection would produce a full report labelled
     * "OPPONENT" over numbers measured against dice, and nothing downstream
     * could tell. A measurement that did not happen must never look like one
     * that came back uninteresting.
     */
    bool opponentLoaded() const { return m_opponentLoaded; }

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
        /**
         * HOW FAR THIS COUNTRY IS FROM ITS OWN PROVINCES, POLITICALLY.
         *
         * m_provinceCompass has always existed and always mattered: rebel
         * factions are formed by grouping provinces of SIMILAR compass, their
         * composition and name come from the average of it, and doctrines shift
         * it toward or away from the government. And buildFeatures never read
         * it once. So the politics module chose doctrines that move the
         * country's own compass with no way to see whom that alienated -- while
         * rebellion is the single largest penalty in its reward, at
         * -2.5 x tanh(rebels/2). It was being punished hardest for an outcome
         * whose main driver it could not observe.
         *
         * The same three questions the minority features above answer, asked of
         * political rather than ethnic composition: how far on average, how far
         * at the worst, and how much of the country is a long way off.
         * Euclidean over the two axes, each clamped to [-100, 100] by the game,
         * so a distance of 200 is as far apart as two provinces can be.
         */
        float compassGapMean  = 0.0f;   // 0..1
        float compassGapWorst = 0.0f;   // 0..1
        float compassGapShare = 0.0f;   // fraction of provinces beyond FAR
        float minorityCost = 0.0f;     // what that option set costs per turn
    };
    // Reward horizon: each decision is judged by the state change over the
    // NEXT N_STEP turns, not the same turn. One-turn deltas taught passivity —
    // spending money was punished instantly while the payoff (industry income,
    // conquered provinces, suppressed rebellions) landed many turns later,
    // credited to nothing.
    static constexpr int N_STEP = 12;

    /**
     * What the state at the END of a reward window is worth to the decision
     * that opened it. Per WINDOW, not per turn -- one step here is N_STEP turns.
     *
     * 0.9 gives an effective horizon of about ten windows, i.e. 120 turns,
     * against the 12 it had when the window's own reward was the entire target.
     * A whole war now fits inside what a decision can be credited for.
     *
     * Zero restores the old behaviour exactly, which is what makes this a
     * one-line experiment rather than a rewrite.
     */
    static constexpr float BOOTSTRAP_DISCOUNT = 0.9f;

    /**
     * How loudly Q is allowed to argue with the policy. 0 disables it entirely
     * and restores pure REINFORCE action selection.
     *
     * Q is trained toward the same normalised target the value head is, so its
     * spread is in reward standard deviations -- roughly the same scale the
     * policy's logits end up at, which is why 1.0 is a sensible starting point
     * rather than an arbitrary one. Turn it up and the actor becomes a prior
     * the critic overrules; turn it down and the critic only breaks ties.
     */
    static constexpr float Q_BLEND = 1.0f;

    /**
     * Updates a Q head needs before it is allowed to influence anything.
     *
     * Every model file written before Q existed has no Q head, so it starts
     * from random weights. An untrained critic overruling a policy with
     * millions of updates behind it does not degrade gracefully -- it looks
     * exactly like the actor having forgotten how to play. Below this count
     * Q is trained and ignored.
     *
     * THE OLD 2,000,000 WAS FAR TOO LOW, and the paragraph above turned out to
     * describe the shipping model exactly. Bisected 2026-08-06 by forcing each
     * warmup gate shut in turn and measuring land share against the scripted
     * rung over 400 turns, three seeds, on the shipping model:
     *     everything learned          62.7%
     *     target head on its rule     62.7%   (bit-identical: never graduated)
     *     attack head on its rule     62.7%   (bit-identical: never graduated)
     *     CRITIC ON ITS RULE          76.7%
     *     all three on their rules    76.7%   (i.e. the critic was the only
     *                                          gate actually open)
     * Fourteen points of play, given away by a critic that had passed the
     * threshold without having learned enough to be worth listening to.
     *
     * Raised rather than deleted, and Q is still TRAINED below the gate, so
     * re-enabling is a one-line change once there is evidence the critic helps.
     * That evidence does not exist today: no configuration measured has been
     * better with the critic on. OD_Q_WARMUP overrides this for experiments.
     *
     * For scale, the shipping model carries ~25M policy updates, so this is
     * "not until somebody demonstrates it earns its place".
     */
    static constexpr uint64_t Q_WARMUP_UPDATES = 100000000000ULL;

    /**
     * BISECTION HOOKS for the three warmup gates.
     *
     * Each head defers to a hand-written rule until its update counter passes
     * the threshold above, then takes over. Measured 2026-08-06: a model with
     * zero updates -- rules everywhere -- holds 92.4% of the land against the
     * scripted rung at 400 turns, while the shipping model at ~25M updates
     * holds 62.7%. Every head that has graduated has made the AI worse, so
     * these exist to find out WHICH one by forcing a gate to stay shut.
     *
     * OD_TARGET_WARMUP / OD_ATTACK_WARMUP / OD_Q_WARMUP, in updates. Set one
     * absurdly high to keep that head on its rule while the others run learned.
     */
    static uint64_t warmupOverride(const char* var, uint64_t dflt) {
        if (const char* e = std::getenv(var)) {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(e, &end, 10);
            if (end && end != e) return (uint64_t)v;
        }
        return dflt;
    }
    static uint64_t targetWarmup() {
        static const uint64_t v = warmupOverride("OD_TARGET_WARMUP", TARGET_WARMUP_UPDATES);
        return v;
    }
    static uint64_t attackWarmup() {
        static const uint64_t v = warmupOverride("OD_ATTACK_WARMUP", ATTACK_WARMUP_UPDATES);
        return v;
    }
    static uint64_t qWarmup() {
        static const uint64_t v = warmupOverride("OD_Q_WARMUP", Q_WARMUP_UPDATES);
        return v;
    }

    /**
     * What a war costs the war module beyond what the fighting itself costs.
     *
     * BOTH ARE ZERO NOW, and the reason is BOOTSTRAP_DISCOUNT rather than a
     * change of heart about aggression.
     *
     * These charges existed to stand in for a payoff the learner could not see.
     * The comments below them say so outright: conquest pays +2.0 x tanh(dProv)
     * but "a war rarely concludes inside the twelve-turn reward window", so the
     * gain landed after the window closed while the cost landed inside it, and
     * the policy read the arithmetic and stopped fighting. The answer at the
     * time was to shrink and reshape the charges until war was affordable
     * again.
     *
     * The value bootstrap removes the premise. A decision is now credited with
     * what the country is worth at the END of its window, so a war that takes
     * forty turns to pay off is visible to the update that started it. With the
     * horizon fixed, these terms stop being a correction and become a thumb on
     * the scale -- and the measurement says which way it was pressing: against
     * a control that invades at random, the trained policy declared 2.34 wars
     * per thousand country-turns to the control's 10.64, and held 42% of the
     * world to its 58%, at equal survival.
     *
     * Restore either by setting it back: -0.5 and -0.35 are what they were.
     */
    static constexpr float WAR_AGGRESSION_CHARGE = 0.0f;  // was -0.35

    /**
     * WHAT A WASTED WINDOW COSTS -- war or no war, ONE price.
     *
     * This replaces two separate terms that were always meant to be the same
     * one. The peace case charged a hardcoded -0.5 and carried the comment
     * "Equal to phoneyWar, deliberately ... so peace is never the cheap way to
     * avoid the tax"; the war case went through WAR_PHONEY_CHARGE, which was
     * later set to zero on its own. From that moment the two were not equal,
     * and the asymmetry pointed the other way: sitting out a war cost nothing
     * while sitting still at peace cost 0.5, so DECLARING WAR WAS A DISCOUNT.
     *
     * Measured thirty minutes into the first training run on the corrected
     * armyTerm: the war module chose "declare war" on 93% of the turns it was
     * offered and "recruit" on 0.6% -- an exact inversion of the months it had
     * previously spent recruiting on 98.6% and never fighting. Closing the
     * recruiting escape had not fixed the incentive, it had moved the policy to
     * the next-cheapest door out of the same room.
     *
     * So: one condition, one constant, no way for the two halves to drift apart
     * again. A war buys exactly one window of grace -- mobilising and reaching
     * a border genuinely takes time -- and after that it has to be producing
     * something, which is what is asked of every other turn in the game.
     */
    static constexpr float IDLE_CHARGE = -0.5f;

    /**
     * How much unrest counts against every module's shared reward.
     *
     * Halved from 0.8, because conquest CAUSES rebellions: newly taken
     * provinces are the unhappy ones. At full price this was the anti-war
     * charge arriving again through a side door, and it applied to the economy
     * and navy modules too, which do not choose the wars.
     *
     * Not removed. Unrest that is actually costing provinces is already priced
     * by the dLost term; this is what remains for unrest that has not cost
     * anything yet, and a country tearing itself apart should still notice.
     */
    static constexpr float UNREST_WEIGHT = 0.4f;  // was 0.8

    /**
     * WHAT PLAYING AGAINST SOMEBODY IS WORTH.
     *
     * Every other dense term in this reward is a quantity of our OWN: land
     * gained, income raised, unrest suffered, nodes finished. Add them up and
     * the policy is optimising a dashboard. A country can hold every one of
     * those numbers flat while the strongest power on the map doubles, and
     * nothing in the dense reward has anything to say about it -- so nothing
     * ever taught the AI that a neighbour running away with the game is its
     * problem. That is why it will not coalition against a runaway player,
     * which is the behaviour anyone who has played a grand strategy game
     * expects first.
     *
     * The competitive signal did exist, but only at the TERMINAL: +4 for
     * winning, -4 for elimination, and a final-standing term in [-2,+2] for a
     * map that never resolved. One outcome, after hundreds of windows of dense
     * shaping, reached through a value function fitted mostly on the dashboard.
     *
     * These two terms put the same question in the dense reward:
     *
     *   STANDING  -- did we move up or down the table. The percentile is what
     *                the world snapshot already ranks for the features; this
     *                pays for changing it. Overtaking somebody is worth
     *                something even on a turn we took no ground, because
     *                somebody else lost some.
     *   LEAD      -- did the gap between us and the strongest OTHER country
     *                narrow or widen. Deliberately not "our share": that is
     *                dProv again in different units. This term moves when the
     *                leader moves, which is the entire point -- standing still
     *                while the leader grows is now a loss, and it is a loss for
     *                every module, because staying in the game is not the war
     *                module's private problem.
     *
     * Scales: a rank step on a forty-country map is about 0.026, so STANDING
     * saturates at roughly two places. Three provinces on a six-hundred
     * province map is about 0.005 of the world, so LEAD reads a good-sized
     * conquest as about a quarter of its range. Both are set comparable to the
     * 0.6 on dProv rather than larger: they are meant to be the reason a close
     * call goes one way, not a new thing to farm.
     */
    static constexpr float STANDING_WEIGHT = 0.4f;
    static constexpr float STANDING_SCALE  = 0.05f;
    static constexpr float LEAD_WEIGHT     = 0.4f;
    static constexpr float LEAD_SCALE      = 0.02f;

    /**
     * How far the policy may move on one decision before the update stops
     * paying for more. The standard 0.2, and standard for a reason: it is loose
     * enough that ordinary learning is untouched and tight enough that a stale
     * sample cannot take a large step.
     *
     * It matters more here than in a typical setup. A decision waits N_STEP
     * turns for its reward and the policy is updated every turn, so by the time
     * a sample is used the weights that produced it are hundreds of updates
     * old. Every sample is off-policy, and until now nothing accounted for it.
     */
    static constexpr float PPO_CLIP = 0.2f;

    /**
     * Weight on the entropy bonus, which pays the policy for staying undecided.
     *
     * RAISED TO 0.03 ON A HUNCH AND PUT BACK BY AN EXPERIMENT. Both arms were
     * trained from one checkpoint, on one binary, with a pinned seed, for forty
     * minutes each, differing ONLY in this number (see ppoEntropy below, which
     * exists so that was possible), and landing within 1.6% of each other on
     * samples. Measured over six seeds on a deterministic build:
     *
     *              300 turns   400 turns
     *     0.01       1.08        1.10
     *     0.03       0.78        0.77
     *     paired    -0.31       -0.34     both 95% CI excluding zero
     *
     * WHY it lost is the useful part. At 0.03 the war module issued MORE
     * attacks (98.7 against 74.0 per map) and won FEWER provinces with them
     * (102.7 against 184.7) -- 1.04 provinces per attack against 2.50. The
     * extra exploration bought attacks the policy had been right to decline.
     *
     * So the low attack rate this constant was raised to "fix" was not the
     * pathology it looked like. Selectivity is where the model beats the random
     * control: it attacks about a ninth as often and takes twice as much ground
     * per attempt. An entropy bonus large enough to flatten that preference
     * flattens the edge with it. The module was not refusing to fight; it was
     * undertrained, and 1.77M further samples at 0.01 moved it from 0.37 to
     * 1.10 -- the first time this project has measured play above the control.
     *
     * Small on purpose, then, and now for a measured reason rather than a
     * stated one: enough that a distribution cannot collapse to a point, not
     * enough to argue with what the policy has learned.
     */
    static constexpr float PPO_ENTROPY = 0.01f;

    /**
     * What ending a war is worth, on top of whatever ground it won.
     *
     * Nothing scored CONCLUDING a war. Conquest paid +2.0 x tanh(dProv/3) and
     * the phoney-war charge was zero, so a war that was neither won nor ended
     * cost nothing at all -- and it behaved accordingly: ceasefires
     * offered 2.4 per thousand country-turns against a random control's 45.5,
     * a take rate under one percent, wars that simply never finish. A player
     * on the other side of that sees a neighbour who will not make peace at
     * any price, which reads as broken rather than as stubborn.
     *
     * Scaled by ground so that ending a war one is WINNING pays more than
     * bailing out of one it is losing -- but both pay something, because
     * cutting losses is a real decision and the reward should let the module
     * make it.
     *
     * Only for wars at least N_STEP turns old. Without that guard the cheapest
     * way to collect this is to declare a war and immediately peace out, which
     * is a worse behaviour than the one being fixed.
     */
    static constexpr float WAR_END_REWARD = 0.5f;


    /**
     * How much army counts as ENOUGH, as a multiple of what is on our borders.
     *
     * The war module collapsed onto one action: measured at recruit 2392 times
     * out of 2429 offered (98.5%), with attack at 0.6%, declare war at 0.0% --
     * zero out of 1827 opportunities -- and every other action under 1%. It is
     * not undertrained. It has learned "recruit, always", and there is nothing
     * left of the distribution for training to move.
     *
     * The reward taught it that. armyTerm paid 0.3 x tanh(dArmy/40000) whenever
     * `exp.atWar || exp.threatened > 0`, and this model is at war almost
     * permanently as a DEFENDER -- so the gate that was supposed to make troops
     * conditional was true nearly every turn, and recruiting became free money
     * again. The comment above armyTerm already describes this exact failure
     * from a previous round ("recruit 14,849 times and attack 214"); the fix
     * applied then moved the threshold rather than removing the incentive.
     *
     * Sufficiency is the missing idea. Men are worth their upkeep while the
     * army cannot yet handle what is on its borders. At twice the hostile
     * strength adjacent to us, another division is not defence, it is a bill --
     * and the module holding it has no reason to keep buying instead of using
     * what it has.
     */
    static constexpr float ARMY_SUFFICIENCY = 2.0f;

    /**
     * THE PEACETIME BAR, as a fraction of the world's mean garrison density.
     *
     * Sufficiency is measured against hostile troops ADJACENT to us, which is
     * zero at peace -- so the bar fell to max(1, 0) = 1 and PHI saturated at an
     * army of two men. A standing army earned nothing while costing upkeep, so
     * recruit's only surviving signal was its price, and the policy did the
     * arithmetic: the 2026-08-06 run drove war:recruit to 0.0% and lost to the
     * scripted rung while being the most solvent model on disk. It was rich
     * because it did nothing.
     *
     * The first fix used a constant -- 200 troops per province. It never bound
     * once: countries hold ~231,000 per province, so PHI stayed at 1 and the
     * term stayed dead. A constant cannot know the scale of an army, which
     * belongs to the economy and moves whenever the economy does. Expressed
     * against the world mean instead, "ready" means holding roughly what
     * everyone else holds, and the bar tracks the economy for free.
     *
     * 0.5 IS NOT ARBITRARY -- IT IS THE LARGEST VALUE THAT CAN BE SATISFIED.
     * The bar is ARMY_SUFFICIENCY * PARITY * worldMean, so reaching it takes
     * (2 * PARITY) times the world's mean garrison density. At 0.75 that is
     * 1.5x the mean, which every country cannot hold at once by definition:
     * the target would recede as fast as anyone chased it, PHI would sit under
     * 1 forever, and recruit would become free money again -- the exact failure
     * that killed the third shape. At 0.5 the bar IS the mean, so a country of
     * average density is exactly sufficient, a weak one has real gradient to
     * catch up, and a strong one earns nothing more. Measured at 0.75: 19 of 21
     * countries below the bar. Do not raise this above 0.5.
     *
     * Does NOT revive the "recruit is free money" failure that killed the third
     * shape: PHI is still clamped at 1, so every man past sufficiency is worth
     * exactly nothing.
     */
    static constexpr double PEACETIME_PARITY = 0.5;

    /**
     * HOW MANY THINGS A MODULE MAY DO IN ONE TURN.
     *
     * See Experience::extras for why this is not 1. The module keeps picking
     * until it picks its pass action (0, always valid in every mask) or hits
     * this cap, and the validity mask is recomputed between picks -- so money
     * already spent, garrisons already moved and orders already queued are all
     * visible to the next choice. The mask is therefore the real budget; this
     * is only a ceiling on how much a single country can do in a turn.
     *
     * 3 rather than something larger because every action is also a training
     * sample: the cap multiplies experience volume per turn, and the point is
     * to let the policy express a small portfolio, not to let one country run
     * away with the map.
     */
    static constexpr int ACTIONS_PER_MODULE_PER_TURN = 3;

    /**
     * WHAT BEING TOO WEAK FOR YOUR OWN BORDERS COSTS, per window.
     *
     * Fourth shape this term has had. The first three each produced a policy
     * that had stopped choosing, and each failure said what the next one needed:
     *
     *   ANNUITY. 0.3 x tanh(dArmy) every window the gate was open -- riskless,
     *     repeating, and the gate is open almost permanently for a country at
     *     war a lot. Recruit on 100.000% of offers, never fight. Measured
     *     1.645x against the scripted rung, which is the awkward part: it wins.
     *   PROGRESS. 0.6 once for closing the gap, clamped. Not farmable, and
     *     worth a fraction of what one conquest pays per window, so the policy
     *     skipped the prerequisite and went to war with no army: recruit 0.0%,
     *     two assaults lost in three, 0.585x.
     *   DEFICIT. Charge the shortfall every window. A STATE penalty is paid
     *     identically whichever action you chose, so it creates no gradient
     *     BETWEEN recruiting and attacking, and left recruiting with no upside
     *     at all: recruit 0.0%, attack 100.0%.
     *
     * POTENTIAL-BASED SHAPING, which is what all three were groping at.
     *
     *     F(s, s') = PHI(s') - PHI(s),  PHI(s) = min(1, army / (2 x adjacent threat))
     *
     * PHI is a pure function of the state, so the term TELESCOPES: whatever
     * route a country takes, the total shaping it can collect over an episode
     * is PHI(end) - PHI(start), bounded by one. No annuity to farm, no gap to
     * reopen for profit. Its defining property is that it is provably
     * POLICY-INVARIANT -- it cannot change which policy is optimal, only how
     * quickly the learner finds it. After a day of reward edits that each moved
     * the optimum somewhere new, that guarantee is the reason to choose it.
     *
     * It is aimed at a measured problem rather than a suspected one. Recruiting
     * earns +0.047 of advantage against attacking's +0.338 at every horizon
     * tested; the payoff for recruiting is that it makes later attacks work;
     * and that payoff can only arrive through the bootstrap, which was measured
     * to carry no action-discriminating signal at all (baseline ~ bootstrap for
     * every action). A head-to-head of two hand-written players then confirmed
     * the target is real: one that builds an army and attacks beats one that
     * builds an army and never attacks, on every seed tried.
     *
     * WEIGHT. Closing the whole gap in one window is worth about what taking
     * two or three provinces is worth (2.0 x tanh(dProv/3) saturates near 1.5),
     * so a recruit closing a tenth of it earns roughly what one province does.
     * Below this the term is real but never competitive -- which is exactly
     * what 0.6 was.
     *
     * Nothing gates it. A country at peace has no adjacent threat, so PHI is
     * already 1 and the term is zero without being told; upkeep is priced where
     * it belongs, in income.
     */
    static constexpr float ARMY_SHAPING_WEIGHT = 2.0f;

    /**
     * Turns a decision waits for its reward, overridable with OD_N_STEP.
     *
     * This is the bias/variance dial, and it has been a single hardcoded point
     * for its whole life. GAE(lambda) is the usual way to make it adjustable,
     * and it does not drop into this design: rewards here are computed ONCE per
     * window from aggregate deltas, deliberately, so a build queued on turn one
     * is scored against what it produced by turn twelve. GAE needs a per-turn
     * reward and a per-turn value to form TD errors, so adopting it means
     * rewriting the reward as per-step -- which would discard the shaping that
     * currently measures 1.10 against the control.
     *
     * Exposing the horizon gives the same knob honestly: sweep 6 / 12 / 24 with
     * tools/ai_bench.py, which can now resolve differences this size, and let
     * the measurement pick. If a shorter or longer window wins clearly, that is
     * also the evidence needed to justify the larger rewrite.
     */
    static int nStep() {
        static const int v = [] {
            if (const char* e = std::getenv("OD_N_STEP")) {
                const int n = std::atoi(e);
                if (n >= 2 && n <= 64) {
                    printf("[AI] N_STEP overridden: %d (was %d)\n", n, N_STEP);
                    return n;
                }
                printf("[AI] OD_N_STEP=%s ignored (want 2..64)\n", e);
            }
            return (int)N_STEP;
        }();
        return v;
    }

    /**
     * PPO_ENTROPY, overridable at runtime with OD_PPO_ENTROPY.
     *
     * Exists so a controlled experiment can run two arms from ONE binary. The
     * alternative -- building twice -- makes every other difference between
     * those two builds a candidate explanation for whatever the arms disagree
     * about, which is exactly the confound that made the first attempt at this
     * comparison unreadable (it measured a binary an hour out of date).
     *
     * Read once and cached: this is on the per-sample update path, and a getenv
     * per gradient would be absurd.
     */
    static float ppoEntropy() {
        static const float v = [] {
            if (const char* e = std::getenv("OD_PPO_ENTROPY")) {
                const float f = (float)std::atof(e);
                if (f >= 0.0f && f < 1.0f) {
                    printf("[AI] PPO_ENTROPY overridden: %.4f (was %.4f)\n", f, PPO_ENTROPY);
                    return f;
                }
                printf("[AI] OD_PPO_ENTROPY=%s ignored (want 0 <= x < 1)\n", e);
            }
            return PPO_ENTROPY;
        }();
        return v;
    }

    /** How many past selves the league keeps. Oldest is overwritten. */
    static constexpr int   LEAGUE_CHECKPOINTS = 6;
    /** Updates between checkpoints. Roughly an hour of a four-worker run. */
    static constexpr uint64_t LEAGUE_CHECKPOINT_EVERY = 3000000;
    /**
     * Share of countries on a map handed a frozen past self.
     *
     * A third: enough that the learner meets a stationary opponent often, few
     * enough that most of the world is still the policy playing itself, which
     * is where the volume of experience comes from. At 1.0 there would be no
     * learning signal at all -- league countries do not teach.
     */
    static constexpr float LEAGUE_SHARE = 0.33f;

    struct Experience {
        std::vector<float> features;
        // +1 = diplo slot, +2 = stance slot
        int action[MOD_COUNT + 2] = {-1, -1, -1, -1, -1, -1};
        bool acted[MOD_COUNT + 2] = {false, false, false, false, false, false};
        // log pi(a|s) as the policy stood when the action was chosen. PPO's
        // ratio is measured against this; see accumulatePPOInto.
        float logProb[MOD_COUNT + 2] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        // Which neighbour the target head picked this turn, and what it was
        // shown to pick from. Empty unless a war was actually declared.
        std::vector<std::vector<float>> targetCand;
        // The assault chosen this turn and what it was chosen from. Empty
        // unless the war module actually attacked. Judged by the war module's
        // reward, like the declaration: choosing where to push and choosing
        // whether to push are the same decision with the same consequence.
        std::vector<std::vector<float>> attackCand;
        int attackChosen = -1;
        std::vector<std::vector<float>> relCand;
        /**
         * WHAT THE DIPLOMACY HEAD ACTUALLY SAW, and the neighbour rows that
         * went with it. Empty unless this window carries a diplomatic answer.
         *
         * `features` above is the country's own turn state, in which slots
         * 80-84 and 88-94 are ZERO -- they are reserved for request context and
         * only decideDiplomacy writes them: who is asking, for what, at what
         * odds, on what terms. Training the head on `features` therefore showed
         * it a world with no request in it. It could not distinguish a
         * ceasefire from a call to arms, because by the time it was updated the
         * difference had been erased, and it was fitted on an input it never
         * receives at decision time.
         *
         * Kept SEPARATE rather than written into `features`, because the same
         * Experience trains the four module heads and the stance, and their
         * states genuinely do not contain a request.
         */
        std::vector<float> diploFeatures;
        std::vector<std::vector<float>> diploRelCand;
        int targetChosen = -1;
        /**
         * WHERE THIS COUNTRY STOOD when the decision was taken.
         *
         * Every other snapshot here is a STOCK -- provinces, treasury, army --
         * and the reward built from them measures a dashboard. A country can
         * hold its numbers perfectly still while the leader doubles, and
         * nothing in the dense reward notices, because nothing in it mentions
         * anybody else. The only competitive signal was the terminal, which
         * arrives once per map after hundreds of windows of shaping.
         *
         * These are the same quantities the value head already sees in features
         * 104-109; what was missing was being PAID for changing them.
         */
        /**
         * Hostile troops on our borders when the window opened.
         *
         * The bar armyTerm measures progress against, frozen at the start so it
         * measures OURS. Taken from the live figure instead, a neighbour's
         * mobilisation would read as this country losing ground it never held,
         * and the war module would be charged for a decision somebody else made.
         */
        long long enemyAdjArmy = 0;
        float worldRank  = 0.0f;   // land percentile among the living, 0..1
        float ownShare   = 0.0f;   // our share of all owned land
        float rivalShare = 0.0f;   // the strongest OTHER country's share
        int age = 0;        // turns since the decision
        int rebellions = 0; // rebellions suffered within the window
        // Troops put on a hostile shore within the window. The province a
        // landing wins may fall well outside N_STEP turns, so without this the
        // decision that mounted the invasion is scored on the crossing alone —
        // an army removed from the map and a bill for the hulls.
        int landings = 0;
        long long crewDrowned = 0;   // enemy troops sent to the bottom
        long long crewLost = 0;      // our own, lost with a loaded transport
        /** Hulls bought and paid off inside the window. See m_shipsBoughtThisTurn. */
        int shipsBought = 0;
        int shipsSold = 0;
        // Turns spent with an empty treasury inside the window. Bankruptcy is
        // now expensive in the game (BANKRUPTCY_UNREST_PCT) and has to be
        // expensive in the reward too, or the modules keep spending and let the
        // austerity reflex clean up after them.
        int bankruptTurns = 0;
        // Activations cached at decision time so the learning step can skip
        // the policy re-forward (~1/3 of the per-country net cost).
        std::vector<std::vector<float>> acts[MOD_COUNT];
        /**
         * THE SECOND AND THIRD THING A MODULE DID THIS TURN.
         *
         * Each module used to take exactly one action per turn, which forced
         * the policy to RANK actions that are complements rather than
         * substitutes: a player does not choose between recruiting and
         * attacking, they recruit and reinforce and attack in the same turn
         * until the money runs out. With one slot, the only way to express
         * "recruiting matters" is to take nearly all of the mass, so there was
         * no stable interior optimum and the war head slid between corners --
         * recruit 100% then recruit 0%, attack 100% then 20%. Five successive
         * shapes of armyTerm failed to fix that, because no shaping of a
         * one-of-N choice can encode "do both".
         *
         * The tell was that the BEST model on disk (candidate-t2h, ADVANTAGE
         * 2.35/2.74) fails the recruit shape gate at ~90%, while the model that
         * sat inside the band scored 0.55. The metric was describing a world
         * where these actions compete; they do not.
         *
         * The first action of each module stays in the scalar fields above so
         * every existing path -- diplo and stance slots, the decision trace,
         * the target and attack heads -- is untouched. Anything after the first
         * lands here and becomes its own WorkItem sharing the window's reward.
         */
        struct ExtraAction {
            int module = -1;
            int action = -1;
            float logProb = 0.0f;
            std::vector<std::vector<float>> acts;
            /**
             * THE STATE AS IT WAS WHEN *THIS* ACTION WAS CHOSEN.
             *
             * The turn's features are a snapshot taken before the module acted,
             * so reusing them for a second or third pick would both choose and
             * train on a world where the first pick's money had not been spent
             * and its troops had not moved. It also makes the value baseline
             * identical for every action in the turn -- which is precisely the
             * credit-assignment failure that showed up as declare war and
             * ceasefire collapsing to zero while the frequent cheap actions
             * spread out: a rare, expensive decision was being scored against
             * the same baseline as the 35 routine ones around it, so its
             * advantage was indistinguishable from theirs.
             *
             * With its own features each action gets its own V(s), so the
             * advantage R - V(s_k) differs per action even though they share
             * the window's return -- which is correct, because they genuinely
             * do share the future.
             */
            std::vector<float> features;
        };
        std::vector<ExtraAction> extras;
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
        // How long that war had already been running when the decision was
        // taken, in turns. The phoney-war charge is for STALLING, and a war
        // that started this turn has not had the chance yet: mobilising,
        // marching and winning the first battle do not fit inside one window.
        // Charging it from turn one is what made the total price of a
        // declaration exceed the flat penalty it was supposed to replace.
        int warTurns = 0;
        // Was there a war at ANY point in the window, rather than only at the
        // snapshot? `atWar` is read before the war module acts, so the window a
        // country declares war in has atWar == false — and a war-module
        // idleness charge keyed on the snapshot would fire on precisely the
        // turn the module did the most decisive thing it can do.
        bool warInWindow = false;
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
    /** Shared encoder: FEATURE_COUNT -> 512 -> TRUNK_OUT, tanh on the output
     *  because that layer is a HIDDEN layer of the nets it replaced. */
    NeuralNet m_trunk;
    /** Heads now, not whole nets: TRUNK_OUT -> actions. */
    NeuralNet m_relEncoder;
    NeuralNet m_relScore;
    NeuralNet m_stanceHead;   // TRUNK_OUT -> STANCE_COUNT; see STANCE_COUNT
    NeuralNet m_policy[MOD_COUNT];
    NeuralNet m_value[MOD_COUNT];
    /**
     * Q(s,a): what each action in this state turned out to be worth.
     *
     * The policy is trained by REINFORCE, which nudges the logit of whatever
     * was sampled by its advantage. That is a slow and noisy way to discover
     * that one action is better than another, and at decision time the net has
     * nothing to say about the actions it did NOT sample.
     *
     * A value head cannot fill that gap: V(s) scores the STATE, so it is the
     * same number whichever action is being considered. Ranking actions needs a
     * per-action estimate, and without a forward model -- the game cannot cheaply
     * answer "what would the world look like if I declared war on France" --
     * that estimate has to be learned rather than searched for.
     *
     * So Q is trained on the same bootstrapped target the value head gets, but
     * written to the taken action's output alone, and at decision time it is
     * blended into the policy's logits. That makes action selection a
     * policy-improvement step over the actor instead of a straight sample from
     * it, which is the model-free half of what a one-ply search would buy.
     */
    NeuralNet m_q[MOD_COUNT];
    /**
     * WHOM to attack, as a learned choice rather than a rule.
     *
     * The war module decides only whether to declare; findWarTarget decided on
     * whom, by a fixed rule -- prefer a neighbour holding land we claim, then
     * whoever has the smallest army. That rule is most of the strategy. It
     * cannot weigh a weak neighbour who is someone's ally against a stronger
     * one who is already fighting two wars, because it looks at one number.
     *
     * That also bounds what the benchmark can ever show. ADVANTAGE measures
     * only what the LEARNED decisions add, and both cohorts share this rule, so
     * however good the policy gets, the choice that decides the war is made
     * identically for the trained AI and for the control.
     *
     * So the rule now produces CANDIDATES -- it keeps every gate, every army
     * bar, every restraint -- and this net scores them. One forward pass per
     * candidate, softmax across the scores, sampled. The policy chooses its
     * war; the heuristics still say which wars are allowed to be chosen.
     */
    NeuralNet m_target;
    /**
     * The scoring inputs and the pick, from the moment execWar chose, waiting
     * for takeTurn to attach them to this turn's Experience. Set and consumed
     * within a single country's turn, so one slot is enough.
     */
    /** log pi of the last diplomatic answer, handed to the Experience below. */
    float m_lastDiploLogProb = 0.0f;
    std::vector<std::vector<float>> m_pendingTargetCand;
    int m_pendingTargetChosen = -1;
    /** Where to attack, scored the same way. See ATTACK_FEATURES. */
    NeuralNet m_attack;
    std::vector<std::vector<float>> m_pendingAttackCand;
    int m_pendingAttackChosen = -1;
    NeuralNet m_diplo;
    /**
     * The baseline the diplomacy head never had.
     *
     * Every other module subtracts V(s) from its target; the diplomacy head was
     * trained on the raw normalised reward, which is pure REINFORCE with no
     * variance reduction at all -- and it decides the one thing in the game
     * with the longest gap between cost and payoff. Answering a call to arms
     * charges seven points of unrest on the spot; the war it wins pays out
     * thirty turns later, outside a twelve-turn window it also could not see
     * past, because it was the only head with no bootstrap either.
     *
     * Given that, refusing every call was the correct answer to the only
     * reward it could perceive: a refusal costs one alliance, an acceptance
     * costs unrest now and risks provinces, and the upside was invisible. It
     * converged there over 183M updates, and the measured result was calls
     * answered 0, refused 181.
     */
    NeuralNet m_diploValue;
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
    // cid -> the turn its current, unbroken run of being at war began. Erased
    // the moment it is at peace, so the entry always describes ONE war period
    // and Experience::warTurns is its age. Maintained in beginTurn only:
    // refreshStats runs twice a turn and counting there would double.
    std::unordered_map<int, int> m_warSince;
    // cid -> sliding window of decisions awaiting their N_STEP reward
    std::unordered_map<int, std::deque<Experience>> m_pending;
    // Overture cooldown, keyed on the UNORDERED pair. An ordered key let A and
    // B alternate proposals to each other every single turn, so the pair never
    // actually cooled down.
    std::unordered_map<long long, int> m_diploCooldownUntil;
    // Per-country budget. The pair cooldown alone still let a country with a
    // dozen neighbours fire one overture every turn for a dozen turns straight
    // — that, not the per-pair rate, is what flooded the log.
    std::unordered_map<int, int> m_diploNextTurn;
    // Per-RECIPIENT budget, and the one that was missing.
    //
    // The two above throttle a proposer: a pair may talk every 25 turns, and
    // any one country may open its mouth every 5. Neither says anything about
    // how often a country is ASKED. With a hundred and eighty-five countries
    // in play, twenty neighbours each behaving perfectly still lands an offer
    // on the same doorstep several times a turn, which is what being on the
    // receiving end actually feels like -- and the player, who has to answer
    // every one with a modal popup, feels it hardest.
    //
    // Symmetrical with m_diploNextTurn on purpose. It applies to every country
    // equally rather than special-casing the player, because an AI drowning in
    // offers it must evaluate is the same waste of turns.
    std::unordered_map<int, int> m_diploNextIncoming;
    // Gold-denominated value of trades resolved since this country's last
    // reward window. See noteTradeOutcome.
    std::unordered_map<int, float> m_tradeOutcome;
    static long long diploKey(int a, int b) {
        int lo = a < b ? a : b, hi = a < b ? b : a;
        return ((long long)lo << 24) | (long long)hi;
    }
    bool diploBudgetReady(int cid) const {
        auto it = m_diploNextTurn.find(cid);
        return it == m_diploNextTurn.end() || m_turn >= it->second;
    }
    /** Has this country been left alone long enough to be asked again? */
    bool diploIncomingReady(int targetCid) const {
        auto it = m_diploNextIncoming.find(targetCid);
        return it == m_diploNextIncoming.end() || m_turn >= it->second;
    }
    bool diploReady(int sourceCid, int targetCid) const {
        if (!diploBudgetReady(sourceCid)) return false;
        if (!diploIncomingReady(targetCid)) return false;
        auto it = m_diploCooldownUntil.find(diploKey(sourceCid, targetCid));
        return it == m_diploCooldownUntil.end() || m_turn >= it->second;
    }
    void diploCoolDown(int sourceCid, int targetCid) {
        m_diploCooldownUntil[diploKey(sourceCid, targetCid)] = m_turn + 25;
        m_diploNextTurn[sourceCid] = m_turn + 5;
        // Four turns of quiet for whoever was just asked. Deliberately shorter
        // than the proposer's own budget: this is meant to stop a queue
        // forming at one country's door, not to stop the world from talking.
        m_diploNextIncoming[targetCid] = m_turn + 4;
    }
    long long m_worldArmy = 0;
    long long m_worldProvinces = 0;
    double m_medianArmyPerProvince = 0.0;  // typical country, not the aggregate
    size_t m_worldPixels = 0;
public:
    /**
     * Mean army per province across every real country, refreshed each turn.
     *
     * The sufficiency bar is expressed against THIS rather than a constant,
     * because the absolute scale of an army is a property of the economy and
     * not of the design. Measured 2026-08-06: countries hold ~231,000 troops
     * per province, while a hand-set floor of 400 was in the reward -- 578x too
     * small, so it never bound and the term stayed dead. A world-relative bar
     * cannot drift out of range that way.
     */
    double worldArmyPerProvince() const { return m_medianArmyPerProvince; }
private:

    // Running reward normalisation (mean/var per module), so advantage scale
    // is stable across maps of very different sizes.
    float m_rMean[MOD_COUNT] = {0, 0, 0, 0};
    float m_rVar[MOD_COUNT] = {1, 1, 1, 1};
    /**
     * UPDATE TRACE, enabled with OD_UPDATE_TRACE=1.
     *
     * A healthy PPO advantage is centred near zero: it says "this action was
     * better or worse than the state was worth", and across many samples the
     * two halves cancel. A large systematic MEAN means every update is pushing
     * the policy the same direction regardless of what it did, which is how a
     * good model is walked off a good optimum in minutes. Kept per module
     * because the four are scored differently and only one may be broken.
     */
    struct UpdTrace {
        double normSum = 0, normSq = 0;      // the reward, normalised
        double baseSum = 0, baseSq = 0;      // V(s), the baseline
        double advSum = 0, advSq = 0;        // what actually drives the update
        long long n = 0, advClipped = 0, tgtClipped = 0;
    };
    UpdTrace m_upd[MOD_COUNT];
    long long m_updTraceBatches = 0;

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
        float norm = 0;      // normalised reward for THIS window
        float advantage = 0; // filled in by the worker, for the debug log
        int cid = 0;
        std::vector<float> features;
        std::vector<std::vector<float>> acts; // cached policy activations
        /**
         * The state the window ended in, and what a value there is worth.
         *
         * Without these the learner's horizon is exactly N_STEP turns: the
         * target was the window's own reward and nothing else, so anything a
         * decision caused after twelve turns was invisible to it. That is
         * shorter than a war. An invasion launched at turn 40 and won at turn
         * 70 trained as twelve turns of cost and no gain whatsoever.
         *
         * With them the target becomes reward + BOOTSTRAP_DISCOUNT * V(end of
         * window), so value propagates backwards one window at a time and the
         * effective horizon stops being a constant.
         *
         * Empty features, or a zero discount, means the window ended the
         * episode -- the country was eliminated or the map was decided -- and a
         * terminal state is worth nothing by definition.
         */
        std::vector<float> nextFeatures;
        float bootDiscount = 0.0f;
        float oldLogProb = 0.0f;   // the behaviour policy's, for PPO's ratio
        std::vector<std::vector<float>> targetCand;   // war-target choice, if any
        std::vector<std::vector<float>> attackCand;   // assault choice, if any
        std::vector<std::vector<float>> relCand;      // neighbour rows, for the encoder
        int targetChosen = -1;
        int attackChosen = -1;
    };
    std::vector<WorkItem> m_work;
    struct WorkerScratch {
        // The shared trunk's activations for the sample being learned from.
        // Every head's gradient chains back through this one.
        NeuralNet::Scratch trunk;
        NeuralNet::Scratch policy[MOD_COUNT];
        NeuralNet::Scratch value[MOD_COUNT];
        // A SECOND value scratch, for V(end of window). It cannot share the one
        // above: that holds the activations the value update backpropagates
        // through, and evaluating the bootstrap into it would overwrite them
        // with the wrong state's.
        NeuralNet::Scratch valueNext[MOD_COUNT];
        NeuralNet::Scratch q[MOD_COUNT];
        NeuralNet::Scratch target;
        NeuralNet::Scratch attack;
        NeuralNet::Scratch diplo;
        NeuralNet::Scratch diploValue;
        NeuralNet::Scratch diploValueNext;
        NeuralNet::Scratch stance;
        std::vector<NeuralNet::Scratch> relEnc, relSco;
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
    // Rebels belong to NEITHER cohort, and until they had somewhere of their
    // own to go they were counted as the model's.
    //
    // Every country takes an AI turn, rebels included (processCountryTurn
    // excludes only UNC/BLC/SPC), so a map with 113 rebellions per thousand
    // country-turns puts thousands of rebel decisions through statsFor. The
    // old two-way split sent all of them to m_trainStats, while the trainer's
    // denominator (trainedCountryTurns) counts real model countries only. The
    // result was rates that cannot physically happen: 3,371 repressions per
    // 1,000 model country-turns, when a country can repress at most once a
    // turn. Nothing reads this bucket; it exists so the other two stay clean.
    TrainStats m_rebelStats;
    /**
     * Whichever cohort `cid` belongs to. All one pool when no split is set.
     *
     * Out of line because it needs Game::REBEL_CID_MIN and Game is only
     * forward-declared here.
     */
    TrainStats& statsFor(int cid);
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
    /** World aggregates, computed once a turn -- see WORLD_FEATURES. Per-turn
     *  rather than per-country: recomputing inside buildFeatures would be
     *  O(countries^2) on a map that already carries hundreds. */
    struct WorldSnapshot {
        int turn = -1;
        float herfindahl = 0.0f;      // sum of squared land shares; 1 = one owner
        float largestShare = 0.0f;
        float atWarFrac = 0.0f;
        float aliveNorm = 0.0f;
        float meanUnrest = 0.0f;
        long long totalProvinces = 0;
        std::unordered_map<int, float> rank;   // cid -> land percentile 0..1
        // WHO the largest power is, and how big the next one is. Needed to
        // answer "how am I doing against the strongest country that is not me",
        // which largestShare alone cannot: for the leader it reads as its own
        // size, so the leader's own dominance looked like a rival's.
        int   largestCid  = -1;
        float secondShare = 0.0f;
        /** The strongest country OTHER than `cid`, as a share of owned land. */
        float rivalShareFor(int cid) const {
            return cid == largestCid ? secondShare : largestShare;
        }
        /** `cid`'s own share of owned land. */
        float shareOf(int provinces) const {
            return totalProvinces > 0 ? (float)provinces / (float)totalProvinces
                                      : 0.0f;
        }
        float rankOf(int cid) const {
            auto it = rank.find(cid);
            return it != rank.end() ? it->second : 0.0f;
        }
    };
    WorldSnapshot m_world;
    std::vector<std::vector<float>> m_lastRelCand;
    /** cid -> (stance, turn it was chosen). Held for STANCE_WINDOW turns. */
    std::unordered_map<int, std::pair<int,int>> m_stance;
    int stanceOf(int cid) const {
        auto it = m_stance.find(cid);
        return it == m_stance.end() ? -1 : it->second.first;
    }
    void updateWorld();
    void backpropRelational(WorkerScratch& ws, const WorkItem& w);
    void buildRelational(int cid, std::vector<std::vector<float>>& cand,
                         std::vector<float>& pooled);

    /** Baseline for the trend features, refreshed every TREND_WINDOW turns. */
    struct TrendPoint {
        int turn = -1;
        float provinces = 0, army = 0, industry = 0, population = 0;
        float treasury = 0, threat = 0, align = 0, weariness = 0;
    };
    std::unordered_map<int, TrendPoint> m_trend;
    /** Read the eight tracked quantities as they stand now. Pure: reads only
     *  refreshStats() output, Country fields and province armies. */
    TrendPoint sampleTrend(int cid) const;
    /** Roll expired baselines forward. Called once from beginTurn, never from
     *  buildFeatures -- features are built several times a turn and the
     *  baseline must not move underneath them. */
    void updateTrends();

    // ── The league ──────────────────────────────────────────────────────
    //
    // Every country evaluates one shared brain, so self-play here means the
    // policy plays ITSELF, always at exactly its own current strength. That is
    // the setup that cycles: the policy learns to beat what it is this hour,
    // the counter to that, then the counter to the counter, and can arrive back
    // where it started having forgotten how to handle any of it. Nothing in the
    // training loop notices, because every game still ends with a winner.
    //
    // So a fraction of countries on each map are handed a FROZEN past self
    // instead. They play, they do not learn -- the same arrangement the random
    // control group already uses -- and the learner faces opponents that do not
    // move while it is trying to beat them.
    //
    // Checkpoints are the model as it was, saved on a rotation and drawn from
    // at random. Beating last hour's policy is worth something; beating one
    // from ten hours ago and one from two hours ago in the same session is what
    // stops the cycle.
    NeuralNet m_leagueTrunk;
    NeuralNet m_leaguePolicy[MOD_COUNT];
    bool m_leagueLoaded = false;
    std::unordered_set<int> m_leagueCids;
    bool m_leagueThisCountry = false;
    /**
     * The diplomacy net of the frozen side, and whether we have one.
     *
     * A league CHECKPOINT does not carry it — the pool stores a trunk and the
     * policy heads and nothing else — so in training a frozen opponent answers
     * treaties with the CURRENT model's diplomacy net. That is a tolerable
     * approximation there, where the league exists to stop policy cycling.
     *
     * It is not tolerable under --vs-model, where the whole point is that two
     * named files play each other: leaving diplomacy shared would report the
     * opponent's "calls answered" and "ceasefires offered" as the challenger's
     * own behaviour, and those two counters are exactly what a head-to-head is
     * usually run to compare. A full model file HAS a diplomacy net, so when
     * one is loaded from a model rather than a checkpoint, the opponent gets
     * its own.
     */
    NeuralNet m_leagueDiplo;
    bool m_leagueDiploLoaded = false;
    /**
     * The frozen side's stance head, and whether we have one.
     *
     * Same split as m_leagueDiplo: a league CHECKPOINT carries no stance head,
     * so in training a frozen opponent simply holds no posture -- which cost
     * nothing while the stance only set a feature bit. Now that it steers
     * action selection (STANCE_BIAS), a side with no stance is a side playing
     * under different rules, and under --vs-model that turns a head-to-head
     * into a handicap match. A full model file has the head, so when the
     * opponent comes from one it picks its own posture.
     */
    NeuralNet m_leagueStance;
    bool m_leagueStanceLoaded = false;
    /** Set by loadOpponentModel: the control cohort is a model, not dice. */
    bool m_opponentLoaded = false;
    /**
     * Load a full model file (ODAI) as the frozen opponent: its trunk, its four
     * policy heads and its diplomacy net, into the same slots the league fills
     * from a checkpoint. Everything else in the file — value heads, Q, reward
     * statistics — describes how to KEEP LEARNING, and a frozen opponent does
     * not. False, with a reason on stderr, if the file is missing or is not a
     * model this build can read.
     */
    bool loadOpponentModel(const std::string& path);
    /**
     * STATIC, because this object does not live long enough to hold it.
     *
     * AISystem is destroyed and rebuilt on every map rotation, so a member
     * resets to zero several times an hour -- the interval gate then always
     * passes, every map writes a checkpoint, and because the slot is derived
     * from an update count that has barely moved they all land in the SAME
     * slot. The pool stays one file deep and the league has one opponent,
     * which is the situation it exists to fix. A worker is one process for its
     * whole life, so process lifetime is the right scope.
     */
    static uint64_t s_lastCheckpointUpdates;
    /**
     * PRIORITISED FICTITIOUS SELF-PLAY.
     *
     * The league picked its opponent uniformly, which spends most of its games
     * against checkpoints the current policy already beats comfortably -- and a
     * win you were always going to get teaches nothing. PFSP weights the draw
     * towards the opponents that actually trouble us: w = (1 - winrate)^2 + eps,
     * so a slot we lose to is sampled far more often than one we crush, and the
     * epsilon keeps every slot reachable so a beaten opponent can be re-checked
     * as the policy drifts.
     *
     * Static because the AISystem is destroyed and rebuilt on every map
     * rotation; per-instance counters would reset before they meant anything.
     */
    static int s_leagueGames[LEAGUE_CHECKPOINTS];
    static int s_leagueLosses[LEAGUE_CHECKPOINTS];   // maps where the frozen side held more land
    /** Which slot the current map is playing against, or -1. */
    int m_leagueSlot = -1;
    /** Score the finished map against the opponent that played it. */
    void recordLeagueOutcome();

    /** Save the current policy into the checkpoint pool, oldest slot first. */
    void writeLeagueCheckpoint();
    /** Load a random checkpoint as this map's opponent. False if none exist. */
    bool loadLeagueOpponent();
    /** Choose which countries the frozen opponent plays, for a fresh map. */
    void assignLeagueCountries();
    bool m_randomThisCountry = false;
    /** This country is playing the scripted rung. See s_scriptedControl. */
    bool m_scriptedThisCountry = false;

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
    /** The tier in force, or the top one while self-play is learning. */
    const DifficultyProfile& difficulty() const;
public:
    /**
     * Gradient updates behind one module's policy head.
     *
     * Reported by the eval because the reward-term gates cannot be read without
     * it. A take rate pinned to one action means a COLLAPSED distribution when
     * the head has millions of updates behind it, and means nothing at all when
     * it has just been reset -- a fresh head has arbitrary logits and at a
     * temperature of 0.35 will concentrate on whichever one initialisation
     * happened to favour. Those two look identical in the take rate and need
     * opposite responses, so the gate has to be told which it is looking at.
     */
    unsigned long long moduleUpdates(int m) const {
        return (m >= 0 && m < MOD_COUNT) ? m_policy[m].updateCount() : 0;
    }
private:

    // `graveAction`, when >= 0, names an action that epsilon-random exploration
    // must not fire during normal play. See the note in pickAction.
    // `logitBias`, when given, is added to the logits before masking and
    // sampling — a standing preference the caller wants applied to this
    // decision without teaching the net anything (the learning step uses the
    // net's own unbiased activations). See AI_CALL_RELUCTANCE.
    /**
     * `logProbOut`, when given, receives log pi(a|s) under the MASKED logits at
     * temperature 1 -- the probability the policy being trained assigned to the
     * action, not the probability the exploration actually used.
     *
     * That is the quantity PPO's ratio needs: it measures how far the policy
     * has moved since the decision, and an epsilon-random pick is simply a very
     * off-policy sample, which is precisely what the clip exists to bound.
     */
    int  pickAction(NeuralNet& net, const std::vector<float>& feats,
                    const std::vector<bool>& valid, float& scoreOut,
                    int graveAction = -1,
                    const std::vector<float>* logitBias = nullptr,
                    float* logProbOut = nullptr,
                    // The policy's own distribution over the MASKED logits at a
                    // neutral temperature of 1.0, for TrainStats::warProbMass.
                    // Purely observational: filled after the choice is made and
                    // never read back, so it cannot affect what was chosen.
                    std::vector<float>* neutralProbsOut = nullptr);
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
    /**
     * NAVAL COMBAT, IN THE ONLY UNITS THAT MATTER: men.
     *
     * Keyed by country, cleared each turn, folded into the open windows in
     * endTurn exactly as landings are. Crew rather than hulls because the hull
     * is not the prize -- sinking a loaded transport kills the invasion it
     * carries, and losing one deletes those troops from your own land army
     * outright. Both were worth precisely zero to the module responsible.
     */
    std::unordered_map<int, long long> m_crewDrownedThisTurn;   // we sank theirs
    std::unordered_map<int, long long> m_crewLostThisTurn;      // they sank ours
    /**
     * Hulls bought and hulls paid off this turn, per country.
     *
     * dShips -- the fleet's net change -- cannot attribute either decision,
     * because the ECONOMY module buys ships and the NAVY module scraps them,
     * and one number covering both credits each for what the other did. It also
     * sat entirely in the navy's reward, so the module that spent the treasury
     * on a fleet was never told whether the fleet was worth having, and the
     * module that was told could not build one.
     */
    std::unordered_map<int, int> m_shipsBoughtThisTurn;
    std::unordered_map<int, int> m_shipsScrappedThisTurn;

    // Action execution (mirrors player enqueue rules incl. treasury deduction)
    std::string execEconomy(int cid, int action);
    std::string execPolitics(int cid, int action);
    std::string execWar(int cid, int action);
    std::string execNavy(int cid, int action);
    /**
     * Where a hull should actually steer to reach (tLon, tLat).
     *
     * Follows the sea route and returns the furthest waypoint that is both in
     * range and reachable in a straight line, so open water is crossed in one
     * leg and coastlines are rounded rather than run into. Falls back to the
     * target itself when no route exists, which is what the open-sea case
     * wants anyway.
     *
     * Used by BOTH the navy action and amphibiousReflex. Routing only the navy
     * action moved the stall rate from 93% to 91%, because the reflex runs
     * every turn for every country and issues most of the move orders.
     */
    void aimAlongRoute(const NavyShip& s, double tLon, double tLat,
                       double& aimLon, double& aimLat) const;
    void validEconomy(int cid, std::vector<bool>& out);
    void validPolitics(int cid, std::vector<bool>& out);
    void validWar(int cid, std::vector<bool>& out);
    void validNavy(int cid, std::vector<bool>& out);

    /**
     * The declaration this country would actually issue, if any.
     *
     * ONE function so the mask and the executor cannot disagree.
     *
     * They did, badly. validWar offered "declare war" whenever the country had
     * any non-friendly neighbour and any army at all, while execWar refused
     * unless it was under the concurrent-war cap, under the weariness cap, and
     * holding AI_WAR_BAR_* times the target's army — so the action was offered
     * constantly and answered "war: no suitable target". Every one of those was
     * a recorded decision with a gradient behind it, teaching the war head that
     * declaring war is a no-op. This is the same mask/executor mismatch that
     * had the politics head choosing "repress" into "already hardest" forever;
     * it is fixed the same way, by making the mask ask the executor.
     *
     * Returns false when no declaration is possible. `out` is untouched then.
     */
    struct WarTarget {
        int cid = -1;           // who to declare on
        bool claimed = false;   // they hold land we claim: a war of reconquest
        bool naval = false;     // overseas, reached by sea rather than a border
        bool napBlocked = false;// a pact stands and must be broken first
    };
    /** One neighbour that has cleared every gate and bar. */
    struct WarCandidate {
        int cid = -1;
        bool claimed = false;
        bool naval = false;
        bool napBlocked = false;
        long long army = 0;
    };
    /**
     * `learnedChoice` picks among the candidates with the target head and
     * SAMPLES; false applies the old deterministic rule.
     *
     * The mask must pass false and the executor true. Both call this so they
     * cannot disagree about whether a war is possible -- a bug this code has
     * had before -- but if the mask sampled too, the two calls would draw
     * different targets and the mask would be describing a war that is not the
     * one about to be declared.
     */
    /**
     * Wars against OTHER COUNTRIES. Rebellions do not count.
     *
     * Both war gates counted every relation with war set, and putting down your
     * own rebels is one of those: m_relations[parent][rebel].war is how a
     * revolt is expressed. So a country with a single active rebellion read as
     * "already fighting a war" and, with AI_MAX_CONCURRENT_WARS at 1, could
     * never declare a foreign one at all. Rebellions run at 15 to 43 per
     * thousand country-turns, so most countries were gated most of the time.
     *
     * That is not what either constant is for. AI_MAX_CONCURRENT_WARS says one
     * war of one's own CHOOSING at a time, and its own comment says so; a
     * revolt is not chosen, it is suffered.
     */
    int  foreignWarCount(int cid) const;
    bool findWarTarget(int cid, WarTarget& out, bool learnedChoice = false);
    /** Score the candidates and sample one. -1 to fall back to the rule. */
    int  chooseWarTarget(int cid, const std::vector<WarCandidate>& cands);
    /** The candidate's own slice of the target head's input. */
    void buildTargetFeatures(int cid, const WarCandidate& cand,
                             std::vector<float>& out) const;
    /**
     * Score the winnable assaults and pick one, or -1 to let the old rule
     * decide. Records the candidate inputs either way -- see
     * ATTACK_WARMUP_UPDATES for why that is not optional.
     */
    int  chooseAttack(int cid, const std::vector<AttackCandidate>& cands,
                      std::vector<int>* rankingOut = nullptr);
    /**
     * The scripted opponent's move for one module. See s_scriptedControl.
     *
     * Returns an index into that module's action list, always one the mask
     * allows -- the preference list is consulted in order and the first
     * permitted entry wins, so the rules never have to re-check the conditions
     * the masks already enforce.
     */
    int  scriptedChoice(int module, int cid, const std::vector<bool>& valid,
                        int variant = SCRIPT_AGGRESSOR) const;
    /** Whether the scripted opponent accepts a request. */
    bool scriptedDiplomacy(int targetCid, const std::string& action,
                           const std::string& sourceIso) const;
    /** The candidate's own slice of the attack head's input. */
    void buildAttackFeatures(int cid, const AttackCandidate& cand,
                             std::vector<float>& out) const;
    /**
     * How likely `partnerCid` is to accept `requestKind`, from 0 to 1.
     *
     * OPPONENT MODELLING, and unusually cheap here. Every country evaluates the
     * same weights, so the policy that will answer this request IS the policy
     * asking the question -- running the diplomacy net on the partner's own
     * features, with the same request bias answerDiplomacy applies, is not an
     * approximation of their behaviour. It is their behaviour, evaluated early.
     *
     * The politics module proposed to the STRONGEST neighbour and nothing else,
     * so it spent its turns asking the countries least likely to say yes, and
     * every refusal is a turn and a cooldown for nothing.
     */
    // `askerCid` is who would be doing the asking -- needed because the answer
    // depends on what THEIR word is worth to `partnerCid`. Without it the
    // planner models a country everyone trusts, and a serial liar keeps
    // spending its overture budget on partners who have stopped believing it.
    float predictAcceptance(int partnerCid, const char* requestKind,
                            int askerCid) const;

    /**
     * What a resolved trade was actually worth to this country, in gold.
     *
     * Positive when more value arrived than left. Consumed once by the next
     * reward window (see where diploReward is assembled) and then cleared, so a
     * single deal is credited to the decision that made it and to nothing else.
     *
     * This exists because the diplomacy head had no way to feel a bad trade.
     * Every non-terminal decision was rewarded from the same global signal, so
     * accepting a ruinous offer and refusing a generous one produced identical
     * learning -- which is why the accept rate sat at 95.0%, 95.3% and 95.5%
     * across a flat price, an income-based price and a corrected valuation.
     * None of those told the net anything; this does.
     */

    bool loadModel();
};
