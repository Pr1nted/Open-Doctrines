#include "Game.h"
#include "Audio.h"
#include "SaveManager.h"
#include "GameInternals.h"
#include "Keybinds.h"
#include "raymath.h"
#include "miniz.h"
#include "miniz_zip.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <fstream>
#include <cstdio>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <ctime>

// ─── Thumbnail cache ─────────────────────────────────────
void Game::clearThumbCache() {
    for (auto& [path, tex] : m_thumbCache) {
        if (tex.id > 0) UnloadTexture(tex);
    }
    m_thumbCache.clear();
}

// ─── Save / change tracking ──────────────────────────────
bool Game::trySaveGame() {
    if (m_currentSavePath.empty()) {
        m_saveFeedback = "No save file active!";
        m_saveFeedbackTimer = 2.0f;
        return false;
    }

    // Sync current treasuries into save metadata before writing
    SaveMetadata meta = SaveManager::readMetadata(m_currentSavePath);
    for (auto& [cid, c] : m_countries.getAll())
        meta.countryTreasuries[cid] = c.treasury;

    // Update metadata with treasury + timestamp
    if (!SaveManager::updateLastPlayed(m_currentSavePath, &meta)) {
        m_saveFeedback = "Failed to save game!";
        m_saveFeedbackTimer = 2.0f;
        return false;
    }
    // Save full state snapshot
    {
        std::vector<std::pair<std::string, std::string>> rebelFiles;
        for (auto& [cid2, svg] : m_rebelFlagSvgs)
            rebelFiles.push_back({"rebellion/" + std::to_string(cid2) + ".svg", svg});
        // Persist the rebel countries themselves, not just their flags —
        // otherwise their provinces reload as ownerless limbo.
        { std::string rj = buildRebelsJson(); if (!rj.empty()) rebelFiles.push_back({"rebels.json", rj}); }
        SaveManager::writeState(m_currentSavePath, saveStateJson(), rebelFiles);
    }

    m_unsavedChanges = false;
    m_autoCreatedSave = false;
    m_saveFeedback = "Game saved successfully!";
    m_saveFeedbackTimer = 2.0f;
    std::cout << "Game saved: " << m_currentSavePath << std::endl;
    return true;
}

void Game::trackChange() {
    m_unsavedChanges = true;
}

// ─── Thumbnail loading ───────────────────────────────────
Texture2D Game::getThumbTexture(const std::string& path) {
    auto it = m_thumbCache.find(path);
    if (it != m_thumbCache.end() && it->second.id > 0)
        return it->second;

    // Load and cache
    Image img = LoadImage(path.c_str());
    if (img.data == nullptr) return Texture2D{0};
    ImageResize(&img, 160, 80);
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    m_thumbCache[path] = tex;
    return tex;
}

Texture2D Game::getThumbTextureFromODM(const std::string& odmPath) {
    auto it = m_thumbCache.find(odmPath);
    if (it != m_thumbCache.end() && it->second.id > 0)
        return it->second;

    Texture2D tex{0};
    unsigned char* zipData = nullptr;
    int zipSize = 0;
    zipData = LoadFileData(odmPath.c_str(), &zipSize);
    if (zipData && zipSize > 0) {
        mz_zip_archive zip{};
        if (mz_zip_reader_init_mem(&zip, zipData, zipSize, 0)) {
            int idx = mz_zip_reader_locate_file(&zip, "thumb.png", nullptr, 0);
            if (idx >= 0) {
                size_t sz = 0;
                void* pngData = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
                if (pngData && sz > 0) {
                    Image img = LoadImageFromMemory(".png", (const unsigned char*)pngData, (int)sz);
                    if (img.data) {
                        ImageResize(&img, 160, 80);
                        tex = LoadTextureFromImage(img);
                        UnloadImage(img);
                    }
                    mz_free(pngData);
                }
            }
            mz_zip_reader_end(&zip);
        }
        RL_FREE(zipData);
    }
    m_thumbCache[odmPath] = tex;
    return tex;
}

// ─── Map entry loading ───────────────────────────────────
void Game::loadMapEntries() {
    m_mapEntries.clear();
    clearThumbCache();

    // Load standard maps from maps_index.json
    std::string indexPath = m_dataDir + "maps_index.json";
    std::ifstream f(indexPath);
    if (f) {
        try {
            auto j = nlohmann::json::parse(f);
            for (auto& entry : j) {
                MapEntry me;
                me.id = entry.value("id", "unknown");
                me.name = entry.value("name", me.id);
                me.filename = entry.value("filename", me.id + ".odmap");
                me.description = entry.value("description", "");
                me.author = entry.value("author", "");
                me.license = entry.value("license", "");
                me.hasScripts = entry.value("scripts", false);
                me.directory = m_dataDir + "STDmaps/";
                me.thumbPath = m_dataDir + "STDmaps/" + entry.value("thumb", me.id + "_thumb.png");
                me.isStandard = true;
                m_mapEntries.push_back(me);
            }
        } catch (std::exception& e) {
            std::cerr << "Failed to parse maps_index.json: " << e.what() << std::endl;
        }
    } else {
        // Fallback: scan STDmaps/ for .odmap files
        std::string stdDir = m_dataDir + "STDmaps/";
        DIR* dir = opendir(stdDir.c_str());
        if (dir) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != nullptr) {
                std::string name = ent->d_name;
#ifndef _WIN32
                if (ent->d_type == DT_DIR) continue;
#endif
                if (name == "." || name == "..") continue;
                if (name.size() < 6 || name.substr(name.size() - 6) != ".odmap") continue;

                std::string fullPath = stdDir + name;
                std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
                if (!file) continue;
                std::streamsize fsize = file.tellg();
                file.seekg(0, std::ios::beg);
                if (fsize < 22) { file.close(); continue; }
                std::vector<uint8_t> zipData(fsize);
                if (!file.read(reinterpret_cast<char*>(zipData.data()), fsize)) { file.close(); continue; }
                file.close();

                mz_zip_archive zip{};
                if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0)) continue;

                MapEntry me;
                me.id = name.substr(0, name.size() - 6);
                me.name = me.id;
                me.filename = name;
                me.directory = stdDir;
                me.thumbPath = me.directory + me.id + "_thumb.png";
                me.isStandard = true;
                me.description = "";
                me.author = "";
                me.license = "";
                me.hasScripts = false;

                int metaIdx = mz_zip_reader_locate_file(&zip, "metadata.json", nullptr, 0);
                if (metaIdx >= 0) {
                    size_t metaSize = 0;
                    char* metaData = (char*)mz_zip_reader_extract_to_heap(&zip, metaIdx, &metaSize, 0);
                    if (metaData && metaSize > 0) {
                        try {
                            auto j = nlohmann::json::parse(metaData, metaData + metaSize);
                            if (j.contains("name") && j["name"].is_string())
                                me.name = j["name"];
                            if (j.contains("author") && j["author"].is_string())
                                me.author = j["author"];
                            if (j.contains("license") && j["license"].is_string())
                                me.license = j["license"];
                            if (j.contains("has_scripts") && j["has_scripts"].is_boolean())
                                me.hasScripts = j["has_scripts"];
                            if (j.contains("description") && j["description"].is_string())
                                me.description = j["description"];
                        } catch (...) {}
                        mz_free(metaData);
                    }
                }

                mz_zip_reader_end(&zip);
                m_mapEntries.push_back(me);
            }
            closedir(dir);
        }
    }

    // Scan custom maps directory
    std::string customDir = m_dataDir + "custom_maps/";
    DIR* dir = opendir(customDir.c_str());
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string id = ent->d_name;
            if (id == "." || id == "..") continue;

            bool isDir;
#ifdef _WIN32
            struct stat st;
            std::string fullPath = customDir + id;
            isDir = (stat(fullPath.c_str(), &st) == 0 && (st.st_mode & S_IFDIR));
#else
            isDir = (ent->d_type == DT_DIR);
#endif

            // A bare "<name>.odmap" dropped in here (which is exactly what the
            // map editor's Export produces) is a valid custom map too — read
            // its metadata straight out of the archive. Without this, exported
            // maps sat in the folder but never appeared in the browser.
            if (!isDir) {
                if (id.size() < 7 || id.substr(id.size() - 6) != ".odmap") continue;

                MapEntry me;
                me.id = id.substr(0, id.size() - 6);
                me.name = me.id;
                me.filename = id;
                me.directory = customDir;
                me.thumbPath = customDir + me.id + "_thumb.png"; // fallback; thumb.png inside the zip wins
                me.isStandard = false;
                me.isLooseFile = true;

                std::string odmPath = customDir + id;
                int zipSize = 0;
                unsigned char* zipData = LoadFileData(odmPath.c_str(), &zipSize);
                if (zipData && zipSize > 22) {
                    mz_zip_archive zip{};
                    if (mz_zip_reader_init_mem(&zip, zipData, zipSize, 0)) {
                        int metaIdx = mz_zip_reader_locate_file(&zip, "metadata.json", nullptr, 0);
                        if (metaIdx >= 0) {
                            size_t metaSize = 0;
                            char* metaData = (char*)mz_zip_reader_extract_to_heap(&zip, metaIdx, &metaSize, 0);
                            if (metaData && metaSize > 0) {
                                try {
                                    auto j = nlohmann::json::parse(metaData, metaData + metaSize);
                                    if (j.contains("name") && j["name"].is_string()) me.name = j["name"];
                                    if (j.contains("author") && j["author"].is_string()) me.author = j["author"];
                                    if (j.contains("license") && j["license"].is_string()) me.license = j["license"];
                                    if (j.contains("description") && j["description"].is_string()) me.description = j["description"];
                                    if (j.contains("has_scripts") && j["has_scripts"].is_boolean()) me.hasScripts = j["has_scripts"];
                                } catch (...) {}
                                mz_free(metaData);
                            }
                        }
                        mz_zip_reader_end(&zip);
                    }
                }
                if (zipData) RL_FREE(zipData);
                m_mapEntries.push_back(me);
                continue;
            }

            MapEntry me;
            me.id = id;
            me.directory = customDir + id + "/";
            me.thumbPath = me.directory + "map_thumb.png";
            me.isStandard = false;

            // Load meta.json
            std::string metaPath = me.directory + "meta.json";
            std::ifstream mf(metaPath);
            if (mf) {
                try {
                    auto j = nlohmann::json::parse(mf);
                    me.name = j.value("name", id);
                    me.description = j.value("description", "");
                    me.author = j.value("author", "");
                    me.license = j.value("license", "");
                    me.hasScripts = j.value("hasScripts", false);
                    me.filename = j.value("filename", "map.odmap");
                } catch (...) {}
            } else {
                me.name = id;
                me.filename = "map.odmap";
            }
            m_mapEntries.push_back(me);
        }
        closedir(dir);
    }
}

