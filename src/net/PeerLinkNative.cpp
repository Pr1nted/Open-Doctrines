// The desktop peer link, on libdatachannel.
//
// THE THREADING RULE, WHICH IS THE WHOLE DIFFICULTY
//
// libdatachannel calls back on its own threads -- ICE on one, SCTP on another --
// and it may call back while we are inside one of its methods. Nothing here
// touches game state from those callbacks. They append to queues under a mutex
// and return; update() drains them on the game thread. Every field below is
// either atomic or guarded, and the callbacks never call back INTO
// libdatachannel, because doing so from its own thread is how this deadlocks.

#include "PeerLink.h"

#if defined(OD_ENABLE_P2P) && !defined(__EMSCRIPTEN__)

#include <rtc/rtc.hpp>

#include <atomic>
#include <deque>
#include <mutex>

struct PeerLink::Impl {
    mutable std::mutex mutex;

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel>    dc;

    std::atomic<PeerState> state{PeerState::Idle};
    std::string errorText;

    std::deque<NetSignal>            outgoing;   // ours, to be sent to the peer
    std::deque<std::vector<uint8_t>> inbox;      // game messages that arrived

    Role role = Role::Offerer;

    void setError(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        if (errorText.empty()) errorText = text;
    }

    void pushSignal(NetSignal::Kind kind, const std::string& payload) {
        std::lock_guard<std::mutex> lock(mutex);
        // Bounded: a peer that never completes must not make us hold an
        // unbounded pile of candidates.
        if (outgoing.size() > 256) return;
        NetSignal s;
        s.kind = kind;
        s.payload = payload;
        outgoing.push_back(std::move(s));
    }

    /** Wire a data channel up once, whichever side created it. */
    void adopt(std::shared_ptr<rtc::DataChannel> channel);
};

void PeerLink::Impl::adopt(std::shared_ptr<rtc::DataChannel> channel) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        dc = channel;
    }

    channel->onOpen([this] { state.store(PeerState::Open); });

    channel->onClosed([this] {
        if (state.load() != PeerState::Closed) {
            setError("the peer-to-peer connection closed");
            state.store(PeerState::Closed);
        }
    });

    channel->onError([this](std::string e) {
        setError(e.empty() ? "the peer-to-peer connection failed" : e);
        state.store(PeerState::Closed);
    });

    channel->onMessage(
        [this](rtc::binary data) {
            std::lock_guard<std::mutex> lock(mutex);
            if (inbox.size() > 4096) return;      // a peer cannot flood us
            inbox.emplace_back(reinterpret_cast<const uint8_t*>(data.data()),
                               reinterpret_cast<const uint8_t*>(data.data()) + data.size());
        },
        [](rtc::string) {
            // Text is not part of this protocol. Everything above is framed
            // binary, so a text message is a peer speaking something else.
        });
}

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
    return {
        "stun:stun.l.google.com:19302",
        "stun:stun.cloudflare.com:3478",
    };
}

PeerLink::PeerLink() : m_impl(std::make_unique<Impl>()) {}
PeerLink::~PeerLink() { close(); }

bool PeerLink::available() { return true; }

