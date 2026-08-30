#include "Expr.h"
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace odscript {
namespace {

// ── Tokens ──
struct Tok {
    enum Kind { NUM, STR, IDENT, OP, LPAREN, RPAREN, END };
    Kind kind = END;
    std::string text;
    ScriptValue val;
};

bool identStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
// Dots and digits continue an identifier so `province.42.population` is one
// token rather than a reference minus a number.
bool identBody(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.'; }

bool lex(const std::string& s, std::vector<Tok>& out, std::string& err) {
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }

        if (c == '(') { out.push_back({Tok::LPAREN, "(", {}}); ++i; continue; }
        if (c == ',') { out.push_back({Tok::OP, ",", {}}); ++i; continue; }
        if (c == ')') { out.push_back({Tok::RPAREN, ")", {}}); ++i; continue; }

        if (c == '"') {
            std::string lit;
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) ++i;   // \" keeps the quote
                lit.push_back(s[i++]);
            }
            if (i >= s.size()) { err = "unterminated string"; return false; }
            ++i;                                             // closing quote
            Tok t{Tok::STR, lit, ScriptValue::makeStr(lit)};
            out.push_back(t);
            continue;
        }

        // A number. Leading '-' is handled as unary minus by the parser, so a
        // digit is the only thing that starts one -- otherwise `a-1` would lex
        // as `a` and `-1` and silently become two operands with no operator.
        if (std::isdigit((unsigned char)c)) {
            size_t j = i;
            bool dot = false;
            while (j < s.size() && (std::isdigit((unsigned char)s[j]) || (s[j] == '.' && !dot))) {
                if (s[j] == '.') {
                    // Only a fractional point if a digit follows; otherwise it
                    // belongs to something else and the number ends here.
                    if (j + 1 >= s.size() || !std::isdigit((unsigned char)s[j + 1])) break;
                    dot = true;
                }
                ++j;
            }
            std::string num = s.substr(i, j - i);
            Tok t{Tok::NUM, num, dot ? ScriptValue::makeFloat(std::strtod(num.c_str(), nullptr))
                                     : ScriptValue::makeInt(std::strtoll(num.c_str(), nullptr, 10))};
            out.push_back(t);
            i = j;
            continue;
        }

        if (identStart(c)) {
            size_t j = i;
            while (j < s.size() && identBody(s[j])) ++j;
            std::string word = s.substr(i, j - i);
            // The word forms of the boolean operators. `true`/`false` stay
            // literals; everything else is a reference for the caller.
            if (word == "and" || word == "or" || word == "not")
                out.push_back({Tok::OP, word, {}});
            else if (word == "true")  out.push_back({Tok::NUM, word, ScriptValue::makeBool(true)});
            else if (word == "false") out.push_back({Tok::NUM, word, ScriptValue::makeBool(false)});
            else out.push_back({Tok::IDENT, word, {}});
            i = j;
            continue;
        }

        // Operators, two-character forms first so `>=` never lexes as `>`.
        static const char* two[] = {"==", "!=", "<=", ">=", "&&", "||"};
        bool matched = false;
        for (const char* t : two) {
            if (s.compare(i, 2, t) == 0) {
                out.push_back({Tok::OP, t, {}});
                i += 2; matched = true; break;
            }
        }
        if (matched) continue;
        if (std::string("+-*/%<>!").find(c) != std::string::npos) {
            out.push_back({Tok::OP, std::string(1, c), {}});
            ++i; continue;
        }
        err = std::string("unexpected character '") + c + "'";
        return false;
    }
    out.push_back({Tok::END, "", {}});
    return true;
}

// ── Precedence ──
// Higher binds tighter. Comparison sits below arithmetic so `a + 1 > b` groups
// the way it reads.
int binPrec(const std::string& op) {
    if (op == "or"  || op == "||") return 1;
    if (op == "and" || op == "&&") return 2;
    if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") return 3;
    if (op == "+" || op == "-") return 4;
    if (op == "*" || op == "/" || op == "%") return 5;
    return 0;
}
const int UNARY_PREC = 6;