// ─── World browser ───────────────────────────────────────
void Game::drawWorldBrowser() {
    int centerX = m_screenW / 2;

    drawMenuBackground();
    DrawRectangleGradientV(0, 0, m_screenW, 100, {0, 0, 0, 200}, {0, 0, 0, 0});

    // Title
    const char* title = "Select World";
    int titleSize = 40;
    int titleW = MeasureText(title, titleSize);
    DrawText(title, centerX - titleW / 2, 40, titleSize, hexToColor(m_config.accentColor));

    if (m_showDeleteConfirm) {
        // ── Delete confirmation dialog ──
        DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
        int dlgW = 500, dlgH = 180;
        int dlgX = (m_screenW - dlgW) / 2;
        int dlgY = (m_screenH - dlgH) / 2;
        DrawRectangle(dlgX, dlgY, dlgW, dlgH, {20, 20, 30, 240});

        const char* msg = "Delete this world permanently?";
        int msgSize = 24;
        int msgW = MeasureText(msg, msgSize);
        DrawText(msg, centerX - msgW / 2, dlgY + 30, msgSize, WHITE);

        std::string sub = m_worldInfos[m_deleteWorldIndex].worldName;
        int subSize = 18;
        int subW = MeasureText(sub.c_str(), subSize);
        DrawText(sub.c_str(), centerX - subW / 2, dlgY + 65, subSize, hexToColor(m_config.accentColor));

        // Buttons
        int btnW = 120, btnH = 40;
        int btnY = dlgY + dlgH - 60;
        Vector2 mouse = getMouse();

        // Delete button (red)
        Rectangle delBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        bool delHov = CheckCollisionPointRec(mouse, delBtn);
        DrawRectangleRounded(delBtn, 0.2f, 8, delHov ? Color{180, 40, 40, 255} : Color{120, 30, 30, 255});
        int delW = MeasureText("Delete", 20);
        DrawText("Delete", (int)(delBtn.x + (btnW - delW) / 2), (int)(delBtn.y + 10), 20, WHITE);

        // Cancel button
        Rectangle canBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};
        bool canHov = CheckCollisionPointRec(mouse, canBtn);
        DrawRectangleRounded(canBtn, 0.2f, 8, canHov ? Color{80, 80, 90, 255} : Color{50, 50, 60, 255});
        int canW = MeasureText("Cancel", 20);
        DrawText("Cancel", (int)(canBtn.x + (btnW - canW) / 2), (int)(canBtn.y + 10), 20, WHITE);

        return;
    }

    if (m_showWorldSettings) {
        // ── World settings overlay ──
        DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
        int dlgW = 500, dlgH = 340;
        int dlgX = (m_screenW - dlgW) / 2;
        int dlgY = (m_screenH - dlgH) / 2;
        DrawRectangle(dlgX, dlgY, dlgW, dlgH, {20, 20, 30, 240});

        auto& wi = m_worldInfos[m_worldSettingsIndex];
        DrawText("World Settings", centerX - MeasureText("World Settings", 28) / 2, dlgY + 20, 28, hexToColor(m_config.accentColor));

        int yOff = dlgY + 70;
        int fs = 18;
        DrawText(("Name: " + wi.worldName).c_str(), dlgX + 30, yOff, fs, WHITE); yOff += 30;
        DrawText(("File: " + wi.filename).c_str(), dlgX + 30, yOff, fs, LIGHTGRAY); yOff += 30;
        DrawText(("Version: " + wi.version).c_str(), dlgX + 30, yOff, fs, LIGHTGRAY); yOff += 30;
        DrawText(("Last Played: " + wi.lastPlayed).c_str(), dlgX + 30, yOff, fs, LIGHTGRAY); yOff += 30;
        DrawText(("Turns: " + std::to_string(wi.turnCount)).c_str(), dlgX + 30, yOff, fs, LIGHTGRAY); yOff += 40;

        // Turn History — full-width row above the Rename/Close row
        Vector2 mouse = getMouse();
        int histW = 260, histH = 38;
        Rectangle histBtn = {(float)(centerX - histW / 2), (float)(dlgY + dlgH - 108), (float)histW, (float)histH};
        bool histHov = CheckCollisionPointRec(mouse, histBtn);
        DrawRectangleRounded(histBtn, 0.2f, 8, histHov ? ColorAlpha(hexToColor(m_config.accentColor), 0.30f)
                                                       : ColorAlpha(hexToColor(m_config.accentColor), 0.15f));
        DrawRectangleRoundedLines(histBtn, 0.2f, 8, hexToColor(m_config.accentColor));
        const char* histLabel = "Turn History / Timelapse";
        DrawText(histLabel, (int)(histBtn.x + (histW - MeasureText(histLabel, 18)) / 2),
                 (int)(histBtn.y + 10), 18, WHITE);

        int btnW = 120, btnH = 40;
        int btnY = dlgY + dlgH - 55;

        // Rename button
        Rectangle renameBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        bool renameHov = CheckCollisionPointRec(mouse, renameBtn);
        DrawRectangleRounded(renameBtn, 0.2f, 8, renameHov ? Color{50, 120, 80, 255} : Color{40, 80, 60, 255});
        int rw = MeasureText("Rename", 20);
        DrawText("Rename", (int)(renameBtn.x + (btnW - rw) / 2), (int)(renameBtn.y + 10), 20, WHITE);

        // Close button
        Rectangle closeBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};
        bool closeHov = CheckCollisionPointRec(mouse, closeBtn);
        DrawRectangleRounded(closeBtn, 0.2f, 8, closeHov ? Color{80, 80, 90, 255} : Color{50, 50, 60, 255});
        int cw = MeasureText("Close", 20);
        DrawText("Close", (int)(closeBtn.x + (btnW - cw) / 2), (int)(closeBtn.y + 10), 20, WHITE);

        return;
    }

    if (m_showRenameDialog) {
        // ── Rename dialog ──
        DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
        int dlgW = 520, dlgH = 220;
        int dlgX = (m_screenW - dlgW) / 2;
        int dlgY = (m_screenH - dlgH) / 2;
        DrawRectangle(dlgX, dlgY, dlgW, dlgH, {20, 20, 30, 240});

        Vector2 mouse = getMouse();

        const char* title2 = "Rename World";
        int titleW2 = MeasureText(title2, 28);
        DrawText(title2, centerX - titleW2 / 2, dlgY + 20, 28, hexToColor(m_config.accentColor));

        DrawText("Enter new name:", dlgX + 30, dlgY + 65, 16, LIGHTGRAY);

        // Text input box
        int inputX = dlgX + 30, inputY = dlgY + 90;
        int inputW = dlgW - 60, inputH = 36;
        DrawRectangleRounded({(float)inputX, (float)inputY, (float)inputW, (float)inputH}, 0.1f, 6, {40, 42, 55, 255});
        DrawRectangleRoundedLines({(float)inputX, (float)inputY, (float)inputW, (float)inputH}, 0.1f, 6, {100, 100, 120, 200});

        DrawText(m_renameWorldNewName.c_str(), inputX + 8, inputY + 8, 20, WHITE);

        // Blinking cursor
        if ((int)(GetTime() * 2) % 2 == 0) {
            int cursorX = inputX + 8 + MeasureText(m_renameWorldNewName.c_str(), 20);
            DrawRectangle(cursorX, inputY + 8, 2, 20, {200, 200, 210, 255});
        }

        // Confirm / Cancel buttons
        int btnW = 120, btnH = 40;
        int btnY = dlgY + dlgH - 58;
        bool canConfirm = !m_renameWorldNewName.empty();
        Rectangle confBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        bool confHov = CheckCollisionPointRec(mouse, confBtn) && canConfirm;
        DrawRectangleRounded(confBtn, 0.2f, 8, confHov ? Color{50, 120, 80, 255} : (canConfirm ? Color{40, 80, 60, 255} : Color{30, 40, 35, 200}));
        DrawText("Rename", (int)(confBtn.x + (btnW - MeasureText("Rename", 20)) / 2), (int)(confBtn.y + 10), 20, canConfirm ? WHITE : (Color){100, 100, 100, 200});

        Rectangle canBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};
        bool canHov = CheckCollisionPointRec(mouse, canBtn);
        DrawRectangleRounded(canBtn, 0.2f, 8, canHov ? Color{80, 80, 90, 255} : Color{50, 50, 60, 255});
        DrawText("Cancel", (int)(canBtn.x + (btnW - MeasureText("Cancel", 20)) / 2), (int)(canBtn.y + 10), 20, WHITE);

        // Warning about duplicate name
        std::string checkPath = m_dataDir + "saves/" + m_renameWorldNewName + ".odsv";
        struct stat chkStat;
        if (stat(checkPath.c_str(), &chkStat) == 0 && m_renameWorldNewName != m_renameWorldOldName) {
            std::string warn = "A save with this name exists - will add (1), (2), etc.";
            DrawText(warn.c_str(), dlgX + 30, dlgY + dlgH - 102, 12, hexToColor(m_config.accentColor));
        }

        return;
    }

    if (m_worldInfos.empty()) {
        const char* empty = "No worlds found. Start a new game and save it!";
        int emptyW = MeasureText(empty, 24);
        DrawText(empty, centerX - emptyW / 2, m_screenH / 2, 24, (Color){200, 100, 100, 255});
        // Back button
        int backSize = 24;
        const char* backLabel = "< Back";
        int backW = MeasureText(backLabel, backSize);
        DrawText(backLabel, 20, m_screenH - 50, backSize, (Color){200, 200, 210, 255});
        return;
    }

    // World list
    int count = (int)m_worldInfos.size();
    int itemH = 72;
    int startY = 90;
    int maxVisible = std::max(1, (m_screenH - startY - 60) / itemH);
    int maxScroll = std::max(0, count - maxVisible);
    m_fileScroll = std::clamp(m_fileScroll, 0, maxScroll);

    int visibleCount = std::min(count - m_fileScroll, maxVisible);
    DrawRectangle(20, startY - 8, m_screenW - 40, std::max(0, visibleCount) * itemH + 16, {0, 0, 0, 100});

    Vector2 mouse = getMouse();

    for (int i = 0; i < count; ++i) {
        int y = startY + (i - m_fileScroll) * itemH;
        if (y + itemH < 0 || y > m_screenH) continue;

        auto& wi = m_worldInfos[i];
        bool isSelected = (i == m_fileIndex);
        bool isHovered = CheckCollisionPointRec(mouse, {(float)20, (float)y, (float)(m_screenW - 40), (float)itemH});

        // Row background
        Color bg = isSelected ? Color{60, 70, 100, 40} : (isHovered ? Color{255, 255, 255, 10} : Color{0, 0, 0, 0});
        DrawRectangleRounded({20, (float)y, (float)(m_screenW - 40), (float)itemH}, 0.08f, 8, bg);

        // World name
        DrawText(wi.worldName.c_str(), 36, y + 6, 22, WHITE);

        // Date line
        std::string dates = "v" + wi.version + "  |  Played: " + wi.lastPlayed;
        DrawText(dates.c_str(), 36, y + 34, 13, (Color){160, 160, 170, 255});

        // Turn count
        std::string turns = std::to_string(wi.turnCount) + " turn" + (wi.turnCount == 1 ? "" : "s");
        int turnW = MeasureText(turns.c_str(), 13);
        DrawText(turns.c_str(), 40 + MeasureText(dates.c_str(), 13) + 20, y + 34, 13, ColorAlpha(hexToColor(m_config.accentColor), 200.0f/255.0f));

        // Gear button
        int btnSize = 32;
        int gearX = m_screenW - 50 - btnSize - 10;
        int gearY = y + (itemH - btnSize) / 2;
        Rectangle gearRect = {(float)gearX, (float)gearY, (float)btnSize, (float)btnSize};
        bool gearHov = CheckCollisionPointRec(mouse, gearRect);
        DrawRectangleRounded(gearRect, 0.3f, 6, gearHov ? Color{200, 200, 200, 40} : Color{0, 0, 0, 0});
        // Gear symbol (cog-like: circle + teeth)
        int gcx = gearX + btnSize / 2, gcy = gearY + btnSize / 2;
        DrawCircle(gcx, gcy, 7, gearHov ? (Color){220, 220, 230, 255} : (Color){160, 160, 170, 255});
        DrawCircle(gcx, gcy, 4, bg);  // subtract center
        for (int t = 0; t < 8; ++t) {
            float ang = t * 3.14159f / 4;
            int tx = gcx + (int)(9 * cosf(ang));
            int ty = gcy + (int)(9 * sinf(ang));
            DrawCircle(tx, ty, 2.5f, gearHov ? (Color){220, 220, 230, 255} : (Color){160, 160, 170, 255});
        }

        // Delete button (X)
        int delX = gearX - 10 - btnSize;
        Rectangle delRect = {(float)delX, (float)gearY, (float)btnSize, (float)btnSize};
        bool delHov = CheckCollisionPointRec(mouse, delRect);
        DrawRectangleRounded(delRect, 0.3f, 6, delHov ? Color{180, 50, 50, 40} : Color{0, 0, 0, 0});
        DrawText("x", delX + 11, gearY + 6, 20, delHov ? (Color){255, 80, 80, 255} : (Color){200, 100, 100, 255});

        // Selection highlight underline
        if (isSelected) {
            int nameW = MeasureText(wi.worldName.c_str(), 22);
            DrawRectangle(36, y + 30, nameW, 2, ColorAlpha(hexToColor(m_config.accentColor), 200.0f/255.0f));
        }
    }

    // Scroll indicators
    if (m_fileScroll > 0) {
        DrawText("^", centerX - 8, startY - 22, 18, (Color){160, 160, 170, 200});
    }
    if (m_fileScroll < maxScroll) {
        DrawText("v", centerX - 8, m_screenH - 55, 18, (Color){160, 160, 170, 200});
    }

    // Back button
    int backSize = 24;
    const char* backLabel = "< Back";
    int backW = MeasureText(backLabel, backSize);
    Rectangle backRect = {20, (float)(m_screenH - 48), (float)(backW + 24), (float)(backSize + 12)};
    bool backHov = CheckCollisionPointRec(mouse, backRect);
    DrawRectangleRounded(backRect, 0.1f, 8, backHov ? (Color){255, 255, 255, 20} : BLANK);
    DrawText(backLabel, 20 + 12, m_screenH - 48 + 6, backSize, (Color){200, 200, 210, 255});
}

