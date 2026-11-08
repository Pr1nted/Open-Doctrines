#include "Dialects.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "json.hpp"

namespace odscript {
namespace {

using Json = nlohmann::json;

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/// A quoted Open Doctrines string literal. The tokenizer respects quotes but
/// has no escapes, so a quote inside a GD5 message would end the token early
/// and leave the rest as stray words; it becomes an apostrophe instead.
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '\'';
        else if (c == '\n' || c == '\r') out += ' ';
        else out += c;
    }
    return out + "\"";
}

/// An identifier safe to paste into a reference: a label, a variable name, an
/// ISO code. Anything else would build a reference the resolver cannot parse.
std::string ident(const std::string& s) {
    std::string out;
    for (char c : s)
        if (std::isalnum((unsigned char)c) || c == '_') out += c;
    return out;
}

/// A JSON value as script text. GD5 writes numbers as strings about as often
/// as it writes them as numbers, so a string that is a number is passed
/// through bare rather than quoted -- `map.turn == "12"` would compare an int
/// to a string.
std::string valueText(const Json& v) {
    if (v.is_number_integer())  return std::to_string(v.get<long long>());
    if (v.is_number())          return std::to_string(v.get<double>());
    if (v.is_boolean())         return v.get<bool>() ? "true" : "false";
    if (!v.is_string())         return "0";
    const std::string s = v.get<std::string>();
    if (s.empty()) return "\"\"";
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    bool numeric = i < s.size();
    bool dot = false;
    for (; i < s.size() && numeric; ++i) {
        if (s[i] == '.') { numeric = !dot; dot = true; }
        else if (!std::isdigit((unsigned char)s[i])) numeric = false;
    }
    return numeric ? s : quoted(s);
}

/// The comparison operators GD5 and Open Doctrines share. "between" is GD5's
/// and has no single-expression form here, so it is refused by name rather
/// than quietly turned into something else.
const char* comparisonOp(const std::string& op) {
    if (op == "==" || op == "=" || op.empty()) return "==";
    if (op == "!=" || op == "<>")              return "!=";
    if (op == ">")  return ">";
    if (op == "<")  return "<";
    if (op == ">=") return ">=";
    if (op == "<=") return "<=";
    return nullptr;
}

/**
 * One GD5 condition as an Open Doctrines expression.
 *
 * Returns "" when there is no counterpart, with why in `why`. The event is
 * then not written: a condition silently dropped would turn a gate into an
 * unconditional fire, which is worse than not running at all.
 */
std::string conditionExpr(const Json& c, const std::string& owner, std::string& why) {
    const std::string type = c.value("type", std::string());
    const std::string opRaw = c.value("operator", std::string("=="));
    const char* op = comparisonOp(opRaw);
    const Json val = c.contains("value") ? c["value"] : Json("");

    if (type == "True")        return "true";

    if (type == "Turn Number") {
        if (!op) { why = "the operator \"" + opRaw + "\" on a Turn Number condition"; return ""; }
        return std::string("map.turn ") + op + " " + valueText(val);
    }
    if (type == "Variable") {
        if (!op) { why = "the operator \"" + opRaw + "\" on a Variable condition"; return ""; }
        const std::string name = ident(c.value("variable", std::string()));
        if (name.empty()) { why = "a Variable condition with no variable name"; return ""; }
        return "var." + name + " " + op + " " + valueText(val);
    }
    if (type == "At War With" || type == "In Faction With") {
        const std::string other = ident(val.is_string() ? val.get<std::string>() : std::string());
        if (other.empty()) { why = "a \"" + type + "\" condition naming no country"; return ""; }
        const std::string ref = "country." + owner +
                                (type == "At War With" ? ".at_war_with." : ".allied_with.") + other;
        // A bool against == / != rather than a comparison, so `!=` reads as
        // `not` instead of comparing a bool to a string.
        if (opRaw == "!=" || opRaw == "<>") return "not " + ref;
        return ref;
    }
    if (type == "Country Exists") {
        const std::string who = ident(val.is_string() ? val.get<std::string>() : std::string());
        if (who.empty()) { why = "a Country Exists condition naming no country"; return ""; }
        // Open Doctrines has no "exists": a country that holds no ground is
        // gone, which is the same test its own scripts use.
        const bool negated = (opRaw == "!=" || opRaw == "<>");
        return "country." + who + ".province_count " + (negated ? "== 0" : "> 0");
    }
    why = "the GD5 condition \"" + (type.empty() ? std::string("(none)") : type) + "\"";
    return "";
}

/// How a condition joins the one before it. GD5's `chain` sits on the LATER
/// condition and the fold is left to right.
std::string joinChain(const std::string& acc, const std::string& chain, const std::string& next) {
    const std::string c = chain.empty() ? "AND" : chain;
    const std::string a = "(" + acc + ")", b = "(" + next + ")";
    if (c == "OR")   return a + " or " + b;
    if (c == "XOR")  return "(" + a + " or " + b + ") and not (" + a + " and " + b + ")";
    if (c == "NOR")  return "not (" + a + " or " + b + ")";
    if (c == "NAND") return "not (" + a + " and " + b + ")";
    return a + " and " + b;          // AND, and anything unrecognised
}

