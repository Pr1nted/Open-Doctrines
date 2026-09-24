#include "TlsSocket.h"

#if !defined(__EMSCRIPTEN__) && defined(OD_ENABLE_NET)

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

// For the connect below. mbedtls_net_connect has no timeout of its own, so the
// socket calls have to be made here.
#if defined(_WIN32)
// NOMINMAX before winsock2.h, which drags in windows.h: without it `min`
// and `max` become macros and the std::min below stops parsing.
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
// The root certificates. Windows keeps them in the Schannel store and NOT in
// any file on disk, which is why the PEM list below finds nothing here.
#include <wincrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

std::string mbedError(int rc) {
    char buf[128];
    mbedtls_strerror(rc, buf, sizeof(buf));
    return std::string(buf);
}

// Where each platform keeps its root certificates. mbedTLS, unlike NSS or
// Schannel, ships none of its own -- it is a protocol implementation, not a
// trust store -- so the system's has to be found.
//
// EVERY PATH HERE IS A UNIX PATH, AND WINDOWS HAS NONE.
//
// Windows does not keep its roots in a file. They live in the Schannel
// certificate store, reachable only through CryptoAPI -- so every one of these
// parses failed there, haveTrust stayed false, and the connection was refused
// with "no system certificate store was found". Refusing was right; having no
// way to succeed was not. The symptom reported was that signing in works on
// macOS and on the web and fails on Windows, which is exactly this: macOS finds
// /etc/ssl/cert.pem, the web build never runs this code because the browser
// does its own TLS, and Windows found nothing.
//
// loadWindowsRoots below is the Windows half. This list stays Unix-only on
// purpose: a made-up Windows path would be a guess that fails the same way.
const char* const kTrustBundles[] = {
    "/etc/ssl/cert.pem",                        // macOS, FreeBSD
    "/etc/ssl/certs/ca-certificates.crt",       // Debian, Ubuntu, Alpine
    "/etc/pki/tls/certs/ca-bundle.crt",         // Fedora, RHEL
    "/etc/ssl/ca-bundle.pem",                   // openSUSE
    "/etc/ssl/certs/ca-bundle.crt",
    "/usr/local/share/certs/ca-root-nss.crt",   // FreeBSD ports
    "/etc/certs/ca-certificates.crt",           // Solaris
};

#if defined(_WIN32)
/**
 * The Windows root store, fed into mbedTLS one certificate at a time.
 *
 * CertEnumCertificatesInStore walks the "ROOT" system store, which is the set
 * Windows itself trusts and the set a user's enterprise policy edits. Each
 * entry is already DER, which is what mbedtls_x509_crt_parse_der wants, so
 * nothing has to be re-encoded.
 *
 * PARTIAL SUCCESS IS SUCCESS. A store can contain a certificate mbedTLS will
 * not parse -- an unsupported algorithm, a malformed legacy root -- and
 * refusing the whole chain because one of several hundred is unreadable would
 * put us straight back to "cannot connect". So a parse failure skips that one
 * and the count decides: any root at all is a usable chain.
 *
 * Returns how many were accepted.
 */
int loadWindowsRoots(mbedtls_x509_crt* chain) {
    HCERTSTORE store = ::CertOpenSystemStoreW(0, L"ROOT");
    if (!store) return 0;
    int added = 0;
    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = ::CertEnumCertificatesInStore(store, ctx)) != nullptr) {
        if (!ctx->pbCertEncoded || ctx->cbCertEncoded == 0) continue;
        if (mbedtls_x509_crt_parse_der(chain, ctx->pbCertEncoded,
                                       ctx->cbCertEncoded) == 0)
            ++added;
    }
    // CERT_CLOSE_STORE_FORCE_FLAG would free contexts still in use; the
    // enumeration above has released each as it went, so the plain close is
    // the correct one and leaks nothing.
    ::CertCloseStore(store, 0);
    return added;
}
#endif

}  // namespace

struct TlsSocket::Impl {
    mbedtls_net_context      net{};
    mbedtls_entropy_context  entropy{};
    mbedtls_ctr_drbg_context drbg{};
    mbedtls_ssl_context      ssl{};
    mbedtls_ssl_config       conf{};
    mbedtls_x509_crt         cacert{};
    bool                     drbgReady = false;
    bool                     netReady = false;
    bool                     sslReady = false;

