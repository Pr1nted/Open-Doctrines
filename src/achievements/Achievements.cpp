#include "achievements/Achievements.h"

#include "achievements/AchievementCatalog.gen.h"
#include "achievements/GrantVerify.h"
#include "net/AccountClient.h"
#include "net/HttpClient.h"
#include "platform/SteamBridge.h"
#include "json.hpp"


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace odach {
namespace {

long long nowUnix() { return (long long)std::time(nullptr); }

const Def* findDef(const std::string& id) {
    for (int i = 0; i < kCatalogCount; ++i)
        if (id == kCatalog[i].id) return &kCatalog[i];
    return nullptr;
}

bool readFile(const fs::path& p, std::string& out) {
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

// Write-then-rename, so a crash mid-write leaves the previous file rather than
// half of one. progress.json is years of counters for a dedicated player.
bool writeFileAtomic(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) { fs::remove(tmp, ec); return false; }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(p, ec); fs::rename(tmp, p, ec); }
    return !ec;
}

struct Grant {
    std::string token;
    std::string sub;
    long long iat = 0;
    bool modded = false;
};

}  // namespace

struct Tracker::Impl {
    mutable std::mutex mutex;
    bool ready = false;
    std::string dataDir;
    std::string build;
    std::string seal;

    // progress.json
    std::map<std::string, double> counters;              // cumulative
    std::map<std::string, std::set<std::string>> sets;   // distinct members
    std::map<std::string, long long> earned;             // id -> when the game saw it
    // measured now, never written: recomputed every turn
    std::unordered_map<std::string, double> values;

    // grants.json: by achievement id. More than one account's grants can be
    // present (a shared machine, an imported .odstate); entries() shows the
    // signed-in account's, or every valid one when nobody is signed in.
    std::multimap<std::string, Grant> grants;

    std::deque<Toast> toasts;
    bool dirty = false;

    // sync state
    std::atomic<bool> busy{false};
    std::thread worker;
    long long nextSyncAt = 0;
    std::string play;               // current play ticket
    long long playAt = 0;
    std::string playAccount;
    std::string status;
    std::map<std::string, long long> retryAt;   // id -> earliest next claim

    ~Impl() { if (worker.joinable()) worker.join(); }

    fs::path progressPath() const { return fs::path(dataDir) / "achievements" / "progress.json"; }
    fs::path grantsPath() const { return fs::path(dataDir) / "achievements" / "grants.json"; }

    double statValue(const std::string& s) const {
        auto v = values.find(s);
        if (v != values.end()) return v->second;
        auto c = counters.find(s);
        if (c != counters.end()) return c->second;
        auto st = sets.find(s);
        if (st != sets.end()) return (double)st->second.size();
        return 0;
    }

    void loadProgress() {
        std::string text;
        if (!readFile(progressPath(), text)) return;
        json j = json::parse(text, nullptr, false);
        if (!j.is_object()) return;
        if (j.contains("counters") && j["counters"].is_object())
            for (auto& [k, v] : j["counters"].items()) if (v.is_number()) counters[k] = v.get<double>();
        if (j.contains("sets") && j["sets"].is_object())
            for (auto& [k, v] : j["sets"].items())
                if (v.is_array()) for (auto& m : v) if (m.is_string()) sets[k].insert(m.get<std::string>());
        if (j.contains("earned") && j["earned"].is_object())
            for (auto& [k, v] : j["earned"].items())
                if (v.is_number_integer() && findDef(k)) earned[k] = v.get<long long>();
    }

    void saveProgress() {
        json j;
        j["counters"] = json::object();
        for (auto& [k, v] : counters) j["counters"][k] = v;
        j["sets"] = json::object();
        for (auto& [k, v] : sets) j["sets"][k] = std::vector<std::string>(v.begin(), v.end());
        j["earned"] = json::object();
        for (auto& [k, v] : earned) j["earned"][k] = v;
        j["catalog"] = kCatalogHash;
        writeFileAtomic(progressPath(), j.dump(1));
    }