bool PeerLink::begin(Role role, std::string& error) {
    if (m_impl->state.load() != PeerState::Idle) return false;
    m_impl->role = role;

    rtc::Configuration config;
    for (const std::string& s : peerDefaultStunServers()) {
        try {
            config.iceServers.emplace_back(s);
        } catch (const std::exception&) {
            // One unusable STUN entry is not worth refusing to connect over;
            // the others still discover an address.
        }
    }

    try {
        m_impl->pc = std::make_shared<rtc::PeerConnection>(config);
    } catch (const std::exception& e) {
        error = std::string("could not start a peer-to-peer connection: ") + e.what();
        m_impl->setError(error);
        m_impl->state.store(PeerState::Closed);
        return false;
    }

    Impl* impl = m_impl.get();

    impl->pc->onLocalDescription([impl](rtc::Description d) {
        impl->pushSignal(d.type() == rtc::Description::Type::Offer
                             ? NetSignal::Kind::Offer
                             : NetSignal::Kind::Answer,
                         std::string(d));
    });

    impl->pc->onLocalCandidate([impl](rtc::Candidate c) {
        impl->pushSignal(NetSignal::Kind::Candidate, std::string(c));
    });

    impl->pc->onStateChange([impl](rtc::PeerConnection::State s) {
        if (s == rtc::PeerConnection::State::Failed) {
            // The honest message. Both peers behind symmetric NAT is the case
            // STUN cannot solve, and fixing it would mean a relay.
            impl->setError("Could not open a direct connection to that player. "
                           "Ask the host for an address you can reach.");
            impl->state.store(PeerState::Closed);
        } else if (s == rtc::PeerConnection::State::Closed) {
            impl->state.store(PeerState::Closed);
        }
    });

    if (role == Role::Answerer) {
        // The host waits to be offered a channel rather than creating one.
        impl->pc->onDataChannel([impl](std::shared_ptr<rtc::DataChannel> channel) {
            impl->adopt(std::move(channel));
        });
    }

    m_impl->state.store(PeerState::Signalling);

    if (role == Role::Offerer) {
        try {
            // Creating the channel is what makes libdatachannel emit an offer,
            // so this is also the moment signalling starts.
            m_impl->adopt(impl->pc->createDataChannel("opendoctrines"));
        } catch (const std::exception& e) {
            error = std::string("could not open a data channel: ") + e.what();
            m_impl->setError(error);
            m_impl->state.store(PeerState::Closed);
            return false;
        }
    }

    error.clear();
    return true;
}

void PeerLink::update() {
    // Everything happens on libdatachannel's threads; the queues are drained by
    // nextSignal() and poll(). Nothing to pump here, and deliberately so --
    // calling into the library from the game thread while it is calling back is
    // the deadlock this design avoids.
}

bool PeerLink::nextSignal(NetSignal& out) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->outgoing.empty()) return false;
    out = std::move(m_impl->outgoing.front());
    m_impl->outgoing.pop_front();
    return true;
}

void PeerLink::acceptSignal(const NetSignal& in) {
    if (!m_impl->pc) return;
    try {
        switch (in.kind) {
            case NetSignal::Kind::Offer:
                m_impl->pc->setRemoteDescription(rtc::Description(in.payload, "offer"));
                break;
            case NetSignal::Kind::Answer:
                m_impl->pc->setRemoteDescription(rtc::Description(in.payload, "answer"));
                break;
            case NetSignal::Kind::Candidate:
                m_impl->pc->addRemoteCandidate(rtc::Candidate(in.payload));
                break;
            case NetSignal::Kind::Failed:
                m_impl->setError("The other player could not connect.");
                m_impl->state.store(PeerState::Closed);
                break;
        }
    } catch (const std::exception&) {
        // Every one of these parses text the peer sent. A malformed offer is a
        // broken or hostile peer, not a reason to take the process down -- and
        // a candidate that will not parse is simply one route fewer.
        if (in.kind != NetSignal::Kind::Candidate) {
            m_impl->setError("That player sent something this game could not read.");
            m_impl->state.store(PeerState::Closed);
        }
    }
}

void PeerLink::send(const std::vector<uint8_t>& payload) {
    std::shared_ptr<rtc::DataChannel> channel;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        channel = m_impl->dc;
    }
    if (!channel || m_impl->state.load() != PeerState::Open) return;
    try {
        channel->send(reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    } catch (const std::exception&) {
        m_impl->setError("the peer-to-peer connection was lost while sending");
        m_impl->state.store(PeerState::Closed);
    }
}

bool PeerLink::poll(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->inbox.empty()) return false;
    out = std::move(m_impl->inbox.front());
    m_impl->inbox.pop_front();
    return true;
}

void PeerLink::close() {
    if (!m_impl) return;
    std::shared_ptr<rtc::DataChannel> channel;
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        channel.swap(m_impl->dc);
        pc.swap(m_impl->pc);
    }
    // Released outside the lock: closing runs callbacks, and those take it.
    try {
        if (channel) channel->close();
        if (pc) pc->close();
    } catch (const std::exception&) {
        // Nothing useful to do while tearing down.
    }
    m_impl->state.store(PeerState::Closed);
}

PeerState PeerLink::state() const { return m_impl->state.load(); }

std::string PeerLink::error() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->errorText;
}

#endif  // OD_ENABLE_P2P && !__EMSCRIPTEN__
