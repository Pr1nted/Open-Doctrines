#include "ScriptEngine.h"
#include "script/Expr.h"
#include "Game.h"
#include "GameInternals.h"
#include <cctype>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cmath>

ScriptEngine::ScriptEngine(Game* game) : m_game(game) {}

std::vector<std::string> ScriptEngine::tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuote = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '"') { inQuote = !inQuote; continue; }
        if (c == ' ' || c == '\t') {
            if (!inQuote && !cur.empty()) { tokens.push_back(cur); cur.clear(); }
            else if (inQuote) cur += c;
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

void ScriptEngine::addError(const std::string& scriptName, int lineNum, const std::string& msg) {
    m_errors.push_back({scriptName, msg, lineNum});
    printf("[SCRIPT] Error in %s line %d: %s\n", scriptName.c_str(), lineNum, msg.c_str());
}

bool ScriptEngine::isEntrypoint(const std::string& content) {
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        return line.compare(start, 14, "#OD/MapEngine/") == 0;
    }
    return false;
}

bool ScriptEngine::runScripts(const std::unordered_map<std::string, std::string>& scriptData) {
    m_errors.clear();
    m_suspended.clear();
    m_globals.clear();
    m_arrays.clear();
    m_lists.clear();

    // First pass: index every script by name — the include universe
    m_library.clear();
    for (auto& [path, content] : scriptData) {
        if (path.rfind("scripts/", 0) != 0) continue;
        if (path.back() == '/') continue;
        std::string name = path.substr(8);
        if (name.size() > 4 && name.substr(name.size() - 4) == ".txt")
            name = name.substr(0, name.size() - 4);
        m_library[name] = content;
    }

    // Second pass: run only entrypoints (files carrying the #OD/MapEngine/
    // header); the rest are libraries reachable via `include`
    for (auto& [name, content] : m_library) {
        if (!isEntrypoint(content)) {
            printf("[SCRIPT] Library (not run): %s\n", name.c_str());
            continue;
        }
        printf("[SCRIPT] Running script: %s\n", name.c_str());
        std::vector<std::string> lines;
        std::vector<std::string> stack;
        if (preprocess(name, content, lines, stack))
            runLines(name, std::move(lines), 0);
    }
    return m_errors.empty();
}

bool ScriptEngine::preprocess(const std::string& name, const std::string& content,
                              std::vector<std::string>& outLines,
                              std::vector<std::string>& includeStack) {
    if (includeStack.size() > 16) {
        addError(name, 0, "include depth exceeded (16)");
        return false;
    }
    if (std::find(includeStack.begin(), includeStack.end(), name) != includeStack.end()) {
        addError(name, 0, "circular include of '" + name + "'");
        return false;
    }
    includeStack.push_back(name);

    std::istringstream ss(content);
    std::string line;
    int lineNum = 0;
    bool ok = true;
    while (std::getline(ss, line)) {
        lineNum++;
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line.rfind("#OD/MapEngine/", 0) == 0) {
                int ver = 0;
                sscanf(line.c_str() + 14, "%d", &ver);
                if (ver < MIN_ENGINE_VERSION || ver > ENGINE_VERSION) {
                    addError(name, lineNum, "Unsupported script engine version " + std::to_string(ver) +
                                            " (this build runs " + std::to_string(MIN_ENGINE_VERSION) +
                                            "-" + std::to_string(ENGINE_VERSION) + ")");
                    ok = false;
                    break;
                }
            }
            continue;
        }
        auto tokens = tokenize(line);
        if (!tokens.empty() && tokens[0] == "include") {
            if (tokens.size() < 2) {
                addError(name, lineNum, "include: missing script name");
                continue;
            }
            std::string inc = tokens[1];
            if (inc.size() > 4 && inc.substr(inc.size() - 4) == ".txt")
                inc = inc.substr(0, inc.size() - 4);
            auto it = m_library.find(inc);
            if (it == m_library.end()) {
                addError(name, lineNum, "include: script '" + inc + "' not found");
                continue;
            }
            preprocess(inc, it->second, outLines, includeStack); // errors already recorded
            continue;
        }
        outLines.push_back(line);
    }
    includeStack.pop_back();
    return ok;
}

bool ScriptEngine::isWaitLine(const std::string& line) {
    return line.rfind("waitUntil", 0) == 0 &&
           (line.size() == 9 || line[9] == ' ' || line[9] == '\t' || line[9] == '(');
}

std::string ScriptEngine::waitCondition(const std::string& line) {
    std::string expr = line.substr(9); // past "waitUntil"
    size_t s = expr.find_first_not_of(" \t");
    expr = (s == std::string::npos) ? "" : expr.substr(s);
    while (!expr.empty() && (expr.back() == ' ' || expr.back() == '\t')) expr.pop_back();
    // Accept waitUntil(cond) form: strip one surrounding paren pair
    if (expr.size() >= 2 && expr.front() == '(' && expr.back() == ')')
        expr = expr.substr(1, expr.size() - 2);
    return expr;
}

void ScriptEngine::runLines(const std::string& name, std::vector<std::string> lines, int startLine) {
    int pc = startLine;
    while (pc < (int)lines.size()) {
        if (isWaitLine(lines[pc])) {
            std::string expr = waitCondition(lines[pc]);
            if (expr.empty()) {
                addError(name, pc + 1, "waitUntil: missing condition");
                pc++;
                continue;
            }
            std::unordered_map<std::string, ScriptValue> lv;
            if (!evalExpr(expr, lv).asBool()) {
                // Park here; tick() re-tests the condition each turn
                m_suspended.push_back({name, std::move(lines), pc});
                return;
            }
            pc++;
            continue;
        }
        // Gather the segment up to the next top-level waitUntil (block depth
        // aware); nested waitUntils are rejected — top level only
        int depth = 0, end = pc;
        while (end < (int)lines.size()) {
            auto t = tokenize(lines[end]);
            if (!t.empty()) {
                if (t[0] == "if" || t[0] == "foreach" || t[0] == "while") depth++;
                else if (t[0] == "endif" || t[0] == "next" || t[0] == "endwhile") depth--;
                else if (isWaitLine(lines[end])) {
                    if (depth == 0) break;
                    addError(name, end + 1, "waitUntil is only allowed at top level (not inside if/foreach/while)");
                    lines[end] = "# waitUntil ignored (nested)";
                }
            }
            end++;
        }
        std::vector<std::string> segment(lines.begin() + pc, lines.begin() + end);
        std::unordered_map<std::string, ScriptValue> localVars;
        int idx = 0;
        executeBlock(segment, idx, name, localVars);
        pc = end;
    }
}