    bool addGrantLocked(const std::string& token) {
        std::string sub, ach;
        bool modded = false;
        long long iat = 0;
        if (!Tracker::verifyGrant(token, issuer(), &sub, &ach, &modded, &iat)) return false;
        auto range = grants.equal_range(ach);
        for (auto it = range.first; it != range.second; ++it)
            if (it->second.sub == sub) return false;          // already held
        grants.emplace(ach, Grant{token, sub, iat, modded});
        return true;
    }

    void loadGrants() {
        std::string text;
        if (!readFile(grantsPath(), text)) return;
        json j = json::parse(text, nullptr, false);
        if (!j.is_object() || !j.contains("grants") || !j["grants"].is_array()) return;
        for (auto& t : j["grants"]) if (t.is_string()) addGrantLocked(t.get<std::string>());
    }

    void saveGrants() {
        json j;
        j["format"] = "od-achievement-grants-1";
        j["grants"] = json::array();
        for (auto& [ach, g] : grants) j["grants"].push_back(g.token);
        writeFileAtomic(grantsPath(), j.dump(1));
    }

    /**
     * Grants that arrived in a .odstate. OdState::load writes them beside the
     * real file instead of over it (see the note there); here each one is
     * verified and the survivors join the collection. The file is deleted
     * either way -- what did not verify now will not verify later.
     */
    void absorbImportsLocked() {
        const fs::path imp = fs::path(dataDir) / "achievements" / "grants.import.json";
        std::error_code ec;
        if (!fs::exists(imp, ec)) return;
        std::string text;
        if (readFile(imp, text)) {
            json j = json::parse(text, nullptr, false);
            int added = 0;
            if (j.is_object() && j.contains("grants") && j["grants"].is_array())
                for (auto& t : j["grants"]) if (t.is_string() && addGrantLocked(t.get<std::string>())) ++added;
            if (added) saveGrants();
            std::fprintf(stderr, "achievements: imported %d verified grant(s)\n", added);
        }
        fs::remove(imp, ec);
    }

    static std::string issuer() {
        std::string i = AccountClient::get().issuer();
        while (!i.empty() && i.back() == '/') i.pop_back();
        return i;
    }

    static std::string currentAccount() {
        auto& ac = AccountClient::get();
        if (ac.status() != AccountClient::Status::SignedIn) return {};
        return ac.account().id;
    }

    bool grantedFor(const std::string& ach, const std::string& account) const {
        auto range = grants.equal_range(ach);
        for (auto it = range.first; it != range.second; ++it)
            if (account.empty() || it->second.sub == account) return true;
        return false;
    }

    // ---- the network half. Runs off the game thread on desktop. ----

    void syncOnce();
};

Tracker& Tracker::get() {
    static Tracker t;
    return t;
}

Tracker::Tracker() : m_impl(std::make_unique<Impl>()) {}

bool Tracker::keysBaked() { return !grantPublicKeys().empty(); }

bool Tracker::verifyGrant(const std::string& token, const std::string& issuer,
                          std::string* sub, std::string* ach, bool* modded, long long* iat) {
    GrantFields f;
    if (!verifyGrantToken(token, issuer, grantPublicKeys(), f)) return false;
    if (!findDef(f.ach)) return false;   // a grant for an achievement this build does not know
    if (sub) *sub = f.sub;
    if (ach) *ach = f.ach;
    if (modded) *modded = f.modded;
    if (iat) *iat = f.iat;
    return true;
}

void Tracker::init(const std::string& dataDir, const std::string& build, unsigned long long seal) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->ready) return;
    m_impl->dataDir = dataDir;
    m_impl->build = build;
    char buf[24];
    std::snprintf(buf, sizeof buf, "%llx", seal);
    m_impl->seal = buf;
    m_impl->loadProgress();
    m_impl->loadGrants();
    m_impl->absorbImportsLocked();
    m_impl->ready = true;
    m_impl->nextSyncAt = nowUnix() + 5;
}

