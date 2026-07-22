#include "Game.h"
#include "GameInternals.h"
#include "SaveManager.h"
#include "raymath.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>

void Game::addNotification(const std::string& msg, Color color, float duration) {
    m_notifications.push_back({msg, duration, duration, color});
}

void Game::updateNotifications() {
    for (auto it = m_notifications.begin(); it != m_notifications.end(); ) {
        it->timer -= GetFrameTime();
        if (it->timer <= 0) {
            it = m_notifications.erase(it);
        } else {
            ++it;
        }
    }
}

void Game::pushPopup(PopupType type, const std::string& title, const std::string& message,
                     int countryId, const std::string& action,
                     const std::string& sourceIso, const std::string& targetIso) {
    PopupEntry entry;
    entry.type = type;
    entry.title = title;
    entry.message = message;
    entry.countryId = countryId;
    entry.action = action;
    entry.sourceIso = sourceIso;
    entry.targetIso = targetIso;
    m_popupQueue.push_back(entry);
}

void Game::drawPopup() {
    if (m_popupQueue.empty()) return;

    auto& popup = m_popupQueue.front();

    // Ceasefire popups need more vertical space to show the terms summary
    int popW = (popup.type == PopupType::CEASEFIRE_REQUEST) ? 560 : 480;
    int popH = (popup.type == PopupType::CEASEFIRE_REQUEST) ? 360 : 260;
    int popX = (m_screenW - popW) / 2;
    int popY = (m_screenH - popH) / 2;

    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});
    DrawRectangleRounded({(float)popX, (float)popY, (float)popW, (float)popH}, 0.1f, 8, {30, 30, 40, 255});
    DrawRectangleRoundedLines({(float)popX, (float)popY, (float)popW, (float)popH}, 0.1f, 8, {80, 80, 100, 255});

    int titleW = MeasureText(popup.title.c_str(), 22);
    DrawText(popup.title.c_str(), popX + (popW - titleW) / 2, popY + 20, 22, WHITE);

    // Render the message with simple multi-line wrapping on newlines
    int msgX = popX + 30, msgY = popY + 60;
    int msgW = popW - 60;
    const std::string& msg = popup.message;
    size_t pos = 0;
    while (pos < msg.size()) {
        size_t nl = msg.find('\n', pos);
        std::string line = (nl == std::string::npos) ? msg.substr(pos) : msg.substr(pos, nl - pos);
        DrawText(line.c_str(), msgX, msgY, 16, LIGHTGRAY);
        msgY += 22;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    // Buttons
    int btnW = 140, btnH = 40;
    int btnY = popY + popH - btnH - 20;

    if (popup.type == PopupType::DIPLOMATIC_REQUEST || popup.type == PopupType::CEASEFIRE_REQUEST) {
        // Approve / Reject
        Rectangle approveBtn = {(float)(popX + popW / 2 - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        Rectangle rejectBtn = {(float)(popX + popW / 2 + 10), (float)btnY, (float)btnW, (float)btnH};

        Vector2 mouse = getMouse();
        bool appHover = CheckCollisionPointRec(mouse, approveBtn);
        bool rejHover = CheckCollisionPointRec(mouse, rejectBtn);

        DrawRectangleRounded(approveBtn, 0.15f, 6, appHover ? Color{40, 180, 60, 255} : Color{30, 120, 40, 220});
        DrawRectangleRoundedLines(approveBtn, 0.15f, 6, appHover ? Color{60, 220, 80, 255} : Color{50, 150, 60, 200});
        const char* appLbl = (popup.type == PopupType::CEASEFIRE_REQUEST) ? "Accept" : "Approve";
        int appW = MeasureText(appLbl, 18);
        DrawText(appLbl, (int)(approveBtn.x + (btnW - appW) / 2), (int)(approveBtn.y + 10), 18, WHITE);

        DrawRectangleRounded(rejectBtn, 0.15f, 6, rejHover ? Color{200, 50, 50, 255} : Color{140, 30, 30, 220});
        DrawRectangleRoundedLines(rejectBtn, 0.15f, 6, rejHover ? Color{240, 70, 70, 255} : Color{170, 50, 50, 200});
        int rejW = MeasureText("Reject", 18);
        DrawText("Reject", (int)(rejectBtn.x + (btnW - rejW) / 2), (int)(rejectBtn.y + 10), 18, WHITE);
    } else {
        // OK button (REBELLION, WAR_DECLARED)
        Rectangle okBtn = {(float)(popX + (popW - btnW) / 2), (float)btnY, (float)btnW, (float)btnH};
        Vector2 mouse = getMouse();
        bool okHover = CheckCollisionPointRec(mouse, okBtn);

        DrawRectangleRounded(okBtn, 0.15f, 6, okHover ? Color{60, 60, 80, 255} : Color{40, 40, 60, 220});
        DrawRectangleRoundedLines(okBtn, 0.15f, 6, okHover ? Color{100, 100, 130, 255} : Color{70, 70, 90, 200});
        int okW = MeasureText("OK", 18);
        DrawText("OK", (int)(okBtn.x + (btnW - okW) / 2), (int)(okBtn.y + 10), 18, WHITE);
    }
}

void Game::updatePopup() {
    if (m_popupQueue.empty()) return;

    auto& popup = m_popupQueue.front();
    Vector2 mouse = getMouse();

    int popW = (popup.type == PopupType::CEASEFIRE_REQUEST) ? 560 : 480;
    int popH = (popup.type == PopupType::CEASEFIRE_REQUEST) ? 360 : 260;
    int popX = (m_screenW - popW) / 2;
    int popY = (m_screenH - popH) / 2;
    int btnW = 140, btnH = 40;
    int btnY = popY + popH - btnH - 20;

    if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return;

    if (popup.type == PopupType::DIPLOMATIC_REQUEST) {
        Rectangle approveBtn = {(float)(popX + popW / 2 - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        Rectangle rejectBtn = {(float)(popX + popW / 2 + 10), (float)btnY, (float)btnW, (float)btnH};

        if (CheckCollisionPointRec(mouse, approveBtn)) {
            // Execute the diplomatic action
            processDiplomaticRequests(); // process the approval
            m_popupQueue.erase(m_popupQueue.begin());
        } else if (CheckCollisionPointRec(mouse, rejectBtn)) {
            // Remove the pending action from queue
            if (!m_pendingDiplomaticActions.empty()) {
                m_pendingDiplomaticActions.erase(m_pendingDiplomaticActions.begin());
            }
            m_popupQueue.erase(m_popupQueue.begin());
        }
    } else if (popup.type == PopupType::CEASEFIRE_REQUEST) {
        Rectangle approveBtn = {(float)(popX + popW / 2 - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        Rectangle rejectBtn = {(float)(popX + popW / 2 + 10), (float)btnY, (float)btnW, (float)btnH};

        if (CheckCollisionPointRec(mouse, approveBtn)) {
            // Player accepted the ceasefire: schedule an apply_ceasefire action
            // that fires on the NEXT turn (1 → 0 → apply). Stash the terms so
            // processDiplomaticRequests can pick them up when apply_ceasefire runs.
            PendingDiplomaticAction da;
            da.sourceIso = popup.sourceIso;
            da.targetIso = popup.targetIso;
            da.action = "apply_ceasefire";
            da.turnsRemaining = 1;
            m_pendingDiplomaticActions.push_back(da);
            std::string key = popup.sourceIso + "|" + popup.targetIso;
            m_acceptedCeasefireTerms[key] = popup.terms;
            // The original request_ceasefire action was already erased from
            // m_pendingDiplomaticActions when the popup was pushed. We also
            // remove the original terms from m_pendingCeasefireTerms so the
            // sender can issue new offers if needed later.
            auto tit = m_pendingCeasefireTerms.find(key);
            if (tit != m_pendingCeasefireTerms.end()) m_pendingCeasefireTerms.erase(tit);
            m_popupQueue.erase(m_popupQueue.begin());
            printf("[CEASEFIRE] Player accepted offer from %s — apply scheduled for next turn\n", popup.sourceIso.c_str());
        } else if (CheckCollisionPointRec(mouse, rejectBtn)) {
            // Erase the waiting terms and notify sender side (no automatic
            // re-permit for now — sender will see the war persists).
            std::string key = popup.sourceIso + "|" + popup.targetIso;
            auto tit = m_pendingCeasefireTerms.find(key);
            if (tit != m_pendingCeasefireTerms.end()) m_pendingCeasefireTerms.erase(tit);
            m_popupQueue.erase(m_popupQueue.begin());
            printf("[CEASEFIRE] Player rejected offer from %s\n", popup.sourceIso.c_str());
        }
    } else {
        // OK button (REBELLION, WAR_DECLARED)
        Rectangle okBtn = {(float)(popX + (popW - btnW) / 2), (float)btnY, (float)btnW, (float)btnH};
        if (CheckCollisionPointRec(mouse, okBtn)) {
            m_popupQueue.erase(m_popupQueue.begin());
        }
    }
}

void Game::drawCeasefireScreen() {
    // Full-screen ceasefire/peace negotiation UI
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 220});

    // Use nearly the full screen for the panel
    int panelW = m_screenW - 16;
    int panelH = m_screenH - 16;
    int panelX = 8;
    int panelY = 8;

    DrawRectangleRounded({(float)panelX, (float)panelY, (float)panelW, (float)panelH}, 0.02f, 4, {20, 20, 30, 240});
    DrawRectangleRoundedLines({(float)panelX, (float)panelY, (float)panelW, (float)panelH}, 0.02f, 4, {80, 80, 100, 200});

    const Country* playerC = m_countries.getCountry(m_playerCountryId);
    const Country* targetC = !m_ceasefireTargetIso.empty() ? m_countries.getCountryByCode(m_ceasefireTargetIso) : nullptr;
    if (!playerC || !targetC) {
        std::string title = "Peace Negotiation";
        int titleW = MeasureText(title.c_str(), 28);
        DrawText(title.c_str(), panelX + (panelW - titleW) / 2, panelY + 16, 28, hexToColor(m_config.accentColor));
        DrawText("ESC to close", 10, m_screenH - 24, 14, (Color){80, 80, 90, 200});
        return;
    }

    std::string title = "Peace Negotiation - " + targetC->name;
    int titleW = MeasureText(title.c_str(), 28);
    DrawText(title.c_str(), panelX + (panelW - titleW) / 2, panelY + 12, 28, hexToColor(m_config.accentColor));

    // Close button (top-right of panel)
    Rectangle closeBtn = {(float)(panelX + panelW - 44), (float)(panelY + 4), 36, 36};
    Vector2 mouse = getMouse();
    if (CheckCollisionPointRec(mouse, closeBtn)) {
        DrawRectangleRounded(closeBtn, 0.2f, 6, {80, 60, 60, 220});
    } else {
        DrawRectangleRounded(closeBtn, 0.2f, 6, {60, 50, 50, 180});
    }
    DrawRectangleRoundedLines(closeBtn, 0.2f, 6, {180, 150, 150, 200});
    int xw = MeasureText("X", 20);
    DrawText("X", (int)(closeBtn.x + closeBtn.width/2 - xw/2), (int)(closeBtn.y + 8), 20, {200, 200, 200, 220});

    // ── Compute bounding box for the relevant provinces (player + target owned + claims) ──
    int mapTexW = m_provinces.getWidth();
    int mapTexH = m_provinces.getHeight();

    // Layout: sidebar on right (narrower), map fills the rest
    int sidebarW = 280;
    int sidebarPad = 8;
    int titleBarH = 50;
    int bottomBarH = 30;
    int sbX = panelX + panelW - sidebarW - sidebarPad;
    int sbY = panelY + titleBarH;
    int sbH = panelH - titleBarH - bottomBarH;
    int mapX = panelX + sidebarPad;
    int mapY = panelY + titleBarH;
    int mapW = sbX - mapX - sidebarPad;
    int mapH = sbH;

    std::unordered_set<int> relevantPids;
    for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
        if (prov.countryId == m_playerCountryId) relevantPids.insert(pid);
        if (prov.countryId == targetC->id) relevantPids.insert(pid);
    }
    auto addClaims = [&](const std::string& iso) {
        auto it = m_claims.find(iso);
        if (it != m_claims.end()) for (int pid : it->second) relevantPids.insert(pid);
    };
    addClaims(playerC->isoA3);
    addClaims(targetC->isoA3);

    int minPx = 0, maxPx = mapTexW - 1, minPy = 0, maxPy = mapTexH - 1;
    bool hasBounds = false;
    for (int pid : relevantPids) {
        auto ppIt = m_provincePixels.find(pid);
        if (ppIt == m_provincePixels.end() || ppIt->second.empty()) continue;
        for (int idx : ppIt->second) {
            int px = idx % mapTexW;
            int py = idx / mapTexW;
            if (!hasBounds) { minPx = maxPx = px; minPy = maxPy = py; hasBounds = true; }
            else {
                if (px < minPx) minPx = px; if (px > maxPx) maxPx = px;
                if (py < minPy) minPy = py; if (py > maxPy) maxPy = py;
            }
        }
    }

    int padX = hasBounds ? std::max((maxPx - minPx) / 6, 50) : 0;
    int padY = hasBounds ? std::max((maxPy - minPy) / 6, 50) : 0;
    int baseSrcX = hasBounds ? std::max(0, minPx - padX) : 0;
    int baseSrcY = hasBounds ? std::max(0, minPy - padY) : 0;
    int srcW = hasBounds ? std::min(mapTexW - baseSrcX, maxPx - minPx + 2 * padX) : mapTexW;
    int srcH = hasBounds ? std::min(mapTexH - baseSrcY, maxPy - minPy + 2 * padY) : mapTexH;
    if (!hasBounds) { baseSrcX = 0; baseSrcY = 0; srcW = mapTexW; srcH = mapTexH; }

    float z = m_ceasefireMapZoom;
    if (z < 1.0f) z = 1.0f;
    if (z > 5.0f) z = 5.0f;
    m_ceasefireMapZoom = z;
    int zoomedW = std::max((int)(srcW / z), 10);
    int zoomedH = std::max((int)(srcH / z), 10);
    int zoomShiftX = (srcW - zoomedW) / 2;
    int zoomShiftY = (srcH - zoomedH) / 2;
    int srcX = baseSrcX + m_ceasefireMapSrcX + zoomShiftX;
    int srcY = baseSrcY + m_ceasefireMapSrcY + zoomShiftY;
    srcX = std::clamp(srcX, 0, mapTexW - zoomedW);
    srcY = std::clamp(srcY, 0, mapTexH - zoomedH);
    srcW = zoomedW;
    srcH = zoomedH;

    // Letterboxed destination rect
    float srcAspect = (float)srcW / srcH;
    float dstAspect = (float)mapW / mapH;
    float dW, dH, dX, dYf;
    if (srcAspect > dstAspect) {
        dW = (float)mapW;
        dH = mapW / srcAspect;
        dX = (float)mapX;
        dYf = mapY + (mapH - dH) * 0.5f;
    } else {
        dH = (float)mapH;
        dW = mapH * srcAspect;
        dX = mapX + (mapW - dW) * 0.5f;
        dYf = (float)mapY;
    }

    // Collect provinces locked by pending ceasefire offers to OTHER countries
    std::unordered_set<int> lockedPids;
    for (auto& [key, terms] : m_pendingCeasefireTerms) {
        // Don't lock if this is the current target pair
        if (key == playerC->isoA3 + "|" + targetC->isoA3) continue;
        for (int pid : terms.ourProvs) lockedPids.insert(pid);
        for (int pid : terms.theirProvs) lockedPids.insert(pid);
        for (int pid : terms.ourDropClaims) lockedPids.insert(pid);
        for (int pid : terms.theirDropClaims) lockedPids.insert(pid);
    }

    // Build overlay stripes (cached, only rebuild when dirty)
    bool needRebuild = m_ceasefireOverlayDirty || m_ceasefireOverlayBuf.size() != (size_t)(mapTexW * mapTexH);
    if (needRebuild) {
        m_ceasefireOverlayBuf.assign(mapTexW * mapTexH, Color{0, 0, 0, 0});
        auto paintStripes = [&](const std::vector<int>& pids, Color col, int phaseMod) {
            for (int pid : pids) {
                auto ppIt = m_provincePixels.find(pid);
                if (ppIt == m_provincePixels.end()) continue;
                for (int idx : ppIt->second) {
                    int px = idx % mapTexW;
                    int py = idx / mapTexW;
                    if (((px + py + phaseMod) % 10) < 5) m_ceasefireOverlayBuf[idx] = col;
                }
            }
        };
        // Paint locked provinces grey
        for (int pid : lockedPids) {
            auto ppIt = m_provincePixels.find(pid);
            if (ppIt == m_provincePixels.end()) continue;
            for (int idx : ppIt->second)
                m_ceasefireOverlayBuf[idx] = Color{80, 80, 80, 160};
        }
        // Cede our province: blue stripes (we lose territory)
        paintStripes(m_ceasefireOurProvs, Color{40, 120, 220, 200}, 0);
        // Demand their province: orange stripes (we gain territory)
        paintStripes(m_ceasefireTheirProvs, Color{220, 140, 30, 210}, 5);
        // Drop our claim: red stripes (we give up a claim)
        paintStripes(m_ceasefireOurDropClaims, Color{220, 40, 40, 210}, 0);
        // Demand they drop their claim: purple stripes (they give up a claim)
        paintStripes(m_ceasefireTheirDropClaims, Color{180, 50, 200, 210}, 5);
    }

    // Upload overlay texture (re-create or update)
    if (m_ceasefireOverlayTex.id == 0 || m_ceasefireOverlayTex.width != mapTexW || m_ceasefireOverlayTex.height != mapTexH) {
        if (m_ceasefireOverlayTex.id > 0) UnloadTexture(m_ceasefireOverlayTex);
        Image img{};
        img.data = m_ceasefireOverlayBuf.data();
        img.width = mapTexW;
        img.height = mapTexH;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        m_ceasefireOverlayTex = LoadTextureFromImage(img);
    } else if (needRebuild) {
        UpdateTexture(m_ceasefireOverlayTex, m_ceasefireOverlayBuf.data());
    }
    m_ceasefireOverlayDirty = false;

    if (m_politicalTex.id > 0) {
        DrawTexturePro(m_politicalTex,
            {(float)srcX, (float)srcY, (float)srcW, (float)srcH},
            {dX, dYf, dW, dH}, {0, 0}, 0, WHITE);
        DrawTexturePro(m_ceasefireOverlayTex,
            {(float)srcX, (float)srcY, (float)srcW, (float)srcH},
            {dX, dYf, dW, dH}, {0, 0}, 0, WHITE);
    } else {
        DrawRectangle((int)dX, (int)dYf, (int)dW, (int)dH, {30, 30, 50, 255});
    }
    DrawRectangleLines((int)dX, (int)dYf, (int)dW, (int)dH, {80, 80, 120, 180});
    DrawText("Drag map to pan | Scroll to zoom | Click province to toggle", (int)dX + 4, (int)(dYf + dH - 18), 12, Color{180, 180, 200, 140});

    // ── Sidebar ─────
    DrawRectangle(sbX, sbY, sidebarW, sbH, {15, 15, 25, 220});
    DrawRectangleLines(sbX, sbY, sidebarW, sbH, {60, 60, 90, 200});

    int curY = sbY + 8;

    // Section header: "Selection Mode"
    DrawText("Selection Mode", sbX + 8, curY, 13, hexToColor(m_config.accentColor));
    curY += 20;

    int modeBtnH = 26;
    int modeBtnGap = 4;

    auto drawModeBtn = [&](int mode, const char* label, Color col) {
        Rectangle r = {(float)sbX + 8, (float)curY, (float)(sidebarW - 16), (float)modeBtnH};
        bool active = (m_ceasefireSelectMode == mode);
        bool hov = CheckCollisionPointRec(mouse, r);
        Color bg = active ? col : (hov ? Color{50, 50, 70, 220} : Color{30, 30, 45, 220});
        if (active) { bg.r = (unsigned char)std::min(255, (int)(bg.r * 0.6f + 80));
                      bg.g = (unsigned char)std::min(255, (int)(bg.g * 0.6f + 80));
                      bg.b = (unsigned char)std::min(255, (int)(bg.b * 0.6f + 80)); }
        DrawRectangleRounded(r, 0.08f, 4, bg);
        DrawRectangleRoundedLines(r, 0.08f, 4, active ? col : Color{80, 80, 110, 200});
        int tw = MeasureText(label, 11);
        DrawText(label, (int)(r.x + (r.width - tw) / 2), (int)(r.y + 7), 11, WHITE);
        curY += modeBtnH + modeBtnGap;
    };

    drawModeBtn(1, "Cede Our Province", Color{40, 120, 220, 230});
    drawModeBtn(2, "Drop Our Claim",    Color{220, 40, 40, 230});
    drawModeBtn(3, "Demand Their Province", Color{220, 140, 30, 230});
    drawModeBtn(4, "Demand They Drop Claim", Color{180, 50, 200, 230});

    curY += 2;
    auto modeLabel = [](int m) -> const char* {
        switch (m) {
            case 1: return "Cede Our Province (blue)";
            case 2: return "Drop Our Claim (red)";
            case 3: return "Demand Their Province (orange)";
            case 4: return "Demand They Drop Claim (purple)";
            default: return "Idle";
        }
    };
    DrawText(TextFormat("Mode: %s", modeLabel(m_ceasefireSelectMode)), sbX + 8, curY, 11, LIGHTGRAY);
    curY += 18;

    // ── Money inputs (integer sliders) ──
    DrawText("Money", sbX + 8, curY, 13, hexToColor(m_config.accentColor));
    curY += 18;

    auto drawMoneySlider = [&](const char* label, int& value, int max, Color col) {
        DrawText(label, sbX + 8, curY, 11, WHITE);
        char buf[32]; snprintf(buf, sizeof(buf), "%d", value);
        int tw = MeasureText(buf, 11);
        DrawText(buf, sbX + sidebarW - 8 - tw, curY, 11, col);
        curY += 14;
        Rectangle slr = {(float)(sbX + 8), (float)curY, (float)(sidebarW - 16), 10};
        DrawRectangle((int)slr.x, (int)slr.y, (int)slr.width, (int)slr.height, Color{40, 40, 50, 200});
        if (max > 0) {
            int hov = (int)((mouse.x - slr.x) / slr.width * max);
            bool dragHeld = IsMouseButtonDown(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, slr);
            if (dragHeld) { value = std::max(0, std::min(max, hov)); }
            int fill = (int)(slr.width * value / std::max(1, max));
            DrawRectangle((int)slr.x, (int)slr.y, fill, (int)slr.height, col);
        }
        DrawRectangleLines((int)slr.x, (int)slr.y, (int)slr.width, (int)slr.height, Color{80, 80, 100, 220});
        curY += 16;
    };

    int pMax = (int)std::max(0.0f, m_countries.getAll()[m_playerCountryId].treasury + 1000);
    int tMax = (int)std::max(0.0f, targetC->treasury + 1000);
    drawMoneySlider("Money we offer", m_ceasefireOurMoney, pMax, Color{40, 200, 40, 220});
    drawMoneySlider("Money we demand", m_ceasefireTheirMoney, tMax, Color{220, 60, 60, 220});

    curY += 4;

    // ── Summary ──
    DrawText("Summary", sbX + 8, curY, 13, hexToColor(m_config.accentColor));
    curY += 18;
    DrawText(TextFormat("Cede: %d province(s)", (int)m_ceasefireOurProvs.size()), sbX + 8, curY, 11, Color{120, 180, 255, 255}); curY += 16;
    DrawText(TextFormat("Demand: %d province(s)", (int)m_ceasefireTheirProvs.size()), sbX + 8, curY, 11, Color{255, 180, 80, 255}); curY += 16;
    DrawText(TextFormat("Drop our claims: %d", (int)m_ceasefireOurDropClaims.size()), sbX + 8, curY, 11, Color{255, 100, 100, 255}); curY += 16;
    DrawText(TextFormat("They drop claims: %d", (int)m_ceasefireTheirDropClaims.size()), sbX + 8, curY, 11, Color{220, 130, 220, 255}); curY += 16;

    if (!lockedPids.empty()) {
        DrawText(TextFormat("Locked by other offers: %d", (int)lockedPids.size()), sbX + 8, curY, 11, Color{120, 120, 120, 255}); curY += 16;
    }

    // Bottom buttons (anchored to sidebar bottom)
    int buttonY = sbY + sbH - 72;

    Rectangle sendBtn = {(float)(sbX + 8), (float)buttonY, (float)(sidebarW - 16), 32};
    bool sendHov = CheckCollisionPointRec(mouse, sendBtn);
    Color sendBg = sendHov ? Color{40, 180, 60, 240} : Color{30, 120, 40, 220};
    DrawRectangleRounded(sendBtn, 0.1f, 4, sendBg);
    DrawRectangleRoundedLines(sendBtn, 0.1f, 4, Color{80, 220, 100, 220});
    int sendW = MeasureText("Send Ceasefire Offer", 13);
    DrawText("Send Ceasefire Offer", (int)(sendBtn.x + (sendBtn.width - sendW) / 2), (int)(sendBtn.y + 9), 13, WHITE);

    Rectangle cancelBtn = {(float)(sbX + 8), (float)(buttonY + 36), (float)(sidebarW - 16), 28};
    bool cancelHov = CheckCollisionPointRec(mouse, cancelBtn);
    DrawRectangleRounded(cancelBtn, 0.1f, 4, cancelHov ? Color{80, 40, 40, 240} : Color{60, 30, 30, 220});
    DrawRectangleRoundedLines(cancelBtn, 0.1f, 4, Color{180, 100, 100, 220});
    int cancelW = MeasureText("Cancel", 12);
    DrawText("Cancel", (int)(cancelBtn.x + (cancelBtn.width - cancelW) / 2), (int)(cancelBtn.y + 8), 12, WHITE);

    DrawText("ESC or X to close", panelX + 8, panelY + panelH - 18, 11, Color{120, 120, 140, 200});
}

void Game::updateCeasefireScreen() {
    if (IsKeyPressed(KEY_ESCAPE)) {
        m_inCeasefireScreen = false;
        m_ceasefireSelectMode = 0;
        return;
    }
    Vector2 mouse = getMouse();

    // Layout must match drawCeasefireScreen exactly
    int panelW = m_screenW - 16;
    int panelH = m_screenH - 16;
    int panelX = 8;
    int panelY = 8;

    int sidebarW = 280;
    int sidebarPad = 8;
    int titleBarH = 50;
    int bottomBarH = 30;
    int sbX = panelX + panelW - sidebarW - sidebarPad;
    int sbY = panelY + titleBarH;
    int sbH = panelH - titleBarH - bottomBarH;
    int mapX = panelX + sidebarPad;
    int mapY = panelY + titleBarH;
    int mapW = sbX - mapX - sidebarPad;
    int mapH = sbH;

    // Close (X) button
    Rectangle closeBtn = {(float)(panelX + panelW - 44), (float)(panelY + 4), 36, 36};
    if (CheckCollisionPointRec(mouse, closeBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        m_inCeasefireScreen = false;
        m_ceasefireSelectMode = 0;
        return;
    }

    int mapTexW = m_provinces.getWidth();
    int mapTexH = m_provinces.getHeight();
    const Country* targetC = !m_ceasefireTargetIso.empty() ? m_countries.getCountryByCode(m_ceasefireTargetIso) : nullptr;
    const Country* playerC = m_countries.getCountry(m_playerCountryId);
    if (!targetC || !playerC) {
        m_inCeasefireScreen = false;
        return;
    }

    // Compute visible src rect (identical to drawCeasefireScreen)
    auto computeSrc = [&]() -> Rectangle {
        float z = m_ceasefireMapZoom;
        if (z < 1.0f) z = 1.0f; if (z > 5.0f) z = 5.0f;
        int srcW = mapTexW, srcH = mapTexH, baseSrcX = 0, baseSrcY = 0;
        int minPx = 0, maxPx = mapTexW - 1, minPy = 0, maxPy = mapTexH - 1; bool hasBounds = false;
        std::unordered_set<int> relevantPids;
        for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
            if (prov.countryId == m_playerCountryId) relevantPids.insert(pid);
            if (prov.countryId == targetC->id) relevantPids.insert(pid);
        }
        auto addCl = [&](const std::string& iso){ auto it = m_claims.find(iso); if (it != m_claims.end()) for (int pid : it->second) relevantPids.insert(pid); };
        addCl(playerC->isoA3); addCl(targetC->isoA3);
        for (int pid : relevantPids) {
            auto ppIt = m_provincePixels.find(pid);
            if (ppIt == m_provincePixels.end()) continue;
            for (int idx : ppIt->second) {
                int px = idx % mapTexW; int py = idx / mapTexW;
                if (!hasBounds) { minPx = maxPx = px; minPy = maxPy = py; hasBounds = true; }
                else { if (px < minPx) minPx = px; if (px > maxPx) maxPx = px; if (py < minPy) minPy = py; if (py > maxPy) maxPy = py; }
            }
        }
        int padX = hasBounds ? std::max((maxPx - minPx) / 6, 50) : 0;
        int padY = hasBounds ? std::max((maxPy - minPy) / 6, 50) : 0;
        baseSrcX = hasBounds ? std::max(0, minPx - padX) : 0;
        baseSrcY = hasBounds ? std::max(0, minPy - padY) : 0;
        srcW = hasBounds ? std::min(mapTexW - baseSrcX, maxPx - minPx + 2 * padX) : mapTexW;
        srcH = hasBounds ? std::min(mapTexH - baseSrcY, maxPy - minPy + 2 * padY) : mapTexH;
        int zoomedW = std::max((int)(srcW / z), 10);
        int zoomedH = std::max((int)(srcH / z), 10);
        int zoomShiftX = (srcW - zoomedW) / 2;
        int zoomShiftY = (srcH - zoomedH) / 2;
        int sx = std::clamp(baseSrcX + m_ceasefireMapSrcX + zoomShiftX, 0, mapTexW - zoomedW);
        int sy = std::clamp(baseSrcY + m_ceasefireMapSrcY + zoomShiftY, 0, mapTexH - zoomedH);
        return {(float)sx, (float)sy, (float)zoomedW, (float)zoomedH};
    };

    Rectangle srcRect = computeSrc();
    float srcAspect = srcRect.width / srcRect.height;
    float dstAspect = (float)mapW / mapH;
    float dW, dH, dX, dY;
    if (srcAspect > dstAspect) { dW = (float)mapW; dH = mapW / srcAspect; dX = (float)mapX; dY = mapY + (mapH - dH) * 0.5f; }
    else { dH = (float)mapH; dW = mapH * srcAspect; dX = mapX + (mapW - dW) * 0.5f; dY = (float)mapY; }
    Rectangle dstRect = {dX, dY, dW, dH};

    // Mouse scroll for zoom
    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        m_ceasefireMapZoom *= (wheel > 0 ? 1.15f : 0.87f);
        if (m_ceasefireMapZoom < 1.0f) m_ceasefireMapZoom = 1.0f;
        if (m_ceasefireMapZoom > 5.0f) m_ceasefireMapZoom = 5.0f;
    }

    // Map dragging
    if (CheckCollisionPointRec(mouse, dstRect) && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && m_ceasefireSelectMode == 0) {
        if (!m_ceasefireMapDragging) {
            m_ceasefireMapDragging = true;
            m_ceasefireMapDragPrevX = (int)mouse.x;
            m_ceasefireMapDragPrevY = (int)mouse.y;
        } else {
            int dx = (int)mouse.x - m_ceasefireMapDragPrevX;
            int dy = (int)mouse.y - m_ceasefireMapDragPrevY;
            m_ceasefireMapDragPrevX = (int)mouse.x;
            m_ceasefireMapDragPrevY = (int)mouse.y;
            float scale = srcRect.width / dstRect.width;
            m_ceasefireMapSrcX -= (int)(dx * scale);
            m_ceasefireMapSrcY -= (int)(dy * scale);
        }
    } else {
        m_ceasefireMapDragging = false;
    }

    // Sidebar buttons — curY must match drawCeasefireScreen EXACTLY
    int curY = sbY + 8;
    // "Selection Mode" header
    curY += 20;
    // Mode buttons
    int modeBtnH = 26;
    int modeBtnGap = 4;
    auto modeBtnHit = [&]() -> Rectangle {
        Rectangle r = {(float)sbX + 8, (float)curY, (float)(sidebarW - 16), (float)modeBtnH};
        curY += modeBtnH + modeBtnGap;
        return r;
    };
    Rectangle m1r = modeBtnHit();
    Rectangle m2r = modeBtnHit();
    Rectangle m3r = modeBtnHit();
    Rectangle m4r = modeBtnHit();
    bool click = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    if (click) {
        if (CheckCollisionPointRec(mouse, m1r)) m_ceasefireSelectMode = (m_ceasefireSelectMode == 1) ? 0 : 1;
        else if (CheckCollisionPointRec(mouse, m2r)) m_ceasefireSelectMode = (m_ceasefireSelectMode == 2) ? 0 : 2;
        else if (CheckCollisionPointRec(mouse, m3r)) m_ceasefireSelectMode = (m_ceasefireSelectMode == 3) ? 0 : 3;
        else if (CheckCollisionPointRec(mouse, m4r)) m_ceasefireSelectMode = (m_ceasefireSelectMode == 4) ? 0 : 4;
    }

    // Skip past money sliders + summary (consume clicks on slider areas)
    curY += 18; // mode label
    curY += 18; // "Money" header
    // Two money sliders: each = label(14) + track(16) = 30
    // Slider drag handling is done in draw, but we need to not fall through to send/cancel
    // Just skip past the curY
    curY += 30; // first slider
    curY += 30; // second slider
    curY += 4;
    curY += 18; // "Summary"
    curY += 16 * 4; // 4 summary lines
    // lockedPids line may or may not be present — just advance a bit
    curY += 16;

    // Send / Cancel buttons (anchored to sidebar bottom)
    int buttonY = sbY + sbH - 72;
    Rectangle sendBtn = {(float)(sbX + 8), (float)buttonY, (float)(sidebarW - 16), 32};
    Rectangle cancelBtn = {(float)(sbX + 8), (float)(buttonY + 36), (float)(sidebarW - 16), 28};

    if (CheckCollisionPointRec(mouse, sendBtn) && click) {
        // Clamp offer to what the player can actually pay (treasury >= 0)
        float& pTreas = m_countries.getAll()[m_playerCountryId].treasury;
        if (m_ceasefireOurMoney > (int)pTreas) m_ceasefireOurMoney = (int)pTreas;
        if (m_ceasefireOurMoney < 0) m_ceasefireOurMoney = 0;
        // Deduct offered money from treasury immediately (refunded if rejected)
        pTreas -= m_ceasefireOurMoney;

        // Clamp demand to what target can actually pay
        if (m_ceasefireTheirMoney > (int)targetC->treasury) m_ceasefireTheirMoney = (int)targetC->treasury;
        if (m_ceasefireTheirMoney < 0) m_ceasefireTheirMoney = 0;

        PendingDiplomaticAction da;
        da.sourceIso = playerC->isoA3;
        da.targetIso = targetC->isoA3;
        da.action = "request_ceasefire";
        da.turnsRemaining = 2;
        m_pendingDiplomaticActions.push_back(da);
        CeasefireTerms terms;
        terms.ourMoney = m_ceasefireOurMoney;
        terms.theirMoney = m_ceasefireTheirMoney;
        terms.ourProvs = m_ceasefireOurProvs;
        terms.theirProvs = m_ceasefireTheirProvs;
        terms.ourDropClaims = m_ceasefireOurDropClaims;
        terms.theirDropClaims = m_ceasefireTheirDropClaims;
        std::string key = playerC->isoA3 + "|" + targetC->isoA3;
        m_pendingCeasefireTerms[key] = terms;
        printf("[CEASEFIRE] Offer sent: %s -> %s (offer=$%d demand=$%d)\n",
               playerC->isoA3.c_str(), targetC->isoA3.c_str(),
               m_ceasefireOurMoney, m_ceasefireTheirMoney);
        m_inCeasefireScreen = false;
        m_ceasefireSelectMode = 0;
        return;
    }
    if (CheckCollisionPointRec(mouse, cancelBtn) && click) {
        m_inCeasefireScreen = false;
        m_ceasefireSelectMode = 0;
        return;
    }

    // Map click — toggle province based on current mode
    if (m_ceasefireSelectMode != 0
        && CheckCollisionPointRec(mouse, dstRect)
        && click
        && !m_ceasefireMapDragging) {
        float tx = (mouse.x - dstRect.x) / dstRect.width * srcRect.width + srcRect.x;
        float ty = (mouse.y - dstRect.y) / dstRect.height * srcRect.height + srcRect.y;
        int px = (int)std::clamp(tx, 0.0f, (float)(mapTexW - 1));
        int py = (int)std::clamp(ty, 0.0f, (float)(mapTexH - 1));
        const Province* pcProv = (px >= 0 && py >= 0 && px < mapTexW && py < mapTexH) ? m_provinces.getProvince(px, py) : nullptr;
        if (!pcProv) return;
        int pid = pcProv->id;
        if (pid <= 0) return;
        Province* pp = m_provinces.getProvinceById(pid);
        if (!pp) return;

        // Check if province is locked by another pending ceasefire offer
        bool locked = false;
        for (auto& [key, terms] : m_pendingCeasefireTerms) {
            if (key == playerC->isoA3 + "|" + targetC->isoA3) continue;
            for (int lpid : terms.ourProvs) if (lpid == pid) { locked = true; break; }
            for (int lpid : terms.theirProvs) if (lpid == pid) { locked = true; break; }
            for (int lpid : terms.ourDropClaims) if (lpid == pid) { locked = true; break; }
            for (int lpid : terms.theirDropClaims) if (lpid == pid) { locked = true; break; }
            if (locked) break;
        }
        if (locked) return;

        auto toggle = [](std::vector<int>& v, int id) {
            auto it = std::find(v.begin(), v.end(), id);
            if (it != v.end()) v.erase(it);
            else v.push_back(id);
        };

        switch (m_ceasefireSelectMode) {
            case 1:
                if (pp->countryId == m_playerCountryId) { toggle(m_ceasefireOurProvs, pid); m_ceasefireOverlayDirty = true; }
                break;
            case 2: {
                auto it = m_claims.find(playerC->isoA3);
                if (it != m_claims.end() && std::find(it->second.begin(), it->second.end(), pid) != it->second.end()) {
                    toggle(m_ceasefireOurDropClaims, pid); m_ceasefireOverlayDirty = true;
                }
                break;
            }
            case 3:
                if (pp->countryId == targetC->id) { toggle(m_ceasefireTheirProvs, pid); m_ceasefireOverlayDirty = true; }
                break;
            case 4: {
                auto it = m_claims.find(targetC->isoA3);
                if (it != m_claims.end() && std::find(it->second.begin(), it->second.end(), pid) != it->second.end()) {
                    toggle(m_ceasefireTheirDropClaims, pid); m_ceasefireOverlayDirty = true;
                }
                break;
            }
            default: break;
        }
    }
}

std::string Game::saveStateJson() {
    nlohmann::json j;

    // Pending orders
    for (auto& u : m_pendingUpgrades) {
        nlohmann::json entry;
        entry["provinceId"] = u.provinceId;
        entry["type"] = u.type;
        entry["targetLevel"] = u.targetLevel;
        entry["turnsRemaining"] = u.turnsRemaining;
        j["pendingUpgrades"].push_back(entry);
    }
    for (auto& s : m_pendingSpecializations) {
        nlohmann::json entry;
        entry["provinceId"] = s.provinceId;
        entry["specialization"] = s.specialization;
        entry["turnsRemaining"] = s.turnsRemaining;
        j["pendingSpecializations"].push_back(entry);
    }
    for (auto& r : m_pendingRecruitments) {
        nlohmann::json entry;
        entry["provinceId"] = r.provinceId;
        entry["count"] = r.count;
        entry["turnsRemaining"] = r.turnsRemaining;
        j["pendingRecruitments"].push_back(entry);
    }
    for (auto& m : m_pendingMoveOrders) {
        nlohmann::json entry;
        entry["fromProvince"] = m.fromProvince;
        entry["toProvince"] = m.toProvince;
        entry["pct"] = m.pct;
        entry["countryId"] = m.countryId;
        j["pendingMoveOrders"].push_back(entry);
    }
    for (auto& d : m_pendingDisbandOrders) {
        nlohmann::json entry;
        entry["provinceId"] = d.provinceId;
        entry["count"] = d.count;
        j["pendingDisbandOrders"].push_back(entry);
    }
    for (auto& sb : m_pendingShipBuilds) {
        nlohmann::json entry;
        entry["provinceId"] = sb.provinceId;
        entry["type"] = sb.type;
        entry["turnsRemaining"] = sb.turnsRemaining;
        j["pendingShipBuilds"].push_back(entry);
    }
    for (auto& ss : m_pendingScrapShips) {
        nlohmann::json entry;
        entry["shipIndex"] = ss.shipIndex;
        j["pendingScrapShips"].push_back(entry);
    }
    for (auto& e : m_pendingEmbarkations) {
        nlohmann::json entry;
        entry["provinceId"] = e.provinceId;
        entry["count"] = e.count;
        entry["turnsRemaining"] = e.turnsRemaining;
        j["pendingEmbarkations"].push_back(entry);
    }
    for (auto& a : m_pendingArtilleryOrders) {
        nlohmann::json entry;
        entry["fromProvince"] = a.fromProvince;
        entry["targetProvince"] = a.targetProvince;
        entry["ammoType"] = a.ammoType;
        j["pendingArtilleryOrders"].push_back(entry);
    }
    for (auto& sm : m_pendingShipMoveOrders) {
        nlohmann::json entry;
        entry["shipIndex"] = sm.shipIndex;
        entry["destLon"] = sm.destLon;
        entry["destLat"] = sm.destLat;
        j["pendingShipMoveOrders"].push_back(entry);
    }
    for (auto& se : m_pendingShipEngageOrders) {
        nlohmann::json entry;
        entry["shipIndex"] = se.shipIndex;
        entry["targetIndex"] = se.targetIndex;
        j["pendingShipEngageOrders"].push_back(entry);
    }
    for (auto& sb : m_pendingShipBombardOrders) {
        nlohmann::json entry;
        entry["shipIndex"] = sb.shipIndex;
        entry["targetProvince"] = sb.targetProvince;
        entry["ammoType"] = sb.ammoType;
        j["pendingShipBombardOrders"].push_back(entry);
    }
    for (auto& sd : m_pendingShipDisembarks) {
        nlohmann::json entry;
        entry["shipIndex"] = sd.shipIndex;
        entry["targetProvince"] = sd.targetProvince;
        j["pendingShipDisembarks"].push_back(entry);
    }
    for (auto& da : m_pendingDiplomaticActions) {
        nlohmann::json entry;
        entry["sourceIso"] = da.sourceIso;
        entry["targetIso"] = da.targetIso;
        entry["action"] = da.action;
        entry["turnsRemaining"] = da.turnsRemaining;
        j["pendingDiplomaticActions"].push_back(entry);
    }

    // Active policies
    for (auto& ap : m_activePolicies) {
        nlohmann::json entry;
        entry["policyId"] = ap.policyId;
        entry["countryId"] = ap.countryId;
        entry["turnsRemaining"] = ap.turnsRemaining;
        entry["targetProvince"] = ap.targetProvince;
        entry["targetMinority"] = ap.targetMinority;
        j["activePolicies"].push_back(entry);
    }

    // Research
    for (auto& [cid, researched] : m_countryResearched) {
        for (auto& nodeId : researched) {
            nlohmann::json entry;
            entry["countryId"] = cid;
            entry["nodeId"] = nodeId;
            j["researched"].push_back(entry);
        }
    }

    // Balances
    for (auto& [cid, bal] : m_countryBalances) {
        j["balances"][std::to_string(cid)] = bal;
    }

    // Minority alignment drift
    for (auto& [name, drift] : m_minorityAlignmentDrift) {
        j["alignmentDrift"][name] = drift;
    }

    // Claims pending
    for (auto& pid : m_claimsPendingAdd) {
        j["claimsPendingAdd"].push_back(pid);
    }
    for (auto& pid : m_claimsPendingDrop) {
        j["claimsPendingDrop"].push_back(pid);
    }

    // Province conquest tracking
    for (auto& [pid, turn] : m_provinceConquestTurn) {
        j["provinceConquestTurn"][std::to_string(pid)] = turn;
    }
    for (auto& [pid, prevOwner] : m_conqueredProvincePrevOwner) {
        j["conqueredProvincePrevOwner"][std::to_string(pid)] = prevOwner;
    }

    // Pacification
    for (auto& [cid, val] : m_countryPacification) {
        j["pacification"][std::to_string(cid)] = val;
    }

    // Turn number
    j["turnNumber"] = m_turnNumber;

    return j.dump();
}

void Game::loadStateJson(const std::string& json) {
    if (json.empty()) return;

    nlohmann::json j = nlohmann::json::parse(json);

    // Pending upgrades
    if (j.contains("pendingUpgrades")) {
        for (auto& entry : j["pendingUpgrades"]) {
            PendingUpgrade u;
            u.provinceId = entry["provinceId"];
            u.type = entry["type"].get<std::string>();
            u.targetLevel = entry["targetLevel"];
            u.turnsRemaining = entry.value("turnsRemaining", 0);
            m_pendingUpgrades.push_back(u);
        }
    }

    // Pending specializations
    if (j.contains("pendingSpecializations")) {
        for (auto& entry : j["pendingSpecializations"]) {
            PendingSpecialization s;
            s.provinceId = entry["provinceId"];
            s.specialization = entry["specialization"].get<std::string>();
            s.turnsRemaining = entry.value("turnsRemaining", 3);
            m_pendingSpecializations.push_back(s);
        }
    }

    // Pending recruitments
    if (j.contains("pendingRecruitments")) {
        for (auto& entry : j["pendingRecruitments"]) {
            PendingRecruitment r;
            r.provinceId = entry["provinceId"];
            r.count = entry["count"];
            r.turnsRemaining = entry.value("turnsRemaining", 1);
            m_pendingRecruitments.push_back(r);
        }
    }

    // Pending move orders
    if (j.contains("pendingMoveOrders")) {
        for (auto& entry : j["pendingMoveOrders"]) {
            PendingMoveOrder m;
            m.fromProvince = entry["fromProvince"];
            m.toProvince = entry["toProvince"];
            m.pct = entry.value("pct", 50);
            m.countryId = entry.value("countryId", 0);
            m_pendingMoveOrders.push_back(m);
        }
    }

    // Pending disband orders
    if (j.contains("pendingDisbandOrders")) {
        for (auto& entry : j["pendingDisbandOrders"]) {
            PendingDisbandOrder d;
            d.provinceId = entry["provinceId"];
            d.count = entry.value("count", 0);
            m_pendingDisbandOrders.push_back(d);
        }
    }

    // Pending ship builds
    if (j.contains("pendingShipBuilds")) {
        for (auto& entry : j["pendingShipBuilds"]) {
            PendingShipBuild sb;
            sb.provinceId = entry["provinceId"];
            sb.type = entry["type"].get<std::string>();
            sb.turnsRemaining = entry.value("turnsRemaining", 3);
            m_pendingShipBuilds.push_back(sb);
        }
    }

    // Pending scrap ships
    if (j.contains("pendingScrapShips")) {
        for (auto& entry : j["pendingScrapShips"]) {
            PendingScrapShip ss;
            ss.shipIndex = entry["shipIndex"];
            m_pendingScrapShips.push_back(ss);
        }
    }

    // Pending embarkations
    if (j.contains("pendingEmbarkations")) {
        for (auto& entry : j["pendingEmbarkations"]) {
            PendingEmbark e;
            e.provinceId = entry["provinceId"];
            e.count = entry["count"];
            e.turnsRemaining = entry.value("turnsRemaining", 1);
            m_pendingEmbarkations.push_back(e);
        }
    }

    // Pending artillery orders
    if (j.contains("pendingArtilleryOrders")) {
        for (auto& entry : j["pendingArtilleryOrders"]) {
            PendingArtilleryOrder a;
            a.fromProvince = entry["fromProvince"];
            a.targetProvince = entry["targetProvince"];
            a.ammoType = entry["ammoType"].get<std::string>();
            m_pendingArtilleryOrders.push_back(a);
        }
    }

    // Pending ship move orders
    if (j.contains("pendingShipMoveOrders")) {
        for (auto& entry : j["pendingShipMoveOrders"]) {
            PendingShipMoveOrder sm;
            sm.shipIndex = entry["shipIndex"];
            sm.destLon = entry["destLon"];
            sm.destLat = entry["destLat"];
            m_pendingShipMoveOrders.push_back(sm);
        }
    }

    // Pending ship engage orders
    if (j.contains("pendingShipEngageOrders")) {
        for (auto& entry : j["pendingShipEngageOrders"]) {
            PendingShipEngageOrder se;
            se.shipIndex = entry["shipIndex"];
            se.targetIndex = entry["targetIndex"];
            m_pendingShipEngageOrders.push_back(se);
        }
    }

    // Pending ship bombard orders
    if (j.contains("pendingShipBombardOrders")) {
        for (auto& entry : j["pendingShipBombardOrders"]) {
            PendingShipBombardOrder sb;
            sb.shipIndex = entry["shipIndex"];
            sb.targetProvince = entry["targetProvince"];
            sb.ammoType = entry["ammoType"].get<std::string>();
            m_pendingShipBombardOrders.push_back(sb);
        }
    }

    // Pending ship disembarks
    if (j.contains("pendingShipDisembarks")) {
        for (auto& entry : j["pendingShipDisembarks"]) {
            PendingShipDisembark sd;
            sd.shipIndex = entry["shipIndex"];
            sd.targetProvince = entry["targetProvince"];
            m_pendingShipDisembarks.push_back(sd);
        }
    }

    // Pending diplomatic actions
    if (j.contains("pendingDiplomaticActions")) {
        for (auto& entry : j["pendingDiplomaticActions"]) {
            PendingDiplomaticAction da;
            da.sourceIso = entry["sourceIso"].get<std::string>();
            da.targetIso = entry["targetIso"].get<std::string>();
            da.action = entry["action"].get<std::string>();
            da.turnsRemaining = entry.value("turnsRemaining", 1);
            m_pendingDiplomaticActions.push_back(da);
        }
    }

    // Active policies
    if (j.contains("activePolicies")) {
        for (auto& entry : j["activePolicies"]) {
            ActivePolicy ap;
            ap.policyId = entry["policyId"].get<std::string>();
            ap.countryId = entry["countryId"];
            ap.turnsRemaining = entry.value("turnsRemaining", 0);
            ap.targetProvince = entry.value("targetProvince", -1);
            ap.targetMinority = entry.value("targetMinority", "");
            m_activePolicies.push_back(ap);
            m_countryActivePolicyIndices[ap.countryId].push_back((int)m_activePolicies.size() - 1);
        }
    }

    // Research
    if (j.contains("researched")) {
        for (auto& entry : j["researched"]) {
            int cid = entry["countryId"];
            std::string nodeId = entry["nodeId"].get<std::string>();
            m_countryResearched[cid].insert(nodeId);
        }
    }

    // Balances
    if (j.contains("balances")) {
        for (auto& [key, val] : j["balances"].items()) {
            m_countryBalances[std::stoi(key)] = val.get<float>();
        }
    }

    // Minority alignment drift
    if (j.contains("alignmentDrift")) {
        for (auto& [key, val] : j["alignmentDrift"].items()) {
            m_minorityAlignmentDrift[key] = val.get<float>();
        }
    }

    // Claims pending
    if (j.contains("claimsPendingAdd")) {
        for (auto& pid : j["claimsPendingAdd"]) {
            m_claimsPendingAdd.push_back(pid.get<int>());
        }
    }
    if (j.contains("claimsPendingDrop")) {
        for (auto& pid : j["claimsPendingDrop"]) {
            m_claimsPendingDrop.push_back(pid.get<int>());
        }
    }

    // Province conquest tracking
    if (j.contains("provinceConquestTurn")) {
        for (auto& [key, val] : j["provinceConquestTurn"].items()) {
            m_provinceConquestTurn[std::stoi(key)] = val.get<int>();
        }
    }
    if (j.contains("conqueredProvincePrevOwner")) {
        for (auto& [key, val] : j["conqueredProvincePrevOwner"].items()) {
            m_conqueredProvincePrevOwner[std::stoi(key)] = val.get<int>();
        }
    }

    // Pacification
    if (j.contains("pacification")) {
        for (auto& [key, val] : j["pacification"].items()) {
            m_countryPacification[std::stoi(key)] = val.get<float>();
        }
    }

    // Turn number
    if (j.contains("turnNumber")) {
        m_turnNumber = j["turnNumber"];
    }
}