void ScriptEngine::tick() {
    if (m_suspended.empty()) return;
    std::vector<SuspendedScript> pending;
    pending.swap(m_suspended);
    for (auto& s : pending) {
        std::string expr = waitCondition(s.lines[s.resumeLine]);
        std::unordered_map<std::string, ScriptValue> lv;
        if (evalExpr(expr, lv).asBool()) {
            printf("[SCRIPT] Resuming script: %s\n", s.name.c_str());
            runLines(s.name, std::move(s.lines), s.resumeLine + 1);
        } else {
            m_suspended.push_back(std::move(s));
        }
    }
}

bool ScriptEngine::executeBlock(const std::vector<std::string>& lines, int& lineIdx,
                                const std::string& scriptName,
                                const std::unordered_map<std::string, ScriptValue>& localVars) {
    while (lineIdx < (int)lines.size()) {
        std::string line = lines[lineIdx];
        auto tokens = tokenize(line);
        if (tokens.empty()) { lineIdx++; continue; }

        // ── C-STYLE SHORTHANDS, NORMALISED TO `set` ──
        //
        // `var.i++`, `++var.i`, `var.i--`, `--var.i`, and a bare assignment
        // like `var.gold += 100` or `country.USA.treasury = 5000` with no
        // leading `set`. Each is rewritten into the `set` form and then runs
        // through exactly one implementation, so the shorthand cannot drift
        // from the statement it stands for.
        //
        // Unambiguous because a statement here always begins with a keyword;
        // a line starting with a reference was previously "Unknown command".
        // Nothing that used to work changes meaning.
        {
            const std::string t = odscript::normaliseAssignment(line);
            if (!t.empty() && t != line) {
                line = t;
                tokens = tokenize(line);
                if (tokens.empty()) { lineIdx++; continue; }
            }
        }

        // Check for block-ending keywords
        std::string kw = tokens[0];
        if (kw == "endif" || kw == "next" || kw == "else" || kw == "elseif" || kw == "endwhile") {
            return true; // block ended — caller handles
        }

        // break / continue — consumed by the innermost enclosing loop.
        if (kw == "break" || kw == "continue") {
            m_loopSignal = (kw == "break") ? LoopSignal::BREAK : LoopSignal::CONTINUE;
            lineIdx++;
            return true;   // stop this block; the loop decides what happens next
        }

        // print <expr-or-text> — the only way to see inside a running script.
        if (kw == "print") {
            std::string rest = line.size() > 5 ? line.substr(6) : "";
            size_t rs = rest.find_first_not_of(" \t");
            rest = (rs == std::string::npos) ? "" : rest.substr(rs);
            bool ok = false; std::string perr;
            ScriptValue v = odscript::evaluate(
                rest, [&](const std::string& n) { return resolveRef(n, localVars); }, ok, perr);
            printf("[SCRIPT] %s: %s\n", scriptName.c_str(),
                   ok ? v.asString().c_str() : rest.c_str());
            lineIdx++;
            continue;
        }

        // if / elseif / else / endif
        //
        // `unless X` is `if not X`, rewritten here rather than duplicated.
        if (kw == "if" || kw == "unless") {
            if (tokens.size() < 2) { addError(scriptName, lineIdx + 1, kw + ": missing condition"); lineIdx++; return false; }
            std::string expr = line.substr(kw.size());
            if (kw == "unless") expr = "not (" + expr + ")";

            // ── FIND THE endif, AND THE else/elseif ARMS SEPARATELY ──
            //
            // The previous version stopped its scan at `else` and then set the
            // resume point to that line + 1 -- the first line of the else body,
            // which therefore ran whatever the condition was, and the trailing
            // `endif` ended the ENCLOSING block early. No shipped map used
            // `else`, so it never surfaced.
            const int blockStart = lineIdx + 1;
            int endIdx = -1;
            int depth = 1;
            std::vector<int> arms;      // the else/elseif lines at this level
            for (int i = blockStart; i < (int)lines.size(); ++i) {
                auto bt = tokenize(lines[i]);
                if (bt.empty()) continue;
                if (bt[0] == "if" || bt[0] == "unless" || bt[0] == "foreach" ||
                    bt[0] == "while" || bt[0] == "for" || bt[0] == "repeat") {
                    depth++;
                } else if (bt[0] == "endif" || bt[0] == "next" || bt[0] == "endwhile") {
                    if (--depth == 0) { endIdx = i; break; }
                } else if ((bt[0] == "else" || bt[0] == "elseif") && depth == 1) {
                    arms.push_back(i);
                }
            }
            if (endIdx < 0) { addError(scriptName, lineIdx + 1, "if: missing endif"); return false; }

            // Walk the arms in order and run the first whose condition holds.
            int bodyStart = -1;
            if (evalExpr(expr, localVars).asBool()) {
                bodyStart = blockStart;
            } else {
                for (size_t a = 0; a < arms.size(); ++a) {
                    auto bt = tokenize(lines[arms[a]]);
                    if (bt[0] == "else") { bodyStart = arms[a] + 1; break; }
                    // elseif <cond>
                    std::string sub = lines[arms[a]].substr(lines[arms[a]].find("elseif") + 6);
                    if (evalExpr(sub, localVars).asBool()) { bodyStart = arms[a] + 1; break; }
                }
            }
            if (bodyStart >= 0) {
                int subIdx = bodyStart;
                executeBlock(lines, subIdx, scriptName, localVars);
            }
            lineIdx = endIdx + 1;
            if (m_loopSignal != LoopSignal::NONE) return true;   // let the loop see it
            continue;
        }

        // foreach province in country.ISO
        // foreach item in array.NAME / foreach item in list.NAME
        if (kw == "foreach") {
            if (tokens.size() < 4 || tokens[2] != "in" ||
                (tokens[1] != "province" && tokens[1] != "item")) {
                addError(scriptName, lineIdx + 1,
                         "foreach: expected 'foreach province in country.ISO' or 'foreach item in array/list.NAME'");
                lineIdx++; return false;
            }
            std::string sourceRef = tokens[3];

            // Find matching next
            int blockStart = lineIdx + 1;
            int blockEnd = blockStart;
            int depth = 1;
            while (blockEnd < (int)lines.size() && depth > 0) {
                auto bt = tokenize(lines[blockEnd]);
                if (!bt.empty()) {
                    if (bt[0] == "if" || bt[0] == "foreach" || bt[0] == "while") depth++;
                    else if (bt[0] == "next" || bt[0] == "endif" || bt[0] == "endwhile") depth--;
                }
                if (depth == 0) break;
                blockEnd++;
            }
            if (depth != 0) { addError(scriptName, lineIdx + 1, "foreach: missing next"); return false; }

            auto& vars = const_cast<std::unordered_map<std::string, ScriptValue>&>(localVars);

            // ── foreach item in array.NAME / list.NAME ──
            if (tokens[1] == "item") {
                std::vector<ScriptValue> items;
                if (sourceRef.rfind("array.", 0) == 0) {
                    auto it = m_arrays.find(sourceRef.substr(6));
                    if (it == m_arrays.end()) {
                        addError(scriptName, lineIdx + 1, "foreach: unknown array " + sourceRef);
                        lineIdx = blockEnd + 1;
                        continue;
                    }
                    items = it->second;
                } else if (sourceRef.rfind("list.", 0) == 0) {
                    auto it = m_lists.find(sourceRef.substr(5));
                    if (it == m_lists.end()) {
                        addError(scriptName, lineIdx + 1, "foreach: unknown list " + sourceRef);
                        lineIdx = blockEnd + 1;
                        continue;
                    }
                    items.assign(it->second.begin(), it->second.end());
                } else {
                    addError(scriptName, lineIdx + 1, "foreach item: expected array.NAME or list.NAME");
                    lineIdx = blockEnd + 1;
                    continue;
                }
                for (size_t i = 0; i < items.size(); ++i) {
                    vars["item"] = items[i];
                    vars["item.index"] = ScriptValue::makeInt((long long)i);
                    int subIdx = blockStart;
                    executeBlock(lines, subIdx, scriptName, localVars);
                if (m_loopSignal == LoopSignal::BREAK) { m_loopSignal = LoopSignal::NONE; break; }
                if (m_loopSignal == LoopSignal::CONTINUE) m_loopSignal = LoopSignal::NONE;
                }
                vars.erase("item");
                vars.erase("item.index");
                lineIdx = blockEnd + 1;
                continue;
            }

            // ── foreach province in country.ISO ──
            if (sourceRef.rfind("country.", 0) != 0) {
                addError(scriptName, lineIdx + 1, "foreach: expected country.ISO, got " + sourceRef);
                lineIdx = blockEnd + 1;
                continue;
            }
            std::string iso = sourceRef.substr(8);
            int targetCid = -1;
            for (auto& [cid, c] : m_game->m_countries.getAll())
                if (c.isoA3 == iso) { targetCid = cid; break; }
            if (targetCid < 0) {
                addError(scriptName, lineIdx + 1, "foreach: country " + iso + " not found");
                lineIdx = blockEnd + 1;
                continue;
            }
            for (auto& [pid, prov] : m_game->m_provinces.getAllProvinces()) {
                if (prov.countryId != targetCid) continue;
                // Set local var "province" = this province ID
                vars["province"] = ScriptValue::makeInt(pid);
                vars["province.id"] = ScriptValue::makeInt(pid);
                // Also set province properties as locals
                vars["province.population"] = ScriptValue::makeInt(
                    m_game->m_provincePopulations.count(pid) ? m_game->m_provincePopulations[pid] : 0);
                auto indIt = m_game->m_provinceIndustry.find(pid);
                int indLevel = (indIt != m_game->m_provinceIndustry.end()) ? indIt->second.level : 0;
                int fortLevel = (indIt != m_game->m_provinceIndustry.end()) ? indIt->second.fortification : 0;
                vars["province.industry"] = ScriptValue::makeInt(indLevel);
                vars["province.fortification"] = ScriptValue::makeInt(fortLevel);
                vars["province.owner"] = ScriptValue::makeStr(iso);
                // Execute the block body
                int subIdx = blockStart;
                executeBlock(lines, subIdx, scriptName, localVars);
                if (m_loopSignal == LoopSignal::BREAK) { m_loopSignal = LoopSignal::NONE; break; }
                if (m_loopSignal == LoopSignal::CONTINUE) m_loopSignal = LoopSignal::NONE;
            }
            // Clean up local var
            vars.erase("province");
            vars.erase("province.id");
            vars.erase("province.population");
            vars.erase("province.industry");
            vars.erase("province.fortification");
            vars.erase("province.owner");
            lineIdx = blockEnd + 1;
            continue;
        }

        // array/list statements
        if (kw == "array" || kw == "list") {
            execCollectionStmt(tokens, scriptName, lineIdx + 1, localVars);
            lineIdx++;
            continue;
        }

        // while loop
        // for <ref> = <from> to <to>   ... next
        // repeat <count>                ... next
        //
        // Both are counting loops, which the language had no way to write: a
        // fixed number of turns of something took a var, a while, and a manual
        // increment, three lines to say "ten times".
        if (kw == "for" || kw == "repeat") {
            std::string loopRef;
            long long from = 1, to = 0;
            bool parsedHead = false;
            if (kw == "repeat" && tokens.size() >= 2) {
                std::string cntExpr = line.substr(6);
                bool ok = false; std::string cerr;
                ScriptValue c = odscript::evaluate(
                    cntExpr, [&](const std::string& n) { return resolveRef(n, localVars); }, ok, cerr);
                if (ok) { from = 1; to = c.asInt(); parsedHead = true; }
                else addError(scriptName, lineIdx + 1, "repeat: " + cerr);
            } else if (kw == "for" && tokens.size() >= 5 && tokens[2] == "=") {
                // for var.i = <expr> to <expr>
                const size_t toPos = line.find(" to ");
                if (toPos != std::string::npos) {
                    const size_t eqPos = line.find('=');
                    std::string fromExpr = line.substr(eqPos + 1, toPos - eqPos - 1);
                    std::string toExpr = line.substr(toPos + 4);
                    bool ok1 = false, ok2 = false; std::string e1, e2;
                    auto res = [&](const std::string& n) { return resolveRef(n, localVars); };
                    ScriptValue a = odscript::evaluate(fromExpr, res, ok1, e1);
                    ScriptValue b = odscript::evaluate(toExpr, res, ok2, e2);
                    if (ok1 && ok2) {
                        loopRef = tokens[1]; from = a.asInt(); to = b.asInt(); parsedHead = true;
                    } else {
                        addError(scriptName, lineIdx + 1, "for: " + (ok1 ? e2 : e1));
                    }
                } else {
                    addError(scriptName, lineIdx + 1, "for: expected 'for <var> = <from> to <to>'");
                }
            } else {
                addError(scriptName, lineIdx + 1, kw + ": malformed loop header");
            }

            const int blockStart = lineIdx + 1;
            int endIdx = -1, depth = 1;
            for (int i = blockStart; i < (int)lines.size(); ++i) {
                auto bt = tokenize(lines[i]);
                if (bt.empty()) continue;
                if (bt[0] == "if" || bt[0] == "unless" || bt[0] == "foreach" ||
                    bt[0] == "while" || bt[0] == "for" || bt[0] == "repeat") depth++;
                else if (bt[0] == "next" || bt[0] == "endif" || bt[0] == "endwhile") {
                    if (--depth == 0) { endIdx = i; break; }
                }
            }
            if (endIdx < 0) { addError(scriptName, lineIdx + 1, kw + ": missing next"); return false; }

            if (parsedHead) {
                long long guard = 0;
                for (long long v = from; v <= to && guard < 100000; ++v, ++guard) {
                    if (!loopRef.empty()) setRef(loopRef, ScriptValue::makeInt(v), localVars);
                    int subIdx = blockStart;
                    executeBlock(lines, subIdx, scriptName, localVars);
                    if (m_loopSignal == LoopSignal::BREAK) { m_loopSignal = LoopSignal::NONE; break; }
                    if (m_loopSignal == LoopSignal::CONTINUE) m_loopSignal = LoopSignal::NONE;
                }
            }
            lineIdx = endIdx + 1;
            continue;
        }

        if (kw == "while") {
            if (tokens.size() < 2) { addError(scriptName, lineIdx + 1, "while: missing condition"); lineIdx++; return false; }
            std::string expr = line.substr(5);
            int blockStart = lineIdx + 1;
            int blockEnd = blockStart;
            int depth = 1;
            while (blockEnd < (int)lines.size() && depth > 0) {
                auto bt = tokenize(lines[blockEnd]);
                if (!bt.empty()) {
                    if (bt[0] == "if" || bt[0] == "unless" || bt[0] == "foreach" ||
                        bt[0] == "while" || bt[0] == "for" || bt[0] == "repeat") depth++;
                    else if (bt[0] == "endwhile" || bt[0] == "endif" || bt[0] == "next") depth--;
                }
                if (depth == 0) break;
                blockEnd++;
            }
            if (depth != 0) { addError(scriptName, lineIdx + 1, "while: missing endwhile"); return false; }

            int maxIters = 10000; // safety limit
            while (evalExpr(expr, localVars).asBool() && maxIters-- > 0) {
                int subIdx = blockStart;
                executeBlock(lines, subIdx, scriptName, localVars);
                if (m_loopSignal == LoopSignal::BREAK) { m_loopSignal = LoopSignal::NONE; break; }
                if (m_loopSignal == LoopSignal::CONTINUE) m_loopSignal = LoopSignal::NONE;
            }
            if (maxIters <= 0) addError(scriptName, lineIdx + 1, "while: exceeded 10000 iterations");
            lineIdx = blockEnd + 1;
            continue;
        }

        // set statement
        if (kw == "set") {
            if (tokens.size() < 3) {
                addError(scriptName, lineIdx + 1, "set: requires <ref> <value>");
                lineIdx++; continue;
            }
            std::string ref = tokens[1];
            // Value is everything after the reference token
            std::string valStr;
            int refEnd = line.find(tokens[1]) + tokens[1].size();
            valStr = line.substr(refEnd);
            // Trim
            size_t s = valStr.find_first_not_of(" \t");
            if (s != std::string::npos) valStr = valStr.substr(s);
            // ── `set x = <expr>` AND `set x += <expr>` ──
            //
            // The `=` is what makes the value an EXPRESSION rather than a
            // literal, and it is required for exactly that reason: `set
            // map.date Modern Day` is legal in shipped maps, so a bare value
            // cannot be parsed as arithmetic without turning every hyphenated
            // name into a subtraction. With the marker there is no ambiguity
            // -- old scripts have no `=` and are untouched.
            bool assignHandled = false;
            {
                static const char* kAssign[] = {"+=", "-=", "*=", "/=", "="};
                for (const char* aop : kAssign) {
                    const size_t alen = strlen(aop);
                    if (valStr.compare(0, alen, aop) != 0) continue;
                    std::string rhs = valStr.substr(alen);
                    size_t rs = rhs.find_first_not_of(" \t");
                    rhs = (rs == std::string::npos) ? "" : rhs.substr(rs);

                    bool ok = false;
                    std::string err;
                    ScriptValue rv = odscript::evaluate(
                        rhs, [&](const std::string& n) { return resolveRef(n, localVars); },
                        ok, err);
                    if (!ok) {
                        addError(scriptName, lineIdx + 1, "set: " + err);
                        assignHandled = true;
                        break;
                    }
                    if (alen == 2) {   // compound: fold against what is there
                        const std::string binop(1, aop[0]);
                        auto tree = odscript::parse(ref + " " + binop + " (" + rhs + ")", err);
                        if (!tree) {
                            addError(scriptName, lineIdx + 1, "set: " + err);
                            assignHandled = true;
                            break;
                        }
                        rv = odscript::eval(*tree,
                                            [&](const std::string& n) { return resolveRef(n, localVars); },
                                            err);
                        if (!err.empty()) {
                            addError(scriptName, lineIdx + 1, "set: " + err);
                            assignHandled = true;
                            break;
                        }
                    }
                    if (!setRef(ref, rv, localVars))
                        addError(scriptName, lineIdx + 1, "set: cannot set " + ref);
                    assignHandled = true;
                    break;
                }
            }
            if (assignHandled) { lineIdx++; continue; }

            ScriptValue val;
            // Try to parse as int, float, bool, or string
            if (valStr == "true") val = ScriptValue::makeBool(true);
            else if (valStr == "false") val = ScriptValue::makeBool(false);
            else {
                bool isNumber = true;
                bool hasDot = false;
                for (char c : valStr) {
                    if (c == '.' && !hasDot) { hasDot = true; continue; }
                    if (c < '0' || c > '9') { isNumber = false; break; }
                }
                if (isNumber && !valStr.empty()) {
                    if (hasDot) val = ScriptValue::makeFloat(std::stod(valStr));
                    else val = ScriptValue::makeInt(std::stoll(valStr));
                } else {
                    // Check if it's a reference
                    if (valStr.rfind("country.", 0) == 0 || valStr.rfind("province.", 0) == 0 ||
                        valStr.rfind("map.", 0) == 0 || valStr.rfind("var.", 0) == 0 ||
                        valStr.rfind("array.", 0) == 0 || valStr.rfind("list.", 0) == 0 ||
                        valStr == "item" || valStr.rfind("item.", 0) == 0) {
                        val = resolveRef(valStr, localVars);
                    } else {
                        // Strip quotes if present
                        if (valStr.size() >= 2 && valStr.front() == '"' && valStr.back() == '"')
                            valStr = valStr.substr(1, valStr.size() - 2);
                        val = ScriptValue::makeStr(valStr);
                    }
                }
            }
            if (!setRef(ref, val, localVars)) {
                addError(scriptName, lineIdx + 1, "set: cannot set " + ref);
            }
            lineIdx++;
            continue;
        }

        // Unknown keyword
        addError(scriptName, lineIdx + 1, "Unknown command: " + kw);
        lineIdx++;
    }
    return true;
}

