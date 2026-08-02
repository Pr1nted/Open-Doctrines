#include "Config.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>

// Set by the release build (see CMakeLists.txt). Guarded so this file still
// compiles in a target that does not define it -- an unset build gets "",
// which is the "offer no sign-in" behaviour the comment on accountIssuer
// describes.
#ifndef OD_ACCOUNT_ISSUER
#define OD_ACCOUNT_ISSUER ""
#endif

const std::string& bakedAccountIssuer() {
    static const std::string kIssuer = OD_ACCOUNT_ISSUER;
    return kIssuer;
}

static float findFloat(const std::string& json, const std::string& key, float def) {
    auto pos = json.find('"' + key + '"');
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    pos = json.find_first_of("0123456789.-", pos);
    if (pos == std::string::npos) return def;
    char* end = nullptr;
    float val = std::strtof(json.c_str() + pos, &end);
    return (end == json.c_str() + pos) ? def : val;
}

static int findInt(const std::string& json, const std::string& key, int def) {
    auto pos = json.find('"' + key + '"');
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    pos = json.find_first_of("-0123456789", pos);
    if (pos == std::string::npos) return def;
    char* end = nullptr;
    long val = std::strtol(json.c_str() + pos, &end, 10);
    return (end == json.c_str() + pos) ? def : (int)val;
}

static bool findBool(const std::string& json, const std::string& key, bool def) {
    auto pos = json.find('"' + key + '"');
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    pos = json.find_first_of("tf", pos);
    if (pos == std::string::npos) return def;
    return json[pos] == 't';
}


namespace {

// The config parser is a set of small scanners rather than a JSON library.
// This is the string equivalent; it deliberately does not handle escapes,
// because every value it reads is a URL the user typed into a settings file.
std::string findConfigString(const std::string& json, const char* key,
                             const std::string& fallback) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t at = json.find(needle);
    if (at == std::string::npos) return fallback;
    at = json.find(':', at + needle.size());
    if (at == std::string::npos) return fallback;
    const size_t open = json.find('"', at);
    if (open == std::string::npos) return fallback;
    const size_t close = json.find('"', open + 1);
    if (close == std::string::npos) return fallback;
    return json.substr(open + 1, close - open - 1);
}

}  // namespace

bool Config::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) return false;
    std::stringstream ss;
    ss << file.rdbuf();
    std::string json = ss.str();

    flySpeed = findFloat(json, "flySpeed", 2.0f);
    maxZoom = findFloat(json, "maxZoom", 5.0f);
    screenW = findInt(json, "screenW", 1600);
    screenH = findInt(json, "screenH", 900);
    fullscreen = findBool(json, "fullscreen", false);
    showActualFlags = findBool(json, "showActualFlags", true);
    debugMode = findBool(json, "debugMode", false);
    showFps = findBool(json, "showFps", true);
    showZoom = findBool(json, "showZoom", false);
    showConsole = findBool(json, "showConsole", false);
    fpsTarget = findInt(json, "fpsTarget", 0);
    // Absent means unlimited: an older config predates the limiter and its
    // owner never chose to be throttled.
    resourceBudget = std::clamp(findFloat(json, "resourceBudget", 1.0f), 0.10f, 1.0f);
    aiDifficulty = findInt(json, "aiDifficulty", 1);
    if (aiDifficulty < 0) aiDifficulty = 0;
    if (aiDifficulty > 3) aiDifficulty = 3;
    aiDebug = findBool(json, "aiDebug", false);
    aiLearning = findBool(json, "aiLearning", false);
    masterVolume = std::clamp(findFloat(json, "masterVolume", 0.8f), 0.0f, 1.0f);
    musicVolume  = std::clamp(findFloat(json, "musicVolume",  0.6f), 0.0f, 1.0f);
    sfxVolume    = std::clamp(findFloat(json, "sfxVolume",    0.8f), 0.0f, 1.0f);
    nowPlayingToast = findBool(json, "nowPlayingToast", true);
    mapAtmosphere   = findBool(json, "mapAtmosphere", true);
    // Absent means off: an older config file must not silently opt the
    // player into an outbound request they never agreed to.
    modUpdateChecks = findBool(json, "modUpdateChecks", false);
    gameUpdateChecks = findBool(json, "gameUpdateChecks", true);
    accentColor = findInt(json, "accentColor", 0xFFD700);
    // OD_ACCOUNT_ISSUER is baked in at BUILD time, and is empty unless the
    // person building set it. See the note in Config.h: a source build must not
    // guess at a host, because whoever made it may be running their own and a
    // hardcoded fallback would send their players' logins somewhere else.
    //
    // What that reasoning did not cover is the OFFICIAL build. config.json is
    // user data and is deliberately never packaged, so there was nowhere for a
    // release to carry this -- and every shipped copy offered no sign-in at
    // all, on every platform, telling the player to edit a file that is not
    // there. The release workflow sets this; nothing else does.
    //
    // A config.json value still wins, so a player can point at their own.
    accountIssuer = findConfigString(json, "accountIssuer", bakedAccountIssuer());

    // An EMPTY value in the file means "nobody ever set one", not "this build
    // has no service". save() writes every field on every settings change, so a
    // copy that once ran without a baked-in issuer -- v1.0.4a, or any build
    // made from source before one was set -- has "accountIssuer": "" on disk
    // and would carry that emptiness forward through every later version,
    // sign-in staying unavailable for as long as the file survives.
    if (accountIssuer.empty()) accountIssuer = bakedAccountIssuer();
    serverCredential = findConfigString(json, "serverCredential", "");
    accountAgreed = findBool(json, "accountAgreed", false);

    // Load keybinds
    auto kbPos = json.find("\"keybinds\"");
    if (kbPos != std::string::npos) {
        kbPos = json.find('[', kbPos);
        if (kbPos != std::string::npos) {
            size_t end = json.find(']', kbPos);
            if (end != std::string::npos) {
                std::string arr = json.substr(kbPos + 1, end - kbPos - 1);
                size_t start = 0;
                for (int i = 0; i < ACTION_COUNT; ++i) {
                    while (start < arr.size() && (arr[start] == ' ' || arr[start] == ','))
                        ++start;
                    if (start >= arr.size()) break;
                    char* endp = nullptr;
                    long val = std::strtol(arr.c_str() + start, &endp, 10);
                    if (endp != arr.c_str() + start)
                        keybinds[i] = (int)val;
                    start = endp - arr.c_str();
                }
            }
        }
    }

    return true;
}

