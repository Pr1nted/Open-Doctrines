// The browser peer link, on the RTCPeerConnection the browser already has.
//
// NOTHING IS SHIPPED FOR THIS, AND NOTHING COULD BE
//
// A page cannot open a listening socket and cannot run a tunnel, so for the web
// build this is not a fallback -- it is the only transport that can host or join
// at all. Happily it is also the one that costs zero bytes to distribute:
// WebRTC is part of every browser.
//
// HOW THE TWO HALVES TALK
//
// The JavaScript side owns the connection and pushes everything interesting
// into two queues on a global object. C++ drains those queues from update(),
// on the one thread emscripten gives us. No callbacks cross into C++, which
// keeps this free of the re-entrancy the native backend has to be careful
// about, and keeps the glue small enough to read.

#include "PeerLink.h"

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

#include <deque>
#include <string>

// clang-format off
EM_JS(void, od_peer_init, (), {
    // One namespace, created once. Reloading the module must not orphan a
    // half-open connection, so an existing one is torn down first.
    if (Module.odPeer && Module.odPeer.pc) {
        try { Module.odPeer.pc.close(); } catch (e) {}
    }
    Module.odPeer = {
        pc: null,
        dc: null,
        signals: [],      // {kind, payload} produced by this side
        inbox: [],        // Uint8Array game messages
        state: 0,         // 0 idle, 1 signalling, 2 open, 3 closed
        error: "",
    };
});

EM_JS(int, od_peer_begin, (int isOfferer), {
    var P = Module.odPeer;
    if (!P) return 0;
    try {
        P.pc = new RTCPeerConnection({
            iceServers: [
                { urls: "stun:stun.l.google.com:19302" },
                { urls: "stun:stun.cloudflare.com:3478" },
            ],
        });
    } catch (e) {
        P.error = "this browser would not open a peer connection: " + e;
        P.state = 3;
        return 0;
    }

    P.pc.onicecandidate = function (ev) {
        // A null candidate means gathering finished; there is nothing to send.
        if (ev.candidate && P.signals.length < 256) {
            P.signals.push({ kind: 2, payload: ev.candidate.candidate });
        }
    };

    P.pc.onconnectionstatechange = function () {
        var s = P.pc.connectionState;
        if (s === "failed") {
            // The case STUN cannot solve. Saying so beats a silent hang.
            P.error = "Could not open a direct connection to that player. " +
                      "Ask the host for an address you can reach.";
            P.state = 3;
        } else if (s === "closed" || s === "disconnected") {
            P.state = 3;
        }
    };

    var adopt = function (channel) {
        P.dc = channel;
        channel.binaryType = "arraybuffer";
        channel.onopen = function () { P.state = 2; };
        channel.onclose = function () { if (P.state !== 3) P.state = 3; };
        channel.onmessage = function (ev) {
            if (P.inbox.length > 4096) return;      // a peer cannot flood us
            P.inbox.push(new Uint8Array(ev.data));
        };
    };

    if (isOfferer) {
        adopt(P.pc.createDataChannel("opendoctrines"));
        P.pc.createOffer().then(function (offer) {
            return P.pc.setLocalDescription(offer).then(function () {
                P.signals.push({ kind: 0, payload: offer.sdp });
            });
        }).catch(function (e) { P.error = "" + e; P.state = 3; });
    } else {
        P.pc.ondatachannel = function (ev) { adopt(ev.channel); };
    }

    P.state = 1;
    return 1;
});

/** Length of the next queued signal's payload, or -1 when there is none. */
EM_JS(int, od_peer_signal_kind, (), {
    var P = Module.odPeer;
    return (P && P.signals.length) ? P.signals[0].kind : -1;
});

EM_JS(char*, od_peer_signal_take, (), {
    var P = Module.odPeer;
    if (!P || !P.signals.length) return 0;
    var s = P.signals.shift();
    var n = lengthBytesUTF8(s.payload) + 1;
    var p = _malloc(n);
    stringToUTF8(s.payload, p, n);
    return p;
});

EM_JS(void, od_peer_accept, (int kind, const char* payload), {
    var P = Module.odPeer;
    if (!P || !P.pc) return;
    var text = UTF8ToString(payload);
    try {
        if (kind === 0 || kind === 1) {
            var type = kind === 0 ? "offer" : "answer";
            P.pc.setRemoteDescription({ type: type, sdp: text }).then(function () {
                if (type !== "offer") return;
                // Answering is the other half of being offered to.
                return P.pc.createAnswer().then(function (a) {
                    return P.pc.setLocalDescription(a).then(function () {
                        P.signals.push({ kind: 1, payload: a.sdp });
                    });
                });
            }).catch(function (e) { P.error = "" + e; P.state = 3; });
        } else if (kind === 2) {
            // A candidate that will not parse is one route fewer, not a
            // failure: the others may still work.
            P.pc.addIceCandidate({ candidate: text, sdpMid: "0" }).catch(function () {});
        } else {
            P.error = "The other player could not connect.";
            P.state = 3;
        }
    } catch (e) {
        P.error = "" + e;
        P.state = 3;
    }
});

EM_JS(int, od_peer_state, (), {
    return Module.odPeer ? Module.odPeer.state : 0;
});

