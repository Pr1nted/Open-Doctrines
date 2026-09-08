#include "Advisor.h"

#include "../Mail.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace llm {

std::string systemPrompt(const Persona& persona, const Situation& situation,
                         const std::string& languageName) {
    std::ostringstream p;
    p << "You are the foreign minister of " << persona.countryName
      << " in a turn-based strategy game. You are writing a private letter to "
      << persona.correspondent << ".\n\n";

    p << "HOW TO WRITE\n";
    p << "- Write only the letter. No preamble, no explanation, no stage directions.\n";
    p << "- Two or three short paragraphs at most. This is correspondence, not a speech.\n";
    p << "- Never use markdown, headings or bullet points.\n";
    p << "- Do not sign it and do not write your own name at the top.\n";
    // The one instruction that is about the game rather than the prose. A model
    // told only "be a diplomat" plays a helpful assistant in period costume.
    p << "- Write in " << languageName << ".\n\n";

    p << "WHO YOU ARE\n";
    p << "- You want what is good for " << persona.countryName
      << ", and you are under no obligation to be honest about it.\n";
    p << "- You may bluff, flatter, conceal your intentions, and promise things\n"
         "  your country may not do. A diplomat who never misleads is not one.\n";
    p << "- You may not invent events that did not happen in the game, quote\n"
         "  letters you were not sent, or claim to know things nobody told you.\n";
    p << "- Never claim to be a human being. If asked what you are, say plainly\n"
         "  that you are an artificial correspondent.\n";

    if (persona.toAnotherAdvisor) {
        p << "- Your correspondent is another artificial minister, not a person.\n";
    }
    if (!persona.standing.empty()) p << "- " << persona.standing << "\n";

    p << "\nTHE SITUATION\n";
    p << "- It is turn " << situation.turn;
    if (!situation.date.empty()) p << ", " << situation.date;
    p << ".\n";
    p << "- You are " << (situation.atWar ? "AT WAR with" : "at peace with") << " "
      << persona.correspondent << ".\n";
    if (!situation.relativeStrength.empty()) {
        p << "- Their position relative to yours: " << situation.relativeStrength << ".\n";
    }
    if (!situation.proximity.empty()) p << "- " << situation.proximity << ".\n";
    if (!situation.pact.empty()) {
        p << "- You and " << persona.correspondent << " have " << situation.pact << ".\n";
    }
    if (!situation.theirWars.empty()) p << "- " << situation.theirWars << ".\n";
    if (!situation.ourWars.empty())   p << "- " << situation.ourWars << ".\n";
    if (!situation.ourFortunes.empty()) p << "- " << situation.ourFortunes << ".\n";
    if (!situation.historyWithThem.empty()) {
        p << "- " << situation.historyWithThem << ".\n";
    }

    // Deliberately no army counts, no treasury, no province list. A
    // correspondent who can quote your exact strength is reading the save file,
    // and a player can tell instantly. Everything above is the kind of thing a
    // foreign ministry would actually know: who is fighting whom, whether there
    // is a treaty, whether the war has been going well.
    p << "- You know only what has been said in this correspondence and the\n"
         "  broad situation above. You do not have access to their orders.\n";
    p << "\nWrite as somebody who knows all of that and is deciding how much of\n"
         "it to admit.\n";
    return p.str();
}

std::vector<Turn> buildConversation(const std::vector<mail::Message>& thread, int me,
                                    const Persona& persona, const Situation& situation,
                                    const std::string& languageName, size_t maxLetters) {
    std::vector<Turn> out;
    out.push_back(Turn{"system", systemPrompt(persona, situation, languageName)});

    // Only what was actually delivered. A pending letter has not been sent, so
    // the recipient has not read it -- feeding it to the model would let an
    // advisor answer a letter the player is still deciding whether to send.
    std::vector<const mail::Message*> visible;
    for (const mail::Message& m : thread) {
        if (m.status != mail::Status::Delivered) continue;
        visible.push_back(&m);
    }
    // The most recent, when there are more than fit. Old letters fall off the
    // front rather than the back: a correspondence is answered from its end.
    const size_t start = visible.size() > maxLetters ? visible.size() - maxLetters : 0;
    for (size_t i = start; i < visible.size(); ++i) {
        const mail::Message& m = *visible[i];
        out.push_back(Turn{m.fromCountry == me ? "assistant" : "user", m.body});
    }
    return out;
}