struct Parser {
    std::vector<Tok> toks;
    size_t pos = 0;
    std::string err;

    const Tok& peek() const { return toks[pos]; }
    const Tok& take() { return toks[pos++]; }

    NodePtr parseExpr(int minPrec) {
        NodePtr lhs = parseUnary();
        if (!lhs) return nullptr;
        while (peek().kind == Tok::OP) {
            const std::string op = peek().text;
            const int prec = binPrec(op);
            if (prec == 0 || prec < minPrec) break;
            take();
            NodePtr rhs = parseExpr(prec + 1);   // left-associative
            if (!rhs) return nullptr;
            auto n = std::make_unique<Node>();
            n->kind = Node::BINARY;
            n->text = op;
            n->kids.push_back(std::move(lhs));
            n->kids.push_back(std::move(rhs));
            lhs = std::move(n);
        }
        return lhs;
    }

    NodePtr parseUnary() {
        if (peek().kind == Tok::OP && (peek().text == "-" || peek().text == "not" || peek().text == "!")) {
            const std::string op = take().text;
            NodePtr operand = parseExpr(UNARY_PREC);
            if (!operand) return nullptr;
            auto n = std::make_unique<Node>();
            n->kind = Node::UNARY;
            n->text = (op == "!") ? "not" : op;
            n->kids.push_back(std::move(operand));
            return n;
        }
        return parsePrimary();
    }

    NodePtr parsePrimary() {
        const Tok& t = peek();
        if (t.kind == Tok::NUM || t.kind == Tok::STR) {
            auto n = std::make_unique<Node>();
            n->kind = Node::LITERAL;
            n->literal = t.val;
            n->text = t.text;
            take();
            return n;
        }
        if (t.kind == Tok::IDENT) {
            const std::string name = t.text;
            take();
            // `name(` is a call; a bare name is a reference. Checked here
            // rather than by a keyword list, so a map that happens to have a
            // reference called `min` is not broken by the function's existence.
            if (peek().kind == Tok::LPAREN) {
                take();
                auto n = std::make_unique<Node>();
                n->kind = Node::CALL;
                n->text = name;
                if (peek().kind != Tok::RPAREN) {
                    for (;;) {
                        NodePtr arg = parseExpr(1);
                        if (!arg) return nullptr;
                        n->kids.push_back(std::move(arg));
                        if (peek().kind == Tok::OP && peek().text == ",") { take(); continue; }
                        break;
                    }
                }
                if (peek().kind != Tok::RPAREN) { err = "expected ')' closing " + name + "()"; return nullptr; }
                take();
                return n;
            }
            auto n = std::make_unique<Node>();
            n->kind = Node::REF;
            n->text = name;
            return n;
        }
        if (t.kind == Tok::LPAREN) {
            take();
            NodePtr inner = parseExpr(1);
            if (!inner) return nullptr;
            if (peek().kind != Tok::RPAREN) { err = "expected ')'"; return nullptr; }
            take();
            return inner;
        }
        err = t.kind == Tok::END ? "unexpected end of expression"
                                 : "unexpected '" + t.text + "'";
        return nullptr;
    }
};

bool isNumeric(const ScriptValue& v) {
    return v.type == ScriptValue::INT || v.type == ScriptValue::FLOAT || v.type == ScriptValue::BOOL;
}

int cmp(const ScriptValue& a, const ScriptValue& b) {
    if (isNumeric(a) && isNumeric(b)) {
        const double x = a.asFloat(), y = b.asFloat();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    const std::string x = a.asString(), y = b.asString();
    return x < y ? -1 : (x > y ? 1 : 0);
}

}  // namespace

NodePtr parse(const std::string& src, std::string& err) {
    Parser p;
    if (!lex(src, p.toks, err)) return nullptr;
    NodePtr n = p.parseExpr(1);
    if (!n) { err = p.err.empty() ? "could not parse expression" : p.err; return nullptr; }
    if (p.peek().kind != Tok::END) {
        err = "trailing '" + p.peek().text + "'";
        return nullptr;
    }
    return n;
}

