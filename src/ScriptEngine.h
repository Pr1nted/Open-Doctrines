#pragma once
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>

class Game;

struct ScriptValue {
    // MOD is a reference to an installed mod. It carries the mod's id in
    // strVal and whether it is loaded in boolVal, so `if mod.com.example.x`
    // reads as "is that mod here", and asking about a mod that is not
    // installed is false rather than an error -- detection is the whole point.
    enum Type { INT, FLOAT, STRING, BOOL, MOD, NONE };
    Type type = NONE;
    long long intVal = 0;
    double floatVal = 0;
    std::string strVal;
    bool boolVal = false;

    static ScriptValue makeInt(long long v) { ScriptValue s; s.type = INT; s.intVal = v; return s; }
    static ScriptValue makeFloat(double v) { ScriptValue s; s.type = FLOAT; s.floatVal = v; return s; }
    static ScriptValue makeStr(const std::string& v) { ScriptValue s; s.type = STRING; s.strVal = v; return s; }
    static ScriptValue makeBool(bool v) { ScriptValue s; s.type = BOOL; s.boolVal = v; return s; }
    static ScriptValue makeMod(const std::string& id, bool loaded) {
        ScriptValue s; s.type = MOD; s.strVal = id; s.boolVal = loaded; return s;
    }

    std::string asString() const;
    long long asInt() const;
    double asFloat() const;
    bool asBool() const;
};

struct ScriptError {
    std::string scriptName;
    std::string message;
    int lineNum = 0;
};

class ScriptEngine {
public:
    ScriptEngine(Game* game);

    // Load all scripts from the map's scripts/ directory and run the
    // entrypoints (files whose first non-blank line is #OD/MapEngine/N).
    // Files without the header are libraries, reachable only via `include`.
    // scriptData: map of filename → content (from m_odmJsonData).
    // Returns true if all scripts executed without errors.
    bool runScripts(const std::unordered_map<std::string, std::string>& scriptData);

    // Per-turn hook: re-evaluate scripts suspended on `waitUntil` and resume
    // the ones whose condition has become true.
    void tick();
    bool hasSuspended() const { return !m_suspended.empty(); }

    // True when the first non-blank line carries the #OD/MapEngine/ header
    // (shared with the map editor's script browser badges).
    static bool isEntrypoint(const std::string& content);

    // Get the last error (for debug display)
    const std::vector<ScriptError>& getErrors() const { return m_errors; }

    // 2 adds expressions (arithmetic, and/or, parentheses, calls), `set =`
    // and its compound forms, elseif, for/repeat, break/continue and print.
    //
    // 3 adds the game the game has become: `wait N turns`, `set x to <expr>`,
    // `{value}` interpolation inside print, and `foreach district`. The refs
    // that came with it -- an army by kind, a country's income and expenses,
    // its districts, its research lock -- are not gated, because a reference to
    // something that does not exist has always read as an empty value rather
    // than as an error, and making it fail now would break scripts that guard
    // on it.
    //
    // 5 adds the two things a script needed to make the map act on its own
    // rather than be redrawn by a precomputed list: `foreach neighbour in
    // province.<id>`, which is the adjacency graph the turn logic already
    // keeps, and `foreach province in array.NAME`, which lets a script hold a
    // frontier it collected and apply it afterwards. Those two together are
    // the difference between a script that plays back a shape and one that
    // spreads -- see docs/scripting.md, "Spreading across the map".
    //
    // `random.N` came with it and is NOT gated, because it is a reference and
    // references have never been: one that names nothing has always read as an
    // empty value rather than as an error.
    //
    // Version 1 and 2 files still run: everything added is backward compatible,
    // and a file that declares an older version GETS that version's language,
    // which is what makes pinning mean something.
    static const int ENGINE_VERSION = 5;
    static const int MIN_ENGINE_VERSION = 1;

