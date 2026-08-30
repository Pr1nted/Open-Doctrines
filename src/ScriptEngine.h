#pragma once
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>

class Game;

struct ScriptValue {
    enum Type { INT, FLOAT, STRING, BOOL, NONE };
    Type type = NONE;
    long long intVal = 0;
    double floatVal = 0;
    std::string strVal;
    bool boolVal = false;

    static ScriptValue makeInt(long long v) { ScriptValue s; s.type = INT; s.intVal = v; return s; }
    static ScriptValue makeFloat(double v) { ScriptValue s; s.type = FLOAT; s.floatVal = v; return s; }
    static ScriptValue makeStr(const std::string& v) { ScriptValue s; s.type = STRING; s.strVal = v; return s; }
    static ScriptValue makeBool(bool v) { ScriptValue s; s.type = BOOL; s.boolVal = v; return s; }

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

    // Error helper
    void addError(const std::string& scriptName, int lineNum, const std::string& msg);
};
