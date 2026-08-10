#include "Game.h"
#include "Audio.h"
#include "GameInternals.h"
#include "SaveManager.h"
#include "Keybinds.h"
#include "ai/AISystem.h"
#include "renderer/FlagRenderer.h"
#include "OdFile.h"
#include "util/WebAssets.h"
#include "miniz.h"
#include "miniz_zip.h"
#include "raymath.h"
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <filesystem>
#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>
#ifndef _WIN32
#endif
#include <ctime>
#include <random>

template<typename F>
static Texture2D makeIcon(int w, int h, F drawFn) {
    Image img = GenImageColor(w, h, {0, 0, 0, 0});
    drawFn(img);
    Texture2D tex = LoadTextureFromImage(img);
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    UnloadImage(img);
    return tex;
}

static Color blendColor(Color base, float t) {
    uint8_t r = (uint8_t)std::min(255, std::max(0, (int)(base.r * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
    uint8_t g = (uint8_t)std::min(255, std::max(0, (int)(base.g * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
    uint8_t b = (uint8_t)std::min(255, std::max(0, (int)(base.b * (1.0f - t * 0.4f) + 40.0f * t * 0.3f)));
    return {r, g, b, 255};
}

void Game::showLoadingScreen() {
    m_showLoadingScreen = true;
    // Any load that starts here supersedes a queued revert — otherwise a failed
    // or abandoned load would leave the request armed to fire on the next world.
    // revertToTurn() re-arms it immediately after kicking its load off.
    m_pendingRevertTurn = -1;
    m_pendingRevertSave.clear();
    m_loadingProgress = 0.0f;
    m_loadingStatus = "Initializing...";
    m_tipTimer = 0;
    m_currentTipIndex = 0;

    // Load tips from JSON file
    m_loadingTips.clear();
    std::string tipsPath = m_dataDir + "tips.json";
    std::ifstream tipsFile(tipsPath);
    if (tipsFile) {
        try {
            auto j = nlohmann::json::parse(tipsFile);
            if (j.contains("tips") && j["tips"].is_array()) {
                for (auto& tip : j["tips"]) {
                    if (tip.is_string()) {
                        m_loadingTips.push_back(tip.get<std::string>());
                    }
                }
            }
        } catch (...) {
            std::cerr << "Failed to parse tips.json" << std::endl;
        }
    }

    // Add default tips if none loaded
    if (m_loadingTips.empty()) {
        m_loadingTips.push_back("Loading game data...");
        m_loadingTips.push_back("Preparing the world...");
        m_loadingTips.push_back("Almost ready...");
    }

    // Shuffle tips for variety
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(m_loadingTips.begin(), m_loadingTips.end(), g);
}

void Game::hideLoadingScreen() {
    m_showLoadingScreen = false;
    m_loadingPhase = LOAD_NONE;
    m_loadingShouldCreateSave = false;
    m_loadingWorldName.clear();
}

void Game::startLoading(const std::string& odmPath) {
    showLoadingScreen();
    m_loadingPhase = LOAD_ODM;
    m_loadingOdmPath = odmPath;
    m_loadingSavePath.clear();
    m_loadingTempOdm.clear();
    m_loadingResIdx = 0;
    m_loadingFailed = false;
    m_loadError.clear();
    std::cout << "Started async loading: " << odmPath << std::endl;
}

void Game::startLoadingSave(const std::string& savePath) {
    showLoadingScreen();
    m_loadingPhase = LOAD_ODM_SAVE;
    m_loadingSavePath = savePath;
    m_loadingTempOdm.clear();
    m_loadingOdmPath.clear();
    m_loadingResIdx = 0;
    m_loadingFailed = false;
    m_loadError.clear();
    std::cout << "Started async save loading: " << savePath << std::endl;
}

void Game::updateLoading() {
    if (m_loadingPhase == LOAD_NONE || m_loadingPhase == LOAD_DONE) return;

    switch (m_loadingPhase) {
        case LOAD_ODM_SAVE: {
            setLoadingProgress(0.03f, "Extracting map from save...");
            std::vector<uint8_t> odmData = SaveManager::extractODM(m_loadingSavePath);
            if (odmData.empty()) {
                std::cerr << "  Failed to extract .odmap from " << m_loadingSavePath << std::endl;
                m_loadError = "That save file has no map inside it, or could not "
                              "be read:\n" + m_loadingSavePath;
                m_loadingFailed = true;
                m_loadingPhase = LOAD_DONE;
                hideLoadingScreen();
                return;
            }
            m_loadingTempOdm = m_loadingSavePath + ".tmp.odmap";
            {
                std::ofstream out(m_loadingTempOdm, std::ios::binary);
                if (!out) {
                    std::cerr << "  Failed to write temp .odmap" << std::endl;
                    m_loadError = "The map could not be unpacked from the save. "
                                  "The game could not write to:\n" + m_loadingTempOdm;
                    m_loadingFailed = true;
                    m_loadingPhase = LOAD_DONE;
                    hideLoadingScreen();
                    return;
                }
                out.write(reinterpret_cast<const char*>(odmData.data()), odmData.size());
            }
            m_loadingOdmPath = m_loadingTempOdm;
            m_loadingPhase = LOAD_ODM;
            break;
        }
        case LOAD_ODM: {
            setLoadingProgress(0.04f, "Loading map data...");
            // THE ONE PHASE WITH NOWHERE TO PUT A PUMP. Every other heavy phase
            // is a loop over the raster or the country list, so it can yield
            // from inside; this one is a zip walk and then two stb_image
            // decodes of an 8192x4096 PNG -- single opaque calls, seconds each,
            // with no iteration of ours to hang a pump on. On the web that made
            // it the one part of loading that still looped a fragment after
            // every other phase had been fixed.
            //
            // So it is bracketed instead: silence for the decode, music for the
            // rest of the load. See Audio::BlockingCall.
            //
            // The scope ENDS ABOVE the failure branch, and that placement is
            // the point: the branch returns, and a suspend still held at that
            // return would outlive the load and silence the rest of the
            // session.
            bool odmOk;
    {
        Audio::BlockingCall quiet;
        odmOk = loadFromODM(m_loadingOdmPath);
        if (!odmOk) {
            std::string dir = m_loadingOdmPath.substr(0, m_loadingOdmPath.find_last_of('/') + 1);
            if (dir.empty()) dir = m_dataDir;
            odmOk = loadFromFiles();
        }
    }
    if (!odmOk) {
        // The one that a player actually hits. loadFromODM() failing means the
        // .odmap could not be opened or parsed, and loadFromFiles() cannot
        // rescue it in a shipped copy: it reads loose JSON that is not
        // packaged, so it only ever succeeds in a working tree.
        m_loadError = "This map could not be loaded:\n" + m_loadingOdmPath +
                      "\n\nThe file may be missing, incomplete or blocked by "
                      "security software. Check that the game's data folder is "
                      "intact.";
        m_loadingFailed = true;
        m_loadingPhase = LOAD_DONE;
        hideLoadingScreen();
        return;
    }
    // Reload countries from individual file to override .odmap's baked-in -99 ISO codes
    std::string countriesPath = m_dataDir + "countries.json";
    m_countries.load(countriesPath);
    m_loadingPhase = LOAD_GAME_DATA_RESOURCES;
            break;
        }
        case LOAD_GAME_DATA_RESOURCES: {
            setLoadingProgress(0.15f, "Loading resources...");
            // Always load step 1 - ODM may have resources but policies/etc are in JSON files
            loadGameDataStep1();
            m_loadingPhase = LOAD_GAME_DATA_OTHER;
            break;
        }
        case LOAD_GAME_DATA_OTHER: {
            setLoadingProgress(0.30f, "Loading game data...");
            printf("[LOAD] LOAD_GAME_DATA_OTHER: m_relations.size=%zu\n", m_relations.size());
            // Always load step 2 - relations may be in ODM but policies/ships/etc are in JSON files
            loadGameDataStep2();
            m_loadingPhase = LOAD_INIT_RENDERER;
            break;
        }
        case LOAD_INIT_RENDERER: {
            setLoadingProgress(0.40f, "Initializing renderer...");
            // Free the previous world's renderer — each one holds full-map
            // textures, so leaking it per load exhausts GPU memory (fatal for
            // long AI training runs that reload dozens of maps).
            if (m_renderer) { delete m_renderer; m_renderer = nullptr; }
            m_renderer = new MapRenderer(m_screenW, m_screenH,
                                         m_landSea.getWidth(), m_landSea.getHeight());
            if (m_renderer) {
                m_renderer->setDpiScale(GetWindowScaleDPI().x);
                m_renderer->computeBorderTexture(m_provinces.getImage());
                m_renderer->setPoliticalTexture(m_politicalTex);
                m_renderer->setFallbackFont(m_gameFont);
            }
            m_loadingPhase = LOAD_BUILD_POP;
            break;
        }
        case LOAD_BUILD_POP: {
            setLoadingProgress(0.50f, "Building population data...");
            buildPopulationLookups();
            m_loadingPhase = LOAD_GEN_RESOURCE_TEXTURES;
            break;
        }
        case LOAD_GEN_RESOURCE_TEXTURES: {
            int w = m_provinces.getWidth();
            int h = m_provinces.getHeight();
            if (m_loadingResIdx == 0) {
                for (int r = 0; r < 5; ++r)
                    m_resourceBuffers[r].resize(w * h, Color{40, 40, 40, 255});
            }
            if (m_loadingResIdx < 5 && !m_provinceResources.empty()) {
                generateResourceTextureFor(m_loadingResIdx);
                setLoadingProgress(0.55f + m_loadingResIdx * 0.05f,
                    TextFormat("Generating %s textures...", RESOURCE_NAMES[m_loadingResIdx]));
                m_loadingResIdx++;
                break;
            }
            if (!m_provinceResources.empty() || m_loadingResIdx >= 5) {
                // Two more full-map uploads, adjacent, and neither has a loop
                // to pump from -- but measured at about 200 ms together, which
                // is four audio periods. Too short to be worth suspending the
                // device: the stop and restart of the music would be more
                // noticeable than the few repeated blocks it prevents. Top the
                // buffer up instead. Same reasoning as computeBorderTexture().
                Audio::get().pump();

                Image resImg{};
                resImg.data = m_resourceBuffers[0].data();
                resImg.width = w;
                resImg.height = h;
                resImg.mipmaps = 1;
                resImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                Texture2D resTex = LoadTextureFromImage(resImg);
                SetTextureFilter(resTex, TEXTURE_FILTER_BILINEAR);
                if (m_renderer) m_renderer->setResourceTexture(resTex);

                m_claimsPixelBuffer.resize(w * h, Color{0, 0, 0, 0});
                Image claimsImg{};
                claimsImg.data = m_claimsPixelBuffer.data();
                claimsImg.width = w;
                claimsImg.height = h;
                claimsImg.mipmaps = 1;
                claimsImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
                Texture2D claimsTex = LoadTextureFromImage(claimsImg);
                SetTextureFilter(claimsTex, TEXTURE_FILTER_BILINEAR);
                if (m_renderer) m_renderer->setClaimsTexture(claimsTex);
            }
            m_loadingPhase = LOAD_GEN_ICONS;
            break;
        }
        case LOAD_GEN_ICONS: {
            setLoadingProgress(0.82f, "Generating icons...");
            generateIcons();
            m_loadingPhase = LOAD_BUILD_PROV_DATA;
            break;
        }
        case LOAD_BUILD_PROV_DATA: {
            setLoadingProgress(0.88f, "Building province data...");
            if (m_renderer) {
                m_renderer->buildProvinceData(m_provinces, m_provinceCenters, m_provinceRadius);
            }
            {
                std::unordered_map<int, Vector2> sums;
                std::unordered_map<int, int> cnts;
                for (auto& [pid, pc] : m_provinceCenters) {
                    auto* p = m_provinces.getProvinceById(pid);
                    if (p && p->countryId > 0) {
                        sums[p->countryId].x += pc.x;
                        sums[p->countryId].y += pc.y;
                        cnts[p->countryId]++;
                    }
                }
                for (auto& [cid, s] : sums)
                    m_countryCenters[cid] = {s.x / cnts[cid], s.y / cnts[cid]};
            }
            m_loadingPhase = LOAD_COMPUTE_LABELS;
            break;
        }
        case LOAD_COMPUTE_LABELS: {
            setLoadingProgress(0.90f, "Computing country labels...");
            computeCountryLabels();
            if (m_renderer) {
                m_renderer->setCountryLabels(&m_countryLabels);
                m_renderer->setMaxZoom(m_config.maxZoom);
                m_renderer->setDebugMode(m_config.debugMode);
            }
            rebuildFlags();
            if (m_renderer) m_renderer->setCountryFlags(&m_countryFlags);
            m_loadingPhase = LOAD_CREATE_SAVE;
            break;
        }
        case LOAD_CREATE_SAVE: {
            setLoadingProgress(0.95f, "Creating save file...");
            if (m_loadingShouldCreateSave && !m_loadingWorldName.empty()) {
                // Network games live apart from singleplayer worlds. They are
                // not interchangeable -- a multiplayer save is one machine's
                // view of a game somebody else is the authority for, and
                // loading one from Load World would look like a normal world
                // and behave like nothing at all.
                std::string saveDir = m_mpLoad != MpLoad::None
                    ? m_dataDir + "saves/multiplayer/"
                    : m_dataDir + "saves/";
                std::error_code mkec;
                std::filesystem::create_directories(saveDir, mkec);
                std::string worldName = m_loadingWorldName;
                std::string savePath = saveDir + worldName + ".odsv";
                struct stat chkStat;
                std::string baseName = worldName;
                int suffix = 1;
                while (stat(savePath.c_str(), &chkStat) == 0) {
                    worldName = baseName + " (" + std::to_string(suffix) + ")";
                    savePath = saveDir + worldName + ".odsv";
                    suffix++;
                }

                const std::string odmBytes = odFile::readAll(m_loadingOdmPath);
                std::vector<uint8_t> odmData(odmBytes.begin(), odmBytes.end());

                time_t now = time(nullptr);
                struct tm* tmLocal = localtime(&now);
                char ts[64];
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tmLocal);
                std::string timestamp = ts;

                SaveMetadata meta;
                meta.saveName = worldName;
                meta.version = GAME_VERSION;
                meta.created = timestamp;
                meta.lastPlayed = timestamp;
                meta.turnCount = 0;
                meta.provinceCount = (int)m_provincePopulations.size();
                meta.shipCount = (int)m_ships.size();
                for (auto& [cid, c] : m_countries.getAll())
                    meta.countryTreasuries[cid] = c.treasury;

                m_currentSavePath = savePath;
                if (SaveManager::createSave(m_currentSavePath,
                        std::string(odmData.begin(), odmData.end()), meta)) {
                    m_autoCreatedSave = true;
                    m_unsavedChanges = false;
                    m_turnCount = 0;
                    // Write initial state.json snapshot (turn 0, no pending orders yet)
                    {
                        std::vector<std::pair<std::string, std::string>> rebelFiles;
                        for (auto& [cid2, svg] : m_rebelFlagSvgs)
                            rebelFiles.push_back({"rebellion/" + std::to_string(cid2) + ".svg", svg});
                        // Persist the rebel countries themselves, not just their flags —
                        // otherwise their provinces reload as ownerless limbo.
                        { std::string rj = buildRebelsJson(); if (!rj.empty()) rebelFiles.push_back({"rebels.json", rj}); }
                        SaveManager::writeState(m_currentSavePath, saveStateJson(), rebelFiles);
                    }
                    std::cout << "Auto-created save: " << m_currentSavePath << std::endl;
                }
                m_loadingShouldCreateSave = false;
                m_loadingWorldName.clear();
            }
            m_loadingPhase = LOAD_SAVE_FINALIZE;
            break;
        }
        case LOAD_SAVE_FINALIZE: {
            setLoadingProgress(0.96f, "Replaying save turns...");
            if (!m_loadingSavePath.empty()) {
                m_currentSavePath = m_loadingSavePath;
                m_autoCreatedSave = false;
                try {
                    auto meta = SaveManager::readMetadata(m_loadingSavePath);
                    m_turnCount = meta.turnCount;
                    // Restore treasuries from save metadata
                    for (auto& [cid, tr] : meta.countryTreasuries) {
                        auto& all = m_countries.getAll();
                        auto it = all.find(cid);
                        if (it != all.end()) it->second.treasury = tr;
                    }
                } catch (...) {
                    m_turnCount = 0;
                }
                m_unsavedChanges = false;

                bool replayOk = replaySaveTurns(m_loadingSavePath);
                if (!replayOk) {
                    std::cerr << "  Failed to replay save turns" << std::endl;
                }
                if (!m_loadingTempOdm.empty()) {
                    std::remove(m_loadingTempOdm.c_str());
                    m_loadingTempOdm.clear();
                }
                m_loadingSavePath.clear();
            }
            m_loadingPhase = LOAD_FINALIZE;
            break;
        }
        case LOAD_FINALIZE: {
            setLoadingProgress(0.98f, "Finalizing...");
            m_screenW = GetScreenWidth();
            m_screenH = GetScreenHeight();
            if (m_renderer) m_renderer->resize(m_screenW, m_screenH);

            // Build list of playable countries (exclude UNC, BLC, and countries with no provinces)
            std::unordered_map<int, int> countryProvCount;
            for (auto& [pid, p] : m_provinces.getAllProvinces()) {
                countryProvCount[p.countryId]++;
            }
            m_playableCountryIds.clear();
            for (auto& [cid, c] : m_countries.getAll()) {
                if (cid == UNC_CID || cid == BLC_CID) continue;
                if (countryProvCount[cid] == 0) continue;
                m_playableCountryIds.push_back(cid);
            }
            // Sort by country name
            std::sort(m_playableCountryIds.begin(), m_playableCountryIds.end(),
                [this](int a, int b) {
                    auto ac = m_countries.getCountry(a);
                    auto bc = m_countries.getCountry(b);
                    if (!ac || !bc) return a < b;
                    return ac->name < bc->name;
                });
            m_playerCountryId = 0;
            m_countrySelectIndex = -1; // -1 = no country selected
            m_countrySelectScroll = 0;

            // Claims on ground the claimant already holds are bookkeeping that
            // cannot mean anything, so they are cleaned up here. Relations are
            // NOT: whatever the map declared is the situation the map wanted.
            dropSelfOwnedClaims();

            // Reset renderer state for country selection mode
            m_renderer->setSelectedProvince(0);
            m_renderer->rebuildSelectionGlow();
            m_renderer->setBlockLeftPan(false);
            m_renderer->setShowClaims(false);
            m_renderer->setShowPopulation(false);
            m_renderer->setShowRelations(false);
            m_renderer->setShowIndustry(false);
            m_renderer->setShowResource(-1);
            m_renderer->setShowCountryNames(true);
            // Zoom out to show the full world
            {
                float worldZ = std::min((float)m_screenW / (float)m_landSea.getWidth(),
                                        (float)m_screenH / (float)m_landSea.getHeight()) * 0.9f;
                m_renderer->flyTo(m_landSea.getWidth() / 2.0f, m_landSea.getHeight() / 2.0f, worldZ, 10.0f);
            }

            std::cout << "  Loaded " << m_provinces.getAllProvinces().size() << " provinces, "
                      << m_countries.size() << " countries, "
                      << m_playableCountryIds.size() << " playable" << std::endl;

            setLoadingProgress(1.0f, "Select your country!");
            m_loadingPhase = LOAD_DONE;
            hideLoadingScreen();
            // A multiplayer load ends somewhere else entirely: a host goes back
            // to its lobby, a joiner goes straight into the game as the country
            // the server gave it. Neither wants the country-select screen.
            if (m_mpLoad != MpLoad::None) {
                mpOnWorldLoaded();
                break;
            }
            if (m_loadingShouldCreateSave) {
                // New game: show country selection (no saved player country yet)
                m_currentScreen = SCREEN_COUNTRY_SELECT;
                recordIncomeSnapshot();
            } else {
                // Loaded game: check if a player country was saved
                int savedCid = 0;
                SaveMetadata meta = SaveManager::readMetadata(m_currentSavePath);
                savedCid = meta.playerCountryId;
                if (savedCid > 0 && m_countries.getCountry(savedCid)) {
                    m_playerCountryId = savedCid;
                    // Sync per-country research into global nodes
                    for (auto& n : m_researchNodes)
                        n.researched = hasResearched(n.id, savedCid);
                    std::cout << "  Restored player: " << m_countries.getCountry(savedCid)->name << std::endl;
                } else {
                    // No saved country — prompt user to choose
                    m_playerCountryId = 0;
                    m_currentScreen = SCREEN_COUNTRY_SELECT;
                }
if (m_currentScreen != SCREEN_COUNTRY_SELECT) {
                    // Skip selection, go straight to playing
                    if (m_renderer) {
                        m_renderer->setBlockLeftPan(false);
                        m_renderer->setShowCountryNames(false);
                    }
                    const Country* pc = m_countries.getCountry(m_playerCountryId);
                    std::cerr << "[DIAG] Entering gameplay as country " << m_playerCountryId
                              << " (" << (pc ? pc->isoA3 : "?") << ")" << std::endl;
                    auto compassIt = m_countryCompass.find(m_playerCountryId);
                    if (compassIt != m_countryCompass.end()) {
                        std::cerr << "[DIAG]   Compass: econ=" << compassIt->second.economic
                                  << " soc=" << compassIt->second.social << std::endl;
                    } else {
                        std::cerr << "[DIAG]   NO COMPASS ENTRY!" << std::endl;
                    }
                    m_currentScreen = SCREEN_PLAYING;
                }
            }
            // A revert queued from the history screen: the world only just came
            // back up, so this is the first point at which the rewind is safe.
            if (m_pendingRevertTurn >= 0) {
                int t = m_pendingRevertTurn;
                std::string sp = m_pendingRevertSave;
                m_pendingRevertTurn = -1;
                m_pendingRevertSave.clear();
                if (!applyTurnRewind(sp, t))
                    std::cerr << "  Revert to turn " << t << " failed: "
                              << m_historyStatus << std::endl;
            }

            // Record initial income snapshot
            recordIncomeSnapshot();
            break;
        }
        default:
            break;
    }
}

void Game::setLoadingProgress(float progress, const std::string& status) {
    m_loadingProgress = std::clamp(progress, 0.0f, 1.0f);
    m_loadingStatus = status;

    // Every loading phase announces itself through here, which makes this the
    // one place that reliably sits between two blocks of heavy work. The main
    // loop is not running during those, so without this the music stream is
    // never refilled and stutters its way through the whole loading screen.
    Audio::get().pump();
}

void Game::drawLoadingScreen() {
    if (!m_showLoadingScreen) return;

    int centerX = m_screenW / 2;
    int centerY = m_screenH / 2;

    // Dark background
    DrawRectangle(0, 0, m_screenW, m_screenH, {10, 10, 15, 255});

    // Throbber - 8 squares in a square arrangement, scrolling clockwise with opacity fade
    float time = GetTime();

    // Layout: 3x3 grid, positions 0-7 as: 0 1 2 / 7 c 3 / 6 5 4
    static const int s_offsets[8][2] = {
        {-1, -1}, {0, -1}, {1, -1},  // top row: 0, 1, 2
        {1, 0},                      // right: 3
        {1, 1}, {0, 1}, {-1, 1},    // bottom row: 4, 5, 6
        {-1, 0}                      // left: 7
    };

    float sqSize = 12.0f;
    float gap = 4.0f;
    float totalSize = sqSize * 3 + gap * 2;
    float startX = centerX - totalSize / 2 + sqSize / 2;
    float startY = centerY - totalSize / 2 + sqSize / 2;

    float period = 1.2f;  // full rotation period in seconds

    for (int i = 0; i < 8; ++i) {
        // Current rotation phase: each square is at a different phase offset
        float phase = (float)i / 8.0f;  // 0.0 to 0.875
        float t = fmodf(time / period + phase, 1.0f);  // 0..1 for this square

        // Opacity: peak at t=0, fades to 0 at t=1
        float alpha = (1.0f - t) * 255.0f;
        if (alpha < 0) alpha = 0;
        if (alpha > 255) alpha = 255;

        float px = startX + s_offsets[i][0] * (sqSize + gap);
        float py = startY + s_offsets[i][1] * (sqSize + gap);

        DrawRectangle((int)(px - sqSize/2), (int)(py - sqSize/2), (int)sqSize, (int)sqSize,
                      ColorAlpha(hexToColor(m_config.accent()), alpha/255.0f));
    }

    // Status text
    int statusW = MeasureText(m_loadingStatus.c_str(), 24);
    DrawText(m_loadingStatus.c_str(), centerX - statusW / 2, centerY + 80, 24, WHITE);

    // Percentage (bottom right)
    float progress = m_loadingProgress;
    int percent = (int)(progress * 100);
    char percentStr[16];
    snprintf(percentStr, sizeof(percentStr), "%d%%", percent);
    int percentW = MeasureText(percentStr, 32);
    DrawText(percentStr, m_screenW - percentW - 30, m_screenH - 50, 32, hexToColor(m_config.accent()));

    // Progress bar (bottom center)
    int barW = 400;
    int barH = 8;
    int barX = centerX - barW / 2;
    int barY = m_screenH - 100;
    DrawRectangleRounded({(float)barX, (float)barY, (float)barW, (float)barH}, 0.1f, 4, {40, 40, 50, 255});
    DrawRectangleRounded({(float)barX, (float)barY, (float)(barW * progress), (float)barH}, 0.1f, 4, hexToColor(m_config.accent()));

    // Tips (bottom left)
    if (!m_loadingTips.empty()) {
        // Update tip timer
        m_tipTimer += GetFrameTime();
        if (m_tipTimer > 5.0f) { // Change tip every 5 seconds
            m_tipTimer = 0;
            m_currentTipIndex = (m_currentTipIndex + 1) % m_loadingTips.size();
        }

        const std::string& tip = m_loadingTips[m_currentTipIndex];
        DrawText("Tip:", 30, m_screenH - 50, 18, hexToColor(m_config.accent()));
        DrawText(tip.c_str(), 30, m_screenH - 25, 16, LIGHTGRAY);
    }

    EndDrawing();
}

void Game::buildPopulationLookups() {
    rebuildIsoIndex(); // countries are final by the time lookups are (re)built
    int maxPid = 0;
    for (auto& [pid, prov] : m_provinces.getAllProvinces())
        if (pid > maxPid) maxPid = pid;
    maxPid = std::max(maxPid, 65534);
    m_provinceCountryLookup.assign(maxPid + 1, 0);
    m_provincePopArray.assign(maxPid + 1, 0);
    m_provinceConquestTurn.clear();
    m_conqueredProvincePrevOwner.clear();
    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        m_provinceCountryLookup[pid] = prov.countryId;
        auto it = m_provincePopulations.find(pid);
        if (it != m_provincePopulations.end())
            m_provincePopArray[pid] = it->second;
    }
    rebuildCountryProvinceIndex();

    // Build per-pixel country array and country pixel lists for fast updates
    const Image& provImg = m_provinces.getImage();
    int w = provImg.width;
    int h = provImg.height;
    int totalPixels = w * h;
    const auto* srcPixels = (const Color*)provImg.data;

    m_pixelCountryArray.assign(totalPixels, 0);
    m_populationPixelBuffer.resize(totalPixels);
    m_politicalPixelBuffer.resize(totalPixels);
    m_gradientDist.assign(totalPixels, 255);

    int maxCid = 0;
    for (auto& [cid, c] : m_countries.getAll())
        if (cid > maxCid) maxCid = cid;
    // These two per-pixel index maps are rebuilt from scratch by the loop
    // below. On the async (training) reload path nothing else clears them, and
    // province/country IDs repeat across maps — so without clearing, each map
    // load APPENDS its pixels onto the previous map's lists. That is a
    // ~100 MB-per-map leak that OOM-kills long training runs (the crash after a
    // dozen map rotations). Clear both before repopulating.
    m_countryPixels.clear();
    m_provincePixels.clear();
    m_coastalCache.clear();   // answers belong to the map that is going away
    m_portAnchorCache.clear();
    m_countryPixels.resize(maxCid + 1);
    m_countryRelationColors.assign(maxCid + 1, Color{80, 80, 80, 255});

    for (int i = 0; i < totalPixels; ++i) {
        // Once a raster row's worth. Same reason as the scans in MapRenderer:
        // this is a full-map pass and nothing refills the music while it runs.
        if ((i & 8191) == 0) Audio::get().pump();
        Color src = srcPixels[i];
        int pid = Province::colorToId(src.r, src.g, src.b);
        int cid = 0;
        if (pid > 0 && (size_t)pid < m_provinceCountryLookup.size())
            cid = m_provinceCountryLookup[pid];
        m_pixelCountryArray[i] = cid;

        if (pid == 0 || cid == 0) {
            m_populationPixelBuffer[i] = Color{10, 15, 40, 255};
            m_politicalPixelBuffer[i] = Color{10, 15, 40, 255};
        } else {
            m_populationPixelBuffer[i] = Color{60, 60, 60, 255};
            const Country* c = m_countries.getCountry(cid);
            m_politicalPixelBuffer[i] = c ? c->color : Color{80, 80, 80, 255};
        }

        if (cid > 0 && cid <= maxCid)
            m_countryPixels[cid].push_back(i);

        if (pid > 0)
            m_provincePixels[pid].push_back(i);
    }

    // Compute gradient distance field (multi-source BFS from borders)
    // 8-neighbor with scaled distances: orth = +2, diag = +3 (ratio ~1:1.5 approximating 1:√2)
    {
        struct QE { int idx; uint8_t dist; };
        std::vector<QE> queue;
        auto enqueue = [&](int idx, uint8_t d) {
            if (idx < 0 || idx >= totalPixels) return;
            if (m_gradientDist[idx] <= d) return;
            m_gradientDist[idx] = d;
            queue.push_back({idx, d});
        };
        // First pass: mark pixels adjacent to a different country or land/sea boundary
        for (int y = 0; y < h; ++y) {
            // MEASURED: this field is the last unpumped stall of a web load.
            // It sits between the per-pixel loop above, which yields, and the
            // texture uploads below, which are bracketed -- so it was the one
            // stretch left where the browser looped a fragment, about 0.8 s of
            // it. Attributing it took a timeline of the audio callback against
            // the suspend windows either side; it is not obvious from reading,
            // because nothing here looks as expensive as a raster scan.
            Audio::get().pump();
            for (int x = 0; x < w; ++x) {
                int i = y * w + x;
                int cid = m_pixelCountryArray[i];
                // Check 4 orthogonal neighbors for border detection
                int nx[4] = {x-1, x+1, x, x};
                int ny[4] = {y, y, y-1, y+1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || nx[k] >= w) continue;
                    if (ny[k] < 0 || ny[k] >= h) continue;
                    int ni = ny[k] * w + nx[k];
                    if (m_pixelCountryArray[ni] != cid) {
                        enqueue(i, 0);
                        break;
                    }
                }
            }
        }
        // BFS outward using 8 neighbors
        size_t qpos = 0;
        while (qpos < queue.size()) {
            // The queue is seeded with every border pixel and grows to cover
            // the interiors, so this runs far longer than the seeding pass
            // above. Same reason, same yield.
            if ((qpos & 8191) == 0) Audio::get().pump();
            QE cur = queue[qpos++];
            if (cur.dist >= 60) continue;
            int x = cur.idx % w;
            int y = cur.idx / w;
            int nx[8] = {x-1, x+1, x, x, x-1, x-1, x+1, x+1};
            int ny[8] = {y, y, y-1, y+1, y-1, y+1, y-1, y+1};
            for (int k = 0; k < 8; ++k) {
                if (nx[k] < 0 || nx[k] >= w || ny[k] < 0 || ny[k] >= h) continue;
                int step = (k < 4) ? 2 : 3; // orth +2, diag +3
                enqueue(ny[k] * w + nx[k], cur.dist + step);
            }
        }
    }

    // ONE guard around all three, not one each. Two full-map uploads and a
    // third pass inside generatePoliticalTexture() run back to back with
    // nothing between them, and the bracket nests -- so this is a single
    // suspend and resume rather than three, which is one gap in the music
    // instead of three.
    {
        Audio::BlockingCall quiet;

        // Create political texture from the pixel buffer
        Image polImg{};
        polImg.data = m_politicalPixelBuffer.data();
        polImg.width = w;
        polImg.height = h;
        polImg.mipmaps = 1;
        polImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D polTex = LoadTextureFromImage(polImg);
        SetTextureFilter(polTex, TEXTURE_FILTER_BILINEAR);
        m_renderer->setPoliticalTexture(polTex);
        m_politicalTex = polTex;

        // Create initial population texture from the base buffer
        Image initImg{};
        initImg.data = m_populationPixelBuffer.data();
        initImg.width = w;
        initImg.height = h;
        initImg.mipmaps = 1;
        initImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D tex = LoadTextureFromImage(initImg);
        SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
        m_renderer->setPopulationTexture(tex);

        // Apply gradient to political texture immediately (not just after turn processing)
        generatePoliticalTexture();
    }
}

// The inward shading every country carries is a distance field: 0 on a border
// pixel, growing to 60 as you move into the interior. It has to be rebuilt
// whenever a border moves, which before this was only on load, sync and
// replay -- so in a running game the shading kept fading toward borders that
// had been redrawn turns ago.
//
// One full-raster BFS. Cheap enough once per turn, far too expensive per
// frame, which is why the caller checks m_gradientDirty first.
void Game::rebuildGradientField() {
    const Image& provImg = m_provinces.getImage();
    int w2 = provImg.width, h2 = provImg.height;
    int totalPixels = w2 * h2;
    if ((int)m_pixelCountryArray.size() != totalPixels) return;

    struct QE2 { int idx; uint8_t dist; };
    std::vector<QE2> queue;
    m_gradientDist.assign(totalPixels, 255);
    auto enqueue = [&](int idx, uint8_t d) {
        if (idx < 0 || idx >= totalPixels) return;
        if (m_gradientDist[idx] <= d) return;
        m_gradientDist[idx] = d;
        queue.push_back({idx, d});
    };
    // Yielded exactly like the copy of this field inside
    // buildPopulationLookups(), and for a reason that applies more often: that
    // one runs once per load, this runs once per TURN, so an unyielded pass
    // here is a fragment of music looping every time a border moves.
    for (int y = 0; y < h2; ++y) {
        Audio::get().pump();
        for (int x = 0; x < w2; ++x) {
            int i = y * w2 + x;
            int cid = m_pixelCountryArray[i];
            int nx[4] = {x-1, x+1, x, x};
            int ny[4] = {y, y, y-1, y+1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || nx[k] >= w2) continue;
                if (ny[k] < 0 || ny[k] >= h2) continue;
                if (m_pixelCountryArray[ny[k] * w2 + nx[k]] != cid) { enqueue(i, 0); break; }
            }
        }
    }
    size_t qpos = 0;
    while (qpos < queue.size()) {
        if ((qpos & 8191) == 0) Audio::get().pump();
        QE2 cur = queue[qpos++];
        if (cur.dist >= 60) continue;
        int x = cur.idx % w2, y = cur.idx / w2;
        int nx[8] = {x-1, x+1, x, x, x-1, x-1, x+1, x+1};
        int ny[8] = {y, y, y-1, y+1, y-1, y+1, y-1, y+1};
        for (int k = 0; k < 8; ++k) {
            if (nx[k] < 0 || nx[k] >= w2 || ny[k] < 0 || ny[k] >= h2) continue;
            enqueue(ny[k] * w2 + nx[k], cur.dist + ((k < 4) ? 2 : 3));
        }
    }
    m_gradientDirty = false;
}

void Game::generatePoliticalTexture() {
    int w = m_provinces.getWidth();
    int h = m_provinces.getHeight();
    int total = w * h;
    // Borders moved since the field was built, so rebuild it before shading.
    if (m_gradientDirty) rebuildGradientField();
    if ((int)m_politicalPixelBuffer.size() != total || (int)m_gradientDist.size() != total) {
        return;
    }
    const Image& provImg = m_provinces.getImage();
    const auto* srcPixels = (const Color*)provImg.data;
    for (int i = 0; i < total; ++i) {
        int cid = m_pixelCountryArray[i];
        float t = std::min(1.0f, m_gradientDist[i] / 60.0f);
        if (cid <= 0) {
            uint8_t rb = (uint8_t)(8 + (uint8_t)((1.0f - t) * 16));
            uint8_t gb = (uint8_t)(10 + (uint8_t)((1.0f - t) * 22));
            uint8_t bb = (uint8_t)(15 + (uint8_t)((1.0f - t) * 38));
            m_politicalPixelBuffer[i] = {rb, gb, bb, 255};
        } else {
            const Country* c = m_countries.getCountry(cid);
            Color base = c ? c->color : Color{80, 80, 80, 255};
            m_politicalPixelBuffer[i] = blendColor(base, t);
        }
    }
    // 1px dark borders at country boundaries (handles gradient field staleness after ownership changes)
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int i = y * w + x;
            int cid = m_pixelCountryArray[i];
            if (cid <= 0) continue;
            int nx[4] = {x-1, x+1, x, x};
            int ny[4] = {y, y, y-1, y+1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || nx[k] >= w || ny[k] < 0 || ny[k] >= h) continue;
                int ni = ny[k] * w + nx[k];
                if (m_pixelCountryArray[ni] != cid) {
                    m_politicalPixelBuffer[i] = {(uint8_t)(m_politicalPixelBuffer[i].r / 3),
                                                  (uint8_t)(m_politicalPixelBuffer[i].g / 3),
                                                  (uint8_t)(m_politicalPixelBuffer[i].b / 3), 255};
                    break;
                }
            }
        }
    }
    m_renderer->updatePoliticalTexture(m_politicalPixelBuffer.data());
}