    Impl() {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_x509_crt_init(&cacert);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_entropy_init(&entropy);

        // Seeded in the constructor rather than on connect, so random() works
        // before the socket does -- the WebSocket handshake key is generated
        // first.
        const char* pers = "opendoctrines";
        drbgReady = mbedtls_ctr_drbg_seed(
            &drbg, mbedtls_entropy_func, &entropy,
            reinterpret_cast<const unsigned char*>(pers), strlen(pers)) == 0;
    }

    ~Impl() {
        mbedtls_net_free(&net);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&cacert);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
};

TlsSocket::TlsSocket() : m_impl(new Impl) {}

TlsSocket::~TlsSocket() {
    close();
    delete m_impl;
}

std::string TlsSocket::trustStorePath() {
#if defined(_WIN32)
    // There is no path to report on Windows: the roots are in the Schannel
    // store, not a file. Naming one would be a lie to whatever prints it.
    return {};
#else
    for (const char* path : kTrustBundles) {
        mbedtls_x509_crt probe;
        mbedtls_x509_crt_init(&probe);
        const bool ok = mbedtls_x509_crt_parse_file(&probe, path) == 0;
        mbedtls_x509_crt_free(&probe);
        if (ok) return path;
    }
    return {};
#endif
}

bool TlsSocket::random(uint8_t* out, size_t n) {
    if (!m_impl->drbgReady) return false;
    return mbedtls_ctr_drbg_random(&m_impl->drbg, out, n) == 0;
}

namespace {

#if defined(_WIN32)
inline void closeFd(int fd) { closesocket((SOCKET)fd); }
inline bool setNonBlocking(int fd, bool on) {
    u_long v = on ? 1 : 0;
    return ioctlsocket((SOCKET)fd, FIONBIO, &v) == 0;
}
inline bool connectInProgress() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
inline void closeFd(int fd) { ::close(fd); }
inline bool setNonBlocking(int fd, bool on) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}
inline bool connectInProgress() { return errno == EINPROGRESS; }
#endif

/**
 * Connect within a deadline, trying each address the name resolves to.
 *
 * WHY THIS EXISTS. mbedtls_net_connect is a blocking connect with no timeout,
 * so an address that silently drops packets is bounded only by the operating
 * system -- about 75 seconds on macOS. Every caller of HttpClient inherited
 * that: a request could sit for over a minute with a timeoutMs of 3000 set,
 * because that field only ever bounded the READ, which cannot start until a
 * connection exists.
 *
 * AND WHY IT WALKS THE LIST RATHER THAN TAKING THE FIRST. getaddrinfo commonly
 * returns IPv6 first, and a host with a broken IPv6 route is the ordinary case
 * this was reported from -- the address exists, nothing answers, and the whole
 * budget is spent there while a working IPv4 address sits second in the list.
 * So each address gets a share of the remaining time and the next one is tried
 * when it expires. Not full Happy Eyeballs -- attempts are sequential, not
 * overlapped -- but it is the part that matters here.
 */
/**
 * Winsock, started once per process.
 *
 * On Windows getaddrinfo and socket fail outright until WSAStartup has run, and
 * they fail with WSANOTINITIALISED -- which surfaces here as "could not look up
 * <host>", a name-resolution message for something that is not a name problem.
 *
 * mbedtls does this itself inside mbedtls_net_connect, and that used to be the
 * only way out of this file, so it was covered by accident. connectWithin was
 * then written to hand-roll getaddrinfo and connect -- deliberately, because
 * mbedtls_net_connect is a blocking connect with no timeout -- and the init
 * went with it. Every HTTPS call made before something happened to construct a
 * WsServer (whose constructor is the one other WSAStartup in the tree) failed
 * on Windows: sign-in, the account service, update checks, the announcement
 * board, feedback.
 *
 * So it belongs HERE, at the bottom of the stack every caller goes through,
 * rather than in one caller's constructor.
 */
static void ensureSocketsReady() {
#if defined(_WIN32)
    static const bool started = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)started;
#endif
}

int connectWithin(const std::string& host, const std::string& port, int timeoutMs,
                  std::string& error) {
    ensureSocketsReady();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* list = nullptr;
    const int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &list);
    if (gai != 0 || !list) {
        error = "could not look up " + host;
        return -1;
    }
    const int fd = odnet::connectAny(list, timeoutMs, host, error);
    freeaddrinfo(list);
    return fd;
}

}  // namespace