void Game::updateWorldBrowser() {
    if (isMouseOverConsole()) return;
    if (m_showDeleteConfirm) {
        Vector2 mouse = getMouse();
        int btnW = 120, btnH = 40;
        int centerX = m_screenW / 2;
        int dlgH = 180;
        int btnY = (m_screenH - dlgH) / 2 + dlgH - 60;

        Rectangle delBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        Rectangle canBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};

        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (CheckCollisionPointRec(mouse, delBtn)) {
                Audio::get().playSfx("confirm");
                // Delete the world file
                std::string path = m_dataDir + "saves/" + m_worldInfos[m_deleteWorldIndex].filename;
                std::remove(path.c_str());
                m_worldInfos.erase(m_worldInfos.begin() + m_deleteWorldIndex);
                m_showDeleteConfirm = false;
                m_deleteWorldIndex = -1;
                if (m_worldInfos.empty()) {
                    m_fileScroll = 0;
                }
            } else if (CheckCollisionPointRec(mouse, canBtn)) {
                Audio::get().playSfx("back");
                m_showDeleteConfirm = false;
                m_deleteWorldIndex = -1;
            }
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_showDeleteConfirm = false;
            m_deleteWorldIndex = -1;
        }
        return;
    }

    if (m_showWorldSettings) {
        Vector2 mouse = getMouse();
        int centerX = m_screenW / 2;
        int dlgW = 500, dlgH = 340;
        int dlgY = (m_screenH - dlgH) / 2;
        int btnW = 120, btnH = 40;
        int btnY = dlgY + dlgH - 55;

        int histW = 260, histH = 38;
        Rectangle histBtn = {(float)(centerX - histW / 2), (float)(dlgY + dlgH - 108), (float)histW, (float)histH};
        Rectangle renameBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        Rectangle closeBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};

        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (CheckCollisionPointRec(mouse, histBtn)) {
                Audio::get().playSfx("click_light");
                std::string path = m_dataDir + "saves/" + m_worldInfos[m_worldSettingsIndex].filename;
                m_showWorldSettings = false;
                m_worldSettingsIndex = -1;
                openHistoryScreen(path);
            } else if (CheckCollisionPointRec(mouse, renameBtn)) {
                Audio::get().playSfx("click_light");
                // Open rename dialog
                m_renameWorldOldName = m_worldInfos[m_worldSettingsIndex].worldName;
                m_renameWorldNewName = m_renameWorldOldName;
                m_renameWorldIndex = m_worldSettingsIndex;
                m_showRenameDialog = true;
                m_showWorldSettings = false;
            } else if (CheckCollisionPointRec(mouse, closeBtn)) {
                Audio::get().playSfx("back");
                m_showWorldSettings = false;
                m_worldSettingsIndex = -1;
            }
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_showWorldSettings = false;
            m_worldSettingsIndex = -1;
        }
        return;
    }

    if (m_showRenameDialog) {
        Vector2 mouse = getMouse();
        int centerX = m_screenW / 2;

        // Handle text input
        int c = GetCharPressed();
        while (c > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            if (c >= 32 && c < 127 && m_renameWorldNewName.size() < 64) {
                // Sanitize: disallow path separators
                if (c != '/' && c != '\\' && c != ':')
                    m_renameWorldNewName += (char)c;
            }
            c = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !m_renameWorldNewName.empty())
            m_renameWorldNewName.pop_back();

        if (IsKeyPressed(KEY_ENTER) && !m_renameWorldNewName.empty()) {
            // Handle duplicate names by appending (1), (2), etc.
            std::string baseName = m_renameWorldNewName;
            std::string savePath = m_dataDir + "saves/" + m_renameWorldNewName + ".odsv";
            struct stat chkStat;
            int suffix = 1;
            while (stat(savePath.c_str(), &chkStat) == 0) {
                m_renameWorldNewName = baseName + " (" + std::to_string(suffix) + ")";
                savePath = m_dataDir + "saves/" + m_renameWorldNewName + ".odsv";
                suffix++;
            }

            // Rename the file
            std::string oldPath = m_dataDir + "saves/" + m_worldInfos[m_renameWorldIndex].filename;
            if (std::rename(oldPath.c_str(), savePath.c_str()) == 0) {
                // Update metadata with new name
                auto meta = SaveManager::readMetadata(savePath);
                meta.saveName = m_renameWorldNewName;
                // Rebuild archive with updated metadata
                std::vector<uint8_t> odmData = SaveManager::extractODM(savePath);
                if (!odmData.empty()) {
                    SaveManager::createSave(savePath, std::string(odmData.begin(), odmData.end()), meta);
                }
                // Update world info
                m_worldInfos[m_renameWorldIndex].worldName = m_renameWorldNewName;
                m_worldInfos[m_renameWorldIndex].filename = m_renameWorldNewName + ".odsv";
            }

            m_showRenameDialog = false;
            m_renameWorldOldName.clear();
            m_renameWorldNewName.clear();
            m_renameWorldIndex = -1;
            return;
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_showRenameDialog = false;
            m_renameWorldOldName.clear();
            m_renameWorldNewName.clear();
            m_renameWorldIndex = -1;
            return;
        }

        // Mouse button clicks for Rename / Cancel
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            int btnW = 120, btnH = 40;
            int btnY = (m_screenH - 220) / 2 + 220 - 58;
            Rectangle confBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
            Rectangle canBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};
            if (CheckCollisionPointRec(mouse, confBtn) && !m_renameWorldNewName.empty()) {
                Audio::get().playSfx("confirm");
                // Handle duplicate names by appending (1), (2), etc.
                std::string baseName = m_renameWorldNewName;
                std::string savePath = m_dataDir + "saves/" + m_renameWorldNewName + ".odsv";
                struct stat chkStat;
                int suffix = 1;
                while (stat(savePath.c_str(), &chkStat) == 0) {
                    m_renameWorldNewName = baseName + " (" + std::to_string(suffix) + ")";
                    savePath = m_dataDir + "saves/" + m_renameWorldNewName + ".odsv";
                    suffix++;
                }

                // Rename the file
                std::string oldPath = m_dataDir + "saves/" + m_worldInfos[m_renameWorldIndex].filename;
                if (std::rename(oldPath.c_str(), savePath.c_str()) == 0) {
                    // Update metadata with new name
                    auto meta = SaveManager::readMetadata(savePath);
                    meta.saveName = m_renameWorldNewName;
                    // Rebuild archive with updated metadata
                    std::vector<uint8_t> odmData = SaveManager::extractODM(savePath);
                    if (!odmData.empty()) {
                        SaveManager::createSave(savePath, std::string(odmData.begin(), odmData.end()), meta);
                    }
                    // Update world info
                    m_worldInfos[m_renameWorldIndex].worldName = m_renameWorldNewName;
                    m_worldInfos[m_renameWorldIndex].filename = m_renameWorldNewName + ".odsv";
                }

                m_showRenameDialog = false;
                m_renameWorldOldName.clear();
                m_renameWorldNewName.clear();
                m_renameWorldIndex = -1;
                return;
            }
            if (CheckCollisionPointRec(mouse, canBtn)) {
                Audio::get().playSfx("back");
                m_showRenameDialog = false;
                m_renameWorldOldName.clear();
                m_renameWorldNewName.clear();
                m_renameWorldIndex = -1;
                return;
            }
        }
        return;
    }

    if (m_worldInfos.empty()) {
        if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_currentScreen = SCREEN_SINGLEPLAYER;
        }
        return;
    }

    int count = (int)m_worldInfos.size();
    int itemH = 72;
    int startY = 90;
    int maxVisible = std::max(1, (m_screenH - startY - 60) / itemH);
    int maxScroll = std::max(0, count - maxVisible);

    if (IsKeyPressed(KEY_UP)) {
        m_fileIndex = (m_fileIndex + count - 1) % count;
        if (m_fileIndex < m_fileScroll) m_fileScroll = m_fileIndex;
    }
    if (IsKeyPressed(KEY_DOWN)) {
        m_fileIndex = (m_fileIndex + 1) % count;
        if (m_fileIndex >= m_fileScroll + maxVisible) m_fileScroll = m_fileIndex - maxVisible + 1;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        m_currentScreen = SCREEN_SINGLEPLAYER;
        return;
    }

    // Mouse wheel
    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        m_fileScroll -= (int)wheel;
        m_fileScroll = std::clamp(m_fileScroll, 0, maxScroll);
    }

    Vector2 mouse = getMouse();
    int btnSize = 32;

    // Back button click
    Rectangle backRect = {20, (float)(m_screenH - 48), (float)(MeasureText("< Back", 24) + 24), (float)(24 + 12)};
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, backRect)) {
        Audio::get().playSfx("back");
        m_currentScreen = SCREEN_SINGLEPLAYER;
        return;
    }

    // Check per-row buttons and clicks
    int clickRow = -1;
    bool gearClick = false, delClick = false;
    for (int i = 0; i < count; ++i) {
        int y = startY + (i - m_fileScroll) * itemH;
        if (y + itemH < 0 || y > m_screenH) continue;

        int gearX = m_screenW - 50 - btnSize - 10;
        int gearY = y + (itemH - btnSize) / 2;
        Rectangle gearRect = {(float)gearX, (float)gearY, (float)btnSize, (float)btnSize};

        int delX = gearX - 10 - btnSize;
        Rectangle delRect = {(float)delX, (float)gearY, (float)btnSize, (float)btnSize};

        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (CheckCollisionPointRec(mouse, gearRect)) {
                Audio::get().playSfx("click_light");
                m_worldSettingsIndex = i;
                m_showWorldSettings = true;
                return;
            }
            if (CheckCollisionPointRec(mouse, delRect)) {
                Audio::get().playSfx("click_light");
                m_deleteWorldIndex = i;
                m_showDeleteConfirm = true;
                return;
            }
            // Row click
            if (CheckCollisionPointRec(mouse, {(float)20, (float)y, (float)(m_screenW - 40), (float)itemH})) {
                clickRow = i;
            }
        }
    }

    // Activate: click or enter/space
    bool activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
    if (clickRow >= 0) {
        Audio::get().playSfx("click_light");
        m_fileIndex = clickRow;
        activate = true;
    }

    if (activate) {
        m_fileIndex = std::clamp(m_fileIndex, 0, count - 1);
        // Update last_played before loading
        std::string path = m_dataDir + "saves/" + m_worldInfos[m_fileIndex].filename;
        SaveManager::updateLastPlayed(path);
        startLoadedGame(m_worldInfos[m_fileIndex].filename);
    }
}

