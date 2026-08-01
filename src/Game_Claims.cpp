#include "Game.h"
#include "Audio.h"
#include "GameInternals.h"
#include "raymath.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstdio>

void Game::clearClaimsView() {
    m_showClaims = false;
    m_lastClaimsCountryId = -1;
    m_renderer->setShowClaims(false);
    m_activeSidebarTab = 0;
    std::fill(m_claimsPixelBuffer.begin(), m_claimsPixelBuffer.end(), Color{0, 0, 0, 0});
    m_renderer->updateClaimsTexture(m_claimsPixelBuffer.data());
}

void Game::grantClaim(const std::string& claimantIso, int pid) {
    if (claimantIso.empty() || pid <= 0) return;
    auto& cl = m_claims[claimantIso];
    if (std::find(cl.begin(), cl.end(), pid) == cl.end()) cl.push_back(pid);
    // Checked separately rather than under the same early-out: a world saved
    // by a build that only wrote one of the two indexes loads with them
    // already disagreeing, and the repair belongs here.
    auto& rev = m_claimsByProvince[pid];
    if (std::find(rev.begin(), rev.end(), claimantIso) == rev.end())
        rev.push_back(claimantIso);
}

// === dropSelfOwnedClaims ===
// A claim is a demand for land you do not have, so holding the land ends it.
// Transfers clear the new owner's claim as they happen now, but a world saved
// before they did -- or a scenario that ships a country claiming its own
// ground -- loads with claims that can never be satisfied because they already
// are: painted contested on the overlay, listed under "Claimed by" on your own
// province, and feeding the AI's reconquer-our-land war bar for ever.
void Game::dropSelfOwnedClaims() {
    std::vector<std::pair<std::string, int>> stale;
    for (auto& [iso, pids] : m_claims) {
        const int cid = cidForIso(iso);
        if (cid <= 0) continue;
        for (int pid : pids) {
            const Province* p = m_provinces.getProvinceById(pid);
            if (p && p->countryId == cid) stale.emplace_back(iso, pid);
        }
    }
    for (auto& [iso, pid] : stale) revokeClaim(iso, pid);
    if (!stale.empty())
        printf("[CLAIM] dropped %zu claim(s) on ground the claimant already holds\n",
               stale.size());
}

void Game::revokeClaim(const std::string& claimantIso, int pid) {
    if (claimantIso.empty() || pid <= 0) return;
    auto it = m_claims.find(claimantIso);
    if (it != m_claims.end()) {
        auto& v = it->second;
        v.erase(std::remove(v.begin(), v.end(), pid), v.end());
        if (v.empty()) m_claims.erase(it);
    }
    auto bp = m_claimsByProvince.find(pid);
    if (bp != m_claimsByProvince.end()) {
        auto& v = bp->second;
        v.erase(std::remove(v.begin(), v.end(), claimantIso), v.end());
        if (v.empty()) m_claimsByProvince.erase(bp);
    }
}

bool Game::isCountryInvolvedInClaims(int countryId, int claimantCid) {
    if (claimantCid <= 0) return false;
    if (countryId == claimantCid) return true;
    const Country* claimer = m_countries.getCountry(claimantCid);
    if (!claimer) return false;
    auto it = m_claims.find(claimer->isoA3);
    if (it == m_claims.end()) return false;
    for (int cpid : it->second) {
        if ((size_t)cpid < m_provinceCountryLookup.size() && m_provinceCountryLookup[cpid] == countryId)
            return true;
    }
    return false;
}

