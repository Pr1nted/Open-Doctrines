#include "ScriptCommands.h"

#include <algorithm>
#include <unordered_set>

namespace odscript {
namespace {

/// Long enough to read, short enough that a script cannot be padded with one.
constexpr size_t kMaxName = 48;

/**
 * Every word the language itself uses.
 *
 * Taken from ScriptEngine.cpp's own keyword chain plus the block words its
 * nesting scanner matches on -- `include` and `endif` never reach the keyword
 * chain but are still structure, and a mod claiming one would break the parse
 * rather than the execution.
 *
 * A word missing from this list is a word a mod can take over, so it is worth
 * being generous: refusing a name that was never a keyword costs a mod author
 * one rename, and missing one costs every script in the game.
 */
const std::unordered_set<std::string>& reserved() {
    static const std::unordered_set<std::string> kWords = {
        "array", "break", "catch", "continue", "dialog", "else", "elseif",
        "endif", "endtry", "endwhile", "for", "foreach", "if", "include",
        "jump", "label", "list", "next", "print", "repeat", "set", "spawn",
        "stop", "try", "unless", "while",
        // Not keywords today, and the shape of a future one. A mod that takes
        // "function" or "return" now would have to be broken later to add it.
        "function", "return", "import", "export", "const", "var", "let",
        "true", "false", "null", "and", "or", "not",
    };
    return kWords;
}

}  // namespace

bool isReservedWord(const std::string& name) {
    std::string lower = name;
    // Case-insensitively, because a script engine that matched "if" exactly
    // would still be confused by a mod registering "IF" the moment anyone
    // made the parser case-insensitive.
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return reserved().count(lower) != 0;
}

bool validCommandName(const std::string& name) {
    if (name.empty() || name.size() > kMaxName) return false;
    // An identifier: a letter first, then letters, digits or underscores. Not
    // because the parser demands it, but because a command is the first token
    // of a line and anything else would be ambiguous with a reference, a
    // number or a string -- ScriptEngine already has a case where "a line
    // starting with a reference was previously Unknown command".
    if (!std::isalpha((unsigned char)name[0])) return false;
    for (unsigned char c : name)
        if (!std::isalnum(c) && c != '_') return false;
    return !isReservedWord(name);
}

bool Registry::add(const std::string& modId, const std::string& name) {
    if (modId.empty() || !validCommandName(name)) return false;
    for (const Command& c : m_commands) {
        if (c.name != name) continue;
        // Yours already: fine, and expected -- a mod re-registers on load.
        // Somebody else's: refused, rather than silently taken, because a
        // silent overwrite makes which mod runs a line depend on load order.
        return c.modId == modId;
    }
    m_commands.push_back({modId, name});
    return true;
}

bool Registry::remove(const std::string& modId, const std::string& name) {
    for (auto it = m_commands.begin(); it != m_commands.end(); ++it) {
        if (it->name != name) continue;
        if (it->modId != modId) return false;   // not yours to give up
        m_commands.erase(it);
        return true;
    }
    return false;
}

std::string Registry::ownerOf(const std::string& name) const {
    for (const Command& c : m_commands)
        if (c.name == name) return c.modId;
    return {};
}

std::vector<std::string> Registry::commandsOf(const std::string& modId) const {
    std::vector<std::string> out;
    for (const Command& c : m_commands)
        if (c.modId == modId) out.push_back(c.name);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Command> Registry::all() const {
    std::vector<Command> out = m_commands;
    std::sort(out.begin(), out.end(),
              [](const Command& a, const Command& b) { return a.name < b.name; });
    return out;
}

void Registry::removeAll(const std::string& modId) {
    m_commands.erase(std::remove_if(m_commands.begin(), m_commands.end(),
                                    [&](const Command& c) { return c.modId == modId; }),
                     m_commands.end());
}

void Registry::clear() { m_commands.clear(); }

}  // namespace odscript
