#pragma once

// Mail: letters between countries, delivered when the turn resolves.
//
// WHY IT IS NOT A CHAT BOX
//
// A chat box is instant, and instant messaging in a turn-based game quietly
// becomes the real interface -- the map stops being where diplomacy happens and
// the window becomes it. Mail travels at the speed of the world instead: you
// write this turn, it arrives next turn, and until the turn resolves the letter
// is still on your desk. That gives the one property a chat box cannot have --
// YOU CAN CHANGE YOUR MIND -- and it is why a message may be edited or torn up
// right up to the moment the turn is processed, and never afterwards.
//
// It is also what makes an AI correspondent fair. A bot that answers instantly
// would out-talk any human; a bot that answers next turn is on the same clock
// as everybody else.
//
// WHAT IS IN HERE AND WHAT IS NOT
//
// This header is the model and the rules, and nothing else: no raylib, no
// networking, no Game. That is deliberate -- these rules decide who may write
// to whom, which is exactly the kind of thing that must be testable without
// standing up a world. See tests/mail_rules_test.cpp.

#include <cstdint>
#include <string>
#include <vector>

namespace mail {

/// Where a letter is in its life. The order matters: it only ever moves down.
enum class Status : uint8_t {
    Pending = 0,   ///< written, still on the desk, still yours to change
    Delivered,     ///< the turn resolved and it left; now it is history
    Blocked,       ///< refused before sending, and the writer was told why
};

/// Who wrote it, as the reader is told.
enum class Author : uint8_t {
    Human = 0,     ///< a player, in single player or multiplayer
    Bot,           ///< a language model. ALWAYS shown as such -- see botTag()
};

/**
 * One letter.
 *
 * `body` is what was typed. It is not trusted: it may be edited until the turn
 * resolves, it may come off a network, and in multiplayer it may come from a
 * stranger. Everything that renders it treats it as text, never as markup.
 */
struct Message {
    int         id = 0;
    int         fromCountry = 0;
    /// The recipient of an ordinary letter. 0 when this went to a group.
    int         toCountry = 0;
    /// The room it was written in, or 0. Never both this and toCountry.
    int         groupId = 0;
    std::string body;
    int         writtenTurn = 0;
    /// The turn it lands. Always writtenTurn + 1: a letter takes a turn.
    int         deliverTurn = 0;
    Status      status = Status::Pending;
    Author      author = Author::Human;
    /// Who to name in multiplayer. Empty in single player, where the country is
    /// the whole identity.
    std::string authorName;

