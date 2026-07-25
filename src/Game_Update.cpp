#include "Game.h"
#include "GameInternals.h"
#include "Keybinds.h"
#include "renderer/FlagRenderer.h"
#include "raymath.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdio>
#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <ctime>

void Game::handlePauseMenu() {
    if (isMouseOverConsole()) return;
    int count = MENU_COUNT;
    int itemH = 50;
    int centerX = m_screenW / 2;
    int centerY = m_screenH / 2;
    int startY = centerY - (count * itemH) / 2;

    Vector2 mouse = getMouse();
    int hovered = -1;
    for (int i = 0; i < count; ++i) {
        int y = startY + i * itemH;
        int tw = MeasureText(MENU_ITEMS[i], 30);
        if (CheckCollisionPointRec(mouse, { (float)(centerX - tw/2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) }))
            { hovered = i; break; }
    }

    if (IsKeyPressed(KEY_UP)) m_menuIndex = (m_menuIndex + count - 1) % count;
    if (IsKeyPressed(KEY_DOWN)) m_menuIndex = (m_menuIndex + 1) % count;

    bool activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && hovered >= 0) {
        m_menuIndex = hovered;
        activate = true;
    }

    if (activate) {
        m_menuIndex = std::clamp(m_menuIndex, 0, MENU_COUNT - 1);
        if (m_menuIndex == 0) m_paused = false;
        else if (m_menuIndex == 1) { m_inSettings = true; m_settingsIndex = 0; m_settingsScroll = 0; }
        else if (m_menuIndex == 2) {
            m_config.save(m_configPath);
            trySaveGame();
        }
        else if (m_menuIndex == 3) {
            if (m_unsavedChanges) {
                m_showUnsavedWarning = true;
                m_unsavedChoice = 0;
            } else {
                unloadGameData();
                m_paused = false;
                m_currentScreen = SCREEN_MENU;
            }
        }
    }
}

