#include "AccountClient.h"

#include "HttpClient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

// The device flow's own recommended interval, and a ceiling on how long we
// will sit at the consent screen before giving up.
constexpr int  kPollIntervalMs = 2000;
constexpr int  kLoginTimeoutMs = 10 * 60 * 1000;

constexpr size_t kMaxTokenBytes = 8 * 1024;

// Mirrors net/src/accounts/nickname.ts. Kept in step by hand, and deliberately
// only the SHAPE rules -- the blocklist is deployment data that never reaches
// a client, so this can say "looks fine" about a name the server will refuse.
constexpr int kNickMin = 3;
constexpr int kNickMax = 24;

bool isSeparator(char c) { return c == ' ' || c == '_' || c == '-' || c == '.'; }

}  // namespace

const char* authProviderId(AuthProvider p) {
    switch (p) {
        case AuthProvider::Google:  return "google";
        case AuthProvider::Discord: return "discord";
        case AuthProvider::GitHub:  return "github";
    }
    return "google";
}

const char* authProviderLabel(AuthProvider p) {
    switch (p) {
        case AuthProvider::Google:  return "Google";
        case AuthProvider::Discord: return "Discord";
        case AuthProvider::GitHub:  return "GitHub";
    }
    return "Google";
}

bool AccountInfo::hasBadge(const char* name) const {
    for (const auto& b : badges) if (b == name) return true;
    return false;
}

std::string AccountInfo::banRemaining(long long nowUnix) const {
    if (!banned) return {};
    // A ban with no end says so plainly. "Indefinitely" would be a weasel word
    // for the same thing, and a player is owed the real answer.
    if (banUntil <= 0) return "permanent";

    long long left = banUntil - nowUnix;
    if (left <= 0) return "expired";
    if (left < 3600)  return std::to_string(left / 60 + 1) + " minutes";
    if (left < 86400) return std::to_string(left / 3600 + 1) + " hours";
    const long long days = left / 86400 + 1;
    return std::to_string(days) + (days == 1 ? " day" : " days");
}

// ------------------------------------------------------------------ impl ----

struct AccountClient::Impl {
    mutable std::mutex mutex;

    std::string issuer;
    std::string tokenPath;
    bool        inited = false;

    // Guarded by mutex; read by the game thread through the accessors.
    Status      status = Status::SignedOut;
    AccountInfo account;
    std::string message;
    bool        messageError = false;
    std::string verifyUrl;
    std::vector<std::string> deletionSummary;

    std::vector<AuthProvider> providers;
    std::vector<AuthProvider> linkOnly;
    bool serviceReachable = false;
    bool probed = false;

    std::string token;            // session token; never leaves this process
    std::string signupTicket;     // set between sign-in and account creation
    std::string deleteConfirm;

    std::atomic<bool> busy{false};
    std::atomic<bool> cancelRequested{false};
    std::thread       worker;

    // Results are applied here, on the game thread, in update().
    std::mutex                         resultMutex;
    std::vector<std::function<void()>> results;

    void post(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(resultMutex);
        results.push_back(std::move(fn));
    }

    void setMessage(const std::string& text, bool isError) {
        std::lock_guard<std::mutex> lock(mutex);
        message = text;
        messageError = isError;
    }

    std::string issuerCopy() const {
        std::lock_guard<std::mutex> lock(mutex);
        return issuer;
    }

    std::string tokenCopy() const {
        std::lock_guard<std::mutex> lock(mutex);
        return token;
    }

    bool localIssuer() const {
        const std::string i = issuerCopy();
        return i.rfind("http://localhost", 0) == 0 ||
               i.rfind("http://127.0.0.1", 0) == 0;
    }

    HttpRequest baseRequest(const char* method, const char* path, bool withToken) const {
        HttpRequest r;
        r.method = method;
        r.url = issuerCopy() + path;
        // Only ever true for a localhost issuer, which is `wrangler dev`.
        r.allowInsecure = localIssuer();
        if (withToken) r.bearer = tokenCopy();
        return r;
    }

    void saveToken(const std::string& value);
    std::string loadToken() const;
    void clearToken();

    void applyAccountJson(const std::string& json);
    void probeInto();
    void runJob(std::function<void()> job);
    void joinWorker();
};

// ---------------------------------------------------------------- storage ----

void AccountClient::Impl::saveToken(const std::string& value) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex);
        token = value;
        path = tokenPath;
    }
    if (path.empty()) return;

    // Written before the mode is set, so there is a window where the file
    // exists with the default mode. Creating it empty first and chmod-ing
    // before writing closes that.
    {
        std::ofstream create(path, std::ios::binary | std::ios::trunc);
    }
