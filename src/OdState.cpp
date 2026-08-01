#include "OdState.h"
#include "miniz.h"
#include "miniz_zip.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <ctime>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace fs = std::filesystem;

namespace {

// Top-level names under data/ that the BUILD ships, not the player. Everything
// else is state and goes into the archive.
//
// This mirrors OD_SHIPPED_DATA in CMakeLists.txt and DATA_ALLOWLIST in
// tools/release.py -- the same split, used the other way round. If a new
// shipped directory is added there and not here, the only cost is a fatter
// .odstate; forgetting it in the other direction would silently drop something
// the player made, which is why the default is to include.
const char* kShipped[] = {
    "STDmaps", "audio", "flags", "fonts", "icons", "symbols", "licenses", "ai",
    "tips.json", "credits.txt",
    // Not content, but not the player's either.
    "Icon", "MANAGED", "VERSION",
    "tools",        // downloaded cloudflared; refetched on demand
};

bool isShipped(const std::string& topLevel) {
    for (const char* s : kShipped)
        if (topLevel == s) return true;
    return false;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// An archive must never contain archives. The desktop build writes them into
// exports/, which is otherwise player content and so is captured -- without
// this, every export would swallow the previous one and each file would be
// bigger than the last for no added content.
bool isArchive(const fs::path& p) { return endsWith(p.filename().string(), ".odstate"); }

// Entries that put executable mod content back into the game.
bool isModEntry(const std::string& name) {
    return name.rfind("mods/", 0) == 0 || name == "mods.json";
}

// A zip entry is safe only if it stays under the destination. Anything absolute
// or containing ".." is refused rather than sanitised: a rewritten path is a
// guess at what the archive meant, and this archive is a file the player picked
// off their disk.
bool safeEntryName(const std::string& name) {
    if (name.empty()) return false;
    if (name.front() == '/' || name.front() == '\\') return false;
    if (name.size() > 1 && name[1] == ':') return false;          // C:\...
    for (const auto& part : fs::path(name)) {
        std::string p = part.string();
        if (p == ".." ) return false;
    }
    return true;
}

std::string readWhole(const fs::path& p, bool& ok) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { ok = false; return {}; }
    std::string data((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    ok = true;
    return data;
}

}  // namespace

namespace OdState {

std::string suggestedFilename() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "OpenDoctrines-%Y%m%d-%H%M.odstate", &tmv);
    return buf;
}

std::string defaultSaveDir(const std::string& dataDir) {
    std::error_code ec;
    fs::path d = fs::path(dataDir) / "exports";
    fs::create_directories(d, ec);
    return d.generic_string();
}

std::vector<std::string> findArchives(const std::string& dataDir) {
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, std::string>> found;
    fs::path d(defaultSaveDir(dataDir));
    for (const auto& e : fs::directory_iterator(d, ec)) {
        if (!e.is_regular_file(ec) || !isArchive(e.path())) continue;
        found.push_back({e.last_write_time(ec), e.path().generic_string()});
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });  // newest first
    std::vector<std::string> out;
    out.reserve(found.size());
    for (auto& f : found) out.push_back(f.second);
    return out;
}

int countMods(const std::string& archivePath) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archivePath.c_str(), 0)) return -1;
    int mods = 0;
    int n = (int)mz_zip_reader_get_num_files(&zip);
    for (int i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        // Count the mod packages, not their unpacked storage: one .odmod is one
        // thing the player recognises, a mod's scratch files are not.
        std::string nm = st.m_filename;
        if (isModEntry(nm) && endsWith(nm, ".odmod")) mods++;
    }
    mz_zip_reader_end(&zip);
    return mods;
}

