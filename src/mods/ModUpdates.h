#pragma once

// "Can be updated" — asking a mod's own updateUrl whether a newer version
// exists.
//
// GATED, ALWAYS. Nothing here runs unless config.modUpdateChecks is on, which
// is off by default. This is the only outbound request the game makes, and it
// goes to a URL a mod author controls: enabling it tells that author the player
// runs their mod, roughly when, and from which IP. That is a disclosure the
// player makes, not one made for them.
//
// LOOKS, NEVER TOUCHES. There is no download and no install. A newer version
// turns on a button that opens the author's page in a browser, and the player
// installs it the same way they installed the mod. Auto-updating a mod would
// mean the game fetching and running code chosen by a third party -- exactly
// what the whole capability sandbox exists to avoid.
//
// WHY curl RATHER THAN A LINKED HTTP LIBRARY
//
// The response is untrusted input from an author-controlled server. Running the
// fetch in a separate process means a malformed or hostile response cannot
// corrupt the game's memory -- the worst case is a subprocess that fails and a
// mod that shows no update. It also adds no build dependency: curl ships with
// macOS, Linux and Windows 10+.
//
// THE FORMAT a mod author publishes at updateUrl:
//
//     {"version": "1.4.0", "url": "https://example.com/mymod"}
//
// `url` is optional and only changes where the button points; without it the
// button opens updateUrl itself.

#include <atomic>
#include <map>
#include <string>
#include <vector>

class ModManager;

class ModUpdates {
public:
    static ModUpdates& get();

    struct Info {
        std::string latest;      // version reported by the author
        std::string page;        // where to send the player
        bool        newer = false;
        std::string error;       // why the check failed, shown only on request
    };

    // Starts one background check per mod that declares an updateUrl. Returns
    // immediately and does nothing at all when `enabled` is false.
    void checkAll(const ModManager& mm, bool enabled);

    // Nothing until a check has come back. Never blocks.
    const Info* infoFor(const std::string& modId) const;
    bool busy() const { return m_pending.load() > 0; }
    void clear();

    // Public for testing: "is `latest` newer than `have`", by semver.
    static bool isNewer(const std::string& have, const std::string& latest);
    // Public for testing: pull version/url out of the author's JSON without
    // trusting any of it.
    static bool parseResponse(const std::string& body, std::string& version,
                              std::string& page);

private:
    mutable std::atomic<int> m_pending{0};
    std::map<std::string, Info> m_info;   // guarded by the mutex in the .cpp
};
