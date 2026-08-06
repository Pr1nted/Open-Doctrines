#include "Game.h"
#include "TextInput.h"
#include "Audio.h"
#include "GameInternals.h"
#include "mods/ModManager.h"
#include "mods/ModUpdates.h"
#include "GameUpdates.h"
#include "OdState.h"
#include "SaveManager.h"
#include "miniz.h"
#include "miniz_zip.h"
#include "Keybinds.h"
#include "raymath.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdio>
#include <fstream>
#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>
#ifndef _WIN32
#endif
#include <ctime>

namespace {

// Whether this build has a desktop to quit to.
//
// FALSE ON THE WEB, and everything that offers to leave the game asks this
// first. Leaving in a browser means closing the tab, which the page cannot do
// for you: all the game can do is stop its own frame loop, which leaves the
// canvas frozen on whatever it drew last with no way back but a reload. So the
// button and the Escape shortcut did not quit anything -- they broke the page,
// and looked like a crash while doing it.
#ifdef __EMSCRIPTEN__
constexpr bool kCanQuitToDesktop = false;
#else
constexpr bool kCanQuitToDesktop = true;
#endif

// Where the menu's top-right icons sit.
//
// Here rather than in each of the two functions that need them, because the
// drawing and the click handling work in different places and have to agree on
// the same rectangles -- and now also on how many there are.
constexpr int kMenuIconSize = 36;

Rectangle menuQuitIconRect(int screenW, int dy) {
    return { (float)(screenW - 16 - kMenuIconSize), (float)(16 + dy),
             (float)kMenuIconSize, (float)kMenuIconSize };
}

// The gear takes the corner when there is no quit icon beside it, rather than
// staying put and leaving a hole where the X used to be.
Rectangle menuGearIconRect(int screenW, int dy) {
    const float rightmost = (float)(screenW - 16 - kMenuIconSize);
    return { kCanQuitToDesktop ? rightmost - 16 - kMenuIconSize : rightmost,
             (float)(16 + dy), (float)kMenuIconSize, (float)kMenuIconSize };
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// clearThumbCache
// ────────────────────────────────────────────────────────────────────────────

// ────────────────────────────────────────────────────────────────────────────
// initMenuBackground
// ────────────────────────────────────────────────────────────────────────────
void Game::initMenuBackground() {
    // Unload previous texture
    if (m_menuBgTex.id > 0) {
        UnloadTexture(m_menuBgTex);
        m_menuBgTex = {};
    }

    // Try loading from already-loaded m_landSea data first (set by loadFromODM)
    bool lsImgOwned = false;
    Image lsImg{};
    if (m_landSea.getImage().data != nullptr) {
        lsImg = m_landSea.getImage();
    } else {
        std::string landSeaPath = m_dataDir + "land_sea.png";
        struct stat st;
        if (stat(landSeaPath.c_str(), &st) == 0) {
            lsImg = LoadImage(landSeaPath.c_str());
            lsImgOwned = true;
        }
        // Fallback: try loading land_sea.png from STDmaps/.odmap directly
        if (lsImg.data == nullptr) {
            std::string odmPath = m_dataDir + "STDmaps/map.odmap";
            struct stat odmSt;
            if (stat(odmPath.c_str(), &odmSt) == 0) {
                int dataSize = 0;
                unsigned char* zipData = LoadFileData(odmPath.c_str(), &dataSize);
                if (zipData) {
                    mz_zip_archive zip{};
                    if (mz_zip_reader_init_mem(&zip, zipData, dataSize, 0)) {
                        int idx = mz_zip_reader_locate_file(&zip, "land_sea.png", nullptr, 0);
                        if (idx >= 0) {
                            size_t sz = 0;
                            void* pngData = mz_zip_reader_extract_to_heap(&zip, idx, &sz, 0);
                            if (pngData && sz > 0) {
                                lsImg = LoadImageFromMemory(".png", (const unsigned char*)pngData, (int)sz);
                                lsImgOwned = true;
                            }
                            mz_free(pngData);
                        }
                        mz_zip_reader_end(&zip);
                    }
                    RL_FREE(zipData);
                }
            }
        }
    }
    if (lsImg.data == nullptr) {
        // No land_sea.png available yet (will be loaded with first game)
        return;
    }

    // Resize to screen height (fills full vertical space)
    int bgH = m_screenH;
    float ratio = (float)bgH / lsImg.height;
    int bgW = (int)(lsImg.width * ratio);

    Image resized = ImageCopy(lsImg);
    ImageResize(&resized, bgW, bgH);

    // Create silhouette pixels
    Color* pixels = LoadImageColors(resized);
    m_menuBgPixelsW = bgW;
    m_menuBgPixelsH = bgH;
    m_menuBgLandPixels.resize(bgW * bgH, false);
    m_menuBgLandCoords.clear();

    Color* bgPixels = new Color[bgW * bgH];
    Color seaColor = {8, 14, 32, 255};
    Color landColor = {55, 58, 65, 255};
    for (int i = 0; i < bgW * bgH; ++i) bgPixels[i] = seaColor;

    for (int y = 0; y < bgH; ++y) {
        for (int x = 0; x < bgW; ++x) {
            int idx = y * bgW + x;
            bool isLand = pixels[idx].r > 128;
            m_menuBgLandPixels[idx] = isLand;
            if (isLand) {
                bgPixels[idx] = landColor;
                m_menuBgLandCoords.push_back({x, y});
            }
        }
    }

    // Create display texture
    Image bgImg{};
    bgImg.data = bgPixels;
    bgImg.width = bgW;
    bgImg.height = bgH;
    bgImg.mipmaps = 1;
    bgImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    m_menuBgTex = LoadTextureFromImage(bgImg);
    SetTextureFilter(m_menuBgTex, TEXTURE_FILTER_BILINEAR);
    m_menuBgTexW = bgW;
    m_menuBgTexH = bgH;

    UnloadImageColors(pixels);
    delete[] bgPixels;
    UnloadImage(resized);
    if (lsImgOwned) UnloadImage(lsImg);

    std::cout << "  Menu background: " << bgW << "x" << bgH
              << ", " << m_menuBgLandCoords.size() << " land pixels" << std::endl;
    m_menuBgInitScreenW = m_screenW;
    m_menuBgInitScreenH = m_screenH;
}

// ────────────────────────────────────────────────────────────────────────────
// drawMenuBackground
// ────────────────────────────────────────────────────────────────────────────
void Game::drawMenuBackground(bool dimmed) {
    if (m_menuBgTex.id > 0) {
        int bgW = m_menuBgTexW;
        int bgH = m_menuBgTexH;
        float scrollRatio = fmodf(m_menuBgScroll, (float)bgW) / (float)bgW;
        DrawTexturePro(m_menuBgTex,
            {0, 0, (float)bgW, (float)bgH},
            {-scrollRatio * m_screenW, (float)(m_screenH - bgH), (float)m_screenW, (float)bgH},
            {0, 0}, 0.0f, WHITE);
        DrawTexturePro(m_menuBgTex,
            {0, 0, (float)bgW, (float)bgH},
            {(1.0f - scrollRatio) * m_screenW, (float)(m_screenH - bgH), (float)m_screenW, (float)bgH},
            {0, 0}, 0.0f, WHITE);

        for (auto& p : m_menuParticles) {
            float sx = (p.tx / (float)bgW) * m_screenW - scrollRatio * m_screenW;
            float sy = m_screenH - bgH + p.ty;
            if (sx < -200) sx += m_screenW;
            if (sx > m_screenW + 200) sx -= m_screenW;
            if (sx < -200 || sx > m_screenW + 200) continue;

            float lifeRatio = p.lifetime > 0 ? std::min(1.0f, p.alpha / p.lifetime) : 0;
            float a = lifeRatio < 0.5f
                ? lifeRatio * 2.0f
                : (1.0f - lifeRatio) * 2.0f;
            unsigned char alpha = (unsigned char)(a * 160);
            float s = p.size;
            DrawCircleLines((int)sx, (int)sy, s, {255, 200, 100, alpha});
            DrawCircleLines((int)sx, (int)sy, s * 0.6f, {255, 160, 60, (unsigned char)(alpha * 0.6f)});
            if (lifeRatio < 0.3f) {
                float flashA = (1.0f - lifeRatio / 0.3f) * 200;
                DrawCircle((int)sx, (int)sy, s * 0.3f, {255, 220, 150, (unsigned char)flashA});
            }
        }

        if (dimmed) {
            DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// updateMenuBackground
// ────────────────────────────────────────────────────────────────────────────
void Game::updateMenuBackground() {
    // ── Background scrolling ──
    float dt = GetFrameTime();
    m_menuBgScroll += dt * 30.0f; // scroll speed: 30 px/s

    // ── Particle system ──
    for (auto it = m_menuParticles.begin(); it != m_menuParticles.end(); ) {
        it->alpha += dt;
        it->size = std::min(it->size + dt * 25.0f, it->maxSize);
        if (it->alpha > it->lifetime) {
            it = m_menuParticles.erase(it);
        } else {
            ++it;
        }
    }
    // Spawn chance per frame (~3% = roughly 2 per second at 60fps)
    if ((float)rand() / (float)RAND_MAX < 0.03f && !m_menuBgLandCoords.empty()) {
        int idx = rand() % (int)m_menuBgLandCoords.size();
        auto [px, py] = m_menuBgLandCoords[idx];
        int bgH = m_menuBgPixelsH;
        float sy = m_screenH - bgH + py;
        // Avoid very top bar area
        if (sy < 60) return;
        float sz = 8.0f + (rand() % 40) * 0.6f;
        BgParticle p;
        p.tx = (float)px;
        p.ty = (float)py;
        p.size = 2.0f;
        p.maxSize = sz;
        p.alpha = 0.0f;
        p.lifetime = 1.5f + (rand() % 10) * 0.2f;
        m_menuParticles.push_back(p);
    }
}

// ────────────────────────────────────────────────────────────────────────────
// loadCredits
// ────────────────────────────────────────────────────────────────────────────
void Game::loadCredits() {
    m_credits.clear();
    std::ifstream f(m_dataDir + "credits.txt");
    if (!f) { std::cerr << "[CREDITS] File not found: data/credits.txt" << std::endl; return; }
    std::string line;
    while (std::getline(f, line)) {
        // Trim trailing whitespace
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty()) {
            m_credits.push_back({CreditEntry::SPACER, "", 40.0f});
            continue;
        }
        if (line[0] == '#') {
            m_credits.push_back({CreditEntry::ROLE, line.substr(1)});
        } else if (line[0] == '@') {
            m_credits.push_back({CreditEntry::NAME, line.substr(1)});
        } else if (line[0] == '$') {
            m_credits.push_back({CreditEntry::SMALL, line.substr(1)});
        } else if (line.find("---") != std::string::npos || line.find("---") != std::string::npos) {
            m_credits.push_back({CreditEntry::DIVIDER, ""});
        } else {
            // Treat unrecognized as name
            m_credits.push_back({CreditEntry::NAME, line});
        }
    }
    m_creditsLoaded = true;
    m_creditsScroll = 0.0f;
    std::cout << "[CREDITS] Loaded " << m_credits.size() << " entries" << std::endl;
}

// ────────────────────────────────────────────────────────────────────────────
// updateCredits
// ────────────────────────────────────────────────────────────────────────────
void Game::updateCredits() {
    if (isMouseOverConsole()) return;
    float dt = GetFrameTime();
    m_creditsScroll += dt * m_creditsSpeed;

    // Close on ESC
    if (IsKeyPressed(KEY_ESCAPE))
        m_currentScreen = SCREEN_MENU;

    // Close on X button
    Vector2 mouse = getMouse();
    int xBtnSz = 36;
    Rectangle xBtn = {(float)(m_screenW - 16 - xBtnSz), 16, (float)xBtnSz, (float)xBtnSz};
    bool xHov = CheckCollisionPointRec(mouse, xBtn);
    if (xHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
        m_currentScreen = SCREEN_MENU;
}

// ────────────────────────────────────────────────────────────────────────────
// drawCredits
// ────────────────────────────────────────────────────────────────────────────
void Game::drawCredits() {
    drawMenuBackground();

    // Dim overlay so text is readable
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 160});

    // Precompute per-entry heights
    struct EntryH { float h; };
    std::vector<EntryH> entryHs;
    for (auto& e : m_credits) {
        float eh = 0;
        if (e.type == CreditEntry::ROLE) eh = 56;
        else if (e.type == CreditEntry::NAME) eh = 34;
        else if (e.type == CreditEntry::SMALL) eh = 28;
        else if (e.type == CreditEntry::DIVIDER) eh = 24;
        else if (e.type == CreditEntry::SPACER) eh = e.spacing;
        entryHs.push_back({eh});
    }
    float gap = (float)m_screenH; // gap between copies = one screen height to avoid duplicates

    int centerX = m_screenW / 2;

    // Compute how many copies to draw to fill the screen
    float copyH = gap;
    for (auto& eh : entryHs) copyH += eh.h;

    if (copyH <= 0) return;

    // Draw copies covering the screen — no wrapping, efficient range
    float baseY = (float)m_screenH - m_creditsScroll;
    float margin = copyH * 2.0f;
    int firstCopy = (int)floorf((-baseY - margin) / copyH);
    int lastCopy = (int)ceilf((-baseY + (float)m_screenH + margin) / copyH);
    for (int copy = firstCopy; copy <= lastCopy; ++copy) {
        float y = baseY + copy * copyH;
        for (size_t ei = 0; ei < m_credits.size(); ++ei) {
            auto& e = m_credits[ei];
            float eh = entryHs[ei].h;

            if (e.type == CreditEntry::ROLE) {
                int fs = 52;
                int tw = MeasureText(e.text.c_str(), fs);
                DrawText(e.text.c_str(), centerX - tw / 2, (int)(y + 4), fs, hexToColor(m_config.accent()));
            } else if (e.type == CreditEntry::NAME) {
                int fs = 24;
                int tw = MeasureText(e.text.c_str(), fs);
                DrawText(e.text.c_str(), centerX - tw / 2, (int)y, fs, WHITE);
            } else if (e.type == CreditEntry::SMALL) {
                int fs = 18;
                int tw = MeasureText(e.text.c_str(), fs);
                DrawText(e.text.c_str(), centerX - tw / 2, (int)y, fs, LIGHTGRAY);
            } else if (e.type == CreditEntry::DIVIDER) {
                DrawRectangle(centerX - 150, (int)y + 6, 300, 1, {160, 160, 170, 100});
            }
            // SPACER: nothing to draw, just advance y

            y += eh;
        }
    }

    // Close (X) button
    int xBtnSz = 36;
    Vector2 mouse = getMouse();
    Rectangle xBtn = {(float)(m_screenW - 16 - xBtnSz), 16, (float)xBtnSz, (float)xBtnSz};
    bool xHov = CheckCollisionPointRec(mouse, xBtn);
    Color xBg = xHov ? Color{255, 64, 64, 32} : BLANK;
    DrawRectangleRounded(xBtn, 0.2f, 8, xBg);
    int cx = (int)(xBtn.x + xBtnSz / 2);
    int cy = (int)(xBtn.y + xBtnSz / 2);
    Color xCol = xHov ? Color{255, 80, 80, 255} : Color{160, 160, 170, 200};
    DrawLine(cx - 8, cy - 8, cx + 8, cy + 8, xCol);
    DrawLine(cx + 8, cy - 8, cx - 8, cy + 8, xCol);

    // Hint
    DrawText("ESC to close", 10, m_screenH - 24, 14, Color{80, 80, 90, 200});
}

// ────────────────────────────────────────────────────────────────────────────
// updateCommunityMenu
// ────────────────────────────────────────────────────────────────────────────
void Game::updateCommunityMenu() {
    if (isMouseOverConsole()) return;
    Vector2 mouse = getMouse();

    int btnW = 260, btnH = 80, gap = 20, backBtnH = 48;
    int totalH = btnH * 2 + gap + backBtnH + 30;
    int startY = (m_screenH - totalH) / 2;
    int centerX = m_screenW / 2;

    Rectangle discordBtn = {(float)(centerX - btnW / 2), (float)startY, (float)btnW, (float)btnH};
    Rectangle githubBtn = {(float)(centerX - btnW / 2), (float)(startY + btnH + gap), (float)btnW, (float)btnH};
    Rectangle backBtn = {(float)(centerX - 80), (float)(startY + btnH * 2 + gap + 30), 160, (float)backBtnH};

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec(mouse, discordBtn)) {
            system("open https://discord.gg/wqS65jzVv5");
        } else if (CheckCollisionPointRec(mouse, githubBtn)) {
            system("open https://github.com/Pr1nted/Open-Doctrines");
        } else if (CheckCollisionPointRec(mouse, backBtn)) {
            Audio::get().playSfx("back");
            m_currentScreen = SCREEN_MENU;
        }
    }

    if (IsKeyPressed(KEY_ESCAPE))
        m_currentScreen = SCREEN_MENU;
}

// ────────────────────────────────────────────────────────────────────────────
// drawCommunityMenu
// ────────────────────────────────────────────────────────────────────────────
void Game::drawCommunityMenu() {
    drawMenuBackground();
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 160});

    // Loaded once, and from m_dataDir like every other asset. These two were the
    // only paths in the game that assumed the process working directory was the
    // project root, so they could resolve from a source checkout and never from
    // the .app bundle, where the CWD is wherever the user launched it from.
    //
    // Checked with FileExists first because both files are absent from the
    // repository: raylib logs a WARNING per failed LoadTexture, and the buttons
    // below already fall back to their text labels, so the only thing the
    // attempt produced was noise in the log every run.
    static bool linkIconsTried = false;
    static Texture2D discordIcon{};
    static Texture2D githubIcon{};
    if (!linkIconsTried) {
        linkIconsTried = true;
        const std::string dir = m_dataDir + "icons/links/";
        const std::string dPath = dir + "discord.png";
        const std::string gPath = dir + "github.png";
        if (FileExists(dPath.c_str())) discordIcon = LoadTexture(dPath.c_str());
        if (FileExists(gPath.c_str())) githubIcon  = LoadTexture(gPath.c_str());
    }

    Vector2 mouse = getMouse();
    int btnW = 260, btnH = 80, gap = 20, backBtnH = 48;
    int totalH = btnH * 2 + gap + backBtnH + 30;
    int startY = (m_screenH - totalH) / 2;
    int centerX = m_screenW / 2;
    int fontSize = 24;

    // Title
    const char* title = "Community";
    int titleW = MeasureText(title, 48);
    DrawText(title, centerX - titleW / 2, startY - 90, 48, hexToColor(m_config.accent()));

    // Discord button
    {
        Rectangle r = {(float)(centerX - btnW / 2), (float)startY, (float)btnW, (float)btnH};
        bool hover = CheckCollisionPointRec(mouse, r);
        Color bg = hover ? Color{60, 50, 80, 240} : Color{40, 35, 55, 220};
        Color bd = hover ? Color{130, 100, 180, 255} : Color{100, 80, 140, 200};
        DrawRectangleRounded(r, 0.15f, 8, bg);
        DrawRectangleRoundedLines(r, 0.15f, 8, bd);

        if (discordIcon.id > 0) {
            float scale = 36.0f / discordIcon.height;
            float iw = discordIcon.width * scale;
            DrawTexturePro(discordIcon, {0, 0, (float)discordIcon.width, (float)discordIcon.height},
                {r.x + 16, r.y + (btnH - 36) / 2.0f, iw, 36}, {0, 0}, 0, WHITE);
        }
        DrawText("Discord", centerX - MeasureText("Discord", fontSize) / 2, startY + btnH / 2 - fontSize / 2, fontSize, hover ? WHITE : LIGHTGRAY);
    }

    // GitHub button
    {
        Rectangle r = {(float)(centerX - btnW / 2), (float)(startY + btnH + gap), (float)btnW, (float)btnH};
        bool hover = CheckCollisionPointRec(mouse, r);
        Color bg = hover ? Color{50, 50, 60, 240} : Color{35, 35, 45, 220};
        Color bd = hover ? Color{150, 150, 170, 255} : Color{110, 110, 130, 200};
        DrawRectangleRounded(r, 0.15f, 8, bg);
        DrawRectangleRoundedLines(r, 0.15f, 8, bd);

        if (githubIcon.id > 0) {
            float scale = 36.0f / githubIcon.height;
            float iw = githubIcon.width * scale;
            DrawTexturePro(githubIcon, {0, 0, (float)githubIcon.width, (float)githubIcon.height},
                {r.x + 16, r.y + (btnH - 36) / 2.0f, iw, 36}, {0, 0}, 0, WHITE);
        }
        DrawText("GitHub", centerX - MeasureText("GitHub", fontSize) / 2, startY + btnH + gap + btnH / 2 - fontSize / 2, fontSize, hover ? WHITE : LIGHTGRAY);
    }

    // Back button
    {
        Rectangle r = {(float)(centerX - 80), (float)(startY + btnH * 2 + gap + 30), 160, (float)backBtnH};
        bool hover = CheckCollisionPointRec(mouse, r);
        Color bg = hover ? Color{70, 70, 80, 240} : Color{50, 50, 60, 220};
        Color bd = hover ? Color{150, 150, 170, 255} : Color{110, 110, 130, 200};
        DrawRectangleRounded(r, 0.15f, 8, bg);
        DrawRectangleRoundedLines(r, 0.15f, 8, bd);
        DrawText("Back", centerX - MeasureText("Back", 22) / 2, r.y + (backBtnH - 22) / 2, 22, hover ? WHITE : LIGHTGRAY);
    }

    DrawText("ESC to go back", 10, m_screenH - 24, 14, Color{80, 80, 90, 200});
}

