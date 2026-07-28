// The peer link when this build has none.
//
// Same bargain the mod runtime and the transport already make: a build without
// the dependency still compiles and links, and says plainly that it cannot do
// this -- rather than failing to build on a machine that could not fetch
// libdatachannel. The direct connection is unaffected, which is why this is a
// reduced game and not a broken one.

#include "PeerLink.h"

#if !defined(OD_ENABLE_P2P) && !defined(__EMSCRIPTEN__)

struct PeerLink::Impl {
    std::string errorText =
        "this build of OpenDoctrines was compiled without the peer-to-peer "
        "fallback (-DOD_ENABLE_P2P=OFF), so it can only reach hosts that have "
        "an address you can connect to directly";
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

std::vector<std::string> peerDefaultStunServers() { return {}; }

PeerLink::PeerLink() : m_impl(std::make_unique<Impl>()) {}
PeerLink::~PeerLink() = default;

bool PeerLink::available() { return false; }

bool PeerLink::begin(Role, std::string& error) {
    error = m_impl->errorText;
    return false;
}

void PeerLink::update() {}
bool PeerLink::nextSignal(NetSignal&) { return false; }
void PeerLink::acceptSignal(const NetSignal&) {}
void PeerLink::send(const std::vector<uint8_t>&) {}
bool PeerLink::poll(std::vector<uint8_t>&) { return false; }
void PeerLink::close() {}

PeerState   PeerLink::state() const { return PeerState::Closed; }
std::string PeerLink::error() const { return m_impl->errorText; }

#endif  // !OD_ENABLE_P2P && !__EMSCRIPTEN__