/**
 * One GD5 action as Open Doctrines statements, appended to `out`.
 *
 * Returns false when there is no counterpart. Unlike a condition, an action
 * that cannot be carried is left out of the event rather than killing it --
 * an event that does three things and can do two is still worth running, and
 * the diagnostic says which one went missing.
 */
bool actionLines(const Json& a, const std::string& owner,
                 std::vector<std::string>& out, std::string& why) {
    const std::string type = a.value("type", std::string());
    const std::string target = ident(a.value("target", std::string()));
    const std::string message = a.value("message", std::string());

    if (type == "Declare War" || type == "Send Ceasefire") {
        if (target.empty()) { why = "a \"" + type + "\" action naming no country"; return false; }
        out.push_back("set country." + owner + ".at_war_with." + target + " " +
                      (type == "Declare War" ? "true" : "false"));
        return true;
    }
    if (type == "Join Faction" || type == "Invite to Faction") {
        if (target.empty()) { why = "a \"" + type + "\" action naming no country"; return false; }
        // GD5's factions are named groups; Open Doctrines has bilateral
        // alliances and no group object, so the closest true statement is an
        // alliance with the country named. Said in the script, not hidden.
        out.push_back("# " + type + ": GD5 factions are groups; this is the bilateral alliance");
        out.push_back("set country." + owner + ".allied_with." + target + " true");
        return true;
    }
    if (type == "Set Variable") {
        if (target.empty()) { why = "a Set Variable action with no variable name"; return false; }
        const Json v = a.contains("value") ? a["value"] : Json("");
        out.push_back("set var." + target + " = " + valueText(v));
        return true;
    }
    if (type == "Send Custom Message") {
        out.push_back("print " + quoted(message.empty() ? "(no message)" : message));
        return true;
    }
    if (type == "Create Faction") {
        why = "Create Faction: Open Doctrines has no faction object, only bilateral pacts";
        return false;
    }
    if (type == "Give Territory") {
        why = "Give Territory: GD5 gives by nation, and `set province.ID.owner` needs province ids";
        return false;
    }
    if (type == "Edit Name") {
        why = "Edit Name: country.ISO.name is readable but not writable";
        return false;
    }
    if (type == "Spawn Unit") {
        why = "Spawn Unit: recruitment is not reachable from a map script";
        return false;
    }
    why = "the GD5 action \"" + (type.empty() ? std::string("(none)") : type) + "\"";
    return false;
}

}  // namespace

// ─────────────────────────────────────────────────────────── gd5 ──────────