void Game::update(float dt) {
    // Turn history takes all input while open (it draws over the pause menu)
    if (m_inHistory) { updateHistoryScreen(); return; }
    int newW = GetScreenWidth();
    int newH = GetScreenHeight();
    if (newW != m_screenW || newH != m_screenH) {
        m_screenW = newW;
        m_screenH = newH;
        m_renderer->resize(m_screenW, m_screenH);
    }
    // Keybind capture mode — freezes all other input
    if (m_waitingForKey) {
        int pressed = GetKeyPressed();
        if (pressed != 0) {
            if (pressed == KEY_ESCAPE) {
                // Escape cancels rebinding
            } else if (pressed == KEY_BACKSPACE || pressed == KEY_DELETE) {
                // Backspace/Delete clears the binding
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

    // Block updates and handle ESC when ceasefire screen is active
    if (m_inCeasefireScreen) {
        updateCeasefireScreen();
        return;
    }

    // Handle ESC in economy overlay
    if (m_inEconomy && IsKeyPressed(KEY_ESCAPE)) {
        m_inEconomy = false;
        m_activeSidebarTab = 0;
        m_economyTab = 0;
        m_economyShowWorst = false;
        m_economyScroll = 0;
        m_economyExpScroll = 0;
        m_economyGrossScroll = 0;
        return;
    }

    // Handle ESC in research panel overlay
    if (m_inResearch && IsKeyPressed(KEY_ESCAPE)) {
        m_inResearch = false;
        m_inPolitics = false;
        m_activeSidebarTab = 0;
        if (m_renderer) m_renderer->setPaused(false);
        return;
    }

    // Handle ESC in claims panel overlay (before main ESC handler)
    if (m_inClaims && IsKeyPressed(KEY_ESCAPE)) {
        m_inClaims = false;
        m_activeSidebarTab = 0;
        m_claimsEditMode = false;
        m_claimsEditToAdd.clear();
        m_claimsEditToDrop.clear();
        m_showClaims = false;
        if (m_renderer) {
            m_renderer->setShowClaims(false);
            m_renderer->setPaused(false);
        }
        std::fill(m_claimsPixelBuffer.begin(), m_claimsPixelBuffer.end(), Color{0, 0, 0, 0});
        m_renderer->updateClaimsTexture(m_claimsPixelBuffer.data());
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (m_editingValue) {
            m_editingValue = false;
        } else if (m_paused && m_inSettings) {
            m_inSettings = false;
            m_settingsScroll = 0;
            m_config.save(m_configPath);
            // If no renderer, we came from main menu -> go back
            if (!m_renderer) {
                m_currentScreen = SCREEN_MENU;
                m_paused = false;
            }
        } else {
            m_paused = !m_paused;
            m_menuIndex = 0;
            m_settingsIndex = 0;
        }
    }

    // ─── View setup: always run regardless of turn state ───
    if (m_renderer) {
        m_renderer->setShowCountryNames(m_activeViewTab == 8);
        m_renderer->setShowPopulation(m_activeViewTab == 1);
        m_renderer->setShowIndustry(m_activeViewTab == 2);
        m_renderer->setShowRelations(m_activeViewTab == 4);
        m_renderer->setShowResource(m_activeViewTab == 7 ? m_activeResourceIdx : -1);
    }

    // Reset overlay pixel buffer when entering a view that uses it
    {
        static int lastOverlayTab = 0;
        int overlayTab = (m_activeViewTab == 1 || m_activeViewTab == 4) ? m_activeViewTab : (m_activeViewTab == 7 ? 7 : 0);
        if (overlayTab != lastOverlayTab) {
            lastOverlayTab = overlayTab;
            if (m_activeViewTab == 1 || m_activeViewTab == 4) {
                m_lastPopCountryId = -1;
                m_lastRelationsCountryId = -1;
                Color landColor = (m_activeViewTab == 4) ? Color{80, 80, 80, 255} : Color{60, 60, 60, 255};
                m_countryRelationColors.assign(m_countryRelationColors.size(), landColor);
                int w = m_provinces.getWidth();
                int h = m_provinces.getHeight();
                const auto* srcPixels = (const Color*)m_provinces.getImage().data;
                for (int i = 0; i < w * h; ++i) {
                    int pid = Province::colorToId(srcPixels[i].r, srcPixels[i].g, srcPixels[i].b);
                    m_populationPixelBuffer[i] = (pid == 0)
                        ? Color{10, 15, 40, 255}
                        : landColor;
                }
                m_renderer->updatePopulationTexture(m_populationPixelBuffer.data());
            }
        }
    }

    // Regenerate resource texture when entering resource view or switching resource
    {
        static int lastResTab = 0;
        static int lastResIdx = -1;
        int isRes = (m_activeViewTab == 7);
        int resIdx = isRes ? m_activeResourceIdx : -1;
        if (resIdx != lastResIdx || isRes != lastResTab) {
            lastResTab = isRes;
            lastResIdx = resIdx;
            if (isRes) generateResourceTexture();
        }
    }

    int barW = std::min(880, m_screenW - 32);
    int barH = 80;
    int barX = m_screenW - barW - 16;
    int barY = m_screenH - barH - 16;
    Rectangle bpr = {(float)barX, (float)barY, (float)barW, (float)barH};
    m_renderer->setBottomPanelRect(bpr);

    // Compute resource buttons rect to block province selection through buttons
    Rectangle resBtnRect{0, 0, 0, 0};
    if (m_activeViewTab == 7) {
        int dateH_rb = 20 + 6 * 2;
        int btnY_rb = barY - dateH_rb - 4;
        int totalBtnW = 5 * 80 + 4 * 4;
        resBtnRect = {(float)(barX + 8), (float)btnY_rb, (float)totalBtnW, (float)dateH_rb};
    }
    m_renderer->setSkipClickRect(resBtnRect);

    // Province info panel rect — block province selection through the panel
    {
        int panelY = 48;
        int panelH = std::min(m_screenH - 80 - 16 - 48, 700);
        Rectangle pRect = {0, (float)panelY, 360, (float)panelH};
        if (m_renderer->getSelectedProvinceId() <= 0 && m_selectedShipIndices.empty()) pRect.height = 0;
        m_renderer->setProvincePanelRect(pRect);
    }

    // Track any player interaction as an unsaved change
    if (!m_paused && !m_unsavedChanges &&
        (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ||
         GetMouseWheelMove() != 0 || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
         IsKeyPressed(KEY_TAB))) {
        trackChange();
    }

    // Politics overlay: block map interaction
    if (m_inPolitics) {
        if (m_renderer) {
            m_renderer->setPaused(true);
            m_renderer->update(dt);
        }
        updatePoliciesTab();
        return;
    }

    // Economy overlay: block map interaction
    if (m_inEconomy) {
        if (m_renderer) {
            m_renderer->setPaused(true);
            m_renderer->update(dt);
        }
        updateEconomy();
        return;
    }

    // Research panel: block map interaction
    if (m_inResearch) {
        if (m_renderer) {
            m_renderer->setPaused(true);
            m_renderer->update(dt);
        }
        return;
    }

    // Claims panel: block sidebar clicks and main map interaction
    if (m_inClaims) {
        if (m_renderer) {
            m_renderer->setPaused(true);
            m_renderer->update(dt);
        }
        // Handle scroll wheel for the claims list
        float wheel = GetMouseWheelMove();
        if (wheel != 0) {
            int listH = std::min(m_screenH - 250, 320);
            int contentH = 0;
            const Country* pc = m_countries.getCountry(m_playerCountryId);
            if (pc && m_claimsTab == 0) {
                auto it = m_claims.find(pc->isoA3);
                int n = (int)(it != m_claims.end() ? it->second.size() : 0);
                contentH = (m_claimsEditMode ? n + 4 : n) * 22 + 40;
            } else if (pc && m_claimsTab == 1) {
                contentH = (int)m_claimsPovList.size() * 22 + 40;
            } else if (pc && m_claimsTab == 2) {
                auto it = m_claims.find(pc->isoA3);
                if (it != m_claims.end()) {
                    for (int pid : it->second) {
                        auto cpIt = m_claimsByProvince.find(pid);
                        if (cpIt != m_claimsByProvince.end()) {
                            for (const std::string& ci : cpIt->second) {
                                if (ci != pc->isoA3) { contentH += 22; break; }
                            }
                        }
                    }
                }
                contentH += 40;
            }
            int maxScroll = std::max(0, contentH - listH);
            m_claimsScroll = std::clamp(m_claimsScroll - (int)(wheel * 24), 0, maxScroll);
        }
        // Inline map panning (drag) + province selection in edit mode
        {
            int listW = std::min(380, m_screenW / 3);
            int mapTexW = m_provinces.getWidth();
            int mapTexH = m_provinces.getHeight();
            int mapW = m_screenW - listW - 40;
            int mapX = 20;
            int mapY = 155;
            int mapH = std::min(m_screenH - mapY - 220, 320);
            if (mapH < 120) mapH = 120;

            // Compute letterboxed draw rect (same as in drawClaimsTab)
            const Country* pc = m_countries.getCountry(m_playerCountryId);
            if (pc) {
                auto getRelPids = [&]() -> std::vector<int> {
                    std::vector<int> pids;
                    if (m_claimsTab == 0) {
                        auto it = m_claims.find(pc->isoA3);
                        if (it != m_claims.end()) pids = it->second;
                        for (auto& [pid, prov] : m_provinces.getAllProvinces())
                            if (prov.countryId == m_playerCountryId) pids.push_back(pid);
                        if (m_claimsEditMode) {
                            for (int pid : m_claimsEditToAdd) pids.push_back(pid);
                            for (int pid : m_claimsEditToDrop) {
                                auto cp = std::find(pids.begin(), pids.end(), pid);
                                if (cp != pids.end()) pids.erase(cp);
                            }
                        }
                    } else if (m_claimsTab == 1 && !m_claimsPovList.empty()) {
                        int povIdx = std::min(m_claimsPovIndex, (int)m_claimsPovList.size() - 1);
                        auto it = m_claims.find(m_claimsPovList[povIdx]);
                        if (it != m_claims.end())
                            for (int pid : it->second) {
                                auto ppIt = m_provinces.getAllProvinces().find(pid);
                                if (ppIt != m_provinces.getAllProvinces().end() && ppIt->second.countryId == m_playerCountryId)
                                    pids.push_back(pid);
                            }
                        for (auto& [pid, prov] : m_provinces.getAllProvinces())
                            if (prov.countryId == m_playerCountryId) pids.push_back(pid);
                    } else if (m_claimsTab == 2) {
                        auto myIt = m_claims.find(pc->isoA3);
                        if (myIt != m_claims.end())
                            for (int pid : myIt->second) {
                                auto cpIt = m_claimsByProvince.find(pid);
                                if (cpIt != m_claimsByProvince.end())
                                    for (const std::string& ci : cpIt->second)
                                        if (ci != pc->isoA3) { pids.push_back(pid); break; }
                            }
                        for (auto& [pid, prov] : m_provinces.getAllProvinces())
                            if (prov.countryId == m_playerCountryId) pids.push_back(pid);
                    }
                    return pids;
                };
                std::vector<int> rpids = getRelPids();
                int minPx = 0, maxPx = mapTexW - 1, minPy = 0, maxPy = mapTexH - 1;
                bool hasBounds = false;
                for (int pid : rpids) {
                    auto ppIt = m_provincePixels.find(pid);
                    if (ppIt == m_provincePixels.end() || ppIt->second.empty()) continue;
                    for (int idx : ppIt->second) {
                        int px = idx % mapTexW, py = idx / mapTexW;
                        if (!hasBounds) { minPx = maxPx = px; minPy = maxPy = py; hasBounds = true; }
                        else { if (px < minPx) minPx = px; if (px > maxPx) maxPx = px; if (py < minPy) minPy = py; if (py > maxPy) maxPy = py; }
                    }
                }
                int padX = hasBounds ? std::max((maxPx - minPx) / 6, 50) : 0;
                int padY = hasBounds ? std::max((maxPy - minPy) / 6, 50) : 0;
                int baseSrcX = hasBounds ? std::max(0, minPx - padX) : 0;
                int baseSrcY = hasBounds ? std::max(0, minPy - padY) : 0;
                int srcW = hasBounds ? std::min(mapTexW - baseSrcX, maxPx - minPx + 2 * padX) : mapTexW;
                int srcH = hasBounds ? std::min(mapTexH - baseSrcY, maxPy - minPy + 2 * padY) : mapTexH;
                if (!hasBounds) { baseSrcX = 0; baseSrcY = 0; srcW = mapTexW; srcH = mapTexH; }
                int srcX = std::clamp(baseSrcX + m_claimsMapSrcX, 0, mapTexW - srcW);
                int srcY = std::clamp(baseSrcY + m_claimsMapSrcY, 0, mapTexH - srcH);

                // Letterboxed destination rect
                float srcAspect = (float)srcW / srcH;
                float dstAspect = (float)mapW / mapH;
                int iDrawX, iDrawY, iDrawW, iDrawH;
                if (srcAspect > dstAspect) {
                    iDrawW = mapW;
                    iDrawH = (int)(mapW / srcAspect);
                    iDrawX = mapX;
                    iDrawY = mapY + (mapH - iDrawH) / 2;
                } else {
                    iDrawH = mapH;
                    iDrawW = (int)(mapH * srcAspect);
                    iDrawX = mapX + (mapW - iDrawW) / 2;
                    iDrawY = mapY;
                }

                Rectangle drawRect = {(float)iDrawX, (float)iDrawY, (float)iDrawW, (float)iDrawH};
                Vector2 mp = getMouse();

                // Drag to pan (only on the actual drawn map area, not pillarbox)
                if (CheckCollisionPointRec(mp, drawRect)) {
                    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !m_claimsEditMode) {
                        m_claimsMapDragging = true;
                        m_claimsMapDragPrevX = (int)mp.x;
                        m_claimsMapDragPrevY = (int)mp.y;
                    }
                }
                if (m_claimsMapDragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    int dx = (int)mp.x - m_claimsMapDragPrevX;
                    int dy = (int)mp.y - m_claimsMapDragPrevY;
                    float scaleX = (float)srcW / (iDrawW * m_claimsMapZoom);
                    float scaleY = (float)srcH / (iDrawH * m_claimsMapZoom);
                    m_claimsMapSrcX -= (int)(dx * scaleX);
                    m_claimsMapSrcY -= (int)(dy * scaleY);
                    m_claimsMapDragPrevX = (int)mp.x;
                    m_claimsMapDragPrevY = (int)mp.y;
                }
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                    m_claimsMapDragging = false;

                // Zoom with mouse wheel (towards cursor)
                float wheelClaims = GetMouseWheelMove();
                if (wheelClaims != 0 && !m_claimsEditMode) {
                    float oldZoom = m_claimsMapZoom;
                    m_claimsMapZoom *= (wheelClaims > 0) ? 1.15f : 0.87f;
                    if (m_claimsMapZoom < 1.0f) m_claimsMapZoom = 1.0f;
                    if (m_claimsMapZoom > 5.0f) m_claimsMapZoom = 5.0f;
                    float factor = oldZoom / m_claimsMapZoom;
                    float mx = (mp.x - iDrawX) / iDrawW - 0.5f;
                    float my = (mp.y - iDrawY) / iDrawH - 0.5f;
                    m_claimsMapSrcX += (int)(mx * srcW * (1.0f - factor));
                    m_claimsMapSrcY += (int)(my * srcH * (1.0f - factor));
                }

                // In edit mode: click on inline map to add/drop provinces
                if (m_claimsEditMode && !m_claimsMapDragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mp, drawRect)) {
                    float z = m_claimsMapZoom;
                    if (z < 1.0f) z = 1.0f;
                    if (z > 5.0f) z = 5.0f;
                    int zoomedW = std::max((int)(srcW / z), 10);
                    int zoomedH = std::max((int)(srcH / z), 10);
                    int zoomShiftX = (srcW - zoomedW) / 2;
                    int zoomShiftY = (srcH - zoomedH) / 2;
                    int clickSrcX = baseSrcX + m_claimsMapSrcX + zoomShiftX;
                    int clickSrcY = baseSrcY + m_claimsMapSrcY + zoomShiftY;
                    clickSrcX = std::clamp(clickSrcX, 0, mapTexW - zoomedW);
                    clickSrcY = std::clamp(clickSrcY, 0, mapTexH - zoomedH);
                    int px = clickSrcX + (int)((mp.x - iDrawX) * zoomedW / iDrawW);
                    int py = clickSrcY + (int)((mp.y - iDrawY) * zoomedH / iDrawH);
                    if (px >= 0 && px < mapTexW && py >= 0 && py < mapTexH) {
                        const Province* prov = m_provinces.getProvince(px, py);
                        if (prov && prov->countryId != UNC_CID && prov->countryId != BLC_CID && prov->countryId != m_playerCountryId) {
                            int pid = prov->id;
                            auto& claims = m_claims[pc->isoA3];
                            bool isClaimed = std::find(claims.begin(), claims.end(), pid) != claims.end();
                            bool inAdd = std::find(m_claimsEditToAdd.begin(), m_claimsEditToAdd.end(), pid) != m_claimsEditToAdd.end();
                            bool inDrop = std::find(m_claimsEditToDrop.begin(), m_claimsEditToDrop.end(), pid) != m_claimsEditToDrop.end();
                            if (isClaimed && !inDrop) m_claimsEditToDrop.push_back(pid);
                            else if (isClaimed && inDrop) m_claimsEditToDrop.erase(std::remove(m_claimsEditToDrop.begin(), m_claimsEditToDrop.end(), pid), m_claimsEditToDrop.end());
                            else if (!isClaimed && !inAdd) m_claimsEditToAdd.push_back(pid);
                            else if (!isClaimed && inAdd) m_claimsEditToAdd.erase(std::remove(m_claimsEditToAdd.begin(), m_claimsEditToAdd.end(), pid), m_claimsEditToAdd.end());
                            m_claimsOverlayDirty = true;
                        }
                    }
                }
            }
        }
        // ESC is already handled above in the main ESC handler
        return;
    }

    // Keyboard shortcuts to switch view tabs (from keybinds or sidebar click)
    if (!m_paused && !m_inEconomy && !m_inResearch && !m_inClaims) {
        static const int tabActions[] = {ACTION_TAB_1, ACTION_TAB_2, ACTION_TAB_3, ACTION_TAB_4,
                                         ACTION_TAB_5, ACTION_TAB_6, ACTION_TAB_7, ACTION_TAB_8};
        for (int i = 0; i < 8; ++i) {
            if (IsKeyPressed(m_config.keybinds[tabActions[i]])) {
                int tab = i + 1;
                m_activeViewTab = (m_activeViewTab == tab) ? 0 : tab;
                if (m_showClaims) clearClaimsView();
                break;
            }
        }
    }
    if (!m_paused && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        Vector2 mp = getMouse();
        if (mp.x >= barX && mp.x < barX + barW && mp.y >= barY && mp.y < barY + barH) {
            int buttonStartX = barX + 8;
            int buttonEndX = barX + barW - 8;
            int buttonW = (buttonEndX - buttonStartX) / 8;
            int idx = ((int)mp.x - buttonStartX) / buttonW;
            if (idx >= 0 && idx < 8) {
                int tab = idx + 1;
                m_activeViewTab = (m_activeViewTab == tab) ? 0 : tab;
                // Clear claims view when switching tabs
                if (m_showClaims) clearClaimsView();
            }
        }
        // Resource selection buttons click (already inside IsMouseButtonReleased check)
        if (m_activeViewTab == 7) {
            Vector2 mp = getMouse();
            int barW2 = std::min(880, m_screenW - 32);
            int barX2 = m_screenW - barW2 - 16;
            int barY2 = m_screenH - 80 - 16;
            int dateH2 = 20 + 6 * 2;
            int btnY2 = barY2 - dateH2 - 4;
            if (mp.y >= btnY2 && mp.y < btnY2 + dateH2) {
                int btnW2 = 80;
                int btnStartX2 = barX2 + 8;
                int relX = (int)mp.x - btnStartX2;
                int idx = relX / (btnW2 + 4);
                if (idx >= 0 && idx < 5) {
                    int bx = btnStartX2 + idx * (btnW2 + 4);
                    if (mp.x >= bx && mp.x < bx + btnW2 && idx != m_activeResourceIdx) {
                        m_activeResourceIdx = idx;
                    }
                }
            }
        }
        // Sidebar buttons click
        {
            Vector2 mp = getMouse();
            bool isSpectator = (m_playerCountryId == SPC_CID);
            int btnSize = 100;
            int btnSpacing = 8;
            int startX = m_screenW - btnSize - 12;
            int totalH = 4 * btnSize + 3 * btnSpacing;
            int startY = (m_screenH - totalH) / 2;
            struct { int id; bool disabled; } sbtns[] = {{1, isSpectator}, {2, false}, {3, isSpectator}, {4, false}};
            for (int i = 0; i < 4; ++i) {
                if (sbtns[i].disabled) continue;
                Rectangle r = {(float)startX, (float)(startY + i * (btnSize + btnSpacing)),
                               (float)btnSize, (float)btnSize};
                if (CheckCollisionPointRec(mp, r)) {
                    int tid = sbtns[i].id;
                    if (tid == 3) {
                        // Claims — open/close claims panel
                        if (m_inClaims) {
                            // Close — keep pending claims alive until next turn
                            m_inClaims = false;
                            m_activeSidebarTab = 0;
                            m_claimsEditMode = false;
                            m_claimsEditToAdd.clear();
                            m_claimsEditToDrop.clear();
                            m_showClaims = false;
                            if (m_renderer) {
                                m_renderer->setShowClaims(false);
                                m_renderer->setPaused(false);
                            }
                            std::fill(m_claimsPixelBuffer.begin(), m_claimsPixelBuffer.end(), Color{0, 0, 0, 0});
                            m_renderer->updateClaimsTexture(m_claimsPixelBuffer.data());
                        } else {
                            // Open (close economy/policy first)
                            m_inEconomy = false;
                            m_inPolitics = false;
                            m_inClaims = true;
                            m_activeSidebarTab = 3;
                            m_claimsOverlayDirty = true;
                            if (m_renderer) m_renderer->setPaused(true);
                            m_claimsEditMode = false;
                            m_claimsEditToAdd.clear();
                            m_claimsEditToDrop.clear();
                            m_claimsPovList.clear();
                            m_claimsPovIndex = 0;
                            m_claimsMapSrcX = m_claimsMapSrcY = 0;
                            m_claimsMapDragging = false;
                            // Show claims on main map
                            m_showClaims = true;
                            m_renderer->setShowClaims(true);
                            if (m_playerCountryId > 0 && m_playerCountryId != SPC_CID) {
                                m_lastClaimsCountryId = m_playerCountryId;
                                generateClaimsTexture();
                            }
                            // Build POV list: countries that have claims on the player
                            if (m_playerCountryId > 0 && m_playerCountryId != SPC_CID) {
                                const Country* pc = m_countries.getCountry(m_playerCountryId);
                                if (pc) {
                                    for (auto& [iso, pids] : m_claims) {
                                        if (iso == pc->isoA3) continue;
                                        for (int pid : pids) {
                                            auto ppIt = m_provinces.getAllProvinces().find(pid);
                                            if (ppIt != m_provinces.getAllProvinces().end() && ppIt->second.countryId == m_playerCountryId) {
                                                m_claimsPovList.push_back(iso);
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    } else if (tid == 2) {
                        // Economy — open/close overlay (close claims first)
                        m_inClaims = false;
                        m_inPolitics = false;
                        m_inEconomy = !m_inEconomy;
                        m_activeSidebarTab = m_inEconomy ? 2 : 0;
                    } else if (tid == 1) {
                        // Politics — open/close policy overlay (close claims first)
                        m_inClaims = false;
                        m_inEconomy = false;
                        m_inPolitics = !m_inPolitics;
                        m_activeSidebarTab = m_inPolitics ? 1 : 0;
                        if (m_inPolitics) {
                            m_policyTab = 0;
                            m_policyScroll = 0;
                            m_claimsEditMode = false;
                            m_claimsEditToAdd.clear();
                            m_claimsEditToDrop.clear();
                            m_politicsAlert = false; // seen
                        }
                    } else if (tid == 4) {
                        // Research — open/close (close other panels first)
                        m_inClaims = false;
                        m_inEconomy = false;
                        m_inPolitics = false;
                        m_inResearch = !m_inResearch;
                        m_activeSidebarTab = m_inResearch ? 4 : 0;
                        if (m_inResearch) m_researchAlert = false; // seen
                        if (m_renderer) m_renderer->setPaused(m_inResearch);
                    }
                    break;
                }
            }
        }
    }

        if (!m_paused) {
            if (m_activeViewTab == 7) {
                if (IsKeyPressed(KEY_ONE)) m_activeResourceIdx = 0;
                if (IsKeyPressed(KEY_TWO)) m_activeResourceIdx = 1;
                if (IsKeyPressed(KEY_THREE)) m_activeResourceIdx = 2;
                if (IsKeyPressed(KEY_FOUR)) m_activeResourceIdx = 3;
                if (IsKeyPressed(KEY_FIVE)) m_activeResourceIdx = 4;
            }
        }

        if (!m_paused) {
            // Block left-click handling in MapRenderer briefly after Go-to click
            if (m_blockLeftPanTimer > 0) {
                m_blockLeftPanTimer--;
                m_renderer->setBlockLeftPan(true);
            } else if (m_armySliderActive) {
                m_renderer->setBlockLeftPan(true);
            } else if (m_armyMovePctSliderFrom) {
                m_renderer->setBlockLeftPan(true);
            } else if (m_activeViewTab == 6 && IsKeyDown(m_config.keybinds[ACTION_BOX_SELECT])) {
                m_renderer->setBlockLeftPan(true);
            } else {
                m_renderer->setBlockLeftPan(false);
            }
            m_renderer->setPaused(false);
            int sel = m_renderer->getSelectedProvinceId();

            // Clear claims view when selecting a non-involved province
            // (check before updating m_lastSelectedProvince so we detect the change)
            if (m_showClaims && sel > 0 && sel != m_lastSelectedProvince) {
                auto it = m_provinces.getAllProvinces().find(sel);
                if (it != m_provinces.getAllProvinces().end()) {
                    int cid = it->second.countryId;
                    if (cid != m_lastClaimsCountryId && !isCountryInvolvedInClaims(cid, m_lastClaimsCountryId))
                        clearClaimsView();
                }
            }

            if (sel != m_lastSelectedProvince) {
                m_lastSelectedProvince = sel;
                if (sel > 0) {
                    buildCountryProvinceList(sel);
                    // Selecting a province clears ship selection
                    m_selectedShipIndices.clear();
                    m_shipListFocusIndex = -1;
                }
            }
            if (m_lastSelectedProvince > 0) {
                if (IsKeyPressed(m_config.keybinds[ACTION_PREV_PROVINCE])) cycleProvince(-1);
                if (IsKeyPressed(m_config.keybinds[ACTION_NEXT_PROVINCE])) cycleProvince(1);
            }
            // Ship cycling in navy tab
            if (m_activeViewTab == 6 && !m_selectedShipIndices.empty()) {
                if (IsKeyPressed(m_config.keybinds[ACTION_PREV_PROVINCE])) cycleShip(-1);
                if (IsKeyPressed(m_config.keybinds[ACTION_NEXT_PROVINCE])) cycleShip(1);
            }

            // ─── Army move orders (keybindable drag, any non-popup view tab) ───
            if (m_turnState == TURN_NORMAL && !m_inEconomy && !m_inResearch && !m_inClaims) {
                int armyKey = m_config.keybinds[ACTION_ARMY_MOVE];
                bool btnDownThis = false;
                bool btnPressedThis = false;
                // Check both keyboard (≥32) and mouse (<32) triggers + always allow right-click
                if (armyKey >= 32) {
                    btnDownThis = IsKeyDown(armyKey);
                    btnPressedThis = IsKeyPressed(armyKey);
                } else {
                    btnDownThis = IsMouseButtonDown(armyKey);
                    btnPressedThis = IsMouseButtonPressed(armyKey);
                }
                btnDownThis = btnDownThis || IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
                btnPressedThis = btnPressedThis || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) || IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE);
                // Detect release: button was down last frame and is no longer down
                bool btnReleasedThis = m_armyMoveDragBtnDown && !btnDownThis;
                m_armyMoveDragBtnDown = btnDownThis;

                Vector2 mp = getMouse();
                int px, py;
                m_renderer->screenToPixel(mp.x, mp.y, px, py);
                const Province* hp = m_provinces.getProvince(px, py);
                int hoverPid = hp ? hp->id : -1;
                // Update hover province during drag (both while held AND on release frame)
                if (m_armyMoveDragActive) {
                    if (hoverPid == m_armyMoveDragSource || hoverPid <= 0) {
                        m_armyMoveDragHoverPid = -1;
                        m_armyMoveDragValidDest = false;
                    } else {
                        auto nIt = m_provinceNeighbors.find(m_armyMoveDragSource);
                        bool isNb = (nIt != m_provinceNeighbors.end()) &&
                            std::find(nIt->second.begin(), nIt->second.end(), hoverPid) != nIt->second.end();
                        m_armyMoveDragHoverPid = isNb ? hoverPid : -1;
                        // Validate destination
                        if (isNb) {
                            Province* dp = m_provinces.getProvinceById(hoverPid);
                            m_armyMoveDragValidDest = false;
                            if (dp) {
                                if (dp->countryId == UNC_CID) {
                                    m_armyMoveDragValidDest = true;
                                } else if (dp->countryId == m_playerCountryId) {
                                    m_armyMoveDragValidDest = true;
                                } else {
                                    const Country* pc = m_countries.getCountry(m_playerCountryId);
                                    const Country* dc = m_countries.getCountry(dp->countryId);
                                    if (pc && dc) {
                                        auto pr = m_relations.find(pc->isoA3);
                                        if (pr != m_relations.end()) {
                                            auto dr = pr->second.find(dc->isoA3);
                                            if (dr != pr->second.end())
                                                m_armyMoveDragValidDest = dr->second.war || dr->second.alliance;
                                        }
                                    }
                                }
                            }
                        } else {
                            m_armyMoveDragValidDest = false;
                        }
                    }
                }
                // Press: start drag on province with player's own troops (allied troops move alongside)
                if (btnPressedThis && !m_armyMoveDragActive) {
                    if (hp) {
                        auto aIt = m_provinceArmies.find(hp->id);
                        bool hasMyTroops = false;
                        if (aIt != m_provinceArmies.end()) {
                            for (auto& u : aIt->second)
                                if (u.countryId == m_playerCountryId && u.count > 0) { hasMyTroops = true; break; }
                        }
                        if (hasMyTroops) {
                            m_armyMoveDragSource = hp->id;
                            m_armyMoveDragActive = true;
                            m_armyMoveDragHoverPid = -1;
                            m_renderer->setSelectedProvince(hp->id);
                        }
                    }
                }
                // Release: create order, cancel, or abort
                if (btnReleasedThis && m_armyMoveDragActive) {
                    if (m_armyMoveDragHoverPid > 0 && m_armyMoveDragHoverPid != m_armyMoveDragSource) {
                        // Validate destination: same country, at war, or allied
                        bool validDest = false;
                        Province* destProv = m_provinces.getProvinceById(m_armyMoveDragHoverPid);
                        if (destProv) {
                            if (destProv->countryId == UNC_CID) {
                                validDest = true;
                            } else if (destProv->countryId == m_playerCountryId) {
                                validDest = true;
                            } else {
                                const Country* pc = m_countries.getCountry(m_playerCountryId);
                                const Country* dstC = m_countries.getCountry(destProv->countryId);
                                if (pc && dstC) {
                                    auto pr = m_relations.find(pc->isoA3);
                                    if (pr != m_relations.end()) {
                                        auto dr = pr->second.find(dstC->isoA3);
                                        if (dr != pr->second.end())
                                            if (dr->second.war || dr->second.alliance)
                                                validDest = true;
                                    }
                                }
                            }
                        }
                        if (validDest) {
                            // Drag ended on a valid neighbor
                            bool found = false;
                            for (auto it = m_pendingMoveOrders.begin(); it != m_pendingMoveOrders.end(); ++it) {
                                if (it->fromProvince == m_armyMoveDragSource && it->toProvince == m_armyMoveDragHoverPid) {
                                    m_pendingMoveOrders.erase(it);
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                int sumOthers = 0;
                                for (auto& om : m_pendingMoveOrders)
                                    if (om.fromProvince == m_armyMoveDragSource)
                                        sumOthers += om.pct;
                                int newPct = 50;
                                int maxPct = 100 - sumOthers;
                                if (maxPct < 1) maxPct = 1;
                                if (newPct > maxPct) newPct = maxPct;
                                m_pendingMoveOrders.push_back({m_armyMoveDragSource, m_armyMoveDragHoverPid, newPct, m_playerCountryId});
                            }
                        }
                    } else if (hoverPid == m_armyMoveDragSource && hoverPid > 0) {
                        // Released on same province → cancel ALL orders from this province
                        for (auto it = m_pendingMoveOrders.begin(); it != m_pendingMoveOrders.end(); ) {
                            if (it->fromProvince == m_armyMoveDragSource)
                                it = m_pendingMoveOrders.erase(it);
                            else ++it;
                        }
                    }
                    // Reset drag state
                    m_armyMoveDragSource = -1;
                    m_armyMoveDragActive = false;
                    m_armyMoveDragHoverPid = -1;
                }
                // ESC cancels drag
                if (IsKeyPressed(KEY_ESCAPE) && m_armyMoveDragActive) {
                    m_armyMoveDragSource = -1;
                    m_armyMoveDragActive = false;
                    m_armyMoveDragHoverPid = -1;
                    m_armyMoveDragBtnDown = false;
                }
            }

            // ─── Artillery Wheel Menu (hold key over province) ───
            if (m_turnState == TURN_NORMAL && m_activeViewTab == 5 && !m_paused && !m_inEconomy && !m_inResearch && !m_inClaims && !m_inPolitics) {
                int wheelKey = m_config.keybinds[ACTION_ARTILLERY_WHEEL];
                if (wheelKey >= 32 && wheelKey <= 350 && IsKeyPressed(wheelKey) && m_artillerySourceProvince < 0) {
                    Vector2 mp = getMouse();
                    int px, py;
                    m_renderer->screenToPixel(mp.x, mp.y, px, py);
                    const Province* hp = m_provinces.getProvince(px, py);
                    if (hp && hp->countryId == m_playerCountryId) {
                        m_artilleryWheelProvince = hp->id;
                        m_artilleryWheelHover = -1;
                    }
                }
                if (m_artilleryWheelProvince > 0) {
                    if (wheelKey >= 32 && wheelKey <= 350 && IsKeyDown(wheelKey)) {
                        Vector2 mp = getMouse();
                        auto cit = m_provinceCenters.find(m_artilleryWheelProvince);
                        if (cit != m_provinceCenters.end()) {
                            int mw = m_landSea.getWidth();
                            const Camera2D& cam2 = m_renderer->getCamera();
                            Vector2 wwp = cit->second;
                            while (wwp.x - cam2.target.x > mw * 0.5f) wwp.x -= mw;
                            while (wwp.x - cam2.target.x < -mw * 0.5f) wwp.x += mw;
                            Vector2 center = GetWorldToScreen2D(wwp, cam2);
                            float dx = mp.x - center.x, dy = mp.y - center.y;
                            float dist = sqrtf(dx*dx + dy*dy);
                            if (dist > 30.0f) {
                                float angle = atan2f(dy, dx);
                                if (angle < 0) angle += 2*PI;
                                m_artilleryWheelHover = ((int)((angle + PI*0.625f) / (PI*0.25f))) % 8;
                            } else {
                                m_artilleryWheelHover = -1;
                            }
                        }
                    } else {
                        // Key released — commit selection
                        if (m_artilleryWheelHover >= 0) {
                            static const char* TYPE_IDS[] = {"mortar","light","heavy","napalm","carpet","chemical","nuclear","biological"};
                            static const char* TYPE_NODES[] = {"arty1","arty2","arty3","arty4a","arty4b","arty5","arty6a","arty6b"};
                            if (hasResearched(TYPE_NODES[m_artilleryWheelHover])) {
                                m_artillerySourceProvince = m_artilleryWheelProvince;
                                m_artillerySelectedType = TYPE_IDS[m_artilleryWheelHover];
                                m_artilleryTargetPid = -1;
                            }
                        }
                        m_artilleryWheelProvince = -1;
                        m_artilleryWheelHover = -1;
                    }
                }
            }

            // ─── Artillery Targeting (left-click on neighbor province) ───
            if (m_artillerySourceProvince > 0) {
                // Block province selection while in artillery targeting mode
                m_renderer->setBlockLeftPan(true);
                if (!m_artillerySelectedType.empty()) {
                // ESC cancels artillery targeting
                if (IsKeyPressed(KEY_ESCAPE)) {
                    m_artillerySourceProvince = -1;
                    m_artillerySelectedType.clear();
                    m_artilleryTargetPid = -1;
                    m_blockLeftPanTimer = 2;
                }
                // Left-click on a province to target
                Vector2 _mpTarget = getMouse();
                bool _onPanel = (_mpTarget.x >= 0 && _mpTarget.x < 360 &&
                                 _mpTarget.y >= 68 && _mpTarget.y < 68 + std::min(m_screenH - 80 - 16 - 68, 700));
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !m_armyMoveDragActive && !_onPanel) {
                    static const struct { const char* id; float cost; } ARTY_COST[] = {
                        {"mortar",5},{"light",10},{"heavy",20},{"napalm",30},
                        {"carpet",25},{"chemical",40},{"nuclear",80},{"biological",60},{nullptr,0}
                    };
                    auto getArtyCost = [&](const std::string& tid) -> float {
                        for (int i = 0; ARTY_COST[i].id; ++i)
                            if (tid == ARTY_COST[i].id) return ARTY_COST[i].cost;
                        return 0;
                    };

                    Vector2 mp = getMouse();
                    int px, py;
                    m_renderer->screenToPixel(mp.x, mp.y, px, py);
                    const Province* hp = m_provinces.getProvince(px, py);
                    if (hp && hp->id != m_artillerySourceProvince && hp->id > 0) {
                        auto nIt = m_provinceNeighbors.find(m_artillerySourceProvince);
                        bool isNb = (nIt != m_provinceNeighbors.end()) &&
                            std::find(nIt->second.begin(), nIt->second.end(), hp->id) != nIt->second.end();
                        if (isNb) {
                            Province* _tgt = m_provinces.getProvinceById(hp->id);
                            bool canShoot = false;
                            if (_tgt && _tgt->countryId == m_playerCountryId) {
                                canShoot = true;
                            } else if (_tgt && _tgt->countryId == UNC_CID) {
                                canShoot = true;
                            } else if (_tgt && _tgt->countryId > 0) {
                                Province* _src = m_provinces.getProvinceById(m_artillerySourceProvince);
                                if (_src) {
                                    const Country* sc = m_countries.getCountry(_src->countryId);
                                    const Country* dc = m_countries.getCountry(_tgt->countryId);
                                    if (sc && dc) {
                                        auto sr = m_relations.find(sc->isoA3);
                                        if (sr != m_relations.end()) {
                                            auto dr = sr->second.find(dc->isoA3);
                                            if (dr != sr->second.end() && dr->second.war) canShoot = true;
                                        }
                                        auto sr2 = m_relations.find(dc->isoA3);
                                        if (sr2 != m_relations.end()) {
                                            auto dr2 = sr2->second.find(sc->isoA3);
                                            if (dr2 != sr2->second.end() && dr2->second.war) canShoot = true;
                                        }
                                    }
                                }
                            }
                            if (!canShoot) {
                                printf("[DIAG] Artillery target invalid — cancelling targeting\n");
                                m_artillerySourceProvince = -1;
                                m_artillerySelectedType.clear();
                                m_artilleryTargetPid = -1;
                                m_blockLeftPanTimer = 2;
                            } else {
                            double& treasury = m_countries.getAll()[m_playerCountryId].treasury;
                            float cost = getArtyCost(m_artillerySelectedType);
                            // Check if we already have an order for (source, target, type) — toggle it off
                            bool found = false;
                            for (auto it = m_pendingArtilleryOrders.begin(); it != m_pendingArtilleryOrders.end(); ) {
                                if (it->fromProvince == m_artillerySourceProvince && it->targetProvince == hp->id && it->ammoType == m_artillerySelectedType) {
                                    treasury += getArtyCost(it->ammoType);
                                    it = m_pendingArtilleryOrders.erase(it);
                                    found = true;
                                } else ++it;
                            }
                            if (!found) {
                                if (treasury >= cost) {
                                    // Cancel any other order from this source (different target/type)
                                    for (auto it = m_pendingArtilleryOrders.begin(); it != m_pendingArtilleryOrders.end(); ) {
                                        if (it->fromProvince == m_artillerySourceProvince) {
                                            treasury += getArtyCost(it->ammoType);
                                            it = m_pendingArtilleryOrders.erase(it);
                                        } else ++it;
                                    }
                                    treasury -= cost;
                                    m_pendingArtilleryOrders.push_back({m_artillerySourceProvince, hp->id, m_artillerySelectedType});
                                } else {
                                    printf("[DIAG] Not enough treasury for artillery strike ($%.0f needed, $%.0f available)\n", cost, treasury);
                                }
                            }
                            m_artillerySourceProvince = -1;
                            m_artillerySelectedType.clear();
                            m_artilleryTargetPid = -1;
                            m_blockLeftPanTimer = 2;
                            }
                        } else {
                            // Clicked on non-neighbor province — cancel targeting
                            m_artillerySourceProvince = -1;
                            m_artillerySelectedType.clear();
                            m_artilleryTargetPid = -1;
                            m_blockLeftPanTimer = 2;
                        }
                    } else {
                        // Clicked on empty space or non-neighbor — cancel targeting mode
                        m_artillerySourceProvince = -1;
                        m_artillerySelectedType.clear();
                        m_artilleryTargetPid = -1;
                        m_blockLeftPanTimer = 2;
                    }
                }
                } // end if !m_artillerySelectedType.empty()
            }

            // Regenerate population texture in Population view when selection or view changes
            if (m_activeViewTab == 1) {
                int curCid = 0;
                int sp = m_renderer->getSelectedProvinceId();
                if (sp > 0) {
                    auto it = m_provinces.getAllProvinces().find(sp);
                    if (it != m_provinces.getAllProvinces().end()) curCid = it->second.countryId;
                } else {
                    // No selection: use the country under the mouse cursor
                    Vector2 mp = getMouse();
                    int px, py;
                    m_renderer->screenToPixel(mp.x, mp.y, px, py);
                    const Province* hp = m_provinces.getProvince(px, py);
                    if (hp && hp->countryId > 0) curCid = hp->countryId;
                }
                if (curCid != m_lastPopCountryId) {
                    int oldCid = m_lastPopCountryId;
                    m_lastPopCountryId = curCid;
                    generatePopulationTexture(curCid, oldCid);
                }
            }

            // Regenerate relations texture in Relations view when selection or view changes
            if (m_activeViewTab == 4) {
                int curCid = 0;
                int sp = m_renderer->getSelectedProvinceId();
                if (sp > 0) {
                    auto it = m_provinces.getAllProvinces().find(sp);
                    if (it != m_provinces.getAllProvinces().end()) curCid = it->second.countryId;
                } else {
                    Vector2 mp = getMouse();
                    int px, py;
                    m_renderer->screenToPixel(mp.x, mp.y, px, py);
                    const Province* hp = m_provinces.getProvince(px, py);
                    if (hp && hp->countryId > 0) curCid = hp->countryId;
                }
                // Default to player's country when nothing selected/hovered
                if (curCid == 0) curCid = m_playerCountryId;
                if (curCid != m_lastRelationsCountryId) {
                    int oldCid = m_lastRelationsCountryId;
                    m_lastRelationsCountryId = curCid;
                    generateRelationsTexture(curCid, oldCid);
                }
            }

        // Zoom-to-province key
        if (IsKeyPressed(m_config.keybinds[ACTION_ZOOM_TO_PROVINCE]) && m_lastSelectedProvince > 0) {
            auto it2 = m_provinceCenters.find(m_lastSelectedProvince);
            if (it2 != m_provinceCenters.end()) {
                float r = m_provinceRadius[m_lastSelectedProvince];
                float targetZoom = std::min((float)m_screenW, (float)m_screenH) * 0.4f / std::max(r * 2.0f, 1.0f);
                float minZoom = std::max(m_screenW / (float)m_provinces.getWidth(),
                                         m_screenH / (float)m_provinces.getHeight());
                targetZoom = std::clamp(targetZoom, minZoom, 3.0f);
                m_renderer->flyTo(it2->second.x, it2->second.y, targetZoom, m_config.flySpeed);
            }
        }
        // Zoom-to-ship key (navy tab, space with no province selected)
        if (!m_selectedShipIndices.empty() && m_activeViewTab == 6 &&
            IsKeyPressed(m_config.keybinds[ACTION_ZOOM_TO_PROVINCE])) {
            // Compute bounding box of all selected ships (handles map wrap)
            float minPx = 1e9f, maxPx = -1e9f, minPy = 1e9f, maxPy = -1e9f;
            int mapW = m_provinces.getWidth();
            float refPx = -1.0f;
            for (int idx : m_selectedShipIndices) {
                auto& ship = m_ships[idx];
                int px = 0, py = 0;
                m_landSea.lonLatToPixel((float)ship.lon, (float)ship.lat, px, py);
                if (refPx < 0) refPx = (float)px;
                // Unwrap: if pixel is more than half a map away from reference, wrap it
                float upx = (float)px;
                while (upx - refPx > mapW / 2) upx -= mapW;
                while (upx - refPx < -mapW / 2) upx += mapW;
                if (upx < minPx) minPx = upx;
                if (upx > maxPx) maxPx = upx;
                if (py < minPy) minPy = (float)py;
                if (py > maxPy) maxPy = (float)py;
            }
            float cx = (minPx + maxPx) / 2.0f;
            float cy = (minPy + maxPy) / 2.0f;
            // Wrap center back into [0, mapW)
            while (cx < 0) cx += mapW;
            while (cx >= mapW) cx -= mapW;
            float bbW = maxPx - minPx + 40.0f;
            float bbH = maxPy - minPy + 40.0f;
            float availW = (float)(m_screenW - (m_renderer->getProvincePanelRect().height > 0 ? 370 : 30));
            float availH = (float)(m_screenH - 80);
            float zoomFit = std::min(availW / bbW, availH / bbH);
            float targetZoom = std::clamp(zoomFit, 0.5f, 5.0f);
            m_renderer->flyTo(cx, cy, targetZoom, m_config.flySpeed);
        }
        // Key-based zoom
        if (IsKeyPressed(m_config.keybinds[ACTION_ZOOM_IN])) m_renderer->addZoom(0.1f);
        if (IsKeyPressed(m_config.keybinds[ACTION_ZOOM_OUT])) m_renderer->addZoom(-0.1f);
        if (m_renderer) m_renderer->update(dt);
        return;
    }

    if (m_renderer) m_renderer->setPaused(true);

    if (m_inSettings) {
        if (isMouseOverConsole()) return;
        if (m_settingsTab < 0 || m_settingsTab >= TAB_COUNT) m_settingsTab = 0;
        const Setting* items = TAB_ITEMS[m_settingsTab];
        int count = TAB_ITEM_COUNTS[m_settingsTab];
        int itemH = 80;
        int centerX = m_screenW / 2;
        int centerY = m_screenH / 2;
        int fontSize = 30;
        int smFont = 24;

        // Scrollable list — fixed tab bar, fixed startY, items offset by scroll
        int tabY = 100;
        int startY = tabY + 70;
        int maxVisible = std::max(1, (m_screenH - startY - 20) / itemH);
        int maxScroll = std::max(0, count - maxVisible);
        m_settingsScroll = std::clamp(m_settingsScroll, 0, maxScroll);

        // Tab bar layout
        int tabSpacing = 200;
        int visibleTabs = 0;
        for (int t = 0; t < TAB_COUNT; ++t) {
            if (t == 4 && !m_config.debugMode) continue;
            ++visibleTabs;
        }
        int tabStartX = centerX - (visibleTabs * tabSpacing) / 2 + tabSpacing / 2;

        // Keybind search input (keybinds tab only, only when active)
        if (m_keybindFilterActive && m_settingsTab == 3 && !m_waitingForKey && !m_editingValue) {
            int c = GetCharPressed();
            while (c > 0) {
                if (c >= 32 && c <= 126) m_keybindFilter += (char)c;
                c = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && !m_keybindFilter.empty()) {
                m_keybindFilter.pop_back();
            }
            if (IsKeyPressed(KEY_ESCAPE)) {
                m_keybindFilterActive = false;
            }
        }

        // Editing mode
        if (m_editingValue) {
            int c = GetCharPressed();
            while (c > 0) {
                if (c >= '0' && c <= '9') m_editBuffer += (char)c;
                else if (c == '.' && m_editBuffer.find('.') == std::string::npos) m_editBuffer += '.';
                c = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && !m_editBuffer.empty()) m_editBuffer.pop_back();
            if (IsKeyPressed(KEY_ENTER)) {
                float val = std::strtof(m_editBuffer.c_str(), nullptr);
                if (m_settingsTab == 0 && m_settingsIndex == 2) {
                    m_config.maxZoom = std::clamp(val, 1.0f, 50.0f);
                    m_renderer->setMaxZoom(m_config.maxZoom);
                } else if (m_settingsTab == 1 && m_settingsIndex == 0) {
                    m_config.flySpeed = std::clamp(val, 0.1f, 10.0f);
                }
                m_editingValue = false;
            }
            if (IsKeyPressed(KEY_ESCAPE)) m_editingValue = false;
            return;
        }

        Vector2 mouse = getMouse();

        // Tab click detection
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            int tabIdx = 0;
            for (int t = 0; t < TAB_COUNT; ++t) {
                if (t == 4 && !m_config.debugMode) continue;
                int tx = tabStartX + tabIdx * tabSpacing;
                int tw = MeasureText(TAB_NAMES[t], fontSize);
                Rectangle tabRect = { (float)(tx - tw/2), (float)tabY, (float)tw, (float)(fontSize + 10) };
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

        // Scroll wheel for scrolling item list
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            m_settingsScroll -= (int)wheel;
            m_settingsScroll = std::clamp(m_settingsScroll, 0, maxScroll);
        }

        // Search box focus on click
        if (m_settingsTab == 3 && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            int sbY = tabY + fontSize + 8;
            int sbW = 300;
            int sbH = 24;
            int sbX = centerX - sbW / 2;
            bool overSearch = CheckCollisionPointRec(mouse, {(float)sbX, (float)sbY, (float)sbW, (float)sbH});
            m_keybindFilterActive = overSearch;
        }

        // Build visible index list for hover detection
        std::vector<int> visIdx;
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
            if (!skip) visIdx.push_back(i);
        }

        // Mouse hover and reset button detection (compacted)
        int hovered = -1;
        int resetHovered = -1;
        int visCount = (int)visIdx.size();
        int maxVisScroll = std::max(0, visCount - maxVisible);
        int effScroll = std::min(m_settingsScroll, maxVisScroll);
        for (int vi = 0; vi < visCount; ++vi) {
            int i = visIdx[vi];
            int y = startY + (vi - effScroll) * itemH;
            std::string label = makeSettingLabel(m_settingsTab, i, m_config);
            int tw = MeasureText(label.c_str(), fontSize);
            if (m_settingsTab == 0 && i == 5) {
                if (CheckCollisionPointRec(mouse, { (float)(centerX - 180), (float)(y - 5), 360, (float)(itemH - 10) }))
                    { hovered = i; }
            } else {
                if (CheckCollisionPointRec(mouse, { (float)(centerX - tw/2 - 20), (float)(y - 5), (float)(tw + 40), (float)(itemH - 10) }))
                    { hovered = i; }
            }
            if (i < count && (items[i].isValue || (m_settingsTab == 0 && i <= 6) || (m_settingsTab == 3 && items[i].actionId >= 0) || (m_settingsTab == 4 && i < 3))) {
                const char* resetLabel = "R";
                int rw = MeasureText(resetLabel, smFont);
                float rx = (m_settingsTab == 0 && i == 5) ? (centerX + 175) : (centerX + tw / 2 + 14);
                Rectangle resetRect = { rx, (float)(y + 5), (float)(rw + 16), (float)(smFont + 8) };
                if (CheckCollisionPointRec(mouse, resetRect))
                    { resetHovered = i; }
            }
        }

        // Tab switching (LEFT/RIGHT) — skip when on value items, FPS slider, or resolution
        bool left = IsKeyPressed(KEY_LEFT);
        bool right = IsKeyPressed(KEY_RIGHT);
        bool onValue = m_settingsIndex >= 0 && m_settingsIndex < count && items[m_settingsIndex].isValue;
        bool onFps = (m_settingsTab == 0 && m_settingsIndex == 5);
        bool onResolution = (m_settingsTab == 0 && m_settingsIndex == 4);
        if (left && !onValue && !onFps && !onResolution) {
            int nt = m_settingsTab;
            do { nt = (nt + TAB_COUNT - 1) % TAB_COUNT; } while (nt == 4 && !m_config.debugMode);
            m_settingsTab = nt; m_settingsIndex = 0; m_settingsScroll = 0; m_keybindFilter.clear(); m_keybindFilterActive = false;
        }
        if (right && !onValue && !onFps && !onResolution) {
            int nt = m_settingsTab;
            do { nt = (nt + 1) % TAB_COUNT; } while (nt == 4 && !m_config.debugMode);
            m_settingsTab = nt; m_settingsIndex = 0; m_settingsScroll = 0; m_keybindFilter.clear(); m_keybindFilterActive = false;
        }

        // Item navigation with auto-scroll (skip collapsed items)
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

        // LEFT/RIGHT value adjustment on value items / FPS cycling
        if ((left || right) && onValue) {
            int dir = right ? 1 : -1;
            if (m_settingsTab == 0 && m_settingsIndex == 3) {
                int idx = nearestIndex(m_config.maxZoom, MAX_ZOOM_VALS, MAX_ZOOM_COUNT);
                idx = (idx + dir + MAX_ZOOM_COUNT) % MAX_ZOOM_COUNT;
                m_config.maxZoom = MAX_ZOOM_VALS[idx];
                m_renderer->setMaxZoom(m_config.maxZoom);
            } else if (m_settingsTab == 1 && m_settingsIndex == 0) {
                int idx = nearestIndex(m_config.flySpeed, FLY_SPEED_VALS, FLY_SPEED_COUNT);
                idx = (idx + dir + FLY_SPEED_COUNT) % FLY_SPEED_COUNT;
                m_config.flySpeed = FLY_SPEED_VALS[idx];
            }
        }

        // Reset via keyboard (KEY_R on selected item)
        if (IsKeyPressed(KEY_R) && m_settingsIndex >= 0 && m_settingsIndex < count) {
            const Setting& rs = items[m_settingsIndex];
            if (rs.isValue || strcmp(rs.label, "Fullscreen") == 0 || strcmp(rs.label, "Show Actual Flags") == 0 || strcmp(rs.label, "Debug Mode") == 0 || strcmp(rs.label, "FPS") == 0 || strcmp(rs.label, "Accent Color") == 0 || strcmp(rs.label, "AI Difficulty") == 0 || strcmp(rs.label, "Display FPS") == 0 || strcmp(rs.label, "Display Zoom") == 0 || strcmp(rs.label, "Console Window") == 0 || strcmp(rs.label, "AI Debug") == 0 || strcmp(rs.label, "AI Learning") == 0) {
                if (m_settingsTab == 0 && m_settingsIndex == 0) {
                    if (m_config.fullscreen) {
                        setFullscreenAttrs(false, &m_windowedX, &m_windowedY, &m_windowedW, &m_windowedH);
                        PollInputEvents();
                    }
                    m_config.fullscreen = false;
                    m_screenW = GetScreenWidth();
                    m_screenH = GetScreenHeight();
                    m_renderer->resize(m_screenW, m_screenH);
                }
                else if (m_settingsTab == 0 && m_settingsIndex == 1) { m_config.showActualFlags = true; rebuildFlags(); m_renderer->setCountryFlags(&m_countryFlags); }
                else if (m_settingsTab == 0 && m_settingsIndex == 2) { m_config.debugMode = false; m_renderer->setDebugMode(m_config.debugMode); }
                else if (m_settingsTab == 0 && m_settingsIndex == 3) { m_config.maxZoom = 5.0f; m_renderer->setMaxZoom(m_config.maxZoom); }
                else if (m_settingsTab == 0 && m_settingsIndex == 4) { /* Resolution - handled below */ }
                else if (m_settingsTab == 0 && m_settingsIndex == 5) { m_config.fpsTarget = 0; applyFpsTarget(m_config.fpsTarget); }
                else if (m_settingsTab == 0 && m_settingsIndex == 6) { m_config.accentColor = 0xFFD700; }
                else if (m_settingsTab == 0 && m_settingsIndex == 7) { m_config.aiDifficulty = 1; }
                else if (m_settingsTab == 1 && m_settingsIndex == 0) { m_config.flySpeed = 2.0f; }
                else if (m_settingsTab == 4 && m_settingsIndex == 0) { m_config.showFps = true; }
                else if (m_settingsTab == 4 && m_settingsIndex == 1) { m_config.showZoom = false; }
                else if (m_settingsTab == 4 && m_settingsIndex == 2) { m_config.showConsole = false; }
                else if (m_settingsTab == 4 && m_settingsIndex == 3) { m_config.aiDebug = false; }
                else if (m_settingsTab == 4 && m_settingsIndex == 4) { m_config.aiLearning = true; }
            } else if (m_settingsTab == 3 && items[m_settingsIndex].actionId >= 0) {
                m_config.keybinds[items[m_settingsIndex].actionId] = DEFAULT_KEYBINDS[items[m_settingsIndex].actionId];
                m_config.save(m_configPath);
            }
        }

        // Resolution cycling (LEFT/RIGHT on Resolution item)
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
            m_renderer->resize(m_screenW, m_screenH);
        }

        // FPS slider keyboard handling (LEFT/RIGHT on FPS item)
        if (m_settingsIndex == 5 && m_settingsTab == 0 && (left || right)) {
            int idx = fpsTargetToIndex(m_config.fpsTarget);
            idx = (idx + (right ? 1 : -1) + 14) % 14;
            m_config.fpsTarget = indexToFpsTarget(idx);
            applyFpsTarget(m_config.fpsTarget);
        }

        // FPS slider drag — capture on press, track while held, release on mouse up
        if (m_settingsTab == 0) {
            int fpsIdx = 5;
            int sy = startY + (fpsIdx - m_settingsScroll) * itemH;
            int sliderW = 300;
            int sliderX = centerX - sliderW / 2;
            Rectangle sliderRow = { (float)(centerX - 260), (float)sy, 520, (float)itemH };

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, sliderRow)) {
                m_draggingFpsSlider = true;
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                m_draggingFpsSlider = false;
            }
            if (m_draggingFpsSlider && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                float t = (mouse.x - sliderX) / sliderW;
                int idx = (int)roundf(t * 13.0f);
                idx = std::clamp(idx, 0, 13);
                int newTarget = indexToFpsTarget(idx);
                if (newTarget != m_config.fpsTarget) {
                    m_config.fpsTarget = newTarget;
                    applyFpsTarget(m_config.fpsTarget);
                }
            }
        }

        // Accent color cycling (LEFT/RIGHT on Accent Color item)
        if (m_settingsTab == 0 && m_settingsIndex == 6 && (left || right)) {
            int curIdx = 0;
            for (int p = 0; p < ACCENT_PRESETS_COUNT; ++p)
                if (ACCENT_PRESETS[p] == m_config.accentColor) { curIdx = p; break; }
            curIdx = (curIdx + (right ? 1 : -1) + ACCENT_PRESETS_COUNT) % ACCENT_PRESETS_COUNT;
            m_config.accentColor = ACCENT_PRESETS[curIdx];
        }

        // AI difficulty cycling (LEFT/RIGHT on AI Difficulty item)
        if (m_settingsTab == 0 && m_settingsIndex == 7 && (left || right)) {
            m_config.aiDifficulty = (m_config.aiDifficulty + (right ? 1 : -1) + AI_DIFFICULTY_COUNT) % AI_DIFFICULTY_COUNT;
        }

        // Activation — resetHovered first so reset buttons overrides item click
        bool activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (resetHovered >= 0) {
                m_settingsIndex = resetHovered;
                // Reset the value
                if (m_settingsTab == 0 && m_settingsIndex == 0) {
                    if (m_config.fullscreen) {
                        setFullscreenAttrs(false, &m_windowedX, &m_windowedY, &m_windowedW, &m_windowedH);
                        PollInputEvents();
                    }
                    m_config.fullscreen = false;
                    m_screenW = GetScreenWidth();
                    m_screenH = GetScreenHeight();
                    m_renderer->resize(m_screenW, m_screenH);
                }
                else if (m_settingsTab == 0 && m_settingsIndex == 1) { m_config.showActualFlags = true; rebuildFlags(); m_renderer->setCountryFlags(&m_countryFlags); }
                else if (m_settingsTab == 0 && m_settingsIndex == 2) { m_config.debugMode = false; m_renderer->setDebugMode(m_config.debugMode); }
                else if (m_settingsTab == 0 && m_settingsIndex == 3) { m_config.maxZoom = 5.0f; m_renderer->setMaxZoom(m_config.maxZoom); }
                else if (m_settingsTab == 0 && m_settingsIndex == 4) { m_config.screenW = 1920; m_config.screenH = 1080; forceWindowResize(1920, 1080); m_renderer->resize(1920, 1080); }
                else if (m_settingsTab == 0 && m_settingsIndex == 5) { m_config.fpsTarget = 0; applyFpsTarget(m_config.fpsTarget); }
                else if (m_settingsTab == 0 && m_settingsIndex == 6) { m_config.accentColor = 0xFFD700; }
                else if (m_settingsTab == 0 && m_settingsIndex == 7) { m_config.aiDifficulty = 1; }
                else if (m_settingsTab == 1 && m_settingsIndex == 0) { m_config.flySpeed = 2.0f; }
                else if (m_settingsTab == 4 && m_settingsIndex == 0) { m_config.showFps = true; }
                else if (m_settingsTab == 4 && m_settingsIndex == 1) { m_config.showZoom = false; }
                else if (m_settingsTab == 4 && m_settingsIndex == 2) { m_config.showConsole = false; }
                else if (m_settingsTab == 4 && m_settingsIndex == 3) { m_config.aiDebug = false; }
                else if (m_settingsTab == 4 && m_settingsIndex == 4) { m_config.aiLearning = true; }
                else if (m_settingsTab == 3 && items[m_settingsIndex].actionId >= 0) { m_config.keybinds[items[m_settingsIndex].actionId] = DEFAULT_KEYBINDS[items[m_settingsIndex].actionId]; }
                m_config.save(m_configPath);
            } else if (hovered >= 0) {
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
                m_renderer->resize(m_screenW, m_screenH);
                m_config.screenW = m_screenW;
                m_config.screenH = m_screenH;
            } else if (strcmp(s.label, "Show Actual Flags") == 0) {
                m_config.showActualFlags = !m_config.showActualFlags;
                rebuildFlags();
                m_renderer->setCountryFlags(&m_countryFlags);
            } else if (strcmp(s.label, "Debug Mode") == 0) {
                m_config.debugMode = !m_config.debugMode;
                m_renderer->setDebugMode(m_config.debugMode);
            } else if (strcmp(s.label, "Display FPS") == 0) {
                m_config.showFps = !m_config.showFps;
            } else if (strcmp(s.label, "Display Zoom") == 0) {
                m_config.showZoom = !m_config.showZoom;
            } else if (strcmp(s.label, "Console Window") == 0) {
                m_config.showConsole = !m_config.showConsole;
            } else if (strcmp(s.label, "AI Debug") == 0) {
                m_config.aiDebug = !m_config.aiDebug;
            } else if (strcmp(s.label, "AI Learning") == 0) {
                m_config.aiLearning = !m_config.aiLearning;
            } else if (strcmp(s.label, "AI Difficulty") == 0) {
                m_config.aiDifficulty = (m_config.aiDifficulty + 1) % AI_DIFFICULTY_COUNT;
            } else if (strcmp(s.label, "Accent Color") == 0) {
                int curIdx = 0;
                for (int p = 0; p < ACCENT_PRESETS_COUNT; ++p)
                    if (ACCENT_PRESETS[p] == m_config.accentColor) { curIdx = p; break; }
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
            initMenuBackground();
            m_menuBgScroll = 0;
                forceWindowResize(m_screenW, m_screenH);
                PollInputEvents();
                m_screenW = GetScreenWidth();
                m_screenH = GetScreenHeight();
                m_renderer->resize(m_screenW, m_screenH);
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
    } else if (m_showUnsavedWarning) {
        // Unsaved changes warning dialog — handle choices
        Vector2 mouse = getMouse();
        int centerX = m_screenW / 2;

        // Arrow keys to cycle choices
        if (IsKeyPressed(KEY_LEFT)) m_unsavedChoice = (m_unsavedChoice + 2) % 3;
        if (IsKeyPressed(KEY_RIGHT)) m_unsavedChoice = (m_unsavedChoice + 1) % 3;

        bool activate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
        int dlgH = 220;
        int btnW = 180, btnH = 40;
        int btnY = (m_screenH - dlgH) / 2 + dlgH - 58;

        // Mouse hover for choice buttons
        int hoverChoice = -1;
        for (int c = 0; c < 3; ++c) {
            int bx = centerX - ((3 * btnW + 20) / 2) + c * (btnW + 10);
            Rectangle btn = {(float)bx, (float)btnY, (float)btnW, (float)btnH};
            if (CheckCollisionPointRec(mouse, btn)) {
                hoverChoice = c;
            }
        }

        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && hoverChoice >= 0) {
            m_unsavedChoice = hoverChoice;
            activate = true;
        }

        if (activate) {
            if (m_unsavedChoice == 0) {
                // Save and Quit
                trySaveGame();
                m_popupQueue.clear();
                unloadGameData();
                initMenuBackground();
                m_paused = false;
                m_showUnsavedWarning = false;
                m_currentScreen = SCREEN_MENU;
            } else if (m_unsavedChoice == 1) {
                // Quit Without Saving
                m_popupQueue.clear();
                unloadGameData();
                initMenuBackground();
                m_paused = false;
                m_showUnsavedWarning = false;
                m_currentScreen = SCREEN_MENU;
            } else {
                // Cancel — close dialog
                m_showUnsavedWarning = false;
            }
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            m_showUnsavedWarning = false;
        }
        return;
    } else {
        handlePauseMenu();
    }
}
