#pragma once

// A TLS client socket, and the game's only certificate-verification policy.
//
// Shared by the WebSocket transport and the HTTPS client. One copy, because
// two would be two places to get certificate verification wrong, and the
// second one always ends up being the one nobody reviewed.
//
// THE POLICY, IN FULL
//
//   - Certificates are verified. There is no switch to turn that off, not even
//     behind a build flag, because the switch that exists is the one that ends
//     up enabled in somebody's build.
//   - The hostname is checked against the certificate, and the same string is
//     sent as SNI, so the two cannot disagree.
//   - mbedTLS ships no trust store, so the platform's is located at connect
//     time. If none is found the connection is REFUSED. A client that cannot
//     verify must not connect anyway and call it success.

#include <cstdint>
#include <string>

class TlsSocket {
public:
    TlsSocket();
    ~TlsSocket();

    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    // Blocking. `secure` false skips TLS entirely and is only reachable from
    // callers that have already decided plaintext is acceptable -- in practice
    // `wrangler dev` on localhost and nothing else.
    bool open(const std::string& host, uint16_t port, bool secure, std::string& error);

    enum : int { kRetry = -2, kClosed = 0, kError = -1 };

    /**
     * Make read() give up after `ms` and return kRetry instead of blocking.
     *
     * Off by default, because the HTTP client wants a read that waits for the
     * reply it is expecting. The WebSocket wants the opposite: its one thread
     * both reads and writes, so a read that blocks until the peer says
     * something holds every OUTGOING message hostage until it does. That is not
     * theoretical -- it stalled every join where the client took more than a
     * few milliseconds to fetch its ticket.
     *
     * Call before open().
     */
    void setReadTimeoutMs(int ms);

    // >0 bytes read, kClosed on a clean close, kRetry if the caller should
    // simply try again, kError otherwise.
    int read(uint8_t* buf, size_t n);

    bool writeAll(const uint8_t* data, size_t n);

    // Bytes from the TLS CSPRNG. Available before open() succeeds, because the
    // WebSocket handshake key is generated before the socket is used.
    bool random(uint8_t* out, size_t n);

    void close();

    bool isOpen() const { return m_open; }

    // Where a system trust store was found, or empty if none was. Exposed only
    // so a diagnostic can say which one is in use.
    static std::string trustStorePath();

private:
    struct Impl;
    Impl* m_impl;
    bool  m_open = false;
    bool  m_secure = true;
    int   m_readTimeoutMs = 0;      // 0 = block until data arrives
};