ScriptValue ScriptEngine::parseValueToken(const std::string& tok,
                                          const std::unordered_map<std::string, ScriptValue>& localVars) {
    if (tok == "true") return ScriptValue::makeBool(true);
    if (tok == "false") return ScriptValue::makeBool(false);
    if (!tok.empty() && ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '-')) {
        bool hasDot = tok.find('.') != std::string::npos;
        try {
            if (hasDot) return ScriptValue::makeFloat(std::stod(tok));
            return ScriptValue::makeInt(std::stoll(tok));
        } catch (...) {}
    }
    if (tok.rfind("country.", 0) == 0 || tok.rfind("province.", 0) == 0 ||
        tok.rfind("map.", 0) == 0 || tok.rfind("var.", 0) == 0 ||
        tok.rfind("array.", 0) == 0 || tok.rfind("list.", 0) == 0 ||
        tok == "item" || tok.rfind("item.", 0) == 0 || localVars.count(tok))
        return resolveRef(tok, localVars);
    return ScriptValue::makeStr(tok); // plain string (quotes already stripped)
}

// array create|push|set|remove NAME …   /   list create|pushfront|pushback|popfront|popback NAME …
void ScriptEngine::execCollectionStmt(const std::vector<std::string>& tokens, const std::string& scriptName,
                                      int lineNum, const std::unordered_map<std::string, ScriptValue>& localVars) {
    const std::string& kind = tokens[0];
    if (tokens.size() < 3) {
        addError(scriptName, lineNum, kind + ": expected '" + kind + " <op> <name> ...'");
        return;
    }
    const std::string& op = tokens[1];
    const std::string& name = tokens[2];

    if (kind == "array") {
        if (op == "create") { m_arrays[name].clear(); m_arrays[name] = {}; return; }
        auto it = m_arrays.find(name);
        if (it == m_arrays.end()) {
            addError(scriptName, lineNum, "array: '" + name + "' does not exist (use 'array create " + name + "')");
            return;
        }
        if (op == "push") {
            if (tokens.size() < 4) { addError(scriptName, lineNum, "array push: missing value"); return; }
            it->second.push_back(parseValueToken(tokens[3], localVars));
            return;
        }
        if (op == "set") {
            if (tokens.size() < 5) { addError(scriptName, lineNum, "array set: expected NAME <index> <value>"); return; }
            long long idx = parseValueToken(tokens[3], localVars).asInt();
            if (idx < 0 || idx >= (long long)it->second.size()) {
                addError(scriptName, lineNum, "array set: index " + std::to_string(idx) + " out of range");
                return;
            }
            it->second[idx] = parseValueToken(tokens[4], localVars);
            return;
        }
        if (op == "remove") {
            if (tokens.size() < 4) { addError(scriptName, lineNum, "array remove: missing index"); return; }
            long long idx = parseValueToken(tokens[3], localVars).asInt();
            if (idx < 0 || idx >= (long long)it->second.size()) {
                addError(scriptName, lineNum, "array remove: index " + std::to_string(idx) + " out of range");
                return;
            }
            it->second.erase(it->second.begin() + idx);
            return;
        }
        addError(scriptName, lineNum, "array: unknown op '" + op + "' (create/push/set/remove)");
        return;
    }

    // list …
    if (op == "create") { m_lists[name].clear(); m_lists[name] = {}; return; }
    auto it = m_lists.find(name);
    if (it == m_lists.end()) {
        addError(scriptName, lineNum, "list: '" + name + "' does not exist (use 'list create " + name + "')");
        return;
    }
    if (op == "pushfront" || op == "pushback") {
        if (tokens.size() < 4) { addError(scriptName, lineNum, "list " + op + ": missing value"); return; }
        ScriptValue v = parseValueToken(tokens[3], localVars);
        if (op == "pushfront") it->second.push_front(v);
        else it->second.push_back(v);
        return;
    }
    if (op == "popfront" || op == "popback") {
        if (it->second.empty()) { addError(scriptName, lineNum, "list " + op + ": '" + name + "' is empty"); return; }
        if (op == "popfront") it->second.pop_front();
        else it->second.pop_back();
        return;
    }
    addError(scriptName, lineNum, "list: unknown op '" + op + "' (create/pushfront/pushback/popfront/popback)");
}