void Game::generatePopulationTexture(int countryId, int prevCountryId) {
    int w = m_provinces.getWidth();
    int h = m_provinces.getHeight();

    // Reset previous country's pixels to grey
    if (prevCountryId > 0 && (size_t)prevCountryId < m_countryPixels.size()) {
        for (int idx : m_countryPixels[prevCountryId]) {
            m_populationPixelBuffer[idx] = Color{60, 60, 60, 255};
        }
    }

    // Color new country's pixels with red-green gradient
    if (countryId > 0 && (size_t)countryId < m_countryPixels.size()) {
        long long maxPop = 1;
        for (size_t pid = 0; pid < m_provinceCountryLookup.size(); ++pid) {
            if (m_provinceCountryLookup[pid] == countryId && m_provincePopArray[pid] > maxPop)
                maxPop = m_provincePopArray[pid];
        }

        if (!m_countryPixels[countryId].empty()) {
            const auto* srcPixels = (const Color*)m_provinces.getImage().data;
            for (int idx : m_countryPixels[countryId]) {
                int pid = Province::colorToId(srcPixels[idx].r, srcPixels[idx].g, srcPixels[idx].b);
                long long pop = (size_t)pid < m_provincePopArray.size() ? m_provincePopArray[pid] : 0;
                float t = (float)pop / maxPop;
                m_populationPixelBuffer[idx] = Color{
                    (uint8_t)(255.0f * (1.0f - t)),
                    (uint8_t)(255.0f * t),
                    0, 255
                };
            }
        }
    }

    m_renderer->updatePopulationTexture(m_populationPixelBuffer.data());
}

