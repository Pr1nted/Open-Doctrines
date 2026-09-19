#pragma once

#include <string>
#include <vector>

/**
 * Commands a mod adds to the map script language.
 *
 * ── WHY A SCRIPT CANNOT JUST CALL A MOD ──
 *
 * A scenario script is written by a map author, not a programmer, and the
 * language is deliberately small: twenty keywords, no functions, no imports.
 * When a map wants something the language cannot say -- "hand this country the
 * fleet it historically had", "run my succession rules" -- the author's only
 * options today are to write it out longhand or to go without.
 *
 * A mod can already do the work. What it could not do is be REACHABLE from the
 * script, because there was no way to spell it. So a mod registers a command
 * name, and a script line starting with that name is handed to the mod whole.
 *
 * ── WHY NAMES ARE FIRST-COME AND NOT NAMESPACED ──
 *
 * A script writes `reinforce FRA 3`, not `com.example.mod:reinforce FRA 3`.
 * Prefixing every call with a mod id would make scripts unreadable and would
 * couple a map to the exact mod that happens to provide the command, which is
 * the opposite of what a map author wants.
 *
 * So the name is global and the FIRST mod to register it wins. The second gets
 * a plain false from command_add -- not a silent overwrite, which would make
 * which mod ran a given line depend on load order.
 *
 * ── A MOD CANNOT REDEFINE THE LANGUAGE ──
 *
 * The built-in keywords are refused outright. Without that, a mod could
 * register `if` or `set` and every script in the game would start running
 * through it -- including scripts belonging to maps that never asked for the
 * mod, since scripts ship inside .odmap files and mods are enabled globally.
 * That is a takeover, not an extension.
 */
namespace odscript {

/** One registered command. */
struct Command {
    std::string modId;
    std::string name;
};

/** Whether a name is one of the language's own keywords. */
bool isReservedWord(const std::string& name);

/** Whether a name is usable as a command at all. */
bool validCommandName(const std::string& name);

class Registry {
public:
    /**
     * Claim a command name. False if it is reserved, malformed, or already
     * claimed by another mod. Re-registering your own is fine and succeeds,
     * because a mod redeclares its commands on every load.
     */
    bool add(const std::string& modId, const std::string& name);

    /** Give up one of your own. False if it was not yours. */
    bool remove(const std::string& modId, const std::string& name);

    /** Which mod owns this command, or empty. */
    std::string ownerOf(const std::string& name) const;

    /** Every command a mod owns, sorted. */
    std::vector<std::string> commandsOf(const std::string& modId) const;

    /** Everything, sorted by name. */
    std::vector<Command> all() const;

    /** Drop everything a mod registered. Called when it unloads. */
    void removeAll(const std::string& modId);

    void clear();

private:
    std::vector<Command> m_commands;   ///< small; order is registration order
};

}  // namespace odscript
