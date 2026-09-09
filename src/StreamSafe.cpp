#include "StreamSafe.h"

#include <algorithm>
#include <cctype>

namespace streamsafe {

std::string maskSecret(const std::string& value) {
    if (value.empty()) return value;
    // A fixed number of dots rather than one per character: the length of an
    // invite code is a hint, and a short one is a smaller search space to
    // guess at. Eight is enough to read as "hidden" and no more.
    return "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"
           "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";
}

std::string redactPath(const std::string& path, const std::string& home) {
    if (path.empty()) return path;

    // The home directory, when we know it. Compared as a prefix and only on a
    // boundary, so "/Users/anna" does not swallow "/Users/annabel/...".
    if (!home.empty() && path.size() >= home.size() &&
        path.compare(0, home.size(), home) == 0 &&
        (path.size() == home.size() || path[home.size()] == '/' ||
         path[home.size()] == '\\')) {
        return "~" + path.substr(home.size());
    }

    // Failing that, the shapes a home directory takes on each platform. This is
    // the case that matters on somebody ELSE's machine -- a path inside a log
    // that was written elsewhere -- and on a build where HOME is not set.
    static const char* kRoots[] = {"/Users/", "/home/", "C:\\Users\\", "C:/Users/"};
    for (const char* root : kRoots) {
        const std::string r = root;
        const size_t at = path.find(r);
        if (at == std::string::npos) continue;
        const size_t nameStart = at + r.size();
        size_t nameEnd = path.find_first_of("/\\", nameStart);
        if (nameEnd == std::string::npos) nameEnd = path.size();
        // Keep everything after the user's name, drop the name itself.
        return "~" + path.substr(nameEnd);
    }
    return path;
}

bool looksSensitive(const std::string& text) {
    if (text.empty()) return false;
    static const char* kRoots[] = {"/Users/", "/home/", "C:\\Users\\", "C:/Users/"};
    for (const char* root : kRoots)
        if (text.find(root) != std::string::npos) return true;
    return false;
}

}  // namespace streamsafe
