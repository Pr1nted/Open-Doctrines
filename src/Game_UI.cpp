#include "Game.h"
#include "TextInput.h"
#include "Audio.h"
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
    // Silent until data/audio/sfx/notify.* exists; see data/audio/README.md.
    Audio::get().playSfx("notify");
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
    entry.id = ++m_popupNextId;
    m_popupQueue.push_back(entry);
    // No sound here. Queueing is not appearing -- see PopupEntry::id and
    // announceFrontPopup(), which plays it when this one reaches the screen.
}

void Game::announceFrontPopup() {
    if (m_popupQueue.empty()) {
        m_popupAnnouncedId = 0;    // nothing showing; the next one is new again
        return;
    }
    const unsigned long long front = m_popupQueue.front().id;
    if (front == m_popupAnnouncedId) return;
    m_popupAnnouncedId = front;
    Audio::get().playSfx("panel_open");
}

namespace {
// Both drawPopup() and updatePopup() need identical geometry -- they were
// already duplicating these numbers, and making the height depend on the terms
// panel would have let them drift, putting the buttons somewhere the click
// handler was not looking.
// Every offset in an expanded ceasefire popup, from one place. The summary
// above the terms is as many lines as the offer has clauses, so a fixed
// terms-panel top put the divider through the last line of a busy offer -- and
// the map has to be hit-tested by updatePopup() at exactly the rectangle
// drawPopup() drew it into.
struct CeasefireLayout {
    int termsY;   // top of the itemised rows, relative to popY
    int mapY;     // top of the map slot, relative to popY
    int mapH;
    int height;   // popup height with the terms panel open
};
CeasefireLayout ceasefireLayout(const PopupEntry& p) {
    int lines = 1;
    for (char c : p.message) if (c == '\n') lines++;

    CeasefireLayout L{};
    L.termsY = 60 + lines * 22 + 14;                  // title, message, divider
    const int rowsH = (24 + 3 * 20) + 10 + (24 + 3 * 20);   // two sections
    L.mapY   = L.termsY + rowsH + 12;
    L.mapH   = 150;
    // map, gap, "Hide full terms", the Accept/Reject row, bottom margin
    L.height = L.mapY + L.mapH + 8 + 46 + 40 + 20;
    return L;
}

void popupGeometry(const PopupEntry& p, bool showTerms, int& w, int& h) {
    if (p.type == PopupType::CEASEFIRE_REQUEST) {
        w = 560;
        h = showTerms ? ceasefireLayout(p).height
                      : std::max(360, ceasefireLayout(p).termsY + 46 + 60);
    } else {
        w = 480;
        h = 260;
    }
}

Rectangle popupTermsMapRect(const PopupEntry& p, int popX, int popY, int popW) {
    CeasefireLayout L = ceasefireLayout(p);
    return {(float)(popX + 30), (float)(popY + L.mapY), (float)(popW - 60), (float)L.mapH};
}
}  // namespace