void Game::generateRelationsTexture(int countryId, int prevCountryId) {
    int maxCid = (int)m_countryRelationColors.size() - 1;

    const Country* hc = (countryId > 0) ? m_countries.getCountry(countryId) : nullptr;
    if (hc)
        std::cout << "Relations: selected " << hc->name << " (ISO=" << hc->isoA3 << ")" << std::endl;

    // Precompute relation colors for all countries relative to the new selected country
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid < 0 || cid > maxCid) continue;

        Color col{80, 80, 80, 255};

        if (cid == countryId) {
            col = Color{0, 100, 255, 255};
        } else if (countryId > 0) {
            const Country* hc = m_countries.getCountry(countryId);
            if (hc) {
                // Check selected → target
                auto rt = m_relations.find(hc->isoA3);
                bool found = false;
                if (rt != m_relations.end()) {
                    auto st = rt->second.find(c.isoA3);
                    if (st != rt->second.end()) {
                        found = true;
                        auto& rel = st->second;
                        if (rel.war)               col = Color{255, 50, 50, 255};
                        else if (rel.alliance)      col = Color{50, 200, 50, 255};
                        else if (rel.guarantee)     col = Color{255, 255, 50, 255};
                        else if (rel.nonAggression) col = Color{255, 165, 0, 255};
                    }
                }
                // Fallback: check target → selected (symmetric display)
                if (!found) {
                    auto rt2 = m_relations.find(c.isoA3);
                    if (rt2 != m_relations.end()) {
                        auto st2 = rt2->second.find(hc->isoA3);
                        if (st2 != rt2->second.end()) {
                            auto& rel = st2->second;
                            if (rel.war)               col = Color{255, 50, 50, 255};
                            else if (rel.alliance)      col = Color{50, 200, 50, 255};
                            else if (rel.guarantee)     col = Color{255, 255, 50, 255};
                            else if (rel.nonAggression) col = Color{255, 165, 0, 255};
                        }
                    }
                }
            }
        }

        // Only write pixels if the color changed for this country
        Color& prevCol = m_countryRelationColors[cid];
        if (col.r != prevCol.r || col.g != prevCol.g || col.b != prevCol.b || col.a != prevCol.a) {
            auto& pixels = m_countryPixels[cid];
            if (!pixels.empty()) {
                for (int idx : pixels)
                    m_populationPixelBuffer[idx] = col;
            }
            m_countryRelationColors[cid] = col;
        }
    }

    m_renderer->updatePopulationTexture(m_populationPixelBuffer.data());
}

void Game::generateClaimsTexture() {
    int w = m_provinces.getWidth();
    std::fill(m_claimsPixelBuffer.begin(), m_claimsPixelBuffer.end(), Color{0, 0, 0, 0});

    int countryId = m_lastClaimsCountryId;
    if (countryId <= 0 || (size_t)countryId >= m_countryPixels.size()) {
        m_renderer->updateClaimsTexture(m_claimsPixelBuffer.data());
        return;
    }

    const Country* claimer = m_countries.getCountry(countryId);
    if (!claimer) {
        m_renderer->updateClaimsTexture(m_claimsPixelBuffer.data());
        return;
    }

    // Build set of involved country IDs: claimant + owners of claimed provinces
    std::unordered_set<int> involvedCids;
    involvedCids.insert(countryId);

    auto claimIt = m_claims.find(claimer->isoA3);
    if (claimIt != m_claims.end()) {
        for (int pid : claimIt->second) {
            if ((size_t)pid < m_provinceCountryLookup.size())
                involvedCids.insert(m_provinceCountryLookup[pid]);
        }
    }

    Color stripeCol{220, 140, 30, 200};

    // Color all countries: grey for non-involved, blue for claimant, stripes for claimed
    size_t maxCid = m_countryPixels.size();
    for (int cid = 0; cid < (int)maxCid; ++cid) {
        auto& pixels = m_countryPixels[cid];
        if (pixels.empty()) continue;

        if (cid == countryId) {
            // Claimant: blue tint
            for (int idx : pixels)
                m_claimsPixelBuffer[idx] = Color{0, 60, 180, 100};
        } else if (involvedCids.find(cid) != involvedCids.end()) {
            // Country that owns claimed provinces: stripe pattern on claimed provinces,
            // grey for non-claimed provinces
            for (int idx : pixels)
                m_claimsPixelBuffer[idx] = Color{60, 60, 60, 180};
        } else {
            // Non-involved: grey tint
            for (int idx : pixels)
                m_claimsPixelBuffer[idx] = Color{60, 60, 60, 180};
        }
    }

    // Overlay stripe pattern only on the specific claimed provinces
    if (claimIt != m_claims.end()) {
        for (int pid : claimIt->second) {
            auto ppIt = m_provincePixels.find(pid);
            if (ppIt == m_provincePixels.end()) continue;
            for (int idx : ppIt->second) {
                int x = idx % w;
                int y = idx / w;
                bool onStripe = ((x + y) % 10) < 4;
                m_claimsPixelBuffer[idx] = onStripe ? stripeCol : Color{0, 0, 0, 0};
            }
        }
    }

    m_renderer->updateClaimsTexture(m_claimsPixelBuffer.data());
}

void Game::generateResourceTextureFor(int resIdx) {
    int w = m_provinces.getWidth();
    int h = m_provinces.getHeight();

    auto& buf = m_resourceBuffers[resIdx];

    // First pass: find max resource amount for this resource type
    float maxAmount = 1.0f;
    auto getAmount = [&](int pid) -> float {
        auto it = m_provinceResources.find(pid);
        if (it == m_provinceResources.end()) return 0.0f;
        switch (resIdx) {
            case 0: return it->second.oil.amount;
            case 1: return it->second.gold.amount;
            case 2: return it->second.rubber.amount;
            case 3: return it->second.gemstones.amount;
            case 4: return it->second.metal.amount;
            default: return 0.0f;
        }
    };
    for (auto& [pid, _] : m_provinceResources) {
        float a = getAmount(pid);
        if (a > maxAmount) maxAmount = a;
    }

    const auto* srcPixels = (const Color*)m_provinces.getImage().data;
    for (int idx = 0; idx < w * h; ++idx) {
        // Five of these run back to back, one per resource, each a full-map
        // pass. Unpumped they were five separate stalls in one loading phase.
        if ((idx & 8191) == 0) Audio::get().pump();
        auto& sp = srcPixels[idx];
        if (sp.r == 0 && sp.g == 0 && sp.b == 0) {
            buf[idx] = Color{0, 0, 0, 255};
            continue;
        }
        int pid = Province::colorToId(sp.r, sp.g, sp.b);
        float amount = getAmount(pid);
        float t = amount / maxAmount;

        uint8_t baseR = 80, baseG = 80, baseB = 80;
        uint8_t targetR, targetG, targetB;
        switch (resIdx) {
            case 0: targetR = 160; targetG = 50;  targetB = 200; break; // Oil:   grey -> purple
            case 1: targetR = 255; targetG = 215; targetB = 0;   break; // Gold:  grey -> gold-ish yellow
            case 2: targetR = 50;  targetG = 200; targetB = 50;  break; // Rubber: grey -> green
            case 3: targetR = 100; targetG = 200; targetB = 255; break; // Gems:  grey -> light-blue
            case 4: targetR = 255; targetG = 255; targetB = 255; break; // Metal: grey -> white
            default: targetR = 255; targetG = 255; targetB = 255; break;
        }
        uint8_t r = (uint8_t)(baseR + (targetR - baseR) * t);
        uint8_t g = (uint8_t)(baseG + (targetG - baseG) * t);
        uint8_t b = (uint8_t)(baseB + (targetB - baseB) * t);
        buf[idx] = Color{r, g, b, 255};
    }
}

void Game::generateResourceTexture() {
    m_renderer->updateResourceTexture(m_resourceBuffers[m_activeResourceIdx].data());
}

void Game::generateIcons() {
    m_iconPopulation = makeIcon(64, 64, [](Image& img) {
        ImageDrawCircle(&img, 16, 20, 6, WHITE);
        ImageDrawTriangle(&img, {8, 28}, {16, 50}, {24, 28}, WHITE);
        ImageDrawCircle(&img, 48, 20, 6, WHITE);
        ImageDrawTriangle(&img, {40, 28}, {48, 50}, {56, 28}, WHITE);
    });
    m_iconIndustry = makeIcon(64, 64, [](Image& img) {
        ImageDrawRectangle(&img, 14, 30, 36, 26, WHITE);
        ImageDrawTriangle(&img, {8, 30}, {32, 8}, {56, 30}, WHITE);
    });
    m_iconDefence = makeIcon(64, 64, [](Image& img) {
        ImageDrawRectangle(&img, 14, 7, 36, 28, WHITE);
        ImageDrawTriangle(&img, {14, 35}, {32, 57}, {50, 35}, WHITE);
    });
    m_iconRelations = makeIcon(64, 64, [](Image& img) {
        ImageDrawCircle(&img, 22, 20, 12, WHITE);
        ImageDrawCircle(&img, 42, 20, 12, WHITE);
        ImageDrawTriangle(&img, {10, 26}, {32, 56}, {54, 26}, WHITE);
    });
    m_iconArmyNav = makeIcon(64, 64, [](Image& img) {
        for (int r = 16; r <= 20; ++r) ImageDrawCircleLines(&img, 32, 32, r, WHITE);
        ImageDrawRectangle(&img, 29, 6, 6, 52, WHITE);
        ImageDrawRectangle(&img, 6, 29, 52, 6, WHITE);
        ImageDrawCircle(&img, 32, 32, 5, WHITE);
    });
    m_iconNavy = makeIcon(64, 64, [](Image& img) {
        ImageDrawTriangle(&img, {10, 42}, {54, 42}, {32, 54}, WHITE);
        ImageDrawRectangle(&img, 12, 38, 40, 4, WHITE);
        ImageDrawRectangle(&img, 26, 26, 16, 12, WHITE);
        ImageDrawRectangle(&img, 30, 8, 4, 18, WHITE);
        ImageDrawTriangle(&img, {30, 8}, {42, 12}, {30, 16}, WHITE);
    });
    m_iconResources = makeIcon(64, 64, [](Image& img) {
        ImageDrawCircle(&img, 32, 40, 16, WHITE);
        ImageDrawTriangle(&img, {16, 36}, {48, 36}, {32, 8}, WHITE);
    });
    m_iconCountryNames = makeIcon(64, 64, [](Image& img) {
        for (int r = 25; r <= 29; ++r) ImageDrawCircleLines(&img, 32, 32, r, WHITE);
        ImageDrawCircle(&img, 32, 18, 8, WHITE);
        ImageDrawTriangle(&img, {16, 34}, {32, 56}, {48, 34}, WHITE);
    });
    m_iconPolicies = makeIcon(64, 64, [](Image& img) {
        // Government building: pediment + 3 columns + base
        // Triangular pediment (roof)
        ImageDrawTriangle(&img, {6, 22}, {32, 4}, {58, 22}, WHITE);
        // Horizontal entablature below pediment
        ImageDrawRectangle(&img, 10, 22, 44, 5, WHITE);
        // 3 columns
        ImageDrawRectangle(&img, 14, 27, 7, 22, WHITE);
        ImageDrawRectangle(&img, 28, 27, 8, 22, WHITE);
        ImageDrawRectangle(&img, 43, 27, 7, 22, WHITE);
        // Base/steps
        ImageDrawRectangle(&img, 7, 49, 50, 10, WHITE);
    });
    m_iconEconomy = makeIcon(64, 64, [](Image& img) {
        // Economy: $ sign — 7px bold traced from SVG path
        int t = 7;
        ImageDrawLineEx(&img, {32, 2}, {32, 62}, t, WHITE);
        ImageDrawLineEx(&img, {48, 12}, {20, 12}, t, WHITE);
        ImageDrawLineEx(&img, {20, 12}, {12, 24}, t, WHITE);
        ImageDrawLineEx(&img, {12, 24}, {28, 34}, t, WHITE);
        ImageDrawLineEx(&img, {28, 34}, {36, 34}, t, WHITE);
        ImageDrawLineEx(&img, {36, 34}, {52, 44}, t, WHITE);
        ImageDrawLineEx(&img, {52, 44}, {40, 54}, t, WHITE);
        ImageDrawLineEx(&img, {40, 54}, {12, 54}, t, WHITE);
    });
    m_iconClaims = makeIcon(64, 64, [](Image& img) {
        // Flag with pole: swallowtail (notched) flag
        // Flagpole
        ImageDrawRectangle(&img, 8, 4, 5, 56, WHITE);
        // Pole top ornament
        ImageDrawCircle(&img, 10, 4, 4, WHITE);
        // Flag body: swallowtail polygon using triangle fan
        // Polygon: (10,10) → (56,10) → (42,18) → (56,26) → (10,26)
        Vector2 pts[] = {
            {16, 18},  // interior fan center
            {10, 10},  // top-left (at pole)
            {56, 10},  // top-right
            {42, 18},  // notch tip
            {56, 26},  // bottom-right
            {10, 26},  // bottom-left (at pole)
        };
        ImageDrawTriangleFan(&img, pts, 6, WHITE);
    });
    m_iconResearch = makeIcon(64, 64, [](Image& img) {
        // Florence flask: neck (thin) + bulbous body + liquid line
        int cx = 32;
        // Neck
        ImageDrawRectangle(&img, cx - 5, 6, 10, 22, WHITE);
        // Rim at top of neck
        ImageDrawRectangle(&img, cx - 8, 4, 16, 3, WHITE);
        // Body - left sloping side
        ImageDrawTriangle(&img, {27, 28}, {8, 54}, {27, 56}, WHITE);
        // Body - right sloping side
        ImageDrawTriangle(&img, {37, 28}, {56, 54}, {37, 56}, WHITE);
        // Fill center rectangle
        ImageDrawRectangle(&img, 27, 28, 10, 28, WHITE);
        // Bottom closure
        ImageDrawRectangle(&img, 8, 54, 48, 4, WHITE);
        // Inner liquid level line
        ImageDrawRectangle(&img, 18, 42, 28, 3, WHITE);
    });
}

