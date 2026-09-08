#pragma once

// Countries that answer their own mail, written by a language model.
//
// WHAT THIS IS AND IS NOT
//
// It is a correspondent, not a player. It does not move armies, set budgets or
// declare war -- the existing AI does all of that and keeps doing it. This only
// writes letters. That division is deliberate: the AI is measured, tuned and
// deterministic, and handing a language model the levers would throw all three
// away for something nobody can bench. What a model is genuinely good at is the
// thing the AI has never been able to do at all, which is talk.
//
// SO IT IS ALLOWED TO LIE
//
// A diplomat who cannot lie is not a diplomat, and a correspondent who always
// tells you what it is really planning is a hint system. Advisors may bluff,
// flatter, mislead and make promises their country will not keep. What they may
// never do is pretend to be a person: every letter carries the bot tag, in
// every language, on every screen that renders it. The lying is a game
// mechanic; concealing the speaker would be a lie about the game.
//
// THREE THINGS THAT MUST HOLD, AND ARE TESTED
//
//   1. CONTEXT IS PER CORRESPONDENCE. The model writing as Britain to France
//      sees the Britain-France thread and nothing else. Not Britain's other
//      threads, not the world state it has no business knowing. A model that
//      quotes a letter it was never sent has broken the game.
//   2. IT ANSWERS IN THE PLAYER'S LANGUAGE. The game ships in twenty-one; an
//      advisor that only speaks English is a feature for a fifth of the players.
//   3. IT IS OPTIONAL AND ABSENT BY DEFAULT. No module, no advisors, no Mail
//      button, and a game that plays exactly as it did before.
//
// WHERE THE MODEL RUNS
//
// Not here. This speaks HTTP to a runner -- one the player installed through
// the game, or one they already had, or a remote API they hold a key for. The
// game ships no model and no inference code, which is what makes "works on any
// platform" true rather than aspirational. See llm/Runner.h.

#include <string>
#include <vector>

namespace mail { class Box; struct Message; }

namespace llm {

/// One turn of the conversation, as a chat API wants it.
struct Turn {
    /// "system", "user", "assistant" or "tool".
    std::string role;
    std::string content;
    /// Set only on a "tool" turn: which call this answers. Some runners require
    /// it and some ignore it, so it is sent whenever it was given.
    std::string toolCallId;
    std::string toolName;
};

/// Who the advisor is being, and who it is writing to.
struct Persona {
    std::string countryName;      ///< the country the model writes AS
    std::string correspondent;    ///< who it is writing TO
    /// Free text a scenario may set: "recently humiliated at the conference".
    /// Empty by default -- flavour a mapmaker can add, not a requirement.
    std::string standing;
    /// Whether the correspondent is another advisor rather than a person.
    /// Advisors write to each other, and knowing which is which changes the
    /// register -- you do not flatter a machine the way you flatter a king.
    bool toAnotherAdvisor = false;
};

/**
 * What the model is told about the world.
 *
 * EVERY FIELD IS A WORD, NEVER A NUMBER, and that is the rule that keeps an
 * advisor feeling like a correspondent rather than a readout. A minister who
 * can quote your exact army size is reading the save file, and a player spots
 * it in one letter. What a real foreign ministry would plausibly know -- who is
 * fighting whom, whether we share a border, whether things have been getting
 * better or worse -- is fair game and is what makes the letters specific
 * instead of generic.
 */
struct Situation {
    int  turn = 0;
    std::string date;
    bool atWar = false;           ///< between these two countries specifically
    /// "considerably stronger than you", "comparable to you", ...
    std::string relativeStrength;

    // ── Added because the letters were vague without them ──