#ifndef _WIN32
    // Other users on this machine must not be able to read it. On Windows the
    // equivalent is a DACL, which is not wired up -- see the header.
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        // Silently failing here means being signed in until the game closes and
        // signed out forever after, with nothing said. Better to sign in and be
        // told the session will not survive a restart.
        setMessage("Signed in, but the session could not be saved to " + path +
                   " -- you will have to sign in again next launch.", true);
        return;
    }
    out << "{\"token\":\"" << httpJsonEscape(value) << "\",\"issuer\":\""
        << httpJsonEscape(issuerCopy()) << "\"}\n";
    out.flush();
    if (!out) {
        setMessage("Signed in, but the session could not be written to " + path +
                   " -- you will have to sign in again next launch.", true);
    }
}

std::string AccountClient::Impl::loadToken() const {
    std::string path;
    std::string wantIssuer;
    {
        std::lock_guard<std::mutex> lock(mutex);
        path = tokenPath;
        wantIssuer = issuer;
    }
    if (path.empty()) return {};

    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (text.size() > kMaxTokenBytes) return {};

    // A token minted by a different issuer is not ours to present. This is
    // what stops a stale file surviving a change of deployment and producing
    // a confusing 401 instead of a clean signed-out state.
    if (httpJsonString(text, "issuer") != wantIssuer) return {};
    return httpJsonString(text, "token", kMaxTokenBytes);
}

void AccountClient::Impl::clearToken() {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex);
        token.clear();
        path = tokenPath;
    }
    if (!path.empty()) std::remove(path.c_str());
}

void AccountClient::Impl::applyAccountJson(const std::string& json) {
    // Scope to the "account" object before reading anything.
    //
    // Replies wrap it differently -- {token, account}, {kind, provider,
    // account} -- and an unscoped read picks up whatever matches first. The
    // link reply is the case that actually broke: its top level contains
    // "kind":"linked", and a search for the `linked` key landed there, so the
    // provider list came back as the BADGES array and the UI never showed the
    // new link.
    const size_t at = httpJsonScope(json, "account");

    AccountInfo info;
    info.id       = httpJsonString(json, "id", 64, at);
    info.nickname = httpJsonString(json, "nickname", 64, at);
    info.created  = httpJsonNumber(json, "created", 0, at);
    info.badges   = httpJsonStringArray(json, "badges", 8, at);
    info.linked   = httpJsonStringArray(json, "linked", 8, at);

    // The ban block is nested one deeper again, so scope to it in turn.
    const size_t banAt = httpJsonScope(json, "banned");
    if (banAt > at) {
        info.banned    = true;
        info.banReason = httpJsonString(json, "reason", 256, banAt);
        info.banUntil  = httpJsonNumber(json, "until", 0, banAt);
    }

    // A reply that carried no account at all must not blank the one we have --
    // that is how a transient error would look like being signed out.
    if (info.id.empty() && info.nickname.empty()) return;

    std::lock_guard<std::mutex> lock(mutex);
    account = info;
}

