// Greater Diplomacy translation layer -- see Game_Gdtl.h.
#include "Game_Gdtl.h"

#include "util/NativeDialog.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include "miniz.h"
#include "miniz_zip.h"

#ifdef OD_ENABLE_GDTL
#include <dragoman/dragoman.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

namespace Gdtl {

namespace {

bool isDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

bool isFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && !(st.st_mode & S_IFDIR);
}

// Only the platforms that actually search for an installation need these. On
// the web and on Android nothing calls them, and an unused static function is
// a warning -- which, in a build that turns warnings into errors, is a broken
// build on the two platforms this feature is quietest on.
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string home() {
#if defined(_WIN32)
    const std::string p = env("USERPROFILE");
    if (!p.empty()) return p;
    return env("HOMEDRIVE") + env("HOMEPATH");
#else
    return env("HOME");
#endif
}

// Names of directories inside a search root that are worth a look. Anything
// else is skipped without opening it -- this is the whole of the "one level
// deep" promise in the header.
bool nameLooksLikeGd5(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("greater") != std::string::npos ||
           lower.find("diplomacy") != std::string::npos ||
           lower.find("gd5") != std::string::npos;
}

// Immediate subdirectories only, and only ones whose name already suggests the
// game. Reading a directory listing is the least a search can do; walking into
// every folder it finds is not something to do to somebody's home directory.
std::vector<std::string> candidateChildren(const std::string& root) {
    std::vector<std::string> out;
    if (!isDirectory(root)) return out;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (nameLooksLikeGd5(name)) out.push_back(root + "\\" + name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(root.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (!nameLooksLikeGd5(name)) continue;
        const std::string full = root + "/" + name;
        if (isDirectory(full)) out.push_back(full);
    }
    closedir(d);
#endif
    std::sort(out.begin(), out.end());
    return out;
}

#endif  // !__EMSCRIPTEN__ && !__ANDROID__

#ifdef OD_ENABLE_GDTL
// Turn open-dragoman's report into the sentences the dialog shows, worst
// first, and free it. Errors are kept apart from warnings because a caller
// showing five remarks about ethnic minorities alongside "the map did not
// load" would bury the one that matters.
void drainReport(dg_report* report, Result& out) {
    if (!report) return;
    const int n = dg_report_count(report);
    std::vector<std::pair<int, std::string>> notes;
    notes.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const char* msg = dg_report_message(report, i);
        notes.emplace_back(dg_report_severity(report, i), msg ? msg : "");
    }
    std::stable_sort(notes.begin(), notes.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& note : notes)
        if (!note.second.empty()) out.notes.push_back(note.second);
    dg_report_free(report);
}
#endif

}  // namespace

// ------------------------------------------------------------------ presence

bool available() {
#ifdef OD_ENABLE_GDTL
    // Not merely compiled in: the library has to be the one this was built
    // against. dg_abi_version moves when an existing call changes meaning.
    return dg_abi_version() == DRAGOMAN_ABI_VERSION;
#else
    return false;
#endif
}

std::string version() {
#ifdef OD_ENABLE_GDTL
    const char* v = dg_version_string();
    return v ? v : "unknown";
#else
    return "unavailable";
#endif
}

// --------------------------------------------------------------- conversions

#ifdef OD_ENABLE_GDTL
static Result convert(const std::string& in, const std::string& out, dg_format to) {
    Result r;
    r.outputPath = out;

    dg_options opt;
    dg_options_defaults(&opt);

    dg_report* report = nullptr;
    // Zero is success. It is the C convention and not the C++ one, and reading
    // it the other way made every conversion look like a failure while the map
    // it had just written sat on disk beside the error.
    const int rc = dg_convert(in.c_str(), out.c_str(), to, &opt, &report);
    drainReport(report, r);

    r.ok = rc == 0;
    if (!r.ok) {
        const char* err = dg_last_error();
        r.error = (err && *err) ? err : "the conversion failed without saying why";
    }
    return r;
}
#endif

Result toGd5(const std::string& odmapPath, const std::string& outDir) {
#ifdef OD_ENABLE_GDTL
    return convert(odmapPath, outDir, DG_FORMAT_GD5);
#else
    (void)odmapPath;
    Result r;
    r.outputPath = outDir;
    r.error = "this build has no translation layer (rebuild with -DOD_ENABLE_GDTL=ON)";
    return r;
#endif
}

Result toOdmap(const std::string& gd5Dir, const std::string& odmapPath) {
#ifdef OD_ENABLE_GDTL
    return convert(gd5Dir, odmapPath, DG_FORMAT_ODMAP);
#else
    (void)gd5Dir;
    Result r;
    r.outputPath = odmapPath;
    r.error = "this build has no translation layer (rebuild with -DOD_ENABLE_GDTL=ON)";
    return r;
#endif
}

// ---------------------------------------------------------------- finding GD5

bool canChooseLocation() {
    return NativeDialog::available();
}

std::string unattendedDestination(const std::string& dataDir, const std::string& mapName) {
    std::string safe;
    for (char c : mapName) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
        safe += ok ? c : '_';
    }
    if (safe.empty()) safe = "Translated Map";
    return dataDir + "gdtl/" + safe;
}

bool looksLikeGd5(const std::string& dir) {
    if (dir.empty() || !isDirectory(dir)) return false;
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    const std::string base = dir.back() == sep ? dir : dir + sep;

    // Two independent marks, both required. `base_maps` alone is a plausible
    // folder name for anything; the research template is that game's own file
    // and is what open-dragoman reads the tech ceilings out of, so a directory
    // without it cannot be translated into properly anyway.
    const std::string maps = base + "base_maps";
    std::string tech = base + "data";
    tech += sep; tech += "json"; tech += sep; tech += "research_template.json";
    return isDirectory(maps) && isFile(tech);
}

