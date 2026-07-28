#pragma once

// Where long-form turn data lives, and what everyone must be told about it.
//
// THE ASYMMETRY THAT MAKES THIS WORK
//
//   Server -> everyone: the turn bundle (.odsv delta). PUBLIC and IMMUTABLE.
//                       It is the state of a game, not a secret, and making it
//                       readable is what lets spectators follow a tournament
//                       without joining it. Making it un-editable is what stops
//                       anyone rewriting history.
//
//   Player -> server:   orders. ENCRYPTED and AUTHENTICATED. Nobody, including
//                       other players, may read or forge them.
//
// Confidentiality never comes from the store. Any of these backends hands out
// a URL that is the only thing protecting a blob -- and on jsonblob, anyone
// holding that URL can OVERWRITE it. So orders are sealed with a key the host
// publishes in the lobby over the authenticated WebSocket, and a blob that
// fails its MAC is treated as NO SUBMISSION, a case the turn logic already has
// to handle. Tampering can destroy a turn; it can never forge one.

#include <cstdint>
#include <string>
#include <vector>

enum class TurnStoreKind : uint8_t {
    /**
     * The session's own Durable Object.
     *
     * The default, and the reason is worth stating: it needs NOTHING enabled
     * that a working account does not already have. R2 was the obvious choice
     * on quota -- 1M writes a month against KV's 1,000 a day -- but it has to
     * be switched on in the dashboard, and Cloudflare wants a card on file
     * before it will do that, free tier or not. A turn store that costs
     * somebody a billing relationship is not free in the way that matters.
     *
     * Durable Objects are already in use for the relay, include 5 GB of SQLite
     * storage on the free plan, and free-plan accounts are not billed for it.
     * A tournament's turn deltas are KB-scale, so 5 GB is not a constraint
     * anyone will meet.
     */
    DurableObject = 0,
    /** jsonblob.com. Anonymous, no API key, and a blob can be revised. */
    JsonBlob = 1,
    /** Cloudflare R2. Needs enabling, which needs a card on file. */
    R2 = 3,
    /** No infrastructure: the game shows text to paste, and a box to paste back. */
    Manual = 2,
};

const char* turnStoreName(TurnStoreKind k);

/**
 * What the HOST must read before choosing a store, and what PLAYERS must be
 * shown in the lobby before they commit an evening to it.
 *
 * Both come from here rather than being written twice. A warning shown to a
 * host that differs from the one shown to players is how somebody ends up
 * agreeing to something nobody described to them.
 */
struct TurnStoreWarning {
    /** True when turn data leaves machines the host controls. */
    bool thirdParty = false;
    /** True when the published turn bundles are readable by anyone with a URL. */
    bool publiclyReadable = false;
    /** True when the store has no uptime commitment of any kind. */
    bool noGuarantee = false;

    /** Shown to the host, in the server configuration. Several lines. */
    std::vector<std::string> forHost;
    /** Shown to every player in the lobby before the game starts. */
    std::vector<std::string> forPlayers;

    /** True when this store should not be chosen without a deliberate act. */
    bool requiresConsent() const { return thirdParty || noGuarantee; }
};

TurnStoreWarning turnStoreWarning(TurnStoreKind kind);

// ============================================================== the client ===
//
// Reading and writing turn data, whichever backend the host chose.
//
// EVERYTHING WRITTEN HERE IS ALREADY SEALED OR ALREADY PUBLIC. This layer moves
// opaque bytes and never decides what may be read: orders arrive sealed (see
// TurnSeal.h) and turn bundles are public by design. So a backend being a dumb,
// world-readable bucket is a property, not a compromise.

/** Where one blob lives. Opaque to the game; meaningful to its backend. */
struct TurnStoreRef {
    std::string id;      // blob id, object key, or a session code
    std::string url;     // what a spectator could open, when there is one

    bool empty() const { return id.empty() && url.empty(); }
};

/**
 * A long-form store.
 *
 * Every call BLOCKS on the network, so nothing here may be called from the
 * render thread -- the same rule as HttpClient, for the same reason.
 */
class TurnStoreClient {
public:
    struct Config {
        TurnStoreKind kind = TurnStoreKind::DurableObject;
        /** The account service, for the backends hosted there. */
        std::string issuer;
        /** Session token, for writes to our own infrastructure. */
        std::string token;
        /** The session this belongs to. */
        std::string sessionCode;
    };

    void configure(const Config& c) { m_config = c; }
    const Config& config() const { return m_config; }

    /** True when this backend moves bytes itself rather than via the player. */
    bool automatic() const { return m_config.kind != TurnStoreKind::Manual; }

    /**
     * Publish a turn bundle. Public and immutable by intent.
     * `ref` is filled in with something players can read it back by.
     */
    bool publishTurn(uint32_t turnNumber, const std::vector<uint8_t>& bundle,
                     TurnStoreRef& ref, std::string& error);

    bool fetchTurn(const TurnStoreRef& ref, std::vector<uint8_t>& out,
                   std::string& error);

    /** Publish one player's SEALED orders. */
    bool publishOrders(uint32_t turnNumber, const std::string& psid,
                       const std::vector<uint8_t>& sealed,
                       TurnStoreRef& ref, std::string& error);

    bool fetchOrders(const TurnStoreRef& ref, std::vector<uint8_t>& out,
                     std::string& error);

private:
    Config m_config;
};

/**
 * The text a player copies out, and the reader for what they paste back.
 *
 * Manual mode has no infrastructure at all: the game shows a block of text and
 * takes one back. It is base64url with a short header so a human can tell at a
 * glance what they are holding and which turn it belongs to -- pasting last
 * turn's orders is otherwise an invisible mistake.
 */
std::string turnStoreEncodeText(const char* what, uint32_t turnNumber,
                                const std::vector<uint8_t>& payload);

/** Reads what turnStoreEncodeText produced. False on anything malformed. */
bool turnStoreDecodeText(const std::string& text, std::string& whatOut,
                         uint32_t& turnOut, std::vector<uint8_t>& payloadOut);