bool save(const std::string& dataDir, const std::string& outPath,
          std::string& err, int* outCount) {
    std::error_code ec;
    fs::path root(dataDir);
    if (!fs::is_directory(root, ec)) { err = "no data directory to archive"; return false; }

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, outPath.c_str(), 0)) {
        err = "could not open " + outPath + " for writing";
        return false;
    }

    int stored = 0;
    for (const auto& top : fs::directory_iterator(root, ec)) {
        std::string name = top.path().filename().string();
        if (name == "." || name == ".." || name == ".DS_Store") continue;
        if (isShipped(name)) continue;

        if (fs::is_directory(top.path(), ec)) {
            for (auto it = fs::recursive_directory_iterator(top.path(), ec);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (!it->is_regular_file(ec)) continue;
                if (it->path().filename() == ".DS_Store") continue;
                if (isArchive(it->path())) continue;   // never nest exports
                bool ok = false;
                std::string data = readWhole(it->path(), ok);
                if (!ok) continue;   // vanished mid-walk; not worth failing over
                std::string rel = fs::relative(it->path(), root, ec).generic_string();
                if (rel.empty()) continue;
                if (mz_zip_writer_add_mem(&zip, rel.c_str(), data.data(), data.size(),
                                          MZ_BEST_COMPRESSION))
                    stored++;
            }
        } else if (fs::is_regular_file(top.path(), ec)) {
            if (isArchive(top.path())) continue;   // never nest exports
            bool ok = false;
            std::string data = readWhole(top.path(), ok);
            if (!ok) continue;
            if (mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(),
                                      MZ_BEST_COMPRESSION))
                stored++;
        }
    }

    if (stored == 0) {
        // An empty archive would load back as "success" having restored nothing.
        mz_zip_writer_end(&zip);
        fs::remove(outPath, ec);
        err = "there is no saved progress to export yet";
        return false;
    }

    if (!mz_zip_writer_finalize_archive(&zip)) {
        mz_zip_writer_end(&zip);
        err = "could not finish writing the archive";
        return false;
    }
    mz_zip_writer_end(&zip);
    if (outCount) *outCount = stored;
    return true;
}

bool load(const std::string& dataDir, const std::string& archivePath,
          std::string& err, int* outCount) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archivePath.c_str(), 0)) {
        err = "that file is not a readable .odstate archive";
        return false;
    }

    std::error_code ec;
    fs::path root(dataDir);
    int n = (int)mz_zip_reader_get_num_files(&zip);
    int written = 0, refused = 0;

    for (int i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        std::string name = st.m_filename;
        if (!safeEntryName(name)) { refused++; continue; }

        size_t sz = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (!data) continue;

        fs::path dest = root / name;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write((const char*)data, (std::streamsize)sz);
            out.close();
            written++;
        }
        mz_free(data);
    }
    mz_zip_reader_end(&zip);

    if (written == 0) {
        err = refused > 0 ? "that archive's entries were all rejected as unsafe"
                          : "that archive held nothing to restore";
        return false;
    }
    if (refused > 0) {
        // Loud, but not fatal: the rest did restore, and the player should know
        // the file was not what it claimed to be.
        err = "restored " + std::to_string(written) + " files; refused " +
              std::to_string(refused) + " with unsafe paths";
    }
    if (outCount) *outCount = written;
    return true;
}

#ifdef __EMSCRIPTEN__

void webDownload(const std::string& fsPath, const std::string& filename) {
    // Same shape as the timelapse export in Game_History.cpp: read it back out
    // of MEMFS and hand the bytes to the browser, because a file written inside
    // the sandbox is not a file on the player's machine.
    std::string js =
        "var d=FS.readFile('" + fsPath + "');"
        "var b=new Blob([d],{type:'application/zip'});"
        "var u=URL.createObjectURL(b);var a=document.createElement('a');"
        "a.href=u;a.download='" + filename + "';document.body.appendChild(a);"
        "a.click();document.body.removeChild(a);URL.revokeObjectURL(u);";
    emscripten_run_script(js.c_str());
}

EM_JS(void, od_state_pick, (), {
    // The picker must be opened from a real user gesture, which this is: it
    // runs inside the click handler that reached the menu item.
    var inp = document.createElement('input');
    inp.type = 'file';
    inp.accept = '.odstate,application/zip';
    inp.style.display = 'none';
    inp.onchange = function() {
        if (!inp.files || !inp.files.length) { document.body.removeChild(inp); return; }
        var f = inp.files[0];
        var r = new FileReader();
        r.onload = function() {
            try {
                FS.writeFile('/odstate_in.odstate', new Uint8Array(r.result));
                Module.odstateIncoming = '/odstate_in.odstate';
            } catch (e) {
                Module.odstateIncoming = '';
            }
            document.body.removeChild(inp);
        };
        r.onerror = function() { Module.odstateIncoming = ''; document.body.removeChild(inp); };
        r.readAsArrayBuffer(f);
    };
    document.body.appendChild(inp);
    inp.click();
});

EM_JS(char*, od_state_take, (), {
    var p = Module.odstateIncoming;
    if (!p) return 0;
    Module.odstateIncoming = null;      // reported once
    var len = lengthBytesUTF8(p) + 1;
    var buf = _malloc(len);
    stringToUTF8(p, buf, len);
    return buf;
});

void webPickFile() { od_state_pick(); }

bool webTakeImport(std::string& outPath) {
    char* p = od_state_take();
    if (!p) return false;
    outPath = p;
    free(p);
    return !outPath.empty();
}

#endif  // __EMSCRIPTEN__

}  // namespace OdState
