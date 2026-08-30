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
/// Only ever held by pointer here; the definition lives in GameStructs.h.
struct Policy;

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
    /// The widest module head, for arrays indexed by [module][action].
    static constexpr int MAX_MODULE_ACTIONS = 12;
    static_assert(ECON_ACTIONS <= MAX_MODULE_ACTIONS &&
                  POL_ACTIONS  <= MAX_MODULE_ACTIONS &&
                  WAR_ACTIONS  <= MAX_MODULE_ACTIONS &&
                  NAVY_ACTIONS <= MAX_MODULE_ACTIONS,
                  "MAX_MODULE_ACTIONS must cover every module head");
    static constexpr int DIPLO_ACTIONS = 2; // 0=reject 1=accept
    /** One-hot width for (module, action) on the dynamics head's input. */
    static constexpr int DYN_ACTION_ONEHOT = MOD_COUNT * MAX_MODULE_ACTIONS;
    /**
     * Updates the dynamics head needs before search is allowed to influence
     * anything. Same arrangement Q and the attack head got: an untrained model
     * is noise, and blending noise into a policy with hundreds of millions of
     * updates behind it would make the shipped AI worse the moment it shipped.
     * Until this is cleared, searchDepth is ignored and play is bit-identical
     * to a build without any of this.
     */
    static constexpr unsigned long long DYN_WARMUP_UPDATES = 200000ULL;
    /** How loudly the search is allowed to argue with the policy, like Q_BLEND. */
    static constexpr float SEARCH_BLEND = 0.7f;
    /**
     * ── AND THE SEARCH IS OFF, BECAUSE Q ALREADY KNOWS WHAT IT FINDS ──
     *
     * Measured at Insane on a FROZEN model (a live worker file is rewritten
     * every sixty seconds and the first attempt at this A/B compared two
     * different models without noticing), everything else held equal:
     *
     *                       depth 0   depth 2
     *     land vs script      1.94x     1.69x
     *     think time         0.338ms   2.881ms
     *
     * Eight and a half times the thinking for a quarter less land. The
     * machinery is correct -- the forward model trains, the file migrates, the
     * beam does what it says -- and the idea is still wrong in this shape, for
     * a reason worth writing down.
     *
     * A ply is scored by predicting the next embedding and reading what that
     * position is worth. The only head that can read an embedding is Q, so the
     * score is max_a' Q(g(s, a), a'). But Q(s, a) ALREADY estimates the
     * discounted value of taking a: it was trained on exactly that target. So
     * the search re-derives a quantity the critic already holds, through a
     * learned dynamics model, and the only thing it can contribute over Q
     * directly is the model's own error. Strictly noisier, by construction.
     *
     * (The first version was worse and for a duller reason: it scored with
     * m_value, which takes the 143-float FEATURE vector, while the dynamics
     * head emits a 320-float embedding. NeuralNet::forward returns an empty
     * vector on a size mismatch instead of complaining, so every score was
     * exactly zero -- a search that cost seven times the thinking and could not
     * express a preference. See embeddingValue.)
     *
     * Search is worth revisiting when it can do something Q cannot. Two
     * candidates, both real: a horizon LONGER than Q's -- unrolling four or
     * five plies, where a bootstrapped one-step critic genuinely runs out --
     * and search over the OPPONENT's replies rather than one's own follow-ups,
     * which is the half a single-agent value function cannot represent at all.
     * Neither is a tuning change; both are the next attempt rather than this
     * one retuned.
     */

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
    /**
     * WHAT EACH RUNG OF THE LADDER CAN DO -- not how much noise it plays with.
     *
     * The first two fields were, for a long time, the whole ladder: easy to
     * insane was a temperature slide from 1.60 to 0.05 and an epsilon slide
     * from 0.08 to 0.00. Insane was therefore the SAME policy with the
     * randomness switched off. It knew nothing more, saw nothing more and could
     * do nothing more than Easy, and against a human who reloads a save it is
     * the most exploitable setting in the game: argmax answers a position the
     * same way every time, so it has to be solved once and never again.
     *
     * The three booleans were the beginning of the right idea -- a rung should
     * GRANT something. These finish it: the top rungs get a wider action
     * budget, a coalition against whoever is running away with the map, and a
     * search over their own value function. A player cannot feel a faculty as
     * unfair the way they feel a resource bonus, because every one of them is
     * something a good human already does.
     */
    struct DifficultyProfile {
        float temperature;
        float epsilon;
        bool  useCritic;
        bool  useLearnedAim;
        bool  usePosture;
        /**
         * How much of the size-scaled action budget this rung is allowed. 0
         * pins every country to ACTIONS_PER_MODULE_PER_TURN whatever it holds;
         * 1 gives it the full scaling. See actionsPerModule for why a flat
         * budget is a cap on competence rather than a difficulty.
         */
        float actionScale;
        /**
         * Whether this rung's countries gang up on the leader. See
         * COALITION_SHARE: the thing that makes a strategy game hard is not
         * each opponent being clever, it is several of them deciding you are
         * the problem.
         */
        bool  useCoalition;
        /**
         * How many plies of lookahead over the learned dynamics model. 0 is the
         * model-free Q blend every rung above Easy already had. See
         * searchDepth() and the dynamics head.
         */
        int   searchDepth;
    };
    /**
     * Four rungs a player can pick, and a fifth row that only self-play uses.
     *
     * TRAINING IS NOT A DIFFICULTY. It used to borrow the Insane row, which was
     * fine while a rung was only a temperature -- but a rung now GRANTS things,
     * and the two purposes pull apart the moment one of those grants is
     * something the current policy cannot yet use. The wider action budget is
     * exactly that: self-play must train under it or no model will ever learn
     * to use it, and Insane must not ship it until one has.
     */
    static const DifficultyProfile DIFFICULTY[5];
    static constexpr int DIFFICULTY_SELFPLAY = 4;

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
     * ── SAVING UP DOES NOT WORK, AND THE REASON GENERALISES ──
     *
     * Recruit spends a SHARE of the treasury (20%), not a price, so a country
     * can never save past a recruit decision: at a treasury of 8 -- exactly the
     * cheapest industry level -- recruiting drops it to 6.4 and the factory is
     * unaffordable again. Modern China sits under $8 on 83.3% of turns and is
     * denied industry on 87.8% of the turns it wants it, while choosing `save`
     * half the time. It is trying to save and the arithmetic will not let it.
     *
     * TRIED: leave the price of the next factory standing when recruiting.
     * Industry is the best-paying thing in the game -- level 1 repays in four
     * turns, see BuildCosts.h -- so a deferred recruit should be a raise.
     *
     *     seat                 before  after
     *     1914 Sweden             120    240
     *     modern China              5     25
     *     1914 France             210    144
     *     1939 USA                196    126
     *     1914 France, at war      97     32
     *     1939 Norway, one rusher 144     77
     *     rating                  129    108
     *
     * It does exactly what it was designed to do for the countries that were
     * too poor to build anything -- Sweden doubles -- and it takes the army away
     * from every country that was using it. A reserve is a good policy when
     * broke and a bad one when threatened, and the treasury cannot tell those
     * apart.
     *
     * THAT IS THE SEVENTH BUDGET RULE WITH THIS SHAPE. Research level, research
     * direction, the social cap, wartime minority austerity, port affordability,
     * scrapping the fleet, and now saving for industry: every one helps one kind
     * of country and hurts another, because how to spend depends on the position
     * -- rich or poor, coastal or landlocked, safe or invaded -- and a constant
     * here is applied to all of them at once. The policy CAN see position; it
     * has features for all of it. Do not write an eighth. Fix the head.
     */

    /**
     * ── THE WAR HEAD'S PASSIVITY IS LOAD-BEARING. DO NOT "FIX" IT. ──
     *
     * It takes `attack` on about 51% of the turns it is offered against the
     * ordinary rung and 29% against a relentless opponent that attacks on 100%.
     * That reads exactly like the paralysis a player complains about, and the
     * obvious repair is a reflex that takes the assaults the head is declining
     * -- only the free ones, 2.5x the defender and above, leaving everything
     * between 1.05 and 2.5 to the head. That is the shape that made
     * fortifyReflex worth +4.7 against a rusher.
     *
     * IT IS CATASTROPHIC. Measured on the seat bench, three seeds:
     *
     *                        before   after
     *     France, world at war   6.5     0.5     (par 6.7)
     *     Norway, one rusher     2.8     0.4     (par 1.3)
     *
     * A ninety per cent collapse on both survival seats, while ordinary France
     * improved slightly -- so the reflex is not simply bad, it trades away
     * exactly the thing that is already weakest.
     *
     * The reason is worth keeping: a 2.5x local margin is not a free province
     * when the opponent never stops. Taking it moves the garrison off its own
     * ground, and a relentless enemy counterattacks the front that just
     * emptied. The head declining those attacks is not paralysis, it is the
     * only thing holding the line -- it has learned something about this game
     * that the margin rule does not encode.
     *
     * So the passivity is a SYMPTOM of a hard position, not the cause of it.
     * Anything that makes the AI fight more has to make it stronger first.
     */

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

    /**
     * WHAT AN ALLIANCE COSTS, which nothing priced until now.
     *
     * Reported from 1.0.8a with a screenshot of Britain allied to essentially
     * the entire map: "the game isnt really fun if you use diplomacy for
     * anything." Measured on the current model at the same time: 97.9% of
     * diplomatic requests accepted.
     *
     * The AI was reading its reward correctly. Pacts are pure upside there --
     * +1.0 x tanh(pacts/3) in the politics reward, +0.6 in diplomacy's, and
     * -0.8 x tanh(pactsLost) for letting one go -- while NOTHING scores what an
     * alliance commits a country to. A non-aggression pact gets a +0.80 thumb
     * toward yes and a call to arms gets four hard gates and a reluctance;
     * an alliance request got neither, so "yes" was free and the net took it
     * every time.
     *
     * TWO PARTS, both machinery rather than policy, in the same sense as the
     * call-to-arms gates:
     *
     * MAX_PACTS is a cap, and it is not arbitrary. The reward saturates:
     * tanh(6/3) = 0.96, so the seventh ally is worth about four hundredths of
     * a point and the twentieth is worth nothing measurable. A country holding
     * six alliances has already collected everything the reward pays for, so
     * refusing beyond that costs the policy nothing it was being paid for --
     * it only stops the behaviour nobody wanted.
     *
     * RELUCTANCE scales with the ASKER'S ACTIVE WARS, because that is the
     * concrete thing being agreed to: every war they are in is a call to arms
     * this country has just promised to answer. Allying with a peaceful
     * neighbour stays nearly free; allying with somebody already fighting two
     * wars is joining two wars on a handshake.
     *
     * A bias and a gate work on the SHIPPED model without retraining, which
     * matters here -- players have this in 1.0.8a now. The reward itself still
     * wants fixing so a retrained model learns it rather than being fenced in.
     *
     * ── AND IT HAS TO BIND THE ASKER, NOT ONLY THE ANSWERED ──
     *
     * The cap as first written tested the pact count of the country being
     * ASKED, and only for a request_alliance. That is exactly the wrong side
     * for the case it was written for. Britain allied to the whole map is one
     * country holding two hundred pacts and two hundred countries holding one
     * apiece; every one of those answerers is far under the cap and says yes,
     * so the screenshot the cap exists to prevent reproduces perfectly with
     * the cap in place. A hub is never stopped by a limit on its spokes.
     *
     * So the count is now taken on BOTH sides, and for all three standing
     * agreements rather than alliances alone -- a world where nobody may ally
     * but everybody may sign a non-aggression pact with everybody is the same
     * frozen map with a different colour on the overlay. The player is subject
     * to it too: `pacts` comes from m_stats, which is built from the relation
     * graph and does not care who is human.
     */
    /**
     * THE MOST OF ITS INCOME A COUNTRY WILL COMMIT TO STANDING POLITICAL COSTS.
     *
     * Doctrines, minority settlements and pacification all bill every turn for
     * ever. canCountryEnactPolicy asks whether ONE more is affordable out of
     * currently-spare income; nothing asks whether the stack is. So a run of
     * individually-affordable doctrines sinks the country, and nothing revisits
     * the decision when income later falls.
     *
     * Measured 2026-08-25 with the money ledger, on the model that spent 378 of
     * 400 turns bankrupt: doctrine upkeep alone was 34.4% of everything it
     * spent -- the largest single line, ahead of research at 27.2% -- and the
     * whole economy ran at NET -2.07 per country-turn. Structurally
     * loss-making, not unlucky. (The navy, which I had blamed first, was 5.5%.)
     *
     * 25% of GROSS income, because gross is what a country can actually see
     * coming; net is the thing this cap exists to keep positive, so budgeting
     * against it would be circular.
     *
     * Applied to the AI's own choice rather than to canCountryEnactPolicy,
     * which the player's UI also uses. A player may over-commit and go broke --
     * that is their decision to make, and they can watch net income go red
     * before they make it. The AI has no equivalent foresight, which is the
     * asymmetry this closes.
     */
    /**
     * POTENTIAL-BASED SHAPING: the weights below multiply a DIFFERENCE OF
     * POTENTIALS, not a bounded function of a delta.
     *
     * The old form was w * tanh(dx / k) -- tanh OF a change. That is not
     * potential-based, so it has an optimum of its own, and the optimum is not
     * winning. A concrete exploit it allows, with the shipped constants:
     * losing three provinces scores tanh(-1) = -0.762, and retaking them one
     * at a time scores 3 * tanh(1/3) = +0.965. The round trip pays +0.203 for
     * ending exactly where it started. Every such cycle is free reward, and a
     * policy that has stopped collapsing WILL find them -- which is what
     * 2026-08-25 measured when the estimator was fixed and the model promptly
     * got worse.
     *
     * F = Phi(s') - Phi(s) telescopes across an episode to Phi(end) -
     * Phi(start): a cycle is worth exactly zero, and the optimal policy is
     * provably unchanged (Ng, Harada & Russell 1999). It can only change how
     * fast that policy is found.
     *
     * WHY log1p AND NOT tanh FOR THE POTENTIAL. A saturating potential has
     * almost no gradient once a country is large -- tanh(p/3) at ten provinces
     * is flat, so an empire would be paid nothing for growing. log1p keeps a
     * diminishing but non-vanishing gradient at every size: gaining one
     * province is worth about 1/(1+p), which is the diminishing-returns shape
     * the tanh was reaching for without the dead zone.
     *
     * WHAT IS DELIBERATELY *NOT* CONVERTED: rebellions and insolvency. Those
     * are not proxies for winning, they are things a competent player does not
     * do, and expressing them as potentials would let a policy pay for one by
     * arranging the mirror of it later. They stay real costs, and this is a
     * choice rather than an oversight.
     */
    /**
     * BEHAVIOURAL CLONING FROM THE SCRIPTED PLAYER -- AlphaGo's first stage.
     *
     * Switched on with OD_BC_FROM_SCRIPT=<weight>. While it is on, every
     * decision ALSO asks the scripted rung what it would have done and pushes
     * the policy head toward that answer by cross-entropy, alongside the
     * ordinary reinforcement update.
     *
     * WHY: the teacher is better than the student, and measurably so where it
     * counts. Against the scripted rung on 2026-08-25, per thousand
     * country-turns:
     *
     *     minorities conciliated    2.97  vs  263.23
     *     ceasefires offered        3.96  vs   13.06
     *     agreements accepted        95%  vs      32%
     *     turns bankrupt          144.55  vs     3.92
     *
     * The script is not better at everything -- it never builds a navy and
     * never stages -- but it is overwhelmingly better at politics, diplomacy
     * and staying solvent, which is precisely the list of things a week of
     * reward tuning has failed to teach. AlphaGo did not start from self-play
     * either; it started supervised on a teacher and used RL to improve on it.
     *
     * A WEIGHT AND NOT A SWITCH, because cloning outright would cap the policy
     * at the teacher's strength. This is a pull toward it, strongest when the
     * policy disagrees, and it composes with the reward rather than replacing
     * it. 0 is off.
     */
    /**
     * MONTE-CARLO VALUE TARGET -- AlphaZero's value head, adapted.
     *
     * AlphaZero regresses V(s) onto "did this position win", so the critic
     * never inherits a mistake in the shaping. Ours is trained on a
     * bootstrapped sum of ~15 hand-weighted terms, so every error in the reward
     * is also an error in the baseline that judges every action.
     *
     * IT CANNOT SIMPLY BE SWAPPED, and the reason matters. AlphaZero has ONE
     * value because it has ONE reward. Here each module has its own shaped
     * reward, and the advantage every policy step uses is target - V(s). Point
     * V at the map outcome while target stays shaped and the two are in
     * different units: the advantage stops meaning anything and every head
     * trains on noise. So this is a BLEND, at a weight, behind a flag
     * (OD_VALUE_MC), default off, and it has to be measured rather than
     * assumed.
     *
     * The states are buffered as their windows settle and regressed at map end,
     * when the outcome is known. ~6k states a map at FEATURE_COUNT floats is a
     * few MB, which is affordable; the cap below stops a pathological map from
     * growing it without bound.
     */
    static constexpr float  VALUE_MC_WEIGHT   = 0.25f;
    static constexpr size_t VALUE_MC_MAX      = 40000;
    static constexpr float  VALUE_MC_LR       = 0.0015f;
    /// Read once from OD_VALUE_MC. 0 = off (the default).
    static float s_valueMcWeight;

    /**
     * THE OPENING BOOK: hand the first N turns to the scripted player.
     *
     * Chess engines do not think in the opening, and for the same reason this
     * one should not: the early game is near-stereotyped -- industrialise,
     * secure borders, do not start a war you cannot fund -- and it is where a
     * bad decision compounds for the remaining 370 turns. A policy still
     * learning the midgame pays for its opening mistakes for the whole game,
     * which is the worst possible place to be exploring.
     *
     * The script is measurably better here: it finishes the early game solvent
     * (3.92 turns bankrupt per run against the model's 144.55) and with its
     * minorities conciliated, which is the position the midgame is played from.
     *
     * ON IN TRAINING AS WELL AS IN PLAY, deliberately. A book used only at play
     * time would train the policy on openings it never actually faces, and the
     * states it then meets in the midgame would be drawn from a distribution
     * the trained policy never produced. Both halves must see the same opening
     * or the training distribution and the play distribution diverge.
     *
     * The decisions are still RECORDED for learning -- the policy is trained on
     * what the book did, which is behavioural cloning by another name and the
     * reason the two features sit next to each other.
     */
    static constexpr int    AI_OPENING_TURNS  = 20;

    static constexpr float  BC_DEFAULT_WEIGHT = 0.30f;
    /**
     * WHY THIS IS NO LONGER A LEARNING RATE.
     *
     * The first cloning run applied the teacher IMMEDIATELY, one weight update
     * per decision, at BC_LR = 0.002 scaled by the weight. That is ~247,000
     * unbatched updates per map against PPO's batched few thousand, so cloning
     * was not a term in the objective -- it WAS the objective, and the run
     * collapsed the war head onto the teacher's most common answer: hold
     * 100.00%, recruit/attack/declare-war/stage/reinforce all exactly 0.00%,
     * ADVANTAGE 1.25 -> 0.61 against the script.
     *
     * Two things were wrong and both are fixed here. The volume: cloning now
     * accumulates into the SAME batch as the policy gradient, one gradient per
     * sample, so BC_DEFAULT_WEIGHT is a weight on an auxiliary loss and the
     * batch learning rate is the only learning rate. And the imbalance: the
     * teacher answers `hold` far more often than it answers `attack`, so an
     * unweighted clone fits the teacher's MARGINAL and the rare branches --
     * which are the ones worth learning -- are drowned. See bcSampleWeight.
     */
    static constexpr float  BC_LR             = 0.002f;
    /**
     * Batches over which the cloning weight decays from BC_DEFAULT_WEIGHT to
     * nothing.
     *
     * AlphaGo cloned FIRST and reinforced AFTER; running both at once at a
     * fixed weight leaves a teacher arguing with the reward for the whole run,
     * and the teacher -- being a fixed script -- has a ceiling the policy is
     * supposed to pass. So cloning is a warm start with an end: it shapes the
     * head while the head is noise, then gets out of the way.
     */
    static constexpr long long BC_ANNEAL_BATCHES = 40000;
    /**
     * Weight on a booked move, which is a demonstration by construction.
     *
     * Not annealed and not opt-in: see bcSampleWeight. Lower than
     * BC_DEFAULT_WEIGHT because the book fires on EVERY country for the first
     * AI_OPENING_TURNS turns of every map, so it is already the densest
     * supervision in the run without being weighted up as well.
     */
    static constexpr float  BOOK_CLONE_WEIGHT = 0.15f;
    /**
     * Exponent on the class-balance term. ZERO -- balancing is OFF.
     *
     * balance = (uniform_share / this_action_share) ^ BC_BALANCE_POWER, so 0
     * makes it exactly 1 for every action and the whole term disappears; 0.5 is
     * the inverse-square-root weighting it had; 1.0 is full inverse-frequency.
     *
     * ── WHY IT IS OFF ──
     *
     * It was added to stop cloning fitting the teacher's MARGINAL -- the head
     * learning `hold` because `hold` is what the teacher says most often. That
     * pathology was real, but it was caused by applying the clone as 247,000
     * unbatched weight updates per map, and BATCHING alone removes it. The
     * balancing was a second fix for an already-fixed problem, and it brought
     * its own failure, exactly inverted.
     *
     * Measured over 527 maps at power 0.5, clamped to [0.25, 4], against the
     * baseline it was trained from (400 turns, six seeds):
     *
     *                            unbalanced BC   balanced BC   baseline
     *     ADVANTAGE @400              0.61          1.08         1.25
     *     shape war:hold %          100.00          0.00         1.53
     *     shape war:attack %          0.00         81.98         6.50
     *     attack orders issued         709          1407          709
     *     agreements accepted %       97.0           1.5         97.0
     *     turns bankrupt              23.8          34.6         23.8
     *
     * Balancing fixed the collapse -- one reward term collapsed instead of four
     * -- and then overshot through it. Up-weighting what the teacher does
     * rarely and down-weighting what it does constantly spanned a factor of
     * sixteen, and the policy learned to prefer the rare side on its own
     * account: it attacked twice as often, lost 85% of those assaults, refused
     * essentially every agreement, and went broke.
     *
     * So the term is off rather than tuned down. Halving the clamp would only
     * choose a smaller number for a correction that the batching had already
     * made unnecessary, and carrying a knob that has never been shown to help
     * is how a system accumulates settings nobody can justify. The counters
     * that feed it are still kept, so turning it back on is this one constant
     * -- but it has to earn its way back with a measurement.
     */
    static constexpr float  BC_BALANCE_POWER = 0.0f;

    /**
     * ── THE COLLAPSE GUARD ──
     *
     * PPO_ENTROPY is 0.01 because 0.03 was measured to be worse (see there),
     * and that measurement stands: a large FIXED entropy bonus flattens the
     * selectivity the model wins with. But a fixed small bonus is also what
     * failed to stop a head collapsing to one action.
     *
     * Both are true because they are different questions. What is wanted is
     * not more exploration everywhere -- it is a FLOOR under each head's
     * entropy, inert while the head is healthy and firm when it is not. So the
     * coefficient is per-module and controlled: raised while a module's mean
     * entropy sits under the floor, decayed back toward PPO_ENTROPY when it is
     * clear. A healthy run therefore trains at exactly the 0.01 that was
     * measured best, and a collapsing one gets pushed back off the wall.
     *
     * The floor is a FRACTION OF log(support) rather than a constant, because
     * a head choosing among two legal actions cannot have the entropy of one
     * choosing among eight, and a constant floor would either be unreachable
     * for the first or useless for the second.
     */
    /**
     * ── WHAT THE GUARD ACTUALLY WATCHES ──
     *
     * NOT per-state entropy. That was tried first and measured, on the model
     * that benches at ADVANTAGE 1.25 and the collapsed one that benches at
     * 0.61, as mean per-state entropy over 30 turns on two seeds, as a
     * percentage of each head's own ceiling:
     *
     *                healthy            collapsed
     *     econ       13.3%  0.1%        33.1%  37.7%
     *     politics   27.9%  9.8%         9.0%  33.6%
     *     war        25.2% 52.3%         7.4%   8.6%
     *     navy        1.5% 42.0%         0.0%   0.0%
     *
     * The healthy model's econ head reads 0.1% on one seed and its navy head
     * 1.5% on another, while the collapsed model's econ head reads 37.7%. No
     * threshold separates those columns, and the reason is not noise: a policy
     * that has learned which action a state calls for SHOULD be certain in that
     * state. Per-state entropy measures confidence, and confidence is what the
     * model wins with -- an entropy bonus large enough to argue with it was
     * measured to cost 0.33 of ADVANTAGE (see PPO_ENTROPY).
     *
     * What went wrong is a different quantity. The collapsed head did not
     * become certain state by state; it stopped VARYING ITS ANSWER AT ALL --
     * `hold` on every turn of every country, with recruit, attack, declare war,
     * stage and reinforce at exactly 0.00%. That is the MARGINAL over chosen
     * actions, and in the same two columns it separates cleanly and on both
     * seeds: the war head's marginal entropy is what the bench prints as
     * "shape war:hold 100.00%".
     *
     * So the guard reads the marginal, and the floor below applies to it. The
     * per-state figure is still measured and logged, because it is the thing
     * the entropy bonus can act on -- but it is a diagnostic, not the trigger.
     *
     * ── AND WHAT THIS GUARD IS NOT ──
     *
     * It is a backstop, not the fix. The fix for the collapse that prompted it
     * is the cloning redesign (see BC_LR): batched instead of applied per
     * decision, annealed instead of permanent, class-balanced instead of
     * fitting the teacher's marginal. That is where the failure came from and
     * that is where it is closed.
     *
     * The threshold below could not be tuned finely, and pretending otherwise
     * would be worse than saying so. Probing both models over 30 turns on two
     * seeds, the marginal ranged 0.62-1.42 on the healthy model and 0.27-1.34
     * on the collapsed one -- overlapping, because thirty turns of one map is
     * not the four hundred the bench collapse was measured over, and because
     * the opening book owns the first twenty of them. So the floor is set where
     * it is inert on every head of the healthy model across both seeds and
     * still fires on the collapsed model's worst head: it catches a policy
     * whose marginal has genuinely folded up, and it does not have an opinion
     * about anything short of that.
     *
     * If a run collapses anyway, the number that will show it is the
     * "marginal" column in the [GUARD] line, printed every GUARD_LOG_BATCHES
     * batches. The point of that line is that the last collapse cost fourteen
     * hours before a bench revealed it.
     */
    /**
     * The floor, as a fraction of the head's own log(support).
     *
     * CALIBRATED, not chosen. Measured on two real models -- the one that
     * benches at ADVANTAGE 1.25 and the collapsed one that benches at 0.61 --
     * as mean per-state entropy over a 30-turn run, against each head's
     * ceiling:
     *
     *                healthy      collapsed
     *     econ        7.8%          35%
     *     politics     29%          11%
     *     war          25%         7.5%
     *     navy          0%           0%
     *
     * Two things follow, and the second is the awkward one. A collapsing head
     * does fall -- war went 25% -> 7.5% -- so the signal is real. But a HEALTHY
     * econ head also sits at 7.8%, because a policy that has learned which
     * action a state calls for is supposed to be confident about it. Per-state
     * entropy cannot separate "confident" from "degenerate" in general.
     *
     * So the floor is set where it separates the case that actually destroyed a
     * run -- entropy at or near ZERO, one action everywhere -- and nowhere
     * tighter. At 5% the guard is inert on every head of the healthy model
     * except navy, which is genuinely collapsed there and has been all along.
     * It is a backstop against total degeneracy, not an opinion about how
     * decisive a head ought to be, and it is deliberately not tuned to be one:
     * an entropy bonus large enough to argue with a confident policy was
     * measured to cost 0.33 of ADVANTAGE. See PPO_ENTROPY.
     */
    static constexpr float  ENTROPY_FLOOR_FRAC = 0.15f;
    /// Actions offered less often than this are not counted toward the
    /// marginal's ceiling: a head cannot be blamed for never taking an action
    /// the mask almost never offers it.
    static constexpr double MARGINAL_OFFER_FLOOR = 0.05;
    /// Per-batch decay on the marginal counters: a window of a few hundred
    /// batches, long enough that one quiet turn is not a collapse and short
    /// enough that a real one is caught while it is still recoverable.
    static constexpr double MARGINAL_DECAY = 0.995;
    static constexpr float  ENTROPY_COEF_MAX   = 0.15f;
    /**
     * The guard aims ABOVE the floor it defends, because a proportional
     * controller settles where its correction balances the push, which is
     * always short of where it aimed. Measured on a saturated head under a
     * relentless advantage (tests/neural_net_test.cpp reproduces it):
     *
     *     target       4-action head   8-action head   floor
     *     1.0 x floor      0.258           0.332       0.277 / 0.416
     *     1.5 x floor      0.369           0.546
     *     2.0 x floor      0.468           0.689
     *
     * At 1.0 the equilibrium sits UNDER the floor on both. 1.5 clears it on
     * both with room, and is chosen for that rather than for looking round.
     */
    static constexpr float  ENTROPY_GUARD_TARGET_MUL = 1.5f;
    /**
     * ── WHY THE ENTROPY BONUS IS NOT ENOUGH BY ITSELF ──
     *
     * Its gradient is coef * p_i * (log p_i + H). As p -> 1, log p -> 0 and
     * H -> 0, so the force VANISHES exactly where it is needed. That is not a
     * tuning problem, it is the shape of the term: raising the coefficient to
     * ENTROPY_COEF_MAX on a fully collapsed head moved its entropy from 0.00001
     * to 0.029 against a floor of 0.277, and even an instant proportional
     * controller with no cap only reached 0.185. The bonus cannot climb out of
     * a corner it failed to prevent.
     *
     * Cross-entropy toward UNIFORM has gradient (p_i - 1/k). At saturation
     * that is (1 - 1/k) for the dominant action -- its LARGEST value, not its
     * smallest. Added at a weight proportional to the entropy deficit, it took
     * the same collapsed head from 0.00001 to 0.258 in about 500 batches and
     * held it there against the same relentless advantage.
     *
     * ZERO WHEN THE HEAD IS HEALTHY. Above the target the deficit is zero, the
     * pull is not applied at all, and the coefficient is PPO_ENTROPY -- so an
     * ordinary run trains at exactly the settings that were measured best and
     * this whole mechanism is inert. It is a guard rail, not a nudge.
     */
    static constexpr float  UNIFORM_PULL_K     = 1.0f;

    /**
     * KL(behaviour || policy) per batch above which the step is scaled down.
     *
     * PPO's clip bounds each SAMPLE; nothing bounded the batch. A batch whose
     * samples all point the same way walks the policy a long distance in one
     * step even though no single sample was allowed to -- which is how a head
     * arrives at a corner it cannot climb out of. Standard trust-region
     * practice: measure the divergence the batch actually produced, and if it
     * overshoots the target, shrink the step that produced it.
     */
    static constexpr float  PPO_KL_TARGET      = 0.02f;
    /// How many decisions the trust region re-measures after each step.
    static constexpr int    KL_PROBE_N         = 96;
    /**
     * One decision, kept across the optimiser step so the step can be measured.
     *
     * WHAT THIS FIXES. The first version of the trust region used the KL the
     * PPO update already computes -- KL(behaviour || policy) -- which is the
     * wrong quantity twice over. It is measured against the policy as it stood
     * when the sample was COLLECTED, N_STEP turns and several hundred updates
     * ago, so most of it is the off-policy staleness this design has by
     * construction rather than anything this batch did. On a healthy 250-turn
     * run it read 0.076 against a 0.02 target and quietly scaled every step to
     * 26%, which is not a trust region, it is a learning-rate cut with a
     * plausible name.
     *
     * A trust region has to measure the step. So the distribution is recorded
     * before the flush and recomputed after it, and the KL between those two
     * is exactly how far the update moved the policy -- nothing else.
     */
    struct KLProbe {
        int module = 0;
        std::vector<float> features;
        std::vector<uint8_t> validMask;
        std::vector<float> before;   ///< masked probabilities, pre-step
    };
    std::vector<KLProbe> m_klProbe;
    /// Scratches the probe forwards through. Its own, so a probe cannot land
    /// on activations a worker still needs.
    NeuralNet::Scratch m_probeTrunk, m_probePolicy;
    bool m_probeReady = false;
    /// Mean KL(before || after) across the probe, last step. For the log.
    float m_stepKl = 0.0f;
    /// Softmax over the masked logits, or over all of them if the mask is
    /// empty. The one place the masking convention is written down.
    static std::vector<float> maskedProbs(const std::vector<float>& logits,
                                          const std::vector<uint8_t>& mask);
    /// Fills m_klProbe from the batch about to be flushed.
    void captureKLProbe();
    /// Re-measures m_klProbe after the step and sets m_klStepScale.
    void measureStepAndSetScale();
    /// Read once from OD_BC_FROM_SCRIPT. 0 = off, which is the default.
    static float s_bcWeight;
    /**
     * OD_BC_OBSERVE=1: ask the teacher and COUNT the agreement, but apply no
     * gradient.
     *
     * The control this measurement needs. With cloning off the counter never
     * increments, so "agreement rose from 51% to 65% while cloning ran" cannot
     * be told apart from "agreement rises anyway as a policy settles". Observing
     * without updating gives the same number from an untouched run, which is
     * the only way the first number means anything.
     */
    static bool s_bcObserve;

    static constexpr float  PHI_PROV      = 2.4f;   ///< land held
    static constexpr float  PHI_TREASURY  = 0.35f;  ///< money in hand
    static constexpr float  PHI_NET       = 0.55f;  ///< earning power
    static constexpr float  PHI_INDUSTRY  = 1.2f;   ///< built capacity
    static constexpr float  PHI_RESEARCH  = 1.0f;   ///< nodes completed

    static constexpr float  AI_DOCTRINE_BUDGET_SHARE   = 0.25f;
    /**
     * ── AND THE SAME CEILING ON WHAT GOVERNING COSTS ──
     *
     * Doctrines answer to a budget; minority programmes and the pacification
     * slider never did. Each conciliation is a PERMANENT rise in the per-turn
     * bill, and the only check on one was whether this turn's net income could
     * absorb the marginal increase -- which says nothing about the twentieth.
     *
     * Measured against the blitz exploit, per country-turn:
     *
     *                       model    blitz
     *     standing bill     25.09     4.03
     *     gross income      86.91   353.80
     *     net income         2.36   109.43
     *     turns under $8    91.6%    33.5%
     *
     * The blitz does nothing politically at all, and that is most of the gap.
     * The model spends a quarter of its gross on governing, has nothing left,
     * cannot buy industry (279 builds against 2,286), so its income never
     * grows, so it cannot recruit, so it has no army and no winnable assaults.
     * The war module's passivity is the last link in that chain rather than the
     * first, which is why removing options from it changed nothing.
     *
     * ── AND IT IS SET WHERE IT DOES NOTHING, ON PURPOSE ──
     *
     * The obvious conclusion from those numbers is that the AI over-spends on
     * governing and should be made to stop. That was measured, and it is wrong.
     * Sweeping the share on one frozen model, vs the scripted rung:
     *
     *     share    bill   net income   industry built   rebellions   land
     *     none    25.09        2.36            2,195         74.6   1.50x
     *     0.33    25.03        3.13            2,195         74.6   1.54x
     *     0.15    23.94        9.54            4,125        101.4   1.23x
     *     0.08    12.78       15.00            4,561         98.2   1.18x
     *
     * Cutting the bill does everything it promises to the ECONOMY -- six times
     * the net income, twice the industry, the share of turns holding under $8
     * down from 92% to 66% -- and the AI holds LESS LAND for it, monotonically,
     * because the rebellions come with it. The poverty is not waste. It is what
     * holding a multi-ethnic empire together costs, and buying the economy back
     * costs more ground than the economy wins.
     *
     * A THIRD experiment said the same thing from a different direction:
     * refusing a new minority programme on any turn the country could instead
     * buy a factory -- "build first, appease from a bigger base", which is what
     * a good player does. Industry built rose 2,195 -> 2,732, and rebellions
     * rose 74.6 -> 129.9, alignment fell 91% -> 79%, and the land fell 1.54x ->
     * 1.19x. Deferring appeasement costs more than the industry earns.
     *
     * Three angles, one answer: this spending is load-bearing, and the AI's
     * poverty cannot be fixed by redirecting it. If the AI is to out-earn an
     * opponent it has to do it with income it does not currently have -- trade,
     * specialisation, resource development -- rather than by governing less.
     *
     * So the ceiling sits at a third, which is above where the bill naturally
     * sits (27% of gross) and therefore never binds. It is here as a backstop
     * against a pathological case rather than as a policy, and the sweep is
     * here so the next person does not spend the evening rediscovering that
     * the cheap-looking win is a real loss.
     */
    static constexpr float  AI_SOCIAL_BUDGET_SHARE     = 0.33f;
    /**
     * ── THE ONE DIAL THAT IS A GAME-DESIGN CHOICE, NOT A BUG FIX ──
     *
     * Tightening this ceiling looked like a real tradeoff and is not one: it is
     * a loss. Measured with tools/ai_ab.py on one frozen model, five worlds,
     * paired seed by seed, land share against the scripted rung:
     *
     *     0.33 (inert)   60.6%
     *     0.15           52.1%
     *     per seed       -5.4  -14.0  -15.4  +1.7  -9.3
     *     paired         -8.5 points (band +/-6.2), better on 1 world set of 5
     *
     * It does buy real resistance to being rushed -- against a relentless
     * aggressor the AI holds 23.4% at the loose ceiling and 34.5% at 0.15 --
     * and it costs more than that everywhere else. Note the per-seed row: the
     * effect swings from -15.4 to +1.7 depending on the world, so this is not a
     * small consistent cost but a change whose sign is not even stable. Nothing
     * measured on one seed pair could have shown that, and two earlier attempts
     * to settle it on one seed pair reached opposite conclusions.
     *
     * Governing less makes the AI far richer -- net income 2.36 to 15.00, twice
     * the industry, the share of turns holding under $8 down from 92% to 66% --
     * and that money goes straight into the army: recruit is offered on 1,478
     * decisions at the loose ceiling and 4,755 at the tight one, and taken 680
     * times against 4,016. The AI was never refusing to defend itself. It could
     * not afford to; recruiting needs one dollar and it held nothing on four
     * turns in five.
     *
     * What it buys with that is resistance to being rushed. What it costs is
     * land against an ordinary opponent, because the rebellions come back.
     *
     * CONDITIONING IT ON BEING AT WAR DOES NOT WORK, and the reason is worth
     * keeping: a version that spent 8% while invaded and 33% at peace measured
     * 1.54x against the script -- peacetime performance perfectly preserved --
     * and 21.4% against the aggressor, WORSE than never capping at all. The
     * benefit was never wartime reallocation; it was compounding wealth across
     * the whole game. By the time an army is on the border it is too late to
     * have built one.
     *
     * Left inert.
     *
     * ── AND DO NOT SWEEP THIS AGAINST ADVANTAGE ──
     *
     * Getting to that answer took four wrong turns, all the same wrong turn.
     * ADVANTAGE is a RATIO of two land totals, so it is unbounded and it
     * explodes whenever the scripted cohort is nearly wiped out. Measured on
     * the SAME configuration and the same model at 400 turns, it read 1.51x on
     * one seed and 4.04x on another -- a spread of two and a half, against an
     * effect size of about a third. On one of those seeds the capped build
     * looked like a 55% IMPROVEMENT and the conclusion was written up before
     * the control run on the second seed contradicted it.
     *
     * Land share is bounded in [0, 1], it moves monotonically with the thing
     * being measured, and on it the answer is consistent across both horizons
     * and both seeds. tools/train_parallel.py's _fitness docstring says exactly
     * this and has said it since PBT was ranking workers backwards. Read the
     * `land held` line.
     */

    /**
     * HOW FAR AHEAD THE ECONOMY IS ALLOWED TO PLAN, in turns.
     *
     * N_STEP, deliberately: that is the window every economy decision is
     * already judged over, so a country saving inside this horizon is saving
     * inside the span of the reward that will grade it. Planning further than
     * you are scored is how a policy learns to hoard.
     */
    // 12 rather than `N_STEP` only because N_STEP is declared further down
    // this header; the static_assert beside it keeps the two equal.
    static constexpr int    AI_PLAN_HORIZON            = 12;

    /**
     * ── WHEN THE WORLD DECIDES YOU ARE THE PROBLEM ──
     *
     * What makes a grand-strategy game hard to beat is not each opponent being
     * clever. It is five of them independently concluding that you are the one
     * who has to be stopped. Nothing in this AI did that: every country played
     * its own game, and `m_playerCountryId` appeared in AISystem.cpp exactly
     * once, to read a pacification slider. The machinery to notice a runaway
     * power has been sitting in WorldSnapshot the whole time -- largestCid,
     * largestShare, rankOf -- and was fed to the net as features and to nothing
     * else.
     *
     * COALITION_SHARE is where a power stops being large and starts being the
     * problem. A tenth of the owned world, which sounds modest and is not: in a
     * fifty-country world an even split is two per cent, so a tenth is five
     * times everybody and the largest empire on the shipped 1914 map does not
     * start there. Below it nothing happens at all -- an ordinary game is not a
     * dogpile.
     *
     * SET FROM A MEASUREMENT, and the first guess was wrong. At 0.18 the
     * coalition never fired on the shipped scenarios: the largest power ends a
     * 200-turn run holding 18.0% of the world, so the threshold was reached on
     * the last turn of the game and the mechanism might as well not have
     * existed (six calls to arms in a whole run). The interesting part of a
     * campaign is the hour BEFORE somebody has won.
     *
     * Above it the pressure ramps to 1 at COALITION_FULL and buys three things,
     * all of which a good human already does: the leader finds it harder to
     * sign anything, its neighbours will take a worse fight to stop it (the
     * declaration bar comes down), and calls to arms AGAINST it get answered.
     *
     * A FACULTY OF THE LADDER, not a rule of the game. See
     * DifficultyProfile::useCoalition -- a player should be able to choose an
     * opponent that does not gang up.
     *
     * ── AND IT IS OFF, ON THE SECOND MEASUREMENT AS WELL ──
     *
     * The FIRST version cut each member's declaration bar, so members picked
     * fights they could not individually win, lost their armies, and the leader
     * ate them. Measured at Insane, everything else held equal:
     *
     *                       off      on
     *     largest power    20.1%   26.1%
     *     concentration    0.094   0.111
     *     land vs script    1.44x   1.10x
     *
     * A mechanism whose entire purpose is to stop one power running away with
     * the map made the map MORE concentrated. With COALITION_BAR_CUT set to
     * zero -- the diplomatic denial alone -- every figure returned to the "off"
     * column to three decimals, so the denial half was inert and the bar cut
     * was the whole effect, backwards.
     *
     * The SECOND version is the one in the code now: no bar cut, the bar
     * measured against the coalition's COMBINED army (COALITION_ODDS), and the
     * declarations bunched into a window so they arrive together. It is a
     * better mechanism and it is still a loss. Measured on the frozen model at
     * Insane, five worlds, paired seed by seed, land held:
     *
     *                              vs the rung     vs a rusher
     *     coalition off               58.8%           26.2%
     *     coalition on                63.2%           16.0%
     *     paired difference           +4.4            -10.1
     *     worlds it helped on          5/5             0/5
     *
     * It buys 4.4 points against a scripted opponent the AI already beats, and
     * costs 10.1 against the one strategy that actually beats it -- on every
     * world, both times. It also made the top rung WEAKER THAN THE ONE BELOW
     * IT: Hard, which has no coalition, holds 29.4% against the same rusher
     * where Insane held 16.7%. A difficulty setting that makes the AI easier to
     * beat is not a difficulty setting.
     *
     * ── AND IT IS NOT AN AIMING PROBLEM. THAT WAS CHECKED. ──
     *
     * There is a real defect in here: forming a coalition leans the war head
     * toward declaring and clears the bar against the leader, but NOTHING
     * pointed the target CHOICE at the leader -- and the leader is by
     * construction the strongest neighbour, which is the one the fallback rule
     * ("the weakest wins") and a head trained on it both like least.
     * chooseWarTarget had no coalition input at all. That looks like the whole
     * explanation and it is not: adding the lean in both places moved the
     * result by 63.2% -> 62.5% and 16.0% -> 16.9%, both inside the band. The
     * fix was reverted because it bought nothing, and the fact that it bought
     * nothing is the useful part of the result.
     *
     * What is left is the likeliest explanation and the one to design against:
     * clearing a bar with a COMBINED army does not put a combined army on the
     * battlefield. Members declare together and then fight separately, each
     * with its own small army against the leader's large one, and lose it. A
     * coalition that attacks feeds the thing it is trying to stop no matter how
     * well it is aimed. If this is tried a third time it should be DEFENSIVE --
     * members answering calls to arms and reinforcing each other's threatened
     * provinces, so the combined army is combined where the fighting is -- and
     * not another way of getting more war declared.
     *
     * The machinery -- coalitionPressure, isCoalitionTarget, updateCoalition
     * and the hooks that read them -- is correct and stays, gated off by
     * DifficultyProfile::useCoalition.
     */
    static constexpr float  COALITION_SHARE            = 0.10f;
    static constexpr float  COALITION_FULL             = 0.22f;
    /** The logit lean at full pressure, on the diplomacy head. */
    static constexpr float  COALITION_WEIGHT           = 1.20f;

    /**
     * HOW MUCH STRONGER THE COALITION MUST BE THAN ITS TARGET before it forms.
     *
     * This is the number the failed first attempt did not have. That version
     * cut each member's own declaration bar, so members picked fights they
     * individually could not win, lost their armies, and the leader ate them --
     * the map came out MORE concentrated, not less. The bar was never the
     * problem; whose strength it was measured against was.
     *
     * A coalition is not several countries being braver. It is several
     * countries being, together, stronger than the thing they are afraid of. So
     * the bar is unchanged and the ARMY on the near side of it becomes the
     * coalition's combined one -- and if that still does not clear it, no
     * coalition forms and nobody throws anything away.
     */
    static constexpr double COALITION_ODDS             = 1.25;
    /**
     * How long a formed coalition stays formed, in turns.
     *
     * The point of forming one at all is that the declarations arrive TOGETHER.
     * Members deciding independently over sixty turns is what the AI already
     * did, and it is how a leader eats its neighbours one at a time. Fifteen
     * turns is long enough for members on opposite sides of a large power to
     * reach the same conclusion and short enough that it reads as a war rather
     * than as a mood.
     */
    static constexpr int    COALITION_WINDOW           = 15;

    static constexpr int    AI_ALLY_MAX_PACTS          = 6;
    static constexpr float  AI_ALLY_WAR_RELUCTANCE     = 0.55f;
    /**
     * THE APPROACH TO THE CAP, so that it is a slope and not only a wall.
     *
     * A hard limit alone produces a country that signs its first six treaties
     * as readily as its first, then refuses absolutely -- which reads, from
     * the other side of the table, as a bug. This is subtracted in proportion
     * to how full the fuller of the two ledgers already is, so the sixth pact
     * is a genuinely harder sell than the first and the wall is only ever the
     * last step of a hill. At the hard-difficulty temperature 0.6 is roughly a
     * halving of the odds at the cap, which is a lean and not a veto.
     */
    static constexpr float  AI_ALLY_CROWDING           = 0.60f;
    /**
     * A GUARANTEE IS THE ONE PROMISE WITH NO SECOND CHANCE TO SAY NO.
     *
     * An alliance drags you into somebody's war through a call to arms, and
     * that is a DECISION: four hard gates, a reluctance, and the policy still
     * gets the last word (see AI_CALL_*). A guarantee has none of that.
     * declareWar chains in every guarantor of the defender automatically -- no
     * question is asked, no gate is consulted, the war simply arrives. So a
     * guarantee is strictly the more dangerous of the two to hold and should be
     * the harder of the two to agree to, where it was the only one of the three
     * with no thumb on the scale at all.
     *
     * Measured after the pact cap landed: the politics head moved 36.1% of its
     * decisions onto proposing guarantees, up from 0.2%, having had its
     * non-aggression spam capped. It was not defeated, it relocated -- and it
     * relocated onto the agreement that commits a country hardest. Reward
     * shapes behaviour; a cap only redirects it.
     */
    static constexpr float  AI_GUARANTEE_RELUCTANCE     = 0.45f;
    /**
     * WHAT A REFUSED OVERTURE COSTS THE COUNTRY THAT MADE IT.
     *
     * The politics reward pays +1.0 x tanh(pacts/3) for agreements HELD and
     * charges nothing for the asking, so proposing is free and the module
     * spends its turns doing it -- 3.3% of politics decisions were `hold` on
     * the run this was written for. A refusal is not free in the game: it costs
     * the turn, the overture budget, and a sixty-turn cooldown with that
     * partner (see noteDiploRejected).
     *
     * The mask was fixed for this once already (diploBudgetReady, which stopped
     * three eighths of the softmax parking on "propose something" every turn)
     * and the note there says the rest plainly: "no reward term ever taught the
     * net that was wasteful -- so it just kept proposing forever." This is that
     * term. Saturating, so a country that is refused twice in a window is not
     * punished twice as hard as the game punishes it.
     */
    static constexpr float  AI_OVERTURE_REFUSED_CHARGE = 0.35f;

    /**
     * HOW BADLY A SELF-PLAY OFFER MAY BE PRICED, as a multiple of the fair price.
     *
     * The offer composer (exec case 11) prices every province at a payback
     * period on its income and offers money only, so across every hour of
     * self-play ever run, the ONLY trades the answering head has seen are
     * fairly-priced purchases. "Give me your country for nothing" -- the alpha's
     * reported exploit -- is not a question it answered wrong; it is one outside
     * the training distribution entirely, which is why a human walks through it.
     *
     * So during training the asking price is scaled by a factor drawn across
     * this range: 0 is a bare demand, 1 the fair price, the top end overpays.
     * The reward is already correct and signed (noteTradeOutcome pays the
     * recipient the gold value of what the deal did to them), so with the full
     * spread on the input side the boundary is learnable from what exists.
     *
     * THIS TEACHES THE ANSWERER, NOT THE ASKER: the price is computed by hand,
     * no head chooses it. Training only, via selfPlayLearning().
     */
    static constexpr float  AI_TRADE_TRAIN_PRICE_MIN   = 0.00f;
    static constexpr float  AI_TRADE_TRAIN_PRICE_MAX   = 1.60f;

    /**
     * WHAT A COUNTRY WILL NEVER TRADE AWAY, whatever the policy thinks.
     *
     * Reported from the alpha 2026-08-21: the AI "can be VERY easily convinced
     * to trade (give up) all their land and as much money as they can muster".
     * The terms are not hidden from it -- decideDiplomacy prices both halves in
     * gold -- and it accepts anyway: 95.0% of offers under flat pricing, 95.3%
     * after income pricing, over two 3,400-trade runs. A threshold nothing
     * fails is not a threshold, and "hand over the country" should not be on
     * the table at all. Shares, so a small state and an empire are judged by
     * the same rule. Trades only: a ceasefire that cedes land can be the best
     * move on the board.
     */
    static constexpr float  AI_TRADE_RUIN_PROV_SHARE   = 0.34f;
    static constexpr float  AI_TRADE_RUIN_CASH_SHARE   = 0.60f;

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
    // ── DRIVING THE POLICY'S OWN ACTION SPACE BY HAND (--bench-agent) ──
    //
    // The point of the agent mode is a FAIR comparison: whoever is playing gets
    // the same menu the policy gets and the same executor runs the choice, so a
    // difference in result is a difference in judgement rather than in what was
    // on offer. These are the two doors into that, and they are deliberately
    // thin -- no second copy of the mask, no second copy of the executor, both
    // of which is how this file has twice grown two versions of one rule.
    //
    // agentRefresh() first: the masks read m_stats, which is filled at the top
    // of a country's turn, and a seat's country is not one the AI takes.
    void agentRefresh() { refreshStats(); }
    void agentLegal(int cid, int module, std::vector<bool>& out) {
        switch (module) {
            case MOD_ECONOMY:  validEconomy(cid, out);  break;
            case MOD_POLITICS: validPolitics(cid, out); break;
            case MOD_WAR:      validWar(cid, out);      break;
            default:           validNavy(cid, out);     break;
        }
    }
    std::string agentExec(int cid, int module, int action) {
        switch (module) {
            case MOD_ECONOMY:  return execEconomy(cid, action);
            case MOD_POLITICS: return execPolitics(cid, action);
            case MOD_WAR:      return execWar(cid, action);
            default:           return execNavy(cid, action);
        }
    }
