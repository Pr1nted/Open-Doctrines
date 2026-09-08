#include "Mail.h"

#include <algorithm>
#include <cctype>

namespace mail {

const char* botTag() { return "bot"; }

const char* refusalText(Refusal r) {
    switch (r) {
        case Refusal::MailOff:         return "Mail is switched off on this server.";
        case Refusal::NotToPeople:     return "This server allows letters to advisors only.";
        case Refusal::NotToMachines:   return "This server allows letters between players only.";
        case Refusal::RecipientClosed: return "They are not accepting letters.";
        case Refusal::Empty:           return "There is nothing written.";
        case Refusal::TooLong:         return "That is longer than a letter can be.";
        case Refusal::Blacklisted:     return "That contains a word this server does not allow.";
        default:                       return "";
    }
}

bool available(const Rules& rules) {
    switch (rules.policy) {
        case Policy::Nobody:      return false;
        // A players-only server is pointless without other players, and a
        // bots-only one is pointless without the module that provides them.
        case Policy::PlayersOnly: return rules.multiplayer;
        case Policy::BotsOnly:    return rules.llmAvailable;
        case Policy::Everyone:    return rules.multiplayer || rules.llmAvailable;
    }
    return false;
}

Refusal mayWrite(const Rules& rules, bool toIsBot, Lock toLock) {
    if (rules.policy == Policy::Nobody) return Refusal::MailOff;

    // What the host permits, by who the recipient is.
    if (toIsBot) {
        if (!rules.llmAvailable) return Refusal::MailOff;
        if (rules.policy == Policy::PlayersOnly) return Refusal::NotToMachines;
    } else {
        if (!rules.multiplayer) return Refusal::MailOff;
        if (rules.policy == Policy::BotsOnly) return Refusal::NotToPeople;
    }

    // THE RECIPIENT HAS THE LAST WORD.
    //
    // Checked after the host's policy and never before it: a host can narrow
    // who may speak on their server, but nothing a host permits can force a
    // letter on somebody who has said they do not want them. A player who has
    // shut their door is entitled to have it stay shut.
    switch (toLock) {
        case Lock::Nobody:   return Refusal::RecipientClosed;
        case Lock::BotsOnly: if (!toIsBot) return Refusal::RecipientClosed; break;
        case Lock::Open:     break;
    }
    return Refusal::None;
}

bool blacklisted(const std::string& body, const std::vector<std::string>& words) {
    if (words.empty()) return false;
    std::string lower = body;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    for (const std::string& w : words) {
        if (w.empty()) continue;
        std::string needle = w;
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

Refusal check(const std::string& body, const Rules& rules, bool toIsBot, Lock toLock,
              const std::vector<std::string>& blacklist) {
    // Who first, then what. Being told "that word is not allowed" about a
    // letter that was never going to be delivered anyway is a worse answer.
    const Refusal who = mayWrite(rules, toIsBot, toLock);
    if (who != Refusal::None) return who;

    // Trim before judging emptiness: a letter of spaces is an empty letter.
    const size_t first = body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return Refusal::Empty;
    if (body.size() > kMaxBody) return Refusal::TooLong;
    if (blacklisted(body, blacklist)) return Refusal::Blacklisted;
    return Refusal::None;
}

// ─────────────────────────────────────────────────────────────── Thread ────

size_t Thread::pendingCount() const {
    size_t n = 0;
    for (const Message& m : messages) if (m.status == Status::Pending) ++n;
    return n;
}

std::vector<const Message*> Thread::delivered() const {
    std::vector<const Message*> out;
    for (const Message& m : messages)
        if (m.status == Status::Delivered) out.push_back(&m);
    return out;
}

// ────────────────────────────────────────────────────────────────── Box ────

Thread& Box::threadFor(int otherCountry) {
    for (Thread& t : m_threads) if (t.otherCountry == otherCountry) return t;
    m_threads.push_back(Thread{otherCountry, {}});
    return m_threads.back();
}

Thread* Box::thread(int otherCountry) {
    for (Thread& t : m_threads) if (t.otherCountry == otherCountry) return &t;
    return nullptr;
}

const Thread* Box::thread(int otherCountry) const {
    for (const Thread& t : m_threads) if (t.otherCountry == otherCountry) return &t;
    return nullptr;
}

int Box::write(int fromCountry, int toCountry, const std::string& body, int turn,
               Author author, const std::string& authorName) {
    if (fromCountry == toCountry) return 0;   // nobody writes to themselves
    Message m;
    m.id = m_nextId++;
    m.fromCountry = fromCountry;
    m.toCountry = toCountry;
    m.body = body;
    m.writtenTurn = turn;
    // A letter takes a turn. This is the whole mechanic in one line: it is why
    // it can still be edited, and why a bot cannot out-talk a person.
    m.deliverTurn = turn + 1;
    m.status = Status::Pending;
    m.author = author;
    m.authorName = authorName;
    threadFor(toCountry).messages.push_back(m);
    return m.id;
}

Message* Box::find(int id) {
    for (Thread& t : m_threads)
        for (Message& m : t.messages)
            if (m.id == id) return &m;
    return nullptr;
}

const Message* Box::find(int id) const {
    for (const Thread& t : m_threads)
        for (const Message& m : t.messages)
            if (m.id == id) return &m;
    return nullptr;
}

bool Box::edit(int id, const std::string& body) {
    Message* m = find(id);
    if (!m || !m->editable()) return false;
    m->body = body;
    return true;
}

bool Box::discard(int id) {
    for (Thread& t : m_threads) {
        for (size_t i = 0; i < t.messages.size(); ++i) {
            if (t.messages[i].id != id) continue;
            if (!t.messages[i].editable()) return false;   // already gone
            t.messages.erase(t.messages.begin() + (long)i);
            return true;
        }
    }
    return false;
}

void Box::adopt(const Message& m) {
    threadFor(m.toCountry).messages.push_back(m);
    if (m.id >= m_nextId) m_nextId = m.id + 1;
}

void Box::receive(const Message& m) {
    // Filed under whoever wrote it, which from this box's side is the other
    // party. Ids are the sender's, so a message keeps one identity across both
    // boxes -- that is what lets a report name a letter unambiguously later.
    Message copy = m;
    copy.status = Status::Delivered;
    threadFor(m.fromCountry).messages.push_back(copy);
}

int Box::deliver(int turnNow) {
    int sent = 0;
    for (Thread& t : m_threads) {
        for (Message& m : t.messages) {
            if (m.status != Status::Pending) continue;
            m.status = Status::Delivered;
            // Stamped with the turn it ACTUALLY landed on, not the turn it was
            // promised for. A game loaded from an old save, or a turn that was
            // skipped, would otherwise leave letters claiming a delivery date
            // that never happened.
            m.deliverTurn = turnNow;
            ++sent;
        }
    }
    return sent;
}

std::vector<const Thread*> Box::threads() const {
    std::vector<const Thread*> out;
    out.reserve(m_threads.size());
    for (const Thread& t : m_threads) out.push_back(&t);
    // Most recently active first: a correspondence list is read from the top.
    std::stable_sort(out.begin(), out.end(), [](const Thread* a, const Thread* b) {
        const int at = a->messages.empty() ? -1 : a->messages.back().writtenTurn;
        const int bt = b->messages.empty() ? -1 : b->messages.back().writtenTurn;
        return at > bt;
    });
    return out;
}

std::vector<const Message*> Box::arrivedOn(int turn) const {
    std::vector<const Message*> out;
    for (const Thread& t : m_threads)
        for (const Message& m : t.messages)
            if (m.status == Status::Delivered && m.deliverTurn == turn) out.push_back(&m);
    return out;
}

void Box::clear() {
    m_threads.clear();
    m_nextId = 1;
}


// ─────────────────────────────────────────────────────────── group rooms ────

bool Group::has(int country) const {
    for (int m : members) if (m == country) return true;
    return false;
}

bool Group::mayRemove(int who, int whom) const {
    // Nobody may remove anybody once the owner has gone: see the note on
    // Group. An unowned room is not a room anybody has authority over.
    if (owner == 0) return false;
    if (who != owner) return false;
    // Removing yourself is LEAVING, and goes through the other path. Allowing
    // it here would let an owner "remove" themselves and leave the room with an
    // owner id pointing at somebody who is not in it.
    if (whom == who) return false;
    return has(whom);
}

Thread& Box::groupThread(int groupId) {
    for (Thread& t : m_threads)
        if (t.groupId == groupId) return t;
    m_threads.push_back(Thread{});
    m_threads.back().groupId = groupId;
    return m_threads.back();
}

const Thread* Box::groupThreadIfAny(int groupId) const {
    for (const Thread& t : m_threads)
        if (t.groupId == groupId) return &t;
    return nullptr;
}

int Box::writeToGroup(int fromCountry, int groupId, const std::string& body, int turn,
                      Author author, const std::string& authorName) {
    if (groupId <= 0) return 0;
    std::string trimmed = body;
    if (trimmed.size() > kMaxBody) trimmed.resize(kMaxBody);
    if (trimmed.empty()) return 0;

    Message m;
    m.id = m_nextId++;
    m.fromCountry = fromCountry;
    m.toCountry = 0;             // a room has no single recipient
    m.groupId = groupId;
    m.body = trimmed;
    m.writtenTurn = turn;
    m.status = Status::Pending;
    m.author = author;
    m.authorName = authorName;
    groupThread(groupId).messages.push_back(m);
    return m.id;
}

}  // namespace mail