// ────────────────────────────────────────────────────────────────────────────
// drawMainMenu
// ────────────────────────────────────────────────────────────────────────────
void Game::drawMainMenu() {
    int centerX = m_screenW / 2;
    int fontSize = 30;
    int itemH = 50;

    // ── Intro animation (after the splash): title slides in from the left,
    //    buttons from the right, icons from above, everything fading up.
    //    Purely visual — updateMainMenu() ignores input until it finishes,
    //    so the click rects never disagree with the drawn positions. ──
    if (m_menuIntro < 1.0f) {
        m_menuIntro += GetFrameTime() / 0.45f;
        if (m_menuIntro > 1.0f) m_menuIntro = 1.0f;
    }
    float e = 1.0f - powf(1.0f - m_menuIntro, 3.0f); // ease-out cubic
    float a = e;                                     // group alpha
    int titleDX = (int)((1.0f - e) * -140.0f);
    int btnDX   = (int)((1.0f - e) *  140.0f);
    int iconDY  = (int)((1.0f - e) *  -60.0f);
    auto fade = [&](Color c) { return ColorAlpha(c, a * (c.a / 255.0f)); };

    // ── Background: scrolling landmass silhouette ──
    drawMenuBackground();

    // Dark gradient behind title (top area)
    DrawRectangleGradientV(0, 0, m_screenW, 200, fade({0, 0, 0, 200}), {0, 0, 0, 0});
    // Dark gradient behind buttons (center area)
    int btnStartY = m_screenH / 2 - (MAIN_MENU_COUNT * itemH) / 2 + 30 - 20;
    int btnEndY = btnStartY + MAIN_MENU_COUNT * itemH + 40;
    DrawRectangleGradientV(0, btnStartY, m_screenW, btnEndY - btnStartY, fade({0, 0, 0, 180}), {0, 0, 0, 0});
    DrawRectangleGradientV(0, btnStartY, m_screenW, btnEndY - btnStartY, {0, 0, 0, 0}, fade({0, 0, 0, 180}));
    // Solid dark strip for readability
    DrawRectangle(centerX - 250, btnStartY, 500, btnEndY - btnStartY, fade({0, 0, 0, 100}));

    // Title
    const char* title = "OpenDoctrines";
    int titleSize = 60;
    int titleW = MeasureText(title, titleSize);
    DrawText(title, centerX - titleW / 2 + titleDX, 80, titleSize, fade(hexToColor(m_config.accent())));

    const char* subtitle = "A Grand Strategy Game";
    int subSize = 20;
    int subW = MeasureText(subtitle, subSize);
    DrawText(subtitle, centerX - subW / 2 + titleDX, 150, subSize, fade(Color{160, 160, 170, 255}));

    // Decorative line
    int lineW = 300;
    DrawRectangle(centerX - lineW / 2 + titleDX, 180, lineW, 2, fade(ColorAlpha(hexToColor(m_config.accent()), 100.0f/255.0f)));
    DrawRectangle(centerX - lineW / 2 + 1 + titleDX, 181, lineW - 2, 1, fade(Color{100, 90, 50, 60}));

    // Buttons
    int count = MAIN_MENU_COUNT;
    int startY = m_screenH / 2 - (count * itemH) / 2 + 30;

    Vector2 mouse = getMouse();
    int hovered = -1;
    if (m_menuIntro >= 1.0f) {
        for (int i = 0; i < count; ++i) {
            int y = startY + i * itemH;
            int tw = MeasureText(MAIN_MENU_ITEMS[i], fontSize);
            Rectangle rect = { (float)(centerX - tw / 2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) };
            if (CheckCollisionPointRec(mouse, rect)) { hovered = i; break; }
        }
    }

    for (int i = 0; i < count; ++i) {
        int y = startY + i * itemH;
        bool isSelected = (i == m_menuIndex);
        bool isHovered = (i == hovered);
        Color textColor = isSelected ? hexToColor(m_config.accent()) : (isHovered ? WHITE : LIGHTGRAY);
        Color bgColor = isHovered ? Color{255, 255, 255, 16} : BLANK;

        int tw = MeasureText(MAIN_MENU_ITEMS[i], fontSize);
        Rectangle rect = { (float)(centerX - tw / 2 - 20 + btnDX), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) };
        DrawRectangleRounded(rect, 0.1f, 8, fade(bgColor));
        DrawText(MAIN_MENU_ITEMS[i], centerX - tw / 2 + btnDX, y, fontSize, fade(textColor));

        if (isSelected) {
            int underlineW = tw + 20;
            DrawRectangle(centerX - underlineW / 2 + btnDX, y + fontSize + 4, underlineW, 2, fade(hexToColor(m_config.accent())));
        }
    }

    // What just happened to the player's .odstate, under the buttons. It has to
    // be said somewhere: a save that silently failed and one that silently
    // succeeded look identical from in here.
    if (m_odStateMsgTimer > 0.0f && !m_odStateMsg.empty()) {
        int msgFs = 16;
        int msgW = MeasureText(m_odStateMsg.c_str(), msgFs);
        int msgY = startY + count * itemH + 10;
        float ma = m_odStateMsgTimer < 1.0f ? m_odStateMsgTimer : 1.0f;   // fade out
        Color msgCol = m_odStateMsgBad ? Color{235, 110, 110, 255} : Color{150, 210, 150, 255};
        DrawText(m_odStateMsg.c_str(), centerX - msgW / 2 + btnDX, msgY, msgFs,
                 fade(ColorAlpha(msgCol, ma)));
    }

    // Top-right icons: Settings (gear), and Quit (X) where there is somewhere
    // to quit to -- see kCanQuitToDesktop.

    // Gear icon (settings) — reuse style from world browser
    {
        Rectangle gearRect = menuGearIconRect(m_screenW, iconDY);
        bool gearHover = m_menuIntro >= 1.0f && CheckCollisionPointRec(mouse, gearRect);
        Color gearBg = gearHover ? Color{255, 255, 255, 24} : BLANK;
        DrawRectangleRounded(gearRect, 0.3f, 6, fade(gearBg));

        int gcx = (int)gearRect.x + kMenuIconSize / 2;
        int gcy = (int)gearRect.y + kMenuIconSize / 2;
        Color gearColor = fade(gearHover ? Color{220, 220, 230, 255} : Color{160, 160, 170, 200});
        DrawCircle(gcx, gcy, 7, gearColor);
        for (int t = 0; t < 8; ++t) {
            float ang = t * 45.0f * DEG2RAD;
            int tx = gcx + (int)(cosf(ang) * 8);
            int ty = gcy + (int)(sinf(ang) * 8);
            DrawCircle(tx, ty, 2.5f, gearColor);
        }
    }

    // Quit X icon
    if constexpr (kCanQuitToDesktop) {
        Rectangle quitRect = menuQuitIconRect(m_screenW, iconDY);
        bool quitHover = m_menuIntro >= 1.0f && CheckCollisionPointRec(mouse, quitRect);
        Color quitBg = quitHover ? Color{255, 64, 64, 32} : BLANK;
        DrawRectangleRounded(quitRect, 0.2f, 8, fade(quitBg));

        int cx = (int)quitRect.x + kMenuIconSize / 2;
        int cy = (int)quitRect.y + kMenuIconSize / 2;
        int arm = 8;
        Color xColor = fade(quitHover ? Color{255, 80, 80, 255} : Color{160, 160, 170, 200});
        DrawLine(cx - arm, cy - arm, cx + arm, cy + arm, xColor);
        DrawLine(cx + arm, cy - arm, cx - arm, cy + arm, xColor);
    }

    // Version info
    DrawText(TextFormat("v%s", GAME_VERSION), 10, m_screenH - 24, 14, fade(Color{80, 80, 90, 200}));

    // Why the last load failed, if it did.
    //
    // A failed load drops the player back here. Until this was drawn it did so
    // in complete silence: pick a scenario, watch the loading screen flicker,
    // end up on the menu with no idea why. That is what "scenarios do not load"
    // looks like from the outside, and it reads identically whether the map file
    // is missing, the download was truncated, or antivirus ate the data folder.
    // The reasons existed all along, on std::cerr, which a Windows GUI build
    // does not have.
    //
    // Drawn above the menu's own text rather than over it, and cleared the
    // moment another load starts.
    if (!m_loadError.empty()) {
        const int pad = 10;
        const int fs  = 15;
        std::vector<std::string> lines;
        {
            std::string cur;
            for (char ch : m_loadError) {
                if (ch == '\n') { lines.push_back(cur); cur.clear(); }
                else cur += ch;
            }
            lines.push_back(cur);
        }
        int wide = 0;
        for (const auto& l : lines) wide = std::max(wide, MeasureText(l.c_str(), fs));

        const int boxH = (int)lines.size() * (fs + 4) + pad * 2;
        const int boxW = wide + pad * 2;
        const int boxX = m_screenW / 2 - boxW / 2;
        const int boxY = m_screenH - boxH - 40;

        DrawRectangle(boxX, boxY, boxW, boxH, ColorAlpha(Color{40, 10, 10, 255}, a * 0.92f));
        DrawRectangleLines(boxX, boxY, boxW, boxH, fade(Color{200, 70, 70, 255}));
        for (size_t i = 0; i < lines.size(); ++i)
            DrawText(lines[i].c_str(), boxX + pad,
                     boxY + pad + (int)i * (fs + 4), fs,
                     fade(Color{255, 190, 190, 255}));
    }

    // The "!" — only ever drawn when a newer release actually exists, so its
    // presence is information rather than decoration.
    if (GameUpdates::get().updateAvailable()) {
        Rectangle r = updateBadgeRect();
        bool hov = m_menuIntro >= 1.0f && CheckCollisionPointRec(mouse, r);
        Color accent = hexToColor(m_config.accent());
        DrawRectangleRounded(r, 0.4f, 8, fade(ColorAlpha(accent, hov ? 0.35f : 0.20f)));
        DrawRectangleRoundedLines(r, 0.4f, 8, fade(ColorAlpha(accent, 0.8f)));
        // A gentle pulse, so it reads as new without demanding attention.
        float pulse = 0.75f + 0.25f * sinf((float)GetTime() * 3.0f);
        const int badgeR = 7;
        const int dotX = (int)r.x + 14;
        const int dotY = (int)(r.y + r.height / 2);
        DrawCircle(dotX, dotY, (float)badgeR, fade(ColorAlpha(accent, pulse)));

        // The mark is DRAWN, not typed, and that is the fix rather than a
        // flourish.
        //
        // As text it never fitted. DrawText positions a glyph by the top-left
        // of its LINE BOX, and the box is as tall as the font size -- so a 16px
        // "!" in a circle 14px across overflowed it, and the ink sits high
        // inside its own box, so centring the box still left the mark high. The
        // old code answered both with a hand-tuned offset that was simply wrong
        // in a different direction, and at this size one pixel is visible.
        //
        // Two rectangles have none of those problems: the ink is exactly what
        // is asked for, it is symmetric about the circle's centre by
        // construction, and it stays crisp because the edges land on pixels
        // instead of on a scaled glyph. Sized from badgeR so the two cannot
        // drift apart if the badge ever changes.
        const Color ink = fade(Color{20, 20, 25, 255});
        const int barW = std::max(2, badgeR / 3);          // 2 at r=7
        const int barH = std::max(3, (badgeR * 5) / 7);    // 5 at r=7
        const int gap  = std::max(1, badgeR / 7);          // 1 at r=7
        // Whole ink height is barH + gap + barW, centred on dotY.
        const int inkTop = dotY - (barH + gap + barW) / 2;
        DrawRectangle(dotX - barW / 2, inkTop, barW, barH, ink);
        DrawRectangle(dotX - barW / 2, inkTop + barH + gap, barW, barW, ink);

        // The label sits on the same centre line, by the same rule.
        const int labelSize = 14;
        DrawText("Update available", (int)r.x + 28, dotY - labelSize / 2, labelSize,
                 fade(hov ? WHITE : Color{200, 200, 210, 255}));
    }

    // Feedback message
    if (m_menuFeedbackTimer > 0 && !m_menuFeedback.empty()) {
        int fbW = MeasureText(m_menuFeedback.c_str(), 16);
        DrawText(m_menuFeedback.c_str(), centerX - fbW / 2, m_screenH - 80, 16, ColorAlpha(hexToColor(m_config.accent()), std::min(1.0f, m_menuFeedbackTimer)));
    }

    if (m_updatePanel) drawUpdatePanel();
    // Last, so it covers everything above -- it owns the keyboard while it is
    // up and must look like it does.
    drawOdStatePrompt();
}