bool Tracker::initialised() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->ready;
}

void Tracker::add(const char* stat, double n) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->ready || n == 0) return;
    m_impl->counters[stat] += n;
    m_impl->dirty = true;
}

void Tracker::set(const char* stat, double v) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->ready) return;
    m_impl->values[stat] = v;
}

void Tracker::addToSet(const char* stat, const std::string& member) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->ready || member.empty()) return;
    if (m_impl->sets[stat].insert(member).second) m_impl->dirty = true;
}

void Tracker::resetWorld() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->values.clear();
}

void Tracker::evaluate() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->ready) return;
    Impl& im = *m_impl;
    // Earned count feeds the completionist row, so it is measured first and
    // the loop runs twice: a batch that crosses 75 earns the 76th in one go.
    for (int pass = 0; pass < 2; ++pass) {
        int earnedOthers = 0;
        for (int i = 0; i < kCatalogCount; ++i)
            if (std::strcmp(kCatalog[i].stat, "achievements_earned") != 0 && im.earned.count(kCatalog[i].id))
                ++earnedOthers;
        im.values["achievements_earned"] = earnedOthers;

        for (int i = 0; i < kCatalogCount; ++i) {
            const Def& d = kCatalog[i];
            if (im.earned.count(d.id)) continue;
            if (im.statValue(d.stat) < d.gte) continue;
            // Prerequisites are the service's rule too; checking them here
            // keeps a claim from being made that is certain to be refused.
            bool reqOk = true;
            std::string req = d.prereqs;
            size_t s = 0;
            while (!req.empty() && s <= req.size()) {
                size_t c = req.find(',', s);
                std::string r = req.substr(s, c == std::string::npos ? std::string::npos : c - s);
                if (!r.empty() && !im.earned.count(r)) { reqOk = false; break; }
                if (c == std::string::npos) break;
                s = c + 1;
            }
            if (!reqOk) continue;
            im.earned[d.id] = nowUnix();
            im.toasts.push_back({d.id, false});
            im.dirty = true;
            im.nextSyncAt = std::min(im.nextSyncAt, nowUnix() + 2);
        }
    }
    if (im.dirty) {
        im.saveProgress();
        im.dirty = false;
    }
}

void Tracker::syncSoon() {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->nextSyncAt = 0;
}

std::string Tracker::syncStatus() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->status;
}

int Tracker::grantedCount() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const std::string acct = Impl::currentAccount();
    int n = 0;
    for (int i = 0; i < kCatalogCount; ++i) if (m_impl->grantedFor(kCatalog[i].id, acct)) ++n;
    return n;
}

int Tracker::earnedCount() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return (int)m_impl->earned.size();
}

std::vector<Entry> Tracker::entries() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const std::string acct = Impl::currentAccount();
    std::vector<Entry> out;
    out.reserve(kCatalogCount);
    for (int i = 0; i < kCatalogCount; ++i) {
        Entry e;
        e.def = &kCatalog[i];
        auto er = m_impl->earned.find(e.def->id);
        e.earned = er != m_impl->earned.end();
        if (e.earned) e.when = er->second;
        auto range = m_impl->grants.equal_range(e.def->id);
        for (auto it = range.first; it != range.second; ++it) {
            if (!acct.empty() && it->second.sub != acct) continue;
            e.granted = true;
            e.modded = it->second.modded;
            e.when = it->second.iat;
            break;
        }
        e.progress = m_impl->statValue(e.def->stat);
        out.push_back(e);
    }
    return out;
}

bool Tracker::popToast(Toast& out) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->toasts.empty()) return false;
    out = m_impl->toasts.front();
    m_impl->toasts.pop_front();
    return true;
}

