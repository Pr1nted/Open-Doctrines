#include "Game.h"
#include "Audio.h"
#include "GameInternals.h"
#include "Keybinds.h"
#include "renderer/FlagRenderer.h"
#include "raymath.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <string>
#include <unordered_set>
#include <cstdio>
#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>
#ifndef _WIN32
#endif
#include <ctime>


void Game::drawBottomPanel() {
    const int barW = std::min(880, m_screenW - 32);
    const int barH = 80;
    const int barX = m_screenW - barW - 16;
    const int barY = m_screenH - barH - 16;

    DrawRectangleGradientH(barX, barY, barW, barH, {0, 0, 0, 0}, {0, 0, 0, 180});

    Texture2D icons[] = {m_iconPopulation, m_iconIndustry, m_iconDefence, m_iconRelations,
                          m_iconArmyNav, m_iconNavy, m_iconResources, m_iconCountryNames};
    const char* labels[] = {"Population", "Industry", "Defence", "Relations",
                             "Army Navigation", "Navy", "Resources", "Country Names"};
    const int count = 8;
    const int iconSize = 32;
    const int fontSize = 12;

    int buttonStartX = barX + 8;
    int buttonEndX = barX + barW - 8;
    int buttonW = (buttonEndX - buttonStartX) / count;

    int iconY = barY + 8;
    int labelY = iconY + iconSize + 10;

    for (int i = 0; i < count; ++i) {
        int cx = buttonStartX + i * buttonW + buttonW / 2;
        int ix = cx - iconSize / 2;

        Vector2 mouse = getMouse();
        Rectangle btnRect = {(float)(buttonStartX + i * buttonW), (float)barY, (float)buttonW, (float)barH};
        bool hovered = !m_paused && CheckCollisionPointRec(mouse, btnRect);

        Color iconColor = LIGHTGRAY;
        Color textColor = LIGHTGRAY;
        if (m_activeViewTab == i + 1) {
            iconColor = hexToColor(m_config.accentColor);
            textColor = hexToColor(m_config.accentColor);
        } else if (hovered) {
            iconColor = WHITE;
            textColor = WHITE;
            DrawRectangleRounded(btnRect, 0.1f, 8, {255, 255, 255, 16});
        }

        DrawTextureEx(icons[i], {(float)ix, (float)iconY}, 0.0f, (float)iconSize / 64.0f, iconColor);
        int tw = MeasureText(labels[i], fontSize);
        DrawText(labels[i], cx - tw / 2, labelY, fontSize, textColor);

        if (m_activeViewTab == i + 1) {
            int lineW = tw + 12;
            DrawRectangle(cx - lineW / 2, labelY + fontSize + 3, lineW, 2, hexToColor(m_config.accentColor));
        }
    }
}