ScriptValue ScriptEngine::evalExpr(const std::string& expr,
                                    const std::unordered_map<std::string, ScriptValue>& localVars) {
    // ── THE REAL PARSER FIRST, THE OLD ONE AS A FALLBACK ──
    //
    // odscript handles precedence, parentheses, arithmetic and and/or; the
    // code below it handles exactly one comparison and cannot be given more.
    // Both are kept because the old one accepts something the new one cannot:
    // an UNQUOTED multi-word value, as in `if map.date == Modern Day`. That is
    // in shipped maps, and a stricter parser would fail them at load with a
    // syntax error, so a parse failure here means "try the old way" rather
    // than "the script is wrong". A parse that SUCCEEDS is always preferred.
    {
        bool ok = false;
        std::string err;
        ScriptValue v = odscript::evaluate(
            expr,
            [&](const std::string& name) { return resolveRef(name, localVars); },
            ok, err);
        if (ok) return v;
    }

    // Parse: <value> <operator> <value>  OR  just <value>
    std::string trimmed = expr;
    size_t s = trimmed.find_first_not_of(" \t");
    if (s != std::string::npos) trimmed = trimmed.substr(s);
    // Trim trailing
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\r'))
        trimmed.pop_back();

    // Find operator (only at top level — no nested expressions)
    // Operators: ==, !=, >, <, >=, <=
    std::string ops[] = {"==", "!=", ">=", "<=", ">", "<"};
    int opIdx = -1, opPos = -1;
    for (int i = 0; i < 6; i++) {
        // Search from the end to handle references with dots
        size_t pos = trimmed.rfind(ops[i]);
        if (pos != std::string::npos) {
            // Make sure it's not inside a quoted string
            opIdx = i;
            opPos = (int)pos;
            break;
        }
    }

    if (opIdx < 0) {
        // Just a value
        return resolveRef(trimmed, localVars);
    }

    std::string lhsStr = trimmed.substr(0, opPos);
    std::string rhsStr = trimmed.substr(opPos + ops[opIdx].size());
    // Trim
    size_t ls = lhsStr.find_first_not_of(" \t");
    if (ls != std::string::npos) lhsStr = lhsStr.substr(ls);
    while (!lhsStr.empty() && (lhsStr.back() == ' ' || lhsStr.back() == '\t')) lhsStr.pop_back();
    size_t rs = rhsStr.find_first_not_of(" \t");
    if (rs != std::string::npos) rhsStr = rhsStr.substr(rs);
    while (!rhsStr.empty() && (rhsStr.back() == ' ' || rhsStr.back() == '\t' || rhsStr.back() == '\r')) rhsStr.pop_back();

    ScriptValue lhs = resolveRef(lhsStr, localVars);
    ScriptValue rhs = resolveRef(rhsStr, localVars);
    return ScriptValue::makeBool(compareValues(lhs, ops[opIdx], rhs));
}