// ─── Native file dialog (macOS via osascript) ───────────
static std::string nativeOpenFileDialog(const std::string& title, const std::string& type) {
#ifdef __APPLE__
    std::string script = "osascript -e 'POSIX path of (choose file with prompt \"" + title + "\" of type {\"" + type + "\"})' 2>/dev/null";
    FILE* pipe = popen(script.c_str(), "r");
    if (!pipe) return "";
    char buf[4096];
    std::string result;
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
#else
    (void)title; (void)type;
    return "";
#endif
}

// ─── Validate .odmap file ──────────────────────────────
static bool isValidOdomap(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 22) return false;
    std::vector<uint8_t> data(size);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) return false;
    file.close();

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0))
        return false;

    const char* required[] = {"land_sea.png", "provinces.png", "provinces.json", "countries.json"};
    int found = 0;
    for (auto& name : required) {
        if (mz_zip_reader_locate_file(&zip, name, nullptr, 0) >= 0)
            found++;
    }
    mz_zip_reader_end(&zip);
    return found >= 3; // at minimum land_sea + provinces.png + provinces.json or countries.json
}

// ─── Get unique directory name ─────────────────────────
static std::string uniqueDirName(const std::string& baseDir, const std::string& desired) {
    std::string dir = baseDir + desired;
    struct stat st;
    if (stat(dir.c_str(), &st) != 0)
        return dir; // doesn't exist, use as-is

    for (int i = 2; i < 999; ++i) {
        dir = baseDir + desired + "_" + std::to_string(i);
        if (stat(dir.c_str(), &st) != 0)
            return dir;
    }
    return baseDir + desired + "_" + std::to_string(rand() % 10000);
}

// ─── Map import ──────────────────────────────────────────
void Game::executeMapImport() {
    std::string destDir = uniqueDirName(m_dataDir + "custom_maps/", m_importName);
#ifdef _WIN32
    _mkdir(destDir.c_str());
#else
    mkdir(destDir.c_str(), 0755);
#endif

    std::string odmDest = destDir + "/map.odmap";
    std::ifstream src(m_importPath, std::ios::binary);
    std::ofstream dst(odmDest, std::ios::binary);
    if (src && dst) {
        dst << src.rdbuf();
        std::cout << "Imported map: " << m_importPath << " -> " << odmDest << std::endl;
    }
    src.close();
    dst.close();

    // Generate thumbnail via temp Python script
    std::string pyScript = destDir + "/_gen_thumb.py";
    std::ofstream pyf(pyScript);
    pyf << "import zipfile, io, sys\n"
           "from PIL import Image\n"
           "z = zipfile.ZipFile(sys.argv[1])\n"
           "name = 'political.png' if 'political.png' in z.namelist() else 'provinces.png'\n"
           "d = z.read(name)\n"
           "img = Image.open(io.BytesIO(d))\n"
           "thumb = img.resize((160, 80), Image.NEAREST)\n"
           "thumb.save(sys.argv[2], 'PNG')\n";
    pyf.close();
    std::string thumbCmd = "python3 " + pyScript + " " + odmDest + " " + destDir + "/map_thumb.png";
    system(thumbCmd.c_str());
    std::remove(pyScript.c_str());

    // Check for scripts/ in the .odmap archive and detect has_scripts
    bool hasScripts = false;
    {
        std::ifstream zf(odmDest, std::ios::binary | std::ios::ate);
        if (zf) {
            std::streamsize zsize = zf.tellg();
            zf.seekg(0, std::ios::beg);
            std::vector<uint8_t> zdata(zsize);
            if (zf.read(reinterpret_cast<char*>(zdata.data()), zsize)) {
                mz_zip_archive zip{};
                if (mz_zip_reader_init_mem(&zip, zdata.data(), zdata.size(), 0)) {
                    int numEntries = mz_zip_reader_get_num_files(&zip);
                    for (int zi = 0; zi < numEntries; ++zi) {
                        mz_zip_archive_file_stat fstat;
                        if (mz_zip_reader_file_stat(&zip, zi, &fstat)) {
                            std::string ename = fstat.m_filename;
                            if (ename.rfind("scripts/", 0) == 0 && ename.size() > 8) {
                                hasScripts = true;
                                break;
                            }
                        }
                    }
                    mz_zip_reader_end(&zip);
                }
            }
        }
    }

    // Write meta.json
    nlohmann::json mj;
    mj["name"] = m_importName;
    mj["description"] = "Imported from: " + m_importPath;
    mj["filename"] = "map.odmap";
    mj["hasScripts"] = hasScripts;
    std::ofstream mf(destDir + "/meta.json");
    if (mf) mf << mj.dump(2);
    mf.close();

    // Reload and switch to custom tab
    loadMapEntries();
    m_mapTabIndex = 1;
    m_mapIndex = 0;
    m_mapScroll = 0;
    m_showImportNameDialog = false;
    m_importPath.clear();
    m_importName.clear();
}

