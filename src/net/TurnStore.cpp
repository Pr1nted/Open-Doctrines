#include "TurnStore.h"

const char* turnStoreName(TurnStoreKind k) {
    switch (k) {
        case TurnStoreKind::DurableObject: return "the session's own storage";
        case TurnStoreKind::R2:       return "Cloudflare R2";
        case TurnStoreKind::JsonBlob: return "jsonblob.com";
        case TurnStoreKind::Manual:   return "manual copy and paste";
    }
    return "unknown";
}

TurnStoreWarning turnStoreWarning(TurnStoreKind kind) {
    TurnStoreWarning w;

    switch (kind) {
        case TurnStoreKind::DurableObject:
            w.thirdParty = false;
            w.publiclyReadable = true;
            w.noGuarantee = false;
            w.forHost = {
                "Turn data is stored with the game session itself, on your own "
                "Cloudflare account. Nothing extra needs enabling and no payment "
                "details are involved.",
                "Published turns are readable by anyone holding the link. That is "
                "deliberate: it is what lets people spectate. They cannot be "
                "edited by anyone but you.",
                "Player orders are encrypted before they leave their machine, so "
                "nobody -- including you, before the turn resolves -- can read "
                "another player's orders.",
                "A session with nobody connected is kept for 90 days, so being "
                "away between turns is expected rather than a problem.",
            };
            w.forPlayers = {
                "This game stores its turns with the session itself, on the "
                "host's own Cloudflare account.",
                "Each processed turn is published so people can spectate. Your "
                "ORDERS are encrypted and only the host can read them.",
            };
            return w;

        case TurnStoreKind::R2:
            w.thirdParty = false;
            w.publiclyReadable = true;
            w.noGuarantee = false;
            w.forHost = {
                "Turn data is stored on your own Cloudflare account, in R2.",
                "R2 has to be enabled in the Cloudflare dashboard, and Cloudflare "
                "asks for a payment method before it will enable it -- even "
                "though the free allowance is generous and this will not "
                "approach it. If you would rather not, the session's own storage "
                "does the same job with nothing to enable.",
                "Published turns are readable by anyone holding the link. That is "
                "deliberate: it is what lets people spectate a tournament. They "
                "cannot be edited by anyone but you.",
                "Player orders are encrypted before they leave their machine, so "
                "you cannot read another player's orders before the turn resolves "
                "and neither can anyone else.",
            };
            w.forPlayers = {
                "This game stores its turns on the host's own Cloudflare account.",
                "Each processed turn is published so people can spectate. Your "
                "ORDERS are encrypted and only the host can read them.",
            };
            return w;

        case TurnStoreKind::JsonBlob:
            w.thirdParty = true;
            w.publiclyReadable = true;
            w.noGuarantee = true;
            w.forHost = {
                "READ THIS BEFORE CHOOSING IT.",
                "Turn data will be stored on jsonblob.com, a free public service "
                "run by someone unconnected to this game and to you.",
                "It makes NO promise of uptime, retention or privacy. It can "
                "delete your data, rate-limit you, or disappear entirely, without "
                "notice and without recourse. If that happens mid-tournament, the "
                "tournament is gone.",
                "Anyone who obtains a blob URL can READ it, and can also OVERWRITE "
                "it. Player orders are encrypted and authenticated, so tampering "
                "destroys a turn rather than forging one -- but a destroyed turn "
                "is still a turn that player loses.",
                "Published turn bundles are world-readable. Do not use this store "
                "for a game whose state you would not put on a public web page.",
                "Cloudflare R2 does the same job on infrastructure you control. "
                "Choose this only if you have a specific reason not to use it.",
            };
            w.forPlayers = {
                "This game sends its turn data through jsonblob.com, a free "
                "public service run by a third party.",
                "Your orders are encrypted, so nobody there can read them. But "
                "the game state after each turn is world-readable, and the "
                "service offers no guarantee it will keep anything or stay up.",
                "If it goes down or loses data, this game may not be finishable.",
            };
            return w;

        case TurnStoreKind::Manual:
            w.thirdParty = true;    // wherever the players choose to paste it
            w.publiclyReadable = true;
            w.noGuarantee = true;
            w.forHost = {
                "Every turn will be copied and pasted by hand, by you and by "
                "every player, wherever you each choose to put it.",
                "Whatever service you use sees the data. Orders stay encrypted, "
                "so their contents are safe, but you are trusting each player to "
                "paste the right thing to the right place every turn.",
                "This is workable for a small group who have agreed to it. It is "
                "not workable for strangers.",
            };
            w.forPlayers = {
                "This game has no automatic turn transport. Every turn you will "
                "copy text out of the game and paste a reply back in by hand.",
                "Your orders are encrypted before you copy them, so wherever you "
                "paste them cannot read them.",
            };
            return w;
    }
    return w;
}

