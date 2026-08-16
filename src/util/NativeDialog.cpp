#include "NativeDialog.h"

#include <cstdio>
#include <cstdlib>

namespace NativeDialog {

namespace {

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
// Run a command and take its first line of output as a path.
std::string capture(const std::string& command) {
#if defined(_WIN32)
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return "";
    std::string result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

// A title goes into a shell command and then into a script, so it is kept to
// characters that cannot end a quote or start a command. Callers pass literals
// today; this is here so that stays true if one ever passes a map name.
std::string safeTitle(const std::string& title) {
    std::string out;
    for (char c : title) {
        const bool bad = c == '"' || c == '\'' || c == '`' || c == '$' || c == '\\' ||
                         c == '\n' || c == '\r' || c == ';' || c == '&' || c == '|';
        out += bad ? ' ' : c;
    }
    return out;
}

#if defined(__linux__) && !defined(__ANDROID__)
// Whichever of the two desktops' dialogs is installed, or empty for neither.
const char* linuxHelper() {
    if (system("command -v zenity >/dev/null 2>&1") == 0) return "zenity";
    if (system("command -v kdialog >/dev/null 2>&1") == 0) return "kdialog";
    return nullptr;
}
#endif
#endif  // !__EMSCRIPTEN__ && !__ANDROID__

}  // namespace

bool available() {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    return false;
#else
    return true;
#endif
}

std::string openFile(const std::string& title, const std::string& extension) {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    (void)title;
    (void)extension;
    return "";
#elif defined(__APPLE__)
    const std::string ofType = extension.empty() ? "" : " of type {\"" + extension + "\"}";
    return capture("osascript -e 'POSIX path of (choose file with prompt \"" +
                   safeTitle(title) + "\"" + ofType + ")' 2>/dev/null");
#elif defined(_WIN32)
    const std::string filter = extension.empty()
        ? "All files (*.*)|*.*"
        : extension + " files (*." + extension + ")|*." + extension;
    // -STA because the common dialogs are single-threaded-apartment COM.
    return capture(
        "powershell -NoProfile -STA -Command \""
        "Add-Type -AssemblyName System.Windows.Forms;"
        "$d=New-Object System.Windows.Forms.OpenFileDialog;"
        "$d.Title='" + safeTitle(title) + "';"
        "$d.Filter='" + filter + "';"
        "if($d.ShowDialog() -eq 'OK'){$d.FileName}\" 2>NUL");
#else
    const char* helper = linuxHelper();
    if (!helper) return "";
    const std::string t = safeTitle(title);
    if (std::string(helper) == "zenity") {
        const std::string filter =
            extension.empty() ? "" : " --file-filter='*." + extension + "'";
        return capture("zenity --file-selection --title='" + t + "'" + filter + " 2>/dev/null");
    }
    const std::string filter = extension.empty() ? "" : " '*." + extension + "'";
    return capture("kdialog --getopenfilename ~" + filter + " --title '" + t + "' 2>/dev/null");
#endif
}

std::string openFolder(const std::string& title) {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    (void)title;
    return "";
#elif defined(__APPLE__)
    return capture("osascript -e 'POSIX path of (choose folder with prompt \"" +
                   safeTitle(title) + "\")' 2>/dev/null");
#elif defined(_WIN32)
    return capture(
        "powershell -NoProfile -STA -Command \""
        "Add-Type -AssemblyName System.Windows.Forms;"
        "$d=New-Object System.Windows.Forms.FolderBrowserDialog;"
        "$d.Description='" + safeTitle(title) + "';"
        "if($d.ShowDialog() -eq 'OK'){$d.SelectedPath}\" 2>NUL");
#else
    const char* helper = linuxHelper();
    if (!helper) return "";
    const std::string t = safeTitle(title);
    if (std::string(helper) == "zenity")
        return capture("zenity --file-selection --directory --title='" + t + "' 2>/dev/null");
    return capture("kdialog --getexistingdirectory ~ --title '" + t + "' 2>/dev/null");
#endif
}

}  // namespace NativeDialog