private:
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
    /**
     * AN ARMY IS ASHORE -- reported by the resolver, which is the only place
     * that knows one is.
     *
     * `landings` and `unloadsHome` used to be incremented where the ORDER was
     * issued, in two AI code paths. processShipDisembarks then dropped any
     * order whose hull was out of range and said nothing, so the amphibious
     * line of the report counted invasions the AI had merely attempted:
     * measured on one 100-turn map, 108 of 168 "landings" never happened.
     * Counted here instead, the number means what its label says.
     */
    void noteLanding(int cid, bool hostileShore);
    /** Marks the decision now executing as having had no effect, and hands back
     *  its own reason string so a failure return stays one line. See
     *  TrainStats::noopChosen. */
    std::string didNothing(std::string why);
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
    /**
     * THE TUTORIAL'S OPPONENTS ARE HAND-WRITTEN, ALWAYS.
     *
     * A tutorial is a script, and a learned policy is not one. The model is
     * trained to win, it changes every time it is retrained, and it is under
     * no obligation to behave the way the lesson standing beside it says it
     * will -- so "Kestrel is weak, take them" is a promise the game may
     * simply break, on a build nobody thought was a tutorial change.
     *
     * With this set every AI country plays the hand-written turtle: it
     * defends what it has, it does not launch offensives, and it does the
     * same thing on Tuesday that it did on Monday. Predictable is the whole
     * requirement; being good at the game is not one at all here.
     */
    static bool s_tutorialAI;
    /**
     * ── THE HAND-WRITTEN OPPONENTS, INCLUDING THE DISHONEST ONES ──
     *
     * The first two are yardsticks: a competent attacker and a competent
     * defender, written to be beaten or matched.
     *
     * The four after them are not yardsticks, they are EXPLOITS. Each plays one
     * human idea single-mindedly and refuses to be talked out of it, which is
     * exactly what a person does when they find something that works. An AI
     * that beats the aggressor rung and loses to a man who only ever builds
     * factories has not been measured yet -- it has been measured against
     * somebody playing the same game it is.
     *
     * They are cheap to write and they are the only thing here that answers
     * "would a player beat this", which is the question that actually matters
     * and the one ADVANTAGE-vs-script cannot ask.
     */
    enum ScriptVariant {
        SCRIPT_AGGRESSOR = 0,
        SCRIPT_TURTLE    = 1,
        /// Never fights. Builds industry and research and outgrows the world.
        SCRIPT_TECH      = 2,
        /// Never stops fighting, never makes peace, always attacks the weakest.
        SCRIPT_BLITZ     = 3,
        /// The pact hub: asks everybody for everything, every turn. This is the
        /// "canon british diplomacy" screenshot, played deliberately.
        SCRIPT_DIPLO     = 4,
        /// Buys the sea the AI never contests -- ports, hulls, then landings.
        SCRIPT_NAVY      = 5,
        SCRIPT_VARIANT_COUNT
    };
    /** Which exploit the control cohort plays, or -1 for the ordinary rungs. */
    static int s_exploitVariant;
    /**
     * WHO plays that exploit, when it should not be everybody.
     *
     * Empty means the whole control cohort does, which is the ordinary
     * --vs-exploit run. Non-empty restricts it to these country ids and leaves
     * every other control country on the ordinary rung.
     *
     * It exists for the benchmark's neighbourhood seats. A seat where the ENTIRE
     * world attacks without pause is not a hard test, it is an unsurvivable one:
     * every model tried scored 13 out of 100 on 1939:NOR that way, so the seat
     * ranked nobody. A seat is only worth having if better play scores better,
     * and being surrounded by three relentless neighbours is both harder to tell
     * apart and closer to the thing a player actually meets.
     */
    static std::unordered_set<int> s_exploitCids;
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
        /// Behavioural cloning: how often the teacher was consulted, and how
        /// often the policy already agreed with it. The agreement rate is the
        /// evidence that cloning is doing something -- see the note on
        /// BC_DEFAULT_WEIGHT.
        long long bcSamples = 0, bcAgreed = 0;
        long long warsDeclared = 0, ceasefiresOffered = 0,
                  pactsProposed = 0, researchCompleted = 0,
                  tradesOffered = 0;
        /**
         * WHAT HAPPENED TO THE OFFERS. tradesOffered counts proposals put on
         * the table; without the other two, a run in which the AI never trades
         * and one in which it trades constantly and is always refused report
         * the same single number, and those need opposite fixes.
         */
        long long tradesAccepted = 0, tradesRefused = 0;
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
         * WHAT THE MASK TOOK AWAY, AND WHY -- for the economy only, because
         * the economy is the module whose whole mask is money.
         *
         * offered/chosen answers "did the policy want this action". It cannot
         * answer "was the action ever on the menu", and for a build that is
         * the more important question: an AI country spends to zero every turn
         * (see the note on the sealift lump sum), so a treasury floor on an
         * action is not a price, it is a prohibition, and one that looks
         * exactly like a policy that has stopped choosing.
         *
         * cashBlocked counts the decisions where every non-money condition for
         * the action held and the treasury alone withheld it. econOffered +
         * econCashBlocked is therefore the number of turns the action was
         * SUBSTANTIVELY available, and the gap between the two rates says
         * whether to retrain the head or to reprice the action.
         */
        long long econCashBlocked[ECON_ACTIONS] = {0};
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
        /**
         * THE OTHER TWO MODULES, counted the same way.
         *
         * Economy and war have had offered/chosen since the first collapse;
         * politics and navy never did. So the three things a player is most
         * likely to notice going wrong -- who the AI signs treaties with, what
         * it offers to buy, and whether its fleet does anything -- were the
         * three the report could say nothing about. A head that has stopped
         * choosing is only visible in this pair of numbers, and both of these
         * modules turn out to have one.
         */
        /**
         * THE PURSE. What an AI country actually has and earns, per turn.
         *
         * Every economy action is gated on the treasury, and the report could
         * say how often that gate closed (econCashBlocked) without saying
         * anything about WHY. "Broke because it earns nothing" and "broke
         * because it spends everything the moment it has it" are opposite
         * problems with opposite fixes, and the take rate on `save` cannot tell
         * them apart either -- money saved and money still there next turn are
         * not the same thing when the standing bill is larger than the income.
         *
         * Accumulated in takeTurn, which already computes the snapshot for the
         * reward, so this costs nothing.
         */
        double purseTreasury = 0.0;   // sum of treasury at decision time
        double purseGross    = 0.0;   // sum of pre-expense income
        double purseNet      = 0.0;   // sum of net income
        double purseUpkeep   = 0.0;   // sum of the standing political bill
        long long purseTurns = 0;
        long long purseNetNegative = 0;   // country-turns running at a loss
        long long purseBroke = 0;         // country-turns with under $8 -- the
                                          // price of the cheapest thing on the
                                          // economy menu (IND_COST[1])
        /**
         * WHERE THE MONEY ACTUALLY WENT, split by the module that spent it.
         *
         * The modules run in a fixed order -- economy, politics, war, navy --
         * and each keeps acting until it passes, so economy decides how to
         * spend the purse before war is ever asked what it needs. Under a rush
         * that ordering is a live suspicion rather than a fact, and purseBroke
         * cannot tell the two stories apart: a treasury at zero looks the same
         * whether economy spent it on industry or war spent it on troops.
         *
         * spendWar being small while cashAtWar is ALSO small is the starvation
         * story -- war wanted to act and there was nothing left. spendWar small
         * while cashAtWar is large is the opposite and much more interesting:
         * the money was there and the war head declined it, which is a policy
         * problem no budgeting change can reach.
         */
        double spendEcon = 0.0;    // treasury drop across each module
        double spendPol  = 0.0;
        double spendWar  = 0.0;
        double spendNavy = 0.0;
        double cashAtWar = 0.0;    // treasury when war was first asked
        long long spendTurns = 0;
        // The means above are badly skewed -- a treasury averaging 25 while
        // 83% of turns are under $8 is a handful of rich turns carrying the
        // mean, and "offered to war $24" is then a sentence about almost no
        // turn in the run. These two are rates over the same denominator and
        // do not have that problem: how often war was handed enough to buy
        // anything, and how often it then bought something.
        long long warRich = 0;     // country-turns with >= $8 when war was asked
        long long warSpent = 0;    // country-turns where the war module spent
        // THE STANDING BILL, SPLIT. purseUpkeep is the sum of three very
        // different things and the split decides whether there is anything to
        // fix here: minority costs follow from which provinces are owned and
        // are largely not a decision, whereas the policy bill is the enact
        // head's own signature on a recurring commitment.
        double upkeepPolicy = 0.0;
        double upkeepMinority = 0.0;
        double upkeepPacify = 0.0;
        // THE WHOLE EXPENSE SIDE, not just the standing bill. The AI runs at
        // about 90% of gross in expenses where a strong opponent runs at 54%,
        // and the standing bill is only a third of that difference -- so the
        // split that matters is this one. Army and industry upkeep are
        // CONSEQUENCES of things the AI bought and are the two that can grow
        // without anybody deciding to grow them; research is a slider.
        double expArmy = 0.0;
        double expNavy = 0.0;
        double expIndustry = 0.0;
        double expResearch = 0.0;
        /**
         * OF THE PACT PROPOSALS THE HEAD CHOSE, HOW MANY WERE REAL.
         *
         * polChosen counts the DECISION; execPolitics then walks the frontier
         * and very often finds nobody it may ask -- already pacted, at war,
         * claimed, in talks, or inside a cooldown -- and returns "no suitable
         * target" having done nothing. That is a wasted country-turn which
         * still records a sample and still generates a gradient, which is
         * exactly the defect the minority actions were fixed for. Without this
         * pair the report cannot tell "the AI signs too many treaties" from
         * "the AI spends its politics turns asking nobody".
         */
        long long pactTried = 0, pactNoTarget = 0;
        /**
         * ACTIONS THE POLICY CHOSE THAT DID NOTHING AT ALL.
         *
         * Every real defect found in this AI so far has been this shape: the
         * mask offers an action, the head picks it, the executor walks the map,
         * finds nothing it may do, and returns a reason string -- and the
         * wasted country-turn is still recorded as a sample and still trains
         * the head that the action was safe and free. It was the port mask, the
         * pact mask, the minority actions before them, and it was 97.9% of
         * every pact proposal in the run this was added for.
         *
         * The executors already distinguish the two cases in their own labels:
         * a failure reads "<what>: <why>" and a success never does. didNothing
         * makes that machine-readable, so the report can name any action whose
         * chosen count and whose EFFECT count disagree, instead of the next one
         * being found by hand.
         *
         * Action 0 is the deliberate pass and is never counted here.
         */
        long long noopChosen[MOD_COUNT][MAX_MODULE_ACTIONS] = {};
        long long polOffered[POL_ACTIONS] = {0};
        long long polChosen[POL_ACTIONS]  = {0};
        long long navyOffered[NAVY_ACTIONS] = {0};
        long long navyChosen[NAVY_ACTIONS]  = {0};
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
    static_assert(AI_PLAN_HORIZON == N_STEP,
                  "the economy plans exactly as far ahead as it is scored");

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
    /**
     * RE-ENABLED 2026-08-26, with the evidence the note above asked for.
     *
     * It was parked at 1e11 -- effectively never -- because "no configuration
     * measured has been better with the critic on", and it had cost fourteen
     * points of play. That was measured against a learner whose PPO ratio was
     * computed against the wrong distribution and whose policy heads regularly
     * collapsed onto a single action; a critic cannot help a policy that is not
     * listening.
     *
     * Re-measured against the scripted rung, four paired seeds, 2 maps x 300
     * turns, same model both sides:
     *
     *     seed        off     on
     *     4242       0.31   0.37
     *     777        0.35   0.74
     *     20260801   0.32   0.26
     *     31337      0.26   0.43
     *     mean delta +0.140, better on 3 of 4
     *
     * Four seeds is thin and one of them went the other way, so this is
     * "demonstrated, worth keeping, re-check it" rather than settled. 10M is a
     * real warmup rather than a wall: the shipping model carries ~250M policy
     * updates and clears it easily, while a freshly reset head still defers to
     * the hand-written rule until it has learned something. OD_Q_WARMUP still
     * overrides for experiments.
     */
    static constexpr uint64_t Q_WARMUP_UPDATES = 10000000ULL;

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
     *
     * THIS IS NOW A FLOOR, NOT THE BUDGET. See actionsPerModule.
     */
    static constexpr int ACTIONS_PER_MODULE_PER_TURN = 3;

    /**
     * How many REQUESTS a country may put to other countries in one turn.
     *
     * The action budget is a throttle the player does not have -- they may
     * build in as many provinces as they can afford -- so uncapping it is
     * parity rather than a bonus. But one module's actions are not like the
     * others': economy and war spend the country's own money on its own
     * ground, while politics SENDS SOMETHING TO SOMEBODY. An uncapped politics
     * module proposes pacts, guarantees and trades until it runs out of
     * neighbours, which lands on the player as a mailbox full of offers and on
     * the other AIs as noise.
     *
     * So the cap moves from "how many things may I do" to "how many things may
     * I ask of other people", which is the part that has an audience. Two is a
     * country pursuing an agenda; ten is a country nobody wants to hear from.
     */
    static constexpr int AI_REQUESTS_PER_TURN = 2;
    /**
     * ...AND THE CEILING, so a continent-sized empire cannot think for a second
     * a turn.
     *
     * EIGHT, NOT TWELVE, and one extra go per FORTY provinces rather than per
     * twenty-five. The aggressive version was measured on the shipped model and
     * cost it a third of everything it holds -- land against the scripted
     * player fell from 1.86x to 0.91x, with bankruptcy up from 0.8% to 1.3% of
     * country-turns. A wider budget is only worth having to a policy that knows
     * what to do with it, and this one was trained under the flat three: given
     * nine more goes it simply does more of whatever it already over-uses. The
     * gentler ramp still removes the worst of the cap -- a 200-province empire
     * goes from 3 decisions to 8 -- and leaves less room to compound a bad
     * habit before training catches up.
     */
    static constexpr int ACTIONS_PER_MODULE_MAX = 8;
    /**
     * HOW MANY THINGS THIS COUNTRY MAY DO, which is not the same for a city
     * state and for an empire.
     *
     * ── THE ONE ASYMMETRY THAT RAN AGAINST THE AI ──
     *
     * Everything else in this file is written to the rule that something a
     * player cannot do is something the AI cannot do. This constant was the
     * reverse, and it was the largest single cap on how well the AI could
     * possibly play. A human holding two hundred provinces spends two hundred
     * provinces' worth of clicks in their turn: they paint the bulk brush over
     * a continent and commit thirty upgrades at once (commitBulkSelection),
     * move every army, and queue every order. The AI holding the same two
     * hundred provinces got THREE economy decisions, the same three a country
     * holding one province gets.
     *
     * Measured on the shipped scenarios: 0.16 industry levels built per
     * country-turn. No amount of training climbs over that, because it is not a
     * thing the policy is choosing -- it is the number of times the policy is
     * asked.
     *
     * So the budget scales with the size of the thing being governed, which is
     * what a player's own budget does. Deliberately sub-linear and clamped: the
     * point is parity with a human's attention, not a bonus, and every action
     * is still a training sample and still costs a trunk pass, so this is also
     * the AI's think-time bill.
     *
     * NOT a difficulty lever. A handicap removed at Insane and left in place at
     * Easy would make the lower difficulties dishonest in a different
     * direction; the ladder earns its rungs from faculties instead (see
     * DifficultyProfile).
     */
    int actionsPerModule(int cid) const;
    /**
     * How strongly the world is ganging up, 0..1. Zero unless this rung of the
     * ladder does coalitions at all and somebody is genuinely running away with
     * the map. See COALITION_SHARE.
     */
    float coalitionPressure() const;
    /**
     * The standing coalition, if one has formed. Rebuilt once a turn by
     * updateCoalition; see COALITION_ODDS for what forming requires.
     */
    struct CoalitionState {
        int target = -1;
        int formedTurn = -1;
        long long combinedArmy = 0;
        std::unordered_set<int> members;
    };
    const CoalitionState& coalition() const { return m_coalition; }
    /** Is this country in a coalition that is currently in its window? */
    bool inCoalition(int cid) const {
        return m_coalition.target >= 0 && m_coalition.members.count(cid) > 0 &&
               (m_turn - m_coalition.formedTurn) < COALITION_WINDOW;
    }
    void updateCoalition();
    /** Embedding ++ one-hot(module, action), the dynamics head's input. */
    static std::vector<float> dynamicsInput(const std::vector<float>& emb,
                                            int module, int action);
    /** Is the forward model trained enough to plan with? See DYN_WARMUP_UPDATES. */
    bool searchReady() const {
        return difficulty().searchDepth > 0 &&
               m_dynamics.updateCount() >= DYN_WARMUP_UPDATES;
    }
    /**
     * Model-based scores for every valid action, or empty when the search is
     * not available. Blended into the logits exactly as the Q head is.
     */
    void searchScores(int module, const std::vector<float>& emb,
                      const std::vector<bool>& valid, std::vector<float>& out);
    /** What a PREDICTED embedding is worth. Read through Q, which is the head
     *  that takes an embedding -- see the definition for why not m_value. */
    float embeddingValue(int module, const std::vector<float>& emb) const;
    /** Is `other` the power `cid` should be treating as the problem? Never true
     *  of oneself, and never true below the threshold. */
    bool isCoalitionTarget(int cid, int other) const;

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
        /** Overtures refused inside this window. See AI_OVERTURE_REFUSED_CHARGE. */
        int overturesRefused = 0;
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
            /// The legality mask this action was chosen under. See validMask
            /// on the outer Experience.
            std::vector<uint8_t> validMask;
            /// The behaviour mixture this action was drawn from; see
            /// Experience::mixScale.
            float mixScale = 1.0f, mixFloor = 0.0f;
            /// What the scripted teacher would have done here, or -1. See
            /// Experience::teacher.
            int teacher = -1;
            /// True when the opening book chose this, not the policy. See
            /// Experience::fromBook.
            bool fromBook = false;
        };
        std::vector<ExtraAction> extras;
        /**
         * WHICH ACTIONS WERE LEGAL when each slot's action was taken.
         *
         * The behaviour policy is a softmax over masked logits, so the update
         * has to renormalise over the same support or its ratio compares the
         * played distribution against one that includes actions the mask had
         * already deleted. For the war module -- where most turns offer two or
         * three legal actions out of the head's full set -- the unmasked
         * distribution is mostly mass the decision never saw.
         */
        std::vector<uint8_t> validMask[MOD_COUNT + 2];
        /**
         * The BEHAVIOUR policy each slot's action was drawn from, as
         * p_behaviour = mixScale * p_policy + mixFloor.
         *
         * Training acts through an epsilon-mixture, so this is not the policy;
         * the update needs both ends measured the same way or the importance
         * ratio silently becomes "how much did exploration over-sample this",
         * which crushes exactly the rare actions it exists to surface. See
         * NeuralNet::accumulatePPOInto.
         */
        float mixScale[MOD_COUNT + 2] = {1, 1, 1, 1, 1, 1};
        float mixFloor[MOD_COUNT + 2] = {0, 0, 0, 0, 0, 0};
        /**
         * WHAT THE SCRIPTED TEACHER WOULD HAVE DONE in this state, or -1.
         *
         * Recorded at decision time -- the teacher has to be asked while the
         * world still stands where the decision stood -- but APPLIED with the
         * batch, as one cross-entropy gradient beside the policy gradient.
         * Applying it at decision time instead is what collapsed the war head:
         * see BC_LR.
         */
        int teacher[MOD_COUNT + 2] = {-1, -1, -1, -1, -1, -1};
        /**
         * THE OPENING BOOK CHOSE THIS, not the policy.
         *
         * Such a sample carries no policy-gradient information: the behaviour
         * was deterministic, so the importance ratio pi(a|s)/b(a) has b = 1 and
         * bears no relation to the pi_old/pi_new the surrogate is built from.
         * Recording the policy's own log-prob instead made a forced move look
         * like a sampled one, which is off-policy data entering PPO disguised
         * as on-policy.
         *
         * They are still worth everything else they carry -- the value target,
         * the Q target, and the teacher above, for which a demonstration is
         * exactly the right kind of data. Only the surrogate skips them.
         */
        bool fromBook[MOD_COUNT + 2] = {false, false, false, false, false, false};
        // pre-turn snapshot for reward deltas
        int provinces = 0;
        double treasury = 0;
        /**
         * The RECURRING political bill at window start: doctrines, minority
         * settlements, pacification, per turn.
         *
         * Measured 2026-08-20 with the money ledger: every model that trained
         * itself bankrupt had grown these three channels 700-800x while they
         * were ~1.5% of the healthy baseline's spending -- and the politics
         * module, which signs all three, was never charged for any of it. The
         * unrest relief a doctrine buys lands inside the reward window; the
         * standing cost runs forever after, mostly outside it, priced only
         * through the shared `broke` term at a third the weight the economy
         * head carries. The delta over the window makes the module SEE the
         * bill it signed. See rewards[MOD_POLITICS].
         */
        float polUpkeep = 0;
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
     * ── g(embedding, action) -> embedding: THE FORWARD MODEL ──
     *
     * The note on m_q above says exactly what was missing and why: "without a
     * forward model -- the game cannot cheaply answer 'what would the world
     * look like if I declared war on France' -- that estimate has to be LEARNED
     * rather than searched for", and Q is "the model-free half of what a
     * one-ply search would buy". This is the other half.
     *
     * It does not predict the world. It predicts the TRUNK'S VIEW of the world,
     * which is a 320-float embedding rather than a map with sixteen hundred
     * provinces on it, and that is the only reason searching is affordable at
     * all: a ply costs one small matrix multiply instead of a turn of the game.
     * The trick is MuZero's -- a latent model needs only to be accurate about
     * the things the value head reads, not about everything.
     *
     * Input is the embedding with a one-hot of (module, action) appended, so
     * one net covers all four modules and every action shares its statistics.
     * Output is the next embedding, and it is trained as a plain regression
     * against the trunk's own embedding of the state that actually followed.
     *
     * THE TRUNK IS NOT TRAINED THROUGH IT. The target is taken with a
     * stop-gradient, because a trunk that is rewarded for being PREDICTABLE
     * learns to throw away the things that are hard to predict -- which are
     * exactly the things worth planning about.
     */
    NeuralNet m_dynamics;
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
    /// The mixture the last pickAction() drew from. See Experience::mixScale.
    float m_lastMixScale = 1.0f, m_lastMixFloor = 0.0f;
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
        /// Legality mask the sample was taken under; empty = unmasked head.
        std::vector<uint8_t> validMask;
        /// The behaviour mixture; see Experience::mixScale.
        float mixScale = 1.0f, mixFloor = 0.0f;
        /// See Experience::teacher and Experience::fromBook.
        int teacher = -1;
        bool fromBook = false;
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
        /// For a dynamics sample (module == MOD_COUNT + 2): which module's
        /// action `action` refers to. `module` itself carries the sentinel.
        int dynModule = -1;
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
        /// The dynamics head, and a second trunk scratch for its target state.
        NeuralNet::Scratch dynamics;
        NeuralNet::Scratch trunkNext;
        NeuralNet::Scratch target;
        NeuralNet::Scratch attack;
        NeuralNet::Scratch diplo;
        NeuralNet::Scratch diploValue;
        NeuralNet::Scratch diploValueNext;
        NeuralNet::Scratch stance;
        std::vector<NeuralNet::Scratch> relEnc, relSco;
        /**
         * This worker's share of the batch's health numbers, merged with the
         * gradients. Per-thread on purpose: the existing UpdTrace writes shared
         * counters and therefore forces the single-threaded path, which is an
         * acceptable price for an opt-in diagnostic and not one for a guard
         * that has to be on for every run.
         */
        struct HeadStats {
            double entropySum = 0;   ///< nats, summed over samples
            double ceilingSum = 0;   ///< log(support), the entropy's own ceiling
            double klSum      = 0;
            long long n = 0;
        };
        HeadStats head[MOD_COUNT];
        bool ready = false;
    };
    std::vector<WorkerScratch> m_scratch;

    /**
     * Per-module entropy coefficient, moved by the collapse guard.
     *
     * Starts at PPO_ENTROPY and stays there unless a head's mean entropy falls
     * under ENTROPY_FLOOR_FRAC of its ceiling. Not persisted with the model: a
     * resumed run re-measures the policy it actually loaded rather than
     * inheriting a correction for a state it may no longer be in.
     */
    float m_entropyCoef[MOD_COUNT] = {PPO_ENTROPY, PPO_ENTROPY, PPO_ENTROPY, PPO_ENTROPY};
    /// Mean entropy and its ceiling per module, last batch. For the log.
    float m_headEntropy[MOD_COUNT] = {0, 0, 0, 0};
    float m_headCeiling[MOD_COUNT] = {0, 0, 0, 0};
    float m_headKl[MOD_COUNT]      = {0, 0, 0, 0};
    /**
     * How far under the guard's target each head sat last batch, as a fraction
     * of the target: 0 = healthy and the guard is entirely inert, 1 = the head
     * has no entropy left at all. Written between batches and read by every
     * worker during one, so no synchronisation is needed.
     */
    float m_headDeficit[MOD_COUNT] = {0, 0, 0, 0};
    /// Batches this head has spent under its floor. Reported so a run that is
    /// being held up by the guard is distinguishable from one that never
    /// needed it.
    long long m_collapseBatches[MOD_COUNT] = {0, 0, 0, 0};
    /**
     * Decayed counts of what each head was OFFERED and what it CHOSE.
     *
     * The marginal the guard reads. Counted only for decisions the learning
     * policy actually made -- not the scripted cohort, not the frozen league,
     * not the opening book -- because the question is what THIS policy does
     * with its choices, and a booked turn is not one of its choices.
     *
     * Written during the serial decision phase and read in the serial guard,
     * so no synchronisation is needed.
     */
    double m_marginalChosen[MOD_COUNT][MAX_MODULE_ACTIONS] = {};
    double m_marginalOffered[MOD_COUNT][MAX_MODULE_ACTIONS] = {};
    /// Marginal entropy and its ceiling per module, last batch. For the log.
    float m_marginalH[MOD_COUNT]       = {0, 0, 0, 0};
    float m_marginalCeiling[MOD_COUNT] = {0, 0, 0, 0};
    static constexpr long long GUARD_LOG_BATCHES = 200;
    long long m_learnBatches = 0;
    /// Merged across workers each batch, then consumed by the guard.
    struct BatchHead { double entropySum = 0, ceilingSum = 0, klSum = 0; long long n = 0; };
    BatchHead m_batchHead[MOD_COUNT];
    /// What the trust region asked for last batch; multiplies the policy step.
    float m_klStepScale = 1.0f;
    /**
     * How often the teacher has named each action, per module.
     *
     * The clone is weighted by the inverse square root of this, so a branch the
     * script takes once in a hundred turns is not worth a hundredth of one it
     * takes every turn. Square root rather than inverse: full inverse-frequency
     * weighting makes the rarest action the loudest thing in the batch, which
     * is its own instability.
     */
    double m_teacherCount[MOD_COUNT][MAX_MODULE_ACTIONS] = {};
    double m_teacherTotal[MOD_COUNT] = {0, 0, 0, 0};
    /**
     * Weight for one cloning sample: class-balanced, never negative.
     *
     * `fromBook` separates the two kinds of demonstration. A booked move is the
     * ONLY thing its sample can now teach the policy head -- the surrogate
     * skips it, because the book is deterministic and there is no behaviour
     * distribution to measure a ratio against -- so it is cloned whether or not
     * OD_BC_FROM_SCRIPT is set, and it is not annealed: the book already stops
     * on its own at AI_OPENING_TURNS. A cloned move from the teacher proper is
     * an optional extra opinion on a decision the policy made itself, so it is
     * both opt-in and annealed away.
     */
    float bcSampleWeight(int module, int teacherAction, bool fromBook) const;
    /// Runs the collapse guard over a finished batch. Returns the step scale
    /// the KL trust region asks for (1.0 = take the step as computed).
    float applyStabilityGuards();
    /** Worker count: cores minus one, scaled by the resource limiter. */
    int learningThreads() const;
    /** Drains m_work across `learningThreads()` workers and merges gradients. */
    void runLearningWork();

    std::deque<Decision> m_log;
    /** See nextPortBuy: one answer per country per turn. */
    struct PortBuy { int turn = -1; int pid = -1; float cost = 0.0f; };
    /** See pactTargets: whether each of the three pacts has anyone to ask. */
    struct PactTargets { int turn = -1; bool any[3] = {false, false, false}; };
    /** See attackCandidates: the winnable assaults, scanned once a turn. */
    struct AttackScan { int turn = -1; std::vector<AttackCandidate> cands; };
    /** See enactablePolicy: one answer per country per turn. */
    struct EnactPick { int turn = -1; const Policy* policy = nullptr; };
    /** See shipsWithDestination: one scan per country per turn. */
    struct ShipScan { int turn = -1; std::vector<int> ships; };
    mutable std::unordered_map<int, PortBuy> m_portBuyCache;
    /** See pactTargets: one answer per country per turn. */
    mutable std::unordered_map<int, PactTargets> m_pactTargetCache;
    /** See attackCandidates: one scan per country per turn. */
    mutable std::unordered_map<int, AttackScan> m_attackScanCache;
    mutable std::unordered_map<int, EnactPick> m_enactCache;
    mutable std::unordered_map<int, ShipScan> m_shipScanCache;
    /** Cleared before every exec call, set by didNothing. See noopChosen. */
    bool m_execNoop = false;
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
    /** Requests each country has made this turn. See AI_REQUESTS_PER_TURN. */
    std::unordered_map<int, int> m_requestsThisTurn;
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
    /// States awaiting their map's outcome. See VALUE_MC_WEIGHT.
    struct OutcomeSample { std::vector<float> features; int cid; };
    std::vector<OutcomeSample> m_outcomeBuf;
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
    CoalitionState m_coalition;
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
    /**
     * THE NEXT PORT THIS COUNTRY WOULD ACTUALLY BUY, and what it would cost.
     *
     * Mirrors execEconomy's port case exactly, because it IS that decision:
     * upgrade the first owned harbour still under the researched cap, else
     * found one in the most populous coastal province that has none. The mask,
     * the savings reserve and the executor all ask this, so none of the three
     * can believe in a purchase the others do not.
     *
     * The mask used to offer "build a port" whenever the treasury held $60 --
     * with no question of whether a port could be built at all -- so a country
     * with every harbour already at its cap was offered the action, chose it,
     * and got back "port: no candidate". A wasted decision that still generated
     * a gradient, which is the same defect the minority actions were fixed for.
     *
     * Cached per country per turn: the fallback branch tests coastal-ness,
     * which is a bounded BFS, and the mask is rebuilt up to three times a turn.
     */
    bool nextPortBuy(int cid, int& outPid, float& outCost) const;
    /**
     * WHAT THE EXECUTOR WOULD ACTUALLY BUY, asked by the mask as well.
     *
     * Same rule as nextPortBuy and pactTargets, for the same measured reason.
     * The economy mask tested a cheap proxy -- "some province is under the cap
     * and the treasury clears IND_COST[1]" -- while the executor picks a
     * SPECIFIC province and pays THAT province's next-level price, which for a
     * developed one is many times the level-1 cost. So the action was offered,
     * chosen, and answered "cannot afford": 51.3% of every industry decision in
     * a 400-turn run did nothing.
     *
     * Specialisation was worse. Its mask asked for $2; the real price is
     * IND_COST[level] x 1.5, from $12 upwards. It succeeded ZERO times out of
     * sixteen. A mask that cannot be failed is not a mask.
     *
     * Deliberately NOT cached, unlike nextPortBuy. A module acts up to
     * ACTIONS_PER_MODULE_PER_TURN times a turn and each pick changes what the
     * next one may buy, so a per-turn answer would go stale inside the turn and
     * manufacture the very no-op it exists to remove. The scan is a walk over
     * the country's provinces with two hash lookups -- the executor was already
     * paying it on every turn this action was chosen.
     */
    bool nextIndustryBuy(int cid, int& outPid, int& outLevel, float& outCost) const;
    bool nextSpecBuy(int cid, int& outPid, const char*& outRes, float& outCost) const;
    /** The port province this country would load troops at, and its garrison.
     *  The navy mask tested the country's WHOLE army against 1,000 while the
     *  executor needs 1,000 standing at one port with no embarkation already
     *  pending there -- 67.8% of embark decisions did nothing. */
    bool bestEmbarkPort(int cid, int& outPid, int& outGarrison) const;
    /**
     * IS THERE ANYBODY THIS COUNTRY MAY EVEN ASK, for each of the three pacts?
     *
     * The mask for actions 5-7 used to be "has a neighbour, and has its
     * overture budget", on the reasoning that WHO to ask is the executor's job
     * and repeating its frontier walk in the mask would cost that walk on every
     * country's turn instead of only on the turns the action is chosen.
     *
     * Measured, that reasoning is backwards: the action IS chosen, constantly,
     * and 97.9% of the time the executor walks the frontier and finds nobody --
     * 7,564 no-ops out of 7,725 proposals in a 400-turn run. The walk was
     * already happening on almost every one of those turns; all the mask bought
     * by skipping it was a wasted country-turn that still recorded a sample and
     * still generated a gradient. Exactly the defect the minority actions were
     * fixed for, in the same file.
     *
     * So the cheap half of the executor's filter -- not at war, not already in
     * this relation, not somebody whose land we claim, nothing pending, off
     * cooldown -- runs here, ONCE per country per turn, and both the mask and
     * the executor read it. The expensive half (predictAcceptance, a net pass
     * per candidate) stays in the executor, where it only runs on turns the
     * action was actually taken.
     */
    const PactTargets& pactTargets(int cid) const;
    /**
     * EVERY ASSAULT THIS COUNTRY COULD WIN THIS TURN, asked by the mask as well
     * as by the executor.
     *
     * The war mask offered `attack` whenever the country was at war anywhere
     * and had an army. The executor then walks every frontier, needs 500 men
     * standing on one of them (150 against rebels), an adjacent enemy province,
     * and a margin over 1.05 once fortification and the defender's own defence
     * research are priced in -- and most of the time there is no such assault.
     * Measured over 400 turns of the shipped scenarios: 78% of every attack
     * decision did nothing, about 5,400 wasted country-turns, each of them
     * still recorded as a training sample teaching the head the action was free.
     * The largest single waste in the AI, and the same defect as the port mask,
     * the pact mask and the minority actions before them.
     *
     * CACHED PER COUNTRY PER TURN. The scan is frontiers x neighbours with a
     * garrison lookup each, and a module now gets up to ACTIONS_PER_MODULE_MAX
     * goes in a turn -- but nothing it reads changes inside the turn. Committing
     * an assault queues a move ORDER; it does not move the men until the
     * resolver runs, so garrisons, fortifications and margins are all still
     * what they were. What DOES change is which launch provinces are spoken
     * for, and that is re-derived from m_pendingMoveOrders on every call rather
     * than cached -- see attackAvailable.
     */
    const std::vector<AttackCandidate>& attackCandidates(int cid) const;
    /** Is there a candidate whose launch province is not already carrying an
     *  order? The mask's question, and the first thing the executor's issuing
     *  loop asks of each candidate. */
    bool attackAvailable(int cid) const;
    /**
     * The doctrine this country would enact, or nullptr.
     *
     * The politics mask asked only whether the game HAS doctrines, while the
     * executor needs one that clears canCountryEnactPolicy AND fits inside what
     * is left of AI_DOCTRINE_BUDGET_SHARE. Measured after the attack mask was
     * fixed and this became the largest remaining waste: 1,741 of 1,788 enact
     * decisions did nothing, 97.4%.
     *
     * Cached per country per turn like the others, and invalidated when a
     * doctrine is actually enacted -- that is the one thing inside a turn which
     * changes the answer, because it spends the budget the next one would need.
     */
    const Policy* enactablePolicy(int cid) const;
    /**
     * Is there an allied crossing this country could actually stage into?
     *
     * The mask tested `army > 500` -- the WHOLE army -- while the executor
     * needs 500 men standing on the specific crossing province, and that
     * province not already carrying an order. 85.7% of stage decisions did
     * nothing. Cheap enough to run outright: it walks the staging list, which
     * refreshStats has already built.
     */
    bool stageAvailable(int cid) const;
    /**
     * Where this hull would sail, if anywhere: the nearest at-war enemy port,
     * else the nearest friendly one it is not already sitting on.
     *
     * ONE RULE for the mask and execNavy's move case. The mask tested only
     * `ships > 0`, so a fleet with nothing to sail to -- at peace, every hull
     * already on station -- was offered the action and did nothing 48% of the
     * time.
     */
    bool shipDestination(int cid, double fromLon, double fromLat,
                         int& outPid, double& outLon, double& outLat) const;
    /** Ship indices that have somewhere to go, scanned once a turn. Hulls do
     *  not move until the resolver runs, so the answer is stable inside a turn;
     *  which of them are already under orders is not, and is re-derived. */
    const std::vector<int>& shipsWithDestination(int cid) const;
    bool navyMoveAvailable(int cid) const;
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
    /** Updates the forward model has had, and whether that is enough to plan
     *  with yet. Reported by the bench so "search is off" is visible rather
     *  than inferred from behaviour that looks identical either way. */
    unsigned long long dynamicsUpdates() const { return m_dynamics.updateCount(); }
    bool searchActive() const { return searchReady(); }
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
    /** Buys the fort the economy head never buys. See the definition. */
    void fortifyReflex(int cid);
    /**
     * ── THE NAVY BILL IS WASTE FOR A LAND POWER AND DEFENCE FOR A COASTAL ONE ──
     *
     * The seat bench shows a country being bled by a fleet it never uses.
     * Modern China over 120 turns: navy 53.88 a turn against an ARMY of 2.70,
     * on gross 248.53, expenses at 99.4% of income, industry unaffordable on
     * 87.8% of the turns it was wanted -- annihilated, scoring 5 out of 100,
     * while paying twenty times more for hulls than for the army being
     * destroyed. The `scrap` mask already offers exactly this and says why; the
     * head takes it on 1.4% of the turns. It reads like the fort all over again.
     *
     * TRIED, and it is a net loss: pay off the dearest uncrewed warship when the
     * navy bill is both large (>15% of gross) and unaffordable (net under 5%).
     *
     *     seat            before   after
     *     modern China         5      37
     *     1914 Sweden        120      23
     *     1939 Norway        144      44
     *     rating             129      97   (better on 1 seat of 6)
     *
     * China gains sevenfold and the small coastal countries are destroyed,
     * because for them the fleet IS the defence -- Sweden and Norway are not
     * paying for idle hulls, they are paying for the thing that keeps an army
     * off their coast. On the ledger the two cases are identical: a big navy
     * bill and no money. Nothing in the income snapshot separates "this fleet
     * is useless" from "this fleet is why I am still alive".
     *
     * So this is NOT a reflex-shaped problem. fortifyReflex works because
     * "fortify the border an enemy army is standing on" is unambiguous wherever
     * you are; "sell a warship" depends on geography the reflex cannot see. A
     * fix needs the fleet's actual USE -- landings made, coasts held, invasions
     * turned back -- not its price. See the fleetUseful term in the reward,
     * which already tries to express this for training.
     */
    /**
     * Turns of net loss the treasury can absorb before austerity starts.
     *
     * Cutting on the first bad turn would have a country cancelling a doctrine
     * because it built something last turn. Cutting only at zero would be the
     * bankruptcy cascade, which is the thing this exists to avoid.
     */
    static constexpr double AI_AUSTERITY_RUNWAY_TURNS = 8.0;

    /**
     * What share of the scripted training cohort plays something OTHER than the
     * ordinary aggressor rung. See the note in takeTurn.
     *
     * Zero restores the single-opponent training every model before this was
     * raised on, which is what makes this a one-constant experiment. A quarter
     * is deliberately modest: the point is for the policy to MEET a blitz, a
     * tech rush, a pact hub and a turtle often enough to have an answer to
     * them, not to retune the whole reward landscape by making the control
     * cohort harder.
     *
     * UNMEASURED AT THE TIME OF WRITING, and it cannot be measured the way
     * everything else in this file was: it changes what training SEES, so the
     * only honest test is to train a model under it and bench that model
     * head-to-head against one trained without it (tools/ai_bench.py
     * --vs-model). The keep-the-winner guard in tools/train_parallel.py will
     * reject the merge if it regresses, which is what makes it safe to leave
     * on while that runs.
     */
    static constexpr float AI_TRAINING_VARIANT_SHARE = 0.25f;



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
    /** Overtures this country made that were refused this turn, accumulated
     *  into every open window exactly as landings are. See
     *  AI_OVERTURE_REFUSED_CHARGE. */
    std::unordered_map<int, int> m_overturesRefusedThisTurn;
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