    /**
     * The lowest engine version that accepts this statement: 1, 2 or 3.
     *
     * The editor lints against the same function the engine enforces, so a line
     * the game will refuse is flagged while it is being typed -- and with three
     * versions a boolean "is this new" no longer says which header to write.
     */
    static int statementMinVersion(const std::string& kw);

private:
    Game* m_game;
    std::vector<ScriptError> m_errors;

    // All map scripts by name (extension stripped) — the include universe
    std::unordered_map<std::string, std::string> m_library;

    // Script-global state: `var.NAME` variables, arrays, linked lists
    std::unordered_map<std::string, ScriptValue> m_globals;
    std::unordered_map<std::string, std::vector<ScriptValue>> m_arrays;
    std::unordered_map<std::string, std::deque<ScriptValue>> m_lists;

    // A script parked on a top-level `waitUntil`: preprocessed lines plus the
    // index of the waitUntil line to re-test each turn.
    struct SuspendedScript {
        std::string name;
        std::vector<std::string> lines;
        int resumeLine = 0;
        /// Set by `wait N turns`: resume on this turn rather than on a
        /// condition. -1 means the parked line is an ordinary waitUntil.
        int waitUntilTurn = -1;
    };
    std::vector<SuspendedScript> m_suspended;

    // ── Tasks: several flows in one script, without a thread ──
    //
    // A `spawn` starts another flow at a label. It is NOT parallelism: each
    // task runs until it reaches a waitUntil, then parks in m_suspended
    // exactly like a script does, and tick() resumes whichever ones are ready
    // on the next turn. So they interleave at wait boundaries, one at a time,
    // which is the only kind of "at the same time" that works in a browser
    // and the only kind that keeps the simulation deterministic.
    struct PendingTask { std::string name; std::vector<std::string> lines; std::string label; };
    std::vector<PendingTask> m_pendingTasks;

    /**
     * The WHOLE script runLines is working through, for `spawn` to capture.
     *
     * executeBlock is handed one SEGMENT -- the lines up to the next top-level
     * waitUntil -- so a `spawn` that captured what executeBlock can see could
     * only ever reach a label inside that segment. With one task that is
     * invisible, because its label is in the spawner's own segment; with
     * several, each body ends at its own waitUntil and every label but the
     * first falls outside, and the task fails with "spawn: no label".
     *
     * `jump` already resolves against every line for exactly this reason ("so
     * a jump can cross the waitUntil boundaries this loop splits on"). This
     * gives `spawn` the same reach. Saved and restored around each call
     * because runLines re-enters itself when it drains the tasks.
     */
    const std::vector<std::string>* m_scriptLines = nullptr;
    static const int MAX_LIVE_TASKS = 64;

    // ── The `province.*` locals, saved so loops can nest ──
    //
    // The province loops bind their fields as literal local keys (so
    // `province.population` in a body is an exact lookup, not a reference the
    // resolver takes apart) by const_cast-ing the caller's map, and they used
    // to clean up with erase(). That is wrong the moment one province loop
    // sits inside another: the inner loop's cleanup deleted the OUTER loop's
    // binding, and the rest of the outer body then saw no province at all.
    //
    // Nobody could hit it before, because there was only one way to loop over
    // provinces. `foreach neighbour` inside `foreach province` is the whole
    // point of having neighbours, so the keys are saved and put back instead.
    class ProvinceLocals {
    public:
        explicit ProvinceLocals(std::unordered_map<std::string, ScriptValue>& v) : m_vars(v) {}
        ~ProvinceLocals() { restore(); }
        ProvinceLocals(const ProvinceLocals&) = delete;
        ProvinceLocals& operator=(const ProvinceLocals&) = delete;
        /// Bind one key, remembering whatever it held first.
        void set(const std::string& key, const ScriptValue& val);
        /// Put every key back as it was. Called by the destructor.
        void restore();
    private:
        std::unordered_map<std::string, ScriptValue>& m_vars;
        std::vector<std::pair<std::string, ScriptValue>> m_saved;   // key → old value
        std::vector<std::string> m_added;                            // keys that were absent
    };