// ============================================================== the client ===

#include "HttpClient.h"

namespace {

// jsonblob's public API. No key, no account, no promises -- which is exactly
// what its warning text says out loud.
const char* kJsonBlobApi = "https://jsonblob.com/api/jsonBlob";

std::string b64urlEncodeBytes(const std::vector<uint8_t>& v) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((v.size() * 4 + 2) / 3);
    for (size_t i = 0; i < v.size(); i += 3) {
        const uint32_t a = v[i];
        const uint32_t b = i + 1 < v.size() ? v[i + 1] : 0;
        const uint32_t c = i + 2 < v.size() ? v[i + 2] : 0;
        const uint32_t x = (a << 16) | (b << 8) | c;
        out += T[(x >> 18) & 63];
        out += T[(x >> 12) & 63];
        if (i + 1 < v.size()) out += T[(x >> 6) & 63];
        if (i + 2 < v.size()) out += T[x & 63];
    }
    return out;
}

bool b64urlDecodeBytes(const std::string& in, std::vector<uint8_t>& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' ) break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;  // pasted text
        const int v = value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF)); }
    }
    if (bits >= 6) return false;
    if (bits && (acc & ((1u << bits) - 1))) return false;
    return true;
}

/** A blob is JSON, so opaque bytes travel inside it as one base64url string. */
std::string wrapBlob(uint32_t turnNumber, const std::vector<uint8_t>& payload) {
    return "{\"od\":1,\"turn\":" + std::to_string(turnNumber) +
           ",\"data\":\"" + b64urlEncodeBytes(payload) + "\"}";
}

bool unwrapBlob(const std::string& body, std::vector<uint8_t>& out) {
    // Bounded: a store is a stranger's machine, and the reply is untrusted.
    const std::string data = httpJsonString(body, "data", 64u * 1024 * 1024);
    if (data.empty()) return false;
    return b64urlDecodeBytes(data, out);
}

}  // namespace

std::string turnStoreEncodeText(const char* what, uint32_t turnNumber,
                                const std::vector<uint8_t>& payload) {
    // The header is for the human holding it. Pasting the wrong turn back is
    // otherwise an invisible mistake, and one that costs somebody a turn.
    std::string out = "--- OpenDoctrines ";
    out += what;
    out += " turn ";
    out += std::to_string(turnNumber);
    out += " ---\n";
    const std::string body = b64urlEncodeBytes(payload);
    for (size_t i = 0; i < body.size(); i += 76) {
        out += body.substr(i, 76);
        out += '\n';
    }
    out += "--- end ---\n";
    return out;
}

bool turnStoreDecodeText(const std::string& text, std::string& whatOut,
                         uint32_t& turnOut, std::vector<uint8_t>& payloadOut) {
    const size_t head = text.find("--- OpenDoctrines ");
    if (head == std::string::npos) return false;
    const size_t headEnd = text.find(" ---\n", head);
    if (headEnd == std::string::npos) return false;

    const std::string header = text.substr(head + 18, headEnd - head - 18);
    const size_t turnAt = header.rfind(" turn ");
    if (turnAt == std::string::npos) return false;
    whatOut = header.substr(0, turnAt);
    turnOut = static_cast<uint32_t>(strtoul(header.c_str() + turnAt + 6, nullptr, 10));

    const size_t bodyStart = headEnd + 5;
    const size_t tail = text.find("--- end ---", bodyStart);
    if (tail == std::string::npos) return false;

    return b64urlDecodeBytes(text.substr(bodyStart, tail - bodyStart), payloadOut);
}