int Tracker::mergeGrantsFrom(const std::string& otherGrantsJson, int* rejected) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    int added = 0, bad = 0;
    json j = json::parse(otherGrantsJson, nullptr, false);
    if (j.is_object() && j.contains("grants") && j["grants"].is_array()) {
        for (auto& t : j["grants"]) {
            if (!t.is_string()) { ++bad; continue; }
            const std::string tok = t.get<std::string>();
            std::string sub, ach;
            if (!Tracker::verifyGrant(tok, Impl::issuer(), &sub, &ach, nullptr, nullptr)) { ++bad; continue; }
            if (m_impl->addGrantLocked(tok)) ++added;
        }
    } else {
        bad = 1;
    }
    if (added && m_impl->ready) m_impl->saveGrants();
    if (rejected) *rejected = bad;
    return added;
}

// --------------------------------------------------------------------- sync

void Tracker::Impl::syncOnce() {
    auto& ac = AccountClient::get();
    std::string token, account, iss;
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(mutex);
        token = ac.sessionToken();
        account = currentAccount();
        iss = issuer();
        if (account.empty() || token.empty() || iss.empty()) {
            status = earned.size() > 0 && grants.empty() ? "sign_in" : "";
            return;
        }
        if (!keysBaked()) {
            status = "no_keys";
            return;
        }
        const long long now = nowUnix();
        for (auto& [id, when] : earned) {
            if (grantedFor(id, account)) continue;
            auto r = retryAt.find(id);
            if (r != retryAt.end() && r->second > now) continue;
            pending.push_back(id);
        }
    }

    // Grants earned on other devices come down first, so a claim is never
    // spent on something already held.
    {
        HttpRequest req;
        req.method = "GET";
        req.url = iss + "/achievements/mine";
        req.bearer = token;
        HttpResponse res = httpRequest(req);
        if (res.ok()) {
            json j = json::parse(res.body, nullptr, false);
            if (j.is_object() && j.contains("grants") && j["grants"].is_array()) {
                std::vector<std::string> fresh;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    for (auto& t : j["grants"]) {
                        if (!t.is_string()) continue;
                        const std::string tok = t.get<std::string>();
                        std::string ach;
                        if (!Tracker::verifyGrant(tok, iss, nullptr, &ach, nullptr, nullptr)) continue;
                        if (addGrantLocked(tok)) {
                            fresh.push_back(ach);
                            // Earned on another device: it is ours here too,
                            // and must not be claimed again.
                            if (!earned.count(ach)) earned[ach] = nowUnix();
                        }
                    }
                    if (!fresh.empty()) { saveGrants(); saveProgress(); }
                }
                for (auto& a : fresh) if (const Def* d = findDef(a)) odsteam::setAchievement(d->steam);
            }
        } else if (res.status == 503) {
            std::lock_guard<std::mutex> lock(mutex);
            status = "not_issuing";
            return;
        }
    }
    if (pending.empty()) {
        std::lock_guard<std::mutex> lock(mutex);
        status.clear();
        // Steam may have missed a grant held from before Steam was running.
        for (auto& [ach, g] : grants)
            if (g.sub == account) if (const Def* d = findDef(ach)) odsteam::setAchievement(d->steam);
        return;
    }

    // A play ticket per account per 30 hours: the service's clock for this
    // sitting. Started when the first claim needs one, which is close enough
    // to the session's real start for the session-length rules, because the
    // ticket is fetched at startup too (Tracker::update's first call).
    std::string play;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (playAccount == account && nowUnix() - playAt < 30 * 3600) play = this->play;
    }
    if (play.empty()) {
        HttpRequest req;
        req.method = "POST";
        req.url = iss + "/achievements/session";
        req.bearer = token;
        json body = {{"seal", seal}, {"build", build}};
        req.body = body.dump();
        HttpResponse res = httpRequest(req);
        if (!res.ok()) {
            std::lock_guard<std::mutex> lock(mutex);
            status = "unreachable";
            return;
        }
        play = httpJsonString(res.body, "play", 4096);
        std::lock_guard<std::mutex> lock(mutex);
        this->play = play;
        playAt = nowUnix();
        playAccount = account;
    }

    // Batches of the service's maximum.
    for (size_t at = 0; at < pending.size(); at += 24) {
        json claims = json::array();
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (size_t i = at; i < pending.size() && i < at + 24; ++i) {
                const Def* d = findDef(pending[i]);
                json ev = {{"stat", d ? d->stat : ""}, {"value", statValue(d ? d->stat : "")},
                           {"earned", earned[pending[i]]}, {"build", build}, {"catalog", kCatalogHash}};
                claims.push_back({{"ach", pending[i]}, {"ev", ev.dump()}});
            }
        }
        HttpRequest req;
        req.method = "POST";
        req.url = iss + "/achievements/claim";
        req.bearer = token;
        req.body = json{{"play", play}, {"claims", claims}}.dump();
        HttpResponse res = httpRequest(req);
        if (res.status == 401) {
            std::lock_guard<std::mutex> lock(mutex);
            this->play.clear();     // a stale play ticket; the next pass starts another
            return;
        }
        if (!res.ok()) {
            std::lock_guard<std::mutex> lock(mutex);
            status = "unreachable";
            return;
        }
        json j = json::parse(res.body, nullptr, false);
        if (!j.is_object() || !j.contains("decisions") || !j["decisions"].is_array()) return;
        std::vector<const Def*> newlyGranted;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const long long now = nowUnix();
            for (auto& d : j["decisions"]) {
                const std::string ach = d.value("ach", "");
                if (d.contains("token") && d["token"].is_string()) {
                    if (addGrantLocked(d["token"].get<std::string>()))
                        if (const Def* def = findDef(ach)) { newlyGranted.push_back(def); toasts.push_back({ach, true}); }
                    retryAt.erase(ach);
                } else if (d.contains("retryAfter") && d["retryAfter"].is_number()) {
                    retryAt[ach] = now + std::max<long long>(30, d["retryAfter"].get<long long>());
                } else if (d.contains("refused")) {
                    // "requires:" settles itself once the tier below is
                    // granted; anything else will never succeed as sent.
                    const std::string why = d.value("refused", "");
                    retryAt[ach] = now + (why.rfind("requires:", 0) == 0 ? 300 : 24 * 3600);
                }
            }
            if (!newlyGranted.empty()) saveGrants();
        }
        for (const Def* d : newlyGranted) odsteam::setAchievement(d->steam);
    }

    std::lock_guard<std::mutex> lock(mutex);
    int waiting = 0;
    long long soonest = 0;
    for (auto& [id, when] : earned) {
        if (grantedFor(id, account)) continue;
        ++waiting;
        auto r = retryAt.find(id);
        if (r != retryAt.end() && (soonest == 0 || r->second < soonest)) soonest = r->second;
    }
    if (waiting == 0) status.clear();
    else status = "waiting:" + std::to_string(waiting);
    if (soonest) nextSyncAt = std::max(nextSyncAt, soonest);
}

void Tracker::update() {
    Impl& im = *m_impl;
    odsteam::runCallbacks();
    {
        std::lock_guard<std::mutex> lock(im.mutex);
        if (!im.ready || im.busy.load()) return;
        const long long now = nowUnix();
        if (now < im.nextSyncAt) return;
        im.absorbImportsLocked();
        im.nextSyncAt = now + 120;
    }
    if (AccountClient::get().status() != AccountClient::Status::SignedIn) return;
    im.busy = true;
#ifdef __EMSCRIPTEN__
    im.syncOnce();
    im.busy = false;
#else
    if (im.worker.joinable()) im.worker.join();
    im.worker = std::thread([&im] {
        im.syncOnce();
        im.busy = false;
    });
#endif
}

}  // namespace odach