EM_JS(char*, od_peer_error, (), {
    var P = Module.odPeer;
    if (!P || !P.error) return 0;
    var n = lengthBytesUTF8(P.error) + 1;
    var p = _malloc(n);
    stringToUTF8(P.error, p, n);
    return p;
});

EM_JS(int, od_peer_inbox_size, (), {
    var P = Module.odPeer;
    return (P && P.inbox.length) ? P.inbox[0].length : -1;
});

EM_JS(void, od_peer_inbox_take, (uint8_t* dest, int n), {
    var P = Module.odPeer;
    if (!P || !P.inbox.length) return;
    HEAPU8.set(P.inbox.shift().subarray(0, n), dest);
});

EM_JS(void, od_peer_send, (const uint8_t* data, int n), {
    var P = Module.odPeer;
    if (!P || !P.dc || P.dc.readyState !== "open") return;
    // Copied out of the heap: the buffer is reused the moment this returns.
    P.dc.send(new Uint8Array(HEAPU8.subarray(data, data + n)));
});

EM_JS(void, od_peer_close, (), {
    var P = Module.odPeer;
    if (!P) return;
    try { if (P.dc) P.dc.close(); } catch (e) {}
    try { if (P.pc) P.pc.close(); } catch (e) {}
    P.dc = null;
    P.pc = null;
    P.state = 3;
});
// clang-format on

struct PeerLink::Impl {
    std::deque<NetSignal>            outgoing;
    std::deque<std::vector<uint8_t>> inbox;
    std::string                      errorText;
    bool                             started = false;
};

const char* peerStateName(PeerState s) {
    switch (s) {
        case PeerState::Idle:       return "Idle";
        case PeerState::Signalling: return "Signalling";
        case PeerState::Open:       return "Open";
        case PeerState::Closed:     return "Closed";
    }
    return "Unknown";
}

std::vector<std::string> peerDefaultStunServers() {
    // Stated here too, though the browser is given them in od_peer_begin: this
    // is what the privacy documentation refers to, and one list that disagrees
    // with the other would make that documentation wrong.
    return {
        "stun:stun.l.google.com:19302",
        "stun:stun.cloudflare.com:3478",
    };
}

PeerLink::PeerLink() : m_impl(std::make_unique<Impl>()) {}
PeerLink::~PeerLink() { close(); }

bool PeerLink::available() { return true; }

bool PeerLink::begin(Role role, std::string& error) {
    if (m_impl->started) return false;
    od_peer_init();
    if (!od_peer_begin(role == Role::Offerer ? 1 : 0)) {
        error = "this browser could not open a peer-to-peer connection";
        m_impl->errorText = error;
        return false;
    }
    m_impl->started = true;
    error.clear();
    return true;
}

void PeerLink::update() {
    if (!m_impl->started) return;

    // Signals the browser produced.
    for (;;) {
        const int kind = od_peer_signal_kind();
        if (kind < 0) break;
        char* text = od_peer_signal_take();
        if (!text) break;
        NetSignal s;
        s.kind = kind == 0   ? NetSignal::Kind::Offer
               : kind == 1   ? NetSignal::Kind::Answer
               : kind == 2   ? NetSignal::Kind::Candidate
                             : NetSignal::Kind::Failed;
        s.payload = text;
        free(text);
        if (m_impl->outgoing.size() < 256) m_impl->outgoing.push_back(std::move(s));
    }

    // Messages that arrived.
    for (;;) {
        const int n = od_peer_inbox_size();
        if (n < 0) break;
        std::vector<uint8_t> msg(static_cast<size_t>(n));
        od_peer_inbox_take(msg.empty() ? nullptr : msg.data(), n);
        if (m_impl->inbox.size() < 4096) m_impl->inbox.push_back(std::move(msg));
    }

    if (m_impl->errorText.empty()) {
        if (char* e = od_peer_error()) {
            m_impl->errorText = e;
            free(e);
        }
    }
}

bool PeerLink::nextSignal(NetSignal& out) {
    if (m_impl->outgoing.empty()) return false;
    out = std::move(m_impl->outgoing.front());
    m_impl->outgoing.pop_front();
    return true;
}

void PeerLink::acceptSignal(const NetSignal& in) {
    od_peer_accept(static_cast<int>(in.kind), in.payload.c_str());
}

void PeerLink::send(const std::vector<uint8_t>& payload) {
    if (payload.empty()) return;
    od_peer_send(payload.data(), static_cast<int>(payload.size()));
}

bool PeerLink::poll(std::vector<uint8_t>& out) {
    if (m_impl->inbox.empty()) return false;
    out = std::move(m_impl->inbox.front());
    m_impl->inbox.pop_front();
    return true;
}

void PeerLink::close() {
    if (m_impl && m_impl->started) {
        od_peer_close();
        m_impl->started = false;
    }
}

PeerState PeerLink::state() const {
    switch (od_peer_state()) {
        case 1:  return PeerState::Signalling;
        case 2:  return PeerState::Open;
        case 3:  return PeerState::Closed;
        default: return PeerState::Idle;
    }
}

std::string PeerLink::error() const { return m_impl->errorText; }

#endif  // __EMSCRIPTEN__