void Game::drawCountryPanel() {
    const int panelW = 360;
    const int panelX = 0;
    const int panelH = std::min(m_screenH - 80 - 16 - 68, 700);
    const int panelY = 68;
    const int pad = 16;

    // Solid panel background
    DrawRectangle(panelX, panelY, panelW, panelH, {10, 10, 15, 180});

    // Ship info overrides province info when a ship is selected in navy view
    bool showShipInfo = !m_selectedShipIndices.empty() && m_activeViewTab == 6;
    if (showShipInfo) {
        int selCount = (int)m_selectedShipIndices.size();

        if (selCount == 1) {
            // ── Single ship detailed view ──
            int shipIdx = m_selectedShipIndices[0];
            auto& ship = m_ships[shipIdx];
            int cid = ship.countryId;
            const Country* country = m_countries.getCountry(cid);
            const char* cname = country ? country->name.c_str() : "Unknown";
            Color shipCol = country ? country->color : WHITE;

            auto fit = m_countryFlags.find(cid);
            if (fit != m_countryFlags.end() && fit->second.id > 0) {
                DrawTexturePro(fit->second,
                    {0, 0, (float)fit->second.width, (float)fit->second.height},
                    {(float)(panelX + panelW - pad - 96), (float)(panelY + 4 + pad), 96, 48},
                    {0, 0}, 0.0f, WHITE);
            }

            DrawText(TextFormat("%s", ship.type.c_str()), panelX + pad, panelY + 28, 24, WHITE);
            DrawText(TextFormat("Country: %s", cname), panelX + pad, panelY + 56, 14, LIGHTGRAY);

            int iconX = panelX + pad;
            int iconY = panelY + 82;
            int iconSz = 16;
            if (ship.type == "boat") {
                DrawTriangle({(float)iconX, (float)(iconY - iconSz)},
                             {(float)(iconX - iconSz * 0.6f), (float)(iconY + iconSz * 0.4f)},
                             {(float)(iconX + iconSz * 0.6f), (float)(iconY + iconSz * 0.4f)}, shipCol);
            } else if (ship.type == "destroyer") {
                DrawRectangle(iconX - iconSz/2, iconY - iconSz/2, iconSz, iconSz, shipCol);
            } else {
                DrawCircle(iconX, iconY, iconSz * 0.5f, shipCol);
            }

            int yOff = panelY + 110;
            if (ship.crew > 0) {
                DrawText(TextFormat("Crew: %d", ship.crew), panelX + pad, yOff, 14, LIGHTGRAY);
                yOff += 20;
            }

            DrawText("Health:", panelX + pad, yOff, 14, LIGHTGRAY);
            int hbX = panelX + 70;
            int hbY = yOff;
            int hbW = 120;
            int hbH = 14;
            float healthPct = ship.health / 100.0f;
            DrawRectangle(hbX, hbY, hbW, hbH, Color{60, 60, 60, 200});
            DrawRectangle(hbX, hbY, (int)(hbW * healthPct), hbH,
                          ship.health > 50 ? GREEN : (ship.health > 25 ? YELLOW : RED));
            DrawText(TextFormat("%d%%", ship.health), hbX + hbW + 6, hbY + 1, 12, WHITE);
            yOff += 30;

            int ci = 0, cn = 0;
            for (int i = 0; i < (int)m_countryShipIndices.size(); i++) {
                if (m_countryShipIndices[i] == shipIdx) { ci = i; }
                cn++;
            }
            DrawText(TextFormat("Ship %d of %d", ci + 1, cn), panelX + pad, yOff, 14, LIGHTGRAY);
            yOff += 40;

            // Destroy / Cancel Scrap button
            bool shipScrapping = false;
            for (auto& ss : m_pendingScrapShips) if (ss.shipIndex == shipIdx) shipScrapping = true;
            bool canScrap = !shipScrapping && ship.countryId == m_playerCountryId;
            Rectangle sRect = {(float)(panelX + pad), (float)yOff, (float)(panelW - pad * 2), 28};
            Vector2 sms = getMouse();
            bool sHov = !m_paused && m_turnState == TURN_NORMAL && CheckCollisionPointRec(sms, sRect);
            if (shipScrapping) {
                Color sBg = sHov ? Color{80, 60, 20, 220} : Color{60, 40, 10, 220};
                Color sBd = sHov ? Color{180, 140, 50, 200} : Color{140, 100, 30, 150};
                DrawRectangleRounded(sRect, 0.08f, 6, sBg);
                DrawRectangleRoundedLines(sRect, 0.08f, 6, sBd);
                const char* sLabel = "Cancel Scrap";
                int stw = MeasureText(sLabel, 11);
                DrawText(sLabel, (int)(sRect.x + (sRect.width - stw) / 2), (int)(sRect.y + (sRect.height - 11) / 2), 11, WHITE);
                if (sHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    auto& vec = m_pendingScrapShips;
                    for (auto it = vec.begin(); it != vec.end(); ) {
                        if (it->shipIndex == shipIdx) it = vec.erase(it);
                        else ++it;
                    }
                }
            } else if (canScrap) {
                Color sBg = sHov ? Color{120, 30, 30, 220} : Color{80, 20, 20, 220};
                Color sBd = sHov ? Color{220, 80, 80, 200} : Color{180, 60, 60, 200};
                DrawRectangleRounded(sRect, 0.08f, 6, sBg);
                DrawRectangleRoundedLines(sRect, 0.08f, 6, sBd);
                const char* sLabel = "Destroy Ship";
                int stw = MeasureText(sLabel, 11);
                DrawText(sLabel, (int)(sRect.x + (sRect.width - stw) / 2), (int)(sRect.y + (sRect.height - 11) / 2), 11, WHITE);
                if (sHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    m_pendingScrapShips.push_back({shipIdx});
                    // Cancel all pending orders for this ship
                    auto rmShip = [&](auto& vec) {
                        for (auto it = vec.begin(); it != vec.end(); )
                            if (it->shipIndex == shipIdx) it = vec.erase(it); else ++it;
                    };
                    rmShip(m_pendingShipMoveOrders);
                    rmShip(m_pendingShipEngageOrders);
                    rmShip(m_pendingShipBombardOrders);
                    rmShip(m_pendingShipDisembarks);
                    m_shipActionMode = 0; m_shipActionShipIdx = -1;
                }
            }

            // ── Cancel existing pending orders for this ship ──
            auto cancelBtn = [&](int y, const char* label, Color bg, Color bd, auto& vec) {
                Rectangle r = {(float)(panelX+pad), (float)y, (float)(panelW-pad*2), 22};
                Vector2 sm = getMouse();
                bool hov = !m_paused && m_turnState == TURN_NORMAL && CheckCollisionPointRec(sm, r);
                Color bgc = hov ? Color{(unsigned char)std::min(255,bg.r+30), (unsigned char)std::min(255,bg.g+30), (unsigned char)std::min(255,bg.b+30), bg.a} : bg;
                DrawRectangleRounded(r, 0.08f, 6, bgc);
                DrawRectangleRoundedLines(r, 0.08f, 6, bd);
                int tw = MeasureText(label, 10);
                DrawText(label, (int)(r.x+(r.width-tw)/2), (int)(r.y+(r.height-10)/2), 10, WHITE);
                if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    for (auto it = vec.begin(); it != vec.end(); )
                        if (it->shipIndex == shipIdx) it = vec.erase(it); else ++it;
                    m_shipActionMode = 0; m_shipActionShipIdx = -1;
                }
            };
            int pendY = yOff + 32;
            for (auto& mo : m_pendingShipMoveOrders)
                if (mo.shipIndex == shipIdx)
                    { cancelBtn(pendY, "Cancel Move Order", Color{60,40,10,220}, Color{180,140,50,200}, m_pendingShipMoveOrders); pendY += 26; }
            for (auto& eo : m_pendingShipEngageOrders)
                if (eo.shipIndex == shipIdx)
                    { cancelBtn(pendY, "Cancel Engage Order", Color{60,20,20,220}, Color{180,60,60,200}, m_pendingShipEngageOrders); pendY += 26; }
            for (auto& bo : m_pendingShipBombardOrders)
                if (bo.shipIndex == shipIdx)
                    { cancelBtn(pendY, "Cancel Bombard Order", Color{40,20,40,220}, Color{140,60,140,200}, m_pendingShipBombardOrders); pendY += 26; }
            for (auto& do_ : m_pendingShipDisembarks)
                if (do_.shipIndex == shipIdx)
                    { cancelBtn(pendY, "Cancel Disembark Order", Color{30,40,10,220}, Color{100,160,50,200}, m_pendingShipDisembarks); pendY += 26; }

            // ── Ship action buttons (own country only, not scrapping) ──
            if (cid == m_playerCountryId && !shipScrapping) {
                int sBtnH = 28;
                int sBtnG = 4;
                int sHalf = (panelW - pad * 2 - sBtnG) / 2;
                int sY = pendY + 8;
                Vector2 sm = getMouse();
                auto shipBtn = [&](int bx, int by, int bw, const char* label, bool disabled, Color bg, Color bd, bool& clicked) {
                    Rectangle r = {(float)bx, (float)by, (float)bw, (float)sBtnH};
                    bool hov = !m_paused && m_turnState == TURN_NORMAL && !disabled && CheckCollisionPointRec(sm, r);
                    Color bgc = disabled ? Color{20,20,25,200} : (hov ? Color{(unsigned char)std::min(255,bg.r+30), (unsigned char)std::min(255,bg.g+30), (unsigned char)std::min(255,bg.b+30), bg.a} : bg);
                    DrawRectangleRounded(r, 0.08f, 6, bgc);
                    DrawRectangleRoundedLines(r, 0.08f, 6, disabled ? Color{40,40,50,150} : bd);
                    int tw = MeasureText(label, 11);
                    DrawText(label, bx+(bw-tw)/2, by+(sBtnH-11)/2, 11, disabled ? Color{80,80,80,200} : WHITE);
                    clicked = hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
                };
                bool clicked;
                bool anyModeActive = (m_shipActionMode > 0 && m_shipActionShipIdx == shipIdx);
                bool inMove = (m_shipActionMode == 1 && m_shipActionShipIdx == shipIdx);
                bool inEngage = (m_shipActionMode == 2 && m_shipActionShipIdx == shipIdx);
                bool inBombard = (m_shipActionMode == 3 && m_shipActionShipIdx == shipIdx);
                // Check if this ship has any pending order
                bool hasPending = false;
                for (auto& mo : m_pendingShipMoveOrders) if (mo.shipIndex == shipIdx) hasPending = true;
                for (auto& eo : m_pendingShipEngageOrders) if (eo.shipIndex == shipIdx) hasPending = true;
                for (auto& bo : m_pendingShipBombardOrders) if (bo.shipIndex == shipIdx) hasPending = true;
                for (auto& do_ : m_pendingShipDisembarks) if (do_.shipIndex == shipIdx) hasPending = true;

                bool canAct = !hasPending;

                // Move — all ships (boats: move=water or disembark=land)
                shipBtn(panelX+pad, sY, sHalf,
                    inMove ? (ship.type=="boat" ? "Click: water move, land disembark" : "Set destination...") : "Move",
                    !canAct || inEngage || inBombard,
                    inMove ? Color{40,60,40,220} : Color{30,50,80,220},
                    inMove ? Color{60,100,60,200} : Color{60,120,200,200}, clicked);
                if (clicked && canAct) {
                    if (inMove) { m_shipActionMode=0; m_shipActionShipIdx=-1; m_shipBombardDropdownOpen=false; }
                    else { m_shipActionMode=1; m_shipActionShipIdx=shipIdx; m_shipBombardAmmo.clear(); m_shipBombardDropdownOpen=false; }
                }

                // Engage Ship — destroyers/carriers/frigates (not boats)
                if (ship.type=="destroyer" || ship.type=="carrier" || ship.type=="frigate") {
                    shipBtn(panelX+pad+sHalf+sBtnG, sY, sHalf,
                        inEngage ? "Click target ship..." : "Engage Ship",
                        !canAct || inMove || inBombard,
                        inEngage ? Color{40,60,40,220} : Color{80,30,30,220},
                        inEngage ? Color{60,100,60,200} : Color{200,60,60,200}, clicked);
                    if (clicked && canAct) {
                        if (inEngage) { m_shipActionMode=0; m_shipActionShipIdx=-1; m_shipBombardDropdownOpen=false; }
                        else { m_shipActionMode=2; m_shipActionShipIdx=shipIdx; m_shipBombardAmmo.clear(); m_shipBombardDropdownOpen=false; }
                    }
                }

                int row2Y = sY + sBtnH + sBtnG;

                // Bombard Province — carriers only (with artillery type selector)
                if (ship.type == "carrier") {
                    // Find researched artillery types (aircraft-compatible only)
                    std::vector<std::string> artyTypes;
                    auto isAircraftAmmo = [](const std::string& t) {
                        return t == "napalm" || t == "carpet" || t == "nuclear" || t == "biological";
                    };
                    for (auto& n : m_researchNodes) {
                        if (!n.artilleryType.empty() && isAircraftAmmo(n.artilleryType) && hasResearched(n.id))
                            artyTypes.push_back(n.artilleryType);
                    }
                    // Deduplicate
                    std::sort(artyTypes.begin(), artyTypes.end());
                    artyTypes.erase(std::unique(artyTypes.begin(), artyTypes.end()), artyTypes.end());
                    bool hasArty = !artyTypes.empty();

                    // Bombard button
                    shipBtn(panelX+pad, row2Y, sHalf,
                        inBombard ? "Select target province..." : "Bombard Province",
                        !canAct || !hasArty || inMove || inEngage,
                        inBombard ? Color{40,60,40,220} : Color{60,30,60,220},
                        inBombard ? Color{60,100,60,200} : Color{160,60,160,200}, clicked);
                    if (clicked && hasArty && canAct) {
                        if (inBombard) { m_shipActionMode=0; m_shipActionShipIdx=-1; m_shipBombardDropdownOpen=false; }
                        else { m_shipActionMode=3; m_shipActionShipIdx=shipIdx; m_shipBombardDropdownOpen=false; }
                    }

                    // Ammo dropdown next to bombard button
                    bool showAmmoArea = (inBombard || (m_shipActionMode==3 && m_shipActionShipIdx==shipIdx));
                    if (showAmmoArea) {
                        int ammoY = row2Y;
                        int ammoX = panelX+pad+sHalf+sBtnG;
                        int ammoW = sHalf;
                        int ddItemH = 18;
                        int ddTotalH = (int)artyTypes.size() * ddItemH;
                        // Draw the dropdown button
                        const char* ammoLabel = m_shipBombardAmmo.empty() ? "[Select Ammo]" : m_shipBombardAmmo.c_str();
                        Rectangle ammoRect = {(float)ammoX, (float)ammoY, (float)ammoW, (float)sBtnH};
                        bool ammoHov = !m_paused && CheckCollisionPointRec(sm, ammoRect);
                        Color ammoBg = ammoHov ? Color{60,50,60,220} : Color{40,30,40,200};
                        DrawRectangleRounded(ammoRect, 0.08f, 6, ammoBg);
                        DrawRectangleRoundedLines(ammoRect, 0.08f, 6, Color{140,80,140,200});
                        int atw = MeasureText(ammoLabel, 11);
                        DrawText(ammoLabel, ammoX+(ammoW-atw)/2, ammoY+(sBtnH-11)/2, 11, WHITE);
                        // Toggle dropdown on button click
                        if (ammoHov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
                            m_shipBombardDropdownOpen = !m_shipBombardDropdownOpen;
                        // Draw dropdown list
                        if (m_shipBombardDropdownOpen && !artyTypes.empty()) {
                            float artyCosts[] = {5,10,20,30,25,40,80,60};
                            float ctreasury = m_countries.getAll()[m_playerCountryId].treasury;
                            int ddX = ammoX;
                            int ddY = ammoY + sBtnH + 2;
                            for (int ai = 0; ai < (int)artyTypes.size(); ++ai) {
                                float cost = 0;
                                for (int ci = 0; ci < 8; ++ci) {
                                    static const char* AC[] = {"mortar","light","heavy","napalm","carpet","chemical","nuclear","biological"};
                                    if (artyTypes[ai] == AC[ci]) { cost = artyCosts[ci]; break; }
                                }
                                bool canAfford = (ctreasury >= cost);
                                int iy = ddY + ai * ddItemH;
                                bool hov = CheckCollisionPointRec(sm, {(float)ddX, (float)iy, (float)ammoW, (float)ddItemH});
                                Color ibg = hov ? Color{70,50,70,240} : Color{35,25,35,235};
                                DrawRectangle(ddX, iy, ammoW, ddItemH, ibg);
                                DrawRectangleLines(ddX, iy, ammoW, ddItemH, Color{60,40,60,200});
                                // Colored swatch
                                int swatchR = 12, swatchY = iy + (ddItemH - swatchR) / 2;
                                Color artyCol = WHITE;
                                if (artyTypes[ai] == "mortar") artyCol = GREEN;
                                else if (artyTypes[ai] == "light") artyCol = YELLOW;
                                else if (artyTypes[ai] == "heavy") artyCol = ORANGE;
                                else if (artyTypes[ai] == "napalm") artyCol = RED;
                                else if (artyTypes[ai] == "carpet") artyCol = BLUE;
                                else if (artyTypes[ai] == "chemical") artyCol = BROWN;
                                else if (artyTypes[ai] == "nuclear") artyCol = GRAY;
                                else if (artyTypes[ai] == "biological") artyCol = PURPLE;
                                Color textCol = canAfford ? WHITE : Color{100,100,100,255};
                                DrawCircle(ddX + 10, swatchY + swatchR / 2, swatchR / 2, artyCol);
                                DrawText(artyTypes[ai].c_str(), ddX + 20, iy + 2, 10, textCol);
                                DrawText(TextFormat("$%.0f", cost), ddX + ammoW - 40, iy + 2, 10, canAfford ? LIGHTGRAY : Color{80,80,80,255});
                                if (hov && canAfford && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                                    m_shipBombardAmmo = artyTypes[ai];
                                    m_shipBombardDropdownOpen = false;
                                }
                            }
                        }
                        // Click outside closes dropdown
                        if (m_shipBombardDropdownOpen && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                            Rectangle ddRect = {(float)ammoX, (float)(ammoY + sBtnH + 2),
                                               (float)ammoW, (float)((int)artyTypes.size() * ddItemH)};
                            if (!CheckCollisionPointRec(sm, ammoRect) && !CheckCollisionPointRec(sm, ddRect))
                                m_shipBombardDropdownOpen = false;
                        }
                    }
                }

                // Boats: Move handles both water movement and disembark (no separate disembark button)
            }
        } else {
            // ── Multi-ship scrollable list ──
            const int entryH = 44;
            const int headerH = 32;
            int contentH = selCount * entryH + headerH;
            int scrollH = panelH - pad;
            float maxScroll = std::max(0, contentH - scrollH);

            // Clamp scroll
            if (m_shipPanelScroll < 0) m_shipPanelScroll = 0;
            if (m_shipPanelScroll > maxScroll) m_shipPanelScroll = maxScroll;

            // Handle mouse wheel on panel
            Vector2 mouse = getMouse();
            Rectangle pRect = {(float)panelX, (float)panelY, (float)panelW, (float)panelH};
            if (!m_paused && CheckCollisionPointRec(mouse, pRect)) {
                float wheel = GetMouseWheelMove();
                if (wheel != 0) {
                    m_shipPanelScroll -= wheel * 20.0f;
                }
                // Row click: highlight or open
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    int clipY = panelY + headerH;
                    float relY = mouse.y - (clipY + 2 - m_shipPanelScroll);
                    int clickedRow = (relY >= 0) ? (int)(relY / entryH) : -1;
                    if (clickedRow >= 0 && clickedRow < selCount) {
                        if (clickedRow == m_shipListFocusIndex) {
                            // Second click: open this ship (single ship view)
                            int shipIdx = m_selectedShipIndices[clickedRow];
                            Audio::get().playSfx("select_province", 0.05f);
                            m_selectedShipIndices.clear();
                            m_selectedShipIndices.push_back(shipIdx);
                            m_shipListFocusIndex = -1;
                            m_renderer->setSelectedProvince(0);
                            buildCountryShipList(shipIdx);
                        } else {
                            m_shipListFocusIndex = clickedRow;
                            Audio::get().playSfx("click_light");
                        }
                    } else {
                        m_shipListFocusIndex = -1;
                    }
                }
            }

            // Title
            DrawText(TextFormat("Selected Ships (%d)", selCount), panelX + pad, panelY + 6, 16, WHITE);
            DrawLine(panelX, panelY + headerH - 2, panelX + panelW, panelY + headerH - 2, Color{100, 100, 150, 150});

            // Clip region
            int clipY = panelY + headerH;
            int clipH = scrollH - headerH;
            BeginScissorMode(panelX, clipY, panelW, clipH);

            int yOff = clipY + 2 - (int)m_shipPanelScroll;
            for (int i = 0; i < selCount; i++) {
                int idx = m_selectedShipIndices[i];
                auto& ship = m_ships[idx];
                const Country* c = m_countries.getCountry(ship.countryId);
                Color col = c ? c->color : WHITE;
                const char* cname = c ? c->name.c_str() : "Unknown";

                // Row background
                Color bg = Color{255, 255, 255, 8};
                if (i == m_shipListFocusIndex) {
                    bg = Color{80, 120, 200, 60};  // Focus highlight
                } else if ((yOff / entryH) % 2 == 0) {
                    bg = Color{255, 255, 255, 8};
                } else {
                    bg = Color{0, 0, 0, 0};
                }
                DrawRectangle(panelX + 4, yOff, panelW - 8, entryH - 2, bg);

                // Focus border
                if (i == m_shipListFocusIndex) {
                    DrawRectangleLines(panelX + 4, yOff, panelW - 8, entryH - 2, Color{100, 150, 255, 120});
                }

                // Type icon (small)
                int iconX = panelX + 8;
                int iconY = yOff + entryH / 2;
                int sz = 8;
                if (ship.type == "boat") {
                    DrawTriangle({(float)iconX, (float)(iconY - sz)},
                                 {(float)(iconX - sz * 0.6f), (float)(iconY + sz * 0.4f)},
                                 {(float)(iconX + sz * 0.6f), (float)(iconY + sz * 0.4f)}, col);
                } else if (ship.type == "destroyer") {
                    DrawRectangle(iconX - sz/2, iconY - sz/2, sz, sz, col);
                } else {
                    DrawCircle(iconX, iconY, sz * 0.5f, col);
                }

                // Type + crew
                int tx = panelX + 24;
                DrawText(TextFormat("%s", ship.type.c_str()), tx, yOff + 2, 12, WHITE);
                if (ship.crew > 0)
                    DrawText(TextFormat("Crew: %d", ship.crew), tx, yOff + 16, 10, LIGHTGRAY);

                // Country on right
                DrawText(cname, panelX + panelW - pad - MeasureText(cname, 10), yOff + 2, 10, LIGHTGRAY);

                // Health bar
                float hp = ship.health / 100.0f;
                int barX = tx + 90;
                int barY = yOff + 16;
                int barW = 80;
                int barH = 8;
                DrawRectangle(barX, barY, barW, barH, Color{60, 60, 60, 200});
                DrawRectangle(barX, barY, (int)(barW * hp), barH,
                              ship.health > 50 ? GREEN : (ship.health > 25 ? YELLOW : RED));
                DrawText(TextFormat("%d%%", ship.health), barX + barW + 3, barY, 9, WHITE);

                yOff += entryH;
            }

            EndScissorMode();

            // Scrollbar
            if (contentH > scrollH) {
                float barH_f = (float)scrollH / contentH * (clipH);
                float barY_f = clipY + m_shipPanelScroll / maxScroll * (clipH - barH_f);
                DrawRectangle(panelX + panelW - 6, (int)barY_f, 4, (int)barH_f, Color{200, 200, 200, 150});
            }
        }

        m_renderer->setProvincePanelRect({(float)panelX, (float)panelY, (float)panelW, (float)panelH});
        return;
    }

    // === Province info section ===
    int selPid = m_renderer->getSelectedProvinceId();
    int cid = 0;
    const Country* country = nullptr;

    if (selPid > 0) {
        Province* prov = m_provinces.getProvinceById(selPid);
        if (prov) { cid = prov->countryId; country = m_countries.getCountry(cid); }
    }

    // In relations view, use hovered/player country when no province is selected
    if (cid <= 0 && m_activeViewTab == 4) {
        Vector2 mp = getMouse();
        int px, py;
        m_renderer->screenToPixel(mp.x, mp.y, px, py);
        const Province* hp = m_provinces.getProvince(px, py);
        cid = (hp && hp->countryId > 0) ? hp->countryId : m_playerCountryId;
        country = m_countries.getCountry(cid);
    }
    if (!country || cid <= 0) return;

    const int provTopY = panelY + 4;
    const int flagW = 120;
    const int flagH = 60;
    int flagX = panelX + panelW - pad - flagW;
    int flagY = provTopY + pad;

    // Draw province country flag
    auto fit = m_countryFlags.find(cid);
    if (fit != m_countryFlags.end() && fit->second.id > 0) {
        DrawTexturePro(fit->second,
            {0, 0, (float)fit->second.width, (float)fit->second.height},
            {(float)flagX, (float)flagY, (float)flagW, (float)flagH},
            {0, 0}, 0.0f, WHITE);
    }

    // Country name
    int nameY = flagY + flagH + 8;
    int nameSize = 24;
    DrawText(country->name.c_str(), panelX + pad, nameY, nameSize, WHITE);

    // ─── Claim info (shown in relations view or claims view) ─────
    if (m_activeViewTab == 4 || m_showClaims) {
        auto claimIt = m_claimsByProvince.find(selPid);
        if (claimIt != m_claimsByProvince.end() && !claimIt->second.empty()) {
            int claimY = nameY + nameSize + 8;
            DrawText("Claimed by:", panelX + pad, claimY, 16, Color{255, 200, 100, 255});
            int lineY = claimY + 20;
            for (const std::string& claimantIso : claimIt->second) {
                const Country* claimant = nullptr;
                for (auto& [cid, c] : m_countries.getAll()) {
                    if (c.isoA3 == claimantIso) { claimant = &c; break; }
                }
                const char* name = claimant ? claimant->name.c_str() : claimantIso.c_str();
                DrawText(TextFormat("  %s", name), panelX + pad, lineY, 14, LIGHTGRAY);
                lineY += 16;
            }
        }
    }

    // Cache all per-country aggregates in one pass
    if (cid != m_lastPanelCountryId) {
        m_lastPanelCountryId = cid;
        m_cachedProvCount = 0;
        m_cachedCountryIncome = 0.0f;
        m_cachedIndustryCount = 0;
        m_cachedCountryPop = 0;
        m_cachedAvgCompass = {0, 0};
        m_cachedAvgCompassCount = 0;
        for (auto& [pid, p] : m_provinces.getAllProvinces()) {
            if (p.countryId == cid) {
                m_cachedProvCount++;
                auto indIt = m_provinceIndustry.find(pid);
                if (indIt != m_provinceIndustry.end()) {
                    m_cachedCountryIncome += indIt->second.income + indIt->second.resourceIncome + indIt->second.popIncome;
                    if (indIt->second.level > 0) m_cachedIndustryCount++;
                }
                auto popIt = m_provincePopulations.find(pid);
                if (popIt != m_provincePopulations.end()) m_cachedCountryPop += popIt->second;
                auto compIt = m_provinceCompass.find(pid);
                if (compIt != m_provinceCompass.end()) {
                    m_cachedAvgCompass.x += compIt->second.x;
                    m_cachedAvgCompass.y += compIt->second.y;
                    m_cachedAvgCompassCount++;
                }
            }
        }
        if (m_cachedAvgCompassCount > 0) {
            m_cachedAvgCompass.x /= m_cachedAvgCompassCount;
            m_cachedAvgCompass.y /= m_cachedAvgCompassCount;
        }
    }
    int provY = nameY + nameSize + 8;
    DrawText(TextFormat("%d provinces", m_cachedProvCount), panelX + pad, provY, 18, LIGHTGRAY);

    // ─── Country statistics (shown in general view and industry view) ─────
    if (m_activeViewTab == 0 || m_activeViewTab == 2) {
        int statsY = provY + 24;
        if (m_activeViewTab == 4 || m_showClaims) {
            auto claimIt = m_claimsByProvince.find(selPid);
            if (claimIt != m_claimsByProvince.end() && !claimIt->second.empty()) {
                statsY += 20 + claimIt->second.size() * 16;
            }
        }
        DrawText("Country Statistics", panelX + pad, statsY, 18, WHITE);
        DrawText(TextFormat("Annual Income: %.1f", m_cachedCountryIncome),
                 panelX + pad, statsY + 24, 16, hexToColor(m_config.accentColor));
        DrawText(TextFormat("Industrial Provinces: %d", m_cachedIndustryCount),
                 panelX + pad, statsY + 44, 14, LIGHTGRAY);
        if (m_activeViewTab == 0 && cid == m_playerCountryId) {
            DrawText("Keys: 2=Industry 3=Fort 5=Army 6=Navy",
                     panelX + pad, statsY + 64, 12, Color{180, 180, 200, 200});
        }
    }

    // ─── Relations list (shown in relations view) ─────
    if (m_activeViewTab == 4) {
        // Compute claim section height so relations don't overlap
        int claimSectionH = 0;
        {
            auto claimIt2 = m_claimsByProvince.find(selPid);
            if (claimIt2 != m_claimsByProvince.end() && !claimIt2->second.empty()) {
                claimSectionH = 20 + (int)claimIt2->second.size() * 16 + 8;
            }
        }
        int relY = provY + 24 + claimSectionH;
        static int relDebugCounter = 0;
        if (relDebugCounter++ % 120 == 0) {
            std::cout << "Panel: country=" << country->name << " iso=" << country->isoA3
                      << " m_relations.size=" << m_relations.size() << std::endl;
            if (m_relations.count(country->isoA3)) {
                std::cout << "  FRA has " << m_relations[country->isoA3].size() << " entries" << std::endl;
            }
        }
        auto rt = m_relations.find(country->isoA3);
        // Also collect incoming relations for symmetric display
        std::vector<std::pair<std::string, std::string>> incoming;
        if (rt == m_relations.end() || rt->second.empty()) {
            for (auto& [iso, targets] : m_relations) {
                auto st = targets.find(country->isoA3);
                if (st != targets.end()) {
                    std::string relType;
                    if (st->second.war) relType = "war";
                    else if (st->second.alliance) relType = "alliance";
                    else if (st->second.guarantee) relType = "guarantee";
                    else if (st->second.nonAggression) relType = "non_aggression";
                    if (!relType.empty())
                        incoming.push_back({relType, iso});
                }
            }
        }

        if (rt != m_relations.end() && !rt->second.empty()) {
            // Outgoing relations
            for (auto& [target, rel] : rt->second) {
                const char* relLabel = nullptr;
                Color relCol;
                if (rel.war) { relLabel = "War"; relCol = Color{255, 50, 50, 255}; }
                else if (rel.alliance) { relLabel = "Alliance"; relCol = Color{50, 200, 50, 255}; }
                else if (rel.guarantee) { relLabel = "Guarantee"; relCol = Color{255, 255, 50, 255}; }
                else if (rel.nonAggression) { relLabel = "Non-aggr"; relCol = Color{255, 165, 0, 255}; }
                else continue;

                const Country* tc = nullptr;
                for (auto& [tcid, tcEntry] : m_countries.getAll())
                    if (tcEntry.isoA3 == target) { tc = &tcEntry; break; }
                if (tc && relY + 16 < panelY + panelH - 8) {
                    DrawRectangle(panelX + pad, relY, 8, 8, relCol);
                    DrawText(TextFormat("%s: %s", relLabel, tc->name.c_str()),
                             panelX + pad + 14, relY - 3, 12, LIGHTGRAY);
                    relY += 16;
                }
            }
        } else if (!incoming.empty()) {
            // Show incoming relations (no outgoing defined)
            for (auto& [relType, iso] : incoming) {
                Color relCol;
                const char* relLabel;
                if (relType == "war") { relLabel = "War"; relCol = Color{255, 50, 50, 255}; }
                else if (relType == "alliance") { relLabel = "Alliance"; relCol = Color{50, 200, 50, 255}; }
                else if (relType == "guarantee") { relLabel = "Guarantee"; relCol = Color{255, 255, 50, 255}; }
                else if (relType == "non_aggression") { relLabel = "Non-aggr"; relCol = Color{255, 165, 0, 255}; }
                else continue;

                const Country* tc = nullptr;
                for (auto& [tcid2, tcEntry2] : m_countries.getAll())
                    if (tcEntry2.isoA3 == iso) { tc = &tcEntry2; break; }
                if (tc && relY + 16 < panelY + panelH - 8) {
                    DrawRectangle(panelX + pad, relY, 8, 8, relCol);
                    DrawText(TextFormat("%s: %s", relLabel, tc->name.c_str()),
                             panelX + pad + 14, relY - 3, 12, LIGHTGRAY);
                    relY += 16;
                }
            }
        }
    }

    // View-specific info below (province-specific sections guard themselves by tab)
    if (m_activeViewTab == 1) {
        auto provPopIt = m_provincePopulations.find(selPid);
        long long provPop = (provPopIt != m_provincePopulations.end()) ? provPopIt->second : 0;
        float pct = (m_cachedCountryPop > 0) ? (100.0f * provPop / m_cachedCountryPop) : 0.0f;

        int popY = provY + 28;
        DrawText("Population", panelX + pad, popY, 22, WHITE);
        DrawText(TextFormat("Country: %s", formatPop(m_cachedCountryPop).c_str()), panelX + pad + 8, popY + 28, 18, LIGHTGRAY);
        DrawText(TextFormat("Province: %s (%.1f%%)", formatPop(provPop).c_str(), pct), panelX + pad + 8, popY + 52, 18, LIGHTGRAY);

        // Unrest
        float unrest = getProvinceRebellionChance(selPid);
        int unrestY = popY + 80;
        Color uc = unrest < 20 ? GREEN : (unrest < 50 ? ORANGE : RED);
        DrawText(TextFormat("Unrest: %.1f%%", unrest), panelX + pad + 8, unrestY, 16, uc);

        // ─── Political compass ────────────────────────────
        int compSize = 120;
        int compY = popY + 135;
        int compX = panelX + pad + 60;
        int midX = compX + compSize / 2;
        int midY = compY + compSize / 2;
        const int labelFont = 13;

        DrawText("Political Compass", panelX + pad, compY - 30, 18, WHITE);

        // Background
        DrawRectangle(compX, compY, compSize, compSize, {20, 20, 30, 220});
        DrawRectangleLines(compX, compY, compSize, compSize, {100, 100, 120, 200});

        // Axis lines
        DrawLine(compX, midY, compX + compSize, midY, {70, 70, 90, 180});
        DrawLine(midX, compY, midX, compY + compSize, {70, 70, 90, 180});

        // Labels
        DrawText("Left", compX - MeasureText("Left", labelFont) - 4, midY - 7, labelFont, {150, 100, 100, 220});
        DrawText("Right", compX + compSize + 4, midY - 7, labelFont, {100, 100, 150, 220});
        DrawText("Authoritarian", midX - MeasureText("Authoritarian", labelFont) / 2, compY - 14, labelFont, {150, 100, 100, 220});
        DrawText("Liberal", midX - MeasureText("Liberal", labelFont) / 2, compY + compSize + 2, labelFont, {100, 100, 150, 220});

        // Ensure compass data exists (fallback hash-based if missing)
        if (m_provinceCompass.find(selPid) == m_provinceCompass.end()) {
            int h = selPid * 1103515245 + 12345;
            m_provinceCompass[selPid] = {(float)((h & 0xFF) % 201 - 100), (float)(((h >> 8) & 0xFF) % 201 - 100)};
        }

        auto compIt = m_provinceCompass.find(selPid);
        if (compIt != m_provinceCompass.end()) {
            float px = compX + compSize / 2 - compIt->second.x * (compSize / 2 - 8) / 100.0f;
            float py = compY + compSize / 2 - compIt->second.y * (compSize / 2 - 8) / 100.0f;

            // Glow
            DrawCircle((int)px, (int)py, 12, {255, 220, 50, 60});            DrawCircle((int)px, (int)py, 9, {255, 220, 50, 120});
            DrawCircle((int)px, (int)py, 6, {255, 220, 50, 255});
            DrawCircleLines((int)px, (int)py, 6, WHITE);

            // Numerical values
            int valY = compY + compSize + 22;
            DrawText(TextFormat("Left-Right: %+d", (int)compIt->second.x), panelX + pad, valY, 14, LIGHTGRAY);
            DrawText(TextFormat("Auth-Lib:   %+d", (int)compIt->second.y), panelX + pad, valY + 18, 14, LIGHTGRAY);

            // Draw country average as a smaller cross
            float ax = compX + compSize / 2 - m_cachedAvgCompass.x * (compSize / 2 - 8) / 100.0f;
            float ay = compY + compSize / 2 - m_cachedAvgCompass.y * (compSize / 2 - 8) / 100.0f;
            DrawLine((int)ax - 4, (int)ay, (int)ax + 4, (int)ay, LIGHTGRAY);
            DrawLine((int)ax, (int)ay - 4, (int)ax, (int)ay + 4, LIGHTGRAY);
        }

        // ─── Minority pie chart ────────────────────────────
        auto minorIt = m_provinceMinorities.find(selPid);
        if (minorIt != m_provinceMinorities.end() && !minorIt->second.empty()) {
            int pieY = compY + compSize + 80;
            int pieSize = 90;
            int pieX = panelX + pad + 8;
            Vector2 pieCenter{(float)(pieX + pieSize / 2), (float)(pieY + pieSize / 2)};
            float pieRadius = pieSize / 2.0f - 2;

            DrawText("Ethnic Groups", panelX + pad, pieY - 18, 16, WHITE);

            float startAngle = -90.0f;
            for (auto& g : minorIt->second) {
                float sweep = g.pct * 3.6f;
                auto colIt = m_minorityColors.find(g.name);
                Color col = colIt != m_minorityColors.end() ? colIt->second : DARKGRAY;
                DrawCircleSector(pieCenter, pieRadius, startAngle, startAngle + sweep, 32, col);
                startAngle += sweep;
            }
            DrawCircleLines((int)pieCenter.x, (int)pieCenter.y, (int)pieRadius, {100, 100, 120, 200});

            // Legend
            int legX = pieX + pieSize + 12;
            int legY = pieY;
            int maxItems = (compY + compSize + 55 + pieSize - legY) / 16;
            int itemCount = 0;
            for (auto& g : minorIt->second) {
                if (itemCount >= maxItems) break;
                auto colIt = m_minorityColors.find(g.name);
                Color col = colIt != m_minorityColors.end() ? colIt->second : DARKGRAY;
                DrawRectangle(legX, legY + itemCount * 16, 10, 10, col);
                DrawRectangleLines(legX, legY + itemCount * 16, 10, 10, {100, 100, 120, 150});
                DrawText(g.name.c_str(), legX + 14, legY + itemCount * 16, 12, LIGHTGRAY);
                DrawText(TextFormat("%.1f%%", g.pct), legX + 14 + MeasureText(g.name.c_str(), 12) + 6, legY + itemCount * 16, 12, GRAY);
                itemCount++;
            }
        }
    }

    // ─── Resource info (when in resource view) ─────
    if (m_activeViewTab == 7) {
        auto resIt = m_provinceResources.find(selPid);
        if (resIt != m_provinceResources.end()) {
            int rY = panelY + 155;
            int rX = panelX + pad;
            DrawText("Resources", rX, rY - 18, 16, WHITE);
            const auto& res = resIt->second;
            const ProvinceResource* prs[5] = {&res.oil, &res.gold, &res.rubber, &res.gemstones, &res.metal};
            for (int i = 0; i < 5; ++i) {
                int lineY = rY + i * 16;
                Color col = (i == m_activeResourceIdx) ? WHITE : LIGHTGRAY;
                DrawText(TextFormat("%s: %.1f  (boost: +%.1f%%)",
                    RESOURCE_NAMES[i], prs[i]->amount, prs[i]->boost),
                    rX, lineY, 14, col);
            }
        }
    }

    // ─── Industry info (when in industry view) ─────
    if (m_activeViewTab == 2) {
        auto indIt = m_provinceIndustry.find(selPid);
        if (indIt != m_provinceIndustry.end()) {
            int rY = panelY + 200;
            int rX = panelX + pad;
            const auto& ind = indIt->second;
            DrawText(TextFormat("Level %s", ROMAN_NUMERALS[ind.level]),
                     rX, rY, 16, WHITE);
            DrawText(TextFormat("Income: %.1f/turn", ind.income),
                     rX, rY + 20, 14, YELLOW);
            DrawText(TextFormat("Resource income: %.1f", ind.resourceIncome),
                     rX, rY + 40, 14, LIGHTGRAY);
            DrawText(TextFormat("Population bonus: +%.0f%% (x%.2f)",
                     (ind.popModifier - 1.0f) * 100.0f, ind.popModifier),
                     rX, rY + 60, 14, LIGHTGRAY);
            if (!ind.specialization.empty()) {
                DrawText(TextFormat("Specialization: %s", ind.specialization.c_str()),
                         rX, rY + 82, 14, LIGHTGRAY);
                auto resIt = m_provinceResources.find(selPid);
                if (resIt != m_provinceResources.end()) {
                    const auto& res = resIt->second;
                    float bonus = 0.0f;
                    if (ind.specialization == "Oil") bonus = res.oil.boost;
                    else if (ind.specialization == "Gold") bonus = res.gold.boost;
                    else if (ind.specialization == "Rubber") bonus = res.rubber.boost;
                    else if (ind.specialization == "Gemstones") bonus = res.gemstones.boost;
                    else if (ind.specialization == "Metal") bonus = res.metal.boost;
                    if (bonus > 0.0f)
                        DrawText(TextFormat("Specialization bonus: +%.1f%%", bonus),
                                 rX, rY + 102, 14, GREEN);
                }
            }
        }
    }

    // ─── Defence info (when in defence view) ─────
    if (m_activeViewTab == 3) {
        auto indIt = m_provinceIndustry.find(selPid);
        if (indIt != m_provinceIndustry.end()) {
            int rY = panelY + 200;
            int rX = panelX + pad;
            const auto& ind = indIt->second;
            DrawText("Fortification", rX, rY, 18, WHITE);
            DrawText(TextFormat("Level: %d/6", ind.fortification),
                     rX, rY + 24, 16, LIGHTGRAY);
            DrawText(TextFormat("Defence bonus: +%d%%", ind.fortification * 10),
                     rX, rY + 44, 14, GREEN);
        } else {
            int rY = panelY + 200;
            int rX = panelX + pad;
            DrawText("Fortification", rX, rY, 18, WHITE);
            DrawText("Level: 0/6", rX, rY + 24, 16, LIGHTGRAY);
            DrawText("Defence bonus: +0%", rX, rY + 44, 14, LIGHTGRAY);
        }
    }

    // ─── Army info (when in army view) ─────
    if (m_activeViewTab == 5) {
        auto armyIt = m_provinceArmies.find(selPid);
        int rY = panelY + 200;
        int rX = panelX + pad;
        DrawText("Garrison", rX, rY, 18, WHITE);
        if (armyIt != m_provinceArmies.end() && !armyIt->second.empty()) {
            int lineY = rY + 24;
            for (auto& unit : armyIt->second) {
                const Country* c = m_countries.getCountry(unit.countryId);
                const char* cname = c ? c->name.c_str() : "Unknown";
                long long disp = unit.count / 100;
                DrawText(TextFormat("%s: %s soldiers", cname, formatPop(disp).c_str()),
                         rX, lineY, 14, LIGHTGRAY);
                lineY += 18;
            }
        } else {
            DrawText("No garrison", rX, rY + 24, 14, LIGHTGRAY);
        }
    }

    // ─── Navy info (when in navy view) ─────
    if (m_activeViewTab == 6) {
        int rY = panelY + 200;
        int rX = panelX + pad;
        auto portIt = m_provincePorts.find(selPid);
        if (portIt != m_provincePorts.end()) {
            DrawText("Port", rX, rY, 18, WHITE);
            DrawText(TextFormat("Level: %d", portIt->second.level),
                     rX, rY + 24, 14, LIGHTGRAY);
        }
        // Check for ships near this province
        auto cit = m_provinceCenters.find(selPid);
        if (cit != m_provinceCenters.end()) {
            int shipCount = 0;
            for (auto& ship : m_ships) {
                if (ship.countryId == cid) shipCount++;
            }
            if (shipCount > 0) {
                int sy = rY + (portIt != m_provincePorts.end() ? 48 : 0);
                DrawText(TextFormat("Country ships: %d", shipCount), rX, sy, 14, LIGHTGRAY);
            }
        }
    }

    // ─── Action Buttons ────────────
    // Helper: draw a single action button, returns true if clicked
    auto drawActBtn = [&](int x, int y, int w, int h, const char* label, bool disabled, Color bg, Color border) -> bool {
        Rectangle r = {(float)x, (float)y, (float)w, (float)h};
        Vector2 mse = getMouse();
        bool hovered = !m_paused && m_turnState == TURN_NORMAL && !disabled && CheckCollisionPointRec(mse, r);
        Color bgc = disabled ? Color{20, 20, 25, 200} : (hovered ? Color{
            (unsigned char)std::min(255, bg.r + 30),
            (unsigned char)std::min(255, bg.g + 30),
            (unsigned char)std::min(255, bg.b + 30), bg.a} : bg);
        DrawRectangleRounded(r, 0.08f, 6, bgc);
        Color bdc = disabled ? Color{40, 40, 50, 150} : border;
        DrawRectangleRoundedLines(r, 0.08f, 6, bdc);
        Color tc = disabled ? Color{80, 80, 80, 200} : WHITE;
        int fs = 11;
        int tw = MeasureText(label, fs);
        DrawText(label, x + (w - tw) / 2, y + (h - fs) / 2, fs, tc);

        // Centralised on purpose: seventeen call sites reach this lambda, and
        // wiring the sound at each of them is seventeen chances to forget one.
        const int id = x * 73856093 ^ y * 19349663 ^ w * 83492791;
        if (hovered && m_lastHoverBtn != id) {
            m_lastHoverBtn = id;
            Audio::get().playSfx("hover");
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(mse, r)) {
            // A disabled button still answers -- saying no is feedback too.
            Audio::get().playSfx(disabled ? "deny"
                                          : (m_btnSfxOverride ? m_btnSfxOverride
                                                              : "click_heavy"));
        }
        m_btnSfxOverride = nullptr;   // one button only, never the next one
        return hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    };

    // ─── Diplomatic Action Buttons (foreign province, not spectator, not UNC/BLC) ───
    if (cid != m_playerCountryId && m_playerCountryId > 0 && cid > 0 && cid != UNC_CID && cid != BLC_CID) {
        const Country* playerC = m_countries.getCountry(m_playerCountryId);
        const Country* targetC = m_countries.getCountry(cid);
        if (playerC && targetC) {
            bool hasWar = false, hasAlly = false, hasGuar = false, hasNonAgg = false;
            auto rt1 = m_relations.find(playerC->isoA3);
            if (rt1 != m_relations.end()) {
                auto st = rt1->second.find(targetC->isoA3);
                if (st != rt1->second.end()) {
                    hasWar = st->second.war;
                    hasAlly = st->second.alliance;
                    hasGuar = st->second.guarantee;
                    hasNonAgg = st->second.nonAggression;
                }
            }
            if (!hasWar && !hasAlly && !hasGuar && !hasNonAgg) {
                auto rt2 = m_relations.find(targetC->isoA3);
                if (rt2 != m_relations.end()) {
                    auto st = rt2->second.find(playerC->isoA3);
                    if (st != rt2->second.end()) {
                        hasWar = st->second.war;
                        hasAlly = st->second.alliance;
                        hasGuar = st->second.guarantee;
                        hasNonAgg = st->second.nonAggression;
                    }
                }
            }

            auto hasPending = [&](const std::string& action) -> bool {
                for (auto& p : m_pendingDiplomaticActions)
                    if (p.sourceIso == playerC->isoA3 && p.targetIso == targetC->isoA3 && p.action == action)
                        return true;
                return false;
            };
            auto cancelPending = [&](const std::string& action) {
                for (size_t i = 0; i < m_pendingDiplomaticActions.size(); ) {
                    if (m_pendingDiplomaticActions[i].sourceIso == playerC->isoA3 &&
                        m_pendingDiplomaticActions[i].targetIso == targetC->isoA3 &&
                        m_pendingDiplomaticActions[i].action == action) {
                        m_pendingDiplomaticActions.erase(m_pendingDiplomaticActions.begin() + i);
                    } else { ++i; }
                }
            };

            // Lock other diplo buttons when any action is pending for this pair
            bool anyDiploPending = false;
            for (auto& pda : m_pendingDiplomaticActions)
                if (pda.sourceIso == playerC->isoA3 && pda.targetIso == targetC->isoA3)
                    { anyDiploPending = true; break; }

            struct ActBtn { const char* label; std::string action; bool disabled; };
            std::vector<ActBtn> acts;

            if (hasAlly) {
                acts.push_back({"Break Alliance", "break_alliance", anyDiploPending && !hasPending("break_alliance")});
                acts.push_back({"Request Mutual Guarantee", "add_guarantee", anyDiploPending && !hasPending("add_guarantee")});
            } else if (hasGuar) {
                acts.push_back({"Break Guarantee", "break_guarantee", anyDiploPending && !hasPending("break_guarantee")});
                acts.push_back({"Request Alliance", "request_alliance", anyDiploPending && !hasPending("request_alliance")});
            } else if (hasWar) {
                acts.push_back({"Request Ceasefire", "request_ceasefire", anyDiploPending && !hasPending("request_ceasefire")});
            } else if (!hasWar) {
                if (hasNonAgg) {
                    acts.push_back({"Break NAP", "break_nap", anyDiploPending && !hasPending("break_nap")});
                } else {
                    acts.push_back({"Request NAP", "request_nap", anyDiploPending && !hasPending("request_nap")});
                }
                acts.push_back({"Request Alliance", "request_alliance", anyDiploPending && !hasPending("request_alliance")});
                acts.push_back({"Request Guarantee", "request_guarantee", anyDiploPending && !hasPending("request_guarantee")});
                if (!hasNonAgg)
                    acts.push_back({"Declare War", "declare_war", anyDiploPending && !hasPending("declare_war")});
            }

            // Layout: 2 columns, single button spans full width
            int btnGap = 4;
            int fullBtnW = panelW - pad * 2;
            int halfBtnW = (panelW - pad * 2 - btnGap) / 2;
            int btnH = 26;
            int rows = (int)acts.size();
            bool singleRow = (rows <= 1);
            int cols = singleRow ? 1 : 2;
            int totalRows = singleRow ? 1 : (rows + 1) / 2;
            int areaH = totalRows * (btnH + btnGap);
            int btnStartY = panelY + panelH - 56 - areaH - 6;

            int col = 0, row = 0;
            for (auto& ab : acts) {
                int btnW = (singleRow || (row == totalRows - 1 && col == 0 && rows % 2 == 1)) ? fullBtnW : halfBtnW;
                int bx = col == 1 ? panelX + pad + halfBtnW + btnGap : panelX + pad;
                int by = btnStartY + row * (btnH + btnGap);
                bool pending = hasPending(ab.action);
                std::string labelStr = pending ? std::string("Cancel ") + ab.action : std::string(ab.label);
                // Replace underscores with spaces for cancel text
                for (size_t i = 0; i < labelStr.size(); ++i)
                    if (labelStr[i] == '_') labelStr[i] = ' ';
                if (ab.action == "request_ceasefire" && pending)
                    labelStr = "Ceasefire Pending...";
                const char* label = labelStr.c_str();
                bool disabled = ab.disabled;
                // Disable cancel for ceasefire requests while awaiting review
                if (ab.action == "request_ceasefire" && pending)
                    disabled = true;
                Color bg, border;
                if (ab.action == "declare_war") {
                    bg = Color{80, 20, 20, 220}; border = Color{180, 60, 60, 200};
                } else if (ab.action.find("break_") == 0) {
                    bg = Color{80, 40, 20, 220}; border = Color{180, 120, 60, 200};
                } else if (ab.action.find("request_") == 0 || ab.action == "add_guarantee") {
                    bg = Color{20, 50, 80, 220}; border = Color{60, 120, 180, 200};
                } else {
                    bg = Color{30, 30, 50, 220}; border = Color{80, 80, 120, 200};
                }
                if (pending) { bg = Color{40, 30, 10, 220}; border = Color{200, 160, 50, 200}; }
                // Each diplomatic act has its own recording. Cancelling a
                // pending one is not the act itself, so that keeps the plain
                // click -- as do break_* and anything else unnamed.
                if (!pending) {
                    if      (ab.action == "declare_war")        m_btnSfxOverride = "declare_war";
                    else if (ab.action == "request_alliance")   m_btnSfxOverride = "request_alliance";
                    else if (ab.action == "request_nap")        m_btnSfxOverride = "request_nap";
                    else if (ab.action == "request_guarantee")  m_btnSfxOverride = "request_guarantee";
                    else if (ab.action == "request_ceasefire")  m_btnSfxOverride = "request_ceasefire";
                }
                if (drawActBtn(bx, by, btnW, btnH, label, disabled, bg, border) && !disabled) {
                    if (pending) cancelPending(ab.action);
                    else if (ab.action == "request_ceasefire") {
                        // Open ceasefire negotiation screen
                        m_ceasefireTargetIso = targetC->isoA3;
                        m_ceasefireOurMoney = 0;
                        m_ceasefireTheirMoney = 0;
                        m_ceasefireOurProvs.clear();
                        m_ceasefireTheirProvs.clear();
                        m_ceasefireOurDropClaims.clear();
                        m_ceasefireTheirDropClaims.clear();
                        m_ceasefireSelectMode = 0;
                        m_ceasefireOverlayDirty = true;
                        m_inCeasefireScreen = true;
                    } else {
                        m_pendingDiplomaticActions.push_back({playerC->isoA3, targetC->isoA3, ab.action, 1});
                    }
                }
                col++;
                if (col >= cols) { col = 0; row++; }
            }
        }
    }

    // ─── Industry Action Buttons (own province, industry view only) ─────
    if (cid == m_playerCountryId && m_activeViewTab == 2) {
        Province* pInfo = m_provinces.getProvinceById(selPid);
        bool isOwnProv = (selPid > 0 && pInfo && pInfo->countryId == m_playerCountryId);
        auto indIt = m_provinceIndustry.find(selPid);
        int indLevel = (indIt != m_provinceIndustry.end()) ? indIt->second.level : 0;
        int maxIndLevel = getResearchedIndustryLevel();
        bool atResearchCap = (indLevel >= maxIndLevel);
        bool atHardCap = (indLevel >= 10);

        // Check if upgrade is pending
        bool upgradePending = false;
        for (auto& pu : m_pendingUpgrades)
            if (pu.provinceId == selPid && pu.type == "industry") upgradePending = true;
        bool specPending = false;
        for (auto& ps : m_pendingSpecializations)
            if (ps.provinceId == selPid) specPending = true;

        static const int IND_COST[] = {0, 1, 10, 15, 25, 50, 75, 100, 150, 200, 300};
        static const int IND_TURNS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

        auto cs = computeCountryIncome(m_playerCountryId);
        double& treasury = m_countries.getAll()[m_playerCountryId].treasury;
        // Clamp both ends: `level` comes unvalidated from resources.json and
        // from save deltas, and a negative or >10 value used to index these
        // fixed-size tables out of bounds, printing genuine garbage.
        static const int IND_MAX_LV = (int)(sizeof(IND_COST) / sizeof(IND_COST[0])) - 1;
        int nextLv = std::clamp(indLevel + 1, 0, IND_MAX_LV);
        bool nextLvValid = (indLevel + 1) >= 0 && (indLevel + 1) <= IND_MAX_LV;
        // "Industry cost -50%" is registered as a POSITIVE 50, so this has to
        // subtract. It used to add, making the discount research raise the
        // price to 1.5x instead of halving it.
        float costMod = std::max(0.0f, 1.0f - getTotalEffect("industryCostPct") / 100.0f);
        float upgradeCost = (float)IND_COST[nextLv] * costMod;
        bool canAfford = nextLvValid && (treasury >= upgradeCost);
        int turnsToBuild = IND_TURNS[nextLv];

        int btnW = (panelW - pad * 2 - 4) / 2;
        int btnH = 28;
        int btnGap = 4;
        int btnStartY = panelY + panelH - 56 - 2 * (btnH + btnGap) - 8;

        // Upgrade / locked / max level button
        bool upgDisabled = !isOwnProv || upgradePending || atHardCap;
        const char* upgLabel;
        Color upgBg, upgBd;
        if (atHardCap) {
            upgLabel = "Max level (10)";
            upgDisabled = true;
            upgBg = Color{30, 30, 20, 200}; upgBd = Color{80, 80, 50, 150};
        } else if (atResearchCap && maxIndLevel < 10) {
            upgLabel = "Upgrade locked, research next industry";
            upgDisabled = true;
            upgBg = Color{30, 30, 40, 200}; upgBd = Color{80, 80, 120, 150};
        } else if (!canAfford) {
            upgLabel = TextFormat("Upgrade to level %d ($%.0f, 0/%dt)", nextLv, upgradeCost, turnsToBuild);
            upgDisabled = true;
            upgBg = Color{20, 20, 25, 200}; upgBd = Color{40, 40, 50, 150};
        } else if (upgradePending) {
            upgLabel = TextFormat("Building... (%d turns)", turnsToBuild);
            upgBg = Color{30, 40, 20, 220}; upgBd = Color{80, 120, 60, 200};
        } else {
            upgLabel = TextFormat("Upgrade to level %d ($%.0f, 0/%dt)", nextLv, upgradeCost, turnsToBuild);
            upgBg = Color{20, 60, 30, 220}; upgBd = Color{60, 180, 80, 200};
        }
        if (drawActBtn(panelX + pad, btnStartY, btnW * 2 + btnGap, btnH, upgLabel, upgDisabled, upgBg, upgBd) && !upgDisabled && !atHardCap && !atResearchCap && canAfford && !upgradePending) {
            treasury -= upgradeCost;
            m_pendingUpgrades.push_back({selPid, "industry", nextLv, turnsToBuild});
            Audio::get().playSfx("build_industry", 0.04f);
        }

        // Specialization: dropdown on click
        int specBtnY = btnStartY + btnH + btnGap;
        static const char* specOpts[] = {"Oil", "Gold", "Metal", "Rubber", "Gemstones"};
        // Priced off the CURRENT industry level, not the next one. Deriving it
        // from upgradeCost meant that at max level it inherited the
        // "no next level" sentinel and displayed ~$150,000, which also made
        // specialising impossible exactly when you'd most want it.
        float specCost = (float)IND_COST[std::clamp(indLevel, 0, IND_MAX_LV)] * 1.5f * costMod;
        bool canSpec = isOwnProv && indLevel >= 1 && !specPending && treasury >= specCost;
        Color specBg = canSpec ? Color{40, 30, 60, 220} : Color{20, 20, 25, 200};
        Color specBd = canSpec ? Color{120, 80, 180, 200} : Color{40, 40, 50, 150};
        std::string currentSpec = (indIt != m_provinceIndustry.end()) ? indIt->second.specialization : "";
        std::string specTarget;
        for (auto& sp : m_pendingSpecializations) if (sp.provinceId == selPid) specTarget = sp.specialization;
        const char* specLabel = specPending
                               ? TextFormat("Specializing to %s... (3t)", specTarget.c_str())
                               : TextFormat("Specialize (%s) ($%.0f, 3t)",
                                   currentSpec.empty() ? "choose" : currentSpec.c_str(), specCost);
        if (drawActBtn(panelX + pad, specBtnY, btnW * 2 + btnGap, btnH, specLabel, !canSpec, specBg, specBd) && canSpec) {
            if (m_specDropdownProvince == selPid) m_specDropdownProvince = -1;
            else { m_specDropdownProvince = selPid; m_specDropdownHover = 0; }
        }

        // Draw specialization dropdown if open
        if (m_specDropdownProvince == selPid && isOwnProv) {
            int ddX = panelX + pad;
            int ddY = specBtnY + btnH + 2;
            int ddW = btnW * 2 + btnGap;
            int ddH = 26;
            Vector2 mse = getMouse();
            int optsCount = 0;
            for (int i = 0; i < 5; ++i) {
                if (std::string(specOpts[i]) == currentSpec && !currentSpec.empty()) continue;
                int iy = ddY + optsCount * ddH;
                bool hover = CheckCollisionPointRec(mse, {(float)ddX, (float)iy, (float)ddW, (float)ddH});
                Color bgc = hover ? Color{60, 60, 100, 250} : Color{35, 35, 55, 240};
                DrawRectangle(ddX, iy, ddW, ddH, bgc);
                DrawRectangleLines(ddX, iy, ddW, ddH, Color{80, 80, 120, 220});
                DrawText(specOpts[i], ddX + 6, iy + 4, 13, WHITE);
                if (hover) m_specDropdownHover = i;
                optsCount++;
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                float relY = mse.y;
                if (relY >= ddY && relY < ddY + optsCount * ddH) {
                    int idx = (int)((relY - ddY) / ddH);
                    int realIdx = 0;
                    int foundIdx = -1;
                    for (int i = 0; i < 5; ++i) {
                        if (std::string(specOpts[i]) == currentSpec && !currentSpec.empty()) continue;
                        if (realIdx == idx) { foundIdx = i; break; }
                        realIdx++;
                    }
                    if (foundIdx >= 0) {
                        treasury -= specCost;
                        m_pendingSpecializations.push_back({selPid, specOpts[foundIdx], 3});
                    }
                    m_specDropdownProvince = -1;
                }
            }
        }
    }

    // ─── Fortification Action Buttons (own province, defence view) ─────
    if (cid == m_playerCountryId && m_activeViewTab == 3) {
        Province* pInfo = m_provinces.getProvinceById(selPid);
        bool isOwnProv = (selPid > 0 && pInfo && pInfo->countryId == m_playerCountryId);
        auto indIt = m_provinceIndustry.find(selPid);
        int fortLevel = (indIt != m_provinceIndustry.end()) ? indIt->second.fortification : 0;
        int maxFort = getResearchedFortLevel();
        bool atResearchCap = (fortLevel >= maxFort);
        bool atHardCap = (fortLevel >= 5);
        bool fortPending = false;
        for (auto& pu : m_pendingUpgrades)
            if (pu.provinceId == selPid && pu.type == "fortification") fortPending = true;

        static const int FORT_COST[] = {0, 20, 30, 50, 100, 200};
        static const int FORT_TURNS[] = {0, 1, 1, 1, 1, 1};

        auto cs = computeCountryIncome(m_playerCountryId);
        double& treasury = m_countries.getAll()[m_playerCountryId].treasury;
        int nextLv = fortLevel + 1;
        float fortCost = (nextLv <= 5) ? (float)FORT_COST[nextLv] : 99999.0f;
        // No research affects fortification cost (there is no "fortCostPct"
        // effect); the old lookup of that name silently returned 0 anyway.
        float costMod = 1.0f;
        fortCost *= costMod;
        bool canAfford = (treasury >= fortCost);
        int turnsToBuild = (nextLv <= 5) ? FORT_TURNS[nextLv] : 0;
        int btnH = 28;

        // Calculate dynamic Y position based on claim section height
        int claimSectionH = 0;
        if (m_activeViewTab == 4 || m_showClaims) {
            auto claimIt = m_claimsByProvince.find(selPid);
            if (claimIt != m_claimsByProvince.end() && !claimIt->second.empty())
                claimSectionH = 20 + (int)claimIt->second.size() * 16;
        }
        int statsUsedH = 0;
        if (m_activeViewTab == 0 || m_activeViewTab == 2)
            statsUsedH = 80;
        int btnStartY = panelY + panelH - 56 - btnH - 4;

        bool btnDisabled = !isOwnProv || fortPending || atHardCap;
        const char* label;
        Color bg, bd;
        if (atHardCap) {
            label = "Max level (5)";
            btnDisabled = true;
            bg = Color{30, 30, 20, 200}; bd = Color{80, 80, 50, 150};
        } else if (atResearchCap && maxFort < 5) {
            label = TextFormat("Research next level (max %d)", maxFort);
            btnDisabled = true;
            bg = Color{30, 30, 40, 200}; bd = Color{80, 80, 120, 150};
        } else if (!canAfford) {
            label = fortLevel == 0
                ? TextFormat("Build Fort ($%.0f, 0/%dt)", fortCost, turnsToBuild)
                : TextFormat("Upgrade Fort to level %d ($%.0f, 0/%dt)", nextLv, fortCost, turnsToBuild);
            btnDisabled = true;
            bg = Color{20, 20, 25, 200}; bd = Color{40, 40, 50, 150};
        } else if (fortPending) {
            label = TextFormat("Building... (%d turns)", turnsToBuild);
            bg = Color{30, 40, 20, 220}; bd = Color{80, 120, 60, 200};
        } else {
            label = fortLevel == 0
                ? TextFormat("Build Fort ($%.0f, 0/%dt)", fortCost, turnsToBuild)
                : TextFormat("Upgrade Fort to level %d ($%.0f, 0/%dt)", nextLv, fortCost, turnsToBuild);
            bg = Color{20, 50, 50, 220}; bd = Color{60, 160, 160, 200};
        }
        if (drawActBtn(panelX + pad, btnStartY, panelW - pad * 2, btnH, label, btnDisabled, bg, bd) && !btnDisabled && !atHardCap && !atResearchCap && canAfford && !fortPending) {
            treasury -= fortCost;
            m_pendingUpgrades.push_back({selPid, "fortification", nextLv, turnsToBuild});
            // At the order, not at completion: this is the click's feedback.
            // Finishing is already announced by a notification.
            Audio::get().playSfx("build_fortification", 0.04f);
        }
    }

    // ─── Army Action Buttons (own province, army view) ─────
    if (cid == m_playerCountryId && m_activeViewTab == 5) {
        Province* pInfo = m_provinces.getProvinceById(selPid);
        bool isOwnProv = (selPid > 0 && pInfo && pInfo->countryId == m_playerCountryId);
        auto armyIt = m_provinceArmies.find(selPid);
        int totalSoldiers = 0;
        if (armyIt != m_provinceArmies.end()) {
            for (auto& u : armyIt->second)
                if (u.countryId == m_playerCountryId) totalSoldiers += u.count;
        }
        auto popIt = m_provincePopulations.find(selPid);
        long long provPop = (popIt != m_provincePopulations.end()) ? popIt->second : 0;

        int btnH = 28;
        int btnGap = 4;
        int btnW = (panelW - pad * 2 - 4) / 2;
        int btnStartY = panelY + panelH - 56 - 3 * (btnH + btnGap) - 8;

        // Recruit button
        bool hasPendingRecruit = false;
        for (auto& pr : m_pendingRecruitments)
            if (pr.provinceId == selPid) hasPendingRecruit = true;

        long long maxRecruit = provPop / 5;  // 20% of population per turn max
        // Apply research modifiers (increases conscription cap)
        float conscriptionMod = 1.0f + getTotalEffect("conscriptionPct") / 100.0f;
        maxRecruit = (long long)(maxRecruit * conscriptionMod);
        // Unrest reduces willingness to be conscripted
        float unrestFactor = 1.0f - getProvinceRebellionChance(selPid);
        if (unrestFactor < 0.1f) unrestFactor = 0.1f;
        maxRecruit = (long long)(maxRecruit * unrestFactor);
        if (maxRecruit < 0) maxRecruit = 0;

        int recruitCount = (int)(maxRecruit * m_armyRecruitPct / 100);
        // Cost: $1 per 10k soldiers, research cost modifier applies
        // "armyCostPct" is not a real effect name, so this always returned 0 and
        // recruit prices ignored research entirely. The actual effect is
        // conscriptionCostPct, and it's a cost REDUCTION, so it subtracts.
        float costMod = std::max(0.0f, 1.0f - getTotalEffect("conscriptionCostPct") / 100.0f);
        float recruitCost = (recruitCount / 10000.0f) * costMod;
        if (recruitCost < 1.0f && recruitCount > 0) recruitCost = 1.0f;
        auto cs = computeCountryIncome(m_playerCountryId);
        double& treasury = m_countries.getAll()[m_playerCountryId].treasury;
        bool canRecruit = isOwnProv && recruitCount > 0 && !hasPendingRecruit && treasury >= recruitCost;

        // Recruit button with slider
        int sliderY = btnStartY - 22;
        int sliderW = btnW * 2 + btnGap - 60;
        int slX = panelX + pad;
        int slY = sliderY + 2;
        DrawText("R:", slX, sliderY, 11, LIGHTGRAY);
        slX += 18;
        sliderW -= 18;
        DrawRectangle(slX, slY, sliderW, 14, {30, 30, 40, 200});
        int fill = sliderW * m_armyRecruitPct / 100;
        if (fill > 0) DrawRectangle(slX, slY, fill, 14, {40, 80, 40, 200});
        DrawRectangleLines(slX, slY, sliderW, 14, {60, 60, 80, 200});
        DrawText(TextFormat("%d%%", m_armyRecruitPct), slX + sliderW + 3, slY, 9, WHITE);
        Vector2 mse = getMouse();
        Rectangle slRec = {(float)slX, (float)slY, (float)sliderW, 14.0f};
        bool sliderChanged = false;
        m_armySliderActive = !m_paused && CheckCollisionPointRec(mse, slRec) && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        if (m_armySliderActive) {
            int oldPct = m_armyRecruitPct;
            m_armyRecruitPct = (int)((mse.x - slX) / sliderW * 100);
            if (m_armyRecruitPct < 0) m_armyRecruitPct = 0;
            if (m_armyRecruitPct > 100) m_armyRecruitPct = 100;
            // If pending order exists, cap by remaining treasury (one stable clamp)
            if (hasPendingRecruit) {
                long long oldCount = 0;
                for (auto& pr : m_pendingRecruitments)
                    if (pr.provinceId == selPid) { oldCount = pr.count; break; }
                float oldCost = (oldCount / 10000.0f) * costMod;
                if (oldCost < 1.0f && oldCount > 0) oldCost = 1.0f;
                float maxCost = oldCost + treasury;
                int tryPct = m_armyRecruitPct;
                for (int p = tryPct; p >= 0; --p) {
                    int c = (int)(maxRecruit * p / 100);
                    float cost = (c / 10000.0f) * costMod;
                    if (cost < 1.0f && c > 0) cost = 1.0f;
                    if (cost <= maxCost) { m_armyRecruitPct = p; break; }
                }
            }
            if (m_armyRecruitPct != oldPct) sliderChanged = true;
        }

        int recruitBtnY = btnStartY;
        if (hasPendingRecruit) {
            // Update existing order in real-time when slider changes
            for (size_t pri = 0; pri < m_pendingRecruitments.size(); ++pri) {
                auto& pr = m_pendingRecruitments[pri];
                if (pr.provinceId != selPid) continue;
                int newCount = recruitCount;
                int oldCount = pr.count;
                if (sliderChanged && newCount != oldCount) {
                    float oldCost = (oldCount / 10000.0f) * costMod;
                    if (oldCost < 1.0f && oldCount > 0) oldCost = 1.0f;
                    float maxNewCost = oldCost + treasury;
                    float newCost = (newCount / 10000.0f) * costMod;
                    if (newCost < 1.0f && newCount > 0) newCost = 1.0f;
                    if (newCost > maxNewCost) {
                        newCost = maxNewCost;
                        newCount = (int)(newCost / costMod * 10000);
                        if (newCount > maxRecruit) newCount = maxRecruit;
                        if (newCount < 1) newCount = 0;
                    }
                    treasury += oldCost - newCost;
                    pr.count = newCount;
                }
                // Show cancel button with live cost
                float currentCost = (pr.count / 10000.0f) * costMod;
                if (currentCost < 1.0f && pr.count > 0) currentCost = 1.0f;
                Color cBg = Color{80, 30, 20, 220};
                Color cBd = Color{180, 80, 50, 200};
if (drawActBtn(panelX + pad, recruitBtnY, btnW * 2 + btnGap, btnH,
                TextFormat("Cancel (%s, $%.0f)", formatPop(pr.count / 100).c_str(), currentCost),
                false, cBg, cBd)) {
                    treasury += currentCost;
                    m_pendingRecruitments.erase(m_pendingRecruitments.begin() + pri);
                }
                break;
            }
        } else {
            Color rBg = canRecruit ? Color{20, 60, 30, 220} : Color{20, 20, 25, 200};
            Color rBd = canRecruit ? Color{60, 180, 80, 200} : Color{40, 40, 50, 150};
            if (drawActBtn(panelX + pad, recruitBtnY, btnW * 2 + btnGap, btnH,
                TextFormat("Recruit %s ($%.0f)", formatPop(recruitCount / 100).c_str(), recruitCost),
                !canRecruit, rBg, rBd) && canRecruit) {
                treasury -= recruitCost;
                m_pendingRecruitments.push_back({selPid, recruitCount, 1});
            }
        }

        // Disband / Cancel Disband button
        bool hasPendingDisband = false;
        for (auto& pd : m_pendingDisbandOrders) if (pd.provinceId == selPid) hasPendingDisband = true;
        bool canDisband = isOwnProv && totalSoldiers > 0 && !hasPendingDisband;
        int disbandBtnY = recruitBtnY + btnH + btnGap;
        if (hasPendingDisband) {
            Color cBg = Color{80, 60, 20, 220};
            Color cBd = Color{180, 140, 50, 200};
            if (drawActBtn(panelX + pad, disbandBtnY, btnW, btnH,
                "Cancel Disband", false, cBg, cBd)) {
                auto& vec = m_pendingDisbandOrders;
                for (auto it = vec.begin(); it != vec.end(); ) {
                    if (it->provinceId == selPid) it = vec.erase(it);
                    else ++it;
                }
            }
        } else {
            Color dBg = canDisband ? Color{80, 20, 20, 220} : Color{20, 20, 25, 200};
            Color dBd = canDisband ? Color{180, 60, 60, 200} : Color{40, 40, 50, 150};
            if (drawActBtn(panelX + pad, disbandBtnY, btnW, btnH,
                "Disband All", !canDisband, dBg, dBd) && canDisband) {
                m_pendingDisbandOrders.push_back({selPid, 0});
            }
        }
        // Cancel All Orders button (visible when this province has outgoing orders)
        {
            int moveCount = 0, artyCount = 0;
            for (auto& mo : m_pendingMoveOrders) if (mo.fromProvince == selPid) moveCount++;
            for (auto& ao : m_pendingArtilleryOrders) if (ao.fromProvince == selPid) artyCount++;
            int orderCount = moveCount + artyCount;
            if (orderCount > 0) {
                int coY = disbandBtnY;
                if (drawActBtn(panelX + pad + btnW + btnGap, coY, btnW, btnH,
                    TextFormat("Cancel Orders (%d)", orderCount), false,
                    Color{60, 30, 20, 220}, Color{180, 80, 40, 200})) {
                    for (auto it = m_pendingMoveOrders.begin(); it != m_pendingMoveOrders.end(); ) {
                        if (it->fromProvince == selPid) it = m_pendingMoveOrders.erase(it);
                        else ++it;
                    }
                    static const struct { const char* id; float cost; } ARTY_CANCEL_COST[] = {
                        {"mortar",5},{"light",10},{"heavy",20},{"napalm",30},
                        {"carpet",25},{"chemical",40},{"nuclear",80},{"biological",60},{nullptr,0}
                    };
                    double& ctreasury = m_countries.getAll()[m_playerCountryId].treasury;
                    for (auto it = m_pendingArtilleryOrders.begin(); it != m_pendingArtilleryOrders.end(); ) {
                        if (it->fromProvince == selPid) {
                            for (int ci = 0; ARTY_CANCEL_COST[ci].id; ++ci)
                                if (it->ammoType == ARTY_CANCEL_COST[ci].id) ctreasury += ARTY_CANCEL_COST[ci].cost;
                            it = m_pendingArtilleryOrders.erase(it);
                        } else ++it;
                    }
                }
            }
        }
    }

    // ─── Artillery Action Buttons (own province, army view) ─────
    if (selPid > 0 && cid == m_playerCountryId && m_activeViewTab == 5) {
        Province* pInfo = m_provinces.getProvinceById(selPid);
        bool isOwnProv = (selPid > 0 && pInfo && pInfo->countryId == m_playerCountryId);

        if (isOwnProv) {
            struct ArtyInfo { const char* id; const char* name; float troopKill; float popKill; float fortDmg; int indDmg; float fortChance; float cost; Color col; int tris; };
            static const ArtyInfo ALL_ARTY[] = {
                {"mortar","Mortar",5,0,0,0,0,5.0f,GREEN,1},
                {"light","Light Arty",10,0,0,0,0,10.0f,YELLOW,2},
                {"heavy","Heavy Arty",20,5,0,0,0,20.0f,ORANGE,2},
                {"napalm","Napalm",25,15,0,0,0,30.0f,RED,3},
                {"carpet","Carpet Bomb",15,10,0,0,50,25.0f,BLUE,3},
                {"chemical","Chemical",50,30,0,0,0,40.0f,BROWN,4},
                {"nuclear","Nuclear",75,0,2,3,0,80.0f,GRAY,4},
                {"biological","Biological",80,95,0,0,0,60.0f,PURPLE,4}
            };
            auto getNodeId = [](const char* tid) -> std::string {
                if (strcmp(tid,"mortar")==0) return "arty1";
                if (strcmp(tid,"light")==0) return "arty2";
                if (strcmp(tid,"heavy")==0) return "arty3";
                if (strcmp(tid,"napalm")==0) return "arty4a";
                if (strcmp(tid,"carpet")==0) return "arty4b";
                if (strcmp(tid,"chemical")==0) return "arty5";
                if (strcmp(tid,"nuclear")==0) return "arty6a";
                if (strcmp(tid,"biological")==0) return "arty6b";
                return "";
            };

            int btnGap = 4;
            int btnW = (panelW - pad * 2 - 4) / 2;
            int btnH = 28;
            // Place artillery 3 rows below recruit start
            int artBtnStartY = panelY + panelH - 56 - 3 * (btnH + btnGap) - 8;
            int artBtnY = artBtnStartY + 3 * (btnH + btnGap);

            bool artyActive = (m_artillerySourceProvince == selPid);
            bool showDropdown = (artyActive && m_artillerySelectedType.empty());
            bool hasSelectedType = (artyActive && !m_artillerySelectedType.empty());

            const char* btnLabel = artyActive ? "Cancel Fire Mission" : "Fire Artillery";
            Color artBg = artyActive ? Color{50, 30, 30, 220} : Color{40, 30, 50, 220};
            Color artBd = artyActive ? Color{180, 60, 60, 200} : Color{100, 80, 140, 200};

            if (drawActBtn(panelX + pad, artBtnY, btnW * 2 + btnGap, btnH, btnLabel, false, artBg, artBd)) {
                if (artyActive) {
                    // Cancel mode: refund pending orders for this province
                    auto& treasury = m_countries.getAll()[m_playerCountryId].treasury;
                    for (auto it = m_pendingArtilleryOrders.begin(); it != m_pendingArtilleryOrders.end(); ) {
                        if (it->fromProvince == selPid) {
                            for (auto& art : ALL_ARTY) {
                                if (art.id == it->ammoType) { treasury += art.cost; break; }
                            }
                            it = m_pendingArtilleryOrders.erase(it);
                        } else ++it;
                    }
                    m_artillerySourceProvince = -1;
                    m_artillerySelectedType.clear();
                    m_artilleryTargetPid = -1;
                    m_renderer->setBlockLeftPan(true);
                    m_blockLeftPanTimer = 2;
                } else {
                    m_artillerySourceProvince = selPid;
                    m_artillerySelectedType.clear();
                    m_artilleryTargetPid = -1;
                }
            }

            // Dropdown for artillery type selection
            if (showDropdown) {
                int ddX = panelX + pad;
                int ddY = artBtnY + btnH + 2;
                int ddW = btnW * 2 + btnGap;
                int ddH = 22;
                Vector2 mse = getMouse();
                int optIdx = 0;
                for (auto& art : ALL_ARTY) {
                    if (!hasResearched(getNodeId(art.id))) continue;
                    int iy = ddY + optIdx * ddH;
                    bool hover = CheckCollisionPointRec(mse, {(float)ddX, (float)iy, (float)ddW, (float)ddH});
                    bool selected = (m_artillerySelectedType == art.id);
                    Color bgc = selected ? Color{40, 60, 40, 240} : (hover ? Color{50, 50, 80, 240} : Color{30, 30, 50, 230});
                    DrawRectangle(ddX, iy, ddW, ddH, bgc);
                    DrawRectangleLines(ddX, iy, ddW, ddH, Color{60, 60, 90, 200});
                    DrawRectangle(ddX + 3, iy + 4, 14, 14, art.col);
                    DrawText(art.name, ddX + 22, iy + 3, 12, WHITE);
                    std::string effect = TextFormat("$%.0f %d%% troops", art.cost, (int)art.troopKill);
                    if (art.popKill > 0) effect += TextFormat(" %d%% pop", (int)art.popKill);
                    if (art.fortDmg > 0) effect += TextFormat(" fort-%d", (int)art.fortDmg);
                    if (art.indDmg > 0) effect += TextFormat(" ind-%d", art.indDmg);
                    DrawText(effect.c_str(), ddX + ddW - MeasureText(effect.c_str(), 10) - 4, iy + 4, 10, LIGHTGRAY);
                    if (hover && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                        m_artillerySelectedType = art.id;
                        m_artilleryTargetPid = -1;
                    }
                    optIdx++;
                }
                if (optIdx == 0) {
                    DrawText("No artillery techs researched", ddX + 4, ddY + 4, 12, Color{120, 120, 140, 200});
                }
                // Close dropdown on click outside — exit mode entirely
                Vector2 mse2 = getMouse();
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mse2, {(float)ddX, (float)ddY, (float)ddW, (float)(optIdx * ddH)})
                    && !CheckCollisionPointRec(mse2, {(float)(panelX + pad), (float)artBtnY, (float)(btnW * 2 + btnGap), (float)btnH})) {
                    m_artillerySourceProvince = -1;
                    m_artillerySelectedType.clear();
                    m_artilleryTargetPid = -1;
                    m_renderer->setBlockLeftPan(true);
                    m_blockLeftPanTimer = 2;
                }
            }

            // After type selected, show targeting hint and cancel/confirmation
            if (hasSelectedType) {
                if (m_artilleryTargetPid <= 0) {
                    DrawText("Click a neighboring province to target", panelX + pad, artBtnY - 14, 11, YELLOW);
                } else {
                    for (auto& art : ALL_ARTY) {
                        if (art.id == m_artillerySelectedType) {
                            DrawText(TextFormat("Firing %s PID %d ($%.0f)", art.name, m_artilleryTargetPid, art.cost),
                                     panelX + pad, artBtnY + btnH + 2, 11, GREEN);
                            break;
                        }
                    }
                }
            }

            // Cancel Artillery Order button (when this province has pending strikes, without targeting mode active)
            if (!artyActive) {
                int artyOrderCount = 0;
                for (auto& ao : m_pendingArtilleryOrders) if (ao.fromProvince == selPid) artyOrderCount++;
                if (artyOrderCount > 0) {
                    int caY = artBtnY + btnH + btnGap;
                    if (drawActBtn(panelX + pad, caY, btnW * 2 + btnGap, btnH,
                        TextFormat("Cancel Artillery (%d)", artyOrderCount), false,
                        Color{60, 30, 20, 220}, Color{180, 80, 40, 200})) {
                        double& ctreasury = m_countries.getAll()[m_playerCountryId].treasury;
                        for (auto it = m_pendingArtilleryOrders.begin(); it != m_pendingArtilleryOrders.end(); ) {
                            if (it->fromProvince == selPid) {
                                for (auto& art : ALL_ARTY) {
                                    if (art.id == it->ammoType) { ctreasury += art.cost; break; }
                                }
                                it = m_pendingArtilleryOrders.erase(it);
                            } else ++it;
                        }
                    }
                }
            }
        }
    }

    // ─── Navy Action Buttons (own province, navy view) ─────
    if (cid == m_playerCountryId && m_activeViewTab == 6) {
        Province* pInfo = m_provinces.getProvinceById(selPid);
        bool isOwnProv = (selPid > 0 && pInfo && pInfo->countryId == m_playerCountryId);
        auto portIt = m_provincePorts.find(selPid);
        int portLevel = (portIt != m_provincePorts.end()) ? portIt->second.level : 0;
        int maxPort = getResearchedPortLevel();
        bool portPending = false;
        for (auto& pu : m_pendingUpgrades)
            if (pu.provinceId == selPid && pu.type == "port") portPending = true;
        auto cs = computeCountryIncome(m_playerCountryId);
        double& treasury = m_countries.getAll()[m_playerCountryId].treasury;
        int btnH = 28;
        int btnGap = 4;
        int btnW = (panelW - pad * 2 - 4) / 2;
        int btnStartY = panelY + panelH - 56 - 2 * (btnH + btnGap) - 8;

        // Port already exists
        if (portLevel > 0) {
            bool atHardCap = (portLevel >= 3);
            bool atResearchCap = (portLevel >= maxPort);
            bool canUpgradePort = !atHardCap && !atResearchCap && !portPending && treasury >= 60.0f * (portLevel + 1);
            const char* pLabel;
            Color pBg, pBd;
            if (atHardCap) {
                pLabel = "Max port level (3)";
                pBg = Color{30, 30, 20, 200}; pBd = Color{80, 80, 50, 150};
            } else if (atResearchCap) {
                pLabel = "Research next port level";
                pBg = Color{30, 30, 40, 200}; pBd = Color{80, 80, 120, 150};
            } else if (portPending) {
                pLabel = TextFormat("Building Port... (3 turns)");
                pBg = Color{30, 40, 20, 220}; pBd = Color{80, 120, 60, 200};
            } else if (!canUpgradePort) {
                pLabel = TextFormat("Upgrade Port lv%d ($%.0f, 3t)", portLevel + 1, 60.0f * (portLevel + 1));
                pBg = Color{20, 20, 25, 200}; pBd = Color{40, 40, 50, 150};
            } else {
                pLabel = TextFormat("Upgrade Port lv%d ($%.0f, 3t)", portLevel + 1, 60.0f * (portLevel + 1));
                pBg = Color{20, 50, 70, 220}; pBd = Color{60, 140, 200, 200};
            }
            if (drawActBtn(panelX + pad, btnStartY, btnW * 2 + btnGap, btnH, pLabel, !canUpgradePort, pBg, pBd) && canUpgradePort) {
                treasury -= 60.0f * (portLevel + 1);
                m_pendingUpgrades.push_back({selPid, "port", portLevel + 1, 3});
            }

            // Row 2: Embark Army (level 1+) and ship building (level 2+)
            int row2Y = btnStartY + btnH + btnGap;
            // Embark Army — available only if garrison exists (free, 1 turn)
            bool alreadyEmbarking = false;
            for (auto& eb : m_pendingEmbarkations) if (eb.provinceId == selPid) alreadyEmbarking = true;
            bool hasGarrison = false;
            auto aIt2 = m_provinceArmies.find(selPid);
            if (aIt2 != m_provinceArmies.end())
                for (auto& u : aIt2->second) if (u.countryId == m_playerCountryId) hasGarrison = true;
            bool canEmbark = !alreadyEmbarking && hasGarrison;
            Color eBg = canEmbark ? Color{30, 40, 60, 220} : Color{20, 20, 25, 200};
            Color eBd = canEmbark ? Color{60, 100, 200, 200} : Color{40, 40, 50, 150};
            if (drawActBtn(panelX + pad, row2Y, btnW, btnH,
                alreadyEmbarking ? "Embarking... (1t)" : "Embark Army (free, 1t)",
                !canEmbark, eBg, eBd) && canEmbark) {
                // Embark all garrison troops from this province
                int totalTroops = 0;
                auto aIt = m_provinceArmies.find(selPid);
                if (aIt != m_provinceArmies.end())
                    for (auto& u : aIt->second) if (u.countryId == m_playerCountryId) totalTroops += u.count;
                if (totalTroops > 0)
                    m_pendingEmbarkations.push_back({selPid, totalTroops, 1});
            }

            // Build ships (destroyer at level 2+, carrier at level 3+)
            bool shipBuilding = false;
            for (auto& sb : m_pendingShipBuilds) if (sb.provinceId == selPid) shipBuilding = true;
            bool canDestroyer = portLevel >= 2 && !shipBuilding && treasury >= 15.0f;
            bool canCarrier = portLevel >= 3 && !shipBuilding && treasury >= 40.0f;
            Color dBg = canDestroyer ? Color{20, 50, 50, 220} : Color{20, 20, 25, 200};
            Color dBd = canDestroyer ? Color{60, 160, 160, 200} : Color{40, 40, 50, 150};
            if (drawActBtn(panelX + pad + btnW + btnGap, row2Y, btnW, btnH,
                portLevel < 2 ? "Requires Port Lv.2"
                : "Build Destroyer ($15, 3t)", !canDestroyer, dBg, dBd) && canDestroyer) {
                treasury -= 15.0f;
                m_pendingShipBuilds.push_back({selPid, "destroyer", 3});
            }
            int row3Y = row2Y + btnH + btnGap;
            Color cBg = canCarrier ? Color{50, 30, 60, 220} : Color{20, 20, 25, 200};
            Color cBd = canCarrier ? Color{140, 80, 180, 200} : Color{40, 40, 50, 150};
            if (drawActBtn(panelX + pad, row3Y, btnW * 2 + btnGap, btnH,
                portLevel < 3 ? "Requires Port Lv.3"
                : "Build Carrier ($40, 3t)", !canCarrier, cBg, cBd) && canCarrier) {
                treasury -= 40.0f;
                m_pendingShipBuilds.push_back({selPid, "carrier", 3});
            }
        } else if (isOwnProv) {
            bool coastal = isProvinceCoastal(selPid);
            bool canBuildPort = coastal && !portPending && treasury >= 60.0f;
            const char* noPortReason = coastal ? "" : " (not coastal)";
            Color pBg = canBuildPort ? Color{20, 50, 70, 220} : Color{20, 20, 25, 200};
            Color pBd = canBuildPort ? Color{60, 140, 200, 200} : Color{40, 40, 50, 150};
            if (drawActBtn(panelX + pad, btnStartY, btnW * 2 + btnGap, btnH,
                TextFormat("Build Port%s ($60, 3t)", noPortReason), !canBuildPort, pBg, pBd) && canBuildPort) {
                treasury -= 60.0f;
                m_pendingUpgrades.push_back({selPid, "port", 1, 3});
                Audio::get().playSfx("build_port", 0.04f);
            }
        }
    }

    // ─── Current Claims button (only in general view and relations view) ─────
    if ((m_activeViewTab == 0 || m_activeViewTab == 4) && cid > 0) {
        int btnY = panelY + panelH - 56;
        int btnX = panelX + pad;
        int btnW = panelW - pad * 2;
        int btnH = 40;
        Rectangle btnRect = {(float)btnX, (float)btnY, (float)btnW, (float)btnH};
        Vector2 mouse = getMouse();
        bool hovered = !m_paused && CheckCollisionPointRec(mouse, btnRect);

        Color btnColor = m_showClaims ? (hovered ? Color{80, 60, 20, 220} : Color{60, 40, 10, 220})
                                      : (hovered ? Color{30, 50, 80, 220} : Color{20, 30, 50, 220});
        DrawRectangleRounded(btnRect, 0.1f, 8, btnColor);
        DrawRectangleRoundedLines(btnRect, 0.1f, 8, m_showClaims ? Color{200, 160, 50, 200} : Color{80, 120, 180, 200});

        const char* label = m_showClaims ? TextFormat("Claims: %s (click to close)", country->name.c_str())
                                         : "Current Claims";
        int labelW = MeasureText(label, 16);
        DrawText(label, btnX + (btnW - labelW) / 2, btnY + (btnH - 16) / 2, 16, WHITE);

        if (hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (m_showClaims) {
                clearClaimsView();
                m_activeSidebarTab = 0;
            } else {
                m_showClaims = true;
                m_lastClaimsCountryId = cid;
                m_renderer->setShowClaims(true);
                generateClaimsTexture();
                m_activeSidebarTab = 3;
            }
        }
    }
}