// ─── Map browser ─────────────────────────────────────────
void Game::drawMapBrowser() {
    int centerX = m_screenW / 2;

    drawMenuBackground();
    DrawRectangleGradientV(0, 0, m_screenW, 130, {0, 0, 0, 200}, {0, 0, 0, 0});

    // Title
    const char* title = "New World";
    int titleSize = 40;
    int titleW = MeasureText(title, titleSize);
    DrawText(title, centerX - titleW / 2, 30, titleSize, hexToColor(m_config.accentColor));

    // Tab bar
    const char* tabs[] = {"Standard Worlds", "Custom Worlds"};
    int tabCount = 2;
    int tabW = 200;
    int tabH = 36;
    int tabY = 80;
    int totalTabW = tabCount * tabW;
    int tabStartX = centerX - totalTabW / 2;

    Vector2 mouse = getMouse();
    for (int t = 0; t < tabCount; ++t) {
        Rectangle tabRect = {(float)(tabStartX + t * tabW), (float)tabY, (float)tabW, (float)tabH};
        bool active = (t == m_mapTabIndex);
        bool hover = CheckCollisionPointRec(mouse, tabRect);
        Color bg = active ? (Color){60, 70, 100, 255} : (hover ? (Color){40, 45, 60, 255} : (Color){20, 22, 35, 255});
        DrawRectangleRounded(tabRect, 0.1f, 6, bg);
        int tw = MeasureText(tabs[t], 18);
        DrawText(tabs[t], (int)(tabRect.x + (tabW - tw) / 2), tabY + 8, 18, active ? hexToColor(m_config.accentColor) : LIGHTGRAY);
    }

    // Name input dialog for new world
    if (m_showNewWorldDialog) {
        DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
        int dlgW = 520, dlgH = 220;
        int dlgX = (m_screenW - dlgW) / 2;
        int dlgY = (m_screenH - dlgH) / 2;
        DrawRectangle(dlgX, dlgY, dlgW, dlgH, {20, 20, 30, 240});

        const char* title2 = "Name Your World";
        int titleW2 = MeasureText(title2, 28);
        DrawText(title2, centerX - titleW2 / 2, dlgY + 20, 28, hexToColor(m_config.accentColor));

        DrawText("Enter a name for your world:", dlgX + 30, dlgY + 65, 16, LIGHTGRAY);

        // Text input box
        int inputX = dlgX + 30, inputY = dlgY + 90;
        int inputW = dlgW - 60, inputH = 36;
        DrawRectangleRounded({(float)inputX, (float)inputY, (float)inputW, (float)inputH}, 0.1f, 6, {40, 42, 55, 255});
        DrawRectangleRoundedLines({(float)inputX, (float)inputY, (float)inputW, (float)inputH}, 0.1f, 6, {100, 100, 120, 200});

        DrawText(m_newWorldName.c_str(), inputX + 8, inputY + 8, 20, WHITE);

        // Blinking cursor
        if ((int)(GetTime() * 2) % 2 == 0) {
            int cursorX = inputX + 8 + MeasureText(m_newWorldName.c_str(), 20);
            DrawRectangle(cursorX, inputY + 8, 2, 20, {200, 200, 210, 255});
        }

        // Confirm / Cancel buttons
        int btnW = 120, btnH = 40;
        int btnY = dlgY + dlgH - 58;
        bool canConfirm = !m_newWorldName.empty();
        Rectangle confBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        bool confHov = CheckCollisionPointRec(mouse, confBtn) && canConfirm;
        DrawRectangleRounded(confBtn, 0.2f, 8, confHov ? Color{50, 120, 80, 255} : (canConfirm ? Color{40, 80, 60, 255} : Color{30, 40, 35, 200}));
        DrawText("Start", (int)(confBtn.x + (btnW - MeasureText("Start", 20)) / 2), (int)(confBtn.y + 10), 20, canConfirm ? WHITE : (Color){100, 100, 100, 200});

        Rectangle canBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};
        bool canHov = CheckCollisionPointRec(mouse, canBtn);
        DrawRectangleRounded(canBtn, 0.2f, 8, canHov ? Color{80, 80, 90, 255} : Color{50, 50, 60, 255});
        DrawText("Cancel", (int)(canBtn.x + (btnW - MeasureText("Cancel", 20)) / 2), (int)(canBtn.y + 10), 20, WHITE);

        // Warning about duplicate save name
        std::string checkPath = m_dataDir + "saves/" + m_newWorldName + ".odsv";
        struct stat chkStat;
        if (stat(checkPath.c_str(), &chkStat) == 0) {
            std::string warn = "A save with this name exists - will add (1), (2), etc.";
            DrawText(warn.c_str(), dlgX + 30, dlgY + dlgH - 102, 12, hexToColor(m_config.accentColor));
        }

        return;
    }

    // Name input dialog
    if (m_showImportNameDialog) {
        DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
        int dlgW = 520, dlgH = 220;
        int dlgX = (m_screenW - dlgW) / 2;
        int dlgY = (m_screenH - dlgH) / 2;
        DrawRectangle(dlgX, dlgY, dlgW, dlgH, {20, 20, 30, 240});

        const char* title2 = "Name Your World";
        int titleW2 = MeasureText(title2, 28);
        DrawText(title2, centerX - titleW2 / 2, dlgY + 20, 28, hexToColor(m_config.accentColor));

        DrawText("Enter a name for this map:", dlgX + 30, dlgY + 65, 16, LIGHTGRAY);

        // Text input box
        int inputX = dlgX + 30, inputY = dlgY + 90;
        int inputW = dlgW - 60, inputH = 36;
        DrawRectangleRounded({(float)inputX, (float)inputY, (float)inputW, (float)inputH}, 0.1f, 6, {40, 42, 55, 255});
        DrawRectangleRoundedLines({(float)inputX, (float)inputY, (float)inputW, (float)inputH}, 0.1f, 6, {100, 100, 120, 200});

        std::string display = m_importName;
        if (display.empty() && !IsKeyPressed(KEY_ENTER)) display = " ";
        DrawText(m_importName.c_str(), inputX + 8, inputY + 8, 20, WHITE);

        // Blinking cursor
        if ((int)(GetTime() * 2) % 2 == 0) {
            int cursorX = inputX + 8 + MeasureText(m_importName.c_str(), 20);
            DrawRectangle(cursorX, inputY + 8, 2, 20, {200, 200, 210, 255});
        }

        // Confirm / Cancel buttons
        int btnW = 120, btnH = 40;
        int btnY = dlgY + dlgH - 58;
        bool canConfirm = !m_importName.empty();
        Rectangle confBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        bool confHov = CheckCollisionPointRec(mouse, confBtn) && canConfirm;
        DrawRectangleRounded(confBtn, 0.2f, 8, confHov ? Color{50, 120, 80, 255} : (canConfirm ? Color{40, 80, 60, 255} : Color{30, 40, 35, 200}));
        DrawText("Create", (int)(confBtn.x + (btnW - MeasureText("Create", 20)) / 2), (int)(confBtn.y + 10), 20, canConfirm ? WHITE : (Color){100, 100, 100, 200});

        Rectangle canBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};
        bool canHov = CheckCollisionPointRec(mouse, canBtn);
        DrawRectangleRounded(canBtn, 0.2f, 8, canHov ? Color{80, 80, 90, 255} : Color{50, 50, 60, 255});
        DrawText("Cancel", (int)(canBtn.x + (btnW - MeasureText("Cancel", 20)) / 2), (int)(canBtn.y + 10), 20, WHITE);

        // Warning about duplicate path
        std::string checkPath = m_dataDir + "custom_maps/" + m_importName;
        struct stat chkStat;
        if (stat(checkPath.c_str(), &chkStat) == 0) {
            std::string warn = "Warning: \"" + m_importName + "\" already exists -- will create a unique folder";
            DrawText(warn.c_str(), dlgX + 30, dlgY + dlgH - 102, 12, hexToColor(m_config.accentColor));
        }

        return;
    }

    // Delete confirmation overlay
    if (m_showMapDeleteConfirm) {
        DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
        int dlgW = 500, dlgH = 180;
        int dlgX = (m_screenW - dlgW) / 2;
        int dlgY = (m_screenH - dlgH) / 2;
        DrawRectangle(dlgX, dlgY, dlgW, dlgH, {20, 20, 30, 240});

        const char* msg = "Delete this custom map permanently?";
        int msgW = MeasureText(msg, 24);
        DrawText(msg, centerX - msgW / 2, dlgY + 30, 24, WHITE);

        if (m_mapDeleteIndex >= 0 && m_mapDeleteIndex < (int)m_mapEntries.size()) {
            std::string sub = m_mapEntries[m_mapDeleteIndex].name;
            int subW = MeasureText(sub.c_str(), 18);
            DrawText(sub.c_str(), centerX - subW / 2, dlgY + 65, 18, hexToColor(m_config.accentColor));
        }

        int btnW = 120, btnH = 40;
        int btnY = dlgY + dlgH - 60;
        Rectangle delBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        bool delHov = CheckCollisionPointRec(mouse, delBtn);
        DrawRectangleRounded(delBtn, 0.2f, 8, delHov ? Color{180, 40, 40, 255} : Color{120, 30, 30, 255});
        DrawText("Delete", (int)(delBtn.x + (btnW - MeasureText("Delete", 20)) / 2), (int)(delBtn.y + 10), 20, WHITE);

        Rectangle canBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};
        bool canHov = CheckCollisionPointRec(mouse, canBtn);
        DrawRectangleRounded(canBtn, 0.2f, 8, canHov ? Color{80, 80, 90, 255} : Color{50, 50, 60, 255});
        DrawText("Cancel", (int)(canBtn.x + (btnW - MeasureText("Cancel", 20)) / 2), (int)(canBtn.y + 10), 20, WHITE);
        return;
    }

    // Map info popup
    if (m_showMapInfoPopup && m_mapInfoIndex >= 0 && m_mapInfoIndex < (int)m_mapEntries.size()) {
        auto& entry = m_mapEntries[m_mapInfoIndex];
        DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
        int dlgW = 600, dlgH = 400;
        int dlgX = (m_screenW - dlgW) / 2;
        int dlgY = (m_screenH - dlgH) / 2;
        DrawRectangle(dlgX, dlgY, dlgW, dlgH, {20, 20, 30, 240});

        // Title
        int titleW = MeasureText(entry.name.c_str(), 28);
        DrawText(entry.name.c_str(), dlgX + (dlgW - titleW) / 2, dlgY + 20, 28, hexToColor(m_config.accentColor));

        // Thumbnail
        int thumbX = dlgX + 20;
        int thumbY = dlgY + 60;
        int thumbW = 240, thumbH = 120;
        Texture2D thumbTex = getThumbTextureFromODM(entry.directory + entry.filename);
        if (thumbTex.id == 0)
            thumbTex = getThumbTexture(entry.thumbPath);
        if (thumbTex.id > 0) {
            DrawTexturePro(thumbTex,
                {0, 0, (float)thumbTex.width, (float)thumbTex.height},
                {(float)thumbX, (float)thumbY, (float)thumbW, (float)thumbH},
                {0, 0}, 0, WHITE);
        } else {
            DrawRectangle(thumbX, thumbY, thumbW, thumbH, (Color){40, 42, 55, 255});
            DrawText("no preview", thumbX + 60, thumbY + 45, 14, (Color){100, 100, 110, 200});
        }

        // Description
        int descX = thumbX + thumbW + 20;
        int descY = thumbY;
        int maxLineW = dlgX + dlgW - descX - 20;
        int charsPerLine = std::max(20, maxLineW / 9);
        std::string desc = entry.description.empty() ? "(no description)" : entry.description;
        for (size_t ci = 0; ci < desc.size(); ci += charsPerLine) {
            std::string line = desc.substr(ci, charsPerLine);
            DrawText(line.c_str(), descX, descY, 16, LIGHTGRAY);
            descY += 22;
        }

        // Details
        int detailY = dlgY + 200;
        std::string author = entry.author.empty() ? "(unknown)" : entry.author;
        std::string license = entry.license.empty() ? "(not specified)" : entry.license;
        std::string scripts = entry.hasScripts ? "Yes" : "No";

        DrawText("Author:", descX, detailY, 16, (Color){160, 180, 200, 255});
        DrawText(author.c_str(), descX + 100, detailY, 16, WHITE);
        detailY += 24;

        DrawText("License:", descX, detailY, 16, (Color){160, 180, 200, 255});
        DrawText(license.c_str(), descX + 100, detailY, 16, WHITE);
        detailY += 24;

        DrawText("Scripted Events:", descX, detailY, 16, (Color){160, 180, 200, 255});
        DrawText(scripts.c_str(), descX + 150, detailY, 16, entry.hasScripts ? (Color){100, 255, 100, 255} : (Color){200, 100, 100, 255});

        // View License button (if license is specified)
        bool showLicenseBtn = !entry.license.empty() && entry.license != "(not specified)";
        int licenseBtnY = dlgY + dlgH - 50;
        int licenseBtnX = dlgX + 20;
        int licenseBtnW = 140, licenseBtnH = 35;
        if (showLicenseBtn) {
            Rectangle licenseBtn = {(float)licenseBtnX, (float)licenseBtnY, (float)licenseBtnW, (float)licenseBtnH};
            bool licenseHov = CheckCollisionPointRec(mouse, licenseBtn);
            DrawRectangleRounded(licenseBtn, 0.2f, 8, licenseHov ? Color{40, 80, 120, 255} : Color{30, 60, 100, 255});
            DrawText("View License", licenseBtnX + (licenseBtnW - MeasureText("View License", 20)) / 2, licenseBtnY + 8, 20, WHITE);
            if (licenseHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                m_showLicensePopup = true;
                m_licenseEntryIndex = m_mapInfoIndex;
                m_licenseScroll = 0;
                m_showMapInfoPopup = false;
                m_mapInfoIndex = -1;
            }
        }

        // Close button
        int btnW = 100, btnH = 35;
        Rectangle closeBtn = {(float)(dlgX + dlgW - btnW - 20), (float)(dlgY + dlgH - btnH - 15), (float)btnW, (float)btnH};
        bool closeHov = CheckCollisionPointRec(mouse, closeBtn);
        DrawRectangleRounded(closeBtn, 0.2f, 8, closeHov ? Color{80, 80, 100, 255} : Color{60, 60, 80, 255});
        DrawText("Close", (int)(closeBtn.x + (btnW - MeasureText("Close", 20)) / 2), (int)(closeBtn.y + 8), 20, WHITE);

        if (closeHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_showMapInfoPopup = false;
            m_mapInfoIndex = -1;
        }

        // ESC to close
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_showMapInfoPopup = false;
            m_mapInfoIndex = -1;
        }

        return;
    }

    // License popup — scrollable separate viewer with Close/X
    if (m_showLicensePopup && m_licenseEntryIndex >= 0 && m_licenseEntryIndex < (int)m_mapEntries.size()) {
        auto& entry = m_mapEntries[m_licenseEntryIndex];
        DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 200});
        int dlgW = 700, dlgH = 550;
        int dlgX = (m_screenW - dlgW) / 2;
        int dlgY = (m_screenH - dlgH) / 2;
        DrawRectangleRounded({(float)dlgX, (float)dlgY, (float)dlgW, (float)dlgH}, 0.04f, 8, {18, 18, 30, 250});
        DrawRectangleRoundedLines({(float)dlgX, (float)dlgY, (float)dlgW, (float)dlgH}, 0.04f, 8, {60, 70, 100, 200});

        // Title
        std::string licenseTitle = entry.license.empty() ? "License" : entry.license + " License";
        int titleW = MeasureText(licenseTitle.c_str(), 28);
        DrawText(licenseTitle.c_str(), dlgX + (dlgW - titleW) / 2, dlgY + 20, 28, hexToColor(m_config.accentColor));

        // Load license text from m_odmJsonData first, then from .odmap archive
        std::string licenseText;
        if (!m_cachedLicenseText.empty()) {
            licenseText = m_cachedLicenseText;
        } else {
            std::string licEntry = "licenses/" + entry.license + ".txt";
            auto odmIt = m_odmJsonData.find(licEntry);
            if (odmIt != m_odmJsonData.end()) {
                licenseText = odmIt->second;
            } else {
                std::string odmPath = entry.directory + entry.filename;
                unsigned char* zipData = nullptr;
                int zipSize = 0;
                zipData = LoadFileData(odmPath.c_str(), &zipSize);
                if (zipData && zipSize > 0) {
                    mz_zip_archive zip{};
                    if (mz_zip_reader_init_mem(&zip, zipData, zipSize, 0)) {
                        int idx = mz_zip_reader_locate_file(&zip, licEntry.c_str(), nullptr, 0);
                        if (idx >= 0) {
                            size_t sz = 0;
                            char* txt = (char*)mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
                            if (txt && sz > 0) {
                                licenseText.assign(txt, sz);
                                mz_free(txt);
                            }
                        }
                        mz_zip_reader_end(&zip);
                    }
                    RL_FREE(zipData);
                }
            }
            if (licenseText.empty()) {
                licenseText = "(License text not found inside the .odmap archive. The license file should be included under licenses/.)";
            }
            m_cachedLicenseText = licenseText;
        }

        // Scrollable text area
        int textAreaX = dlgX + 30;
        int textAreaY = dlgY + 65;
        int textAreaW = dlgW - 100;
        int textAreaH = dlgH - 120;
        int fontSize = 14;
        int lineH = fontSize + 4;
        int charsPerLine = std::max(40, textAreaW / (fontSize * 2 / 3));

        // Compute total lines (word-wrapped)
        int totalLines = 0;
        size_t tp = 0;
        while (tp < licenseText.size()) {
            size_t remaining = licenseText.size() - tp;
            size_t lineLen = std::min(remaining, (size_t)charsPerLine);
            size_t nl = licenseText.find('\n', tp);
            if (nl != std::string::npos && nl - tp < lineLen) {
                tp = nl + 1;
            } else {
                size_t end = tp + lineLen;
                size_t space = licenseText.rfind(' ', end);
                if (space != std::string::npos && space > tp)
                    tp = space + 1;
                else
                    tp = end;
            }
            totalLines++;
        }

        int maxScroll = std::max(0, totalLines - textAreaH / lineH);
        m_licenseScroll = std::clamp(m_licenseScroll, 0, maxScroll);

        // Scroll wheel
        if (CheckCollisionPointRec(mouse, {(float)textAreaX, (float)textAreaY, (float)textAreaW, (float)textAreaH})) {
            float wheel = GetMouseWheelMove();
            if (wheel != 0) {
                m_licenseScroll -= (int)wheel * 3;
                m_licenseScroll = std::clamp(m_licenseScroll, 0, maxScroll);
            }
        }

        // Draw scrollbar
        if (maxScroll > 0) {
            int sbX = dlgX + dlgW - 22;
            int sbY = textAreaY;
            int sbH = textAreaH;
            int sbW = 8;
            DrawRectangleRounded({(float)sbX, (float)sbY, (float)sbW, (float)sbH}, 0.5f, 4, {40, 40, 50, 200});
            float thumbH = (float)sbH * (float)textAreaH / (float)(totalLines * lineH);
            float thumbY = (float)sbY + (float)(sbH - (int)thumbH) * ((float)m_licenseScroll / (float)std::max(1, maxScroll));
            DrawRectangleRounded({(float)sbX, thumbY, (float)sbW, thumbH}, 0.5f, 4, {120, 130, 160, 220});
        }

        // Clip region for text (per-character hybrid rendering via drawHybridText)
        BeginScissorMode(textAreaX, textAreaY, textAreaW, textAreaH);
        int textY = textAreaY - m_licenseScroll * lineH;
        size_t pos = 0;
        while (pos < licenseText.size()) {
            size_t remaining = licenseText.size() - pos;
            if (remaining == 0) break;
            size_t lineLen = std::min(remaining, (size_t)charsPerLine);
            size_t nl = licenseText.find('\n', pos);
            if (nl != std::string::npos && nl - pos < lineLen) {
                drawHybridText(textAreaX, textY, fontSize, licenseText.substr(pos, nl - pos).c_str(), LIGHTGRAY);
                pos = nl + 1;
                textY += lineH;
                continue;
            }
            size_t end = pos + lineLen;
            size_t space = licenseText.rfind(' ', end);
            if (space != std::string::npos && space > pos) end = space;
            drawHybridText(textAreaX, textY, fontSize, licenseText.substr(pos, end - pos).c_str(), LIGHTGRAY);
            pos = (space != std::string::npos && space > pos) ? space + 1 : end;
            textY += lineH;
        }
        EndScissorMode();

        // Close button
        int btnW = 100, btnH = 35;
        Rectangle closeBtn = {(float)(dlgX + dlgW - btnW - 20), (float)(dlgY + dlgH - btnH - 15), (float)btnW, (float)btnH};
        bool closeHov = CheckCollisionPointRec(mouse, closeBtn);
        DrawRectangleRounded(closeBtn, 0.2f, 8, closeHov ? Color{80, 80, 100, 255} : Color{60, 60, 80, 255});
        DrawText("Close", (int)(closeBtn.x + (btnW - MeasureText("Close", 20)) / 2), (int)(closeBtn.y + 8), 20, WHITE);

        if (closeHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_showLicensePopup = false;
            m_licenseEntryIndex = -1;
            m_cachedLicenseText.clear();
        }

        // X button (top-right corner)
        int xBtnSz = 30;
        Rectangle xBtn = {(float)(dlgX + dlgW - xBtnSz - 12), (float)(dlgY + 12), (float)xBtnSz, (float)xBtnSz};
        bool xHov = CheckCollisionPointRec(mouse, xBtn);
        Color xCol = xHov ? Color{200, 60, 60, 255} : Color{140, 140, 160, 200};
        DrawLineEx({xBtn.x + 6, xBtn.y + 6}, {xBtn.x + xBtnSz - 6, xBtn.y + xBtnSz - 6}, 2.5f, xCol);
        DrawLineEx({xBtn.x + xBtnSz - 6, xBtn.y + 6}, {xBtn.x + 6, xBtn.y + xBtnSz - 6}, 2.5f, xCol);
        if (xHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_showLicensePopup = false;
            m_licenseEntryIndex = -1;
            m_cachedLicenseText.clear();
        }

        // ESC to close
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_showLicensePopup = false;
            m_licenseEntryIndex = -1;
            m_cachedLicenseText.clear();
        }

        return;
    }

    // Filter entries by current tab
    std::vector<int> visible;
    for (int i = 0; i < (int)m_mapEntries.size(); ++i) {
        if ((m_mapTabIndex == 0) == m_mapEntries[i].isStandard)
            visible.push_back(i);
    }

    // Custom tab: add "Import .odmap" entry as last item
    bool showImportBtn = (m_mapTabIndex == 1);
    int importIdx = (int)visible.size();

    // Layout: vertical list with cards
    int cardX = 60;
    int cardW = m_screenW - 120;
    int cardH = 100;
    int cardGap = 8;
    int listStartY = 130;

    int totalItems = (int)visible.size() + (showImportBtn ? 1 : 0);
    int maxVisibleCards = std::max(1, (m_screenH - listStartY - 60) / (cardH + cardGap));
    int maxScroll = std::max(0, totalItems - maxVisibleCards);
    m_mapScroll = std::clamp(m_mapScroll, 0, maxScroll);

    // Draw cards
    for (int vi = 0; vi < totalItems; ++vi) {
        int y = listStartY + (vi - m_mapScroll) * (cardH + cardGap);
        if (y + cardH < 0 || y > m_screenH) continue;

        bool isImportBtn = showImportBtn && vi == importIdx;
        bool isSelected = (vi == m_mapIndex);
        bool isHovered = CheckCollisionPointRec(mouse, {(float)cardX, (float)y, (float)cardW, (float)cardH});
        Color bgColor = isSelected ? (Color){60, 70, 100, 180} : (isHovered ? (Color){255, 255, 255, 15} : (Color){15, 17, 28, 200});
        DrawRectangleRounded({(float)cardX, (float)y, (float)cardW, (float)cardH}, 0.08f, 8, bgColor);

        // Border for selected
        if (isSelected) {
            DrawRectangleRoundedLines({(float)cardX, (float)y, (float)cardW, (float)cardH}, 0.08f, 8, ColorAlpha(hexToColor(m_config.accentColor), 200.0f/255.0f));
        }

        if (isImportBtn) {
            // Import button
            int icX = centerX;
            int icY = y + cardH / 2;
            int plusSize = 40;
            DrawText("+", icX - MeasureText("+", plusSize) / 2, icY - plusSize / 2, plusSize, (Color){100, 200, 100, 200});
            DrawText("Import .odmap", icX - MeasureText("Import .odmap", 20) / 2, icY + 10, 20, (Color){100, 200, 100, static_cast<unsigned char>(isHovered ? 255 : 180)});
        } else {
            int realIdx = visible[vi];
            auto& entry = m_mapEntries[realIdx];

            // Thumbnail
            int thumbX = cardX + 12;
            int thumbY = y + (cardH - 80) / 2;
            int thumbW = 160, thumbH = 80;
            Texture2D thumbTex = getThumbTextureFromODM(entry.directory + entry.filename);
            if (thumbTex.id == 0)
                thumbTex = getThumbTexture(entry.thumbPath);
            if (thumbTex.id > 0) {
                DrawTexturePro(thumbTex,
                    {0, 0, (float)thumbTex.width, (float)thumbTex.height},
                    {(float)thumbX, (float)thumbY, (float)thumbW, (float)thumbH},
                    {0, 0}, 0, WHITE);
            } else {
                DrawRectangle(thumbX, thumbY, thumbW, thumbH, (Color){40, 42, 55, 255});
                DrawText("no preview", thumbX + 30, thumbY + 30, 14, (Color){100, 100, 110, 200});
            }

            // Name
            int textX = thumbX + thumbW + 16;
            DrawText(entry.name.c_str(), textX, y + 12, 22, WHITE);

            // Description
            std::string desc = entry.description;
            if (desc.empty()) desc = "(no description)";
            // Simple word-wrap: 50 chars per line
            int descY = y + 42;
            int maxLineW = cardW - (textX - cardX) - 60 - (entry.isStandard ? 0 : 50);
            int charsPerLine = std::max(20, maxLineW / 9);
            for (size_t ci = 0; ci < desc.size(); ci += charsPerLine) {
                std::string line = desc.substr(ci, charsPerLine);
                DrawText(line.c_str(), textX, descY, 14, (Color){160, 160, 170, 255});
                descY += 18;
                if (descY > y + cardH - 8) break;
            }

            // Tags
            std::string tag = entry.isStandard ? "STANDARD" : "CUSTOM";
            int tagW = MeasureText(tag.c_str(), 11);
            int tagX = cardX + cardW - tagW - 16;
            int tagY2 = y + 10;
            DrawRectangleRounded({(float)tagX - 4, (float)tagY2 - 2, (float)(tagW + 8), 18}, 0.15f, 6,
                                entry.isStandard ? (Color){50, 120, 80, 200} : (Color){120, 90, 40, 200});
            DrawText(tag.c_str(), tagX, tagY2, 11, (Color){200, 220, 210, 220});

            // Info button (all maps) - right side, vertically centered
            int infoX = cardX + cardW - 40;
            int infoY = y + cardH / 2;
            int infoRadius = 14;
            Rectangle infoRect = {(float)(infoX - infoRadius), (float)(infoY - infoRadius), (float)(infoRadius * 2), (float)(infoRadius * 2)};
            bool infoHov = CheckCollisionPointRec(mouse, infoRect);
            
            // Draw circle background
            Color circleBg = infoHov ? (Color){40, 80, 120, 200} : (Color){0, 0, 0, 120};
            DrawCircle(infoX, infoY, infoRadius, circleBg);
            // Draw circle border
            Color circleBorder = infoHov ? (Color){100, 180, 255, 255} : (Color){100, 150, 200, 200};
            DrawCircleLines(infoX, infoY, infoRadius, circleBorder);
            // Draw "i" character
            int iW = MeasureText("i", 18);
            DrawText("i", infoX - iW / 2, infoY - 9, 18, circleBorder);

            // Delete button (custom only)
            if (!entry.isStandard) {
                std::string ver = "v" + entry.id;
                int verW = MeasureText(ver.c_str(), 11);
                DrawText(ver.c_str(), cardX + cardW - verW - 14, y + 14, 11, (Color){100, 100, 110, 200});
            }
        }
    }

    // Scroll indicators
    if (m_mapScroll > 0) {
        DrawText("^", centerX - 6, listStartY - 18, 16, (Color){160, 160, 170, 200});
    }
    if (m_mapScroll < maxScroll) {
        DrawText("v", centerX - 6, m_screenH - 50, 16, (Color){160, 160, 170, 200});
    }

    // Back button
    int backSize = 24;
    const char* backLabel = "< Back";
    Rectangle backRect = {20, (float)(m_screenH - 44), (float)(MeasureText(backLabel, backSize) + 24), (float)(backSize + 10)};
    bool backHov = CheckCollisionPointRec(mouse, backRect);
    DrawRectangleRounded(backRect, 0.1f, 8, backHov ? (Color){255, 255, 255, 20} : BLANK);
    DrawText(backLabel, 32, m_screenH - 44 + 5, backSize, (Color){200, 200, 210, 255});
}

