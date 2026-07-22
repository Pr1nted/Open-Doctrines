#pragma once
#include <string>
#include <vector>
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

    // Load and execute all scripts from the map's scripts/ directory.
    // scriptData: map of filename → content (from m_odmJsonData, filtered by "scripts/" prefix).
    // Returns true if all scripts executed without errors.
    bool runScripts(const std::unordered_map<std::string, std::string>& scriptData);

    // Get the last error (for debug display)
    const std::vector<ScriptError>& getErrors() const { return m_errors; }

    static const int ENGINE_VERSION = 1;

private:
    Game* m_game;
    std::vector<ScriptError> m_errors;

    // Tokenize a line into space-separated tokens (respecting quoted strings)
    std::vector<std::string> tokenize(const std::string& line);

    // Parse and execute a single script
    bool executeScript(const std::string& name, const std::string& content);

    // Execute a block of lines (with nesting support for if/foreach)
    bool executeBlock(const std::vector<std::string>& lines, int& lineIdx,
                      const std::string& scriptName,
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

    // Error helper
    void addError(const std::string& scriptName, int lineNum, const std::string& msg);
};