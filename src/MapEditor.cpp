// See the note below on why these are STATIC.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
// STATIC keeps both stb implementations private to this file. raylib links its
// own copies; on desktop the linker drops the duplicates out of a static
// archive, but linking for web pulls in every object and they collide
// (wasm-ld: duplicate symbol stbi_load). Going through raylib's copy instead is
// not an option: the headless export path calls stbi_write_png_to_mem, which
// the implementation keeps internal.
#define STB_IMAGE_STATIC
#define STB_IMAGE_WRITE_STATIC
#include "MapEditor.h"
#include "util/LoadLog.h"
// The editor draws its own hints and labels, and they are translated too. It
// does not include Game.h, which is where everything else picks this up.
#include "i18n/Locale.h"
#include "TextInput.h"
#include "ProceduralGenerator.h"
#include "util/PngWrite.h"
#include "util/FileDialog.h"
#include "util/WebAssets.h"
#include "Audio.h"
#include "ScriptEngine.h"
#include <sstream>
#include "json.hpp"
#include "miniz.h"
#include "stb_image_write.h"
#include "stb_image.h"
#include "raylib.h"
#include "raymath.h"
#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <random>
#include <iostream>
#include <sys/stat.h>
// Directory listing is std::filesystem below, not dirent.h: MSVC has no such
// header, so this file could never compile on Windows. <filesystem> is already
// a dependency of this translation unit and does the same job portably.
#include <fstream>
#include <filesystem>

// THE EDITOR DREW ITS TRANSLATED LABELS IN A FONT WITH NO GLYPHS FOR THEM.
//
// drawButton already puts every label through T(), and every one of them is in
// the language files -- and the whole screen came out as rows of boxes. The
// translation was never the missing part: this file includes MapEditor.h
// rather than Game.h, and Game.h is where the rest of the game picks up the
// shadowed DrawText that draws non-ASCII out of the atlas. So the labels were
// translated and then handed to raylib's built-in font, which is ASCII and
// nothing else.
//
// LAST IN THE BLOCK, ON PURPOSE. The header ends with `#define DrawText ...`,
// so anything included after it has its own DrawText DECLARATION rewritten --
// raylib.h above being exactly that case.
#include "i18n/Text.h"

namespace fs = std::filesystem;

// std::filesystem rather than stat/S_ISDIR/mkdir: MSVC has none of those three
// spellings (it offers _stat, _S_IFDIR and _mkdir instead), so this pair was a
// Windows build failure on its own. create_directories also succeeds when the
// directory already exists, which is what the errno == EEXIST check was for.
static bool dirExists(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
static bool createDir(const std::string& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    return fs::is_directory(p, ec);
}
static Rectangle Rect(float x, float y, float w, float h) { return {x, y, w, h}; }

static const Color ACCENT = {255, 215, 0, 255};  // Gold accent
static const Color COL_LAND  = {200, 190, 160, 255}; // matches LandSeaMap in-game land
static const Color COL_SEA   = { 40,  80, 160, 255}; // matches LandSeaMap in-game sea

// Mirrors Game::drawLoadingScreen()'s look (throbber, status, percentage,
// progress bar) for the editor's own blocking save/load/import operations.
// Self-contained BeginDrawing/EndDrawing since it's called synchronously
// mid-function, outside the normal per-frame draw pass.
void MapEditor::drawMiniLoadingScreen(float progress, const std::string& status) {
    BeginDrawing();
    ClearBackground(Color{10, 10, 15, 255});
    int centerX = m_screenW / 2, centerY = m_screenH / 2;

    static const int offsets[8][2] = {
        {-1,-1},{0,-1},{1,-1}, {1,0}, {1,1},{0,1},{-1,1}, {-1,0}
    };
    float time = (float)GetTime();
    float sqSize = 12.0f, gap = 4.0f;
    float totalSize = sqSize * 3 + gap * 2;
    float startX = centerX - totalSize / 2 + sqSize / 2;
    float startY = centerY - totalSize / 2 + sqSize / 2;
    float period = 1.2f;
    for (int i = 0; i < 8; ++i) {
        float phase = (float)i / 8.0f;
        float t = fmodf(time / period + phase, 1.0f);
        float alpha = std::max(0.0f, std::min(255.0f, (1.0f - t) * 255.0f));
        float px = startX + offsets[i][0] * (sqSize + gap);
        float py = startY + offsets[i][1] * (sqSize + gap);
        DrawRectangle((int)(px - sqSize/2), (int)(py - sqSize/2), (int)sqSize, (int)sqSize,
                      ColorAlpha(ACCENT, alpha / 255.0f));
    }

    int statusW = MeasureText(status.c_str(), 24);
    DrawText(status.c_str(), centerX - statusW / 2, centerY + 80, 24, WHITE);

    int percent = (int)(std::max(0.0f, std::min(1.0f, progress)) * 100);
    std::string percentStr = std::to_string(percent) + "%";
    int percentW = MeasureText(percentStr.c_str(), 32);
    DrawText(percentStr.c_str(), m_screenW - percentW - 30, m_screenH - 50, 32, ACCENT);

    int barW = 400, barH = 8;
    int barX = centerX - barW / 2, barY = m_screenH - 100;
    DrawRectangleRounded({(float)barX, (float)barY, (float)barW, (float)barH}, 0.1f, 4, Color{40,40,50,255});
    DrawRectangleRounded({(float)barX, (float)barY, (float)(barW * std::max(0.0f, std::min(1.0f, progress))), (float)barH}, 0.1f, 4, ACCENT);

    EndDrawing();
}

MapEditor::MapEditor() {}
MapEditor::~MapEditor() {
    if (m_flagPreviewTex.id > 0) UnloadTexture(m_flagPreviewTex);
    if (m_thumbnailTex.id > 0) UnloadTexture(m_thumbnailTex);
    if (m_renderer) { delete m_renderer; m_renderer = nullptr; }
}

// Copies a dropped image into the project so the thumbnail survives even if
// the original is moved/deleted, then invalidates the cached preview.
bool MapEditor::setThumbnailFromFile(const std::string& srcPath) {
    int w = 0, h = 0, comp = 0;
    if (!stbi_info(srcPath.c_str(), &w, &h, &comp)) return false;
    fs::create_directories(m_dataDir + "projects/thumbs/");
    std::string ext = ".png";
    auto dot = srcPath.find_last_of('.');
    if (dot != std::string::npos) ext = srcPath.substr(dot);
    // Unique name per drop: the texture cache keys on path, so reusing a name
    // would keep showing the previously dropped image.
    std::string dest = m_dataDir + "projects/thumbs/thumb_" + std::to_string(rand() % 1000000) + ext;
    std::ifstream src(srcPath, std::ios::binary);
    if (!src) return false;
    std::ofstream dst(dest, std::ios::binary);
    dst << src.rdbuf();
    dst.close();
    m_thumbnailPath = dest;
    m_thumbnailTexPath.clear(); // force the preview to reload
    trackChange();
    return true;
}

void MapEditor::init(int screenW, int screenH, const std::string& dataDir) {
    m_screenW = screenW; m_screenH = screenH; m_dataDir = dataDir;
    m_canvasX = 0; m_canvasY = m_toolbarH;
    m_canvasW = screenW - m_panelW;
    m_canvasH = screenH - m_toolbarH - m_bottomH;
    m_projectState = PROJ_STARTUP;
    buildResearchNodes(m_researchNodes); // for the country-editor research picker
}

void MapEditor::resize(int screenW, int screenH) {
    m_screenW = screenW; m_screenH = screenH;
    m_canvasX = 0; m_canvasY = m_toolbarH;
    m_canvasW = screenW - m_panelW;
    m_canvasH = screenH - m_toolbarH - m_bottomH;
    if (m_renderer) m_renderer->resize(m_canvasW, m_canvasH);
}

// ════════════════════════════════════════════════════════════════
//  Button helper - matches main menu style
// ════════════════════════════════════════════════════════════════

bool MapEditor::drawButtonCol(const char* label, Rectangle rect, Color accent, bool selected, int fontSize) {
    Vector2 mouse = GetMousePosition();
    bool hover = CheckCollisionPointRec(mouse, rect);
    Color bg = hover ? Color{255,255,255,20} : (selected ? ColorAlpha(accent, 0.15f) : BLANK);
    Color border = selected ? accent : (hover ? Color{255,255,255,80} : Color{255,255,255,30});
    Color textCol = selected ? accent : (hover ? WHITE : LIGHTGRAY);

    DrawRectangleRounded(rect, 0.12f, 8, bg);
    DrawRectangleRoundedLines(rect, 0.12f, 8, border);
    const char* shown = T(label);
    // THE LABEL IS TRANSLATED HERE, not at the hundred call sites.
    //
    // Every button on this screen comes through this function, so this is the
    // one place that has to know about the language -- the same reasoning that
    // put the shadowed DrawText in i18n/Text.h rather than editing 970 draw
    // sites. A caller passing a literal gets it translated for free; the
    // extractor is told about this function so those literals reach en.json.
    int tw = MeasureText(shown, fontSize);
    DrawText(shown, (int)(rect.x + rect.width/2 - tw/2), (int)(rect.y + rect.height/2 - fontSize/2), fontSize, textCol);

    const int id = (int)rect.x * 73856093 ^ (int)rect.y * 19349663 ^ (int)rect.width * 83492791;
    if (hover && m_lastHoverBtn != id) {
        m_lastHoverBtn = id;
        Audio::get().playSfx("hover");
    }
    // Lighter than the menus on purpose: the editor is a dense tool surface and
    // a heavy click on every brush and layer button would wear thin fast.
    if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
        Audio::get().playSfx("click_light");
    return hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

bool MapEditor::drawButton(const char* label, Rectangle rect, bool selected, int fontSize) {
    return drawButtonCol(label, rect, ACCENT, selected, fontSize);
}

// ════════════════════════════════════════════════════════════════
//  Project Dialogs
// ════════════════════════════════════════════════════════════════

void MapEditor::drawStartupDialog() {
    ClearBackground(Color{15, 15, 20, 255});
    float cx = m_screenW / 2.0f, cy = m_screenH / 2.0f - 80.0f;

    int titleW = MeasureText(T("Map Editor"), 40);
    DrawText(T("Map Editor"), cx - titleW/2, cy - 70, 40, ACCENT);

    const char* labels[] = {"Open Existing Project", "Create New Project"};
    Rectangle btns[] = { {cx-160, cy, 320, 44}, {cx-160, cy+54, 320, 44} };
    for (int i = 0; i < 2; i++) {
        if (drawButton(labels[i], btns[i], false, 20)) {
            if (i == 0) {
                m_projectState = PROJ_OPEN;
                m_projFiles.clear();
                std::string dir = m_dataDir + "projects/";
                {
                    std::error_code ec;
                    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                        std::string n = e.path().filename().string();
                        if (n.size() >= 7 && n.substr(n.size() - 7) == ".uodmap")
                            m_projFiles.push_back(n);
                    }
                }
                std::sort(m_projFiles.begin(), m_projFiles.end());
                m_projChoice = 0; m_projScroll = 0;
            } else {
                m_projectState = PROJ_CREATE;
                m_projChoice = 0;
            }
        }
    }

    // Third way in, alongside the two lists: a file from anywhere on the
    // machine. Hidden where the platform has no file picker (web, Android)
    // rather than shown as a button that does nothing when pressed.
    float nextY = cy + 108;
    if (fileDialog::available()) {
        Rectangle importBtn = {cx-160, nextY, 320, 44};
        if (drawButton("Import Map File...", importBtn, false, 20)) promptImportFromFile();
        int hintW = MeasureText(T(".odmap or .uodmap from anywhere on this computer"), 12);
        DrawText(T(".odmap or .uodmap from anywhere on this computer"),
                 (int)(cx - hintW / 2), (int)nextY + 48, 12, GRAY);
        nextY += 74;
    }

    // Explicit, separate action for exiting the tool entirely (back to the
    // game's main menu) — distinct from ESC/unsaved-changes, which only
    // returns here, to this map menu.
    Rectangle leaveBtn = {cx-160, nextY + 10, 320, 36};
    if (drawButton("Leave Editor", leaveBtn, false, 16)) {
        m_wantsExit = true;
    }

    if (m_warningTimer > 0 && !m_warningMsg.empty()) {
        int tw = MeasureText(m_warningMsg.c_str(), 14);
        DrawText(m_warningMsg.c_str(), (int)(cx - tw / 2), (int)nextY + 56, 14,
                 Color{255, 120, 120, 255});
    }
}

void MapEditor::drawOpenDialog() {
    ClearBackground(Color{15, 15, 20, 255});
    float cx = m_screenW / 2.0f, cy = m_screenH / 2.0f - 120.0f;
    DrawText(T("Open Project"), cx - 80, cy - 40, 28, WHITE);

    Rectangle listRect = {cx-200, cy, 400, 260};
    DrawRectangleRounded(listRect, 0.05f, 4, Color{30,30,35,255});
    DrawRectangleRoundedLines(listRect, 0.05f, 4, Color{80,80,90,255});

    if (m_projFiles.empty()) {
        DrawText(T("No .uodmap files found"), cx-100, cy+120, 16, GRAY);
        DrawText(T("in data/projects/"), cx-70, cy+142, 14, GRAY);
    } else {
        int itemH = 28;
        for (int i = 0; i < 9 && (i + m_projScroll) < (int)m_projFiles.size(); i++) {
            int idx = i + m_projScroll;
            Rectangle item = {listRect.x+5, listRect.y+5+i*itemH, listRect.width-10-70, (float)(itemH-2)};
            bool sel = (idx == m_projChoice);
            bool hov = CheckCollisionPointRec(GetMousePosition(), item);
            DrawRectangleRounded(item, 0.1f, 4, sel ? ColorAlpha(ACCENT,0.2f) : (hov ? Color{255,255,255,16} : BLANK));
            DrawText(m_projFiles[idx].c_str(), (int)item.x+10, (int)item.y+6, 14, sel ? ACCENT : (hov ? WHITE : LIGHTGRAY));
            if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) { m_projChoice = idx; m_projDeleteArm = -1; }

            bool armed = (m_projDeleteArm == idx);
            Rectangle delBtn = {listRect.x + listRect.width - 68, listRect.y+5+i*itemH, 63, (float)(itemH-2)};
            if (drawButton(armed ? "Confirm?" : "Delete", delBtn, armed, 11)) {
                if (armed) {
                    std::string path = m_dataDir + "projects/" + m_projFiles[idx];
                    fs::remove(path);
                    m_projFiles.erase(m_projFiles.begin() + idx);
                    m_projDeleteArm = -1;
                    if (m_projChoice >= (int)m_projFiles.size()) m_projChoice = (int)m_projFiles.size() - 1;
                } else {
                    m_projDeleteArm = idx;
                }
            }
        }
        float wheel = GetMouseWheelMove();
        if (wheel > 0 && m_projScroll > 0) m_projScroll--;
        if (wheel < 0 && m_projScroll < (int)m_projFiles.size() - 9) m_projScroll++;
    }

    cy += 280;
    // Same reasoning as the map browser: the list only knows data/projects,
    // so a project from anywhere else needs its own way in.
    if (fileDialog::available()) {
        Rectangle browseBtn = {cx-220, cy, 200, 40};
        if (drawButton("Open from File...", browseBtn, false, 16)) promptImportFromFile();
    }
    Rectangle openBtn = {fileDialog::available() ? cx+20 : cx-100, cy, 200, 40};
    Rectangle cancelBtn = {cx-100, cy+50, 200, 36};
    if (drawButton("Open", openBtn, false, 18)) {
        if (m_projChoice >= 0 && m_projChoice < (int)m_projFiles.size()) {
            std::string path = m_dataDir + "projects/" + m_projFiles[m_projChoice];
            if (!loadProject(path)) {
                m_warningMsg = T("Failed to load ") + m_projFiles[m_projChoice];
                m_warningTimer = 3.0f;
            }
        }
    }
    if (drawButton("Cancel", cancelBtn, false, 16)) { m_projectState = PROJ_STARTUP; }
    if (m_warningTimer > 0) {
        int tw2 = MeasureText(m_warningMsg.c_str(), 14);
        DrawText(m_warningMsg.c_str(), (int)(cx - tw2 / 2), (int)cy + 96, 14, Color{255, 120, 120, 255});
    }
}

void MapEditor::drawCreateDialog() {
    ClearBackground(Color{15, 15, 20, 255});
    float cx = m_screenW / 2.0f, cy = m_screenH / 2.0f - 100.0f;
    int titleW = MeasureText(T("Create New Map"), 32);
    DrawText(T("Create New Map"), cx - titleW/2, cy - 60, 32, ACCENT);

    const char* labels[] = {"Blank Canvas", "Generate Procedurally", "Based on Existing Map"};
    Rectangle btns[3];
    for (int i = 0; i < 3; i++) btns[i] = {cx-160.0f, cy+i*52.0f, 320.0f, 44.0f};
    for (int i = 0; i < 3; i++) {
        if (drawButton(labels[i], btns[i], m_projChoice == i, 18)) { m_projChoice = i; }
    }

    cy += 168;
    Rectangle confirmBtn = {cx-100, cy, 200, 42};
    Rectangle cancelBtn = {cx-100, cy+52, 200, 36};
    const char* confirmLabel = (m_projChoice == 2) ? "Browse Maps..." : "Create";
    if (drawButton(confirmLabel, confirmBtn, false, 18)) {
        if (m_projChoice == 2) {
            m_projectState = PROJ_IMPORT;
            m_importScanned = false;
            m_importChoice = -1;
            m_importScroll = 0;
        } else {
            m_genParams.seed = rand();
            initBlankMap();
            if (m_projChoice == 1) {
                m_genPending = 1;
                m_genStatus = "Generating world...";
            }
            m_projectState = PROJ_EDITING;
        }
    }
    if (drawButton("Cancel", cancelBtn, false, 16)) { m_projectState = PROJ_STARTUP; }
}

void MapEditor::scanImportableMaps() {
    m_importScanned = true;
    m_importEntries.clear();

    auto readNameFromOdmap = [](const std::string& path, const std::string& fallback) -> std::string {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) return fallback;
        int idx = mz_zip_reader_locate_file(&zip, "metadata.json", nullptr, 0);
        std::string name = fallback;
        if (idx >= 0) {
            size_t sz = 0;
            void* p = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
            if (p) {
                try {
                    auto j = nlohmann::json::parse(std::string((char*)p, sz));
                    name = j.value("name", fallback);
                } catch (...) {}
                mz_free(p);
            }
        }
        mz_zip_reader_end(&zip);
        return name;
    };

    // Standard maps: the six in data/STDmaps.
    //
    // From maps_index.json rather than by scanning the directory, because on
    // the web those archives are not on disk until something asks for one --
    // they are excluded from the preload and fetched on demand, see
    // util/WebAssets.h. A scan there finds nothing but thumbnails and this
    // dialog reports that the game ships no maps. The index lists what ships,
    // which is exactly the question being asked here, and it carries the
    // scenario's proper name so no archive has to be opened to label a row.
    {
        std::string dir = m_dataDir + "STDmaps/";
        bool listed = false;
        std::ifstream idx(dir + "maps_index.json");
        if (idx) {
            try {
                auto j = nlohmann::json::parse(idx);
                for (auto& entry : j) {
                    std::string n = entry.value("filename", std::string());
                    if (n.size() < 7 || n.substr(n.size() - 6) != ".odmap") continue;
                    std::string label =
                        entry.value("name", n.substr(0, n.size() - 6));
                    m_importEntries.push_back({label + "  [standard]", dir + n, true});
                    listed = true;
                }
            } catch (std::exception& e) {
                LoadLog() << "Map editor: maps_index.json — " << e.what() << std::endl;
            }
        }
        // No index, or an unusable one: read the archives themselves, which is
        // what this always did and still works wherever they are on disk.
        if (!listed) {
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                std::string n = e.path().filename().string();
                if (n.size() < 6 || n.substr(n.size() - 6) != ".odmap") continue;
                std::string path = dir + n;
                std::string label = readNameFromOdmap(path, n.substr(0, n.size() - 6));
                m_importEntries.push_back({label + "  [standard]", path, true});
            }
        }
    }
    // Custom maps: data/custom_maps/<name>/*.odmap
    {
        std::string dir = m_dataDir + "custom_maps/";
        // directory_iterator never yields "." or "..", so the explicit skips the
        // readdir version needed are gone rather than merely unnecessary.
        std::error_code ec;
        for (const auto& sd : std::filesystem::directory_iterator(dir, ec)) {
            if (!sd.is_directory()) continue;
            const std::string sub = sd.path().filename().string();
            const std::string subdir = dir + sub + "/";
            std::error_code ec2;
            for (const auto& e : std::filesystem::directory_iterator(subdir, ec2)) {
                std::string n = e.path().filename().string();
                if (n.size() < 6 || n.substr(n.size() - 6) != ".odmap") continue;
                std::string path = subdir + n;
                std::string label = readNameFromOdmap(path, sub);
                m_importEntries.push_back({label + "  [custom]", path, false});
            }
        }
    }
    std::sort(m_importEntries.begin(), m_importEntries.end(),
             [](auto& a, auto& b) { return a.label < b.label; });
}

void MapEditor::drawImportDialog() {
    ClearBackground(Color{15, 15, 20, 255});
    float cx = m_screenW / 2.0f, cy = m_screenH / 2.0f - 160.0f;
    DrawText(T("Based on Existing Map"), cx - 130, cy - 40, 26, ACCENT);
    DrawText(T("Loads that map's terrain, provinces and countries as a starting point."), cx - 240, cy - 8, 12, GRAY);

    if (!m_importScanned) scanImportableMaps();

    Rectangle listRect = {cx - 220, cy + 20, 440, 300};
    DrawRectangleRounded(listRect, 0.05f, 4, Color{30,30,35,255});
    DrawRectangleRoundedLines(listRect, 0.05f, 4, Color{80,80,90,255});

    if (m_importEntries.empty()) {
        DrawText(T("No maps found in STDmaps/ or custom_maps/"), (int)listRect.x + 16, (int)listRect.y + 16, 14, GRAY);
    } else {
        const int itemH = 26;
        int visible = (int)listRect.height / itemH;
        Vector2 mouse = GetMousePosition();
        if (CheckCollisionPointRec(mouse, listRect)) m_importScroll -= (int)GetMouseWheelMove();
        m_importScroll = std::max(0, std::min(m_importScroll, std::max(0, (int)m_importEntries.size() - visible)));
        for (int i = m_importScroll; i < (int)m_importEntries.size() && i < m_importScroll + visible; ++i) {
            Rectangle row = {listRect.x + 4, listRect.y + 4 + (i - m_importScroll) * itemH, listRect.width - 8, (float)(itemH - 2)};
            bool sel = (i == m_importChoice);
            bool hov = CheckCollisionPointRec(mouse, row);
            DrawRectangleRounded(row, 0.1f, 4, sel ? ColorAlpha(ACCENT, 0.2f) : (hov ? Color{255,255,255,16} : BLANK));
            DrawText(m_importEntries[i].label.c_str(), (int)row.x + 8, (int)row.y + 5, 13, sel ? ACCENT : (hov ? WHITE : LIGHTGRAY));
            if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_importChoice = i;
        }
    }

    cy += 340;
    // The list above can only show what is already in the data directory, so
    // a map that arrived by download or messenger has no row to click. This is
    // the way in for those, without asking the player to find data/custom_maps.
    if (fileDialog::available()) {
        Rectangle browseBtn = {cx - 220, cy, 200, 40};
        if (drawButton("Browse Files...", browseBtn, false, 16)) promptImportFromFile();
    }
    Rectangle loadBtn = {fileDialog::available() ? cx + 20 : cx - 100, cy, 200, 40};
    Rectangle cancelBtn = {cx - 100, cy + 48, 200, 36};
    bool canLoad = m_importChoice >= 0 && m_importChoice < (int)m_importEntries.size();
    if (drawButton("Load", loadBtn, false, 18) && canLoad) {
        if (loadExistingMap(m_importEntries[m_importChoice].path)) {
            std::string base = m_importEntries[m_importChoice].label;
            size_t bracket = base.find("  [");
            if (bracket != std::string::npos) base = base.substr(0, bracket);
            m_mapName = base + " (copy)";
            m_projectState = PROJ_EDITING;
        } else {
            m_warningMsg = "Failed to load that map";
            m_warningTimer = 3.0f;
        }
    }
    if (drawButton("Cancel", cancelBtn, false, 16)) { m_projectState = PROJ_CREATE; }
    if (m_warningTimer > 0) {
        int tw = MeasureText(m_warningMsg.c_str(), 14);
        DrawText(m_warningMsg.c_str(), (int)(cx - tw / 2), (int)cy + 92, 14, Color{255, 120, 120, 255});
    }
}

// ════════════════════════════════════════════════════════════════
//  Files in and out of the game
// ════════════════════════════════════════════════════════════════

// Lowercase extension including the dot, "" if the name has none.
static std::string extensionOf(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return ext;
}

bool MapEditor::importFromPath(const std::string& path) {
    if (path.empty()) return false;
    const std::string ext = extensionOf(path);
    // A project carries the editor's own state (undo-able game data, scripts,
    // generator settings); a map is the packaged result. Both are zips and
    // both are things a player will drag in, so accept either and pick the
    // loader by extension rather than making them remember which is which.
    bool ok = false;
    if (ext == ".uodmap") {
        ok = loadProject(path);
    } else if (ext == ".odmap") {
        ok = loadExistingMap(path);
        if (ok) {
            std::string base = fs::path(path).stem().string();
            m_mapName = base.empty() ? "Imported Map" : base;
        }
    } else {
        m_warningMsg = "Not a map file (need .odmap or .uodmap)";
        m_warningTimer = 3.0f;
        return false;
    }

    if (ok) {
        m_projectState = PROJ_EDITING;
        // Anything from outside data/projects has no project file behind it --
        // saveProject() writes to data/projects/<name>.uodmap, not back to
        // where this came from. Marking it unsaved is what makes the exit
        // guard offer to put a copy there, which is the whole point of having
        // imported it. A project opened from that folder by way of the file
        // dialog is already where it belongs, so it stays clean.
        std::error_code ec;
        fs::path projects = fs::path(m_dataDir) / "projects";
        m_dirty = !fs::equivalent(fs::path(path).parent_path(), projects, ec);
        m_saveStatus = "Imported " + fs::path(path).filename().string();
        m_saveStatusTimer = 3.0f;
    } else {
        m_warningMsg = "Could not read " + fs::path(path).filename().string();
        m_warningTimer = 3.0f;
    }
    return ok;
}

void MapEditor::promptImportFromFile() {
    std::string path = fileDialog::open("Open a map or project", {"odmap", "uodmap"});
    if (!path.empty()) importFromPath(path);
}

void MapEditor::promptExportToFile() {
    if (!m_hasProvinces) {
        m_warningMsg = "Generate provinces before exporting";
        m_warningTimer = 3.0f;
        return;
    }
    std::string suggested = (m_mapName.empty() ? std::string("map") : m_mapName) + ".odmap";
    std::string dest = fileDialog::save("Export map", suggested, "odmap");
    if (dest.empty()) return;  // cancelled
    std::string written = exportODMap(dest);
    if (written.empty()) {
        m_warningMsg = "Could not write " + fs::path(dest).filename().string();
        m_warningTimer = 3.0f;
    } else {
        m_saveStatus = "Exported to " + fs::path(written).filename().string();
        m_saveStatusTimer = 4.0f;
    }
}

bool MapEditor::loadExistingMap(const std::string& path) {
    drawMiniLoadingScreen(0.05f, "Importing map...");
    // On the web the shipped scenarios are fetched rather than preloaded, and
    // this is the one place the editor opens one. Blocking, behind the mini
    // loading screen just drawn. A no-op everywhere else. See WebAssets.h.
    odEnsureAsset(path);
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) return false;
    std::map<std::string, std::vector<uint8_t>> entries;
    int n = (int)mz_zip_reader_get_num_files(&zip);
    for (int i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (st.m_is_directory) continue;
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (!p) continue;
        entries[st.m_filename] = std::vector<uint8_t>((uint8_t*)p, (uint8_t*)p + sz);
        mz_free(p);
    }
    mz_zip_reader_end(&zip);

    auto getStr = [&](const std::string& name) -> std::string {
        auto it = entries.find(name);
        if (it == entries.end()) return {};
        return std::string(it->second.begin(), it->second.end());
    };

    auto lsIt = entries.find("land_sea.png");
    auto ppIt = entries.find("provinces.png");
    std::string provJson = getStr("provinces.json");
    std::string countryJson = getStr("countries.json");
    if (lsIt == entries.end() || ppIt == entries.end() || provJson.empty() || countryJson.empty())
        return false;

    Image ls = LoadImageFromMemory(".png", lsIt->second.data(), (int)lsIt->second.size());
    if (ls.data == nullptr) return false;
    if (ls.width != MAP_W || ls.height != MAP_H) { UnloadImage(ls); return false; }
    ImageFormat(&ls, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    Image pp = LoadImageFromMemory(".png", ppIt->second.data(), (int)ppIt->second.size());
    if (pp.data == nullptr || pp.width != MAP_W || pp.height != MAP_H) {
        UnloadImage(ls); if (pp.data) UnloadImage(pp);
        return false;
    }
    ImageFormat(&pp, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    initBlankMap(); // ensures renderer + m_pixels exist, resets to a clean slate
    memcpy(m_pixels.data(), ls.data, (size_t)MAP_W * MAP_H * sizeof(Color));
    UnloadImage(ls);
    // Sync the CPU-side land/sea map immediately — everything below (political
    // regen, sanitizeProvincePixels' land test) reads m_editLandSea, and
    // leaving it at initBlankMap()'s all-sea default corrupts the whole load
    // (every pixel reads as "not land", so province pixels get stripped).
    m_editLandSea.updatePixels(m_pixels.data());

    m_provincePixels.resize(MAP_W * MAP_H);
    memcpy(m_provincePixels.data(), pp.data, (size_t)MAP_W * MAP_H * sizeof(Color));
    m_editProvinces.clear();
    m_editProvinces.loadFromMemory(ppIt->second.data(), (int)ppIt->second.size(), provJson);
    m_editCountries.clear();
    bool countriesOk = m_editCountries.loadFromJson(countryJson);
    if (!countriesOk || m_editCountries.getAll().empty())
        LoadLog() << "  WARNING: countries.json failed to parse or was empty\n";
    m_provinceJson = provJson;
    m_countryJson = countryJson;

    // Flag images referenced by countries.json ("image": "flags/xxx.png") are
    // packed inside the .odmap archive, not present on disk — CountryMap just
    // stores that zip-relative path verbatim, and the flag preview code later
    // does LoadImage() straight from disk, which fails for anything that
    // isn't a real filesystem path. Extract each referenced flag out of the
    // archive we already read into `entries` and repoint imagePath at the
    // extracted file so preview/export both keep working.
    {
        fs::create_directories(m_dataDir + "projects/flags/");
        auto extractFlag = [&](std::string& imagePath, int cid, const char* suffix) {
            if (imagePath.empty()) return;
            std::string key = imagePath;
            auto it = entries.find(key);
            if (it == entries.end() && !key.empty() && key[0] == '/') {
                key = key.substr(1);
                it = entries.find(key);
            }
            if (it == entries.end()) { imagePath.clear(); return; }
            std::string ext = ".png";
            auto dot = key.find_last_of('.');
            if (dot != std::string::npos) ext = key.substr(dot);
            std::string dest = m_dataDir + "projects/flags/imp_" + std::to_string(cid) + suffix + ext;
            std::ofstream out(dest, std::ios::binary);
            out.write((const char*)it->second.data(), (std::streamsize)it->second.size());
            out.close();
            imagePath = dest;
        };
        for (auto& [cid, c] : m_editCountries.getAll()) {
            extractFlag(c.flagActual.imagePath, cid, "_a");
            extractFlag(c.flagCensored.imagePath, cid, "_c");
        }
    }
    drawMiniLoadingScreen(0.4f, "Loading provinces and countries...");

    // No political.png is read, even from an archive that still carries one.
    // computePoliticalGradient() below redraws this layer unconditionally --
    // it always did -- so decoding a 4.8 MB PNG here only ever produced pixels
    // that were overwritten a few hundred lines later. Sizing the buffer is
    // all that pass needs from us.
    m_politicalPixels.resize(MAP_W * MAP_H);
    UnloadImage(pp);

    m_populationJson = getStr("population.json");
    m_resourcesJson = getStr("resources.json");
    m_minoritiesJson = getStr("minorities.json");
    m_minorityColorsJson = getStr("minority_colors.json");
    m_hasProvinces = true;
    m_hasGameData = !m_populationJson.empty();

    parseGeneratedGameData(); // resources/population/minorities -> m_provinceData

    auto parseIsoKeyedArmies = [&]() {
        std::string armies = getStr("armies.json");
        if (armies.empty()) return;
        try {
            auto j = nlohmann::json::parse(armies);
            for (auto& [pidStr, units] : j.items()) {
                int pid = std::stoi(pidStr);
                Province* prov = m_editProvinces.getProvinceById(pid);
                int ownerCid = prov ? prov->countryId : 0;
                auto& troops = m_provinceData[pid].troops;
                troops.clear();
                for (auto& u : units)
                    troops.push_back(ArmyUnit{u.value("country_id", ownerCid), u.value("count", 0)});
            }
        } catch (...) { LoadLog() << "  Bad armies.json in imported map\n"; }
    };
    parseIsoKeyedArmies();
    try {
        std::string ports = getStr("ports.json");
        if (!ports.empty()) {
            auto j = nlohmann::json::parse(ports);
            for (auto& [pidStr, info] : j.items())
                m_provinceData[std::stoi(pidStr)].portLevel = info.value("level", 1);
        }
    } catch (...) { LoadLog() << "  Bad ports.json in imported map\n"; }
    try {
        std::string pc = getStr("political_compass.json");
        if (!pc.empty()) {
            auto j = nlohmann::json::parse(pc);
            for (auto& [pidStr, comp] : j.items()) {
                EditorProvinceData& d = m_provinceData[std::stoi(pidStr)];
                // "left" and "auth" are the file's convention: positive is
                // further left, further authoritarian. compassEconomic and
                // compassSocial are PoliticalCompass's: -100 left to +100
                // right, -100 authoritarian to +100 libertarian. Opposite on
                // both axes, so the sign flips crossing the boundary.
                d.compassEconomic = -comp.value("left", 0.0f);
                d.compassSocial = -comp.value("auth", 0.0f);
            }
        }
    } catch (...) { LoadLog() << "  Bad political_compass.json in imported map\n"; }

    m_editorShips.clear();
    try {
        std::string ships = getStr("ships.json");
        if (!ships.empty()) {
            auto j = nlohmann::json::parse(ships);
            for (auto& e : j) {
                NavyShip s;
                s.countryId = e.value("country_id", 0);
                s.type = e.value("type", "destroyer");
                s.lat = e.value("lat", 0.0);
                s.lon = e.value("lon", 0.0);
                s.health = e.value("health", 100);
                s.crew = e.value("crew", 0);
                m_editorShips.push_back(s);
            }
        }
    } catch (...) { LoadLog() << "  Bad ships.json in imported map\n"; }

    m_editorRelations.clear();
    try {
        std::string rels = getStr("relations.json");
        if (!rels.empty()) {
            std::unordered_map<std::string, int> cidByIso;
            for (auto& [cid, c] : m_editCountries.getAll())
                if (!c.isoA3.empty()) cidByIso[c.isoA3] = cid;
            auto j = nlohmann::json::parse(rels);
            for (auto& [isoA, targets] : j.items()) {
                auto a = cidByIso.find(isoA);
                if (a == cidByIso.end()) continue;
                for (auto& [isoB, r] : targets.items()) {
                    auto b = cidByIso.find(isoB);
                    if (b == cidByIso.end()) continue;
                    CountryRelation cr;
                    cr.war = r.value("war", false);
                    cr.alliance = r.value("ally", false);
                    cr.nonAggression = r.value("nonAggression", false);
                    cr.guarantee = r.value("guarantee", false);
                    if (!cr.war && !cr.alliance && !cr.nonAggression && !cr.guarantee) continue;
                    auto key = std::make_pair(std::min(a->second, b->second), std::max(a->second, b->second));
                    m_editorRelations[key] = cr;
                }
            }
        }
    } catch (...) { LoadLog() << "  Bad relations.json in imported map\n"; }

    m_countryPolicies.clear();
    m_ethnicityPolicies.clear();
    m_ethnicRelations.clear();
    m_thumbnailPath.clear();
    m_thumbnailTexPath.clear();
    m_editorClaims.clear();
    m_claimsPixels.clear();
    m_claimsOverlayCid = -2;
    m_editorPolicies.clear();
    m_editorPoliciesLoaded = false;

    // Claims must be parsed after the reset block above, which clears them.
    try {
        std::string cj = getStr("claims.json");
        if (!cj.empty()) {
            std::unordered_map<std::string, int> cidByIso;
            for (auto& [cid, c] : m_editCountries.getAll())
                if (!c.isoA3.empty()) cidByIso[c.isoA3] = cid;
            auto j = nlohmann::json::parse(cj);
            for (auto& [iso, entry] : j.items()) {
                auto it2 = cidByIso.find(iso);
                if (it2 == cidByIso.end()) continue;
                // Both shapes the game accepts: a bare array, or {"provinces": [...]}
                const nlohmann::json* arr = nullptr;
                if (entry.is_array()) arr = &entry;
                else if (entry.is_object() && entry.contains("provinces")) arr = &entry["provinces"];
                if (!arr) continue;
                for (auto& v : *arr) m_editorClaims[it2->second].insert(v.get<int>());
            }
        }
    } catch (...) { LoadLog() << "  Bad claims.json in imported map\n"; }
    try {
        std::string pol = getStr("policies.json");
        if (!pol.empty()) {
            auto j = nlohmann::json::parse(pol);
            for (auto& p : j["policies"]) {
                Policy policy;
                policy.id = p.value("id", "");
                policy.name = p.value("name", "");
                policy.category = p.value("category", "");
                policy.folder = p.value("folder", "");
                policy.description = p.value("description", "");
                policy.costPerTurn = p.value("cost_per_turn", 0);
                policy.implementationTurns = p.value("implementation_turns", 3);
                policy.econShift = p.value("compass_shift", nlohmann::json::object()).value("economic", 0.0f);
                policy.socShift = p.value("compass_shift", nlohmann::json::object()).value("social", 0.0f);
                if (p.contains("incompatible_with"))
                    for (auto& inc : p["incompatible_with"]) policy.incompatibleWith.push_back(inc.get<std::string>());
                m_editorPolicies.push_back(policy);
            }
            m_editorPoliciesLoaded = true;
        }
        std::string sp = getStr("starting_policies.json");
        if (!sp.empty()) {
            std::unordered_map<std::string, int> cidByIso;
            for (auto& [cid, c] : m_editCountries.getAll())
                if (!c.isoA3.empty()) cidByIso[c.isoA3] = cid;
            auto j = nlohmann::json::parse(sp);
            if (j.contains("starting_policies")) {
                for (auto& [iso, ids] : j["starting_policies"].items()) {
                    auto it = cidByIso.find(iso);
                    if (it == cidByIso.end()) continue;
                    for (auto& id : ids) m_countryPolicies[it->second].push_back(id.get<std::string>());
                }
            }
        }
    } catch (...) { LoadLog() << "  Bad policies.json in imported map\n"; }

    drawMiniLoadingScreen(0.8f, "Building political map...");
    rebuildProvinceCounts();
    sanitizeProvincePixels();
    computePoliticalGradient(); // match the in-game border-glow rendering
    m_polGradientDirty = false; m_polGradientFullDirty = false; // supersedes any pending partial refresh
    rebuildFromPixelState();
    m_hlDirty = true;
    m_dirty = true; // this is a starting point, not a saved project yet
    drawMiniLoadingScreen(1.0f, "Ready");
    LoadLog() << "  Imported existing map from " << path << "\n";
    return true;
}

// ════════════════════════════════════════════════════════════════
//  Map initialization
// ════════════════════════════════════════════════════════════════

void MapEditor::initBlankMap() {
    m_pixels.resize(MAP_W * MAP_H, COL_SEA);
    m_editLandSea.setFromPixels(m_pixels.data(), MAP_W, MAP_H);
    if (!m_renderer) {
        m_renderer = new MapRenderer(m_canvasW, m_canvasH, MAP_W, MAP_H);
        m_renderer->setMaxZoom(20.0f);
    }
    m_renderer->resize(m_canvasW, m_canvasH);
    m_dirty = false;
}

// ════════════════════════════════════════════════════════════════
//  Brush operations
// ════════════════════════════════════════════════════════════════

void MapEditor::applyBrush(int cx, int cy, bool land) {
    int r = m_brushSize;
    Color c = land ? COL_LAND : COL_SEA;
    const Color SEA_POL = {35, 60, 80, 255};
    const Color UNASSIGNED_POL = {140, 140, 140, 255};
    int minX = MAP_W, maxX = 0;
    int minY = std::max(0, cy - r), maxY = std::min(MAP_H - 1, cy + r);
    bool wrapped = false, provDirty = false;
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= MAP_H) continue;
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy > r*r) continue;
            int px = (cx + dx) % MAP_W;
            if (px < 0) px += MAP_W;
            if (cx + dx != px) wrapped = true;
            int idx = py * MAP_W + px;
            m_pixels[idx] = c;
            // Live province/political feedback while terraforming: sea eats
            // provinces immediately; fresh land shows unassigned grey until
            // localAssignNewLand below pulls it into a nearby province
            if (m_hasProvinces && !m_provincePixels.empty()) {
                const Color& pp = m_provincePixels[idx];
                if (!land) {
                    int pid = Province::colorToId(pp.r, pp.g, pp.b);
                    if (pid != 0) {
                        m_provincePixelCounts[pid]--;
                        m_provincePixels[idx] = Color{0, 0, 0, 0};
                        m_politicalPixels[idx] = SEA_POL;
                        provDirty = true;
                    }
                } else {
                    if (Province::colorToId(pp.r, pp.g, pp.b) == 0) {
                        const Color& pol = m_politicalPixels[idx];
                        if (memcmp(&pol, &UNASSIGNED_POL, sizeof(Color)) != 0) {
                            m_politicalPixels[idx] = UNASSIGNED_POL;
                            provDirty = true;
                        }
                    }
                }
            }
            if (px < minX) minX = px;
            if (px > maxX) maxX = px;
        }
    }
    trackChange();
    if (!wrapped && minX <= maxX && maxY >= minY) {
        int uw = maxX - minX + 1;
        int uh = maxY - minY + 1;
        std::vector<Color> rectData(uw * uh);
        for (int row = 0; row < uh; row++)
            memcpy(&rectData[row * uw], &m_pixels[(minY + row) * MAP_W + minX], uw * sizeof(Color));
        m_editLandSea.updatePixelsRect(rectData.data(), minX, minY, uw, uh);
        if (m_hasProvinces && !m_provincePixels.empty()) {
            growStrokeBBox(minX, minY, maxX, maxY);
            // New coastal land joins adjacent provinces in realtime; detached
            // land (a new island) stays grey and becomes its own province on
            // stroke end (finalizeLandStroke)
            int margin = 12;
            int ax0 = std::max(0, minX - margin), ay0 = std::max(0, minY - margin);
            int ax1 = std::min(MAP_W - 1, maxX + margin), ay1 = std::min(MAP_H - 1, maxY + margin);
            if (land) localAssignNewLand(ax0, ay0, ax1, ay1);
            if (provDirty || land) liveUpdateRegion(ax0, ay0, ax1, ay1);
        }
    } else {
        m_editLandSea.updatePixels(m_pixels.data());
        if (m_hasProvinces && wrapped) {
            m_strokeWrapped = true;
            growStrokeBBox(0, minY, MAP_W - 1, maxY);
        }
    }
}

void MapEditor::liveUpdateRegion(int minX, int minY, int maxX, int maxY) {
    if (!m_renderer || m_politicalPixels.empty() || minX > maxX || minY > maxY) return;
    int w = maxX - minX + 1, h = maxY - minY + 1;
    std::vector<Color> rect((size_t)w * h);
    for (int row = 0; row < h; ++row)
        memcpy(&rect[(size_t)row * w], &m_politicalPixels[(size_t)(minY + row) * MAP_W + minX],
               (size_t)w * sizeof(Color));
    m_renderer->updatePoliticalTextureRec(rect.data(), minX, minY, w, h);
    m_renderer->updateBorderRegion(m_provincePixels.data(), MAP_W, MAP_H, minX, minY, w, h);
}

void MapEditor::applyProvinceBrush(int cx, int cy) {
    Province* prov = m_editProvinces.getProvinceById(m_selectedProvince);
    if (!prov || m_provincePixels.empty()) return;
    Color pc = {prov->r, prov->g, prov->b, 255};
    Color ownerCol = {140, 140, 140, 255};
    if (const Country* oc = m_editCountries.getCountry(prov->countryId))
        ownerCol = oc->color;
    int r = m_brushSize;
    int minX = MAP_W, maxX = -1;
    int minY = std::max(0, cy - r), maxY = std::min(MAP_H - 1, cy + r);
    bool wrapped = false, painted = false;
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= MAP_H) continue;
        for (int dx = -r; dx <= r; dx++) {
            if (dx*dx + dy*dy > r*r) continue;
            int px = (cx + dx) % MAP_W;
            if (px < 0) px += MAP_W;
            if (cx + dx != px) wrapped = true;
            int idx = py * MAP_W + px;
            if (!m_editLandSea.isLand(px, py)) continue; // sea can't join a province
            Color cur = m_provincePixels[idx];
            if (cur.r == pc.r && cur.g == pc.g && cur.b == pc.b) continue;
            int oldPid = Province::colorToId(cur.r, cur.g, cur.b);
            if (oldPid != 0) m_provincePixelCounts[oldPid]--;
            m_provincePixelCounts[prov->id]++;
            m_provincePixels[idx] = pc;
            m_politicalPixels[idx] = ownerCol;
            painted = true;
            if (px < minX) minX = px;
            if (px > maxX) maxX = px;
        }
    }
    if (!painted) return;
    if (wrapped) {
        m_strokeWrapped = true;
        growStrokeBBox(0, minY, MAP_W - 1, maxY);
        return; // seam-crossing dab: the release commit refreshes textures
    }
    growStrokeBBox(minX, minY, maxX, maxY);
    // Live feedback: patch the dirty region's textures each frame
    liveUpdateRegion(minX, minY, maxX, maxY);
}

void MapEditor::applyProvinceRect(int x1, int y1, int x2, int y2) {
    Province* prov = m_editProvinces.getProvinceById(m_selectedProvince);
    if (!prov || m_provincePixels.empty()) return;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(MAP_W - 1, x2); y2 = std::min(MAP_H - 1, y2);
    if (x1 > x2 || y1 > y2) return;
    Color pc = {prov->r, prov->g, prov->b, 255};
    Color ownerCol = {140, 140, 140, 255};
    if (const Country* oc = m_editCountries.getCountry(prov->countryId)) ownerCol = oc->color;
    bool painted = false;
    for (int py = y1; py <= y2; ++py) {
        for (int px = x1; px <= x2; ++px) {
            int idx = py * MAP_W + px;
            if (!m_editLandSea.isLand(px, py)) continue;
            const Color& cur = m_provincePixels[idx];
            if (cur.r == pc.r && cur.g == pc.g && cur.b == pc.b) continue;
            int oldPid = Province::colorToId(cur.r, cur.g, cur.b);
            if (oldPid != 0) m_provincePixelCounts[oldPid]--;
            m_provincePixelCounts[prov->id]++;
            m_provincePixels[idx] = pc;
            m_politicalPixels[idx] = ownerCol;
            painted = true;
        }
    }
    if (!painted) return;
    liveUpdateRegion(x1, y1, x2, y2);
    m_editProvinces.updatePixelsRect(m_provincePixels.data(), x1, y1, x2 - x1 + 1, y2 - y1 + 1);
    garbageCollectProvinces();
    trackChange();
    m_hlDirty = true;
    markPoliticalDirty(x1, y1, x2, y2);
}

// Fill tool for provinces: click a contiguous chunk of land (a piece of some
// other province, or unassigned land) and hand the whole chunk to the
// selected province — e.g. grab the separated half of a split province.
void MapEditor::applyProvinceFill(int cx, int cy) {
    Province* prov = m_editProvinces.getProvinceById(m_selectedProvince);
    if (!prov || m_provincePixels.empty()) return;
    if (cx < 0 || cx >= MAP_W || cy < 0 || cy >= MAP_H) return;
    if (!m_editLandSea.isLand(cx, cy)) return;
    int start = cy * MAP_W + cx;
    const Color& sc = m_provincePixels[start];
    int targetPid = Province::colorToId(sc.r, sc.g, sc.b);
    if (targetPid == prov->id) return; // already ours

    Color pc = {prov->r, prov->g, prov->b, 255};
    Color ownerCol = {140, 140, 140, 255};
    if (const Country* oc = m_editCountries.getCountry(prov->countryId)) ownerCol = oc->color;

    // Flood the contiguous same-province (or unassigned) land component
    int bx0 = cx, by0 = cy, bx1 = cx, by1 = cy;
    int reassigned = 0;
    std::vector<int> stack{start};
    std::unordered_set<int> visited{start};
    while (!stack.empty()) {
        int idx = stack.back(); stack.pop_back();
        const Color& cur = m_provincePixels[idx];
        if (Province::colorToId(cur.r, cur.g, cur.b) != targetPid) continue;
        m_provincePixels[idx] = pc;
        m_politicalPixels[idx] = ownerCol;
        reassigned++;
        int x = idx % MAP_W, y = idx / MAP_W;
        bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
        by0 = std::min(by0, y); by1 = std::max(by1, y);
        int nbs[4][2] = {{x-1,y},{x+1,y},{x,y-1},{x,y+1}};
        for (auto& nb : nbs) {
            int nx = nb[0], ny = nb[1];
            if (nx < 0) nx = MAP_W - 1; else if (nx >= MAP_W) nx = 0;
            if (ny < 0 || ny >= MAP_H) continue;
            int ni = ny * MAP_W + nx;
            if (visited.count(ni)) continue;
            if (!m_editLandSea.isLand(nx, ny)) continue;
            const Color& n = m_provincePixels[ni];
            if (Province::colorToId(n.r, n.g, n.b) != targetPid) continue;
            visited.insert(ni);
            stack.push_back(ni);
        }
    }
    if (reassigned == 0) return;
    if (targetPid != 0) m_provincePixelCounts[targetPid] -= reassigned;
    m_provincePixelCounts[prov->id] += reassigned;
    liveUpdateRegion(bx0, by0, bx1, by1);
    m_editProvinces.updatePixelsRect(m_provincePixels.data(), bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
    garbageCollectProvinces();
    trackChange();
    m_hlDirty = true;
    markPoliticalDirty(bx0, by0, bx1, by1);
}

// Countries-tab paint tool: drag over provinces to hand them to whichever
// country is currently selected in the side panel. Reassigns at most once
// per province per stroke (m_countryBrushTouched), since each reassignment
// scans the map to recolor that province's political pixels.
void MapEditor::applyCountryOwnershipBrush(int cx, int cy) {
    if (!m_hasProvinces || m_selectedCountry < 0 || m_provincePixels.empty()) return;
    if (cx < 0 || cx >= MAP_W || cy < 0 || cy >= MAP_H) return;
    if (!m_editLandSea.isLand(cx, cy)) return;
    const Province* pc = m_editProvinces.getProvince(cx, cy);
    if (!pc) return;
    Province* prov = m_editProvinces.getProvinceById(pc->id);
    if (!prov || prov->countryId == m_selectedCountry) return;
    if (m_countryBrushTouched.count(prov->id)) return;
    m_countryBrushTouched.insert(prov->id);

    const Country* nc = m_editCountries.getCountry(m_selectedCountry);
    if (!nc) return;
    Color ownerCol = nc->color;
    int targetPid = prov->id;
    prov->countryId = m_selectedCountry;

    // Flood the contiguous same-province component from the click point
    // (same approach as applyProvinceFill) instead of scanning the whole
    // map — touches only this province's own pixels, not all 33M map pixels.
    int start = cy * MAP_W + cx;
    int bx0 = cx, by0 = cy, bx1 = cx, by1 = cy;
    std::vector<int> stack{start};
    std::unordered_set<int> visited{start};
    while (!stack.empty()) {
        int idx = stack.back(); stack.pop_back();
        const Color& cur = m_provincePixels[idx];
        if (Province::colorToId(cur.r, cur.g, cur.b) != targetPid) continue;
        m_politicalPixels[idx] = ownerCol;
        int x = idx % MAP_W, y = idx / MAP_W;
        bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
        by0 = std::min(by0, y); by1 = std::max(by1, y);
        int nbs[4][2] = {{x-1,y},{x+1,y},{x,y-1},{x,y+1}};
        for (auto& nb : nbs) {
            int nx = nb[0], ny = nb[1];
            if (nx < 0) nx = MAP_W - 1; else if (nx >= MAP_W) nx = 0;
            if (ny < 0 || ny >= MAP_H) continue;
            int ni = ny * MAP_W + nx;
            if (visited.count(ni)) continue;
            const Color& n = m_provincePixels[ni];
            if (Province::colorToId(n.r, n.g, n.b) != targetPid) continue;
            visited.insert(ni);
            stack.push_back(ni);
        }
    }
    liveUpdateRegion(bx0, by0, bx1, by1);

    try {
        auto j = nlohmann::json::parse(m_provinceJson.empty() ? "{}" : m_provinceJson);
        std::string key = std::to_string(prov->id);
        if (j.contains(key)) j[key]["country_id"] = m_selectedCountry;
        m_provinceJson = j.dump(2);
    } catch (...) {}

    trackChange();
    m_hlDirty = true;
    markPoliticalDirty(bx0, by0, bx1, by1);
}

// ── Claims painting ─────────────────────────────────────────────
// A claim is one country asserting a right to a province (usually one it
// doesn't own). Painted per-province like the ownership brush, but it only
// touches the claims overlay — province ownership is left alone.

static const Color CLAIM_COL = {255, 70, 70, 150}; // translucent red wash

void MapEditor::paintClaimsProvince(int pid, bool claimed, int& bx0, int& by0, int& bx1, int& by1) {
    if (m_claimsPixels.empty()) return;
    Color c = claimed ? CLAIM_COL : Color{0, 0, 0, 0};
    for (int y = 0; y < MAP_H; ++y) {
        const Color* row = &m_provincePixels[(size_t)y * MAP_W];
        for (int x = 0; x < MAP_W; ++x) {
            if (Province::colorToId(row[x].r, row[x].g, row[x].b) != pid) continue;
            m_claimsPixels[(size_t)y * MAP_W + x] = c;
            bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
            by0 = std::min(by0, y); by1 = std::max(by1, y);
        }
    }
}

void MapEditor::rebuildClaimsOverlay() {
    if (!m_renderer || !m_hasProvinces || m_provincePixels.empty()) return;
    if (m_claimsPixels.size() != (size_t)MAP_W * MAP_H)
        m_claimsPixels.assign((size_t)MAP_W * MAP_H, Color{0, 0, 0, 0});
    else
        std::fill(m_claimsPixels.begin(), m_claimsPixels.end(), Color{0, 0, 0, 0});

    // Only the selected country's claims are shown — overlapping claims from
    // every country at once would be unreadable.
    auto it = m_editorClaims.find(m_selectedCountry);
    if (it != m_editorClaims.end() && !it->second.empty()) {
        const std::set<int>& pids = it->second;
        for (int y = 0; y < MAP_H; ++y) {
            const Color* row = &m_provincePixels[(size_t)y * MAP_W];
            for (int x = 0; x < MAP_W; ++x) {
                int pid = Province::colorToId(row[x].r, row[x].g, row[x].b);
                if (pid != 0 && pids.count(pid))
                    m_claimsPixels[(size_t)y * MAP_W + x] = CLAIM_COL;
            }
        }
    }

    if (!m_renderer->hasClaimsTexture()) {
        Image img{};
        img.data = m_claimsPixels.data();
        img.width = MAP_W;
        img.height = MAP_H;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D t = LoadTextureFromImage(img);
        SetTextureFilter(t, TEXTURE_FILTER_POINT);
        m_renderer->setClaimsTexture(t);
    } else {
        m_renderer->updateClaimsTexture(m_claimsPixels.data());
    }
    m_claimsOverlayCid = m_selectedCountry;
}

void MapEditor::applyClaimsBrush(int cx, int cy) {
    if (!m_hasProvinces || m_selectedCountry < 0 || m_provincePixels.empty()) return;
    if (cx < 0 || cx >= MAP_W || cy < 0 || cy >= MAP_H) return;
    if (!m_editLandSea.isLand(cx, cy)) return;
    const Province* pc = m_editProvinces.getProvince(cx, cy);
    if (!pc) return;
    int pid = pc->id;
    if (m_claimsBrushTouched.count(pid)) return; // one toggle per province per stroke
    m_claimsBrushTouched.insert(pid);

    auto& claims = m_editorClaims[m_selectedCountry];
    bool nowClaimed;
    if (m_claimsBrushErase) {
        if (!claims.erase(pid)) return; // wasn't claimed — nothing to redraw
        nowClaimed = false;
    } else {
        if (!claims.insert(pid).second) return; // already claimed
        nowClaimed = true;
    }

    int bx0 = MAP_W, by0 = MAP_H, bx1 = -1, by1 = -1;
    paintClaimsProvince(pid, nowClaimed, bx0, by0, bx1, by1);
    if (bx1 >= 0 && m_renderer && m_renderer->hasClaimsTexture()) {
        int rw = bx1 - bx0 + 1, rh = by1 - by0 + 1;
        std::vector<Color> rect((size_t)rw * rh);
        for (int row = 0; row < rh; ++row)
            memcpy(&rect[(size_t)row * rw], &m_claimsPixels[(size_t)(by0 + row) * MAP_W + bx0],
                   (size_t)rw * sizeof(Color));
        m_renderer->updateClaimsTextureRec(rect.data(), bx0, by0, rw, rh);
    }
    trackChange();
}

// {ISO: [pid, ...]} — the shape Game::loadGameData() parses into m_claims.
std::string MapEditor::buildClaimsJson() const {
    nlohmann::json root = nlohmann::json::object();
    for (auto& [cid, pids] : m_editorClaims) {
        if (pids.empty()) continue;
        const Country* c = m_editCountries.getCountry(cid);
        if (!c || c->isoA3.empty()) continue;
        nlohmann::json arr = nlohmann::json::array();
        for (int pid : pids) arr.push_back(pid);
        root[c->isoA3] = arr;
    }
    if (root.empty()) return std::string();
    return root.dump(2);
}

void MapEditor::commitProvincePixels() {
    if (!m_renderer) return;
    m_editProvinces.updatePixels(m_provincePixels.data());
    m_renderer->updatePoliticalTexture(m_politicalPixels.data());
    m_renderer->computeBorderTexture(m_editProvinces.getImage());
    trackChange();
}

void MapEditor::removeProvinceEntry(int pid) {
    try {
        auto j = nlohmann::json::parse(m_provinceJson);
        j.erase(std::to_string(pid));
        m_provinceJson = j.dump(2);
    } catch (...) {}
    m_editProvinces.removeProvince(pid);
    m_provinceData.erase(pid);
    m_provincePixelCounts.erase(pid);
    if (m_selectedProvince == pid) m_selectedProvince = -1;
}

// ── WHICH NEIGHBOUR TO MERGE INTO IS THE MAP-MAKER'S CHOICE ──
//
// This used to count shared border pixels, take the winner, and say nothing.
// The longest border is a good guess and it is still the one offered first,
// but it is only a guess: a province carved out of a coastline usually borders
// the sea province most, and a corridor drawn between two countries borders
// the one it was never meant to join. Both cases silently ate the province
// into the wrong neighbour, and the only way back was undo.
//
// So the votes are all kept now and offered as a list. One candidate merges
// without asking -- there is no choice to make -- and none is still an error.
void MapEditor::deleteSelectedProvince() {
    if (m_selectedProvince < 0 || !m_hasProvinces || m_provincePixels.empty()) return;
    int pid = m_selectedProvince;

    // Collect the province's pixels and count the border shared with each
    // adjacent province.
    std::map<int, int> votes;
    std::vector<int> pixels;
    for (int y = 0; y < MAP_H; ++y) {
        for (int x = 0; x < MAP_W; ++x) {
            int i = y * MAP_W + x;
            const Color& c = m_provincePixels[i];
            if (Province::colorToId(c.r, c.g, c.b) != pid) continue;
            pixels.push_back(i);
            int nbs[4][2] = {{x-1,y},{x+1,y},{x,y-1},{x,y+1}};
            for (auto& nb : nbs) {
                int nx = nb[0], ny = nb[1];
                if (nx < 0) nx = MAP_W - 1; else if (nx >= MAP_W) nx = 0;
                if (ny < 0 || ny >= MAP_H) continue;
                const Color& n = m_provincePixels[ny * MAP_W + nx];
                int npid = Province::colorToId(n.r, n.g, n.b);
                if (npid != 0 && npid != pid) votes[npid]++;
            }
        }
    }
    if (pixels.empty()) { // ghost province (no pixels left) — just drop the entry
        removeProvinceEntry(pid);
        trackChange();
        return;
    }
    if (votes.empty()) {
        m_warningMsg = "Cannot delete: no adjacent province to merge into";
        m_warningTimer = 2.5f;
        return;
    }
    // Most shared border first: that is the old automatic choice, still the
    // sensible default, now the top row rather than the only outcome.
    std::vector<std::pair<int, int>> candidates(votes.begin(), votes.end());
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    if (candidates.size() == 1) { mergeProvinceInto(pid, candidates[0].first); return; }
    openMergePicker(pid, std::move(candidates));
}

// The merge itself, once a target is known -- either because there was only
// one candidate or because the picker was answered. Re-scans for the pixels
// rather than carrying them across the modal: the map-maker can paint while
// the picker is open, and a stale pixel list would recolour ground that has
// since moved.
void MapEditor::mergeProvinceInto(int pid, int target) {
    if (pid < 0 || target <= 0 || pid == target) return;
    if (!m_hasProvinces || m_provincePixels.empty()) return;
    Province* bp = m_editProvinces.getProvinceById(target);
    if (!bp) return;

    std::vector<int> pixels;
    for (int i = 0; i < (int)m_provincePixels.size(); ++i) {
        const Color& c = m_provincePixels[i];
        if (Province::colorToId(c.r, c.g, c.b) == pid) pixels.push_back(i);
    }

    Color bc = {bp->r, bp->g, bp->b, 255};
    Color ownerCol = {140, 140, 140, 255};
    if (const Country* oc = m_editCountries.getCountry(bp->countryId)) ownerCol = oc->color;
    for (int i : pixels) {
        m_provincePixels[i] = bc;
        m_politicalPixels[i] = ownerCol;
    }
    m_provincePixelCounts[target] += (int)pixels.size(); // absorbed pixels
    removeProvinceEntry(pid);
    m_selectedProvince = target;
    commitProvincePixels();
    m_hlDirty = true;
    markPoliticalDirtyFull(); // deletion can be large/scattered
}

void MapEditor::rebuildProvinceCounts() {
    m_provincePixelCounts.clear();
    for (const Color& c : m_provincePixels) {
        int pid = Province::colorToId(c.r, c.g, c.b);
        if (pid != 0) m_provincePixelCounts[pid]++;
    }
}

// The generator works at half resolution and upscales ×2, while land/sea is
// full resolution — coastlines end up with sea pixels that still carry a
// province color (uneditable specks) and land pixels with none. Strip the
// former, then run the orphan-land pass to fix the latter.
void MapEditor::computePoliticalGradient() {
    if (!m_hasProvinces || m_provincePixels.empty()) return;
    int total = MAP_W * MAP_H;

    // cid per pixel (0 = sea/unclaimed), same source data the game builds
    // m_pixelCountryArray from.
    std::vector<int> cidArr(total, 0);
    for (int i = 0; i < total; ++i) {
        const Color& pc = m_provincePixels[i];
        int pid = Province::colorToId(pc.r, pc.g, pc.b);
        if (pid == 0) continue;
        if (Province* prov = m_editProvinces.getProvinceById(pid)) cidArr[i] = prov->countryId;
    }

    // Multi-source BFS distance-to-border field, identical weights to
    // Game_Loading.cpp's gradient pass (orth +2, diag +3, capped at 60).
    std::vector<uint8_t> dist(total, 255);
    struct QE { int idx; uint8_t dist; };
    std::vector<QE> queue;
    auto enqueue = [&](int idx, uint8_t d) {
        if (idx < 0 || idx >= total) return;
        if (dist[idx] <= d) return;
        dist[idx] = d;
        queue.push_back({idx, d});
    };
    for (int y = 0; y < MAP_H; ++y) {
        for (int x = 0; x < MAP_W; ++x) {
            int i = y * MAP_W + x;
            int cid = cidArr[i];
            int nx4[4] = {x-1, x+1, x, x};
            int ny4[4] = {y, y, y-1, y+1};
            for (int k = 0; k < 4; ++k) {
                if (nx4[k] < 0 || nx4[k] >= MAP_W || ny4[k] < 0 || ny4[k] >= MAP_H) continue;
                if (cidArr[ny4[k] * MAP_W + nx4[k]] != cid) { enqueue(i, 0); break; }
            }
        }
    }
    size_t qpos = 0;
    while (qpos < queue.size()) {
        QE cur = queue[qpos++];
        if (cur.dist >= 60) continue;
        int x = cur.idx % MAP_W, y = cur.idx / MAP_W;
        int nx8[8] = {x-1, x+1, x, x, x-1, x-1, x+1, x+1};
        int ny8[8] = {y, y, y-1, y+1, y-1, y+1, y-1, y+1};
        for (int k = 0; k < 8; ++k) {
            if (nx8[k] < 0 || nx8[k] >= MAP_W || ny8[k] < 0 || ny8[k] >= MAP_H) continue;
            int step = (k < 4) ? 2 : 3;
            enqueue(ny8[k] * MAP_W + nx8[k], cur.dist + step);
        }
    }

    // Same blend formula as Game_Loading.cpp's static blendColor().
    auto blend = [](Color base, float t) -> Color {
        uint8_t r = (uint8_t)std::min(255, std::max(0, (int)(base.r * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
        uint8_t g = (uint8_t)std::min(255, std::max(0, (int)(base.g * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
        uint8_t b = (uint8_t)std::min(255, std::max(0, (int)(base.b * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
        return Color{r, g, b, 255};
    };
    for (int i = 0; i < total; ++i) {
        int cid = cidArr[i];
        float t = std::min(1.0f, dist[i] / 60.0f);
        if (cid <= 0) {
            uint8_t rb = (uint8_t)(8 + (uint8_t)((1.0f - t) * 16));
            uint8_t gb = (uint8_t)(10 + (uint8_t)((1.0f - t) * 22));
            uint8_t bb = (uint8_t)(15 + (uint8_t)((1.0f - t) * 38));
            m_politicalPixels[i] = {rb, gb, bb, 255};
        } else {
            const Country* c = m_editCountries.getCountry(cid);
            Color base = c ? c->color : Color{80, 80, 80, 255};
            m_politicalPixels[i] = blend(base, t);
        }
    }

    // 1px dark border line at country boundaries.
    for (int y = 0; y < MAP_H; ++y) {
        for (int x = 0; x < MAP_W; ++x) {
            int i = y * MAP_W + x;
            int cid = cidArr[i];
            if (cid <= 0) continue;
            int nx4[4] = {x-1, x+1, x, x};
            int ny4[4] = {y, y, y-1, y+1};
            for (int k = 0; k < 4; ++k) {
                if (nx4[k] < 0 || nx4[k] >= MAP_W || ny4[k] < 0 || ny4[k] >= MAP_H) continue;
                if (cidArr[ny4[k] * MAP_W + nx4[k]] != cid) {
                    Color& px = m_politicalPixels[i];
                    px = {(uint8_t)(px.r / 3), (uint8_t)(px.g / 3), (uint8_t)(px.b / 3), 255};
                    break;
                }
            }
        }
    }

    if (m_renderer) m_renderer->updatePoliticalTexture(m_politicalPixels.data());
}

void MapEditor::markPoliticalDirty(int x0, int y0, int x1, int y1) {
    if (m_polGradientFullDirty) return; // already covers everything
    if (!m_polGradientDirty) {
        m_polDirtyX0 = x0; m_polDirtyY0 = y0; m_polDirtyX1 = x1; m_polDirtyY1 = y1;
    } else {
        m_polDirtyX0 = std::min(m_polDirtyX0, x0); m_polDirtyY0 = std::min(m_polDirtyY0, y0);
        m_polDirtyX1 = std::max(m_polDirtyX1, x1); m_polDirtyY1 = std::max(m_polDirtyY1, y1);
    }
    m_polGradientDirty = true;
}

// Same algorithm as computePoliticalGradient(), but scoped to a padded
// rectangle instead of the whole map — a paint stroke only ever needs the
// gradient refreshed near where it touched, and this is what keeps
// per-stroke updates fast instead of a ~33M-pixel scan every mouse release.
void MapEditor::computePoliticalGradientRegion(int x0, int y0, int x1, int y1) {
    if (!m_hasProvinces || m_provincePixels.empty()) return;
    const int PAD = 32; // > 60/2 BFS cap, so edge-of-region distances still settle correctly
    x0 = std::max(0, x0 - PAD); y0 = std::max(0, y0 - PAD);
    x1 = std::min(MAP_W - 1, x1 + PAD); y1 = std::min(MAP_H - 1, y1 + PAD);
    int rw = x1 - x0 + 1, rh = y1 - y0 + 1;
    if (rw <= 0 || rh <= 0) return;

    auto cidAt = [&](int x, int y) -> int {
        if (x < 0) x += MAP_W; else if (x >= MAP_W) x -= MAP_W;
        if (y < 0 || y >= MAP_H) return 0;
        const Color& pc = m_provincePixels[(size_t)y * MAP_W + x];
        int pid = Province::colorToId(pc.r, pc.g, pc.b);
        if (pid == 0) return 0;
        Province* prov = m_editProvinces.getProvinceById(pid);
        return prov ? prov->countryId : 0;
    };

    std::vector<int> cidArr((size_t)rw * rh);
    for (int ly = 0; ly < rh; ++ly)
        for (int lx = 0; lx < rw; ++lx)
            cidArr[(size_t)ly * rw + lx] = cidAt(x0 + lx, y0 + ly);

    std::vector<uint8_t> dist((size_t)rw * rh, 255);
    struct QE { int idx; uint8_t dist; };
    std::vector<QE> queue;
    auto enqueue = [&](int lx, int ly, uint8_t d) {
        if (lx < 0 || lx >= rw || ly < 0 || ly >= rh) return;
        int idx = ly * rw + lx;
        if (dist[idx] <= d) return;
        dist[idx] = d;
        queue.push_back({idx, d});
    };
    for (int ly = 0; ly < rh; ++ly) {
        for (int lx = 0; lx < rw; ++lx) {
            int cid = cidArr[ly * rw + lx];
            int gx = x0 + lx, gy = y0 + ly;
            int nb4[4][2] = {{gx-1,gy},{gx+1,gy},{gx,gy-1},{gx,gy+1}};
            for (auto& n : nb4) {
                if (cidAt(n[0], n[1]) != cid) { enqueue(lx, ly, 0); break; }
            }
        }
    }
    size_t qpos = 0;
    while (qpos < queue.size()) {
        QE cur = queue[qpos++];
        if (cur.dist >= 60) continue;
        int lx = cur.idx % rw, ly = cur.idx / rw;
        int nx8[8] = {lx-1, lx+1, lx, lx, lx-1, lx-1, lx+1, lx+1};
        int ny8[8] = {ly, ly, ly-1, ly+1, ly-1, ly+1, ly-1, ly+1};
        for (int k = 0; k < 8; ++k) {
            int step = (k < 4) ? 2 : 3;
            enqueue(nx8[k], ny8[k], cur.dist + step);
        }
    }

    auto blend = [](Color base, float t) -> Color {
        uint8_t r = (uint8_t)std::min(255, std::max(0, (int)(base.r * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
        uint8_t g = (uint8_t)std::min(255, std::max(0, (int)(base.g * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
        uint8_t b = (uint8_t)std::min(255, std::max(0, (int)(base.b * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
        return Color{r, g, b, 255};
    };
    std::vector<Color> rect((size_t)rw * rh);
    for (int ly = 0; ly < rh; ++ly) {
        for (int lx = 0; lx < rw; ++lx) {
            int idx = ly * rw + lx;
            int cid = cidArr[idx];
            float t = std::min(1.0f, dist[idx] / 60.0f);
            Color out;
            if (cid <= 0) {
                uint8_t rb = (uint8_t)(8 + (uint8_t)((1.0f - t) * 16));
                uint8_t gb = (uint8_t)(10 + (uint8_t)((1.0f - t) * 22));
                uint8_t bb = (uint8_t)(15 + (uint8_t)((1.0f - t) * 38));
                out = {rb, gb, bb, 255};
            } else {
                const Country* c = m_editCountries.getCountry(cid);
                Color base = c ? c->color : Color{80, 80, 80, 255};
                out = blend(base, t);
            }
            rect[idx] = out;
        }
    }
    // 1px dark border line at country boundaries.
    for (int ly = 0; ly < rh; ++ly) {
        for (int lx = 0; lx < rw; ++lx) {
            int cid = cidArr[ly * rw + lx];
            if (cid <= 0) continue;
            int gx = x0 + lx, gy = y0 + ly;
            int nb4[4][2] = {{gx-1,gy},{gx+1,gy},{gx,gy-1},{gx,gy+1}};
            for (auto& n : nb4) {
                if (cidAt(n[0], n[1]) != cid) {
                    Color& px = rect[ly * rw + lx];
                    px = {(uint8_t)(px.r / 3), (uint8_t)(px.g / 3), (uint8_t)(px.b / 3), 255};
                    break;
                }
            }
        }
    }
    for (int ly = 0; ly < rh; ++ly)
        memcpy(&m_politicalPixels[(size_t)(y0 + ly) * MAP_W + x0], &rect[(size_t)ly * rw], (size_t)rw * sizeof(Color));

    if (m_renderer) m_renderer->updatePoliticalTextureRec(rect.data(), x0, y0, rw, rh);
}

void MapEditor::sanitizeProvincePixels() {
    if (!m_hasProvinces || m_provincePixels.empty()) return;
    const Color SEA_POL = {35, 60, 80, 255};
    for (int y = 0; y < MAP_H; ++y) {
        for (int x = 0; x < MAP_W; ++x) {
            int idx = y * MAP_W + x;
            const Color& pp = m_provincePixels[idx];
            int pid = Province::colorToId(pp.r, pp.g, pp.b);
            if (pid == 0) continue;
            bool bad = !m_editLandSea.isLand(x, y);                       // province color on sea
            if (!bad && !m_editProvinces.getProvinceById(pid)) bad = true; // color of no known province
            if (bad) {
                auto it = m_provincePixelCounts.find(pid);
                if (it != m_provincePixelCounts.end()) it->second--;
                m_provincePixels[idx] = Color{0, 0, 0, 0};
                m_politicalPixels[idx] = m_editLandSea.isLand(x, y)
                                             ? Color{140, 140, 140, 255} : SEA_POL;
            }
        }
    }
    // Orphan land (including half-res gaps at coastlines) joins its touching
    // province or becomes a new unclaimed province
    resetStrokeBBox();
    growStrokeBBox(0, 0, MAP_W - 1, MAP_H - 1);
    m_strokeWrapped = true;
    finalizeLandStroke();
    resetStrokeBBox();
}

// Assign unassigned land pixels inside the rect to a neighboring province by
// repeated relaxation passes. Bounded to the rect, so it's cheap enough to run
// every brush frame — this is what makes provinces reshape in realtime.
void MapEditor::localAssignNewLand(int x0, int y0, int x1, int y1) {
    if (m_provincePixels.empty()) return;
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(MAP_W - 1, x1); y1 = std::min(MAP_H - 1, y1);
    if (x0 > x1 || y0 > y1) return;

    std::unordered_map<int, std::pair<Color, Color>> colorCache; // pid -> {prov, political}
    auto colorsFor = [&](int pid) -> std::pair<Color, Color>& {
        auto it = colorCache.find(pid);
        if (it != colorCache.end()) return it->second;
        Color pc{0,0,0,0}, oc{140,140,140,255};
        if (Province* p = m_editProvinces.getProvinceById(pid)) {
            pc = {p->r, p->g, p->b, 255};
            if (const Country* c = m_editCountries.getCountry(p->countryId)) oc = c->color;
        }
        return colorCache.emplace(pid, std::make_pair(pc, oc)).first->second;
    };

    bool any = true;
    int guard = 0;
    while (any && guard++ < 96) {
        any = false;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                int idx = y * MAP_W + x;
                if (!m_editLandSea.isLand(x, y)) continue;
                const Color& cur = m_provincePixels[idx];
                if (Province::colorToId(cur.r, cur.g, cur.b) != 0) continue;
                int nbs[4][2] = {{x-1,y},{x+1,y},{x,y-1},{x,y+1}};
                for (auto& nb : nbs) {
                    int nx = nb[0], ny = nb[1];
                    if (nx < 0) nx = MAP_W - 1; else if (nx >= MAP_W) nx = 0;
                    if (ny < 0 || ny >= MAP_H) continue;
                    const Color& n = m_provincePixels[ny * MAP_W + nx];
                    int npid = Province::colorToId(n.r, n.g, n.b);
                    if (npid == 0) continue;
                    auto& cols = colorsFor(npid);
                    if (Province::colorToId(cols.first.r, cols.first.g, cols.first.b) == 0) continue;
                    m_provincePixels[idx] = cols.first;
                    m_politicalPixels[idx] = cols.second;
                    m_provincePixelCounts[npid]++;
                    any = true;
                    break;
                }
            }
        }
    }
}

// Land stroke finished: any land still unassigned forms connected components.
// Components touching an existing province get absorbed into it; detached
// components (new islands) become brand-new UNCLAIMED provinces.
void MapEditor::finalizeLandStroke() {
    if (!m_hasProvinces || m_provincePixels.empty() || !strokeBBoxValid()) return;
    const int UNC_CID = 65534;

    int sx0 = m_strokeWrapped ? 0 : std::max(0, m_strokeMinX - 2);
    int sx1 = m_strokeWrapped ? MAP_W - 1 : std::min(MAP_W - 1, m_strokeMaxX + 2);
    int sy0 = std::max(0, m_strokeMinY - 2);
    int sy1 = std::min(MAP_H - 1, m_strokeMaxY + 2);

    // Union of everything we touch (for the final rect updates)
    int ux0 = sx0, uy0 = sy0, ux1 = sx1, uy1 = sy1;

    auto pidAt = [&](int i) {
        const Color& c = m_provincePixels[i];
        return Province::colorToId(c.r, c.g, c.b);
    };

    std::unordered_set<int> visited;
    for (int y = sy0; y <= sy1; ++y) {
        for (int x = sx0; x <= sx1; ++x) {
            int start = y * MAP_W + x;
            if (visited.count(start)) continue;
            if (!m_editLandSea.isLand(x, y) || pidAt(start) != 0) continue;

            // Flood this orphan land component (may extend past the bbox)
            std::vector<int> comp;
            std::unordered_set<int> compSet;
            std::vector<int> stack{start};
            visited.insert(start);
            compSet.insert(start);
            std::unordered_set<int> adjacentPids;
            int cx0 = x, cy0 = y, cx1 = x, cy1 = y;
            while (!stack.empty()) {
                int idx = stack.back(); stack.pop_back();
                comp.push_back(idx);
                int px = idx % MAP_W, py = idx / MAP_W;
                cx0 = std::min(cx0, px); cx1 = std::max(cx1, px);
                cy0 = std::min(cy0, py); cy1 = std::max(cy1, py);
                int nbs[4][2] = {{px-1,py},{px+1,py},{px,py-1},{px,py+1}};
                for (auto& nb : nbs) {
                    int nx = nb[0], ny = nb[1];
                    if (nx < 0) nx = MAP_W - 1; else if (nx >= MAP_W) nx = 0;
                    if (ny < 0 || ny >= MAP_H) continue;
                    int ni = ny * MAP_W + nx;
                    if (!m_editLandSea.isLand(nx, ny)) continue;
                    int npid = pidAt(ni);
                    if (npid != 0) { adjacentPids.insert(npid); continue; }
                    if (visited.count(ni)) continue;
                    visited.insert(ni);
                    compSet.insert(ni);
                    stack.push_back(ni);
                }
            }

            if (!adjacentPids.empty()) {
                // Touches existing land: absorb into the adjacent province(s)
                // via BFS from the shared border
                std::unordered_map<int, std::pair<Color, Color>> colorCache;
                auto colorsFor = [&](int pid) -> std::pair<Color, Color>& {
                    auto it = colorCache.find(pid);
                    if (it != colorCache.end()) return it->second;
                    Color pc{0,0,0,0}, oc{140,140,140,255};
                    if (Province* p = m_editProvinces.getProvinceById(pid)) {
                        pc = {p->r, p->g, p->b, 255};
                        if (const Country* c = m_editCountries.getCountry(p->countryId)) oc = c->color;
                    }
                    return colorCache.emplace(pid, std::make_pair(pc, oc)).first->second;
                };
                std::vector<std::pair<int,int>> frontier; // idx, pid
                for (int idx : comp) {
                    int px = idx % MAP_W, py = idx / MAP_W;
                    int nbs[4][2] = {{px-1,py},{px+1,py},{px,py-1},{px,py+1}};
                    for (auto& nb : nbs) {
                        int nx = nb[0], ny = nb[1];
                        if (nx < 0) nx = MAP_W - 1; else if (nx >= MAP_W) nx = 0;
                        if (ny < 0 || ny >= MAP_H) continue;
                        int npid = pidAt(ny * MAP_W + nx);
                        if (npid != 0) { frontier.push_back({idx, npid}); break; }
                    }
                }
                size_t head = 0;
                while (head < frontier.size()) {
                    auto [idx, pid] = frontier[head++];
                    if (pidAt(idx) != 0) continue;
                    auto& cols = colorsFor(pid);
                    m_provincePixels[idx] = cols.first;
                    m_politicalPixels[idx] = cols.second;
                    m_provincePixelCounts[pid]++;
                    int px = idx % MAP_W, py = idx / MAP_W;
                    int nbs[4][2] = {{px-1,py},{px+1,py},{px,py-1},{px,py+1}};
                    for (auto& nb : nbs) {
                        int nx = nb[0], ny = nb[1];
                        if (nx < 0) nx = MAP_W - 1; else if (nx >= MAP_W) nx = 0;
                        if (ny < 0 || ny >= MAP_H) continue;
                        int ni = ny * MAP_W + nx;
                        if (compSet.count(ni) && pidAt(ni) == 0)
                            frontier.push_back({ni, pid});
                    }
                }
            } else if (comp.size() < 8) {
                // Tiny detached speck: not worth a province — leave it
                // unassigned (grey); the Fill tool can claim it later
                Color grey = {140, 140, 140, 255};
                for (int idx : comp) m_politicalPixels[idx] = grey;
            } else {
                // Detached island: it becomes a brand-new unclaimed province
                int pid = createProvinceEntry(UNC_CID);
                Province* np = m_editProvinces.getProvinceById(pid);
                Color pc = np ? Color{np->r, np->g, np->b, 255} : Color{0,0,0,0};
                Color oc = {140, 140, 140, 255};
                if (const Country* c = m_editCountries.getCountry(UNC_CID)) oc = c->color;
                for (int idx : comp) {
                    m_provincePixels[idx] = pc;
                    m_politicalPixels[idx] = oc;
                }
                m_provincePixelCounts[pid] = (int)comp.size();
                m_provinceData[pid].population = std::max(100LL, (long long)comp.size() * 12);
                LoadLog() << "  New island province #" << pid << " (" << comp.size() << " px)\n";
            }
            ux0 = std::min(ux0, cx0); uy0 = std::min(uy0, cy0);
            ux1 = std::max(ux1, cx1); uy1 = std::max(uy1, cy1);
        }
    }

    // Push the touched region to the CPU province image + textures once
    m_editProvinces.updatePixelsRect(m_provincePixels.data(), ux0, uy0, ux1 - ux0 + 1, uy1 - uy0 + 1);
    liveUpdateRegion(ux0, uy0, ux1, uy1);
    garbageCollectProvinces();
    trackChange();
    m_hlDirty = true;
    markPoliticalDirty(ux0, uy0, ux1, uy1);
}

bool MapEditor::garbageCollectProvinces() {
    if (!m_hasProvinces || m_provincePixels.empty()) return false;
    if (m_provincePixelCounts.empty()) rebuildProvinceCounts();
    std::vector<int> dead;
    for (auto& [id, p] : m_editProvinces.getAllProvinces()) {
        if (id == m_selectedProvince) continue; // keep freshly created/being-painted
        auto it = m_provincePixelCounts.find(id);
        if (it == m_provincePixelCounts.end() || it->second <= 0) dead.push_back(id);
    }
    if (dead.empty()) return false;
    try {
        auto j = nlohmann::json::parse(m_provinceJson);
        for (int id : dead) j.erase(std::to_string(id));
        m_provinceJson = j.dump(2);
    } catch (...) {}
    for (int id : dead) {
        m_editProvinces.removeProvince(id);
        m_provinceData.erase(id);
        m_provincePixelCounts.erase(id);
    }
    LoadLog() << "  Removed " << dead.size() << " empty province(s)\n";
    return true;
}

int MapEditor::createProvinceEntry(int countryId) {
    int nextId = 1;
    for (auto& [id, p] : m_editProvinces.getAllProvinces())
        nextId = std::max(nextId, id + 1);
    Province p;
    p.id = nextId;
    p.countryId = countryId;
    p.name = "Province #" + std::to_string(nextId);
    p.isoA3 = "";
    p.r = (uint8_t)((nextId >> 16) & 0xFF);
    p.g = (uint8_t)((nextId >> 8) & 0xFF);
    p.b = (uint8_t)(nextId & 0xFF);
    m_editProvinces.addProvince(p);
    try {
        auto j = nlohmann::json::parse(m_provinceJson.empty() ? "{}" : m_provinceJson);
        char hex[8]; snprintf(hex, sizeof(hex), "#%06x", nextId);
        j[std::to_string(nextId)] = {
            {"id", nextId}, {"name", p.name}, {"country_id", countryId},
            {"iso_a3", ""}, {"color", std::string(hex)}
        };
        m_provinceJson = j.dump(2);
    } catch (...) {}
    m_provinceData[nextId] = EditorProvinceData{};
    m_provincePixelCounts[nextId] = 0;
    trackChange();
    return nextId;
}

// Blank projects start with no province layer at all. Rather than forcing the
// generator, stand up an empty one on demand so a map can be authored entirely
// by hand: no provinces yet, political layer showing sea vs. unclaimed land.
void MapEditor::initEmptyProvinceLayer() {
    if (m_hasProvinces || !m_renderer) return;
    const Color SEA_POL = {35, 60, 80, 255};
    const Color UNASSIGNED_POL = {140, 140, 140, 255};

    m_provincePixels.assign((size_t)MAP_W * MAP_H, Color{0, 0, 0, 0}); // pid 0 = no province
    m_politicalPixels.resize((size_t)MAP_W * MAP_H);
    for (int y = 0; y < MAP_H; ++y)
        for (int x = 0; x < MAP_W; ++x)
            m_politicalPixels[(size_t)y * MAP_W + x] =
                m_editLandSea.isLand(x, y) ? UNASSIGNED_POL : SEA_POL;

    m_provinceJson = "{}";
    if (m_countryJson.empty()) m_countryJson = "{}";
    m_provinceData.clear();
    m_provincePixelCounts.clear();

    // ProvinceMap needs a backing image even with zero provinces defined —
    // opaque black decodes to pid 0 everywhere, i.e. "unassigned".
    {
        std::vector<uint8_t> raw((size_t)MAP_W * MAP_H * 4);
        for (size_t i = 0; i < (size_t)MAP_W * MAP_H; ++i) {
            raw[i*4] = 0; raw[i*4+1] = 0; raw[i*4+2] = 0; raw[i*4+3] = 255;
        }
        int pngLen = 0;
        unsigned char* pngData = stbi_write_png_to_mem(raw.data(), MAP_W * 4, MAP_W, MAP_H, 4, &pngLen);
        if (pngData) {
            m_editProvinces.clear();
            m_editProvinces.loadFromMemory(pngData, pngLen, m_provinceJson);
            stbi_image_free(pngData);
        }
    }

    Image polImg{};
    polImg.data = m_politicalPixels.data();
    polImg.width = MAP_W;
    polImg.height = MAP_H;
    polImg.mipmaps = 1;
    polImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    Texture2D polTex = LoadTextureFromImage(polImg);
    SetTextureFilter(polTex, TEXTURE_FILTER_POINT);
    m_renderer->setPoliticalTexture(polTex);
    if (m_editProvinces.getImage().data != nullptr)
        m_renderer->computeBorderTexture(m_editProvinces.getImage());

    m_hasProvinces = true;
    m_hlDirty = true;
    trackChange();
    LoadLog() << "  Started an empty province layer\n";
}

void MapEditor::createNewProvince() {
    if (!m_hasProvinces) {
        // Blank canvas: bootstrap the province layer instead of refusing.
        initEmptyProvinceLayer();
        if (!m_hasProvinces) {
            m_warningMsg = "Create or open a project first";
            m_warningTimer = 2.0f;
            return;
        }
    }
    // Inherit the owner of the selected province (carving), else unclaimed
    int owner = 65534;
    if (m_selectedProvince >= 0)
        if (Province* sp = m_editProvinces.getProvinceById(m_selectedProvince))
            owner = sp->countryId;
    m_selectedProvince = createProvinceEntry(owner);
    m_provTool = 1; // straight into paint mode so it can be drawn in
}

void MapEditor::regenerateFlag(Country& c) {
    std::mt19937 rng((unsigned)rand());
    auto ri = [&](int n) { return (int)(rng() % (unsigned)n); };
    int rr = c.color.r, gg = c.color.g, bb = c.color.b;
    Color main = {(uint8_t)rr, (uint8_t)gg, (uint8_t)bb, 255};
    Color light = {(uint8_t)std::min(rr + 60, 255), (uint8_t)std::min(gg + 60, 255), (uint8_t)std::min(bb + 60, 255), 255};
    Color dark = {(uint8_t)std::max(rr - 40, 0), (uint8_t)std::max(gg - 40, 0), (uint8_t)std::max(bb - 40, 0), 255};
    Color darker = {(uint8_t)std::max(rr - 80, 0), (uint8_t)std::max(gg - 80, 0), (uint8_t)std::max(bb - 80, 0), 255};
    Color white = {255, 255, 255, 255};

    FlagPattern f;
    switch (ri(14)) {
        case 0: f.type = FlagType::SOLID; f.colors = {main}; break;
        case 1: f.type = FlagType::HSTRIPES_3; f.colors = {dark, main, dark}; break;
        case 2: f.type = FlagType::VSTRIPES_3; f.colors = {dark, main, dark}; break;
        case 3: f.type = FlagType::DIAGONAL_R; f.colors = {main, darker}; break;
        case 4: f.type = FlagType::DIAGONAL_L; f.colors = {light, main}; break;
        case 5: f.type = FlagType::PALE; f.colors = {dark, main, dark}; break;
        case 6: f.type = FlagType::FESS; f.colors = {dark, main, dark}; break;
        case 7: f.type = FlagType::CROSS_GREEK; f.colors = {main, white}; break;
        case 8: f.type = FlagType::SALTIR; f.colors = {main, white}; break;
        case 9: f.type = FlagType::TRIANGLE; f.colors = {light, main}; break;
        case 10: f.type = FlagType::CANTON; f.colors = {main, light}; break;
        case 11: f.type = FlagType::QUARTERED; f.colors = {main, light, light, main}; break;
        case 12: f.type = FlagType::CROSS_NORDIC; f.colors = {main, white}; break;
        default: f.type = FlagType::SUNBURST; f.colors = {main, light}; break;
    }

    // Always add a symbol (SVG-backed types only), in a contrasting color
    FlagSymbol s;
    static const SymbolType symTypes[] = {
        SymbolType::STAR_5, SymbolType::STAR_6, SymbolType::CRESCENT, SymbolType::SUN,
        SymbolType::GEAR, SymbolType::MOUNTAIN, SymbolType::TREE, SymbolType::DIAMOND,
        SymbolType::CROSS_LATIN, SymbolType::CROSS_SALTIR,
        // The rest of data/symbols/, which a generated flag could not reach
        // while no SymbolType named them.
        SymbolType::ANCHOR, SymbolType::TORCH, SymbolType::ROSE,
        SymbolType::CROSS_PATTEE, SymbolType::STAR_4, SymbolType::STAR_7,
        SymbolType::CROSS_MALTESE, SymbolType::CRESCENT_STAR};
    s.type = symTypes[ri((int)(sizeof(symTypes) / sizeof(symTypes[0])))];
    float lum = (0.299f * rr + 0.587f * gg + 0.114f * bb) / 255.0f;
    s.colors = { lum > 0.6f ? Color{26, 26, 34, 255} : white };
    s.x = 0.5f;
    s.y = 0.5f;
    s.size = 0.28f + ri(5) * 0.02f;
    if (f.type == FlagType::CANTON) { s.x = 0.25f; s.y = 0.25f; s.size = 0.18f; }
    f.symbols = {s};

    c.flagActual = f;
    c.flagCensored = f;
    m_flagPreviewCountry = -1; // refresh cached preview
    trackChange();
}

void MapEditor::liveRecolorCountry(int cid) {
    if (!m_hasProvinces || m_politicalPixels.empty() || cid < 0 || !m_renderer) return;
    const Country* c = m_editCountries.getCountry(cid);
    if (!c) return;
    // Build the pixel cache once per country (invalidated on selection change
    // and whenever provinces are reshaped)
    if (m_recolorCid != cid) {
        m_recolorCid = cid;
        m_recolorIdx.clear();
        m_recolorX0 = MAP_W; m_recolorY0 = MAP_H; m_recolorX1 = -1; m_recolorY1 = -1;
        int maxId = 0;
        for (auto& [pid, p] : m_editProvinces.getAllProvinces()) maxId = std::max(maxId, pid);
        std::vector<uint8_t> mine((size_t)maxId + 1, 0);
        for (auto& [pid, p] : m_editProvinces.getAllProvinces())
            if (p.countryId == cid) mine[pid] = 1;
        for (int y = 0; y < MAP_H; ++y) {
            const Color* row = &m_provincePixels[(size_t)y * MAP_W];
            for (int x = 0; x < MAP_W; ++x) {
                int pid = Province::colorToId(row[x].r, row[x].g, row[x].b);
                if (pid <= 0 || pid > maxId || !mine[pid]) continue;
                m_recolorIdx.push_back(y * MAP_W + x);
                if (x < m_recolorX0) m_recolorX0 = x;
                if (x > m_recolorX1) m_recolorX1 = x;
                if (y < m_recolorY0) m_recolorY0 = y;
                if (y > m_recolorY1) m_recolorY1 = y;
            }
        }
    }
    if (m_recolorIdx.empty() || m_recolorX1 < m_recolorX0) return;
    for (int idx : m_recolorIdx) m_politicalPixels[idx] = c->color;
    int w = m_recolorX1 - m_recolorX0 + 1, h = m_recolorY1 - m_recolorY0 + 1;
    std::vector<Color> rect((size_t)w * h);
    for (int row = 0; row < h; ++row)
        memcpy(&rect[(size_t)row * w],
               &m_politicalPixels[(size_t)(m_recolorY0 + row) * MAP_W + m_recolorX0],
               (size_t)w * sizeof(Color));
    m_renderer->updatePoliticalTextureRec(rect.data(), m_recolorX0, m_recolorY0, w, h);
    trackChange();
}

void MapEditor::updateSelectionHighlight() {
    if (!m_renderer) return;
    if (!m_hasProvinces || m_provincePixels.empty()) { m_renderer->clearHighlight(); return; }
    int maxId = 0;
    for (auto& [pid, p] : m_editProvinces.getAllProvinces()) maxId = std::max(maxId, pid);
    // Per-province highlight color (alpha 0 = not highlighted). DrawTexture
    // tints with a pulsing-alpha WHITE, so whatever RGB we bake in here comes
    // through unmodified — that's how country A/B get distinct colors from a
    // single highlight texture/draw call.
    std::vector<Color> matchColor((size_t)maxId + 1, Color{0, 0, 0, 0});
    bool anyMatch = false;
    static const Color HL_WHITE = {255, 255, 255, 255};
    static const Color HL_GOLD  = {255, 205, 60, 255};
    static const Color HL_CYAN  = {80, 220, 255, 255};

    if (m_mode == MODE_PROVINCES && m_selectedProvince >= 0 && m_selectedProvince <= maxId) {
        if (m_editProvinces.getProvinceById(m_selectedProvince)) {
            matchColor[m_selectedProvince] = HL_WHITE;
            anyMatch = true;
        }
    } else if (m_mode == MODE_COUNTRIES && m_selectedCountry >= 0) {
        for (auto& [pid, p] : m_editProvinces.getAllProvinces())
            if (p.countryId == m_selectedCountry && pid <= maxId) { matchColor[pid] = HL_WHITE; anyMatch = true; }
    } else if (m_mode == MODE_RELATIONS) {
        // Country A in gold, country B in cyan — both baked into one texture
        if (m_relCountryA >= 0)
            for (auto& [pid, p] : m_editProvinces.getAllProvinces())
                if (p.countryId == m_relCountryA && pid <= maxId) { matchColor[pid] = HL_GOLD; anyMatch = true; }
        if (m_relCountryB >= 0)
            for (auto& [pid, p] : m_editProvinces.getAllProvinces())
                if (p.countryId == m_relCountryB && pid <= maxId) { matchColor[pid] = HL_CYAN; anyMatch = true; }
    }
    if (!anyMatch) { m_renderer->clearHighlight(); return; }

    // Bounding box of the matching pixels, then a small highlight texture
    int x0 = MAP_W, y0 = MAP_H, x1 = -1, y1 = -1;
    for (int y = 0; y < MAP_H; ++y) {
        const Color* row = &m_provincePixels[(size_t)y * MAP_W];
        for (int x = 0; x < MAP_W; ++x) {
            int pid = Province::colorToId(row[x].r, row[x].g, row[x].b);
            if (pid <= 0 || pid > maxId || matchColor[pid].a == 0) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < x0) { m_renderer->clearHighlight(); return; }
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<Color> px((size_t)w * h, Color{0, 0, 0, 0});
    for (int y = y0; y <= y1; ++y) {
        const Color* row = &m_provincePixels[(size_t)y * MAP_W];
        for (int x = x0; x <= x1; ++x) {
            int pid = Province::colorToId(row[x].r, row[x].g, row[x].b);
            if (pid > 0 && pid <= maxId && matchColor[pid].a != 0)
                px[(size_t)(y - y0) * w + (x - x0)] = matchColor[pid];
        }
    }
    Image img{};
    img.data = px.data();
    img.width = w;
    img.height = h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    Texture2D t = LoadTextureFromImage(img);
    SetTextureFilter(t, TEXTURE_FILTER_POINT);
    m_renderer->setHighlight(t, x0, y0);
}

// Snapshot pass (called when the overlay is toggled on, or after edits):
// finds each qualifying province's pixel centroid so drawBuildingBadges()
// can place icon badges there every frame without rescanning the map.
void MapEditor::rebuildBuildingsOverlay() {
    m_buildingCentroids.clear();
    if (!m_hasProvinces || m_provincePixels.empty()) return;
    std::unordered_map<int, std::pair<long long,long long>> sum; // pid -> (sumX, sumY)
    std::unordered_map<int, long long> count;
    for (int y = 0; y < MAP_H; ++y) {
        const Color* row = &m_provincePixels[(size_t)y * MAP_W];
        for (int x = 0; x < MAP_W; ++x) {
            int pid = Province::colorToId(row[x].r, row[x].g, row[x].b);
            if (pid == 0) continue;
            auto dit = m_provinceData.find(pid);
            if (dit == m_provinceData.end()) continue;
            const EditorProvinceData& d = dit->second;
            if (d.industryLevel <= 0 && d.fortification <= 0 && d.portLevel <= 0) continue;
            sum[pid].first += x; sum[pid].second += y; count[pid]++;
        }
    }
    for (auto& [pid, c] : count)
        m_buildingCentroids[pid] = {(float)(sum[pid].first / c), (float)(sum[pid].second / c)};
}

// Small icon badges (I/F/P + level) drawn in screen space at each qualifying
// province's centroid — replaces the old blended-color texture so industry,
// fortification and port each get their own clearly labeled, side-by-side
// indicator instead of one muddled tint.
void MapEditor::drawBuildingBadges() {
    if (!m_renderer || !m_renderer->getShowEditorOverlay() || m_buildingCentroids.empty()) return;
    struct Badge { const char* letter; Color color; int EditorProvinceData::*level; };
    static const Badge badges[] = {
        {"I", Color{255, 170, 40, 255}, &EditorProvinceData::industryLevel},
        {"F", Color{230, 70, 70, 255}, &EditorProvinceData::fortification},
        {"P", Color{60, 160, 255, 255}, &EditorProvinceData::portLevel},
    };
    const int badgeW = 20, badgeH = 16, gap = 2;
    for (auto& [pid, centroid] : m_buildingCentroids) {
        auto dit = m_provinceData.find(pid);
        if (dit == m_provinceData.end()) continue;
        const EditorProvinceData& d = dit->second;

        // Count how many badges this province needs, to center the row
        int n = 0;
        for (auto& b : badges) if (d.*(b.level) > 0) n++;
        if (n == 0) continue;

        float sx, sy;
        canvasToScreen((int)centroid.x, (int)centroid.y, sx, sy);
        if (sx < m_canvasX - 40 || sx > m_canvasX + m_canvasW + 40 ||
            sy < m_canvasY - 20 || sy > m_canvasY + m_canvasH + 20) continue; // off-screen

        float rowW = n * badgeW + (n - 1) * gap;
        float bx = sx - rowW / 2.0f;
        for (auto& b : badges) {
            int lvl = d.*(b.level);
            if (lvl <= 0) continue;
            Rectangle r = {bx, sy - badgeH / 2.0f, (float)badgeW, (float)badgeH};
            DrawRectangleRounded(r, 0.3f, 4, ColorAlpha(b.color, 0.85f));
            DrawRectangleRoundedLines(r, 0.3f, 4, ColorAlpha(BLACK, 0.5f));
            std::string label = std::string(b.letter) + std::to_string(lvl);
            int tw = MeasureText(label.c_str(), 10);
            DrawText(label.c_str(), (int)(r.x + r.width / 2 - tw / 2), (int)(r.y + 3), 10, BLACK);
            bx += badgeW + gap;
        }
    }
}

void MapEditor::applyRect(int x1, int y1, int x2, int y2, bool land) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min(MAP_W-1, x2); y2 = std::min(MAP_H-1, y2);
    if (x1 > x2 || y1 > y2) return;
    Color c = land ? COL_LAND : COL_SEA;
    const Color SEA_POL = {35, 60, 80, 255};
    const Color UNASSIGNED_POL = {140, 140, 140, 255};
    bool provAware = m_hasProvinces && !m_provincePixels.empty();
    for (int py = y1; py <= y2; py++) {
        for (int px = x1; px <= x2; px++) {
            int idx = py * MAP_W + px;
            m_pixels[idx] = c;
            if (provAware) {
                const Color& pp = m_provincePixels[idx];
                int pid = Province::colorToId(pp.r, pp.g, pp.b);
                if (!land) {
                    if (pid != 0) {
                        m_provincePixelCounts[pid]--;
                        m_provincePixels[idx] = Color{0, 0, 0, 0};
                        m_politicalPixels[idx] = SEA_POL;
                    }
                } else if (pid == 0) {
                    m_politicalPixels[idx] = UNASSIGNED_POL;
                }
            }
        }
    }
    trackChange();
    {
        int uw = x2 - x1 + 1, uh = y2 - y1 + 1;
        std::vector<Color> rectData((size_t)uw * uh);
        for (int row = 0; row < uh; row++)
            memcpy(&rectData[(size_t)row * uw], &m_pixels[(size_t)(y1 + row) * MAP_W + x1],
                   (size_t)uw * sizeof(Color));
        m_editLandSea.updatePixelsRect(rectData.data(), x1, y1, uw, uh);
    }
    if (provAware) {
        growStrokeBBox(x1, y1, x2, y2);
        int margin = 12;
        int ax0 = std::max(0, x1 - margin), ay0 = std::max(0, y1 - margin);
        int ax1 = std::min(MAP_W - 1, x2 + margin), ay1 = std::min(MAP_H - 1, y2 + margin);
        if (land) localAssignNewLand(ax0, ay0, ax1, ay1);
        liveUpdateRegion(ax0, ay0, ax1, ay1);
    }
}

void MapEditor::applyFloodFill(int sx, int sy, bool land) {
    if (sx < 0 || sx >= MAP_W || sy < 0 || sy >= MAP_H) return;
    Color target = m_pixels[sy * MAP_W + sx];
    Color fill = land ? COL_LAND : COL_SEA;
    if (target.r == fill.r && target.g == fill.g && target.b == fill.b) return;
    const Color SEA_POL = {35, 60, 80, 255};
    const Color UNASSIGNED_POL = {140, 140, 140, 255};
    bool provAware = m_hasProvinces && !m_provincePixels.empty();
    int bx0 = sx, by0 = sy, bx1 = sx, by1 = sy;
    std::vector<std::pair<int,int>> stack; stack.push_back({sx, sy});
    std::unordered_set<int> visited;
    while (!stack.empty()) {
        auto [x, y] = stack.back(); stack.pop_back();
        int idx = y * MAP_W + x;
        if (visited.count(idx) || x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) continue;
        if (m_pixels[idx].r != target.r || m_pixels[idx].g != target.g || m_pixels[idx].b != target.b) continue;
        visited.insert(idx);
        m_pixels[idx] = fill;
        if (provAware) {
            const Color& pp = m_provincePixels[idx];
            int pid = Province::colorToId(pp.r, pp.g, pp.b);
            if (!land) {
                if (pid != 0) {
                    m_provincePixelCounts[pid]--;
                    m_provincePixels[idx] = Color{0, 0, 0, 0};
                    m_politicalPixels[idx] = SEA_POL;
                }
            } else if (pid == 0) {
                m_politicalPixels[idx] = UNASSIGNED_POL;
            }
        }
        bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
        by0 = std::min(by0, y); by1 = std::max(by1, y);
        stack.push_back({(x+1)%MAP_W, y});
        stack.push_back({(x-1+MAP_W)%MAP_W, y});
        stack.push_back({x, y+1});
        stack.push_back({x, y-1});
    }
    trackChange();
    m_editLandSea.updatePixels(m_pixels.data());
    if (provAware) {
        // Region may hug the seam; treat a full-width fill as wrapped
        if (bx0 == 0 && bx1 == MAP_W - 1) m_strokeWrapped = true;
        growStrokeBBox(bx0, by0, bx1, by1);
        liveUpdateRegion(std::max(0, bx0 - 2), std::max(0, by0 - 2),
                         std::min(MAP_W - 1, bx1 + 2), std::min(MAP_H - 1, by1 + 2));
    }
}

// ── Periodic noise with hash-grid X wrapping ───────────────────
// period = number of integer hash-grid cells before wrapping.
// For pixel-perfect seam: pass period = (int)(MAP_W * scale) so
// the hash at pixel 0 and pixel MAP_W-1 land in the same cell.
static float pnoise(float x, float y, int seed, int period) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    fx = fx*fx*(3-2*fx); fy = fy*fy*(3-2*fy);

    auto hash = [&](int hx, int hy) -> float {
        hx = hx % period;
        if (hx < 0) hx += period;
        unsigned int h = (unsigned int)(hx * 374761393 + hy * 668265263 + seed * 982451653);
        h = (h ^ (h >> 13)) * 1274126177u;
        return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
    };
    float a = hash(ix, iy), b = hash(ix+1, iy);
    float c = hash(ix, iy+1), d = hash(ix+1, iy+1);
    return a + (b-a)*fx + (c-a)*fy + (a-b-c+d)*fx*fy;
}

static float pfbm(float x, float y, int oct, int seed, int period) {
    float val = 0, amp = 1, freq = 1, maxVal = 0;
    for (int i = 0; i < oct; i++) {
        val += pnoise(x*freq, y*freq, seed+i*1000, period) * amp;
        maxVal += amp; amp *= 0.5f; freq *= 2.0f;
    }
    return val / maxVal;
}

// ════════════════════════════════════════════════════════════════
//  Map Generator - multi-octave noise with horizontal wrapping
// ════════════════════════════════════════════════════════════════

void MapEditor::generateMap(const GeneratorParams& p) {
    m_genParams = p;
    // Clear province/country state when generating new landmass
    m_hasProvinces = false;
    m_hasGameData = false;
    m_provincePixels.clear();
    m_provinceJson.clear();
    m_politicalPixels.clear();
    m_countryJson.clear();
    m_populationJson.clear();
    m_resourcesJson.clear();
    m_minoritiesJson.clear();
    m_minorityColorsJson.clear();
    m_ethnicColors.clear();
    m_provinceData.clear();
    m_provincePixelCounts.clear();
    m_editorShips.clear();
    m_editorRelations.clear();
    m_countryPolicies.clear();
    m_ethnicityPolicies.clear();
    m_ethnicRelations.clear();
    m_thumbnailPath.clear();
    m_thumbnailTexPath.clear();
    m_editorClaims.clear();
    m_claimsPixels.clear();
    m_claimsOverlayCid = -2;
    m_selectedProvince = -1;
    m_selectedCountry = -1;
    resetStrokeBBox();
    if (m_renderer) {
        m_renderer->setPoliticalTexture(Texture2D{}); // clear political overlay
        m_renderer->setEditorOverlay(Texture2D{});
        m_renderer->setShowEditorOverlay(false);
        m_buildingCentroids.clear();
        m_renderer->clearHighlight();
    }
    int w = MAP_W, h = MAP_H;
    std::mt19937 rng((unsigned int)p.seed);

    // ── Continent centers with toroidal placement ──
    struct Center { float x, y; };
    std::vector<Center> centers;
    float minDist = (float)w / p.numContinents * 0.45f;
    for (int i = 0; i < p.numContinents; i++) {
        for (int attempt = 0; attempt < 50; attempt++) {
            float cx = (float)(rng() % w);
            float cy = h * 0.2f + (rng() % (int)(h * 0.6f));
            bool ok = true;
            for (auto& c : centers) {
                float dx = fabsf(cx - c.x);
                dx = fminf(dx, (float)w - dx);
                if (sqrtf(dx*dx + (cy-c.y)*(cy-c.y)) < minDist) { ok = false; break; }
            }
            if (ok) { centers.push_back({cx, cy}); break; }
        }
        if (centers.size() <= (size_t)i)
            centers.push_back({(float)(rng() % w), h * 0.2f + (rng() % (int)(h * 0.6f))});
    }

    // Ghost copies for wrapping
    auto distToLand = [&](float px, float py) -> float {
        float best = 1e9f;
        for (auto& c : centers) {
            // Toroidal distance: wrap X
            float dx = fabsf(px - c.x);
            dx = fminf(dx, (float)w - dx);
            float dy = py - c.y;
            best = fminf(best, sqrtf(dx*dx + dy*dy));
        }
        return best;
    };

    float zoom = 0.0035f;
    float warpStr = 80.0f + p.jaggedness * 120.0f;
    float threshold = 0.48f - p.landCoverage * 0.22f;
    float contRad = (float)w / p.numContinents * 0.65f;

    // Period for each noise scale: number of hash-grid cells across one map width.
    // This ensures noise(px=0) and noise(px=MAP_W-1) share the same hash cell.
    int pWarp  = std::max(1, (int)((float)w * zoom * 0.3f));   // ~8
    int pFine  = std::max(1, (int)((float)w * zoom * 2.0f));   // ~57
    int pBase  = std::max(1, (int)((float)w * zoom));           // ~28
    int pScat  = std::max(1, (int)((float)w * 0.02f));          // ~163

    // ── Generate elevation at 1/4 resolution (16x fewer pixels) ──
    const int SCALE = 4;
    int sw = w / SCALE, sh = h / SCALE;
    std::vector<float> smallElev(sw * sh);
    for (int py = 0; py < sh; py++) {
        for (int px = 0; px < sw; px++) {
            float fullX = (float)(px * SCALE), fullY = (float)(py * SCALE);
            float wx = fullX, wy = fullY;

            float w1 = pfbm(wx * zoom * 0.3f, wy * zoom * 0.3f, 3, p.seed + 10, pWarp);
            float w2 = pfbm(wx * zoom * 0.3f + 50, wy * zoom * 0.3f + 50, 3, p.seed + 20, pWarp);
            wx += (w1 - 0.5f) * warpStr;
            wy += (w2 - 0.5f) * warpStr;

            float w3 = pfbm(wx * zoom * 2.0f, wy * zoom * 2.0f, 2, p.seed + 30, pFine);
            float w4 = pfbm(wx * zoom * 2.0f + 100, wy * zoom * 2.0f + 100, 2, p.seed + 40, pFine);
            wx += (w3 - 0.5f) * warpStr * 0.4f;
            wy += (w4 - 0.5f) * warpStr * 0.4f;

            float n = pfbm(wx * zoom, wy * zoom, 6, p.seed, pBase);
            n = 1.0f - fabsf(n * 2.0f - 1.0f);

            float scatter = pfbm(fullX * 0.02f, fullY * 0.02f, 3, p.seed + 100, pScat);
            n += (scatter - 0.5f) * 0.12f;

            float d = distToLand(fullX, fullY);
            float mask = d / contRad;
            if (mask < 1.0f) mask = mask * mask;

            float val = n - mask * 0.55f;
            float latF = 1.0f - fabsf((float)(fullY - h/2) / (h/2));
            val -= (1.0f - latF) * 0.35f;

            smallElev[py * sw + px] = val;
        }
    }

    // ── Bilinear upscale elevation to full resolution ──
    std::vector<float> elevation(w * h);
    for (int py = 0; py < h; py++) {
        float sy = ((float)py + 0.5f) / SCALE - 0.5f;
        int iy = (int)sy; if (iy < 0) iy = 0; if (iy >= sh - 1) iy = sh - 2;
        float fy = sy - iy;
        for (int px = 0; px < w; px++) {
            float sx = ((float)px + 0.5f) / SCALE - 0.5f;
            int ix = (int)sx; if (ix < 0) ix = 0; if (ix >= sw - 1) ix = sw - 2;
            float fx = sx - ix;
            float v00 = smallElev[iy * sw + ix];
            float v10 = smallElev[iy * sw + ix + 1];
            float v01 = smallElev[(iy + 1) * sw + ix];
            float v11 = smallElev[(iy + 1) * sw + ix + 1];
            float v = v00 * (1-fx)*(1-fy) + v10 * fx*(1-fy) + v01 * (1-fx)*fy + v11 * fx*fy;
            elevation[py * w + px] = v;
        }
    }

    // ── X-seam: smooth elevation blend ──
    int seamW = 80;
    for (int py = 0; py < h; ++py) {
        float leftEdge = elevation[py * w];
        for (int b = 0; b < seamW; ++b) {
            float t = (float)b / (seamW - 1);
            int ri = py * w + (w - seamW + b);
            elevation[ri] = elevation[ri] * (1.0f - t) + leftEdge * t;
        }
        elevation[py * w + (w - 1)] = elevation[py * w];
    }

    // ── Threshold elevation to land/sea ──
    m_pixels.resize(w * h);
    for (int i = 0; i < w * h; ++i) {
        m_pixels[i] = (elevation[i] > threshold) ? COL_LAND : COL_SEA;
    }

    // ── Find inland lakes via edge-connected ocean flood fill ──
    auto isSea = [&](int i) { return m_pixels[i].b > m_pixels[i].g; }; // COL_SEA has b=120 > g=0
    std::vector<uint8_t> ocean(w * h, 0);
    std::vector<std::pair<int,int>> queue;
    for (int px = 0; px < w; px++) {
        if (isSea(px)) { ocean[px] = 1; queue.push_back({px, 0}); }
        if (isSea((h-1)*w + px)) { ocean[(h-1)*w + px] = 1; queue.push_back({px, h-1}); }
    }
    for (int py = 0; py < h; py++) {
        if (isSea(py*w)) { ocean[py*w] = 1; queue.push_back({0, py}); }
        if (isSea(py*w + w-1)) { ocean[py*w + w-1] = 1; queue.push_back({w-1, py}); }
    }
    while (!queue.empty()) {
        auto [x, y] = queue.back(); queue.pop_back();
        int nbs[4][2] = {{x-1,y},{x+1,y},{x,y-1},{x,y+1}};
        for (auto& nb : nbs) {
            int nx = nb[0], ny = nb[1];
            if (ny < 0 || ny >= h) continue;
            if (nx < 0) nx = w - 1; else if (nx >= w) nx = 0;
            int idx = ny * w + nx;
            if (isSea(idx) && !ocean[idx]) { ocean[idx] = 1; queue.push_back({nx, ny}); }
        }
    }

    // Lakes are already COL_SEA (no visual change), but they're now identified.
    // They'll remain as COL_SEA — visually they're already "lakes" (sea inside land).

    m_editLandSea.updatePixels(m_pixels.data());
    m_dirty = true;
}

// ════════════════════════════════════════════════════════════════
//  Procedural Province, Country & Game Data Generation
// ════════════════════════════════════════════════════════════════

void MapEditor::generateProvincesCountries() {
    m_provincePixels.clear();
    m_provinceJson.clear();
    m_politicalPixels.clear();
    m_countryJson.clear();
    m_hasProvinces = false;

    // Downscale land/sea for generation. SCALE 2 keeps the editor's province
    // outlines crisp; headless training uses 4 (16x fewer pixels than full
    // res), which is what this path used historically before it was lowered
    // for visual quality.
    const int SCALE = m_fastGen ? 4 : 2;
    int sw = MAP_W / SCALE, sh = MAP_H / SCALE;
    std::vector<Color> smallPixels(sw * sh);
    for (int py = 0; py < sh; ++py) {
        int srcRow = py * SCALE * MAP_W;
        for (int px = 0; px < sw; ++px)
            smallPixels[py * sw + px] = m_pixels[srcRow + px * SCALE];
    }

    // Province count must not move with SCALE: halving SCALE quadruples the
    // pixel count, so the density divisor is 16/SCALE^2 (4 at SCALE 2, 1 at
    // SCALE 4 — the original tuning).
    float adjustedDensity = m_genParams.provinceDensity * (float)(SCALE * SCALE) / 16.0f;
    auto result = generateProcedural(smallPixels, sw, sh,
                                     m_genParams.seed, m_genParams.numCountries,
                                     adjustedDensity);

    // ── 3×3 mode filter on province pixels to straighten jagged borders ──
    {
        std::vector<Color> smoothed(sw * sh);
        for (int py = 0; py < sh; ++py) {
            for (int px = 0; px < sw; ++px) {
                Color c = result.provincePixels[py * sw + px];
                // Fast path: check if pixel is on a border first
                bool onBorder = false;
                if (px > 0 && memcmp(&c, &result.provincePixels[py * sw + px - 1], sizeof(Color))) onBorder = true;
                else if (px < sw-1 && memcmp(&c, &result.provincePixels[py * sw + px + 1], sizeof(Color))) onBorder = true;
                else if (py > 0 && memcmp(&c, &result.provincePixels[(py-1) * sw + px], sizeof(Color))) onBorder = true;
                else if (py < sh-1 && memcmp(&c, &result.provincePixels[(py+1) * sw + px], sizeof(Color))) onBorder = true;
                else { smoothed[py * sw + px] = c; continue; }

                // 3×3 majority vote
                Color candidates[9]; int votes[9] = {0}, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int ny = py + dy;
                        int nx = (px + dx + sw) % sw;
                        if (ny < 0 || ny >= sh) continue;
                        Color nc = result.provincePixels[ny * sw + nx];
                        int idx = 0;
                        while (idx < n && memcmp(&candidates[idx], &nc, sizeof(Color))) ++idx;
                        if (idx == n && n < 9) { candidates[n] = nc; votes[n] = 1; ++n; }
                        else if (idx < n) ++votes[idx];
                    }
                }
                int best = 0;
                for (int i = 1; i < n; ++i) if (votes[i] > votes[best]) best = i;
                smoothed[py * sw + px] = candidates[best];
            }
        }
        result.provincePixels.swap(smoothed);

        // Apply same smoothing to political pixels
        std::vector<Color> polSmoothed(sw * sh);
        for (int py = 0; py < sh; ++py) {
            for (int px = 0; px < sw; ++px) {
                Color c = result.politicalPixels[py * sw + px];
                bool onBorder = false;
                if (px > 0 && memcmp(&c, &result.politicalPixels[py * sw + px - 1], sizeof(Color))) onBorder = true;
                else if (px < sw-1 && memcmp(&c, &result.politicalPixels[py * sw + px + 1], sizeof(Color))) onBorder = true;
                else if (py > 0 && memcmp(&c, &result.politicalPixels[(py-1) * sw + px], sizeof(Color))) onBorder = true;
                else if (py < sh-1 && memcmp(&c, &result.politicalPixels[(py+1) * sw + px], sizeof(Color))) onBorder = true;
                else { polSmoothed[py * sw + px] = c; continue; }
                Color candidates[9]; int votes[9] = {0}, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int ny = py + dy, nx = (px + dx + sw) % sw;
                        if (ny < 0 || ny >= sh) continue;
                        Color nc = result.politicalPixels[ny * sw + nx];
                        int idx = 0;
                        while (idx < n && memcmp(&candidates[idx], &nc, sizeof(Color))) ++idx;
                        if (idx == n && n < 9) { candidates[n] = nc; votes[n] = 1; ++n; }
                        else if (idx < n) ++votes[idx];
                    }
                }
                int best = 0;
                for (int i = 1; i < n; ++i) if (votes[i] > votes[best]) best = i;
                polSmoothed[py * sw + px] = candidates[best];
            }
        }
        result.politicalPixels.swap(polSmoothed);
    }

    // Scale province/political pixels back to full resolution (nearest-neighbor)
    m_provincePixels.resize(MAP_W * MAP_H, Color{0,0,0,0});
    m_politicalPixels.resize(MAP_W * MAP_H, Color{0,0,0,0});
    for (int py = 0; py < MAP_H; ++py) {
        int si = (py / SCALE) * sw;
        int di = py * MAP_W;
        for (int px = 0; px < MAP_W; ++px)
            m_provincePixels[di + px] = result.provincePixels[si + (px / SCALE)];
    }
    for (int py = 0; py < MAP_H; ++py) {
        int si = (py / SCALE) * sw;
        int di = py * MAP_W;
        for (int px = 0; px < MAP_W; ++px)
            m_politicalPixels[di + px] = result.politicalPixels[si + (px / SCALE)];
    }

    m_provinceJson = std::move(result.provinceJson);
    m_countryJson = std::move(result.countryJson);
    m_populationJson = std::move(result.populationJson);
    m_resourcesJson = std::move(result.resourcesJson);
    m_minoritiesJson = std::move(result.minoritiesJson);
    m_minorityColorsJson = std::move(result.minorityColorsJson);

    m_hasProvinces = !m_provincePixels.empty() && !m_provinceJson.empty();
    m_hasGameData = m_hasProvinces && !m_populationJson.empty();

    // ── Parse generated JSON into editable per-province data, then merge
    //    the generator's structured outputs (ports, armies, ships, relations) ──
    rebuildProvinceCounts();
    m_hlDirty = true;
    parseGeneratedGameData();
    for (auto& [pid, lvl] : result.portLevelByPid) m_provinceData[pid].portLevel = lvl;
    for (auto& [pid, units] : result.armiesByPid) m_provinceData[pid].troops = units;
    for (auto& [pid, comp] : result.provinceCompassByPid) {
        m_provinceData[pid].compassEconomic = comp.first;
        m_provinceData[pid].compassSocial = comp.second;
    }
    m_editorShips = std::move(result.ships);
    m_editorRelations.clear();
    for (auto& [pair, rel] : result.relations) m_editorRelations[pair] = rel;
    m_countryPolicies.clear(); // country cids are freshly assigned by generation

    if (m_hasProvinces && m_renderer) {
        // ── Encode province pixels to PNG in memory ──
        std::vector<uint8_t> rawProv(MAP_W * MAP_H * 4);
        for (int i = 0; i < MAP_W * MAP_H; ++i) {
            rawProv[i*4]   = m_provincePixels[i].r;
            rawProv[i*4+1] = m_provincePixels[i].g;
            rawProv[i*4+2] = m_provincePixels[i].b;
            rawProv[i*4+3] = 255;
        }
        int pngLen = 0;
        unsigned char* pngData = stbi_write_png_to_mem(rawProv.data(), MAP_W * 4,
                                                       MAP_W, MAP_H, 4, &pngLen);
        if (pngData) {
            m_editProvinces.clear(); // drop provinces/image from a previous generation
            m_editProvinces.loadFromMemory(pngData, pngLen, m_provinceJson);
            stbi_image_free(pngData);
        }

        // ── Load countries (clear first — loadFromJson merges into existing) ──
        m_editCountries.clear();
        m_editCountries.loadFromJson(m_countryJson);

        // ── Create political texture from m_politicalPixels ──
        Image polImg{};
        polImg.data = m_politicalPixels.data();
        polImg.width = MAP_W;
        polImg.height = MAP_H;
        polImg.mipmaps = 1;
        polImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D polTex = LoadTextureFromImage(polImg);
        SetTextureFilter(polTex, TEXTURE_FILTER_POINT);
        m_renderer->setPoliticalTexture(polTex);

        // ── Compute province border texture (only if ProvinceMap loaded OK) ──
        if (m_editProvinces.getImage().data != nullptr)
            m_renderer->computeBorderTexture(m_editProvinces.getImage());

        // Fix half-res upscale artifacts (province specks on sea, bare land)
        sanitizeProvincePixels();
        computePoliticalGradient(); // match the in-game border-glow rendering
        m_polGradientDirty = false; m_polGradientFullDirty = false; // supersedes any pending partial refresh

        LoadLog() << "  Province data loaded into editor\n";
    }

    m_dirty = true;
    LoadLog() << "  Generated " << (m_hasProvinces ? "OK" : "FAILED") << "\n";
}

void MapEditor::generateGameData() {
    // Already generated alongside provinces in generateProvincesCountries().
    m_hasGameData = !m_populationJson.empty();
    m_dirty = true;
    LoadLog() << "  Game data confirmed\n";
}

// ════════════════════════════════════════════════════════════════
//  Editable game data: parsing + serialization helpers
// ════════════════════════════════════════════════════════════════

void MapEditor::parseGeneratedGameData() {
    m_provinceData.clear();
    // population.json: { "pid": pop }
    try {
        if (!m_populationJson.empty()) {
            auto pj = nlohmann::json::parse(m_populationJson);
            for (auto& [pidStr, val] : pj.items())
                m_provinceData[std::stoi(pidStr)].population = val.get<long long>();
        }
    } catch (...) { LoadLog() << "  Failed to parse population.json\n"; }
    // resources.json (generator schema: conditional resource keys, industry, fortification)
    try {
        if (!m_resourcesJson.empty()) {
            auto rj = nlohmann::json::parse(m_resourcesJson);
            for (auto& [pidStr, res] : rj.items()) {
                EditorProvinceData& d = m_provinceData[std::stoi(pidStr)];
                auto readRes = [&](const char* key, float& amt, float& boost) {
                    if (!res.contains(key)) return;
                    amt = res[key].value("a", 0.0f);
                    boost = res[key].value("b", boost);
                };
                readRes("oil", d.oil, d.oilB);
                readRes("gold", d.gold, d.goldB);
                readRes("rubber", d.rubber, d.rubberB);
                readRes("gemstones", d.gemstones, d.gemB);
                readRes("metal", d.metal, d.metalB);
                if (res.contains("industry")) {
                    d.industryLevel = res["industry"].value("level", 0);
                    d.industryIncome = res["industry"].value("income", 10.0f);
                    d.resourceIncome = res["industry"].value("resourceIncome", 0.0f);
                    d.popIncome = res["industry"].value("popIncome", 0.0f);
                    d.popModifier = res["industry"].value("popModifier", 1.0f);
                }
                d.fortification = res.value("fortification", 0);
            }
        }
    } catch (...) { LoadLog() << "  Failed to parse resources.json\n"; }
    // minority_colors.json: { "name": [r,g,b] } -> the ethnicity name registry
    m_ethnicColors.clear();
    try {
        if (!m_minorityColorsJson.empty()) {
            auto cj = nlohmann::json::parse(m_minorityColorsJson);
            for (auto& [name, rgb] : cj.items()) {
                if (rgb.size() < 3) continue;
                m_ethnicColors[name] = {(uint8_t)rgb[0].get<int>(), (uint8_t)rgb[1].get<int>(),
                                        (uint8_t)rgb[2].get<int>(), 255};
            }
        }
    } catch (...) { LoadLog() << "  Failed to parse minority_colors.json\n"; }
    // minorities.json: { "pid": [{"n":name,"p":percent}, ...] }
    try {
        if (!m_minoritiesJson.empty()) {
            auto mj = nlohmann::json::parse(m_minoritiesJson);
            for (auto& [pidStr, arr] : mj.items()) {
                EditorProvinceData& d = m_provinceData[std::stoi(pidStr)];
                d.ethnicGroups.clear();
                for (auto& e : arr)
                    d.ethnicGroups.push_back({e.value("n", "Unknown"), e.value("p", 0.0f)});
            }
        }
    } catch (...) { LoadLog() << "  Failed to parse minorities.json\n"; }
}

std::string MapEditor::buildResourcesJson() const {
    // The game's parser requires every province entry to carry all 5 resources
    // and the full industry object — emit the complete schema for each.
    nlohmann::json rj = nlohmann::json::object();
    auto resObj = [](float a, float b) { return nlohmann::json{{"a", a}, {"b", b}}; };
    for (auto& [pid, d] : m_provinceData) {
        nlohmann::json e;
        e["oil"] = resObj(d.oil, d.oilB);
        e["gold"] = resObj(d.gold, d.goldB);
        e["rubber"] = resObj(d.rubber, d.rubberB);
        e["gemstones"] = resObj(d.gemstones, d.gemB);
        e["metal"] = resObj(d.metal, d.metalB);
        e["industry"] = {
            {"level", d.industryLevel},
            {"income", d.industryIncome},
            {"specialization", ""},
            // Carried through from whatever produced the map; see EditorProvinceData.
            {"resourceIncome", d.resourceIncome},
            {"popIncome", d.popIncome},
            {"popModifier", d.popModifier}
        };
        e["fortification"] = d.fortification;
        rj[std::to_string(pid)] = e;
    }
    return rj.empty() ? std::string() : rj.dump(2);
}

std::string MapEditor::buildMinoritiesJson() const {
    nlohmann::json mj = nlohmann::json::object();
    for (auto& [pid, d] : m_provinceData) {
        if (d.ethnicGroups.empty()) continue;
        nlohmann::json arr = nlohmann::json::array();
        for (auto& [name, pct] : d.ethnicGroups)
            arr.push_back({{"n", name}, {"p", pct}});
        mj[std::to_string(pid)] = arr;
    }
    return mj.empty() ? std::string() : mj.dump(2);
}

std::string MapEditor::buildMinorityColorsJson() const {
    nlohmann::json cj = nlohmann::json::object();
    for (auto& [name, c] : m_ethnicColors) cj[name] = {c.r, c.g, c.b};
    return cj.empty() ? std::string() : cj.dump(2);
}

std::string MapEditor::buildProvinceCompassJson() const {
    nlohmann::json cj = nlohmann::json::object();
    for (auto& [pid, d] : m_provinceData) {
        if (d.compassEconomic == 0.0f && d.compassSocial == 0.0f) continue;
        // Negated on the way out, so the key named "left" holds a number that
        // MEANS left. It did not: this wrote economic-convention values under
        // left/auth names, which made a generated map's province compasses the
        // mirror of a shipped map's while sharing the file format. The game
        // converts left/auth on load, so the two disagreed by a sign, the
        // province-to-government distance became a sum, and generated maps
        // dissolved -- 24,030 countries alive by turn 14 of a training run.
        cj[std::to_string(pid)] = {{"left", -d.compassEconomic},
                                   {"auth", -d.compassSocial}};
    }
    return cj.empty() ? std::string() : cj.dump(2);
}

std::vector<std::string> MapEditor::ethnicityNamePool() const {
    std::vector<std::string> names;
    names.reserve(m_ethnicColors.size());
    for (auto& [name, c] : m_ethnicColors) names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

std::string MapEditor::buildPopulationJson() const {
    nlohmann::json pj = nlohmann::json::object();
    for (auto& [pid, d] : m_provinceData) pj[std::to_string(pid)] = d.population;
    return pj.empty() ? std::string() : pj.dump(2);
}

std::string MapEditor::buildPortsJson() const {
    nlohmann::json j = nlohmann::json::object();
    for (auto& [pid, d] : m_provinceData)
        if (d.portLevel > 0) j[std::to_string(pid)] = {{"level", d.portLevel}};
    return j.empty() ? std::string() : j.dump(2);
}

std::string MapEditor::buildArmiesJson() const {
    nlohmann::json j = nlohmann::json::object();
    for (auto& [pid, d] : m_provinceData) {
        if (d.troops.empty()) continue;
        nlohmann::json arr = nlohmann::json::array();
        for (auto& u : d.troops)
            arr.push_back({{"country_id", u.countryId}, {"count", u.count}});
        j[std::to_string(pid)] = arr;
    }
    return j.empty() ? std::string() : j.dump(2);
}

std::string MapEditor::buildShipsJson() const {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& s : m_editorShips) {
        arr.push_back({
            {"country_id", s.countryId},
            {"type", s.type},
            {"lat", s.lat},
            {"lon", s.lon},
            {"health", s.health},
            {"crew", s.crew}
        });
    }
    return arr.empty() ? std::string() : arr.dump(2);
}

std::string MapEditor::buildRelationsJson() const {
    nlohmann::json j = nlohmann::json::object();
    auto& all = m_editCountries.getAll();
    for (auto& [pair, rel] : m_editorRelations) {
        if (!rel.war && !rel.alliance && !rel.nonAggression && !rel.guarantee) continue;
        auto a = all.find(pair.first), b = all.find(pair.second);
        if (a == all.end() || b == all.end()) continue;
        const std::string& isoA = a->second.isoA3;
        const std::string& isoB = b->second.isoA3;
        if (isoA.empty() || isoB.empty()) continue;
        nlohmann::json r = {{"war", rel.war}, {"ally", rel.alliance},
                            {"nonAggression", rel.nonAggression}, {"guarantee", rel.guarantee}};
        j[isoA][isoB] = r;
        j[isoB][isoA] = r;
    }
    return j.empty() ? std::string() : j.dump(2);
}

void MapEditor::ensureIsoCodes() {
    auto& all = m_editCountries.getAll();
    std::unordered_set<std::string> used = {"UNC", "BLC", "SPC"};
    for (auto& [cid, c] : all)
        if (!c.isoA3.empty()) used.insert(c.isoA3);
    std::vector<int> cids;
    for (auto& [cid, c] : all) if (c.isoA3.empty()) cids.push_back(cid);
    std::sort(cids.begin(), cids.end()); // deterministic assignment order
    for (int cid : cids) {
        Country& c = all[cid];
        c.isoA3 = makeIsoA3(c.name, used);
        used.insert(c.isoA3);
    }
}

std::vector<uint8_t> MapEditor::encodePng(const std::vector<Color>& px, int w, int h) {
    std::vector<uint8_t> raw(w * h * 4);
    for (int i = 0; i < w * h; ++i) {
        raw[i*4] = px[i].r; raw[i*4+1] = px[i].g; raw[i*4+2] = px[i].b; raw[i*4+3] = 255;
    }
    // A layer with 256 or fewer distinct colours indexes to a fraction of the
    // truecolour size and decodes back byte-identical -- land_sea holds three.
    // Empty means "too many colours to index", which is the province layer.
    if (std::vector<uint8_t> indexed = pngw::encodeIndexedRGBA(raw.data(), w, h); !indexed.empty())
        return indexed;
    // stb_image_write's default settings run a per-scanline filter-choice
    // heuristic and a high zlib compression level — fine for small images,
    // but brutally slow at map resolution (8192x4096, called 3x per save).
    // These maps are mostly flat-colored regions, so a fixed filter and a
    // lower compression level cost very little file size for a large speedup.
    stbi_write_force_png_filter = 0;
    stbi_write_png_compression_level = 4;
    int len = 0;
    unsigned char* png = stbi_write_png_to_mem(raw.data(), w * 4, w, h, 4, &len);
    std::vector<uint8_t> out;
    if (png) { out.assign(png, png + len); free(png); }
    return out;
}

std::string MapEditor::buildCountriesJson() const {
    nlohmann::json cj;
    for (auto& [cid, c] : m_editCountries.getAll()) {
        nlohmann::json e;
        e["id"] = c.id;
        e["name"] = c.name;
        e["iso_a3"] = c.isoA3;
        char ch[8]; snprintf(ch, sizeof(ch), "#%02x%02x%02x", c.color.r, c.color.g, c.color.b);
        e["color"] = std::string(ch);
        e["treasury"] = c.treasury;
        e["compass_economic"] = c.compassEconomic;
        e["compass_social"] = c.compassSocial;
        nlohmann::json rarr = nlohmann::json::array();
        for (auto& r : c.research) rarr.push_back(r);
        e["research"] = rarr;
        // Rebuild flag JSON from FlagPattern
        auto flagToJson = [](const FlagPattern& fp) -> nlohmann::json {
            nlohmann::json f;
            if (!fp.imagePath.empty()) { f["image"] = fp.imagePath; return f; }
            const char* tn = "solid";
            switch (fp.type) {
                case FlagType::SOLID: tn = "solid"; break;
                case FlagType::HSTRIPES_2: tn = "hstripes_2"; break;
                case FlagType::HSTRIPES_3: tn = "hstripes_3"; break;
                case FlagType::VSTRIPES_2: tn = "vstripes_2"; break;
                case FlagType::VSTRIPES_3: tn = "vstripes_3"; break;
                case FlagType::DIAGONAL_L: tn = "diagonal_l"; break;
                case FlagType::DIAGONAL_R: tn = "diagonal_r"; break;
                case FlagType::TRIANGLE: tn = "triangle"; break;
                case FlagType::TRIANGLE_DOUBLE: tn = "triangle_double"; break;
                case FlagType::QUARTERED: tn = "quartered"; break;
                case FlagType::SALTIR: tn = "saltir"; break;
                case FlagType::CANTON: tn = "canton"; break;
                case FlagType::PALE: tn = "pale"; break;
                case FlagType::FESS: tn = "fess"; break;
                case FlagType::CROSS_NORDIC: tn = "cross_nordic"; break;
                case FlagType::CROSS_GREEK: tn = "cross_greek"; break;
                case FlagType::STRIPED_EDGE: tn = "striped_edge"; break;
                case FlagType::SUNBURST: tn = "sunburst"; break;
                default: tn = "solid"; break;
            }
            f["type"] = tn;
            nlohmann::json cols = nlohmann::json::array();
            for (auto& cl : fp.colors) {
                char buf[8]; snprintf(buf, sizeof(buf), "#%02x%02x%02x", cl.r, cl.g, cl.b);
                cols.push_back(std::string(buf));
            }
            f["colors"] = cols;
            f["starCount"] = fp.starCount;
            if (!fp.symbols.empty()) {
                nlohmann::json syms = nlohmann::json::array();
                for (auto& sym : fp.symbols) {
                    nlohmann::json s;
                    const char* st = "star_5";
                    switch (sym.type) {
                        case SymbolType::STAR_5: st = "star_5"; break;
                        case SymbolType::STAR_6: st = "star_6"; break;
                        case SymbolType::CRESCENT: st = "crescent"; break;
                        case SymbolType::CRESCENT_STAR: st = "crescent_star"; break;
                        case SymbolType::CROSS_LATIN: st = "cross_latin"; break;
                        case SymbolType::CROSS_SALTIR: st = "cross_saltir"; break;
                        case SymbolType::CIRCLE: st = "circle"; break;
                        case SymbolType::DIAMOND: st = "diamond"; break;
                        case SymbolType::SUN: st = "sun"; break;
                        case SymbolType::GEAR: st = "gear"; break;
                        case SymbolType::WREATH: st = "wreath"; break;
                        case SymbolType::HAMMER: st = "hammer"; break;
                        case SymbolType::LIGHTNING: st = "lightning"; break;
                        case SymbolType::SUN_SPLENDOUR: st = "sun_splendour"; break;
                        case SymbolType::ANCHOR: st = "anchor"; break;
                        case SymbolType::TORCH: st = "torch"; break;
                        case SymbolType::ROSE: st = "rose"; break;
                        case SymbolType::FASCES: st = "fasces"; break;
                        case SymbolType::CROSS_PATTEE: st = "cross_pattee"; break;
                        case SymbolType::STAR_4: st = "star_4"; break;
                        case SymbolType::STAR_OF_DAVID: st = "star_of_david"; break;
                        case SymbolType::MOUNTAIN: st = "mountain"; break;
                        case SymbolType::TREE: st = "tree"; break;
                        case SymbolType::TEXT_BLOCK: st = "text_block"; break;
                        default: st = "star_5"; break;
                    }
                    s["type"] = st;
                    s["x"] = sym.x; s["y"] = sym.y; s["size"] = sym.size;
                    s["count"] = sym.count;
                    nlohmann::json sc = nlohmann::json::array();
                    for (auto& cl : sym.colors) {
                        char buf[8]; snprintf(buf, sizeof(buf), "#%02x%02x%02x", cl.r, cl.g, cl.b);
                        sc.push_back(std::string(buf));
                    }
                    s["colors"] = sc;
                    syms.push_back(s);
                }
                f["symbols"] = syms;
            }
            return f;
        };
        e["flag_actual"] = flagToJson(c.flagActual);
        e["flag_censored"] = flagToJson(c.flagCensored);
        cj[std::to_string(cid)] = e;
    }
    return cj.dump(2);
}

std::string MapEditor::generateAndExportHeadless(const GeneratorParams& p, const std::string& mapName) {
    m_mapName = mapName;
    // NOT enabling m_fastGen. SCALE 4 cuts generation from ~12s to ~8s, but it
    // point-samples the land/sea map every 4th pixel, so small islands and thin
    // land bridges disappear and connected-component analysis sees a different
    // world. Measured on seed 4242, islands scenario, same map seed: landings
    // fell 406 -> 204 and survivors 27 -> 18, i.e. the archipelago scenarios
    // quietly became land campaigns. Flip this to true only if map throughput
    // matters more than keeping the naval scenarios intact.
    // initBlankMap normally seeds the land/sea map before any generation; do
    // the state half here (no renderer). Without it isLand() answers "sea"
    // for the whole map and sanitizeProvincePixels garbage-collects every
    // province out of the export.
    m_pixels.assign((size_t)MAP_W * MAP_H, COL_SEA);
    m_editLandSea.setFromPixels(m_pixels.data(), MAP_W, MAP_H);
    // Same sequence the deferred-generation path in update() runs, minus the
    // overlay frames. No m_renderer exists, so every texture branch is skipped.
    generateMap(p);
    generateProvincesCountries();
    generateGameData();
    if (!m_hasProvinces) return std::string();

    // The renderer path of generateProvincesCountries also sets up editor
    // STATE the export depends on (m_editProvinces, m_editCountries, pixel
    // sanitation) — without it the .odmap ships an empty countries.json.
    // Recreate just the state here; textures stay skipped.
    if (!m_renderer) {
        std::vector<uint8_t> rawProv(MAP_W * MAP_H * 4);
        for (int i = 0; i < MAP_W * MAP_H; ++i) {
            rawProv[i*4]   = m_provincePixels[i].r;
            rawProv[i*4+1] = m_provincePixels[i].g;
            rawProv[i*4+2] = m_provincePixels[i].b;
            rawProv[i*4+3] = 255;
        }
        int pngLen = 0;
        unsigned char* pngData = stbi_write_png_to_mem(rawProv.data(), MAP_W * 4,
                                                       MAP_W, MAP_H, 4, &pngLen);
        if (pngData) {
            m_editProvinces.clear();
            m_editProvinces.loadFromMemory(pngData, pngLen, m_provinceJson);
            stbi_image_free(pngData);
        }
        m_editCountries.clear();
        m_editCountries.loadFromJson(m_countryJson);
        sanitizeProvincePixels();
    }

    std::string path = exportODMap();
    return (!path.empty() && fs::exists(path)) ? path : std::string();
}

std::string MapEditor::exportODMap(const std::string& destPath) {
    if (!m_hasProvinces) {
        LoadLog() << "  No provinces to export. Generate provinces first.\n";
        return {};
    }
    std::string odmPath = destPath;
    if (odmPath.empty()) {
        fs::create_directories(m_dataDir + "custom_maps/");
        odmPath = m_dataDir + "custom_maps/" +
                  (m_mapName.empty() ? std::string("map") : m_mapName) + ".odmap";
    } else {
        // A path out of the file dialog can name a directory that was deleted
        // between the dialog opening and Export being pressed.
        std::error_code ec;
        fs::create_directories(fs::path(odmPath).parent_path(), ec);
    }

    // Write temp files
    auto writeStr = [](const std::string& path, const std::string& content) {
        std::ofstream f(path, std::ios::binary); f << content; f.close();
    };

    // Per-map staging dir: two concurrent exporters (e.g. two --train-ai
    // processes, whose map names already carry their PID) must not write into
    // the same tmp_export/ and corrupt each other's intermediate files.
    std::string tmpDir = m_dataDir + "tmp_export_" + m_mapName + "/";
    fs::create_directories(tmpDir);

    // PNG encoding of the 8192x4096 layers dominates headless export —
    // profiled at more self time than the terrain generator itself, because
    // stb tries all five row filters per scanline and then runs a long LZ77
    // hash chain over 33M pixels. The interactive save path already makes this
    // trade (see writePngFast above); the training path was still on stb's
    // defaults. These maps are broad flat-coloured regions, so a fixed filter
    // and a short chain cost very little file size. Restored on scope exit
    // since these are stb globals shared with the other export paths. Only
    // the province layer still reaches stb — land/sea goes out indexed.
    struct PngSpeedGuard {
        int filter, level;
        PngSpeedGuard() : filter(stbi_write_force_png_filter),
                          level(stbi_write_png_compression_level) {
            stbi_write_force_png_filter = 0;
            stbi_write_png_compression_level = 5; // stb clamps anything lower to 5
        }
        ~PngSpeedGuard() {
            stbi_write_force_png_filter = filter;
            stbi_write_png_compression_level = level;
        }
    } pngSpeedGuard;

    // Save land_sea.png and provinces.png. Through encodePng, which indexes
    // the land/sea layer down to two bits a pixel; political.png is NOT
    // written at all. The game has never read it -- Game_Loading's needed[]
    // list does not name it, and generatePoliticalTexture() draws the board
    // from province ownership at load -- and the editor recomputes the same
    // picture with computePoliticalGradient(). It was four fifths of every
    // archive for a layer nothing opened.
    auto writeBytes = [](const std::string& path, const std::vector<uint8_t>& bytes) {
        if (bytes.empty()) return;
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    };
    writeBytes(tmpDir + "land_sea.png", encodePng(m_pixels, MAP_W, MAP_H));
    if (!m_provincePixels.empty())
        writeBytes(tmpDir + "provinces.png", encodePng(m_provincePixels, MAP_W, MAP_H));

    // Save JSON files (countries/relations need ISO codes assigned first)
    ensureIsoCodes();
    writeStr(tmpDir + "provinces.json", m_provinceJson);
    // countries.json — embed dropped-in custom flag SVGs into the archive and
    // point the "image" fields at the embedded copies
    std::vector<std::pair<std::string, std::string>> customFlags; // archive name -> disk path
    {
        std::string cj = buildCountriesJson();
        try {
            auto j = nlohmann::json::parse(cj);
            for (auto& [cidStr, e] : j.items()) {
                auto patchFlag = [&](const char* key) {
                    if (!e.contains(key) || !e[key].contains("image")) return;
                    std::string p = e[key]["image"].get<std::string>();
                    if (p.empty()) return;
                    std::ifstream f(p, std::ios::binary);
                    if (!f) return;
                    std::string arcName = "flags/custom_" + cidStr + ".svg";
                    bool known = false;
                    for (auto& [an, fp] : customFlags) if (an == arcName) { known = true; break; }
                    if (!known) customFlags.push_back({arcName, p});
                    e[key]["image"] = arcName;
                };
                patchFlag("flag_actual");
                patchFlag("flag_censored");
            }
            cj = j.dump(2);
        } catch (...) {}
        writeStr(tmpDir + "countries.json", cj);
    }
    // Prefer the parsed/editable data; fall back to raw generated strings
    std::string popJson = buildPopulationJson();
    std::string resJson = buildResourcesJson();
    writeStr(tmpDir + "population.json", popJson.empty() ? m_populationJson : popJson);
    writeStr(tmpDir + "resources.json", resJson.empty() ? m_resourcesJson : resJson);
    std::string portsJson = buildPortsJson();
    std::string armiesJson = buildArmiesJson();
    std::string shipsJson = buildShipsJson();
    std::string relationsJson = buildRelationsJson();
    if (!portsJson.empty()) writeStr(tmpDir + "ports.json", portsJson);
    if (!armiesJson.empty()) writeStr(tmpDir + "armies.json", armiesJson);
    if (!shipsJson.empty()) writeStr(tmpDir + "ships.json", shipsJson);
    if (!relationsJson.empty()) writeStr(tmpDir + "relations.json", relationsJson);
    std::string minoritiesJsonOut = buildMinoritiesJson();
    std::string minorityColorsJsonOut = buildMinorityColorsJson();
    std::string provCompassJson = buildProvinceCompassJson();
    if (!minoritiesJsonOut.empty()) writeStr(tmpDir + "minorities.json", minoritiesJsonOut);
    if (!minorityColorsJsonOut.empty()) writeStr(tmpDir + "minority_colors.json", minorityColorsJsonOut);
    if (!provCompassJson.empty()) writeStr(tmpDir + "political_compass.json", provCompassJson);
    std::string claimsJson = buildClaimsJson();
    if (!claimsJson.empty()) writeStr(tmpDir + "claims.json", claimsJson);
    std::string policiesJson = buildPoliciesJson();
    std::string startingPoliciesJson = buildStartingPoliciesJson();
    std::string startingMinorityPoliciesJson = buildStartingMinorityPoliciesJson();
    if (!policiesJson.empty()) writeStr(tmpDir + "policies.json", policiesJson);
    if (!startingPoliciesJson.empty()) writeStr(tmpDir + "starting_policies.json", startingPoliciesJson);
    if (!startingMinorityPoliciesJson.empty()) writeStr(tmpDir + "starting_minority_policies.json", startingMinorityPoliciesJson);
    if (m_licenseCustom && !m_licenseText.empty()) {
        fs::create_directories(tmpDir + "licenses/");
        writeStr(tmpDir + "licenses/LICENSE.txt", m_licenseText);
    }

    // Scripts
    if (!m_scripts.empty()) {
        fs::create_directories(tmpDir + "scripts/");
        for (auto& [sname, content] : m_scripts)
            writeStr(tmpDir + "scripts/" + sname, content);
    }

    // Metadata
    nlohmann::json meta;
    meta["name"] = m_mapName;
    meta["map_date"] = m_mapDate.empty() ? "January 2000" : m_mapDate;
    meta["author"] = m_author;
    meta["license"] = m_license;
    meta["has_scripts"] = !m_scripts.empty();
    writeStr(tmpDir + "metadata.json", meta.dump());

    // Thumbnail: the author's custom image if they set one, else downsample
    // the political map. Either way it ships as a THUMB_W x THUMB_H PNG.
    // Sampled straight out of m_politicalPixels rather than off a file, since
    // the political layer is no longer written to the staging dir.
    {
        const int tw = THUMB_W, th = THUMB_H;
        int sw = 0, sh = 0, sc = 0;
        unsigned char* srcData = m_thumbnailPath.empty()
                                     ? nullptr
                                     : stbi_load(m_thumbnailPath.c_str(), &sw, &sh, &sc, 4);
        if (!srcData && !m_thumbnailPath.empty())
            LoadLog() << "  Custom thumbnail unreadable, falling back to auto-generated\n";
        const unsigned char* src = srcData;
        if (!src && !m_politicalPixels.empty()) {
            src = (const unsigned char*)m_politicalPixels.data();
            sw = MAP_W; sh = MAP_H;
        }
        if (src && sw > 0 && sh > 0) {
            std::vector<unsigned char> thumbData((size_t)tw * th * 4);
            for (int y = 0; y < th; ++y)
                for (int x = 0; x < tw; ++x)
                    std::memcpy(&thumbData[((size_t)y * tw + x) * 4],
                                &src[((size_t)(y * sh / th) * sw + x * sw / tw) * 4], 4);
            stbi_write_png((tmpDir + "thumb.png").c_str(), tw, th, 4, thumbData.data(), tw * 4);
        }
        if (srcData) stbi_image_free(srcData);
    }

    // Package into .odmap
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, odmPath.c_str(), 0)) {
        LoadLog() << "  Failed to create .odmap: " << odmPath << "\n";
        fs::remove_all(tmpDir);
        return {};
    }

    auto addFile = [&](const std::string& name) {
        std::string full = tmpDir + name;
        std::ifstream f(full, std::ios::binary | std::ios::ate);
        if (!f) { LoadLog() << "  Skipping " << name << "\n"; return; }
        std::streamsize sz = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> buf(sz);
        if (f.read((char*)buf.data(), sz))
            mz_zip_writer_add_mem(&zip, name.c_str(), buf.data(), buf.size(), MZ_BEST_COMPRESSION);
        f.close();
    };

    addFile("land_sea.png");
    addFile("provinces.png");
    addFile("provinces.json");
    addFile("countries.json");
    addFile("population.json");
    addFile("resources.json");
    if (!portsJson.empty()) addFile("ports.json");
    if (!armiesJson.empty()) addFile("armies.json");
    if (!shipsJson.empty()) addFile("ships.json");
    if (!relationsJson.empty()) addFile("relations.json");
    if (!claimsJson.empty()) addFile("claims.json");
    if (!minoritiesJsonOut.empty()) addFile("minorities.json");
    if (!minorityColorsJsonOut.empty()) addFile("minority_colors.json");
    if (!provCompassJson.empty()) addFile("political_compass.json");
    if (!policiesJson.empty()) addFile("policies.json");
    if (!startingPoliciesJson.empty()) addFile("starting_policies.json");
    // This was written to the temp dir but never added to the archive, so every
    // Ethnic Relations setting was silently discarded on export.
    if (!startingMinorityPoliciesJson.empty()) addFile("starting_minority_policies.json");
    if (m_licenseCustom && !m_licenseText.empty()) addFile("licenses/LICENSE.txt");

    // Exporting with no policy data is almost always a mistake rather than an
    // intent, and it used to fail silently — say so in the log.
    if (startingPoliciesJson.empty())
        LoadLog() << "  NOTE: no country starting policies to export\n";
    if (startingMinorityPoliciesJson.empty())
        LoadLog() << "  NOTE: no ethnic-relations settings to export\n";
    addFile("metadata.json");
    addFile("thumb.png");
    // Embed custom flag SVGs
    for (auto& [arcName, filePath] : customFlags) {
        std::ifstream f(filePath, std::ios::binary | std::ios::ate);
        if (!f) continue;
        std::streamsize sz = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> buf(sz);
        if (f.read((char*)buf.data(), sz))
            mz_zip_writer_add_mem(&zip, arcName.c_str(), buf.data(), buf.size(), MZ_BEST_COMPRESSION);
    }
    if (m_scripts.empty()) {
        mz_zip_writer_add_mem(&zip, "scripts/", nullptr, 0, MZ_NO_COMPRESSION);
    } else {
        for (auto& [sname, content] : m_scripts) addFile("scripts/" + sname);
    }

    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    fs::remove_all(tmpDir);

    LoadLog() << "  Exported .odmap to: " << odmPath << "\n";
    return odmPath;
}

// ════════════════════════════════════════════════════════════════
//  .uodmap project save / load (full editor state)
// ════════════════════════════════════════════════════════════════

bool MapEditor::saveProject() {
    if (m_projectState != PROJ_EDITING) return false;
    drawMiniLoadingScreen(0.05f, "Saving map...");
    fs::create_directories(m_dataDir + "projects/");
    std::string name = m_mapName.empty() ? "untitled" : m_mapName;
    std::string path = m_dataDir + "projects/" + name + ".uodmap";

    // Gather entries in memory (PNGs are already compressed — store them raw)
    std::vector<std::pair<std::string, std::vector<uint8_t>>> entries;
    auto addStr = [&](const std::string& n, const std::string& s) {
        if (s.empty()) return;
        entries.emplace_back(n, std::vector<uint8_t>(s.begin(), s.end()));
    };

    entries.emplace_back("land_sea.png", encodePng(m_pixels, MAP_W, MAP_H));
    if (m_hasProvinces) {
        ensureIsoCodes();
        drawMiniLoadingScreen(0.3f, "Encoding map images...");
        entries.emplace_back("provinces.png", encodePng(m_provincePixels, MAP_W, MAP_H));
        // political.png is not saved: it is province ownership run through
        // computePoliticalGradient(), recomputed on load for a fraction of
        // what storing a third 8192x4096 layer costs.
        drawMiniLoadingScreen(0.6f, "Writing game data...");
        addStr("provinces.json", m_provinceJson);
        addStr("countries.json", buildCountriesJson());
        std::string pop = buildPopulationJson();
        std::string res = buildResourcesJson();
        addStr("population.json", pop.empty() ? m_populationJson : pop);
        addStr("resources.json", res.empty() ? m_resourcesJson : res);
        addStr("ports.json", buildPortsJson());
        addStr("armies.json", buildArmiesJson());
        addStr("ships.json", buildShipsJson());
        addStr("relations.json", buildRelationsJson());
        addStr("claims.json", buildClaimsJson());
        addStr("minorities.json", buildMinoritiesJson());
        addStr("minority_colors.json", buildMinorityColorsJson());
        addStr("political_compass.json", buildProvinceCompassJson());
        addStr("policies.json", buildPoliciesJson());
        addStr("starting_policies.json", buildStartingPoliciesJson());
        addStr("starting_minority_policies.json", buildStartingMinorityPoliciesJson());
    }
    for (auto& [sname, content] : m_scripts)
        addStr("scripts/" + sname, content);

    nlohmann::json ed;
    ed["version"] = 1;
    ed["name"] = m_mapName;
    ed["author"] = m_author;
    ed["license"] = m_license;
    ed["licenseCustom"] = m_licenseCustom;
    ed["licenseText"] = m_licenseText;
    ed["mapDate"] = m_mapDate;
    ed["thumbnailPath"] = m_thumbnailPath;
    // Country policy selections keyed by cid (survives even before ISO assignment)
    {
        nlohmann::json cp = nlohmann::json::object();
        for (auto& [cid, ids] : m_countryPolicies) {
            if (ids.empty()) continue;
            nlohmann::json arr = nlohmann::json::array();
            for (auto& id : ids) arr.push_back(id);
            cp[std::to_string(cid)] = arr;
        }
        ed["countryPolicies"] = cp;
    }
    // Ethnicity policy selections keyed by ethnicity name
    {
        nlohmann::json ep = nlohmann::json::object();
        for (auto& [name, ids] : m_ethnicityPolicies) {
            if (ids.empty()) continue;
            nlohmann::json arr = nlohmann::json::array();
            for (auto& id : ids) arr.push_back(id);
            ep[name] = arr;
        }
        ed["ethnicityPolicies"] = ep;
    }
    // Government-to-ethnicity relation choices (in-game Ethnic tab parity)
    {
        nlohmann::json er = nlohmann::json::object();
        for (auto& [iso, byName] : m_ethnicRelations) {
            nlohmann::json perIso = nlohmann::json::object();
            for (auto& [name, opts] : byName) {
                if (opts.empty()) continue;
                perIso[name] = opts;
            }
            if (!perIso.empty()) er[iso] = perIso;
        }
        ed["ethnicRelations"] = er;
    }
    ed["gen"]["seed"] = m_genParams.seed;
    ed["gen"]["landCoverage"] = m_genParams.landCoverage;
    ed["gen"]["numContinents"] = m_genParams.numContinents;
    ed["gen"]["numCountries"] = m_genParams.numCountries;
    ed["gen"]["jaggedness"] = m_genParams.jaggedness;
    ed["gen"]["provinceDensity"] = m_genParams.provinceDensity;
    addStr("editor.json", ed.dump(2));

    drawMiniLoadingScreen(0.85f, "Writing archive...");
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, path.c_str(), 0)) {
        m_saveStatus = "Save failed!";
        m_saveStatusTimer = 3.0f;
        LoadLog() << "  Failed to create " << path << "\n";
        return false;
    }
    for (auto& [n, buf] : entries) {
        bool isPng = n.size() > 4 && n.substr(n.size() - 4) == ".png";
        mz_zip_writer_add_mem(&zip, n.c_str(), buf.data(), buf.size(),
                              isPng ? MZ_NO_COMPRESSION : MZ_BEST_COMPRESSION);
    }
    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    drawMiniLoadingScreen(1.0f, "Save complete");

    m_dirty = false;
    m_saveStatus = "Saved " + name + ".uodmap";
    m_saveStatusTimer = 3.0f;
    LoadLog() << "  Saved project to " << path << "\n";
    return true;
}

void MapEditor::rebuildFromPixelState() {
    if (!m_renderer) return;
    m_editLandSea.updatePixels(m_pixels.data());
    if (m_hasProvinces) {
        Image polImg{};
        polImg.data = m_politicalPixels.data();
        polImg.width = MAP_W;
        polImg.height = MAP_H;
        polImg.mipmaps = 1;
        polImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D polTex = LoadTextureFromImage(polImg);
        SetTextureFilter(polTex, TEXTURE_FILTER_POINT);
        m_renderer->setPoliticalTexture(polTex);
        if (m_editProvinces.getImage().data != nullptr)
            m_renderer->computeBorderTexture(m_editProvinces.getImage());
    }
}

bool MapEditor::loadProject(const std::string& path) {
    drawMiniLoadingScreen(0.05f, "Opening project...");
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) return false;
    std::map<std::string, std::vector<uint8_t>> entries;
    int n = (int)mz_zip_reader_get_num_files(&zip);
    for (int i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (st.m_is_directory) continue;
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (!p) continue;
        entries[st.m_filename] = std::vector<uint8_t>((uint8_t*)p, (uint8_t*)p + sz);
        mz_free(p);
    }
    mz_zip_reader_end(&zip);

    auto getStr = [&](const std::string& name) -> std::string {
        auto it = entries.find(name);
        if (it == entries.end()) return {};
        return std::string(it->second.begin(), it->second.end());
    };

    // land_sea.png (required, must match editor resolution)
    auto lsIt = entries.find("land_sea.png");
    if (lsIt == entries.end()) return false;
    Image ls = LoadImageFromMemory(".png", lsIt->second.data(), (int)lsIt->second.size());
    if (ls.data == nullptr) return false;
    if (ls.width != MAP_W || ls.height != MAP_H) { UnloadImage(ls); return false; }
    ImageFormat(&ls, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    initBlankMap(); // ensures renderer + m_pixels exist
    memcpy(m_pixels.data(), ls.data, (size_t)MAP_W * MAP_H * sizeof(Color));
    UnloadImage(ls);

    // Reset generated/edited state
    m_hasProvinces = false;
    m_hasGameData = false;
    m_provincePixels.clear();
    m_politicalPixels.clear();
    m_minoritiesJson.clear();
    m_minorityColorsJson.clear();
    m_ethnicColors.clear();
    m_provinceData.clear();
    m_provincePixelCounts.clear();
    m_editorShips.clear();
    m_editorRelations.clear();
    m_countryPolicies.clear();
    m_ethnicityPolicies.clear();
    m_ethnicRelations.clear();
    m_thumbnailPath.clear();
    m_thumbnailTexPath.clear();
    m_editorClaims.clear();
    m_claimsPixels.clear();
    m_claimsOverlayCid = -2;
    m_editProvinces.clear();
    m_editCountries.clear();
    m_scripts.clear();
    m_scriptSel = -1;
    m_selectedProvince = -1;
    m_selectedCountry = -1;
    m_selectedShip = -1;
    m_renderer->setPoliticalTexture(Texture2D{});
    m_renderer->setEditorOverlay(Texture2D{});
    m_renderer->setShowEditorOverlay(false);
    m_renderer->clearHighlight();

    // Provinces + countries (optional: project may be landmass-only)
    std::string provJson = getStr("provinces.json");
    std::string countryJson = getStr("countries.json");
    auto ppIt = entries.find("provinces.png");
    // A political.png in an older project is ignored rather than read: the
    // gradient pass at the end of this function overwrites it either way.
    if (ppIt != entries.end() && !provJson.empty() && !countryJson.empty()) {
        Image pp = LoadImageFromMemory(".png", ppIt->second.data(), (int)ppIt->second.size());
        if (pp.data && pp.width == MAP_W && pp.height == MAP_H) {
            ImageFormat(&pp, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            m_provincePixels.resize(MAP_W * MAP_H);
            // Sized, not filled: computePoliticalGradient() at the end of this
            // function redraws every pixel of it regardless.
            m_politicalPixels.resize(MAP_W * MAP_H);
            memcpy(m_provincePixels.data(), pp.data, (size_t)MAP_W * MAP_H * sizeof(Color));

            m_editProvinces.loadFromMemory(ppIt->second.data(), (int)ppIt->second.size(), provJson);
            m_editCountries.loadFromJson(countryJson);
            m_provinceJson = provJson;
            m_countryJson = countryJson;
            m_populationJson = getStr("population.json");
            m_resourcesJson = getStr("resources.json");
            m_minoritiesJson = getStr("minorities.json");
            m_minorityColorsJson = getStr("minority_colors.json");
            m_hasProvinces = true;
            m_hasGameData = !m_populationJson.empty();

            // Per-province data + ports + armies
            parseGeneratedGameData();
            try {
                std::string ports = getStr("ports.json");
                if (!ports.empty()) {
                    auto j = nlohmann::json::parse(ports);
                    for (auto& [pidStr, info] : j.items())
                        m_provinceData[std::stoi(pidStr)].portLevel = info.value("level", 1);
                }
            } catch (...) { LoadLog() << "  Bad ports.json in project\n"; }
            try {
                std::string armies = getStr("armies.json");
                if (!armies.empty()) {
                    auto j = nlohmann::json::parse(armies);
                    for (auto& [pidStr, units] : j.items()) {
                        auto& troops = m_provinceData[std::stoi(pidStr)].troops;
                        troops.clear();
                        for (auto& u : units)
                            troops.push_back(ArmyUnit{u.value("country_id", 0), u.value("count", 0)});
                    }
                }
            } catch (...) { LoadLog() << "  Bad armies.json in project\n"; }
            try {
                std::string ships = getStr("ships.json");
                if (!ships.empty()) {
                    auto j = nlohmann::json::parse(ships);
                    for (auto& e : j) {
                        NavyShip s;
                        s.countryId = e.value("country_id", 0);
                        s.type = e.value("type", "destroyer");
                        s.lat = e.value("lat", 0.0);
                        s.lon = e.value("lon", 0.0);
                        s.health = e.value("health", 100);
                        s.crew = e.value("crew", 0);
                        m_editorShips.push_back(s);
                    }
                }
            } catch (...) { LoadLog() << "  Bad ships.json in project\n"; }
            try {
                std::string rels = getStr("relations.json");
                if (!rels.empty()) {
                    // iso-keyed -> cid pairs
                    std::unordered_map<std::string, int> cidByIso;
                    for (auto& [cid, c] : m_editCountries.getAll())
                        if (!c.isoA3.empty()) cidByIso[c.isoA3] = cid;
                    auto j = nlohmann::json::parse(rels);
                    for (auto& [isoA, targets] : j.items()) {
                        auto a = cidByIso.find(isoA);
                        if (a == cidByIso.end()) continue;
                        for (auto& [isoB, r] : targets.items()) {
                            auto b = cidByIso.find(isoB);
                            if (b == cidByIso.end()) continue;
                            CountryRelation cr;
                            cr.war = r.value("war", false);
                            cr.alliance = r.value("ally", false);
                            cr.nonAggression = r.value("nonAggression", false);
                            cr.guarantee = r.value("guarantee", false);
                            if (!cr.war && !cr.alliance && !cr.nonAggression && !cr.guarantee) continue;
                            auto key = std::make_pair(std::min(a->second, b->second),
                                                      std::max(a->second, b->second));
                            m_editorRelations[key] = cr;
                        }
                    }
                }
            } catch (...) { LoadLog() << "  Bad relations.json in project\n"; }
            m_editorClaims.clear();
            try {
                std::string cj = getStr("claims.json");
                if (!cj.empty()) {
                    std::unordered_map<std::string, int> cidByIso;
                    for (auto& [cid, c] : m_editCountries.getAll())
                        if (!c.isoA3.empty()) cidByIso[c.isoA3] = cid;
                    auto j = nlohmann::json::parse(cj);
                    for (auto& [iso, entry] : j.items()) {
                        auto ci = cidByIso.find(iso);
                        if (ci == cidByIso.end()) continue;
                        const nlohmann::json* arr = nullptr;
                        if (entry.is_array()) arr = &entry;
                        else if (entry.is_object() && entry.contains("provinces")) arr = &entry["provinces"];
                        if (!arr) continue;
                        for (auto& v : *arr) m_editorClaims[ci->second].insert(v.get<int>());
                    }
                }
            } catch (...) { LoadLog() << "  Bad claims.json in project\n"; }
            try {
                std::string pc = getStr("political_compass.json");
                if (!pc.empty()) {
                    auto j = nlohmann::json::parse(pc);
                    for (auto& [pidStr, comp] : j.items()) {
                        EditorProvinceData& d = m_provinceData[std::stoi(pidStr)];
                        // Same conversion as the loader above.
                        d.compassEconomic = -comp.value("left", 0.0f);
                        d.compassSocial = -comp.value("auth", 0.0f);
                    }
                }
            } catch (...) { LoadLog() << "  Bad political_compass.json in project\n"; }
        }
        if (pp.data) UnloadImage(pp);
    }

    // Map scripts
    for (auto& [ename, bytes] : entries) {
        if (ename.rfind("scripts/", 0) != 0 || ename.back() == '/') continue;
        m_scripts[ename.substr(8)] = std::string(bytes.begin(), bytes.end());
    }

    // Editor metadata + generator params
    try {
        std::string edStr = getStr("editor.json");
        if (!edStr.empty()) {
            auto ed = nlohmann::json::parse(edStr);
            m_mapName = ed.value("name", m_mapName);
            m_author = ed.value("author", m_author);
            m_license = ed.value("license", m_license);
            m_licenseCustom = ed.value("licenseCustom", false);
            m_licenseText = ed.value("licenseText", std::string());
            m_mapDate = ed.value("mapDate", std::string("January 2000"));
            syncDateFromString();
            m_thumbnailPath = ed.value("thumbnailPath", std::string());
            if (!m_thumbnailPath.empty() && !fs::exists(m_thumbnailPath))
                m_thumbnailPath.clear(); // source image is gone — fall back to auto-generated
            m_thumbnailTexPath.clear();
            m_countryPolicies.clear();
            if (ed.contains("countryPolicies")) {
                for (auto& [cidStr, arr] : ed["countryPolicies"].items()) {
                    int cid = atoi(cidStr.c_str());
                    for (auto& id : arr) m_countryPolicies[cid].push_back(id.get<std::string>());
                }
            }
            m_ethnicityPolicies.clear();
            if (ed.contains("ethnicityPolicies")) {
                for (auto& [name, arr] : ed["ethnicityPolicies"].items())
                    for (auto& id : arr) m_ethnicityPolicies[name].push_back(id.get<std::string>());
            }
            m_ethnicRelations.clear();
            if (ed.contains("ethnicRelations")) {
                for (auto& [iso, byName] : ed["ethnicRelations"].items())
                    for (auto& [name, arr] : byName.items())
                        for (auto& v : arr) m_ethnicRelations[iso][name].push_back(v.get<int>());
            }
            if (ed.contains("gen")) {
                auto& g = ed["gen"];
                m_genParams.seed = g.value("seed", m_genParams.seed);
                m_genParams.landCoverage = g.value("landCoverage", m_genParams.landCoverage);
                m_genParams.numContinents = g.value("numContinents", m_genParams.numContinents);
                m_genParams.numCountries = g.value("numCountries", m_genParams.numCountries);
                m_genParams.jaggedness = g.value("jaggedness", m_genParams.jaggedness);
                m_genParams.provinceDensity = g.value("provinceDensity", m_genParams.provinceDensity);
            }
        }
    } catch (...) { LoadLog() << "  Bad editor.json in project\n"; }

    drawMiniLoadingScreen(0.7f, "Rebuilding map state...");
    rebuildFromPixelState();
    rebuildProvinceCounts();
    sanitizeProvincePixels(); // older projects may carry upscale artifacts
    computePoliticalGradient(); // match the in-game border-glow rendering
    m_polGradientDirty = false; m_polGradientFullDirty = false; // supersedes any pending partial refresh
    m_hlDirty = true;
    m_dirty = false;
    m_projectState = PROJ_EDITING;
    drawMiniLoadingScreen(1.0f, "Ready");
    LoadLog() << "  Loaded project from " << path << "\n";
    return true;
}

// ════════════════════════════════════════════════════════════════
//  Main update / draw
// ════════════════════════════════════════════════════════════════

void MapEditor::update(float dt) {
    // ── Two-step deferred generation ──
    // m_genPending=2: loading overlay was drawn in previous frame, now run generation
    if (m_genPending == 2) {
        // Tens of seconds of blocking work on the main thread, with no frames in
        // between -- exactly the starvation the world loader had. The helper
        // thread keeps the music fed for the duration; in a browser, where
        // there is no thread to have and no loop of ours to pump from, the
        // device is suspended instead so the stall is silent rather than a
        // repeating fragment. See Audio::BlockingCall.
        //
        // An explicit scope rather than letting the guard run to the return
        // below, so the device comes back at the same point the hand-written
        // pair released it. The return is also why this is a guard at all: an
        // early exit added anywhere in the three calls would, with a manual
        // pair, have left the game silent until the tab was reloaded.
        {
            Audio::BlockingCall quiet;
            m_genStatus = "Generating landmass...";
            generateMap(m_genParams);
            m_genStatus = "Generating provinces & countries...";
            generateProvincesCountries();
            m_genStatus = "Generating game data...";
            generateGameData();
        }
        m_genPending = 0;
        return;
    }
    if (m_saveStatusTimer > 0) m_saveStatusTimer -= dt;

    // ── Central ESC dispatcher: innermost context first, exit last ──
    if (IsKeyPressed(KEY_ESCAPE)) {
        bool textEditActive = m_editingSeed || m_editingCountryCount ||
                              m_editingCountryName || m_metaEditField >= 0 || m_licenseTextFocus ||
                              m_editingDateYear || m_provPopEditing;
        if (m_licenseTextFocus) {
            m_licenseTextFocus = false;
        } else if (m_editingDateYear) {
            m_editingDateYear = false;
        } else if (m_dateMonthDropdownOpen) {
            m_dateMonthDropdownOpen = false;
        } else if (m_provPopEditing) {
            m_provPopEditing = false;
        } else if (textEditActive) {
            // The field's own handler cancels the edit this frame — do nothing here.
        } else if (m_pickerOpen) {
            m_pickerOpen = false;        // ESC closes the ethnicity/country picker
        } else if (m_countryEthnicListOpen) {
            m_countryEthnicListOpen = false; // ESC closes the country's ethnic-relations list
        } else if (m_scriptDocsOpen) {
            m_scriptDocsOpen = false;    // ESC closes the docs viewer first
        } else if (m_scriptRenaming) {
            m_scriptRenaming = false;
        } else if (m_scriptEdOpen) {
            saveScriptEditor();          // ESC = save & close the script IDE
            m_scriptEdOpen = false;
        } else if (m_setModeOpen) {
            // For an ethnicity opened from the country list, ESC steps back to
            // that list rather than closing everything at once.
            if (m_setModeIsEthnicity) closeSetModeEthnicity();
            else m_setModeOpen = false;
        } else if (m_exitDialogOpen) {
            m_exitDialogOpen = false; // ESC in the dialog = Cancel
        } else if (m_projectState == PROJ_IMPORT) {
            m_projectState = PROJ_CREATE; // back to the Blank/Procedural/Existing chooser
        } else if (m_projectState == PROJ_OPEN || m_projectState == PROJ_CREATE) {
            m_projectState = PROJ_STARTUP;
        } else if (m_projectState == PROJ_EDITING) {
            requestReturnToMapMenu();
        } else {
            // Already on the map menu (PROJ_STARTUP) — ESC here leaves the editor
            m_wantsExit = true;
        }
    }

    // Ctrl/Cmd+S saves the project (not while typing in a field)
    if (m_projectState == PROJ_EDITING &&
        (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
         IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER)) &&
        IsKeyPressed(KEY_S) &&
        !(m_editingSeed || m_editingCountryCount || m_editingCountryName ||
          m_metaEditField >= 0 || m_licenseTextFocus || m_scriptEdOpen)) {
        saveProject();
    }

    // ── Dropped map files ──
    // Handled here rather than in updateEdit's drop block because a map can be
    // dropped on the project chooser too, which is where you would drop one:
    // you have a file and no project open yet. Only map extensions are taken;
    // anything else is left on the drop list for updateEdit, which reads it
    // as a thumbnail or a country flag depending on the tab.
    if (IsFileDropped()) {
        FilePathList dropped = LoadDroppedFiles();
        for (unsigned int i = 0; i < dropped.count; ++i) {
            std::string ext = extensionOf(dropped.paths[i]);
            if (ext != ".odmap" && ext != ".uodmap") continue;
            std::string path = dropped.paths[i];
            UnloadDroppedFiles(dropped);  // consumed — updateEdit must not see it
            importFromPath(path);
            if (m_warningTimer > 0) m_warningTimer -= dt;
            return;
        }
        // Not ours. Left loaded on purpose for the handler in updateEdit.
    }

    if (m_projectState != PROJ_EDITING) {
        if (m_warningTimer > 0) m_warningTimer -= dt; // dialogs show warnings too
        return;
    }
    updateEdit(dt);
}

void MapEditor::draw() {
    // ── Two-step deferred generation ──
    // m_genPending=1: draw loading overlay, advance to step 2
    if (m_genPending == 1) {
        ClearBackground(Color{15, 15, 20, 255});
        DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 180});
        const char* msg = m_genStatus.c_str();
        int tw = MeasureText(msg, 28);
        DrawText(msg, m_screenW/2 - tw/2, m_screenH/2 - 14, 28, ACCENT);
        m_genPending = 2;
        return;
    }

    if (m_projectState == PROJ_STARTUP) { drawStartupDialog(); return; }
    if (m_projectState == PROJ_OPEN) { drawOpenDialog(); return; }
    if (m_projectState == PROJ_CREATE) { drawCreateDialog(); return; }
    if (m_projectState == PROJ_IMPORT) { drawImportDialog(); return; }
    drawToolbar();
    drawCanvas();
    drawSidePanel();
    drawBottomBar();

    // Warning message (hint when trying to paint in wrong mode)
    if (m_warningTimer > 0) {
        int tw = MeasureText(m_warningMsg.c_str(), 16);
        int wx = m_screenW / 2 - tw / 2, wy = m_toolbarH + 40;
        DrawRectangle(wx - 10, wy - 6, tw + 20, 30, Color{255, 215, 0, 200});
        DrawText(m_warningMsg.c_str(), wx, wy, 16, Color{20, 20, 25, 255});
    }

    // Full-screen overlays draw on top of everything
    drawCountryEthnicListOverlay();
    drawSetModeOverlay();
    drawScriptEditorOverlay();
    drawPickerOverlay();
    drawExitDialog();

    // Save feedback toast
    if (m_saveStatusTimer > 0 && !m_saveStatus.empty()) {
        int tw = MeasureText(m_saveStatus.c_str(), 15);
        int wx = m_screenW / 2 - tw / 2, wy = m_screenH - m_bottomH - 40;
        DrawRectangle(wx - 12, wy - 6, tw + 24, 28, Color{30, 60, 30, 230});
        DrawText(m_saveStatus.c_str(), wx, wy, 15, Color{150, 230, 150, 255});
    }
}

// ════════════════════════════════════════════════════════════════
//  Generic searchable picker (ethnicity for province minorities,
//  country for troop allegiance) — a small modal with a search box
//  and a filtered, scrollable list.
// ════════════════════════════════════════════════════════════════

void MapEditor::openEthnicityPicker(int provId, int slotIdx) {
    m_pickerOpen = true;
    m_pickerMode = 0;
    m_pickerProvId = provId;
    m_pickerSlot = slotIdx;
    m_pickerQuery.clear();
    m_pickerScroll = 0;
    m_pickerEthnicityPool = ethnicityNamePool();
}

void MapEditor::openCountryPicker(int provId, int troopIdx) {
    m_pickerOpen = true;
    m_pickerMode = 1;
    m_pickerProvId = provId;
    m_pickerSlot = troopIdx;
    m_pickerQuery.clear();
    m_pickerScroll = 0;
}

void MapEditor::openMergePicker(int provId, std::vector<std::pair<int, int>> candidates) {
    m_pickerOpen = true;
    m_pickerMode = 2;
    m_pickerProvId = provId;
    m_pickerSlot = -1;
    m_pickerQuery.clear();
    m_pickerScroll = 0;
    m_pickerMergeCandidates = std::move(candidates);
}

void MapEditor::drawPickerOverlay() {
    if (!m_pickerOpen) return;
    Vector2 mouse = GetMousePosition();
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});

    const int w = 420, h = 460;
    int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
    DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.04f, 6, Color{24, 24, 32, 255});
    DrawRectangleRoundedLines({(float)x, (float)y, (float)w, (float)h}, 0.04f, 6, ACCENT);

    const char* title = m_pickerMode == 0 ? "Select Ethnicity"
                      : m_pickerMode == 1 ? "Select Allegiance"
                                          : "Merge Into Which Province?";
    DrawText(title, x + 16, y + 14, 18, ACCENT);
    Rectangle closeBtn = {(float)(x + w - 34), (float)(y + 10), 24, 24};
    bool closeHov = CheckCollisionPointRec(mouse, closeBtn);
    DrawText("X", (int)closeBtn.x + 6, (int)closeBtn.y + 3, 16, closeHov ? ACCENT : LIGHTGRAY);
    if (closeHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) { m_pickerOpen = false; return; }

    // Search field
    Rectangle searchRect = {(float)(x + 16), (float)(y + 44), (float)(w - 32), 26};
    DrawRectangleRounded(searchRect, 0.1f, 4, Color{35, 35, 48, 255});
    DrawRectangleRoundedLines(searchRect, 0.1f, 4, ACCENT);
    DrawText(m_pickerQuery.c_str(), (int)searchRect.x + 8, (int)searchRect.y + 5, 14, WHITE);
    DrawText("|", (int)(searchRect.x + 8 + MeasureText(m_pickerQuery.c_str(), 14)), (int)searchRect.y + 5, 14, ACCENT);
    if (m_pickerQuery.empty())
        DrawText(T("Type to search..."), (int)searchRect.x + 8, (int)searchRect.y + 5, 14, GRAY);

    int key = GetCharPressed();
    while (key > 0) {
        // Every character the field takes. Jittered, because a
        // typed word is a run of distinct taps, not one tap looped.
        Audio::get().playSfx("key_type", 0.12f);
        if (key >= 32 && key < 127 && m_pickerQuery.size() < 40) m_pickerQuery.push_back((char)key);
        key = GetCharPressed();
    }
    odTextEditKeys(m_pickerQuery, 40);

    // Build the filtered option list: (display label, payload)
    // payload = ethnicity name (mode 0) or country id as string (mode 1, via parallel vector)
    std::vector<std::string> labels;
    std::vector<int> cidForRow; // country id (mode 1) or target province id (mode 2)
    std::string qLower = m_pickerQuery;
    for (auto& c : qLower) c = (char)tolower(c);
    auto containsCI = [&](const std::string& hay) {
        std::string h = hay;
        for (auto& c : h) c = (char)tolower(c);
        return h.find(qLower) != std::string::npos;
    };

    bool exactMatch = false;
    if (m_pickerMode == 0) {
        for (auto& name : m_pickerEthnicityPool) {
            if (qLower.empty() || containsCI(name)) labels.push_back(name);
            if (!qLower.empty()) { std::string ln = name; for (auto& c : ln) c = (char)tolower(c); if (ln == qLower) exactMatch = true; }
        }
    } else if (m_pickerMode == 1) {
        auto& all = m_editCountries.getAll();
        std::vector<int> cids;
        for (auto& [cid, c] : all) if (cid < 65533) cids.push_back(cid);
        std::sort(cids.begin(), cids.end());
        for (int cid : cids) {
            const Country& c = all[cid];
            if (qLower.empty() || containsCI(c.name)) { labels.push_back(c.name); cidForRow.push_back(cid); }
        }
    } else {
        // Already ordered by shared border, so the first row is the neighbour
        // the old automatic merge would have picked. The border length is
        // shown because it is the reason the order is what it is.
        for (const auto& [np, shared] : m_pickerMergeCandidates) {
            const Province* p = m_editProvinces.getProvinceById(np);
            std::string name = p ? p->name : std::string("(unnamed)");
            std::string owner;
            if (p) {
                const Country* oc = m_editCountries.getCountry(p->countryId);
                owner = oc ? oc->name : std::string("no owner");
            }
            std::string label = "#" + std::to_string(np) + "  " + name;
            if (!owner.empty()) label += "  -- " + owner;
            label += "   (" + std::to_string(shared) + " px border)";
            if (qLower.empty() || containsCI(label)) { labels.push_back(label); cidForRow.push_back(np); }
        }
    }

    const int rowH = 26;
    int listY = y + 78;
    int listH = h - 78 - (m_pickerMode == 0 ? 40 : 12); // room for "create new" row in ethnicity mode
    int visRows = listH / rowH;
    Rectangle listRect = {(float)(x + 16), (float)listY, (float)(w - 32), (float)listH};
    if (CheckCollisionPointRec(mouse, listRect)) m_pickerScroll -= (int)GetMouseWheelMove();
    m_pickerScroll = std::max(0, std::min(m_pickerScroll, std::max(0, (int)labels.size() - visRows)));

    DrawRectangle((int)listRect.x, (int)listRect.y, (int)listRect.width, (int)listRect.height, Color{18, 18, 25, 255});
    if (labels.empty()) {
        DrawText(T("No matches"), (int)listRect.x + 10, (int)listRect.y + 10, 13, GRAY);
    }
    for (int i = m_pickerScroll; i < (int)labels.size() && i < m_pickerScroll + visRows; ++i) {
        int ry = listY + (i - m_pickerScroll) * rowH;
        Rectangle row = {listRect.x, (float)ry, listRect.width, (float)(rowH - 1)};
        bool hov = CheckCollisionPointRec(mouse, row);
        if (hov) DrawRectangle((int)row.x, (int)row.y, (int)row.width, (int)row.height, Color{255,255,255,12});
        DrawText(labels[i].c_str(), (int)row.x + 8, ry + 5, 13, hov ? ACCENT : WHITE);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (m_pickerMode == 2) {
                // mergeProvinceInto commits and tracks the change itself.
                const int target = cidForRow[i];
                m_pickerOpen = false;
                m_pickerMergeCandidates.clear();
                mergeProvinceInto(m_pickerProvId, target);
                return;
            }
            if (m_pickerMode == 0) {
                EditorProvinceData& d = m_provinceData[m_pickerProvId];
                if (m_pickerSlot < 0) d.ethnicGroups.push_back({labels[i], 10.0f});
                else if (m_pickerSlot < (int)d.ethnicGroups.size()) d.ethnicGroups[m_pickerSlot].first = labels[i];
            } else {
                EditorProvinceData& d = m_provinceData[m_pickerProvId];
                if (m_pickerSlot >= 0 && m_pickerSlot < (int)d.troops.size())
                    d.troops[m_pickerSlot].countryId = cidForRow[i];
            }
            trackChange();
            m_pickerOpen = false;
            return;
        }
    }

    // Ethnicity mode: offer to create a brand-new name from the typed query
    if (m_pickerMode == 0 && !m_pickerQuery.empty() && !exactMatch) {
        Rectangle createRow = {(float)(x + 16), (float)(listY + listH + 8), (float)(w - 32), 28};
        bool hov = CheckCollisionPointRec(mouse, createRow);
        DrawRectangleRounded(createRow, 0.1f, 4, hov ? ColorAlpha(ACCENT, 0.2f) : Color{30,30,42,255});
        DrawRectangleRoundedLines(createRow, 0.1f, 4, ACCENT);
        std::string label = "+ Create \"" + m_pickerQuery + "\"";
        DrawText(label.c_str(), (int)createRow.x + 8, (int)createRow.y + 6, 13, hov ? ACCENT : WHITE);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            std::string newName = m_pickerQuery;
            if (!m_ethnicColors.count(newName)) {
                size_t hh = std::hash<std::string>{}(newName);
                m_ethnicColors[newName] = {(uint8_t)(80 + (hh % 150)), (uint8_t)(80 + ((hh >> 8) % 150)),
                                           (uint8_t)(80 + ((hh >> 16) % 150)), 255};
            }
            EditorProvinceData& d = m_provinceData[m_pickerProvId];
            if (m_pickerSlot < 0) d.ethnicGroups.push_back({newName, 10.0f});
            else if (m_pickerSlot < (int)d.ethnicGroups.size()) d.ethnicGroups[m_pickerSlot].first = newName;
            trackChange();
            m_pickerOpen = false;
        }
    }
}

void MapEditor::drawExitDialog() {
    if (!m_exitDialogOpen) return;
    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 170});
    const int w = 380, h = 190;
    int x = m_screenW / 2 - w / 2, y = m_screenH / 2 - h / 2;
    Rectangle box = {(float)x, (float)y, (float)w, (float)h};
    DrawRectangleRounded(box, 0.05f, 6, Color{25, 25, 32, 255});
    DrawRectangleRoundedLines(box, 0.05f, 6, ACCENT);

    DrawText(T("Unsaved changes"), x + 20, y + 16, 20, WHITE);
    DrawText(T("Save your project before returning"), x + 20, y + 44, 13, LIGHTGRAY);
    DrawText(T("to the map menu?"), x + 20, y + 60, 13, LIGHTGRAY);

    Rectangle saveBtn    = {(float)(x + 20), (float)(y + 92), (float)(w - 40), 28};
    Rectangle discardBtn = {(float)(x + 20), (float)(y + 126), (float)((w - 48) / 2), 26};
    Rectangle cancelBtn  = {(float)(x + 28 + (w - 48) / 2), (float)(y + 126), (float)((w - 48) / 2), 26};
    // Both actions return to the editor's own map menu (PROJ_STARTUP), not
    // all the way out to the game's main menu — "Leave Editor" on that
    // screen is the explicit, separate action for exiting entirely.
    if (drawButton("Save Project", saveBtn, false, 14)) {
        if (saveProject()) {
            m_exitDialogOpen = false;
            m_projectState = PROJ_STARTUP;
        }
    }
    if (drawButton("Discard", discardBtn, false, 12)) {
        m_exitDialogOpen = false;
        m_projectState = PROJ_STARTUP;
    }
    if (drawButton("Cancel", cancelBtn, false, 12)) {
        m_exitDialogOpen = false;
    }
    DrawText(T("ESC = cancel"), x + 20, y + 160, 10, GRAY);
}

void MapEditor::screenToCanvas(int sx, int sy, int& cx, int& cy) const {
    if (!m_renderer) { cx = 0; cy = 0; return; }
    m_renderer->screenToPixel(sx - m_canvasX, sy - m_canvasY, cx, cy);
}

void MapEditor::canvasToScreen(int cx, int cy, float& sx, float& sy) const {
    if (!m_renderer) { sx = 0; sy = 0; return; }
    m_renderer->pixelToScreen((float)cx, (float)cy, sx, sy);
    sx += m_canvasX;
    sy += m_canvasY;
}

void MapEditor::updateEdit(float dt) {
    if (!m_renderer) return;
    // Full-screen overlays swallow all edit input; they handle their own
    // interaction immediate-mode in drawSetModeOverlay().
    if (anyModalOpen()) {
        m_renderer->setPaused(true);
        if (m_warningTimer > 0) m_warningTimer -= dt;
        return;
    }
    Vector2 mouse = GetMousePosition();
    bool inCanvas = mouse.x >= m_canvasX && mouse.x < m_canvasX + m_canvasW &&
                    mouse.y >= m_canvasY && mouse.y < m_canvasY + m_canvasH;

    // Pan mode (true): left click pans (blockLeftPan=false)
    // Draw mode (false): left click draws (blockLeftPan=true)
    m_renderer->setBlockLeftPan(!m_isPanMode);
    // Right-click always pans (default in MapRenderer)
    // Middle-click always pans (default in MapRenderer)

    if (inCanvas) {
        m_renderer->setPaused(false);
        m_renderer->update(dt);
    } else {
        m_renderer->setPaused(true);
    }

    updateToolbar();
    updateBottomBar();
    updateSidePanel(); // always allow panel interaction

    // ── Landmass tools: Brush/Erase drag, Rect drag, Fill click ──
    bool lmb = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
    if (!m_isPanMode && inCanvas && m_mode == MODE_LANDMASS) {
        if ((m_tool == TOOL_BRUSH || m_tool == TOOL_ERASE) && lmb) {
            int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
            applyBrush(cx, cy, m_tool == TOOL_ERASE ? false : m_drawAsLand);
            // Provinces exist: brush already gives live feedback (sea clears
            // provinces, new land shows grey); reshape fully on stroke end
            if (m_hasProvinces) m_landStrokeActive = true;
        } else if (m_tool == TOOL_RECT && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
            m_rectAnchorX = cx;
            m_rectAnchorY = std::max(0, std::min(MAP_H - 1, cy));
            m_rectLand = m_drawAsLand;
            m_rectDragActive = true;
        } else if (m_tool == TOOL_FILL && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
            applyFloodFill(cx, cy, m_drawAsLand);
            if (m_hasProvinces && strokeBBoxValid()) {
                finalizeLandStroke();
                resetStrokeBBox();
            }
        }
    } else if (!m_isPanMode && lmb && inCanvas && m_tool == TOOL_BRUSH
               && m_mode != MODE_PROVINCES && m_mode != MODE_COUNTRIES
               && m_mode != MODE_NAVY && m_mode != MODE_RELATIONS && m_warningTimer <= 0) {
        m_warningMsg = "Switch to Landmass mode to paint";
        m_warningTimer = 2.5f;
    }
    // Land stroke finished while provinces exist: orphan land becomes new
    // unclaimed provinces (islands) or joins the touching province; cleanup.
    if (m_landStrokeActive && !lmb) {
        m_landStrokeActive = false;
        finalizeLandStroke();
        resetStrokeBBox();
    }
    // Rect-tool release: apply the dragged rectangle
    if (m_rectDragActive && !lmb) {
        m_rectDragActive = false;
        int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
        cy = std::max(0, std::min(MAP_H - 1, cy));
        if (m_mode == MODE_LANDMASS) {
            applyRect(m_rectAnchorX, m_rectAnchorY, cx, cy, m_rectLand);
            if (m_hasProvinces && strokeBBoxValid()) {
                finalizeLandStroke();
                resetStrokeBBox();
            }
        } else if (m_mode == MODE_PROVINCES && m_provTool == 1 && m_selectedProvince >= 0) {
            applyProvinceRect(m_rectAnchorX, m_rectAnchorY, cx, cy);
        }
    }

    // ── Relations tab: click two countries on the map to pick the pair ──
    if (inCanvas && m_mode == MODE_RELATIONS && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !m_isPanMode) {
        int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
        if (cx >= 0 && cx < MAP_W && cy >= 0 && cy < MAP_H && m_editProvinces.getWidth() > 0) {
            const Province* p = m_editProvinces.getProvince(cx, cy);
            if (p && p->countryId < 65533) { // skip UNC/BLC/SPC — no relations for those
                if (m_relCountryA < 0 || (m_relCountryB >= 0)) {
                    // Start a fresh pair
                    m_relCountryA = p->countryId;
                    m_relCountryB = -1;
                } else if (p->countryId != m_relCountryA) {
                    m_relCountryB = p->countryId;
                }
            }
        }
    }

    // ── Country / Province selection click + province shape tools ──
    // Provinces tab: Select picks a province; Paint uses the bottom-bar tool —
    // Brush drags, Rect drags a box, Fill grabs a whole contiguous chunk.
    if (inCanvas && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !m_isPanMode) {
        if (m_mode == MODE_COUNTRIES || m_mode == MODE_PROVINCES) {
            bool provPainting = (m_mode == MODE_PROVINCES && m_provTool == 1
                                 && m_hasProvinces && m_selectedProvince >= 0);
            bool countryPainting = (m_mode == MODE_COUNTRIES && m_countryBrushActive
                                    && m_hasProvinces && m_selectedCountry >= 0);
            bool claimsPainting = (m_mode == MODE_COUNTRIES && m_claimsBrushActive
                                   && m_hasProvinces && m_selectedCountry >= 0);
            if (provPainting) {
                int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
                if (m_tool == TOOL_RECT) {
                    m_rectAnchorX = cx;
                    m_rectAnchorY = std::max(0, std::min(MAP_H - 1, cy));
                    m_rectDragActive = true;
                } else if (m_tool == TOOL_FILL) {
                    applyProvinceFill(cx, cy);
                } else {
                    m_provStrokeActive = true; // brush/erase: freehand painting
                }
            } else if (claimsPainting) {
                m_claimsStrokeActive = true;
                m_claimsBrushTouched.clear();
                int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
                applyClaimsBrush(cx, cy);
            } else if (countryPainting) {
                m_countryBrushStrokeActive = true;
                m_countryBrushTouched.clear();
                int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
                applyCountryOwnershipBrush(cx, cy);
            } else {
                int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
                if (cx >= 0 && cx < MAP_W && cy >= 0 && cy < MAP_H && m_editProvinces.getWidth() > 0) {
                    const Province* p = m_editProvinces.getProvince(cx, cy);
                    if (p) {
                        m_selectedProvince = p->id;
                        if (m_mode == MODE_COUNTRIES) m_selectedCountry = p->countryId;
                    }
                }
            }
        }
    }
    if (m_provStrokeActive) {
        if (lmb && inCanvas) {
            int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
            if (cx >= 0 && cx < MAP_W && cy >= 0 && cy < MAP_H)
                applyProvinceBrush(cx, cy);
        }
        if (!lmb) {
            // Stroke finished: textures are already live-patched, so only sync
            // the CPU province image for the dirty rect and drop any province
            // painted out of existence. No full-map work = no release hitch.
            m_provStrokeActive = false;
            if (strokeBBoxValid()) {
                if (m_strokeWrapped) {
                    commitProvincePixels(); // seam stroke: full refresh (rare)
                } else {
                    m_editProvinces.updatePixelsRect(m_provincePixels.data(),
                        m_strokeMinX, m_strokeMinY,
                        m_strokeMaxX - m_strokeMinX + 1, m_strokeMaxY - m_strokeMinY + 1);
                    trackChange();
                }
                garbageCollectProvinces();
                markPoliticalDirty(m_strokeMinX, m_strokeMinY, m_strokeMaxX, m_strokeMaxY);
                resetStrokeBBox();
                m_hlDirty = true;
            }
        }
    }
    if (m_countryBrushStrokeActive) {
        if (lmb && inCanvas) {
            int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
            applyCountryOwnershipBrush(cx, cy);
        }
        if (!lmb) {
            m_countryBrushStrokeActive = false;
            m_countryBrushTouched.clear();
        }
    }
    if (m_claimsStrokeActive) {
        if (lmb && inCanvas) {
            int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
            applyClaimsBrush(cx, cy);
        }
        if (!lmb) {
            m_claimsStrokeActive = false;
            m_claimsBrushTouched.clear();
        }
    }
    // Claims overlay follows the claims brush: visible only while it's armed,
    // and rebuilt whenever the selected country changes (it only ever shows
    // one country's claims at a time).
    {
        bool wantClaims = (m_mode == MODE_COUNTRIES && m_claimsBrushActive && m_hasProvinces);
        if (wantClaims && m_claimsOverlayCid != m_selectedCountry) rebuildClaimsOverlay();
        if (m_renderer && m_renderer->getShowClaims() != wantClaims)
            m_renderer->setShowClaims(wantClaims);
    }

    // ── Navy: place / select / delete ships on the canvas ──
    if (m_mode == MODE_NAVY && m_hasProvinces) {
        if (inCanvas && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !m_isPanMode) {
            // Hit-test existing ships first (screen-space distance)
            int hit = -1;
            for (int i = 0; i < (int)m_editorShips.size(); ++i) {
                const NavyShip& s = m_editorShips[i];
                int spx = (int)((s.lon + 180.0) / 360.0 * MAP_W);
                int spy = (int)((90.0 - s.lat) / 180.0 * MAP_H);
                float sx, sy;
                canvasToScreen(spx, spy, sx, sy);
                float ddx = mouse.x - sx, ddy = mouse.y - sy;
                if (ddx * ddx + ddy * ddy < 12.0f * 12.0f) { hit = i; break; }
            }
            if (hit >= 0) {
                m_selectedShip = hit;
                m_navyDraggingShip = true;
            } else {
                int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
                if (cx >= 0 && cx < MAP_W && cy >= 0 && cy < MAP_H && !m_editLandSea.isLand(cx, cy)) {
                    if (m_navyCountry < 0) {
                        m_warningMsg = "Pick a country in the Navy panel first";
                        m_warningTimer = 2.0f;
                    } else {
                        NavyShip s;
                        s.countryId = m_navyCountry;
                        s.type = m_navyType;
                        s.lon = (double)cx / MAP_W * 360.0 - 180.0;
                        s.lat = 90.0 - (double)cy / MAP_H * 180.0;
                        s.health = (int)m_navyDefaultHealth;
                        s.crew = m_navyType == "boat" ? (int)m_navyDefaultTroops : 0;
                        m_editorShips.push_back(s);
                        m_selectedShip = (int)m_editorShips.size() - 1;
                        trackChange();
                    }
                }
            }
        }
        if ((IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) &&
            m_selectedShip >= 0 && m_selectedShip < (int)m_editorShips.size()) {
            m_editorShips.erase(m_editorShips.begin() + m_selectedShip);
            m_selectedShip = -1;
            trackChange();
        }
        // Drag the selected ship to reposition it (must stay on sea)
        if (m_navyDraggingShip) {
            if (lmb && inCanvas && m_selectedShip >= 0 && m_selectedShip < (int)m_editorShips.size()) {
                int cx, cy; screenToCanvas((int)mouse.x, (int)mouse.y, cx, cy);
                if (cx >= 0 && cx < MAP_W && cy >= 0 && cy < MAP_H && !m_editLandSea.isLand(cx, cy)) {
                    NavyShip& s = m_editorShips[m_selectedShip];
                    s.lon = (double)cx / MAP_W * 360.0 - 180.0;
                    s.lat = 90.0 - (double)cy / MAP_H * 180.0;
                }
            }
            if (!lmb) { m_navyDraggingShip = false; trackChange(); }
        }
    }

    // ── Drag-and-drop SVG flag import ──
    // Refresh the pulsing selection highlight when mode/selection changes;
    // the live-recolor pixel cache goes stale at the same moments
    if ((int)m_mode != m_hlMode || m_selectedProvince != m_hlProv ||
        m_selectedCountry != m_hlCountry || m_relCountryA != m_hlRelA ||
        m_relCountryB != m_hlRelB || m_hlDirty) {
        m_hlMode = (int)m_mode;
        m_hlProv = m_selectedProvince;
        m_hlCountry = m_selectedCountry;
        m_hlRelA = m_relCountryA;
        m_hlRelB = m_relCountryB;
        m_hlDirty = false;
        m_recolorCid = -1;
        updateSelectionHighlight();
    }

    if (IsFileDropped()) {
        FilePathList dropped = LoadDroppedFiles();
        // In the Metadata tab a dropped image sets the map thumbnail; every
        // other tab keeps the existing "drop an SVG onto a country" behaviour.
        if (dropped.count > 0 && m_mode == MODE_METADATA) {
            std::string path = dropped.paths[0];
            std::string lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            bool isImage = lower.size() > 4 &&
                           (lower.substr(lower.size()-4) == ".png" || lower.substr(lower.size()-4) == ".jpg" ||
                            lower.substr(lower.size()-4) == ".bmp" || lower.substr(lower.size()-4) == ".tga" ||
                            (lower.size() > 5 && lower.substr(lower.size()-5) == ".jpeg"));
            if (!isImage) {
                m_warningMsg = "Thumbnail must be a PNG (or JPG/BMP/TGA)";
                m_warningTimer = 2.5f;
            } else if (setThumbnailFromFile(path)) {
                m_warningMsg = "Thumbnail set";
                m_warningTimer = 1.5f;
            } else {
                m_warningMsg = "Could not read that image";
                m_warningTimer = 2.5f;
            }
        } else if (dropped.count > 0 && m_selectedCountry >= 65533) {
            // UNC/BLC/SPC territories keep their fixed flags
            m_warningMsg = "This territory cannot have a custom flag";
            m_warningTimer = 2.5f;
        } else if (dropped.count > 0 && m_selectedCountry >= 0) {
            std::string path = dropped.paths[0];
            std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : "";
            if (ext == ".svg" || ext == ".SVG") {
                auto& all = m_editCountries.getAll();
                auto it = all.find(m_selectedCountry);
                if (it != all.end()) {
                    // Copy SVG to project flags dir. Unique name per drop —
                    // the SVG rasterizer caches by path, so reusing a name
                    // would keep showing the previously dropped flag.
                    fs::create_directories(m_dataDir + "projects/flags/");
                    std::string dest = m_dataDir + "projects/flags/c" + std::to_string(m_selectedCountry)
                                     + "_" + std::to_string(rand() % 1000000) + ".svg";
                    std::ifstream src(path, std::ios::binary);
                    if (src) {
                        std::ofstream dst(dest, std::ios::binary);
                        dst << src.rdbuf();
                        dst.close();
                        it->second.flagActual.imagePath = dest;
                        m_flagPreviewCountry = -1;
                        m_dirty = true;
                    }
                }
            }
        }
        UnloadDroppedFiles(dropped);
    }

    if (m_warningTimer > 0) m_warningTimer -= dt;

    // Refresh the border-glow gradient as soon as the mouse releases from
    // whatever ownership/shape/color edit marked it dirty — keeps it live
    // without recomputing the whole map every single dragging frame.
    if (m_polGradientDirty && !IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        if (m_polGradientFullDirty) computePoliticalGradient();
        else computePoliticalGradientRegion(m_polDirtyX0, m_polDirtyY0, m_polDirtyX1, m_polDirtyY1);
        m_polGradientDirty = false;
        m_polGradientFullDirty = false;
    }
}

// ════════════════════════════════════════════════════════════════
//  Toolbar
// ════════════════════════════════════════════════════════════════

void MapEditor::requestReturnToMapMenu() {
    if (m_dirty) m_exitDialogOpen = true;  // confirm before returning to the map menu
    else m_projectState = PROJ_STARTUP;    // no unsaved changes: go straight there
}

void MapEditor::updateToolbar() {
    Vector2 mouse = GetMousePosition();
    Rectangle menuBtn = {(float)(m_screenW - 220), (float)((m_toolbarH-30)/2), 90, 30};
    if (CheckCollisionPointRec(mouse, menuBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        requestReturnToMapMenu();
        return;
    }
    // Settings, so a mapmaker can reach volume, resolution and keybinds without
    // leaving a session. Game owns that screen and draws it over the editor.
    Rectangle setBtn = {(float)(m_screenW - 318), (float)((m_toolbarH-30)/2), 90, 30};
    if (CheckCollisionPointRec(mouse, setBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_wantsSettings = true;
        return;
    }
    if (mouse.y < 0 || mouse.y > m_toolbarH) return;
    const char* names[] = {"Landmass","Provinces","Countries","Navy","Relations","Scripts","Generator","Metadata"};
    int btnW = 95, btnH = 34, gap = 4, startX = 8;
    for (int i = 0; i < 8; i++) {
        Rectangle r = {(float)(startX + i*(btnW+gap)), (float)((m_toolbarH-btnH)/2), (float)btnW, (float)btnH};
        if (CheckCollisionPointRec(mouse, r) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_mode = (EditMode)i;
        }
    }
}

void MapEditor::drawToolbar() {
    DrawRectangle(0, 0, m_screenW, m_toolbarH, Color{25, 25, 30, 255});
    const char* names[] = {"Landmass","Provinces","Countries","Navy","Relations","Scripts","Generator","Metadata"};
    int btnW = 95, btnH = 34, gap = 4, startX = 8;
    for (int i = 0; i < 8; i++) {
        Rectangle r = {(float)(startX + i*(btnW+gap)), (float)((m_toolbarH-btnH)/2), (float)btnW, (float)btnH};
        bool sel = (m_mode == i);
        Color accent = sel ? ACCENT : Color{255,255,255,40};
        Color bg = sel ? ColorAlpha(ACCENT, 0.12f) : BLANK;
        bool hov = CheckCollisionPointRec(GetMousePosition(), r);
        if (hov && !sel) bg = Color{255,255,255,10};
        DrawRectangleRounded(r, 0.1f, 6, bg);
        DrawRectangleRoundedLines(r, 0.1f, 6, accent);
        int tw = MeasureText(names[i], 13);
        DrawText(names[i], (int)(r.x + r.width/2 - tw/2), (int)(r.y + r.height/2 - 7), 13, sel ? ACCENT : (hov ? WHITE : LIGHTGRAY));
    }
    // "Menu" button — returns to the editor's project chooser (warns if dirty)
    Rectangle menuBtn = {(float)(m_screenW - 220), (float)((m_toolbarH-30)/2), 90, 30};
    bool menuHov = CheckCollisionPointRec(GetMousePosition(), menuBtn);
    DrawRectangleRounded(menuBtn, 0.15f, 6, menuHov ? Color{60,50,30,220} : Color{45,40,30,180});
    DrawRectangleRoundedLines(menuBtn, 0.15f, 6, menuHov ? Color{230,180,80,255} : Color{160,140,90,200});
    int mtw = MeasureText(T("Menu"), 13);
    DrawText(T("Menu"), (int)(menuBtn.x + menuBtn.width/2 - mtw/2), (int)(menuBtn.y + 8), 13, menuHov ? Color{230,180,80,255} : LIGHTGRAY);
    if (m_dirty) DrawCircle((int)(menuBtn.x + menuBtn.width - 6), (int)menuBtn.y + 4, 3, Color{230, 140, 60, 255});

    // Settings — same shape as Menu but neutral, so it does not compete with
    // the one button that leaves the project.
    Rectangle setBtn = {(float)(m_screenW - 318), (float)((m_toolbarH-30)/2), 90, 30};
    bool setHov = CheckCollisionPointRec(GetMousePosition(), setBtn);
    DrawRectangleRounded(setBtn, 0.15f, 6, setHov ? Color{45,45,55,220} : Color{35,35,42,180});
    DrawRectangleRoundedLines(setBtn, 0.15f, 6, setHov ? Color{200,200,215,255} : Color{110,110,125,200});
    int stw = MeasureText(T("Settings"), 13);
    DrawText(T("Settings"), (int)(setBtn.x + setBtn.width/2 - stw/2), (int)(setBtn.y + 8), 13,
             setHov ? WHITE : LIGHTGRAY);

    DrawText(TextFormat(T("Zoom: %.2fx"), m_renderer ? m_renderer->getZoom() : 1.0f), m_screenW - 120, 16, 12, WHITE);
}

// ════════════════════════════════════════════════════════════════
//  Canvas (uses MapRenderer)
// ════════════════════════════════════════════════════════════════

void MapEditor::drawCanvas() {
    DrawRectangle(m_canvasX, m_canvasY, m_canvasW, m_canvasH, Color{10, 10, 15, 255});
    if (m_renderer && m_editLandSea.getWidth() > 0) {
        m_renderer->drawSubregion(m_canvasX, m_canvasY, m_canvasW, m_canvasH,
            m_renderer->getCameraTarget().x, m_renderer->getCameraTarget().y,
            m_renderer->getZoom(), m_editLandSea, m_editProvinces, m_editCountries);
    }
    DrawRectangleLines(m_canvasX, m_canvasY, m_canvasW, m_canvasH, Color{60, 60, 70, 255});

    // Brush preview
    Vector2 mouse = GetMousePosition();
    bool provPaint = m_mode == MODE_PROVINCES && m_provTool == 1
                     && m_selectedProvince >= 0 && m_hasProvinces;
    bool circleTool = m_tool == TOOL_BRUSH || m_tool == TOOL_ERASE;
    if (((m_mode == MODE_LANDMASS && circleTool) || (provPaint && circleTool)) && !m_rectDragActive) {
        if (mouse.x >= m_canvasX && mouse.x < m_canvasX + m_canvasW && mouse.y >= m_canvasY && mouse.y < m_canvasY + m_canvasH) {
            // approximate brush size on screen
            float z = m_renderer ? m_renderer->getZoom() : 1.0f;
            int r = (int)(m_brushSize * z);
            if (r < 2) r = 2;
            Color pc = provPaint ? Color{255, 210, 90, 220}
                                 : ((m_tool != TOOL_ERASE && m_drawAsLand) ? Color{130,255,130,200} : Color{130,130,255,200});
            DrawCircleLines((int)mouse.x, (int)mouse.y, (float)r, pc);
        }
    }

    // Rect-tool drag preview
    if (m_rectDragActive) {
        float ax, ay;
        canvasToScreen(m_rectAnchorX, m_rectAnchorY, ax, ay);
        float x0 = std::min(ax, mouse.x), y0 = std::min(ay, mouse.y);
        float w = fabsf(mouse.x - ax), h = fabsf(mouse.y - ay);
        Color rc = (m_mode == MODE_PROVINCES) ? Color{255, 210, 90, 220}
                 : (m_rectLand ? Color{130,255,130,220} : Color{130,130,255,220});
        DrawRectangleLinesEx({x0, y0, w, h}, 2, rc);
    }

    drawBuildingBadges();

    // ── Claims legend: while the claims brush is armed, say plainly whose
    //    claims the red wash represents and how many there are, so an empty
    //    map reads as "no claims yet" rather than "overlay is broken". ──
    if (m_mode == MODE_COUNTRIES && m_claimsBrushActive && m_hasProvinces) {
        const Country* cc = m_editCountries.getCountry(m_selectedCountry);
        int n = 0;
        auto ci = m_editorClaims.find(m_selectedCountry);
        if (ci != m_editorClaims.end()) n = (int)ci->second.size();
        std::string line = std::string("Claims of ") + (cc ? cc->name : std::string("(no country)"))
                         + ": " + std::to_string(n) + (n == 1 ? " province" : " provinces");
        const char* hint = n == 0 ? "Drag over provinces to claim them"
                                  : (m_claimsBrushErase ? "Unclaim mode — drag to remove"
                                                        : "Claim mode — drag to add");
        int tw = std::max(MeasureText(line.c_str(), 14), MeasureText(hint, 11));
        int bw = tw + 34, bh = 46;
        int bx = m_canvasX + 12, by = m_canvasY + 12;
        DrawRectangleRounded({(float)bx, (float)by, (float)bw, (float)bh}, 0.15f, 6, Color{15, 15, 20, 220});
        DrawRectangleRoundedLines({(float)bx, (float)by, (float)bw, (float)bh}, 0.15f, 6, CLAIM_COL);
        DrawRectangle(bx + 10, by + 11, 12, 12, Color{CLAIM_COL.r, CLAIM_COL.g, CLAIM_COL.b, 255});
        DrawText(line.c_str(), bx + 28, by + 10, 14, WHITE);
        DrawText(hint, bx + 28, by + 28, 11, Color{170, 170, 180, 220});
    }

    // ── Ship markers (Navy mode only) ──
    if (m_mode != MODE_NAVY) return;
    for (int i = 0; i < (int)m_editorShips.size(); ++i) {
        const NavyShip& s = m_editorShips[i];
        int spx = (int)((s.lon + 180.0) / 360.0 * MAP_W);
        int spy = (int)((90.0 - s.lat) / 180.0 * MAP_H);
        float sx, sy;
        canvasToScreen(spx, spy, sx, sy);
        if (sx < m_canvasX - 12 || sx > m_canvasX + m_canvasW + 12 ||
            sy < m_canvasY - 12 || sy > m_canvasY + m_canvasH + 12) continue;
        Color cc = {200, 200, 200, 255};
        if (const Country* oc = m_editCountries.getCountry(s.countryId)) cc = oc->color;
        bool sel = (m_mode == MODE_NAVY && i == m_selectedShip);
        Color outline = sel ? GOLD : WHITE;
        if (s.type == "boat") { // boats = triangles
            Vector2 p1 = {sx, sy - 6}, p2 = {sx - 5, sy + 4}, p3 = {sx + 5, sy + 4};
            DrawTriangle(p1, p2, p3, cc);
            DrawTriangleLines(p1, p2, p3, outline);
        } else if (s.type == "carrier") { // carriers = circles
            DrawCircleV({sx, sy}, 5, cc);
            DrawCircleLinesV({sx, sy}, 5, outline);
        } else { // destroyer = squares
            DrawRectangle((int)sx - 5, (int)sy - 4, 10, 8, cc);
            DrawRectangleLines((int)sx - 5, (int)sy - 4, 10, 8, outline);
        }
    }
}

// ════════════════════════════════════════════════════════════════
//  Bottom Bar
// ════════════════════════════════════════════════════════════════

void MapEditor::updateBottomBar() {
    Vector2 mouse = GetMousePosition();
    int by = m_screenH - m_bottomH;
    if (mouse.y < by) return;
    const char* names[] = {"Brush","Rect","Fill","Erase"};
    int startX = 60, gap = 5, btnW = 70;
    for (int i = 0; i < 4; i++) {
        Rectangle r = {(float)(startX + i*(btnW+gap)), (float)(by + 10), (float)btnW, 30};
        if (CheckCollisionPointRec(mouse, r) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_tool = (Tool)i;
    }
    // Brush size slider. The drag-to-value mapping and the drawn fill must use
    // the same max (BRUSH_MAX) — they used to disagree (40 vs 60), so the fill
    // always lagged behind the cursor and the slider felt offset.
    Rectangle slider = {(float)(startX + 4*75 + gap + 80), (float)(by + 17), 120, 14};
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {slider.x-5, slider.y-5, slider.width+10, slider.height+10})) {
        float t = (GetMouseX() - slider.x) / slider.width;
        m_brushSize = (int)lroundf(std::max(0.0f, std::min(1.0f, t)) * BRUSH_MAX);
        m_brushSize = std::max(1, std::min((int)BRUSH_MAX, m_brushSize));
    }
    // Paint land / sea toggle
    Rectangle landBtn = {(float)(startX + 4*75 + gap + 220), (float)(by+10), 70, 30};
    if (CheckCollisionPointRec(mouse, landBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_drawAsLand = true;
    Rectangle seaBtn = {(float)(startX + 4*75 + gap + 295), (float)(by+10), 70, 30};
    if (CheckCollisionPointRec(mouse, seaBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_drawAsLand = false;
    // Draw / Pan toggle
    Rectangle modeBtn = {(float)(startX + 4*75 + gap + 380), (float)(by+10), 90, 30};
    if (CheckCollisionPointRec(mouse, modeBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_isPanMode = !m_isPanMode;
}

void MapEditor::drawBottomBar() {
    int by = m_screenH - m_bottomH;
    DrawRectangle(0, by, m_screenW, m_bottomH, Color{25, 25, 30, 255});

    const char* names[] = {"Brush","Rect","Fill","Erase"};
    int startX = 60, gap = 5, btnW = 70;
    DrawText(T("Tools"), 10, by + 18, 14, LIGHTGRAY);
    for (int i = 0; i < 4; i++) {
        Rectangle r = {(float)(startX + i*(btnW+gap)), (float)(by + 10), (float)btnW, 30};
        bool sel = (m_tool == i);
        Color accent = sel ? ACCENT : Color{255,255,255,30};
        Color bg = sel ? ColorAlpha(ACCENT, 0.12f) : BLANK;
        bool hov = CheckCollisionPointRec(GetMousePosition(), r);
        if (hov && !sel) bg = Color{255,255,255,10};
        DrawRectangleRounded(r, 0.1f, 6, bg);
        DrawRectangleRoundedLines(r, 0.1f, 6, accent);
        int tw = MeasureText(names[i], 13);
        DrawText(names[i], (int)(r.x + r.width/2 - tw/2), (int)(r.y + 9), 13, sel ? ACCENT : (hov ? WHITE : LIGHTGRAY));
    }

    DrawText(T("Size"), startX + 4*75 + gap + 50, by+18, 13, LIGHTGRAY);
    Rectangle slider = {(float)(startX + 4*75 + gap + 80), (float)(by + 17), 120, 14};
    DrawRectangleRounded(slider, 0.3f, 4, Color{50,50,60,255});
    Rectangle fill = {slider.x, slider.y, slider.width * m_brushSize / BRUSH_MAX, slider.height};
    if (fill.width > 0) DrawRectangleRounded(fill, 0.3f, 4, ACCENT);
    DrawText(TextFormat("%d", m_brushSize), (int)(slider.x + slider.width + 8), by+18, 13, WHITE);

    // Land / Sea toggle
    Rectangle landBtn = {(float)(startX + 4*75 + gap + 220), (float)(by+10), 70, 30};
    Rectangle seaBtn = {(float)(startX + 4*75 + gap + 295), (float)(by+10), 70, 30};
    drawButton("Land", landBtn, m_drawAsLand, 14);
    drawButton("Sea", seaBtn, !m_drawAsLand, 14);
    // Draw / Pan toggle
    Rectangle modeBtn = {(float)(startX + 4*75 + gap + 380), (float)(by+10), 90, 30};
    drawButton(m_isPanMode ? "Pan" : "Draw", modeBtn, false, 14);
}

// ════════════════════════════════════════════════════════════════
//  Side Panel
// ══════════════════════════════════════════════

void MapEditor::updateSidePanel() {
    Vector2 mouse = GetMousePosition();
    int px = m_screenW - m_panelW;
    if (mouse.x < px || mouse.x >= m_screenW) return;

    switch (m_mode) {
        case MODE_LANDMASS: updateLandmassPanel(); break;
        case MODE_GENERATOR: updateGeneratorPanel(); break;
        case MODE_PROVINCES: updateProvincePanel(); break;
        case MODE_COUNTRIES: updateCountryPanel(); break;
        case MODE_RELATIONS: updateRelationsPanel(); break;
        case MODE_NAVY: updateNavyPanel(); break;
        case MODE_SCRIPTS: updateScriptPanel(); break;
        case MODE_METADATA: updateMetadataPanel(); break;
    }
}

void MapEditor::drawSidePanel() {
    int px = m_screenW - m_panelW, py = m_toolbarH, ph = m_screenH - m_toolbarH - m_bottomH;
    DrawRectangle(px, py, m_panelW, ph, Color{30, 30, 35, 255});
    DrawRectangleLines(px, py, m_panelW, ph, Color{60, 60, 70, 255});
    switch (m_mode) {
        case MODE_LANDMASS: drawLandmassPanel(); break;
        case MODE_PROVINCES: drawProvincePanel(); break;
        case MODE_COUNTRIES: drawCountryPanel(); break;
        case MODE_RELATIONS: drawRelationsPanel(); break;
        case MODE_NAVY: drawNavyPanel(); break;
        case MODE_SCRIPTS: drawScriptPanel(); break;
        case MODE_GENERATOR: drawGeneratorPanel(); break;
        case MODE_METADATA: drawMetadataPanel(); break;
    }
}

// ── Landmass Panel ─────────────────────────────────────────────

void MapEditor::updateLandmassPanel() {}
void MapEditor::drawLandmassPanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    DrawText(T("Landmass Editor"), px, y, 18, ACCENT); y += 35;
    DrawText(T("Use brush tools below."), px, y, 13, LIGHTGRAY); y += 20;
    DrawText(T("Left-drag to paint,"), px, y, 13, GRAY); y += 16;
    DrawText(T("right-drag to pan."), px, y, 13, GRAY); y += 16;
    DrawText(T("Scroll to zoom."), px, y, 13, GRAY);
}

// ── Generator Panel (update & draw use identical y layout) ───────

void MapEditor::updateGeneratorPanel() {
    Vector2 mouse = GetMousePosition();
    int px = m_screenW - m_panelW + 12;
    // y = m_toolbarH+56: "Seed" label
    // y = m_toolbarH+67: seed input rect (h=26) at y+3
    //    RND button at x+204, 28x26
    // y = m_toolbarH+110: "Land Coverage:" label
    // y = m_toolbarH+128: coverage slider
    // y = m_toolbarH+163: "Continents:" label
    // y = m_toolbarH+181: continents ± buttons
    // y = m_toolbarH+215: "Jaggedness:" label
    // y = m_toolbarH+233: jaggedness slider
    // y = m_toolbarH+273: countries ± buttons
    // y = m_toolbarH+315: province size slider
    // y = m_toolbarH+360: Generate World button
    // y = m_toolbarH+410: Export .odmap button
    // y = m_toolbarH+448: Export to File... button (desktop only)

    // Seed + RND button
    Rectangle seedRect = {(float)px, (float)(m_toolbarH + 70), 172, 26};
    Rectangle rndBtn = {(float)(px + 178), (float)(m_toolbarH + 70), 34, 26};
    if (!m_editingSeed && CheckCollisionPointRec(mouse, seedRect) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_editingSeed = true;
        m_seedText = TextFormat("%d", m_genParams.seed);
    }
    if (CheckCollisionPointRec(mouse, rndBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_genParams.seed = rand() % 100000;
    }
    if (m_editingSeed) {
        int key = GetCharPressed();
        while (key > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            if (key >= '0' && key <= '9' && m_seedText.size() < 10) m_seedText.push_back((char)key);
            key = GetCharPressed();
        }
        odTextEditKeys(m_seedText, 10, "", true);
        if (IsKeyPressed(KEY_ENTER)) { m_genParams.seed = std::max(0, atoi(m_seedText.c_str())); m_editingSeed = false; }
        if (IsKeyPressed(KEY_ESCAPE)) m_editingSeed = false;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, seedRect)) m_editingSeed = false;
    }

    // Coverage slider
    int y = m_toolbarH + 128;
    Rectangle cov = {(float)px, (float)y, 200, 12};
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {cov.x-4, cov.y-4, cov.width+8, cov.height+8})) {
        m_genParams.landCoverage = (GetMouseX() - cov.x) / cov.width;
        m_genParams.landCoverage = std::max(0.1f, std::min(0.8f, m_genParams.landCoverage));
    }

    // Continents ±
    y = m_toolbarH + 181;
    Rectangle minus = {(float)px, (float)y, 28, 24};
    Rectangle plus = {(float)(px+170), (float)y, 28, 24};
    if (CheckCollisionPointRec(mouse, minus) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_genParams.numContinents = std::max(1, m_genParams.numContinents-1);
    if (CheckCollisionPointRec(mouse, plus) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) m_genParams.numContinents = std::min(20, m_genParams.numContinents+1);

    // Jaggedness slider
    y = m_toolbarH + 233;
    Rectangle jag = {(float)px, (float)y, 200, 12};
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {jag.x-4, jag.y-4, jag.width+8, jag.height+8})) {
        m_genParams.jaggedness = (GetMouseX() - jag.x) / jag.width;
        m_genParams.jaggedness = std::max(0.1f, std::min(1.0f, m_genParams.jaggedness));
    }

    // Countries slider
    y = m_toolbarH + 270;
    Rectangle cntSlider = {(float)px, (float)y, 200, 12};
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {cntSlider.x-4, cntSlider.y-4, cntSlider.width+8, cntSlider.height+8})) {
        float s = (GetMouseX() - cntSlider.x) / cntSlider.width;
        m_genParams.numCountries = (int)(1 + s * 299);
        m_genParams.numCountries = std::max(1, std::min(300, m_genParams.numCountries));
    }
    // Countries text input (click to type exact value)
    Rectangle cntText = {(float)(px + 164), (float)(m_toolbarH + 288), 44, 22};
    if (!m_editingCountryCount && CheckCollisionPointRec(mouse, cntText) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_editingCountryCount = true;
        m_countryCountText = TextFormat("%d", m_genParams.numCountries);
    }
    if (m_editingCountryCount) {
        int key = GetCharPressed();
        while (key > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            if (key >= '0' && key <= '9' && m_countryCountText.size() < 4) m_countryCountText.push_back((char)key);
            key = GetCharPressed();
        }
        odTextEditKeys(m_countryCountText, 4, "", true);
        if (IsKeyPressed(KEY_ENTER)) { m_genParams.numCountries = std::max(1, std::min(300, atoi(m_countryCountText.c_str()))); m_editingCountryCount = false; }
        if (IsKeyPressed(KEY_ESCAPE)) m_editingCountryCount = false;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, cntText)) m_editingCountryCount = false;
    }

    // Province size slider (exponential: 0.2–5.0, 1.0 at midpoint)
    y = m_toolbarH + 315;
    Rectangle provSize = {(float)px, (float)y, 200, 12};
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {provSize.x-4, provSize.y-4, provSize.width+8, provSize.height+8})) {
        float s = (GetMouseX() - provSize.x) / provSize.width;
        s = std::max(0.0f, std::min(1.0f, s));
        m_genParams.provinceDensity = 0.2f * powf(500.0f, s);
    }

    // Generate World button (does landmass + provinces + game data)
    y = m_toolbarH + 360;
    Rectangle genBtn = {(float)px, (float)y, 200, 38};
    if (drawButton("Generate World", genBtn, false, 16)) {
        m_genPending = 1;
        m_genStatus = "Generating world...";
    }

    // Export buttons: into the game's own custom_maps (where the map browser
    // will find it) and out to a path the player chooses (where they can send
    // it to someone). Both write the same archive.
    y = m_toolbarH + 410;
    Rectangle exportBtn = {(float)px, (float)y, 200, 34};
    if (drawButton("Export .odmap", exportBtn, false, 14)) {
        if (exportODMap().empty()) {
            m_warningMsg = "Export failed";
            m_warningTimer = 3.0f;
        } else {
            m_saveStatus = "Exported to data/custom_maps/";
            m_saveStatusTimer = 4.0f;
        }
    }
    if (fileDialog::available()) {
        Rectangle exportFileBtn = {(float)px, (float)(m_toolbarH + 448), 200, 30};
        if (drawButton("Export to File...", exportFileBtn, false, 13)) promptExportToFile();
    }
}

void MapEditor::drawGeneratorPanel() {
    int px = m_screenW - m_panelW + 12;
    const int LBL = 13;

    DrawText(T("Map Generator"), px, m_toolbarH + 16, 18, ACCENT);

    // Seed label above the input box
    DrawText(T("Seed"), px, m_toolbarH + 56, LBL, LIGHTGRAY);
    Rectangle seedRect = {(float)px, (float)(m_toolbarH + 70), 172, 26};
    DrawRectangleRounded(seedRect, 0.08f, 4, m_editingSeed ? Color{35,35,50,255} : Color{28,28,35,255});
    DrawRectangleRoundedLines(seedRect, 0.08f, 4, m_editingSeed ? ACCENT : Color{60,60,70,255});
    const char* s = m_editingSeed ? m_seedText.c_str() : TextFormat("%d", m_genParams.seed);
    DrawText(s, (int)seedRect.x + 8, (int)seedRect.y + 5, 14, m_editingSeed ? ACCENT : WHITE);
    if (m_editingSeed) DrawText("|", (int)(seedRect.x + 8 + MeasureText(s, 14)), (int)seedRect.y + 5, 14, ACCENT);
    // RND button
    Rectangle rndBtn = {(float)(px + 178), (float)(m_toolbarH + 70), 34, 26};
    bool rndHov = CheckCollisionPointRec(GetMousePosition(), rndBtn);
    DrawRectangleRounded(rndBtn, 0.08f, 4, rndHov ? Color{40,40,55,255} : Color{30,30,40,255});
    DrawRectangleRoundedLines(rndBtn, 0.08f, 4, Color{80,80,90,255});
    DrawText(T("RND"), px + 185, (int)rndBtn.y + 6, 12, rndHov ? ACCENT : LIGHTGRAY);

    // Coverage label + slider
    DrawText(T("Land Coverage:"), px, m_toolbarH + 110, LBL, LIGHTGRAY);
    Rectangle cov = {(float)px, (float)(m_toolbarH + 128), 200, 12};
    DrawRectangleRounded(cov, 0.3f, 4, Color{50,50,60,255});
    Rectangle cfill = {cov.x, cov.y, cov.width * m_genParams.landCoverage, cov.height};
    if (cfill.width > 1) DrawRectangleRounded(cfill, 0.3f, 4, ACCENT);
    DrawText(TextFormat("%.0f%%", m_genParams.landCoverage*100), px+210, m_toolbarH + 127, LBL, WHITE);

    // Continents label + ±
    DrawText(T("Landmass centers:"), px, m_toolbarH + 163, LBL, LIGHTGRAY);
    DrawText(TextFormat("%d", m_genParams.numContinents), px+140, m_toolbarH + 163, 14, WHITE);
    Rectangle cmin = {(float)px, (float)(m_toolbarH + 181), 28, 24}, cplu = {(float)(px+170), (float)(m_toolbarH + 181), 28, 24};
    DrawText("-", (int)cmin.x+9, (int)cmin.y+2, 16, WHITE);
    DrawText("+", (int)cplu.x+9, (int)cplu.y+2, 16, WHITE);

    // Jaggedness label + slider
    DrawText(T("Jaggedness:"), px, m_toolbarH + 215, LBL, LIGHTGRAY);
    Rectangle jag = {(float)px, (float)(m_toolbarH + 233), 200, 12};
    DrawRectangleRounded(jag, 0.3f, 4, Color{50,50,60,255});
    Rectangle jfill = {jag.x, jag.y, jag.width * m_genParams.jaggedness, jag.height};
    if (jfill.width > 1) DrawRectangleRounded(jfill, 0.3f, 4, ACCENT);
    DrawText(TextFormat("%.0f", m_genParams.jaggedness), px+210, m_toolbarH + 232, LBL, WHITE);

    // Countries slider + editable number
    DrawText(T("Countries:"), px, m_toolbarH + 255, LBL, LIGHTGRAY);
    Rectangle cntSlider = {(float)px, (float)(m_toolbarH + 270), 200, 12};
    DrawRectangleRounded(cntSlider, 0.3f, 4, Color{50,50,60,255});
    float cntPct = (m_genParams.numCountries - 1) / 299.0f;
    Rectangle cntFill = {cntSlider.x, cntSlider.y, cntSlider.width * cntPct, cntSlider.height};
    if (cntFill.width > 1) DrawRectangleRounded(cntFill, 0.3f, 4, ACCENT);
    // Editable text field for exact number
    Rectangle cntText = {(float)(px + 164), (float)(m_toolbarH + 288), 44, 22};
    bool cntHov = CheckCollisionPointRec(GetMousePosition(), cntText);
    DrawRectangleRounded(cntText, 0.08f, 4, m_editingCountryCount ? Color{35,35,50,255} : (cntHov ? Color{30,30,40,255} : Color{25,25,35,255}));
    DrawRectangleRoundedLines(cntText, 0.08f, 4, m_editingCountryCount ? ACCENT : Color{60,60,70,255});
    const char* cntStr = m_editingCountryCount ? m_countryCountText.c_str() : TextFormat("%d", m_genParams.numCountries);
    DrawText(cntStr, (int)cntText.x + 6, (int)cntText.y + 3, 12, m_editingCountryCount ? ACCENT : WHITE);
    if (m_editingCountryCount) DrawText("|", (int)(cntText.x + 6 + MeasureText(cntStr, 12)), (int)cntText.y + 3, 12, ACCENT);

    // Province size slider
    DrawText(T("Province Size:"), px, m_toolbarH + 295, LBL, LIGHTGRAY);
    Rectangle provSize = {(float)px, (float)(m_toolbarH + 315), 200, 12};
    DrawRectangleRounded(provSize, 0.3f, 4, Color{50,50,60,255});
    {
        float s = logf(m_genParams.provinceDensity / 0.2f) / logf(500.0f);
        s = std::max(0.0f, std::min(1.0f, s));
        Rectangle pfill = {provSize.x, provSize.y, provSize.width * s, provSize.height};
        if (pfill.width > 2) DrawRectangleRounded(pfill, 0.3f, 4, ACCENT);
    }
    const char* szLabel = "Medium";
    float pd = m_genParams.provinceDensity;
    if (pd < 0.4f) szLabel = "Very Large";
    else if (pd < 0.8f) szLabel = "Large";
    else if (pd < 2.0f) szLabel = "Medium";
    else if (pd < 5.0f) szLabel = "Small";
    else if (pd < 12.0f) szLabel = "Very Small";
    else if (pd < 30.0f) szLabel = "Tiny";
    else szLabel = "Micro";
    DrawText(szLabel, px+210, m_toolbarH + 314, LBL, WHITE);

    // Generate World button (landmass + provinces + game data)
    Rectangle genBtn = {(float)px, (float)(m_toolbarH + 360), 200, 38};
    drawButton("Generate World", genBtn, false, 16);

    // Export buttons (see updateGeneratorPanel for the layout this mirrors)
    Rectangle exportBtn = {(float)px, (float)(m_toolbarH + 410), 200, 34};
    drawButton("Export .odmap", exportBtn, false, 14);
    if (fileDialog::available()) {
        Rectangle exportFileBtn = {(float)px, (float)(m_toolbarH + 448), 200, 30};
        drawButton("Export to File...", exportFileBtn, false, 13);
    }

    // Status indicators
    int sy = m_toolbarH + (fileDialog::available() ? 490 : 460);
    DrawText(m_hasProvinces ? "Provinces: OK" : "Provinces: --", px, sy, 12, m_hasProvinces ? GREEN : GRAY);
    DrawText(m_hasGameData ? "Game Data: OK" : "Game Data: --", px, sy + 15, 12, m_hasGameData ? GREEN : GRAY);
}

// ── Province Panel ─────────────────────────────────────────────

void MapEditor::updateProvincePanel() {}
void MapEditor::drawProvincePanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    int listW = m_panelW - 24;
    bool inputOk = !anyModalOpen();
    Vector2 mouse = GetMousePosition();
    DrawText(T("Province Editor"), px, y, 18, ACCENT); y += 26;

    // ── Tools: Select picks a province, Paint grows the selected one ──
    {
        int half = (listW - 8) / 2;
        Rectangle selBtn = {(float)px, (float)y, (float)half, 24};
        Rectangle paintBtn = {(float)(px + half + 8), (float)y, (float)half, 24};
        if (drawButton("Select", selBtn, m_provTool == 0, 12) && inputOk) m_provTool = 0;
        if (drawButton("Paint", paintBtn, m_provTool == 1, 12) && inputOk) m_provTool = 1;
        y += 28;
        if (m_provTool == 1)
            DrawText(T("Drag on land to grow the province"), px, y, 10, Color{140,160,140,220});
        else
            DrawText(T("Click the map to pick a province"), px, y, 10, GRAY);
        y += 16;
        Rectangle newBtn = {(float)px, (float)y, (float)listW, 24};
        if (drawButton("+ New Province (paint it in)", newBtn, false, 12) && inputOk)
            createNewProvince();
        y += 28;
        if (!m_hasProvinces) {
            // Blank project: make it obvious you don't have to run the generator
            DrawText(T("No provinces yet — this starts one"), px, y, 10, Color{140,160,140,220});
            y += 13;
            DrawText(T("on a blank map. Paint land first."), px, y, 10, Color{140,160,140,220});
            y += 15;
        }

        // Buildings overlay: see industry/forts/ports across the whole map
        bool ovOn = m_renderer && m_renderer->getShowEditorOverlay();
        Rectangle ovBtn = {(float)px, (float)y, (float)listW, 22};
        if (drawButton(ovOn ? "Buildings overlay: ON" : "Buildings overlay: OFF", ovBtn, ovOn, 11)
            && inputOk && m_renderer) {
            if (!ovOn) rebuildBuildingsOverlay(); // fresh data every time it's enabled
            m_renderer->setShowEditorOverlay(!ovOn);
        }
        y += 24;
        if (ovOn) {
            DrawText("industry", px, y, 9, Color{255, 170, 40, 255});
            DrawText("fort", px + 60, y, 9, Color{230, 60, 60, 255});
            DrawText("port", px + 95, y, 9, Color{40, 160, 255, 255});
            DrawText(T("(re-toggle to refresh)"), px + 130, y, 9, GRAY);
            y += 14;
        }

        // Manual refresh for the border-glow gradient (auto-runs after
        // generation/import/ownership-brush edits; use this after color
        // tweaks or other edits that don't trigger it automatically).
        Rectangle gradBtn = {(float)px, (float)y, (float)listW, 22};
        if (drawButton("Refresh Gradient", gradBtn, false, 11) && inputOk)
            computePoliticalGradient();
        y += 26;
        y += 4;
    }

    if (m_selectedProvince < 0) {
        DrawText(T("No province selected."), px, y, 13, GRAY); y += 22;
        DrawText(T("Click one on the map, then use"), px, y, 12, GRAY); y += 15;
        DrawText(T("Paint to reshape it, or edit its"), px, y, 12, GRAY); y += 15;
        DrawText(T("resources and troops below."), px, y, 12, GRAY);
        return;
    }
    Province* prov = m_editProvinces.getProvinceById(m_selectedProvince);
    if (!prov) {
        DrawText(T("Province not found."), px, y, 13, GRAY);
        return;
    }
    EditorProvinceData& d = m_provinceData[m_selectedProvince];

    // Header (fixed above the scroll region)
    const Country* owner = m_editCountries.getCountry(prov->countryId);
    DrawText(TextFormat("#%d  %s", prov->id, prov->name.c_str()), px, y, 13, WHITE); y += 16;
    DrawText(TextFormat(T("Owner: %s"), owner ? owner->name.c_str() : "(none)"), px, y, 11, LIGHTGRAY); y += 16;
    {
        Rectangle delBtn = {(float)px, (float)y, (float)listW, 24};
        if (drawButton("Delete Province (merge into...)", delBtn, false, 11) && inputOk)
            deleteSelectedProvince();
        y += 30;
    }

    // ── Scrollable content region ──
    int contentTop = y;
    int contentBottom = m_screenH - m_bottomH - 8;
    int viewH = contentBottom - contentTop;
    Rectangle viewRect = {(float)(px - 4), (float)contentTop, (float)(listW + 8), (float)viewH};
    if (inputOk && CheckCollisionPointRec(mouse, viewRect))
        m_provPanelScroll -= (int)(GetMouseWheelMove() * 24);

    // Immediate-mode widgets, laid out with a running cursor
    int cy = contentTop - m_provPanelScroll;
    BeginScissorMode(px - 4, contentTop, listW + 8, viewH);

    auto slider = [&](const char* label, float& v) {
        DrawText(label, px, cy, 11, DARKGRAY);
        DrawText(TextFormat("%.0f", v), px + listW - 28, cy, 11, WHITE);
        Rectangle sl = {(float)px, (float)(cy + 13), (float)(listW - 34), 10};
        DrawRectangleRounded(sl, 0.3f, 4, Color{50,50,60,255});
        float pct = std::max(0.0f, std::min(1.0f, v / 100.0f));
        Rectangle fill = {sl.x + 2, sl.y + 2, (sl.width - 4) * pct, sl.height - 4};
        if (fill.width > 2) DrawRectangleRounded(fill, 0.3f, 4, ACCENT);
        if (inputOk && IsMouseButtonDown(MOUSE_LEFT_BUTTON) &&
            CheckCollisionPointRec(mouse, {sl.x-4, sl.y-4, sl.width+8, sl.height+8}) &&
            CheckCollisionPointRec(mouse, viewRect)) {
            float p = (GetMouseX() - sl.x) / sl.width;
            v = std::max(0.0f, std::min(1.0f, p)) * 100.0f;
            trackChange();
        }
        cy += 30;
    };
    auto stepper = [&](const char* label, int& v, int lo, int hi) {
        DrawText(label, px, cy + 4, 11, DARKGRAY);
        Rectangle minus = {(float)(px + listW - 76), (float)cy, 24, 20};
        Rectangle plus  = {(float)(px + listW - 24), (float)cy, 24, 20};
        DrawText(TextFormat("%d", v), px + listW - 46, cy + 3, 13, WHITE);
        bool inView = CheckCollisionPointRec(mouse, viewRect);
        if (drawButton("-", minus, false, 13) && inputOk && inView && v > lo) { v--; trackChange(); }
        if (drawButton("+", plus, false, 13) && inputOk && inView && v < hi) { v++; trackChange(); }
        cy += 26;
    };

    DrawText(T("Resources (amount 0-100):"), px, cy, 12, LIGHTGRAY); cy += 16;
    slider("Oil", d.oil);
    slider("Gold", d.gold);
    slider("Rubber", d.rubber);
    slider("Gemstones", d.gemstones);
    slider("Metal", d.metal);
    cy += 4;

    DrawText(T("Buildings:"), px, cy, 12, LIGHTGRAY); cy += 16;
    stepper("Industry (0-10)", d.industryLevel, 0, 10);
    stepper("Fortification (0-5)", d.fortification, 0, 5);
    stepper("Port (0-3)", d.portLevel, 0, 3);
    cy += 4;

    bool inView = CheckCollisionPointRec(mouse, viewRect);

    // Population: a typed numeric field
    {
        DrawText(T("Population:"), px, cy, 12, LIGHTGRAY); cy += 16;
        if (m_provPopEditing && m_provPopEditPid != m_selectedProvince) m_provPopEditing = false;
        Rectangle popRect = {(float)px, (float)cy, (float)listW, 22};
        bool hov = inputOk && inView && CheckCollisionPointRec(mouse, popRect);
        Color bg = m_provPopEditing ? Color{35,35,50,255} : (hov ? Color{30,30,40,255} : Color{25,25,35,255});
        DrawRectangleRounded(popRect, 0.08f, 4, bg);
        DrawRectangleRoundedLines(popRect, 0.08f, 4, m_provPopEditing ? ACCENT : Color{60,60,70,255});
        const char* txt = m_provPopEditing ? m_provPopEditText.c_str() : TextFormat("%lld", d.population);
        DrawText(txt, (int)popRect.x + 6, (int)popRect.y + 4, 13, m_provPopEditing ? ACCENT : WHITE);
        if (m_provPopEditing) DrawText("|", (int)(popRect.x + 6 + MeasureText(txt, 13)), (int)popRect.y + 4, 13, ACCENT);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !m_provPopEditing) {
            m_provPopEditing = true;
            m_provPopEditPid = m_selectedProvince;
            m_provPopEditText = std::to_string(d.population);
        }
        if (m_provPopEditing) {
            int key = GetCharPressed();
            while (key > 0) {
                // Every character the field takes. Jittered, because a
                // typed word is a run of distinct taps, not one tap looped.
                Audio::get().playSfx("key_type", 0.12f);
                if (key >= '0' && key <= '9' && m_provPopEditText.size() < 12) m_provPopEditText.push_back((char)key);
                key = GetCharPressed();
            }
            odTextEditKeys(m_provPopEditText, 12, "", true);
            bool commit = IsKeyPressed(KEY_ENTER);
            bool clickAway = inputOk && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, popRect);
            if (commit || clickAway) {
                long long v = m_provPopEditText.empty() ? 0 : atoll(m_provPopEditText.c_str());
                d.population = std::max(0LL, v);
                trackChange();
                m_provPopEditing = false;
            } else if (IsKeyPressed(KEY_ESCAPE)) {
                m_provPopEditing = false;
            }
        }
        cy += 28;
    }

    // ── Political compass (province-level) ──
    DrawText(T("Political Compass:"), px, cy, 12, LIGHTGRAY); cy += 16;
    auto compassSlider = [&](const char* label, const char* lo, const char* hi, float& v) {
        DrawText(label, px, cy, 10, DARKGRAY);
        Rectangle sl = {(float)px, (float)(cy + 12), (float)(listW - 44), 10};
        DrawRectangleRounded(sl, 0.3f, 4, Color{50,50,60,255});
        float pct = std::max(0.0f, std::min(1.0f, (v + 100.0f) / 200.0f));
        Rectangle fill = {sl.x + 2, sl.y + 2, (sl.width - 4) * pct, sl.height - 4};
        if (fill.width > 2) DrawRectangleRounded(fill, 0.3f, 4, ACCENT);
        DrawText(TextFormat("%+.0f", v), (int)(sl.x + sl.width + 6), (int)cy + 10, 11, WHITE);
        if (inputOk && IsMouseButtonDown(MOUSE_LEFT_BUTTON) &&
            CheckCollisionPointRec(mouse, {sl.x-4, sl.y-4, sl.width+8, sl.height+8}) &&
            CheckCollisionPointRec(mouse, viewRect)) {
            float p = (GetMouseX() - sl.x) / sl.width;
            v = std::max(0.0f, std::min(1.0f, p)) * 200.0f - 100.0f;
            trackChange();
        }
        DrawText(lo, px, cy + 24, 9, GRAY);
        int hw = MeasureText(hi, 9);
        DrawText(hi, px + listW - 44 - hw, cy + 24, 9, GRAY);
        cy += 38;
    };
    compassSlider("Economic", "Left", "Right", d.compassEconomic);
    compassSlider("Social", "Auth", "Libertarian", d.compassSocial);
    cy += 4;

    // ── Ethnic groups ──
    DrawText(T("Ethnic Groups:"), px, cy, 12, LIGHTGRAY);
    float ethSum = 0; for (auto& [n, p] : d.ethnicGroups) ethSum += p;
    DrawText(TextFormat("%.0f%%", ethSum), px + listW - 40, cy, 11, fabsf(ethSum - 100.0f) < 1.0f ? Color{140,200,140,255} : Color{220,160,90,255});
    cy += 16;
    {
        int removeEthIdx = -1;
        for (int ei = 0; ei < (int)d.ethnicGroups.size(); ++ei) {
            auto& [name, pct] = d.ethnicGroups[ei];
            // Row 1: swatch + name (click to reassign) ... %  [x]
            Rectangle nameBtn = {(float)px, (float)cy, (float)(listW - 92), 16};
            bool nameHov = inputOk && inView && CheckCollisionPointRec(mouse, nameBtn);
            Color swatch = m_ethnicColors.count(name) ? m_ethnicColors[name] : Color{150,150,150,255};
            DrawRectangle(px + 2, cy + 3, 10, 10, swatch);
            std::string label = name;
            if (MeasureText(label.c_str(), 11) > (int)nameBtn.width - 18)
                label = label.substr(0, std::max(1, ((int)nameBtn.width - 24) / 6)) + "..";
            DrawText(label.c_str(), px + 16, cy + 2, 11, nameHov ? ACCENT : WHITE);
            if (nameHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) openEthnicityPicker(m_selectedProvince, ei);

            DrawText(TextFormat("%.0f%%", pct), px + listW - 66, cy + 2, 11, WHITE);
            Rectangle xBtn = {(float)(px + listW - 20), (float)cy, 20, 16};
            if (drawButton("x", xBtn, false, 11) && inputOk && inView) removeEthIdx = ei;

            // Row 2: full-width drag slider (0-100%), redistributing the
            // difference across the other groups so the total stays at 100%.
            Rectangle sl = {(float)px, (float)(cy + 18), (float)listW, 10};
            DrawRectangleRounded(sl, 0.3f, 4, Color{50,50,60,255});
            float p = std::max(0.0f, std::min(1.0f, pct / 100.0f));
            Rectangle fill = {sl.x + 2, sl.y + 2, (sl.width - 4) * p, sl.height - 4};
            if (fill.width > 2) DrawRectangleRounded(fill, 0.3f, 4, swatch);
            if (inputOk && IsMouseButtonDown(MOUSE_LEFT_BUTTON) &&
                CheckCollisionPointRec(mouse, {sl.x-4, sl.y-4, sl.width+8, sl.height+8}) &&
                CheckCollisionPointRec(mouse, viewRect)) {
                float mp = (GetMouseX() - sl.x) / sl.width;
                float newPct = std::max(0.0f, std::min(1.0f, mp)) * 100.0f;
                float delta = newPct - pct;
                if (fabsf(delta) > 0.001f) {
                    float otherTotal = 0.0f;
                    for (int oi = 0; oi < (int)d.ethnicGroups.size(); ++oi)
                        if (oi != ei) otherTotal += d.ethnicGroups[oi].second;
                    if (otherTotal > 0.01f) {
                        delta = std::min(delta, otherTotal); // can't take more than others have
                        newPct = pct + delta;
                        float scale = (otherTotal - delta) / otherTotal;
                        for (int oi = 0; oi < (int)d.ethnicGroups.size(); ++oi)
                            if (oi != ei) d.ethnicGroups[oi].second *= scale;
                    } else {
                        newPct = 100.0f; // only group with any share
                    }
                    pct = newPct;
                    trackChange();
                }
            }
            cy += 32;

            // Row 3: this ethnicity's own starting-policy set (separate from
            // the country's) — opens the shared policy-screen overlay.
            int policyCount = (int)m_ethnicityPolicies[name].size();
            Rectangle policyBtn = {(float)px, (float)cy, (float)listW, 20};
            std::string policyLabel = policyCount > 0
                ? TextFormat("Policies (%d)", policyCount) : "Policies...";
            if (drawButton(policyLabel.c_str(), policyBtn, false, 10) && inputOk && inView)
                openSetModeEthnicity(name, owner ? owner->isoA3 : "", prov->countryId);
            cy += 26;
        }
        if (removeEthIdx >= 0) { d.ethnicGroups.erase(d.ethnicGroups.begin() + removeEthIdx); trackChange(); }

        Rectangle addEthBtn = {(float)px, (float)cy, (float)listW, 22};
        if (drawButton("+ Add ethnicity", addEthBtn, false, 11) && inputOk && inView)
            openEthnicityPicker(m_selectedProvince, -1);
        cy += 30;

        // Pie chart of ethnic makeup
        if (!d.ethnicGroups.empty() && ethSum > 0.01f) {
            float pieR = 44.0f;
            float pieCx = px + pieR + 4, pieCy = cy + pieR;
            float startAngle = 0.0f;
            for (auto& [name, pct] : d.ethnicGroups) {
                if (pct <= 0.01f) continue;
                float sweep = pct / ethSum * 360.0f;
                Color swatch = m_ethnicColors.count(name) ? m_ethnicColors[name] : Color{150,150,150,255};
                DrawCircleSector({pieCx, pieCy}, pieR, startAngle, startAngle + sweep, 24, swatch);
                startAngle += sweep;
            }
            DrawCircleSectorLines({pieCx, pieCy}, pieR, 0, 360, 32, Color{20,20,24,200});
            // Legend to the right of the chart
            float lx = pieCx + pieR + 14, ly = cy;
            for (auto& [name, pct] : d.ethnicGroups) {
                if (pct <= 0.01f) continue;
                Color swatch = m_ethnicColors.count(name) ? m_ethnicColors[name] : Color{150,150,150,255};
                DrawRectangle((int)lx, (int)ly + 2, 8, 8, swatch);
                std::string lbl = name + TextFormat(" %.0f%%", pct / ethSum * 100.0f);
                if (MeasureText(lbl.c_str(), 10) > (int)(listW - (lx - px) - 8))
                    lbl = lbl.substr(0, std::max(1, (int)((listW - (lx - px) - 12) / 6))) + "..";
                DrawText(lbl.c_str(), (int)lx + 12, (int)ly, 10, LIGHTGRAY);
                ly += 14;
            }
            cy += pieR * 2 + 10;
        }
    }
    cy += 4;

    // ── Troops ──
    DrawText(T("Troops:"), px, cy, 12, LIGHTGRAY); cy += 16;
    auto& all = m_editCountries.getAll();
    std::vector<int> cids;
    for (auto& [cid, c] : all) if (cid < 65533) cids.push_back(cid);
    std::sort(cids.begin(), cids.end());

    if (m_provTroopEditPid != m_selectedProvince) { m_provTroopEditing = false; m_provTroopEditIdx = -1; }

    int removeIdx = -1;
    for (int ti = 0; ti < (int)d.troops.size(); ++ti) {
        ArmyUnit& u = d.troops[ti];
        const Country* uc = m_editCountries.getCountry(u.countryId);
        // Click the name to open a searchable allegiance picker
        Rectangle nameBtn = {(float)px, (float)cy, (float)(listW - 118), 20};
        bool nameHov = inputOk && inView && CheckCollisionPointRec(mouse, nameBtn);
        if (uc) DrawRectangle(px + 2, cy + 5, 10, 10, uc->color);
        std::string uname = uc ? uc->name : TextFormat("cid %d", u.countryId);
        if (MeasureText(uname.c_str(), 11) > (int)nameBtn.width - 20)
            uname = uname.substr(0, std::max(1, ((int)nameBtn.width - 26) / 6)) + "..";
        DrawText(uname.c_str(), px + 16, cy + 4, 11, nameHov ? ACCENT : WHITE);
        if (nameHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
            openCountryPicker(m_selectedProvince, ti);

        // Troop count: typed numeric field
        Rectangle countRect = {(float)(px + listW - 114), (float)cy, 94, 20};
        Rectangle xBtn = {(float)(px + listW - 20), (float)cy, 20, 20};
        bool rowEditing = (m_provTroopEditIdx == ti) ? m_provTroopEditing : false;
        long long cv = u.count;
        if (drawIntField(countRect, cv, 0, 100000000, rowEditing, m_provTroopEditText, inputOk && inView)) {
            u.count = (int)cv;
            trackChange();
        }
        if (rowEditing) { m_provTroopEditing = true; m_provTroopEditIdx = ti; m_provTroopEditPid = m_selectedProvince; }
        else if (m_provTroopEditIdx == ti) { m_provTroopEditing = false; }

        if (drawButton("x", xBtn, false, 12) && inputOk && inView) removeIdx = ti;
        cy += 24;
    }
    if (removeIdx >= 0) { d.troops.erase(d.troops.begin() + removeIdx); trackChange(); }

    Rectangle addBtn = {(float)px, (float)cy, (float)listW, 24};
    if (drawButton("+ Add unit", addBtn, false, 12) && inputOk && inView) {
        int cid = prov->countryId > 0 ? prov->countryId : (cids.empty() ? 0 : cids[0]);
        d.troops.push_back(ArmyUnit{cid, 1000});
        trackChange();
    }
    cy += 30;
    DrawText(T("Shift = +/-10k per click"), px, cy, 10, GRAY);
    cy += 18;

    EndScissorMode();

    // Clamp scroll to content height
    int contentH = (cy + m_provPanelScroll) - contentTop;
    int maxScroll = std::max(0, contentH - viewH);
    m_provPanelScroll = std::max(0, std::min(m_provPanelScroll, maxScroll));
}

// ── Country Panel ──────────────────────────────────────────────

void MapEditor::updateCountryPanel() {
    Vector2 mouse = GetMousePosition();
    int px = m_screenW - m_panelW + 12;
    int panelH = m_screenH - m_toolbarH - m_bottomH;
    // Compact list takes top ~40% of panel (MUST match drawCountryPanel)
    const int LIST_START = m_toolbarH + 50;
    const int LIST_HEIGHT = (int)(panelH * 0.4f);
    int listW = m_panelW - 24;
    int itemH = 18;

    auto& all = m_editCountries.getAll();
    std::vector<int> cids;
    for (auto& [cid, c] : all) cids.push_back(cid);
    std::sort(cids.begin(), cids.end());

    int visible = LIST_HEIGHT / itemH;

    if (CheckCollisionPointRec(mouse, {(float)px, (float)LIST_START, (float)listW, (float)LIST_HEIGHT}))
        m_countryScroll -= GetMouseWheelMove();
    int maxScroll = std::max(0, (int)cids.size() - visible);
    if (m_countryScroll < 0) m_countryScroll = 0;
    if (m_countryScroll > maxScroll) m_countryScroll = maxScroll;

    for (int i = m_countryScroll; i < (int)cids.size() && i < m_countryScroll + visible; ++i) {
        int yi = LIST_START + (i - m_countryScroll) * itemH;
        Rectangle r = {(float)px, (float)yi, (float)listW, (float)(itemH - 1)};
        if (CheckCollisionPointRec(mouse, r) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_selectedCountry = cids[i];
            m_editingCountryName = false;
        }
    }

    // ── Selected country details (bottom ~55% of panel) ──
    if (m_selectedCountry >= 0) {
        auto it = all.find(m_selectedCountry);
        if (it == all.end()) { m_selectedCountry = -1; return; }
        Country& c = it->second;

        int editY = LIST_START + LIST_HEIGHT + 6;
        int maxEditY = m_screenH - m_bottomH - 50;

        // Name editing ("Name:" label occupies 16px in the draw pass)
        editY += 16;
        Rectangle nameRect = {(float)px, (float)editY, (float)listW, 24};
        if (!m_editingCountryName && CheckCollisionPointRec(mouse, nameRect) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_editingCountryName = true;
            m_editingNameText = c.name;
            m_renameCountryId = m_selectedCountry;
        }
        if (m_editingCountryName && m_renameCountryId != m_selectedCountry) m_editingCountryName = false;
        if (m_editingCountryName) {
            int key = GetCharPressed();
            while (key > 0) {
                // Every character the field takes. Jittered, because a
                // typed word is a run of distinct taps, not one tap looped.
                Audio::get().playSfx("key_type", 0.12f);
                if (key >= 32 && key < 128 && m_editingNameText.size() < 40)
                    m_editingNameText.push_back((char)key);
                key = GetCharPressed();
            }
            odTextEditKeys(m_editingNameText, 40);
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE)) {
                if (!m_editingNameText.empty() && IsKeyPressed(KEY_ENTER)) {
                    it->second.name = m_editingNameText;
                    m_dirty = true;
                }
                m_editingCountryName = false;
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, nameRect)) {
                if (!m_editingNameText.empty()) {
                    it->second.name = m_editingNameText;
                    m_dirty = true;
                }
                m_editingCountryName = false;
            }
        }

        // Flag preview bounds (matching drawCountryPanel layout)
        editY += 30;
        Rectangle flagRect = {(float)px, (float)editY, 120, 60};

        // Color swatch
        editY += (int)flagRect.height + 10;
        // Buttons
        if (editY + 28 < maxEditY) {
            Rectangle delBtn = {(float)px, (float)editY, (float)(listW * 0.42f), 24};
            if (CheckCollisionPointRec(mouse, delBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                if (all.size() > 1) { all.erase(m_selectedCountry); m_selectedCountry = -1; m_dirty = true; }
            }
            Rectangle createBtn = {(float)(px + listW * 0.46f + 4), (float)editY, (float)(listW * 0.42f), 24};
            if (CheckCollisionPointRec(mouse, createBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                int newCid = 1; while (all.count(newCid)) newCid++;
                Country nc; nc.id = newCid; nc.name = "New Country";
                nc.color = Color{(uint8_t)(rand()%200), (uint8_t)(rand()%200), (uint8_t)(rand()%200), 255};
                nc.isoA3 = ""; nc.treasury = 1000.0f;
                all[newCid] = nc; m_selectedCountry = newCid; m_dirty = true;
            }
        }

        editY += 30;

        // ── Ownership brush toggle (visual mirror lives in drawCountryPanel;
        //    editY must advance by the exact same amount in both functions or
        //    every hitbox below drifts from what's drawn) ──
        editY += 6;
        Rectangle brushBtn = {(float)px, (float)editY, (float)listW, 22};
        if (CheckCollisionPointRec(mouse, brushBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_countryBrushActive = !m_countryBrushActive;
            if (m_countryBrushActive) m_claimsBrushActive = false; // mutually exclusive paint modes
        }
        editY += 24;
        if (m_countryBrushActive) editY += 16;
        editY += 4;

        // Claims brush (visual mirror in drawCountryPanel — editY must advance
        // identically in both or every hitbox below drifts)
        Rectangle claimBtn = {(float)px, (float)editY, (float)listW, 22};
        if (CheckCollisionPointRec(mouse, claimBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_claimsBrushActive = !m_claimsBrushActive;
            if (m_claimsBrushActive) {
                m_countryBrushActive = false; // the two paint modes are mutually exclusive
                rebuildClaimsOverlay();
            }
        }
        editY += 24;
        if (m_claimsBrushActive) {
            int half = (listW - 6) / 2;
            Rectangle addBtn = {(float)px, (float)editY, (float)half, 20};
            Rectangle eraseBtn = {(float)(px + half + 6), (float)editY, (float)half, 20};
            if (CheckCollisionPointRec(mouse, addBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                m_claimsBrushErase = false;
            if (CheckCollisionPointRec(mouse, eraseBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                m_claimsBrushErase = true;
            editY += 24;
            editY += 14;
        }
        editY += 4;

        Rectangle ethnicBtn = {(float)px, (float)editY, (float)listW, 22};
        if (CheckCollisionPointRec(mouse, ethnicBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
            openCountryEthnicList(m_selectedCountry);
        editY += 26;

        // ── Regenerate flag + country colour sliders ──
        // UNC/BLC/SPC territories keep their fixed flags
        bool specialTerritory = m_selectedCountry >= 65533;
        editY += 30;
        Rectangle regenBtn = {(float)px, (float)editY, (float)listW, 22};
        if (!specialTerritory && CheckCollisionPointRec(mouse, regenBtn) &&
            IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
            regenerateFlag(c);
        editY += 26;
        editY += 14; // "Country color:" label
        {
            uint8_t* chans[3] = {&c.color.r, &c.color.g, &c.color.b};
            for (int ch = 0; ch < 3; ++ch) {
                Rectangle sl = {(float)(px + 16), (float)editY, (float)(listW - 60), 10};
                if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) &&
                    CheckCollisionPointRec(mouse, {sl.x - 4, sl.y - 4, sl.width + 8, sl.height + 8})) {
                    float pct = (GetMouseX() - sl.x) / sl.width;
                    pct = std::max(0.0f, std::min(1.0f, pct));
                    uint8_t nv = (uint8_t)(pct * 255.0f);
                    if (*chans[ch] != nv) {
                        *chans[ch] = nv;
                        m_dirty = true;
                        markPoliticalDirtyFull(); // this country's pixels can be scattered anywhere
                        liveRecolorCountry(m_selectedCountry); // territory updates live
                    }
                }
                editY += 18;
            }
        }
        editY += 4;

        // ── Compass sliders ──
        editY += 6;
        if (editY + 120 < maxEditY) {
            editY += 16;
            int sliderW = listW - 10;
            // Economic slider drag
            editY += 14;
            Rectangle econSlide = {(float)px, (float)editY, (float)sliderW, 10};
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {econSlide.x-4, econSlide.y-4, econSlide.width+8, econSlide.height+8})) {
                float pct = (GetMouseX() - econSlide.x) / econSlide.width;
                pct = std::max(0.0f, std::min(1.0f, pct));
                c.compassEconomic = pct * 200.0f - 100.0f;
                m_dirty = true;
            }
            editY += 30;
            // Social slider drag
            editY += 14;
            Rectangle socSlide = {(float)px, (float)editY, (float)sliderW, 10};
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(mouse, {socSlide.x-4, socSlide.y-4, socSlide.width+8, socSlide.height+8})) {
                float pct = (GetMouseX() - socSlide.x) / socSlide.width;
                pct = std::max(0.0f, std::min(1.0f, pct));
                c.compassSocial = pct * 200.0f - 100.0f;
                m_dirty = true;
            }
            editY += 30;
            // ── Research / policy set-mode overlays (separate buttons) ──
            editY += 6;
            int halfW = (listW - 8) / 2;
            Rectangle resBtn = {(float)px, (float)editY, (float)halfW, 28};
            Rectangle docBtn = {(float)(px + halfW + 8), (float)editY, (float)halfW, 28};
            if (CheckCollisionPointRec(mouse, resBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                openSetMode(m_selectedCountry, false);
            if (CheckCollisionPointRec(mouse, docBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                openSetMode(m_selectedCountry, true);
        }
    }
}

void MapEditor::drawCountryPanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    DrawText(T("Country Editor"), px, y, 18, ACCENT); y += 30;

    auto& all = m_editCountries.getAll();
    if (all.empty()) {
        DrawText(T("No countries loaded."), px, y, 14, GRAY); y += 18;
        DrawText(T("Generate a world first."), px, y, 14, GRAY);
        return;
    }

    std::vector<int> cids;
    for (auto& [cid, c] : all) cids.push_back(cid);
    std::sort(cids.begin(), cids.end());

    int panelH = m_screenH - m_toolbarH - m_bottomH;
    const int LIST_START = m_toolbarH + 50;
    const int LIST_HEIGHT = (int)(panelH * 0.4f);
    int listW = m_panelW - 24;
    int itemH = 18;
    int visible = LIST_HEIGHT / itemH;

    DrawText(TextFormat(T("Countries: %zu"), all.size()), px, LIST_START - 14, 12, LIGHTGRAY);

    // Scrollbar
    float totalH = (float)cids.size() * itemH;
    float viewH = (float)visible * itemH;
    if (totalH > viewH) {
        float thumbH = viewH / totalH * viewH;
        float thumbY = LIST_START + (float)m_countryScroll / std::max(1, (int)cids.size() - visible) * (viewH - thumbH);
        DrawRectangle(px + listW + 2, LIST_START, 6, (int)viewH, Color{50,50,60,255});
        DrawRectangle(px + listW + 2, (int)thumbY, 6, (int)thumbH, Color{100,100,120,255});
    }

    // Draw compact list
    for (int i = m_countryScroll; i < (int)cids.size() && i < m_countryScroll + visible; ++i) {
        int cid = cids[i];
        auto it = all.find(cid);
        if (it == all.end()) continue;
        Country& c = it->second;
        int yi = LIST_START + (i - m_countryScroll) * itemH;
        bool sel = (cid == m_selectedCountry);
        Color bg = sel ? ColorAlpha(ACCENT, 0.12f) : Color{0,0,0,0};
        if (!sel) {
            Vector2 mp = GetMousePosition();
            if (mp.x >= px && mp.x < px + listW && mp.y >= yi && mp.y < yi + itemH)
                bg = Color{255,255,255,8};
        }
        DrawRectangle(px, yi, listW, itemH - 1, bg);
        DrawRectangle(px + 2, yi + 2, 10, 10, c.color);
        const char* label = c.name.c_str();
        int tw = MeasureText(label, 12);
        if (tw > listW - 20) {
            std::string sn = c.name.substr(0, (listW - 24) / 7) + "..";
            DrawText(sn.c_str(), px + 16, yi + 2, 12, sel ? ACCENT : WHITE);
        } else {
            DrawText(label, px + 16, yi + 2, 12, sel ? ACCENT : WHITE);
        }
    }

    // ── Selected country details ──
    if (m_selectedCountry >= 0) {
        auto it = all.find(m_selectedCountry);
        if (it == all.end()) return;
        Country& c = it->second;

        int editY = LIST_START + LIST_HEIGHT + 6;
        int maxEditY = m_screenH - m_bottomH - 50;

        // Divider line
        DrawLine(px, editY - 3, px + listW, editY - 3, Color{60,60,70,255});

        // Name
        DrawText(T("Name:"), px, editY, 12, LIGHTGRAY); editY += 16;
        Rectangle nameRect = {(float)px, (float)editY, (float)listW, 24};
        bool nameHov = CheckCollisionPointRec(GetMousePosition(), nameRect);
        Color nameBg = m_editingCountryName ? Color{35,35,50,255} : (nameHov ? Color{30,30,40,255} : Color{25,25,35,255});
        DrawRectangleRounded(nameRect, 0.06f, 4, nameBg);
        DrawRectangleRoundedLines(nameRect, 0.06f, 4, m_editingCountryName ? ACCENT : Color{60,60,70,255});
        const char* nameDisplay = m_editingCountryName ? m_editingNameText.c_str() : c.name.c_str();
        DrawText(nameDisplay, (int)nameRect.x + 6, (int)nameRect.y + 4, 13, m_editingCountryName ? ACCENT : WHITE);
        if (m_editingCountryName)
            DrawText("|", (int)(nameRect.x + 6 + MeasureText(nameDisplay, 13)), (int)nameRect.y + 4, 13, ACCENT);

        // Flag preview (cached — only re-render when selection changes)
        editY += 30;
        Rectangle flagRect = {(float)px, (float)editY, 120, 60};
        DrawRectangleLinesEx(flagRect, 1, Color{60,60,70,255});
        if (m_flagPreviewCountry != m_selectedCountry) {
            if (m_flagPreviewTex.id > 0) UnloadTexture(m_flagPreviewTex);
            m_flagPreviewTex = {};
            m_flagPreviewCountry = m_selectedCountry;
            if (!c.flagActual.imagePath.empty()) {
                const std::string& ip = c.flagActual.imagePath;
                bool isSvg = ip.size() > 4 &&
                             (ip.substr(ip.size() - 4) == ".svg" || ip.substr(ip.size() - 4) == ".SVG");
                // raylib's LoadImage can't decode SVG — rasterize via nanosvg
                Image img = isSvg ? FlagRenderer::rasterizeSVG(ip, 240, 120, "")
                                  : LoadImage(ip.c_str());
                if (img.data) {
                    ImageResize(&img, 240, 120);
                    m_flagPreviewTex = LoadTextureFromImage(img);
                    UnloadImage(img);
                }
            } else {
                Texture2D genFlag = FlagRenderer::render(c.flagActual, 240, 120, m_dataDir);
                if (genFlag.id > 0) m_flagPreviewTex = genFlag;
            }
        }
        if (m_flagPreviewTex.id > 0) {
            float sc = fminf(flagRect.width/240.0f, flagRect.height/120.0f);
            float fw = 240*sc, fh = 120*sc;
            DrawTexturePro(m_flagPreviewTex, {0,0,240,120},
                {flagRect.x+(flagRect.width-fw)/2, flagRect.y+(flagRect.height-fh)/2, fw, fh}, {0,0}, 0, WHITE);
        } else {
            DrawText(T("No flag"), (int)flagRect.x + 30, (int)flagRect.y + 22, 12, GRAY);
        }
        DrawText(T("Drop .svg to set your own flag"), (int)(px + flagRect.width + 6), (int)editY + 2, 10, GRAY);

        // Color swatch + ID
        DrawRectangle(px + (int)flagRect.width + 6, (int)editY + 20, 20, 14, c.color);
        DrawText(TextFormat(T("ID: %d"), m_selectedCountry), px + (int)flagRect.width + 30, (int)editY + 20, 11, DARKGRAY);

        // Buttons (if space allows)
        editY += (int)flagRect.height + 10;
        if (editY + 28 < maxEditY) {
            Rectangle delBtn = {(float)px, (float)editY, (float)(listW * 0.42f), 24};
            drawButton("Delete", delBtn, false, 11);
            Rectangle createBtn = {(float)(px + listW * 0.46f + 4), (float)editY, (float)(listW * 0.42f), 24};
            drawButton("Create", createBtn, false, 11);
        }
        editY += 30;

        // ── Ownership brush: drag over provinces to hand them to this country
        //    (click handling lives in updateCountryPanel; this just renders —
        //    editY must advance identically in both or hitboxes drift) ──
        editY += 6;
        Rectangle brushBtn = {(float)px, (float)editY, (float)listW, 22};
        drawButton(m_countryBrushActive ? "Ownership Brush: ON" : "Ownership Brush: OFF",
                   brushBtn, m_countryBrushActive, 11);
        editY += 24;
        if (m_countryBrushActive) {
            DrawText(T("Drag over provinces to give them to this country"), px, editY, 9, GRAY);
            editY += 16;
        }
        editY += 4;

        // ── Claims brush: paint which provinces this country lays claim to ──
        Rectangle claimBtn = {(float)px, (float)editY, (float)listW, 22};
        int claimCount = 0;
        {
            auto ci = m_editorClaims.find(m_selectedCountry);
            if (ci != m_editorClaims.end()) claimCount = (int)ci->second.size();
        }
        drawButton(m_claimsBrushActive ? TextFormat(T("Claims Brush: ON (%d)"), claimCount)
                                       : TextFormat("Claims Brush: OFF (%d)", claimCount),
                   claimBtn, m_claimsBrushActive, 11);
        editY += 24;
        if (m_claimsBrushActive) {
            int half = (listW - 6) / 2;
            Rectangle addBtn = {(float)px, (float)editY, (float)half, 20};
            Rectangle eraseBtn = {(float)(px + half + 6), (float)editY, (float)half, 20};
            drawButton("Claim", addBtn, !m_claimsBrushErase, 11);
            drawButton("Unclaim", eraseBtn, m_claimsBrushErase, 11);
            editY += 24;
            DrawText(T("Drag over provinces to mark claims"), px, editY, 9, GRAY);
            editY += 14;
        }
        editY += 4;

        // ── Ethnic Relations: see every minority this country has and set
        //    how it treats each one (click handling in updateCountryPanel) ──
        Rectangle ethnicBtn = {(float)px, (float)editY, (float)listW, 22};
        drawButton("Ethnic Relations...", ethnicBtn, false, 11);
        editY += 26;

        // ── Regenerate flag + country colour sliders ──
        bool specialTerritory = m_selectedCountry >= 65533; // UNC/BLC/SPC
        editY += 30;
        Rectangle regenBtn = {(float)px, (float)editY, (float)listW, 22};
        if (specialTerritory) {
            DrawText(T("This territory keeps its fixed flag"), px, editY + 5, 11, GRAY);
        } else {
            drawButton("Regenerate Flag", regenBtn, false, 11);
        }
        editY += 26;
        DrawText(T("Country color:"), px, editY, 11, LIGHTGRAY);
        editY += 14;
        {
            const char* chLabels[3] = {"R", "G", "B"};
            uint8_t chVals[3] = {c.color.r, c.color.g, c.color.b};
            Color chCols[3] = {{220, 90, 90, 255}, {90, 200, 90, 255}, {100, 140, 255, 255}};
            for (int ch = 0; ch < 3; ++ch) {
                DrawText(chLabels[ch], px, editY - 1, 11, chCols[ch]);
                Rectangle sl = {(float)(px + 16), (float)editY, (float)(listW - 60), 10};
                DrawRectangleRounded(sl, 0.3f, 4, Color{50, 50, 60, 255});
                float pct = chVals[ch] / 255.0f;
                Rectangle fill = {sl.x + 2, sl.y + 2, (sl.width - 4) * pct, sl.height - 4};
                if (fill.width > 2) DrawRectangleRounded(fill, 0.3f, 4, chCols[ch]);
                DrawText(TextFormat("%d", chVals[ch]), (int)(sl.x + sl.width + 6), (int)editY - 1, 10, WHITE);
                editY += 18;
            }
        }
        editY += 4;

        // ── Compass sliders ──
        editY += 6;
        if (editY + 120 < maxEditY) {
            DrawText(T("Compass Options:"), px, editY, 12, LIGHTGRAY); editY += 16;
            int sliderW = listW - 10;
            // Economic
            DrawText(T("Economic:"), px, editY, 11, DARKGRAY);
            float econPct = (c.compassEconomic + 100.0f) / 200.0f;
            econPct = std::max(0.0f, std::min(1.0f, econPct));
            Rectangle econSlide = {(float)px, (float)(editY + 14), (float)sliderW, 10};
            DrawRectangleRounded(econSlide, 0.3f, 4, Color{50,50,60,255});
            Rectangle econFill = {econSlide.x + 2, econSlide.y + 2, (econSlide.width - 4) * econPct, econSlide.height - 4};
            if (econFill.width > 2) DrawRectangleRounded(econFill, 0.3f, 4, ACCENT);
            DrawText(TextFormat("%+.0f", c.compassEconomic), (int)(px + sliderW + 6), (int)editY + 12, 11, WHITE);
            DrawText(T("Left"), px + 2, editY + 26, 9, GRAY);
            DrawText(T("Right"), (int)(px + sliderW - 30), editY + 26, 9, GRAY);
            editY += 42;

            // Social
            DrawText(T("Social:"), px, editY, 11, DARKGRAY);
            float socPct = (c.compassSocial + 100.0f) / 200.0f;
            socPct = std::max(0.0f, std::min(1.0f, socPct));
            Rectangle socSlide = {(float)px, (float)(editY + 14), (float)sliderW, 10};
            DrawRectangleRounded(socSlide, 0.3f, 4, Color{50,50,60,255});
            Rectangle socFill = {socSlide.x + 2, socSlide.y + 2, (socSlide.width - 4) * socPct, socSlide.height - 4};
            if (socFill.width > 2) DrawRectangleRounded(socFill, 0.3f, 4, ACCENT);
            DrawText(TextFormat("%+.0f", c.compassSocial), (int)(px + sliderW + 6), (int)editY + 12, 11, WHITE);
            DrawText(T("Auth"), px + 2, editY + 26, 9, GRAY);
            DrawText(T("Lib"), (int)(px + sliderW - 20), editY + 26, 9, GRAY);
            editY += 42;

            // ── Research & policy summary + set-mode buttons ──
            editY += 6;
            int halfW = (listW - 8) / 2;
            Rectangle resBtn = {(float)px, (float)editY, (float)halfW, 28};
            Rectangle docBtn = {(float)(px + halfW + 8), (float)editY, (float)halfW, 28};
            drawButton("Set Research", resBtn, false, 12);
            drawButton("Set Policies", docBtn, false, 12);
            editY += 34;
            int polCount = (int)m_countryPolicies.count(m_selectedCountry) ? (int)m_countryPolicies[m_selectedCountry].size() : 0;
            const char* docStr = polCount == 0 ? "Policies: (none)" : TextFormat(T("Policies: %d selected"), polCount);
            DrawText(docStr, px, editY, 11, polCount == 0 ? GRAY : LIGHTGRAY); editY += 15;
            const char* resStr = c.research.empty()
                ? "Research: (auto-unlocked)"
                : TextFormat(T("Research: %d techs set"), (int)c.research.size());
            DrawText(resStr, px, editY, 11, c.research.empty() ? GRAY : LIGHTGRAY);
        }
    }
}

// ════════════════════════════════════════════════════════════════
//  Research/policy "set mode" overlay
//  Mirrors the in-game research tree (Game::drawResearchTab) but clicking a
//  node toggles it as pre-researched for the selected country.
// ════════════════════════════════════════════════════════════════

void MapEditor::openSetMode(int cid, bool policyMode) {
    auto& all = m_editCountries.getAll();
    auto it = all.find(cid);
    if (it == all.end()) return;
    m_setModeCountry = cid;
    m_setModeOpen = true;
    m_setModePolicyMode = policyMode;
    m_setModeTab = 0;
    m_setModeZoom = 1.0f;
    m_setModeCamX = 0; m_setModeCamY = 60;
    m_setModeDragging = false;
    m_setModeHoveredNode = -1;
    if (policyMode) {
        loadEditorPolicies();
        m_policyScroll = 0;
    }
    m_setModeIsEthnicity = false;
    // Working copy with the country's research marked as researched
    m_setModeNodes = m_researchNodes;
    const auto& res = it->second.research;
    for (auto& n : m_setModeNodes)
        n.researched = std::find(res.begin(), res.end(), n.id) != res.end();
}

void MapEditor::openSetModeEthnicity(const std::string& name, const std::string& iso, int cid,
                                     int initialTab, bool fromList) {
    m_setModeIsEthnicity = true;
    m_setModeEthnicity = name;
    // m_ethnicRelations is keyed by ISO (that's the shape the game's
    // starting_minority_policies.json uses). Countries created by hand in the
    // editor start with an empty ISO, so anything set for them would be stored
    // under "" and then dropped at export. Assign codes up front instead.
    std::string useIso = iso;
    if (useIso.empty() && cid >= 0) {
        ensureIsoCodes();
        if (const Country* c = m_editCountries.getCountry(cid)) useIso = c->isoA3;
    }
    m_setModeEthnicityIso = useIso;
    m_setModeEthnicityCid = cid;
    m_setModeEthnicTab = initialTab;
    m_setModeEthnicFromList = fromList;
    m_setModeOpen = true;
    m_setModePolicyMode = true; // ethnicities only get the policy screen, no research tree
    m_setModeCountry = -1;
    loadEditorPolicies();
    m_policyScroll = 0;
}

void MapEditor::closeSetModeEthnicity() {
    m_setModeOpen = false;
    if (m_setModeEthnicFromList && m_setModeEthnicityCid >= 0) {
        // Came from the country's ethnic list — go back there rather than
        // dumping the user all the way out to the map.
        openCountryEthnicList(m_setModeEthnicityCid);
    }
    m_setModeEthnicFromList = false;
}

std::vector<std::string> MapEditor::minoritiesOfCountry(int cid) const {
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    for (auto& [pid, prov] : m_editProvinces.getAllProvinces()) {
        if (prov.countryId != cid) continue;
        auto it = m_provinceData.find(pid);
        if (it == m_provinceData.end()) continue;
        for (auto& [name, pct] : it->second.ethnicGroups)
            if (seen.insert(name).second) names.push_back(name);
    }
    return names;
}

// Fixed 6-category / 3-option ethnic relations model, mirroring
// Game::initEthnicPolicyCategories() exactly so an editor-authored map's
// starting_minority_policies.json means the same thing in-game.
static const std::vector<EthnicPolicyCategory>& ethnicRelationCategories() {
    static const std::vector<EthnicPolicyCategory> cats = {
        {"deportation", "Deportation Policy", {
            {"Harsh",   "Force relocation. -2.5% align/turn, -2% pop/turn, shifts right", -2.5f, -2.0f, 0.0f, 1.0f, 0.0f, false},
            {"Medium",  "Status quo. No alignment or population changes.", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
            {"Light",   "Encourage immigration. +1.5% align/turn, +1.5% pop/turn", 1.5f, 1.5f, 0.0f, 0.0f, 0.0f, false},
        }},
        {"economic", "Economic Incentives", {
            {"Big Incentives",  "3 cost/turn. +5% align/turn, provinces shift toward gov compass", 5.0f, 0.0f, 3.0f, 0.3f, 0.0f, false},
            {"Some Incentives", "1 cost/turn. +2.5% align/turn, slight compass shift", 2.5f, 0.0f, 1.0f, 0.1f, 0.0f, false},
            {"No Incentives",   "Free. No alignment or compass changes.", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
        }},
        {"cultural", "Cultural Autonomy", {
            {"Full Autonomy",    "Free. +3% align/turn, +1% pop/turn", 3.0f, 1.0f, 0.0f, 0.0f, 0.0f, false},
            {"Partial Autonomy", "Free. +1% align/turn, no pop change", 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
            {"Suppression",      "Free. -3% align/turn, -1% pop/turn, shifts auth", -3.0f, -1.0f, 0.0f, 0.0f, 0.5f, false},
        }},
        {"political", "Political Representation", {
            {"Reserved Seats",   "2 cost/turn. +4% align/turn, minority representation", 4.0f, 0.0f, 2.0f, 0.0f, 0.0f, false},
            {"Standard Rights",  "Free. +1% align/turn, equal legal rights", 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
            {"Disenfranchised",  "Free. -4% align/turn, shifts auth. No political voice", -4.0f, 0.0f, 0.0f, 0.0f, 1.0f, false},
        }},
        {"language", "Language Policy", {
            {"Official Recognition", "0.5 cost/turn. +2% align/turn, minority language official", 2.0f, 0.0f, 0.5f, 0.0f, 0.0f, false},
            {"Tolerance",            "Free. +1% align/turn, minority language tolerated", 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
            {"Ban",                  "Free. -5% align/turn, -2% pop/turn, shifts right", -5.0f, -2.0f, 0.0f, 0.5f, 0.0f, false},
        }},
        {"integration", "Integration Programs", {
            {"Active Programs", "2 cost/turn. +3% align/turn, active cultural exchange", 3.0f, 0.0f, 2.0f, 0.0f, 0.0f, false},
            {"Passive Programs","1 cost/turn. +1% align/turn, basic integration", 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, false},
            {"None",            "Free. No alignment or population effects.", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, true},
        }},
    };
    return cats;
}

void MapEditor::drawEthnicRelationsScreen(const std::string& iso, const std::string& ethnicity, int topY) {
    Vector2 mouse = GetMousePosition();
    const int listX = 16, listY = topY, listW = m_screenW - 32;
    const int bottomH = 40;
    const int viewH = m_screenH - listY - bottomH - 8;

    auto& cats = ethnicRelationCategories();
    auto& selected = m_ethnicRelations[iso][ethnicity];
    if (selected.size() != cats.size()) {
        selected.assign(cats.size(), 0);
        for (size_t ci = 0; ci < cats.size(); ++ci)
            for (size_t oi = 0; oi < cats[ci].options.size(); ++oi)
                if (cats[ci].options[oi].isDefault) { selected[ci] = (int)oi; break; }
    }

    Rectangle viewRect = {(float)listX, (float)listY + 30, (float)listW, (float)(viewH - 30)};
    if (CheckCollisionPointRec(mouse, viewRect))
        m_policyScroll -= (int)(GetMouseWheelMove() * 24);

    BeginScissorMode(listX, (int)viewRect.y, listW, (int)viewRect.height);
    int cy = (int)viewRect.y - m_policyScroll;
    for (size_t ci = 0; ci < cats.size(); ++ci) {
        auto& cat = cats[ci];
        DrawText(cat.displayName.c_str(), listX, cy, 16, ACCENT);
        cy += 22;
        for (size_t oi = 0; oi < cat.options.size(); ++oi) {
            auto& opt = cat.options[oi];
            bool isSel = selected[ci] == (int)oi;
            Rectangle row = {(float)(listX + 8), (float)cy, (float)(listW - 16), 46};
            bool rowHov = CheckCollisionPointRec(mouse, row);
            Color bg = isSel ? ColorAlpha(ACCENT, 0.16f) : (rowHov ? Color{34,34,46,255} : Color{24,24,32,255});
            DrawRectangleRounded(row, 0.08f, 4, bg);
            DrawRectangleRoundedLines(row, 0.08f, 4, isSel ? ACCENT : Color{55,55,68,255});
            Rectangle dot = {row.x + 8, row.y + 15, 16, 16};
            DrawRectangleRoundedLines(dot, 0.5f, 6, isSel ? ACCENT : Color{100,100,110,255});
            if (isSel) DrawRectangleRounded({dot.x+3,dot.y+3,10,10}, 0.5f, 6, ACCENT);
            DrawText(opt.name.c_str(), (int)row.x + 32, (int)row.y + 6, 14, WHITE);
            DrawText(opt.desc.c_str(), (int)row.x + 32, (int)row.y + 24, 11, Color{190,190,200,255});
            if (rowHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) { selected[ci] = (int)oi; trackChange(); }
            cy += 50;
        }
        cy += 8;
    }
    EndScissorMode();

    int barY = m_screenH - bottomH;
    DrawRectangle(0, barY, m_screenW, bottomH, {10, 10, 15, 220});
    DrawText(TextFormat(T("Government relations toward %s (as %s)"), ethnicity.c_str(), iso.empty() ? "?" : iso.c_str()),
             16, barY + 12, 13, Color{160, 160, 180, 220});
    DrawText(T("Pick one option per category — mirrors the in-game Ethnic tab."),
             16, barY + 26, 10, Color{140, 140, 160, 200});
}

void MapEditor::openCountryEthnicList(int cid) {
    m_countryEthnicListOpen = true;
    m_countryEthnicListCid = cid;
    m_countryEthnicListScroll = 0;
}

// Country-panel overview: every minority found anywhere in this country's
// provinces, one row each, click to open its Policies/Ethnic Relations
// overlay — so defining how the country treats every minority doesn't
// require hunting through provinces one at a time.
void MapEditor::drawCountryEthnicListOverlay() {
    if (!m_countryEthnicListOpen) return;
    const Country* c = m_editCountries.getCountry(m_countryEthnicListCid);
    if (!c) { m_countryEthnicListOpen = false; return; }

    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 210});
    Vector2 mouse = GetMousePosition();

    Rectangle closeBtn = {(float)(m_screenW - 44), 8, 36, 36};
    DrawRectangleRounded(closeBtn, 0.2f, 6, {60, 60, 70, 180});
    DrawRectangleRoundedLines(closeBtn, 0.2f, 6, {180, 180, 180, 200});
    int xw = MeasureText("X", 20);
    DrawText("X", (int)(closeBtn.x + closeBtn.width/2 - xw/2), 12, 20, {180, 180, 180, 200});
    if (CheckCollisionPointRec(mouse, closeBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_countryEthnicListOpen = false;
        return;
    }
    DrawText(T("ESC to close"), m_screenW - 140, 55, 14, Color{120, 120, 140, 150});
    DrawText(TextFormat(T("%s — Ethnic Relations"), c->name.c_str()), 16, 16, 20, ACCENT);

    auto names = minoritiesOfCountry(m_countryEthnicListCid);
    // Aggregate each minority's total pct across the country's provinces
    std::unordered_map<std::string, float> pctByName;
    for (auto& [pid, prov] : m_editProvinces.getAllProvinces()) {
        if (prov.countryId != m_countryEthnicListCid) continue;
        auto it = m_provinceData.find(pid);
        if (it == m_provinceData.end()) continue;
        for (auto& [n, pct] : it->second.ethnicGroups) pctByName[n] += pct;
    }
    std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
        return pctByName[a] > pctByName[b];
    });

    const int listX = 16, listY = 56, listW = m_screenW - 32;
    const int viewH = m_screenH - listY - 20;
    const int rowH = 34;
    Rectangle viewRect = {(float)listX, (float)listY, (float)listW, (float)viewH};
    if (CheckCollisionPointRec(mouse, viewRect))
        m_countryEthnicListScroll -= (int)(GetMouseWheelMove() * 24);
    int maxScroll = std::max(0, (int)names.size() * rowH - viewH);
    m_countryEthnicListScroll = std::max(0, std::min(m_countryEthnicListScroll, maxScroll));

    BeginScissorMode(listX, listY, listW, viewH);
    int cy = listY - m_countryEthnicListScroll;
    if (names.empty()) {
        DrawText(T("No minorities recorded for this country yet."), listX, cy, 14, GRAY);
    }
    for (auto& name : names) {
        Rectangle row = {(float)listX, (float)cy, (float)listW, (float)(rowH - 4)};
        bool hov = CheckCollisionPointRec(mouse, row);
        Color swatch = m_ethnicColors.count(name) ? m_ethnicColors[name] : Color{150,150,150,255};
        DrawRectangleRounded(row, 0.08f, 4, hov ? Color{34,34,46,255} : Color{24,24,32,255});
        DrawRectangle((int)row.x + 8, (int)row.y + 9, 12, 12, swatch);
        DrawText(od::i18n::properName(name).c_str(), (int)row.x + 28, (int)row.y + 6, 15, hov ? ACCENT : WHITE);
        DrawText(TextFormat(T("%.0f%% of country pop."), pctByName[name]), (int)row.x + listW - 180, (int)row.y + 8, 12, LIGHTGRAY);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            // This list *is* the ethnic-relations screen, so open straight on
            // that tab (tab 1) rather than dropping the user into starting
            // policies, and remember to come back here when it closes.
            openSetModeEthnicity(name, c->isoA3, m_countryEthnicListCid, 1, true);
            m_countryEthnicListOpen = false;
        }
        cy += rowH;
    }
    EndScissorMode();
}

void MapEditor::loadEditorPolicies() {
    if (m_editorPoliciesLoaded) return;
    m_editorPoliciesLoaded = true;
    m_editorPolicies.clear();
    std::ifstream f(m_dataDir + "policies.json");
    if (!f) { LoadLog() << "  policies.json not found — policy screen will be empty\n"; return; }
    try {
        nlohmann::json j; f >> j;
        for (auto& p : j["policies"]) {
            Policy policy;
            policy.id = p.value("id", "");
            policy.name = p.value("name", "");
            policy.category = p.value("category", "");
            policy.folder = p.value("folder", "");
            policy.description = p.value("description", "");
            policy.costPerTurn = p.value("cost_per_turn", 0);
            policy.implementationTurns = p.value("implementation_turns", 3);
            policy.propagandaDuration = p.value("propaganda_duration", 0);
            policy.econShift = p.value("compass_shift", nlohmann::json::object()).value("economic", 0.0f);
            policy.socShift = p.value("compass_shift", nlohmann::json::object()).value("social", 0.0f);
            policy.minEcon = p.value("requirements", nlohmann::json::object()).value("min_economic", -100.0f);
            policy.maxEcon = p.value("requirements", nlohmann::json::object()).value("max_economic", 100.0f);
            policy.minSoc = p.value("requirements", nlohmann::json::object()).value("min_social", -100.0f);
            policy.maxSoc = p.value("requirements", nlohmann::json::object()).value("max_social", 100.0f);
            if (p.contains("incompatible_with"))
                for (auto& inc : p["incompatible_with"]) policy.incompatibleWith.push_back(inc.get<std::string>());
            if (p.contains("tradeoffs")) {
                auto& t = p["tradeoffs"];
                if (t.contains("gains")) for (auto& g : t["gains"]) policy.tradeoffs.gains.push_back(g.get<std::string>());
                if (t.contains("costs")) for (auto& c2 : t["costs"]) policy.tradeoffs.costs.push_back(c2.get<std::string>());
            }
            m_editorPolicies.push_back(policy);
        }
        LoadLog() << "  Loaded " << m_editorPolicies.size() << " policies for the editor\n";
    } catch (const std::exception& e) {
        LoadLog() << "  Failed to parse policies.json: " << e.what() << "\n";
    }
}

void MapEditor::togglePolicyInList(std::vector<std::string>& list, const std::string& policyId) {
    auto pos = std::find(list.begin(), list.end(), policyId);
    if (pos != list.end()) {
        list.erase(pos);
        trackChange();
        return;
    }
    // Enforce incompatibility (mirrors the in-game policy screen's rule)
    const Policy* p = nullptr;
    for (auto& pol : m_editorPolicies) if (pol.id == policyId) { p = &pol; break; }
    if (p) {
        for (auto it = list.begin(); it != list.end();) {
            bool conflicts = std::find(p->incompatibleWith.begin(), p->incompatibleWith.end(), *it) != p->incompatibleWith.end();
            if (!conflicts) {
                for (auto& pol : m_editorPolicies)
                    if (pol.id == *it && std::find(pol.incompatibleWith.begin(), pol.incompatibleWith.end(), policyId) != pol.incompatibleWith.end())
                        conflicts = true;
            }
            if (conflicts) it = list.erase(it); else ++it;
        }
    }
    list.push_back(policyId);
    trackChange();
}

void MapEditor::togglePolicyForCountry(int cid, const std::string& policyId) {
    togglePolicyInList(m_countryPolicies[cid], policyId);
}

void MapEditor::togglePolicyForEthnicity(const std::string& name, const std::string& policyId) {
    togglePolicyInList(m_ethnicityPolicies[name], policyId);
}

std::string MapEditor::buildPoliciesJson() const {
    // Pass the loaded master list straight through unchanged — the editor
    // doesn't currently support authoring new policies, only assigning them.
    std::ifstream f(m_dataDir + "policies.json");
    if (!f) return std::string();
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::string MapEditor::buildStartingPoliciesJson() const {
    nlohmann::json root;
    nlohmann::json sp = nlohmann::json::object();
    for (auto& [cid, ids] : m_countryPolicies) {
        if (ids.empty()) continue;
        const Country* c = m_editCountries.getCountry(cid);
        if (!c || c->isoA3.empty()) continue;
        nlohmann::json arr = nlohmann::json::array();
        for (auto& id : ids) arr.push_back(id);
        sp[c->isoA3] = arr;
    }
    if (sp.empty()) return std::string();
    root["starting_policies"] = sp;
    return root.dump(2);
}

// {iso: {ethnicityName: [optIdx x6]}} — same schema Game::loadFromODM parses
// into m_startingMinorityPolicies for the in-game Ethnic tab.
std::string MapEditor::buildStartingMinorityPoliciesJson() const {
    nlohmann::json root = nlohmann::json::object();
    for (auto& [iso, byName] : m_ethnicRelations) {
        if (iso.empty()) continue;
        nlohmann::json perIso = nlohmann::json::object();
        for (auto& [name, opts] : byName) {
            if (opts.empty()) continue;
            perIso[name] = opts;
        }
        if (!perIso.empty()) root[iso] = perIso;
    }
    if (root.empty()) return std::string();
    return root.dump(2);
}

void MapEditor::applySetModeResearch() {
    auto& all = m_editCountries.getAll();
    auto it = all.find(m_setModeCountry);
    if (it == all.end()) return;
    it->second.research.clear();
    for (auto& n : m_setModeNodes)
        if (n.researched) it->second.research.push_back(n.id);
    m_dirty = true;
}

void MapEditor::toggleSetModeNode(int idx) {
    auto& nodes = m_setModeNodes;
    auto indexOf = [&](const std::string& id) -> int {
        for (int i = 0; i < (int)nodes.size(); ++i) if (nodes[i].id == id) return i;
        return -1;
    };
    // Mutually recursive enable/disable with dep + mutex cascades
    std::function<void(int)> enable, disable;
    disable = [&](int i) {
        if (i < 0 || !nodes[i].researched) return;
        nodes[i].researched = false;
        // Cascade: disable dependents that lost their prerequisites
        for (int j = 0; j < (int)nodes.size(); ++j) {
            if (!nodes[j].researched) continue;
            bool depends = false;
            for (auto& d : nodes[j].deps) if (d == nodes[i].id) { depends = true; break; }
            if (!depends) continue;
            if (nodes[j].depsAny) {
                bool stillOk = false;
                for (auto& d : nodes[j].deps) {
                    int di = indexOf(d);
                    if (di >= 0 && nodes[di].researched) { stillOk = true; break; }
                }
                if (stillOk) continue;
            }
            disable(j);
        }
    };
    enable = [&](int i) {
        if (i < 0 || nodes[i].researched) return;
        // Mutex: clear other researched members of the same group first
        if (nodes[i].mutexGroup > 0)
            for (int j = 0; j < (int)nodes.size(); ++j)
                if (j != i && nodes[j].mutexGroup == nodes[i].mutexGroup && nodes[j].researched)
                    disable(j);
        nodes[i].researched = true;
        if (nodes[i].depsAny) {
            bool any = nodes[i].deps.empty();
            for (auto& d : nodes[i].deps) {
                int di = indexOf(d);
                if (di >= 0 && nodes[di].researched) { any = true; break; }
            }
            if (!any) enable(indexOf(nodes[i].deps[0]));
        } else {
            for (auto& d : nodes[i].deps) enable(indexOf(d));
        }
    };
    if (nodes[idx].researched) disable(idx);
    else enable(idx);
    applySetModeResearch();
}

void MapEditor::drawSetModeOverlay() {
    if (!m_setModeOpen) return;

    if (m_setModeIsEthnicity) {
        DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 210});
        Vector2 emouse = GetMousePosition();
        Rectangle eCloseBtn = {(float)(m_screenW - 44), 8, 36, 36};
        DrawRectangleRounded(eCloseBtn, 0.2f, 6, {60, 60, 70, 180});
        DrawRectangleRoundedLines(eCloseBtn, 0.2f, 6, {180, 180, 180, 200});
        int exw = MeasureText("X", 20);
        DrawText("X", (int)(eCloseBtn.x + eCloseBtn.width/2 - exw/2), 12, 20, {180, 180, 180, 200});
        if (CheckCollisionPointRec(emouse, eCloseBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            closeSetModeEthnicity();
            return;
        }
        DrawText(m_setModeEthnicFromList ? "ESC to go back" : "ESC to close",
                 m_screenW - 140, 55, 14, Color{120, 120, 140, 150});

        // Explicit back-to-list control, so changing one minority's settings
        // doesn't strand you outside the country's ethnic list.
        int headerX = 16;
        if (m_setModeEthnicFromList) {
            Rectangle backBtn = {16, 44, 90, 24};
            bool backHov = CheckCollisionPointRec(emouse, backBtn);
            DrawRectangleRounded(backBtn, 0.15f, 6, backHov ? Color{60,60,80,220} : Color{35,35,48,200});
            DrawRectangleRoundedLines(backBtn, 0.15f, 6, ACCENT);
            DrawText(T("< Back"), 30, 50, 14, ACCENT);
            if (backHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                closeSetModeEthnicity();
                return;
            }
            headerX = 116;
        }
        DrawText(TextFormat(T("%s (ethnicity)"),
                            od::i18n::properName(m_setModeEthnicity).c_str()), headerX, 48, 16, ACCENT);

        // Tabs: starting policies vs. the in-game-parity ethnic relations screen
        static const char* ethTabNames[] = {"Starting Policies", "Ethnic Relations"};
        int ethTabY = 8, ethTabH = 30, ethTabX = 16, ethTabSpacing = 190;
        for (int t = 0; t < 2; ++t) {
            int tx = ethTabX + t * ethTabSpacing;
            Rectangle tr = {(float)tx, (float)ethTabY, (float)(ethTabSpacing - 8), (float)ethTabH};
            bool active = (t == m_setModeEthnicTab);
            bool hovered = CheckCollisionPointRec(emouse, tr);
            Color bg = active ? Color{60, 60, 80, 200} : (hovered ? Color{40, 40, 60, 180} : Color{30, 30, 50, 150});
            DrawRectangleRounded(tr, 0.1f, 6, bg);
            if (active) DrawRectangleRoundedLines(tr, 0.1f, 6, ACCENT);
            int tw = MeasureText(ethTabNames[t], 14);
            DrawText(ethTabNames[t], tx + (ethTabSpacing - 8 - tw) / 2, ethTabY + 8, 14, active ? ACCENT : LIGHTGRAY);
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && hovered) m_setModeEthnicTab = t;
        }

        // Copy this tab's current choice to every other minority the owning
        // country has — the country-wide "how do we treat everyone" button.
        if (m_setModeEthnicityCid >= 0) {
            Rectangle bulkBtn = {(float)(m_screenW - 280), (float)ethTabY, 260, (float)ethTabH};
            bool bulkHov = CheckCollisionPointRec(emouse, bulkBtn);
            DrawRectangleRounded(bulkBtn, 0.1f, 6, bulkHov ? Color{70, 55, 20, 220} : Color{40, 35, 25, 180});
            DrawRectangleRoundedLines(bulkBtn, 0.1f, 6, ACCENT);
            const char* bulkLabel = "Apply to all minorities in country";
            int blw = MeasureText(bulkLabel, 12);
            DrawText(bulkLabel, (int)(bulkBtn.x + bulkBtn.width / 2 - blw / 2), (int)bulkBtn.y + 9, 12, ACCENT);
            if (bulkHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                auto names = minoritiesOfCountry(m_setModeEthnicityCid);
                if (m_setModeEthnicTab == 0) {
                    auto src = m_ethnicityPolicies[m_setModeEthnicity];
                    for (auto& n : names) if (n != m_setModeEthnicity) m_ethnicityPolicies[n] = src;
                } else {
                    auto src = m_ethnicRelations[m_setModeEthnicityIso][m_setModeEthnicity];
                    for (auto& n : names) if (n != m_setModeEthnicity) m_ethnicRelations[m_setModeEthnicityIso][n] = src;
                }
                trackChange();
                m_warningMsg = "Applied to " + std::to_string(names.size()) + " minorit" + (names.size() == 1 ? "y" : "ies");
                m_warningTimer = 2.0f;
            }
        }

        if (m_setModeEthnicTab == 0) drawPolicyScreenEthnicity(m_setModeEthnicity, 46);
        else drawEthnicRelationsScreen(m_setModeEthnicityIso, m_setModeEthnicity, 46);
        return;
    }

    auto& all = m_editCountries.getAll();
    auto it = all.find(m_setModeCountry);
    if (it == all.end()) { m_setModeOpen = false; return; }
    Country& c = it->second;

    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 210});
    Vector2 mouse = GetMousePosition();

    // ── Close button ──
    Rectangle closeBtn = {(float)(m_screenW - 44), 8, 36, 36};
    DrawRectangleRounded(closeBtn, 0.2f, 6, {60, 60, 70, 180});
    DrawRectangleRoundedLines(closeBtn, 0.2f, 6, {180, 180, 180, 200});
    int xw = MeasureText("X", 20);
    DrawText("X", (int)(closeBtn.x + closeBtn.width/2 - xw/2), 12, 20, {180, 180, 180, 200});
    if (CheckCollisionPointRec(mouse, closeBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_setModeOpen = false;
        return;
    }
    DrawText(T("ESC to close"), m_screenW - 140, 55, 14, Color{120, 120, 140, 150});

    // ── Header: country + hint ──
    if (m_setModePolicyMode) {
        DrawText(TextFormat(T("SET POLICIES — %s"), c.name.c_str()), 16, 16, 20, ACCENT);
    } else {
        DrawText(TextFormat(T("SET RESEARCH — %s"), c.name.c_str()), 16, 48, 16, ACCENT);
        DrawText(T("Click a node to toggle it as pre-researched (deps auto-set)"), 16, 68, 12, Color{160, 160, 180, 200});
    }

    // ── Tabs: 4 research categories (research view only) ──
    const char* catNames[] = {"Buildings", "Army", "Population", "Misc"};
    const char* catKeys[] = {"buildings", "army", "population", "misc"};
    int catCount = 4;
    int catTabY = 8, catTabH = 30, catTabStartX = 16, catSpacing = 170;
    if (!m_setModePolicyMode) {
        for (int t = 0; t < catCount; ++t) {
            int tx = catTabStartX + t * catSpacing;
            Rectangle cr = {(float)tx, (float)catTabY, (float)(catSpacing - 8), (float)catTabH};
            bool active = (t == m_setModeTab);
            bool hovered = CheckCollisionPointRec(mouse, cr);
            Color bg = active ? Color{60, 60, 80, 200} : (hovered ? Color{40, 40, 60, 180} : Color{30, 30, 50, 150});
            DrawRectangleRounded(cr, 0.1f, 6, bg);
            if (active) DrawRectangleRoundedLines(cr, 0.1f, 6, ACCENT);
            int tw = MeasureText(catNames[t], 16);
            DrawText(catNames[t], tx + (catSpacing - 8 - tw) / 2, catTabY + 6, 16, active ? ACCENT : LIGHTGRAY);
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && hovered) m_setModeTab = t;
        }
    }

    // ── Policy screen: the real in-game policy layout, adapted to assign
    //    a country's STARTING policies instead of gradually enacting them ──
    if (m_setModePolicyMode) {
        drawPolicyScreen(m_setModeCountry);
        return;
    }

    // ── Pan/zoom (below the tab bar) ──
    bool overCatTab = mouse.y < catTabY + catTabH;
    if (!overCatTab && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (!m_setModeDragging) { m_setModeDragging = true; m_setModeDragPrevX = (int)mouse.x; m_setModeDragPrevY = (int)mouse.y; }
        m_setModeCamX += (int)mouse.x - m_setModeDragPrevX;
        m_setModeCamY += (int)mouse.y - m_setModeDragPrevY;
        m_setModeDragPrevX = (int)mouse.x; m_setModeDragPrevY = (int)mouse.y;
    } else { m_setModeDragging = false; }

    float wheel = GetMouseWheelMove();
    if (wheel != 0 && !overCatTab) {
        float oldZoom = m_setModeZoom;
        m_setModeZoom *= (wheel > 0) ? 1.2f : 0.833f;
        m_setModeZoom = std::max(0.5f, std::min(3.0f, m_setModeZoom));
        float factor = m_setModeZoom / oldZoom;
        m_setModeCamX = mouse.x - factor * (mouse.x - m_setModeCamX);
        m_setModeCamY = mouse.y - factor * (mouse.y - m_setModeCamY);
    }

    // ── Collect nodes for the active category ──
    std::vector<int> catIndices;
    float catMinX = 1e9f, catMaxX = -1e9f, catMinY = 1e9f, catMaxY = -1e9f;
    for (int i = 0; i < (int)m_setModeNodes.size(); i++) {
        if (m_setModeNodes[i].category == catKeys[m_setModeTab]) {
            catIndices.push_back(i);
            auto& n = m_setModeNodes[i];
            catMinX = std::min(catMinX, n.posX); catMaxX = std::max(catMaxX, n.posX);
            catMinY = std::min(catMinY, n.posY); catMaxY = std::max(catMaxY, n.posY);
        }
    }

    // ── Clamp camera ──
    float pad = 80.0f;
    float scaledNodeW = 160.0f * m_setModeZoom;
    float scaledNodeH = 50.0f * m_setModeZoom;
    if (catMinX < 1e8f) {
        float viewW = m_screenW / m_setModeZoom;
        float viewH = (m_screenH - 80) / m_setModeZoom;
        float minCamX = -(catMaxX + scaledNodeW + pad - viewW);
        float maxCamX = -catMinX + pad;
        float minCamY = -(catMaxY + scaledNodeH + pad - viewH);
        float maxCamY = -catMinY + pad;
        if (minCamX > maxCamX) { float avg = (minCamX + maxCamX) / 2; minCamX = avg; maxCamX = avg; }
        if (minCamY > maxCamY) { float avg = (minCamY + maxCamY) / 2; minCamY = avg; maxCamY = avg; }
        m_setModeCamX = std::max(minCamX, std::min(maxCamX, m_setModeCamX));
        m_setModeCamY = std::max(minCamY, std::min(maxCamY, m_setModeCamY));
    }

    if (catIndices.empty())
        DrawText(T("No research trees in this category"), m_screenW / 2 - 100, m_screenH / 2, 14, LIGHTGRAY);

    // ── Subcategory labels ──
    std::string lastSubcat;
    for (int idx : catIndices) {
        auto& node = m_setModeNodes[idx];
        if (node.subcategory != lastSubcat) {
            lastSubcat = node.subcategory;
            int lx = (int)(node.posX * m_setModeZoom + m_setModeCamX);
            int ly = (int)(node.posY * m_setModeZoom + m_setModeCamY);
            DrawText(node.subcategory.c_str(), lx, ly - (int)(26 * m_setModeZoom), (int)(14 * m_setModeZoom), {180, 180, 200, 200});
        }
    }

    int nodeW = (int)(160 * m_setModeZoom); if (nodeW < 40) nodeW = 40;
    int nodeH = (int)(50 * m_setModeZoom); if (nodeH < 14) nodeH = 14;

    // ── Dependency lines ──
    for (int idx : catIndices) {
        auto& node = m_setModeNodes[idx];
        int nx = (int)(node.posX * m_setModeZoom + m_setModeCamX);
        int ny = (int)(node.posY * m_setModeZoom + m_setModeCamY);
        for (const auto& req : node.deps) {
            int depMutexGroup = 0;
            for (auto& pn : m_setModeNodes) {
                if (pn.id == req) {
                    depMutexGroup = pn.mutexGroup;
                    int px = (int)(pn.posX * m_setModeZoom + m_setModeCamX);
                    int py = (int)(pn.posY * m_setModeZoom + m_setModeCamY);
                    Color lineCol = pn.researched ? Color{100, 200, 100, 120} : Color{100, 100, 100, 80};
                    DrawLine(px + nodeW / 2, py + nodeH, nx + nodeW / 2, ny, lineCol);
                    break;
                }
            }
            if (depMutexGroup > 0) {
                for (auto& sn : m_setModeNodes) {
                    if (sn.id == req || sn.mutexGroup != depMutexGroup) continue;
                    int sx = (int)(sn.posX * m_setModeZoom + m_setModeCamX);
                    int sy = (int)(sn.posY * m_setModeZoom + m_setModeCamY);
                    Color sCol = sn.researched ? Color{100, 200, 100, 80} : Color{80, 80, 80, 60};
                    DrawLine(sx + nodeW / 2, sy + nodeH, nx + nodeW / 2, ny, sCol);
                }
            }
        }
    }

    // ── Nodes ──
    m_setModeHoveredNode = -1;
    for (int idx : catIndices) {
        auto& node = m_setModeNodes[idx];
        int nx = (int)(node.posX * m_setModeZoom + m_setModeCamX);
        int ny = (int)(node.posY * m_setModeZoom + m_setModeCamY);
        Rectangle r = {(float)nx, (float)ny, (float)nodeW, (float)nodeH};

        Color bg, border;
        if (node.researched) { bg = {40, 120, 40, 220}; border = {80, 200, 80, 255}; }
        else if (node.isAvailable(m_setModeNodes)) { bg = {40, 40, 60, 220}; border = {120, 120, 180, 255}; }
        else { bg = {30, 30, 35, 180}; border = {60, 60, 70, 150}; }

        bool hovered = CheckCollisionPointRec(mouse, r) && !overCatTab;
        if (hovered) { m_setModeHoveredNode = idx; border = ACCENT; }

        DrawRectangleRounded(r, 0.15f, 8, bg);
        DrawRectangleRoundedLines(r, 0.15f, 8, border);
        int fs = (int)(12 * m_setModeZoom); if (fs < 7) fs = 7; if (fs > 20) fs = 20;
        int textW = MeasureText(node.name.c_str(), fs);
        DrawText(node.name.c_str(), nx + nodeW / 2 - textW / 2, ny + 4, fs, WHITE);
        int infoFs = (int)(8 * m_setModeZoom); if (infoFs < 6) infoFs = 6; if (infoFs > 14) infoFs = 14;
        if (node.researched)
            DrawText(T("SET"), nx + 4, ny + nodeH - infoFs - 6, infoFs, {100, 200, 100, 200});
        else
            DrawText(TextFormat(T("%d RP"), node.cost), nx + 4, ny + nodeH - infoFs - 6, infoFs, {160, 160, 160, 200});
    }

    // ── Hover tooltip ──
    if (m_setModeHoveredNode >= 0) {
        auto& node = m_setModeNodes[m_setModeHoveredNode];
        int tw0 = MeasureText(node.name.c_str(), 14);
        int dw = MeasureText(node.desc.c_str(), 11);
        int tipW = std::max(tw0, dw) + 20;
        int tipX = (int)mouse.x + 16; if (tipX + tipW > m_screenW) tipX = m_screenW - tipW - 8;
        int tipY = (int)mouse.y + 16;
        DrawRectangle(tipX, tipY, tipW, 60, {10, 10, 20, 220});
        DrawRectangleLines(tipX, tipY, tipW, 60, {100, 100, 140, 200});
        DrawText(node.name.c_str(), tipX + 10, tipY + 4, 14, WHITE);
        DrawText(node.desc.c_str(), tipX + 10, tipY + 22, 11, {200, 200, 200, 255});
        DrawText(node.researched ? "Click to unset (dependents unset too)"
                                 : "Click to set (prerequisites auto-set)",
                 tipX + 10, tipY + 40, 10, {160, 200, 160, 220});
    }

    // ── Click to toggle ──
    if (!m_setModeDragging && m_setModeHoveredNode >= 0 && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        toggleSetModeNode(m_setModeHoveredNode);
    }

    // ── Bottom info bar ──
    int barY = m_screenH - 36;
    DrawRectangle(0, barY, m_screenW, 36, {10, 10, 15, 220});
    int setCount = 0;
    for (auto& n : m_setModeNodes) if (n.researched) setCount++;
    DrawText(TextFormat(T("%d techs pre-researched"), setCount), 16, barY + 10, 14, ACCENT);
    DrawText(T("Note: infrastructure techs may also auto-unlock in game from built industry/forts/ports"),
             240, barY + 12, 11, Color{140, 140, 160, 200});
}

// Mirrors the in-game policy screen's "Available" tab layout (folder
// groupings, cost/shift/tradeoffs), but toggles whether the country STARTS
// with a policy already active instead of gradually enacting it.
void MapEditor::drawPolicyScreen(int cid) {
    drawPolicyScreenFor(m_countryPolicies[cid],
        [this, cid](const std::string& id) { togglePolicyForCountry(cid, id); });
}

void MapEditor::drawPolicyScreenEthnicity(const std::string& name, int topY) {
    drawPolicyScreenFor(m_ethnicityPolicies[name],
        [this, name](const std::string& id) { togglePolicyForEthnicity(name, id); }, topY);
}

void MapEditor::drawPolicyScreenFor(std::vector<std::string>& selected, const std::function<void(const std::string&)>& onToggle, int topY) {
    Vector2 mouse = GetMousePosition();
    const int listX = 16, listY = topY, listW = m_screenW - 32;
    const int bottomH = 40;
    const int viewH = m_screenH - listY - bottomH - 8;

    if (m_editorPolicies.empty()) {
        DrawText(T("No policies.json found — place one in data/policies.json"), listX, listY + 40, 16, GRAY);
        return;
    }

    static const char* FOLDER_ORDER[] = {"Left", "Right", "Authoritarian", "Libertarian", "Miscellaneous"};

    Rectangle viewRect = {(float)listX, (float)listY + 30, (float)listW, (float)(viewH - 30)};
    if (CheckCollisionPointRec(mouse, viewRect))
        m_policyScroll -= (int)(GetMouseWheelMove() * 24);

    BeginScissorMode(listX, (int)viewRect.y, listW, (int)viewRect.height);
    int cy = (int)viewRect.y - m_policyScroll;
    for (const char* folder : FOLDER_ORDER) {
        std::vector<const Policy*> inFolder;
        for (auto& p : m_editorPolicies) if (p.folder == folder) inFolder.push_back(&p);
        if (inFolder.empty()) continue;

        bool open = m_policyFoldersOpen.count(folder) > 0;
        Rectangle hdr = {(float)listX, (float)cy, (float)listW, 26};
        bool hdrHov = CheckCollisionPointRec(mouse, hdr);
        DrawRectangle(listX, cy, listW, 25, hdrHov ? Color{40,40,55,255} : Color{30,30,42,255});
        DrawText(TextFormat("%s %s (%d)", open ? "v" : ">", folder, (int)inFolder.size()), listX + 8, cy + 5, 15, WHITE);
        if (hdrHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (open) m_policyFoldersOpen.erase(folder); else m_policyFoldersOpen.insert(folder);
        }
        cy += 28;
        if (!open) continue;

        for (const Policy* p : inFolder) {
            bool isSel = std::find(selected.begin(), selected.end(), p->id) != selected.end();
            int rowH = 70;
            Rectangle row = {(float)(listX + 8), (float)cy, (float)(listW - 16), (float)(rowH - 4)};
            bool rowHov = CheckCollisionPointRec(mouse, row);
            Color bg = isSel ? ColorAlpha(ACCENT, 0.16f) : (rowHov ? Color{34,34,46,255} : Color{24,24,32,255});
            DrawRectangleRounded(row, 0.06f, 4, bg);
            DrawRectangleRoundedLines(row, 0.06f, 4, isSel ? ACCENT : Color{55,55,68,255});
            // Checkbox
            Rectangle box = {row.x + 8, row.y + 8, 16, 16};
            DrawRectangleRoundedLines(box, 0.2f, 4, isSel ? ACCENT : Color{100,100,110,255});
            if (isSel) DrawRectangleRounded({box.x+3,box.y+3,10,10}, 0.3f, 4, ACCENT);
            DrawText(p->name.c_str(), (int)row.x + 32, (int)row.y + 6, 15, WHITE);
            DrawText(p->description.c_str(), (int)row.x + 32, (int)row.y + 24, 11, Color{190,190,200,255});
            DrawText(TextFormat(T("Cost: %d/turn   Shift: econ %+.0f soc %+.0f"),
                                p->costPerTurn, p->econShift, p->socShift),
                     (int)row.x + 32, (int)row.y + 42, 10, Color{160,160,175,255});
            if (!p->incompatibleWith.empty()) {
                std::string inc = "Conflicts: ";
                for (size_t i = 0; i < p->incompatibleWith.size(); ++i) {
                    if (i) inc += ", ";
                    inc += p->incompatibleWith[i];
                }
                DrawText(inc.c_str(), (int)(row.x + row.width - MeasureText(inc.c_str(), 10) - 10),
                         (int)row.y + 6, 10, Color{200,140,140,255});
            }
            if (rowHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                onToggle(p->id);
            }
            cy += rowH;
        }
    }
    EndScissorMode();

    int barY = m_screenH - bottomH;
    DrawRectangle(0, barY, m_screenW, bottomH, {10, 10, 15, 220});
    DrawText(T("Click a policy to toggle it as a starting policy for this country."),
             16, barY + 12, 13, Color{160, 160, 180, 220});
    DrawText(T("Selecting a conflicting policy clears the ones it's incompatible with."),
             16, barY + 26, 10, Color{140, 140, 160, 200});
    // Counter lives down here rather than up top: the header row is shared
    // with the ethnicity tab bar, which it used to overlap.
    const char* cnt = TextFormat(T("%d selected"), (int)selected.size());
    DrawText(cnt, m_screenW - MeasureText(cnt, 14) - 20, barY + 13, 14, LIGHTGRAY);
}

// ── Relations Panel ─────────────────────────────────────────────

bool MapEditor::drawIntField(Rectangle r, long long& value, long long lo, long long hi,
                              bool& editing, std::string& editBuf, bool inputOk) {
    Vector2 mouse = GetMousePosition();
    bool hov = inputOk && CheckCollisionPointRec(mouse, r);
    Color bg = editing ? Color{35,35,50,255} : (hov ? Color{30,30,40,255} : Color{25,25,35,255});
    DrawRectangleRounded(r, 0.1f, 4, bg);
    DrawRectangleRoundedLines(r, 0.1f, 4, editing ? ACCENT : Color{60,60,70,255});
    const char* txt = editing ? editBuf.c_str() : TextFormat("%lld", value);
    DrawText(txt, (int)r.x + 6, (int)r.y + 4, 13, editing ? ACCENT : WHITE);
    if (editing) DrawText("|", (int)(r.x + 6 + MeasureText(txt, 13)), (int)r.y + 4, 13, ACCENT);
    if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !editing) {
        editing = true;
        editBuf = std::to_string(value);
    }
    bool committed = false;
    if (editing) {
        int key = GetCharPressed();
        while (key > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            if (key >= '0' && key <= '9' && editBuf.size() < 12) editBuf.push_back((char)key);
            key = GetCharPressed();
        }
        odTextEditKeys(editBuf, 256);
        bool commit = IsKeyPressed(KEY_ENTER);
        bool clickAway = inputOk && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, r);
        if (commit || clickAway) {
            long long v = editBuf.empty() ? 0 : atoll(editBuf.c_str());
            value = std::max(lo, std::min(hi, v));
            editing = false;
            committed = true;
        } else if (IsKeyPressed(KEY_ESCAPE)) {
            editing = false;
        }
    }
    return committed;
}

bool MapEditor::drawHealthBar(int px, int& y, int listW, const char* label, int& health, bool inputOk) {
    DrawText(label, px, y, 11, LIGHTGRAY);
    DrawText(TextFormat("%d", health), px + listW - 28, y, 11, WHITE);
    y += 14;
    Rectangle bar = {(float)px, (float)y, (float)listW, 12};
    DrawRectangleRounded(bar, 0.3f, 4, Color{50,50,60,255});
    float p = std::max(0.0f, std::min(1.0f, health / 100.0f));
    Color barCol = p > 0.5f ? Color{90,200,90,255} : (p > 0.25f ? Color{220,180,60,255} : Color{220,80,80,255});
    Rectangle fill = {bar.x + 2, bar.y + 2, (bar.width - 4) * p, bar.height - 4};
    if (fill.width > 2) DrawRectangleRounded(fill, 0.3f, 4, barCol);
    bool changed = false;
    if (inputOk && IsMouseButtonDown(MOUSE_LEFT_BUTTON) &&
        CheckCollisionPointRec(GetMousePosition(), {bar.x - 4, bar.y - 4, bar.width + 8, bar.height + 8})) {
        float mp = (GetMouseX() - bar.x) / bar.width;
        int nv = (int)(std::max(0.0f, std::min(1.0f, mp)) * 100.0f);
        if (nv != health) { health = nv; changed = true; }
    }
    y += 20;
    return changed;
}

int MapEditor::drawCountryList(int x, int y, int w, int rows, int& scroll, int selected, const std::string& filter) {
    auto& all = m_editCountries.getAll();
    std::string needle = filter;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
    std::vector<int> cids;
    for (auto& [cid, c] : all) {
        if (cid >= 65533) continue; // skip UNC/BLC/SPC reserved ids
        if (!needle.empty()) {
            std::string hay = c.name;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(needle) == std::string::npos) continue;
        }
        cids.push_back(cid);
    }
    std::sort(cids.begin(), cids.end());
    const int itemH = 18;
    Vector2 mouse = GetMousePosition();
    Rectangle listRect = {(float)x, (float)y, (float)w, (float)(rows * itemH)};
    if (!anyModalOpen() && CheckCollisionPointRec(mouse, listRect))
        scroll -= (int)GetMouseWheelMove();
    int maxScroll = std::max(0, (int)cids.size() - rows);
    scroll = std::max(0, std::min(scroll, maxScroll));

    // Keep the selected row in view (e.g. after picking a country on the map)
    if (selected >= 0) {
        int selIdx = -1;
        for (int i = 0; i < (int)cids.size(); ++i) if (cids[i] == selected) { selIdx = i; break; }
        if (selIdx >= 0) {
            if (selIdx < scroll) scroll = selIdx;
            if (selIdx >= scroll + rows) scroll = selIdx - rows + 1;
            scroll = std::max(0, std::min(scroll, maxScroll));
        }
    }

    DrawRectangle(x, y, w, rows * itemH, Color{22, 22, 28, 255});
    // Scrollbar
    if ((int)cids.size() > rows) {
        float viewH = (float)(rows * itemH);
        float thumbH = std::max(12.0f, viewH * rows / (float)cids.size());
        float thumbY = y + (viewH - thumbH) * (maxScroll > 0 ? (float)scroll / maxScroll : 0);
        DrawRectangle(x + w - 5, y, 5, (int)viewH, Color{45, 45, 55, 255});
        DrawRectangle(x + w - 5, (int)thumbY, 5, (int)thumbH, Color{100, 100, 120, 255});
    }

    int clicked = -1;
    for (int i = scroll; i < (int)cids.size() && i < scroll + rows; ++i) {
        int cid = cids[i];
        Country& c = all[cid];
        int yi = y + (i - scroll) * itemH;
        Rectangle row = {(float)x, (float)yi, (float)(w - 6), (float)(itemH - 1)};
        bool sel = (cid == selected);
        bool hov = !anyModalOpen() && CheckCollisionPointRec(mouse, row);
        Color bg = sel ? ColorAlpha(ACCENT, 0.15f) : (hov ? Color{255,255,255,10} : Color{0,0,0,0});
        DrawRectangle((int)row.x, (int)row.y, (int)row.width, (int)row.height, bg);
        DrawRectangle(x + 2, yi + 3, 10, 10, c.color);
        std::string label = c.name;
        if (MeasureText(label.c_str(), 12) > w - 26)
            label = label.substr(0, std::max(1, (w - 32) / 7)) + "..";
        DrawText(label.c_str(), x + 16, yi + 2, 12, sel ? ACCENT : WHITE);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) clicked = cid;
    }
    return clicked;
}

void MapEditor::updateRelationsPanel() {}
void MapEditor::drawRelationsPanel() {
    int px = m_screenW - m_panelW + 12;
    int listW = m_panelW - 24;
    int y = m_toolbarH + 16;
    DrawText(T("Relations Editor"), px, y, 18, ACCENT); y += 24;

    auto& all = m_editCountries.getAll();
    if (all.empty()) {
        DrawText(T("Generate a world first."), px, y, 14, GRAY);
        return;
    }
    bool inputOk = !anyModalOpen();

    DrawText(T("Click two countries on the map,"), px, y, 11, Color{150,180,150,220}); y += 13;
    DrawText(T("or pick them from the lists below."), px, y, 11, Color{150,180,150,220}); y += 15;
    DrawRectangle(px, y + 2, 8, 8, Color{255, 205, 60, 255});
    DrawText("A", px + 12, y, 11, Color{255, 205, 60, 255});
    DrawRectangle(px + 32, y + 2, 8, 8, Color{80, 220, 255, 255});
    DrawText("B", px + 44, y, 11, Color{80, 220, 255, 255});
    y += 18;

    Rectangle clearBtn = {(float)px, (float)y, (float)listW, 20};
    if (drawButton("Clear selection", clearBtn, false, 11) && inputOk) {
        m_relCountryA = -1; m_relCountryB = -1;
    }
    y += 26;

    int panelH = m_screenH - m_toolbarH - m_bottomH;
    int rows = std::max(4, (panelH - 330) / (2 * 18));

    DrawText(T("Country A:"), px, y, 12, LIGHTGRAY); y += 15;
    int clickedA = drawCountryList(px, y, listW, rows, m_relScrollA, m_relCountryA);
    if (inputOk && clickedA >= 0) { m_relCountryA = clickedA; if (m_relCountryB == clickedA) m_relCountryB = -1; }
    y += rows * 18 + 8;
    DrawText(T("Country B:"), px, y, 12, LIGHTGRAY); y += 15;
    int clickedB = drawCountryList(px, y, listW, rows, m_relScrollB, m_relCountryB);
    if (inputOk && clickedB >= 0 && clickedB != m_relCountryA) m_relCountryB = clickedB;
    y += rows * 18 + 12;

    if (m_relCountryA < 0 || m_relCountryB < 0 || m_relCountryA == m_relCountryB
        || !all.count(m_relCountryA) || !all.count(m_relCountryB)) {
        DrawText(T("Select two different countries"), px, y, 12, GRAY); y += 15;
        DrawText(T("(click the map or a list) to edit"), px, y, 12, GRAY); y += 15;
        DrawText(T("their relation."), px, y, 12, GRAY);
        return;
    }

    Country& ca = all[m_relCountryA];
    Country& cb = all[m_relCountryB];
    std::string header = ca.name + "  \x3c\x2d\x3e  " + cb.name;
    if (MeasureText(header.c_str(), 12) > listW)
        header = ca.name.substr(0, 12) + ".. <-> " + cb.name.substr(0, 12) + "..";
    DrawText(header.c_str(), px, y, 12, WHITE); y += 22;

    auto key = std::make_pair(std::min(m_relCountryA, m_relCountryB),
                              std::max(m_relCountryA, m_relCountryB));
    CountryRelation& rel = m_editorRelations[key];

    // Relations are mutually exclusive — a pair is in exactly one of these
    // states at a time (can't be both at war and under a non-aggression pact).
    enum RelState { REL_NEUTRAL, REL_WAR, REL_ALLIANCE, REL_NAP, REL_GUARANTEE };
    RelState cur = REL_NEUTRAL;
    if (rel.war) cur = REL_WAR;
    else if (rel.alliance) cur = REL_ALLIANCE;
    else if (rel.nonAggression) cur = REL_NAP;
    else if (rel.guarantee) cur = REL_GUARANTEE;

    struct Opt { const char* label; RelState state; };
    static const Opt opts[] = {
        {"Neutral", REL_NEUTRAL},
        {"War", REL_WAR},
        {"Alliance", REL_ALLIANCE},
        {"Non-Aggression Pact", REL_NAP},
        {"Guarantee", REL_GUARANTEE},
    };
    for (auto& o : opts) {
        Rectangle r = {(float)px, (float)y, (float)listW, 26};
        if (drawButton(o.label, r, cur == o.state, 13) && inputOk && cur != o.state) {
            rel.war = (o.state == REL_WAR);
            rel.alliance = (o.state == REL_ALLIANCE);
            rel.nonAggression = (o.state == REL_NAP);
            rel.guarantee = (o.state == REL_GUARANTEE);
            trackChange();
        }
        y += 30;
    }
    DrawText(T("Selecting one clears the others."), px, y + 4, 11, GRAY);
}

// ── Navy Panel ─────────────────────────────────────────────────

void MapEditor::updateNavyPanel() {}
void MapEditor::drawNavyPanel() {
    int px = m_screenW - m_panelW + 12;
    int listW = m_panelW - 24;
    int y = m_toolbarH + 16;
    DrawText(T("Navy Editor"), px, y, 18, ACCENT); y += 26;

    auto& all = m_editCountries.getAll();
    if (all.empty()) {
        DrawText(T("Generate a world first."), px, y, 14, GRAY);
        return;
    }
    bool inputOk = !anyModalOpen();
    int panelH = m_screenH - m_toolbarH - m_bottomH;

    // Search box filters the country list below. Char capture is gated behind
    // an explicit focus flag (click-to-focus, click-away/Escape to unfocus) —
    // otherwise it would drain GetCharPressed() every frame and steal input
    // from the health/troops fields below it.
    DrawText(T("Country:"), px, y, 12, LIGHTGRAY); y += 15;
    Rectangle searchRect = {(float)px, (float)y, (float)listW, 22};
    Vector2 navyMouse = GetMousePosition();
    bool searchHov = inputOk && CheckCollisionPointRec(navyMouse, searchRect);
    DrawRectangleRounded(searchRect, 0.15f, 4, Color{25,25,35,255});
    DrawRectangleRoundedLines(searchRect, 0.15f, 4, m_navySearchFocused ? ACCENT : (searchHov ? Color{100,100,120,255} : Color{60,60,70,255}));
    DrawText(m_navySearchQuery.empty() ? "Search..." : m_navySearchQuery.c_str(),
              px + 6, (int)y + 4, 12, m_navySearchQuery.empty() ? GRAY : WHITE);
    if (m_navySearchFocused) DrawText("|", px + 6 + MeasureText(m_navySearchQuery.c_str(), 12), (int)y + 4, 12, ACCENT);
    if (searchHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !m_navySearchFocused) {
        m_navySearchFocused = true;
        m_navyDefTroopsEditing = false;
        m_navyShipTroopsEditing = false;
    }
    if (m_navySearchFocused) {
        int key = GetCharPressed();
        while (key > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            if (key >= 32 && key < 127 && m_navySearchQuery.size() < 40) m_navySearchQuery.push_back((char)key);
            key = GetCharPressed();
        }
        odTextEditKeys(m_navySearchQuery, 40);
        bool clickAway = inputOk && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(navyMouse, searchRect);
        if (clickAway || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE)) m_navySearchFocused = false;
    }
    y += 26;
    int rows = std::max(4, (panelH - 380) / 18);
    int clicked = drawCountryList(px, y, listW, rows, m_navyCountryScroll, m_navyCountry, m_navySearchQuery);
    if (inputOk && clicked >= 0) m_navyCountry = clicked;
    y += rows * 18 + 10;

    bool hasSelectedShip = m_selectedShip >= 0 && m_selectedShip < (int)m_editorShips.size();

    // The ship-creation controls (type picker, new-ship defaults, placement
    // hints) are only useful before a ship exists to edit — with a ship
    // selected they just push the selected-ship editor off the bottom of the
    // panel, so collapse them down to a single "back to placing" button.
    if (hasSelectedShip) {
        Rectangle backBtn = {(float)px, (float)y, (float)listW, 24};
        if (drawButton("< Back to placing ships", backBtn, false, 12) && inputOk)
            m_selectedShip = -1;
        y += 30;
    } else {
        DrawText(T("Ship type:"), px, y, 12, LIGHTGRAY); y += 15;
        static const char* types[] = {"boat", "destroyer", "carrier"};
        static const char* typeLabels[] = {"Boat (triangle)", "Destroyer (square)", "Carrier (circle)"};
        for (int t = 0; t < 3; ++t) {
            Rectangle r = {(float)px, (float)y, (float)listW, 24};
            if (drawButton(typeLabels[t], r, m_navyType == types[t], 13) && inputOk)
                m_navyType = types[t];
            y += 28;
        }
        y += 4;

        // Defaults applied to the next ship placed
        drawHealthBar(px, y, listW, "New ship health:", m_navyDefaultHealth, inputOk);
        if (m_navyType == "boat") {
            DrawText(T("New boat troops:"), px, y, 11, LIGHTGRAY); y += 15;
            Rectangle troopsRect = {(float)px, (float)y, (float)listW, 22};
            drawIntField(troopsRect, m_navyDefaultTroops, 0, 100000, m_navyDefTroopsEditing, m_navyDefTroopsText, inputOk);
            if (m_navyDefTroopsEditing) m_navySearchFocused = false;
            y += 28;
        }
        y += 4;

        if (m_navyCountry < 0) {
            DrawText(T("Pick a country, then click"), px, y, 12, GRAY); y += 15;
            DrawText(T("open sea to place a ship."), px, y, 12, GRAY);
        } else {
            DrawText(T("Click open sea to place."), px, y, 12, GRAY); y += 15;
            DrawText(T("Click a ship to select it."), px, y, 12, GRAY);
        }
        y += 22;
    }

    // Selected ship info + reassign owner + health/troops + delete
    if (hasSelectedShip) {
        NavyShip& s = m_editorShips[m_selectedShip];
        const Country* sc = m_editCountries.getCountry(s.countryId);
        DrawText(T("Selected ship:"), px, y, 12, LIGHTGRAY); y += 15;
        DrawText(TextFormat("%s — %s", s.type.c_str(), sc ? sc->name.c_str() : "?"), px, y, 12, WHITE); y += 15;
        DrawText(TextFormat(T("lat %.1f  lon %.1f  (drag to move)"), s.lat, s.lon), px, y, 11, LIGHTGRAY); y += 20;

        if (m_navyShipTroopsEditingIdx != m_selectedShip) m_navyShipTroopsEditing = false;

        if (drawHealthBar(px, y, listW, "Health:", s.health, inputOk)) trackChange();

        if (s.type == "boat") {
            DrawText(T("Troops:"), px, y, 11, LIGHTGRAY); y += 15;
            Rectangle shTroopsRect = {(float)px, (float)y, (float)listW, 22};
            long long tv = s.crew;
            m_navyShipTroopsEditingIdx = m_selectedShip;
            if (drawIntField(shTroopsRect, tv, 0, 100000, m_navyShipTroopsEditing, m_navyShipTroopsText, inputOk)) {
                s.crew = (int)tv;
                trackChange();
            }
            if (m_navyShipTroopsEditing) m_navySearchFocused = false;
            y += 28;
        }
        y += 4;

        if (m_navyCountry >= 0 && m_navyCountry != s.countryId) {
            Rectangle ownBtn = {(float)px, (float)y, (float)listW, 26};
            const Country* nc = m_editCountries.getCountry(m_navyCountry);
            if (drawButton(TextFormat(T("Give to %s"), nc ? nc->name.c_str() : "?"), ownBtn, false, 12) && inputOk) {
                s.countryId = m_navyCountry;
                trackChange();
            }
            y += 30;
        }
        Rectangle delBtn = {(float)px, (float)y, (float)listW, 26};
        if (drawButton("Delete Ship (Del)", delBtn, false, 13) && inputOk) {
            m_editorShips.erase(m_editorShips.begin() + m_selectedShip);
            m_selectedShip = -1;
            trackChange();
        }
    } else {
        DrawText(TextFormat(T("Ships on map: %d"), (int)m_editorShips.size()), px, y, 12, LIGHTGRAY);
    }
}

// ── Script Panel ───────────────────────────────────────────────

void MapEditor::updateScriptPanel() {}
void MapEditor::drawScriptPanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    int listW = m_panelW - 24;
    bool inputOk = !anyModalOpen();
    Vector2 mouse = GetMousePosition();
    DrawText(T("Map Scripts"), px, y, 18, ACCENT); y += 26;

    // ── New script / library ──
    int half = (listW - 8) / 2;
    Rectangle nsBtn = {(float)px, (float)y, (float)half, 24};
    Rectangle nlBtn = {(float)(px + half + 8), (float)y, (float)half, 24};
    auto freshName = [&](const char* base) {
        for (int i = 1;; ++i) {
            std::string n = std::string(base) + std::to_string(i) + ".txt";
            if (!m_scripts.count(n)) return n;
        }
    };
    if (drawButton("+ Script", nsBtn, false, 12) && inputOk) {
        std::string n = freshName("script_");
        m_scripts[n] = "#OD/MapEngine/1\n# Entry script: runs when the map loads.\n"
                       "# waitUntil <cond> suspends until the condition holds (checked each turn).\n\n";
        trackChange();
        openScriptEditor(n);
    }
    if (drawButton("+ Library", nlBtn, false, 12) && inputOk) {
        std::string n = freshName("lib_");
        m_scripts[n] = "# Library: no #OD/MapEngine header, so it never runs on its own.\n"
                       "# Pull it into an entry script with: include \"" ;
        m_scripts[n] += n.substr(0, n.size() - 4);
        m_scripts[n] += "\"\n\n";
        trackChange();
        openScriptEditor(n);
    }
    y += 30;

    // ── Project script list (double-click opens the editor) ──
    std::vector<std::string> names;
    for (auto& [n, c] : m_scripts) names.push_back(n);
    DrawText(TextFormat(T("Project scripts (%d):"), (int)names.size()), px, y, 12, LIGHTGRAY); y += 15;
    const int itemH = 20;
    int rows = 10;
    Rectangle listRect = {(float)px, (float)y, (float)listW, (float)(rows * itemH)};
    DrawRectangle(px, y, listW, rows * itemH, Color{22, 22, 28, 255});
    if (inputOk && CheckCollisionPointRec(mouse, listRect))
        m_scriptScroll -= (int)GetMouseWheelMove();
    m_scriptScroll = std::max(0, std::min(m_scriptScroll, std::max(0, (int)names.size() - rows)));
    for (int i = m_scriptScroll; i < (int)names.size() && i < m_scriptScroll + rows; ++i) {
        int yi = y + (i - m_scriptScroll) * itemH;
        Rectangle row = {(float)px, (float)yi, (float)listW, (float)(itemH - 1)};
        bool sel = (i == m_scriptSel);
        bool hov = inputOk && CheckCollisionPointRec(mouse, row);
        if (sel || hov)
            DrawRectangle(px, yi, listW, itemH - 1, sel ? ColorAlpha(ACCENT, 0.15f) : Color{255,255,255,10});
        bool entry = ScriptEngine::isEntrypoint(m_scripts[names[i]]);
        DrawText(entry ? "[entry]" : "[lib]", px + 4, yi + 4, 10,
                 entry ? Color{130, 220, 130, 255} : Color{150, 150, 170, 255});
        DrawText(names[i].c_str(), px + 48, yi + 3, 12, sel ? ACCENT : WHITE);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            double now = GetTime();
            if (m_scriptLastClickRow == i && now - m_scriptLastClickTime < 0.4)
                openScriptEditor(names[i]);
            m_scriptLastClickRow = i;
            m_scriptLastClickTime = now;
            if (m_scriptSel != i) m_scriptDeleteArm = -1;
            m_scriptSel = i;
        }
    }
    y += rows * itemH + 4;
    DrawText(T("Double-click a script to edit it"), px, y, 10, GRAY); y += 16;

    // ── Edit / Rename / Delete selected ──
    if (m_scriptSel >= 0 && m_scriptSel < (int)names.size()) {
        int third = (listW - 16) / 3;
        Rectangle openBtn = {(float)px, (float)y, (float)third, 24};
        Rectangle renBtn = {(float)(px + third + 8), (float)y, (float)third, 24};
        Rectangle delBtn = {(float)(px + 2 * (third + 8)), (float)y, (float)third, 24};
        if (drawButton("Edit", openBtn, false, 11) && inputOk)
            openScriptEditor(names[m_scriptSel]);
        if (drawButton("Rename", renBtn, m_scriptRenaming, 11) && inputOk) {
            m_scriptRenaming = true;
            m_scriptRenameText = names[m_scriptSel];
            m_scriptDeleteArm = -1;
        }
        bool armed = (m_scriptDeleteArm == m_scriptSel);
        if (drawButton(armed ? "Confirm?" : "Delete", delBtn, armed, 11) && inputOk) {
            if (armed) {
                m_scripts.erase(names[m_scriptSel]);
                m_scriptSel = -1;
                m_scriptDeleteArm = -1;
                trackChange();
            } else {
                m_scriptDeleteArm = m_scriptSel;
                m_scriptRenaming = false;
            }
        }
        y += 28;

        if (m_scriptRenaming) {
            Rectangle nameRect = {(float)px, (float)y, (float)listW, 22};
            DrawRectangleRounded(nameRect, 0.08f, 4, Color{35,35,50,255});
            DrawRectangleRoundedLines(nameRect, 0.08f, 4, ACCENT);
            DrawText(m_scriptRenameText.c_str(), (int)nameRect.x + 6, (int)nameRect.y + 4, 12, ACCENT);
            DrawText("|", (int)(nameRect.x + 6 + MeasureText(m_scriptRenameText.c_str(), 12)),
                     (int)nameRect.y + 4, 12, ACCENT);
            int key = GetCharPressed();
            while (key > 0) {
                // Every character the field takes. Jittered, because a
                // typed word is a run of distinct taps, not one tap looped.
                Audio::get().playSfx("key_type", 0.12f);
                if (key >= 32 && key < 127 && m_scriptRenameText.size() < 64)
                    m_scriptRenameText.push_back((char)key);
                key = GetCharPressed();
            }
            odTextEditKeys(m_scriptRenameText, 64);
            if (IsKeyPressed(KEY_ENTER)) {
                std::string oldName = names[m_scriptSel];
                std::string newName = m_scriptRenameText;
                if (newName.size() < 4 || newName.substr(newName.size() - 4) != ".txt") newName += ".txt";
                if (newName == oldName) {
                    m_scriptRenaming = false;
                } else if (m_scripts.count(newName)) {
                    m_warningMsg = "A script named " + newName + " already exists";
                    m_warningTimer = 2.5f;
                } else {
                    renameScript(oldName, newName);
                    m_scriptRenaming = false;
                }
            } else if (IsKeyPressed(KEY_ESCAPE)) {
                m_scriptRenaming = false;
            }
            y += 26;
            DrawText(T("Enter to confirm, ESC to cancel"), px, y, 10, GRAY);
            y += 14;
        }
        y += 4;
    }

    // ── Disk scripts (data/scripts/) — double-click imports into the project ──
    if (!m_diskScriptsScanned) {
        m_diskScriptsScanned = true;
        m_diskScripts.clear();
        std::string dir = m_dataDir + "scripts/";
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            std::string n = e.path().filename().string();
            if (n.size() > 4 && n.substr(n.size() - 4) == ".txt") m_diskScripts.push_back(n);
        }
        std::sort(m_diskScripts.begin(), m_diskScripts.end());
    }
    DrawText("data/scripts/ on disk:", px, y, 12, LIGHTGRAY);
    Rectangle rfBtn = {(float)(px + listW - 60), (float)(y - 2), 60, 16};
    if (drawButton("rescan", rfBtn, false, 10) && inputOk) m_diskScriptsScanned = false;
    y += 15;
    if (m_diskScripts.empty()) {
        DrawText(T("(none found)"), px + 4, y, 11, GRAY); y += 16;
    } else {
        int drows = std::min(6, (int)m_diskScripts.size());
        Rectangle dRect = {(float)px, (float)y, (float)listW, (float)(drows * itemH)};
        DrawRectangle(px, y, listW, drows * itemH, Color{22, 22, 28, 255});
        if (inputOk && CheckCollisionPointRec(mouse, dRect))
            m_scriptDiskScroll -= (int)GetMouseWheelMove();
        m_scriptDiskScroll = std::max(0, std::min(m_scriptDiskScroll, std::max(0, (int)m_diskScripts.size() - drows)));
        for (int i = m_scriptDiskScroll; i < (int)m_diskScripts.size() && i < m_scriptDiskScroll + drows; ++i) {
            int yi = y + (i - m_scriptDiskScroll) * itemH;
            Rectangle row = {(float)px, (float)yi, (float)listW, (float)(itemH - 1)};
            bool hov = inputOk && CheckCollisionPointRec(mouse, row);
            if (hov) DrawRectangle(px, yi, listW, itemH - 1, Color{255,255,255,10});
            DrawText(m_diskScripts[i].c_str(), px + 4, yi + 3, 12, hov ? WHITE : LIGHTGRAY);
            if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                double now = GetTime();
                int rowId = 1000 + i; // distinct from project rows
                if (m_scriptLastClickRow == rowId && now - m_scriptLastClickTime < 0.4) {
                    std::ifstream f(m_dataDir + "scripts/" + m_diskScripts[i], std::ios::binary);
                    if (f) {
                        std::stringstream ssf; ssf << f.rdbuf();
                        m_scripts[m_diskScripts[i]] = ssf.str();
                        trackChange();
                        openScriptEditor(m_diskScripts[i]);
                    }
                }
                m_scriptLastClickRow = rowId;
                m_scriptLastClickTime = now;
            }
        }
        y += drows * itemH + 4;
        DrawText(T("Double-click to import into project"), px, y, 10, GRAY); y += 14;
    }
    y += 6;
    DrawText(T("[entry] scripts auto-run in game; [lib] files run only via include."),
             px, y, 10, GRAY); y += 26;
    DrawText(T("See docs/scripting.md for the language."), px, y, 10, GRAY);
}

// ════════════════════════════════════════════════════════════════
//  Script editor overlay — a small IDE: cursor editing, syntax
//  highlighting and completion hints for the map scripting DSL
// ════════════════════════════════════════════════════════════════

namespace {

struct ScriptHint { const char* completion; const char* doc; };
static const ScriptHint SCRIPT_HINTS[] = {
    {"set ",            "set <ref> <value> — assign (country.*, province.*, var.*, map.date)"},
    {"if ",             "if <a> <op> <b> ... else ... endif   (ops: == != > < >= <=)"},
    {"else",            "alternative branch of an if"},
    {"endif",           "closes an if"},
    {"foreach province in country.", "loop a country's provinces (locals: province.id/.population/.industry/...)"},
    {"foreach item in array.",       "loop array values (locals: item, item.index)"},
    {"foreach item in list.",        "loop list values (locals: item, item.index)"},
    {"next",            "closes a foreach"},
    {"while ",          "while <cond> ... endwhile   (max 10000 iterations)"},
    {"endwhile",        "closes a while"},
    {"waitUntil ",      "waitUntil <cond> — suspend here until true; re-checked every turn (top level only)"},
    {"include ",        "include \"name\" — splice a library script from scripts/"},
    {"array create ",   "array create NAME — new empty array"},
    {"array push ",     "array push NAME <value> — append"},
    {"array set ",      "array set NAME <index> <value>"},
    {"array remove ",   "array remove NAME <index>"},
    {"list create ",    "list create NAME — new empty linked list"},
    {"list pushfront ", "list pushfront NAME <value>"},
    {"list pushback ",  "list pushback NAME <value>"},
    {"list popfront ",  "list popfront NAME"},
    {"list popback ",   "list popback NAME"},
    {"country.",        "country.ISO.treasury/.name/.province_count/.at_war_with.ISO/.allied_with.ISO"},
    {"province.",       "province.ID.population/.owner/.industry/.fortification/.name"},
    {"map.turn",        "current turn number (read-only)"},
    {"map.date",        "current date string, e.g. \"January 2000\" (settable)"},
    {"map.name",        "map name (read-only)"},
    {"var.",            "var.NAME — your global variable (set var.NAME <value>)"},
    {"array.",          "array.NAME.length / array.NAME.<index>"},
    {"list.",           "list.NAME.length / .front / .back"},
    {"item",            "current foreach value"},
    {"item.index",      "current foreach index"},
    {"#OD/MapEngine/1", "entrypoint header: files carrying it auto-run; without it = library"},
};

// Full in-IDE documentation, keyword by keyword. Mirrors docs/scripting.md
// but is self-contained (baked into the binary) so it's always available
// without shipping the docs/ tree alongside the game data.
struct DocEntry { const char* keyword; const char* body; };
static const DocEntry SCRIPT_DOCS[] = {
    {"Execution model",
     "A script file whose first non-blank line is the #OD/MapEngine/1 header is an\n"
     "ENTRYPOINT: it runs automatically once all map data is loaded. A file WITHOUT\n"
     "that header is a LIBRARY: it never runs on its own, only when pulled in with\n"
     "`include`. A script may contain waitUntil statements; when one's condition is\n"
     "false the script suspends right there and is re-checked once per turn (after\n"
     "the date/turn counter advances) until it becomes true, then execution resumes\n"
     "on the next line. Suspension is NOT saved into save games: reloading a save\n"
     "re-runs entry scripts from the top and they re-suspend at their first false\n"
     "waitUntil, so code before a waitUntil should be safe to run more than once."},
    {"#OD/MapEngine/1",
     "Must be the first non-blank line of a file for it to be treated as an\n"
     "entrypoint (auto-run on map load). The trailing number is the engine version;\n"
     "scripts declaring an unsupported version are rejected with an error. Files\n"
     "without this header are libraries and are only reachable via `include`."},
    {"# comment",
     "Any line starting with # is a comment and ignored (aside from the special\n"
     "#OD/MapEngine/ header line). There are no inline/trailing comments — a # must\n"
     "be the first non-space character on the line."},
    {"set",
     "set <ref> <value>\n\n"
     "Assigns a value to a writable reference. <value> may be a literal (int,\n"
     "float, true/false, \"quoted string\") or another reference (its current value\n"
     "is read and copied). Examples:\n"
     "  set country.USA.treasury 5000\n"
     "  set province.42.owner CAN\n"
     "  set country.RUS.at_war_with UKR true\n"
     "  set var.turnsWaited 0\n"
     "  set map.date \"January 1960\""},
    {"if / else / endif",
     "if <a> <op> <b>\n    ...\nelse\n    ...\nendif\n\n"
     "Conditional branch. <op> is one of == != > < >= <=. Only a single comparison\n"
     "is supported per condition — there is no nested parentheses, and/or, or\n"
     "arithmetic. `else` and its body are optional."},
    {"foreach / next (provinces)",
     "foreach province in country.ISO\n    ...\nnext\n\n"
     "Iterates every province owned by the given country. Inside the body these\n"
     "locals are available: province / province.id, province.population,\n"
     "province.industry, province.fortification, province.owner."},
    {"foreach / next (array/list)",
     "foreach item in array.NAME\n    ...\nnext\n\nforeach item in list.NAME\n    ...\nnext\n\n"
     "Iterates every element of an array or linked list. Inside the body: `item`\n"
     "is the current value, `item.index` is its 0-based position."},
    {"while / endwhile",
     "while <cond>\n    ...\nendwhile\n\n"
     "Repeats the body while the condition holds. Capped at 10,000 iterations as a\n"
     "safety limit — a script that hits the cap logs an error and moves on."},
    {"waitUntil",
     "waitUntil <cond>\nwaitUntil(<cond>)\n\n"
     "Suspends the script at this line until <cond> becomes true, then continues\n"
     "on the next line. Re-checked once per turn. ONLY valid at the top level of a\n"
     "script — not inside if/foreach/while (using it there is a lint/runtime\n"
     "error). A script can have several waitUntils in sequence to stage events:\n"
     "  waitUntil map.turn >= 12\n"
     "  set country.RUS.at_war_with USA true\n"
     "  waitUntil map.date == \"January 1965 AD\"\n"
     "  set country.RUS.at_war_with USA false"},
    {"include",
     "include \"name\"\n\n"
     "Splices another project script's lines in at this point (.txt extension is\n"
     "optional). Included files must be LIBRARIES (no #OD/MapEngine header).\n"
     "Circular includes are rejected; include depth is capped at 16. Renaming a\n"
     "script updates every include that referenced its old name automatically —\n"
     "use the Rename button in the Scripts tab rather than deleting/recreating."},
    {"array",
     "array create NAME              new/reset empty array\n"
     "array push NAME <value>        append a value\n"
     "array set NAME <index> <value> overwrite index (0-based)\n"
     "array remove NAME <index>      delete an element\n\n"
     "Read with: array.NAME.length, array.NAME.<index> (e.g. array.targets.0)."},
    {"list",
     "list create NAME               new/reset empty linked list\n"
     "list pushfront NAME <value>    insert at the front\n"
     "list pushback NAME <value>     insert at the back\n"
     "list popfront NAME             remove the front element\n"
     "list popback NAME              remove the back element\n\n"
     "Read with: list.NAME.length, list.NAME.front, list.NAME.back."},
    {"var (global variables)",
     "set var.NAME <value>    create or update a global variable\n"
     "var.NAME                read it back anywhere\n\n"
     "Variables, arrays and lists are shared across every script and survive\n"
     "waitUntil suspensions (but not save/reload — see Execution model)."},
    {"country.* references",
     "country.ISO.treasury              (float, writable)\n"
     "country.ISO.name                  (string, writable)\n"
     "country.ISO.iso                   (string)\n"
     "country.ISO.province_count        (int)\n"
     "country.ISO.at_war_with.OTHER     (bool, writable via `set ... at_war_with OTHER true`)\n"
     "country.ISO.allied_with.OTHER     (bool, writable)\n"
     "country.ISO.claims_province.ID    (bool)"},
    {"province.* references",
     "province.ID.population      (int, writable)\n"
     "province.ID.owner           (string ISO, writable — set province.ID.owner CAN)\n"
     "province.ID.name            (string)\n"
     "province.ID.industry        (int 0-10, writable)\n"
     "province.ID.fortification   (int 0-5, writable)"},
    {"map.* references",
     "map.turn    (int, read-only) — current turn number\n"
     "map.date    (string, WRITABLE) — e.g. \"January 2000\" / \"44 BC\" style dates\n"
     "map.name    (string, read-only) — the map's display name"},
    {"Value types",
     "Integers: 42  -5  0\n"
     "Floats:   3.14  -1.5\n"
     "Booleans: true  false\n"
     "Strings:  \"double-quoted text\"\n"
     "References: any of the above dotted paths — evaluated to their current value"},
    {"Errors",
     "A malformed line, unknown command, or bad reference logs an error (visible\n"
     "in the console and as an in-game toast for 3 seconds) but does not stop other\n"
     "scripts from running. In the editor, offending lines are underlined in red —\n"
     "hover a line's gutter for the message."},
};

static bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '/' || c == '#';
}

static bool isScriptKeyword(const std::string& t) {
    static const char* kws[] = {"set","if","else","elseif","unless","endif","foreach","in","next",
                                "while","endwhile","for","to","repeat","break","continue","print",
                                "waitUntil","include","array","list","create","push","remove",
                                "label","jump","spawn","stop","try","catch","endtry","mod",
                                "pushfront","pushback","popfront","popback","true","false",
                                // expression functions, so they highlight too
                                "min","max","abs","round","floor","ceil","clamp","len",
                                "and","or","not"};
    for (const char* k : kws) if (t == k) return true;
    return false;
}

// Same rules as ScriptEngine::tokenize — space-separated, quotes group.
static std::vector<std::string> tokenizeStatic(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inQuote = false;
    for (char c : line) {
        if (c == '"') { inQuote = !inQuote; continue; }
        if ((c == ' ' || c == '\t') && !inQuote) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

static bool isScriptRef(const std::string& t) {
    return t.rfind("country.", 0) == 0 || t.rfind("province.", 0) == 0 ||
           t.rfind("map.", 0) == 0 || t.rfind("var.", 0) == 0 ||
           t.rfind("array.", 0) == 0 || t.rfind("list.", 0) == 0 ||
           t == "item" || t.rfind("item.", 0) == 0 || t == "province";
}

// Draw one script line with syntax colors; returns nothing. Proportional font:
// x advances by measured widths so colors line up with the text.
static void drawHighlightedLine(const std::string& line, int x, int y, int fs) {
    const Color COL_COMMENT = {110, 190, 110, 255};
    const Color COL_KEYWORD = {200, 160, 255, 255};
    const Color COL_STRING = {255, 190, 110, 255};
    const Color COL_NUMBER = {130, 200, 255, 255};
    const Color COL_REF = {120, 220, 220, 255};
    const Color COL_PLAIN = {230, 230, 235, 255};

    // Whole-line comment (incl. the #OD header)
    size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line[first] == '#') {
        DrawText(line.c_str(), x, y, fs, COL_COMMENT);
        return;
    }

    size_t i = 0;
    while (i < line.size()) {
        // Space run
        if (line[i] == ' ' || line[i] == '\t') {
            size_t j = i;
            while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) j++;
            x += MeasureText(line.substr(i, j - i).c_str(), fs);
            i = j;
            continue;
        }
        // Quoted string
        if (line[i] == '"') {
            size_t j = line.find('"', i + 1);
            if (j == std::string::npos) j = line.size() - 1;
            std::string seg = line.substr(i, j - i + 1);
            DrawText(seg.c_str(), x, y, fs, COL_STRING);
            x += MeasureText(seg.c_str(), fs);
            i = j + 1;
            continue;
        }
        // Token run
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '"') j++;
        std::string tok = line.substr(i, j - i);
        Color col = COL_PLAIN;
        if (isScriptKeyword(tok)) col = COL_KEYWORD;
        else if (isScriptRef(tok)) col = COL_REF;
        else if (!tok.empty() && ((tok[0] >= '0' && tok[0] <= '9') || (tok[0] == '-' && tok.size() > 1))) col = COL_NUMBER;
        DrawText(tok.c_str(), x, y, fs, col);
        x += MeasureText(tok.c_str(), fs);
        i = j;
    }
}

} // namespace

void MapEditor::openScriptEditor(const std::string& name) {
    auto it = m_scripts.find(name);
    if (it == m_scripts.end()) return;
    m_scriptEdName = name;
    m_scriptEdLines.clear();
    std::istringstream ss(it->second);
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        m_scriptEdLines.push_back(line);
    }
    if (m_scriptEdLines.empty()) m_scriptEdLines.push_back("");
    m_scriptEdCurLine = 0;
    m_scriptEdCurCol = 0;
    m_scriptEdScroll = 0;
    m_scriptEdSelLine = -1;
    m_scriptEdMouseSelecting = false;
    m_scriptEdHints.clear();
    m_scriptEdHintDoc.clear();
    m_scriptRenaming = false;
    m_scriptEdOpen = true;
    lintScriptEditor();
}

// A lightweight static check — mirrors the ScriptEngine's own block/keyword
// rules closely enough to catch the mistakes mapmakers actually make, without
// needing a live Game instance (the editor has none). Populates
// m_scriptEdErrors: line index -> message, drawn as red underlines/gutter dots.
void MapEditor::lintScriptEditor() {
    m_scriptEdErrors.clear();
    struct Frame { std::string kw; int line; };
    int declaredVersion = ScriptEngine::ENGINE_VERSION;
    for (const auto& l : m_scriptEdLines) {
        const size_t a = l.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        if (l.compare(a, 14, "#OD/MapEngine/") == 0) sscanf(l.c_str() + a + 14, "%d", &declaredVersion);
        break;
    }
    std::vector<Frame> stack;
    int depthAtWait = 0;
    for (int i = 0; i < (int)m_scriptEdLines.size(); ++i) {
        std::string line = m_scriptEdLines[i];
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line.empty() || line[0] == '#') continue;

        auto tokens = tokenizeStatic(line);
        if (tokens.empty()) continue;
        // A shorthand (`x++`, `x += 1`) is linted as the `set` it stands for,
        // using the same normaliser the engine runs -- otherwise the editor
        // would red-flag a line the game accepts.
        {
            const std::string norm = odscript::normaliseAssignment(line);
            if (norm != line) { line = norm; tokens = tokenizeStatic(line); if (tokens.empty()) continue; }
        }
        const std::string& kw = tokens[0];
        if (declaredVersion < 2 && ScriptEngine::isVersion2StatementPublic(kw)) {
            m_scriptEdErrors[i] = "'" + kw + "' needs #OD/MapEngine/2";
            continue;
        }

        if (kw == "if" || kw == "unless" || kw == "foreach" || kw == "while" ||
            kw == "for" || kw == "repeat" || kw == "try") {
            stack.push_back({kw, i});
        } else if (kw == "endif" || kw == "next" || kw == "endwhile" || kw == "endtry") {
            // `next` closes foreach, for AND repeat; `endif` closes if and
            // unless. Version 2 added three openers and the lint knew none of
            // them, so a valid script came up red -- six errors on the demo.
            const std::string top = stack.empty() ? "" : stack.back().kw;
            bool matches;
            if (kw == "endif")        matches = (top == "if" || top == "unless");
            else if (kw == "next")    matches = (top == "foreach" || top == "for" || top == "repeat");
            else if (kw == "endtry")  matches = (top == "try");
            else                      matches = (top == "while");
            const char* want = kw == "endif" ? "if"
                             : (kw == "next" ? "foreach/for/repeat"
                             : (kw == "endtry" ? "try" : "while"));
            if (!matches) {
                m_scriptEdErrors[i] = std::string("'") + kw + "' without a matching '" + want + "'";
            } else {
                stack.pop_back();
            }
        } else if (kw == "catch") {
            if (stack.empty() || stack.back().kw != "try")
                m_scriptEdErrors[i] = "'catch' without a 'try'";
        } else if (kw == "else" || kw == "elseif") {
            if (stack.empty() || (stack.back().kw != "if" && stack.back().kw != "unless"))
                m_scriptEdErrors[i] = "'else' outside an if block";
        } else if (kw.rfind("waitUntil", 0) == 0 &&
                   (kw.size() == 9 || line[9] == ' ' || line[9] == '\t' || line[9] == '(')) {
            if (!stack.empty()) {
                m_scriptEdErrors[i] = "waitUntil is only allowed at top level (not inside if/foreach/while)";
            } else {
                std::string rest = line.substr(9);
                size_t s = rest.find_first_not_of(" \t");
                rest = (s == std::string::npos) ? "" : rest.substr(s);
                if (rest.size() >= 2 && rest.front() == '(' && rest.back() == ')')
                    rest = rest.substr(1, rest.size() - 2);
                if (rest.empty()) m_scriptEdErrors[i] = "waitUntil: missing condition";
            }
        } else if (kw == "include") {
            if (tokens.size() < 2) {
                m_scriptEdErrors[i] = "include: missing script name";
            } else {
                std::string inc = tokens[1];
                if (inc.size() >= 2 && inc.front() == '"') inc = inc.substr(1);
                if (!inc.empty() && inc.back() == '"') inc.pop_back();
                std::string incTxt = inc.size() > 4 && inc.substr(inc.size()-4) == ".txt" ? inc : inc + ".txt";
                if (inc == m_scriptEdName || incTxt == m_scriptEdName) {
                    m_scriptEdErrors[i] = "a script cannot include itself";
                } else if (!m_scripts.count(inc) && !m_scripts.count(incTxt)) {
                    m_scriptEdErrors[i] = "include: script '" + inc + "' not found in this project";
                }
            }
        } else if (kw == "set") {
            if (tokens.size() < 3) m_scriptEdErrors[i] = "set: requires <ref> <value>";
        } else if (kw == "array" || kw == "list") {
            static const char* arrOps[] = {"create","push","set","remove"};
            static const char* listOps[] = {"create","pushfront","pushback","popfront","popback"};
            bool ok = false;
            if (tokens.size() >= 2) {
                const char** ops = (kw == "array") ? arrOps : listOps;
                int n = (kw == "array") ? 4 : 5;
                for (int k = 0; k < n; ++k) if (tokens[1] == ops[k]) { ok = true; break; }
            }
            if (!ok) m_scriptEdErrors[i] = kw + ": unknown or missing operation";
        } else if (kw != "set" && !isScriptKeyword(kw) && kw.rfind("country.",0) != 0 &&
                   kw.rfind("province.",0) != 0 && kw.rfind("map.",0) != 0) {
            m_scriptEdErrors[i] = "unknown command: " + kw;
        }
    }
    for (auto& f : stack)
        m_scriptEdErrors[f.line] = std::string("'") + f.kw + "' is never closed";
}

void MapEditor::saveScriptEditor() {
    if (m_scriptEdName.empty()) return;
    std::string joined;
    for (size_t i = 0; i < m_scriptEdLines.size(); ++i) {
        joined += m_scriptEdLines[i];
        if (i + 1 < m_scriptEdLines.size()) joined += '\n';
    }
    joined += '\n';
    if (m_scripts[m_scriptEdName] != joined) {
        m_scripts[m_scriptEdName] = joined;
        trackChange();
    }
    m_saveStatus = "Saved " + m_scriptEdName;
    m_saveStatusTimer = 2.0f;
}

std::string MapEditor::scriptEdCurrentWord(int* startCol) const {
    if (m_scriptEdCurLine >= (int)m_scriptEdLines.size()) return "";
    const std::string& line = m_scriptEdLines[m_scriptEdCurLine];
    int col = std::min(m_scriptEdCurCol, (int)line.size());
    int s = col;
    while (s > 0 && isWordChar(line[s - 1])) s--;
    if (startCol) *startCol = s;
    return line.substr(s, col - s);
}

void MapEditor::refreshScriptHints() {
    m_scriptEdHints.clear();
    m_scriptEdHintDoc.clear();
    std::string word = scriptEdCurrentWord();
    if (word.size() < 1) return;
    for (auto& h : SCRIPT_HINTS) {
        std::string comp = h.completion;
        if (comp.size() > word.size() && comp.compare(0, word.size(), word) == 0) {
            if (m_scriptEdHints.empty()) m_scriptEdHintDoc = h.doc;
            m_scriptEdHints.push_back(comp);
            if (m_scriptEdHints.size() >= 5) break;
        }
    }
}

bool MapEditor::scriptEdHasSelection() const {
    return m_scriptEdSelLine >= 0 &&
           (m_scriptEdSelLine != m_scriptEdCurLine || m_scriptEdSelCol != m_scriptEdCurCol);
}

void MapEditor::scriptEdSelectionRange(int& l0, int& c0, int& l1, int& c1) const {
    l0 = m_scriptEdSelLine; c0 = m_scriptEdSelCol;
    l1 = m_scriptEdCurLine; c1 = m_scriptEdCurCol;
    if (l0 > l1 || (l0 == l1 && c0 > c1)) { std::swap(l0, l1); std::swap(c0, c1); }
}

std::string MapEditor::scriptEdSelectedText() const {
    if (!scriptEdHasSelection()) return "";
    int l0, c0, l1, c1;
    scriptEdSelectionRange(l0, c0, l1, c1);
    if (l0 == l1) return m_scriptEdLines[l0].substr(c0, c1 - c0);
    std::string out = m_scriptEdLines[l0].substr(c0);
    for (int i = l0 + 1; i < l1; ++i) { out += '\n'; out += m_scriptEdLines[i]; }
    out += '\n';
    out += m_scriptEdLines[l1].substr(0, c1);
    return out;
}

void MapEditor::scriptEdDeleteSelection() {
    if (!scriptEdHasSelection()) return;
    int l0, c0, l1, c1;
    scriptEdSelectionRange(l0, c0, l1, c1);
    auto& lines = m_scriptEdLines;
    if (l0 == l1) {
        lines[l0].erase(c0, c1 - c0);
    } else {
        std::string merged = lines[l0].substr(0, c0) + lines[l1].substr(c1);
        lines.erase(lines.begin() + l0 + 1, lines.begin() + l1 + 1);
        lines[l0] = merged;
    }
    m_scriptEdCurLine = l0;
    m_scriptEdCurCol = c0;
    m_scriptEdSelLine = -1;
}

void MapEditor::scriptEdInsertText(const std::string& text) {
    if (m_scriptEdCurLine >= (int)m_scriptEdLines.size()) return;
    if (scriptEdHasSelection()) scriptEdDeleteSelection();
    for (char ch : text) {
        std::string& line = m_scriptEdLines[m_scriptEdCurLine];
        int col = std::min(m_scriptEdCurCol, (int)line.size());
        if (ch == '\n') {
            std::string rest = line.substr(col);
            line = line.substr(0, col);
            m_scriptEdLines.insert(m_scriptEdLines.begin() + m_scriptEdCurLine + 1, rest);
            m_scriptEdCurLine++;
            m_scriptEdCurCol = 0;
        } else if (ch == '\r') {
            continue;
        } else if (ch == '\t') {
            line.insert(col, "  ");
            m_scriptEdCurCol = col + 2;
        } else if ((unsigned char)ch >= 32 && (unsigned char)ch < 127) {
            line.insert(line.begin() + col, ch);
            m_scriptEdCurCol = col + 1;
        }
    }
}

void MapEditor::drawScriptEditorOverlay() {
    if (!m_scriptEdOpen) return;
    if (m_scriptDocsOpen) { drawScriptDocsOverlay(); return; }
    Vector2 mouse = GetMousePosition();
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 215});

    const int fs = 14;
    const int lineH = 19;
    const int gutterW = 52;
    const int areaX = 16 + gutterW;
    const int areaY = 64;
    const int hintBarH = 58;
    const int areaW = m_screenW - areaX - 24;
    const int areaH = m_screenH - areaY - hintBarH - 16;
    const int visLines = std::max(1, areaH / lineH);

    // ── Header ──
    bool entry = false;
    {
        std::string joined;
        for (auto& l : m_scriptEdLines) { joined += l; joined += '\n'; }
        entry = ScriptEngine::isEntrypoint(joined);
    }
    DrawText(TextFormat("%s  %s", m_scriptEdName.c_str(), entry ? "[entry]" : "[library]"),
             16, 14, 20, ACCENT);
    DrawText(T("Ctrl/Cmd+S save · ESC save & close · Tab completes hint · Shift+arrows/drag select · Ctrl+C/X/V/A"),
             16, 40, 12, Color{150, 150, 170, 220});
    // ── A script pinned to an older engine version says so, loudly ──
    //
    // The header is not decoration: version 1 refuses version 2's statements
    // at run time, so an author writing `for` into a file that still says
    // /1 would otherwise only find out when the map loads.
    {
        int declared = ScriptEngine::ENGINE_VERSION;
        for (const auto& l : m_scriptEdLines) {
            const size_t a = l.find_first_not_of(" \t");
            if (a == std::string::npos) continue;
            if (l.compare(a, 14, "#OD/MapEngine/") == 0) { sscanf(l.c_str() + a + 14, "%d", &declared); }
            break;
        }
        if (declared < ScriptEngine::ENGINE_VERSION) {
            const std::string warn =
                TextFormat(T("Engine version %d: version %d statements (for, repeat, break, "
                             "try, jump, print) are refused here"),
                           declared, ScriptEngine::ENGINE_VERSION);
            DrawText(warn.c_str(), 16, m_screenH - 22, 12, Color{235, 190, 90, 255});
        }
    }

    if (!m_scriptEdErrors.empty()) {
        std::string errMsg = std::to_string(m_scriptEdErrors.size()) + " error" +
                             (m_scriptEdErrors.size() == 1 ? "" : "s");
        DrawText(errMsg.c_str(), m_screenW - 300, 18, 14, Color{230, 100, 100, 255});
    }
    Rectangle closeBtn = {(float)(m_screenW - 44), 8, 36, 36};
    DrawRectangleRounded(closeBtn, 0.2f, 6, {60, 60, 70, 180});
    DrawRectangleRoundedLines(closeBtn, 0.2f, 6, {180, 180, 180, 200});
    DrawText("X", (int)closeBtn.x + 13, 14, 20, {180, 180, 180, 200});
    if (CheckCollisionPointRec(mouse, closeBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        saveScriptEditor();
        m_scriptEdOpen = false;
        return;
    }
    // ── Text / Blocks ──
    //
    // The two views never edit at the same time: switching regenerates the
    // other side from this one. Safe only because the round trip is exact --
    // see src/script/Blocks.cpp.
    Rectangle modeBtn = {(float)(m_screenW - 232), 8, 92, 36};
    bool modeHov = CheckCollisionPointRec(mouse, modeBtn);
    DrawRectangleRounded(modeBtn, 0.2f, 6, m_blocksMode ? Color{70, 60, 100, 230}
                                                        : (modeHov ? Color{60, 60, 90, 220} : Color{45, 45, 60, 180}));
    DrawRectangleRoundedLines(modeBtn, 0.2f, 6, modeHov || m_blocksMode ? ACCENT : Color{130, 130, 160, 200});
    DrawText(m_blocksMode ? T("Text view") : T("Blocks"),
             (int)modeBtn.x + 12, (int)modeBtn.y + 10, 15, modeHov || m_blocksMode ? ACCENT : LIGHTGRAY);
    if (modeHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (m_blocksMode) { blocksToText(); m_blocksMode = false; }
        else { blocksFromText(); m_blocksMode = true; }
        Audio::get().playSfx(m_blocksMode ? "toggle_on" : "toggle_off", 0.6f);
    }

    if (m_blocksMode) {
        if (IsKeyPressed(KEY_ESCAPE)) {
            blocksToText();
            m_blocksMode = false;
            saveScriptEditor();
            m_scriptEdOpen = false;
            return;
        }
        const bool ctrlB = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                           IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
        if (ctrlB && IsKeyPressed(KEY_S)) { blocksToText(); saveScriptEditor(); }
        drawBlockEditor(16, areaY - 8, m_screenW - 32, m_screenH - areaY - 8);
        return;
    }

    Rectangle docsBtn = {(float)(m_screenW - 130), 8, 78, 36};
    bool docsHov = CheckCollisionPointRec(mouse, docsBtn);
    DrawRectangleRounded(docsBtn, 0.2f, 6, docsHov ? Color{60, 60, 90, 220} : Color{45, 45, 60, 180});
    DrawRectangleRoundedLines(docsBtn, 0.2f, 6, docsHov ? ACCENT : Color{130, 130, 160, 200});
    DrawText(T("? Docs"), (int)docsBtn.x + 12, (int)docsBtn.y + 10, 15, docsHov ? ACCENT : LIGHTGRAY);
    if (docsHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_scriptDocsOpen = true;
        // Jump straight to the doc entry for the keyword under the cursor, if any
        std::string word = scriptEdCurrentWord();
        for (int i = 0; i < (int)(sizeof(SCRIPT_DOCS)/sizeof(SCRIPT_DOCS[0])); ++i) {
            std::string kw = SCRIPT_DOCS[i].keyword;
            if (!word.empty() && kw.find(word) != std::string::npos) { m_scriptDocsSel = i; break; }
        }
        return;
    }

    // ── Keyboard input ──
    bool edited = false;
    auto& lines = m_scriptEdLines;
    auto clampCursor = [&]() {
        m_scriptEdCurLine = std::max(0, std::min(m_scriptEdCurLine, (int)lines.size() - 1));
        m_scriptEdCurCol = std::max(0, std::min(m_scriptEdCurCol, (int)lines[m_scriptEdCurLine].size()));
    };
    clampCursor();

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (ctrl && IsKeyPressed(KEY_S)) saveScriptEditor();
    if (ctrl && IsKeyPressed(KEY_A)) {
        m_scriptEdSelLine = 0; m_scriptEdSelCol = 0;
        m_scriptEdCurLine = (int)lines.size() - 1;
        m_scriptEdCurCol = (int)lines.back().size();
    }
    if (ctrl && (IsKeyPressed(KEY_C) || IsKeyPressed(KEY_X)) && scriptEdHasSelection()) {
        SetClipboardText(scriptEdSelectedText().c_str());
        if (IsKeyPressed(KEY_X)) { scriptEdDeleteSelection(); edited = true; }
    }
    if (ctrl && IsKeyPressed(KEY_V)) {
        const char* clip = GetClipboardText();
        if (clip && *clip) { scriptEdInsertText(clip); edited = true; }
    }

    int key = GetCharPressed();
    while (key > 0) {
        // Every character the field takes. Jittered, because a
        // typed word is a run of distinct taps, not one tap looped.
        Audio::get().playSfx("key_type", 0.12f);
        if (!ctrl && key >= 32 && key < 127) {
            std::string s(1, (char)key);
            scriptEdInsertText(s);
            edited = true;
        }
        key = GetCharPressed();
    }
    auto pressed = [](int k) { return IsKeyPressed(k) || IsKeyPressedRepeat(k); };
    // Extend/collapse the selection anchor around a cursor-moving key based on Shift.
    auto beginMove = [&]() { if (shift && m_scriptEdSelLine < 0) { m_scriptEdSelLine = m_scriptEdCurLine; m_scriptEdSelCol = m_scriptEdCurCol; } };
    auto endMove = [&]() { if (!shift) m_scriptEdSelLine = -1; };

    if (pressed(KEY_ENTER)) {
        if (scriptEdHasSelection()) scriptEdDeleteSelection();
        // Split + keep the current line's indentation
        std::string& line = lines[m_scriptEdCurLine];
        int col = std::min(m_scriptEdCurCol, (int)line.size());
        std::string indent;
        for (char c : line) { if (c == ' ') indent += ' '; else break; }
        std::string rest = line.substr(col);
        line = line.substr(0, col);
        lines.insert(lines.begin() + m_scriptEdCurLine + 1, indent + rest);
        m_scriptEdCurLine++;
        m_scriptEdCurCol = (int)indent.size();
        edited = true;
    }
    if (pressed(KEY_BACKSPACE)) {
        if (scriptEdHasSelection()) {
            scriptEdDeleteSelection();
        } else {
            std::string& line = lines[m_scriptEdCurLine];
            if (m_scriptEdCurCol > 0) {
                line.erase(line.begin() + (m_scriptEdCurCol - 1));
                m_scriptEdCurCol--;
            } else if (m_scriptEdCurLine > 0) {
                int prevLen = (int)lines[m_scriptEdCurLine - 1].size();
                lines[m_scriptEdCurLine - 1] += line;
                lines.erase(lines.begin() + m_scriptEdCurLine);
                m_scriptEdCurLine--;
                m_scriptEdCurCol = prevLen;
            }
        }
        edited = true;
    }
    if (pressed(KEY_DELETE)) {
        if (scriptEdHasSelection()) {
            scriptEdDeleteSelection();
        } else {
            std::string& line = lines[m_scriptEdCurLine];
            if (m_scriptEdCurCol < (int)line.size()) {
                line.erase(line.begin() + m_scriptEdCurCol);
            } else if (m_scriptEdCurLine + 1 < (int)lines.size()) {
                line += lines[m_scriptEdCurLine + 1];
                lines.erase(lines.begin() + m_scriptEdCurLine + 1);
            }
        }
        edited = true;
    }
    if (pressed(KEY_LEFT)) {
        beginMove();
        if (m_scriptEdCurCol > 0) m_scriptEdCurCol--;
        else if (m_scriptEdCurLine > 0) { m_scriptEdCurLine--; m_scriptEdCurCol = (int)lines[m_scriptEdCurLine].size(); }
        endMove();
    }
    if (pressed(KEY_RIGHT)) {
        beginMove();
        if (m_scriptEdCurCol < (int)lines[m_scriptEdCurLine].size()) m_scriptEdCurCol++;
        else if (m_scriptEdCurLine + 1 < (int)lines.size()) { m_scriptEdCurLine++; m_scriptEdCurCol = 0; }
        endMove();
    }
    if (pressed(KEY_UP)) { beginMove(); m_scriptEdCurLine--; endMove(); }
    if (pressed(KEY_DOWN)) { beginMove(); m_scriptEdCurLine++; endMove(); }
    if (pressed(KEY_PAGE_UP)) { beginMove(); m_scriptEdCurLine -= visLines; endMove(); }
    if (pressed(KEY_PAGE_DOWN)) { beginMove(); m_scriptEdCurLine += visLines; endMove(); }
    if (IsKeyPressed(KEY_HOME)) { beginMove(); m_scriptEdCurCol = 0; endMove(); }
    if (IsKeyPressed(KEY_END)) { beginMove(); m_scriptEdCurCol = (int)lines[std::max(0, std::min(m_scriptEdCurLine, (int)lines.size() - 1))].size(); endMove(); }
    if (pressed(KEY_TAB)) {
        if (!m_scriptEdHints.empty()) {
            int startCol = 0;
            std::string word = scriptEdCurrentWord(&startCol);
            std::string& line = lines[m_scriptEdCurLine];
            line.erase(startCol, word.size());
            line.insert(startCol, m_scriptEdHints[0]);
            m_scriptEdCurCol = startCol + (int)m_scriptEdHints[0].size();
        } else {
            scriptEdInsertText("\t");
        }
        edited = true;
    }
    clampCursor();

    // Wheel scroll + keep the cursor visible after keyboard movement
    float wheel = GetMouseWheelMove();
    if (wheel != 0) m_scriptEdScroll -= (int)(wheel * 3);
    if (m_scriptEdCurLine < m_scriptEdScroll) m_scriptEdScroll = m_scriptEdCurLine;
    if (m_scriptEdCurLine >= m_scriptEdScroll + visLines) m_scriptEdScroll = m_scriptEdCurLine - visLines + 1;
    m_scriptEdScroll = std::max(0, std::min(m_scriptEdScroll, std::max(0, (int)lines.size() - visLines)));

    // Click (and drag) to position the cursor / select text
    Rectangle areaRect = {(float)(16), (float)areaY, (float)(gutterW + areaW), (float)areaH};
    auto colAtMouseX = [&](int li, float mx) {
        const std::string& line = lines[li];
        int best = (int)line.size();
        for (int c = 0; c <= (int)line.size(); ++c) {
            int w = MeasureText(line.substr(0, c).c_str(), fs);
            if (areaX + w >= (int)mx - 3) { best = c; return best; }
        }
        return best;
    };
    if (CheckCollisionPointRec(mouse, areaRect) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        int li = m_scriptEdScroll + (int)((mouse.y - areaY) / lineH);
        li = std::max(0, std::min(li, (int)lines.size() - 1));
        m_scriptEdCurLine = li;
        m_scriptEdCurCol = colAtMouseX(li, mouse.x);
        if (shift && m_scriptEdSelLine < 0) {
            // Extend from wherever the cursor already was
        } else if (!shift) {
            m_scriptEdSelLine = li;
            m_scriptEdSelCol = m_scriptEdCurCol;
        }
        m_scriptEdMouseSelecting = true;
        m_scriptEdHints.clear();
        m_scriptEdHintDoc.clear();
    } else if (m_scriptEdMouseSelecting && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        int li = m_scriptEdScroll + (int)((mouse.y - areaY) / lineH);
        li = std::max(0, std::min(li, (int)lines.size() - 1));
        m_scriptEdCurLine = li;
        m_scriptEdCurCol = colAtMouseX(li, mouse.x);
    } else if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_scriptEdMouseSelecting = false;
    }

    if (edited) { refreshScriptHints(); lintScriptEditor(); }

    // ── Text area ──
    DrawRectangle(12, areaY - 6, gutterW + areaW + 12, areaH + 12, {16, 16, 22, 255});
    DrawRectangleLines(12, areaY - 6, gutterW + areaW + 12, areaH + 12, {70, 70, 85, 255});
    BeginScissorMode(12, areaY - 4, gutterW + areaW + 10, areaH + 8);
    int hoveredErrorLine = -1;
    bool hasSel = scriptEdHasSelection();
    int selL0 = 0, selC0 = 0, selL1 = 0, selC1 = 0;
    if (hasSel) scriptEdSelectionRange(selL0, selC0, selL1, selC1);
    for (int i = m_scriptEdScroll; i < (int)lines.size() && i < m_scriptEdScroll + visLines; ++i) {
        int ly = areaY + (i - m_scriptEdScroll) * lineH;
        if (i == m_scriptEdCurLine)
            DrawRectangle(12, ly - 2, gutterW + areaW + 10, lineH, {255, 255, 255, 8});
        if (hasSel && i >= selL0 && i <= selL1) {
            int fromCol = (i == selL0) ? selC0 : 0;
            int toCol = (i == selL1) ? selC1 : (int)lines[i].size();
            int sx = areaX + MeasureText(lines[i].substr(0, fromCol).c_str(), fs);
            int ex = areaX + MeasureText(lines[i].substr(0, toCol).c_str(), fs);
            if (ex <= sx) ex = sx + 6; // visualize empty/newline selection span
            DrawRectangle(sx, ly, ex - sx, lineH - 1, ColorAlpha(ACCENT, 0.30f));
        }
        bool hasError = m_scriptEdErrors.count(i) > 0;
        const char* num = TextFormat("%d", i + 1);
        DrawText(num, 16 + gutterW - 12 - MeasureText(num, 11), ly + 2, 11,
                 hasError ? Color{230, 90, 90, 255} : Color{110, 110, 130, 255});
        if (hasError) {
            // Gutter error dot (hover for the message) + red underline under the line
            Rectangle dot = {(float)16, (float)(ly + 4), 8, 8};
            DrawCircle((int)(dot.x + 4), (int)(dot.y + 4), 4, Color{230, 90, 90, 255});
            if (CheckCollisionPointRec(mouse, {(float)12, (float)ly, 20, (float)lineH})) hoveredErrorLine = i;
            int lw = std::max(20, MeasureText(lines[i].c_str(), fs));
            for (int ux = areaX; ux < areaX + lw; ux += 4)
                DrawLine(ux, ly + fs + 2, std::min(ux + 2, areaX + lw), ly + fs + 2, Color{230, 90, 90, 220});
        }
        drawHighlightedLine(lines[i], areaX, ly, fs);
    }
    if (hoveredErrorLine >= 0 && m_scriptEdErrors.count(hoveredErrorLine)) {
        const std::string& msg = m_scriptEdErrors[hoveredErrorLine];
        int tw = MeasureText(msg.c_str(), 12);
        int tx = (int)mouse.x + 14, ty2 = (int)mouse.y + 10;
        DrawRectangle(tx, ty2, tw + 16, 24, {40, 15, 15, 240});
        DrawRectangleLines(tx, ty2, tw + 16, 24, Color{230, 90, 90, 255});
        DrawText(msg.c_str(), tx + 8, ty2 + 5, 12, Color{255, 200, 200, 255});
    }
    // Cursor
    m_scriptEdBlink += GetFrameTime();
    if (fmodf(m_scriptEdBlink, 1.0f) < 0.65f &&
        m_scriptEdCurLine >= m_scriptEdScroll && m_scriptEdCurLine < m_scriptEdScroll + visLines) {
        int cx = areaX + MeasureText(lines[m_scriptEdCurLine].substr(0, m_scriptEdCurCol).c_str(), fs);
        int cy = areaY + (m_scriptEdCurLine - m_scriptEdScroll) * lineH;
        DrawRectangle(cx, cy - 1, 2, lineH - 2, ACCENT);
    }
    EndScissorMode();

    // Scrollbar
    if ((int)lines.size() > visLines) {
        float trackH = (float)areaH;
        float thumbH = std::max(20.0f, trackH * visLines / (float)lines.size());
        float thumbY = areaY + (trackH - thumbH) * m_scriptEdScroll / std::max(1, (int)lines.size() - visLines);
        DrawRectangle(m_screenW - 18, areaY, 5, (int)trackH, {40, 40, 50, 255});
        DrawRectangle(m_screenW - 18, (int)thumbY, 5, (int)thumbH, {110, 110, 130, 255});
    }

    // ── Hint bar ──
    int hy = m_screenH - hintBarH - 8;
    DrawRectangle(12, hy, m_screenW - 24, hintBarH, {22, 22, 32, 245});
    DrawRectangleLines(12, hy, m_screenW - 24, hintBarH, {70, 70, 85, 255});
    if (!m_scriptEdHints.empty()) {
        int hx = 20;
        for (size_t i = 0; i < m_scriptEdHints.size(); ++i) {
            std::string label = m_scriptEdHints[i];
            while (!label.empty() && label.back() == ' ') label.pop_back();
            Color c = (i == 0) ? ACCENT : Color{180, 180, 200, 255};
            if (i == 0) {
                int w = MeasureText(label.c_str(), 13);
                DrawRectangle(hx - 4, hy + 6, w + 8, 20, ColorAlpha(ACCENT, 0.15f));
            }
            DrawText(label.c_str(), hx, hy + 9, 13, c);
            hx += MeasureText(label.c_str(), 13) + 22;
            if (hx > m_screenW - 160) break;
        }
        DrawText(T("Tab"), m_screenW - 60, hy + 9, 12, GRAY);
        DrawText(m_scriptEdHintDoc.c_str(), 20, hy + 34, 12, {160, 200, 160, 255});
    } else {
        DrawText(T("Type to see completions — keywords, country./province./var./array./list. refs, waitUntil, include ..."),
                 20, hy + 20, 12, GRAY);
    }
}

// Renames a project script and rewrites every `include "oldname"` (with or
// without .txt) reference to the new name across the OTHER project scripts,
// so a rename never silently breaks an include the way a manual file rename
// on disk would.
void MapEditor::renameScript(const std::string& oldName, const std::string& newName) {
    auto it = m_scripts.find(oldName);
    if (it == m_scripts.end()) return;
    std::string content = it->second;
    m_scripts.erase(it);
    m_scripts[newName] = content;

    auto stripTxt = [](std::string n) {
        if (n.size() > 4 && n.substr(n.size() - 4) == ".txt") n.resize(n.size() - 4);
        return n;
    };
    std::string oldBase = stripTxt(oldName), newBase = stripTxt(newName);

    int refsUpdated = 0;
    for (auto& [sname, scriptContent] : m_scripts) {
        std::istringstream ss(scriptContent);
        std::string line;
        std::vector<std::string> outLines;
        bool changed = false;
        while (std::getline(ss, line)) {
            std::string trimmed = line;
            size_t s = trimmed.find_first_not_of(" \t");
            if (s != std::string::npos && trimmed.compare(s, 7, "include") == 0) {
                size_t q1 = line.find('"', s);
                size_t q2 = (q1 == std::string::npos) ? std::string::npos : line.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    std::string arg = line.substr(q1 + 1, q2 - q1 - 1);
                    std::string argBase = stripTxt(arg);
                    bool hadTxt = arg.size() > argBase.size();
                    if (argBase == oldBase) {
                        std::string replacement = hadTxt ? (newBase + ".txt") : newBase;
                        line = line.substr(0, q1 + 1) + replacement + line.substr(q2);
                        changed = true;
                        refsUpdated++;
                    }
                }
            }
            outLines.push_back(line);
        }
        if (changed) {
            std::string rebuilt;
            for (size_t i = 0; i < outLines.size(); ++i) {
                rebuilt += outLines[i];
                if (i + 1 < outLines.size()) rebuilt += '\n';
            }
            scriptContent = rebuilt;
        }
    }

    // If the renamed script is open in the editor right now, follow the rename
    if (m_scriptEdName == oldName) m_scriptEdName = newName;

    if (refsUpdated > 0) {
        m_saveStatus = "Renamed — updated " + std::to_string(refsUpdated) + " include reference" +
                       (refsUpdated == 1 ? "" : "s");
        m_saveStatusTimer = 2.5f;
    }
    trackChange();
}

void MapEditor::drawScriptDocsOverlay() {
    Vector2 mouse = GetMousePosition();
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 225});
    const int nDocs = (int)(sizeof(SCRIPT_DOCS) / sizeof(SCRIPT_DOCS[0]));

    DrawText(T("Scripting Documentation"), 16, 14, 22, ACCENT);
    Rectangle closeBtn = {(float)(m_screenW - 44), 8, 36, 36};
    bool closeHov = CheckCollisionPointRec(mouse, closeBtn);
    DrawRectangleRounded(closeBtn, 0.2f, 6, {60, 60, 70, 180});
    DrawRectangleRoundedLines(closeBtn, 0.2f, 6, {180, 180, 180, 200});
    DrawText("X", (int)closeBtn.x + 13, 14, 20, {180, 180, 180, 200});
    if (IsKeyPressed(KEY_ESCAPE) || (closeHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))) {
        m_scriptDocsOpen = false;
        return;
    }

    // ── Left: topic list ──
    const int listX = 16, listY = 56, listW = 260;
    const int rowH = 26;
    const int listH = m_screenH - listY - 16;
    int visRows = listH / rowH;
    Rectangle listRect = {(float)listX, (float)listY, (float)listW, (float)listH};
    if (CheckCollisionPointRec(mouse, listRect))
        m_scriptDocsScroll -= (int)GetMouseWheelMove();
    m_scriptDocsScroll = std::max(0, std::min(m_scriptDocsScroll, std::max(0, nDocs - visRows)));
    DrawRectangle(listX, listY, listW, listH, {18, 18, 25, 255});
    DrawRectangleLines(listX, listY, listW, listH, {60, 60, 75, 255});
    for (int i = m_scriptDocsScroll; i < nDocs && i < m_scriptDocsScroll + visRows; ++i) {
        int yi = listY + (i - m_scriptDocsScroll) * rowH;
        Rectangle row = {(float)listX, (float)yi, (float)listW, (float)(rowH - 1)};
        bool sel = (i == m_scriptDocsSel);
        bool hov = CheckCollisionPointRec(mouse, row);
        if (sel || hov) DrawRectangle((int)row.x, (int)row.y, (int)row.width, (int)row.height,
                                      sel ? ColorAlpha(ACCENT, 0.2f) : Color{255,255,255,10});
        DrawText(SCRIPT_DOCS[i].keyword, listX + 10, yi + 5, 13, sel ? ACCENT : WHITE);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) { m_scriptDocsSel = i; m_scriptDocsScroll = m_scriptDocsScroll; }
    }
    if (IsKeyPressed(KEY_DOWN)) m_scriptDocsSel = std::min(m_scriptDocsSel + 1, nDocs - 1);
    if (IsKeyPressed(KEY_UP)) m_scriptDocsSel = std::max(m_scriptDocsSel - 1, 0);
    if (m_scriptDocsSel < m_scriptDocsScroll) m_scriptDocsScroll = m_scriptDocsSel;
    if (m_scriptDocsSel >= m_scriptDocsScroll + visRows) m_scriptDocsScroll = m_scriptDocsSel - visRows + 1;
    static int lastDocsSel = -1;
    if (lastDocsSel != m_scriptDocsSel) { m_scriptDocsBodyScroll = 0; lastDocsSel = m_scriptDocsSel; }

    // ── Right: body text, wrapped and scrollable via the topic's own content ──
    const int bodyX = listX + listW + 20;
    const int bodyY = listY;
    const int bodyW = m_screenW - bodyX - 16;
    const int bodyH = listH;
    DrawRectangle(bodyX, bodyY, bodyW, bodyH, {14, 14, 20, 255});
    DrawRectangleLines(bodyX, bodyY, bodyW, bodyH, {60, 60, 75, 255});
    if (m_scriptDocsSel >= 0 && m_scriptDocsSel < nDocs) {
        const DocEntry& d = SCRIPT_DOCS[m_scriptDocsSel];
        DrawText(d.keyword, bodyX + 16, bodyY + 12, 20, WHITE);
        DrawLine(bodyX + 16, bodyY + 40, bodyX + bodyW - 16, bodyY + 40, Color{60,60,75,255});
        // Split on '\n' and wrap each logical line to bodyW
        const int fs = 14, lh = 19;
        std::string cur;
        std::vector<std::string> outLines;
        for (const char* p = d.body; ; ++p) {
            if (*p == '\n' || *p == '\0') {
                // word-wrap `cur` into bodyW-32
                std::string word, line;
                std::istringstream iss(cur);
                while (iss >> word) {
                    std::string trial = line.empty() ? word : line + " " + word;
                    if (MeasureText(trial.c_str(), fs) > bodyW - 32 && !line.empty()) {
                        outLines.push_back(line);
                        line = word;
                    } else {
                        line = trial;
                    }
                }
                outLines.push_back(line); // keep blank lines too (paragraph spacing)
                cur.clear();
                if (*p == '\0') break;
            } else {
                cur += *p;
            }
        }
        Rectangle bodyTextRect = {(float)bodyX, (float)(bodyY + 48), (float)bodyW, (float)(bodyH - 60)};
        int visBodyLines = (int)bodyTextRect.height / lh;
        if (CheckCollisionPointRec(mouse, bodyTextRect))
            m_scriptDocsBodyScroll -= (int)GetMouseWheelMove();
        m_scriptDocsBodyScroll = std::max(0, std::min(m_scriptDocsBodyScroll,
                                                       std::max(0, (int)outLines.size() - visBodyLines)));
        BeginScissorMode((int)bodyTextRect.x, (int)bodyTextRect.y, (int)bodyTextRect.width, (int)bodyTextRect.height);
        int ty = bodyY + 56;
        for (int i = m_scriptDocsBodyScroll; i < (int)outLines.size(); ++i) {
            DrawText(outLines[i].c_str(), bodyX + 16, ty, fs, Color{215, 215, 225, 255});
            ty += lh;
        }
        EndScissorMode();
    }
    DrawText(T("Up/Down to browse topics, ESC to close"), bodyX, m_screenH - 24, 11, GRAY);
}

// ── Metadata Panel ─────────────────────────────────────────────

static const char* MONTH_NAMES[] = {"January","February","March","April","May","June",
                                    "July","August","September","October","November","December"};

void MapEditor::syncDateFromString() {
    char mb[32] = {};
    int yr = 2000;
    int scanned = sscanf(m_mapDate.c_str(), "%31s %d", mb, &yr);
    m_dateMonth = 0;
    for (int i = 0; i < 12; ++i) if (scanned >= 1 && strcmp(mb, MONTH_NAMES[i]) == 0) { m_dateMonth = i; break; }
    m_dateYearText = std::to_string(scanned >= 2 ? yr : 2000);
    m_dateBC = m_mapDate.find("BC") != std::string::npos;
}

void MapEditor::syncDateToString() {
    int yr = atoi(m_dateYearText.c_str());
    if (yr <= 0) yr = 1;
    m_mapDate = std::string(MONTH_NAMES[m_dateMonth]) + " " + std::to_string(yr) + " " + (m_dateBC ? "BC" : "AD");
}

void MapEditor::updateMetadataPanel() {}
void MapEditor::drawMetadataPanel() {
    int px = m_screenW - m_panelW + 12, y = m_toolbarH + 16;
    int listW = m_panelW - 24;
    DrawText(T("Map Metadata"), px, y, 18, ACCENT); y += 30;

    bool inputOk = !anyModalOpen();
    Vector2 mouse = GetMousePosition();

    struct Field { const char* label; std::string* value; };
    Field fields[] = {
        {"Name:", &m_mapName},
        {"Author:", &m_author},
        {"", nullptr}, // slot 2: date picker (handled separately, not a text field)
        {"License name:", &m_license},
    };
    const int NUM_FIELDS = 4;

    auto commitField = [&]() {
        if (m_metaEditField >= 0 && m_metaEditField < NUM_FIELDS && fields[m_metaEditField].value) {
            std::string t = m_metaEditText;
            if (m_metaEditField == 0 && t.empty()) t = "New Map";       // name must not be empty
            if (*fields[m_metaEditField].value != t) {
                *fields[m_metaEditField].value = t;
                trackChange();
            }
        }
        m_metaEditField = -1;
    };

    Rectangle editingRect{};
    bool focusedThisFrame = false;
    auto drawField = [&](int f) {
        DrawText(fields[f].label, px, y, 12, LIGHTGRAY); y += 16;
        Rectangle rect = {(float)px, (float)y, (float)listW, 24};
        bool editing = (m_metaEditField == f);
        if (editing) editingRect = rect;
        bool hov = inputOk && CheckCollisionPointRec(mouse, rect);
        Color bg = editing ? Color{35,35,50,255} : (hov ? Color{30,30,40,255} : Color{25,25,35,255});
        DrawRectangleRounded(rect, 0.06f, 4, bg);
        DrawRectangleRoundedLines(rect, 0.06f, 4, editing ? ACCENT : Color{60,60,70,255});
        const char* text = editing ? m_metaEditText.c_str() : fields[f].value->c_str();
        DrawText(text, (int)rect.x + 6, (int)rect.y + 5, 13, editing ? ACCENT : WHITE);
        if (editing)
            DrawText("|", (int)(rect.x + 6 + MeasureText(text, 13)), (int)rect.y + 5, 13, ACCENT);
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !editing) {
            commitField();
            m_metaEditField = f;
            m_metaEditText = *fields[f].value;
            m_licenseTextFocus = false;
            focusedThisFrame = true;
        }
        y += 30;
    };

    drawField(0);
    drawField(1);

    // ── Start date: month picker + year field + AD/BC toggle ──
    DrawText(T("Start date:"), px, y, 12, LIGHTGRAY); y += 16;
    {
        int monthW = 110, yearW = 70, eraW = 42, gap = 6;
        Rectangle monthBtn = {(float)px, (float)y, (float)monthW, 24};
        Rectangle yearRect = {(float)(px + monthW + gap), (float)y, (float)yearW, 24};
        Rectangle eraBtn = {(float)(px + monthW + gap + yearW + gap), (float)y, (float)eraW, 24};

        // Month button (click opens a 12-row dropdown below)
        bool monthHov = inputOk && CheckCollisionPointRec(mouse, monthBtn);
        DrawRectangleRounded(monthBtn, 0.08f, 4, m_dateMonthDropdownOpen ? Color{35,35,50,255} : (monthHov ? Color{30,30,40,255} : Color{25,25,35,255}));
        DrawRectangleRoundedLines(monthBtn, 0.08f, 4, m_dateMonthDropdownOpen ? ACCENT : Color{60,60,70,255});
        DrawText(MONTH_NAMES[m_dateMonth], (int)monthBtn.x + 6, (int)monthBtn.y + 5, 13, WHITE);
        DrawText(m_dateMonthDropdownOpen ? "^" : "v", (int)(monthBtn.x + monthBtn.width - 16), (int)monthBtn.y + 5, 13, LIGHTGRAY);
        if (monthHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_dateMonthDropdownOpen = !m_dateMonthDropdownOpen;
            m_editingDateYear = false;
        }

        // Year field
        bool yearEditing = m_editingDateYear;
        bool yearHov = inputOk && CheckCollisionPointRec(mouse, yearRect);
        DrawRectangleRounded(yearRect, 0.08f, 4, yearEditing ? Color{35,35,50,255} : (yearHov ? Color{30,30,40,255} : Color{25,25,35,255}));
        DrawRectangleRoundedLines(yearRect, 0.08f, 4, yearEditing ? ACCENT : Color{60,60,70,255});
        const char* yearText = m_dateYearText.c_str();
        DrawText(yearText, (int)yearRect.x + 6, (int)yearRect.y + 5, 13, yearEditing ? ACCENT : WHITE);
        if (yearEditing) DrawText("|", (int)(yearRect.x + 6 + MeasureText(yearText, 13)), (int)yearRect.y + 5, 13, ACCENT);
        if (yearHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !yearEditing) {
            m_editingDateYear = true;
            m_dateMonthDropdownOpen = false;
        }

        // AD/BC toggle
        Rectangle eraBtn2 = eraBtn;
        if (drawButton(m_dateBC ? "BC" : "AD", eraBtn2, false, 12) && inputOk) {
            m_dateBC = !m_dateBC;
            syncDateToString();
            trackChange();
        }

        y += 28;

        // Year text editing
        if (m_editingDateYear) {
            int key = GetCharPressed();
            while (key > 0) {
                // Every character the field takes. Jittered, because a
                // typed word is a run of distinct taps, not one tap looped.
                Audio::get().playSfx("key_type", 0.12f);
                if (key >= '0' && key <= '9' && m_dateYearText.size() < 6)
                    m_dateYearText.push_back((char)key);
                key = GetCharPressed();
            }
            odTextEditKeys(m_dateYearText, 6, "", true);
            bool commit = IsKeyPressed(KEY_ENTER);
            bool cancel = IsKeyPressed(KEY_ESCAPE);
            bool clickAway = inputOk && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, yearRect);
            if (commit || clickAway) {
                if (m_dateYearText.empty() || atoi(m_dateYearText.c_str()) <= 0) m_dateYearText = "1";
                syncDateToString();
                trackChange();
                m_editingDateYear = false;
            } else if (cancel) {
                m_editingDateYear = false;
            }
        }

        // Month dropdown
        if (m_dateMonthDropdownOpen) {
            int rowH = 20, dropW = monthW;
            Rectangle dropRect = {(float)px, (float)y, (float)dropW, (float)(rowH * 12)};
            DrawRectangle((int)dropRect.x, (int)dropRect.y, (int)dropRect.width, (int)dropRect.height, Color{22,22,30,255});
            DrawRectangleLines((int)dropRect.x, (int)dropRect.y, (int)dropRect.width, (int)dropRect.height, ACCENT);
            for (int m = 0; m < 12; ++m) {
                Rectangle row = {(float)px, (float)(y + m * rowH), (float)dropW, (float)(rowH - 1)};
                bool rHov = inputOk && CheckCollisionPointRec(mouse, row);
                bool rSel = (m == m_dateMonth);
                if (rHov || rSel) DrawRectangle((int)row.x, (int)row.y, (int)row.width, (int)row.height,
                                                rSel ? ColorAlpha(ACCENT, 0.2f) : Color{255,255,255,10});
                DrawText(MONTH_NAMES[m], (int)row.x + 6, (int)row.y + 3, 12, rSel ? ACCENT : WHITE);
                if (rHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    m_dateMonth = m;
                    syncDateToString();
                    trackChange();
                    m_dateMonthDropdownOpen = false;
                }
            }
            y += rowH * 12;
        }
        y += 8;
        DrawText(TextFormat(T("Resolves to: %s"), m_mapDate.c_str()), px, y, 10, GRAY);
        y += 16;
    }

    // ── Thumbnail: shown in the map browser; optional, auto-generated if unset ──
    DrawText(T("Thumbnail:"), px, y, 12, LIGHTGRAY); y += 16;
    {
        // Preview box uses the exact export aspect so what you see is what ships
        float previewW = (float)std::min(listW, THUMB_W * 2);
        float previewH = previewW * THUMB_H / THUMB_W;
        Rectangle box = {(float)px, (float)y, previewW, previewH};
        bool boxHov = inputOk && CheckCollisionPointRec(mouse, box);
        DrawRectangleRounded(box, 0.04f, 4, Color{25,25,35,255});
        DrawRectangleRoundedLines(box, 0.04f, 4, boxHov ? ACCENT : Color{60,60,70,255});

        if (!m_thumbnailPath.empty()) {
            if (m_thumbnailTexPath != m_thumbnailPath) {
                if (m_thumbnailTex.id > 0) UnloadTexture(m_thumbnailTex);
                m_thumbnailTex = LoadTexture(m_thumbnailPath.c_str());
                m_thumbnailTexPath = m_thumbnailPath;
            }
            if (m_thumbnailTex.id > 0) {
                DrawTexturePro(m_thumbnailTex,
                               {0, 0, (float)m_thumbnailTex.width, (float)m_thumbnailTex.height},
                               {box.x + 2, box.y + 2, box.width - 4, box.height - 4}, {0, 0}, 0, WHITE);
            } else {
                DrawText(T("Could not read image"), (int)box.x + 8, (int)(box.y + box.height/2 - 6), 11, Color{220,140,140,255});
            }
        } else {
            const char* t1 = "Auto-generated from the map";
            int t1w = MeasureText(t1, 11);
            DrawText(t1, (int)(box.x + box.width/2 - t1w/2), (int)(box.y + box.height/2 - 6), 11, GRAY);
        }
        y += (int)previewH + 6;

        DrawText(TextFormat(T("Drop an image here — %dx%d PNG"), THUMB_W, THUMB_H), px, y, 10, Color{150,170,150,220});
        y += 12;
        DrawText(T("Other sizes/formats are rescaled to fit."), px, y, 10, GRAY);
        y += 16;

        if (!m_thumbnailPath.empty()) {
            Rectangle clearBtn = {(float)px, (float)y, (float)listW, 20};
            if (drawButton("Use auto-generated instead", clearBtn, false, 11) && inputOk) {
                m_thumbnailPath.clear();
                m_thumbnailTexPath.clear();
                trackChange();
            }
            y += 24;
        }
        y += 6;
    }

    // ── License: pick a preset or define a custom one ──
    DrawText(T("License:"), px, y, 12, LIGHTGRAY); y += 16;
    static const char* presets[] = {"CC-BY-4.0", "CC-BY-SA-4.0", "CC0-1.0",
                                    "MIT", "GPL-3.0", "All Rights Reserved"};
    for (const char* p : presets) {
        Rectangle r = {(float)px, (float)y, (float)listW, 20};
        bool sel = !m_licenseCustom && m_license == p;
        if (drawButton(p, r, sel, 11) && inputOk && !sel) {
            m_licenseCustom = false;
            m_licenseTextFocus = false;
            m_license = p;
            m_licenseText.clear();
            trackChange();
        }
        y += 23;
    }
    Rectangle custBtn = {(float)px, (float)y, (float)listW, 20};
    if (drawButton("Custom license...", custBtn, m_licenseCustom, 11) && inputOk && !m_licenseCustom) {
        m_licenseCustom = true;
        m_license = "Custom";
        trackChange();
    }
    y += 26;

    if (m_licenseCustom) {
        drawField(3); // custom license name

        DrawText(TextFormat(T("License text (%d chars):"), (int)m_licenseText.size()), px, y, 11, LIGHTGRAY); y += 14;
        Rectangle ta = {(float)px, (float)y, (float)listW, 84};
        bool taHov = inputOk && CheckCollisionPointRec(mouse, ta);
        Color bg = m_licenseTextFocus ? Color{35,35,50,255} : (taHov ? Color{30,30,40,255} : Color{25,25,35,255});
        DrawRectangleRounded(ta, 0.04f, 4, bg);
        DrawRectangleRoundedLines(ta, 0.04f, 4, m_licenseTextFocus ? ACCENT : Color{60,60,70,255});
        if (inputOk && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (taHov) {
                commitField();
                m_licenseTextFocus = true;
            } else {
                m_licenseTextFocus = false;
            }
        }
        if (m_licenseTextFocus) {
            const size_t MAX_LICENSE = 20000;
            int key = GetCharPressed();
            while (key > 0) {
                // Every character the field takes. Jittered, because a
                // typed word is a run of distinct taps, not one tap looped.
                Audio::get().playSfx("key_type", 0.12f);
                if (key >= 32 && key < 128 && m_licenseText.size() < MAX_LICENSE) {
                    m_licenseText.push_back((char)key);
                    trackChange();
                }
                key = GetCharPressed();
            }
            if (IsKeyPressed(KEY_ENTER) && m_licenseText.size() < MAX_LICENSE) {
                m_licenseText.push_back('\n');
                trackChange();
            }
            if (odTextEditKeys(m_licenseText, 200)) {
                
                trackChange();
            }
            if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                 IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER)) && IsKeyPressed(KEY_V)) {
                const char* clip = GetClipboardText();
                if (clip && *clip) {
                    m_licenseText += clip;
                    if (m_licenseText.size() > MAX_LICENSE) m_licenseText.resize(MAX_LICENSE);
                    trackChange();
                }
            }
        }
        // Preview: tail of the text (last 5 lines, clipped to the box width)
        {
            std::vector<std::string> lines(1);
            for (char ch : m_licenseText) {
                if (ch == '\n') lines.emplace_back();
                else lines.back().push_back(ch);
            }
            int start = std::max(0, (int)lines.size() - 5);
            int ly = (int)ta.y + 6;
            for (int i = start; i < (int)lines.size(); ++i) {
                std::string ln = lines[i];
                while (!ln.empty() && MeasureText(ln.c_str(), 10) > listW - 14) ln.pop_back();
                DrawText(ln.c_str(), px + 6, ly, 10, WHITE);
                if (i == (int)lines.size() - 1 && m_licenseTextFocus)
                    DrawText("|", px + 6 + MeasureText(ln.c_str(), 10), ly, 10, ACCENT);
                ly += 14;
            }
        }
        y += 88;
        DrawText(T("Type or paste (Cmd/Ctrl+V). Enter = newline"), px, y, 9, GRAY);
        y += 16;
    }

    // Shared keyboard handling for the focused single-line field
    if (m_metaEditField >= 0 && m_metaEditField < NUM_FIELDS) {
        int key = GetCharPressed();
        while (key > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            if (key >= 32 && key < 128 && m_metaEditText.size() < 60)
                m_metaEditText.push_back((char)key);
            key = GetCharPressed();
        }
        odTextEditKeys(m_metaEditText, 60);
        if (IsKeyPressed(KEY_ENTER)) commitField();
        else if (IsKeyPressed(KEY_ESCAPE)) m_metaEditField = -1;
        else if (!focusedThisFrame && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                 !CheckCollisionPointRec(mouse, editingRect)) commitField();
    }

    y += 6;
    Rectangle saveBtn = {(float)px, (float)y, (float)listW, 30};
    if (drawButton("Save Project (Ctrl+S)", saveBtn, false, 13) && inputOk) {
        saveProject();
    }
    y += 38;
    // Import/export sit next to Save because this is the panel about the map
    // as a thing you hand to someone, not about the map as terrain.
    if (fileDialog::available()) {
        Rectangle expBtn = {(float)px, (float)y, (float)listW, 28};
        if (drawButton("Export to File...", expBtn, false, 13) && inputOk) promptExportToFile();
        y += 34;
        Rectangle impBtn = {(float)px, (float)y, (float)listW, 28};
        if (drawButton("Import Map File...", impBtn, false, 13) && inputOk) promptImportFromFile();
        y += 38;
    }
    if (m_saveStatusTimer > 0 && !m_saveStatus.empty()) {
        DrawText(m_saveStatus.c_str(), px, y, 12, Color{140, 220, 140, 255});
        y += 18;
    }
    if (m_warningTimer > 0 && !m_warningMsg.empty()) {
        DrawText(m_warningMsg.c_str(), px, y, 12, Color{255, 120, 120, 255});
        y += 18;
    }
    DrawText(T("Saved as .uodmap in data/projects/"), px, y, 10, GRAY); y += 13;
    DrawText(T("Export .odmap (Generator tab) to play."), px, y, 10, GRAY); y += 13;
    if (fileDialog::available())
        DrawText(T("Or drop an .odmap/.uodmap on this window."), px, y, 10, GRAY);
}