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
    // Version 1 files still run: everything added is backward compatible, and
    // the header is a declaration of intent rather than a gate.
    static const int ENGINE_VERSION = 2;
    static const int MIN_ENGINE_VERSION = 1;

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
    static const int MAX_LIVE_TASKS = 64;

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
    int m_tryDepth = 0;
    bool m_errorCaught = false;
    std::string m_caughtMsg;

    // Error helper
    void addError(const std::string& scriptName, int lineNum, const std::string& msg);
};