std::string tidyReply(std::string text, const std::string& countryName) {
    auto trim = [](std::string& s) {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) { s.clear(); return; }
        const size_t b = s.find_last_not_of(" \t\r\n");
        s = s.substr(a, b - a + 1);
    };
    trim(text);

    // Models routinely answer a "write a letter" instruction with the letter in
    // quotation marks, which then appears inside the bubble as if the country
    // were quoting somebody.
    //
    // EACH END INDEPENDENTLY. Requiring both to match left a one-sided quote
    // behind -- a real reply ended `...mutual respect."` with no opening one,
    // and the stray mark shipped straight into the letter. A model that opens a
    // quotation and forgets to close it is commoner than one that does neither.
    auto quoteMark = [](char c) { return c == '"' || c == '\''; };
    while (!text.empty() && quoteMark(text.front())) { text.erase(0, 1); trim(text); }
    while (!text.empty() && quoteMark(text.back()))  { text.pop_back(); trim(text); }
    // The curly variants, as whole UTF-8 sequences.
    static const char* kCurly[] = {"\xE2\x80\x9C", "\xE2\x80\x9D",
                                   "\xE2\x80\x98", "\xE2\x80\x99"};
    for (const char* q : kCurly) {
        const std::string mark = q;
        while (text.size() > mark.size() && text.compare(0, mark.size(), mark) == 0) {
            text.erase(0, mark.size());
            trim(text);
        }
        while (text.size() > mark.size() &&
               text.compare(text.size() - mark.size(), mark.size(), mark) == 0) {
            text.resize(text.size() - mark.size());
            trim(text);
        }
    }

    // And with "France: ..." or "Foreign Minister of France:" on the front,
    // because they have been told who they are and want to prove it.
    const size_t colon = text.find(':');
    if (colon != std::string::npos && colon < 60) {
        const std::string head = text.substr(0, colon);
        if (head.find(countryName) != std::string::npos ||
            head.find("Minister") != std::string::npos ||
            head.find("Letter") != std::string::npos) {
            text = text.substr(colon + 1);
            trim(text);
        }
    }

    // A salutation, which the instruction does not forbid and models add anyway.
    //
    // "My esteemed Chancellor," on the front of every letter reads as a form
    // letter after the third one, and the screen already says who it is to. Cut
    // only when it is short, on its own line, and ends in a comma -- which is
    // what a salutation looks like and what an opening sentence does not.
    {
        const size_t nl = text.find('\n');
        if (nl != std::string::npos && nl < 48) {
            std::string first = text.substr(0, nl);
            while (!first.empty() && (first.back() == ' ' || first.back() == '\r'))
                first.pop_back();
            if (!first.empty() && first.back() == ',' &&
                first.find('.') == std::string::npos) {
                text.erase(0, nl + 1);
                trim(text);
            }
        }
    }

    // Markdown emphasis, which the instruction forbids and models emit anyway.
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '*' || text[i] == '#' || text[i] == '`') continue;
        out += text[i];
    }
    trim(out);

    if (out.size() > mail::kMaxBody) {
        // Cut at a sentence if there is one nearby, so a trimmed letter still
        // ends like a letter rather than mid-word.
        out.resize(mail::kMaxBody);
        const size_t stop = out.find_last_of(".!?");
        if (stop != std::string::npos && stop > mail::kMaxBody - 300) out.resize(stop + 1);
    }
    return out;
}

bool isLocal(const std::string& baseUrl) {
    // Substring on the HOST only. "http://localhost.evil.example" contains
    // "localhost" and is not local, which is the trap this shape avoids.
    const size_t scheme = baseUrl.find("://");
    if (scheme == std::string::npos) return false;
    const size_t hostStart = scheme + 3;
    const size_t hostEnd = baseUrl.find_first_of(":/", hostStart);
    const std::string host = baseUrl.substr(hostStart, hostEnd == std::string::npos
                                                       ? std::string::npos
                                                       : hostEnd - hostStart);
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]" || host == "::1";
}