void Game::computeCountryLabels() {
    m_countryLabels.clear();
    Font font = GetFontDefault();
    float spacing = 3.0f;

    // Build province → country look-up (use flat array for fast pixel scans)
    std::unordered_map<int, std::vector<int>> countryProvs;
    int maxPid = 0;
    for (auto& [pid, pc] : m_provinceCenters) {
        if (pid > maxPid) maxPid = pid;
        auto* p = m_provinces.getProvinceById(pid);
        if (p && p->countryId > 0 && p->countryId != UNC_CID && p->countryId != BLC_CID) {
            countryProvs[p->countryId].push_back(pid);
        }
    }
    std::vector<int> pidToCountry(maxPid + 1, 0);
    for (auto& [pid, pc] : m_provinceCenters) {
        auto* p = m_provinces.getProvinceById(pid);
        if (p && p->countryId > 0 && p->countryId != BLC_CID)
            pidToCountry[pid] = p->countryId;
    }

    // ── Pixel adjacency phase ──────────────────────────────────────────
    // Scan the province raster once and find every pixel-adjacent pair
    // of provinces that belong to the same country (8-connectivity).
    // This guarantees real landmass connectivity: Alaska vs mainland US,
    // separate islands, etc. all get their own components.
    // 8-connectivity (including diagonals) avoids splitting provinces that
    // only touch at a corner.
    const Image& img = m_provinces.getImage();
    int w = img.width, h = img.height;
    const uint8_t* pixels = static_cast<const uint8_t*>(img.data);

    // Pack adjacent province pairs into int64_t (big << 32 | little)
    std::unordered_set<int64_t> adjPairs;
    auto pack = [](int a, int b) {
        if (a > b) std::swap(a, b);
        return (static_cast<int64_t>(a) << 32) | b;
    };

    // Offsets for 8-con scan: right, bottom, bottom-right, bottom-left
    static const int DX[4] = {1, 0, 1, -1};
    static const int DY[4] = {0, 1, 1,  1};

    // Also populate m_provinceNeighbors with ALL adjacencies (any province pair)
    m_provinceNeighbors.clear();
    std::unordered_set<int64_t> allAdjPairs;

    for (int y = 0; y < h; ++y) {
        // The longest uninterrupted stretch of work in the whole load: a
        // 8192x4096 raster is 33 million pixels, each looking at four
        // neighbours, and until it finishes nothing else on this thread runs.
        // On the web that includes the browser's audio callback, so the music
        // repeats the last fraction of a second it was given for as long as
        // this takes.
        //
        // ONCE A ROW, not once every 64. At this raster size 64 rows is longer
        // than a Web Audio period, so the old interval let the buffer run dry
        // between yields and the fragment looped anyway -- instrumentation that
        // was present and too sparse to work. pump() now rate-limits itself
        // before doing anything, so a call that is not due is a clock read.
        Audio::get().pump();

        for (int x = 0; x < w; ++x) {
            int idx = (y * w + x) * 4;
            int pid = (pixels[idx] << 16) | (pixels[idx + 1] << 8) | pixels[idx + 2];
            if (pid <= 0 || pid > maxPid) continue;
            int cid = pidToCountry[pid];
            if (cid <= 0) continue;

            for (int d = 0; d < 4; ++d) {
                // Wrap horizontally for equirectangular maps
                int nx = x + DX[d];
                if (nx < 0) nx = w - 1;
                else if (nx >= w) nx = 0;
                int ny = y + DY[d];
                if (ny >= h) continue;

                int nidx = (ny * w + nx) * 4;
                int npid = (pixels[nidx] << 16) | (pixels[nidx + 1] << 8) | pixels[nidx + 2];
                if (npid > 0 && npid <= maxPid && npid != pid) {
                    int ncid = pidToCountry[npid];
                    // Track ALL adjacencies for m_provinceNeighbors (not just same-country)
                    allAdjPairs.insert(pack(pid, npid));
                    // Track same-country only for component detection
                    if (ncid == cid)
                        adjPairs.insert(pack(pid, npid));
                }
            }
        }
    }

    // Build m_provinceNeighbors from all adjacency pairs
    for (int64_t pair : allAdjPairs) {
        int a = static_cast<int>(pair >> 32);
        int b = static_cast<int>(pair & 0xFFFFFFFF);
        m_provinceNeighbors[a].push_back(b);
        m_provinceNeighbors[b].push_back(a);
    }

    // Build same-country adjacency graph from the collected pairs
    std::unordered_map<int, std::vector<int>> graph;
    for (int64_t pair : adjPairs) {
        int a = static_cast<int>(pair >> 32);
        int b = static_cast<int>(pair & 0xFFFFFFFF);
        graph[a].push_back(b);
        graph[b].push_back(a);
    }

    // ── Component phase ────────────────────────────────────────────────
    // For each country, BFS through its adjacency graph to find
    // truly connected landmass components.
    for (auto& [cid, pids] : countryProvs) {
        auto* country = m_countries.getCountry(cid);
        if (!country || country->name.empty() || pids.empty()) continue;

        std::unordered_set<int> visited;
        for (int startPid : pids) {
            if (visited.count(startPid)) continue;

            // BFS through pixel-adjacent provinces of this country
            std::vector<int> bfsStack = {startPid};
            visited.insert(startPid);
            std::vector<Vector2> compCenters;

            while (!bfsStack.empty()) {
                int cur = bfsStack.back(); bfsStack.pop_back();
                compCenters.push_back(m_provinceCenters[cur]);

                auto git = graph.find(cur);
                if (git != graph.end()) {
                    for (int nb : git->second) {
                        if (!visited.count(nb)) {
                            visited.insert(nb);
                            bfsStack.push_back(nb);
                        }
                    }
                }
            }

            if (compCenters.empty()) continue;

            Vector2 center = {0, 0};
            float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
            for (auto& c : compCenters) {
                center.x += c.x; center.y += c.y;
                minX = std::min(minX, c.x); maxX = std::max(maxX, c.x);
                minY = std::min(minY, c.y); maxY = std::max(maxY, c.y);
            }
            int cnt = (int)compCenters.size();
            center.x /= cnt; center.y /= cnt;

            float bboxW = maxX - minX;
            float bboxH = maxY - minY;

            float cov_xx = 0, cov_yy = 0, cov_xy = 0;
            for (auto& c : compCenters) {
                float dx = c.x - center.x;
                float dy = c.y - center.y;
                cov_xx += dx * dx; cov_yy += dy * dy; cov_xy += dx * dy;
            }
            cov_xx /= cnt; cov_yy /= cnt; cov_xy /= cnt;

            float angle = 0.0f;
            float diff = cov_xx - cov_yy;
            if (fabs(diff) > 0.001f || fabs(cov_xy) > 0.001f)
                angle = 0.5f * atan2f(2.0f * cov_xy, diff);

            if (angle > PI * 0.5f) angle -= PI;
            if (angle < -PI * 0.5f) angle += PI;

            const char* name = country->name.c_str();

            int maxFsBH = (int)(bboxH * 0.7f);
            int maxFsBW = 60;
            for (int fs = 60; fs >= 12; --fs) {
                if (MeasureTextEx(font, name, (float)fs, spacing).x <= bboxW * 0.85f) {
                    maxFsBW = fs; break;
                }
            }
            int fontSize = std::clamp(std::min(maxFsBH, maxFsBW), 10, 60);

            float span = MeasureTextEx(font, name, (float)fontSize, spacing).x;

            // Signed curvature: arch follows the country's shape
            Vector2 dir = {cosf(angle), sinf(angle)};
            Vector2 perp = {-dir.y, dir.x};
            float maxAbsPerp = 0, perpSum = 0;
            for (auto& c : compCenters) {
                float pd = (c.x - center.x) * perp.x + (c.y - center.y) * perp.y;
                perpSum += pd;
                if (fabs(pd) > maxAbsPerp) maxAbsPerp = fabs(pd);
            }
            float curvMag = std::min(maxAbsPerp * 0.1f, span * 0.03f);
            float curvature = (fabs(perpSum) > 0.001f) ?
                (perpSum > 0 ? curvMag : -curvMag) : 0.0f;

            m_countryLabels.push_back({country->name, center, angle, fontSize, span, curvature});
        }
    }
}

