#include "ChatVote.h"

#include <algorithm>
#include <cctype>

namespace chatvote {

std::string tokenOf(const std::string& message) {
    size_t a = message.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = message.find_last_not_of(" \t\r\n");
    std::string t = message.substr(a, b - a + 1);

    // "!war" and "war" are the same vote. The bang is what people type because
    // every other bot on the platform uses one, and refusing it would lose
    // votes for a reason no viewer could see.
    if (!t.empty() && (t[0] == '!' || t[0] == '#')) t.erase(0, 1);
    if (t.empty()) return {};

    // ONE WORD, or it is conversation. A message with a space in it is somebody
    // talking about the vote rather than casting one, and counting it would
    // make the tally disagree with what chat can see itself saying.
    for (unsigned char c : t)
        if (std::isspace(c)) return {};
    // And short: a token is "1" or "war", never a sentence with no spaces.
    if (t.size() > 16) return {};

    for (auto& c : t) c = (char)std::tolower((unsigned char)c);
    return t;
}

void Poll::open(std::vector<Option> options, double now, double seconds) {
    m_options = std::move(options);
    m_byViewer.clear();
    // No options is a closed poll rather than an empty one that can be voted
    // in: there is nothing to pick.
    m_closesAt = m_options.empty() ? 0.0 : now + seconds;
}

bool Poll::open_at(double now) const {
    return m_closesAt > 0.0 && now < m_closesAt;
}

double Poll::secondsLeft(double now) const {
    if (m_closesAt <= 0.0) return 0.0;
    const double left = m_closesAt - now;
    return left > 0.0 ? left : 0.0;
}

bool Poll::cast(const std::string& who, const std::string& message, double now) {
    if (!open_at(now) || who.empty()) return false;
    const std::string token = tokenOf(message);
    if (token.empty()) return false;

    for (size_t i = 0; i < m_options.size(); ++i) {
        if (m_options[i].key != token) continue;
        // ── ONE VOTE EACH, AND CHANGING IT IS ALLOWED ──
        //
        // The map is keyed by viewer, so a second vote REPLACES the first
        // rather than adding to it. Somebody who types "war" fifty times has
        // one vote, and somebody who changes their mind gets to.
        auto at = m_byViewer.find(who);
        if (at != m_byViewer.end() && at->second == i) return false;   // no change
        m_byViewer[who] = i;
        return true;
    }
    return false;   // not an option: chat is talking, not voting
}

std::vector<Tally> Poll::standings() const {
    std::vector<Tally> out;
    out.reserve(m_options.size());
    for (const Option& o : m_options) out.push_back(Tally{o.key, o.label, o.id, 0});
    for (const auto& [who, index] : m_byViewer) {
        (void)who;
        if (index < out.size()) ++out[index].votes;
    }
    // Most votes first. std::stable_sort so a tie keeps the order the options
    // were given in -- which is what makes a draw resolve the same way twice.
    std::stable_sort(out.begin(), out.end(),
                     [](const Tally& a, const Tally& b) { return a.votes > b.votes; });
    return out;
}

Tally Poll::winner() const {
    const std::vector<Tally> s = standings();
    // Nobody voted: no winner. Acting on an empty poll because the clock ran
    // out is how a stream ends up at war by accident.
    if (s.empty() || s.front().votes == 0) return Tally{};
    return s.front();
}

}  // namespace chatvote
