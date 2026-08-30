// The account screen: sign in, pick a nickname, change it, delete the account.
//
// All of the network work lives in AccountClient, which is non-blocking and
// knows nothing about raylib. This file only draws what that reports and turns
// clicks into calls -- so a slow or dead account service makes the screen say
// so, and never stalls a frame.
//
// A note on what is shown. The delete screen prints the server's own
// description of what will happen, verbatim, rather than a paraphrase written
// here. Two copies of "what deletion does" would drift, and the one people
// read is the one that would be wrong.

#include "Game.h"
#include "TextInput.h"
#include "Audio.h"
#include "GameInternals.h"
#include "net/AccountClient.h"
#include "net/BadgeStyle.h"

#include <cstring>
#include <ctime>

namespace {

Color rgba(uint32_t v) {
    return Color{static_cast<unsigned char>(v >> 24), static_cast<unsigned char>(v >> 16),
                 static_cast<unsigned char>(v >> 8),  static_cast<unsigned char>(v)};
}

// A signed-in player's name is the one thing on this screen that can be
// arbitrary text from elsewhere, so it is clipped rather than trusted to fit.
std::string ellipsize(const std::string& text, int fontSize, int maxWidth) {
    if (MeasureText(text.c_str(), fontSize) <= maxWidth) return text;
    std::string out = text;
    while (!out.empty() && MeasureText((out + "...").c_str(), fontSize) > maxWidth) {
        out.pop_back();
    }
    return out + "...";
}

struct Button {
    Rectangle rect;
    bool      hovered = false;
};

Button buttonAt(float x, float y, float w, float h, Vector2 mouse) {
    Button b{{x, y, w, h}, false};
    b.hovered = CheckCollisionPointRec(mouse, b.rect);
    return b;
}

void drawButton(const Button& b, const char* label, int fontSize,
                Color base, Color border, bool enabled = true) {
    const Color bg = !enabled ? Color{30, 30, 34, 200}
                   : b.hovered ? Color{(unsigned char)(base.r + 20), (unsigned char)(base.g + 20),
                                       (unsigned char)(base.b + 20), 240}
                               : base;
    DrawRectangleRounded(b.rect, 0.15f, 8, bg);
    DrawRectangleRoundedLines(b.rect, 0.15f, 8, enabled ? border : Color{70, 70, 80, 180});
    const char* shown = T(label);
    // THE LABEL IS TRANSLATED HERE, not at the hundred call sites.
    //
    // Every button on this screen comes through this function, so this is the
    // one place that has to know about the language -- the same reasoning that
    // put the shadowed DrawText in i18n/Text.h rather than editing 970 draw
    // sites. A caller passing a literal gets it translated for free; the
    // extractor is told about this function so those literals reach en.json.
    const int tw = MeasureText(shown, fontSize);
    DrawText(shown, (int)(b.rect.x + (b.rect.width - tw) / 2),
             (int)(b.rect.y + (b.rect.height - fontSize) / 2), fontSize,
             enabled ? (b.hovered ? WHITE : LIGHTGRAY) : Color{110, 110, 120, 255});
}

/**
 * Where everything on the signed-in screen goes.
 *
 * Computed ONCE and used by both the click handler and the renderer. They used
 * to work it out separately from the same magic offsets, which is exactly how
 * the account-ID button ended up drawn on top of the nickname label: two copies
 * of a layout drift the moment either one changes.
 */
struct AccountLayout {
    int centerX = 0;
    int btnW = 300, btnH = 52, gap = 14;
    int name = 0, tags = 0, chipHint = 0, chips = 0;
    int nickLabel = 0, nickField = 0, change = 0;
    int signOut = 0, del = 0;
    int idRow = 0, idNote = 0;
    Rectangle idButton{}, idCopy{}, idHide{};
};

AccountLayout accountLayout(int screenW, int screenH, bool banned, bool idShown) {
    AccountLayout l;
    l.centerX = screenW / 2;

    // A running cursor rather than offsets from a common origin: adding a row
    // then shifts everything below it instead of landing on top of something.
    int y = screenH / 2 - 190;
    if (banned) y += 46;          // the ban banner needs its own room

    l.name = y;            y += 46;
    l.tags = y;            y += 26;
    l.chipHint = y;        y += 18;
    l.chips = y;           y += 40;

    l.nickLabel = y;       y += 20;
    l.nickField = y;       y += l.btnH + l.gap;
    l.change = y;          y += l.btnH + l.gap * 2;
    l.signOut = y;         y += l.btnH + l.gap;
    l.del = y;             y += l.btnH + l.gap * 2;

    // Administrative detail, so it sits below the actions rather than between
    // the player's name and the thing they came here to do.
    l.idRow = y;
    const float bw = idShown ? 92.0f : 220.0f;
    l.idButton = {(float)(l.centerX - (idShown ? 150 : 110)), (float)y,
                  idShown ? 196.0f : 220.0f, 26.0f};
    l.idCopy = {(float)(l.centerX + 52), (float)y, bw / 2 + 6, 26.0f};
    l.idHide = {(float)(l.centerX + 106), (float)y, bw / 2 + 6, 26.0f};
    l.idNote = y + 30;
    return l;
}

}  // namespace

