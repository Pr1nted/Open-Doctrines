// The transport when the build has none.
//
// Mirrors how the mod runtime behaves without WAMR: the game still compiles
// and links, and the multiplayer menu reports that this build cannot connect,
// rather than the build breaking on a machine that could not fetch mbedTLS.
// A player who downloaded such a build gets a sentence explaining it instead
// of a connection that hangs.

#include "WebSocket.h"

#if !defined(__EMSCRIPTEN__) && !defined(OD_ENABLE_NET)

struct WebSocket::Impl {
    std::string errorText =
        "this build of OpenDoctrines was compiled without networking "
        "(-DOD_ENABLE_NET=OFF), so it cannot join or host a multiplayer game";
};

WebSocket::WebSocket() : m_impl(std::make_unique<Impl>()) {}
WebSocket::~WebSocket() = default;

bool WebSocket::available() { return false; }

bool WebSocket::connect(const std::string&, bool) { return false; }
void WebSocket::send(const std::vector<uint8_t>&) {}
void WebSocket::sendText(const std::string&) {}
bool WebSocket::poll(std::vector<uint8_t>&) { return false; }
bool WebSocket::pollText(std::string&) { return false; }
void WebSocket::close() {}

WsState     WebSocket::state() const { return WsState::Closed; }
std::string WebSocket::error() const { return m_impl->errorText; }

#endif  // !__EMSCRIPTEN__ && !OD_ENABLE_NET