void Game::updateMapBrowser() {
    if (isMouseOverConsole()) return;

    // The info and licence popups are the only two dialogs on this screen whose
    // buttons are handled in drawMapBrowser() rather than here. Without this
    // guard the click that presses one of their buttons ALSO reaches the map
    // card underneath in the same frame, which starts a new world: pressing
    // "View License" opened the licence and the new-world dialog on top of it,
    // pressing "Close" started a world, and cancelling that revealed the popup
    // still sitting there -- which looked like being thrown back to map info.
    //
    // The other three dialogs here consume their own input below and return, so
    // they need no entry in this list. Moving the popups' input handling out of
    // the draw pass would be the tidier fix; this is the correct one either way,
    // because a modal must stop the screen behind it from seeing the click.
    if (m_showMapInfoPopup || m_showLicensePopup) return;

    Vector2 mouse = getMouse();
    int centerX = m_screenW / 2;

    // New world name input dialog
    if (m_showNewWorldDialog) {
        int c = GetCharPressed();
        while (c > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            if (c >= 32 && c < 127 && m_newWorldName.size() < 64) {
                // Sanitize: disallow path separators
                if (c != '/' && c != '\\' && c != ':')
                    m_newWorldName += (char)c;
            }
            c = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !m_newWorldName.empty())
            m_newWorldName.pop_back();

        if (IsKeyPressed(KEY_ENTER) && !m_newWorldName.empty()) {
            // Handle duplicate names by appending (1), (2), etc.
            std::string baseName = m_newWorldName;
            std::string savePath = m_dataDir + "saves/" + m_newWorldName + ".odsv";
            struct stat chkStat;
            int suffix = 1;
            while (stat(savePath.c_str(), &chkStat) == 0) {
                m_newWorldName = baseName + " (" + std::to_string(suffix) + ")";
                savePath = m_dataDir + "saves/" + m_newWorldName + ".odsv";
                suffix++;
            }
            // Start the game with the custom name
            startNewGameWithName(m_newWorldMapPath, m_newWorldName);
            m_showNewWorldDialog = false;
            m_newWorldMapPath.clear();
            m_newWorldName.clear();
            return;
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_showNewWorldDialog = false;
            m_newWorldMapPath.clear();
            m_newWorldName.clear();
            return;
        }

        // Mouse button clicks for Start / Cancel
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            int btnW = 120, btnH = 40;
            int btnY = (m_screenH - 220) / 2 + 220 - 58;
            Rectangle confBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
            Rectangle canBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};
            if (CheckCollisionPointRec(mouse, confBtn) && !m_newWorldName.empty()) {
                Audio::get().playSfx("confirm");
                // Handle duplicate names by appending (1), (2), etc.
                std::string baseName = m_newWorldName;
                std::string savePath = m_dataDir + "saves/" + m_newWorldName + ".odsv";
                struct stat chkStat;
                int suffix = 1;
                while (stat(savePath.c_str(), &chkStat) == 0) {
                    m_newWorldName = baseName + " (" + std::to_string(suffix) + ")";
                    savePath = m_dataDir + "saves/" + m_newWorldName + ".odsv";
                    suffix++;
                }
                startNewGameWithName(m_newWorldMapPath, m_newWorldName);
                m_showNewWorldDialog = false;
                m_newWorldMapPath.clear();
                m_newWorldName.clear();
                return;
            }
            if (CheckCollisionPointRec(mouse, canBtn)) {
                Audio::get().playSfx("back");
                m_showNewWorldDialog = false;
                m_newWorldMapPath.clear();
                m_newWorldName.clear();
                return;
            }
        }
        return; // block other interactions while naming
    }

    // Name input dialog
    if (m_showImportNameDialog) {
        int c = GetCharPressed();
        while (c > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            if (c >= 32 && c < 127 && m_importName.size() < 64) {
                // Sanitize: disallow path separators
                if (c != '/' && c != '\\' && c != ':')
                    m_importName += (char)c;
            }
            c = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !m_importName.empty())
            m_importName.pop_back();

        if (IsKeyPressed(KEY_ENTER) && !m_importName.empty()) {
            executeMapImport();
            return;
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_showImportNameDialog = false;
            m_importPath.clear();
            m_importName.clear();
            return;
        }

        // Mouse button clicks for Create / Cancel
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            int btnW = 120, btnH = 40;
            int btnY = (m_screenH - 220) / 2 + 220 - 58;
            Rectangle confBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
            Rectangle canBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};
            if (CheckCollisionPointRec(mouse, confBtn) && !m_importName.empty()) {
                Audio::get().playSfx("confirm");
                executeMapImport();
                return;
            }
            if (CheckCollisionPointRec(mouse, canBtn)) {
                Audio::get().playSfx("back");
                m_showImportNameDialog = false;
                m_importPath.clear();
                m_importName.clear();
                return;
            }
        }
        return; // block other interactions while naming
    }

    // Tab bar clicks
    int tabW = 200;
    int tabCount = 2;
    int totalTabW = tabCount * tabW;
    int tabStartX = centerX - totalTabW / 2;
    int tabY = 80;

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        for (int t = 0; t < tabCount; ++t) {
            Rectangle tabRect = {(float)(tabStartX + t * tabW), (float)tabY, (float)tabW, 36};
            if (CheckCollisionPointRec(mouse, tabRect)) {
                Audio::get().playSfx("tab_switch");
                m_mapTabIndex = t;
                m_mapIndex = 0;
                m_mapScroll = 0;
                return;
            }
        }
    }

    // Delete confirmation dialog
    if (m_showMapDeleteConfirm) {
        int dlgW = 500, dlgH = 180;
        int dlgX = (m_screenW - dlgW) / 2;
        int dlgY = (m_screenH - dlgH) / 2;
        int btnW = 120, btnH = 40;
        int btnY = dlgY + dlgH - 60;
        Rectangle delBtn = {(float)(centerX - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        Rectangle canBtn = {(float)(centerX + 10), (float)btnY, (float)btnW, (float)btnH};

        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (CheckCollisionPointRec(mouse, delBtn)) {
                Audio::get().playSfx("confirm");
                // Delete custom map directory
                if (m_mapDeleteIndex >= 0 && m_mapDeleteIndex < (int)m_mapEntries.size()) {
                    const MapEntry& target = m_mapEntries[m_mapDeleteIndex];
                    std::string dir = target.directory;
                    if (target.isLooseFile) {
                        // `directory` is the shared custom_maps/ root here —
                        // sweeping it would delete every other exported map.
                        std::remove((dir + target.filename).c_str());
                    } else {
                        // Own subfolder: remove its files, then the folder
                        DIR* d = opendir(dir.c_str());
                        if (d) {
                            struct dirent* e;
                            while ((e = readdir(d)) != nullptr) {
                                std::string fn = e->d_name;
                                if (fn == "." || fn == "..") continue;
                                std::remove((dir + fn).c_str());
                            }
                            closedir(d);
                        }
                        rmdir(dir.c_str());
                    }
                    // Reload entries
                    loadMapEntries();
                }
                m_showMapDeleteConfirm = false;
                m_mapDeleteIndex = -1;
            } else if (CheckCollisionPointRec(mouse, canBtn)) {
                Audio::get().playSfx("back");
                m_showMapDeleteConfirm = false;
                m_mapDeleteIndex = -1;
            }
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_showMapDeleteConfirm = false;
            m_mapDeleteIndex = -1;
        }
        return;
    }

    // ESC to go back
    if (IsKeyPressed(KEY_ESCAPE)) {
        m_currentScreen = SCREEN_SINGLEPLAYER;
        return;
    }

    // Count visible items
    std::vector<int> visible;
    for (int i = 0; i < (int)m_mapEntries.size(); ++i) {
        if ((m_mapTabIndex == 0) == m_mapEntries[i].isStandard)
            visible.push_back(i);
    }

    bool showImportBtn = (m_mapTabIndex == 1);
    int importIdx = (int)visible.size();
    int totalItems = (int)visible.size() + (showImportBtn ? 1 : 0);
    if (totalItems == 0) return;

    int cardX = 60;
    int cardW = m_screenW - 120;
    int cardH = 100;
    int cardGap = 8;
    int listStartY = 130;
    int maxVisibleCards = std::max(1, (m_screenH - listStartY - 60) / (cardH + cardGap));
    int maxScroll = std::max(0, totalItems - maxVisibleCards);

    // Mouse wheel
    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        m_mapScroll -= (int)wheel;
        m_mapScroll = std::clamp(m_mapScroll, 0, maxScroll);
    }

    // Arrow keys
    if (IsKeyPressed(KEY_UP)) {
        m_mapIndex = (m_mapIndex + totalItems - 1) % totalItems;
        if (m_mapIndex < m_mapScroll) m_mapScroll = m_mapIndex;
    }
    if (IsKeyPressed(KEY_DOWN)) {
        m_mapIndex = (m_mapIndex + 1) % totalItems;
        if (m_mapIndex >= m_mapScroll + maxVisibleCards) m_mapScroll = m_mapIndex - maxVisibleCards + 1;
    }

    // Back button
    Rectangle backRect = {20, (float)(m_screenH - 44), (float)(MeasureText("< Back", 24) + 24), (float)(24 + 10)};
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, backRect)) {
        Audio::get().playSfx("back");
        m_currentScreen = SCREEN_SINGLEPLAYER;
        return;
    }

    // Check per-item interactions
    bool activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
    for (int vi = 0; vi < totalItems; ++vi) {
        int y = listStartY + (vi - m_mapScroll) * (cardH + cardGap);
        if (y + cardH < 0 || y > m_screenH) continue;

        Rectangle cardRect = {(float)cardX, (float)y, (float)cardW, (float)cardH};

        if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) continue;
        if (!CheckCollisionPointRec(mouse, cardRect)) continue;
        Audio::get().playSfx("click_light");

        if (showImportBtn && vi == importIdx) {
            // Import .odmap via native file dialog
            std::string chosen = nativeOpenFileDialog("Select a map file (.odmap)", "odmap");
            if (chosen.empty()) return;

            if (!isValidOdomap(chosen)) {
                std::cerr << "Invalid or incomplete .odmap file: " << chosen << std::endl;
                // TODO: show error overlay
                return;
            }

            m_importPath = chosen;

            // Default name from filename (without extension)
            size_t slash = chosen.rfind('/');
            std::string fname = (slash != std::string::npos) ? chosen.substr(slash + 1) : chosen;
            size_t dot = fname.rfind('.');
            if (dot != std::string::npos) fname = fname.substr(0, dot);
            m_importName = fname;

            m_showImportNameDialog = true;
            return;
        }

        // Check delete button (custom only)
        if (!m_mapEntries[visible[vi]].isStandard) {
            int delX = cardX + cardW - 44;
            int delY = y + cardH - 36;
            Rectangle delRect = {(float)delX, (float)delY, 30, 30};
            if (CheckCollisionPointRec(mouse, delRect)) {
                Audio::get().playSfx("click_light");
                m_mapDeleteIndex = visible[vi];
                m_showMapDeleteConfirm = true;
                return;
            }
        }

        // Check info button (all maps)
        int infoX = cardX + cardW - 40;
        int infoY = y + cardH / 2;
        int infoRadius = 14;
        Rectangle infoRect = {(float)(infoX - infoRadius), (float)(infoY - infoRadius), (float)(infoRadius * 2), (float)(infoRadius * 2)};
        if (CheckCollisionPointRec(mouse, infoRect)) {
            Audio::get().playSfx("click_light");
            m_mapInfoIndex = visible[vi];
            m_showMapInfoPopup = true;
            return;
        }

        // Select this map
        m_mapIndex = vi;
        activate = true;
        break;
    }

    if (activate) {
        m_mapIndex = std::clamp(m_mapIndex, 0, totalItems - 1);
        if (showImportBtn && m_mapIndex == importIdx) return; // handled above
        int realIdx = visible[m_mapIndex];
        auto& entry = m_mapEntries[realIdx];
        // Show world name dialog before starting
        m_newWorldMapPath = entry.directory + entry.filename;
        m_newWorldName = entry.name;
        m_showNewWorldDialog = true;
    }
}