// ────────────────────────────────────────────────────────────────────────────
// Startup splash: "Pr1nted presents" fades in, holds, fades out, then hands
// off to the main menu. Any key/click skips straight to the menu.
// ────────────────────────────────────────────────────────────────────────────
static const float SPLASH_FADE_IN = 0.6f;
static const float SPLASH_HOLD = 1.6f;
static const float SPLASH_FADE_OUT = 0.8f;
static const float SPLASH_TOTAL = SPLASH_FADE_IN + SPLASH_HOLD + SPLASH_FADE_OUT;

void Game::updateSplashScreen(float dt) {
    m_splashTimer += dt;
    updateMenuBackground(); // keep the menu bg animating so the fade-out reveals it in motion
    bool skip = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ESCAPE) ||
                IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (skip || m_splashTimer >= SPLASH_TOTAL) {
        m_currentScreen = SCREEN_MENU;
        m_menuIntro = 0.0f; // play the menu's slide/fade-in instead of popping
    }
}

void Game::drawSplashScreen() {
    float textAlpha, blackAlpha;
    if (m_splashTimer < SPLASH_FADE_IN) {
        textAlpha = m_splashTimer / SPLASH_FADE_IN;
        blackAlpha = 1.0f;
    } else if (m_splashTimer < SPLASH_FADE_IN + SPLASH_HOLD) {
        textAlpha = 1.0f;
        blackAlpha = 1.0f;
    } else {
        float t = (m_splashTimer - SPLASH_FADE_IN - SPLASH_HOLD) / SPLASH_FADE_OUT;
        t = std::max(0.0f, std::min(1.0f, t));
        textAlpha = 1.0f - t;
        blackAlpha = 1.0f - t; // black bg fades out too, revealing the menu bg underneath
    }
    textAlpha = std::max(0.0f, std::min(1.0f, textAlpha));
    blackAlpha = std::max(0.0f, std::min(1.0f, blackAlpha));

    // Menu background sits underneath the whole time; the black overlay just
    // uncovers it as it fades, so the splash crossfades into the real menu
    // instead of hard-cutting.
    drawMenuBackground(false);
    DrawRectangle(0, 0, m_screenW, m_screenH, ColorAlpha(BLACK, blackAlpha));

    const char* line = "Pr1nted presents";
    int fontSize = std::max(20, m_screenW / 28);
    int tw = MeasureText(line, fontSize);
    Color col = ColorAlpha(WHITE, textAlpha);
    DrawText(line, m_screenW / 2 - tw / 2, m_screenH / 2 - fontSize / 2, fontSize, col);
}

// ────────────────────────────────────────────────────────────────────────────
// updateMainMenu
// ────────────────────────────────────────────────────────────────────────────
// ── The game updater's UI ──────────────────────────────────────────────────
//
// Geometry lives in one place so the drawing and the click handling cannot
// disagree about where anything is. That is not fussiness: a button drawn in
// one place and hit-tested in another is the classic way an update button
// becomes unclickable on somebody else's screen resolution.

namespace {

struct UpdatePanelLayout {
    Rectangle panel, primary, secondary;
};

UpdatePanelLayout updatePanelLayout(int screenW, int screenH) {
    UpdatePanelLayout L;
    const float w = 600, h = 320;
    L.panel = {(float)(screenW / 2) - w / 2, (float)(screenH / 2) - h / 2, w, h};
    float by = L.panel.y + h - 60;
    L.primary   = {L.panel.x + w - 380, by, 200, 40};
    L.secondary = {L.panel.x + w - 160, by, 120, 40};
    return L;
}

// Breaks text into lines that fit `maxW`, keeping the newlines the release
// notes already contain.
std::vector<std::string> wrapText(const std::string& text, int fontSize, int maxW) {
    std::vector<std::string> lines;
    std::string line;
    auto flush = [&]() { lines.push_back(line); line.clear(); };
    std::string word;
    auto pushWord = [&]() {
        if (word.empty()) return;
        std::string candidate = line.empty() ? word : line + " " + word;
        if (MeasureText(candidate.c_str(), fontSize) > maxW && !line.empty()) {
            flush();
            line = word;
        } else {
            line = candidate;
        }
        word.clear();
    };
    for (char c : text) {
        if (c == '\n') { pushWord(); flush(); }
        else if (c == ' ') pushWord();
        else word += c;
    }
    pushWord();
    if (!line.empty()) flush();
    return lines;
}

}  // namespace

Rectangle Game::updateBadgeRect() const {
    return {6.0f, (float)m_screenH - 52.0f, 160.0f, 26.0f};
}

void Game::drawUpdatePanel() {
    using Stage = GameUpdates::Stage;
    GameUpdates::Status st = GameUpdates::get().status();
    UpdatePanelLayout L = updatePanelLayout(m_screenW, m_screenH);
    Color accent = hexToColor(m_config.accent());

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 170});
    DrawRectangleRounded(L.panel, 0.04f, 8, Color{24, 24, 30, 250});
    DrawRectangleRoundedLines(L.panel, 0.04f, 8, ColorAlpha(accent, 0.6f));

    int x = (int)L.panel.x + 28;
    int y = (int)L.panel.y + 24;

    DrawText("A new version is available", x, y, 24, accent);
    y += 38;

    DrawText(TextFormat("You have  v%s", GAME_VERSION), x, y, 16,
             Color{170, 170, 180, 255});
    DrawText(TextFormat("Newest  v%s", st.latest.c_str()), x + 260, y, 16, WHITE);
    y += 30;

    DrawLine(x, y, (int)(L.panel.x + L.panel.width) - 28, y,
             Color{60, 60, 70, 255});
    y += 14;

    // What the panel says in the middle depends entirely on where the update
    // has got to, so each stage gets its own words rather than a generic
    // "working…".
    const int notesW = (int)L.panel.width - 56;
    switch (st.stage) {
        case Stage::Downloading: {
            DrawText(TextFormat("Downloading…  %d%%", st.percent), x, y, 16, WHITE);
            Rectangle bar = {(float)x, (float)y + 26, (float)notesW, 10};
            DrawRectangleRounded(bar, 0.5f, 6, Color{50, 50, 58, 255});
            Rectangle fill = bar;
            fill.width = bar.width * (float)st.percent / 100.0f;
            if (fill.width > 4) DrawRectangleRounded(fill, 0.5f, 6, accent);
            break;
        }
        case Stage::Installing:
            DrawText("Installing…", x, y, 16, WHITE);
            DrawText("Your saves, worlds and mods are not touched.", x, y + 24, 14,
                     Color{150, 150, 160, 255});
            break;
        case Stage::Restart:
            DrawText("Installed.", x, y, 18, accent);
            DrawText("Quit and start OpenDoctrines again to play the new version.",
                     x, y + 28, 15, Color{200, 200, 210, 255});
            break;
        case Stage::OpenedPage:
            DrawText("Opened the download page in your browser.", x, y, 16, WHITE);
            DrawText("Download the new version and replace this copy — macOS builds",
                     x, y + 26, 14, Color{150, 150, 160, 255});
            DrawText("are not signed, so the game cannot replace itself here.",
                     x, y + 44, 14, Color{150, 150, 160, 255});
            break;
        case Stage::Managed:
            // Deliberately does NOT offer the download page. This copy is owned
            // by something else, and sending the player to a zip would have
            // them install a second, unmanaged copy beside the managed one.
            DrawText("This copy is updated for you.", x, y, 16, WHITE);
            DrawText("It was installed by a package manager, a store or an installer,",
                     x, y + 26, 14, Color{150, 150, 160, 255});
            DrawText("so updating it here would put the two out of step. Update it the",
                     x, y + 44, 14, Color{150, 150, 160, 255});
            DrawText("same way you installed it.",
                     x, y + 62, 14, Color{150, 150, 160, 255});
            break;
        case Stage::Failed: {
            DrawText("The update did not finish.", x, y, 16, Color{235, 120, 120, 255});
            auto lines = wrapText(st.error, 14, notesW);
            for (size_t i = 0; i < lines.size() && i < 3; ++i)
                DrawText(lines[i].c_str(), x, y + 26 + (int)i * 18, 14,
                         Color{170, 170, 180, 255});
            DrawText("Nothing was changed. You can download it yourself instead.",
                     x, y + 26 + 3 * 18, 14, Color{150, 150, 160, 255});
            break;
        }
        default: {
            if (!st.notes.empty()) {
                auto lines = wrapText(st.notes, 15, notesW);
                for (size_t i = 0; i < lines.size() && i < 5; ++i)
                    DrawText(lines[i].c_str(), x, y + (int)i * 20, 15,
                             Color{190, 190, 200, 255});
            } else {
                DrawText("No release notes were published for this version.", x, y, 15,
                         Color{150, 150, 160, 255});
            }
            break;
        }
    }

    // Buttons. The primary one names what it will actually do — on macOS it
    // opens a page and says so, rather than saying "Update" and then not
    // updating anything.
    const char* primaryLabel = nullptr;
    switch (st.stage) {
        case Stage::Available:
            primaryLabel = GameUpdates::canSelfInstall() ? "Update now"
                                                         : "Open download page";
            break;
        case Stage::Failed:
            primaryLabel = "Open download page";
            break;
        default:
            break;
    }

    Vector2 mouse = getMouse();
    if (primaryLabel) {
        bool hov = CheckCollisionPointRec(mouse, L.primary);
        DrawRectangleRounded(L.primary, 0.25f, 8,
                             ColorAlpha(accent, hov ? 0.85f : 0.6f));
        int tw = MeasureText(primaryLabel, 16);
        DrawText(primaryLabel, (int)(L.primary.x + L.primary.width / 2 - tw / 2),
                 (int)(L.primary.y + 12), 16, Color{20, 20, 25, 255});
    }

    const char* closeLabel = (st.stage == Stage::Restart ||
                              st.stage == Stage::OpenedPage ||
                              st.stage == Stage::Managed) ? "Close" : "Later";
    bool chov = CheckCollisionPointRec(mouse, L.secondary);
    DrawRectangleRounded(L.secondary, 0.25f, 8,
                         chov ? Color{255, 255, 255, 30} : Color{255, 255, 255, 14});
    int cw = MeasureText(closeLabel, 16);
    DrawText(closeLabel, (int)(L.secondary.x + L.secondary.width / 2 - cw / 2),
             (int)(L.secondary.y + 12), 16, chov ? WHITE : Color{200, 200, 210, 255});
}