void Game::drawPopup() {
    if (m_popupQueue.empty()) return;

    auto& popup = m_popupQueue.front();

    // Ceasefire popups need more vertical space to show the terms summary
    int popW, popH;
    popupGeometry(popup, m_popupShowTerms, popW, popH);
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

    // Ceasefire offers: an itemised breakdown behind a toggle. The summary above
    // only gives counts ("cedes 2 provinces"), which is not enough to judge an
    // offer -- you need to know *which* provinces before you sign.
    if (popup.type == PopupType::CEASEFIRE_REQUEST) {
        Rectangle termsBtn = {(float)(popX + (popW - 200) / 2), (float)(btnY - 46),
                              200.0f, 32.0f};
        bool th = CheckCollisionPointRec(getMouse(), termsBtn);
        DrawRectangleRounded(termsBtn, 0.2f, 6,
                             th ? Color{70, 80, 110, 255} : Color{45, 50, 70, 230});
        DrawRectangleRoundedLines(termsBtn, 0.2f, 6, Color{90, 95, 125, 220});
        const char* tl = m_popupShowTerms ? "Hide full terms" : "View full terms";
        int tlw = MeasureText(tl, 16);
        DrawText(tl, (int)(termsBtn.x + (200 - tlw) / 2), (int)termsBtn.y + 8, 16, WHITE);

        if (m_popupShowTerms) {
            const Country* oc = m_countries.getCountry(popup.countryId);
            std::string them = oc ? oc->name : popup.sourceIso;

            auto provName = [&](int pid) -> std::string {
                Province* pp = m_provinces.getProvinceById(pid);
                return (pp && !pp->name.empty()) ? pp->name
                                                 : ("province " + std::to_string(pid));
            };
            // Long lists are truncated rather than overflowing the panel; the
            // count is already in the summary above.
            auto listOf = [&](const std::vector<int>& v) -> std::string {
                if (v.empty()) return "-";
                std::string out;
                size_t n = v.size() < 4 ? v.size() : 4;
                for (size_t i = 0; i < n; i++) { if (i) out += ", "; out += provName(v[i]); }
                if (v.size() > n) out += TextFormat(" +%d more", (int)(v.size() - n));
                return out;
            };
            auto gold = [](int g) -> std::string {
                return g > 0 ? TextFormat("%d gold", g) : "-";
            };

            Color accent = hexToColor(m_config.accent());
            int ty = popY + ceasefireLayout(popup).termsY;
            int tx = popX + 30;
            DrawLine(tx, ty - 12, popX + popW - 30, ty - 12, Color{70, 70, 95, 255});

            auto section = [&](const char* heading, Color c) {
                DrawText(heading, tx, ty, 16, c);
                ty += 24;
            };
            auto row = [&](const char* label, const std::string& value) {
                DrawText(label, tx + 12, ty, 14, Color{150, 150, 165, 255});
                drawHybridText(tx + 150, ty, 14, value.c_str(), WHITE);
                ty += 20;
            };

            section(TextFormat("%s gives you", them.c_str()), Color{120, 210, 140, 255});
            row("Money",           gold(popup.terms.ourMoney));
            row("Provinces",       listOf(popup.terms.ourProvs));
            row("Claims dropped",  listOf(popup.terms.ourDropClaims));
            ty += 10;
            section(TextFormat("%s asks from you", them.c_str()), Color{225, 130, 120, 255});
            row("Money",           gold(popup.terms.theirMoney));
            row("Provinces",       listOf(popup.terms.theirProvs));
            row("Claims to drop",  listOf(popup.terms.theirDropClaims));

            if (popup.terms.ourProvs.empty() && popup.terms.theirProvs.empty() &&
                popup.terms.ourMoney == 0 && popup.terms.theirMoney == 0 &&
                popup.terms.ourDropClaims.empty() && popup.terms.theirDropClaims.empty()) {
                ty += 8;
                DrawText("A white peace: nothing changes hands.", tx, ty, 14, accent);
            } else {
                // A list of province NUMBERS is not something anyone can judge
                // an offer from. "Provinces: 854, 1283, 1290" tells the player
                // nothing about whether they are being asked for a border strip
                // or for half the country. The composer screen has always had a
                // map; the offer you RECEIVE had only the numbers.
                Rectangle mr = popupTermsMapRect(popup, popX, popY, popW);
                drawCeasefireTermsMap(popup.terms, popup.id, (int)mr.x, (int)mr.y,
                                      (int)mr.width, (int)mr.height);
                ty = (int)(mr.y + mr.height) + 8;
            }
        }
    }

    if (popup.type == PopupType::DIPLOMATIC_REQUEST || popup.type == PopupType::CEASEFIRE_REQUEST) {
        // WHAT YOU WILL TELL THEM, if you say no. Cycled by clicking; defaults
        // to saying nothing, so a player who does not care about this never has
        // to touch it. The same list the AI picks from, unfiltered -- you may
        // claim to be at war while at peace, and they can look at the map.
        {
            Rectangle whyBtn = {(float)(popX + 20), (float)(btnY - 34),
                                (float)(popW - 40), 26.0f};
            const bool whyHover = CheckCollisionPointRec(getMouse(), whyBtn);
            DrawRectangleRounded(whyBtn, 0.15f, 6,
                                 whyHover ? Color{60, 60, 84, 255} : Color{38, 38, 54, 220});
            DrawRectangleRoundedLines(whyBtn, 0.15f, 6, Color{80, 80, 105, 200});
            const std::string lbl =
                std::string("If you refuse: ") + refusalTextOwn(m_popupRefusalReason);
            DrawText(lbl.c_str(), (int)whyBtn.x + 10, (int)whyBtn.y + 6, 14,
                     m_popupRefusalReason == REFUSE_NONE ? Color{150, 150, 165, 255}
                                                         : Color{225, 205, 150, 255});
        }
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
    // Before the early-out: this is also how "the queue emptied" is noticed, so
    // the next popup after a quiet spell still announces itself.
    announceFrontPopup();
    if (m_popupQueue.empty()) return;

    auto& popup = m_popupQueue.front();
    Vector2 mouse = getMouse();

    int popW, popH;
    popupGeometry(popup, m_popupShowTerms, popW, popH);
    int popX = (m_screenW - popW) / 2;
    int popY = (m_screenH - popH) / 2;
    int btnW = 140, btnH = 40;
    int btnY = popY + popH - btnH - 20;

    // Before the click gate below: zoom is a wheel event and panning happens
    // while the button is held, neither of which is a release.
    bool wasDragging = m_popupTermsMapDragging;
    if (popup.type == PopupType::CEASEFIRE_REQUEST && m_popupShowTerms &&
        m_popupTermsMapKey == popup.id)
        updateCeasefireTermsMap(popupTermsMapRect(popup, popX, popY, popW));

    if (!IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return;
    // The release that ends a pan is not a click on anything.
    if (wasDragging) return;

    if (popup.type == PopupType::CEASEFIRE_REQUEST) {
        Rectangle termsBtn = {(float)(popX + (popW - 200) / 2), (float)(btnY - 46),
                              200.0f, 32.0f};
        if (CheckCollisionPointRec(mouse, termsBtn)) {
            m_popupShowTerms = !m_popupShowTerms;
            return;   // the popup just changed size; do not also hit a button
        }
    }

    // The reason cycler, on both request popups. Clicking it changes what you
    // would say and does nothing else -- it never accepts or refuses anything.
    if (popup.type == PopupType::DIPLOMATIC_REQUEST ||
        popup.type == PopupType::CEASEFIRE_REQUEST) {
        Rectangle whyBtn = {(float)(popX + 20), (float)(btnY - 34),
                            (float)(popW - 40), 26.0f};
        if (CheckCollisionPointRec(mouse, whyBtn)) {
            m_popupRefusalReason = (m_popupRefusalReason + 1) % REFUSE_COUNT;
            return;
        }
    }

    if (popup.type == PopupType::DIPLOMATIC_REQUEST) {
        Rectangle approveBtn = {(float)(popX + popW / 2 - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        Rectangle rejectBtn = {(float)(popX + popW / 2 + 10), (float)btnY, (float)btnW, (float)btnH};

        if (CheckCollisionPointRec(mouse, approveBtn)) {
            // Apply the request stored in the popup itself. The pending action
            // was erased when this popup was created, so the old
            // processDiplomaticRequests() call here applied nothing — Approve
            // was a silent no-op for alliances/guarantees/NAPs.
            auto& fwd = m_relations[popup.sourceIso][popup.targetIso];
            auto& rev = m_relations[popup.targetIso][popup.sourceIso];
            // NOT WITH SOMEONE YOU ARE AT WAR WITH.
            //
            // A popup outlives the turn it was raised on: the offer can be
            // sitting unanswered when the country that made it declares war,
            // and clicking Approve then wrote alliance=true onto a pair with
            // war=true. Nothing downstream expects both -- allies are skipped
            // as targets, enemies are attacked, and the same pair was each in
            // turn depending on which check ran. The war is the newer fact and
            // the one the player watched happen, so the treaty is what gives.
            if ((popup.action == "request_alliance" || popup.action == "request_guarantee" ||
                 popup.action == "request_nap") && (fwd.war || rev.war)) {
                addNotification(diploDisplayName(popup.sourceIso) +
                                " declared war before you answered — the offer is void",
                                Color{235, 130, 90, 255}, 8.0f);
                Audio::get().playSfx("deal_rejected");
                m_popupQueue.erase(m_popupQueue.begin());
                m_popupShowTerms = false;
                m_popupRefusalReason = REFUSE_NONE;
                return;
            }
            if (popup.action == "request_alliance")      { fwd.alliance = true;      rev.alliance = true; }
            else if (popup.action == "request_guarantee"){ fwd.guarantee = true;     rev.guarantee = true; }
            else if (popup.action == "request_nap")      { fwd.nonAggression = true; rev.nonAggression = true; }
            else if (popup.action == "call_to_arms") {
                // Honouring the alliance: war with the aggressor, and unrest at
                // home for the years that follow. Not chained -- see the note
                // in processDiplomaticRequests.
                declareWar(popup.targetIso, popup.subjectIso, false);
                addWarWeariness(m_playerCountryId, CALL_TO_ARMS_UNREST);
                addNotification("You honour your alliance with " + popup.sourceIso +
                                " — unrest rises at home", ORANGE, 8.0f);
            }
            // The treaty exists as of this click -- the lines above just set it.
            Audio::get().playSfx("treaty_signed");
            printf("[DIPLO] Player approved %s from %s\n", popup.action.c_str(), popup.sourceIso.c_str());
            m_popupQueue.erase(m_popupQueue.begin());
            m_popupShowTerms = false;
            m_popupRefusalReason = REFUSE_NONE;
        } else if (CheckCollisionPointRec(mouse, rejectBtn)) {
            // Nothing to erase from the pending queue: the request was already
            // removed when the popup was created. The old code erased
            // begin(), which could silently drop an unrelated queued action.
            if (popup.action == "call_to_arms") {
                // Refusing is free at home and costs the alliance instead.
                m_relations[popup.sourceIso][popup.targetIso].alliance = false;
                m_relations[popup.targetIso][popup.sourceIso].alliance = false;
                addNotification("You refused " + popup.sourceIso +
                                "'s call to arms — the alliance is over", ORANGE, 8.0f);
            }
            tellRefusal(popup.sourceIso, m_popupRefusalReason);
            Audio::get().playSfx("deal_rejected");
            printf("[DIPLO] Player rejected %s from %s\n", popup.action.c_str(), popup.sourceIso.c_str());
            m_popupQueue.erase(m_popupQueue.begin());
            m_popupShowTerms = false;
            m_popupRefusalReason = REFUSE_NONE;
        }
    } else if (popup.type == PopupType::CEASEFIRE_REQUEST) {
        Rectangle approveBtn = {(float)(popX + popW / 2 - btnW - 10), (float)btnY, (float)btnW, (float)btnH};
        Rectangle rejectBtn = {(float)(popX + popW / 2 + 10), (float)btnY, (float)btnW, (float)btnH};

        if (CheckCollisionPointRec(mouse, approveBtn)) {
            Audio::get().playSfx("deal_accepted");
            // Player accepted the ceasefire: schedule an apply_ceasefire action
            // that fires on the NEXT turn (1 → 0 → apply). Stash the terms so
            // processDiplomaticRequests can pick them up when apply_ceasefire runs.
            PendingDiplomaticAction da;
            da.sourceIso = popup.sourceIso;
            da.targetIso = popup.targetIso;
            da.action = "apply_ceasefire";
            da.turnsRemaining = 1;
            // Exempt from the one-channel rule by name: this is the deferred
            // half of an answer already given, not a new overture.
            queueDiplomaticAction(da);
            std::string key = popup.sourceIso + "|" + popup.targetIso;
            m_acceptedCeasefireTerms[key] = popup.terms;
            // The original request_ceasefire action was already erased from
            // m_pendingDiplomaticActions when the popup was pushed. We also
            // remove the original terms from m_pendingCeasefireTerms so the
            // sender can issue new offers if needed later.
            auto tit = m_pendingCeasefireTerms.find(key);
            if (tit != m_pendingCeasefireTerms.end()) m_pendingCeasefireTerms.erase(tit);
            m_popupQueue.erase(m_popupQueue.begin());
            m_popupShowTerms = false;
            m_popupRefusalReason = REFUSE_NONE;
            printf("[CEASEFIRE] Player accepted offer from %s — apply scheduled for next turn\n", popup.sourceIso.c_str());
        } else if (CheckCollisionPointRec(mouse, rejectBtn)) {
            Audio::get().playSfx("deal_rejected");
            // Erase the waiting terms and notify sender side (no automatic
            // re-permit for now — sender will see the war persists).
            std::string key = popup.sourceIso + "|" + popup.targetIso;
            auto tit = m_pendingCeasefireTerms.find(key);
            if (tit != m_pendingCeasefireTerms.end()) m_pendingCeasefireTerms.erase(tit);
            m_popupQueue.erase(m_popupQueue.begin());
            m_popupShowTerms = false;
            m_popupRefusalReason = REFUSE_NONE;
            printf("[CEASEFIRE] Player rejected offer from %s\n", popup.sourceIso.c_str());
        }
    } else {
        // OK button (REBELLION, WAR_DECLARED)
        Rectangle okBtn = {(float)(popX + (popW - btnW) / 2), (float)btnY, (float)btnW, (float)btnH};
        if (CheckCollisionPointRec(mouse, okBtn)) {
            m_popupQueue.erase(m_popupQueue.begin());
            m_popupShowTerms = false;
            m_popupRefusalReason = REFUSE_NONE;
        }
    }
}

// See src/TextInput.h. A free function rather than a Game method because the
// map editor's fields need it too and MapEditor is not a Game.
bool odTextEditKeys(std::string& field, size_t maxLen, const char* forbidden,
                    bool digitsOnly) {
    bool changed = false;
    auto allowed = [&](char ch) {
        if (ch < 32 || ch >= 127) return false;
        if (digitsOnly && (ch < '0' || ch > '9')) return false;
        for (const char* f = forbidden; f && *f; ++f)
            if (*f == ch) return false;
        return true;
    };

    // Backspace and Delete both take the last character: these fields have no
    // caret to be in front of or behind. Repeat is included so holding the key
    // clears a field, which the map editor's fields already did and the game's
    // did not.
    bool del = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE) ||
               IsKeyPressed(KEY_DELETE)    || IsKeyPressedRepeat(KEY_DELETE);
    if (del && !field.empty()) {
        field.pop_back();
        Audio::get().playSfx("key_type", 0.12f);
        changed = true;
    }

    // Ctrl+V, and Cmd+V too -- on macOS Ctrl+V is not what anyone presses.
    bool paste = (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                  IsKeyDown(KEY_LEFT_SUPER)   || IsKeyDown(KEY_RIGHT_SUPER)) &&
                 IsKeyPressed(KEY_V);
    if (paste) {
        const char* clip = GetClipboardText();
        if (clip && *clip) {
            size_t before = field.size();
            for (const char* q = clip; *q && field.size() < maxLen; ++q) {
                // A pasted newline or tab ends the value rather than joining
                // it: people copy a line out of a terminal, trailing break and
                // all, and a control character in a hostname or a filename is
                // never what they meant.
                if (*q == '\n' || *q == '\r' || *q == '\t') break;
                if (allowed(*q)) field += *q;
            }
            if (field.size() != before) {
                Audio::get().playSfx("key_type", 0.12f);
                changed = true;
            }
        }
    }
    return changed;
}

// Where the offered land actually is.
//
// Crops the political map to everything the terms touch and shades each
// province in the colour of whoever ends up holding it: green for land coming
// to us, red for land going away, amber for a claim being dropped. The shapes
// are the point -- a dot at a province centre says where the land is but not
// whether the offer is a border strip or half a country.
//
// The shading is rastered once per offer into a crop-sized buffer and cached
// under the popup's id, so the frames after the first cost one extra
// DrawTexturePro. A crop-sized buffer is what makes that affordable: the
// composer screen's equivalent overlay spans the whole world, which at this
// map's resolution is tens of millions of pixels.
void Game::drawCeasefireTermsMap(const CeasefireTerms& terms, unsigned long long cacheKey,
                                 int x, int y, int w, int h) {
    if (w < 40 || h < 30) return;
    int texW = m_provinces.getWidth(), texH = m_provinces.getHeight();
    if (texW <= 0 || texH <= 0 || m_politicalTex.id == 0) return;

    // `lean` is the diagonal the stripes run along. Colour alone is not enough:
    // a green province on a green country reads as unmarked, and the countries
    // are not going to change colour to suit the offer.
    struct Mark { int pid; Color col; int lean; };
    std::vector<Mark> marks;
    for (int pid : terms.ourProvs)        marks.push_back({pid, Color{120, 210, 140, 255},  1});
    for (int pid : terms.theirProvs)      marks.push_back({pid, Color{225, 130, 120, 255}, -1});
    for (int pid : terms.ourDropClaims)   marks.push_back({pid, Color{225, 190, 110, 255},  1});
    for (int pid : terms.theirDropClaims) marks.push_back({pid, Color{225, 190, 110, 255}, -1});
    if (marks.empty()) return;

    // One raster pass per offer. Everything below the rebuild reads the cache.
    if (m_popupTermsMapKey != cacheKey || m_popupTermsMapTex.id == 0) {
        m_popupTermsMapKey = cacheKey;
        m_popupTermsMapEmpty = true;

        // Bounding box of the land in question, padded so it has context around it.
        int minPx = texW, maxPx = 0, minPy = texH, maxPy = 0;
        for (auto& m : marks) {
            auto it = m_provincePixels.find(m.pid);
            if (it == m_provincePixels.end() || it->second.empty()) continue;
            for (int idx : it->second) {
                int px = idx % texW, py = idx / texW;
                if (px < minPx) minPx = px;
                if (px > maxPx) maxPx = px;
                if (py < minPy) minPy = py;
                if (py > maxPy) maxPy = py;
            }
            m_popupTermsMapEmpty = false;
        }
        if (m_popupTermsMapEmpty) return;

        int padX = std::max((maxPx - minPx) / 2, 120);
        int padY = std::max((maxPy - minPy) / 2, 120);
        int sx = std::max(0, minPx - padX), sy = std::max(0, minPy - padY);
        int sw = std::min(texW - sx, (maxPx - minPx) + 2 * padX);
        int sh = std::min(texH - sy, (maxPy - minPy) + 2 * padY);
        if (sw <= 0 || sh <= 0) { m_popupTermsMapEmpty = true; return; }
        m_popupTermsMapSrcX = sx; m_popupTermsMapSrcY = sy;
        m_popupTermsMapSrcW = sw; m_popupTermsMapSrcH = sh;
        // A new offer starts fitted to its own terms, not wherever the last one
        // was left pointing.
        m_popupTermsMapZoom = 1.0f;
        m_popupTermsMapCx = sx + sw * 0.5f;
        m_popupTermsMapCy = sy + sh * 0.5f;
        m_popupTermsMapDragging = false;

        // Shade at the crop's own resolution, capped. The cap is well above the
        // ~500px slot because the map zooms: at 6x the visible sixth of the crop
        // still has to hold up, and shading that blurs while the political map
        // under it sharpens is worse than not zooming at all.
        const int CAP = 2048;
        float shrink = std::min(1.0f, std::min((float)CAP / sw, (float)CAP / sh));
        int ovW = std::max(1, (int)(sw * shrink));
        int ovH = std::max(1, (int)(sh * shrink));
        m_popupTermsMapBuf.assign((size_t)ovW * ovH, Color{0, 0, 0, 0});
        for (auto& m : marks) {
            auto it = m_provincePixels.find(m.pid);
            if (it == m_provincePixels.end()) continue;
            Color solid{m.col.r, m.col.g, m.col.b, 235};
            Color wash {m.col.r, m.col.g, m.col.b, 110};
            for (int idx : it->second) {
                int px = idx % texW - sx, py = idx / texW - sy;
                if (px < 0 || py < 0 || px >= sw || py >= sh) continue;
                int ox = px * ovW / sw, oy = py * ovH / sh;
                // Striped in OVERLAY space, not map space: in map space the
                // period would be sub-pixel on a big crop and come out as moire.
                bool bar = ((ox + m.lean * oy) % 8 + 8) % 8 < 5;
                m_popupTermsMapBuf[(size_t)oy * ovW + ox] = bar ? solid : wash;
            }
        }

        if (m_popupTermsMapTex.id > 0) UnloadTexture(m_popupTermsMapTex);
        Image img{};
        img.data = m_popupTermsMapBuf.data();
        img.width = ovW;
        img.height = ovH;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        m_popupTermsMapTex = LoadTextureFromImage(img);
        // Panning past the crop asks for texels outside [0,1]. The default wrap
        // repeats them, which would paste a ghost of the offer over unrelated
        // land; clamped, the transparent border is what extends instead. The
        // crop is padded by >=120px, so that border is always transparent.
        SetTextureWrap(m_popupTermsMapTex, TEXTURE_WRAP_CLAMP);
    }
    if (m_popupTermsMapEmpty || m_popupTermsMapTex.id == 0) return;

    Rectangle slot{(float)x, (float)y, (float)w, (float)h};
    Rectangle v = ceasefireTermsMapView(slot);

    // The crop the shading was rastered for. The view can wander off it once the
    // player pans or zooms out, which is what the clamped wrap mode set at
    // upload time is for: outside the crop there is nothing to shade anyway.
    float cropX = (float)m_popupTermsMapSrcX, cropY = (float)m_popupTermsMapSrcY;
    float cropW = (float)m_popupTermsMapSrcW, cropH = (float)m_popupTermsMapSrcH;
    float ovW = (float)m_popupTermsMapTex.width, ovH = (float)m_popupTermsMapTex.height;

    DrawRectangle(x, y, w, h, Color{12, 14, 22, 255});
    BeginScissorMode(x, y, w, h);
    DrawTexturePro(m_politicalTex, v, slot, {0, 0}, 0.0f, WHITE);
    DrawTexturePro(m_popupTermsMapTex,
                   {(v.x - cropX) / cropW * ovW, (v.y - cropY) / cropH * ovH,
                    v.width / cropW * ovW, v.height / cropH * ovH},
                   slot, {0, 0}, 0.0f, WHITE);

    // A province small enough that its shading is a couple of pixels at this
    // zoom would otherwise be invisible. Pin those, and only those -- a dot on
    // every province buries the shapes the map exists to show, and zooming in
    // is exactly how you stop needing the pin.
    float areaScale = (slot.width / v.width) * (slot.height / v.height);
    for (auto& m : marks) {
        auto pIt = m_provincePixels.find(m.pid);
        if (pIt == m_provincePixels.end()) continue;
        if (pIt->second.size() * areaScale > 16.0f) continue;
        auto cIt = m_provinceCenters.find(m.pid);
        if (cIt == m_provinceCenters.end()) continue;
        float fx = slot.x + (cIt->second.x - v.x) / v.width * slot.width;
        float fy = slot.y + (cIt->second.y - v.y) / v.height * slot.height;
        if (fx < slot.x || fx > slot.x + slot.width ||
            fy < slot.y || fy > slot.y + slot.height) continue;
        DrawCircle((int)fx, (int)fy, 4.5f, Color{0, 0, 0, 170});
        DrawCircle((int)fx, (int)fy, 3.0f, m.col);
    }
    EndScissorMode();
    DrawRectangleLines(x, y, w, h, Color{90, 95, 125, 220});

    // Say so: a map you can zoom looks exactly like one you cannot.
    const char* hint = m_popupTermsMapZoom > 1.01f || m_popupTermsMapZoom < 0.99f
                     ? TextFormat("%.1fx  right-click resets", m_popupTermsMapZoom)
                     : "scroll to zoom, drag to pan";
    int hw = MeasureText(hint, 11);
    DrawRectangle(x + w - hw - 12, y + 4, hw + 8, 15, Color{12, 14, 22, 170});
    DrawText(hint, x + w - hw - 8, y + 6, 11, Color{170, 176, 195, 255});

    // Legend, so the colours mean something without a manual. Measured before
    // it is drawn: it sits on the map, and panning decides what is underneath.
    struct Key { Color c; const char* t; };
    std::vector<Key> keys;
    if (!terms.ourProvs.empty())   keys.push_back({Color{120, 210, 140, 255}, "you gain"});
    if (!terms.theirProvs.empty()) keys.push_back({Color{225, 130, 120, 255}, "you cede"});
    if (!terms.ourDropClaims.empty() || !terms.theirDropClaims.empty())
        keys.push_back({Color{225, 190, 110, 255}, "claim dropped"});
    int lw = 0;
    for (auto& k : keys) lw += 24 + MeasureText(k.t, 11);
    if (lw > 0) {
        int lx = x + 6, ly = y + h - 16;
        DrawRectangle(lx - 4, ly - 3, lw, 17, Color{12, 14, 22, 180});
        for (auto& k : keys) {
            DrawCircle(lx + 4, ly + 5, 3.5f, k.c);
            DrawText(k.t, lx + 12, ly, 11, Color{200, 205, 220, 255});
            lx += 24 + MeasureText(k.t, 11);
        }
    }
}

// The window onto the province texture that the terms map is showing.
//
// Derived every frame from zoom and centre rather than stored, so the clamps
// below are the only thing that decides where the edges of the world are. The
// auto-fit crop is first widened to the slot's aspect: a view that matches the
// slot fills it, and dragging then moves the land by exactly the mouse delta.
Rectangle Game::ceasefireTermsMapView(Rectangle slot) const {
    float texW = (float)m_provinces.getWidth(), texH = (float)m_provinces.getHeight();
    float sw = (float)m_popupTermsMapSrcW, sh = (float)m_popupTermsMapSrcH;
    if (texW <= 0 || texH <= 0 || sw <= 0 || sh <= 0 || slot.width <= 0 || slot.height <= 0)
        return {0, 0, texW, texH};

    float slotAspect = slot.width / slot.height;
    float baseW = sw, baseH = sh;
    if (sw / sh < slotAspect) baseW = sh * slotAspect;
    else                      baseH = sw / slotAspect;

    float vw = baseW / m_popupTermsMapZoom;
    float vh = baseH / m_popupTermsMapZoom;
    // Never wider than the world, and never so far off the edge that half the
    // slot is empty space.
    if (vw > texW) { vw = texW; vh = vw / slotAspect; }
    if (vh > texH) { vh = texH; vw = vh * slotAspect; }
    float cx = std::min(std::max(m_popupTermsMapCx, vw * 0.5f), texW - vw * 0.5f);
    float cy = std::min(std::max(m_popupTermsMapCy, vh * 0.5f), texH - vh * 0.5f);
    return {cx - vw * 0.5f, cy - vh * 0.5f, vw, vh};
}

// Wheel zooms about the cursor, left-drag pans. Called from updatePopup()
// before its click handling, so a drag that happens to end over a button does
// not also press it.
void Game::updateCeasefireTermsMap(Rectangle slot) {
    if (m_popupTermsMapEmpty || m_popupTermsMapSrcW <= 0) return;
    Vector2 mouse = getMouse();
    bool over = CheckCollisionPointRec(mouse, slot);

    float wheel = GetMouseWheelMove();
    if (over && wheel != 0.0f) {
        Rectangle before = ceasefireTermsMapView(slot);
        // Where the cursor is pointing, as a fraction of the slot and as a point
        // on the map. Holding that point still is what makes the wheel feel like
        // it is zooming the map rather than scrolling it.
        float fx = (mouse.x - slot.x) / slot.width;
        float fy = (mouse.y - slot.y) / slot.height;
        float px = before.x + fx * before.width;
        float py = before.y + fy * before.height;

        m_popupTermsMapZoom = std::min(8.0f, std::max(0.25f,
            m_popupTermsMapZoom * (wheel > 0 ? 1.2f : 1.0f / 1.2f)));

        // Re-centre against the NEW view size, then let the shared clamp in
        // ceasefireTermsMapView decide whether the edges allow it.
        Rectangle after = ceasefireTermsMapView(slot);
        m_popupTermsMapCx = px + (0.5f - fx) * after.width;
        m_popupTermsMapCy = py + (0.5f - fy) * after.height;
    }

    // Zoomed in on the wrong end of a two-front offer, there is otherwise no way
    // back to the view that showed the whole deal short of closing the popup.
    if (over && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        m_popupTermsMapZoom = 1.0f;
        m_popupTermsMapCx = m_popupTermsMapSrcX + m_popupTermsMapSrcW * 0.5f;
        m_popupTermsMapCy = m_popupTermsMapSrcY + m_popupTermsMapSrcH * 0.5f;
    }

    if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        m_popupTermsMapDragging = true;
        m_popupTermsMapDragPrev = mouse;
    }
    if (m_popupTermsMapDragging) {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            m_popupTermsMapDragging = false;
        } else {
            Rectangle v = ceasefireTermsMapView(slot);
            // Screen pixels to map pixels. Dragging right pulls the land right,
            // so the view moves left.
            m_popupTermsMapCx -= (mouse.x - m_popupTermsMapDragPrev.x) * v.width / slot.width;
            m_popupTermsMapCy -= (mouse.y - m_popupTermsMapDragPrev.y) * v.height / slot.height;
            // The clamp lives in the view; fold it back so the centre cannot
            // build up a debt of off-map drag that has to be paid back before
            // the map moves again.
            Rectangle c = ceasefireTermsMapView(slot);
            m_popupTermsMapCx = c.x + c.width * 0.5f;
            m_popupTermsMapCy = c.y + c.height * 0.5f;
            m_popupTermsMapDragPrev = mouse;
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
        DrawText(title.c_str(), panelX + (panelW - titleW) / 2, panelY + 16, 28, hexToColor(m_config.accent()));
        DrawText("ESC to close", 10, m_screenH - 24, 14, Color{80, 80, 90, 200});
        return;
    }

    std::string title = "Peace Negotiation - " + targetC->name;
    int titleW = MeasureText(title.c_str(), 28);
    DrawText(title.c_str(), panelX + (panelW - titleW) / 2, panelY + 12, 28, hexToColor(m_config.accent()));

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
    DrawText("Selection Mode", sbX + 8, curY, 13, hexToColor(m_config.accent()));
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
    DrawText("Money", sbX + 8, curY, 13, hexToColor(m_config.accent()));
    curY += 18;

    auto drawMoneySlider = [&](int slot, const char* label, int& value, int max, Color col) {
        DrawText(label, sbX + 8, curY, 11, WHITE);
        char buf[32]; snprintf(buf, sizeof(buf), "%d", value);
        int tw = MeasureText(buf, 11);
        DrawText(buf, sbX + sidebarW - 8 - tw, curY, 11, col);
        curY += 14;
        Rectangle slr = {(float)(sbX + 8), (float)curY, (float)(sidebarW - 16), 10};
        DrawRectangle((int)slr.x, (int)slr.y, (int)slr.width, (int)slr.height, Color{40, 40, 50, 200});
        if (max > 0) {
            // GRAB, then follow the mouse until it is released -- even when it
            // leaves the bar.
            //
            // This used to update only while the cursor was inside the
            // rectangle, and the two things follow from that. Dragging left to
            // empty an offer stopped the instant the cursor crossed the left
            // edge, so the value stuck at whatever it had reached; and the only
            // way to reach zero was to release on the single leftmost column of
            // pixels, because everything right of it rounds to at least one
            // step. Zero is not a corner case here -- it is "never mind, I am
            // not paying them" -- and it was a one-pixel target.
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, slr))
                m_ceasefireMoneyDrag = slot;
            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
                m_ceasefireMoneyDrag = -1;
            if (m_ceasefireMoneyDrag == slot) {
                float t = (mouse.x - slr.x) / slr.width;
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);   // past either end pins
                value = (int)(t * max + 0.5f);
            }
            int fill = (int)(slr.width * value / std::max(1, max));
            DrawRectangle((int)slr.x, (int)slr.y, fill, (int)slr.height, col);
        }
        DrawRectangleLines((int)slr.x, (int)slr.y, (int)slr.width, (int)slr.height, Color{80, 80, 100, 220});
        curY += 16;
    };

    // What each side ACTUALLY has, not a thousand more than that.
    //
    // Both maxima used to be treasury + 1000, so the slider let you compose an
    // offer of money you do not have and a demand for money they do not have.
    // Nothing said so: the terms were clamped silently when the offer was sent
    // (see the clamp further down) and again when it was executed, so the deal
    // you agreed to was not the deal you built, and the number you dragged to
    // was never the number that moved.
    int pMax = (int)std::max(0.0, m_countries.getAll()[m_playerCountryId].treasury);
    int tMax = (int)std::max(0.0, targetC->treasury);
    drawMoneySlider(0, "Money we offer", m_ceasefireOurMoney, pMax, Color{40, 200, 40, 220});
    drawMoneySlider(1, "Money we demand", m_ceasefireTheirMoney, tMax, Color{220, 60, 60, 220});

    curY += 4;

    // ── Summary ──
    DrawText("Summary", sbX + 8, curY, 13, hexToColor(m_config.accent()));
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
        // One thing at a time, and asked BEFORE the offer is paid for. The
        // panel that opened this screen greys the button out for a pair with
        // something pending, so this is the screen agreeing with the panel --
        // but the money below leaves the treasury the moment the offer is
        // sent, and an offer refused after that would be money burnt.
        if (hasPendingDiplomacy(playerC->isoA3, targetC->isoA3)) {
            addNotification("You already have an offer awaiting " +
                            diploDisplayName(targetC->isoA3) + "'s answer",
                            Color{220, 170, 90, 255}, 5.0f);
            m_inCeasefireScreen = false;
            m_ceasefireSelectMode = 0;
            return;
        }
        // Clamp offer to what the player can actually pay (treasury >= 0)
        double& pTreas = m_countries.getAll()[m_playerCountryId].treasury;
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
        queueDiplomaticAction(da);
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
        if (!da.subjectIso.empty()) entry["subjectIso"] = da.subjectIso;
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
    // Per-country research progression (AI countries)
    for (auto& [cid, alloc] : m_countryResearchAllocation)
        if (alloc > 0.0f) j["countryResearchAlloc"][std::to_string(cid)] = alloc;
    for (auto& [cid, pts] : m_countryResearchPoints)
        if (pts > 0) j["countryResearchPoints"][std::to_string(cid)] = pts;
    for (auto& [cid, active] : m_countryResearchActive)
        if (active >= 0 && active < (int)m_researchNodes.size())
            j["countryResearchActive"][std::to_string(cid)] = m_researchNodes[active].id;
    for (auto& [cid, inv] : m_countryResearchInvested)
        if (inv > 0) j["countryResearchInvested"][std::to_string(cid)] = inv;

    // Balances
    for (auto& [cid, bal] : m_countryBalances) {
        j["balances"][std::to_string(cid)] = bal;
    }

    // Minority alignment drift and minority policy, both per country.
    //
    // "alignmentDrift" (a flat minority -> float map) is the pre-per-country
    // shape and is still READ on load, so old saves keep their numbers. It is
    // no longer written: a save that emitted both would be ambiguous about
    // which one wins, and the country-keyed map is strictly more information.
    for (auto& [cid, byName] : m_minorityAlignmentDrift) {
        auto& node = j["alignmentDriftByCountry"][std::to_string(cid)];
        for (auto& [name, drift] : byName) node[name] = drift;
    }
    for (auto& [cid, byName] : m_ethnicPolicies) {
        auto& node = j["ethnicPolicies"][std::to_string(cid)];
        for (auto& [name, opts] : byName) node[name] = opts;
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

    // War weariness. Without this a save/load wipes the entire cost of every
    // alliance the country has honoured, which is the only thing making that
    // decision a decision.
    for (auto& [cid, val] : m_countryWarWeariness) {
        if (val > 0.0f) j["warWeariness"][std::to_string(cid)] = val;
    }

    // Where each government now stands, which is not where the map started it.
    //
    // Doctrines move it: shiftCountryCompass() runs while one is being
    // implemented and every turn it stays in force, and that drift is the whole
    // point of enacting them. None of it was written down. A load rebuilt the
    // compass from the map file, so every government snapped back to its 1939
    // position while keeping the doctrines it had passed to get away from it --
    // and since doctrine availability is decided by the compass, a player who
    // had worked their way left found the left doctrines locked again on
    // reload.
    //
    // Written for every country, not only the player's: the AI shifts too, and
    // a world that resets its politics on load is a different world.
    for (auto& [cid, pc] : m_countryCompass) {
        auto& node = j["countryCompass"][std::to_string(cid)];
        node["economic"] = pc.economic;
        node["social"] = pc.social;
    }

    // Post-revolt cooldowns, for the same reason: without them a save/load
    // makes every province that has just been pacified immediately eligible to
    // rise again, and reloading becomes a way to re-roll the map.
    for (auto& [pid, turns] : m_provinceRebellionCooldown) {
        if (turns > 0) j["rebellionCooldown"][std::to_string(pid)] = turns;
    }

    // Diplomatic relations. These were previously not persisted anywhere, so
    // every war/alliance declared in-game was silently lost on load.
    for (auto& [isoA, targets] : m_relations) {
        for (auto& [isoB, r] : targets) {
            if (!r.war && !r.alliance && !r.nonAggression && !r.guarantee) continue;
            nlohmann::json e;
            e["war"] = r.war;
            e["ally"] = r.alliance;
            e["nonAggression"] = r.nonAggression;
            e["guarantee"] = r.guarantee;
            j["relations"][isoA][isoB] = e;
        }
    }

    // Claims. Only the pending add/drop lists were saved before, so claims
    // created at runtime (e.g. by a rebellion) vanished on reload.
    for (auto& [iso, pids] : m_claims) {
        if (pids.empty()) continue;
        j["claims"][iso] = pids;
    }

    // Calendar date — advanced every turn but only ever read back from the
    // map's metadata, so a loaded save always jumped back to the map's start.
    j["mapDate"] = m_mapDate;

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
            // Money coming back is the one moment the treasury is the point.
            Audio::get().playSfx("coin");
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
            da.subjectIso = entry.value("subjectIso", std::string());
            // Through the same door as everything else, because a save written
            // before the one-channel rule existed can carry a queue that
            // breaks it -- three copies of one declaration, most often. The
            // extras were always no-ops at resolution; dropping them here is
            // what stops the loaded game re-staging the popups they produced.
            queueDiplomaticAction(std::move(da));
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
    // Per-country research progression (AI countries)
    if (j.contains("countryResearchAlloc"))
        for (auto& [key, val] : j["countryResearchAlloc"].items())
            m_countryResearchAllocation[std::stoi(key)] = val.get<float>();
    if (j.contains("countryResearchPoints"))
        for (auto& [key, val] : j["countryResearchPoints"].items())
            m_countryResearchPoints[std::stoi(key)] = val.get<int>();
    if (j.contains("countryResearchActive"))
        for (auto& [key, val] : j["countryResearchActive"].items()) {
            std::string nodeId = val.get<std::string>();
            for (int i = 0; i < (int)m_researchNodes.size(); ++i)
                if (m_researchNodes[i].id == nodeId) {
                    m_countryResearchActive[std::stoi(key)] = i;
                    break;
                }
        }
    if (j.contains("countryResearchInvested"))
        for (auto& [key, val] : j["countryResearchInvested"].items())
            m_countryResearchInvested[std::stoi(key)] = val.get<int>();

    // Balances
    if (j.contains("balances")) {
        for (auto& [key, val] : j["balances"].items()) {
            m_countryBalances[std::stoi(key)] = val.get<float>();
        }
    }

    // Minority alignment drift, per country.
    //
    // CLAMPED ON THE WAY IN. Saves written before the drift bound existed can
    // hold values in the hundreds or thousands -- a single war declaration used
    // to charge -30 per province holding the group -- and the bar can only ever
    // show 0..100. Loading those verbatim would carry the old irreversibility
    // into the fixed build: the save would look identical and still refuse to
    // recover. Anything past the bound was unreachable on screen anyway, so
    // clamping loses nothing a player could see and restores a position they
    // can actually govern their way out of.
    auto loadDrift = [&](int cid, const std::string& name, float v) {
        m_minorityAlignmentDrift[cid][name] =
            std::clamp(v, -MINORITY_DRIFT_LIMIT, MINORITY_DRIFT_LIMIT);
    };
    if (j.contains("alignmentDriftByCountry")) {
        for (auto& [cidKey, node] : j["alignmentDriftByCountry"].items()) {
            const int cid = std::stoi(cidKey);
            for (auto& [name, val] : node.items())
                loadDrift(cid, name, val.get<float>());
        }
    } else if (j.contains("alignmentDrift")) {
        // MIGRATION. Before minority policy was a country's own, drift was one
        // number per minority for the entire world. There is no way to work out
        // retroactively which government earned which part of it, and dropping
        // it would hand every country on an old save a clean slate — turning a
        // long-running grievance into contentment on load. Copying the world
        // figure to every country preserves the state that was actually being
        // simulated; the moment play resumes, the values diverge on their own.
        std::vector<int> realCountries;
        for (auto& [cid, c] : m_countries.getAll())
            if (cid > 0 && cid < SPC_CID) realCountries.push_back(cid);
        for (auto& [name, val] : j["alignmentDrift"].items()) {
            const float drift = val.get<float>();
            for (int cid : realCountries) loadDrift(cid, name, drift);
        }
    }

    // Minority policy, per country. Absent on a pre-per-country save, in which
    // case every country simply starts from the category defaults — the same
    // place the old global table started before the player touched it.
    if (j.contains("ethnicPolicies")) {
        for (auto& [cidKey, node] : j["ethnicPolicies"].items()) {
            const int cid = std::stoi(cidKey);
            for (auto& [name, val] : node.items())
                m_ethnicPolicies[cid][name] = val.get<std::vector<int>>();
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

    // War weariness (see saveStateJson)
    if (j.contains("warWeariness")) {
        for (auto& [key, val] : j["warWeariness"].items()) {
            m_countryWarWeariness[std::stoi(key)] = val.get<float>();
        }
    }

    // Government positions (see saveStateJson). Absent from saves written
    // before this was recorded, which load as they always did -- from the map's
    // starting positions -- rather than with everyone at dead centre.
    //
    // Applied AFTER the map has been read, so it overwrites the starting
    // values rather than being overwritten by them.
    if (j.contains("countryCompass")) {
        for (auto& [key, node] : j["countryCompass"].items()) {
            const int cid = std::stoi(key);
            m_countryCompass[cid] = { node.value("economic", 0.0f),
                                      node.value("social",   0.0f) };
        }
    }

    // Post-revolt cooldowns (see saveStateJson). Absent from older saves, which
    // simply load with nothing cooling down.
    if (j.contains("rebellionCooldown")) {
        for (auto& [key, val] : j["rebellionCooldown"].items()) {
            const int turns = val.get<int>();
            if (turns > 0) m_provinceRebellionCooldown[std::stoi(key)] = turns;
        }
    }

    // Diplomatic relations (see saveStateJson) — restored symmetrically, the
    // same way the in-game setters maintain both directions.
    if (j.contains("relations")) {
        for (auto& [isoA, targets] : j["relations"].items()) {
            for (auto& [isoB, e] : targets.items()) {
                CountryRelation r;
                r.war = e.value("war", false);
                r.alliance = e.value("ally", false);
                r.nonAggression = e.value("nonAggression", false);
                r.guarantee = e.value("guarantee", false);
                m_relations[isoA][isoB] = r;
                m_relations[isoB][isoA] = r;
            }
        }
    }

    // Claims — rebuild both the forward and reverse indices
    if (j.contains("claims")) {
        m_claims.clear();
        m_claimsByProvince.clear();
        for (auto& [iso, pids] : j["claims"].items()) {
            for (auto& p : pids) {
                int pid = p.get<int>();
                m_claims[iso].push_back(pid);
                m_claimsByProvince[pid].push_back(iso);
            }
        }
    }

    if (j.contains("mapDate")) m_mapDate = j["mapDate"].get<std::string>();

    // Turn number
    if (j.contains("turnNumber")) {
        m_turnNumber = j["turnNumber"];
    }
}
