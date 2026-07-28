#include "TlsSocket.h"

#if !defined(__EMSCRIPTEN__) && defined(OD_ENABLE_NET)

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <cstring>
#include <string>

namespace {

std::string mbedError(int rc) {
    char buf[128];
    mbedtls_strerror(rc, buf, sizeof(buf));
    return std::string(buf);
}

// Where each platform keeps its root certificates. mbedTLS, unlike NSS or
// Schannel, ships none of its own -- it is a protocol implementation, not a
// trust store -- so the system's has to be found.
const char* const kTrustBundles[] = {
    "/etc/ssl/cert.pem",                        // macOS, FreeBSD
    "/etc/ssl/certs/ca-certificates.crt",       // Debian, Ubuntu, Alpine
    "/etc/pki/tls/certs/ca-bundle.crt",         // Fedora, RHEL
    "/etc/ssl/ca-bundle.pem",                   // openSUSE
    "/etc/ssl/certs/ca-bundle.crt",
    "/usr/local/share/certs/ca-root-nss.crt",   // FreeBSD ports
    "/etc/certs/ca-certificates.crt",           // Solaris
};

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
    for (const char* path : kTrustBundles) {
        mbedtls_x509_crt probe;
        mbedtls_x509_crt_init(&probe);
        const bool ok = mbedtls_x509_crt_parse_file(&probe, path) == 0;
        mbedtls_x509_crt_free(&probe);
        if (ok) return path;
    }
    return {};
}

bool TlsSocket::random(uint8_t* out, size_t n) {
    if (!m_impl->drbgReady) return false;
    return mbedtls_ctr_drbg_random(&m_impl->drbg, out, n) == 0;
}

bool TlsSocket::open(const std::string& host, uint16_t port, bool secure,
                     std::string& error) {
    m_secure = secure;
    if (!m_impl->drbgReady) {
        error = "the system random number generator is unavailable";
        return false;
    }

    const std::string portText = std::to_string(port);
    int rc = mbedtls_net_connect(&m_impl->net, host.c_str(), portText.c_str(),
                                 MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
        error = "could not reach " + host + ": " + mbedError(rc);
        return false;
    }
    m_impl->netReady = true;

    if (!secure) { m_open = true; return true; }

    rc = mbedtls_ssl_config_defaults(&m_impl->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { error = "TLS setup failed: " + mbedError(rc); return false; }

    mbedtls_ssl_conf_authmode(&m_impl->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&m_impl->conf, mbedtls_ctr_drbg_random, &m_impl->drbg);

    bool haveTrust = false;
    for (const char* path : kTrustBundles) {
        if (mbedtls_x509_crt_parse_file(&m_impl->cacert, path) == 0) {
            haveTrust = true;
            break;
        }
    }
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