bool Config::save(const std::string& path) {
    std::ofstream file(path);
    if (!file) return false;
    file << "{\n";
    file << "  \"flySpeed\": " << flySpeed << ",\n";
    file << "  \"maxZoom\": " << maxZoom << ",\n";
    file << "  \"screenW\": " << screenW << ",\n";
    file << "  \"screenH\": " << screenH << ",\n";
    file << "  \"fullscreen\": " << (fullscreen ? "true" : "false") << ",\n";
    file << "  \"showActualFlags\": " << (showActualFlags ? "true" : "false") << ",\n";
    file << "  \"debugMode\": " << (debugMode ? "true" : "false") << ",\n";
    file << "  \"showFps\": " << (showFps ? "true" : "false") << ",\n";
    file << "  \"showZoom\": " << (showZoom ? "true" : "false") << ",\n";
    file << "  \"showConsole\": " << (showConsole ? "true" : "false") << ",\n";
    file << "  \"fpsTarget\": " << fpsTarget << ",\n";
    file << "  \"resourceBudget\": " << resourceBudget << ",\n";
    file << "  \"aiDifficulty\": " << aiDifficulty << ",\n";
    file << "  \"aiDebug\": " << (aiDebug ? "true" : "false") << ",\n";
    file << "  \"aiLearning\": " << (aiLearning ? "true" : "false") << ",\n";
    file << "  \"masterVolume\": " << masterVolume << ",\n";
    file << "  \"musicVolume\": " << musicVolume << ",\n";
    file << "  \"sfxVolume\": " << sfxVolume << ",\n";
    file << "  \"nowPlayingToast\": " << (nowPlayingToast ? "true" : "false") << ",\n";
    file << "  \"mapAtmosphere\": " << (mapAtmosphere ? "true" : "false") << ",\n";
    file << "  \"modUpdateChecks\": " << (modUpdateChecks ? "true" : "false") << ",\n";
    file << "  \"gameUpdateChecks\": " << (gameUpdateChecks ? "true" : "false") << ",\n";
    file << "  \"accentColor\": " << accentColor << ",\n";
    file << "  \"accountIssuer\": \"" << accountIssuer << "\",\n";
    file << "  \"serverCredential\": \"" << serverCredential << "\",\n";
    file << "  \"accountAgreed\": " << (accountAgreed ? "true" : "false") << ",\n";
    file << "  \"keybinds\": [";
    for (int i = 0; i < ACTION_COUNT; ++i) {
        if (i > 0) file << ", ";
        file << keybinds[i];
    }
    file << "]\n";
    file << "}\n";
    return true;
}