    bool editable() const { return status == Status::Pending; }
};

/**
 * The label a bot's letter carries, in every language, always.
 *
 * NOT OPTIONAL AND NOT STYLING. The bots in this game are allowed to lie --
 * that is the point of them, a diplomat who cannot lie is not a diplomat -- and
 * a player who cannot tell a machine from a person cannot judge what they are
 * being told. The lying is a game mechanic; concealing the speaker would be a
 * deception about the game itself. Any code path that renders a message must
 * render this with it.
 */
const char* botTag();

/// What the host allows. Set by whoever runs the game; see Config/lobby.
enum class Policy : uint8_t {
    Nobody = 0,    ///< mail is off entirely
    PlayersOnly,   ///< humans may write to humans; bots are silent
    BotsOnly,      ///< only the machine correspondents; players cannot write
    Everyone,      ///< both
};

/// What one player allows to reach THEM, whatever the host allows generally.
enum class Lock : uint8_t {
    Open = 0,      ///< anyone the host permits
    BotsOnly,      ///< machines may write; people may not
    Nobody,        ///< nothing arrives
};

/// The state a rule needs to answer "may this be sent?".
struct Rules {
    Policy policy = Policy::Nobody;
    /// Whether a language-model module is loaded and answering. BotsOnly and
    /// Everyone are meaningless without it.
    bool   llmAvailable = false;
    /// Whether this is a networked game. PlayersOnly is meaningless without it.
    bool   multiplayer = false;
};

/**
 * One member of a room, as the rule needs to see them.
 *
 * A ROOM IS NOT A RECIPIENT, and treating it as one was a real bug: the send
 * path described a group as "a recipient who is not a bot", and writing to a
 * non-bot needs multiplayer, so every group in every single-player game was
 * refused with "Mail is switched off on this server" -- in rooms whose members
 * were all bots.
 */
struct RoomMember {
    bool isBot = false;
    Lock lock = Lock::Open;
};

/// Why a letter may not be sent. Empty string means it may.
enum class Refusal : uint8_t {
    None = 0,
    MailOff,          ///< the host turned it off
    NotToPeople,      ///< host allows bots only
    NotToMachines,    ///< host allows people only
    RecipientClosed,  ///< that country is not accepting mail
    Empty,            ///< nothing was written
    TooLong,
    Blacklisted,      ///< the host forbids a word in it
};

/// A sentence for the writer. Translated at the call site.
const char* refusalText(Refusal r);

/** The longest a letter may be. Generous: this is correspondence, not chat. */
constexpr size_t kMaxBody = 2000;

/**
 * Whether the Mail button exists at all.
 *
 * The request was that mail appears "only on servers or when talking to an LLM
 * driven country is available". A Mail button in a single-player game with no
 * language model would open onto a list of countries that can never answer,
 * which is worse than no button.
 */
bool available(const Rules& rules);

/**
 * May `from` write to `to` right now?
 *
 * `toIsBot` says whether the recipient is played by a language model rather
 * than by a person. `toLock` is that recipient's own setting, which overrides
 * anything the host permits -- a host can narrow who may speak, never widen it
 * past what the recipient will accept.
 */
Refusal mayWrite(const Rules& rules, bool toIsBot, Lock toLock);

/**
 * May a letter go into a room with these members?
 *
 * The author's own door, then whether ANY member could receive it. Delivery
 * checks each member again one at a time -- that is where a shut door is
 * honoured -- so this is not about who gets it, only about whether sending is
 * pointless or forbidden outright. A room where nobody can hear you is refused
 * with the reason the first member gave, rather than silently going nowhere.
 */
Refusal mayWriteToRoom(const Rules& rules, const std::vector<RoomMember>& members);

/// The half of check() that judges the LETTER: empty, too long, forbidden word.
Refusal checkBody(const std::string& body, const std::vector<std::string>& blacklist);

/**
 * Check a letter's text against the host's forbidden words.
 *
 * Substring, case-insensitive, and deliberately crude. This is a host's blunt
 * instrument for the handful of words they never want to see on their server;
 * it is not moderation and must not be sold as it. Anything cleverer -- word
 * boundaries, leetspeak, unicode confusables -- is an arms race that a blocklist
 * loses, and the answer to somebody determined to be vile is the report button
 * and a ban, not a bigger list.
 */
bool blacklisted(const std::string& body, const std::vector<std::string>& words);

/// Full check, in the order a writer should be told about problems.
Refusal check(const std::string& body, const Rules& rules, bool toIsBot, Lock toLock,
              const std::vector<std::string>& blacklist);

/**
 * Everything one country has exchanged with another.
 *
 * Kept per pair rather than as one log, because the whole point is that each
 * correspondence is separate: what France writes to Britain is not what France
 * writes to Russia, and a language model answering as Britain must not see the
 * Russian thread. See the context isolation in the LLM module.
 */
/**
 * A conversation with more than two countries in it.
 *
 * WHO MAY REMOVE WHOM. The country that made the room may remove anybody from
 * it; nobody else may remove anybody. That is the whole of the authority, and
 * it is deliberately small -- a room where any member can eject any other is a
 * room that ends in an ejection war, and one where nobody can is a room that
 * cannot be rescued from whoever wandered in.
 *
 * ANYBODY MAY LEAVE, including the owner. An owner who leaves does not take
 * the room with them: the letters already in it belong to everybody who
 * received them, and deleting a conversation out from under four other players
 * because the fifth got bored is not a power worth having. The room simply has
 * no owner afterwards, and nobody can be removed from it again.
 *
 * WHAT REMOVAL IS NOT. It does not erase what somebody already read. A letter
 * delivered is a letter delivered; removal stops the NEXT one. Anything else
 * would mean a player could unsay something by ejecting the person they said
 * it to.
 */
struct Group {
    int id = 0;
    std::string name;
    /// The country that created it. 0 once they have left: see above.
    int owner = 0;
    /// Everyone who receives what is written here, including the owner.
    std::vector<int> members;

