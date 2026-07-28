// Emscripten WebSocket client.
//
// The browser already has a WebSocket, already has the certificate store, and
// would not let us open a raw socket in any case. So this backend is a thin
// adapter over emscripten_websocket_*, and TLS, DNS and the trust store are
// all the browser's problem -- which is the correct division: a page cannot do
// better than the user agent it runs in.
//
// Callbacks arrive on the main thread between frames, so the queues here need
// no locking. The mutex in the native backend is not mirrored, deliberately:
// pretending there is concurrency where there is none would invite someone to
// rely on it.

#include "WebSocket.h"

#ifdef __EMSCRIPTEN__

#include <emscripten/websocket.h>

#include <cstring>

struct WebSocket::Impl {
    EMSCRIPTEN_WEBSOCKET_T socket = 0;
    WsState                state = WsState::Idle;
    std::string            errorText;

    std::deque<std::vector<uint8_t>> inboxBinary;
    std::deque<std::string>          inboxText;
};

namespace {

EM_BOOL onOpen(int, const EmscriptenWebSocketOpenEvent*, void* user) {
    static_cast<WebSocket::Impl*>(user)->state = WsState::Open;
    return EM_TRUE;
}

EM_BOOL onMessage(int, const EmscriptenWebSocketMessageEvent* e, void* user) {
    auto* impl = static_cast<WebSocket::Impl*>(user);
    if (e->isText) {
        // numBytes includes the trailing NUL the runtime appends to text.
        impl->inboxText.emplace_back(reinterpret_cast<const char*>(e->data),
                                     e->numBytes > 0 ? e->numBytes - 1 : 0);
    } else {
        impl->inboxBinary.emplace_back(e->data, e->data + e->numBytes);
    }
    return EM_TRUE;
}

EM_BOOL onError(int, const EmscriptenWebSocketErrorEvent*, void* user) {
    auto* impl = static_cast<WebSocket::Impl*>(user);
    // The browser deliberately does not say why: the detail would be a
    // cross-origin information leak. So neither can we, and a message that
    // guessed would be worse than one that admits it.
    if (impl->errorText.empty()) impl->errorText = "the connection failed";
    return EM_TRUE;
}

EM_BOOL onClose(int, const EmscriptenWebSocketCloseEvent* e, void* user) {
    auto* impl = static_cast<WebSocket::Impl*>(user);
    impl->state = WsState::Closed;
    if (impl->errorText.empty() && e->code != 1000 && e->code != 1005) {
        impl->errorText = e->reason[0] != '\0'
            ? std::string(e->reason)
            : "the connection closed unexpectedly (" + std::to_string(e->code) + ")";
    }
    return EM_TRUE;
}

}  // namespace

WebSocket::WebSocket() : m_impl(std::make_unique<Impl>()) {}

WebSocket::~WebSocket() { close(); }

bool WebSocket::available() {
    return emscripten_websocket_is_supported() != 0;
}

bool WebSocket::connect(const std::string& url, bool allowInsecure) {
    if (m_impl->state != WsState::Idle) return false;

    NetUrl parsed;
    if (!NetUrl::parse(url, parsed)) {
        m_impl->errorText = "that is not a usable WebSocket address";
        m_impl->state = WsState::Closed;
        return false;
    }
    // Same rule as the desktop build. A page served over https cannot open a
    // ws:// socket anyway, but saying so here gives a better message than the
    // browser's mixed-content error.
    if (!parsed.secure && !allowInsecure) {
        m_impl->errorText = "refusing to send credentials over an unencrypted ws:// connection";
        m_impl->state = WsState::Closed;
        return false;
    }
    if (!available()) {
        m_impl->errorText = "this browser does not support WebSockets";
        m_impl->state = WsState::Closed;
        return false;
    }

    EmscriptenWebSocketCreateAttributes attrs = {url.c_str(), nullptr, EM_TRUE};
    m_impl->socket = emscripten_websocket_new(&attrs);
    if (m_impl->socket <= 0) {
        m_impl->errorText = "the browser refused to open the connection";
        m_impl->state = WsState::Closed;
        return false;
    }

    emscripten_websocket_set_onopen_callback(m_impl->socket, m_impl.get(), onOpen);
    emscripten_websocket_set_onmessage_callback(m_impl->socket, m_impl.get(), onMessage);
    emscripten_websocket_set_onerror_callback(m_impl->socket, m_impl.get(), onError);
    emscripten_websocket_set_onclose_callback(m_impl->socket, m_impl.get(), onClose);

    m_impl->state = WsState::Connecting;
    return true;
}

void WebSocket::send(const std::vector<uint8_t>& payload) {
    if (m_impl->state != WsState::Open || m_impl->socket <= 0) return;
    emscripten_websocket_send_binary(
        m_impl->socket, const_cast<uint8_t*>(payload.data()),
        static_cast<uint32_t>(payload.size()));
}

void WebSocket::sendText(const std::string& text) {
    if (m_impl->state != WsState::Open || m_impl->socket <= 0) return;
    emscripten_websocket_send_utf8_text(m_impl->socket, text.c_str());
}

bool WebSocket::poll(std::vector<uint8_t>& out) {
    if (m_impl->inboxBinary.empty()) return false;
    out = std::move(m_impl->inboxBinary.front());
    m_impl->inboxBinary.pop_front();
    return true;
}

bool WebSocket::pollText(std::string& out) {
    if (m_impl->inboxText.empty()) return false;
    out = std::move(m_impl->inboxText.front());
    m_impl->inboxText.pop_front();
    return true;
}

void WebSocket::close() {
    if (m_impl->socket > 0) {
        emscripten_websocket_close(m_impl->socket, 1000, "");
        emscripten_websocket_delete(m_impl->socket);
        m_impl->socket = 0;
    }
    if (m_impl->state != WsState::Idle) m_impl->state = WsState::Closed;
}

WsState WebSocket::state() const { return m_impl->state; }

std::string WebSocket::error() const { return m_impl->errorText; }

#endif  // __EMSCRIPTEN__
