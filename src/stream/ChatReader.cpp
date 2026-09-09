#include "ChatReader.h"

#include "raylib.h"

namespace chatread {

namespace {
// Twitch's read-only endpoint. Hard-coded, like every other host this game
// talks to: a URL taken from anywhere else is a URL somebody else chose.
const char* kEndpoint = "wss://irc-ws.chat.twitch.tv:443";
}  // namespace

Reader::Reader() = default;
Reader::~Reader() = default;

bool Reader::start(const std::string& channel) {
    const std::string c = irc::normaliseChannel(channel);
    if (c.empty()) {
        m_state = State::Failed;
        return false;
    }
    m_channel = c;
    m_carry.clear();
    m_greeted = false;
    m_seen = 0;
    m_ws = std::make_unique<WebSocket>();
    if (!WebSocket::available() || !m_ws->connect(kEndpoint)) {
        m_state = State::Failed;
        return false;
    }
    m_state = State::Connecting;
    return true;
}

void Reader::stop() {
    if (m_ws) m_ws->close();
    m_ws.reset();
    m_state = State::Idle;
    m_carry.clear();
}

std::string Reader::error() const {
    return m_ws ? m_ws->error() : std::string();
}

void Reader::pump(const std::function<void(const std::string&, const std::string&)>& onMessage) {
    if (!m_ws) return;

    // ── RECONNECT, BECAUSE A STREAM IS LONG ──
    //
    // Twitch drops idle readers, networks hiccup, and a four-hour stream will
    // meet both. A reader that gives up the first time is a feature that works
    // in testing and stops working an hour in, which is worse than one that
    // never worked. Backed off so a channel that does not exist is not
    // hammered.
    if (m_ws->state() == WsState::Closed) {
        const double now = GetTime();
        if (m_state != State::Failed) {
            m_state = State::Connecting;
            m_retryAt = now + 5.0;
        }
        if (now >= m_retryAt && !m_channel.empty()) {
            m_retryAt = now + 5.0;
            m_greeted = false;
            m_carry.clear();
            m_ws = std::make_unique<WebSocket>();
            m_ws->connect(kEndpoint);
        }
        return;
    }

    if (m_ws->state() != WsState::Open) return;

    if (!m_greeted) {
        m_greeted = true;
        // The nonce only has to differ between two clients on one machine.
        for (const std::string& line :
             irc::anonymousHandshake(m_channel, (int)(GetTime() * 1000.0)))
            m_ws->sendText(line);
    }

    std::string frame;
    while (m_ws->pollText(frame)) {
        for (const irc::Line& line : irc::parseFrame(frame, m_carry)) {
            switch (line.kind) {
                case irc::Line::Kind::Ping:
                    // Answered immediately, or the server hangs up. See
                    // IrcParse.h.
                    m_ws->sendText("PONG :" + line.token);
                    break;
                case irc::Line::Kind::Welcome:
                    m_state = State::Reading;
                    break;
                case irc::Line::Kind::Failed:
                    m_state = State::Failed;
                    break;
                case irc::Line::Kind::Message:
                    ++m_seen;
                    if (m_state == State::Connecting) m_state = State::Reading;
                    if (onMessage) onMessage(line.who, line.text);
                    break;
                case irc::Line::Kind::Other:
                    break;
            }
        }
    }
}

}  // namespace chatread
