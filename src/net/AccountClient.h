#pragma once

// The account service, as the game sees it.
//
// Everything here is non-blocking. A call starts a job on a background thread
// and returns immediately; the game calls update() once a frame and reads the
// status. Nothing in this file touches raylib, so it stays testable and the
// menu keeps sole ownership of drawing -- the same split the mod host uses.
//
// WHAT IS STORED ON DISK, AND WHAT THAT MEANS
//
// One file, <dataDir>/account.json, holding the session token and the issuer
// it came from. It is written 0600 so other users on the machine cannot read
// it, which is the meaningful boundary on a single-user desktop.
//
// It is NOT in the OS keychain, and that is worth being straight about: anyone
// who can run code as this user can read it. The exposure is bounded by what
// the token can do -- change the nickname, link or unlink a provider, delete
// the account. It cannot join a game as you, because joining needs a ticket
// minted per session, and it cannot be replayed at a game server, because the
// audience is wrong. Keychain, Credential Manager and libsecret integration is
// the right eventual answer; a file with the right mode is an honest interim
// one.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** A provider the player can sign in with. */
enum class AuthProvider { Google, Discord, GitHub };

const char* authProviderId(AuthProvider p);     // "google"
const char* authProviderLabel(AuthProvider p);  // "Google"

struct AccountInfo {
    std::string id;
    std::string nickname;
    std::vector<std::string> badges;
    std::vector<std::string> linked;   // provider ids
    long long   created = 0;

    /** Set while a ban is in force. `until` 0 means permanent. */
    bool        banned = false;
    std::string banReason;
    long long   banUntil = 0;          // unix seconds

    bool valid() const { return !id.empty(); }
    bool hasBadge(const char* name) const;

    /** "3 days", "in 4 hours", or "permanent". Empty when not banned. */
    std::string banRemaining(long long nowUnix) const;
};

class AccountClient {
public:
    static AccountClient& get();

    // `issuer` is the deployed Worker URL. `tokenPath` is where the session
    // token is kept. Safe to call more than once; the second call is ignored.
    void init(const std::string& issuer, const std::string& tokenPath);

    /** Whether an issuer was configured at all. */
    bool configured() const;

    /**
     * Which providers this deployment can actually use, learned from the
     * service itself.
     *
     * Offering a provider whose credentials the operator never set would send
     * the player to a consent screen that refuses them, two steps from the
     * cause. Empty means either "not asked yet" or "the service offers none";
     * serviceReachable() tells the two apart.
     */
    std::vector<AuthProvider> providers() const;

    /**
     * True when this provider can only be ADDED to an existing account, never
     * used to create one. The service says so because such a provider cannot be
     * age-gated; the screen labels it so nobody clicks it while signed out and
     * gets refused after a trip through a consent screen.
     */
    bool isLinkOnly(AuthProvider p) const;

    bool serviceReachable() const;

    /** Asks the service what it offers. Cheap, and does not need a token. */
    bool probeService();

    /** Where the privacy policy lives, for the client to open in a browser. */
    std::string privacyUrl() const;
    /** Where the terms of use live. Same service, same repository file. */
    std::string termsUrl() const;

    /** The account service this is signed in to. */
    std::string issuer() const;

    /**
     * The session token, for the one job that legitimately needs it: minting a
     * join ticket.
     *
     * NARROW ON PURPOSE. This token is the account credential -- it can change
     * the nickname, unlink a provider, delete the account. It must never reach
     * a game server, and does not: NetSession and NetHost use it to call the
     * account service and send only the resulting ticket over the wire. See
     * net/src/auth/token.ts for why the two are different things.
     *
     * Empty unless signed in. Do not store what this returns.
     */
    std::string sessionToken() const;

    enum class Status {
        SignedOut = 0,
        Working,          // a request is in flight
        WaitingForBrowser,// device flow: the player is at the consent screen
        NeedsNickname,    // signed in, but the account does not exist yet
        SignedIn,
        DeleteConfirm,    // delete was asked for; awaiting a second confirmation
    };

    Status      status() const;
    AccountInfo account() const;

    /** Last thing worth telling the player. Empty when there is nothing. */
    std::string message() const;
    bool        messageIsError() const;
    void        clearMessage();

    /** During WaitingForBrowser: the URL the player must visit. */
    std::string verifyUrl() const;

    /** What the deletion step-one reply said would happen. */
    std::vector<std::string> deletionSummary() const;

    // ---- actions. Each returns false if one is already running. ------------

    /**
     * One call at screen-open: ask the service what it offers, and restore a
     * stored session if there is one.
     *
     * Both in a single background job. Issuing them as two calls would make the
     * second block the render thread waiting on the first.
     */
    bool bootstrap();

    bool beginSignIn(AuthProvider provider);

    /**
     * Add another way of signing in to the account already signed in here.
     *
     * Same browser round trip as signing in, but the result attaches the
     * provider to this account instead of making a new one. This is the ONLY
     * way a link-only provider (see isLinkOnly) ever becomes usable: link it
     * once from here, and afterwards it signs you in like any other.
     */
    bool beginLink(AuthProvider provider);

    /** Remove a sign-in method. Refused server-side if it is the last one. */
    bool unlink(AuthProvider provider);

    void cancelSignIn();

    /** First sign-in only: claims a nickname and creates the account. */
    bool createAccount(const std::string& nickname);

    bool changeNickname(const std::string& nickname);

    /** Two steps on purpose. beginDelete() explains, confirmDelete() does it. */
    bool beginDelete();
    bool confirmDelete();
    void cancelDelete();

    /** Forgets the local token. Does not touch the account. */
    void signOut();

    /**
     * Local nickname check, so the player is told why a name is refused as
     * they type instead of after a round trip. Deliberately a SUBSET of what
     * the server enforces: the blocklist lives server-side and is not shipped
     * to clients, so a name that passes here can still be refused.
     */
    static bool nicknameLooksValid(const std::string& nickname, std::string& why);

    /** Drains finished jobs. Call once a frame from the game thread. */
    void update();

    /** Blocks until any in-flight job finishes. Called at shutdown. */
    void shutdown();

private:
    AccountClient();
    ~AccountClient();
    AccountClient(const AccountClient&) = delete;
    AccountClient& operator=(const AccountClient&) = delete;

    bool beginFlow(AuthProvider provider, bool link);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