DialectResult translateGd5(const std::string& source) {
    DialectResult r;
    Json doc;
    try {
        doc = Json::parse(source, nullptr, true, true);   // allow comments
    } catch (const std::exception& e) {
        r.ok = false;
        r.diags.push_back({std::string("not valid JSON: ") + e.what(), true});
        return r;
    }

    // Two shapes, because a GD5 event lives under a nation in meta.json and
    // carries no owner of its own. A bare array needs an `owner` on each
    // event; the object form names one for all of them.
    std::string fileOwner;
    Json events = Json::array();
    if (doc.is_array()) {
        events = doc;
    } else if (doc.is_object()) {
        fileOwner = ident(doc.value("owner", std::string()));
        if (doc.contains("events") && doc["events"].is_array()) events = doc["events"];
        else if (doc.contains("scripted_events") && doc["scripted_events"].is_array())
            events = doc["scripted_events"];
        else {
            r.ok = false;
            r.diags.push_back({"an object with no \"events\" or \"scripted_events\" array", true});
            return r;
        }
    } else {
        r.ok = false;
        r.diags.push_back({"expected a JSON array of events, or an object with \"events\"", true});
        return r;
    }

    std::ostringstream body;
    std::ostringstream tasks;
    int written = 0, refused = 0, n = 0;

    for (const Json& e : events) {
        const int idx = n++;
        if (!e.is_object()) {
            r.diags.push_back({"event " + std::to_string(idx) + " is not an object", false});
            ++refused;
            continue;
        }
        const std::string name = e.value("name", "event " + std::to_string(idx));
        const std::string owner = !fileOwner.empty() ? fileOwner
                                                     : ident(e.value("owner", std::string()));
        if (owner.empty()) {
            r.diags.push_back({"\"" + name + "\" names no owner, and the file names none either; "
                               "add \"owner\" to the event or to the file", false});
            ++refused;
            continue;
        }

        // The legacy single-condition and single-action shapes GD5 still
        // reads, where an event carried one of each inline.
        Json conds = Json::array();
        if (e.contains("conditions") && e["conditions"].is_array()) conds = e["conditions"];
        else if (e.contains("condition_type"))
            conds.push_back(Json{{"type", e["condition_type"]},
                                 {"operator", "=="},
                                 {"value", e.value("condition_val", std::string())},
                                 {"chain", "AND"}});

        std::string gate;
        bool condOk = true;
        for (const Json& c : conds) {
            std::string why;
            const std::string expr = conditionExpr(c, owner, why);
            if (expr.empty()) {
                r.diags.push_back({"\"" + name + "\" was not written: " + why +
                                   " has no Open Doctrines counterpart", false});
                condOk = false;
                break;
            }
            gate = gate.empty() ? expr : joinChain(gate, c.value("chain", std::string("AND")), expr);
        }
        if (!condOk) { ++refused; continue; }
        if (gate.empty()) gate = "true";          // no conditions: fire at once

        Json acts = Json::array();
        if (e.contains("actions") && e["actions"].is_array()) acts = e["actions"];
        else if (e.contains("action_type"))
            acts.push_back(Json{{"type", e["action_type"]},
                                {"target", e.value("action_target", std::string("None"))}});

        std::vector<std::string> doLines;
        for (const Json& a : acts) {
            std::string why;
            if (!actionLines(a, owner, doLines, why))
                r.diags.push_back({"in \"" + name + "\": " + why + " was left out", false});
        }
        if (doLines.empty()) {
            r.diags.push_back({"\"" + name + "\" was not written: none of its actions "
                               "have an Open Doctrines counterpart", false});
            ++refused;
            continue;
        }

        const std::string label = "gd5_" + std::to_string(idx);
        const bool once = e.value("fire_once", true);
        const std::string trig = e.value("trigger_type", std::string("Both"));

        tasks << "spawn " << label << "\n";
        body << "\nlabel " << label << "\n";
        body << "# " << name << "  (" << trig << ", "
             << (once ? "fires once" : "fires every time") << ")\n";
        // TRIGGER IS A COMMENT, NOT A GATE. GD5 fires an event for the AI, the
        // player or both; a map script has no notion of whose turn it is
        // running on, so gating on it here would invent a rule. It is written
        // down so the difference is visible rather than lost.
        body << "waitUntil " << gate << "\n";
        for (const std::string& l : doLines) body << l << "\n";
        if (once) {
            body << "stop\n";
        } else {
            // Re-arm. `wait 1 turns` first, or a condition that stays true
            // would fire again in the same turn, forever.
            body << "wait 1 turns\n";
            body << "jump " << label << "\n";
        }
        ++written;
    }

    if (written == 0) {
        r.ok = false;
        r.diags.push_back({"no event in this file could be written as an Open Doctrines script",
                           true});
        return r;
    }

    std::ostringstream out;
    out << "# Translated from Greater Diplomacy 5 scripted events by the `gd5`\n"
        << "# dialect (src/script/Dialects.cpp). " << written << " of " << (written + refused)
        << " event(s) carried across.\n"
        << "#\n"
        << "# Each event is a task: a flow parked on `waitUntil`, retested once a\n"
        << "# turn, which is how GD5 evaluates an event too.\n";
    out << tasks.str();
    // JUMP PAST THE BODIES, do not `stop`.
    //
    // `stop` would be the obvious way to end the main flow, and it loses every
    // task: it returns out of runLines BEFORE the loop that drains
    // m_pendingTasks, so spawned flows never start. Falling through into the
    // first `label` instead would run that event's body unconditionally. So
    // the main flow jumps to a label past them all and ends by running off the
    // end, which is the one exit that reaches the drain.
    out << "jump gd5_done\n";
    out << body.str();
    out << "\nlabel gd5_done\n";
    r.nativeText = out.str();
    return r;
}

// ────────────────────────────────────────────────────── the registry ──────

const std::vector<Dialect>& dialects() {
    static const std::vector<Dialect> kAll = {
        {"gd5", 1, &translateGd5,
         "Greater Diplomacy 5 scripted events: a JSON array of conditions and actions"},
    };
    return kAll;
}

const Dialect* findDialect(const std::string& name) {
    const std::string want = lower(name);
    for (const Dialect& d : dialects())
        if (want == d.name) return &d;
    return nullptr;
}

std::string dialectNames() {
    std::string s;
    for (const Dialect& d : dialects()) {
        if (!s.empty()) s += ", ";
        s += d.name;
    }
    return s;
}

std::string headerLineOf(const std::string& content) {
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        const size_t a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        line = line.substr(a);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (!line.empty()) return line;
    }
    return std::string();
}

std::string bodyAfterHeader(const std::string& content) {
    std::istringstream ss(content);
    std::string line;
    std::string body;
    bool pastHeader = false;
    while (std::getline(ss, line)) {
        if (!pastHeader) {
            const size_t a = line.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) continue;        // blank lines before it
            pastHeader = true;
            continue;                                     // the header itself
        }
        body += line;
        body += '\n';
    }
    return body;
}

bool parseVolatile(const std::string& headerLine, std::string& out) {
    out.clear();
    std::istringstream ss(headerLine);
    std::string tok;
    while (ss >> tok) {
        if (lower(tok) != "volatile") continue;
        if (!(ss >> tok)) return false;        // `volatile` with nothing after it
        out = tok;
        return true;
    }
    return false;
}

}  // namespace odscript
