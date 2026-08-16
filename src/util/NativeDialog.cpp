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

// A title goes into a shell command and then into a script, so it is kept to
// characters that cannot end a quote or start a command. Callers pass literals
// today; this is here so that stays true if one ever passes a map name.
std::string safeTitle(const std::string& title) {
    std::string out;
    for (char c : title) {
        const bool bad = c == '"' || c == '\'' || c == '`' || c == '$' || c == '\\' ||
                         c == '\n' || c == '\r' || c == ';' || c == '&' || c == '|' ||
                         c == '(' || c == ')' || c == '<' || c == '>';
        out += bad ? ' ' : c;
    }
    return out;
}

bool available() {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    return false;
#else
    return true;
#endif
}

bool helperInstalled() {
    // Built the same way the real call builds it, so the answer cannot drift
    // from what actually happens when the button is pressed.
    return !commandFor(Kind::Folder, "probe", "").empty();
}

// The command itself, built but not run.
//
// Separated from the running so it can be TESTED. A dialog is a modal window
// waiting for a person, which is not something CI can click -- but the thing
// that actually goes wrong here is not the clicking. It is a quote in the
// wrong place, a flag the installed helper does not have, a title that ends
// the string it was pasted into. All of that is in this function, and all of
// it can be checked on a platform that cannot open a single window.
std::string commandFor(Kind kind, const std::string& title, const std::string& extension) {
    const bool folder = kind == Kind::Folder;
    const std::string t = safeTitle(title);
    const std::string ext = safeTitle(extension);
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    (void)folder;
    return "";
#elif defined(__APPLE__)
    const std::string what = folder ? "choose folder" : "choose file";
    const std::string ofType = (folder || ext.empty()) ? "" : " of type {\"" + ext + "\"}";
    return "osascript -e 'POSIX path of (" + what + " with prompt \"" + t + "\"" + ofType +
           ")' 2>/dev/null";
#elif defined(_WIN32)
    if (folder) {
        return "powershell -NoProfile -STA -Command \""
               "Add-Type -AssemblyName System.Windows.Forms;"
               "$d=New-Object System.Windows.Forms.FolderBrowserDialog;"
               "$d.Description='" + t + "';"
               "if($d.ShowDialog() -eq 'OK'){$d.SelectedPath}\" 2>NUL";
    }
    const std::string filter = ext.empty()
        ? "All files (*.*)|*.*"
        : ext + " files (*." + ext + ")|*." + ext;
    // -STA because the common dialogs are single-threaded-apartment COM.
    return "powershell -NoProfile -STA -Command \""
           "Add-Type -AssemblyName System.Windows.Forms;"
           "$d=New-Object System.Windows.Forms.OpenFileDialog;"
           "$d.Title='" + t + "';"
           "$d.Filter='" + filter + "';"
           "if($d.ShowDialog() -eq 'OK'){$d.FileName}\" 2>NUL";
#else
    const char* helper = linuxHelper();
    if (!helper) return "";
    if (std::string(helper) == "zenity") {
        const std::string dir = folder ? " --directory" : "";
        const std::string filter =
            (folder || ext.empty()) ? "" : " --file-filter='*." + ext + "'";
        return "zenity --file-selection" + dir + " --title='" + t + "'" + filter + " 2>/dev/null";
    }
    if (folder) return "kdialog --getexistingdirectory ~ --title '" + t + "' 2>/dev/null";
    const std::string filter = ext.empty() ? "" : " '*." + ext + "'";
    return "kdialog --getopenfilename ~" + filter + " --title '" + t + "' 2>/dev/null";
#endif
}

std::string openFile(const std::string& title, const std::string& extension) {
    if (!available()) return "";
    const std::string command = commandFor(Kind::File, title, extension);
    if (command.empty()) return "";   // no helper installed
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    return "";
#else
    return capture(command);
#endif
}

std::string openFolder(const std::string& title) {
    if (!available()) return "";
    const std::string command = commandFor(Kind::Folder, title, "");
    if (command.empty()) return "";
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    return "";
#else
    return capture(command);
#endif
}

}  // namespace NativeDialog