void Game::drawSidebarButtons() {
    bool isSpectator = (m_playerCountryId == SPC_CID);
    int btnSize = 100;
    int btnSpacing = 8;
    int startX = m_screenW - btnSize - 12;
    int totalH = 4 * btnSize + 3 * btnSpacing;
    int startY = (m_screenH - totalH) / 2;

    struct SBtn { Texture2D tex; const char* label; int id; bool disabled; };
    SBtn btns[] = {
        {m_iconPolicies, "Politics", 1, isSpectator},
        {m_iconEconomy, "Economy", 2, false},
        {m_iconClaims, "Claims", 3, isSpectator},
        {{}, "Research", 4, false},
    };
    static constexpr int BTN_COUNT = 4;

    for (int i = 0; i < BTN_COUNT; ++i) {
        int y = startY + i * (btnSize + btnSpacing);
        Rectangle r = {(float)startX, (float)y, (float)btnSize, (float)btnSize};
        Vector2 mouse = getMouse();
        bool hovered = !m_paused && !btns[i].disabled && CheckCollisionPointRec(mouse, r);
        bool active = (m_activeSidebarTab == btns[i].id);
        // Something finished and hasn't been looked at yet (research done /
        // a policy went live). Accent the button until the panel is opened.
        bool alert = !btns[i].disabled && !active &&
                     ((btns[i].id == 4 && m_researchAlert) || (btns[i].id == 1 && m_politicsAlert));
        Color accent = hexToColor(m_config.accentColor);
        // Gentle pulse so it reads as "new" rather than just another state
        float pulse = alert ? 0.55f + 0.45f * (0.5f + 0.5f * sinf((float)GetTime() * 3.0f)) : 0.0f;

        Color bgColor;
        if (btns[i].disabled) {
            bgColor = {40, 40, 50, 120};
        } else if (active) {
            bgColor = hovered ? Color{60, 50, 20, 220} : Color{50, 40, 10, 220};
        } else if (alert) {
            bgColor = ColorAlpha(accent, (hovered ? 0.30f : 0.20f) * pulse);
        } else {
            bgColor = hovered ? Color{60, 60, 80, 200} : Color{40, 40, 55, 180};
        }
        DrawRectangleRounded(r, 0.15f, 8, bgColor);

        Color borderCol = btns[i].disabled ? Color{60, 60, 70, 100}
                        : active ? Color{200, 160, 50, 200}
                        : alert ? ColorAlpha(accent, pulse)
                        : Color{80, 80, 100, 150};
        DrawRectangleRoundedLines(r, 0.15f, 8, borderCol);

        Color iconCol = btns[i].disabled ? Color{80, 80, 90, 150}
                      : active ? accent
                      : alert ? accent
                      : hovered ? WHITE : LIGHTGRAY;

        int iconDrawSize = 48;
        int iconX = startX + (btnSize - iconDrawSize) / 2;
        int iconY2 = y + 4;
        Texture2D iconTex = (i == 3) ? m_iconResearch : btns[i].tex;
        DrawTextureEx(iconTex, {(float)iconX, (float)iconY2}, 0.0f, (float)iconDrawSize / 64.0f, iconCol);

        int labelW = MeasureText(btns[i].label, 13);
        DrawText(btns[i].label, startX + (btnSize - labelW) / 2, y + btnSize - 18, 13,
                 btns[i].disabled ? Color{80, 80, 90, 150}
                 : (active || alert) ? accent : LIGHTGRAY);

        // Small corner dot as a colour-blind-friendly second cue
        if (alert) DrawCircle((int)(r.x + r.width - 10), (int)(r.y + 10), 4.0f, ColorAlpha(accent, pulse));
    }
}