std::vector<std::string> searchLocations() {
    std::vector<std::string> roots;
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    // Nothing to search. Returned empty rather than listing a phone's home
    // directory, because the dialog prints this count to say what it is about
    // to read and it must not claim to be about to read anything.
    return roots;
#else
    const std::string h = home();

#if defined(_WIN32)
    for (const char* var : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        const std::string v = env(var);
        if (!v.empty()) roots.push_back(v);
    }
    if (!h.empty()) {
        roots.push_back(h + "\\Documents");
        roots.push_back(h + "\\Downloads");
        roots.push_back(h + "\\Games");
    }
    roots.push_back("C:\\Program Files (x86)\\Steam\\steamapps\\common");
#elif defined(__APPLE__)
    roots.push_back("/Applications");
    if (!h.empty()) {
        roots.push_back(h + "/Applications");
        roots.push_back(h + "/Documents");
        roots.push_back(h + "/Downloads");
        roots.push_back(h + "/Games");
        roots.push_back(h + "/Library/Application Support/Steam/steamapps/common");
    }
#else
    if (!h.empty()) {
        roots.push_back(h);
        roots.push_back(h + "/Documents");
        roots.push_back(h + "/Downloads");
        roots.push_back(h + "/Games");
        roots.push_back(h + "/.local/share/Steam/steamapps/common");
        roots.push_back(h + "/.steam/steam/steamapps/common");
    }
    roots.push_back("/opt");
    roots.push_back("/usr/local/games");
#endif
    return roots;
#endif
}

std::vector<std::string> findGd5Installations() {
    std::vector<std::string> found;
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    // No other software on a browser's filesystem to find, and no desktop
    // build of Greater Diplomacy 5 to find on a phone.
    return found;
#else
    for (const std::string& root : searchLocations()) {
        // A root may be the installation itself -- somebody who put the game in
        // ~/Games/GreaterDiplomacy5 and somebody who made ~/Games the game are
        // both ordinary.
        if (looksLikeGd5(root)) {
            found.push_back(root);
            continue;
        }
        for (const std::string& child : candidateChildren(root))
            if (looksLikeGd5(child)) found.push_back(child);
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
#endif
}

std::string mapDestination(const std::string& gd5Dir, const std::string& mapName) {
    if (!looksLikeGd5(gd5Dir)) return "";
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    const std::string base = gd5Dir.back() == sep ? gd5Dir : gd5Dir + sep;

    // base_maps rather than scenarios/: a translated world is a map, not a
    // dated historical setup, and that game lists the two separately.
    std::string safe;
    for (char c : mapName) {
        const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                         c == '"' || c == '<' || c == '>' || c == '|';
        safe += bad ? '_' : c;
    }
    if (safe.empty()) safe = "Translated Map";
    return base + "base_maps" + sep + safe;
}

// ------------------------------------------------------------------ packaging

namespace {

// Every file under `dir`, as paths relative to it, so they can go into a zip
// with the layout the other game expects.
void collectFiles(const std::string& dir, const std::string& prefix,
                  std::vector<std::pair<std::string, std::string>>& out) {
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "\\" + name;
        const std::string rel = prefix.empty() ? name : prefix + "/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) collectFiles(full, rel, out);
        else out.emplace_back(full, rel);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        const std::string rel = prefix.empty() ? name : prefix + "/" + name;
        if (isDirectory(full)) collectFiles(full, rel, out);
        else out.emplace_back(full, rel);
    }
    closedir(d);
#endif
}

}  // namespace

bool zipDirectory(const std::string& dir, const std::string& zipPath, std::string& error) {
    std::vector<std::pair<std::string, std::string>> files;
    collectFiles(dir, "", files);
    if (files.empty()) {
        error = "nothing was written to " + dir;
        return false;
    }
    // Sorted, so the same map zips to the same archive twice running.
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, zipPath.c_str(), 0)) {
        error = "could not create " + zipPath;
        return false;
    }
    for (const auto& f : files) {
        if (!mz_zip_writer_add_file(&zip, f.second.c_str(), f.first.c_str(), nullptr, 0,
                                    MZ_BEST_SPEED)) {
            mz_zip_writer_end(&zip);
            error = "could not add " + f.second + " to the archive";
            return false;
        }
    }
    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        error = "could not finish writing " + zipPath;
        return false;
    }
    mz_zip_writer_end(&zip);
    return true;
}

bool offerBrowserDownload(const std::string& path, const std::string& suggestedName) {
#ifdef __EMSCRIPTEN__
    // Single-quoted into a JS string, so a map called Bob's World does not end
    // the literal early and run the rest of its own name.
    const auto quote = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '\\' || c == '\'') out += '\\';
            if (c == '\n' || c == '\r') { out += ' '; continue; }
            out += c;
        }
        return out;
    };
    const std::string js =
        "var d=FS.readFile('" + quote(path) + "');"
        "var b=new Blob([d],{type:'application/zip'});"
        "var u=URL.createObjectURL(b);var a=document.createElement('a');"
        "a.href=u;a.download='" + quote(suggestedName) + "';document.body.appendChild(a);"
        "a.click();document.body.removeChild(a);URL.revokeObjectURL(u);";
    emscripten_run_script(js.c_str());
    return true;
#else
    (void)path;
    (void)suggestedName;
    return false;
#endif
}

}  // namespace Gdtl