    /// Set once `foreach neighbour` has asked the game to build the
    /// adjacency graph, so a map whose provinces genuinely have no neighbours
    /// does not re-run the label pass on every loop.
    bool m_adjacencyAsked = false;

    /// Bind every `province.*` field for one province id, through `scope`.
    void bindProvinceLocals(ProvinceLocals& scope, int pid);

    // Tokenize a line into space-separated tokens (respecting quoted strings)
    std::vector<std::string> tokenize(const std::string& line);

    // Strip comments/header and splice `include`d libraries (recursive, with
    // circular-include and depth guards). Returns false on a fatal error.
    bool preprocess(const std::string& name, const std::string& content,
                    std::vector<std::string>& outLines,
                    std::vector<std::string>& includeStack);

    // Run preprocessed lines from `startLine`, suspending on a false
    // top-level `waitUntil`.
    void runLines(const std::string& name, std::vector<std::string> lines, int startLine);

    // waitUntil helpers
    static bool isWaitLine(const std::string& line);
    static std::string waitCondition(const std::string& line);
    /// `wait <expr> turns` -- the count is evaluated when the script parks.
    static bool isWaitTurnsLine(const std::string& line);
    static std::string waitTurnsCount(const std::string& line);
    /// Fill `{ref}` holes in a string with what the refs resolve to.
    std::string interpolate(const std::string& in,
                            const std::unordered_map<std::string, ScriptValue>& localVars);

    // Execute a block of lines (with nesting support for if/foreach/while)
    bool executeBlock(const std::vector<std::string>& lines, int& lineIdx,
                      const std::string& scriptName,
                      const std::unordered_map<std::string, ScriptValue>& localVars);

    // `array …` / `list …` statements
    void execCollectionStmt(const std::vector<std::string>& tokens, const std::string& scriptName,
                            int lineNum, const std::unordered_map<std::string, ScriptValue>& localVars);

    // Parse a single value token: literal, reference, or plain string
    ScriptValue parseValueToken(const std::string& tok,
                                const std::unordered_map<std::string, ScriptValue>& localVars);

    // Eval an expression: returns a ScriptValue
    ScriptValue evalExpr(const std::string& expr,
                         const std::unordered_map<std::string, ScriptValue>& localVars);

    // Resolve a reference like "country.USA.treasury" or "province.42.population"
    ScriptValue resolveRef(const std::string& ref,
                           const std::unordered_map<std::string, ScriptValue>& localVars);

    // Set a reference to a value
    bool setRef(const std::string& ref, const ScriptValue& val,
                const std::unordered_map<std::string, ScriptValue>& localVars);

    // Compare two values with an operator
    bool compareValues(const ScriptValue& lhs, const std::string& op, const ScriptValue& rhs);

    // Set by `break` / `continue`, cleared by the loop that consumes it.
    // A member rather than a return value because executeBlock's bool already
    // means "the block ended" and the two are not the same thing.
    enum class LoopSignal { NONE, BREAK, CONTINUE };
    LoopSignal m_loopSignal = LoopSignal::NONE;

    // `jump` unwinds every open block and resumes at the label, so it cannot
    // be a plain lineIdx assignment inside executeBlock. The LABEL is recorded
    // rather than a line number: runLines splits the script into segments at
    // each top-level waitUntil, so an index is only meaningful inside one
    // segment, and a jump across a waitUntil would land in the wrong place.
    // Empty when no jump is pending.
    std::string m_jumpLabel;
    bool m_stopped = false;      // `stop` ends the script, cleanly

    // Inside a `try`, an error is caught here instead of being reported. The
    // depth matters because a try inside a try must not swallow the outer
    // one's errors after its own catch has run.
    // The version the running script declared in its header. Version 2
    // statements are refused in a version 1 file: a script that says it is
    // version 1 must behave the way version 1 did, or "pin the version"
    // means nothing.
    int m_scriptVersion = ENGINE_VERSION;

    int m_tryDepth = 0;
    bool m_errorCaught = false;
    std::string m_caughtMsg;

    // Error helper
    void addError(const std::string& scriptName, int lineNum, const std::string& msg);
};