void Game::drawClaimsTab() {
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 180});

    const Country* pc = m_countries.getCountry(m_playerCountryId);
    if (!pc) return;

    Vector2 mouse = getMouse();

    // Close button (top-right, same as economy)
    Rectangle closeBtn = {(float)(m_screenW - 44), 8, 36, 36};
    DrawRectangleRounded(closeBtn, 0.2f, 6, {60, 60, 70, 180});
    DrawRectangleRoundedLines(closeBtn, 0.2f, 6, {180, 180, 180, 200});
    int xw = MeasureText("X", 20);
    DrawText("X", (int)(closeBtn.x + closeBtn.width/2 - xw/2), 12, 20, {180, 180, 180, 200});
    if (CheckCollisionPointRec(mouse, closeBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        Audio::get().playSfx("back");
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
    }
    DrawText("ESC to close", m_screenW - 140, 55, 14, Color{120, 120, 140, 150});

    // Title
    DrawText(TextFormat("Territorial Claims - %s", pc->name.c_str()), 20, 16, 22, hexToColor(m_config.accentColor));

    // ─── Tabs ───
    int tabY = 100;
    const char* tabs[] = {"My Claims", "Claims on Me", "Disputed"};
    int tabSpacing = 140;
    int nTabs = 3;
    int centerX = m_screenW / 2;
    int tabStartX = centerX - (nTabs * tabSpacing) / 2 + tabSpacing / 2;

    for (int t = 0; t < nTabs; ++t) {
        int tx = tabStartX + t * tabSpacing;
        bool active = (t == m_claimsTab);
        Color tc = active ? hexToColor(m_config.accentColor) : LIGHTGRAY;
        int tw = MeasureText(tabs[t], 20);
        DrawText(tabs[t], tx - tw / 2, tabY, 20, tc);
        if (active) DrawRectangle(tx - tw / 2, tabY + 24, tw, 3, hexToColor(m_config.accentColor));
        Rectangle tr = {(float)(tx - tw / 2 - 10), (float)(tabY - 5), (float)(tw + 20), 30};
        if (CheckCollisionPointRec(mouse, tr) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (m_claimsTab != t) Audio::get().playSfx("tab_switch");
            m_claimsTab = t;
            m_claimsScroll = 0;
            m_claimsMapSrcX = m_claimsMapSrcY = 0;
            m_claimsMapZoom = 1.0f;
            m_claimsOverlayDirty = true;
            // Hide main-map claims overlay when not on "My Claims" tab (avoids ghosting)
            if (t == 0) {
                m_showClaims = true;
                if (m_renderer) {
                    m_renderer->setShowClaims(true);
                    m_lastClaimsCountryId = m_playerCountryId;
                    generateClaimsTexture();
                }
            } else {
                m_showClaims = false;
                if (m_renderer) {
                    m_renderer->setShowClaims(false);
                    std::fill(m_claimsPixelBuffer.begin(), m_claimsPixelBuffer.end(), Color{0, 0, 0, 0});
                    m_renderer->updateClaimsTexture(m_claimsPixelBuffer.data());
                }
            }
        }
    }

    int mapTexW = m_provinces.getWidth();
    int mapTexH = m_provinces.getHeight();

    // ─── Compute relevant provinces for the current tab ───
    auto getRelevantPids = [&]() -> std::vector<int> {
        std::vector<int> pids;
        if (m_claimsTab == 0) {
            auto it = m_claims.find(pc->isoA3);
            if (it != m_claims.end()) pids = it->second;
            for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
                if (prov.countryId == m_playerCountryId) pids.push_back(pid);
            }
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
            if (it != m_claims.end()) {
                for (int pid : it->second) {
                    auto ppIt = m_provinces.getAllProvinces().find(pid);
                    if (ppIt != m_provinces.getAllProvinces().end() && ppIt->second.countryId == m_playerCountryId)
                        pids.push_back(pid);
                }
            }
            for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
                if (prov.countryId == m_playerCountryId) pids.push_back(pid);
            }
        } else if (m_claimsTab == 2) {
            auto myIt = m_claims.find(pc->isoA3);
            if (myIt != m_claims.end()) {
                for (int pid : myIt->second) {
                    auto cpIt = m_claimsByProvince.find(pid);
                    if (cpIt != m_claimsByProvince.end()) {
                        for (const std::string& ci : cpIt->second) {
                            if (ci != pc->isoA3) { pids.push_back(pid); break; }
                        }
                    }
                }
            }
            for (auto& [pid, prov] : m_provinces.getAllProvinces()) {
                if (prov.countryId == m_playerCountryId) pids.push_back(pid);
            }
        }
        return pids;
    };

    std::vector<int> relevantPids = getRelevantPids();

    // ─── Compute bounding box for the inline map ───
    int minPx = 0, maxPx = mapTexW - 1, minPy = 0, maxPy = mapTexH - 1;
    bool hasBounds = false;
    for (int pid : relevantPids) {
        auto ppIt = m_provincePixels.find(pid);
        if (ppIt == m_provincePixels.end() || ppIt->second.empty()) continue;
        for (int idx : ppIt->second) {
            int px = idx % mapTexW;
            int py = idx / mapTexW;
            if (!hasBounds) {
                minPx = maxPx = px;
                minPy = maxPy = py;
                hasBounds = true;
            } else {
                if (px < minPx) minPx = px;
                if (px > maxPx) maxPx = px;
                if (py < minPy) minPy = py;
                if (py > maxPy) maxPy = py;
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

    // Apply pan + zoom (final clamp accounts for both)
    float z = m_claimsMapZoom;
    if (z < 1.0f) z = 1.0f;
    if (z > 5.0f) z = 5.0f;
    m_claimsMapZoom = z;
    int zoomedW = std::max((int)(srcW / z), 10);
    int zoomedH = std::max((int)(srcH / z), 10);
    int zoomShiftX = (srcW - zoomedW) / 2;
    int zoomShiftY = (srcH - zoomedH) / 2;
    int srcX = baseSrcX + m_claimsMapSrcX + zoomShiftX;
    int srcY = baseSrcY + m_claimsMapSrcY + zoomShiftY;
    srcX = std::clamp(srcX, 0, mapTexW - zoomedW);
    srcY = std::clamp(srcY, 0, mapTexH - zoomedH);
    srcW = zoomedW;
    srcH = zoomedH;

    // ─── Inline map (aspect-ratio preserved) ───
    int mapY = tabY + 55;
    int mapH = std::min(m_screenH - mapY - 220, 320);
    if (mapH < 120) mapH = 120;

    int listW = std::min(380, m_screenW / 3);
    int mapW = m_screenW - listW - 40;
    int mapX = 20;

    // Compute letterboxed destination rect
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
    int iDrawX = (int)dX, iDrawY = (int)dYf, iDrawW = (int)dW, iDrawH = (int)dH;

    // Draw the political map zoomed in with claims overlay baked directly into a full-map texture
    if (m_politicalTex.id > 0) {
        // Build full-map claims overlay buffer (only when dirty)
        if (m_claimsOverlayDirty) {
            m_claimsOverlayDirty = false;
            int mapTexWLoc = mapTexW, mapTexHLoc = mapTexH;
            std::vector<Color> overlayBuf(mapTexWLoc * mapTexHLoc, Color{0, 0, 0, 0});

        // Determine claimant and claimed provinces for the current tab
        int claimantCid = -1;
        std::vector<int> claimedPids;
        std::vector<int> pendingAddPids;
        std::vector<int> pendingDropPids;
        if (m_claimsTab == 0) {
            claimantCid = m_playerCountryId;
            auto it = m_claims.find(pc->isoA3);
            if (it != m_claims.end()) claimedPids = it->second;
            for (int pid : m_claimsPendingDrop) {
                auto cp = std::find(claimedPids.begin(), claimedPids.end(), pid);
                if (cp != claimedPids.end()) claimedPids.erase(cp);
            }
            for (int pid : m_claimsPendingAdd) {
                if (std::find(claimedPids.begin(), claimedPids.end(), pid) == claimedPids.end())
                    claimedPids.push_back(pid);
            }
            for (int pid : m_claimsPendingAdd) pendingAddPids.push_back(pid);
            for (int pid : m_claimsPendingDrop) pendingDropPids.push_back(pid);
            if (m_claimsEditMode) {
                for (int pid : m_claimsEditToAdd) pendingAddPids.push_back(pid);
                for (int pid : m_claimsEditToDrop) {
                    pendingDropPids.push_back(pid);
                    auto cp = std::find(claimedPids.begin(), claimedPids.end(), pid);
                    if (cp != claimedPids.end()) claimedPids.erase(cp);
                }
                for (int pid : m_claimsEditToAdd) {
                    if (std::find(claimedPids.begin(), claimedPids.end(), pid) == claimedPids.end())
                        claimedPids.push_back(pid);
                }
            }
        } else if (m_claimsTab == 1 && !m_claimsPovList.empty()) {
            int povIdx = std::min(m_claimsPovIndex, (int)m_claimsPovList.size() - 1);
            auto it = m_claims.find(m_claimsPovList[povIdx]);
            if (it != m_claims.end()) {
                for (int pid : it->second) {
                    auto ppIt = m_provinces.getAllProvinces().find(pid);
                    if (ppIt != m_provinces.getAllProvinces().end() && ppIt->second.countryId == m_playerCountryId)
                        claimedPids.push_back(pid);
                }
            }
            const Country* povCountry = nullptr;
            for (auto& [cid, c] : m_countries.getAll())
                if (c.isoA3 == m_claimsPovList[povIdx]) { povCountry = &c; break; }
            if (povCountry) claimantCid = povCountry->id;
        } else if (m_claimsTab == 2) {
            auto myIt = m_claims.find(pc->isoA3);
            if (myIt != m_claims.end()) {
                for (int pid : myIt->second) {
                    auto cpIt = m_claimsByProvince.find(pid);
                    if (cpIt != m_claimsByProvince.end()) {
                        for (const std::string& ci : cpIt->second) {
                            if (ci != pc->isoA3) { claimedPids.push_back(pid); break; }
                        }
                    }
                }
            }
        }

        // Build set of involved country IDs (owners of claimed provinces)
        std::unordered_set<int> involvedCids;
        if (claimantCid > 0) involvedCids.insert(claimantCid);
        for (int pid : claimedPids) {
            if ((size_t)pid < m_provinceCountryLookup.size())
                involvedCids.insert(m_provinceCountryLookup[pid]);
        }

        // Gray out involved countries (skip player's own in tab 0)
        for (int cid : involvedCids) {
            if (cid < 0 || cid >= (int)m_countryPixels.size()) continue;
            if (m_claimsTab == 0 && cid == m_playerCountryId) continue;
            auto& pixels = m_countryPixels[cid];
            if (pixels.empty()) continue;
            Color countryColor = (claimantCid > 0 && cid == claimantCid)
                ? Color{0, 60, 180, 80} : Color{60, 60, 60, 180};
            for (int idx : pixels)
                overlayBuf[idx] = countryColor;
        }

        // Orange stripes on claimed provinces
        Color stripeCol{220, 140, 30, 210};
        for (int pid : claimedPids) {
            auto ppIt = m_provincePixels.find(pid);
            if (ppIt == m_provincePixels.end()) continue;
            for (int idx : ppIt->second) {
                int px = idx % mapTexWLoc;
                int py = idx / mapTexWLoc;
                if (((px + py) % 10) < 5) overlayBuf[idx] = stripeCol;
            }
        }

        // Green stripes on pending-add provinces
        Color greenOverlay{0, 200, 80, 200};
        for (int pid : pendingAddPids) {
            auto ppIt = m_provincePixels.find(pid);
            if (ppIt == m_provincePixels.end()) continue;
            for (int idx : ppIt->second) {
                int px = idx % mapTexWLoc;
                int py = idx / mapTexWLoc;
                if (((px + py + 5) % 10) < 5) overlayBuf[idx] = greenOverlay;
            }
        }

        // Red stripes on pending-drop provinces
        Color redOverlay{200, 40, 40, 200};
        for (int pid : pendingDropPids) {
            auto ppIt = m_provincePixels.find(pid);
            if (ppIt == m_provincePixels.end()) continue;
            for (int idx : ppIt->second) {
                int px = idx % mapTexWLoc;
                int py = idx / mapTexWLoc;
                if (((px - py) % 10) < 5) overlayBuf[idx] = redOverlay;
            }
        }

        // Upload/update the full-map overlay texture
        if (m_claimsPanelTex.id == 0 || m_claimsPanelTex.width != mapTexWLoc || m_claimsPanelTex.height != mapTexHLoc) {
            if (m_claimsPanelTex.id > 0) UnloadTexture(m_claimsPanelTex);
            Image img{};
            img.data = overlayBuf.data();
            img.width = mapTexWLoc;
            img.height = mapTexHLoc;
            img.mipmaps = 1;
            img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            m_claimsPanelTex = LoadTextureFromImage(img);
        } else {
            UpdateTexture(m_claimsPanelTex, overlayBuf.data());
        }
        } // end if m_claimsOverlayDirty

        // Draw directly: political map first, then overlay on top, both cropped to source rect
        DrawTexturePro(m_politicalTex,
            {(float)srcX, (float)srcY, (float)srcW, (float)srcH},
            {dX, dYf, dW, dH},
            {0, 0}, 0, WHITE);
        DrawTexturePro(m_claimsPanelTex,
            {(float)srcX, (float)srcY, (float)srcW, (float)srcH},
            {dX, dYf, dW, dH},
            {0, 0}, 0, WHITE);
    } else {
        DrawRectangle(iDrawX, iDrawY, iDrawW, iDrawH, {30, 30, 50, 255});
    }
    // Map border
    DrawRectangleLines(iDrawX, iDrawY, iDrawW, iDrawH, {80, 80, 120, 180});
    // Drag hint
    if (!m_claimsEditMode)
        DrawText("Drag to pan | Scroll to zoom", iDrawX + 4, iDrawY + iDrawH - 18, 12, Color{180, 180, 200, 120});

    // ─── List panel (sidebar right of map) ───
    int listX = m_screenW - listW - 20;
    int listY = mapY;
    int listH = mapH;

    DrawRectangle(listX, listY, listW, listH, {15, 15, 25, 220});
    DrawRectangleLines(listX, listY, listW, listH, {60, 60, 90, 200});

    BeginScissorMode(listX + 4, listY + 4, listW - 8, listH - 8);

    int drawY = listY + 8 - m_claimsScroll;

    if (m_claimsTab == 0) {
        if (m_claimsEditMode) {
            DrawText("Click map to add/drop claims", listX + 8, drawY, 11, YELLOW);
            drawY += 18;
        }

        auto it = m_claims.find(pc->isoA3);
        std::vector<int> currentClaims = (it != m_claims.end()) ? it->second : std::vector<int>();

        std::vector<int> display;
        for (int pid : currentClaims) {
            if (std::find(m_claimsEditToDrop.begin(), m_claimsEditToDrop.end(), pid) == m_claimsEditToDrop.end() &&
                std::find(m_claimsPendingDrop.begin(), m_claimsPendingDrop.end(), pid) == m_claimsPendingDrop.end())
                display.push_back(pid);
        }
        for (int pid : m_claimsEditToAdd) {
            if (std::find(display.begin(), display.end(), pid) == display.end())
                display.push_back(pid);
        }
        for (int pid : m_claimsPendingAdd) {
            if (std::find(display.begin(), display.end(), pid) == display.end())
                display.push_back(pid);
        }

        if (display.empty()) {
            DrawText("No claims", listX + listW/2 - 30, drawY + 10, 14, LIGHTGRAY);
            drawY += 36;
        } else {
            for (int pid : display) {
                bool inAdd = std::find(m_claimsEditToAdd.begin(), m_claimsEditToAdd.end(), pid) != m_claimsEditToAdd.end();
                bool inDrop = std::find(m_claimsEditToDrop.begin(), m_claimsEditToDrop.end(), pid) != m_claimsEditToDrop.end();
                bool inPendingAdd = std::find(m_claimsPendingAdd.begin(), m_claimsPendingAdd.end(), pid) != m_claimsPendingAdd.end();
                bool inPendingDrop = std::find(m_claimsPendingDrop.begin(), m_claimsPendingDrop.end(), pid) != m_claimsPendingDrop.end();
                auto ppIt = m_provinces.getAllProvinces().find(pid);
                if (ppIt != m_provinces.getAllProvinces().end()) {
                    const Country* owner = m_countries.getCountry(ppIt->second.countryId);
                    Color dotCol = owner ? owner->color : DARKGRAY;
                    DrawRectangle(listX + 8, drawY + 2, 16, 16, dotCol);

                    std::string label = ppIt->second.name;
                    if (owner) label += " (" + owner->name + ")";
                    DrawText(label.c_str(), listX + 30, drawY + 2, 12, WHITE);

                    if (inPendingAdd || inAdd) DrawText("[+]", listX + listW - 50, drawY, 13, GREEN);
                    else if (inPendingDrop || inDrop) DrawText("[-]", listX + listW - 50, drawY, 13, RED);

                    if (m_claimsEditMode && CheckCollisionPointRec(mouse, {(float)(listX + 4), (float)drawY, (float)(listW - 8), 22}) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                        bool isCur = std::find(currentClaims.begin(), currentClaims.end(), pid) != currentClaims.end();
                        bool isOwnedByPlayer = ppIt->second.countryId == m_playerCountryId;
                        if (isCur && !inDrop) { m_claimsEditToDrop.push_back(pid); Audio::get().playSfx("toggle_off"); }
                        else if (isCur && inDrop) { m_claimsEditToDrop.erase(std::remove(m_claimsEditToDrop.begin(), m_claimsEditToDrop.end(), pid), m_claimsEditToDrop.end()); Audio::get().playSfx("toggle_on"); }
                        else if (!isCur && !inAdd && !isOwnedByPlayer) { m_claimsEditToAdd.push_back(pid); Audio::get().playSfx("stake_claim"); }
                        else if (!isCur && inAdd && !isOwnedByPlayer) { m_claimsEditToAdd.erase(std::remove(m_claimsEditToAdd.begin(), m_claimsEditToAdd.end(), pid), m_claimsEditToAdd.end()); Audio::get().playSfx("toggle_off"); }
                        // Your own province cannot be claimed, and the click
                        // above falls through every branch when it is.
                        else Audio::get().playSfx("deny");
                        m_claimsOverlayDirty = true;
                    }
                }
                drawY += 22;
            }
        }

        EndScissorMode();
        int listBottom = listY + listH;

        int btnAreaY = listBottom + 12;
        if (m_claimsEditMode) {
            int btnY = btnAreaY;
            Rectangle confirmBtn = {(float)(centerX - 170), (float)btnY, 160, 34};
            DrawRectangleRec(confirmBtn, {40, 160, 40, 220});
            DrawRectangleLinesEx(confirmBtn, 1, {80, 200, 80, 255});
            DrawText("Confirm Changes", centerX - 170 + 80 - MeasureText("Confirm Changes", 15)/2, btnY + 8, 15, WHITE);
            if (CheckCollisionPointRec(mouse, confirmBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Audio::get().playSfx("confirm");
                // Move edits to pending (applied on next turn)
                for (int pid : m_claimsEditToDrop) {
                    auto pp = std::find(m_claimsPendingAdd.begin(), m_claimsPendingAdd.end(), pid);
                    if (pp != m_claimsPendingAdd.end()) m_claimsPendingAdd.erase(pp);
                    else m_claimsPendingDrop.push_back(pid);
                }
                for (int pid : m_claimsEditToAdd) {
                    auto pp = std::find(m_claimsPendingDrop.begin(), m_claimsPendingDrop.end(), pid);
                    if (pp != m_claimsPendingDrop.end()) m_claimsPendingDrop.erase(pp);
                    else m_claimsPendingAdd.push_back(pid);
                }
                m_claimsEditToDrop.clear();
                m_claimsEditToAdd.clear();
                m_claimsEditMode = false;
                m_claimsOverlayDirty = true;
            }
            Rectangle cancelBtn = {(float)(centerX + 10), (float)btnY, 160, 34};
            DrawRectangleRec(cancelBtn, {80, 40, 40, 200});
            DrawRectangleLinesEx(cancelBtn, 1, {140, 80, 80, 255});
            DrawText("Cancel", centerX + 10 + 80 - MeasureText("Cancel", 15)/2, btnY + 8, 15, LIGHTGRAY);
            if (CheckCollisionPointRec(mouse, cancelBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Audio::get().playSfx("back");
                m_claimsEditToAdd.clear();
                m_claimsEditToDrop.clear();
                m_claimsEditMode = false;
                m_claimsOverlayDirty = true;
            }
            DrawText(TextFormat("Add: %d  Drop: %d", (int)m_claimsEditToAdd.size(), (int)m_claimsEditToDrop.size()),
                     centerX - 60, btnY + 40, 12, LIGHTGRAY);
        } else {
            Rectangle editBtn = {(float)(centerX - 60), (float)btnAreaY, 120, 32};
            DrawRectangleRec(editBtn, {40, 80, 160, 220});
            DrawRectangleLinesEx(editBtn, 1, {80, 120, 200, 255});
            DrawText("Edit Claims", centerX - MeasureText("Edit Claims", 16)/2, btnAreaY + 6, 16, WHITE);
            if (CheckCollisionPointRec(mouse, editBtn) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Audio::get().playSfx("click_light");
                m_claimsEditMode = true;
                // Merge pending into edit so user can modify them
                m_claimsEditToAdd = m_claimsPendingAdd;
                m_claimsEditToDrop = m_claimsPendingDrop;
                m_claimsPendingAdd.clear();
                m_claimsPendingDrop.clear();
                m_claimsOverlayDirty = true;
            }
            // Show pending changes that will apply next turn
            if (!m_claimsPendingAdd.empty() || !m_claimsPendingDrop.empty()) {
                DrawText(TextFormat("Pending: +%d -%d (next turn)", (int)m_claimsPendingAdd.size(), (int)m_claimsPendingDrop.size()),
                         centerX - 100, btnAreaY + 36, 12, Color{255, 200, 80, 255});
            }
        }

    } else if (m_claimsTab == 1) {
        if (m_claimsPovList.empty()) {
            DrawText("No claims on your territory", listX + 20, drawY + 10, 14, LIGHTGRAY);
            drawY += 36;
        } else {
            int selIdx = std::min(m_claimsPovIndex, (int)m_claimsPovList.size() - 1);
            std::string selIso = m_claimsPovList[selIdx];
            const Country* selCountry = nullptr;
            for (auto& [cid, c] : m_countries.getAll()) {
                if (c.isoA3 == selIso) { selCountry = &c; break; }
            }

            Rectangle selRect = {(float)(listX + 8), (float)drawY, (float)(listW - 16), 26};
            DrawRectangleRec(selRect, {40, 40, 60, 200});
            DrawRectangleLinesEx(selRect, 1, {80, 80, 120, 255});
            bool canPrev = m_claimsPovIndex > 0;
            bool canNext = m_claimsPovIndex < (int)m_claimsPovList.size() - 1;
            DrawText("<", listX + 12, drawY + 3, 16, canPrev ? WHITE : DARKGRAY);
            DrawText(">", listX + listW - 24, drawY + 3, 16, canNext ? WHITE : DARKGRAY);
            DrawText(selCountry ? selCountry->name.c_str() : selIso.c_str(), listX + 30, drawY + 4, 13, hexToColor(m_config.accentColor));
            if (CheckCollisionPointRec(mouse, selRect) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                float relX = mouse.x - selRect.x;
                if (relX < 24 && canPrev) { m_claimsPovIndex--; m_claimsOverlayDirty = true; Audio::get().playSfx("click_light"); }
                else if (relX > selRect.width - 24 && canNext) { m_claimsPovIndex++; m_claimsOverlayDirty = true; Audio::get().playSfx("click_light"); }
                // Either end of the list: the arrow is drawn greyed, so the
                // click needs to say so rather than do nothing.
                else if (relX < 24 || relX > selRect.width - 24) Audio::get().playSfx("deny");
            }
            drawY += 30;

            auto it = m_claims.find(selIso);
            if (it != m_claims.end()) {
                for (int pid : it->second) {
                    auto ppIt = m_provinces.getAllProvinces().find(pid);
                    if (ppIt == m_provinces.getAllProvinces().end() || ppIt->second.countryId != m_playerCountryId)
                        continue;
                    const Country* owner = m_countries.getCountry(ppIt->second.countryId);
                    Color dotCol = owner ? owner->color : DARKGRAY;
                    DrawRectangle(listX + 8, drawY + 2, 16, 16, dotCol);
                    DrawText(ppIt->second.name.c_str(), listX + 28, drawY + 2, 12, WHITE);
                    drawY += 22;
                }
            }
        }
        EndScissorMode();

    } else if (m_claimsTab == 2) {
        auto myIt = m_claims.find(pc->isoA3);
        bool hasDisputed = false;
        if (myIt != m_claims.end()) {
            for (int pid : myIt->second) {
                auto cpIt = m_claimsByProvince.find(pid);
                if (cpIt == m_claimsByProvince.end()) continue;
                std::vector<std::string> others;
                for (const std::string& ci : cpIt->second) {
                    if (ci != pc->isoA3) others.push_back(ci);
                }
                if (others.empty()) continue;
                hasDisputed = true;

                auto ppIt = m_provinces.getAllProvinces().find(pid);
                if (ppIt != m_provinces.getAllProvinces().end()) {
                    const Country* owner = m_countries.getCountry(ppIt->second.countryId);
                    Color dotCol = owner ? owner->color : DARKGRAY;
                    DrawRectangle(listX + 8, drawY + 2, 16, 16, dotCol);
                    DrawText(ppIt->second.name.c_str(), listX + 28, drawY + 2, 12, WHITE);
                    int claimLabelX = listX + 28;
                    DrawText("Disputed by: ", claimLabelX, drawY + 16, 10, Color{180, 180, 180, 255});
                    claimLabelX += MeasureText("Disputed by: ", 10);
                    for (size_t oi = 0; oi < others.size(); oi++) {
                        const Country* claimer = nullptr;
                        for (auto& [cid2, c2] : m_countries.getAll()) {
                            if (c2.isoA3 == others[oi]) { claimer = &c2; break; }
                        }
                        std::string cname = claimer ? claimer->name : others[oi];
                        DrawText(cname.c_str(), claimLabelX, drawY + 16, 10, hexToColor(m_config.accentColor));
                        claimLabelX += MeasureText(cname.c_str(), 10) + 6;
                    }
                }
                drawY += 28;
            }
        }
        if (!hasDisputed) {
            DrawText("No disputed claims", listX + listW/2 - 50, drawY + 10, 14, LIGHTGRAY);
            drawY += 36;
        }
    EndScissorMode();

    // ─── Pacification Budget Slider (right side) ────
    if (m_claimsTab != 2) {
        const Country* pc2 = m_countries.getCountry(m_playerCountryId);
        if (pc2) {
            auto cs = computeCountryIncome(m_playerCountryId);
            int slX = m_screenW - 270;
            int slY = 340;
            int slW = 200;
            int slH = 18;

            float researchAmt = cs.total * m_researchAllocation;
            float maxAffordPac = std::max(0.0f, cs.total - cs.armyExpenses - cs.navyExpenses - cs.policyCosts - cs.minorityCosts - researchAmt);
            float maxAllocFrac = (cs.total > 0) ? maxAffordPac / cs.total : 0;
            if (maxAllocFrac > 1.0f) maxAllocFrac = 1.0f;
            if (m_pacificationAllocation > maxAllocFrac) m_pacificationAllocation = maxAllocFrac;

            DrawText("Pacification Budget:", slX, slY - 20, 13, WHITE);
            DrawRectangle(slX, slY, slW, slH, {40, 40, 50, 200});
            int maxFill = (int)(slW * maxAllocFrac);
            if (maxFill > 0) DrawRectangle(slX, slY, maxFill, slH, {40, 50, 60, 150});
            DrawRectangleLines(slX, slY, slW, slH, {80, 80, 100, 200});
            int fillPac = (int)(slW * m_pacificationAllocation);
            if (fillPac > 0) DrawRectangle(slX, slY, fillPac, slH, {80, 180, 220, 200});
            DrawText(TextFormat("%d%%", (int)(m_pacificationAllocation * 100)), slX + slW + 6, slY + 2, 12, WHITE);
            // 50, matching the engine formula — this screen used to claim x60
            // while the engine applied x50, overstating suppression by 20%.
            float pacPct = m_pacificationAllocation * 50.0f;
            DrawText(TextFormat("Suppression: %.1f%%", pacPct), slX, slY + slH + 4, 11, LIGHTGRAY);

            {
                const Rectangle pacBar = {(float)slX, (float)slY, (float)slW, (float)slH};
                float t = m_pacificationAllocation;
                if (sliderInteract(pacBar, /*steps=*/0, t, m_draggingPacification)) {
                    // Same clamp-after as the research allocation: the ceiling
                    // is what the budget allows, not 1.0.
                    m_pacificationAllocation = std::clamp(t, 0.0f, maxAllocFrac);
                }
            }
        }
    }
    }
}