bool ScriptEngine::compareValues(const ScriptValue& lhs, const std::string& op, const ScriptValue& rhs) {
    if (op == "==") {
        if (lhs.type == ScriptValue::STRING || rhs.type == ScriptValue::STRING)
            return lhs.asString() == rhs.asString();
        return lhs.asFloat() == rhs.asFloat();
    }
    if (op == "!=") {
        if (lhs.type == ScriptValue::STRING || rhs.type == ScriptValue::STRING)
            return lhs.asString() != rhs.asString();
        return lhs.asFloat() != rhs.asFloat();
    }
    double l = lhs.asFloat(), r = rhs.asFloat();
    if (op == ">") return l > r;
    if (op == "<") return l < r;
    if (op == ">=") return l >= r;
    if (op == "<=") return l <= r;
    return false;
}

ScriptValue ScriptEngine::resolveRef(const std::string& ref,
                                     const std::unordered_map<std::string, ScriptValue>& localVars) {
    // Check local variables first
    auto vit = localVars.find(ref);
    if (vit != localVars.end()) return vit->second;

    // Check if it's a literal
    if (ref == "true") return ScriptValue::makeBool(true);
    if (ref == "false") return ScriptValue::makeBool(false);
    if (!ref.empty() && ref[0] >= '0' && ref[0] <= '9') {
        bool hasDot = ref.find('.') != std::string::npos;
        try {
            if (hasDot) return ScriptValue::makeFloat(std::stod(ref));
            return ScriptValue::makeInt(std::stoll(ref));
        } catch (...) {}
    }
    // Quoted string
    if (ref.size() >= 2 && ref.front() == '"' && ref.back() == '"')
        return ScriptValue::makeStr(ref.substr(1, ref.size() - 2));

    // Parse references
    // country.ISO.property
    // province.ID.property
    // map.property
    // country.ISO.at_war_with ISO
    // country.ISO.claims_province ID

    auto parts = tokenize(ref);
    if (parts.empty()) return ScriptValue{};

    std::string root = parts[0];
    std::vector<std::string> dots;
    {
        std::string cur;
        for (char c : root) {
            if (c == '.') { if (!cur.empty()) dots.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) dots.push_back(cur);
    }

    // var.NAME — script-global user variable
    if (dots.size() >= 2 && dots[0] == "var") {
        auto it = m_globals.find(dots[1]);
        return it != m_globals.end() ? it->second : ScriptValue{};
    }

    // array.NAME.length / array.NAME.<index>
    if (dots.size() >= 3 && dots[0] == "array") {
        auto it = m_arrays.find(dots[1]);
        if (it == m_arrays.end()) return ScriptValue{};
        if (dots[2] == "length") return ScriptValue::makeInt((long long)it->second.size());
        try {
            long long idx = std::stoll(dots[2]);
            if (idx >= 0 && idx < (long long)it->second.size()) return it->second[idx];
        } catch (...) {}
        return ScriptValue{};
    }

    // list.NAME.length / list.NAME.front / list.NAME.back
    if (dots.size() >= 3 && dots[0] == "list") {
        auto it = m_lists.find(dots[1]);
        if (it == m_lists.end()) return ScriptValue{};
        if (dots[2] == "length") return ScriptValue::makeInt((long long)it->second.size());
        if (dots[2] == "front") return it->second.empty() ? ScriptValue{} : it->second.front();
        if (dots[2] == "back") return it->second.empty() ? ScriptValue{} : it->second.back();
        return ScriptValue{};
    }

    if (dots.size() >= 2 && dots[0] == "map") {
        std::string prop = dots[1];
        if (prop == "date") {
            // Live date if the game has one (scripts can change it)
            if (!m_game->m_mapDate.empty()) return ScriptValue::makeStr(m_game->m_mapDate);
            // Fall back to metadata
            auto it = m_game->m_odmJsonData.find("metadata.json");
            if (it != m_game->m_odmJsonData.end()) {
                try {
                    auto j = nlohmann::json::parse(it->second);
                    return ScriptValue::makeStr(j.value("map_date", "Unknown"));
                } catch (...) {}
            }
            return ScriptValue::makeStr("Unknown");
        }
        if (prop == "name") {
            auto it = m_game->m_odmJsonData.find("metadata.json");
            if (it != m_game->m_odmJsonData.end()) {
                try {
                    auto j = nlohmann::json::parse(it->second);
                    return ScriptValue::makeStr(j.value("name", "Unknown"));
                } catch (...) {}
            }
            return ScriptValue::makeStr("Unknown");
        }
        if (prop == "turn") return ScriptValue::makeInt(m_game->m_turnNumber);
    }

    // country.ISO.property
    if (dots.size() >= 3 && dots[0] == "country") {
        std::string iso = dots[1];
        std::string prop = dots[2];
        int cid = -1;
        for (auto& [id, c] : m_game->m_countries.getAll())
            if (c.isoA3 == iso) { cid = id; break; }
        if (cid < 0) return ScriptValue{};
        const Country* c = m_game->m_countries.getCountry(cid);
        if (!c) return ScriptValue{};

        if (prop == "treasury") return ScriptValue::makeFloat(c->treasury);
        if (prop == "name") return ScriptValue::makeStr(c->name);
        if (prop == "iso") return ScriptValue::makeStr(c->isoA3);
        if (prop == "color") return ScriptValue::makeStr("color");

        // country.ISO.at_war_with ISO
        if (prop == "at_war_with" && dots.size() >= 4) {
            std::string otherIso = dots[3];
            auto it = m_game->m_relations.find(iso);
            if (it != m_game->m_relations.end()) {
                auto jt = it->second.find(otherIso);
                if (jt != it->second.end()) return ScriptValue::makeBool(jt->second.war);
            }
            return ScriptValue::makeBool(false);
        }
        // country.ISO.allied_with ISO
        if (prop == "allied_with" && dots.size() >= 4) {
            std::string otherIso = dots[3];
            auto it = m_game->m_relations.find(iso);
            if (it != m_game->m_relations.end()) {
                auto jt = it->second.find(otherIso);
                if (jt != it->second.end()) return ScriptValue::makeBool(jt->second.alliance);
            }
            return ScriptValue::makeBool(false);
        }
        // country.ISO.claims_province ID
        if (prop == "claims_province" && dots.size() >= 4) {
            int pid = 0;
            try { pid = std::stoi(dots[3]); } catch (...) { return ScriptValue::makeBool(false); }
            auto it = m_game->m_claims.find(iso);
            if (it != m_game->m_claims.end()) {
                return ScriptValue::makeBool(std::find(it->second.begin(), it->second.end(), pid) != it->second.end());
            }
            return ScriptValue::makeBool(false);
        }
        // country.ISO.province_count
        if (prop == "province_count") {
            int count = 0;
            for (auto& [pid, prov] : m_game->m_provinces.getAllProvinces())
                if (prov.countryId == cid) count++;
            return ScriptValue::makeInt(count);
        }
    }

    // province.ID.property
    if (dots.size() >= 3 && dots[0] == "province") {
        int pid = 0;
        try { pid = std::stoi(dots[1]); } catch (...) { return ScriptValue{}; }
        std::string prop = dots[2];
        Province* p = m_game->m_provinces.getProvinceById(pid);
        if (!p) return ScriptValue{};

        if (prop == "population") {
            auto it = m_game->m_provincePopulations.find(pid);
            return ScriptValue::makeInt(it != m_game->m_provincePopulations.end() ? it->second : 0);
        }
        if (prop == "owner") {
            const Country* c = m_game->m_countries.getCountry(p->countryId);
            return ScriptValue::makeStr(c ? c->isoA3 : "");
        }
        if (prop == "name") return ScriptValue::makeStr(p->name);
        if (prop == "industry") {
            auto it = m_game->m_provinceIndustry.find(pid);
            return ScriptValue::makeInt(it != m_game->m_provinceIndustry.end() ? it->second.level : 0);
        }
        if (prop == "fortification") {
            auto it = m_game->m_provinceIndustry.find(pid);
            return ScriptValue::makeInt(it != m_game->m_provinceIndustry.end() ? it->second.fortification : 0);
        }
    }

    return ScriptValue::makeStr(ref); // unknown ref — return as string
}

bool ScriptEngine::setRef(const std::string& ref, const ScriptValue& val,
                           const std::unordered_map<std::string, ScriptValue>& localVars) {
    auto parts = tokenize(ref);
    if (parts.empty()) return false;
    std::string root = parts[0];
    std::vector<std::string> dots;
    {
        std::string cur;
        for (char c : root) {
            if (c == '.') { if (!cur.empty()) dots.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) dots.push_back(cur);
    }

    if (dots.empty()) return false;

    // set country.ISO.treasury value
    if (dots.size() >= 3 && dots[0] == "country") {
        std::string iso = dots[1];
        std::string prop = dots[2];
        int cid = -1;
        for (auto& [id, c] : m_game->m_countries.getAll())
            if (c.isoA3 == iso) { cid = id; break; }
        if (cid < 0) return false;

        if (prop == "treasury") { m_game->m_countries.getAll()[cid].treasury = (float)val.asFloat(); return true; }
        if (prop == "name") { m_game->m_countries.getAll()[cid].name = val.asString(); return true; }
        // set country.ISO.at_war_with ISO true/false
        if (prop == "at_war_with" && dots.size() >= 4) {
            std::string otherIso = dots[3];
            bool war = val.asBool();
            m_game->m_relations[iso][otherIso].war = war;
            m_game->m_relations[otherIso][iso].war = war;
            return true;
        }
        // set country.ISO.allied_with ISO true/false
        if (prop == "allied_with" && dots.size() >= 4) {
            std::string otherIso = dots[3];
            bool ally = val.asBool();
            m_game->m_relations[iso][otherIso].alliance = ally;
            m_game->m_relations[otherIso][iso].alliance = ally;
            return true;
        }
        return false;
    }

    // set province.ID.property value
    if (dots.size() >= 3 && dots[0] == "province") {
        int pid = 0;
        try { pid = std::stoi(dots[1]); } catch (...) { return false; }
        std::string prop = dots[2];
        Province* p = m_game->m_provinces.getProvinceById(pid);
        if (!p) return false;

        if (prop == "population") {
            m_game->m_provincePopulations[pid] = val.asInt();
            if ((size_t)pid < m_game->m_provincePopArray.size())
                m_game->m_provincePopArray[pid] = val.asInt();
            return true;
        }
        if (prop == "owner") {
            std::string iso = val.asString();
            int newCid = -1;
            for (auto& [id, c] : m_game->m_countries.getAll())
                if (c.isoA3 == iso) { newCid = id; break; }
            if (newCid < 0) return false;
            const int oldCid = p->countryId; // read before the write: the index needs both sides
            p->countryId = newCid;
            if ((size_t)pid < m_game->m_provinceCountryLookup.size())
                m_game->m_provinceCountryLookup[pid] = newCid;
            m_game->reindexProvinceOwner(pid, oldCid, newCid);
            // Update pixel array
            auto ppIt = m_game->m_provincePixels.find(pid);
            if (ppIt != m_game->m_provincePixels.end()) {
                for (int idx : ppIt->second) {
                    if (idx >= 0 && idx < (int)m_game->m_pixelCountryArray.size())
                        m_game->m_pixelCountryArray[idx] = (uint16_t)newCid;
                }
            }
            return true;
        }
        if (prop == "industry") {
            auto it = m_game->m_provinceIndustry.find(pid);
            if (it != m_game->m_provinceIndustry.end()) {
                it->second.level = (int)val.asInt();
                return true;
            }
            return false;
        }
        if (prop == "fortification") {
            auto it = m_game->m_provinceIndustry.find(pid);
            if (it != m_game->m_provinceIndustry.end()) {
                it->second.fortification = (int)val.asInt();
                return true;
            }
            return false;
        }
    }

    // set var.NAME value — script-global user variable
    if (dots.size() >= 2 && dots[0] == "var") {
        m_globals[dots[1]] = val;
        return true;
    }

    // set map.date "Month Year" — the only writable map property
    if (dots.size() >= 2 && dots[0] == "map") {
        if (dots[1] == "date") {
            m_game->m_mapDate = val.asString();
            return true;
        }
        return false;
    }

    return false;
}