// Returns true when the click was the panel's, so the caller can stop looking
// at the menu underneath it.
bool Game::updatePanelClick(Vector2 mouse) {
    using Stage = GameUpdates::Stage;
    UpdatePanelLayout L = updatePanelLayout(m_screenW, m_screenH);
    GameUpdates::Status st = GameUpdates::get().status();

    if (CheckCollisionPointRec(mouse, L.secondary)) {
        m_updatePanel = false;
        return true;
    }
    if (CheckCollisionPointRec(mouse, L.primary)) {
        if (st.stage == Stage::Available) {
            GameUpdates::get().beginUpdate();
            // macOS hands off to the browser rather than installing; see
            // GameUpdates.h for why.
            if (!GameUpdates::canSelfInstall() && !st.pageUrl.empty())
                OpenURL(st.pageUrl.c_str());
        } else if (st.stage == Stage::Failed && !st.pageUrl.empty()) {
            OpenURL(st.pageUrl.c_str());
        }
        return true;
    }
    // A click anywhere else inside the panel is swallowed, so the menu behind
    // it cannot be operated through it.
    return CheckCollisionPointRec(mouse, L.panel);
}

void Game::updateMainMenu() {
    if (isMouseOverConsole()) return;

    // One check per session, and only ever with the setting on. check() is
    // also a no-op when disabled, so the gate holds even if this call site is
    // ever moved somewhere that forgets to ask.
    if (!m_gameUpdateAsked) {
        m_gameUpdateAsked = true;
        GameUpdates::get().check(m_config.gameUpdateChecks);
    }

    // Ignore input while the intro slide is still playing — the drawn
    // positions are offset mid-animation, so a click would otherwise land on
    // a button that isn't visually there.
    if (m_menuIntro < 1.0f) return;

    // The panel is modal: while it is open it takes the input, so Enter does
    // not start a game behind it and Escape closes the panel rather than
    // quitting the game.
    if (m_updatePanel) {
        if (IsKeyPressed(KEY_ESCAPE)) { m_updatePanel = false; return; }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) updatePanelClick(getMouse());
        return;
    }

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
        GameUpdates::get().updateAvailable() &&
        CheckCollisionPointRec(getMouse(), updateBadgeRect())) {
        m_updatePanel = true;
        return;
    }

    // A prompt owns the keyboard while it is up: the menu must not also act on
    // the Enter that dismisses it, or on the letters being typed into a
    // filename. Everything below this point is skipped until it closes.
    if (m_odStateMsgTimer > 0.0f) m_odStateMsgTimer -= GetFrameTime();
#ifdef __EMSCRIPTEN__
    pollOdStateImport();