    bool has(int country) const;
    /// True if `who` is allowed to remove `whom`. Only the owner, never
    /// themselves through this path -- leaving is its own thing.
    bool mayRemove(int who, int whom) const;
};

struct Thread {
    /// The other country, for a two-party correspondence. 0 for a group.
    int otherCountry = 0;
    /// The room this thread belongs to, or 0 for an ordinary letter. A thread
    /// has one or the other and never both.
    int groupId = 0;
    std::vector<Message> messages;

    /// Letters still on the desk, which are the ones that may be changed.
    size_t pendingCount() const;
    /// What the other side can actually see.
    std::vector<const Message*> delivered() const;
};

/**
 * One country's post: every thread it holds, and the letters waiting to go.
 */
class Box {
public:
    /// Write a letter. Returns its id, or 0 if the rules refused it.
    int write(int fromCountry, int toCountry, const std::string& body, int turn,
              Author author = Author::Human, const std::string& authorName = "");

    /// Write into a room. Same rules; the recipient is everybody in it.
    int writeToGroup(int fromCountry, int groupId, const std::string& body, int turn,
                     Author author = Author::Human, const std::string& authorName = "");

    /// The room thread, made if this box has not seen it before.
    Thread& groupThread(int groupId);
    const Thread* groupThreadIfAny(int groupId) const;

    /// Change a letter that has not gone yet. False if it has.
    bool edit(int id, const std::string& body);
    /// Tear one up. False if it has already gone.
    bool discard(int id);

    /**
     * Send everything pending. Called once, when the turn resolves.
     *
     * Returns how many left. After this they are history: `edit` and `discard`
     * both refuse, which is the property the whole design exists to provide.
     */
    int deliver(int turnNow);

    /**
     * Put a letter somebody else sent into this box, already delivered.
     *
     * Boxes are per country and hold both sides of each correspondence, so a
     * delivered letter exists twice: in the sender's box and in the
     * recipient's. That duplication is the point rather than a cost -- it is
     * what makes "what Britain can see" a fact about Britain's box rather than
     * a filter someone has to remember to apply. A language model answering as
     * Britain is handed Britain's box and cannot reach anything else.
     */
    void receive(const Message& m);

    /**
     * Put a letter back exactly as it was, for loading a save.
     *
     * Distinct from receive() because it keeps the status it is given -- a
     * pending letter must come back pending, still yours to change -- and
     * because it files under the RECIPIENT, this being the sender's own copy.
     * Ids are preserved and the next id is advanced past them.
     */
    void adopt(const Message& m);

    Thread* thread(int otherCountry);
    const Thread* thread(int otherCountry) const;
    /// Threads in most-recent-first order, for a list a person reads.
    std::vector<const Thread*> threads() const;

    const Message* find(int id) const;
    Message* find(int id);

    /// Letters that arrived on this turn, for the "you have mail" notice.
    std::vector<const Message*> arrivedOn(int turn) const;

    void clear();
    bool empty() const { return m_threads.empty(); }

private:
    std::vector<Thread> m_threads;
    int m_nextId = 1;
    Thread& threadFor(int otherCountry);
};

}  // namespace mail