namespace odnet {

int connectAny(const addrinfo* list, int timeoutMs, const std::string& hostForError,
               std::string& error) {
    int count = 0;
    for (const addrinfo* a = list; a; a = a->ai_next) ++count;

    const auto started = std::chrono::steady_clock::now();
    auto remainingMs = [&]() {
        const auto gone = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started).count();
        return (int)((long long)timeoutMs - gone);
    };

    int fd = -1;
    int left = count;
    for (const addrinfo* a = list; a && fd < 0; a = a->ai_next, --left) {
        const int remaining = remainingMs();
        if (remaining <= 0) break;
        // A share each, so one dead address cannot spend the lot -- but never
        // so small that a slow-but-working address is given no chance.
        int budget = left > 0 ? remaining / left : remaining;
        if (budget < 1200) budget = std::min(remaining, 1200);

        const int s = (int)socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0) continue;
        if (!setNonBlocking(s, true)) { closeFd(s); continue; }

        int rc = ::connect(s, a->ai_addr, (int)a->ai_addrlen);
        if (rc != 0 && !connectInProgress()) { closeFd(s); continue; }

        if (rc != 0) {
#if defined(_WIN32)
            fd_set wr, ex;
            FD_ZERO(&wr); FD_SET((SOCKET)s, &wr);
            FD_ZERO(&ex); FD_SET((SOCKET)s, &ex);
            timeval tv{budget / 1000, (budget % 1000) * 1000};
            const int ready = select(0, nullptr, &wr, &ex, &tv);
            const bool writable = ready > 0 && FD_ISSET((SOCKET)s, &wr);
#else
            pollfd pfd{};
            pfd.fd = s;
            pfd.events = POLLOUT;
            const int ready = poll(&pfd, 1, budget);
            const bool writable = ready > 0 && (pfd.revents & POLLOUT) != 0;
#endif
            if (!writable) { closeFd(s); continue; }   // timed out, or refused

            // POLLOUT only says the attempt FINISHED. Whether it succeeded is
            // in SO_ERROR, and skipping this check is how a refused connection
            // is mistaken for a live one.
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len) != 0 || soerr != 0) {
                closeFd(s);
                continue;
            }
        }

        // Back to blocking: mbedtls_net_recv and mbedtls_net_recv_timeout both
        // expect it, and the WebSocket's read timeout is built on the latter.
        if (!setNonBlocking(s, false)) { closeFd(s); continue; }
        fd = s;
    }

    if (fd < 0) error = "could not reach " + hostForError + " in time";
    return fd;
}

}  // namespace odnet