// ─── File browser (standard directory picker) ─────────────
void Game::drawFileBrowser() {
    int centerX = m_screenW / 2;

    drawMenuBackground();
    DrawRectangleGradientV(0, 0, m_screenW, 100, {0, 0, 0, 200}, {0, 0, 0, 0});
    const char* title = "Load World";
    int titleSize = 40;
    int titleW = MeasureText(title, titleSize);
    DrawText(title, centerX - titleW / 2, 60, titleSize, hexToColor(m_config.accentColor));

    const char* help = "Select a save file or press ESC to go back";
    int helpW = MeasureText(help, 16);
    DrawText(help, centerX - helpW / 2, 105, 16, (Color){140, 140, 150, 200});

    if (m_fileItems.empty()) {
        const char* msg = "No files found in this directory";
        int msgW = MeasureText(msg, 24);
        DrawText(msg, centerX - msgW / 2, m_screenH / 2, 24, (Color){200, 100, 100, 255});
        return;
    }

    int count = (int)m_fileItems.size();
    int itemH = 50;
    int startY = 140;
    int maxVisible = std::max(1, (m_screenH - startY - 30) / itemH);
    int maxScroll = std::max(0, count - maxVisible);
    m_fileScroll = std::clamp(m_fileScroll, 0, maxScroll);

    int visibleCount = std::min(count - m_fileScroll, maxVisible);
    DrawRectangle(20, startY - 8, m_screenW - 40, std::max(0, visibleCount) * itemH + 16, {0, 0, 0, 100});

    Vector2 mouse = getMouse();

    for (int i = 0; i < count; ++i) {
        int y = startY + (i - m_fileScroll) * itemH;
        if (y + itemH < 0 || y > m_screenH) continue;

        Color c = WHITE;
        if (i == m_fileIndex) c = hexToColor(m_config.accentColor);
        int tw = MeasureText(m_fileItems[i].c_str(), 30);
        bool hovered = CheckCollisionPointRec(mouse, { (float)(centerX - tw/2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) });
        if (hovered) {
            DrawRectangleRounded({(float)(centerX - tw/2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10)}, 0.1f, 8, {255,255,255,15});
        }
        DrawText(m_fileItems[i].c_str(), centerX - tw/2, y, 30, c);
    }

    if (m_fileScroll > 0)
        DrawText("^", centerX - 8, startY - 25, 20, (Color){160, 160, 170, 200});
    if (m_fileScroll < maxScroll)
        DrawText("v", centerX - 8, m_screenH - 50, 20, (Color){160, 160, 170, 200});

    int backSize = 24;
    const char* backLabel = "< Back";
    int backW = MeasureText(backLabel, backSize);
    DrawText(backLabel, 20, m_screenH - 50, backSize, (Color){200, 200, 210, 255});
}