void Game::draw() {
    // Outer wrapper: provides the rendering context for drawInner().
    // drawInner() contains the actual rendering work and may emit extra
    // EndDrawing()/BeginDrawing() pairs internally (e.g. for turn processing)
    // — those break the frame when heavy work happens between draws.
    m_screenW = GetScreenWidth();
    m_screenH = GetScreenHeight();
    if (m_renderer) m_renderer->resize(m_screenW, m_screenH);
    BeginDrawing();
    ClearBackground(BLACK);
    // Guard: if not in playing state, skip game rendering to avoid accessing freed data
    // endFrame() rather than EndDrawing(): this is the map's own frame, outside
    // run()'s draw blocks, and the now-playing toast has to land on it too.
    // Only the closing pair -- the extra pairs drawInner() emits mid-frame are
    // not frame ends and must not draw an overlay.
    if (m_currentScreen != SCREEN_PLAYING) { endFrame(); return; }
    drawInner();
    endFrame();
}

void Game::drawInner() {
    // Rendering body for SCREEN_PLAYING. Called by draw() (which wraps
    // BeginDrawing/EndDrawing) and by Game::run() when a popup is active
    // (so the popup can be drawn over the game world in a single frame
    // without nested BeginDrawing/EndDrawing pairs that cause flicker).
    if (m_renderer) {
        // Set province panel rect BEFORE draw so MapRenderer blocks clicks in the same frame
        {
            int panelW = 360;
            int panelY = 48;
            int panelH = std::min(m_screenH - 80 - 16 - 48, 700);
            if (m_specDropdownProvince > 0) panelH += 160;
            bool panelVisible = m_renderer->getSelectedProvinceId() > 0 || !m_selectedShipIndices.empty();
            m_renderer->setProvincePanelRect(panelVisible
                ? Rectangle{0, (float)panelY, (float)panelW, (float)panelH}
                : Rectangle{});
        }
        // Set skip-click rect for sidebar area
        {
            int btnSize = 100;
            int btnSpacing = 8;
            int startX = m_screenW - btnSize - 20;
            int totalH = 4 * btnSize + 3 * btnSpacing;
            int startY = (m_screenH - totalH) / 2;
            m_renderer->setSkipClickRect({(float)startX, (float)startY - 8, (float)(btnSize + 16), (float)(totalH + 16)});
        }
        m_renderer->draw(m_landSea, m_provinces, m_countries);
    }
    // Wrap-aware world-to-screen: renders elements on the map copy nearest the camera
    auto worldToScreen = [&](Vector2 wp) -> Vector2 {
        int mw = m_landSea.getWidth();
        const Camera2D& cam = m_renderer->getCamera();
        while (wp.x - cam.target.x > mw * 0.5f) wp.x -= mw;
        while (wp.x - cam.target.x < -mw * 0.5f) wp.x += mw;
        return GetWorldToScreen2D(wp, cam);
    };
    // Draw industry circles (if in industry view) using world-to-screen coords
    if (m_activeViewTab == 2) {
        const Camera2D& cam = m_renderer->getCamera();
        for (auto& [pid, ind] : m_provinceIndustry) {
            if (ind.level <= 0) continue;
            auto cit = m_provinceCenters.find(pid);
            if (cit == m_provinceCenters.end()) continue;
            Vector2 sp = worldToScreen(cit->second);
            // Screen-space culling: skip off-screen
            if (sp.x < -50 || sp.x > m_screenW + 50 || sp.y < -50 || sp.y > m_screenH + 50) continue;
            Color specCol;
            if (ind.specialization == "Oil")       specCol = Color{160, 50, 200, 255};
            else if (ind.specialization == "Gold") specCol = hexToColor(m_config.accentColor);
            else if (ind.specialization == "Rubber") specCol = Color{50, 200, 50, 255};
            else if (ind.specialization == "Gemstones") specCol = Color{100, 200, 255, 255};
            else if (ind.specialization == "Metal") specCol = Color{200, 200, 200, 255};
            else specCol = Color{120, 120, 120, 255};
            float sRadius = std::max((12 + ind.level) * cam.zoom, 6.0f);
            // Shadow behind circle
            DrawCircleV({sp.x + 2, sp.y + 2}, sRadius + 2, Color{0, 0, 0, 100});
            // White border (2 concentric lines instead of 3)
            DrawCircleLines(sp.x, sp.y, sRadius, WHITE);
            DrawCircleLines(sp.x, sp.y, sRadius - 1, WHITE);
            // Specialization color ring (skip at low zoom)
            if (cam.zoom > 0.3f)
                DrawCircleLines(sp.x, sp.y, sRadius - 2, specCol);
            const char* roman[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
            const char* numeral = (ind.level >= 0 && ind.level <= 10) ? roman[ind.level] : "";
            // Skip numeral text at low zoom (too small to read)
            if (cam.zoom > 0.4f) {
                int fs = (int)((16 + (ind.level > 5 ? 4 : 0)) * cam.zoom);
                int tw = MeasureText(numeral, fs);
                DrawText(numeral, (int)(sp.x - tw / 2), (int)(sp.y - fs / 2), fs, WHITE);
            }
        }
    }
    // ─── Defence tab: fortification shields ─────
    if (m_activeViewTab == 3) {
        const Camera2D& cam = m_renderer->getCamera();
        for (auto& [pid, ind] : m_provinceIndustry) {
            if (ind.fortification <= 0) continue;
            auto cit = m_provinceCenters.find(pid);
            if (cit == m_provinceCenters.end()) continue;
            Vector2 sp = worldToScreen(cit->second);
            if (sp.x < -50 || sp.x > m_screenW + 50 || sp.y < -50 || sp.y > m_screenH + 50) continue;
            float shieldSize = std::max(18.0f * cam.zoom, 8.0f);
            // Get country color for the shield
            Province* prov = m_provinces.getProvinceById(pid);
            Color shieldCol = prov ? Color{100, 100, 200, 140} : Color{100, 100, 200, 140};
            if (prov) {
                const Country* c = m_countries.getCountry(prov->countryId);
                if (c) {
                    shieldCol.r = (uint8_t)(c->color.r * 0.6f);
                    shieldCol.g = (uint8_t)(c->color.g * 0.6f);
                    shieldCol.b = (uint8_t)(c->color.b * 0.6f);
                    shieldCol.a = 140;
                }
            }
            // Shield shape: rounded top, pointed bottom
            float cx = sp.x;
            float cy = sp.y;
            // Shadow
            DrawCircle((int)(cx + 1), (int)(cy + 1), shieldSize * 0.6f, Color{0, 0, 0, 80});
            // Shield body (circle top half + triangle bottom half)
            DrawCircleSector({cx, cy - shieldSize * 0.1f}, shieldSize * 0.5f, 180, 360, 20, shieldCol);
            DrawTriangle({cx - shieldSize * 0.5f, cy - shieldSize * 0.1f},
                         {cx + shieldSize * 0.5f, cy - shieldSize * 0.1f},
                         {cx, cy + shieldSize * 0.5f}, shieldCol);
            DrawCircleLines((int)cx, (int)(cy - shieldSize * 0.1f), shieldSize * 0.5f, WHITE);
            // Fortification level number
            const char* ftxt = (ind.fortification >= 0 && ind.fortification <= 5) ?
                               TextFormat("%d", ind.fortification) : "?";
            int fs = (int)(12 * cam.zoom);
            if (fs < 8) fs = 8;
            int tw = MeasureText(ftxt, fs);
            DrawText(ftxt, (int)(cx - tw / 2), (int)(cy - fs / 2), fs, WHITE);
        }
    }
    // ─── Army tab: soldier icons + troop counts ─────
    if (m_activeViewTab == 5) {
        const Camera2D& cam = m_renderer->getCamera();
        for (auto& [pid, units] : m_provinceArmies) {
            if (units.empty()) continue;
            auto cit = m_provinceCenters.find(pid);
            if (cit == m_provinceCenters.end()) continue;
            Vector2 sp = worldToScreen(cit->second);
            if (sp.x < -50 || sp.x > m_screenW + 50 || sp.y < -50 || sp.y > m_screenH + 50) continue;
            float sx = sp.x;
            float sy = sp.y;
            // Get country color (brightened for visibility)
            Color accent = WHITE;
            for (auto& unit : units) {
                const Country* c = m_countries.getCountry(unit.countryId);
                if (c) {
                    accent = c->color;
                    break;
                }
            }
            // Compute troop count for scaling
            long long totalCount = 0;
            for (auto& u : units) totalCount += u.count;
            long long displayCount = totalCount / 100;
            // Base icon size scales with zoom and troop count
            float soldierSize = std::max(8.0f * cam.zoom, 4.0f);
            float sizeScale = 1.0f + std::min(std::sqrt((float)displayCount / 10000.0f), 2.0f);
            soldierSize *= sizeScale;
            float maxSize = 35.0f * cam.zoom;
            if (soldierSize > maxSize) soldierSize = maxSize;
            float bodyW = soldierSize * 1.3f;
            float h = soldierSize * 1.3f;
            float topY = sy - h * 0.5f;
            float botY = sy + h * 0.5f;
            // Shadow behind body triangle
            DrawTriangle({sx - bodyW / 2 + 2, topY + 2},
                         {sx + bodyW / 2 + 2, topY + 2},
                         {sx + 2, botY + 2}, Color{0, 0, 0, 80});
            // White body triangle (shoulders → feet)
            DrawTriangle({sx - bodyW / 2, topY},
                         {sx + bodyW / 2, topY},
                         {sx, botY}, WHITE);
            // Country-coloured head (circle on top)
            float headR = bodyW * 0.28f;
            DrawCircleV({sx, topY - headR * 0.3f}, headR, accent);
            const char* countTxt = TextFormat("%s", formatPop(displayCount).c_str());
            int fs = (int)(11 * cam.zoom);
            if (fs < 8) fs = 8;
            int tw = MeasureText(countTxt, fs);
            DrawText(countTxt, (int)(sx - tw / 2), (int)(topY - fs - 2), fs, WHITE);
            // "Disbanding..." text for pending disband orders
            for (auto& pd : m_pendingDisbandOrders) {
                if (pd.provinceId != pid) continue;
                const char* dtext = "Disbanding...";
                int dfs = (int)(10 * cam.zoom);
                if (dfs < 8) dfs = 8;
                int dtw = MeasureText(dtext, dfs);
                // Shadow
                DrawText(dtext, (int)(sx - dtw / 2 + 1), (int)(botY + 3), dfs, Color{0, 0, 0, 160});
                // White text
                DrawText(dtext, (int)(sx - dtw / 2), (int)(botY + 2), dfs, WHITE);
                break;
            }
        }
    }
    // ─── Navy tab: ports (anchor icon) + ships + selection ─────
    if (m_activeViewTab == 6) {
        const Camera2D& cam = m_renderer->getCamera();
        Vector2 mouse = getMouse();

        // ── Step 1: compute ship screen positions ──
        struct ShipInfo { int idx; Vector2 pos; float size; bool visible; };
        std::vector<ShipInfo> shipInfos;
        for (int i = 0; i < (int)m_ships.size(); i++) {
            auto& ship = m_ships[i];
            int px = 0, py = 0;
            m_landSea.lonLatToPixel((float)ship.lon, (float)ship.lat, px, py);
            Vector2 sp = worldToScreen({(float)px, (float)py});
            float sz = std::max(6.0f * cam.zoom, 3.0f);
            bool vis = sp.x >= -50 && sp.x <= m_screenW + 50 && sp.y >= -50 && sp.y <= m_screenH + 50;
            shipInfos.push_back({i, sp, sz, vis});
        }

        // ── Relationship helper for navy filter (defined early, used by selection & draw) ──
        auto getNavyRel = [&](int cid) -> int {
            if (m_playerCountryId == SPC_CID || m_playerCountryId <= 0) return 4;
            if (cid == m_playerCountryId) return 1;
            if (cid <= 0) return 4;
            const Country* pc = m_countries.getCountry(m_playerCountryId);
            const Country* tc = m_countries.getCountry(cid);
            if (!pc || !tc) return 4;
            auto checkRel = [&](const std::string& a, const std::string& b) -> const CountryRelation* {
                auto it = m_relations.find(a);
                if (it == m_relations.end()) return nullptr;
                auto jt = it->second.find(b);
                return (jt != it->second.end()) ? &jt->second : nullptr;
            };
            const CountryRelation* r1 = checkRel(pc->isoA3, tc->isoA3);
            const CountryRelation* r2 = checkRel(tc->isoA3, pc->isoA3);
            bool war = (r1 && r1->war) || (r2 && r2->war);
            bool ally = (r1 && (r1->alliance || r1->guarantee)) || (r2 && (r2->alliance || r2->guarantee));
            if (war) return 3;
            if (ally) return 2;
            return 4;
        };
        // Bitmask: 1=Own, 2=Allies, 4=Enemies, 8=Neutral; 0=All
        auto filterCheck = [&](int cid) -> bool {
            if (m_navyFilter == 0) return true;
            int rel = getNavyRel(cid);
            int bit = (rel == 1) ? 1 : (rel == 2) ? 2 : (rel == 3) ? 4 : 8;
            return (m_navyFilter & bit) != 0;
        };

        // ── Step 2: handle drag box selection ──
        int boxSelectKey = m_config.keybinds[ACTION_BOX_SELECT];
        bool boxSelectHeld = IsKeyDown(boxSelectKey);
        m_renderer->setBlockLeftPan(boxSelectHeld && m_isDragSelecting);

        // Cancel drag if key was released mid-drag
        if (m_isDragSelecting && !boxSelectHeld) {
            m_isDragSelecting = false;
            m_renderer->setBlockLeftPan(false);
            m_renderer->setWasDragged(true);
        }

        if (!m_paused && m_turnState == TURN_NORMAL) {
            // Start drag: key held + left press, not on any UI panel
            if (boxSelectHeld && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                Rectangle provRect = m_renderer->getProvincePanelRect();
                bool onProvPanel = provRect.height > 0 && CheckCollisionPointRec(mouse, provRect);
                if (!onProvPanel) {
                    m_isDragSelecting = true;
                    m_dragSelectStart = mouse;
                }
            }
            // End drag: select ships in box
            if (m_isDragSelecting && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Rectangle selBox{
                    std::min(m_dragSelectStart.x, mouse.x),
                    std::min(m_dragSelectStart.y, mouse.y),
                    std::abs(mouse.x - m_dragSelectStart.x),
                    std::abs(mouse.y - m_dragSelectStart.y)
                };
                if (selBox.width > 5 || selBox.height > 5) {
                    m_selectedShipIndices.clear();
                    m_shipListFocusIndex = -1;
                    for (auto& si : shipInfos) {
                        if (!si.visible) continue;
                        auto& cship = m_ships[si.idx];
                        if (!filterCheck(cship.countryId)) continue;
                        if (CheckCollisionPointRec(si.pos, selBox)) {
                            m_selectedShipIndices.push_back(si.idx);
                        }
                    }
                    if (!m_selectedShipIndices.empty()) {
                        m_renderer->setSelectedProvince(0);
                        m_renderer->rebuildSelectionGlow();
                        buildCountryShipList(m_selectedShipIndices[0]);
                    }
                }
                m_isDragSelecting = false;
                m_renderer->setBlockLeftPan(false);
            }
            // Cancel drag
            if (m_isDragSelecting && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                m_isDragSelecting = false;
                m_renderer->setBlockLeftPan(false);
            }

            // Ship click selection (not on any UI panel)
            if (!m_isDragSelecting && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Rectangle provRect = m_renderer->getProvincePanelRect();
                bool onProvPanel = provRect.height > 0 && CheckCollisionPointRec(mouse, provRect);
                if (!onProvPanel) {
                    int clickedIdx = -1;
                    float hitR = std::max(22.0f * m_dpiScale, 14.0f);
                    for (auto& si : shipInfos) {
                        if (!si.visible) continue;
                        auto& cship = m_ships[si.idx];
                        if (!filterCheck(cship.countryId)) continue;
                        if (CheckCollisionPointCircle(mouse, si.pos, hitR)) {
                            if (clickedIdx < 0 || si.size > shipInfos[clickedIdx].size) {
                                clickedIdx = si.idx;
                            }
                        }
                    }
                    if (clickedIdx >= 0) {
                        // Ship click: clear province selection
                        if (boxSelectHeld) {
                            auto it = std::find(m_selectedShipIndices.begin(), m_selectedShipIndices.end(), clickedIdx);
                            if (it != m_selectedShipIndices.end()) {
                                m_selectedShipIndices.erase(it);
                                m_shipListFocusIndex = -1;
                            }
                            else
                                m_selectedShipIndices.push_back(clickedIdx);
                        } else {
                            m_selectedShipIndices.clear();
                            m_shipListFocusIndex = -1;
                            m_selectedShipIndices.push_back(clickedIdx);
                        }
                        m_renderer->setSelectedProvince(0);
                        m_renderer->rebuildSelectionGlow();
                        buildCountryShipList(clickedIdx);
                    } else if (!boxSelectHeld && !m_selectedShipIndices.empty()) {
                        // Clicked empty sea: deselect all ships and provinces
                        m_selectedShipIndices.clear();
                        m_shipListFocusIndex = -1;
                        m_renderer->setSelectedProvince(0);
                        m_renderer->rebuildSelectionGlow();
                    }
                }
            }
        }

        // ── Ship action click handling & hover updates ──
        // Disembark relation validation: allowed=own/allied/war, blocked=NAP/guarantee/neutral
        auto canDisembark = [&](int targetCid) -> bool {
            if (targetCid == UNC_CID) return true;
            if (targetCid == m_playerCountryId || targetCid <= 0) return targetCid == m_playerCountryId;
            if (m_playerCountryId <= 0 || m_playerCountryId == SPC_CID) return false;
            const Country* pc = m_countries.getCountry(m_playerCountryId);
            const Country* tc = m_countries.getCountry(targetCid);
            if (!pc || !tc) return false;
            auto cr = [&](const std::string& a, const std::string& b) -> const CountryRelation* {
                auto it = m_relations.find(a);
                if (it == m_relations.end()) return nullptr;
                auto jt = it->second.find(b);
                return (jt != it->second.end()) ? &jt->second : nullptr;
            };
            const CountryRelation* r1 = cr(pc->isoA3, tc->isoA3);
            const CountryRelation* r2 = cr(tc->isoA3, pc->isoA3);
            bool war = (r1 && r1->war) || (r2 && r2->war);
            bool ally = (r1 && r1->alliance) || (r2 && r2->alliance); // alliance only, NOT guarantee
            bool guar = (r1 && r1->guarantee) || (r2 && r2->guarantee);
            bool nap = (r1 && r1->nonAggression) || (r2 && r2->nonAggression);
            if (war || ally) return true;  // war/alliance OK
            if (guar || nap) return false; // guarantee/NAP blocked
            return false; // neutral blocked
        };
        // Water body connectivity check (sample points along line)
        auto waterPathClear = [&](int px1, int py1, int px2, int py2) -> bool {
            int steps = std::max(std::abs(px2-px1), std::abs(py2-py1)) / 20 + 2;
            for (int s = 0; s <= steps; s++) {
                float t = (float)s / (float)steps;
                int cx = (int)(px1 + (px2-px1)*t + 0.5f);
                int cy = (int)(py1 + (py2-py1)*t + 0.5f);
                if (cx < 0 || cx >= m_landSea.getWidth() || cy < 0 || cy >= m_landSea.getHeight())
                    return false;
                if (m_landSea.isLand(cx, cy)) return false;
            }
            return true;
        };

        // Update hover targets for ship action modes
        m_shipActionHoverShipIdx = -1;
        m_shipActionHoverProvince = -1;
        if (m_shipActionMode > 0 && m_shipActionShipIdx >= 0) {
            // Find hovered ship (for engage mode)
            float hitR = std::max(22.0f * m_dpiScale, 14.0f);
            for (auto& si : shipInfos) {
                if (!si.visible) continue;
                if (CheckCollisionPointCircle(mouse, si.pos, hitR)) {
                    if (si.idx != m_shipActionShipIdx)
                        m_shipActionHoverShipIdx = si.idx;
                    break;
                }
            }
            // Find hovered province with snapping (for modes 1/3)
            if (m_shipActionMode == 1 || m_shipActionMode == 3) {
                int px, py;
                m_renderer->screenToPixel(mouse.x, mouse.y, px, py);
                const Province* hp = m_provinces.getProvince(px, py);
                if (hp) {
                    m_shipActionHoverProvince = hp->id;
                } else {
                    // Snap to nearest province center within 30 screen pixels
                    float snapPx = 30.0f * m_dpiScale;
                    float bestD2 = snapPx * snapPx;
                    int bestId = -1;
                    for (auto& kv : m_provinceCenters) {
                        Vector2 sp = worldToScreen(kv.second);
                        float dx = mouse.x - sp.x, dy = mouse.y - sp.y;
                        float d2 = dx*dx + dy*dy;
                        if (d2 < bestD2) { bestD2 = d2; bestId = kv.first; }
                    }
                    if (bestId > 0) m_shipActionHoverProvince = bestId;
                }
            }
        }
        // Handle click in action mode (left click on valid target)
        if (m_shipActionMode > 0 && m_shipActionShipIdx >= 0 && !m_isDragSelecting && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            Rectangle provRect = m_renderer->getProvincePanelRect();
            bool onProvPanel = provRect.height > 0 && CheckCollisionPointRec(mouse, provRect);
            if (!onProvPanel && !m_paused && m_turnState == TURN_NORMAL) {
                if (m_shipActionMode == 1) {
                    // Move: water move OR boat disembark on land
                    int px, py;
                    m_renderer->screenToPixel(mouse.x, mouse.y, px, py);
                    if (px >= 0 && px < m_landSea.getWidth() && py >= 0 && py < m_landSea.getHeight()) {
                        auto& srcShip = m_ships[m_shipActionShipIdx];
                        if (m_landSea.isLand(px, py)) {
                            // Disembark (boats only, on land province)
                            if (srcShip.type == "boat" && m_shipActionHoverProvince > 0 && m_shipActionValidDest) {
                                auto& vec = m_pendingShipDisembarks;
                                for (auto it = vec.begin(); it != vec.end(); )
                                    if (it->shipIndex == m_shipActionShipIdx) it = vec.erase(it); else ++it;
                                vec.push_back({m_shipActionShipIdx, m_shipActionHoverProvince});
                                m_shipActionMode = 0; m_shipActionShipIdx = -1;
                            }
                        } else {
                            // Water move
                            float lon, lat;
                            m_landSea.pixelToLonLat(px, py, lon, lat);
                            if (m_shipActionValidDest) {
                                auto& vec = m_pendingShipMoveOrders;
                                for (auto it = vec.begin(); it != vec.end(); )
                                    if (it->shipIndex == m_shipActionShipIdx) it = vec.erase(it); else ++it;
                                vec.push_back({m_shipActionShipIdx, lon, lat});
                                m_shipActionMode = 0; m_shipActionShipIdx = -1;
                            }
                        }
                    }
                } else if (m_shipActionMode == 2 && m_shipActionHoverShipIdx >= 0 && m_shipActionValidDest) {
                    // Engage: create engage order
                    auto& vec = m_pendingShipEngageOrders;
                    for (auto it = vec.begin(); it != vec.end(); )
                        if (it->shipIndex == m_shipActionShipIdx) it = vec.erase(it); else ++it;
                    vec.push_back({m_shipActionShipIdx, m_shipActionHoverShipIdx});
                    m_shipActionMode = 0; m_shipActionShipIdx = -1;
                } else if (m_shipActionMode == 3 && m_shipActionHoverProvince > 0 && m_shipActionValidDest && !m_shipBombardAmmo.empty()) {
                    // Bombard: create bombard order with selected ammo, deduct cost
                    static const struct { const char* id; float cost; } ARTY_COST[] = {
                        {"mortar",5},{"light",10},{"heavy",20},{"napalm",30},
                        {"carpet",25},{"chemical",40},{"nuclear",80},{"biological",60},{nullptr,0}
                    };
                    auto getArtyCost = [&](const std::string& tid) -> float {
                        for (int i = 0; ARTY_COST[i].id; ++i)
                            if (tid == ARTY_COST[i].id) return ARTY_COST[i].cost;
                        return 0;
                    };
                    float cost = getArtyCost(m_shipBombardAmmo);
                    double& treasury = m_countries.getAll()[m_playerCountryId].treasury;
                    if (treasury >= cost) {
                        auto& vec = m_pendingShipBombardOrders;
                        // Refund old orders for this ship before replacing
                        for (auto it = vec.begin(); it != vec.end(); ) {
                            if (it->shipIndex == m_shipActionShipIdx) {
                                float oldCost = getArtyCost(it->ammoType);
                                treasury += oldCost;
                                it = vec.erase(it);
                            } else ++it;
                        }
                        treasury -= cost;
                        vec.push_back({m_shipActionShipIdx, m_shipActionHoverProvince, m_shipBombardAmmo});
                    }
                    m_shipActionMode = 0; m_shipActionShipIdx = -1;
                }
                // Invalid click (no mode matched or validDest false) → cancel action mode
                if (m_shipActionMode > 0) {
                    m_shipActionMode = 0; m_shipActionShipIdx = -1; m_shipBombardDropdownOpen = false;
                }
            }
        }
        // Cancel action mode on right click or ESC
        if (m_shipActionMode > 0) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) || IsKeyPressed(KEY_ESCAPE)) {
                m_shipActionMode = 0; m_shipActionShipIdx = -1; m_shipBombardDropdownOpen = false;
            }
        }

        // ── Ship keyboard shortcuts (Q=Move, W=Artillery Wheel, E=Engage) ──
        int shipWheelKey = m_config.keybinds[ACTION_SHIP_WHEEL];
        auto cancelShipOrders = [&](int shipIdx) {
            // Refund bombard orders before erasing
            static const struct { const char* id; float cost; } CANCEL_ARTY_COST[] = {
                {"mortar",5},{"light",10},{"heavy",20},{"napalm",30},
                {"carpet",25},{"chemical",40},{"nuclear",80},{"biological",60},{nullptr,0}
            };
            for (auto it = m_pendingShipBombardOrders.begin(); it != m_pendingShipBombardOrders.end(); ) {
                if (it->shipIndex == shipIdx) {
                    float refundAmt = 0;
                    for (int ci = 0; CANCEL_ARTY_COST[ci].id; ++ci)
                        if (it->ammoType == CANCEL_ARTY_COST[ci].id) { refundAmt = CANCEL_ARTY_COST[ci].cost; break; }
                    if (refundAmt > 0)
                        m_countries.getAll()[m_playerCountryId].treasury += refundAmt;
                    it = m_pendingShipBombardOrders.erase(it);
                } else ++it;
            }
            auto rm = [&](auto& vec) { for (auto it = vec.begin(); it != vec.end(); ) if (it->shipIndex == shipIdx) it = vec.erase(it); else ++it; };
            rm(m_pendingShipMoveOrders);
            rm(m_pendingShipEngageOrders);
            rm(m_pendingShipDisembarks);
        };
        if (!m_paused) {
            // Find which ship is under the cursor
            int hoverShipIdx = -1;
            float hoverShipSize = 0;
            float hitR = std::max(22.0f * m_dpiScale, 14.0f);
            for (auto& si : shipInfos) {
                if (!si.visible) continue;
                auto& cship = m_ships[si.idx];
                if (!filterCheck(cship.countryId)) continue;
                if (CheckCollisionPointCircle(mouse, si.pos, hitR)) {
                    if (hoverShipIdx < 0 || si.size > hoverShipSize) {
                        hoverShipIdx = si.idx;
                        hoverShipSize = si.size;
                    }
                }
            }
            if (hoverShipIdx >= 0 && m_ships[hoverShipIdx].countryId == m_playerCountryId) {
                // Helper to select and focus this ship
                auto focusShip = [&](int idx) {
                    m_selectedShipIndices.clear();
                    m_selectedShipIndices.push_back(idx);
                    m_shipListFocusIndex = -1;
                    m_renderer->setSelectedProvince(0);
                    m_renderer->rebuildSelectionGlow();
                    buildCountryShipList(idx);
                };
                int shipMoveKey = m_config.keybinds[ACTION_SHIP_MOVE];
                int shipEngageKey = m_config.keybinds[ACTION_SHIP_ENGAGE];
                // Q = Move mode
                if (shipMoveKey >= 32 && IsKeyPressed(shipMoveKey)) {
                    cancelShipOrders(hoverShipIdx);
                    focusShip(hoverShipIdx);
                    m_shipActionMode = 1;
                    m_shipActionShipIdx = hoverShipIdx;
                    m_shipBombardAmmo.clear();
                    m_shipBombardDropdownOpen = false;
                }
                // W = Artillery Wheel (bombard mode with type selection)
                if (shipWheelKey >= 32 && IsKeyPressed(shipWheelKey) && m_shipWheelShipIdx < 0) {
                    if (m_ships[hoverShipIdx].type == "carrier") {
                        focusShip(hoverShipIdx);
                        m_shipWheelShipIdx = hoverShipIdx;
                        m_shipWheelHover = -1;
                    }
                }
                // E = Engage mode
                if (shipEngageKey >= 32 && IsKeyPressed(shipEngageKey)) {
                    auto& s = m_ships[hoverShipIdx];
                    if (s.type == "destroyer" || s.type == "carrier" || s.type == "frigate") {
                        cancelShipOrders(hoverShipIdx);
                        focusShip(hoverShipIdx);
                        m_shipActionMode = 2;
                        m_shipActionShipIdx = hoverShipIdx;
                        m_shipBombardAmmo.clear();
                        m_shipBombardDropdownOpen = false;
                    }
                }
            }
        }

        // ── Ship wheel hover tracking (independent of hoverShipIdx) ──
        if (m_shipWheelShipIdx >= 0 && m_shipWheelShipIdx < (int)m_ships.size()) {
            if (shipWheelKey >= 32 && IsKeyDown(shipWheelKey)) {
                auto& ws = m_ships[m_shipWheelShipIdx];
                int wpx, wpy;
                m_landSea.lonLatToPixel((float)ws.lon, (float)ws.lat, wpx, wpy);
                Vector2 wc = worldToScreen({(float)wpx, (float)wpy});
                float dx = mouse.x - wc.x, dy = mouse.y - wc.y;
                float dist = sqrtf(dx*dx + dy*dy);
                if (dist > 30.0f) {
                    float angle = atan2f(dy, dx);
                    if (angle < 0) angle += 2*PI;
                    m_shipWheelHover = ((int)((angle + PI*0.625f) / (PI*0.25f))) % 8;
                } else {
                    m_shipWheelHover = -1;
                }
            } else {
                if (m_shipWheelHover >= 0) {
                    static const char* WTYPE_IDS[] = {"mortar","light","heavy","napalm","carpet","chemical","nuclear","biological"};
                    static const char* WTYPE_NODES[] = {"arty1","arty2","arty3","arty4a","arty4b","arty5","arty6a","arty6b"};
                    // Only allow researched artillery types
                    std::string selectedType = WTYPE_IDS[m_shipWheelHover];
                    std::string requiredNode = WTYPE_NODES[m_shipWheelHover];
                    if (hasResearched(requiredNode)) {
                        cancelShipOrders(m_shipWheelShipIdx);
                        m_shipActionMode = 3;
                        m_shipActionShipIdx = m_shipWheelShipIdx;
                        m_shipBombardAmmo = selectedType;
                        m_shipBombardDropdownOpen = false;
                    }
                }
                m_shipWheelShipIdx = -1;
                m_shipWheelHover = -1;
            }
        }

        // ── Step 3: Draw ports (filtered) ──
        for (auto& [pid, port] : m_provincePorts) {
            auto cit = m_provinceCenters.find(pid);
            if (cit == m_provinceCenters.end()) continue;
            Province* prov = m_provinces.getProvinceById(pid);
            if (!filterCheck(prov ? prov->countryId : 0)) continue;
            Vector2 sp = worldToScreen(cit->second);
            float aSize = std::max(14.0f * cam.zoom, 6.0f);
            float ax = sp.x;
            float ay = sp.y;
            Color portCol = Color{100, 200, 255, 220};
            if (prov) {
                const Country* c = m_countries.getCountry(prov->countryId);
                if (c) {
                    portCol.r = (uint8_t)(c->color.r * 0.5f + 128);
                    portCol.g = (uint8_t)(c->color.g * 0.5f + 128);
                    portCol.b = (uint8_t)(c->color.b * 0.5f + 128);
                }
            }
            DrawCircle((int)(ax + 1), (int)(ay + 1), aSize * 0.4f, Color{0, 0, 0, 80});
            // Thick ring: 3 concentric circles
            float ringR = aSize * 0.18f;
            for (int ri = -1; ri <= 1; ri++)
                DrawCircleLines(ax, ay - aSize * 0.4f, ringR + ri * 0.5f, portCol);
            // Thick lines: draw each line 3 times with 1px offset
            for (int o = -1; o <= 1; o++) {
                // Shank (vertical)
                DrawLine((int)(ax + o), (int)(ay - aSize * 0.25f), (int)(ax + o), (int)(ay + aSize * 0.45f), portCol);
                // Stock (horizontal)
                DrawLine((int)(ax - aSize * 0.25f), (int)(ay - aSize * 0.15f + o),
                         (int)(ax + aSize * 0.25f), (int)(ay - aSize * 0.15f + o), portCol);
                // Left arm
                DrawLine((int)(ax + o * 0.3f), (int)(ay + aSize * 0.45f),
                         (int)(ax - aSize * 0.3f + o * 0.3f), (int)(ay + aSize * 0.25f + o * 0.3f), portCol);
                // Right arm
                DrawLine((int)(ax + o * 0.3f), (int)(ay + aSize * 0.45f),
                         (int)(ax + aSize * 0.3f + o * 0.3f), (int)(ay + aSize * 0.25f + o * 0.3f), portCol);
                // Left fluke
                DrawLine((int)(ax - aSize * 0.3f + o * 0.3f), (int)(ay + aSize * 0.25f + o * 0.3f),
                         (int)(ax - aSize * 0.38f + o * 0.3f), (int)(ay + aSize * 0.3f + o * 0.3f), portCol);
                // Right fluke
                DrawLine((int)(ax + aSize * 0.3f + o * 0.3f), (int)(ay + aSize * 0.25f + o * 0.3f),
                         (int)(ax + aSize * 0.38f + o * 0.3f), (int)(ay + aSize * 0.3f + o * 0.3f), portCol);
            }
            const char* lvltxt = TextFormat("Lv.%d", port.level);
            int fs = (int)(10 * cam.zoom);
            if (fs < 7) fs = 7;
            int tw = MeasureText(lvltxt, fs);
            DrawText(lvltxt, (int)(ax - tw / 2), (int)(ay + aSize * 0.5f + 2), fs, WHITE);
        }

        // ── Step 4: Draw ships with selection highlight (filtered) ──
        for (auto& si : shipInfos) {
            auto& ship = m_ships[si.idx];
            if (!si.visible) continue;
            if (ship.countryId == UNC_CID) continue; // skip sunk ships
            if (!filterCheck(ship.countryId)) continue;
            Vector2 sp = si.pos;
            float shipSize = si.size;
            bool isSelected = std::find(m_selectedShipIndices.begin(), m_selectedShipIndices.end(), si.idx) != m_selectedShipIndices.end();

            Color shipCol = WHITE;
            const Country* c = m_countries.getCountry(ship.countryId);
            if (c) shipCol = c->color;

            if (isSelected) {
                float ringR = shipSize * 1.0f;
                DrawCircleLines((int)sp.x, (int)sp.y, ringR + 3, Color{255, 255, 255, 200});
                DrawCircleLines((int)sp.x, (int)sp.y, ringR + 5, ColorAlpha(hexToColor(m_config.accentColor), 120.0f/255.0f));
            }

            if (ship.type == "boat") {
                DrawTriangle({sp.x + 1, sp.y - shipSize + 1},
                             {sp.x - shipSize * 0.6f + 1, sp.y + shipSize * 0.4f + 1},
                             {sp.x + shipSize * 0.6f + 1, sp.y + shipSize * 0.4f + 1},
                             Color{0, 0, 0, 80});
                DrawTriangle({sp.x, sp.y - shipSize},
                             {sp.x - shipSize * 0.6f, sp.y + shipSize * 0.4f},
                             {sp.x + shipSize * 0.6f, sp.y + shipSize * 0.4f}, shipCol);
                if (ship.crew > 0) {
                    const char* crewTxt = TextFormat("%s", formatPop(ship.crew).c_str());
                    int fs = (int)(9 * cam.zoom);
                    if (fs < 6) fs = 6;
                    int tw = MeasureText(crewTxt, fs);
                    DrawText(crewTxt, (int)(sp.x - tw / 2), (int)(sp.y - shipSize - fs - 2), fs, WHITE);
                }
            } else if (ship.type == "destroyer") {
                DrawRectangle((int)(sp.x - shipSize * 0.5f + 1), (int)(sp.y - shipSize * 0.5f + 1),
                              (int)shipSize, (int)shipSize, Color{0, 0, 0, 80});
                DrawRectangle((int)(sp.x - shipSize * 0.5f), (int)(sp.y - shipSize * 0.5f),
                              (int)shipSize, (int)shipSize, shipCol);
            } else if (ship.type == "carrier" || ship.type == "frigate") {
                DrawCircle((int)(sp.x + 1), (int)(sp.y + 1), shipSize * 0.6f, Color{0, 0, 0, 80});
                DrawCircle((int)sp.x, (int)sp.y, shipSize * 0.6f, shipCol);
                DrawCircleLines((int)sp.x, (int)sp.y, shipSize * 0.6f, WHITE);
            }

            float healthPct = ship.health / 100.0f;
            float barW = shipSize * 1.5f;
            float barH = std::max(2.0f * cam.zoom, 1.0f);
            float barX = sp.x - barW / 2;
            float barY = sp.y - shipSize - (ship.type == "boat" && ship.crew > 0 ? 14 * cam.zoom : 4);
            DrawRectangle((int)barX, (int)barY, (int)barW, (int)barH, Color{60, 60, 60, 200});
            DrawRectangle((int)barX, (int)barY, (int)(barW * healthPct), (int)barH,
                          healthPct > 0.5f ? GREEN : (healthPct > 0.25f ? YELLOW : RED));
            // "Scrapping..." text overlay for pending scrap ships
            for (auto& ss : m_pendingScrapShips) {
                if (ss.shipIndex != si.idx) continue;
                const char* stxt = "Scrapping...";
                int sfs = (int)(9 * cam.zoom);
                if (sfs < 7) sfs = 7;
                int stw = MeasureText(stxt, sfs);
                float sty = barY - sfs - 2;
                DrawText(stxt, (int)(sp.x - stw / 2 + 1), (int)(sty + 1), sfs, Color{0, 0, 0, 160});
                DrawText(stxt, (int)(sp.x - stw / 2), (int)sty, sfs, WHITE);
                break;
            }
        }

        // ── Step 5: Draw drag selection rectangle ──
        if (m_isDragSelecting) {
            Rectangle selBox{
                std::min(m_dragSelectStart.x, mouse.x),
                std::min(m_dragSelectStart.y, mouse.y),
                std::abs(mouse.x - m_dragSelectStart.x),
                std::abs(mouse.y - m_dragSelectStart.y)
            };
            DrawRectangleRec(selBox, ColorAlpha(hexToColor(m_config.accentColor), 30.0f/255.0f));
            DrawRectangleLinesEx(selBox, 1.5f, ColorAlpha(hexToColor(m_config.accentColor), 180.0f/255.0f));
        }

        // ── Step 6: Ship action preview lines & pending order indicators ──
        if (m_shipActionMode > 0 && m_shipActionShipIdx >= 0 && m_shipActionShipIdx < (int)m_ships.size()) {
            auto& srcShip = m_ships[m_shipActionShipIdx];
            int sx, sy;
            m_landSea.lonLatToPixel((float)srcShip.lon, (float)srcShip.lat, sx, sy);
            Vector2 sp = worldToScreen({(float)sx, (float)sy});

            Vector2 hoverScr{0,0};
            Color lineCol = Color{100,100,150,120};
            bool validDest = false;

            // Base range per type
            float baseRange = (srcShip.type=="boat") ? 200.0f :
                              (srcShip.type=="destroyer") ? 350.0f :
                              (srcShip.type=="carrier") ? 450.0f : 300.0f;
            float speedBonus = getTotalEffect("navySpeedPct");
            float maxRange = baseRange * (1.0f + speedBonus/100.0f);

            // Draw range circle (more visible)
            float screenRange = maxRange * cam.zoom;
            DrawCircleLines((int)sp.x, (int)sp.y, screenRange, ColorAlpha(WHITE, 120.0f/255.0f));
            DrawCircleLines((int)sp.x, (int)sp.y, screenRange + 2, ColorAlpha(WHITE, 60.0f/255.0f));
            DrawCircleLines((int)sp.x, (int)sp.y, screenRange - 2, ColorAlpha(WHITE, 40.0f/255.0f));

            if (m_shipActionMode == 1) {
                int px, py;
                m_renderer->screenToPixel(mouse.x, mouse.y, px, py);
                if (px >= 0 && px < m_landSea.getWidth() && py >= 0 && py < m_landSea.getHeight()) {
                    if (m_landSea.isLand(px, py) && srcShip.type == "boat" && m_shipActionHoverProvince > 0) {
                        // Boat disembark: line to province center
                        auto cit = m_provinceCenters.find(m_shipActionHoverProvince);
                        if (cit != m_provinceCenters.end()) {
                            Province* prov = m_provinces.getProvinceById(m_shipActionHoverProvince);
                            if (prov && isProvinceCoastal(m_shipActionHoverProvince)) {
                                hoverScr = worldToScreen(cit->second);
                                float dx = (float)((int)cit->second.x - sx);
                                float dy = (float)((int)cit->second.y - sy);
                                float dist = sqrtf(dx*dx + dy*dy);
                                bool inRange = (dist <= maxRange);
                                bool relOk = canDisembark(prov->countryId);
                                validDest = inRange && relOk;
                                if (!inRange) lineCol = RED;
                                else if (!relOk) lineCol = RED;
                                else lineCol = YELLOW;
                            }
                        }
                    } else if (!m_landSea.isLand(px, py)) {
                        // Water move
                        float lon, lat;
                        m_landSea.pixelToLonLat(px, py, lon, lat);
                        float dx = (float)(px - sx), dy = (float)(py - sy);
                        float dist = sqrtf(dx*dx + dy*dy);
                        bool inRange = (dist <= maxRange);
                        bool wpOk = waterPathClear(sx, sy, px, py);
                        validDest = inRange && wpOk;
                        lineCol = validDest ? GREEN : RED;
                    }
                }
                hoverScr = mouse;
            } else if (m_shipActionMode == 2 && m_shipActionHoverShipIdx >= 0) {
                // Engage: only enemy ships (war), not own/neutral
                auto& tgt = m_ships[m_shipActionHoverShipIdx];
                int tx, ty;
                m_landSea.lonLatToPixel((float)tgt.lon, (float)tgt.lat, tx, ty);
                hoverScr = worldToScreen({(float)tx, (float)ty});
                // Check target is enemy (war) and not self
                if (tgt.countryId != srcShip.countryId && tgt.countryId > 0) {
                    const Country* srcC = m_countries.getCountry(srcShip.countryId);
                    const Country* tgtC = m_countries.getCountry(tgt.countryId);
                    bool atWar = false;
                    if (srcC && tgtC) {
                        auto cr = [&](const std::string& a, const std::string& b) -> const CountryRelation* {
                            auto it = m_relations.find(a);
                            if (it == m_relations.end()) return nullptr;
                            auto jt = it->second.find(b);
                            return (jt != it->second.end()) ? &jt->second : nullptr;
                        };
                        const CountryRelation* r1 = cr(srcC->isoA3, tgtC->isoA3);
                        atWar = r1 && r1->war;
                    }
                    validDest = atWar;
                }
                lineCol = validDest ? GREEN : RED;
            } else if (m_shipActionMode == 3 && m_shipActionHoverProvince > 0 && !m_shipBombardAmmo.empty()) {
                // Bombard: line to province, only own or enemy provinces
                auto cit = m_provinceCenters.find(m_shipActionHoverProvince);
                if (cit != m_provinceCenters.end()) {
                    float dx = (float)((int)cit->second.x - sx);
                    float dy = (float)((int)cit->second.y - sy);
                    float dist = sqrtf(dx*dx + dy*dy);
                    bool inRange = (dist <= maxRange);
                    bool canBomb = false;
                    Province* tgtProv = m_provinces.getProvinceById(m_shipActionHoverProvince);
                    if (tgtProv && tgtProv->countryId == m_playerCountryId) {
                        canBomb = true; // own province
                    } else if (tgtProv && tgtProv->countryId > 0 && tgtProv->countryId != UNC_CID) {
                        const Country* srcC = m_countries.getCountry(srcShip.countryId);
                        const Country* tgtC = m_countries.getCountry(tgtProv->countryId);
                        if (srcC && tgtC) {
                            auto cr = [&](const std::string& a, const std::string& b) -> const CountryRelation* {
                                auto it = m_relations.find(a);
                                if (it == m_relations.end()) return nullptr;
                                auto jt = it->second.find(b);
                                return (jt != it->second.end()) ? &jt->second : nullptr;
                            };
                            const CountryRelation* r1 = cr(srcC->isoA3, tgtC->isoA3);
                            if (r1 && r1->war) canBomb = true;
                        }
                    }
                    validDest = inRange && canBomb;
                    lineCol = validDest ? PURPLE : RED;
                    hoverScr = worldToScreen(cit->second);
                }
            }

            if (hoverScr.x != 0 || hoverScr.y != 0) {
                DrawLineEx(sp, hoverScr, 2.0f, ColorAlpha(lineCol, 180.0f/255.0f));
                DrawCircle((int)hoverScr.x, (int)hoverScr.y, 6, ColorAlpha(lineCol, 200.0f/255.0f));
            } else {
                DrawLineEx(sp, mouse, 1.0f, Color{100,100,150,80});
            }
            m_shipActionValidDest = validDest;
        }

        // Draw pending ship move order indicators
        for (auto& mo : m_pendingShipMoveOrders) {
            if (mo.shipIndex < 0 || mo.shipIndex >= (int)m_ships.size()) continue;
            auto& ship = m_ships[mo.shipIndex];
            int sx, sy, dx, dy;
            m_landSea.lonLatToPixel((float)ship.lon, (float)ship.lat, sx, sy);
            m_landSea.lonLatToPixel((float)mo.destLon, (float)mo.destLat, dx, dy);
            Vector2 sp2 = worldToScreen({(float)sx, (float)sy});
            Vector2 dp = worldToScreen({(float)dx, (float)dy});
            DrawLineEx(sp2, dp, 1.5f, ColorAlpha(SKYBLUE, 160.0f/255.0f));
            DrawCircle((int)dp.x, (int)dp.y, 4, ColorAlpha(SKYBLUE, 200.0f/255.0f));
        }
        // Draw pending engage order indicators
        for (auto& eo : m_pendingShipEngageOrders) {
            if (eo.shipIndex < 0 || eo.shipIndex >= (int)m_ships.size()) continue;
            if (eo.targetIndex < 0 || eo.targetIndex >= (int)m_ships.size()) continue;
            auto& s1 = m_ships[eo.shipIndex];
            auto& s2 = m_ships[eo.targetIndex];
            int x1,y1,x2,y2;
            m_landSea.lonLatToPixel((float)s1.lon,(float)s1.lat,x1,y1);
            m_landSea.lonLatToPixel((float)s2.lon,(float)s2.lat,x2,y2);
            Vector2 p1 = worldToScreen({(float)x1,(float)y1});
            Vector2 p2 = worldToScreen({(float)x2,(float)y2});
            DrawLineEx(p1, p2, 2.0f, ColorAlpha(RED, 160.0f/255.0f));
            DrawRing(p2, 8, 10, 0, 360, 16, ColorAlpha(RED, 200.0f/255.0f));
        }
        // Draw pending bombard order indicators (artillery-style arrows)
        for (auto& bo : m_pendingShipBombardOrders) {
            if (bo.shipIndex < 0 || bo.shipIndex >= (int)m_ships.size()) continue;
            auto& ship = m_ships[bo.shipIndex];
            int sx2, sy2;
            m_landSea.lonLatToPixel((float)ship.lon,(float)ship.lat,sx2,sy2);
            Vector2 sp3 = worldToScreen({(float)sx2,(float)sy2});
            auto cit = m_provinceCenters.find(bo.targetProvince);
            if (cit == m_provinceCenters.end()) continue;
            Vector2 tp = worldToScreen(cit->second);
            // Determine color and triangle count from ammo type
            struct SAD { Color col; int tris; };
            auto sad = [](const std::string& t) -> SAD {
                if (t=="mortar") return {GREEN,1}; if (t=="light") return {YELLOW,2};
                if (t=="heavy") return {ORANGE,2}; if (t=="napalm") return {RED,3};
                if (t=="carpet") return {BLUE,3}; if (t=="chemical") return {BROWN,4};
                if (t=="nuclear") return {GRAY,4}; if (t=="biological") return {PURPLE,4};
                return {PURPLE,1};
            };
            SAD def = sad(bo.ammoType);
            // Two parallel arrows
            Vector2 dir = {tp.x - sp3.x, tp.y - sp3.y};
            float len = sqrtf(dir.x*dir.x + dir.y*dir.y);
            if (len < 1.0f) continue;
            dir = {dir.x/len, dir.y/len};
            Vector2 perp = {-dir.y, dir.x};
            float aw = std::max(3.0f * cam.zoom, 2.0f);
            float gap = aw * 3.0f;
            float headLen = aw * 2.5f;
            float headW = aw * 0.9f;
            for (int side = -1; side <= 1; side += 2) {
                float off = gap * side;
                Vector2 sOff = {sp3.x + perp.x*off, sp3.y + perp.y*off};
                Vector2 dOff = {tp.x + perp.x*off, tp.y + perp.y*off};
                Vector2 hBase = {dOff.x - dir.x*headLen, dOff.y - dir.y*headLen};
                DrawLineEx(sOff, hBase, aw, def.col);
                Vector2 tipDir = {dOff.x - hBase.x, dOff.y - hBase.y};
                float tipLen = sqrtf(tipDir.x*tipDir.x + tipDir.y*tipDir.y);
                if (tipLen > 0.1f) {
                    tipDir = {tipDir.x/tipLen, tipDir.y/tipLen};
                    Vector2 tipPerp = {-tipDir.y, tipDir.x};
                    float triSp = headW * 0.6f;
                    float triBW = headW * 0.5f;
                    float triH = headLen * 0.4f;
                    for (int t = 0; t < def.tris; ++t) {
                        float tOff = (t - (def.tris-1)*0.5f) * triSp;
                        Vector2 tipPos = {dOff.x + tipPerp.x*tOff, dOff.y + tipPerp.y*tOff};
                        Vector2 tb = {tipPos.x - tipDir.x*triH, tipPos.y - tipDir.y*triH};
                        Vector2 tl = {tb.x - tipPerp.x*triBW, tb.y - tipPerp.y*triBW};
                        Vector2 tr = {tb.x + tipPerp.x*triBW, tb.y + tipPerp.y*triBW};
                        DrawTriangle(tipPos, tl, tr, def.col);
                    }
                }
            }
        }
        // Draw pending disembark order indicators
        for (auto& do_ : m_pendingShipDisembarks) {
            if (do_.shipIndex < 0 || do_.shipIndex >= (int)m_ships.size()) continue;
            auto& ship = m_ships[do_.shipIndex];
            int sx3, sy3;
            m_landSea.lonLatToPixel((float)ship.lon,(float)ship.lat,sx3,sy3);
            Vector2 sp4 = worldToScreen({(float)sx3,(float)sy3});
            auto cit = m_provinceCenters.find(do_.targetProvince);
            if (cit != m_provinceCenters.end()) {
                Vector2 tp = worldToScreen(cit->second);
                DrawLineEx(sp4, tp, 2.0f, ColorAlpha(YELLOW, 160.0f/255.0f));
                DrawRing(tp, 4, 6, 0, 360, 12, ColorAlpha(YELLOW, 200.0f/255.0f));
            }
        }
    }
    // ─── Stub buttons click handler (horizontal, lowered) ───
    if (!m_paused && m_turnState == TURN_NORMAL && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        Vector2 sm = getMouse();
        int sbBtnW = 180, sbBtnH = 36, sbGap = 8;
        int sbX = 12, sbY = m_screenH - 58;
        Rectangle ptRect = {(float)sbX, (float)sbY, (float)sbBtnW, (float)sbBtnH};
        if (CheckCollisionPointRec(sm, ptRect)) {
            if (mpIsClient()) {
                // A client never resolves a turn: it submits and waits for the
                // world the host produced. Resolving locally would compute a
                // second answer, and the two machines would diverge.
                //
                // Pressing it again takes the submission back. Changing your
                // mind before anyone else has finished is ordinary, and having
                // no way to do it makes an accidental press permanent.
                if (mpAmReady()) mpUnready(); else mpSubmitTurn();
            } else if (mpIsHost()) {
                // The host's own orders are already in this world -- but the
                // lobby has to be TOLD, or the host counts as a player who
                // never submitted and the turn waits on itself forever.
                if (mpAmReady()) mpUnready(); else mpHostReady();
            } else {
                showLoadingScreen();
                setLoadingProgress(0.0f, "Processing turn...");
                EndDrawing();
                processTurn();
                hideLoadingScreen();
                BeginDrawing();
                ClearBackground(BLACK);
            }
        }
    }
    // ─── Pending action indicators on provinces ─────
    {
        const Camera2D& cam = m_renderer->getCamera();
        float sz = std::max(6.0f * cam.zoom, 4.0f);
        // Green + for industry/fort/port upgrades (only in relevant tabs)
        for (auto& pu : m_pendingUpgrades) {
            bool show = (pu.type == "industry" && m_activeViewTab == 2) ||
                        (pu.type == "fortification" && m_activeViewTab == 3) ||
                        (pu.type == "port" && m_activeViewTab == 6);
            if (!show) continue;
            auto cit = m_provinceCenters.find(pu.provinceId);
            if (cit == m_provinceCenters.end()) continue;
            Vector2 sp = worldToScreen(cit->second);
            float cx = sp.x + sz * 2.0f;
            float cy = sp.y - sz * 1.5f;
            DrawCircle((int)(cx + 1), (int)(cy + 1), sz + 1, Color{0, 0, 0, 160});
            DrawCircle((int)cx, (int)cy, sz, Color{30, 200, 30, 255});
            DrawCircleLines((int)cx, (int)cy, sz, WHITE);
            int lw = (int)(sz * 0.55f);
            int t = std::max((int)(sz * 0.18f), 1);
            DrawRectangle((int)(cx - lw / 2), (int)(cy - t / 2), lw, t, WHITE);
            DrawRectangle((int)(cx - t / 2), (int)(cy - lw / 2), t, lw, WHITE);
        }
        // Orange gear for specialization (industry tab only)
        for (auto& ps : m_pendingSpecializations) {
            if (m_activeViewTab != 2) continue;
            auto cit = m_provinceCenters.find(ps.provinceId);
            if (cit == m_provinceCenters.end()) continue;
            Vector2 sp = worldToScreen(cit->second);
            float cx = sp.x + sz * 2.0f;
            float cy = sp.y + sz * 1.5f;
            DrawCircle((int)(cx + 1), (int)(cy + 1), sz + 1, Color{0, 0, 0, 160});
            DrawCircle((int)cx, (int)cy, sz, Color{255, 180, 0, 255});
            DrawCircleLines((int)cx, (int)cy, sz, WHITE);
            int fs = (int)(8 * cam.zoom);
            int tw = MeasureText("S", fs);
            DrawText("S", (int)(cx - tw / 2), (int)(cy - fs / 2), fs, WHITE);
        }
        // (disband X drawn on army icon in the army drawing section)
        // Green + for recruitment (army tab only) — positioned above the army icon
        for (auto& pr : m_pendingRecruitments) {
            if (m_activeViewTab != 5) continue;
            auto cit = m_provinceCenters.find(pr.provinceId);
            if (cit == m_provinceCenters.end()) continue;
            Vector2 sp = worldToScreen(cit->second);
            float cx = sp.x + sz * 2.0f;
            float cy = sp.y - sz * 2.5f;
            DrawCircle((int)(cx + 1), (int)(cy + 1), sz + 1, Color{0, 0, 0, 160});
            DrawCircle((int)cx, (int)cy, sz, Color{30, 200, 30, 255});
            DrawCircleLines((int)cx, (int)cy, sz, WHITE);
            int lw = (int)(sz * 0.55f);
            int t = std::max((int)(sz * 0.18f), 1);
            DrawRectangle((int)(cx - lw / 2), (int)(cy - t / 2), lw, t, WHITE);
            DrawRectangle((int)(cx - t / 2), (int)(cy - lw / 2), t, lw, WHITE);
        }
        // ─── Ship building indicator (navy view only) ───
        if (m_activeViewTab == 6) {
            for (auto& sb : m_pendingShipBuilds) {
                auto cit = m_provinceCenters.find(sb.provinceId);
                if (cit == m_provinceCenters.end()) continue;
                Vector2 sp = worldToScreen(cit->second);
                // Position lower-right of province center to avoid overlapping port upgrade + (top-right)
                float cx = sp.x + sz * 2.0f;
                float cy = sp.y + sz * 1.5f;
                // Gear icon for construction
                DrawCircle((int)(cx + 1), (int)(cy + 1), sz + 1, Color{0, 0, 0, 160});
                DrawCircle((int)cx, (int)cy, sz, Color{180, 180, 60, 255});
                DrawCircleLines((int)cx, (int)cy, sz, WHITE);
                int fs2 = (int)(8 * cam.zoom);
                if (fs2 < 7) fs2 = 7;
                int tw2 = MeasureText("B", fs2);
                DrawText("B", (int)(cx - tw2 / 2), (int)(cy - fs2 / 2), fs2, WHITE);
                // Draw semi-transparent ship silhouette at adjacent water pixel
                int waterPx = -1;
                auto ppIt = m_provincePixels.find(sb.provinceId);
                if (ppIt != m_provincePixels.end()) {
                    int w = m_landSea.getWidth();
                    int h = m_landSea.getHeight();
                    for (int idx : ppIt->second) {
                        int px = idx % w;
                        int py = idx / w;
                        int dx[4] = {1, -1, 0, 0};
                        int dy[4] = {0, 0, 1, -1};
                        for (int d = 0; d < 4; ++d) {
                            int nx = px + dx[d];
                            int ny = py + dy[d];
                            if (nx >= 0 && nx < w && ny >= 0 && ny < h && !m_landSea.isLand(nx, ny)) {
                                // BFS water body size check (must match spawn logic)
                                std::unordered_set<int> bVis;
                                std::vector<int> bStk = {ny * w + nx};
                                bVis.insert(ny * w + nx);
                                int bCnt = 0;
                                while (!bStk.empty() && bCnt < 200) {
                                    int bIdx = bStk.back(); bStk.pop_back(); bCnt++;
                                    int bcy = bIdx / w, bcx = bIdx % w;
                                    for (int bd = 0; bd < 4; ++bd) {
                                        int bnx = bcx + dx[bd], bny = bcy + dy[bd];
                                        if (bnx < 0) bnx = w - 1; else if (bnx >= w) bnx = 0;
                                        if (bny < 0 || bny >= h) continue;
                                        int bnIdx = bny * w + bnx;
                                        if (!bVis.count(bnIdx) && !m_landSea.isLand(bnx, bny)) {
                                            bVis.insert(bnIdx); bStk.push_back(bnIdx);
                                        }
                                    }
                                }
                                if (bCnt >= 200) { waterPx = ny * w + nx; break; }
                            }
                        }
                        if (waterPx >= 0) break;
                    }
                }
                if (waterPx >= 0) {
                    int wpx = waterPx % m_landSea.getWidth();
                    int wpy = waterPx / m_landSea.getWidth();
                    Vector2 silScr = worldToScreen({(float)wpx, (float)wpy});
                    float ss = std::max(8.0f * cam.zoom, 4.0f);
                    Color silCol = Color{180, 180, 60, 80};
                    Color silBd = Color{180, 180, 60, 40};
                    if (sb.type == "destroyer") {
                        DrawRectangle((int)(silScr.x - ss * 0.5f + 1), (int)(silScr.y - ss * 0.5f + 1),
                                      (int)ss, (int)ss, Color{0,0,0,40});
                        DrawRectangle((int)(silScr.x - ss * 0.5f), (int)(silScr.y - ss * 0.5f),
                                      (int)ss, (int)ss, silCol);
                    } else {
                        DrawCircle((int)(silScr.x + 1), (int)(silScr.y + 1), ss * 0.6f, Color{0,0,0,40});
                        DrawCircle((int)silScr.x, (int)silScr.y, ss * 0.6f, silCol);
                        DrawCircleLines((int)silScr.x, (int)silScr.y, ss * 0.6f, silBd);
                    }
                    // "Building..." text above silhouette
                    const char* btxt = "Building...";
                    int bfs = (int)(8 * cam.zoom);
                    if (bfs < 6) bfs = 6;
                    int btw = MeasureText(btxt, bfs);
                    float bty = silScr.y - ss - bfs - 2;
                    DrawText(btxt, (int)(silScr.x - btw / 2), (int)(bty), bfs, ColorAlpha(YELLOW, 160.0f/255.0f));
                }
            }
        }
        // ─── Embark boat silhouette (navy view only) ───
        if (m_activeViewTab == 6) {
            for (auto& eb : m_pendingEmbarkations) {
                // Find water pixel adjacent to this province (BFS water body check, same as spawn logic)
                int w = m_landSea.getWidth(), h = m_landSea.getHeight();
                auto ppIt = m_provincePixels.find(eb.provinceId);
                if (ppIt == m_provincePixels.end()) continue;
                int boatPx = -1;
                for (int idx : ppIt->second) {
                    int px = idx % w, py = idx / w;
                    int dx[4] = {1,-1,0,0}, dy[4] = {0,0,1,-1};
                    for (int d = 0; d < 4; ++d) {
                        int nx = px + dx[d], ny = py + dy[d];
                        if (nx >= 0 && nx < w && ny >= 0 && ny < h && !m_landSea.isLand(nx, ny)) {
                            std::unordered_set<int> bVis;
                            std::vector<int> bStk = {ny * w + nx};
                            bVis.insert(ny * w + nx);
                            int bCnt = 0;
                            while (!bStk.empty() && bCnt < 200) {
                                int bIdx = bStk.back(); bStk.pop_back(); bCnt++;
                                int bcy = bIdx / w, bcx = bIdx % w;
                                for (int bd = 0; bd < 4; ++bd) {
                                    int bnx = bcx + dx[bd], bny = bcy + dy[bd];
                                    if (bnx < 0) bnx = w - 1; else if (bnx >= w) bnx = 0;
                                    if (bny < 0 || bny >= h) continue;
                                    int bnIdx = bny * w + bnx;
                                    if (!bVis.count(bnIdx) && !m_landSea.isLand(bnx, bny)) {
                                        bVis.insert(bnIdx); bStk.push_back(bnIdx);
                                    }
                                }
                            }
                            if (bCnt >= 200) { boatPx = ny * w + nx; break; }
                        }
                    }
                    if (boatPx >= 0) break;
                }
                if (boatPx < 0) continue;
                Vector2 silScr = worldToScreen({(float)(boatPx % w), (float)(boatPx / w)});
                float ss = std::max(8.0f * m_renderer->getCamera().zoom, 4.0f);
                // Triangle silhouette (boat shape)
                DrawTriangle({silScr.x + 1, silScr.y - ss * 0.6f + 1},
                             {silScr.x - ss * 0.4f + 1, silScr.y + ss * 0.4f + 1},
                             {silScr.x + ss * 0.4f + 1, silScr.y + ss * 0.4f + 1},
                             Color{0,0,0,40});
                DrawTriangle({silScr.x, silScr.y - ss * 0.6f},
                             {silScr.x - ss * 0.4f, silScr.y + ss * 0.4f},
                             {silScr.x + ss * 0.4f, silScr.y + ss * 0.4f},
                             Color{180, 180, 60, 80});
                // "Embarking..." text above
                const char* eTxt = "Embarking...";
                int efs = (int)(8 * m_renderer->getCamera().zoom);
                if (efs < 6) efs = 6;
                int etw = MeasureText(eTxt, efs);
                DrawText(eTxt, (int)(silScr.x - etw / 2), (int)(silScr.y - ss - efs - 2), efs, ColorAlpha(YELLOW, 160.0f/255.0f));
            }
        }
        // ─── Ship Artillery Wheel (navy tab) ───
        if (m_shipWheelShipIdx >= 0 && m_shipWheelShipIdx < (int)m_ships.size()) {
            const Camera2D& cam = m_renderer->getCamera();
            auto& ws = m_ships[m_shipWheelShipIdx];
            int wpx, wpy;
            m_landSea.lonLatToPixel((float)ws.lon, (float)ws.lat, wpx, wpy);
            Vector2 wc = worldToScreen({(float)wpx, (float)wpy});
            float wrad = 85.0f;
            int wsectors = 8;
            static const char* WTYPE_IDS[] = {"mortar","light","heavy","napalm","carpet","chemical","nuclear","biological"};
            static const char* WTYPE_NODES[] = {"arty1","arty2","arty3","arty4a","arty4b","arty5","arty6a","arty6b"};
            static const Color WTYPE_COL[] = {GREEN,YELLOW,ORANGE,RED,BLUE,BROWN,GRAY,PURPLE};
            static const char* WTYPE_NAME[] = {"Mortar","Light","Heavy","Napalm","Carpet","Chem","Nuclear","Bio"};
            for (int wi = 0; wi < wsectors; ++wi) {
                float startDeg = 247.5f + wi * 45.0f;
                float endDeg = startDeg + 45.0f;
                bool whover = (wi == m_shipWheelHover);
                bool wresearched = hasResearched(WTYPE_NODES[wi]);
                Color wfill = wresearched ? WTYPE_COL[wi] : Color{50,50,50,180};
                if (whover) wfill = wresearched ? Color{255,255,255,200} : Color{100,100,100,200};
                DrawCircleSector(wc, wrad, startDeg, endDeg, 24, ColorAlpha(wfill, whover ? 200 : 130));
                DrawCircleSectorLines(wc, wrad, startDeg, endDeg, 24, whover ? WHITE : Color{255,255,255,50});
                float midDeg = startDeg + 22.5f;
                float midRad = midDeg * DEG2RAD;
                float lr = wrad * 0.55f;
                Vector2 lp = {wc.x + cosf(midRad)*lr, wc.y + sinf(midRad)*lr};
                int lfs = 9;
                Color lc = wresearched ? (whover ? BLACK : WHITE) : Color{80,80,80,200};
                DrawText(WTYPE_NAME[wi], (int)(lp.x - MeasureText(WTYPE_NAME[wi], lfs)*0.5f), (int)(lp.y - lfs*0.5f), lfs, lc);
            }
            DrawText("Artillery", (int)(wc.x - MeasureText("Artillery", 9)*0.5f), (int)(wc.y - 5), 9, ColorAlpha(WHITE, 180));
        }
        // ─── Army move order arrows + sliders ───
        if (m_activeViewTab == 5) {
            const Camera2D& cam = m_renderer->getCamera();
            float arrowSz = std::max(5.0f * cam.zoom, 3.0f);
            int arrowFont = (int)(10 * cam.zoom);
            if (arrowFont < 7) arrowFont = 7;
            // Draw existing move orders + handle slider dragging
            for (auto& mo : m_pendingMoveOrders) {
                auto srcIt = m_provinceCenters.find(mo.fromProvince);
                auto dstIt = m_provinceCenters.find(mo.toProvince);
                if (srcIt == m_provinceCenters.end() || dstIt == m_provinceCenters.end()) continue;
                Vector2 sv = srcIt->second, dv = dstIt->second;
                Vector2 src = worldToScreen(sv);
                Vector2 dst = worldToScreen(dv);
                Vector2 dir = {dst.x - src.x, dst.y - src.y};
                float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
                if (len < 1.0f) continue;
                dir = {dir.x / len, dir.y / len};
                Vector2 perp = {-dir.y, dir.x};
                // Country color for destination end of gradient (less dark: 65% instead of 40%)
                Color ctryColor = {180, 180, 255, 255};
                Province* pp = m_provinces.getProvinceById(mo.fromProvince);
                if (pp) { const Country* cc = m_countries.getCountry(pp->countryId); if (cc) ctryColor = cc->color; }
                uint8_t dr = (uint8_t)(ctryColor.r * 0.65f);
                uint8_t dg = (uint8_t)(ctryColor.g * 0.65f);
                uint8_t db = (uint8_t)(ctryColor.b * 0.65f);
                // Arrowhead dimensions
                float headLen = arrowSz * 3.0f;
                float headW = arrowSz * 1.2f;
                Vector2 hBase = {dst.x - dir.x * headLen, dst.y - dir.y * headLen};
                Vector2 bodyEnd = hBase;
                float bodyW = arrowSz * 1.2f;
                float outlineW = bodyW + 3.0f;
                int segments = 16;
                float bodyFrac = 1.0f - headLen / len;
                if (bodyFrac < 0.0f) bodyFrac = 0.0f;
                // ── Outline ──
                if (bodyFrac > 0.01f) {
                    DrawLineEx(src, bodyEnd, outlineW, Color{0, 0, 0, 160});
                }
                {
                    Vector2 olL = {hBase.x - perp.x * (headW + 1.5f), hBase.y - perp.y * (headW + 1.5f)};
                    Vector2 olR = {hBase.x + perp.x * (headW + 1.5f), hBase.y + perp.y * (headW + 1.5f)};
                    DrawTriangle(dst, olL, olR, Color{0, 0, 0, 160});
                }
                // ── Gradient fill ──
                if (bodyFrac > 0.01f) {
                    for (int i = 0; i < segments; ++i) {
                        float t0 = bodyFrac * i / segments;
                        float t1 = bodyFrac * (i + 1) / segments;
                        Vector2 p0 = {src.x + (bodyEnd.x - src.x) * t0 / bodyFrac,
                                      src.y + (bodyEnd.y - src.y) * t0 / bodyFrac};
                        Vector2 p1 = {src.x + (bodyEnd.x - src.x) * t1 / bodyFrac,
                                      src.y + (bodyEnd.y - src.y) * t1 / bodyFrac};
                        float ct = (t0 + t1) * 0.5f;
                        uint8_t r = (uint8_t)(255 + ct * ((int)dr - 255));
                        uint8_t g = (uint8_t)(255 + ct * ((int)dg - 255));
                        uint8_t b = (uint8_t)(255 + ct * ((int)db - 255));
                        uint8_t a = (uint8_t)(102 + ct * (255 - 102));
                        DrawLineEx(p0, p1, bodyW, {r, g, b, a});
                    }
                }
                // ── Arrowhead fill ──
                {
                    Vector2 hL = {hBase.x - perp.x * headW, hBase.y - perp.y * headW};
                    Vector2 hR = {hBase.x + perp.x * headW, hBase.y + perp.y * headW};
                    Color headCol = {dr, dg, db, 255};
                    DrawTriangle(dst, hL, hR, headCol);
                }
                // ── Slider handle ON the arrow body (0% = src, 100% = head base) ──
                float handleT = mo.pct / 100.0f;
                Vector2 handlePos = {src.x + (bodyEnd.x - src.x) * handleT,
                                     src.y + (bodyEnd.y - src.y) * handleT};
                bool isThisSlider = (m_armyMovePctSliderFrom == mo.fromProvince && m_armyMovePctSliderTo == mo.toProvince);
                float hR2 = std::max(6.0f * cam.zoom, 4.0f);
                Color hCol = isThisSlider ? Color{255, 255, 255, 255} : Color{200, 230, 255, 230};
                // Compute max percentage for this order (total ≤ 100% from this source)
                int sumOthers = 0;
                for (auto& om : m_pendingMoveOrders)
                    if (om.fromProvince == mo.fromProvince && (&om != &mo))
                        sumOthers += om.pct;
                int maxPct = 100 - sumOthers;
                if (maxPct < 1) maxPct = 1;
                // ── Limit line: perpendicular marker at the max position (only if constrained) ──
                if (maxPct < 100) {
                    float limitT = maxPct / 100.0f;
                    Vector2 limitPos = {src.x + (bodyEnd.x - src.x) * limitT,
                                        src.y + (bodyEnd.y - src.y) * limitT};
                    float limitLineLen = bodyW * 1.5f;
                    Vector2 limP0 = {limitPos.x + perp.x * limitLineLen, limitPos.y + perp.y * limitLineLen};
                    Vector2 limP1 = {limitPos.x - perp.x * limitLineLen, limitPos.y - perp.y * limitLineLen};
                    DrawLineEx(limP0, limP1, std::max(1.2f, bodyW * 0.3f), Color{200, 60, 60, 160});
                }
                DrawCircleV(handlePos, hR2 + 1.0f, Color{0, 0, 0, 160});
                DrawCircleV(handlePos, hR2, hCol);
                if (isThisSlider)
                    DrawCircleLines((int)handlePos.x, (int)handlePos.y, hR2, Color{255, 255, 255, 200});
                // Percentage text — only on the active slider (reduce clutter on inactive arrows)
                if (isThisSlider) {
                    const char* pctTxt = TextFormat("%d%%", mo.pct);
                    int tw = MeasureText(pctTxt, arrowFont);
                    Vector2 txtPos = {handlePos.x + perp.x * (hR2 + 3) - tw * 0.5f,
                                      handlePos.y + perp.y * (hR2 + 3)};
                    if (perp.y < 0) txtPos.y -= arrowFont;
                    // Small dark background for readability above the arrow layer
                    int bgPad = 2;
                    DrawRectangle((int)txtPos.x - bgPad, (int)txtPos.y - bgPad,
                                  tw + bgPad * 2, arrowFont + bgPad * 2,
                                  Color{0, 0, 0, 160});
                    DrawText(pctTxt, (int)(txtPos.x + 1), (int)(txtPos.y + 1), arrowFont, Color{0, 0, 0, 200});
                    DrawText(pctTxt, (int)txtPos.x, (int)txtPos.y, arrowFont, WHITE);
                }
                // ── Slider hit area (only on the handle circle) ──
                if (!m_paused && !m_armyMovePctSliderFrom && !m_armyMovePctSliderTo) {
                    Vector2 mse = getMouse();
                    float dist = sqrtf((mse.x - handlePos.x) * (mse.x - handlePos.x) + (mse.y - handlePos.y) * (mse.y - handlePos.y));
                    if (dist < hR2 + 6.0f && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        m_armyMovePctSliderFrom = mo.fromProvince;
                        m_armyMovePctSliderTo = mo.toProvince;
                    }
                }
                // Dragging update for this order's slider
                if (isThisSlider && !m_paused && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    Vector2 mse = getMouse();
                    Vector2 slAxis = {bodyEnd.x - src.x, bodyEnd.y - src.y};
                    float slAxisLen = sqrtf(slAxis.x * slAxis.x + slAxis.y * slAxis.y);
                    if (slAxisLen > 0.001f) {
                        Vector2 mRel = {mse.x - src.x, mse.y - src.y};
                        float t = (mRel.x * slAxis.x + mRel.y * slAxis.y) / (slAxisLen * slAxisLen);
                        t = std::clamp(t, 0.0f, 1.0f);
                        int newPct = (int)(t * 100);
                        if (newPct < 1) newPct = 1;
                        if (newPct > maxPct) newPct = maxPct;
                        mo.pct = newPct;
                    }
                }
            }
            // Clear slider state when mouse released
            if (m_armyMovePctSliderFrom && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                m_armyMovePctSliderFrom = 0;
                m_armyMovePctSliderTo = 0;
            }
            // Draw drag line + hint during active right-click drag
            if (m_armyMoveDragActive && m_armyMoveDragSource > 0) {
                auto srcIt = m_provinceCenters.find(m_armyMoveDragSource);
                if (srcIt != m_provinceCenters.end()) {
                    Vector2 src = worldToScreen(srcIt->second);
                    Vector2 mse = getMouse();
                    if (m_armyMoveDragHoverPid > 0) {
                        auto dstIt2 = m_provinceCenters.find(m_armyMoveDragHoverPid);
                        if (dstIt2 != m_provinceCenters.end()) {
                            Vector2 hov = worldToScreen(dstIt2->second);
                            float hr = std::max(10.0f * cam.zoom, 6.0f);
                            Color lineCol = m_armyMoveDragValidDest ? Color{100, 255, 100, 200} : Color{255, 80, 80, 200};
                            DrawCircleLines((int)hov.x, (int)hov.y, hr, lineCol);
                            DrawLineEx(src, hov, arrowSz * 0.6f, lineCol);
                        }
                    } else {
                        DrawLineEx(src, mse, arrowSz * 0.5f, Color{100, 200, 255, 120});
                        const char* hint = "Drag to neighbor province";
                        int hf = 12;
                        int htw = MeasureText(hint, hf);
                        DrawText(hint, (int)(mse.x - htw * 0.5f), (int)(mse.y - 20), hf, ColorAlpha(WHITE, 0.6f));
                    }
                }
            }

            // ─── Artillery Arrow Drawing ───
            // Color/triangle config per type
            struct ArtyDrawDef { Color col; int tris; };
            auto getArtyDrawDef = [](const std::string& type) -> ArtyDrawDef {
                if (type == "mortar")    return {GREEN, 1};
                if (type == "light")     return {YELLOW, 2};
                if (type == "heavy")     return {ORANGE, 2};
                if (type == "napalm")    return {RED, 3};
                if (type == "carpet")    return {BLUE, 3};
                if (type == "chemical")  return {BROWN, 4};
                if (type == "nuclear")   return {GRAY, 4};
                if (type == "biological")return {PURPLE, 4};
                return {WHITE, 1};
            };
            for (auto& ao : m_pendingArtilleryOrders) {
                auto srcIt = m_provinceCenters.find(ao.fromProvince);
                auto dstIt = m_provinceCenters.find(ao.targetProvince);
                if (srcIt == m_provinceCenters.end() || dstIt == m_provinceCenters.end()) continue;
                Vector2 sv = srcIt->second, dv = dstIt->second;
                Vector2 src = worldToScreen(sv);
                Vector2 dst = worldToScreen(dv);
                Vector2 dir = {dst.x - src.x, dst.y - src.y};
                float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
                if (len < 1.0f) continue;
                dir = {dir.x / len, dir.y / len};
                Vector2 perp = {-dir.y, dir.x};
                ArtyDrawDef def = getArtyDrawDef(ao.ammoType);
                float arrowW = arrowSz * 0.8f;
                float gap = arrowSz * 3.0f;
                float headLen = arrowSz * 2.5f;
                float headW = arrowSz * 0.9f;

                // Draw two parallel arrows (left and right of center)
                for (int side = -1; side <= 1; side += 2) {
                    float offset = gap * side;
                    Vector2 sOff = {src.x + perp.x * offset, src.y + perp.y * offset};
                    Vector2 dOff = {dst.x + perp.x * offset, dst.y + perp.y * offset};
                    Vector2 hBase = {dOff.x - dir.x * headLen, dOff.y - dir.y * headLen};

                    // Arrow body
                    DrawLineEx(sOff, hBase, arrowW, def.col);

                    // Triangle tips at the arrowhead
                    Vector2 tipDir = {dOff.x - hBase.x, dOff.y - hBase.y};
                    float tipLen = sqrtf(tipDir.x * tipDir.x + tipDir.y * tipDir.y);
                    if (tipLen > 0.1f) {
                        tipDir = {tipDir.x / tipLen, tipDir.y / tipLen};
                        Vector2 tipPerp = {-tipDir.y, tipDir.x};
                        float triSpacing = headW * 0.6f;
                        float triBaseW = headW * 0.5f;
                        float triH = headLen * 0.4f;

                        for (int t = 0; t < def.tris; ++t) {
                            float tOff = (t - (def.tris - 1) * 0.5f) * triSpacing;
                            Vector2 tipPos = {dOff.x + tipPerp.x * tOff, dOff.y + tipPerp.y * tOff};
                            Vector2 tBase = {tipPos.x - tipDir.x * triH, tipPos.y - tipDir.y * triH};
                            Vector2 tL = {tBase.x - tipPerp.x * triBaseW, tBase.y - tipPerp.y * triBaseW};
                            Vector2 tR = {tBase.x + tipPerp.x * triBaseW, tBase.y + tipPerp.y * triBaseW};
                            DrawTriangle(tipPos, tL, tR, def.col);
                        }
                    }
                }
            }

            // ─── Artillery Wheel Menu (radial type selection) ───
            if (m_artilleryWheelProvince > 0) {
                auto wit = m_provinceCenters.find(m_artilleryWheelProvince);
                if (wit != m_provinceCenters.end()) {
                    Vector2 wc = worldToScreen(wit->second);
                    float wrad = 85.0f;
                    int wsectors = 8;
                    static const char* WTYPE_IDS[] = {"mortar","light","heavy","napalm","carpet","chemical","nuclear","biological"};
                    static const char* WTYPE_NODES[] = {"arty1","arty2","arty3","arty4a","arty4b","arty5","arty6a","arty6b"};
                    static const Color WTYPE_COL[] = {GREEN,YELLOW,ORANGE,RED,BLUE,BROWN,GRAY,PURPLE};
                    static const char* WTYPE_NAME[] = {"Mortar","Light","Heavy","Napalm","Carpet","Chem","Nuclear","Bio"};
                    for (int wi = 0; wi < wsectors; ++wi) {
                        float startDeg = 247.5f + wi * 45.0f;
                        float endDeg = startDeg + 45.0f;
                        bool whover = (wi == m_artilleryWheelHover);
                        bool wresearched = hasResearched(WTYPE_NODES[wi]);
                        Color wfill = wresearched ? WTYPE_COL[wi] : Color{50,50,50,180};
                        if (whover) wfill = wresearched ? Color{255,255,255,200} : Color{100,100,100,200};
                        DrawCircleSector(wc, wrad, startDeg, endDeg, 24, ColorAlpha(wfill, whover ? 200 : 130));
                        DrawCircleSectorLines(wc, wrad, startDeg, endDeg, 24, whover ? WHITE : Color{255,255,255,50});
                        float midDeg = startDeg + 22.5f;
                        float midRad = midDeg * DEG2RAD;
                        float lr = wrad * 0.55f;
                        Vector2 lp = {wc.x + cosf(midRad)*lr, wc.y + sinf(midRad)*lr};
                        int lfs = wresearched ? 9 : 9;
                        Color lc = wresearched ? (whover ? BLACK : WHITE) : Color{80,80,80,200};
                        DrawText(WTYPE_NAME[wi], (int)(lp.x - MeasureText(WTYPE_NAME[wi], lfs)*0.5f), (int)(lp.y - lfs*0.5f), lfs, lc);
                    }
                    // Center label
                    DrawText("Artillery", (int)(wc.x - MeasureText("Artillery", 9)*0.5f), (int)(wc.y - 5), 9, ColorAlpha(WHITE, 180));
                }
            }

            // ─── Artillery targeting visual indicator ───
            // Line follows cursor continuously throughout targeting mode
            if (m_artillerySourceProvince > 0 && !m_artillerySelectedType.empty()) {
                Vector2 mse = getMouse();
                int px, py;
                m_renderer->screenToPixel(mse.x, mse.y, px, py);
                const Province* hp = m_provinces.getProvince(px, py);
                int hoverPid = hp ? hp->id : -1;
                auto srcIt = m_provinceCenters.find(m_artillerySourceProvince);
                if (srcIt != m_provinceCenters.end()) {
                    Vector2 src = worldToScreen(srcIt->second);
                    Vector2 cursor = mse;
                    Color lineCol = Color{100, 200, 255, 80};

                    // Check if hovering a valid neighboring province
                    bool isNb = false;
                    bool canShoot = false;
                    if (hoverPid > 0 && hoverPid != m_artillerySourceProvince) {
                        auto nIt = m_provinceNeighbors.find(m_artillerySourceProvince);
                        isNb = (nIt != m_provinceNeighbors.end()) &&
                            std::find(nIt->second.begin(), nIt->second.end(), hoverPid) != nIt->second.end();
                        if (isNb) {
                            Province* dp = m_provinces.getProvinceById(hoverPid);
                            if (dp && dp->countryId == m_playerCountryId) {
                                canShoot = true;
                            } else if (dp && dp->countryId > 0 && dp->countryId != UNC_CID) {
                                Province* sp = m_provinces.getProvinceById(m_artillerySourceProvince);
                                if (sp) {
                                    const Country* sc = m_countries.getCountry(sp->countryId);
                                    const Country* dc = m_countries.getCountry(dp->countryId);
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
                            auto dstHov = m_provinceCenters.find(hoverPid);
                            if (dstHov != m_provinceCenters.end()) {
                                cursor = worldToScreen(dstHov->second);
                                float hr = std::max(10.0f * cam.zoom, 6.0f);
                                DrawCircleLines((int)cursor.x, (int)cursor.y, hr,
                                    canShoot ? Color{255, 200, 50, 220} : Color{255, 60, 60, 180});
                            }
                            lineCol = canShoot ? Color{255, 200, 50, 200} : Color{255, 60, 60, 160};
                        } else {
                            lineCol = Color{100, 100, 160, 80};
                        }
                    }
                    // Always draw line from source to cursor position
                    DrawLineEx(src, cursor, arrowSz * 0.6f, lineCol);
                    // Draw hint text near the cursor
                    const char* hint = "Click neighboring province to fire artillery";
                    int hf = 12;
                    int htw = MeasureText(hint, hf);
                    DrawText(hint, (int)(mse.x - htw * 0.5f), (int)(mse.y - 24), hf, ColorAlpha(YELLOW, 0.8f));
                }
            }
        }
    }
    // Always show date and legends, regardless of spectator mode
    if (!m_mapDate.empty() || m_playerCountryId == SPC_CID) {
        const int fontDate = 20;
        const int padX = 12, padY = 6;
        const int mainBarW = std::min(880, m_screenW - 32);
        const int mainBarX = m_screenW - mainBarW - 16;
        const int mainBarY = m_screenH - 80 - 16;
        const char* dateText = m_mapDate.empty() ? "Spectator Mode" : m_mapDate.c_str();
        int dateW = MeasureText(dateText, fontDate);
        int dateH = fontDate + padY * 2;
        int datePanelW = dateW + padX * 2 + 4;
        int dateX = mainBarX + mainBarW - datePanelW;
        int dateY = mainBarY - dateH - 4;
        DrawRectangleGradientH(dateX, dateY, datePanelW, dateH,
                               {0, 0, 0, 0}, {0, 0, 0, 180});
        DrawText(dateText, dateX + padX, dateY + padY, fontDate,
                 ColorAlpha(WHITE, 0.75f));

        // Relations legend — above the toolbar, left of the date
        if (m_activeViewTab == 4) {
            struct LegendItem { const char* label; Color color; };
            LegendItem items[] = {
                {"Self",    {0, 100, 255, 255}},
                {"War",     {255, 50, 50, 255}},
                {"Alliance",{50, 200, 50, 255}},
                {"Guarantee",{255, 255, 50, 255}},
                {"Non-aggr",{255, 165, 0, 255}},
                {"Neutral", {80, 80, 80, 255}},
            };
            int legFont = 14;
            int swatchSize = 10;
            int legPad = 6;
            int totalW = 0;
            int itemWs[6];
            for (int i = 0; i < 6; ++i) {
                int tw = swatchSize + 4 + MeasureText(items[i].label, legFont);
                itemWs[i] = tw;
                totalW += tw + legPad;
            }
            totalW -= legPad;
            int legX = dateX - totalW - 8;
            if (legX < mainBarX) legX = mainBarX; // don't overflow left
            int legY = dateY + (dateH - legFont) / 2;
            DrawRectangleGradientH(legX, dateY, totalW + 8, dateH,
                                   {0, 0, 0, 0}, {0, 0, 0, 180});
            int cx = legX + 4;
            for (int i = 0; i < 6; ++i) {
                DrawRectangle(cx, legY + (legFont - swatchSize) / 2, swatchSize, swatchSize, items[i].color);
                DrawText(items[i].label, cx + swatchSize + 4, legY, legFont, ColorAlpha(WHITE, 0.75f));
                cx += itemWs[i] + legPad;
            }
        }
    }
    // ─── Resource selection buttons ──────────────────
    if (m_activeViewTab == 7 && (!m_mapDate.empty() || m_playerCountryId == SPC_CID)) {
        const int mainBarW = std::min(880, m_screenW - 32);
        const int mainBarX = m_screenW - mainBarW - 16;
        const int mainBarY = m_screenH - 80 - 16;
        const int fontDate = 20;
        const int padX = 12, padY = 6;
        int dateH = fontDate + padY * 2;
        int dateY = mainBarY - dateH - 4;
        int btnCount = 5;
        int btnW = 80;
        int btnH = dateH;
        int totalW = btnCount * btnW + (btnCount - 1) * 4;
        int btnStartX = mainBarX + 8;
        int btnY = dateY;
        DrawRectangleGradientH(btnStartX, btnY, totalW, btnH,
                               {0, 0, 0, 0}, {0, 0, 0, 180});
        for (int i = 0; i < btnCount; ++i) {
            int bx = btnStartX + i * (btnW + 4);
            Vector2 mouse = getMouse();
            Rectangle btnRect = {(float)bx, (float)btnY, (float)btnW, (float)btnH};
            bool hovered = !m_paused && CheckCollisionPointRec(mouse, btnRect);
            Color bg = (i == m_activeResourceIdx) ? Color{60, 120, 60, 200} : (hovered ? Color{60, 60, 70, 200} : Color{40, 40, 50, 200});
            DrawRectangle(bx, btnY, btnW, btnH, bg);
            DrawRectangleLines(bx, btnY, btnW, btnH, {100, 100, 120, 150});
            int tw = MeasureText(RESOURCE_NAMES[i], 16);
            DrawText(RESOURCE_NAMES[i], bx + (btnW - tw) / 2, btnY + (btnH - 16) / 2, 16,
                     i == m_activeResourceIdx ? WHITE : (hovered ? WHITE : LIGHTGRAY));
        }
    }
    // ─── Navy filter buttons (multiselect, same toolbar area) ──
    if (m_activeViewTab == 6 && (!m_mapDate.empty() || m_playerCountryId == SPC_CID)) {
        const int mainBarW2 = std::min(880, m_screenW - 32);
        const int mainBarX2 = m_screenW - mainBarW2 - 16;
        const int mainBarY2 = m_screenH - 80 - 16;
        const int fontD = 20;
        const int pX = 12, pY = 6;
        int dH = fontD + pY * 2;
        int dY = mainBarY2 - dH - 4;
        const char* fLabels[] = {"All", "Own", "Allies", "Enemies", "Neutral"};
        int fCnt = 5;
        int btnW2 = 80;
        int btnH2 = dH;
        int totalW2 = fCnt * btnW2 + (fCnt - 1) * 4;
        int btnStartX2 = mainBarX2 + 8;
        DrawRectangleGradientH(btnStartX2, dY, totalW2, btnH2,
                               {0, 0, 0, 0}, {0, 0, 0, 180});
        for (int i = 0; i < fCnt; ++i) {
            int bx = btnStartX2 + i * (btnW2 + 4);
            Vector2 mouse = getMouse();
            Rectangle btnRect = {(float)bx, (float)dY, (float)btnW2, (float)btnH2};
            bool hovered = !m_paused && CheckCollisionPointRec(mouse, btnRect);
            bool active = (i == 0) ? (m_navyFilter == 0) : (m_navyFilter & (1 << (i - 1)));
            Color bg = active ? Color{60, 120, 60, 200} : (hovered ? Color{60, 60, 70, 200} : Color{40, 40, 50, 200});
            DrawRectangle(bx, dY, btnW2, btnH2, bg);
            DrawRectangleLines(bx, dY, btnW2, btnH2, {100, 100, 120, 150});
            int tw2 = MeasureText(fLabels[i], 16);
            DrawText(fLabels[i], bx + (btnW2 - tw2) / 2, dY + (btnH2 - 16) / 2, 16,
                     active ? WHITE : (hovered ? WHITE : LIGHTGRAY));
            if (!m_paused && hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                if (i == 0) {
                    m_navyFilter = 0;
                } else {
                    m_navyFilter ^= (1 << (i - 1));
                }
            }
        }
    }
    // Player country indicator — always visible, top-left corner
    if (m_playerCountryId > 0) {
        int ph = 64;
        DrawRectangle(0, 0, 360, ph, {10, 10, 15, 220});
        DrawText("PLAYING AS", 12, 4, 10, {160, 160, 170, 255});
        if (m_playerCountryId == SPC_CID) {
            DrawText("SPECTATOR", 12, 18, 20, {200, 200, 210, 255});
        } else {
            const Country* pc = m_countries.getCountry(m_playerCountryId);
            if (pc) {
                auto pfit = m_countryFlags.find(m_playerCountryId);
                if (pfit != m_countryFlags.end() && pfit->second.id > 0) {
                    DrawTexturePro(pfit->second,
                        {0, 0, (float)pfit->second.width, (float)pfit->second.height},
                        {360 - 12 - 80, 6, 80, 32},
                        {0, 0}, 0.0f, WHITE);
                }
                DrawText(pc->name.c_str(), 12, 18, 20, WHITE);
                float treasury = pc->treasury;
                Color balCol = (treasury >= 0) ? Color{180, 230, 180, 255} : Color{230, 130, 130, 255};
                DrawText(TextFormat("$%.2f", treasury), 12, 44, 14, balCol);
            }
        }
    }

    drawBottomPanel();
    drawSidebarButtons();
    if (m_renderer->getSelectedProvinceId() > 0 || !m_selectedShipIndices.empty()) drawCountryPanel();
    // ─── Bottom-left stub buttons (only when not processing turn) ───
    if ((!m_mapDate.empty() || m_playerCountryId == SPC_CID) && m_turnState == TURN_NORMAL) {
        int sbBtnW = 180, sbBtnH = 36, sbGap = 8;
        int sbX = 12, sbY = m_screenH - 58;
        Vector2 sm = getMouse();
        // Process Turn stub
        Rectangle ptRect = {(float)sbX, (float)sbY, (float)sbBtnW, (float)sbBtnH};
        bool ptHov = !m_paused && CheckCollisionPointRec(sm, ptRect);
        DrawRectangleRounded(ptRect, 0.1f, 6, ptHov ? Color{40,100,60,220} : Color{30,70,45,220});
        DrawRectangleRoundedLines(ptRect, 0.1f, 6, Color{60,150,90,180});
        // What the button actually does depends on who you are: a client says
        // "ready", the host says "my orders are in" -- and neither resolves
        // anything by itself.
        const char* ptLabel = (mpIsClient() || mpIsHost())
                            ? (mpAmReady() ? "Not ready" : "Ready")
                            : "Process Turn";
        int ptw = MeasureText(ptLabel, 16);
        DrawText(ptLabel, sbX+(sbBtnW-ptw)/2, sbY+10, 16, WHITE);

        // ─── who the turn is waiting on, and for how long ───
        //
        // In a network game the single most useful thing on screen is why the
        // turn has not moved. Without it, a stalled campaign is indis-
        // tinguishable from a broken one.
        if (mpIsHost() || mpIsClient()) {
            drawMpTurnPanel(sbX, sbY - 12);
        }
    }
    // ─── (TURN_PROCESSED removed — not used) ───
    if (m_inPolitics || m_inEconomy || m_inClaims || m_inResearch) {
        if (m_activeSidebarTab == 1) {
            drawPoliciesTab();
        } else if (m_activeSidebarTab == 3) {
            drawClaimsTab();
        } else if (m_activeSidebarTab == 4) {
            drawResearchTab();
        } else {
            drawEconomy();
        }
    }
    if (m_paused) drawPauseMenu();
    // Turn history sits above the pause menu it was opened from
    if (m_inHistory) drawHistoryScreen();

    // Debug overlay. drawDebugOverlay self-gates on debugMode -- it also draws
    // the resource panel, which must be reachable without debug mode on.
    drawDebugOverlay();
    if (m_config.debugMode) drawScriptErrors();
    if (m_config.showConsole) drawConsoleWindow();

    // Draw notifications
    drawNotifications();

    // Draw ceasefire screen on top if active
    if (m_inCeasefireScreen) drawCeasefireScreen();

    // Mod panels last, so a mod can never paint over a game dialog.
    drawModPanels();
}