bool Game::loadFromFiles() {
    std::string landPath = m_dataDir + "land_sea.png";
    if (!m_landSea.load(landPath)) {
        std::cerr << "Failed to load " << landPath << std::endl;
        return false;
    }

    std::string provImg = m_dataDir + "provinces.png";
    std::string provJson = m_dataDir + "provinces.json";
    if (!m_provinces.load(provImg, provJson)) {
        std::cerr << "Failed to load provinces from " << provImg << std::endl;
        return false;
    }

    std::string countriesPath = m_dataDir + "countries.json";
    m_countries.load(countriesPath);

    // Load population data
    std::string popPath = m_dataDir + "population.json";
    std::ifstream popFile(popPath);
    if (popFile) {
        try {
            auto popJson = nlohmann::json::parse(popFile);
            for (auto& [provStr, popVal] : popJson.items()) {
                int pid = std::stoi(provStr);
                m_provincePopulations[pid] = popVal.get<long long>();
            }
        } catch (...) {}
    }

    std::string compassPath = m_dataDir + "political_compass.json";
    std::ifstream compassFile(compassPath);
    if (compassFile) {
        try {
            auto compJson = nlohmann::json::parse(compassFile);
            for (auto& [provStr, comp] : compJson.items()) {
                int pid = std::stoi(provStr);
                float left = comp["left"].get<float>();
                float auth = comp["auth"].get<float>();
                m_provinceCompass[pid] = {-left, -auth};   // see the note at the country loader
            }
        } catch (...) {}
    }

    // Load minority data
    std::string minorPath = m_dataDir + "minorities.json";
    std::ifstream minorFile(minorPath);
    if (minorFile) {
        try {
            auto mJson = nlohmann::json::parse(minorFile);
            for (auto& [provStr, groups] : mJson.items()) {
                int pid = std::stoi(provStr);
                std::vector<MinorityGroup> vec;
                for (auto& g : groups)
                    vec.push_back({g["n"].get<std::string>(), g["p"].get<float>()});
                if (!vec.empty()) m_provinceMinorities[pid] = std::move(vec);
            }
        } catch (...) {}
    }
    std::string mcolPath = m_dataDir + "minority_colors.json";
    std::ifstream mcolFile(mcolPath);
    if (mcolFile) {
        try {
            auto cJson = nlohmann::json::parse(mcolFile);
            for (auto& [name, rgb] : cJson.items()) {
                auto& arr = rgb;
                m_minorityColors[name] = {(uint8_t)(int)arr[0], (uint8_t)(int)arr[1], (uint8_t)(int)arr[2], 255};
            }
        } catch (...) {}
    }

    // Runtime political texture generated in buildPopulationLookups()

    // Load metadata (for date)
    std::string metaPath = m_dataDir + "metadata.json";
    std::ifstream metaFile(metaPath);
    if (metaFile) {
        try {
            auto meta = nlohmann::json::parse(metaFile);
            if (meta.contains("map_date") && meta["map_date"].is_string())
                m_mapDate = meta["map_date"].get<std::string>();
        } catch (...) {}
    }

    // Load starting policies
    m_startingPolicies.clear();
    std::string spPath = m_dataDir + "starting_policies.json";
    std::ifstream spFile(spPath);
    if (!spFile) spFile.open("data/starting_policies.json");
    printf("[POLICY] Loading starting_policies from: %s (exists=%d)\n", spPath.c_str(), spFile.is_open());
    if (spFile) {
        try {
            auto spJson = nlohmann::json::parse(spFile);
            if (spJson.contains("starting_policies")) {
                for (auto& [iso, policies] : spJson["starting_policies"].items()) {
                    std::vector<std::string> vec;
                    for (auto& p : policies) vec.push_back(p.get<std::string>());
                    m_startingPolicies[iso] = std::move(vec);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "  Failed to parse starting_policies.json: " << e.what() << std::endl;
        }
        printf("[POLICY] Loaded %zu starting policy entries\n", m_startingPolicies.size());
    } else {
        printf("[POLICY] ERROR: starting_policies.json NOT FOUND at %s or data/starting_policies.json\n", spPath.c_str());
    }

    // Load country compass positions
    std::string ccPath = m_dataDir + "country_compass.json";
    std::ifstream ccFile(ccPath);
    if (!ccFile) ccFile.open("data/country_compass.json");
    if (ccFile) {
        try {
            auto ccJson = nlohmann::json::parse(ccFile);
            for (auto& [iso, comp] : ccJson.items()) {
                float left = comp["left"].get<float>();
                float auth = comp["auth"].get<float>();
                // Find country ID by ISO
                for (auto& [cid, c] : m_countries.getAll()) {
                    if (c.isoA3 == iso) {
                        // NEGATED, and both axes, and on BOTH sides.
                        //
                        // The file speaks in "left" and "auth": positive means
                        // more left, more authoritarian. PoliticalCompass means
                        // the opposite on both axes -- economic runs -100 left
                        // to +100 right, social -100 authoritarian to +100
                        // libertarian -- and everything downstream reads it that
                        // way: the doctrine requirements in policies.json, the
                        // compass shifts a doctrine applies, and the compass
                        // widget, which plots +economic toward the label reading
                        // RIGHT.
                        //
                        // Storing the file's numbers unconverted inverted every
                        // one of those. The Soviet Union (left 95) loaded as
                        // hard right: it could not enact land reform, state
                        // industry, worker rights or a wealth tax, and could
                        // privatise freely. Germany (left -55) loaded as hard
                        // left and had the exact opposite menu. France (left 20)
                        // was drawn on the right of her own compass, which is
                        // how it was reported: a player trying to take France
                        // left, told the doctrines were unavailable.
                        //
                        // The earlier note here was right that flipping ONE side
                        // is a bug -- m_provinceCompass is compared against this
                        // directly in getProvinceRebellionChance, so negating
                        // only one turns that distance into a sum, and every
                        // province reads as twice its real distance from its
                        // government. That is why both sides are negated
                        // together. The rebellion code uses |x|+|y| and
                        // sqrt(dx^2+dy^2), and neither changes when both sides
                        // flip sign.
                        m_countryCompass[cid] = {-left, -auth};
                        break;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "  Failed to parse country_compass.json: " << e.what() << std::endl;
        }
    }

    // Load starting minority policies
    m_startingMinorityPolicies.clear();
    std::string smpPath = m_dataDir + "starting_minority_policies.json";
    std::ifstream smpFile(smpPath);
    if (!smpFile) smpFile.open("data/starting_minority_policies.json");
    if (smpFile) {
        try {
            auto smpJson = nlohmann::json::parse(smpFile);
            for (auto& [iso, minorities] : smpJson.items()) {
                std::unordered_map<std::string, std::vector<int>> perMinority;
                for (auto& [mname, opts] : minorities.items()) {
                    std::vector<int> vec;
                    for (auto& o : opts) vec.push_back(o.get<int>());
                    perMinority[mname] = std::move(vec);
                }
                m_startingMinorityPolicies[iso] = std::move(perMinority);
            }
        } catch (const std::exception& e) {
            std::cerr << "  Failed to parse starting_minority_policies.json: " << e.what() << std::endl;
        }
    }

    return true;
}

bool Game::loadFromODM(const std::string& odmPath) {
    // On the web the scenario archives are not in the preload -- they are the
    // bulk of it, and none of them is touched until a player has picked one, so
    // preloading all six meant waiting for five worlds nobody asked for before
    // the menu could draw. This fetches the chosen one, blocking, with the
    // loading screen already up. See src/util/WebAssets.h.
    //
    // Here rather than in the loading state machine because loadMapPack() and
    // the multiplayer client reach this function without going through it, and
    // a map that downloads on one path and 404s on another is worse than
    // either.
    odEnsureAsset(odmPath);

    // Through odFile, not ifstream: on Android the .odmap lives inside the APK
    // and only AAssetManager can reach it. See OdFile.h.
    const std::string zipBytes = odFile::readAll(odmPath);
    if (zipBytes.empty()) return false;
    std::vector<uint8_t> zipData(zipBytes.begin(), zipBytes.end());

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData.data(), zipData.size(), 0))
        return false;

    struct FileEntry { std::string name; void* data; size_t size; };
    // Dynamic — a map can carry far more than a small fixed cap once custom
    // flag SVGs and script files are added (the old fixed-size[128] array
    // silently truncated and dropped scripts/late-appended entries).
    std::vector<FileEntry> entries;
    entries.reserve(256);
    const char* needed[] = {"land_sea.png", "provinces.png", "provinces.json", "countries.json",
                            "metadata.json", "population.json", "political_compass.json",
                            "minorities.json", "minority_colors.json", "starting_policies.json",
                            "country_compass.json", "starting_minority_policies.json", "thumb.png",
                            "resources.json", "relations.json", "claims.json", "ports.json",
                            "armies.json", "ships.json", "policies.json"};
    int found = 0;
    int neededCount = sizeof(needed) / sizeof(needed[0]);

    for (int fi = 0; fi < neededCount; ++fi) {
        int idx = mz_zip_reader_locate_file(&zip, needed[fi], nullptr, 0);
        if (idx < 0) continue;
        size_t outSize = 0;
        void* outData = mz_zip_reader_extract_to_heap(&zip, idx, &outSize, 0);
        if (!outData) continue;
        entries.push_back({needed[fi], outData, outSize});
        found++;
    }

    // Scan for scripts/ entries, licenses/, symbols/, and flags/ SVGs in the archive
    m_loadedMapHasScripts = false;
    {
        int numEntries = mz_zip_reader_get_num_files(&zip);
        for (int zi = 0; zi < numEntries; ++zi) {
            mz_zip_archive_file_stat fstat;
            if (mz_zip_reader_file_stat(&zip, zi, &fstat)) {
                std::string entryName = fstat.m_filename;
                if (entryName.rfind("scripts/", 0) == 0 && entryName.size() > 8) {
                    m_loadedMapHasScripts = true;
                    break;
                }
            }
        }
        // Extract license, symbol, flag SVG, and script entries (not in needed[] list)
        for (int zi = 0; zi < numEntries; ++zi) {
            mz_zip_archive_file_stat fstat;
            if (mz_zip_reader_file_stat(&zip, zi, &fstat)) {
                std::string entryName = fstat.m_filename;
                if (entryName.empty() || entryName.back() == '/') continue; // directory marker
                bool isScript = entryName.rfind("scripts/", 0) == 0;
                if (entryName.rfind("licenses/", 0) == 0 || entryName.rfind("symbols/", 0) == 0 ||
                    (entryName.rfind("flags/", 0) == 0 && entryName.find(".svg") != std::string::npos) ||
                    isScript) {
                    // Check if already extracted
                    bool already = false;
                    for (auto& e : entries) if (e.name == entryName) { already = true; break; }
                    if (already) continue;
                    size_t outSize = 0;
                    void* outData = mz_zip_reader_extract_to_heap(&zip, zi, &outSize, 0);
                    if (!outData) continue;
                    entries.push_back({entryName, outData, outSize});
                    found++;
                }
            }
        }
    }

    mz_zip_reader_end(&zip);

    if (found < 5) {
        for (auto& e : entries) free(e.data);
        std::cerr << "  Incomplete .odmap archive" << std::endl;
        return false;
    }

    for (int i = 0; i < found; ++i) {
        auto& e = entries[i];
        if (e.name == "land_sea.png")
            m_landSea.loadFromMemory(e.data, (int)e.size);
        else if (e.name == "provinces.png" || e.name == "provinces.json") {
            // Need both png + json — load when we have json too
        }
    }

    // Load provinces (need both image and json)
    void* pngData = nullptr; size_t pngSize = 0;
    std::string jsonStr;
    for (int i = 0; i < found; ++i) {
        if (entries[i].name == "provinces.png") {
            pngData = entries[i].data;
            pngSize = entries[i].size;
        } else if (entries[i].name == "provinces.json") {
            jsonStr.assign(static_cast<char*>(entries[i].data), entries[i].size);
        }
    }
    if (pngData && !jsonStr.empty())
        m_provinces.loadFromMemory(pngData, (int)pngSize, jsonStr);

    // Load countries
    for (int i = 0; i < found; ++i) {
        if (entries[i].name == "countries.json") {
            std::string cJson(static_cast<char*>(entries[i].data), entries[i].size);
            m_countries.loadFromJson(cJson);
            // Fix missing/empty ISO codes from legacy .odmap data
            for (auto& [cid, c] : m_countries.getAll()) {
                if (c.isoA3.empty() || c.isoA3 == "-99") {
                    static const std::unordered_map<std::string, std::string> isoFallback = {
                        {"Afghanistan","AFG"},{"Albania","ALB"},{"Algeria","DZA"},{"Andorra","AND"},
                        {"Angola","AGO"},{"Antigua and Barbuda","ATG"},{"Argentina","ARG"},{"Armenia","ARM"},
                        {"Australia","AUS"},{"Austria","AUT"},{"Azerbaijan","AZE"},{"Bahamas","BHS"},
                        {"Bahrain","BHR"},{"Bangladesh","BGD"},{"Barbados","BRB"},{"Belarus","BLR"},
                        {"Belgium","BEL"},{"Belize","BLZ"},{"Benin","BEN"},{"Bhutan","BTN"},
                        {"Bolivia","BOL"},{"Bosnia and Herzegovina","BIH"},{"Botswana","BWA"},{"Brazil","BRA"},
                        {"Brunei","BRN"},{"Bulgaria","BGR"},{"Burkina Faso","BFA"},{"Burundi","BDI"},
                        {"Cambodia","KHM"},{"Cameroon","CMR"},{"Canada","CAN"},{"Cape Verde","CPV"},
                        {"Central African Republic","CAF"},{"Chad","TCD"},{"Chile","CHL"},{"China","CHN"},
                        {"Colombia","COL"},{"Comoros","COM"},{"Congo","COG"},{"Costa Rica","CRI"},
                        {"Croatia","HRV"},{"Cuba","CUB"},{"Cyprus","CYP"},{"Czech Republic","CZE"},
                        {"Denmark","DNK"},{"Djibouti","DJI"},{"Dominica","DMA"},{"Dominican Republic","DOM"},
                        {"DR Congo","COD"},{"East Timor","TLS"},{"Ecuador","ECU"},{"Egypt","EGY"},
                        {"El Salvador","SLV"},{"Equatorial Guinea","GNQ"},{"Eritrea","ERI"},{"Estonia","EST"},
                        {"Eswatini","SWZ"},{"Ethiopia","ETH"},{"Fiji","FJI"},{"Finland","FIN"},
                        {"France","FRA"},{"Gabon","GAB"},{"Gambia","GMB"},{"Georgia","GEO"},
                        {"Germany","DEU"},{"Ghana","GHA"},{"Greece","GRC"},{"Grenada","GRD"},
                        {"Guatemala","GTM"},{"Guinea","GIN"},{"Guinea-Bissau","GNB"},{"Guyana","GUY"},
                        {"Haiti","HTI"},{"Honduras","HND"},{"Hungary","HUN"},{"Iceland","ISL"},
                        {"India","IND"},{"Indonesia","IDN"},{"Iran","IRN"},{"Iraq","IRQ"},
                        {"Ireland","IRL"},{"Israel","ISR"},{"Italy","ITA"},{"Ivory Coast","CIV"},
                        {"Jamaica","JAM"},{"Japan","JPN"},{"Jordan","JOR"},{"Kazakhstan","KAZ"},
                        {"Kenya","KEN"},{"Kiribati","KIR"},{"Kuwait","KWT"},{"Kyrgyzstan","KGZ"},
                        {"Laos","LAO"},{"Latvia","LVA"},{"Lebanon","LBN"},{"Lesotho","LSO"},
                        {"Liberia","LBR"},{"Libya","LBY"},{"Liechtenstein","LIE"},{"Lithuania","LTU"},
                        {"Luxembourg","LUX"},{"Madagascar","MDG"},{"Malawi","MWI"},{"Malaysia","MYS"},
                        {"Maldives","MDV"},{"Mali","MLI"},{"Malta","MLT"},{"Marshall Islands","MHL"},
                        {"Mauritania","MRT"},{"Mauritius","MUS"},{"Mexico","MEX"},{"Micronesia","FSM"},
                        {"Moldova","MDA"},{"Monaco","MCO"},{"Mongolia","MNG"},{"Montenegro","MNE"},
                        {"Morocco","MAR"},{"Mozambique","MOZ"},{"Myanmar","MMR"},{"Namibia","NAM"},
                        {"Nauru","NRU"},{"Nepal","NPL"},{"Netherlands","NLD"},{"New Zealand","NZL"},
                        {"Nicaragua","NIC"},{"Niger","NER"},{"Nigeria","NGA"},{"North Korea","PRK"},
                        {"North Macedonia","MKD"},{"Norway","NOR"},{"Oman","OMN"},{"Pakistan","PAK"},
                        {"Palau","PLW"},{"Palestine","PSE"},{"Panama","PAN"},{"Papua New Guinea","PNG"},
                        {"Paraguay","PRY"},{"Peru","PER"},{"Philippines","PHL"},{"Poland","POL"},
                        {"Portugal","PRT"},{"Qatar","QAT"},{"Romania","ROU"},{"Russia","RUS"},
                        {"Rwanda","RWA"},{"Saint Kitts and Nevis","KNA"},{"Saint Lucia","LCA"},
                        {"Saint Vincent and the Grenadines","VCT"},{"Samoa","WSM"},{"San Marino","SMR"},
                        {"Sao Tome and Principe","STP"},{"Saudi Arabia","SAU"},{"Senegal","SEN"},
                        {"Serbia","SRB"},{"Seychelles","SYC"},{"Sierra Leone","SLE"},{"Singapore","SGP"},
                        {"Slovakia","SVK"},{"Slovenia","SVN"},{"Solomon Islands","SLB"},{"Somalia","SOM"},
                        {"South Africa","ZAF"},{"South Korea","KOR"},{"South Sudan","SSD"},{"Spain","ESP"},
                        {"Sri Lanka","LKA"},{"Sudan","SDN"},{"Suriname","SUR"},{"Sweden","SWE"},
                        {"Switzerland","CHE"},{"Syria","SYR"},{"Taiwan","TWN"},{"Tajikistan","TJK"},
                        {"Tanzania","TZA"},{"Thailand","THA"},{"Togo","TGO"},{"Tonga","TON"},
                        {"Trinidad and Tobago","TTO"},{"Tunisia","TUN"},{"Turkey","TUR"},{"Turkmenistan","TKM"},
                        {"Tuvalu","TUV"},{"Uganda","UGA"},{"Ukraine","UKR"},{"United Arab Emirates","ARE"},
                        {"United Kingdom","GBR"},{"United States","USA"},{"Uruguay","URY"},{"Uzbekistan","UZB"},
                        {"Vanuatu","VUT"},{"Vatican","VAT"},{"Venezuela","VEN"},{"Vietnam","VNM"},
                        {"Yemen","YEM"},{"Zambia","ZMB"},{"Zimbabwe","ZWE"},
                        // Non-UN / disputed
                        {"Kosovo","XKV"},{"Somaliland","XSO"},{"N. Cyprus","XNC"},{"Abkhazia","ABH"},
                        {"S. Ossetia","RSO"},{"W. Sahara","ESH"},{"Transnistria","PMA"},
                    };
                    auto fit = isoFallback.find(c.name);
                    if (fit != isoFallback.end())
                        c.isoA3 = fit->second;
                }
            }
            break;
        }
    }

    // Extract metadata
    m_mapDate.clear();
    for (int i = 0; i < found; ++i) {
        if (entries[i].name == "metadata.json") {
            std::string metaStr(static_cast<char*>(entries[i].data), entries[i].size);
            try {
                auto meta = nlohmann::json::parse(metaStr);
                if (meta.contains("map_date") && meta["map_date"].is_string())
                    m_mapDate = meta["map_date"].get<std::string>();
                if (meta.contains("has_scripts") && meta["has_scripts"].is_boolean())
                    m_loadedMapHasScripts = meta["has_scripts"].get<bool>();
            } catch (...) {}
            break;
        }
    }

    // Extract population data
    for (int i = 0; i < found; ++i) {
        if (entries[i].name == "population.json") {
            std::string popStr(static_cast<char*>(entries[i].data), entries[i].size);
            try {
                auto popJson = nlohmann::json::parse(popStr);
                for (auto& [provStr, popVal] : popJson.items()) {
                    int pid = std::stoi(provStr);
                    m_provincePopulations[pid] = popVal.get<long long>();
                }
            } catch (...) {}
            break;
        }
    }

    // Extract political compass data
    for (int i = 0; i < found; ++i) {
        if (entries[i].name == "political_compass.json") {
            std::string compStr(static_cast<char*>(entries[i].data), entries[i].size);
            try {
                auto compJson = nlohmann::json::parse(compStr);
                for (auto& [provStr, comp] : compJson.items()) {
                    int pid = std::stoi(provStr);
                    float left = comp["left"].get<float>();
                    float auth = comp["auth"].get<float>();
                    m_provinceCompass[pid] = {-left, -auth};   // see the note at the country loader
                }
            } catch (...) {}
            break;
        }
    }

    // Extract starting policies
    m_startingPolicies.clear();
    for (int i = 0; i < found; ++i) {
        if (entries[i].name == "starting_policies.json") {
            std::string spStr(static_cast<char*>(entries[i].data), entries[i].size);
            try {
                auto spJson = nlohmann::json::parse(spStr);
                if (spJson.contains("starting_policies")) {
                    for (auto& [iso, policies] : spJson["starting_policies"].items()) {
                        std::vector<std::string> vec;
                        for (auto& p : policies) vec.push_back(p.get<std::string>());
                        m_startingPolicies[iso] = std::move(vec);
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "  Failed to parse starting_policies.json: " << e.what() << std::endl;
            }
            break;
        }
    }

    // Extract country compass data
    for (int i = 0; i < found; ++i) {
        if (entries[i].name == "country_compass.json") {
            std::string ccStr(static_cast<char*>(entries[i].data), entries[i].size);
            try {
                auto ccJson = nlohmann::json::parse(ccStr);
                for (auto& [iso, comp] : ccJson.items()) {
                    float left = comp["left"].get<float>();
                    float auth = comp["auth"].get<float>();
                    for (auto& [cid, c] : m_countries.getAll()) {
                        if (c.isoA3 == iso) {
                            // Negated on both axes, exactly as m_provinceCompass
                            // is — see the loose-file loader above for why the
                            // two must always be converted together.
                            m_countryCompass[cid] = {-left, -auth};
                            break;
                        }
                    }
                }
            } catch (...) {}
            break;
        }
    }

    // Extract starting minority policies
    m_startingMinorityPolicies.clear();
    for (int i = 0; i < found; ++i) {
        if (entries[i].name == "starting_minority_policies.json") {
            std::string smpStr(static_cast<char*>(entries[i].data), entries[i].size);
            try {
                auto smpJson = nlohmann::json::parse(smpStr);
                for (auto& [iso, minorities] : smpJson.items()) {
                    std::unordered_map<std::string, std::vector<int>> perMinority;
                    for (auto& [mname, opts] : minorities.items()) {
                        std::vector<int> vec;
                        for (auto& o : opts) vec.push_back(o.get<int>());
                        perMinority[mname] = std::move(vec);
                    }
                    m_startingMinorityPolicies[iso] = std::move(perMinority);
                }
            } catch (...) {}
            break;
        }
    }

    // Extract minority data
    for (int i = 0; i < found; ++i) {
        if (entries[i].name == "minorities.json") {
            std::string mStr(static_cast<char*>(entries[i].data), entries[i].size);
            try {
                auto mJson = nlohmann::json::parse(mStr);
                for (auto& [provStr, groups] : mJson.items()) {
                    int pid = std::stoi(provStr);
                    std::vector<MinorityGroup> vec;
                    for (auto& g : groups)
                        vec.push_back({g["n"].get<std::string>(), g["p"].get<float>()});
                    if (!vec.empty()) m_provinceMinorities[pid] = std::move(vec);
                }
                std::cout << "  Loaded minorities for " << m_provinceMinorities.size() << " provinces" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "  Failed to parse minorities.json: " << e.what() << std::endl;
            }
            break;
        }
    }
    // Extract minority colors
    for (int i = 0; i < found; ++i) {
        if (entries[i].name == "minority_colors.json") {
            std::string mcStr(static_cast<char*>(entries[i].data), entries[i].size);
            try {
                auto cJson = nlohmann::json::parse(mcStr);
                for (auto& [name, rgb] : cJson.items()) {
                    auto& arr = rgb;
                    m_minorityColors[name] = {(uint8_t)(int)arr[0], (uint8_t)(int)arr[1], (uint8_t)(int)arr[2], 255};
                }
                std::cout << "  Loaded " << m_minorityColors.size() << " minority colors" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "  Failed to parse minority_colors.json: " << e.what() << std::endl;
            }
            break;
        }
    }

    // Store JSON/script/SVG data in memory (no disk writes). Binary map
    // images are loaded directly above and skipped here. Script bodies MUST
    // be kept — ScriptEngine::runScripts() reads them out of this map.
    m_odmJsonData.clear();
    for (auto& e : entries) {
        const std::string& name = e.name;
        if (name == "land_sea.png" || name == "provinces.png" || name == "political.png" ||
            name == "thumb.png") continue;
        m_odmJsonData[name] = std::string(static_cast<char*>(e.data), e.size);
    }

    // Free all extracted data
    for (auto& e : entries) free(e.data);

    if (!m_provinces.getWidth()) {
        std::cerr << "  Failed to load provinces from .odmap" << std::endl;
        return false;
    }

    return true;
}

void Game::unloadGameData() {
    // Persist the AI model before tearing the world down (destructor saves)
    if (m_ai) { delete m_ai; m_ai = nullptr; }
    m_rebellionsThisTurnByCid.clear();
    m_eliminatedCids.clear();
    // Return the big per-pixel buffers to the OS (swap-with-empty frees
    // capacity; clear() alone would keep it reserved). At 33.5M pixels these
    // total well over a gigabyte — leaving them resident while the NEXT map is
    // generated is what pushes long training runs into an out-of-memory kill.
    std::vector<int>().swap(m_pixelCountryArray);
    std::vector<std::vector<int>>().swap(m_countryPixels);
    std::unordered_map<int, std::vector<int>>().swap(m_provincePixels);
    std::vector<Color>().swap(m_populationPixelBuffer);
    std::vector<Color>().swap(m_politicalPixelBuffer);
    std::vector<uint8_t>().swap(m_gradientDist);
    std::vector<Color>().swap(m_claimsPixelBuffer);
    for (auto& b : m_resourceBuffers) std::vector<Color>().swap(b);
    // Clean up script engine
    if (m_scriptEngine) { delete m_scriptEngine; m_scriptEngine = nullptr; }
    m_scriptErrors.clear();
    m_scriptErrorTimer = 0;

    // Unload country flags
    for (auto& [id, tex] : m_countryFlags) {
        if (tex.id > 0) UnloadTexture(tex);
    }
    m_countryFlags.clear();

    // Unload icons
    auto unload = [](Texture2D& tex) { if (tex.id > 0) { UnloadTexture(tex); tex = {}; } };
    unload(m_iconPopulation);
    unload(m_iconIndustry);
    unload(m_iconDefence);
    unload(m_iconRelations);
    unload(m_iconArmyNav);
    unload(m_iconNavy);
    unload(m_iconResources);
    unload(m_iconCountryNames);
    unload(m_iconPolicies);
    unload(m_iconEconomy);
    unload(m_iconClaims);
    unload(m_iconResearch);
    unload(m_ceasefireOverlayTex);
    unload(m_popupTermsMapTex);
    // Province ids belong to the map being torn down, so the cached shading is
    // meaningless against the next one. Key 0 is never a popup id (pushPopup
    // pre-increments), so this forces a rebuild rather than matching by luck.
    std::vector<Color>().swap(m_popupTermsMapBuf);
    m_popupTermsMapKey = 0;
    m_popupTermsMapEmpty = false;

    // Delete renderer
    delete m_renderer;
    m_renderer = nullptr;
    m_politicalTex = {};

    // Clear thumbnail cache
    clearThumbCache();

    // Clear game data containers to free heap memory (important for Emscripten reload)
    // The province table itself, which nothing ever cleared.
    //
    // Every other per-map container below was reset here; m_provinces was not,
    // and loading a map only ADDS the provinces that map defines. Province ids
    // come from pixel colour, so consecutive maps overlap only partially — and
    // every province unique to the OLD map survived the rotation still owned by
    // whoever held it there.
    //
    // Those owners do not exist on the new map. Their cids resolve to no
    // country, which makes the provinces permanently un-conquerable (the AI's
    // attack and declare-war paths both bail on a null Country) and permanently
    // "alive" in the trainer's country count. Rebel ids created at runtime came
    // across too: a fresh map would load owned by breakaway states from the
    // previous one. Measured on a five-map rotation: 75 provinces held by five
    // ghost countries and nineteen ghost rebels, on turn one.
    m_provinces.clear();
    m_provinceIndustry.clear();
    m_provincePopulations.clear();
    m_provinceCompass.clear();
    m_provinceMinorities.clear();
    m_provincePorts.clear();
    m_provinceArmies.clear();
    m_provinceResources.clear();
    m_claimsByProvince.clear();
    m_ships.clear();
    m_relations.clear();
    // Reputation is world state like any other: a new or loaded world must not
    // inherit who lied to whom in the last one.
    m_credibility.clear();
    m_openClaims.clear();
    m_credibilityHits = 0;
    m_credibilityLow = 1.0f;
    m_pendingMoveOrders.clear();
    m_pendingUpgrades.clear();
    m_pendingSpecializations.clear();
    m_pendingRecruitments.clear();
    m_pendingDisbandOrders.clear();
    m_pendingDiplomaticActions.clear();
    // Ship/artillery queues were never cleared here, so orders (with ship
    // indices and province ids from the OLD world) leaked into the next one —
    // AI training rotates maps in-process, so a stale bombard/disembark could
    // fire at whatever ship or province happened to reuse the id.
    m_pendingShipMoveOrders.clear();
    m_pendingShipEngageOrders.clear();
    m_pendingShipBombardOrders.clear();
    m_pendingShipDisembarks.clear();
    m_pendingShipBuilds.clear();
    m_pendingScrapShips.clear();
    m_pendingEmbarkations.clear();
    m_pendingArtilleryOrders.clear();
    m_researchNodes.clear();
    m_countryResearched.clear();
    m_activePolicies.clear();
    m_countryActivePolicyIndices.clear();
    m_ethnicPolicies.clear();
    m_ethnicPolicyCategories.clear();
    m_minorityAlignmentDrift.clear();
    // Or the next world's countries inherit twenty points of rebellion chance
    // from whoever went broke in the last one. Training rotates maps in-process,
    // so this is not a theoretical leak.
    m_bankruptCountries.clear();
    m_minorityColors.clear();
    m_startingPolicies.clear();
    m_startingMinorityPolicies.clear();
    m_incomeHistory.clear();
    m_playableCountryIds.clear();
    m_rebelFlagSvgs.clear();
    // Country-keyed state. CountryMap::loadFromJson MERGES into the existing
    // map, so without these clears every world load inherited the previous
    // world's countries (stale rebels included) — 25-country maps came up
    // with 100+ countries after a few loads.
    m_countries.clear();
    m_claims.clear();
    m_isoToCid.clear();
    m_countryCompass.clear();
    m_countryPacification.clear();
    // Country ids are reused across maps, so leaving this behind would charge a
    // fresh country for a war the previous map's cid 7 was dragged into.
    m_countryWarWeariness.clear();
    // Province ids are reused across maps too, so a cooldown left behind would
    // silently make an unrelated province on the next map unrebellable.
    m_provinceRebellionCooldown.clear();
    m_callToArmsCooldown.clear();
    m_countryBalances.clear();
    m_pendingCeasefireTerms.clear();
    m_acceptedCeasefireTerms.clear();
    m_countryResearchAllocation.clear();
    m_countryResearchPoints.clear();
    m_countryResearchActive.clear();
    m_countryResearchInvested.clear();
    // All countries were just cleared, so every rebel cid is free again.
    // Without this reset the counter climbed monotonically across worlds —
    // AI self-play (thousands of rebels per session) walked it into the
    // SPC/UNC/BLC sentinel ids and past 65535. Save loads that carry live
    // rebels re-bump it via restoreRebels()/synthesizeMissingRebels().
    m_nextRebelCid = REBEL_CID_MIN;
    m_playerCountryId = 0;
    m_lastPanelCountryId = -1;
    m_lastIncomeCountryId = -1;
}

void Game::scanDirectory(const std::string& dir, const std::string& ext, std::vector<std::string>& out) {
    out.clear();
    // std::filesystem, not dirent.h: MSVC has no such header, so this file
    // could not compile on Windows at all. The error_code overload keeps the
    // old behaviour of returning quietly when the directory is not there --
    // the throwing overload would turn a missing folder into a crash.
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        // "." and ".." are never yielded, so the checks readdir needed are gone.
        std::string name = e.path().filename().string();
        if (name.size() >= ext.size() && name.substr(name.size() - ext.size()) == ext) {
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
}

bool Game::loadGameDataStep1() {
    // Load resource data from in-memory .odmap data or fall back to file
    std::string json;
    auto it = m_odmJsonData.find("resources.json");
    if (it != m_odmJsonData.end()) {
        json = it->second;
    } else {
        std::cerr << "  resources.json not found in .odmap archive" << std::endl;
        return true;
    }
    if (json.empty()) return true;
    try {
        auto resJson = nlohmann::json::parse(json);
        int pumpTick = 0;
        for (auto& [provStr, res] : resJson.items()) {
            // Sixteen hundred provinces, each with five resource objects. One
            // parse is nothing; the loop over all of them is not.
            if ((++pumpTick & 63) == 0) Audio::get().pump();
            int pid = std::stoi(provStr);
            ProvinceResources pr;
            // Tolerate missing keys (older/procedural maps omit absent resources)
            auto readRes = [&](const char* key) -> ProvinceResource {
                if (!res.contains(key)) return {0.0f, 0.0f};
                auto& o = res[key];
                return {o.value("a", 0.0f), o.value("b", 0.0f)};
            };
            pr.oil = readRes("oil");
            pr.gold = readRes("gold");
            pr.rubber = readRes("rubber");
            pr.gemstones = readRes("gemstones");
            pr.metal = readRes("metal");
            m_provinceResources[pid] = pr;
            if (res.contains("industry")) {
                auto& ind = res["industry"];
                ProvinceIndustry pi;
                pi.level = ind.value("level", 0);
                pi.income = ind.value("income", 0.0f);
                pi.specialization = ind.value("specialization", std::string());
                pi.resourceIncome = ind.value("resourceIncome", 0.0f);
                pi.popIncome = ind.value("popIncome", 0.0f);
                pi.popModifier = ind.value("popModifier", 1.0f);
                m_provinceIndustry[pid] = pi;
            }
            if (res.contains("fortification")) {
                auto indIt = m_provinceIndustry.find(pid);
                if (indIt != m_provinceIndustry.end()) {
                    indIt->second.fortification = res.value("fortification", 0);
                } else {
                    ProvinceIndustry pi;
                    pi.fortification = res.value("fortification", 0);
                    m_provinceIndustry[pid] = pi;
                }
            }
        }
        std::cout << "  Loaded resources for " << m_provinceResources.size() << " provinces" << std::endl;
    } catch (...) {
        std::cerr << "  Failed to parse resources.json" << std::endl;
    }
    return true;
}

bool Game::loadGameDataStep2() {
    printf("[LOAD] loadGameDataStep2: m_dataDir='%s'\n", m_dataDir.c_str());
    // Load from in-memory .odmap data only (no filesystem fallback for per-map data)
    auto loadJson = [&](const std::string& name) -> std::string {
        // Every section of this phase comes through here exactly once, which
        // makes it the one place that sits between two parses. Measured on the
        // web, this phase was the last unpumped stall of a load -- about a
        // second, after everything else had been fixed. Putting the yield in
        // the accessor rather than between the blocks means a section added
        // later is covered without anyone remembering to.
        Audio::get().pump();
        auto it = m_odmJsonData.find(name);
        if (it != m_odmJsonData.end()) return it->second;
        return {};
    };

    // Load relations data
    {
        std::string json = loadJson("relations.json");
        if (!json.empty()) {
            try {
                auto j = nlohmann::json::parse(json);
                for (auto& [iso, targets] : j.items()) {
                    for (auto& [targetIso, relType] : targets.items()) {
                        CountryRelation cr;
                        if (relType.is_object()) {
                            cr.war = relType.value("war", false);
                            cr.alliance = relType.value("ally", false);
                            cr.nonAggression = relType.value("nonAggression", false);
                            cr.guarantee = relType.value("guarantee", false);
                        } else if (relType.is_string()) {
                            std::string rt = relType.get<std::string>();
                            if (rt == "war") cr.war = true;
                            else if (rt == "alliance") cr.alliance = true;
                            else if (rt == "nonAggression") cr.nonAggression = true;
                            else if (rt == "guarantee") cr.guarantee = true;
                        }
                        m_relations[iso][targetIso] = cr;
                    }
                }
                std::cout << "  Loaded relations for " << m_relations.size() << " countries" << std::endl;
            } catch (...) {
                std::cerr << "  Failed to parse relations.json" << std::endl;
            }
        }
    }

    // Load claims data
    {
        std::string json = loadJson("claims.json");
        if (!json.empty()) {
            try {
                auto j = nlohmann::json::parse(json);
                for (auto& [iso, entry] : j.items()) {
                    std::vector<int> pids;
                    if (entry.is_array()) {
                        for (auto& v : entry) pids.push_back(v.get<int>());
                    } else if (entry.is_object() && entry.contains("provinces")) {
                        for (auto& v : entry["provinces"]) pids.push_back(v.get<int>());
                    }
                    m_claims[iso] = pids;
                    for (int pid : pids)
                        m_claimsByProvince[pid].push_back(iso);
                }
                std::cout << "  Loaded claims for " << m_claims.size() << " countries" << std::endl;
            } catch (...) {
                std::cerr << "  Failed to parse claims.json" << std::endl;
            }
        }
    }

    // Load port data
    {
        std::string json = loadJson("ports.json");
        if (!json.empty()) {
            try {
                auto j = nlohmann::json::parse(json);
                for (auto& [provStr, info] : j.items()) {
                    int pid = std::stoi(provStr);
                    PortInfo pi;
                    pi.level = info.value("level", 1);
                    m_provincePorts[pid] = pi;
                }
                std::cout << "  Loaded ports for " << m_provincePorts.size() << " provinces" << std::endl;
            } catch (...) {
                std::cerr << "  Failed to parse ports.json" << std::endl;
            }
        }
    }

    // Load armies data
    {
        std::string json = loadJson("armies.json");
        if (!json.empty()) {
            try {
                auto j = nlohmann::json::parse(json);
                for (auto& [provStr, units] : j.items()) {
                    int pid = std::stoi(provStr);
                    Province* prov = m_provinces.getProvinceById(pid);
                    int ownerCid = prov ? prov->countryId : 0;
                    std::vector<ArmyUnit> vec;
                    for (auto& u : units) {
                        ArmyUnit au;
                        // Units may belong to a foreign nation; fall back to the owner
                        au.countryId = u.value("country_id", ownerCid);
                        au.count = u["count"].get<int>();
                        if (au.count <= 0) continue;
                        // ONE STACK PER COUNTRY PER PROVINCE. A move order takes
                        // its share of the first stack it finds with the right
                        // owner, so a second one is an army nothing can order
                        // about. Saves written before that was enforced can
                        // carry them, as can a hand-edited scenario.
                        bool merged = false;
                        for (auto& have : vec)
                            if (have.countryId == au.countryId) { have.count += au.count; merged = true; break; }
                        if (!merged) vec.push_back(au);
                    }
                    if (!vec.empty()) m_provinceArmies[pid] = std::move(vec);
                }
                std::cout << "  Loaded armies for " << m_provinceArmies.size() << " provinces" << std::endl;
            } catch (...) {
                std::cerr << "  Failed to parse armies.json" << std::endl;
            }
        }
    }

    // Load ships data
    {
        std::string json = loadJson("ships.json");
        if (!json.empty()) {
            try {
                auto j = nlohmann::json::parse(json);
                int beached = 0;
                for (auto& entry : j) {
                    NavyShip ns;
                    ns.countryId = entry["country_id"].get<int>();
                    ns.type = entry["type"].get<std::string>();
                    ns.lat = entry["lat"].get<double>();
                    ns.lon = entry["lon"].get<double>();
                    ns.health = entry.value("health", 100);
                    ns.crew = entry.value("crew", 0);
                    if (ns.type != "boat") ns.crew = 0;
                    else ns.crew *= 4;
                    // MAP FILES SHIP HULLS ON LAND. Measured on the shipped
                    // scenarios: 104 of 340 boats, roughly a third, load at a
                    // coordinate the land raster calls land. The movement
                    // resolver now refuses to sail onto land, but it only ever
                    // touches a ship that was given somewhere to go -- an idle
                    // hull would sit in a field for the whole game. Snapping
                    // here fixes old saves and third-party maps too.
                    if (m_landSea.isLand((float)ns.lon, (float)ns.lat)) {
                        if (nudgeShipToWater(ns)) beached++;
                    }
                    m_ships.push_back(ns);
                }
                if (beached)
                    std::cout << "  Refloated " << beached
                              << " ship(s) that loaded on land" << std::endl;
                std::cout << "  Loaded " << m_ships.size() << " ships" << std::endl;
                // Ocean topology, built once now that the raster is in memory.
                // See Game::NavGrid -- the navy plans on this instead of
                // steering at a straight line and stopping at the first coast.
                buildNavGrid();
            } catch (...) {
                std::cerr << "  Failed to parse ships.json" << std::endl;
            }
        }
    }

    // Initialize policy system
    initPolicies();
    initEthnicPolicyCategories();
    initCountryCompass();
    applyStartingPolicies();
    initResearchTrees();

    // Auto-unlock techs matching built industry/fort/port levels for all countries
    for (auto& [cid, c] : m_countries.getAll()) {
        if (cid == UNC_CID || cid == BLC_CID || cid == SPC_CID) continue;
        int maxInd = 0, maxFort = 0, maxPort = 0;
        for (auto& [pid, p] : m_provinces.getAllProvinces()) {
            if (p.countryId != cid) continue;
            auto it = m_provinceIndustry.find(pid);
            if (it != m_provinceIndustry.end()) {
                if (it->second.level > maxInd) maxInd = it->second.level;
                if (it->second.fortification > maxFort) maxFort = it->second.fortification;
            }
            auto pt = m_provincePorts.find(pid);
            if (pt != m_provincePorts.end() && pt->second.level > maxPort)
                maxPort = pt->second.level;
        }
        // Recursively unlock a node and all its dependencies (respects mutexGroup)
        std::function<void(const std::string&)> unlockRecursive;
        unlockRecursive = [&](const std::string& nodeId) {
            for (auto& n : m_researchNodes) {
                if (n.id == nodeId) {
                    if (m_countryResearched[cid].count(n.id)) return;
                    if (n.mutexGroup > 0) {
                        for (auto& sib : m_researchNodes) {
                            if (sib.id != nodeId && sib.mutexGroup == n.mutexGroup && m_countryResearched[cid].count(sib.id))
                                return;
                        }
                    }
                    m_countryResearched[cid].insert(n.id);
                    for (auto& depId : n.deps)
                        unlockRecursive(depId);
                    break;
                }
            }
        };
        for (auto& n : m_researchNodes) {
            if (n.industryLevel > 0 && n.industryLevel <= maxInd)
                unlockRecursive(n.id);
            if (n.fortLevel > 0 && n.fortLevel <= maxFort)
                unlockRecursive(n.id);
            if (n.portLevel > 0 && n.portLevel <= maxPort)
                unlockRecursive(n.id);
        }
        for (auto& ship : m_ships)
            if (ship.countryId == cid) { m_countryResearched[cid].insert("navy1"); break; }
    }

    return true;
}

void Game::loadGameData() {
    loadGameDataStep1();
    loadGameDataStep2();
    // (auto-unlock now handled inside loadGameDataStep2)
    // Run map scripts after all data is loaded
    runMapScripts();
}

bool Game::loadMapPack(const std::string& odmPath) {
    std::cout << "Loading map: " << odmPath << std::endl;

    showLoadingScreen();
    setLoadingProgress(0.1f, "Loading map data...");
    drawLoadingScreen();

    bool odmOk = loadFromODM(odmPath);
    if (!odmOk) {
        // Fallback: look for individual files in the same directory as the odmPath
        std::string dir = odmPath.substr(0, odmPath.find_last_of('/') + 1);
        if (dir.empty()) dir = m_dataDir;
        // Try loading from the data dir as fallback
        odmOk = loadFromFiles();
        if (!odmOk) {
            hideLoadingScreen();
            return false;
        }
    }
    // Reload countries from individual file to override .odmap's baked-in -99 ISO codes
    std::string countriesPath = m_dataDir + "countries.json";
    m_countries.load(countriesPath);

    setLoadingProgress(0.3f, "Loading game data...");
    drawLoadingScreen();

    // Always load game data (policies, ships, etc.) - ODM only contains map data
    loadGameData();

    setLoadingProgress(0.5f, "Initializing renderer...");
    drawLoadingScreen();

    m_renderer = new MapRenderer(m_screenW, m_screenH,
                                 m_landSea.getWidth(), m_landSea.getHeight());
    m_renderer->setDpiScale(GetWindowScaleDPI().x);
    m_renderer->computeBorderTexture(m_provinces.getImage());
    m_renderer->setPoliticalTexture(m_politicalTex);
    m_renderer->setFallbackFont(m_gameFont);

    setLoadingProgress(0.6f, "Building population data...");
    drawLoadingScreen();

    buildPopulationLookups();

    setLoadingProgress(0.7f, "Generating resource textures...");
    drawLoadingScreen();

    // Pre-generate all 5 resource buffers
    {
        int w = m_provinces.getWidth();
        int h = m_provinces.getHeight();
        for (int r = 0; r < 5; ++r)
            m_resourceBuffers[r].resize(w * h, Color{40, 40, 40, 255});
        if (!m_provinceResources.empty()) {
            std::cout << "  Generating resource textures... " << std::flush;
            for (int r = 0; r < 5; ++r) {
                generateResourceTextureFor(r);
                setLoadingProgress(0.7f + (r * 0.04f), "Generating resource textures...");
                drawLoadingScreen();
            }
            std::cout << "done" << std::endl;
        }
        // The synchronous reload path's copy of the pair in
        // LOAD_GEN_RESOURCE_TEXTURES, and it gets the same treatment: the same
        // two uploads take the same ~200 ms, so a suspend is not worth it here
        // either.
        Audio::get().pump();

        Image resImg{};
        resImg.data = m_resourceBuffers[0].data();
        resImg.width = w;
        resImg.height = h;
        resImg.mipmaps = 1;
        resImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D resTex = LoadTextureFromImage(resImg);
        SetTextureFilter(resTex, TEXTURE_FILTER_BILINEAR);
        m_renderer->setResourceTexture(resTex);

        m_claimsPixelBuffer.resize(w * h, Color{0, 0, 0, 0});
        Image claimsImg{};
        claimsImg.data = m_claimsPixelBuffer.data();
        claimsImg.width = w;
        claimsImg.height = h;
        claimsImg.mipmaps = 1;
        claimsImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D claimsTex = LoadTextureFromImage(claimsImg);
        SetTextureFilter(claimsTex, TEXTURE_FILTER_BILINEAR);
        m_renderer->setClaimsTexture(claimsTex);
    }

    setLoadingProgress(0.9f, "Generating icons...");
    drawLoadingScreen();

    generateIcons();

    setLoadingProgress(0.92f, "Building province data...");
    drawLoadingScreen();

    m_renderer->buildProvinceData(m_provinces, m_provinceCenters, m_provinceRadius);

    // Compute country centers
    {
        std::unordered_map<int, Vector2> sums;
        std::unordered_map<int, int> cnts;
        for (auto& [pid, pc] : m_provinceCenters) {
            auto* p = m_provinces.getProvinceById(pid);
            if (p && p->countryId > 0) {
                sums[p->countryId].x += pc.x;
                sums[p->countryId].y += pc.y;
                cnts[p->countryId]++;
            }
        }
        for (auto& [cid, s] : sums)
            m_countryCenters[cid] = {s.x / cnts[cid], s.y / cnts[cid]};
    }

    setLoadingProgress(0.95f, "Computing country labels...");
    drawLoadingScreen();

    computeCountryLabels();
    m_renderer->setCountryLabels(&m_countryLabels);
    m_renderer->setMaxZoom(m_config.maxZoom);
    m_renderer->setDebugMode(m_config.debugMode);
    rebuildFlags();
    m_renderer->setCountryFlags(&m_countryFlags);

    // Set player country (default: Spectator)
    if (m_playerCountryId == 0) {
        m_playerCountryId = SPC_CID;
        std::cout << "  Player country: Spectator (ID " << SPC_CID << ")" << std::endl;
        auto spcFlag = m_countryFlags.find(SPC_CID);
        if (spcFlag != m_countryFlags.end()) {
            std::cout << "  SPC flag tex id: " << spcFlag->second.id << std::endl;
        } else {
            std::cout << "  SPC flag NOT FOUND in m_countryFlags!" << std::endl;
        }
        const Country* spc = m_countries.getCountry(SPC_CID);
        if (spc) {
            std::cout << "  SPC country found: " << spc->name << " (" << spc->isoA3 << ")" << std::endl;
        } else {
            std::cout << "  SPC country NOT FOUND!" << std::endl;
        }
    }

    std::cout << "  Loaded " << m_provinces.getAllProvinces().size() << " provinces, "
              << m_countries.size() << " countries" << std::endl;

    setLoadingProgress(0.98f, "Finalizing...");
    drawLoadingScreen();

    m_screenW = GetScreenWidth();
    m_screenH = GetScreenHeight();
    m_renderer->resize(m_screenW, m_screenH);

    setLoadingProgress(1.0f, "Complete!");
    drawLoadingScreen();

    hideLoadingScreen();

    m_running = true;
    return true;
}

bool Game::loadSaveFile(const std::string& savePath) {
    std::cout << "Loading save: " << savePath << std::endl;

    showLoadingScreen();
    setLoadingProgress(0.1f, "Extracting map data...");
    drawLoadingScreen();

    // Extract map.odmap from the .odsv ZIP into memory
    std::vector<uint8_t> odmData = SaveManager::extractODM(savePath);
    if (odmData.empty()) {
        std::cerr << "  Failed to extract .odmap from " << savePath << std::endl;
        hideLoadingScreen();
        return false;
    }

    setLoadingProgress(0.3f, "Writing temporary map file...");
    drawLoadingScreen();

    // Write extracted .odmap to a temp file, then load it
    std::string tempOdm = savePath + ".tmp.odmap";
    {
        std::ofstream out(tempOdm, std::ios::binary);
        if (!out) { std::cerr << "  Failed to write temp .odmap" << std::endl; hideLoadingScreen(); return false; }
        out.write(reinterpret_cast<const char*>(odmData.data()), odmData.size());
    }

    setLoadingProgress(0.5f, "Loading map pack...");
    drawLoadingScreen();

    bool loaded = loadMapPack(tempOdm);
    std::remove(tempOdm.c_str());
    if (!loaded) {
        hideLoadingScreen();
        return false;
    }

    setLoadingProgress(0.7f, "Reading save metadata...");
    drawLoadingScreen();

    // Read metadata and replay turn deltas
    SaveMetadata meta;
    try {
        meta = SaveManager::readMetadata(savePath);
    } catch (...) {
        std::cerr << "  Failed to read save metadata" << std::endl;
        hideLoadingScreen();
        return false;
    }

    int turnCount = meta.turnCount;
    std::cout << "  Save has " << turnCount << " turn(s) to replay" << std::endl;

    setLoadingProgress(0.75f, "Replaying game turns...");
    drawLoadingScreen();

    for (int t = 1; t <= turnCount; t++) {
        float turnProgress = 0.75f + (t / (float)turnCount) * 0.2f;
        setLoadingProgress(turnProgress, "Replaying turn " + std::to_string(t) + " of " + std::to_string(turnCount) + "...");
        drawLoadingScreen();

        TurnDelta delta = SaveManager::readTurn(savePath, t);
        if (delta.turnNumber != t) {
            std::cerr << "  Turn mismatch at " << t << std::endl;
            continue;
        }
        // Apply province changes
        for (auto& pd : delta.provinces) {
            int pid = pd.provinceId;
            Province* prov = m_provinces.getProvinceById(pid);
            if (!prov) continue;
            if (pd.ownerChanged)          prov->countryId = pd.newOwner;
            if (pd.populationChanged) {
                m_provincePopulations[pid] = pd.newPopulation;
                if ((size_t)pid < m_provincePopArray.size())
                    m_provincePopArray[pid] = pd.newPopulation;
            }
            if (pd.industryLevelChanged)  m_provinceIndustry[pid].level = pd.newIndustryLevel;
            if (pd.fortificationChanged)  m_provinceIndustry[pid].fortification = pd.newFortification;
            if (pd.incomeChanged)         m_provinceIndustry[pid].income = pd.newIncome;
            if (pd.resourceIncomeChanged) m_provinceIndustry[pid].resourceIncome = pd.newResourceIncome;
            if (pd.popIncomeChanged)      m_provinceIndustry[pid].popIncome = pd.newPopIncome;
            if (pd.popModifierChanged)    m_provinceIndustry[pid].popModifier = pd.newPopModifier;
        }
        // Apply ship changes
        for (auto& sd : delta.ships) {
            if (sd.shipIndex >= (int)m_ships.size()) continue;
            auto& ship = m_ships[sd.shipIndex];
            if (sd.latChanged)   ship.lat = sd.newLat;
            if (sd.lonChanged)   ship.lon = sd.newLon;
            if (sd.healthChanged) ship.health = sd.newHealth;
            if (sd.crewChanged)  ship.crew = sd.newCrew;
        }
        // Apply army changes
        for (auto& ad : delta.armies) {
            if (ad.units.empty()) {
                m_provinceArmies.erase(ad.provinceId);
            } else {
                std::vector<ArmyUnit> units;
                for (auto& u : ad.units) {
                    units.push_back({u.countryId, u.count});
                }
                m_provinceArmies[ad.provinceId] = std::move(units);
            }
        }
    }

    setLoadingProgress(0.98f, "Finalizing save load...");
    drawLoadingScreen();

    std::cout << "  Save replayed successfully" << std::endl;

    hideLoadingScreen();
    return true;
}

/**
 * Apply one turn's changes to the world.
 *
 * Lifted out of replaySaveTurns() because a multiplayer client needs exactly
 * this: the host sends turn deltas in the same `.odsv` binary form a save
 * stores, and "bring the world to turn N" is the same operation whether the
 * deltas came off disk or off a socket. Having two copies of it would be having
 * two answers to what a delta means.
 */
void Game::applyTurnDelta(const TurnDelta& delta) {
    // Province changes
    for (auto& pd : delta.provinces) {
        int pid = pd.provinceId;
        Province* prov = m_provinces.getProvinceById(pid);
        if (!prov) continue;
        if (pd.ownerChanged)          prov->countryId = pd.newOwner;
        if (pd.populationChanged) {
            m_provincePopulations[pid] = pd.newPopulation;
            if ((size_t)pid < m_provincePopArray.size())
                m_provincePopArray[pid] = pd.newPopulation;
        }
        if (pd.industryLevelChanged)  m_provinceIndustry[pid].level = pd.newIndustryLevel;
        if (pd.fortificationChanged)  m_provinceIndustry[pid].fortification = pd.newFortification;
        if (pd.incomeChanged)         m_provinceIndustry[pid].income = pd.newIncome;
        if (pd.resourceIncomeChanged) m_provinceIndustry[pid].resourceIncome = pd.newResourceIncome;
        if (pd.popIncomeChanged)      m_provinceIndustry[pid].popIncome = pd.newPopIncome;
        if (pd.popModifierChanged)    m_provinceIndustry[pid].popModifier = pd.newPopModifier;
    }
    // Ship changes
    for (auto& sd : delta.ships) {
        if (sd.shipIndex >= (int)m_ships.size()) continue;
        auto& ship = m_ships[sd.shipIndex];
        if (sd.latChanged)   ship.lat = sd.newLat;
        if (sd.lonChanged)   ship.lon = sd.newLon;
        if (sd.healthChanged) ship.health = sd.newHealth;
        if (sd.crewChanged)  ship.crew = sd.newCrew;
        if (sd.countryIdChanged) ship.countryId = sd.newCountryId;
    }
    // Army changes
    for (auto& ad : delta.armies) {
        if (ad.units.empty()) {
            m_provinceArmies.erase(ad.provinceId);
        } else {
            std::vector<ArmyUnit> units;
            for (auto& u : ad.units) {
                units.push_back({u.countryId, u.count});
            }
            m_provinceArmies[ad.provinceId] = std::move(units);
        }
    }
    // Research/pacification state (recorded per-turn in .dat)
    m_researchAllocation = delta.researchAllocation;
    m_pacificationAllocation = delta.pacificationAllocation;
    m_researchActiveNode = delta.researchActiveNode;
    m_researchPoints = delta.researchPoints;
}

/**
 * Rebuild pixel-level ownership and the border gradient after province owners
 * have moved. Needed after replaying deltas, from a save or from a host.
 */
void Game::rebuildOwnershipPixels() {
    m_provinceCountryLookup.assign(m_provinceCountryLookup.size(), 0);
    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        if ((size_t)pid < m_provinceCountryLookup.size())
            m_provinceCountryLookup[pid] = prov.countryId;
    }
    // Replayed deltas move provinces without going through reindexProvinceOwner.
    rebuildCountryProvinceIndex();
    const Image& provImg = m_provinces.getImage();
    int w2 = provImg.width, h2 = provImg.height;
    int totalPixels = w2 * h2;
    const auto* srcPixels = (const Color*)provImg.data;
    for (auto& vec : m_countryPixels) vec.clear();
    for (int i = 0; i < totalPixels; ++i) {
        // Once a raster row's worth. Same reason as the scans in MapRenderer:
        // this is a full-map pass and nothing refills the music while it runs.
        if ((i & 8191) == 0) Audio::get().pump();
        Color src = srcPixels[i];
        int pid = Province::colorToId(src.r, src.g, src.b);
        int cid = 0;
        if (pid > 0 && (size_t)pid < m_provinceCountryLookup.size())
            cid = m_provinceCountryLookup[pid];
        m_pixelCountryArray[i] = cid;
        m_politicalPixelBuffer[i] = (pid == 0 || cid == 0) ? Color{10, 15, 40, 255} :
            (m_countries.getCountry(cid) ? m_countries.getCountry(cid)->color : Color{80, 80, 80, 255});
        if (cid > 0 && cid < (int)m_countryPixels.size())
            m_countryPixels[cid].push_back(i);
    }
    rebuildGradientField();

    // AND THEN USE IT. The loop above fills m_politicalPixelBuffer with each
    // country's FLAT colour, and the block after it recomputes the distance
    // field -- but nothing ever applied one to the other, so the field was
    // rebuilt and thrown away. The political map came back from a load or a
    // multiplayer sync correctly RECOLOURED and completely flat: no border
    // shading and no dark boundary line, because both of those are what
    // generatePoliticalTexture() adds on top. That is the whole bug -- the
    // colour changed and the gradient did not.
    //
    // It belongs here rather than at the call sites: all three of them
    // (replaySaveTurns, and two in Game_Multiplayer) had the same omission, so
    // a fourth would have made it again.
    if (m_renderer) generatePoliticalTexture();
}

bool Game::replaySaveTurns(const std::string& savePath) {
    SaveMetadata meta;
    try {
        meta = SaveManager::readMetadata(savePath);
    } catch (...) {
        std::cerr << "  Failed to read save metadata" << std::endl;
        return false;
    }

    int turnCount = meta.turnCount;
    std::cout << "  Save has " << turnCount << " turn(s) to replay" << std::endl;

    // Must happen BEFORE the deltas are applied: those deltas set province
    // owners to rebel country ids, and if those countries don't exist yet the
    // territory resolves to nothing (no owner, not even UNC/BLC).
    restoreRebels(savePath);

    for (int t = 1; t <= turnCount; t++) {
        TurnDelta delta = SaveManager::readTurn(savePath, t);
        if (delta.turnNumber != t) {
            std::cerr << "  Turn mismatch at " << t << std::endl;
            continue;
        }
        applyTurnDelta(delta);
    }

    // Safety net for saves that predate rebel persistence (or any gap): a
    // province may now be owned by a rebel cid that restoreRebels() couldn't
    // load, because the save has no rebels.json. Without a country for that
    // cid the territory renders as grey "cid>0, not found" limbo. Synthesize a
    // placeholder country for every such cid so it at least shows as a
    // distinct, coloured breakaway state instead of broken grey.
    synthesizeMissingRebels();

    rebuildOwnershipPixels();

    // Load full state snapshot (pending orders, claims, research, alignment, etc.)
    if (!savePath.empty()) {
        std::string stateJson = SaveManager::readState(savePath);
        loadStateJson(stateJson);
    }

    // Regenerate borders, glow maps, and political texture with updated ownership
    reloadBorders();

    std::cout << "  Save replayed successfully" << std::endl;
    return true;
}

void Game::startNewGame(const std::string& mapName) {
    unloadGameData();
    // Clear all game state
    m_provincePopulations.clear();
    m_provinceCompass.clear();
    m_provinceMinorities.clear();
    m_provinceResources.clear();
    m_provinceIndustry.clear();
    m_provinceArmies.clear();
    m_ships.clear();
    m_provincePorts.clear();
    m_claims.clear();
    m_claimsByProvince.clear();
    m_relations.clear();
    // Reputation is world state like any other: a new or loaded world must not
    // inherit who lied to whom in the last one.
    m_credibility.clear();
    m_openClaims.clear();
    m_credibilityHits = 0;
    m_credibilityLow = 1.0f;
    m_selectedShipIndices.clear();
    m_countryShipIndices.clear();
    m_countryProvinceIds.clear();
    m_provinceCenters.clear();
    m_provinceRadius.clear();
    m_countryCenters.clear();
    m_countryLabels.clear();
    m_provincePixels.clear();
    m_coastalCache.clear();   // answers belong to the map that is going away
    m_portAnchorCache.clear();
    m_pixelCountryArray.clear();
    m_provincePopArray.clear();
    m_provinceCountryLookup.clear();
    m_countryProvinces.clear();
    m_countryPixels.clear();
    m_countryRelationColors.clear();
    m_playerCountryId = 0;
    m_lastSelectedProvince = 0;
    m_lastPopCountryId = -1;
    m_lastRelationsCountryId = -1;
    m_lastClaimsCountryId = -1;
    m_showClaims = false;
    m_activeViewTab = 0;
    m_shipPanelScroll = 0;
    m_shipListFocusIndex = -1;
    m_mapDate.clear();

    std::string odmPath = mapName;
    if (loadMapPack(odmPath)) {
        // Auto-create a save file so this world appears in Load World
        std::string saveDir = m_dataDir + "saves/";
        time_t now = time(nullptr);
        struct tm* tmLocal = localtime(&now);
        char ts[64];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tmLocal);
        std::string timestamp = ts;
        char tsFile[64];
        strftime(tsFile, sizeof(tsFile), "%Y-%m-%d_%H-%M-%S", tmLocal);
        std::string worldName = "AutoSave " + std::string(tsFile);

        m_currentSavePath = saveDir + worldName + ".odsv";

        // Read .odmap bytes for embedding -- asset-aware, see OdFile.h
        const std::string odmBytes = odFile::readAll(odmPath);
        std::vector<uint8_t> odmData(odmBytes.begin(), odmBytes.end());

        SaveMetadata meta;
        meta.saveName = worldName;
        meta.version = GAME_VERSION;
        meta.created = timestamp;
        meta.lastPlayed = timestamp;
        meta.turnCount = 0;
        meta.provinceCount = (int)m_provincePopulations.size();
        meta.shipCount = (int)m_ships.size();
        for (auto& [cid, c] : m_countries.getAll())
            meta.countryTreasuries[cid] = c.treasury;

        if (SaveManager::createSave(m_currentSavePath,
                std::string(odmData.begin(), odmData.end()), meta)) {
            m_autoCreatedSave = true;
            m_unsavedChanges = false;
            m_turnCount = 0;
            std::cout << "Auto-created save: " << m_currentSavePath << std::endl;
        }

        m_currentScreen = SCREEN_PLAYING;
    } else {
        std::cerr << "Failed to load map: " << mapName << std::endl;
        m_currentScreen = SCREEN_MENU;
    }
}

void Game::startNewGameWithName(const std::string& mapName, const std::string& worldName) {
    unloadGameData();
    // Clear all game state
    m_provincePopulations.clear();
    m_provinceCompass.clear();
    m_provinceMinorities.clear();
    m_provinceResources.clear();
    m_provinceIndustry.clear();
    m_provinceArmies.clear();
    m_ships.clear();
    m_provincePorts.clear();
    m_claims.clear();
    m_claimsByProvince.clear();
    m_relations.clear();
    // Reputation is world state like any other: a new or loaded world must not
    // inherit who lied to whom in the last one.
    m_credibility.clear();
    m_openClaims.clear();
    m_credibilityHits = 0;
    m_credibilityLow = 1.0f;
    m_selectedShipIndices.clear();
    m_countryShipIndices.clear();
    m_countryProvinceIds.clear();
    m_provinceCenters.clear();
    m_provinceRadius.clear();
    m_countryCenters.clear();
    m_countryLabels.clear();
    m_provincePixels.clear();
    m_coastalCache.clear();   // answers belong to the map that is going away
    m_portAnchorCache.clear();
    m_pixelCountryArray.clear();
    m_provincePopArray.clear();
    m_provinceCountryLookup.clear();
    m_countryProvinces.clear();
    m_countryPixels.clear();
    m_countryRelationColors.clear();
    m_playerCountryId = 0;
    m_lastSelectedProvince = 0;
    m_lastPopCountryId = -1;
    m_lastRelationsCountryId = -1;
    m_lastClaimsCountryId = -1;
    m_showClaims = false;
    m_activeViewTab = 0;
    m_shipPanelScroll = 0;
    m_shipListFocusIndex = -1;
    m_mapDate.clear();
    // Clear all pending actions (critical: prevents carry-over between games)
    m_pendingUpgrades.clear();
    m_pendingSpecializations.clear();
    m_pendingRecruitments.clear();
    m_pendingDisbandOrders.clear();
    m_pendingMoveOrders.clear();
    m_pendingShipBuilds.clear();
    m_pendingScrapShips.clear();
    m_pendingEmbarkations.clear();
    m_pendingDiplomaticActions.clear();
    m_pendingArtilleryOrders.clear();
    m_incomeHistory.clear();
    m_countryBalances.clear();
    m_inResearch = false;
    m_activeSidebarTab = 0;
    m_inEconomy = false;
    m_inClaims = false;
    m_specDropdownProvince = -1;
    m_armySliderActive = false;
    m_inCeasefireScreen = false;
    m_ceasefireTargetIso.clear();
    m_ceasefireOurMoney = 0;
    m_ceasefireTheirMoney = 0;
    m_ceasefireOurProvs.clear();
    m_ceasefireTheirProvs.clear();
    m_ceasefireOurDropClaims.clear();
    m_ceasefireTheirDropClaims.clear();
    m_ceasefireMapSrcX = 0; m_ceasefireMapSrcY = 0;
    m_ceasefireMapZoom = 1.0f;
    m_ceasefireSelectMode = 0;
    m_ceasefireMapDragging = false;
    m_ceasefireMapDragPrevX = 0;
    m_ceasefireMapDragPrevY = 0;
    m_pendingCeasefireTerms.clear();
    m_acceptedCeasefireTerms.clear();
    m_armyMoveDragSource = -1;
    m_armyMoveDragActive = false;
    m_armyMoveDragBtnDown = false;
    m_armyMoveDragHoverPid = -1;
    m_armyMovePickFrom = -1;
    m_armyMovePctSliderFrom = 0;
    m_armyMovePctSliderTo = 0;
    m_artillerySourceProvince = -1;
    m_artilleryTargetPid = -1;
    m_artillerySelectedType.clear();
    m_artilleryDragSource = -1;
    m_artilleryDragActive = false;
    m_artilleryDragHoverPid = -1;
    m_artilleryDragValidDest = false;
    m_researchAllocation = 0.25f;
    m_researchActiveNode = -1;
    m_researchPoints = 0;
    m_researchHoveredNode = -1;
    m_researchTab = 0;
    m_researchAlert = false;   // don't carry a stale sidebar highlight into a new game
    m_politicsAlert = false;
    // Reset all research nodes to unresearched
    for (auto& n : m_researchNodes) {
        n.researched = false;
        n.inProgress = false;
    }
    // Clear per-country research state
    m_countryResearched.clear();

    m_newWorldName = worldName;
    m_newWorldMapPath = mapName;
    m_showNewWorldDialog = false;

    // Set auto-save creation flag — will create save in LOAD_CREATE_SAVE phase
    m_loadingShouldCreateSave = true;
    m_loadingWorldName = worldName;

    // Use async loading - runs one step per frame in the game loop
    std::string odmPath = mapName;
    startLoading(odmPath);
    m_currentScreen = SCREEN_LOADING;
}

void Game::startLoadedGame(const std::string& saveName) {
    unloadGameData();
    // Clear all game state (same as startNewGame)
    m_provincePopulations.clear();
    m_provinceCompass.clear();
    m_provinceMinorities.clear();
    m_provinceResources.clear();
    m_provinceIndustry.clear();
    m_provinceArmies.clear();
    m_ships.clear();
    m_provincePorts.clear();
    m_claims.clear();
    m_claimsByProvince.clear();
    m_relations.clear();
    // Reputation is world state like any other: a new or loaded world must not
    // inherit who lied to whom in the last one.
    m_credibility.clear();
    m_openClaims.clear();
    m_credibilityHits = 0;
    m_credibilityLow = 1.0f;
    m_selectedShipIndices.clear();
    m_countryShipIndices.clear();
    m_countryProvinceIds.clear();
    m_provinceCenters.clear();
    m_provinceRadius.clear();
    m_countryCenters.clear();
    m_countryLabels.clear();
    m_provincePixels.clear();
    m_coastalCache.clear();   // answers belong to the map that is going away
    m_portAnchorCache.clear();
    m_pixelCountryArray.clear();
    m_provincePopArray.clear();
    m_provinceCountryLookup.clear();
    m_countryProvinces.clear();
    m_countryPixels.clear();
    m_countryRelationColors.clear();
    m_playerCountryId = 0;
    m_lastSelectedProvince = 0;
    m_lastPopCountryId = -1;
    m_lastRelationsCountryId = -1;
    m_lastClaimsCountryId = -1;
    m_showClaims = false;
    m_activeViewTab = 0;
    m_shipPanelScroll = 0;
    m_shipListFocusIndex = -1;
    m_mapDate.clear();

    // saveName might be just filename or full path - extract filename
    std::string fileName = saveName;
    size_t lastSlash = fileName.find_last_of('/');
    if (lastSlash != std::string::npos) fileName = fileName.substr(lastSlash + 1);

    std::string savePath = m_dataDir + "saves/" + fileName;
    startLoadingSave(savePath);
    m_currentScreen = SCREEN_LOADING;
}

void Game::reloadBorders() {
    m_renderer->computeBorderTexture(m_provinces.getImage());
    m_renderer->rebuildGlowMap(m_provinces);
    m_renderer->rebuildSelectionGlow();
    generatePoliticalTexture();
}

void Game::rebuildFlags() {
    // Unload existing flags
    for (auto& [id, tex] : m_countryFlags) {
        if (tex.id > 0) UnloadTexture(tex);
    }
    m_countryFlags.clear();

    for (auto& [id, c] : m_countries.getAll()) {
        // Two hundred-odd countries, each a rendered 256x128 flag. Individually
        // quick, collectively one of the longer phases, and it is the LAST
        // thing before the map appears -- so an unpumped stall here is the one
        // a player hears just as they expect the game to start.
        Audio::get().pump();
        const FlagPattern& fp = m_config.showActualFlags ? c.flagActual : c.flagCensored;
        Texture2D tex = FlagRenderer::render(fp, 256, 128, m_dataDir, &m_odmJsonData);
        m_countryFlags[id] = tex;
    }
}