// Asks the service what it offers. Runs INSIDE a job, never starting one, so
// it can be composed with other work on the same thread.
void AccountClient::Impl::probeInto() {
    const HttpResponse res = httpRequest(baseRequest("GET", "/", false));
    if (!res.ok()) {
        std::lock_guard<std::mutex> lock(mutex);
        serviceReachable = false;
        return;
    }
    // The reply is a small, flat, known object of ours. Looking for the exact
    // id literals beats a parser for two lines of gain.
    std::vector<AuthProvider> found;
    std::vector<AuthProvider> onlyLink;
    const std::pair<const char*, AuthProvider> known[] = {
        {"\"id\":\"google\"",  AuthProvider::Google},
        {"\"id\":\"discord\"", AuthProvider::Discord},
        {"\"id\":\"github\"",  AuthProvider::GitHub},
    };
    for (const auto& [needle, provider] : known) {
        const size_t at = res.body.find(needle);
        if (at == std::string::npos) continue;
        found.push_back(provider);
        const size_t end = res.body.find('}', at);
        const size_t flag = res.body.find("\"canCreate\":false", at);
        if (flag != std::string::npos && end != std::string::npos && flag < end) {
            onlyLink.push_back(provider);
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    providers = found;
    linkOnly = onlyLink;
    serviceReachable = true;
    probed = true;
}

void AccountClient::Impl::joinWorker() {
    if (worker.joinable()) worker.join();
}

void AccountClient::Impl::runJob(std::function<void()> job) {
    joinWorker();
    busy.store(true);
    cancelRequested.store(false);

    // In a browser, inline. The emscripten build is single-threaded and built
    // without exceptions, so std::thread there aborts the tab rather than
    // failing -- and every job routed through here ends up in httpRequest(),
    // which on emscripten is a stub returning "signing in from the web build
    // is not supported yet" without a network round trip. So there is nothing
    // to move off the frame, and the player gets the sentence instead of a
    // dead tab the moment they open the Account screen.
    //
    // The day web HTTP is real this must go back on something asynchronous;
    // a live request run inline here would freeze the page for its duration.
#ifdef __EMSCRIPTEN__
    job();
    busy.store(false);
#else
    worker = std::thread([this, job = std::move(job)] {
        job();
        busy.store(false);
    });
#endif
}

// ------------------------------------------------------------------- api ----

AccountClient::AccountClient() : m_impl(std::make_unique<Impl>()) {}
AccountClient::~AccountClient() { shutdown(); }

AccountClient& AccountClient::get() {
    static AccountClient instance;
    return instance;
}

void AccountClient::init(const std::string& issuer, const std::string& tokenPath) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->inited) return;
    m_impl->issuer = issuer;
    // A trailing slash would produce "//account/me", which some routers treat
    // as a different path.
    while (!m_impl->issuer.empty() && m_impl->issuer.back() == '/') m_impl->issuer.pop_back();
    m_impl->tokenPath = tokenPath;
    m_impl->inited = true;
}

bool AccountClient::configured() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return !m_impl->issuer.empty();
}

std::vector<AuthProvider> AccountClient::providers() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->providers;
}

std::string AccountClient::privacyUrl() const {
    const std::string i = m_impl->issuerCopy();
    return i.empty() ? std::string() : i + "/privacy";
}

std::string AccountClient::termsUrl() const {
    const std::string i = m_impl->issuerCopy();
    return i.empty() ? std::string() : i + "/terms";
}

std::string AccountClient::issuer() const { return m_impl->issuerCopy(); }

std::string AccountClient::sessionToken() const { return m_impl->tokenCopy(); }

bool AccountClient::isLinkOnly(AuthProvider p) const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    for (auto q : m_impl->linkOnly) if (q == p) return true;
    return false;
}

bool AccountClient::serviceReachable() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->serviceReachable;
}

bool AccountClient::probeService() {
    if (m_impl->busy.load() || !configured()) return false;
    m_impl->runJob([impl = m_impl.get()] { impl->probeInto(); });
    return true;
}

AccountClient::Status AccountClient::status() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->status;
}

AccountInfo AccountClient::account() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->account;
}

std::string AccountClient::message() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->message;
}

bool AccountClient::messageIsError() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->messageError;
}

void AccountClient::clearMessage() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->message.clear();
    m_impl->messageError = false;
}

std::string AccountClient::verifyUrl() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->verifyUrl;
}

std::vector<std::string> AccountClient::deletionSummary() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->deletionSummary;
}

void AccountClient::update() {
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(m_impl->resultMutex);
        pending.swap(m_impl->results);
    }
    for (auto& fn : pending) fn();
}

void AccountClient::shutdown() {
    m_impl->cancelRequested.store(true);
    m_impl->joinWorker();
}

// ----------------------------------------------------------------- login ----

bool AccountClient::bootstrap() {
    if (m_impl->busy.load() || !configured()) return false;

    const std::string stored = m_impl->loadToken();
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!stored.empty()) {
            m_impl->token = stored;
            m_impl->status = Status::Working;
        }
    }

    // ONE job for both. They used to be separate calls, and because runJob
    // joins the previous worker, issuing them back to back would have blocked
    // the render thread -- so restoring a session skipped the probe entirely
    // and the provider list stayed empty, which is why no link chips appeared.
    m_impl->runJob([impl = m_impl.get(), hasToken = !stored.empty()] {
        impl->probeInto();
        if (!hasToken) {
            impl->post([impl] {
                std::lock_guard<std::mutex> lock(impl->mutex);
                if (impl->status == Status::Working) impl->status = Status::SignedOut;
            });
            return;
        }

        HttpRequest req = impl->baseRequest("GET", "/account/me", true);
        const HttpResponse res = httpRequest(req);

        if (res.ok()) {
            impl->applyAccountJson(res.body);
            impl->post([impl] {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->status = Status::SignedIn;
            });
            return;
        }
        // 401 means the token expired or the account is gone. Either way the
        // stored copy is useless, and keeping it would make every launch
        // retry a request that cannot succeed.
        if (res.status == 401 || res.status == 404) {
            impl->clearToken();
            impl->post([impl] {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->status = Status::SignedOut;
            });
            return;
        }
        // A network failure is not evidence the token is bad, so it is kept.
        impl->setMessage(res.error.empty() ? "could not reach the account service"
                                           : res.error, true);
        impl->post([impl] {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->status = Status::SignedOut;
        });
    });
    return true;
}