bool TlsSocket::open(const std::string& host, uint16_t port, bool secure,
                     std::string& error, int connectTimeoutMs) {
    m_secure = secure;
    if (!m_impl->drbgReady) {
        error = "the system random number generator is unavailable";
        return false;
    }

    const std::string portText = std::to_string(port);
    int rc = 0;
    if (connectTimeoutMs > 0) {
        const int fd = connectWithin(host, portText, connectTimeoutMs, error);
        if (fd < 0) return false;
        m_impl->net.fd = fd;
    } else {
        // 0 keeps mbedtls's own blocking connect, bounded only by the operating
        // system. Left reachable on purpose rather than removed: it is the
        // behaviour every caller had before, and a way back to it is worth
        // having if the connect above is ever suspected.
        rc = mbedtls_net_connect(&m_impl->net, host.c_str(), portText.c_str(),
                                 MBEDTLS_NET_PROTO_TCP);
        if (rc != 0) {
            error = "could not reach " + host + ": " + mbedError(rc);
            return false;
        }
    }
    m_impl->netReady = true;

    // ── A CLOSED PEER MUST NOT KILL THIS PROCESS ──
    //
    // Writing to a socket the other end has already closed raises SIGPIPE, and
    // its default action is to terminate. mbedtls_net_send does not suppress
    // it, so a host that went away between one frame and the next took the
    // player's game down with it -- no error, no message, the window simply
    // vanishes. The server's own sockets have had this since they were
    // written (WsServer::doAccept); the client's never did.
    //
    // Per-socket on the BSDs and macOS; on Linux mbedtls already passes
    // MSG_NOSIGNAL for every send.
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        ::setsockopt(m_impl->net.fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif

    if (!secure) { m_open = true; return true; }

    rc = mbedtls_ssl_config_defaults(&m_impl->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { error = "TLS setup failed: " + mbedError(rc); return false; }

    mbedtls_ssl_conf_authmode(&m_impl->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&m_impl->conf, mbedtls_ctr_drbg_random, &m_impl->drbg);

    bool haveTrust = false;
#if defined(_WIN32)
    // Windows first and only: its roots are not in any of the files below, and
    // before this every Windows sign-in failed here.
    haveTrust = loadWindowsRoots(&m_impl->cacert) > 0;
#else
    for (const char* path : kTrustBundles) {
        if (mbedtls_x509_crt_parse_file(&m_impl->cacert, path) == 0) {
            haveTrust = true;
            break;
        }
    }
#endif
    if (!haveTrust) {
        // Refusing is the only correct answer. Connecting anyway, with
        // verification disabled, would mean the game silently accepts any
        // certificate on a machine that happens to lack a bundle -- exactly
        // where a user is least able to notice.
        error = "no system certificate store was found, so the server's identity "
                "cannot be verified";
        return false;
    }
    mbedtls_ssl_conf_ca_chain(&m_impl->conf, &m_impl->cacert, nullptr);

    rc = mbedtls_ssl_setup(&m_impl->ssl, &m_impl->conf);
    if (rc != 0) { error = "TLS setup failed: " + mbedError(rc); return false; }
    m_impl->sslReady = true;

    // One string, used both as SNI and as the name the certificate is checked
    // against, so the two cannot drift apart.
    rc = mbedtls_ssl_set_hostname(&m_impl->ssl, host.c_str());
    if (rc != 0) { error = "TLS setup failed: " + mbedError(rc); return false; }

    // recv_timeout is supplied so conf_read_timeout below can take effect; with
    // a null callback mbedTLS ignores the timeout entirely and blocks.
    mbedtls_ssl_set_bio(&m_impl->ssl, &m_impl->net,
                        mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);
    if (m_readTimeoutMs > 0)
        mbedtls_ssl_conf_read_timeout(&m_impl->conf, (uint32_t)m_readTimeoutMs);

    while ((rc = mbedtls_ssl_handshake(&m_impl->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        error = "could not establish a secure connection to " + host + ": " + mbedError(rc);
        return false;
    }
    if (mbedtls_ssl_get_verify_result(&m_impl->ssl) != 0) {
        error = "the certificate presented by " + host + " could not be verified";
        return false;
    }

    m_open = true;
    return true;
}

void TlsSocket::setReadTimeoutMs(int ms) { m_readTimeoutMs = ms; }

int TlsSocket::read(uint8_t* buf, size_t n) {
    if (!m_open) return kError;
    const int rc = m_secure
        ? mbedtls_ssl_read(&m_impl->ssl, buf, n)
        : (m_readTimeoutMs > 0
               ? mbedtls_net_recv_timeout(&m_impl->net, buf, n, (uint32_t)m_readTimeoutMs)
               : mbedtls_net_recv(&m_impl->net, buf, n));

    // A timeout is "nothing yet", not a failure -- the caller loops and gets a
    // chance to send in between, which is the entire point of the timeout.
    if (rc == MBEDTLS_ERR_SSL_TIMEOUT) return kRetry;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) return kRetry;
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return kClosed;
    if (rc == 0) return kClosed;
    if (rc < 0) return kError;
    return rc;
}

bool TlsSocket::writeAll(const uint8_t* data, size_t n) {
    if (!m_open) return false;
    while (n > 0) {
        const int rc = m_secure
            ? mbedtls_ssl_write(&m_impl->ssl, data, n)
            : mbedtls_net_send(&m_impl->net, data, n);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc <= 0) return false;
        data += rc;
        n -= static_cast<size_t>(rc);
    }
    return true;
}

void TlsSocket::close() {
    if (!m_open) return;
    if (m_secure && m_impl->sslReady) mbedtls_ssl_close_notify(&m_impl->ssl);
    if (m_impl->netReady) {
        mbedtls_net_free(&m_impl->net);
        mbedtls_net_init(&m_impl->net);
        m_impl->netReady = false;
    }
    m_open = false;
}

#endif  // !__EMSCRIPTEN__ && OD_ENABLE_NET
