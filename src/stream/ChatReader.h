#pragma once

// The socket half of chat-plays-the-country.
//
// Connects to Twitch's IRC-over-WebSocket as an anonymous reader, feeds every
// line to the parser, answers the PINGs, and hands finished messages to whoever
// is counting. It holds no credential and cannot post: see IrcParse.h.
//
// The reader is deliberately thin. Everything with a decision in it -- what a
// line means, what counts as a vote, who wins -- is in IrcParse.h and
// ChatVote.h, where it is tested without a socket. What is left here is
// connect, pump, reconnect, and that is all this file should ever grow into.

#include "IrcParse.h"
#include "net/WebSocket.h"

#include <functional>
#include <memory>
#include <string>

namespace chatread {

enum class State { Idle = 0, Connecting, Reading, Failed };

class Reader {
public:
    Reader();
    ~Reader();

    /**
     * Start reading `channel`. Safe to call again to switch channels.
     *
     * Returns false only for a channel name this cannot use; everything else
     * (DNS, TLS, the handshake) surfaces later as State::Failed with an error.
     */
    bool start(const std::string& channel);
    void stop();

    /**
     * Pump the socket. Call once a frame.
     *
     * `onMessage(who, text)` is called for each chat line, on the calling
     * thread, so a counter may touch game state from it.
     */
    void pump(const std::function<void(const std::string&, const std::string&)>& onMessage);

    State state() const { return m_state; }
    const std::string& channel() const { return m_channel; }
    std::string error() const;
    /// Chat lines seen since start(). Tells a streamer the reader is alive even
    /// when nobody is voting, which is the difference between "quiet" and
    /// "broken".
    long long seen() const { return m_seen; }

private:
    std::unique_ptr<WebSocket> m_ws;
    std::string m_channel;
    std::string m_carry;
    State m_state = State::Idle;
    long long m_seen = 0;
    bool m_greeted = false;
    double m_retryAt = 0.0;
};

}  // namespace chatread
