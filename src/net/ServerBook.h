#pragma once

// The places you play, kept the way saved worlds are kept.
//
// A server is browsed like a world: a named entry in a list you add to, rename
// and remove. What is stored is only the DIRECTION -- which account service, and
// the current invite code. Nothing about who you are.
//
// WHAT IS DELIBERATELY NOT IN HERE
//
// No token, no ticket, no psid, no account id. Those live in account.json and
// nowhere else, and the split is the point: this file can be shared, synced,
// copied between machines or committed by accident without handing anyone your
// session. The two halves come together only at the moment of joining, in
// memory, when a ticket is minted for one session and spent immediately.
//
// If you ever find yourself wanting to cache "the ticket for this server" here,
// that is the design saying no.
//
// WHY AN INVITE CODE IS NOT A SERVER
//
// A code names one SESSION and expires with it. What persists is the account
// service the server lives on plus a name you gave it, so an entry survives the
// host restarting and you paste a fresh code into the entry you already have
// rather than making a new one every evening.

#include <cstdint>
#include <string>
#include <vector>

struct ServerEntry {
    /** What you called it. The only field that is purely yours. */
    std::string name;

    /** The account service this server authenticates against. */
    std::string issuer;

    /** Current invite code. Expires with the session; may be empty. */
    std::string code;

    /** Unix seconds, 0 if never. Used only for ordering the list. */
    long long lastJoined = 0;

    /** Set once seen, so the list can show it before you connect. */
    std::string lastHostName;

    /**
     * Where this server was last reached: a tunnel hostname, an address and
     * port, or empty for a game reached through the relay.
     *
     * THIS IS WHAT MAKES REJOINING WORK. An entry used to carry only the code,
     * so pressing a saved server cleared the address field -- and an empty
     * address means the relay. A host reached over a tunnel has never been to
     * the relay, so every rejoin after a dropped connection went to a lobby
     * the host was not in, was told "the host left", and then got 404s from
     * the account service because that answer deletes the session. One player
     * losing their connection for a moment ended the game for everybody.
     *
     * Kept per entry rather than per code because the code changes every
     * session and the address usually does not.
     */
    std::string address;

    bool valid() const { return !name.empty() && !issuer.empty(); }
};

class ServerBook {
public:
    /** `path` is the JSON file; missing is not an error, it means empty. */
    void load(const std::string& path);
    bool save() const;

    const std::vector<ServerEntry>& entries() const { return m_entries; }

    /**
     * Add, or update the entry that already names this issuer+name pair.
     *
     * Matching on the pair rather than the code is what makes an entry survive
     * a new session: pasting tonight's code into "Friday game" updates it
     * instead of piling up a fresh row every week.
     */
    void addOrUpdate(const ServerEntry& entry);

    bool remove(size_t index);
    bool rename(size_t index, const std::string& name);
    bool setCode(size_t index, const std::string& code);
    /** Remember where this server was reached, for the next join. */
    bool setAddress(size_t index, const std::string& address);
    void markJoined(size_t index, const std::string& hostName, long long nowUnix);

    /** Most recently joined first, then never-joined, then by name. */
    void sort();

    /**
     * Pull an issuer and code out of a pasted invite.
     *
     * Accepts a bare code ("ABCD-EFGH"), or a full URL from which both are
     * taken. Returns false rather than guessing: an address this is wrong
     * about is a connection somewhere unintended.
     */
    static bool parseInvite(const std::string& text, const std::string& defaultIssuer,
                            std::string& issuerOut, std::string& codeOut);

    /** Shape check for a code. Deliberately strict; codes are ours. */
    static bool validCode(const std::string& code);

private:
    std::vector<ServerEntry> m_entries;
    std::string m_path;
};
