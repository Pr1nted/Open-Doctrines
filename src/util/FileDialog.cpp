#include "FileDialog.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#elif defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
#define OD_DIALOG_VIA_SHELL 1
#endif

namespace fileDialog {
namespace {

#ifdef OD_DIALOG_VIA_SHELL
/** Run a command and return its first line of stdout, trailing newline gone. */
std::string runCapture(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

/** Wrap in single quotes for /bin/sh, so a title or path cannot run anything. */
std::string shQuote(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    return q + "'";
}
#endif

#ifdef __APPLE__
/** Escape for a double-quoted AppleScript literal. */
std::string asQuote(const std::string& s) {
    std::string q;
    for (char c : s) {
        if (c == '"' || c == '\\') q += '\\';
        q += c;
    }
    return q;
}
#endif

#if defined(__linux__) && !defined(__ANDROID__)
bool haveCommand(const char* name) {
    return !runCapture(std::string("command -v ") + name + " 2>/dev/null").empty();
}
#endif

#ifdef _WIN32
std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string fromWide(const wchar_t* s) {
    if (!s || !*s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

/**
 * A comdlg32 filter: pairs of NUL-terminated strings, the whole thing ending
 * in a second NUL. Built as "Map files (*.odmap;*.uodmap)\0*.odmap;*.uodmap\0
 * All files (*.*)\0*.*\0\0".
 */
std::wstring buildFilter(const std::vector<std::string>& extensions) {
    std::wstring f;
    if (!extensions.empty()) {
        std::wstring patterns;
        for (size_t i = 0; i < extensions.size(); ++i) {
            if (i) patterns += L";";
            patterns += L"*." + toWide(extensions[i]);
        }
        f += L"Map files (" + patterns + L")";
        f.push_back(L'\0');
        f += patterns;
        f.push_back(L'\0');
    }
    f += L"All files (*.*)";
    f.push_back(L'\0');
    f += L"*.*";
    f.push_back(L'\0');
    f.push_back(L'\0');
    return f;
}
#endif

}  // namespace

bool available() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#elif defined(__linux__) && !defined(__ANDROID__)
    return haveCommand("zenity") || haveCommand("kdialog");
#else
    return false;
#endif
}

std::string open(const std::string& title, const std::vector<std::string>& extensions) {
#if defined(_WIN32)
    std::wstring wTitle = toWide(title);
    std::wstring filter = buildFilter(extensions);
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = wTitle.empty() ? nullptr : wTitle.c_str();
    // NOCHANGEDIR: without it the picker moves the process's working
    // directory, and every later relative path in the game resolves somewhere
    // the player happened to browse to.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    return GetOpenFileNameW(&ofn) ? fromWide(path) : std::string();

#elif defined(__APPLE__)
    std::string ofType;
    if (!extensions.empty()) {
        ofType = " of type {";
        for (size_t i = 0; i < extensions.size(); ++i) {
            if (i) ofType += ", ";
            ofType += "\"" + asQuote(extensions[i]) + "\"";
        }
        ofType += "}";
    }
    std::string script = "POSIX path of (choose file with prompt \"" + asQuote(title) + "\"" +
                         ofType + ")";
    return runCapture("osascript -e " + shQuote(script) + " 2>/dev/null");

#elif defined(__linux__) && !defined(__ANDROID__)
    if (haveCommand("zenity")) {
        std::string cmd = "zenity --file-selection --title=" + shQuote(title);
        for (auto& e : extensions)
            cmd += " --file-filter=" + shQuote("*." + e);
        if (!extensions.empty()) cmd += " --file-filter=" + shQuote("*");
        return runCapture(cmd + " 2>/dev/null");
    }
    if (haveCommand("kdialog")) {
        std::string patterns;
        for (size_t i = 0; i < extensions.size(); ++i) {
            if (i) patterns += " ";
            patterns += "*." + extensions[i];
        }
        if (patterns.empty()) patterns = "*";
        return runCapture("kdialog --getopenfilename . " + shQuote(patterns) + " --title " +
                          shQuote(title) + " 2>/dev/null");
    }
    return {};
#else
    (void)title; (void)extensions;
    return {};
#endif
}

std::string save(const std::string& title, const std::string& defaultName,
                 const std::string& extension) {
    // Shared across platforms: a player who typed "my map" gets "my map.odmap"
    // rather than an extensionless file the browser will not list.
    auto withExtension = [&](std::string path) {
        if (path.empty() || extension.empty()) return path;
        const std::string suffix = "." + extension;
        if (path.size() >= suffix.size()) {
            std::string tail = path.substr(path.size() - suffix.size());
            std::transform(tail.begin(), tail.end(), tail.begin(),
                           [](unsigned char c) { return (char)tolower(c); });
            if (tail == suffix) return path;
        }
        return path + suffix;
    };

#if defined(_WIN32)
    std::wstring wTitle = toWide(title);
    std::wstring wExt = toWide(extension);
    std::wstring filter = buildFilter(extension.empty() ? std::vector<std::string>{}
                                                        : std::vector<std::string>{extension});
    wchar_t path[MAX_PATH] = {0};
    std::wstring wName = toWide(defaultName);
    wcsncpy(path, wName.c_str(), MAX_PATH - 1);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = wTitle.empty() ? nullptr : wTitle.c_str();
    ofn.lpstrDefExt = wExt.empty() ? nullptr : wExt.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_EXPLORER;
    return GetSaveFileNameW(&ofn) ? withExtension(fromWide(path)) : std::string();

#elif defined(__APPLE__)
    std::string script = "POSIX path of (choose file name with prompt \"" + asQuote(title) +
                         "\" default name \"" + asQuote(defaultName) + "\")";
    return withExtension(runCapture("osascript -e " + shQuote(script) + " 2>/dev/null"));

#elif defined(__linux__) && !defined(__ANDROID__)
    if (haveCommand("zenity")) {
        std::string cmd = "zenity --file-selection --save --confirm-overwrite --title=" +
                          shQuote(title) + " --filename=" + shQuote(defaultName);
        return withExtension(runCapture(cmd + " 2>/dev/null"));
    }
    if (haveCommand("kdialog"))
        return withExtension(runCapture("kdialog --getsavefilename " + shQuote(defaultName) +
                                        " --title " + shQuote(title) + " 2>/dev/null"));
    return {};
#else
    (void)title; (void)defaultName;
    return {};
#endif
}

}  // namespace fileDialog