bool TurnStoreClient::publishTurn(uint32_t turnNumber,
                                  const std::vector<uint8_t>& bundle,
                                  TurnStoreRef& ref, std::string& error) {
    switch (m_config.kind) {
        case TurnStoreKind::Manual:
            // Nothing to publish to: the caller shows the text instead.
            ref = TurnStoreRef{};
            error.clear();
            return true;

        case TurnStoreKind::JsonBlob: {
            HttpRequest req;
            req.method = "POST";
            req.url = kJsonBlobApi;
            req.body = wrapBlob(turnNumber, bundle);
            const HttpResponse res = httpRequest(req);
            if (!res.ok()) {
                error = res.error.empty()
                    ? "jsonblob.com would not accept the turn (it makes no promises)."
                    : res.error;
                return false;
            }
            // The id comes back in Location; the body is the blob itself.
            ref.url = httpJsonString(res.body, "location", 512);
            if (ref.url.empty()) ref.url = res.body;
            ref.id = ref.url;
            error.clear();
            return true;
        }

        case TurnStoreKind::DurableObject:
        case TurnStoreKind::R2: {
            HttpRequest req;
            req.method = "PUT";
            req.url = m_config.issuer + "/session/" + m_config.sessionCode +
                      "/turn/" + std::to_string(turnNumber);
            req.bearer = m_config.token;
            req.body = wrapBlob(turnNumber, bundle);
            req.allowInsecure = m_config.issuer.rfind("http://localhost", 0) == 0;
            const HttpResponse res = httpRequest(req);
            if (!res.ok()) {
                error = res.error.empty() ? "Could not publish the turn." : res.error;
                return false;
            }
            ref.id = std::to_string(turnNumber);
            ref.url = req.url;
            error.clear();
            return true;
        }
    }
    error = "unknown turn store";
    return false;
}

bool TurnStoreClient::fetchTurn(const TurnStoreRef& ref, std::vector<uint8_t>& out,
                                std::string& error) {
    if (m_config.kind == TurnStoreKind::Manual) {
        error = "this game has no automatic turn transport";
        return false;
    }
    if (ref.url.empty()) { error = "no turn to fetch"; return false; }

    HttpRequest req;
    req.url = ref.url;
    req.allowInsecure = ref.url.rfind("http://localhost", 0) == 0;
    const HttpResponse res = httpRequest(req);
    if (!res.ok()) {
        error = res.error.empty() ? "Could not read that turn." : res.error;
        return false;
    }
    if (!unwrapBlob(res.body, out)) {
        error = "That turn was not readable.";
        return false;
    }
    error.clear();
    return true;
}

bool TurnStoreClient::publishOrders(uint32_t turnNumber, const std::string& psid,
                                    const std::vector<uint8_t>& sealed,
                                    TurnStoreRef& ref, std::string& error) {
    // Sealed already; see TurnSeal.h. This layer never sees plaintext orders,
    // which is why handing them to a public bucket is not a contradiction.
    switch (m_config.kind) {
        case TurnStoreKind::Manual:
            ref = TurnStoreRef{};
            error.clear();
            return true;

        case TurnStoreKind::JsonBlob: {
            HttpRequest req;
            req.method = "POST";
            req.url = kJsonBlobApi;
            req.body = wrapBlob(turnNumber, sealed);
            const HttpResponse res = httpRequest(req);
            if (!res.ok()) {
                error = res.error.empty() ? "jsonblob.com would not accept your orders."
                                          : res.error;
                return false;
            }
            ref.url = httpJsonString(res.body, "location", 512);
            if (ref.url.empty()) ref.url = res.body;
            ref.id = ref.url;
            error.clear();
            return true;
        }

        case TurnStoreKind::DurableObject:
        case TurnStoreKind::R2: {
            HttpRequest req;
            req.method = "PUT";
            req.url = m_config.issuer + "/session/" + m_config.sessionCode +
                      "/orders/" + std::to_string(turnNumber) + "/" + psid;
            req.bearer = m_config.token;
            req.body = wrapBlob(turnNumber, sealed);
            req.allowInsecure = m_config.issuer.rfind("http://localhost", 0) == 0;
            const HttpResponse res = httpRequest(req);
            if (!res.ok()) {
                error = res.error.empty() ? "Could not submit your orders." : res.error;
                return false;
            }
            ref.id = psid;
            ref.url = req.url;
            error.clear();
            return true;
        }
    }
    error = "unknown turn store";
    return false;
}

bool TurnStoreClient::fetchOrders(const TurnStoreRef& ref, std::vector<uint8_t>& out,
                                  std::string& error) {
    return fetchTurn(ref, out, error);   // same shape; the difference is the URL
}