bool AccountClient::beginSignIn(AuthProvider provider) { return beginFlow(provider, false); }

bool AccountClient::beginLink(AuthProvider provider) {
    // Linking needs an account to attach to.
    if (m_impl->tokenCopy().empty()) return false;
    return beginFlow(provider, true);
}

bool AccountClient::beginFlow(AuthProvider provider, bool link) {
    if (m_impl->busy.load() || !configured()) return false;

    const Status resting = link ? Status::SignedIn : Status::SignedOut;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->status = Status::Working;
        m_impl->message.clear();
        m_impl->verifyUrl.clear();
    }

    const std::string providerId = authProviderId(provider);
    const std::string providerLabel = authProviderLabel(provider);

    m_impl->runJob([impl = m_impl.get(), providerId, providerLabel, link, resting] {
        HttpRequest start = impl->baseRequest("POST", "/auth/device", link);
        start.body = std::string("{\"provider\":\"") + providerId +
                     "\",\"purpose\":\"" + (link ? "link" : "login") + "\"}";
        const HttpResponse begun = httpRequest(start);

        if (!begun.ok()) {
            const std::string why = !begun.error.empty() ? begun.error
                : httpJsonString(begun.body, "message");
            impl->setMessage(why.empty() ? "could not start sign-in" : why, true);
            impl->post([impl, resting] {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->status = resting;
            });
            return;
        }

        const std::string pollSecret = httpJsonString(begun.body, "pollSecret", 256);
        const std::string verify = httpJsonString(begun.body, "verifyUrl", 2048);
        if (pollSecret.empty() || verify.empty()) {
            impl->setMessage("the account service sent an unusable reply", true);
            impl->post([impl, resting] {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->status = resting;
            });
            return;
        }
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->verifyUrl = verify;
        }
        impl->post([impl] {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->status = Status::WaitingForBrowser;
        });

        // Poll until the browser half finishes. `pollSecret` never appears in
        // the URL the player visited, so seeing that URL does not let anyone
        // else collect this result.
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kLoginTimeoutMs);
        const std::string pollBody =
            "{\"pollSecret\":\"" + httpJsonEscape(pollSecret) + "\"}";

        while (!impl->cancelRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
            if (impl->cancelRequested.load()) break;
            if (std::chrono::steady_clock::now() > deadline) {
                impl->setMessage("that timed out. Try again.", true);
                impl->post([impl, resting] {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->status = resting;
                });
                return;
            }

            HttpRequest poll = impl->baseRequest("POST", "/auth/poll", false);
            poll.body = pollBody;
            const HttpResponse res = httpRequest(poll);
            if (!res.ok()) continue;   // transient; the deadline is the backstop

            const std::string state = httpJsonString(res.body, "status", 32);
            if (state == "pending") continue;

            if (state == "error") {
                const std::string why = httpJsonString(res.body, "message", 512);
                impl->setMessage(why.empty() ? "sign-in failed" : why, true);
                impl->post([impl, resting] {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->status = resting;
                });
                return;
            }

            const std::string kind = httpJsonString(res.body, "kind", 32);
            if (kind == "session") {
                const std::string token = httpJsonString(res.body, "token", 4096);
                if (token.empty()) break;
                impl->saveToken(token);
                impl->applyAccountJson(res.body);
                impl->post([impl] {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->status = Status::SignedIn;
                });
                return;
            }
            if (kind == "linked") {
                impl->applyAccountJson(res.body);
                impl->setMessage(providerLabel + " linked to your account.", false);
                impl->post([impl] {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->status = Status::SignedIn;
                });
                return;
            }
            if (kind == "signup") {
                const std::string ticket = httpJsonString(res.body, "ticket", 4096);
                const std::string suggested = httpJsonString(res.body, "suggested", 64);
                {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->signupTicket = ticket;
                    // Offered as a starting point only; nothing is stored
                    // unless the player keeps it.
                    impl->account.nickname = suggested;
                }
                impl->post([impl] {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->status = Status::NeedsNickname;
                });
                return;
            }
            break;
        }

        impl->post([impl, resting] {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->status = resting;
        });
    });
    return true;
}