namespace {

/// JSON string escaping. Same rules as Feedback.cpp; kept local rather than
/// shared because pulling a JSON dependency into the game for two call sites
/// would be a worse trade than twenty lines twice.
std::string esc(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

std::string jsonEscape(const std::string& in) { return esc(in); }

}  // namespace

std::string chatRequestBody(const std::vector<Turn>& turns, const std::string& model,
                            int maxTokens) {
    std::ostringstream b;
    b << "{\"model\":\"" << esc(model) << "\",\"messages\":[";
    for (size_t i = 0; i < turns.size(); ++i) {
        if (i) b << ",";
        b << "{\"role\":\"" << esc(turns[i].role) << "\",\"content\":\""
          << esc(turns[i].content) << "\"";
        // A tool's answer has to say which call it answers. Runners differ on
        // whether they require it, so it is sent whenever the call carried one.
        if (!turns[i].toolCallId.empty()) {
            b << ",\"tool_call_id\":\"" << esc(turns[i].toolCallId) << "\"";
        }
        if (!turns[i].toolName.empty()) {
            b << ",\"name\":\"" << esc(turns[i].toolName) << "\"";
        }
        b << "}";
    }
    b << "],\"max_tokens\":" << maxTokens
      // Warm rather than wild. A diplomat who is too predictable is a template
      // and one who is too random stops tracking the conversation.
      << ",\"temperature\":0.85,\"stream\":false}";
    return b.str();
}

namespace {

/// Read a JSON string value that starts at `at` (the opening quote).
std::string readJsonString(const std::string& s, size_t at, size_t* end = nullptr) {
    std::string out;
    if (at >= s.size() || s[at] != '"') return out;
    size_t i = at + 1;
    for (; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            switch (n) {
                case 'n': out += '\n'; break;
                case 'r': break;                  // dropped: see Mail's wrapping
                case 't': out += '\t'; break;
                case 'u': {
                    if (i + 4 < s.size()) {
                        const unsigned cp = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
                        i += 4;
                        // Back to UTF-8. Surrogate pairs are left as-is rather
                        // than half-decoded; runners send raw UTF-8 in practice.
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: out += n; break;
            }
            continue;
        }
        if (s[i] == '"') break;
        out += s[i];
    }
    if (end) *end = i;
    return out;
}

}  // namespace

// ─────────────────────────────────────────────── letting it look things up ────

namespace {

constexpr Tool kTools[] = {
    {"standing_with",
     "How your country currently stands with another: whether you are at war, "
     "what treaty you have, and whether you share a border.",
     "country", "The country's name, as it appears in your correspondence."},
    {"who_is_fighting",
     "Which countries a given country is at war with right now. Public "
     "knowledge; every ministry in Europe knows it.",
     "country", "The country's name."},
    {"strength_of",
     "How a country's position compares with yours, in general terms. Never an "
     "exact figure -- no ministry has one.",
     "country", "The country's name."},
    {"our_correspondence",
     "What you and a country have written to each other so far, in general "
     "terms: how much, and how it has gone.",
     "country", "The country's name."},
};

std::string jsonEscape(const std::string& in);

}  // namespace

const Tool* tools(int* count) {
    if (count) *count = (int)(sizeof(kTools) / sizeof(kTools[0]));
    return kTools;
}

std::string toolsJson() {
    std::ostringstream j;
    j << "[";
    int count = 0;
    const Tool* list = tools(&count);
    for (int i = 0; i < count; ++i) {
        if (i) j << ",";
        j << "{\"type\":\"function\",\"function\":{"
          << "\"name\":\"" << list[i].name << "\","
          << "\"description\":\"" << jsonEscape(list[i].description) << "\","
          << "\"parameters\":{\"type\":\"object\",\"properties\":{"
          << "\"" << list[i].argName << "\":{\"type\":\"string\",\"description\":\""
          << jsonEscape(list[i].argDescription) << "\"}},"
          << "\"required\":[\"" << list[i].argName << "\"]}}}";
    }
    j << "]";
    return j.str();
}

std::string chatRequestBodyWithTools(const std::vector<Turn>& turns,
                                     const std::string& model, int maxTokens) {
    std::string body = chatRequestBody(turns, model, maxTokens);
    // Spliced in rather than rebuilt: chatRequestBody is what the plain path
    // uses and is tested, and two builders would drift.
    const std::string tail = ",\"stream\":false}";
    if (body.size() >= tail.size() &&
        body.compare(body.size() - tail.size(), tail.size(), tail) == 0) {
        body.resize(body.size() - tail.size());
        body += ",\"stream\":false,\"tools\":" + toolsJson() + "}";
    }
    return body;
}

std::vector<ToolCall> toolCallsFromResponse(const std::string& json) {
    std::vector<ToolCall> out;
    // Only inside a tool_calls array. Searching the whole reply for "name"
    // would happily pick up the model's own prose.
    size_t at = json.find("\"tool_calls\"");
    if (at == std::string::npos) return out;

    // WITH THE COLON. Searching for `"function"` alone also matches the VALUE in
    // `"type":"function"`, which every call carries -- so one call parsed as
    // two, the second with no name and no argument. The colon is what makes it
    // a key rather than any occurrence of the word.
    const std::string kFnKey = "\"function\":";
    while (true) {
        const size_t fn = json.find(kFnKey, at);
        if (fn == std::string::npos) break;
        const size_t nameAt = json.find("\"name\"", fn);
        if (nameAt == std::string::npos) break;

        ToolCall call;
        size_t q = json.find('"', json.find(':', nameAt) + 1);
        if (q == std::string::npos) break;
        call.name = readJsonString(json, q);

        // The id, when the runner gave one. Some do not, and a missing id is
        // not an error -- it only has to come back on the result turn.
        const size_t idAt = json.rfind("\"id\"", fn);
        if (idAt != std::string::npos && idAt > at) {
            const size_t iq = json.find('"', json.find(':', idAt) + 1);
            if (iq != std::string::npos) call.id = readJsonString(json, iq);
        }

        // Arguments arrive as a STRING containing JSON, which is the shape the
        // API specifies and the thing most easily got wrong.
        const size_t argsAt = json.find("\"arguments\"", fn);
        if (argsAt != std::string::npos) {
            const size_t aq = json.find('"', json.find(':', argsAt) + 1);
            if (aq != std::string::npos) {
                const std::string inner = readJsonString(json, aq);
                // One argument, whatever it is called: take the first string
                // value in the object rather than matching the name, because a
                // model that renames the parameter is commoner than one that
                // sends two.
                const size_t vq = inner.find('"', inner.find(':') + 1);
                if (vq != std::string::npos) call.argument = readJsonString(inner, vq);
            }
        }
        if (!call.name.empty()) out.push_back(call);

        at = fn + kFnKey.size();
        if (out.size() >= 8) break;   // a model asking eight things has lost the thread
    }
    return out;
}

Turn toolResultTurn(const ToolCall& call, const std::string& answer) {
    Turn t;
    t.role = "tool";
    t.content = answer;
    t.toolCallId = call.id;
    t.toolName = call.name;
    return t;
}

std::string replyFromResponse(const std::string& json) {
    // Every shape this has to handle puts the text after a "content" key:
    //   OpenAI / llama.cpp server : choices[0].message.content
    //   Ollama native             : message.content
    // Finding the first "content" whose value is a non-empty string covers all
    // of them without a parser, and returns empty for anything else -- which
    // the caller treats as "no letter this turn" rather than as an error.
    size_t at = 0;
    while ((at = json.find("\"content\"", at)) != std::string::npos) {
        size_t q = json.find('"', at + 9);
        // Skip the colon and any whitespace to the value's opening quote.
        const size_t colon = json.find(':', at + 9);
        if (colon == std::string::npos) return "";
        q = json.find_first_not_of(" \t\r\n", colon + 1);
        if (q == std::string::npos) return "";
        if (json[q] != '"') { at = colon + 1; continue; }   // null, object, etc.
        const std::string value = readJsonString(json, q);
        if (!value.empty()) return value;
        at = q + 1;
    }
    return "";
}

}  // namespace llm