ScriptValue eval(const Node& n, const Resolver& resolve, std::string& err) {
    switch (n.kind) {
        case Node::LITERAL: return n.literal;
        case Node::REF: {
            ScriptValue v = resolve(n.text);
            if (v.type == ScriptValue::NONE) err = "unknown reference '" + n.text + "'";
            return v;
        }
        case Node::UNARY: {
            ScriptValue a = eval(*n.kids[0], resolve, err);
            if (!err.empty()) return {};
            if (n.text == "not") return ScriptValue::makeBool(!a.asBool());
            if (a.type == ScriptValue::FLOAT) return ScriptValue::makeFloat(-a.asFloat());
            return ScriptValue::makeInt(-a.asInt());
        }
        case Node::CALL: {
            std::vector<ScriptValue> a;
            a.reserve(n.kids.size());
            for (const auto& k : n.kids) {
                a.push_back(eval(*k, resolve, err));
                if (!err.empty()) return {};
            }
            const std::string& f = n.text;
            auto arity = [&](size_t want) {
                if (a.size() != want) {
                    err = f + "() takes " + std::to_string(want) + " argument" +
                          (want == 1 ? "" : "s") + ", got " + std::to_string(a.size());
                    return false;
                }
                return true;
            };
            const bool anyFloat = [&] {
                for (const auto& v : a) if (v.type == ScriptValue::FLOAT) return true;
                return false;
            }();
            if (f == "min" || f == "max") {
                if (a.empty()) { err = f + "() needs at least one argument"; return {}; }
                ScriptValue best = a[0];
                for (const auto& v : a)
                    if ((f == "min") ? (v.asFloat() < best.asFloat()) : (v.asFloat() > best.asFloat())) best = v;
                return best;
            }
            if (f == "abs") {
                if (!arity(1)) return {};
                return anyFloat ? ScriptValue::makeFloat(std::fabs(a[0].asFloat()))
                                : ScriptValue::makeInt(a[0].asInt() < 0 ? -a[0].asInt() : a[0].asInt());
            }
            if (f == "round") { if (!arity(1)) return {}; return ScriptValue::makeInt((long long)std::llround(a[0].asFloat())); }
            if (f == "floor") { if (!arity(1)) return {}; return ScriptValue::makeInt((long long)std::floor(a[0].asFloat())); }
            if (f == "ceil")  { if (!arity(1)) return {}; return ScriptValue::makeInt((long long)std::ceil(a[0].asFloat())); }
            if (f == "clamp") {
                if (!arity(3)) return {};
                const double v = a[0].asFloat(), lo = a[1].asFloat(), hi = a[2].asFloat();
                const double r = v < lo ? lo : (v > hi ? hi : v);
                return anyFloat ? ScriptValue::makeFloat(r) : ScriptValue::makeInt((long long)r);
            }
            if (f == "len") {
                if (!arity(1)) return {};
                return ScriptValue::makeInt((long long)a[0].asString().size());
            }
            err = "unknown function '" + f + "()'";
            return {};
        }
        case Node::BINARY: {
            const std::string& op = n.text;
            // Short-circuit, so `country.XXX.exists and country.XXX.treasury > 0`
            // does not evaluate the right side when the left is false.
            if (op == "and" || op == "&&") {
                ScriptValue a = eval(*n.kids[0], resolve, err);
                if (!err.empty()) return {};
                if (!a.asBool()) return ScriptValue::makeBool(false);
                ScriptValue b = eval(*n.kids[1], resolve, err);
                if (!err.empty()) return {};
                return ScriptValue::makeBool(b.asBool());
            }
            if (op == "or" || op == "||") {
                ScriptValue a = eval(*n.kids[0], resolve, err);
                if (!err.empty()) return {};
                if (a.asBool()) return ScriptValue::makeBool(true);
                ScriptValue b = eval(*n.kids[1], resolve, err);
                if (!err.empty()) return {};
                return ScriptValue::makeBool(b.asBool());
            }

            ScriptValue a = eval(*n.kids[0], resolve, err);
            if (!err.empty()) return {};
            ScriptValue b = eval(*n.kids[1], resolve, err);
            if (!err.empty()) return {};

            if (op == "==") return ScriptValue::makeBool(cmp(a, b) == 0);
            if (op == "!=") return ScriptValue::makeBool(cmp(a, b) != 0);
            if (op == "<")  return ScriptValue::makeBool(cmp(a, b) < 0);
            if (op == "<=") return ScriptValue::makeBool(cmp(a, b) <= 0);
            if (op == ">")  return ScriptValue::makeBool(cmp(a, b) > 0);
            if (op == ">=") return ScriptValue::makeBool(cmp(a, b) >= 0);

            // `+` joins strings when either side is one, so a name can be built
            // without a separate concat command.
            if (op == "+" && (a.type == ScriptValue::STRING || b.type == ScriptValue::STRING))
                return ScriptValue::makeStr(a.asString() + b.asString());

            const bool flt = a.type == ScriptValue::FLOAT || b.type == ScriptValue::FLOAT;
            if (op == "%") {
                if (b.asInt() == 0) { err = "modulo by zero"; return {}; }
                return ScriptValue::makeInt(a.asInt() % b.asInt());
            }
            if (op == "/") {
                // Always a float: integer division silently turning 3/2 into 1
                // is the kind of thing a map-maker finds out about much later.
                if (b.asFloat() == 0.0) { err = "divide by zero"; return {}; }
                return ScriptValue::makeFloat(a.asFloat() / b.asFloat());
            }
            if (op == "+") return flt ? ScriptValue::makeFloat(a.asFloat() + b.asFloat())
                                      : ScriptValue::makeInt(a.asInt() + b.asInt());
            if (op == "-") return flt ? ScriptValue::makeFloat(a.asFloat() - b.asFloat())
                                      : ScriptValue::makeInt(a.asInt() - b.asInt());
            if (op == "*") return flt ? ScriptValue::makeFloat(a.asFloat() * b.asFloat())
                                      : ScriptValue::makeInt(a.asInt() * b.asInt());
            err = "unknown operator '" + op + "'";
            return {};
        }
    }
    return {};
}