bool AccountClient::unlink(AuthProvider provider) {
    if (m_impl->busy.load() || m_impl->tokenCopy().empty()) return false;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->status = Status::Working;
        m_impl->message.clear();
    }
    const std::string providerId = authProviderId(provider);
    const std::string label = authProviderLabel(provider);

    m_impl->runJob([impl = m_impl.get(), providerId, label] {
        HttpRequest req = impl->baseRequest("POST", "/account/unlink", true);
        req.body = "{\"provider\":\"" + providerId + "\"}";
        const HttpResponse res = httpRequest(req);

        if (res.ok()) {
            impl->applyAccountJson(res.body);
            impl->setMessage(label + " unlinked.", false);
        } else {
            const std::string why = !res.error.empty() ? res.error
                : httpJsonString(res.body, "message", 512);
            impl->setMessage(why.empty() ? "could not unlink that provider" : why, true);
        }
        impl->post([impl] {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->status = Status::SignedIn;
        });
    });
    return true;
}

void AccountClient::cancelSignIn() {
    m_impl->cancelRequested.store(true);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->status == Status::WaitingForBrowser) m_impl->status = Status::SignedOut;
}

bool AccountClient::createAccount(const std::string& nickname) {
    if (m_impl->busy.load()) return false;

    std::string ticket;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        ticket = m_impl->signupTicket;
        if (ticket.empty()) return false;
        m_impl->status = Status::Working;
        m_impl->message.clear();
    }

    m_impl->runJob([impl = m_impl.get(), ticket, nickname] {
        HttpRequest req = impl->baseRequest("POST", "/account/create", false);
        req.body = "{\"signupTicket\":\"" + httpJsonEscape(ticket) +
                   "\",\"nickname\":\"" + httpJsonEscape(nickname) + "\"}";
        const HttpResponse res = httpRequest(req);

        if (res.ok()) {
            impl->saveToken(httpJsonString(res.body, "token", 4096));
            impl->applyAccountJson(res.body);
            {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->signupTicket.clear();
            }
            impl->setMessage("Account created.", false);
            impl->post([impl] {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->status = Status::SignedIn;
            });
            return;
        }

        const std::string why = !res.error.empty() ? res.error
            : httpJsonString(res.body, "message", 512);
        impl->setMessage(why.empty() ? "that nickname could not be used" : why, true);
        // An expired signup ticket means starting over; anything else is worth
        // another try with a different name.
        const bool expired = res.status == 401;
        impl->post([impl, expired] {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (expired) impl->signupTicket.clear();
            impl->status = expired ? Status::SignedOut : Status::NeedsNickname;
        });
    });
    return true;
}

bool AccountClient::changeNickname(const std::string& nickname) {
    if (m_impl->busy.load() || m_impl->tokenCopy().empty()) return false;

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->status = Status::Working;
        m_impl->message.clear();
    }

    m_impl->runJob([impl = m_impl.get(), nickname] {
        HttpRequest req = impl->baseRequest("POST", "/account/nickname", true);
        req.body = "{\"nickname\":\"" + httpJsonEscape(nickname) + "\"}";
        const HttpResponse res = httpRequest(req);

        if (res.ok()) {
            impl->applyAccountJson(res.body);
            impl->setMessage("Nickname changed.", false);
        } else {
            const std::string why = !res.error.empty() ? res.error
                : httpJsonString(res.body, "message", 512);
            impl->setMessage(why.empty() ? "that nickname could not be used" : why, true);
        }
        impl->post([impl] {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->status = Status::SignedIn;
        });
    });
    return true;
}

// ---------------------------------------------------------------- delete ----