bool accountHasProvider(const AccountInfo& info, AuthProvider p) {
    const std::string id = authProviderId(p);
    for (const auto& l : info.linked) if (l == id) return true;
    return false;
}

int accountProvidersWidth(const std::vector<AuthProvider>& all) {
    int total = 0;
    for (AuthProvider p : all) total += MeasureText(authProviderLabel(p), 14) + 26 + 8;
    return total > 0 ? total - 8 : 0;
}

// ────────────────────────────────────────────────────────────────────────────
// openAccountMenu
// ────────────────────────────────────────────────────────────────────────────
void Game::openAccountMenu() {
    m_accountAgreed = m_config.accountAgreed;
    m_currentScreen = SCREEN_ACCOUNT;
    m_accountNickField.clear();
    m_accountFieldFocused = false;
    AccountClient::get().clearMessage();

    // A first visit tries the stored token. Only once per launch: retrying on
    // every visit would hammer the service when it is down, and the state we
    // already have is the right answer in the meantime.
    // Startup already tried the stored token. This is only a retry for the
    // case where the service was unreachable then -- not a second attempt on
    // every visit, which would hammer a service that is down.
    if (!m_accountRestoreTried) {
        m_accountRestoreTried = true;
        if (AccountClient::get().status() == AccountClient::Status::SignedOut)
            AccountClient::get().bootstrap();
    }
}