#endif
    if (m_odStatePrompt != ODP_NONE) { updateOdStatePrompt(); return; }

    int count = MAIN_MENU_COUNT;

    if (m_menuFeedbackTimer > 0) {
        m_menuFeedbackTimer -= GetFrameTime();
        if (m_menuFeedbackTimer < 0) m_menuFeedbackTimer = 0;
    }

    if (m_saveFeedbackTimer > 0) {
        m_saveFeedbackTimer -= GetFrameTime();
        if (m_saveFeedbackTimer < 0) m_saveFeedbackTimer = 0;
    }

    // ── Background scrolling (moved to updateMenuBackground, called from run()) ──

    // The d-pad walks the list the arrow keys already walk -- the menu half of
    // the hybrid scheme. The stick is left out on purpose: it drives the
    // cursor, and a stick that moved a selection AND a pointer would fight
    // itself on a screen that has both.
    if (IsKeyPressed(KEY_UP) || odPad::navUp()) { m_menuIndex = (m_menuIndex + count - 1) % count; Audio::get().playSfx("hover"); }
    if (IsKeyPressed(KEY_DOWN) || odPad::navDown()) { m_menuIndex = (m_menuIndex + 1) % count; Audio::get().playSfx("hover"); }
    if constexpr (kCanQuitToDesktop) {
        if (IsKeyPressed(KEY_ESCAPE)) { m_running = false; return; }
    }

    Vector2 mouse = getMouse();
    int hovered = -1;
    int itemH = 50;
    int startY = m_screenH / 2 - (count * itemH) / 2 + 30;
    int centerX = m_screenW / 2;

    for (int i = 0; i < count; ++i) {
        int y = startY + i * itemH;
        int tw = MeasureText(MAIN_MENU_ITEMS[i], 30);
        if (CheckCollisionPointRec(mouse, { (float)(centerX - tw/2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) }))
            { hovered = i; break; }
    }

    // On entering a row, not while sitting in it. m_lastMenuHover is what makes
    // this one sound per row rather than one per frame the pointer rests there.
    if (hovered != m_lastMenuHover) {
        if (hovered >= 0) Audio::get().playSfx("hover");
        m_lastMenuHover = hovered;
    }

    bool activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || odPad::navActivate();
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && hovered >= 0) {
        m_menuIndex = hovered;
        activate = true;
    }
    if (activate) Audio::get().playSfx("click_heavy");

    // Check top-right icon clicks. The quit icon is not drawn on the web, so
    // nothing here may answer for where it would have been -- an invisible
    // hitbox in the corner is worse than the button was.
    bool gearHover = CheckCollisionPointRec(mouse, menuGearIconRect(m_screenW, 0));
    bool quitHover = kCanQuitToDesktop &&
                     CheckCollisionPointRec(mouse, menuQuitIconRect(m_screenW, 0));

    if (activate) {
        m_menuIndex = std::clamp(m_menuIndex, 0, count - 1);
        switch (m_menuIndex) {
            case 0: // Play Singleplayer
                m_menuIndex = 0;
                m_currentScreen = SCREEN_SINGLEPLAYER;
                break;
            case 1: // Play Multiplayer
                openMultiplayerMenu();
                break;
            case 2: // Map Editor
                if (!m_mapEditor) {
                    m_mapEditor = new MapEditor();
                    // Loads the map synchronously, right here on the main
                    // thread -- it does not go through the loading screen, so
                    // nothing else refills the music for however long it takes.
                    // Same starvation the world loader had; same fix.
                    Audio::get().beginBackgroundPump();
                    m_mapEditor->init(m_screenW, m_screenH, m_dataDir);
                    Audio::get().endBackgroundPump();
                }
                m_currentScreen = SCREEN_MAP_EDITOR;
                break;
            case 3: // Mod Menu
                m_modIndex = 0;
                m_modScroll = 0;
                m_modAdvancedFor = m_modDeleteFor = m_modAiWarnFor = -1;
                ModManager::get().rescan();
                m_currentScreen = SCREEN_MODS;
                Audio::get().playSfx("click_light");
                break;
            case 4: // Community
                m_currentScreen = SCREEN_COMMUNITY;
                break;
            case 5: // Account
                openAccountMenu();
                break;
            case 6: // Credits
                if (!m_creditsLoaded) loadCredits();
                m_creditsScroll = 0.0f;
                m_currentScreen = SCREEN_CREDITS;
                break;
            case 7: // Save .odstate
                openOdStateSave();
                break;
            case 8: // Load .odstate
                openOdStateLoad();
                break;
        }
    }

    // Top-right icon actions (mouse-only)
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (gearHover) {
            m_inSettings = true;
            Audio::get().playSfx("click_light");
            m_settingsIndex = 0;
            m_settingsScroll = 0;
        }
        if (quitHover) {
            m_running = false;
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// .odstate — the player's whole data/ out of the game and back into it
// ────────────────────────────────────────────────────────────────────────────

void Game::setOdStateMsg(const std::string& msg, bool bad) {
    m_odStateMsg = msg;
    m_odStateMsgBad = bad;
    m_odStateMsgTimer = bad ? 9.0f : 6.0f;   // a failure is worth reading twice
}

void Game::openOdStateSave() {
    // The name is asked for rather than assumed. A player exporting three times
    // in an evening wants to tell them apart, and only they know what by.
    m_odStateName = OdState::suggestedFilename();
    m_odStatePrompt = ODP_SAVE_NAME;
    Audio::get().playSfx("click_light");
}

void Game::openOdStateLoad() {
#ifdef __EMSCRIPTEN__
    // The browser's own picker, opened from inside this click. A file dialog
    // raised outside a user gesture is blocked, so this cannot be deferred to a
    // later frame the way the desktop list can.
    OdState::webPickFile();
    setOdStateMsg("Choose a .odstate file to restore...", false);
    m_odStateMsgTimer = 20.0f;   // the player is off in a file dialog
#else
    // Desktop has no native file dialog here, and adding one would mean a
    // platform toolkit per platform. The game already lists its own files
    // everywhere else, so it lists these too.
    m_odStateFiles = OdState::findArchives(m_dataDir);
    m_odStatePick = 0;
    if (m_odStateFiles.empty()) {
        setOdStateMsg("No .odstate files in " + OdState::defaultSaveDir(m_dataDir), true);
        return;
    }
    m_odStatePrompt = ODP_PICK_FILE;
    Audio::get().playSfx("click_light");
#endif
}

void Game::applyOdStateLoad(const std::string& path, bool modsAccepted) {
    // Asked before unpacking, because after unpacking the mods are already in.
    if (!modsAccepted) {
        int mods = OdState::countMods(path);
        if (mods > 0) {
            m_odStatePending = path;
            m_odStatePendingMods = mods;
            m_odStatePrompt = ODP_MODS_WARNING;
            return;
        }
        if (mods < 0) { setOdStateMsg("That file is not a readable .odstate archive", true); return; }
    }

    std::string err;
    int n = 0;
    if (OdState::load(m_dataDir, path, err, &n)) {
        // config.json is read once during init(), so a restored one changes
        // nothing until it is read again -- the player would otherwise restore
        // their state and find their settings still at the defaults.
        m_config.load(m_configPath);
        // Same for mods: ModManager holds the list it scanned at startup, and a
        // restored mods/ that nothing rescans is a directory of files the game
        // does not know are there.
        ModManager::get().rescan();
        setOdStateMsg(err.empty() ? ("Restored " + std::to_string(n) + " files") : err,
                      !err.empty());
        Audio::get().playSfx("click_light");
    } else {
        setOdStateMsg(err, true);
    }
}

#ifdef __EMSCRIPTEN__
void Game::pollOdStateImport() {
    std::string incoming;
    if (!OdState::webTakeImport(incoming)) return;
    applyOdStateLoad(incoming, false);
    // Left in MEMFS while a mods warning is up: the answer still needs the file.
    if (m_odStatePrompt != ODP_MODS_WARNING) std::remove(incoming.c_str());
}
#endif

void Game::updateOdStatePrompt() {
    if (m_odStatePrompt == ODP_NONE) return;

    if (IsKeyPressed(KEY_ESCAPE)) {
        m_odStatePrompt = ODP_NONE;
        m_odStatePending.clear();
        Audio::get().playSfx("click_light");
        return;
    }

    if (m_odStatePrompt == ODP_SAVE_NAME) {
        int c = GetCharPressed();
        while (c > 0) {
            // A filename, not a path. A separator typed here would write
            // somewhere other than the directory the player was just shown.
            if (c >= 32 && c <= 126 && c != '/' && c != '\\' && c != ':' &&
                m_odStateName.size() < 64) {
                Audio::get().playSfx("key_type", 0.12f);
                m_odStateName += (char)c;
            }
            c = GetCharPressed();
        }
        odTextEditKeys(m_odStateName, 64, "/\\:");

        if (IsKeyPressed(KEY_ENTER)) {
            std::string name = m_odStateName;
            if (name.empty()) name = OdState::suggestedFilename();
            const std::string ext = ".odstate";
            if (name.size() < ext.size() ||
                name.compare(name.size() - ext.size(), ext.size(), ext) != 0)
                name += ext;

            std::string err;
            int n = 0;
#ifdef __EMSCRIPTEN__
            // Written inside MEMFS and handed to the browser second; nothing
            // here reaches the player's disk directly.
            const std::string tmp = "/odstate_out.odstate";
            if (OdState::save(m_dataDir, tmp, err, &n)) {
                OdState::webDownload(tmp, name);
                std::remove(tmp.c_str());
                setOdStateMsg("Downloaded " + name + "  (" + std::to_string(n) + " files)", false);
            } else {
                setOdStateMsg(err, true);
            }
#else
            std::string dest = OdState::defaultSaveDir(m_dataDir) + "/" + name;
            if (OdState::save(m_dataDir, dest, err, &n))
                setOdStateMsg("Saved " + dest + "  (" + std::to_string(n) + " files)", false);
            else
                setOdStateMsg(err, true);
#endif
            m_odStatePrompt = ODP_NONE;
        }
        return;
    }

    if (m_odStatePrompt == ODP_PICK_FILE) {
        int count = (int)m_odStateFiles.size();
        if (count == 0) { m_odStatePrompt = ODP_NONE; return; }
        if (IsKeyPressed(KEY_UP))   { m_odStatePick = (m_odStatePick + count - 1) % count; Audio::get().playSfx("hover"); }
        if (IsKeyPressed(KEY_DOWN)) { m_odStatePick = (m_odStatePick + 1) % count;         Audio::get().playSfx("hover"); }
        if (IsKeyPressed(KEY_ENTER)) {
            std::string chosen = m_odStateFiles[m_odStatePick];
            m_odStatePrompt = ODP_NONE;
            applyOdStateLoad(chosen, false);   // may raise the mods warning
        }
        return;
    }

    if (m_odStatePrompt == ODP_MODS_WARNING) {
        // Enter is deliberately NOT the accept key. This prompt appears at the
        // end of a flow driven by Enter, and a player still pressing it would
        // accept executable content without having read anything.
        if (IsKeyPressed(KEY_Y)) {
            std::string path = m_odStatePending;
            m_odStatePending.clear();
            m_odStatePrompt = ODP_NONE;
            applyOdStateLoad(path, true);
#ifdef __EMSCRIPTEN__
            std::remove(path.c_str());
#endif
        } else if (IsKeyPressed(KEY_N)) {
            m_odStatePrompt = ODP_NONE;
            m_odStatePending.clear();
            setOdStateMsg("Restore cancelled", false);
        }
        return;
    }
}

void Game::drawOdStatePrompt() {
    if (m_odStatePrompt == ODP_NONE) return;

    // Over everything, and dark enough that the menu behind is plainly not what
    // the keyboard is talking to.
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 190});
    int boxW = 620, boxH = 190;
    if (m_odStatePrompt == ODP_PICK_FILE)
        boxH = 130 + std::min((int)m_odStateFiles.size(), 8) * 26;
    int boxX = m_screenW / 2 - boxW / 2;
    int boxY = m_screenH / 2 - boxH / 2;
    Color accent = hexToColor(m_config.accent());
    DrawRectangleRounded({(float)boxX, (float)boxY, (float)boxW, (float)boxH}, 0.05f, 8, {18, 18, 22, 245});
    DrawRectangleRoundedLines({(float)boxX, (float)boxY, (float)boxW, (float)boxH}, 0.05f, 8, accent);

    int pad = 22;
    int y = boxY + pad;

    if (m_odStatePrompt == ODP_SAVE_NAME) {
        DrawText("Save your progress as", boxX + pad, y, 22, accent);
        y += 34;
#ifdef __EMSCRIPTEN__
        const char* where = "Your browser will download it. Keep it safe -- closing this tab loses everything else.";
#else
        static std::string whereBuf;
        whereBuf = "Written to " + OdState::defaultSaveDir(m_dataDir);
        const char* where = whereBuf.c_str();
#endif
        DrawText(where, boxX + pad, y, 14, Color{150, 150, 160, 255});
        y += 30;

        Rectangle field = {(float)(boxX + pad), (float)y, (float)(boxW - pad * 2), 34.0f};
        DrawRectangleRounded(field, 0.2f, 6, {30, 30, 36, 255});
        DrawRectangleRoundedLines(field, 0.2f, 6, Color{90, 90, 100, 255});
        std::string shown = m_odStateName;
        if (((int)(GetTime() * 2)) % 2 == 0) shown += "_";      // caret
        DrawText(shown.c_str(), boxX + pad + 8, y + 8, 18, WHITE);
        y += 46;
        DrawText("Enter to save     Esc to cancel", boxX + pad, y, 14, Color{140, 140, 150, 255});
        return;
    }

    if (m_odStatePrompt == ODP_PICK_FILE) {
        DrawText("Restore which .odstate?", boxX + pad, y, 22, accent);
        y += 36;
        int shown = std::min((int)m_odStateFiles.size(), 8);
        int first = std::max(0, std::min(m_odStatePick - shown + 1, (int)m_odStateFiles.size() - shown));
        for (int i = 0; i < shown; ++i) {
            int idx = first + i;
            bool sel = (idx == m_odStatePick);
            std::string base = m_odStateFiles[idx];
            auto slash = base.find_last_of('/');
            if (slash != std::string::npos) base = base.substr(slash + 1);
            if (sel) DrawRectangle(boxX + pad - 6, y - 3, boxW - pad * 2 + 12, 24, {255, 255, 255, 18});
            DrawText(base.c_str(), boxX + pad, y, 17, sel ? WHITE : Color{160, 160, 170, 255});
            y += 26;
        }
        y += 8;
        DrawText("Enter to restore     Esc to cancel", boxX + pad, y, 14, Color{140, 140, 150, 255});
        return;
    }

    if (m_odStatePrompt == ODP_MODS_WARNING) {
        DrawText("This archive contains mods", boxX + pad, y, 22, Color{240, 190, 90, 255});
        y += 34;
        DrawText(TextFormat("%d installed mod%s will be restored along with your saves.",
                            m_odStatePendingMods, m_odStatePendingMods == 1 ? "" : "s"),
                 boxX + pad, y, 16, Color{215, 215, 225, 255});
        y += 24;
        DrawText("Mods run code inside the game. Restore them only from an archive you trust.",
                 boxX + pad, y, 15, Color{190, 190, 200, 255});
        y += 34;
        DrawText("Y  restore everything        N  cancel", boxX + pad, y, 16, WHITE);
        return;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// drawSingleplayerMenu
// ────────────────────────────────────────────────────────────────────────────
void Game::drawSingleplayerMenu() {
    int centerX = m_screenW / 2;
    int fontSize = 30;
    int itemH = 50;

    drawMenuBackground();
    DrawRectangleGradientV(0, 0, m_screenW, 200, {0, 0, 0, 200}, {0, 0, 0, 0});

    // Title
    const char* title = "Play Singleplayer";
    int titleSize = 50;
    int titleW = MeasureText(title, titleSize);
    DrawText(title, centerX - titleW / 2, 120, titleSize, hexToColor(m_config.accent()));

    // Buttons
    int count = SINGLEPLAYER_COUNT;
    int startY = m_screenH / 2 - (count * itemH) / 2 + 30;

    Vector2 mouse = getMouse();
    int hovered = -1;
    for (int i = 0; i < count; ++i) {
        int y = startY + i * itemH;
        int tw = MeasureText(SINGLEPLAYER_ITEMS[i], fontSize);
        Rectangle rect = { (float)(centerX - tw / 2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) };
        if (CheckCollisionPointRec(mouse, rect)) { hovered = i; break; }
    }

    for (int i = 0; i < count; ++i) {
        int y = startY + i * itemH;
        bool isSelected = (i == m_menuIndex);
        bool isHovered = (i == hovered);
        Color textColor = isSelected ? hexToColor(m_config.accent()) : (isHovered ? WHITE : LIGHTGRAY);
        Color bgColor = isHovered ? Color{255, 255, 255, 16} : BLANK;

        int tw = MeasureText(SINGLEPLAYER_ITEMS[i], fontSize);
        Rectangle rect = { (float)(centerX - tw / 2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) };
        DrawRectangleRounded(rect, 0.1f, 8, bgColor);
        DrawText(SINGLEPLAYER_ITEMS[i], centerX - tw / 2, y, fontSize, textColor);

        if (isSelected) {
            int underlineW = tw + 20;
            DrawRectangle(centerX - underlineW / 2, y + fontSize + 4, underlineW, 2, hexToColor(m_config.accent()));
        }
    }

    // Back button
    int backY = m_screenH - 80;
    const char* backLabel = "Back";
    int backW = MeasureText(backLabel, 22);
    bool backHovered = CheckCollisionPointRec(mouse, { (float)(centerX - backW/2 - 15), (float)(backY - 5), (float)(backW + 30), 34 });
    Color backColor = backHovered ? WHITE : Color{160, 160, 170, 255};
    DrawText(backLabel, centerX - backW / 2, backY, 22, backColor);
}

// ────────────────────────────────────────────────────────────────────────────
// updateSingleplayerMenu
// ────────────────────────────────────────────────────────────────────────────
void Game::updateSingleplayerMenu() {
    if (isMouseOverConsole()) return;
    int count = SINGLEPLAYER_COUNT;

    // This submenu is hand-rolled rather than drawn through drawMenuList, which
    // is why it was the one menu in the game that answered to nothing.
    if (IsKeyPressed(KEY_UP))   { m_menuIndex = (m_menuIndex + count - 1) % count; Audio::get().playSfx("hover"); }
    if (IsKeyPressed(KEY_DOWN)) { m_menuIndex = (m_menuIndex + 1) % count;         Audio::get().playSfx("hover"); }
    if (IsKeyPressed(KEY_ESCAPE)) { Audio::get().playSfx("back"); m_currentScreen = SCREEN_MENU; return; }

    Vector2 mouse = getMouse();
    int centerX = m_screenW / 2;
    int itemH = 50;
    int startY = m_screenH / 2 - (count * itemH) / 2 + 30;
    int hovered = -1;

    for (int i = 0; i < count; ++i) {
        int y = startY + i * itemH;
        int tw = MeasureText(SINGLEPLAYER_ITEMS[i], 30);
        if (CheckCollisionPointRec(mouse, { (float)(centerX - tw/2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) }))
            { hovered = i; break; }
    }

    // Back button
    int backY = m_screenH - 80;
    const char* backLabel = "Back";
    int backW = MeasureText(backLabel, 22);
    bool backHovered = CheckCollisionPointRec(mouse, { (float)(centerX - backW/2 - 15), (float)(backY - 5), (float)(backW + 30), 34 });

    // On entering a row, not while sitting in it.
    const int hoverNow = backHovered ? 1000 : hovered;
    if (hoverNow != m_lastMenuHover) {
        if (hoverNow >= 0) Audio::get().playSfx("hover");
        m_lastMenuHover = hoverNow;
    }

    bool activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (backHovered) {
            Audio::get().playSfx("back");
            m_currentScreen = SCREEN_MENU;
            return;
        }
        if (hovered >= 0) {
            m_menuIndex = hovered;
            activate = true;
        }
    }
    if (activate) Audio::get().playSfx("click_heavy");

    if (activate) {
        m_menuIndex = std::clamp(m_menuIndex, 0, count - 1);
        switch (m_menuIndex) {
            case 0: // New World
                loadMapEntries();
                m_mapTabIndex = 0;
                m_mapIndex = 0;
                m_mapScroll = 0;
                m_currentScreen = SCREEN_MAP_SELECT;
                break;
            case 1: // Load World
                scanDirectory(m_dataDir + "saves", ".odsv", m_fileItems);
                if (m_fileItems.empty()) {
                    m_menuFeedback = "No worlds found in data/saves/. Start a new game and save first!";
                    m_menuFeedbackTimer = 3.0f;
                } else {
                    m_worldInfos.clear();
                    for (auto& fn : m_fileItems) {
                        std::string path = m_dataDir + "saves/" + fn;
                        auto meta = SaveManager::readMetadata(path);
                        SaveWorldInfo wi;
                        wi.filename = fn;
                        wi.worldName = meta.saveName.empty() ? fn : meta.saveName;
                        wi.version = meta.version.empty() ? "unknown" : meta.version;
                        wi.lastPlayed = meta.lastPlayed.empty() ? "unknown" : meta.lastPlayed;
                        wi.turnCount = meta.turnCount;
                        m_worldInfos.push_back(wi);
                    }
                    m_browsingSaves = true;
                    m_fileIndex = 0;
                    m_fileScroll = 0;
                    m_showDeleteConfirm = false;
                    m_showWorldSettings = false;
                    m_currentScreen = SCREEN_FILE_BROWSER;
                }
                break;
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// drawCountrySelect
// ────────────────────────────────────────────────────────────────────────────
void Game::drawCountrySelect() {
    if (!m_renderer) return;
    // Render the political map as the background
    m_renderer->draw(m_landSea, m_provinces, m_countries);

    // Top gradient for title readability
    DrawRectangleGradientV(0, 0, m_screenW, 110, {0, 0, 0, 200}, {0, 0, 0, 0});

    int centerX = m_screenW / 2;

    // Title
    DrawText("SELECT YOUR COUNTRY", centerX - MeasureText("SELECT YOUR COUNTRY", 40) / 2, 15, 40, hexToColor(m_config.accent()));
    DrawText("Click on a country on the map to play, or use the button below",
             centerX - MeasureText("Click on a country on the map to play, or use the button below", 18) / 2, 60, 18, LIGHTGRAY);

    // "Continue as Spectator" button (bottom-right)
    const char* spectateText = "Continue as Spectator";
    int specBtnW = MeasureText(spectateText, 20) + 30;
    int specBtnH = 40;
    int specBtnX = m_screenW - specBtnW - 20;
    int specBtnY = m_screenH - specBtnH - 20;
    DrawRectangleRounded({(float)specBtnX, (float)specBtnY, (float)specBtnW, (float)specBtnH}, 0.3f, 8, {60, 60, 80, 200});
    DrawRectangleRoundedLines({(float)specBtnX, (float)specBtnY, (float)specBtnW, (float)specBtnH}, 0.3f, 8, {120, 120, 140, 200});
    int specTextW = MeasureText(spectateText, 20);
    DrawText(spectateText, specBtnX + (specBtnW - specTextW) / 2, specBtnY + (specBtnH - 20) / 2, 20, WHITE);

    // Confirmation popup
    if (m_pendingCountryId > 0) {
        const Country* c = m_countries.getCountry(m_pendingCountryId);
        if (c) {
            const CountryConfirmLayout L = countryConfirmLayout();

            // Dim background
            DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 120});

            DrawRectangleRounded(L.box, 0.2f, 8, {30, 30, 50, 255});
            DrawRectangleRoundedLines(L.box, 0.2f, 8, {100, 100, 160, 255});

            std::string question = "Play as " + c->name + "?";
            DrawText(question.c_str(),
                     (int)(L.box.x + (L.box.width - MeasureText(question.c_str(), 24)) / 2),
                     (int)L.questionY, 24, WHITE);

            // The flag keeps its own aspect ratio inside L.flag rather than
            // being stretched to fill it: a 3:2 ensign and a 2:1 banner are not
            // the same picture, and one of them was coming out wrong.
            auto fit = m_countryFlags.find(m_pendingCountryId);
            if (fit != m_countryFlags.end() && fit->second.id > 0 && fit->second.height > 0) {
                float scale = L.flag.height / (float)fit->second.height;
                float drawW = fit->second.width * scale;
                if (drawW > L.flag.width) {          // very wide ensign: fit by width instead
                    scale = L.flag.width / (float)fit->second.width;
                    drawW = L.flag.width;
                }
                const float drawH = fit->second.height * scale;
                DrawTexturePro(fit->second,
                    {0, 0, (float)fit->second.width, (float)fit->second.height},
                    {L.flag.x + (L.flag.width - drawW) / 2,
                     L.flag.y + (L.flag.height - drawH) / 2, drawW, drawH},
                    {0, 0}, 0, WHITE);
            } else {
                DrawRectangle((int)(L.flag.x + L.flag.width / 4), (int)L.flag.y,
                              (int)(L.flag.width / 2), (int)L.flag.height, c->color);
            }

            DrawRectangleRounded(L.yes, 0.3f, 6, {50, 180, 70, 255});
            DrawText("Yes", (int)(L.yes.x + (L.yes.width - MeasureText("Yes", 20)) / 2),
                     (int)(L.yes.y + (L.yes.height - 20) / 2), 20, WHITE);

            DrawRectangleRounded(L.no, 0.3f, 6, {200, 60, 60, 255});
            DrawText("No", (int)(L.no.x + (L.no.width - MeasureText("No", 20)) / 2),
                     (int)(L.no.y + (L.no.height - 20) / 2), 20, WHITE);
        }
    }

    // ESC hint
    const char* escHint = "ESC: back to menu";
    DrawText(escHint, centerX - MeasureText(escHint, 14) / 2, m_screenH - 8, 14, GRAY);
}

// ────────────────────────────────────────────────────────────────────────────
// countryConfirmLayout
// ────────────────────────────────────────────────────────────────────────────
Game::CountryConfirmLayout Game::countryConfirmLayout() const {
    // The box was 150 tall and could not hold what was put in it. The flag was
    // drawn at +60 with a height of 60, so it ended at +120; the buttons sat at
    // popH-35-20 = +95. Twenty-five pixels of overlap, which is why the flag
    // appeared to be behind Yes and No and looked enormous -- it was not
    // oversized so much as sharing its space with the buttons.
    constexpr float kW = 400, kH = 200;
    constexpr float kBtnW = 90, kBtnH = 38;

    CountryConfirmLayout L;
    L.box = {(float)((m_screenW - (int)kW) / 2), (float)((m_screenH - (int)kH) / 2), kW, kH};
    L.questionY = L.box.y + 24;

    // Between the question and the buttons, with room to spare on both sides.
    //
    // A confirmation, not a flag viewer. 66 tall filled most of the box and
    // read as the subject of the dialog rather than a label on it -- the
    // question is what the player is answering. 44 is enough to recognise a
    // flag by and leaves the box breathing room.
    L.flag = {L.box.x + 70, L.box.y + 60, kW - 140, 44};

    const float btnY = L.box.y + kH - kBtnH - 22;
    L.yes = {L.box.x + (kW / 2 - kBtnW) / 2, btnY, kBtnW, kBtnH};
    L.no  = {L.box.x + kW / 2 + (kW / 2 - kBtnW) / 2, btnY, kBtnW, kBtnH};
    return L;
}

// ────────────────────────────────────────────────────────────────────────────
// updateCountrySelect
// ────────────────────────────────────────────────────────────────────────────
void Game::updateCountrySelect() {
    if (isMouseOverConsole()) return;
    if (m_playableCountryIds.empty()) {
        if (IsKeyPressed(KEY_ESCAPE)) { m_popupQueue.clear(); unloadGameData(); m_currentScreen = SCREEN_MENU; }
        return;
    }

    // Update renderer for fly-to animation
    m_renderer->update(GetFrameTime());

    Vector2 mouse = getMouse();
    int mx = (int)mouse.x;
    int my = (int)mouse.y;

    // Spectator button rect (must match drawCountrySelect)
    const char* spectateText = "Continue as Spectator";
    int specBtnW = MeasureText(spectateText, 20) + 30;
    int specBtnH = 40;
    Rectangle specBtnRect = {
        (float)(m_screenW - specBtnW - 20),
        (float)(m_screenH - specBtnH - 20),
        (float)specBtnW,
        (float)specBtnH
    };

    // From the one layout the popup is drawn with, so these cannot drift apart.
    const CountryConfirmLayout confirm = countryConfirmLayout();
    const Rectangle yesRect = confirm.yes;
    const Rectangle noRect  = confirm.no;

    // --- Hover detection ---
    if (m_pendingCountryId == 0) {
        int px, py;
        m_renderer->screenToPixel((float)mx, (float)my, px, py);
        int hoveredProv = 0;
        if (px >= 0 && px < m_landSea.getWidth() && py >= 0 && py < m_landSea.getHeight()) {
            const Province* prov = m_provinces.getProvince(px, py);
            if (prov) {
                // Check if this province's country is playable
                int cid = prov->countryId;
                if (cid != UNC_CID && cid != BLC_CID) {
                    auto it = std::find(m_playableCountryIds.begin(), m_playableCountryIds.end(), cid);
                    if (it != m_playableCountryIds.end()) {
                        hoveredProv = prov->id;
                    }
                }
            }
        }
        // Update selection glow only when hovered province changes (skip during fly-to lock)
        if (m_flyToLockTimer > 0) {
            m_flyToLockTimer--;
        } else if (hoveredProv != m_renderer->getSelectedProvinceId()) {
            m_renderer->setSelectedProvince(hoveredProv);
            m_renderer->rebuildSelectionGlow();
        }
    }

    // --- Click detection (only when no popup is active) ---
    if (m_pendingCountryId == 0) {
        // Left click on map → detect country
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !m_renderer->getWasDragged()) {
            // Check if we clicked on the spectator button
            if (CheckCollisionPointRec(mouse, specBtnRect)) {
                // Spectator mode
                m_playerCountryId = SPC_CID;
                // Sync per-country research
                for (auto& n : m_researchNodes)
                    n.researched = false;
                m_pendingCountryId = 0;
                std::cout << "  Player selected: SPECTATOR" << std::endl;
                if (!m_currentSavePath.empty()) {
                    SaveManager::updatePlayerCountry(m_currentSavePath, SPC_CID);
                }
                m_dpiScale = GetWindowScaleDPI().x;
                m_screenW = GetScreenWidth();
                m_screenH = GetScreenHeight();
                if (m_renderer) {
                    m_renderer->resize(m_screenW, m_screenH);
                    m_renderer->setDpiScale(m_dpiScale);
                    m_renderer->setBlockLeftPan(false);
                }
                m_currentScreen = SCREEN_PLAYING;
                return;
            }
            // Check if we clicked on the map (not on UI elements)
            bool onUI = CheckCollisionPointRec(mouse, {0, 0, (float)m_screenW, 110}) ||
                        CheckCollisionPointRec(mouse, specBtnRect);
            if (!onUI) {
                int px, py;
                m_renderer->screenToPixel((float)mx, (float)my, px, py);
                if (px >= 0 && px < m_landSea.getWidth() && py >= 0 && py < m_landSea.getHeight()) {
                    const Province* prov = m_provinces.getProvince(px, py);
                    if (prov) {
                        int cid = prov->countryId;
                        auto it = std::find(m_playableCountryIds.begin(), m_playableCountryIds.end(), cid);
                        if (it != m_playableCountryIds.end()) {
                            m_pendingCountryId = cid;
                        }
                    }
                }
            }
        }

        // ESC to go back to menu
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_renderer->setBlockLeftPan(false);
            m_renderer->setShowCountryNames(false);
            m_pendingCountryId = 0;
            unloadGameData();
            m_currentScreen = SCREEN_MENU;
        }
    } else {
        // --- Confirmation popup active ---
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (CheckCollisionPointRec(mouse, yesRect)) {
                // Confirm: play as this country
                m_playerCountryId = m_pendingCountryId;
                const Country* pc = m_countries.getCountry(m_playerCountryId);
                std::cerr << "[DIAG] Player confirmed country " << m_playerCountryId
                          << " (" << (pc ? pc->isoA3 : "?") << ")" << std::endl;
                auto compassIt = m_countryCompass.find(m_playerCountryId);
                if (compassIt != m_countryCompass.end()) {
                    std::cerr << "[DIAG]   Compass: econ=" << compassIt->second.economic
                              << " soc=" << compassIt->second.social << std::endl;
                } else {
                    std::cerr << "[DIAG]   NO COMPASS ENTRY!" << std::endl;
                }
                // Auto-unlock techs matching built industry/fort/port levels
                {
                    int maxInd = 0, maxFort = 0, maxPort = 0;
                    for (auto& [pid, p] : m_provinces.getAllProvinces()) {
                        if (p.countryId != m_playerCountryId) continue;
                        auto it = m_provinceIndustry.find(pid);
                        if (it != m_provinceIndustry.end()) {
                            if (it->second.level > maxInd) maxInd = it->second.level;
                            if (it->second.fortification > maxFort) maxFort = it->second.fortification;
                        }
                        auto pt = m_provincePorts.find(pid);
                        if (pt != m_provincePorts.end() && pt->second.level > maxPort)
                            maxPort = pt->second.level;
                    }
                    std::function<void(const std::string&)> unlockRec;
                    unlockRec = [&](const std::string& nodeId) {
                        for (auto& n : m_researchNodes) {
                            if (n.id == nodeId) {
                                if (m_countryResearched[m_playerCountryId].count(n.id)) return;
                                // Don't unlock if a mutex sibling is already researched
                                if (n.mutexGroup > 0) {
                                    for (auto& sib : m_researchNodes) {
                                        if (sib.id != nodeId && sib.mutexGroup == n.mutexGroup && m_countryResearched[m_playerCountryId].count(sib.id))
                                            return;
                                    }
                                }
                                m_countryResearched[m_playerCountryId].insert(n.id);
                                for (auto& dep : n.deps)
                                    unlockRec(dep);
                                break;
                            }
                        }
                    };
                    for (auto& n : m_researchNodes) {
                        if (n.industryLevel > 0 && n.industryLevel <= maxInd)
                            unlockRec(n.id);
                        if (n.fortLevel > 0 && n.fortLevel <= maxFort)
                            unlockRec(n.id);
                        if (n.portLevel > 0 && n.portLevel <= maxPort)
                            unlockRec(n.id);
                    }
                    bool hasCarrier = false;
                    for (auto& ship : m_ships) {
                        if (ship.countryId == m_playerCountryId) {
                            m_countryResearched[m_playerCountryId].insert("navy1");
                            if (ship.type == "carrier") hasCarrier = true;
                        }
                    }
                    if (hasCarrier) {
                        m_countryResearched[m_playerCountryId].insert("arty1");
                    }
                }
                // Sync per-country research into global research nodes
                for (auto& n : m_researchNodes)
                    n.researched = hasResearched(n.id, m_playerCountryId);
                auto c = m_countries.getCountry(m_pendingCountryId);
                std::cout << "  Player selected: " << (c ? c->name : "ID " + std::to_string(m_pendingCountryId)) << std::endl;
                if (!m_currentSavePath.empty()) {
                    SaveManager::updatePlayerCountry(m_currentSavePath, m_playerCountryId);
                }
                m_pendingCountryId = 0;
                m_dpiScale = GetWindowScaleDPI().x;
                m_screenW = GetScreenWidth();
                m_screenH = GetScreenHeight();
                if (m_renderer) {
                    m_renderer->resize(m_screenW, m_screenH);
                    m_renderer->setDpiScale(m_dpiScale);
                    m_renderer->setBlockLeftPan(false);
                    m_renderer->setSelectedProvince(0);
                    m_renderer->rebuildSelectionGlow();
                    m_renderer->setShowCountryNames(false);
                }
                m_currentScreen = SCREEN_PLAYING;
            } else if (CheckCollisionPointRec(mouse, noRect)) {
                // Cancel
                m_pendingCountryId = 0;
            }
        }

        // ESC also cancels the popup
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_pendingCountryId = 0;
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// drawMenuList
// ────────────────────────────────────────────────────────────────────────────
void Game::drawMenuList(const std::vector<std::string>& items, int selectedIndex) {
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 160});

    int centerX = m_screenW / 2;
    int centerY = m_screenH / 2;
    int itemH = 50;
    int totalH = (int)items.size() * itemH;
    int startY = centerY - totalH / 2;
    int fontSize = 30;

    Vector2 mouse = getMouse();
    int hovered = -1;
    for (int i = 0; i < (int)items.size(); ++i) {
        int y = startY + i * itemH;
        int textW = MeasureText(items[i].c_str(), fontSize);
        Rectangle rect = { (float)(centerX - textW / 2 - 20), (float)(y - 5), (float)(textW + 40), (float)(itemH - 10) };
        if (CheckCollisionPointRec(mouse, rect)) { hovered = i; break; }
    }

    for (int i = 0; i < (int)items.size(); ++i) {
        int y = startY + i * itemH;
        bool isSelected = (i == selectedIndex);
        bool isHovered = (i == hovered);
        Color textColor = isSelected ? hexToColor(m_config.accent()) : (isHovered ? WHITE : LIGHTGRAY);
        Color bgColor = isHovered ? Color{255, 255, 255, 16} : BLANK;

        int textW = MeasureText(items[i].c_str(), fontSize);
        Rectangle rect = { (float)(centerX - textW / 2 - 20), (float)(y - 5), (float)(textW + 40), (float)(itemH - 10) };
        DrawRectangleRounded(rect, 0.1f, 8, bgColor);
        DrawText(items[i].c_str(), centerX - textW / 2, y, fontSize, textColor);

        if (isSelected) {
            int lineW = textW + 20;
            DrawRectangle(centerX - lineW / 2, y + fontSize + 4, lineW, 2, hexToColor(m_config.accent()));
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// drawSettingsFromMenu
// ────────────────────────────────────────────────────────────────────────────
void Game::drawSettingsFromMenu() {
    // Draw the menu background with dimming
    drawMenuBackground(true);
    // Reuse the settings portion of drawPauseMenu by temporarily enabling it
    m_paused = true;
    drawPauseMenu();
}

// ────────────────────────────────────────────────────────────────────────────
// updateSettingsFromMenu
// ────────────────────────────────────────────────────────────────────────────
void Game::updateSettingsFromMenu() {
    if (isMouseOverConsole()) return;
    // ESC to go back to main menu
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (m_editingValue) {
            m_editingValue = false;
        } else if (m_inSettings) {
            m_inSettings = false;
            m_settingsScroll = 0;
            m_settingsIndex = 0;
            m_config.save(m_configPath);
        }
        return;
    }

    // Keybind capture mode
    if (m_waitingForKey) {
        int pressed = GetKeyPressed();
        if (pressed != 0) {
            if (pressed == KEY_ESCAPE) {
                // Escape cancels
            } else if (pressed == KEY_BACKSPACE || pressed == KEY_DELETE) {
                m_config.keybinds[m_rebindingAction] = 0;
                m_config.save(m_configPath);
            } else {
                m_config.keybinds[m_rebindingAction] = pressed;
                m_config.save(m_configPath);
            }
            m_waitingForKey = false;
            m_rebindingAction = -1;
        }
        return;
    }

    // Debug: log keybinds when opening keybinds tab
    bool firstKeybindTab = false;
    // Keybind search input (keybinds tab only, only when active)
    if (m_keybindFilterActive && m_settingsTab == 3 && !m_waitingForKey && !m_editingValue) {
        int c = GetCharPressed();
        while (c > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            if (c >= 32 && c <= 126) m_keybindFilter += (char)c;
            c = GetCharPressed();
        }
        if (odTextEditKeys(m_keybindFilter, 64)) {
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_keybindFilterActive = false;
        }
    }

    // Only process keyboard/mouse when not editing a value
    if (m_editingValue) {
        int c = GetCharPressed();
        while (c > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            if (c >= '0' && c <= '9') m_editBuffer += (char)c;
            else if (c == '.' && m_editBuffer.find('.') == std::string::npos) m_editBuffer += '.';
            c = GetCharPressed();
        }
        odTextEditKeys(m_editBuffer, 256);
        if (IsKeyPressed(KEY_ENTER)) {
            float val = std::strtof(m_editBuffer.c_str(), nullptr);
            if (m_settingsTab == 0 && m_settingsIndex == 2) {
                m_config.maxZoom = std::clamp(val, 1.0f, 50.0f);
            } else if (m_settingsTab == 1 && m_settingsIndex == 0) {
                m_config.flySpeed = std::clamp(val, 0.1f, 10.0f);
            }
            m_editingValue = false;
        }
        if (IsKeyPressed(KEY_ESCAPE)) m_editingValue = false;
        return;
    }

    // Clamp tab
    if (m_settingsTab < 0 || m_settingsTab >= TAB_COUNT) m_settingsTab = 0;
    const Setting* items = TAB_ITEMS[m_settingsTab];
    int count = TAB_ITEM_COUNTS[m_settingsTab];
    int itemH = 80;
    int centerX = m_screenW / 2;
    int tabY = 100;
    int startY = tabY + 70;
    int maxVisible = std::max(1, (m_screenH - startY - 20) / itemH);
    int maxScroll = std::max(0, count - maxVisible);
    m_settingsScroll = std::clamp(m_settingsScroll, 0, maxScroll);

    // Mouse interaction
    Vector2 mouse = getMouse();
    int hovered = -1;
    int resetHovered = -1;

    int tabSpacing = 200;
    int tabStartX = centerX - (TAB_COUNT * tabSpacing) / 2 + tabSpacing / 2;

    // Tab click detection
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        int visibleTabs = 0;
        for (int t = 0; t < TAB_COUNT; ++t) {
            if (t == 4 && !m_config.debugMode) continue;
            ++visibleTabs;
        }
        int tabStartX2 = centerX - (visibleTabs * tabSpacing) / 2 + tabSpacing / 2;
        int tabIdx = 0;
        for (int t = 0; t < TAB_COUNT; ++t) {
            if (t == 4 && !m_config.debugMode) continue;
            int tx = tabStartX2 + tabIdx * tabSpacing;
            int tw = MeasureText(TAB_NAMES[t], 30);
            Rectangle tabRect = { (float)(tx - tw/2), (float)tabY, (float)tw, (float)(30 + 10) };
            if (CheckCollisionPointRec(mouse, tabRect)) {
                m_settingsTab = t;
                m_settingsIndex = 0;
                m_settingsScroll = 0;
                m_keybindFilter.clear();
                m_keybindFilterActive = false;
                items = TAB_ITEMS[m_settingsTab];
                count = TAB_ITEM_COUNTS[m_settingsTab];
                maxScroll = std::max(0, count - maxVisible);
                break;
            }
            ++tabIdx;
        }
    }

    // Scroll wheel
    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        m_settingsScroll -= (int)wheel;
        m_settingsScroll = std::clamp(m_settingsScroll, 0, maxScroll);
    }

    // Search box focus on click
    if (m_settingsTab == 3 && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        int sbY = tabY + 30 + 8;
        int sbW = 300;
        int sbH = 24;
        int sbX = centerX - sbW / 2;
        bool overSearch = CheckCollisionPointRec(mouse, {(float)sbX, (float)sbY, (float)sbW, (float)sbH});
        m_keybindFilterActive = overSearch;
    }

    // Build visible index list for hover detection (compacted)
    std::vector<int> mmVisIdx;
    for (int i = 0; i < count; ++i) {
        bool isHeader = (items[i].actionId < 0 && items[i].label[0] == '-' && items[i].label[1] == '-');
        bool skip = false;
        if (!isHeader && m_settingsTab == 3) {
            for (int si = i - 1; si >= 0; --si) {
                bool siHeader = (items[si].actionId < 0 && items[si].label[0] == '-' && items[si].label[1] == '-');
                if (siHeader) { if (m_collapsedSections.count(si) > 0) skip = true; break; }
            }
            if (!skip && !m_keybindFilter.empty()) {
                std::string lower = items[i].label;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                std::string filter = m_keybindFilter;
                std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
                if (lower.find(filter) == std::string::npos) {
                    if (items[i].actionId >= 0) {
                        std::string kn = keyName(m_config.keybinds[items[i].actionId]);
                        std::transform(kn.begin(), kn.end(), kn.begin(), ::tolower);
                        if (kn.find(filter) == std::string::npos) skip = true;
                    } else skip = true;
                }
            }
        }
        if (!skip) mmVisIdx.push_back(i);
    }

    // Item hover and reset button detection (compacted)
    int mmVisCount = (int)mmVisIdx.size();
    int mmMaxVisScroll = std::max(0, mmVisCount - maxVisible);
    int mmEffScroll = std::min(m_settingsScroll, mmMaxVisScroll);
    for (int vi = 0; vi < mmVisCount; ++vi) {
        int i = mmVisIdx[vi];
        int y = startY + (vi - mmEffScroll) * itemH;
        std::string label = makeSettingLabel(m_settingsTab, i, m_config);
        int tw = MeasureText(label.c_str(), 30);
        if (CheckCollisionPointRec(mouse, { (float)(centerX - tw/2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) }))
            { hovered = i; }
        // Reset button
        if (items[i].isValue || (m_settingsTab == 0 && i <= 7) || (m_settingsTab == 3 && items[i].actionId >= 0) || (m_settingsTab == 4 && i < 6) || (m_settingsTab == 5 && i < 1) || isVolumeSetting(m_settingsTab, i)) {
            const char* rl = "R";
            int rw = MeasureText(rl, 24);
            float rx = (m_settingsTab == 0 && i == 5) ? (centerX + 175) : (float)(centerX + tw/2 + 14);
            Rectangle rr = { rx, (float)(y + 5), (float)(rw + 16), (float)(24 + 8) };
            if (CheckCollisionPointRec(mouse, rr)) resetHovered = i;
        }
    }

    // Volume sliders. Runs before the activation block below, because a press
    // that lands on a slider must drag it and NOT also activate the row it sits
    // in -- the two hit-test the same rows.
    bool onVolumeSlider = updateVolumeSliders(startY, itemH, centerX, mmEffScroll);

    // Tab switching
    bool left = IsKeyPressed(KEY_LEFT);
    bool right = IsKeyPressed(KEY_RIGHT);
    // Volume rows count as values here even though their isValue is false: what
    // this gates is whether LEFT/RIGHT adjusts the row or flips to the next
    // tab, and on a volume row it has to adjust.
    bool onValue = m_settingsIndex >= 0 && m_settingsIndex < count &&
                   (items[m_settingsIndex].isValue || isVolumeSetting(m_settingsTab, m_settingsIndex));
    bool onFps = (m_settingsTab == 0 && m_settingsIndex == 5);
    bool onResolution = (m_settingsTab == 0 && m_settingsIndex == 4);

    if (left && !onValue && !onFps && !onResolution) {
        int newTab = m_settingsTab;
        do {
            newTab = (newTab + TAB_COUNT - 1) % TAB_COUNT;
        } while (newTab == 4 && !m_config.debugMode);
        m_settingsTab = newTab;
        m_settingsIndex = 0;
        m_settingsScroll = 0;
        m_keybindFilter.clear();
        m_keybindFilterActive = false;
    }
    if (right && !onValue && !onFps && !onResolution) {
        int newTab = m_settingsTab;
        do {
            newTab = (newTab + 1) % TAB_COUNT;
        } while (newTab == 4 && !m_config.debugMode);
        m_settingsTab = newTab;
        m_settingsIndex = 0;
        m_settingsScroll = 0;
        m_keybindFilter.clear();
        m_keybindFilterActive = false;
    }

    // Item navigation (skip collapsed items)
    auto isNavSkip = [&](int idx) -> bool {
        if (m_settingsTab != 3) return false;
        if (idx < 0 || idx >= count) return true;
        const Setting& s = items[idx];
        if (s.actionId < 0 && s.label[0] == '-' && s.label[1] == '-') return true;
        for (int si = idx - 1; si >= 0; --si) {
            bool siHeader = (items[si].actionId < 0 && items[si].label[0] == '-' && items[si].label[1] == '-');
            if (siHeader) return m_collapsedSections.count(si) > 0;
        }
        return false;
    };
    if (IsKeyPressed(KEY_UP)) {
        int ni = (m_settingsIndex + count - 1) % count;
        while (isNavSkip(ni) && ni != m_settingsIndex) ni = (ni + count - 1) % count;
        if (!isNavSkip(ni)) m_settingsIndex = ni;
        if (m_settingsIndex < m_settingsScroll) m_settingsScroll = m_settingsIndex;
    }
    if (IsKeyPressed(KEY_DOWN)) {
        int ni = (m_settingsIndex + 1) % count;
        while (isNavSkip(ni) && ni != m_settingsIndex) ni = (ni + 1) % count;
        if (!isNavSkip(ni)) m_settingsIndex = ni;
        if (m_settingsIndex >= m_settingsScroll + maxVisible) m_settingsScroll = m_settingsIndex - maxVisible + 1;
    }

    // Value cycling
    if ((left || right) && onValue) {
        int dir = right ? 1 : -1;
        if (m_settingsTab == 0 && m_settingsIndex == 3) {
            int idx = nearestIndex(m_config.maxZoom, MAX_ZOOM_VALS, MAX_ZOOM_COUNT);
            idx = (idx + dir + MAX_ZOOM_COUNT) % MAX_ZOOM_COUNT;
            m_config.maxZoom = MAX_ZOOM_VALS[idx];
        } else if (m_settingsTab == 1 && m_settingsIndex == 0) {
            int idx = nearestIndex(m_config.flySpeed, FLY_SPEED_VALS, FLY_SPEED_COUNT);
            idx = (idx + dir + FLY_SPEED_COUNT) % FLY_SPEED_COUNT;
            m_config.flySpeed = FLY_SPEED_VALS[idx];
        } else if (isVolumeSetting(m_settingsTab, m_settingsIndex)) {
            adjustVolume(m_settingsIndex, dir * 0.05f);
        }
    }

    // Resolution cycling
    if (m_settingsIndex == 4 && m_settingsTab == 0 && (left || right)) {
        int curIdx = -1;
        for (int r = 0; r < RES_COUNT; ++r)
            if (RESOLUTIONS[r][0] == m_screenW && RESOLUTIONS[r][1] == m_screenH) { curIdx = r; break; }
        if (curIdx < 0) curIdx = 0;
        curIdx = (curIdx + (right ? 1 : -1) + RES_COUNT) % RES_COUNT;
        m_screenW = RESOLUTIONS[curIdx][0];
        m_screenH = RESOLUTIONS[curIdx][1];
        m_config.screenW = m_screenW;
        m_config.screenH = m_screenH;
        forceWindowResize(m_screenW, m_screenH);
        PollInputEvents();
        m_screenW = GetScreenWidth();
        m_screenH = GetScreenHeight();
    }

    // FPS cycling
    if (m_settingsIndex == 5 && m_settingsTab == 0 && (left || right)) {
        int idx = fpsTargetToIndex(m_config.fpsTarget);
        idx = (idx + (right ? 1 : -1) + FPS_STEPS) % FPS_STEPS;
        m_config.fpsTarget = indexToFpsTarget(idx);
        applyFpsTarget(m_config.fpsTarget);
        Audio::get().playSfx("slider_tick", 0.08f);
    }

    // AI difficulty cycling
    if (m_settingsIndex == 7 && m_settingsTab == 0 && (left || right)) {
        m_config.aiDifficulty = (m_config.aiDifficulty + (right ? 1 : -1) + AI_DIFFICULTY_COUNT) % AI_DIFFICULTY_COUNT;
    }

    // FPS slider drag -- the same control the volume rows use, so grabbing,
    // stepping and letting go sound and behave identically on both.
    if (m_settingsTab == 0) {
        const Rectangle bar = sliderBarRect(startY + (5 - m_settingsScroll) * itemH,
                                            centerX);
        float t = (float)fpsTargetToIndex(m_config.fpsTarget) / (float)(FPS_STEPS - 1);
        if (sliderInteract(bar, FPS_STEPS, t, m_draggingFpsSlider)) {
            m_config.fpsTarget =
                indexToFpsTarget((int)roundf(t * (float)(FPS_STEPS - 1)));
            applyFpsTarget(m_config.fpsTarget);
        }
    }

    // Activation
    bool activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (resetHovered >= 0) {
            m_settingsIndex = resetHovered;
            // Reset value
            if (m_settingsTab == 0 && m_settingsIndex == 0) {
                if (m_config.fullscreen) {
                    setFullscreenAttrs(false, &m_windowedX, &m_windowedY, &m_windowedW, &m_windowedH);
                    PollInputEvents();
                }
                m_config.fullscreen = false;
                m_screenW = GetScreenWidth();
                m_screenH = GetScreenHeight();
            } else if (m_settingsTab == 0 && m_settingsIndex == 1) { m_config.showActualFlags = true; }
            else if (m_settingsTab == 0 && m_settingsIndex == 2) { m_config.debugMode = false; }
            else if (m_settingsTab == 0 && m_settingsIndex == 3) { m_config.maxZoom = 5.0f; }
            else if (m_settingsTab == 0 && m_settingsIndex == 4) { m_config.screenW = 1920; m_config.screenH = 1080; forceWindowResize(1920, 1080); }
            else if (m_settingsTab == 0 && m_settingsIndex == 5) { m_config.fpsTarget = 0; applyFpsTarget(m_config.fpsTarget); }
            else if (m_settingsTab == 0 && m_settingsIndex == 6) { m_config.accentColor = 0xFFD700; }
            else if (m_settingsTab == 0 && m_settingsIndex == 7) { m_config.aiDifficulty = 1; }
            else if (m_settingsTab == 1 && m_settingsIndex == 0) { m_config.flySpeed = 2.0f; }
            else if (m_settingsTab == 4 && m_settingsIndex == 0) { m_config.showFps = true; }
            else if (m_settingsTab == 4 && m_settingsIndex == 1) { m_config.showZoom = false; }
            else if (m_settingsTab == 4 && m_settingsIndex == 2) { m_config.showConsole = false; }
            else if (m_settingsTab == 4 && m_settingsIndex == 3) { m_config.aiDebug = false; }
            else if (m_settingsTab == 4 && m_settingsIndex == 4) {
                m_config.modUpdateChecks = false;
                ModUpdates::get().clear();
                m_modUpdatesAsked = false;
            }
            else if (m_settingsTab == 4 && m_settingsIndex == 5) { m_config.gameUpdateChecks = true; }
            else if (m_settingsTab == 5 && m_settingsIndex == 0) { m_config.aiLearning = false; }
            else if (isVolumeSetting(m_settingsTab, m_settingsIndex)) {
                if (float* v = volumeSettingPtr(m_config, m_settingsTab, m_settingsIndex)) {
                    *v = VOLUME_DEFAULTS[m_settingsIndex];
                    applyVolumes(m_config);
                }
            }
            else if (m_settingsTab == AUDIO_TAB && m_settingsIndex == VOLUME_COUNT) { m_config.nowPlayingToast = true; }
            else if (m_settingsTab == AUDIO_TAB && m_settingsIndex == VOLUME_COUNT + 1) { m_config.mapAtmosphere = true; Audio::get().setMapAtmosphere(true); }
            else if (m_settingsTab == 3 && items[m_settingsIndex].actionId >= 0) { m_config.keybinds[items[m_settingsIndex].actionId] = DEFAULT_KEYBINDS[items[m_settingsIndex].actionId]; }
            m_config.save(m_configPath);
        } else if (hovered >= 0 && !onVolumeSlider) {
            m_settingsIndex = hovered;
            activate = true;
        }
    }

    if (activate && m_settingsIndex >= 0 && m_settingsIndex < count) {
        const Setting& s = items[m_settingsIndex];
        if (strcmp(s.label, "Back") == 0) {
            m_inSettings = false;
            m_settingsScroll = 0;
            m_config.save(m_configPath);
        } else if (strcmp(s.label, "Fullscreen") == 0) {
            m_config.fullscreen = !m_config.fullscreen;
            Audio::get().playSfx(m_config.fullscreen ? "toggle_on" : "toggle_off");
            if (m_config.fullscreen) {
                m_windowedX = (int)GetWindowPosition().x;
                m_windowedY = (int)GetWindowPosition().y;
                m_windowedW = m_screenW;
                m_windowedH = m_screenH;
            }
            setFullscreenAttrs(m_config.fullscreen, &m_windowedX, &m_windowedY, &m_windowedW, &m_windowedH);
            PollInputEvents();
            m_screenW = GetScreenWidth();
            m_screenH = GetScreenHeight();
            m_config.screenW = m_screenW;
            m_config.screenH = m_screenH;
            initMenuBackground();
            m_menuBgScroll = 0;
        } else if (strcmp(s.label, "Show Actual Flags") == 0) {
            m_config.showActualFlags = !m_config.showActualFlags;
            Audio::get().playSfx(m_config.showActualFlags ? "toggle_on" : "toggle_off");
            rebuildFlags();
            if (m_renderer) m_renderer->setCountryFlags(&m_countryFlags);
        } else if (strcmp(s.label, "Debug Mode") == 0) {
            m_config.debugMode = !m_config.debugMode;
            Audio::get().playSfx(m_config.debugMode ? "toggle_on" : "toggle_off");
            if (!m_config.debugMode) {
                // Turn off debug overlays when debug mode is disabled
                m_config.showFps = false;
                m_config.showZoom = false;
                m_config.showConsole = false;
                if (m_settingsTab == 4) {
                    m_settingsTab = 0;
                    m_settingsIndex = 0;
                    m_settingsScroll = 0;
                }
            }
        } else if (strcmp(s.label, "Map Atmosphere") == 0) {
            m_config.mapAtmosphere = !m_config.mapAtmosphere;
            Audio::get().playSfx(m_config.mapAtmosphere ? "toggle_on" : "toggle_off");
            Audio::get().setMapAtmosphere(m_config.mapAtmosphere);
        } else if (strcmp(s.label, "Now Playing Toast") == 0) {
            m_config.nowPlayingToast = !m_config.nowPlayingToast;
            Audio::get().playSfx(m_config.nowPlayingToast ? "toggle_on" : "toggle_off");
            // Show one straight away when switching it on, so the setting
            // demonstrates itself instead of waiting for the next track.
            if (m_config.nowPlayingToast) showNowPlayingToast();
            else m_toastTimer = 0.0f;
        } else if (strcmp(s.label, "Display FPS") == 0) {
            m_config.showFps = !m_config.showFps;
            Audio::get().playSfx(m_config.showFps ? "toggle_on" : "toggle_off");
        } else if (strcmp(s.label, "Display Zoom") == 0) {
            m_config.showZoom = !m_config.showZoom;
            Audio::get().playSfx(m_config.showZoom ? "toggle_on" : "toggle_off");
        } else if (strcmp(s.label, "Console Window") == 0) {
            m_config.showConsole = !m_config.showConsole;
            Audio::get().playSfx(m_config.showConsole ? "toggle_on" : "toggle_off");
        } else if (strcmp(s.label, "AI Debug") == 0) {
            m_config.aiDebug = !m_config.aiDebug;
            Audio::get().playSfx(m_config.aiDebug ? "toggle_on" : "toggle_off");
        } else if (strcmp(s.label, "Check mods for updates") == 0) {
            m_config.modUpdateChecks = !m_config.modUpdateChecks;
            Audio::get().playSfx(m_config.modUpdateChecks ? "toggle_on" : "toggle_off");
            // Off has to mean off *now*, not from the next session: drop
            // whatever earlier checks returned so no "Can be updated" badge
            // outlives the setting that produced it. Clearing on the way on
            // too is what lets the toggle take effect without leaving and
            // re-entering the mods menu.
            ModUpdates::get().clear();
            m_modUpdatesAsked = false;
            // Say plainly what switching it on means. A player agreeing to
            // "check for updates" is not obviously agreeing to tell a stranger
            // which mods they run, so the message says so rather than assuming
            // they inferred it.
            m_menuFeedback = m_config.modUpdateChecks
                ? "On — each mod's author will see that you run their mod when the check runs"
                : "Off — no outbound requests";
            m_menuFeedbackTimer = 4.0f;
        } else if (strcmp(s.label, "Check for game updates") == 0) {
            m_config.gameUpdateChecks = !m_config.gameUpdateChecks;
            Audio::get().playSfx(m_config.gameUpdateChecks ? "toggle_on" : "toggle_off");
            m_menuFeedback = m_config.gameUpdateChecks
                ? "On — the game will ask its release host for new versions"
                : "Off — the game will not check for its own updates";
            m_menuFeedbackTimer = 4.0f;
        } else if (strcmp(s.label, "AI Learning") == 0) {
            // Never on while a mod is: a mod can change what the AI observes,
            // and training against that silently corrupts data/ai/model.bin.
            if (!m_config.aiLearning && ModManager::get().anyEnabled()) {
                m_menuFeedback = "Disable all mods first — training with mods loaded corrupts the model";
                m_menuFeedbackTimer = 4.0f;
            } else {
                m_config.aiLearning = !m_config.aiLearning;
                Audio::get().playSfx(m_config.aiLearning ? "toggle_on" : "toggle_off");
            }
        } else if (strcmp(s.label, "AI Difficulty") == 0) {
            m_config.aiDifficulty = (m_config.aiDifficulty + 1) % AI_DIFFICULTY_COUNT;
        } else if (strcmp(s.label, "Accent Color") == 0) {
            int curIdx = 0;
            for (int p = 0; p < ACCENT_PRESETS_COUNT; ++p)
                if (ACCENT_PRESETS[p] == m_config.accent()) { curIdx = p; break; }
            curIdx = (curIdx + 1) % ACCENT_PRESETS_COUNT;
            m_config.accentColor = ACCENT_PRESETS[curIdx];
        } else if (strcmp(s.label, "Resolution") == 0) {
            int curIdx = -1;
            for (int r = 0; r < RES_COUNT; ++r)
                if (RESOLUTIONS[r][0] == m_screenW && RESOLUTIONS[r][1] == m_screenH) { curIdx = r; break; }
            curIdx = (curIdx + 1) % RES_COUNT;
            m_screenW = RESOLUTIONS[curIdx][0];
            m_screenH = RESOLUTIONS[curIdx][1];
            m_config.screenW = m_screenW;
            m_config.screenH = m_screenH;
            forceWindowResize(m_screenW, m_screenH);
            PollInputEvents();
            m_screenW = GetScreenWidth();
            m_screenH = GetScreenHeight();
        } else if (m_settingsTab == 3 && items[m_settingsIndex].actionId >= 0) {
            m_rebindingAction = items[m_settingsIndex].actionId;
            m_waitingForKey = true;
        } else if (s.isValue) {
            float cur = (m_settingsTab == 0) ? m_config.maxZoom : (m_settingsTab == 1 ? m_config.flySpeed : 0);
            char buf[32]; snprintf(buf, sizeof(buf), "%.1f", cur);
            m_editBuffer = buf;
            m_editingValue = true;
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// flyToProvince
// ────────────────────────────────────────────────────────────────────────────
void Game::flyToProvince(int provinceId) {
    auto it = m_provinceCenters.find(provinceId);
    if (it == m_provinceCenters.end()) return;

    auto rit = m_provinceRadius.find(provinceId);
    float radius = (rit != m_provinceRadius.end()) ? rit->second : 100.0f;

    float targetZoom = (float)std::min(m_screenW, m_screenH) * 0.4f / std::max(radius * 2.0f, 1.0f);
    float minZoom = std::max(m_screenW / (float)m_provinces.getWidth(),
                             m_screenH / (float)m_provinces.getHeight());
    targetZoom = std::clamp(targetZoom, minZoom, 3.0f);

    m_renderer->flyTo(it->second.x, it->second.y, targetZoom, m_config.flySpeed);
}

// ────────────────────────────────────────────────────────────────────────────
// buildCountryProvinceList
// ────────────────────────────────────────────────────────────────────────────
void Game::buildCountryProvinceList(int provinceId) {
    const auto& all = m_provinces.getAllProvinces();
    auto it = all.find(provinceId);
    if (it == all.end()) return;
    int countryId = it->second.countryId;

    m_countryProvinceIds.clear();
    for (auto& [id, p] : all) {
        if (p.countryId == countryId) {
            m_countryProvinceIds.push_back(id);
        }
    }
    std::sort(m_countryProvinceIds.begin(), m_countryProvinceIds.end());

    m_countryProvinceIndex = -1;
    for (size_t i = 0; i < m_countryProvinceIds.size(); ++i) {
        if (m_countryProvinceIds[i] == provinceId) {
            m_countryProvinceIndex = (int)i;
            break;
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// cycleProvince
// ────────────────────────────────────────────────────────────────────────────
void Game::cycleProvince(int direction) {
    if (m_countryProvinceIds.empty() || m_countryProvinceIndex < 0) return;
    int n = (int)m_countryProvinceIds.size();
    m_countryProvinceIndex = (m_countryProvinceIndex + direction + n) % n;
    int nextId = m_countryProvinceIds[m_countryProvinceIndex];
    m_lastSelectedProvince = nextId;
    // Same event as clicking one on the map -- the mouse path plays this in
    // Game_Update, and this path assigns the selection directly, so it was the
    // only way to change province without hearing it.
    Audio::get().playSfx("select_province", 0.05f);
    m_renderer->setSelectedProvince(nextId);
    m_renderer->rebuildSelectionGlow();
    flyToProvince(nextId);
}