bool AccountClient::beginDelete() {
    if (m_impl->busy.load() || m_impl->tokenCopy().empty()) return false;

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->status = Status::Working;
        m_impl->message.clear();
        m_impl->deletionSummary.clear();
    }

    m_impl->runJob([impl = m_impl.get()] {
        // No body: step one asks what would happen, and returns the token that
        // spends it. Nothing is deleted here.
        HttpRequest req = impl->baseRequest("POST", "/account/delete", true);
        req.body = "{}";
        const HttpResponse res = httpRequest(req);

        if (!res.ok()) {
            const std::string why = !res.error.empty() ? res.error
                : httpJsonString(res.body, "message", 512);
            impl->setMessage(why.empty() ? "could not reach the account service" : why, true);
            impl->post([impl] {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->status = Status::SignedIn;
            });
            return;
        }

        const std::string confirm = httpJsonString(res.body, "confirmation", 4096);
        std::vector<std::string> lines;
        // The server describes what goes, what lingers and what it cannot
        // reach. Shown verbatim rather than paraphrased here, so the two
        // cannot drift apart.
        for (const char* key : {"willDelete", "willKeep", "cannotReach"}) {
            const std::string needle = std::string("\"") + key + "\"";
            size_t at = res.body.find(needle);
            if (at == std::string::npos) continue;
            at = res.body.find('[', at);
            const size_t end = at == std::string::npos ? std::string::npos
                                                       : res.body.find(']', at);
            if (end == std::string::npos) continue;
            for (size_t i = at; i < end && lines.size() < 12; ) {
                const size_t open = res.body.find('"', i);
                if (open == std::string::npos || open > end) break;
                size_t close = open + 1;
                while (close < end && !(res.body[close] == '"' && res.body[close - 1] != '\\')) close++;
                if (close >= end) break;
                lines.push_back(res.body.substr(open + 1, close - open - 1));
                i = close + 1;
            }
        }

        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->deleteConfirm = confirm;
            impl->deletionSummary = lines;
        }
        impl->post([impl] {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->status = impl->deleteConfirm.empty() ? Status::SignedIn
                                                       : Status::DeleteConfirm;
        });
    });
    return true;
}

bool AccountClient::confirmDelete() {
    if (m_impl->busy.load()) return false;

    std::string confirm;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        confirm = m_impl->deleteConfirm;
        if (confirm.empty()) return false;
        m_impl->status = Status::Working;
    }

    m_impl->runJob([impl = m_impl.get(), confirm] {
        HttpRequest req = impl->baseRequest("POST", "/account/delete", true);
        req.body = "{\"confirm\":\"" + httpJsonEscape(confirm) + "\"}";
        const HttpResponse res = httpRequest(req);

        if (res.ok()) {
            impl->clearToken();
            impl->setMessage("Your account has been deleted.", false);
            impl->post([impl] {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->account = AccountInfo{};
                impl->deleteConfirm.clear();
                impl->deletionSummary.clear();
                impl->status = Status::SignedOut;
            });
            return;
        }

        const std::string why = !res.error.empty() ? res.error
            : httpJsonString(res.body, "message", 512);
        impl->setMessage(why.empty() ? "the account could not be deleted" : why, true);
        impl->post([impl] {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->deleteConfirm.clear();
            impl->status = Status::SignedIn;
        });
    });
    return true;
}

void AccountClient::cancelDelete() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->deleteConfirm.clear();
    m_impl->deletionSummary.clear();
    if (m_impl->status == Status::DeleteConfirm) m_impl->status = Status::SignedIn;
}

void AccountClient::signOut() {
    m_impl->cancelRequested.store(true);
    m_impl->clearToken();
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->account = AccountInfo{};
    m_impl->signupTicket.clear();
    m_impl->deleteConfirm.clear();
    m_impl->deletionSummary.clear();
    m_impl->status = Status::SignedOut;
    m_impl->message.clear();
}

// ------------------------------------------------------------- validation ----

bool AccountClient::nicknameLooksValid(const std::string& nickname, std::string& why) {
    // Code points, not bytes: a multi-byte character counts once, and it will
    // fail the charset rule below anyway with a message that makes sense.
    int points = 0;
    for (unsigned char c : nickname) if ((c & 0xC0) != 0x80) points++;

    if (points < kNickMin) { why = "At least 3 characters."; return false; }
    if (points > kNickMax) { why = "At most 24 characters."; return false; }

    bool hasLetter = false;
    for (size_t i = 0; i < nickname.size(); i++) {
        const char c = nickname[i];
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9');
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) hasLetter = true;
        if (!alnum && !isSeparator(c)) {
            why = "Letters, numbers, and single spaces, dots, hyphens or underscores.";
            return false;
        }
        if (isSeparator(c)) {
            if (i == 0 || i + 1 == nickname.size()) {
                why = "Cannot start or end with a space, dot, hyphen or underscore.";
                return false;
            }
            if (isSeparator(nickname[i - 1])) {
                why = "Only one separator at a time.";
                return false;
            }
        }
    }
    if (!hasLetter) { why = "Needs at least one letter."; return false; }

    why.clear();
    return true;
}
