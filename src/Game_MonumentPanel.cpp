// The Monuments screen: what a country has, what the slots cost, and the one
// dial that matters -- which of them are switched on this turn.
//
// ── WHY THE LIST IS THE SCREEN ──
//
// There is no build button here. A monument is built in a PROVINCE, from the
// province panel, because where it stands is most of what it does -- a
// university in the capital and the same university in a border province are
// different buildings. What this screen is for is the decision that is not
// about a place: eleven monuments, money for four, which four.
//
// So every row is a switch, and the column beside it is the price of the slot
// that switch takes. The rows are in the order the slots are charged (see
// odmon::chargeOrder), so the cheap slot is at the top and the expensive one
// at the bottom, and a player can read down the list until the money runs out.

#include "Game.h"

#include "GameInternals.h"
#include "i18n/Text.h"

#include <algorithm>

namespace {

/** A row's height, and the gap under the header. Shared by draw and hit test. */
constexpr int kRowH = 46;
constexpr int kHeaderH = 150;

}  // namespace

void Game::drawMonumentsPanel() {
    if (!m_inMonuments) return;
    const int cid = m_playerCountryId;
    const Color accent = hexToColor(m_config.accent());
    const Vector2 mouse = getMouse();
    const bool click = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);

    DrawRectangle(0, 0, m_screenW, m_screenH, Color{0, 0, 0, 200});

    const int panelW = std::min(920, m_screenW - 60);
    const int x0 = (m_screenW - panelW) / 2;
    int y = 40;

    DrawText(T("Monuments"), x0, y, 28, accent);
    y += 38;

    // ── WHAT THIS COSTS, BEFORE THE LIST OF THINGS THAT COST IT ──
    //
    // The slot price is the whole mechanic, and a player who reads the rows
    // first and the price afterwards has already decided. So the running total
    // and the price of the NEXT switch come first.
    const std::vector<odmon::Holding> held = monumentsOf(cid);
    const int active = monumentSlotsUsed(cid);
    DrawText(TextFormat(T("%d built, %d switched on"), (int)held.size(), active),
             x0, y, 18, RAYWHITE);
    y += 24;
    DrawText(TextFormat(T("Slots cost %.0f a turn. Switching another on would add %.0f."),
                        monumentUpkeep(cid), monumentNextSlotCost(cid)),
             x0, y, 16, Color{200, 190, 150, 255});
    y += 22;
    DrawText(T("A monument that is switched off costs nothing and does nothing. "
               "Build them in a province, from its panel."),
             x0, y, 14, Color{140, 148, 165, 255});
    y += 26;

    // Close, in the corner every other full-screen panel puts it.
    {
        const Rectangle close{(float)(m_screenW - 44), 8, 36, 36};
        const bool hov = CheckCollisionPointRec(mouse, close);
        DrawRectangleRounded(close, 0.2f, 6, {60, 60, 70, 180});
        DrawRectangleRoundedLines(close, 0.2f, 6, hov ? RED : Color{180, 180, 180, 200});
        const int xw = MeasureText("X", 20);
        DrawText("X", (int)(close.x + close.width / 2 - xw / 2), 12, 20,
                 hov ? RED : Color{180, 180, 180, 200});
        if (click && hov) {
            m_inMonuments = false;
            m_activeSidebarTab = 0;
            if (m_renderer) m_renderer->setPaused(false);
            return;
        }
    }
    DrawText(T("ESC to close"), m_screenW - 140, 55, 14, Color{120, 120, 140, 150});

    if (held.empty()) {
        DrawText(T("You have not built one yet."), x0, y + 20, 18, Color{170, 170, 190, 255});
        DrawText(T("Select a province you own and use the Monuments tab in its panel."),
                 x0, y + 46, 15, Color{140, 148, 165, 255});
        return;
    }

    // In charge order, so the row at the top is the one paying 50 and the row
    // at the bottom is the one paying the most. That is the order a player
    // decides in.
    const std::vector<odmon::Holding> rows = odmon::chargeOrder(held);
    int slot = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const odmon::Holding& h = rows[i];
        const int ry = y + (int)i * kRowH;
        if (ry > m_screenH - kRowH) break;   // the rest is off the screen
        const Rectangle row{(float)x0, (float)ry, (float)panelW, (float)(kRowH - 6)};
        const bool hov = CheckCollisionPointRec(mouse, row);
        DrawRectangleRounded(row, 0.08f, 6,
                             h.active ? (hov ? Color{40, 56, 44, 230} : Color{30, 44, 34, 220})
                                      : (hov ? Color{44, 40, 40, 230} : Color{32, 30, 34, 200}));

        const Province* p = m_provinces.getProvinceById(h.provinceId);
        const std::string where = p ? od::i18n::properName(p->name) : std::string("?");
        DrawText(TextFormat("%s  %s", T(odmon::kindName(h.kind)),
                            std::string(h.level, 'I').c_str()),
                 x0 + 12, ry + 6, 18, h.active ? RAYWHITE : Color{150, 150, 165, 255});
        DrawText(where.c_str(), x0 + 12, ry + 26, 14, Color{150, 158, 175, 255});

        // What this one is costing, which is the slot its POSITION takes and
        // not anything about the monument itself.
        if (h.active) {
            ++slot;
            DrawText(TextFormat(T("slot %d  -  %.0f a turn"), slot, odmon::slotCost(slot)),
                     x0 + panelW - 340, ry + 14, 16, Color{220, 190, 130, 255});
        } else {
            DrawText(T("off"), x0 + panelW - 340, ry + 14, 16, Color{130, 130, 145, 255});
        }

        // The switch.
        const Rectangle tog{(float)(x0 + panelW - 170), (float)(ry + 8), 76.0f, 24.0f};
        const bool thov = CheckCollisionPointRec(mouse, tog);
        DrawRectangleRounded(tog, 0.25f, 6,
                             h.active ? Color{60, 110, 70, 230} : Color{60, 60, 74, 220});
        DrawRectangleRoundedLines(tog, 0.25f, 6, thov ? WHITE : Color{110, 110, 130, 200});
        const char* tl = h.active ? T("On") : T("Off");
        DrawText(tl, (int)(tog.x + (tog.width - MeasureText(tl, 15)) / 2), (int)tog.y + 5, 15,
                 thov ? WHITE : Color{210, 210, 225, 255});
        if (click && thov) {
            setMonumentActive(cid, h.provinceId, !h.active);
            return;   // the list is re-ordered by that, so stop drawing this one
        }

        // And the way out. Dismantling is a purchase, so it says what it costs.
        const Rectangle dis{(float)(x0 + panelW - 86), (float)(ry + 8), 76.0f, 24.0f};
        const bool dhov = CheckCollisionPointRec(mouse, dis);
        DrawRectangleRounded(dis, 0.25f, 6, dhov ? Color{90, 40, 40, 235} : Color{56, 34, 34, 210});
        DrawRectangleRoundedLines(dis, 0.25f, 6, dhov ? Color{220, 140, 140, 230}
                                                      : Color{110, 80, 80, 200});
        const std::string dl = TextFormat(T("Remove %.0f"), odmon::kDismantleCost);
        DrawText(dl.c_str(), (int)(dis.x + (dis.width - MeasureText(dl.c_str(), 13)) / 2),
                 (int)dis.y + 6, 13, dhov ? WHITE : Color{210, 190, 190, 255});
        if (click && dhov) {
            dismantleMonument(cid, h.provinceId);
            return;
        }
    }
}
