#include "Config.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>

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
    aiDifficulty = findInt(json, "aiDifficulty", 1);
    if (aiDifficulty < 0) aiDifficulty = 0;
    if (aiDifficulty > 3) aiDifficulty = 3;
    aiDebug = findBool(json, "aiDebug", false);
    aiLearning = findBool(json, "aiLearning", false);
    accentColor = findInt(json, "accentColor", 0xFFD700);

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
    file << "  \"aiDifficulty\": " << aiDifficulty << ",\n";
    file << "  \"aiDebug\": " << (aiDebug ? "true" : "false") << ",\n";
    file << "  \"aiLearning\": " << (aiLearning ? "true" : "false") << ",\n";
    file << "  \"accentColor\": " << accentColor << ",\n";
    file << "  \"keybinds\": [";
    for (int i = 0; i < ACTION_COUNT; ++i) {
        if (i > 0) file << ", ";
        file << keybinds[i];
    }
    file << "]\n";
    file << "}\n";
    return true;
}