// ────────────────────────────────────────────────────────────────────────────
// updateAccountMenu
// ────────────────────────────────────────────────────────────────────────────
void Game::updateAccountMenu() {
    AccountClient& client = AccountClient::get();
    client.update();

    if (isMouseOverConsole()) return;

    const Vector2 mouse = getMouse();
    const bool click = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    const auto status = client.status();

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (status == AccountClient::Status::DeleteConfirm) { client.cancelDelete(); return; }
        if (status == AccountClient::Status::WaitingForBrowser) { client.cancelSignIn(); return; }
        m_currentScreen = SCREEN_MENU;
        return;
    }

    const int centerX = m_screenW / 2;
    const int btnW = 300, btnH = 52, gap = 14;
    const int startY = m_screenH / 2 - 150;

    // The text field is live whenever one is on screen; there is only ever one.
    const bool wantsField = status == AccountClient::Status::NeedsNickname ||
                            status == AccountClient::Status::SignedIn;
    if (wantsField && m_accountFieldFocused) {
        int c = GetCharPressed();
        while (c > 0) {
            // Every character the field takes. Jittered, because a
            // typed word is a run of distinct taps, not one tap looped.
            Audio::get().playSfx("key_type", 0.12f);
            // 24 is the server's ceiling. Stopping here means the player is
            // never told off for something they could not see they had typed.
            if (c >= 32 && c < 127 && m_accountNickField.size() < 24) {
                m_accountNickField += (char)c;
            }
            c = GetCharPressed();
        }
        if (odTextEditKeys(m_accountNickField, 24)) {
        }
    }

    switch (status) {
        case AccountClient::Status::SignedOut: {
            if (!client.configured()) break;
            // Whatever the service said it can actually use. A button for a
            // provider it has no credentials for is a dead end.
            const std::vector<AuthProvider> providers = client.providers();
            for (size_t i = 0; i < providers.size(); i++) {
                // A link-only provider CAN sign you in -- it just cannot create
                // the account. Someone who linked it already must be able to
                // use it, so the button stays live.
                const Button b = buttonAt((float)(centerX - btnW / 2),
                                          (float)(startY + (int)i * (btnH + gap)),
                                          (float)btnW, (float)btnH, mouse);
                // Nothing happens until the agreement is ticked. Gating the
                // ACTION rather than only greying the button means a stray
                // click cannot start an OAuth round trip nobody consented to.
                if (click && b.hovered && m_accountAgreed) {
                    client.beginSignIn(providers[i]);
                    return;
                }
            }
            // The policy sits under the sign-in options, where it is read
            // BEFORE authorising anything rather than after.
            if (!providers.empty()) {
                const int noteY = startY + (int)providers.size() * (btnH + gap) + 16;
                const Button agree = buttonAt((float)(centerX - 170), (float)(noteY + 44),
                                              26, 26, mouse);
                if (click && agree.hovered) {
                    m_accountAgreed = !m_accountAgreed;
                    m_config.accountAgreed = m_accountAgreed;
                    m_config.save(m_dataDir + "config.json");
                    return;
                }
                const Button pol = buttonAt((float)(centerX - 170), (float)(noteY + 82),
                                            170, 32, mouse);
                if (click && pol.hovered && !client.privacyUrl().empty()) {
                    OpenURL(client.privacyUrl().c_str());
                    return;
                }
                const Button ter = buttonAt((float)(centerX + 6), (float)(noteY + 82),
                                            170, 32, mouse);
                if (click && ter.hovered && !client.termsUrl().empty()) {
                    OpenURL(client.termsUrl().c_str());
                    return;
                }
            }

            // Nothing on offer: let the player ask again rather than stranding
            // them on a screen with no way forward.
            if (providers.empty()) {
                const Button retry = buttonAt((float)(centerX - btnW / 2),
                                              (float)(startY + 60), (float)btnW,
                                              (float)btnH, mouse);
                if (click && retry.hovered) client.probeService();
            }
            break;
        }

        case AccountClient::Status::WaitingForBrowser: {
            const Button open = buttonAt((float)(centerX - btnW / 2), (float)(startY + 110),
                                         (float)btnW, (float)btnH, mouse);
            const Button cancel = buttonAt((float)(centerX - btnW / 2),
                                           (float)(startY + 110 + btnH + gap),
                                           (float)btnW, (float)btnH, mouse);
            if (click && open.hovered) OpenURL(client.verifyUrl().c_str());
            if (click && cancel.hovered) client.cancelSignIn();
            break;
        }

        case AccountClient::Status::NeedsNickname: {
            const Rectangle field = {(float)(centerX - btnW / 2), (float)(startY + 70),
                                     (float)btnW, (float)btnH};
            if (click) m_accountFieldFocused = CheckCollisionPointRec(mouse, field);

            const Button create = buttonAt((float)(centerX - btnW / 2),
                                           (float)(startY + 70 + btnH + gap + 26),
                                           (float)btnW, (float)btnH, mouse);
            std::string why;
            const bool valid = AccountClient::nicknameLooksValid(m_accountNickField, why);
            if (valid && ((click && create.hovered) || IsKeyPressed(KEY_ENTER))) {
                client.createAccount(m_accountNickField);
            }
            break;
        }

        case AccountClient::Status::SignedIn: {
            const AccountInfo info = client.account();
            const AccountLayout L = accountLayout(m_screenW, m_screenH,
                                                  info.banned, m_accountShowId);

            // Provider chips: click an unlinked one to add it, a linked one to
            // remove it. The server refuses to remove the last, so there is no
            // way to lock yourself out from here.
            const std::vector<AuthProvider> all = client.providers();
            int chipX = L.centerX - accountProvidersWidth(all) / 2;
            for (AuthProvider p : all) {
                const int w = MeasureText(authProviderLabel(p), 14) + 26;
                const Rectangle chip = {(float)chipX, (float)L.chips, (float)w, 24.0f};
                if (click && CheckCollisionPointRec(mouse, chip)) {
                    const bool isLinked = accountHasProvider(info, p);
                    if (!isLinked) client.beginLink(p);
                    else if (info.linked.size() > 1) client.unlink(p);
                    else {
                        m_accountNote = T("That is your only way to sign in. Link another first.");
                        m_accountNoteTimer = 4.0f;
                    }
                    return;
                }
                chipX += w + 8;
            }

            // The account id can be hidden again. Revealing something with no
            // way to put it back means quitting the game to clear the screen.
            if (click) {
                if (!m_accountShowId) {
                    if (CheckCollisionPointRec(mouse, L.idButton)) {
                        m_accountShowId = true;
                        return;
                    }
                } else {
                    if (CheckCollisionPointRec(mouse, L.idCopy)) {
                        SetClipboardText(info.id.c_str());
                        m_accountNote = T("Account ID copied.");
                        m_accountNoteTimer = 3.0f;
                        return;
                    }
                    if (CheckCollisionPointRec(mouse, L.idHide)) {
                        m_accountShowId = false;
                        return;
                    }
                }
            }

            const Rectangle field = {(float)(L.centerX - L.btnW / 2), (float)L.nickField,
                                     (float)L.btnW, (float)L.btnH};
            if (click) m_accountFieldFocused = CheckCollisionPointRec(mouse, field);

            const Button change = buttonAt((float)(L.centerX - L.btnW / 2), (float)L.change,
                                           (float)L.btnW, (float)L.btnH, mouse);
            const Button out = buttonAt((float)(L.centerX - L.btnW / 2), (float)L.signOut,
                                        (float)L.btnW, (float)L.btnH, mouse);
            const Button del = buttonAt((float)(L.centerX - L.btnW / 2), (float)L.del,
                                        (float)L.btnW, (float)L.btnH, mouse);

            std::string why;
            const bool valid = AccountClient::nicknameLooksValid(m_accountNickField, why);
            if (valid && ((click && change.hovered) ||
                          (m_accountFieldFocused && IsKeyPressed(KEY_ENTER)))) {
                client.changeNickname(m_accountNickField);
                m_accountNickField.clear();
                m_accountFieldFocused = false;
            }
            if (click && out.hovered) { client.signOut(); m_accountNickField.clear(); }
            if (click && del.hovered) client.beginDelete();
            break;
        }

        case AccountClient::Status::DeleteConfirm: {
            const int lines = (int)client.deletionSummary().size();
            const float y = (float)(startY + 60 + lines * 22 + 30);
            const Button confirm = buttonAt((float)(centerX - btnW - 10), y,
                                            (float)btnW, (float)btnH, mouse);
            const Button cancel = buttonAt((float)(centerX + 10), y,
                                           (float)btnW, (float)btnH, mouse);
            if (click && confirm.hovered) client.confirmDelete();
            if (click && cancel.hovered) client.cancelDelete();
            break;
        }

        case AccountClient::Status::Working:
            break;
    }

    // Back button, always available except mid-confirmation.
    if (status != AccountClient::Status::DeleteConfirm) {
        const Button back = buttonAt((float)(centerX - 80), (float)(m_screenH - 100),
                                     160, 48, mouse);
        if (click && back.hovered) {
            // Leaving abandons a sign-in rather than letting it poll in the
            // background for ten minutes and then quietly sign the player in
            // somewhere they are no longer looking.
            if (status == AccountClient::Status::WaitingForBrowser) client.cancelSignIn();
            m_currentScreen = SCREEN_MENU;
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// drawAccountMenu
// ────────────────────────────────────────────────────────────────────────────
void Game::drawAccountMenu() {
    drawMenuBackground();
    DrawRectangle(0, 0, m_screenW, m_screenH, {0, 0, 0, 170});

    AccountClient& client = AccountClient::get();
    const Vector2 mouse = getMouse();
    const Color accent = hexToColor(m_config.accent());

    const int centerX = m_screenW / 2;
    const int btnW = 300, btnH = 52, gap = 14, fontSize = 22;
    const int startY = m_screenH / 2 - 150;

    const char* title = "Account";
    DrawText(title, centerX - MeasureText(title, 48) / 2, startY - 120, 48, accent);

    const auto status = client.status();

    if (!client.configured()) {
        const char* a = "No account service is configured.";
        const char* b = "Add \"accountIssuer\": \"https://your-worker.workers.dev\"";
        const char* c = "to data/config.json, then restart the game.";
        DrawText(a, centerX - MeasureText(a, 20) / 2, startY, 20, LIGHTGRAY);
        DrawText(b, centerX - MeasureText(b, 16) / 2, startY + 34, 16, GRAY);
        DrawText(c, centerX - MeasureText(c, 16) / 2, startY + 56, 16, GRAY);
    } else switch (status) {
        case AccountClient::Status::SignedOut: {
            const char* sub = "Sign in to claim a nickname and play multiplayer.";
            DrawText(sub, centerX - MeasureText(sub, 18) / 2, startY - 46, 18, GRAY);

            const std::vector<AuthProvider> providers = client.providers();
            if (providers.empty()) {
                const char* a = client.serviceReachable()
                    ? "The account service has no sign-in providers set up."
                    : "Could not reach the account service.";
                const char* b = client.serviceReachable()
                    ? "Register an OAuth app and set its secrets, then try again."
                    : "Check that it is deployed and that accountIssuer is right.";
                DrawText(a, centerX - MeasureText(a, 19) / 2, startY - 10, 19, LIGHTGRAY);
                DrawText(b, centerX - MeasureText(b, 15) / 2, startY + 18, 15, GRAY);

                const Button retry = buttonAt((float)(centerX - btnW / 2),
                                              (float)(startY + 60), (float)btnW,
                                              (float)btnH, mouse);
                drawButton(retry, "Try again", fontSize,
                           Color{45, 45, 55, 220}, Color{110, 110, 130, 200});
                break;
            }

            for (size_t i = 0; i < providers.size(); i++) {
                const Button b = buttonAt((float)(centerX - btnW / 2),
                                          (float)(startY + (int)i * (btnH + gap)),
                                          (float)btnW, (float)btnH, mouse);
                const Color tint = providers[i] == AuthProvider::Google  ? Color{50, 60, 80, 220}
                                 : providers[i] == AuthProvider::Discord ? Color{60, 50, 85, 220}
                                                                        : Color{45, 45, 55, 220};
                const std::string label =
                    std::string("Sign in with ") + authProviderLabel(providers[i]);
                drawButton(b, label.c_str(), fontSize, tint,
                           Color{120, 120, 150, 200}, m_accountAgreed);
                if (client.isLinkOnly(providers[i])) {
                    // Says so before the click, not after a browser round trip.
                    const char* note = "existing accounts only";
                    DrawText(note, (int)(b.rect.x + b.rect.width) - MeasureText(note, 13) - 12,
                             (int)(b.rect.y + b.rect.height) - 17, 13, Color{150, 150, 165, 255});
                }
            }

            // Said before the browser opens, not after. What a player agrees
            // to should not be a surprise they read on the way back.
            const int noteY = startY + (int)providers.size() * (btnH + gap) + 16;
            const char* note1 = "We ask each provider only for an anonymous user id.";
            const char* note2 = "No email address is requested or stored.";
            DrawText(note1, centerX - MeasureText(note1, 15) / 2, noteY, 15,
                     Color{140, 150, 140, 255});
            DrawText(note2, centerX - MeasureText(note2, 15) / 2, noteY + 20, 15,
                     Color{140, 150, 140, 255});

            // Agreeing comes before signing in, and both documents are one
            // click away from the tick -- being asked to accept something you
            // would have to go looking for is not being asked at all.
            const Button agree = buttonAt((float)(centerX - 170), (float)(noteY + 44),
                                          26, 26, mouse);
            DrawRectangleRounded(agree.rect, 0.2f, 6,
                                 m_accountAgreed ? Color{80, 130, 90, 240}
                                                 : Color{30, 32, 40, 230});
            DrawRectangleRoundedLines(agree.rect, 0.2f, 6,
                                      m_accountAgreed ? Color{150, 200, 165, 220}
                                                      : Color{175, 150, 110, 220});
            if (m_accountAgreed)
                DrawText("x", (int)agree.rect.x + 9, (int)agree.rect.y + 4, 18, WHITE);
            DrawText(T("I agree to the terms of use and privacy policy"),
                     (int)agree.rect.x + 36, (int)agree.rect.y + 5, 16,
                     m_accountAgreed ? Color{200, 210, 200, 255}
                                     : Color{225, 205, 165, 255});

            const Button pol = buttonAt((float)(centerX - 170), (float)(noteY + 82),
                                        170, 32, mouse);
            drawButton(pol, "Privacy policy", 16,
                       Color{34, 34, 42, 220}, Color{95, 95, 115, 200});
            const Button ter = buttonAt((float)(centerX + 6), (float)(noteY + 82),
                                        170, 32, mouse);
            drawButton(ter, "Terms of use", 16,
                       Color{34, 34, 42, 220}, Color{95, 95, 115, 200});
            break;
        }

        case AccountClient::Status::WaitingForBrowser: {
            const char* a = "Finish signing in in your browser.";
            const char* b = "This screen updates by itself when you are done.";
            DrawText(a, centerX - MeasureText(a, 22) / 2, startY + 20, 22, WHITE);
            DrawText(b, centerX - MeasureText(b, 16) / 2, startY + 52, 16, GRAY);

            const Button open = buttonAt((float)(centerX - btnW / 2), (float)(startY + 110),
                                         (float)btnW, (float)btnH, mouse);
            drawButton(open, "Open the page again", fontSize,
                       Color{45, 55, 70, 220}, Color{120, 140, 170, 200});
            const Button cancel = buttonAt((float)(centerX - btnW / 2),
                                           (float)(startY + 110 + btnH + gap),
                                           (float)btnW, (float)btnH, mouse);
            drawButton(cancel, "Cancel", fontSize,
                       Color{45, 45, 50, 220}, Color{110, 110, 120, 200});
            break;
        }

        case AccountClient::Status::NeedsNickname: {
            const char* a = "Choose a nickname.";
            const char* b = "3-24 characters. You can change it once a week.";
            DrawText(a, centerX - MeasureText(a, 24) / 2, startY, 24, WHITE);
            DrawText(b, centerX - MeasureText(b, 16) / 2, startY + 34, 16, GRAY);

            drawAccountField(centerX - btnW / 2, startY + 70, btnW, btnH);

            std::string why;
            const bool valid = AccountClient::nicknameLooksValid(m_accountNickField, why);
            if (!m_accountNickField.empty() && !valid) {
                DrawText(why.c_str(), centerX - MeasureText(why.c_str(), 15) / 2,
                         startY + 70 + btnH + 6, 15, Color{220, 140, 120, 255});
            }

            const Button create = buttonAt((float)(centerX - btnW / 2),
                                           (float)(startY + 70 + btnH + gap + 26),
                                           (float)btnW, (float)btnH, mouse);
            drawButton(create, "Create account", fontSize,
                       Color{40, 70, 45, 220}, Color{110, 170, 120, 200}, valid);
            break;
        }

        case AccountClient::Status::SignedIn: {
            const AccountInfo info = client.account();
            const AccountLayout L = accountLayout(m_screenW, m_screenH,
                                                  info.banned, m_accountShowId);

            if (info.banned) {
                const std::string head = "This account is banned from joining games";
                DrawText(head.c_str(), L.centerX - MeasureText(head.c_str(), 18) / 2,
                         L.name - 62, 18, Color{235, 120, 110, 255});
                const std::string why = ellipsize(
                    info.banReason.empty() ? "No reason was given." : info.banReason,
                    15, m_screenW - 140);
                DrawText(why.c_str(), L.centerX - MeasureText(why.c_str(), 15) / 2,
                         L.name - 40, 15, Color{200, 160, 155, 255});
                const std::string when = "Lifts in: " + info.banRemaining(
                    (long long)std::time(nullptr));
                DrawText(when.c_str(), L.centerX - MeasureText(when.c_str(), 15) / 2,
                         L.name - 22, 15, Color{200, 160, 155, 255});
            }

            // The name carries its badge colour. Same helper the roster and
            // chat will use, so a developer's name is the same green wherever
            // it appears rather than only here.
            const std::string name = ellipsize(info.nickname, 34, m_screenW - 120);
            DrawText(name.c_str(), L.centerX - MeasureText(name.c_str(), 34) / 2,
                     L.name, 34, rgba(badgeNameColor(info.badges)));

            if (!info.badges.empty()) {
                int total = 0;
                for (const auto& b : info.badges) total += MeasureText(badgeTag(b).c_str(), 15) + 12;
                int tagX = L.centerX - (total - 12) / 2;
                for (const auto& b : info.badges) {
                    const std::string tag = badgeTag(b);
                    DrawText(tag.c_str(), tagX, L.tags, 15, rgba(badgeTagColor(b)));
                    tagX += MeasureText(tag.c_str(), 15) + 12;
                }
            }

            drawAccountProviders(L.centerX, L.chips, info);

            const char* prompt = "New nickname (once a week)";
            DrawText(prompt, L.centerX - L.btnW / 2, L.nickLabel, 15, GRAY);
            drawAccountField(L.centerX - L.btnW / 2, L.nickField, L.btnW, L.btnH);

            std::string why;
            const bool valid = AccountClient::nicknameLooksValid(m_accountNickField, why);
            const Button change = buttonAt((float)(L.centerX - L.btnW / 2), (float)L.change,
                                           (float)L.btnW, (float)L.btnH, mouse);
            drawButton(change, "Change nickname", fontSize,
                       Color{45, 55, 70, 220}, Color{120, 140, 170, 200}, valid);
            if (!m_accountNickField.empty() && !valid) {
                DrawText(why.c_str(), L.centerX - MeasureText(why.c_str(), 15) / 2,
                         L.change + L.btnH + 2, 15, Color{220, 140, 120, 255});
            }

            const Button out = buttonAt((float)(L.centerX - L.btnW / 2), (float)L.signOut,
                                        (float)L.btnW, (float)L.btnH, mouse);
            drawButton(out, "Sign out", fontSize, Color{45, 45, 50, 220},
                       Color{110, 110, 120, 200});

            const Button del = buttonAt((float)(L.centerX - L.btnW / 2), (float)L.del,
                                        (float)L.btnW, (float)L.btnH, mouse);
            drawButton(del, "Delete account", fontSize,
                       Color{70, 35, 35, 220}, Color{170, 90, 90, 200});

            // Hidden by default, and hideable again: it is not a password, but
            // it identifies you, and this screen gets streamed.
            if (!m_accountShowId) {
                Button b; b.rect = L.idButton;
                b.hovered = CheckCollisionPointRec(mouse, b.rect);
                drawButton(b, "Show account ID", 14,
                           Color{32, 32, 40, 210}, Color{85, 85, 100, 190});
            } else {
                DrawRectangleRounded(L.idButton, 0.2f, 6, Color{26, 26, 33, 230});
                DrawRectangleRoundedLines(L.idButton, 0.2f, 6, Color{80, 80, 95, 190});
                const std::string shown = ellipsize(info.id, 13, (int)L.idButton.width - 16);
                DrawText(shown.c_str(), (int)L.idButton.x + 8,
                         (int)L.idButton.y + 7, 13, Color{200, 200, 215, 255});

                Button copy; copy.rect = L.idCopy;
                copy.hovered = CheckCollisionPointRec(mouse, copy.rect);
                drawButton(copy, "Copy", 13, Color{36, 44, 36, 220}, Color{95, 125, 95, 190});

                Button hide; hide.rect = L.idHide;
                hide.hovered = CheckCollisionPointRec(mouse, hide.rect);
                drawButton(hide, "Hide", 13, Color{40, 34, 34, 220}, Color{120, 95, 95, 190});

                const char* n = "identifies you to support — keep it off stream";
                DrawText(n, L.centerX - MeasureText(n, 12) / 2, L.idNote, 12,
                         Color{130, 130, 145, 255});
            }
            break;
        }

        case AccountClient::Status::DeleteConfirm: {
            const char* a = "Delete your account?";
            DrawText(a, centerX - MeasureText(a, 28) / 2, startY, 28, Color{235, 130, 120, 255});

            // The server's own words, not a paraphrase written here.
            int y = startY + 50;
            for (const auto& line : client.deletionSummary()) {
                const std::string text = ellipsize(line, 16, m_screenW - 160);
                DrawText(text.c_str(), centerX - MeasureText(text.c_str(), 16) / 2, y, 16, LIGHTGRAY);
                y += 22;
            }

            y += 30;
            const Button confirm = buttonAt((float)(centerX - btnW - 10), (float)y,
                                            (float)btnW, (float)btnH, mouse);
            drawButton(confirm, "Yes, delete it", fontSize,
                       Color{80, 35, 35, 230}, Color{190, 90, 90, 220});
            const Button cancel = buttonAt((float)(centerX + 10), (float)y,
                                           (float)btnW, (float)btnH, mouse);
            drawButton(cancel, "Keep my account", fontSize,
                       Color{45, 55, 45, 230}, Color{120, 160, 120, 220});
            break;
        }

        case AccountClient::Status::Working: {
            const char* a = "Working...";
            DrawText(a, centerX - MeasureText(a, 24) / 2, startY + 40, 24, LIGHTGRAY);
            break;
        }
    }

    if (m_accountNoteTimer > 0.0f) {
        m_accountNoteTimer -= GetFrameTime();
        const std::string t = ellipsize(m_accountNote, 17, m_screenW - 120);
        DrawText(t.c_str(), centerX - MeasureText(t.c_str(), 17) / 2,
                 m_screenH - 178, 17, Color{225, 200, 130, 255});
    }

    const std::string note = client.message();
    if (!note.empty()) {
        const std::string text = ellipsize(note, 17, m_screenW - 120);
        const Color tint = client.messageIsError() ? Color{225, 140, 125, 255}
                                                   : Color{150, 205, 155, 255};
        DrawText(text.c_str(), centerX - MeasureText(text.c_str(), 17) / 2,
                 m_screenH - 150, 17, tint);
    }

    if (status != AccountClient::Status::DeleteConfirm) {
        const Button back = buttonAt((float)(centerX - 80), (float)(m_screenH - 100),
                                     160, 48, mouse);
        drawButton(back, "Back", 20, Color{40, 40, 48, 220}, Color{110, 110, 130, 200});
    }
}

// ────────────────────────────────────────────────────────────────────────────
// drawAccountProviders — one chip per provider, filled when linked
// ────────────────────────────────────────────────────────────────────────────
void Game::drawAccountProviders(int centerX, int y, const AccountInfo& info) {
    AccountClient& client = AccountClient::get();
    std::vector<AuthProvider> all = client.providers();
    if (all.empty()) {
        // The probe failed or has not answered. Show what the account itself
        // says is linked, so a signed-in player always sees their providers --
        // just without the option to add one we do not know exists.
        for (const auto& id : info.linked) {
            if (id == "google")  all.push_back(AuthProvider::Google);
            if (id == "discord") all.push_back(AuthProvider::Discord);
            if (id == "github")  all.push_back(AuthProvider::GitHub);
        }
    }
    if (all.empty()) return;

    const Vector2 mouse = getMouse();
    const char* hint = "click to link or unlink";
    DrawText(hint, centerX - MeasureText(hint, 13) / 2, y - 18, 13,
             Color{110, 110, 125, 255});

    int x = centerX - accountProvidersWidth(all) / 2;
    for (AuthProvider p : all) {
        const char* label = authProviderLabel(p);
        const int w = MeasureText(label, 14) + 26;
        const Rectangle chip = {(float)x, (float)y, (float)w, 24.0f};
        const bool linked = accountHasProvider(info, p);
        const bool hover = CheckCollisionPointRec(mouse, chip);

        DrawRectangleRounded(chip, 0.4f, 6,
            linked ? (hover ? Color{70, 100, 80, 240} : Color{50, 80, 60, 230})
                   : (hover ? Color{60, 60, 72, 240} : Color{38, 38, 46, 220}));
        DrawRectangleRoundedLines(chip, 0.4f, 6,
            linked ? Color{110, 170, 125, 220} : Color{90, 90, 105, 200});
        DrawText(label, x + 13, y + 5, 14, linked ? WHITE : Color{140, 140, 155, 255});
        x += w + 8;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// drawAccountField — the one text input on this screen
// ────────────────────────────────────────────────────────────────────────────
void Game::drawAccountField(int x, int y, int w, int h) {
    const Rectangle r = {(float)x, (float)y, (float)w, (float)h};
    const Color border = m_accountFieldFocused ? hexToColor(m_config.accent())
                                               : Color{100, 100, 120, 200};
    DrawRectangleRounded(r, 0.15f, 8, {25, 25, 32, 235});
    DrawRectangleRoundedLines(r, 0.15f, 8, border);

    const int fontSize = 22;
    if (m_accountNickField.empty() && !m_accountFieldFocused) {
        DrawText(T("click to type"), x + 14, y + (h - 16) / 2, 16, Color{90, 90, 100, 255});
    } else {
        DrawText(m_accountNickField.c_str(), x + 14, y + (h - fontSize) / 2, fontSize, WHITE);
        if (m_accountFieldFocused && ((int)(GetTime() * 2) % 2) == 0) {
            const int caret = x + 14 + MeasureText(m_accountNickField.c_str(), fontSize) + 2;
            DrawRectangle(caret, y + 12, 2, h - 24, WHITE);
        }
    }
}