ScriptValue evaluate(const std::string& src, const Resolver& resolve, bool& ok, std::string& err) {
    err.clear();
    NodePtr n = parse(src, err);
    if (!n) { ok = false; return {}; }
    ScriptValue v = eval(*n, resolve, err);
    ok = err.empty();
    return v;
}

std::string unparse(const Node& n) {
    switch (n.kind) {
        case Node::LITERAL:
            if (n.literal.type == ScriptValue::STRING) return "\"" + n.literal.strVal + "\"";
            return n.literal.asString();
        case Node::REF: return n.text;
        case Node::CALL: {
            std::string out = n.text + "(";
            for (size_t i = 0; i < n.kids.size(); ++i) {
                if (i) out += ", ";
                out += unparse(*n.kids[i]);
            }
            return out + ")";
        }
        case Node::UNARY: {
            const std::string inner = unparse(*n.kids[0]);
            const bool wrap = n.kids[0]->kind == Node::BINARY;
            if (n.text == "not") return wrap ? "not (" + inner + ")" : "not " + inner;
            return wrap ? "-(" + inner + ")" : "-" + inner;
        }
        case Node::BINARY: {
            const int prec = binPrec(n.text);
            auto side = [&](const Node& k, bool right) {
                std::string s = unparse(k);
                // Parenthesise only where dropping them would reassociate: a
                // tighter child never needs them, and neither does a left child
                // of equal precedence.
                if (k.kind == Node::BINARY) {
                    const int kp = binPrec(k.text);
                    if (kp < prec || (kp == prec && right)) return "(" + s + ")";
                }
                return s;
            };
            return side(*n.kids[0], false) + " " + n.text + " " + side(*n.kids[1], true);
        }
    }
    return "";
}

}  // namespace odscript
