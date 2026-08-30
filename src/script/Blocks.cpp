#include "Blocks.h"
#include <algorithm>
#include <cctype>

namespace odscript {
namespace {

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

std::string firstWord(const std::string& s) {
    const std::string t = trimmed(s);
    const size_t sp = t.find_first_of(" \t");
    return sp == std::string::npos ? t : t.substr(0, sp);
}

bool isComment(const std::string& t) { return !t.empty() && t[0] == '#'; }

bool isOpener(const std::string& w) {
    return w == "if" || w == "unless" || w == "foreach" || w == "while" ||
           w == "for" || w == "repeat";
}
bool isTerminator(const std::string& w) {
    return w == "endif" || w == "next" || w == "endwhile";
}
bool isArm(const std::string& w) { return w == "else" || w == "elseif"; }

struct Parser {
    std::vector<std::string> lines;
    size_t i = 0;
    std::string error;

    // Collects comments and blanks so they can be re-emitted above whatever
    // follows. Losing them would make the round trip lossy in the way authors
    // notice first.
    std::vector<std::string> takeLead() {
        std::vector<std::string> lead;
        while (i < lines.size()) {
            const std::string t = trimmed(lines[i]);
            if (t.empty() || isComment(t)) { lead.push_back(t); ++i; }
            else break;
        }
        return lead;
    }

    // Reads statements until a terminator or arm at this level. `stopWord`
    // receives whichever it stopped on ("" at end of input).
    BlockList parseBody(std::string& stopWord, std::vector<std::string>& tailOut) {
        BlockList out;
        for (;;) {
            std::vector<std::string> lead = takeLead();
            if (i >= lines.size()) { tailOut = lead; stopWord.clear(); return out; }

            const std::string line = trimmed(lines[i]);
            const std::string w = firstWord(line);

            if (isTerminator(w) || isArm(w)) { tailOut = lead; stopWord = w; return out; }

            Block b;
            b.lead = std::move(lead);
            b.head = line;
            b.kind = classify(line);
            ++i;

            if (isOpener(w)) {
                for (;;) {
                    std::string stop;
                    std::vector<std::string> tail;
                    BlockList body = parseBody(stop, tail);
                    b.bodies.push_back(std::move(body));
                    if (stop.empty()) {
                        if (error.empty())
                            error = "missing " + std::string(w == "if" || w == "unless" ? "endif"
                                          : (w == "while" ? "endwhile" : "next")) +
                                    " for '" + line + "'";
                        b.tail = tail;
                        return out.push_back(std::move(b)), out;
                    }
                    if (isArm(stop)) {
                        // Only `if`/`unless` take arms; anywhere else it is a
                        // stray line and belongs to the body verbatim.
                        b.armHeads.push_back(trimmed(lines[i]));
                        ++i;
                        continue;
                    }
                    b.terminator = trimmed(lines[i]);
                    b.tail = tail;
                    ++i;
                    break;
                }
            }
            out.push_back(std::move(b));
        }
    }
};

void emit(const BlockList& bl, std::string& out, int depth, int indent) {
    const std::string pad(depth * indent, ' ');
    for (const Block& b : bl) {
        for (const std::string& l : b.lead) out += l.empty() ? "\n" : pad + l + "\n";
        out += pad + b.head + "\n";
        for (size_t a = 0; a < b.bodies.size(); ++a) {
            emit(b.bodies[a], out, depth + 1, indent);
            if (a < b.armHeads.size()) out += pad + b.armHeads[a] + "\n";
        }
        if (!b.tail.empty())
            for (const std::string& l : b.tail)
                out += l.empty() ? "\n" : std::string((depth + 1) * indent, ' ') + l + "\n";
        if (!b.terminator.empty()) out += pad + b.terminator + "\n";
    }
}

}  // namespace

Block::Kind classify(const std::string& line) {
    const std::string w = firstWord(line);
    if (w == "set") return Block::SET;
    if (w == "print") return Block::PRINT;
    if (w == "include") return Block::INCLUDE;
    if (w == "waitUntil") return Block::WAIT;
    if (w == "array" || w == "list") return Block::COLLECTION;
    if (w == "break") return Block::BREAK;
    if (w == "continue") return Block::CONTINUE;
    if (w == "if" || w == "unless") return Block::IF;
    if (w == "foreach") return Block::FOREACH;
    if (w == "while") return Block::WHILE;
    if (w == "for") return Block::FOR;
    if (w == "repeat") return Block::REPEAT;
    return Block::RAW;
}

Block* blockAt(BlockList& root, const std::vector<int>& path) {
    if (path.empty()) return nullptr;
    BlockList* list = &root;
    for (size_t i = 0; i + 1 < path.size(); i += 2) {
        const int bi = path[i];
        if (bi < 0 || bi >= (int)list->size()) return nullptr;
        Block& b = (*list)[bi];
        const int body = path[i + 1];
        if (body < 0 || body >= (int)b.bodies.size()) return nullptr;
        list = &b.bodies[body];
    }
    const int last = path.back();
    if (last < 0 || last >= (int)list->size()) return nullptr;
    return &(*list)[last];
}

// Walks to the body a path addresses, without touching the final index.
static BlockList* parentList(BlockList& root, const std::vector<int>& path) {
    if (path.empty()) return nullptr;
    BlockList* list = &root;
    for (size_t i = 0; i + 1 < path.size(); i += 2) {
        const int bi = path[i];
        if (bi < 0 || bi >= (int)list->size()) return nullptr;
        Block& b = (*list)[bi];
        const int body = path[i + 1];
        if (body < 0 || body >= (int)b.bodies.size()) return nullptr;
        list = &b.bodies[body];
    }
    return list;
}

bool removeBlockAt(BlockList& root, const std::vector<int>& path, Block& out) {
    BlockList* list = parentList(root, path);
    if (!list) return false;
    const int last = path.back();
    if (last < 0 || last >= (int)list->size()) return false;
    out = std::move((*list)[last]);
    list->erase(list->begin() + last);
    return true;
}

bool insertBlockAt(BlockList& root, const std::vector<int>& path, Block&& b) {
    BlockList* list = parentList(root, path);
    if (!list) return false;
    int last = path.back();
    last = std::max(0, std::min(last, (int)list->size()));
    list->insert(list->begin() + last, std::move(b));
    return true;
}

bool pathContains(const std::vector<int>& outer, const std::vector<int>& inner) {
    if (inner.size() < outer.size()) return false;
    for (size_t i = 0; i < outer.size(); ++i)
        if (outer[i] != inner[i]) return false;
    return true;
}

Doc parseScript(const std::string& text) {
    Doc doc;
    Parser p;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { p.lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    if (!cur.empty()) p.lines.push_back(cur);

    // The engine header and anything above the first statement stay put.
    while (p.i < p.lines.size()) {
        const std::string t = trimmed(p.lines[p.i]);
        if (t.empty() || isComment(t)) { doc.header.push_back(t); ++p.i; }
        else break;
    }

    std::string stop;
    std::vector<std::string> tail;
    doc.blocks = p.parseBody(stop, tail);
    doc.trailing = tail;
    if (!stop.empty() && p.error.empty())
        p.error = "stray '" + stop + "' with nothing open";
    doc.error = p.error;
    return doc;
}

std::string unparseScript(const Doc& doc, int indent) {
    std::string out;
    for (const std::string& l : doc.header) out += l.empty() ? "\n" : l + "\n";
    emit(doc.blocks, out, 0, indent);
    for (const std::string& l : doc.trailing) out += l.empty() ? "\n" : l + "\n";
    return out;
}

}  // namespace odscript