void Game::updateFileBrowser() {
    if (isMouseOverConsole()) return;
    if (m_fileItems.empty()) {
        if (IsKeyPressed(KEY_ESCAPE)) m_currentScreen = m_browsingSaves ? SCREEN_SINGLEPLAYER : SCREEN_MENU;
        return;
    }

    int count = (int)m_fileItems.size();
    int itemH = 50;
    int startY = 140;
    int maxVisible = std::max(1, (m_screenH - startY - 30) / itemH);
    int maxScroll = std::max(0, count - maxVisible);

    if (IsKeyPressed(KEY_UP)) {
        m_fileIndex = (m_fileIndex + count - 1) % count;
        if (m_fileIndex < m_fileScroll) m_fileScroll = m_fileIndex;
    }
    if (IsKeyPressed(KEY_DOWN)) {
        m_fileIndex = (m_fileIndex + 1) % count;
        if (m_fileIndex >= m_fileScroll + maxVisible) m_fileScroll = m_fileIndex - maxVisible + 1;
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        m_currentScreen = m_browsingSaves ? SCREEN_SINGLEPLAYER : SCREEN_MAP_SELECT;
        return;
    }

    // Back button click
    int backW = MeasureText("< Back", 24);
    Rectangle backRect = {20, (float)(m_screenH - 50), (float)(backW + 24), (float)(24 + 12)};
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(getMouse(), backRect)) {
        Audio::get().playSfx("back");
        m_currentScreen = m_browsingSaves ? SCREEN_SINGLEPLAYER : SCREEN_MAP_SELECT;
        return;
    }

    // Mouse wheel scroll
    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        m_fileScroll -= (int)wheel;
        m_fileScroll = std::clamp(m_fileScroll, 0, maxScroll);
    }

    Vector2 mouse = getMouse();
    int hovered = -1;
    int centerX = m_screenW / 2;
    for (int i = 0; i < count; ++i) {
        int y = startY + (i - m_fileScroll) * itemH;
        if (y + itemH < 0 || y > m_screenH) continue;
        int tw = MeasureText(m_fileItems[i].c_str(), 30);
        if (CheckCollisionPointRec(mouse, { (float)(centerX - tw/2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) }))
            { hovered = i; break; }
    }

    bool activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && hovered >= 0) {
        Audio::get().playSfx("click_light");
        m_fileIndex = hovered;
        activate = true;
    }

    if (activate) {
        m_fileIndex = std::clamp(m_fileIndex, 0, count - 1);
        std::string filename = m_fileItems[m_fileIndex];
        if (m_browsingSaves) {
            startLoadedGame(filename);
        }
    }
}