    /// "we share a border", "we are neighbours by sea", or empty.
    std::string proximity;
    /// What standing agreement exists: "an alliance", "a non-aggression pact".
    std::string pact;
    /// Wars either side is fighting with somebody else, named: "they are at war
    /// with Russia and Serbia". Empty when there are none.
    std::string theirWars;
    std::string ourWars;
    /// Whether this has been going well or badly lately: "we have been losing
    /// ground", "we have been gaining". Empty when nothing has changed.
    std::string ourFortunes;
    /// How the correspondence itself has gone: "you have written often and
    /// warmly", "this is the first letter between you". Never a number.
    std::string historyWithThem;
};

/**
 * The instruction that makes an advisor an advisor.
 *
 * Public because it is the single most important string in this module and it
 * should be readable, reviewable and testable rather than buried in a request
 * builder.
 */
std::string systemPrompt(const Persona& persona, const Situation& situation,
                         const std::string& languageName);

/**
 * Build the whole conversation for one reply.
 *
 * `thread` is ONE correspondence. Passing a box, or the world, would be the
 * bug this signature exists to prevent -- there is no parameter here through
 * which another country's letters could arrive.
 *
 * `me` is the country the model writes as, so that direction can be worked out:
 * letters from `me` become "assistant" turns and everything else becomes
 * "user". Getting that backwards produces a model that argues with itself.
 */
std::vector<Turn> buildConversation(const std::vector<mail::Message>& thread, int me,
                                    const Persona& persona, const Situation& situation,
                                    const std::string& languageName,
                                    size_t maxLetters = 24);

/**
 * Trim a model's reply into something that can be a letter.
 *
 * Models like to explain themselves, wrap answers in quotation marks, and
 * prefix them with the name of the character they are playing. None of that is
 * a letter. Also caps the length, because a model that runs on is a model that
 * fills the screen.
 */
std::string tidyReply(std::string text, const std::string& countryName);

// ─────────────────────────────────────────────── letting it look things up ────
//
// The Situation above is what an advisor is TOLD. This is what it can ASK.
//
// WHY BOTH. Pre-baking every fact into the instruction has two costs: it is a
// wall of text for a small model to wade through on every letter, and it fixes
// in advance what the model might have wanted to know. Tools invert that -- the
// instruction stays short, and a minister writing about the Balkans can look up
// the Balkans. It also makes the letters feel considered rather than reactive,
// because the model really has gone and checked before answering.
//
// THE RULES ARE THE SAME AS EVERYWHERE ELSE HERE.
//
//   * READ ONLY. Nothing a tool call can do changes the world. A model that can
//     move an army is a model holding the levers, which is exactly what this
//     module is built not to do.
//   * WORDS, NOT NUMBERS. Every answer is the same coarse vocabulary the
//     Situation uses. A tool that returned "412,000 men" would hand the model
//     the save file through the back door.
//   * ONLY WHAT A MINISTRY WOULD KNOW. Who is at war, who has signed what, who
//     borders whom. Never another country's orders, never another
//     correspondence.
//
// AND ONE ASYMMETRY, WHICH IS THE POINT OF HALF OF THESE.
//
// What a country knows about ITSELF and what it knows about its neighbours are
// not the same thing, and flattening them makes both wrong. A foreign minister
// cannot tell you where the Russian army is massed; he can absolutely tell you
// where his own is, because he signed the order. So the `our_*` tools name
// PLACES -- which frontier is held, which district is restless, which province
// is claimed -- while the tools about somebody else stay at "considerably
// stronger than you".
//
// The numbers rule does not bend for either. A minister knows his own army in
// detail and still says "the bulk of it stands on the Prussian border", never
// "412,000 men", because the second is a save file and the first is a letter.
//   * BOUNDED. A fixed number of rounds per letter, because a model that can
//     ask questions is a model that can ask them forever.

/// One tool the model may call, and what it is for.
struct Tool {
    const char* name;
    const char* description;
    /**
     * The single argument this one takes, or null when it takes none.
     *
     * The `our_*` tools ask about the country the model IS, so there is
     * nothing to name -- and a null here has to be handled in toolsJson,
     * because a function advertised with a malformed empty schema gets an
     * argument invented to fill it.
     */
    const char* argName;
    const char* argDescription;
    /**
     * True for the one tool that does not answer a question but records one.
     *
     * THIS IS THE EXCEPTION TO "READ ONLY", AND IT IS A NARROW ONE. Every
     * other tool here reads the world and changes nothing. This one changes
     * nothing about the world either -- it records how the correspondence left
     * THIS COUNTRY FEELING ABOUT THIS CORRESPONDENT, which is a fact about the
     * advisor's own mind and about nothing else.
     *
     * It matters because without it the module is decorative: the letters read
     * well and no decision anywhere is different for having been written. What
     * this feeds is a bounded thumb on the scale in the diplomacy the existing
     * AI already does -- see Game::llmDispositionToward. It cannot move an
     * army, cannot sign anything, and cannot overrule a refusal the AI would
     * have made on the merits.
     *
     * Answers from a recording tool are not precomputed, because there is
     * nothing to look up: the worker handles it and carries the result home.
     */
    bool records = false;
};

/// The tools offered. Fixed, small, and read-only.
const Tool* tools(int* count);

/// The `tools` array for a chat request, as JSON.
std::string toolsJson();

/** One call the model asked for. */
struct ToolCall {
    std::string id;
    std::string name;
    std::string argument;   ///< the one argument, already unquoted
};

/**
 * Pull tool calls out of a chat completion.
 *
 * Empty when the model just wrote a letter, which is the ordinary case and not
 * an error -- a model that never uses a tool is a model that did not need one.
 */
std::vector<ToolCall> toolCallsFromResponse(const std::string& json);

/// A tool's answer, as a turn to send back.
Turn toolResultTurn(const ToolCall& call, const std::string& answer);

/// The request body with tools offered. Otherwise identical to chatRequestBody.
std::string chatRequestBodyWithTools(const std::vector<Turn>& turns,
                                     const std::string& model, int maxTokens = 220);

/**
 * How many times a letter may go round the ask-and-answer loop.
 *
 * Three is enough for "who are they at war with, do we border them, what have
 * we signed" and short enough that a confused model cannot spend a minute of
 * somebody's turn on it.
 */
constexpr int kMaxToolRounds = 3;

/**
 * How far one letter may move a standing disposition.
 *
 * Three consistent letters saturate it. That is the whole design: a player who
 * has genuinely argued a country round gets all of the (small) effect, and a
 * player who sends thirty identical letters gets no more than the player who
 * sent three. Persuasion, not attrition.
 */
constexpr float kDispositionStep = 0.34f;

/**
 * Fold one letter's stance into the standing disposition. Pure.
 *
 * `stance` is -1, 0 or +1. The result is always within [-1, 1] however many
 * times it is applied, which is the property the bias in decideDiplomacy
 * depends on to stay bounded -- see AISystem::AI_LLM_DISPOSITION.
 */
float foldDisposition(float current, int stance);

/// Where a runner lives and how to talk to it.
struct Endpoint {
    /// Base URL, e.g. "http://127.0.0.1:11434/v1". Empty means no module.
    std::string baseUrl;
    std::string model;
    /// Only for a remote API. Never sent to a non-local endpoint the player did
    /// not type themselves -- see Advisor.cpp.
    std::string apiKey;
    int timeoutMs = 45000;
};

/// True when this endpoint is on the machine the game is running on.
bool isLocal(const std::string& baseUrl);

/// The JSON body for a chat completion. Separated out so it can be tested.
std::string chatRequestBody(const std::vector<Turn>& turns, const std::string& model,
                            int maxTokens = 220);

/**
 * Pull the reply out of a chat completion response.
 *
 * Tolerant on purpose: this parses whatever a runner the player chose sends
 * back, and the shape varies between llama.cpp, Ollama's OpenAI mode and the
 * remote APIs. Returns empty on anything it does not recognise, and empty is
 * handled -- the advisor simply does not write this turn.
 */
std::string replyFromResponse(const std::string& json);

}  // namespace llm
