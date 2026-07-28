// The multiplayer screens: pick a server, host one, sit in a lobby.
//
// Like the account screen, everything here draws what the net layer reports and
// turns clicks into calls. `NetHost` and `NetSession` are non-blocking and know
// nothing about raylib, so a host that cannot be reached makes this screen say
// so rather than stalling a frame.
//
// WHAT THIS SCREEN IS RESPONSIBLE FOR SAYING OUT LOUD
//
// Two things the player cannot discover for themselves, and cannot undo:
//
//   - Joining a game reveals your IP address to whoever is hosting. There is no
//     relay in the middle by design, so this is not something the game can fix.
//     The join page states it and requires an acknowledgement.
//   - Hosting reveals YOUR address to everyone who joins, unless you put a
//     tunnel in front. The host page says so and names free options.
//
// Neither is buried in a policy nobody reads. See net/PRIVACY.md.

#include "Game.h"
#include "Audio.h"
#include "GameInternals.h"
#include "net/AccountClient.h"
#include "net/BadgeStyle.h"
#include "net/Host.h"
#include "net/HttpClient.h"
#include "net/ServerBook.h"
#include "net/Session.h"
#include "net/TurnRunner.h"
#include "net/SeatBook.h"
#include "net/Tunnel.h"
#include "net/WebSocket.h"
#include "net/TurnStore.h"
#include "net/WorldSync.h"
#include "SaveManager.h"

#include <algorithm>
#include <ctime>

namespace {

Color rgba(uint32_t v) {
    return Color{static_cast<unsigned char>(v >> 24), static_cast<unsigned char>(v >> 16),
                 static_cast<unsigned char>(v >> 8),  static_cast<unsigned char>(v)};
}

/** Badges travel comma-separated on the wire; BadgeStyle wants them apart. */
std::vector<std::string> splitBadges(const std::string& csv) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < csv.size()) {
        const size_t comma = csv.find(',', at);
        const std::string one = csv.substr(at, comma == std::string::npos
                                               ? std::string::npos : comma - at);
        if (!one.empty()) out.push_back(one);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return out;
}

/**
 * The colour a peer's name is drawn in.
 *
 * A badge from an unofficial issuer gets NO colour. A server configured against
 * a third-party account provider can mint whatever badges it likes, so rendering
 * those identically to real ones would turn the badge into a lie anyone can
 * tell. See BadgeStyle.h.
 */
Color peerNameColor(const std::string& badgesCsv, bool officialIssuer) {
    if (!officialIssuer) return Color{205, 210, 220, 255};
    return rgba(badgeNameColor(splitBadges(badgesCsv)));
}

/**
 * Draws a peer's badge tags right-aligned, returning the new right edge.
 * Unofficial badges are marked as such rather than quietly shown.
 */
int drawBadgeTags(const std::string& badgesCsv, bool officialIssuer, int rightX, int y) {
    const std::vector<std::string> badges = splitBadges(badgesCsv);
    for (size_t i = badges.size(); i-- > 0;) {
        std::string tag = badgeTag(badges[i]);
        if (!officialIssuer) tag += " (unverified)";
        rightX -= MeasureText(tag.c_str(), 13);
        DrawText(tag.c_str(), rightX, y, 13,
                 officialIssuer ? rgba(badgeTagColor(badges[i]))
                                : Color{170, 150, 120, 255});
        rightX -= 8;
    }
    return rightX;
}

struct MpButton {
    Rectangle rect;
    bool      hovered = false;
};

MpButton buttonAt(float x, float y, float w, float h, Vector2 mouse) {
    MpButton b{{x, y, w, h}, false};
    b.hovered = CheckCollisionPointRec(mouse, b.rect);
    return b;
}

void drawButton(const MpButton& b, const char* label, int fontSize,
                Color base, Color border, bool enabled = true) {
    const Color bg = !enabled ? Color{30, 30, 34, 200}
                   : b.hovered ? Color{(unsigned char)(base.r + 20), (unsigned char)(base.g + 20),
                                       (unsigned char)(base.b + 20), 240}
                               : base;
    DrawRectangleRounded(b.rect, 0.15f, 8, bg);
    DrawRectangleRoundedLines(b.rect, 0.15f, 8, enabled ? border : Color{70, 70, 80, 180});
    const int tw = MeasureText(label, fontSize);
    DrawText(label, (int)(b.rect.x + (b.rect.width - tw) / 2),
             (int)(b.rect.y + (b.rect.height - fontSize) / 2), fontSize,
             enabled ? (b.hovered ? WHITE : LIGHTGRAY) : Color{110, 110, 120, 255});

    // Every multiplayer button is drawn through here, and the call sites test
    // `click && b.hovered` themselves -- so pressing inside a drawn button is
    // exactly the event they act on, and this is the one place to say so.
    if (b.hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        Audio::get().playSfx(enabled ? "click_heavy" : "deny");
}

/** A text box. `focused` draws the caret; the caller owns the string. */
void drawField(float x, float y, float w, float h, const std::string& text,
               const char* placeholder, bool focused, int fontSize = 18) {
    DrawRectangleRounded({x, y, w, h}, 0.15f, 8, Color{22, 24, 30, 230});
    DrawRectangleRoundedLines({x, y, w, h}, 0.15f, 8,
                              focused ? Color{150, 180, 220, 230} : Color{80, 85, 100, 200});
    const int ty = (int)(y + (h - fontSize) / 2);
    if (text.empty() && !focused) {
        DrawText(placeholder, (int)x + 12, ty, fontSize, Color{110, 115, 130, 255});
        return;
    }
    // Show the tail when it overflows: the end is what someone is typing.
    std::string shown = text;
    while (!shown.empty() && MeasureText(shown.c_str(), fontSize) > (int)w - 26)
        shown.erase(shown.begin());
    DrawText(shown.c_str(), (int)x + 12, ty, fontSize, RAYWHITE);
    if (focused && ((int)(GetTime() * 2) % 2) == 0) {
        DrawText("_", (int)x + 12 + MeasureText(shown.c_str(), fontSize), ty, fontSize, RAYWHITE);
    }
}

/** Wraps text to a width and draws it, returning the y below the last line. */
int wrapText(const std::string& text, int x, int y, int width, int fontSize,
             Color color, bool draw) {
    std::string line;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t space = text.find(' ', at);
        const std::string word = text.substr(at, space == std::string::npos
                                                 ? std::string::npos : space - at);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (MeasureText(candidate.c_str(), fontSize) > width && !line.empty()) {
            if (draw) DrawText(line.c_str(), x, y, fontSize, color);
            y += fontSize + 5;
            line = word;
        } else {
            line = candidate;
        }
        if (space == std::string::npos) break;
        at = space + 1;
    }
    if (!line.empty()) {
        if (draw) DrawText(line.c_str(), x, y, fontSize, color);
        y += fontSize + 5;
    }
    return y;
}

int drawWrapped(const std::string& text, int x, int y, int width, int fontSize,
                Color color) {
    return wrapText(text, x, y, width, fontSize, color, true);
}

/**
 * The height the same text would occupy, without drawing it.
 *
 * A panel drawn at a guessed height and then filled with text that wraps
 * further is a panel with its own contents sitting on the border -- which is
 * exactly what the join screen's warning did.
 */
int measureWrapped(const std::string& text, int width, int fontSize) {
    return wrapText(text, 0, 0, width, fontSize, BLANK, false);
}

/**
 * Seconds as a person would say them.
 *
 * A long-form campaign is measured in days, and "172800 seconds per turn" is
 * a number nobody can check at a glance -- which matters, because a typo here
 * is a game that either rushes everyone or never ends. The largest sensible
 * unit is used, with the remainder when there is one.
 */
std::string durationWords(int seconds) {
    if (seconds <= 0) return "no countdown at all";

    struct Unit { const char* one; const char* many; int size; };
    static const Unit units[] = {
        {"year",   "years",   365 * 24 * 3600},
        {"day",    "days",    24 * 3600},
        {"hour",   "hours",   3600},
        {"minute", "minutes", 60},
        {"second", "seconds", 1},
    };

    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        const Unit& u = units[i];
        if (seconds < u.size) continue;
        const int whole = seconds / u.size;
        const int rest  = seconds % u.size;
        std::string out = std::to_string(whole) + " " + (whole == 1 ? u.one : u.many);
        // One level of remainder only: "2 days 6 hours" is useful, and
        // "2 days 6 hours 3 minutes 9 seconds" is a serial number.
        if (rest > 0 && i + 1 < sizeof(units) / sizeof(units[0])) {
            const Unit& n = units[i + 1];
            const int sub = rest / n.size;
            if (sub > 0)
                out += " " + std::to_string(sub) + " " + (sub == 1 ? n.one : n.many);
        }
        return out;
    }
    return "no countdown at all";
}

const char* phaseWords(NetSession::Phase p) {
    switch (p) {
        case NetSession::Phase::Idle:        return "";
        case NetSession::Phase::Connecting:  return "Connecting to the server...";
        case NetSession::Phase::Fetching:    return "Checking the game...";
        case NetSession::Phase::Minting:     return "Getting permission to join...";
        case NetSession::Phase::Handshaking: return "Joining...";
        case NetSession::Phase::Lobby:       return "In the lobby";
        case NetSession::Phase::InGame:      return "In game";
        case NetSession::Phase::Closed:      return "Disconnected";
    }
    return "";
}

}  // namespace

// --------------------------------------------------------------------- open --

void Game::openMultiplayerMenu() {
    m_currentScreen = SCREEN_MULTIPLAYER;
    m_mpPage = MpPage::Hub;
    m_mpFocus = -1;
    m_mpNote.clear();
    m_mpNoteTimer = 0.0f;
    m_mpSelected = -1;
    m_mpIpWarningAccepted = false;
    m_mpPlayersTab = false;

    tunnelSetToolsDir(mpToolsDir());

    if (!m_serverBook) {
        m_serverBook = new ServerBook();
        m_serverBook->load(m_dataDir + "/servers.json");
    }
    m_serverBook->sort();

    if (m_mpNameField.empty()) {
        const AccountInfo info = AccountClient::get().account();
        m_mpNameField = info.nickname.empty() ? "OpenDoctrines game"
                                              : info.nickname + "'s game";
    }
    if (m_mpPortField.empty()) m_mpPortField = "27015";
    if (m_mpTurnField.empty()) m_mpTurnField = "0";
}

int Game::mpTurnSeconds() const {
    return std::clamp(atoi(m_mpTurnField.c_str()), 0, 65535);
}

std::string Game::mpToolsDir() const { return m_dataDir + "tools"; }

void Game::mpRefreshSaves() {
    m_mpSavePaths.clear();
    m_mpSaveNames.clear();
    const std::string dir = m_dataDir + "saves/multiplayer/";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const std::string path = e.path().string();
        if (path.size() < 5 || path.compare(path.size() - 5, 5, ".odsv") != 0) continue;
        m_mpSavePaths.push_back(path);
        m_mpSaveNames.push_back(e.path().stem().string());
    }
    // Newest first: the campaign somebody is actually running is the one they
    // touched last, and making them scroll to it is the wrong default.
    std::vector<size_t> order(m_mpSavePaths.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        std::error_code e1, e2;
        const auto ta = std::filesystem::last_write_time(m_mpSavePaths[a], e1);
        const auto tb = std::filesystem::last_write_time(m_mpSavePaths[b], e2);
        return ta > tb;
    });
    std::vector<std::string> paths, names;
    for (size_t i : order) { paths.push_back(m_mpSavePaths[i]); names.push_back(m_mpSaveNames[i]); }
    m_mpSavePaths.swap(paths);
    m_mpSaveNames.swap(names);
    // Not std::clamp: with nothing saved the upper bound is -1, and clamping to
    // a range that ends before it starts is undefined -- it happened to return
    // -1 here and could as well have returned 0, which indexes an empty vector.
    // An empty list has no valid index, and -1 is what the readers check for.
    m_mpSaveIndex = m_mpSavePaths.empty()
                        ? -1
                        : std::min(std::max(m_mpSaveIndex, 0),
                                   (int)m_mpSavePaths.size() - 1);
}

void Game::mpSaveSeats() {
    // Only the host holds the authoritative roster, and only it owns the save.
    if (!m_netHost || m_currentSavePath.empty()) return;

    SeatBook book;
    book.mapId = m_mpMapId;
    book.turnNumber = (uint32_t)std::max(0, m_turnNumber);
    for (const NetPeer& p : m_netHost->lobby().roster()) {
        if (p.psid.empty() || p.countryId == 0) continue;
        // Written whether or not they are connected right now: somebody who
        // closed the game an hour ago is exactly who this is for.
        book.seats.push_back({p.psid, p.name, p.countryId});
    }
    book.save(m_currentSavePath);
}

TurnStoreKind Game::mpStoreKind() const {
    // Ordered so the default is first: the session's own storage needs nothing
    // enabled and no third party, which is the right answer for almost everyone.
    switch (m_mpStore) {
        case 1:  return TurnStoreKind::JsonBlob;
        case 2:  return TurnStoreKind::R2;
        case 3:  return TurnStoreKind::Manual;
        default: return TurnStoreKind::DurableObject;
    }
}

void Game::mpNote(const std::string& text, bool error) {
    m_mpNote = text;
    m_mpNoteError = error;
    m_mpNoteTimer = 6.0f;
}

// ------------------------------------------------------------------ actions --

void Game::mpStartHosting() {
    AccountClient& account = AccountClient::get();
    if (account.status() != AccountClient::Status::SignedIn) {
        mpNote("Please log in to proceed", true);
        return;
    }

    // A server credential is what makes per-player pseudonyms on this server
    // stable. It is issued once and kept; registering again would make every
    // returning player look like a stranger, so it is cached in the config.
    if (m_config.serverCredential.empty()) {
        if (m_mpRegisterState.load() == MpRegister::Working) return;

        mpNote("Registering this server with the account service...");
        m_mpHostAfterRegister = true;
        m_mpRegisterState.store(MpRegister::Working);
        if (m_mpRegisterThread.joinable()) m_mpRegisterThread.join();

        const std::string issuer = account.issuer();
        const std::string token = account.sessionToken();
        m_mpRegisterThread = std::thread([this, issuer, token] {
            HttpRequest req;
            req.method = "POST";
            req.url = issuer + "/server/register";
            req.bearer = token;
            req.allowInsecure = issuer.rfind("http://localhost", 0) == 0 ||
                                issuer.rfind("http://127.0.0.1", 0) == 0;
            const HttpResponse res = httpRequest(req);
            const std::string credential =
                httpJsonString(res.body, "serverCredential", 4096);
            {
                std::lock_guard<std::mutex> lock(m_mpRegisterMutex);
                if (res.ok() && !credential.empty()) {
                    m_mpRegisterResult = credential;
                } else {
                    const std::string why = !res.error.empty() ? res.error
                        : httpJsonString(res.body, "message", 512);
                    m_mpRegisterError = why.empty()
                        ? "Could not register this server with the account service."
                        : why;
                }
            }
            m_mpRegisterState.store(m_mpRegisterResult.empty() ? MpRegister::Failed
                                                               : MpRegister::Done);
        });
        return;     // picked up in updateMultiplayerMenu()
    }

    mpOpenHost();
}

void Game::mpOpenHost() {
    AccountClient& account = AccountClient::get();
    if (!m_netHost) m_netHost = new NetHost();

    NetHost::Config cfg;
    cfg.issuer = account.issuer();
    cfg.token = account.sessionToken();
    cfg.serverCredential = m_config.serverCredential;
    cfg.sessionName = m_mpNameField.empty() ? "OpenDoctrines game" : m_mpNameField;
    {
        // Who is hosting, from the signed-in account. Without this the host's
        // own row is blank and every joiner is told the server has no name.
        const AccountInfo me = account.account();
        cfg.hostName = me.nickname;
        std::string badges;
        for (const std::string& b : me.badges) {
            if (!badges.empty()) badges += ",";
            badges += b;
        }
        cfg.hostBadges = badges;
    }
    cfg.gameVersion = OD_VERSION_STRING;
    cfg.listed = m_mpListed;
    cfg.showBadges = true;
    cfg.turnSeconds = (uint16_t)mpTurnSeconds();
    cfg.anonymous = m_mpAnonymous;
    cfg.dedicated = m_mpDedicated;
    cfg.store = mpStoreKind();
    cfg.lobby.assignment = m_mpAssignment == 0 ? NetAssignment::HostAssigns
                                               : NetAssignment::PlayersPick;
    cfg.lobby.lateJoin = m_mpLateJoin == 0 ? NetLateJoin::Refuse : NetLateJoin::Spectate;
    cfg.lobby.absent   = m_mpAbsent == 0 ? NetAbsent::Ai : NetAbsent::Idle;
    cfg.bindAll = m_mpBindAll;
    cfg.port = (uint16_t)std::clamp(atoi(m_mpPortField.c_str()), 0, 65535);
    cfg.lobby.maxPlayers = (uint8_t)m_mpMaxPlayers;

    if (!m_netHost->open(cfg)) {
        mpNote(m_netHost->error().empty() ? "Could not start hosting."
                                          : m_netHost->error(), true);
        return;
    }
    m_netHost->setMapName(m_mpMapId);
    m_mpPage = MpPage::Lobby;

    // A tunnel, if the host wants one and has one. Started here rather than
    // before the listener so it publishes a port that is already accepting.
    tunnelSetToolsDir(mpToolsDir());
    const std::vector<TunnelProvider> providers = tunnelProvidersAvailable();
    if (m_mpUseTunnel && !providers.empty() && !m_mpBindAll) {
        const TunnelProvider p =
            providers[(size_t)std::clamp(m_mpTunnelChoice, 0, (int)providers.size() - 1)];

        // Being installed is not being usable. Starting something that cannot
        // get an address wastes the host half a minute and then fails, so it is
        // only started when it was chosen on purpose.
        if (!tunnelProviderWorksUnattended(p) && m_mpTunnelChoice == 0) {
            mpNote("No tunnel was started: nothing installed here can open one on "
                   "its own. Install cloudflared from the host screen, or forward "
                   "a port.", true);
        } else {
        if (!m_mpTunnel) m_mpTunnel = new Tunnel();
        std::string why;
        if (!m_mpTunnel->start(p, m_netHost->listenPort(), why)) {
            // Not fatal: the game is still hosted, just not published. A host
            // on a LAN or with a forwarded port needs no tunnel at all.
            mpNote(why, true);
        } else {
            mpNote(std::string("Opening a tunnel via ") + tunnelProviderName(p) + "...");
        }
        }
    }

    // Load the world NOW rather than when the game starts: the lobby needs real
    // countries to offer, and starting is then instant instead of every player
    // waiting on the host's disk.
    if (m_mpResume) {
        // Continuing a campaign. The world comes off disk complete -- turns
        // already replayed by the ordinary save loader -- so there is no map to
        // resolve and nothing to generate. Seats are restored once it lands, in
        // mpOnWorldLoaded, because the lobby has to exist first.
        if (m_mpSaveIndex < 0 || m_mpSaveIndex >= (int)m_mpSavePaths.size()) {
            mpNote("Pick a saved game to continue first.", true);
            return;
        }
        const std::string savePath = m_mpSavePaths[(size_t)m_mpSaveIndex];
        SeatBook book;
        if (SeatBook::load(savePath, book) && !book.mapId.empty()) {
            // Joiners are told a map NAME and find it themselves, so a resumed
            // game must announce the same one the original did.
            m_mpMapId = book.mapId;
            m_netHost->setMapName(m_mpMapId);
        }
        m_mpLoad = MpLoad::HostOpen;
        mpNote("Continuing " + m_mpSaveNames[(size_t)m_mpSaveIndex] + "...");
        startLoadingSave(savePath);
        m_currentScreen = SCREEN_LOADING;
        return;
    }

    std::string path, name;
    if (!mpResolveMap(m_mpMapId, path, name)) {
        mpNote("Could not find that map on this machine.", true);
        return;
    }
    m_mpLoad = MpLoad::HostOpen;
    mpNote("Loading " + name + "...");
    // startNewGameWithName, NOT startNewGame: the latter loads synchronously
    // and ends by putting the player straight onto the map, which for a host is
    // exactly wrong -- they should land back in the lobby waiting for people.
    // This one loads a step per frame and finishes in LOAD_FINALIZE, where
    // mpOnWorldLoaded() decides where to go. It also creates the auto-save, and
    // the host needs that: the snapshot and every turn delta are read from it.
    startNewGameWithName(path, m_mpNameField.empty() ? name : m_mpNameField);
}

void Game::mpBeginJoin(const std::string& address, const std::string& code) {
    AccountClient& account = AccountClient::get();
    if (account.status() != AccountClient::Status::SignedIn) {
        mpNote("Please log in to proceed", true);
        return;
    }
    if (address.empty()) { mpNote("Enter the server's address.", true); return; }
    if (!ServerBook::validCode(code)) {
        mpNote("That invite code does not look right.", true);
        return;
    }

    if (!m_netSession) m_netSession = new NetSession();

    // Every plausible route, in order. A host is often reachable by one address
    // from outside and a different one from the same network, and the player
    // has no way to know which applies to them.
    std::vector<std::string> candidates{address};
    if (address.find(':') == std::string::npos) {
        // No port given: try the default before giving up on the address.
        candidates.push_back(address + ":27015");
    }

    if (!m_netSession->join(candidates, account.issuer(), code,
                            account.sessionToken(), OD_VERSION_STRING, "")) {
        mpNote(m_netSession->error().empty() ? "Could not start joining."
                                             : m_netSession->error(), true);
        return;
    }
    m_mpPage = MpPage::Lobby;
    mpNote("Connecting...");
}

void Game::mpLeave() {
    // The tunnel goes with the game that opened it: an orphan would keep a port
    // published after there is nothing behind it.
    if (m_mpTunnel) { m_mpTunnel->stop(); delete m_mpTunnel; m_mpTunnel = nullptr; }
    if (m_netSession) { m_netSession->leave(); delete m_netSession; m_netSession = nullptr; }
    // Before the host goes: seats claimed since the last turn resolved would
    // otherwise be lost, and re-picking is exactly what this is meant to spare.
    if (m_netHost)    { mpSaveSeats(); m_netHost->close(); delete m_netHost; m_netHost = nullptr; }
    m_mpPage = MpPage::Hub;
    m_mpPlayersTab = false;
    m_mpIpWarningAccepted = false;
}

void Game::mpShutdown() {
    // Deleting these in Game.cpp would be deleting through a forward
    // declaration -- the destructors would not run at all. They are deleted
    // here, where every type is complete.
    mpLeave();
    if (m_mpRegisterThread.joinable()) m_mpRegisterThread.join();
    if (m_serverBook) { delete m_serverBook; m_serverBook = nullptr; }
    if (m_mpTurns)    { delete m_mpTurns;    m_mpTurns = nullptr; }
    if (m_mpTunnel)   { delete m_mpTunnel;   m_mpTunnel = nullptr; }
    if (m_mpTunnelInstaller) {
        m_mpTunnelInstaller->shutdown();
        delete m_mpTunnelInstaller;
        m_mpTunnelInstaller = nullptr;
    }
    if (m_mpReachProbe) { m_mpReachProbe->close(); delete m_mpReachProbe;
                          m_mpReachProbe = nullptr; }
}

void Game::mpDrainEvents() {
    if (m_mpTunnel) {
        const Tunnel::State before = m_mpTunnel->state();
        m_mpTunnel->update();
        const Tunnel::State now = m_mpTunnel->state();
        if (before != now && now == Tunnel::State::Up) {
            mpNote("Tunnel open. Players can use the address shown above.");
        } else if (before != now && now == Tunnel::State::Failed) {
            mpNote(m_mpTunnel->error().empty()
                       ? "The tunnel stopped; players may not be able to reach you."
                       : m_mpTunnel->error(), true);
        }
    }
    if (m_netHost) {
        m_netHost->update();
        NetHostEvent e;
        while (m_netHost->nextEvent(e)) {
            switch (e.kind) {
                case NetHostEvent::Kind::Opened:
                    mpNote("Game open. Share the code: " + e.text);
                    break;
                case NetHostEvent::Kind::PeerJoined:
                    mpNote(e.text + " joined");
                    break;
                case NetHostEvent::Kind::PeerLeft:
                    mpNote("A player disconnected");
                    break;
                case NetHostEvent::Kind::OrdersReceived:
                    if (e.text == "malformed") {
                        // Discarded whole, and recorded as unreadable -- a
                        // different state from "nothing arrived", and it earns
                        // a different explanation when the turn resolves.
                        mpNote("A player's orders could not be read; the AI will "
                               "play that turn for them.", true);
                    }
                    break;
                case NetHostEvent::Kind::Failed:
                    mpNote(e.text, true);
                    break;
                case NetHostEvent::Kind::Closed:
                    mpNote("The game closed.", true);
                    break;
                default:
                    break;
            }
        }
    }

    if (m_netSession) {
        m_netSession->update();
        NetSessionEvent e;
        while (m_netSession->nextEvent(e)) {
            switch (e.kind) {
                case NetSessionEvent::Kind::Welcomed: {
                    const NetWelcome& w = m_netSession->welcome();
                    mpNote("Joined " + w.sessionName);
                    // Remember where this was, so the next evening is one click.
                    if (m_serverBook) {
                        ServerEntry entry;
                        entry.name = w.sessionName.empty() ? m_mpAddressField : w.sessionName;
                        entry.issuer = AccountClient::get().issuer();
                        entry.code = m_mpCodeField;
                        entry.lastJoined = (long long)time(nullptr);
                        entry.lastHostName = w.host.name;
                        m_serverBook->addOrUpdate(entry);
                        m_serverBook->save();
                    }
                    break;
                }
                case NetSessionEvent::Kind::Rejected:
                    mpNote(netRejectAdvice(m_netSession->rejectReason(),
                                           m_netSession->error()), true);
                    break;
                case NetSessionEvent::Kind::Snapshot:
                    mpApplySnapshot(e.payload);
                    break;
                case NetSessionEvent::Kind::Delta:
                    mpApplyDelta(e.turnNumber, e.payload);
                    break;
                case NetSessionEvent::Kind::TurnBegan:
                    m_mpWaitingForTurn = false;
                    mpNote("Turn " + std::to_string(e.turnNumber) + " has begun.");
                    break;
                case NetSessionEvent::Kind::Notice:
                    // "The AI played X because ..." -- said out loud, always.
                    if (!e.notice.text.empty()) mpNote(e.notice.text);
                    break;
                case NetSessionEvent::Kind::Disconnected:
                    mpNote(m_netSession->error().empty() ? "Disconnected."
                                                         : m_netSession->error(), true);
                    break;
                default:
                    break;
            }
        }
    }
}

// ------------------------------------------------------------------- update --

void Game::updateMultiplayerMenu() {
    const float dt = GetFrameTime();
    if (m_mpNoteTimer > 0.0f) m_mpNoteTimer -= dt;

    // Collect a finished server registration, and carry on where the click
    // left off.
    const MpRegister reg = m_mpRegisterState.load();
    if (reg == MpRegister::Done || reg == MpRegister::Failed) {
        if (m_mpRegisterThread.joinable()) m_mpRegisterThread.join();
        m_mpRegisterState.store(MpRegister::Idle);

        std::string credential, error;
        {
            std::lock_guard<std::mutex> lock(m_mpRegisterMutex);
            credential.swap(m_mpRegisterResult);
            error.swap(m_mpRegisterError);
        }
        if (!credential.empty()) {
            m_config.serverCredential = credential;
            m_config.save(m_dataDir + "/config.json");
            if (m_mpHostAfterRegister) mpOpenHost();
        } else {
            mpNote(error.empty() ? "Could not register this server." : error, true);
        }
        m_mpHostAfterRegister = false;
    }

    mpDrainEvents();
    mpUpdateReachTest();

    // Typing goes to whichever box has focus. Fields are indexed so one
    // handler serves every page.
    if (m_mpFocus >= 0) {
        std::string* target = nullptr;
        size_t limit = 128;
        switch (m_mpFocus) {
            case 0: target = &m_mpAddressField; limit = 128; break;
            case 1: target = &m_mpCodeField;    limit = 24;  break;
            case 2: target = &m_mpNameField;    limit = 48;  break;
            case 3: target = &m_mpPortField;    limit = 5;   break;
            case 4: target = &m_mpTurnField;    limit = 5;   break;
            case 5: target = &m_mpMapSearch;    limit = 32;  break;
            default: break;
        }
        if (target) {
            int c = GetCharPressed();
            while (c > 0) {
                // Every character the field takes. Jittered, because a
                // typed word is a run of distinct taps, not one tap looped.
                Audio::get().playSfx("key_type", 0.12f);
                const bool digitsOnly = (m_mpFocus == 3 || m_mpFocus == 4);
                const bool ok = c >= 32 && c < 127 &&
                                (!digitsOnly || (c >= '0' && c <= '9'));
                if (ok && target->size() < limit) *target += (char)c;
                c = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && !target->empty()) target->pop_back();
            if (IsKeyPressed(KEY_TAB)) m_mpFocus = (m_mpFocus + 1) % 6;
        }
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (m_mpPage == MpPage::Hub) {
            m_currentScreen = SCREEN_MENU;
        } else if (m_mpPage == MpPage::Lobby) {
            mpLeave();
        } else {
            m_mpPage = MpPage::Hub;
            m_mpFocus = -1;
        }
    }
}

// --------------------------------------------------------------------- draw --

void Game::drawMultiplayerMenu() {
    drawMenuBackground(true);

    const Vector2 mouse = GetMousePosition();
    const bool click = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    const char* title = m_mpPage == MpPage::Hub       ? "Multiplayer"
                      : m_mpPage == MpPage::Join      ? "Join a game"
                      : m_mpPage == MpPage::HostSetup ? "Host a game"
                                                      : "Lobby";
    DrawText(title, m_screenW / 2 - MeasureText(title, 40) / 2, 60, 40, RAYWHITE);

    switch (m_mpPage) {
        case MpPage::Hub:       drawMpHub(mouse, click); break;
        case MpPage::Join:      drawMpJoin(mouse, click); break;
        case MpPage::HostSetup: drawMpHostSetup(mouse, click); break;
        case MpPage::Lobby:     drawMpLobby(mouse, click); break;
    }

    if (m_mpNoteTimer > 0.0f && !m_mpNote.empty()) {
        const Color c = m_mpNoteError ? Color{230, 150, 130, 255}
                                      : Color{150, 210, 170, 255};
        const int w = MeasureText(m_mpNote.c_str(), 18);
        DrawText(m_mpNote.c_str(), m_screenW / 2 - w / 2, m_screenH - 96, 18, c);
    }

    const char* hint = m_mpPage == MpPage::Hub ? "Esc  back to menu" : "Esc  back";
    DrawText(hint, m_screenW / 2 - MeasureText(hint, 15) / 2, m_screenH - 42, 15,
             Color{130, 135, 150, 255});
}

// ---------------------------------------------------------------------- hub --

void Game::drawMpHub(Vector2 mouse, bool click) {
    const int centerX = m_screenW / 2;
    const int listW = 560;
    int y = 140;

    DrawText("Your servers", centerX - listW / 2, y, 20, Color{170, 180, 200, 255});
    y += 32;

    const auto& entries = m_serverBook ? m_serverBook->entries()
                                       : *(new std::vector<ServerEntry>());
    if (entries.empty()) {
        DrawText("No servers yet. Join one by code, or host your own.",
                 centerX - listW / 2, y, 17, Color{130, 135, 150, 255});
        y += 40;
    } else {
        for (size_t i = 0; i < entries.size() && i < 6; i++) {
            const ServerEntry& e = entries[i];
            const MpButton row = buttonAt((float)(centerX - listW / 2), (float)y,
                                          (float)listW, 46.0f, mouse);
            const bool selected = (int)i == m_mpSelected;
            DrawRectangleRounded(row.rect, 0.12f, 8,
                selected ? Color{40, 52, 68, 230}
                         : row.hovered ? Color{32, 36, 46, 220} : Color{24, 26, 34, 200});
            DrawRectangleRoundedLines(row.rect, 0.12f, 8,
                selected ? Color{120, 160, 200, 220} : Color{70, 75, 90, 180});
            DrawText(e.name.c_str(), (int)row.rect.x + 14, (int)row.rect.y + 6, 19, RAYWHITE);

            std::string sub = e.code.empty() ? "no code yet" : "code " + e.code;
            if (!e.lastHostName.empty()) sub += "  ·  hosted by " + e.lastHostName;
            DrawText(sub.c_str(), (int)row.rect.x + 14, (int)row.rect.y + 27, 14,
                     Color{140, 145, 160, 255});

            if (click && row.hovered) {
                m_mpSelected = (int)i;
                m_mpCodeField = e.code;
                m_mpAddressField.clear();
                m_mpPage = MpPage::Join;
                m_mpIpWarningAccepted = false;
                m_mpFocus = 0;
            }
            y += 52;
        }
    }

    y += 20;
    const int btnW = 260, btnH = 52, gap = 16;
    const MpButton join = buttonAt((float)(centerX - btnW - gap / 2), (float)y,
                                   (float)btnW, (float)btnH, mouse);
    drawButton(join, "Join by code", 20, Color{40, 52, 68, 230}, Color{120, 150, 190, 210});
    if (click && join.hovered) {
        m_mpPage = MpPage::Join;
        m_mpIpWarningAccepted = false;
        m_mpFocus = 0;
    }

    const MpButton host = buttonAt((float)(centerX + gap / 2), (float)y,
                                   (float)btnW, (float)btnH, mouse);
    drawButton(host, "Host a game", 20, Color{44, 62, 50, 230}, Color{130, 190, 140, 210});
    if (click && host.hovered) {
        m_mpPage = MpPage::HostSetup;
        m_mpFocus = 2;
    }
}

// --------------------------------------------------------------------- join --

void Game::drawMpJoin(Vector2 mouse, bool click) {
    const int centerX = m_screenW / 2;
    const int fieldW = 460, fieldH = 46;
    int y = 120;

    DrawText("Server address", centerX - fieldW / 2, y, 16, Color{160, 170, 190, 255});
    y += 22;
    const Rectangle addr{(float)(centerX - fieldW / 2), (float)y, (float)fieldW, (float)fieldH};
    drawField(addr.x, addr.y, addr.width, addr.height, m_mpAddressField,
              "example.trycloudflare.com  or  1.2.3.4:27015", m_mpFocus == 0);
    if (click && CheckCollisionPointRec(mouse, addr)) m_mpFocus = 0;
    y += fieldH + 16;

    DrawText("Invite code", centerX - fieldW / 2, y, 16, Color{160, 170, 190, 255});
    y += 22;
    const Rectangle code{(float)(centerX - fieldW / 2), (float)y, (float)fieldW, (float)fieldH};
    drawField(code.x, code.y, code.width, code.height, m_mpCodeField,
              "ABCD-EFGH", m_mpFocus == 1);
    if (click && CheckCollisionPointRec(mouse, code)) m_mpFocus = 1;
    y += fieldH + 26;

    // The one thing about joining that cannot be taken back afterwards. It is
    // stated here, before connecting, rather than left to the privacy policy.
    const std::string warnText =
        "Your game connects straight to that server -- there is no middleman by "
        "design, so we cannot hide it. It roughly indicates where you are and who "
        "your internet provider is. Use a VPN if you would rather the host did "
        "not see it.";
    // Measured, not guessed: the box is sized to what it will hold.
    const float boxH = 12.0f + 26.0f +
                       (float)measureWrapped(warnText, fieldW, 15) + 10.0f;
    const Rectangle box{(float)(centerX - fieldW / 2 - 10), (float)y - 8,
                        (float)fieldW + 20, boxH};
    DrawRectangleRounded(box, 0.08f, 8, Color{40, 34, 26, 210});
    DrawRectangleRoundedLines(box, 0.08f, 8, Color{170, 140, 90, 200});

    int ty = y + 2;
    DrawText("The host will see your IP address", centerX - fieldW / 2, ty, 18,
             Color{235, 200, 140, 255});
    ty += 26;
    drawWrapped(warnText, centerX - fieldW / 2, ty, fieldW, 15,
                Color{200, 190, 175, 255});

    // Below the panel, never on its edge.
    const MpButton ack = buttonAt((float)(centerX - fieldW / 2),
                                  box.y + box.height + 12.0f, 26.0f, 26.0f, mouse);
    DrawRectangleRounded(ack.rect, 0.2f, 6,
                         m_mpIpWarningAccepted ? Color{80, 130, 90, 240} : Color{30, 32, 40, 230});
    DrawRectangleRoundedLines(ack.rect, 0.2f, 6, Color{150, 160, 175, 200});
    if (m_mpIpWarningAccepted) DrawText("x", (int)ack.rect.x + 9, (int)ack.rect.y + 4, 18, WHITE);
    DrawText("I understand", (int)ack.rect.x + 36, (int)ack.rect.y + 5, 16,
             Color{200, 205, 215, 255});
    if (click && ack.hovered) m_mpIpWarningAccepted = !m_mpIpWarningAccepted;

    y = (int)ack.rect.y + 56;
    const int btnW = 220, btnH = 50, gap = 16;
    const MpButton back = buttonAt((float)(centerX - btnW - gap / 2), (float)y,
                                   (float)btnW, (float)btnH, mouse);
    drawButton(back, "Back", 19, Color{34, 36, 44, 220}, Color{90, 95, 110, 190});
    if (click && back.hovered) { m_mpPage = MpPage::Hub; m_mpFocus = -1; }

    const bool ready = m_mpIpWarningAccepted && !m_mpAddressField.empty() &&
                       !m_mpCodeField.empty();
    const MpButton go = buttonAt((float)(centerX + gap / 2), (float)y,
                                 (float)btnW, (float)btnH, mouse);
    drawButton(go, "Join", 19, Color{40, 60, 48, 230}, Color{130, 190, 140, 210}, ready);
    if (click && go.hovered && ready) mpBeginJoin(m_mpAddressField, m_mpCodeField);
}

// ---------------------------------------------------------------- host setup --

void Game::drawMpHostSetup(Vector2 mouse, bool click) {
    const int centerX = m_screenW / 2;
    const int fieldW = 460, fieldH = 42;
    int y = 108;

    // Three pages, because the whole set does not fit on one screen and a host
    // should not have to scroll past rules they are not changing to reach the
    // button. Basics is what everyone touches; the rest is for fiddling.
    const char* tabs[3] = {"Game", "Rules", "Turns"};
    for (int i = 0; i < 3; i++) {
        const MpButton t = buttonAt((float)(centerX - fieldW / 2 + i * (fieldW / 3)),
                                    (float)y, (float)(fieldW / 3 - 4), 32.0f, mouse);
        drawButton(t, tabs[i], 16,
                   m_mpSetupTab == i ? Color{44, 54, 70, 235} : Color{26, 28, 36, 210},
                   Color{100, 120, 150, 200});
        if (click && t.hovered) { m_mpSetupTab = i; m_mpFocus = -1; }
    }
    y += 44;

    if (m_mpSetupTab == 0) {
        // ---- what is being played, and where -----------------------------
        // New world, or carry on with one. A campaign is the reason turn
        // deltas exist; until now hosting could only ever start from turn 0.
        {
            const MpButton nw = buttonAt((float)(centerX - fieldW / 2), (float)y,
                                         (float)(fieldW / 2 - 4), 30.0f, mouse);
            drawButton(nw, "New world", 15,
                       m_mpResume ? Color{28, 32, 40, 220} : Color{40, 60, 46, 235},
                       m_mpResume ? Color{80, 88, 104, 190} : Color{110, 170, 130, 210});
            if (click && nw.hovered) m_mpResume = false;

            const MpButton cont = buttonAt((float)(centerX + 4), (float)y,
                                           (float)(fieldW / 2 - 4), 30.0f, mouse);
            drawButton(cont, "Continue a game", 15,
                       m_mpResume ? Color{40, 60, 46, 235} : Color{28, 32, 40, 220},
                       m_mpResume ? Color{110, 170, 130, 210} : Color{80, 88, 104, 190});
            if (click && cont.hovered) { m_mpResume = true; mpRefreshSaves(); }
            y += 38;
        }

        if (m_mpResume) {
            DrawText("Saved game", centerX - fieldW / 2, y, 15,
                     Color{160, 170, 190, 255});
            y += 19;
            if (m_mpSavePaths.empty()) {
                y = drawWrapped("No multiplayer games have been saved yet. Start a "
                                "new world -- it is saved from the first turn, and "
                                "will appear here next time.",
                                centerX - fieldW / 2, y, fieldW, 13,
                                Color{200, 190, 175, 255}) + 6;
            } else {
                m_mpSaveIndex = std::clamp(m_mpSaveIndex, 0,
                                           (int)m_mpSavePaths.size() - 1);
                const int total = (int)m_mpSavePaths.size();
                const int rowH = 32, visible = std::min(total, 5);
                m_mpSaveScroll = std::clamp(m_mpSaveScroll, 0,
                                            std::max(0, total - visible));
                const Rectangle listBox{(float)(centerX - fieldW / 2), (float)y,
                                        (float)fieldW, (float)(rowH * visible + 8)};
                DrawRectangleRounded(listBox, 0.06f, 8, Color{18, 20, 26, 240});
                DrawRectangleRoundedLines(listBox, 0.06f, 8, Color{90, 100, 120, 200});
                if (CheckCollisionPointRec(mouse, listBox)) {
                    const float w = GetMouseWheelMove();
                    if (w != 0.0f) m_mpSaveScroll -= (int)w;
                    m_mpSaveScroll = std::clamp(m_mpSaveScroll, 0,
                                                std::max(0, total - visible));
                }

                for (int i = 0; i < visible && m_mpSaveScroll + i < total; i++) {
                    const int idx = m_mpSaveScroll + i;
                    const Rectangle row{listBox.x + 4, listBox.y + 4 + i * (float)rowH,
                                        listBox.width - 8, (float)rowH};
                    const bool chosen = idx == m_mpSaveIndex;
                    const bool hovered = CheckCollisionPointRec(mouse, row);
                    if (chosen)
                        DrawRectangleRounded(row, 0.2f, 6, Color{40, 60, 46, 220});
                    else if (hovered)
                        DrawRectangleRounded(row, 0.2f, 6, Color{34, 38, 48, 200});
                    DrawText(m_mpSaveNames[(size_t)idx].c_str(), (int)row.x + 8,
                             (int)row.y + 8, 15,
                             chosen ? Color{170, 220, 185, 255}
                                    : Color{200, 205, 215, 255});

                    // Delete asks twice, and the second press is a different
                    // word in a different colour -- deleting somebody's
                    // campaign on a mis-click is not recoverable.
                    const bool arming = m_mpSaveDeleting == idx;
                    const float dw = arming ? 74.0f : 54.0f;
                    const MpButton del = buttonAt(row.x + row.width - dw - 6,
                                                  row.y + 4, dw, 24.0f, mouse);
                    drawButton(del, arming ? "Really?" : "Delete", 13,
                               arming ? Color{78, 34, 34, 235} : Color{44, 34, 34, 220},
                               arming ? Color{215, 120, 120, 220}
                                      : Color{130, 100, 100, 190});
                    if (click && del.hovered) {
                        if (arming) {
                            std::error_code ec;
                            std::filesystem::remove(m_mpSavePaths[(size_t)idx], ec);
                            // The seating sidecar goes with it; leaving it
                            // behind would hold countries for a world that no
                            // longer exists.
                            std::filesystem::remove(
                                SeatBook::pathFor(m_mpSavePaths[(size_t)idx]), ec);
                            m_mpSaveDeleting = -1;
                            mpRefreshSaves();
                            m_mpSaveScroll = 0;
                            // NOT index 0: that was a lie whenever the list is
                            // now empty, and everything below indexes off it.
                            // mpRefreshSaves has already put it somewhere real.
                        } else {
                            m_mpSaveDeleting = idx;
                        }
                        break;
                    }
                    if (click && hovered && !del.hovered) {
                        m_mpSaveIndex = idx;
                        m_mpSaveDeleting = -1;
                    }
                }
                y += (int)listBox.height + 6;
                if (total > visible) {
                    const std::string more =
                        std::to_string(m_mpSaveScroll + 1) + "-" +
                        std::to_string(std::min(total, m_mpSaveScroll + visible)) +
                        " of " + std::to_string(total) + "   (scroll)";
                    DrawText(more.c_str(), centerX - fieldW / 2, y, 12,
                             Color{130, 140, 155, 255});
                    y += 16;
                }

                // What continuing actually restores, said before it happens.
                // Re-checked rather than assumed: a Delete above can empty the
                // list halfway through this very frame, and the index that was
                // valid at the top of the block then points at nothing.
                SeatBook book;
                const bool haveSave = m_mpSaveIndex >= 0 &&
                                      m_mpSaveIndex < (int)m_mpSavePaths.size();
                if (haveSave &&
                    SeatBook::load(m_mpSavePaths[(size_t)m_mpSaveIndex], book)) {
                    std::string sum = "Turn " + std::to_string(book.turnNumber);
                    if (!book.seats.empty()) {
                        sum += ".  " + std::to_string(book.seats.size()) +
                               " player(s) keep their country when they rejoin: ";
                        for (size_t i = 0; i < book.seats.size() && i < 6; i++)
                            sum += (i ? ", " : "") +
                                   (book.seats[i].name.empty() ? std::string("someone")
                                                               : book.seats[i].name);
                        if (book.seats.size() > 6) sum += ", ...";
                    } else {
                        sum += ".  Nobody's seat was recorded, so everyone picks "
                               "a country again.";
                    }
                    y = drawWrapped(sum, centerX - fieldW / 2, y, fieldW, 13,
                                    Color{150, 190, 170, 255}) + 6;
                } else if (haveSave) {
                    y = drawWrapped("No seating was recorded beside this save, so "
                                    "everyone picks a country again. The world "
                                    "itself is unaffected.",
                                    centerX - fieldW / 2, y, fieldW, 13,
                                    Color{175, 180, 195, 255}) + 6;
                }
            }
        } else {
            // Braced now. It used to read `} else` with no braces, so the else
            // swallowed only the loadMapEntries() call and everything below it
            // ran unconditionally -- the map picker drew underneath the save
            // list, and rewrote m_mpMapId every frame while resuming. A saved
            // campaign carries its own map; there is nothing here to choose.
            if (m_mapEntries.empty()) loadMapEntries();
            DrawText("Map", centerX - fieldW / 2, y, 15, Color{160, 170, 190, 255});
            y += 19;
            if (m_mapEntries.empty()) {
                y = drawWrapped("No maps are installed.", centerX - fieldW / 2, y,
                                fieldW, 13, Color{200, 190, 175, 255}) + 6;
            } else {
                // A search box rather than a cycle button: with the standard maps
                // and a folder of custom ones, "press until it comes round again"
                // stops being a way to find anything.
                const Rectangle sb{(float)(centerX - fieldW / 2), (float)y,
                                   (float)fieldW, 32.0f};
                drawField(sb.x, sb.y, sb.width, sb.height, m_mpMapSearch,
                          "search maps...", m_mpFocus == 5);
                if (click && CheckCollisionPointRec(mouse, sb)) m_mpFocus = 5;
                y += 38;

                // Matched on name AND id, because a map people call by its folder
                // name is a map they will type the folder name for.
                auto lower = [](std::string t) {
                    for (char& c : t) c = (char)std::tolower((unsigned char)c);
                    return t;
                };
                const std::string needle = lower(m_mpMapSearch);
                std::vector<int> shown;
                for (int i = 0; i < (int)m_mapEntries.size(); i++) {
                    const MapEntry& e = m_mapEntries[(size_t)i];
                    if (needle.empty() ||
                        lower(e.name).find(needle) != std::string::npos ||
                        lower(e.id).find(needle) != std::string::npos)
                        shown.push_back(i);
                }

                const int total = (int)shown.size();
                const int rowH = 30, visible = std::min(total, 5);
                m_mpMapScroll = std::clamp(m_mpMapScroll, 0, std::max(0, total - visible));
                const Rectangle lb{(float)(centerX - fieldW / 2), (float)y, (float)fieldW,
                                   (float)(rowH * std::max(1, visible) + 8)};
                DrawRectangleRounded(lb, 0.06f, 8, Color{18, 20, 26, 240});
                DrawRectangleRoundedLines(lb, 0.06f, 8, Color{90, 100, 120, 200});
                if (CheckCollisionPointRec(mouse, lb)) {
                    const float w = GetMouseWheelMove();
                    if (w != 0.0f) m_mpMapScroll -= (int)w;
                    m_mpMapScroll = std::clamp(m_mpMapScroll, 0, std::max(0, total - visible));
                }

                if (total == 0) {
                    DrawText("Nothing matches that.", (int)lb.x + 10, (int)lb.y + 10, 14,
                             Color{150, 140, 130, 255});
                }
                for (int i = 0; i < visible && m_mpMapScroll + i < total; i++) {
                    const int idx = shown[(size_t)(m_mpMapScroll + i)];
                    const MapEntry& e = m_mapEntries[(size_t)idx];
                    const Rectangle row{lb.x + 4, lb.y + 4 + i * (float)rowH,
                                        lb.width - 8, (float)rowH};
                    const bool chosen = idx == m_mpMapIndex;
                    const bool hovered = CheckCollisionPointRec(mouse, row);
                    if (chosen)      DrawRectangleRounded(row, 0.2f, 6, Color{40, 60, 46, 220});
                    else if (hovered) DrawRectangleRounded(row, 0.2f, 6, Color{34, 38, 48, 200});
                    DrawText((e.name.empty() ? e.id : e.name).c_str(), (int)row.x + 8,
                             (int)row.y + 7, 15,
                             chosen ? Color{170, 220, 185, 255} : Color{200, 205, 215, 255});
                    // Where it came from, because a custom map is one the other
                    // players may simply not have.
                    const char* origin = e.isStandard ? "standard" : "custom";
                    DrawText(origin, (int)(row.x + row.width) - MeasureText(origin, 12) - 8,
                             (int)row.y + 9, 12,
                             e.isStandard ? Color{120, 132, 150, 255}
                                          : Color{160, 145, 110, 255});
                    if (click && hovered) {
                        m_mpMapIndex = idx;
                        m_mpMapId = e.id;
                    }
                }
                m_mpMapIndex = std::clamp(m_mpMapIndex, 0, (int)m_mapEntries.size() - 1);
                m_mpMapId = m_mapEntries[(size_t)m_mpMapIndex].id;
                y += (int)lb.height + 4;
                if (total > visible) {
                    const std::string more =
                        std::to_string(m_mpMapScroll + 1) + "-" +
                        std::to_string(std::min(total, m_mpMapScroll + visible)) +
                        " of " + std::to_string(total) + "   (scroll)";
                    DrawText(more.c_str(), centerX - fieldW / 2, y, 12,
                             Color{130, 140, 155, 255});
                    y += 16;
                }
            }
        }
        y += 8;

        DrawText("Game name", centerX - fieldW / 2, y, 15, Color{160, 170, 190, 255});
        y += 19;
        const Rectangle name{(float)(centerX - fieldW / 2), (float)y, (float)fieldW, (float)fieldH};
        drawField(name.x, name.y, name.width, name.height, m_mpNameField, "My game",
                  m_mpFocus == 2);
        if (click && CheckCollisionPointRec(mouse, name)) m_mpFocus = 2;
        y += fieldH + 12;

        DrawText("Port", centerX - fieldW / 2, y, 15, Color{160, 170, 190, 255});
        DrawText("Who can reach it", centerX - fieldW / 2 + 150, y, 15,
                 Color{160, 170, 190, 255});
        y += 19;
        const Rectangle port{(float)(centerX - fieldW / 2), (float)y, 130.0f, (float)fieldH};
        drawField(port.x, port.y, port.width, port.height, m_mpPortField, "27015",
                  m_mpFocus == 3);
        if (click && CheckCollisionPointRec(mouse, port)) m_mpFocus = 3;

        const MpButton reach = buttonAt((float)(centerX - fieldW / 2 + 150), (float)y,
                                        (float)(fieldW - 150), (float)fieldH, mouse);
        drawButton(reach, m_mpBindAll ? "Anyone on my network"
                                      : "This computer only (use a tunnel)",
                   15, Color{34, 40, 52, 230}, Color{100, 120, 150, 200});
        if (click && reach.hovered) m_mpBindAll = !m_mpBindAll;
        y += fieldH + 14;

        const MpButton fewer = buttonAt((float)(centerX - fieldW / 2), (float)y, 38.0f, 34.0f, mouse);
        drawButton(fewer, "-", 18, Color{34, 36, 44, 220}, Color{90, 95, 110, 190});
        if (click && fewer.hovered) m_mpMaxPlayers = std::max(2, m_mpMaxPlayers - 1);
        const std::string players = std::to_string(m_mpMaxPlayers) + " players";
        DrawText(players.c_str(), centerX - fieldW / 2 + 48, y + 9, 17, RAYWHITE);
        const MpButton more = buttonAt((float)(centerX - fieldW / 2 + 148), (float)y, 38.0f, 34.0f, mouse);
        drawButton(more, "+", 18, Color{34, 36, 44, 220}, Color{90, 95, 110, 190});
        if (click && more.hovered) m_mpMaxPlayers = std::min(32, m_mpMaxPlayers + 1);

        const MpButton listed = buttonAt((float)(centerX + 20), (float)y, 24.0f, 24.0f, mouse);
        DrawRectangleRounded(listed.rect, 0.2f, 6,
                             m_mpListed ? Color{80, 130, 90, 240} : Color{30, 32, 40, 230});
        DrawRectangleRoundedLines(listed.rect, 0.2f, 6, Color{150, 160, 175, 200});
        if (m_mpListed) DrawText("x", (int)listed.rect.x + 8, (int)listed.rect.y + 3, 17, WHITE);
        DrawText("List publicly", (int)listed.rect.x + 32, (int)listed.rect.y + 4, 15,
                 Color{200, 205, 215, 255});
        if (click && listed.hovered) m_mpListed = !m_mpListed;
        y += 46;

        // ---- the tunnel, which is what makes "this computer only" reachable --
        //
        // Only relevant when bound to loopback. Binding to the network is the
        // other way to be reachable, and offering both at once invites a host
        // to turn on something that does nothing.
        if (!m_mpBindAll) {
            tunnelSetToolsDir(mpToolsDir());
            const std::vector<TunnelProvider> providers = tunnelProvidersAvailable();
            const bool haveAuto = !providers.empty() &&
                                  tunnelProviderWorksUnattended(providers[0]);

            const MpButton use = buttonAt((float)(centerX - fieldW / 2), (float)y,
                                          24.0f, 24.0f, mouse);
            DrawRectangleRounded(use.rect, 0.2f, 6,
                                 m_mpUseTunnel ? Color{80, 130, 90, 240}
                                               : Color{30, 32, 40, 230});
            DrawRectangleRoundedLines(use.rect, 0.2f, 6, Color{150, 160, 175, 200});
            if (m_mpUseTunnel)
                DrawText("x", (int)use.rect.x + 8, (int)use.rect.y + 3, 17, WHITE);
            DrawText("Open a tunnel so people outside can reach me",
                     (int)use.rect.x + 32, (int)use.rect.y + 4, 15,
                     Color{200, 205, 215, 255});
            if (click && use.hovered) m_mpUseTunnel = !m_mpUseTunnel;
            y += 34;

            if (m_mpUseTunnel && haveAuto) {
                const std::string ready = std::string("Ready: ") +
                    tunnelProviderName(providers[0]) +
                    ". A public address appears in the lobby once you start.";
                y = drawWrapped(ready, centerX - fieldW / 2, y, fieldW, 13,
                                Color{150, 200, 165, 255}) + 6;

            } else if (m_mpUseTunnel) {
                const TunnelInstallStatus st = m_mpTunnelInstaller
                    ? m_mpTunnelInstaller->status() : TunnelInstallStatus{};

                if (st.busy()) {
                    y = drawWrapped(st.message, centerX - fieldW / 2, y, fieldW, 13,
                                    Color{200, 200, 150, 255}) + 6;
                } else if (st.phase == TunnelInstallStatus::Phase::Done) {
                    y = drawWrapped(st.message, centerX - fieldW / 2, y, fieldW, 13,
                                    Color{150, 210, 170, 255}) + 6;
                } else {
                    y = drawWrapped(providers.empty()
                            ? "Nothing on this computer can open a tunnel yet."
                            : "Nothing here can open one on its own. ssh is present, "
                              "but an anonymous localhost.run tunnel no longer gets "
                              "an address.",
                            centerX - fieldW / 2, y, fieldW, 13,
                            Color{205, 195, 175, 255}) + 6;

                    if (TunnelInstaller::supported()) {
                        const MpButton get = buttonAt((float)(centerX - fieldW / 2),
                                                      (float)y, 210.0f, 32.0f, mouse);
                        drawButton(get, "Install cloudflared", 15,
                                   Color{40, 52, 68, 230}, Color{120, 150, 190, 210});
                        if (click && get.hovered) {
                            if (!m_mpTunnelInstaller)
                                m_mpTunnelInstaller = new TunnelInstaller();
                            m_mpTunnelInstaller->begin(mpToolsDir());
                        }
                        y += 38;
                        // Exactly what pressing that does. A download that then
                        // gets executed deserves describing before it happens,
                        // not after.
                        y = drawWrapped(TunnelInstaller::describe(),
                                        centerX - fieldW / 2, y, fieldW, 12,
                                        Color{135, 143, 157, 255}) + 4;
                    }
                    if (st.phase == TunnelInstallStatus::Phase::Failed &&
                        !st.message.empty()) {
                        y = drawWrapped(st.message, centerX - fieldW / 2, y, fieldW, 13,
                                        Color{225, 165, 140, 255}) + 4;
                    }
                }
            }
        }

    } else if (m_mpSetupTab == 1) {
        // ---- the rules of the table --------------------------------------
        auto choice = [&](const char* label, const char* value, const char* why,
                          int& field, int options) {
            DrawText(label, centerX - fieldW / 2, y, 15, Color{160, 170, 190, 255});
            y += 19;
            const MpButton b = buttonAt((float)(centerX - fieldW / 2), (float)y,
                                        (float)fieldW, (float)fieldH, mouse);
            drawButton(b, value, 16, Color{34, 40, 52, 230}, Color{100, 120, 150, 200});
            if (click && b.hovered) field = (field + 1) % options;
            y += fieldH + 3;
            y = drawWrapped(why, centerX - fieldW / 2, y, fieldW, 13,
                            Color{130, 138, 152, 255}) + 8;
        };

        choice("Who picks countries",
               m_mpAssignment == 0 ? "I assign them" : "Players pick their own",
               m_mpAssignment == 0
                   ? "You allocate a country to each player from the lobby."
                   : "Players claim from the map. One country each either way, and "
                     "the server refuses a country somebody already holds.",
               m_mpAssignment, 2);

        choice("Someone arriving after the start",
               m_mpLateJoin == 0 ? "Turn them away" : "Let them spectate",
               m_mpLateJoin == 0
                   ? "The game is closed once it begins."
                   : "They see the world and the player list, hold no country, and "
                     "anything they submit is discarded rather than merely ignored.",
               m_mpLateJoin, 2);

        choice("A player who submits nothing",
               m_mpAbsent == 0 ? "The AI plays their turn" : "Their country sits idle",
               m_mpAbsent == 0
                   ? "Everyone is told which countries the AI played, and why."
                   : "Nothing happens for them that turn. A country that never moves "
                     "is easier to exploit than one the AI defends.",
               m_mpAbsent, 2);

        const MpButton anon = buttonAt((float)(centerX - fieldW / 2), (float)y, 24.0f, 24.0f, mouse);
        DrawRectangleRounded(anon.rect, 0.2f, 6,
                             m_mpAnonymous ? Color{80, 130, 90, 240} : Color{30, 32, 40, 230});
        DrawRectangleRoundedLines(anon.rect, 0.2f, 6, Color{150, 160, 175, 200});
        if (m_mpAnonymous) DrawText("x", (int)anon.rect.x + 8, (int)anon.rect.y + 3, 17, WHITE);
        DrawText("Host without showing my name", (int)anon.rect.x + 32,
                 (int)anon.rect.y + 4, 15, Color{200, 205, 215, 255});
        if (click && anon.hovered) m_mpAnonymous = !m_mpAnonymous;
        y += 28;
        y = drawWrapped("Players are still told the server is hosted anonymously -- "
                        "that is different from a server that says nothing, which "
                        "clients refuse outright.",
                        centerX - fieldW / 2, y, fieldW, 13, Color{130, 138, 152, 255}) + 6;

        const MpButton ded = buttonAt((float)(centerX - fieldW / 2), (float)y, 24.0f, 24.0f, mouse);
        DrawRectangleRounded(ded.rect, 0.2f, 6,
                             m_mpDedicated ? Color{80, 130, 90, 240} : Color{30, 32, 40, 230});
        DrawRectangleRoundedLines(ded.rect, 0.2f, 6, Color{150, 160, 175, 200});
        if (m_mpDedicated) DrawText("x", (int)ded.rect.x + 8, (int)ded.rect.y + 3, 17, WHITE);
        DrawText("Host only -- I am not playing", (int)ded.rect.x + 32,
                 (int)ded.rect.y + 4, 15, Color{200, 205, 215, 255});
        if (click && ded.hovered) m_mpDedicated = !m_mpDedicated;
        y += 30;

    } else {
        // ---- turns, and where they are stored -----------------------------
        DrawText("Seconds per turn", centerX - fieldW / 2, y, 15, Color{160, 170, 190, 255});
        y += 19;
        const Rectangle turn{(float)(centerX - fieldW / 2), (float)y, 150.0f, (float)fieldH};
        drawField(turn.x, turn.y, turn.width, turn.height, m_mpTurnField,
                  "0 = no timer", m_mpFocus == 4);
        if (click && CheckCollisionPointRec(mouse, turn)) m_mpFocus = 4;
        if (mpTurnSeconds() != 0) m_mpConfirmSlow = false;

        // Typed, so it is read back the same way it will be used.
        const int seconds = mpTurnSeconds();
        const std::string summary = seconds == 0
            ? std::string("Long-form: no countdown at all.")
            : durationWords(seconds) + " per turn.";
        DrawText(summary.c_str(), (int)turn.x + 164, (int)turn.y + 12, 15,
                 seconds == 0 ? Color{205, 190, 140, 255} : Color{170, 180, 200, 255});
        y += fieldH + 6;
        y = drawWrapped(seconds == 0
                ? "This is the SLOW kind of game. Nothing resolves until every "
                  "single player has submitted -- there is no deadline forcing "
                  "it -- so one person who stops playing stops the campaign for "
                  "everyone until you release their seat. Ideal over weeks; "
                  "wrong for an evening."
                : "The turn resolves when everyone has submitted, or when the "
                  "time runs out -- whichever comes first. Anyone who submitted "
                  "nothing is handled by the rule on the Rules tab.",
                centerX - fieldW / 2, y, fieldW, 13, Color{130, 138, 152, 255}) + 10;

        // Where turns live between sessions.
        DrawText("Where turns are stored", centerX - fieldW / 2, y, 15,
                 Color{160, 170, 190, 255});
        y += 19;
        const TurnStoreKind kind = mpStoreKind();
        const MpButton store = buttonAt((float)(centerX - fieldW / 2), (float)y,
                                        (float)fieldW, (float)fieldH, mouse);
        drawButton(store, turnStoreName(kind), 16, Color{34, 40, 52, 230},
                   Color{100, 120, 150, 200});
        if (click && store.hovered) m_mpStore = (m_mpStore + 1) % 4;
        y += fieldH + 6;

        // The store's OWN warning text, verbatim. Two copies of "what this
        // costs you" would drift, and the one people read would be the wrong
        // one -- so there is exactly one, in TurnStore.cpp.
        const TurnStoreWarning w = turnStoreWarning(kind);
        for (const std::string& line : w.forHost) {
            const bool loud = line.find("READ THIS") != std::string::npos;
            y = drawWrapped(line, centerX - fieldW / 2, y, fieldW, 13,
                            loud ? Color{235, 170, 130, 255}
                                 : (w.noGuarantee ? Color{200, 180, 155, 255}
                                                  : Color{140, 148, 162, 255})) + 4;
            if (y > m_screenH - 150) break;   // the rest is in the docs
        }
    }

    // The buttons sit at a fixed height so they do not move as a tab's content
    // changes length -- a Start button that jumps under the cursor is a Start
    // button somebody presses by accident.
    const int btnY = m_screenH - 120;
    const int btnW = 220, btnH = 48, gap = 16;
    const MpButton back = buttonAt((float)(centerX - btnW - gap / 2), (float)btnY,
                                   (float)btnW, (float)btnH, mouse);
    drawButton(back, "Back", 19, Color{34, 36, 44, 220}, Color{90, 95, 110, 190});
    if (click && back.hovered) { m_mpPage = MpPage::Hub; m_mpFocus = -1; }

    // No timer is a legitimate choice and the right one for a campaign, but it
    // is not the choice most people mean by "0" -- so it is said once, plainly,
    // and then got out of the way rather than nagged about.
    if (m_mpConfirmSlow) {
        const std::string warn =
            "No turn timer: nothing resolves until EVERY player has submitted. "
            "One person who stops playing halts the campaign for everyone until "
            "you release their seat in the lobby. Good over weeks, wrong for an "
            "evening -- set a time on the Turns tab if you want one.";
        const int wrapW = 470;
        const float wh = (float)measureWrapped(warn, wrapW, 14) + 20.0f;
        const Rectangle wb{(float)(centerX - wrapW / 2 - 12), (float)(btnY - wh - 16),
                           (float)wrapW + 24, wh};
        DrawRectangleRounded(wb, 0.08f, 8, Color{44, 36, 26, 225});
        DrawRectangleRoundedLines(wb, 0.08f, 8, Color{175, 145, 95, 200});
        drawWrapped(warn, centerX - wrapW / 2, (int)wb.y + 10, wrapW, 14,
                    Color{225, 205, 170, 255});
    }

    const MpButton start = buttonAt((float)(centerX + gap / 2), (float)btnY,
                                    (float)btnW, (float)btnH, mouse);
    drawButton(start, m_mpConfirmSlow ? "Start anyway" : "Start hosting", 19,
               m_mpConfirmSlow ? Color{62, 52, 34, 235} : Color{40, 60, 48, 230},
               m_mpConfirmSlow ? Color{190, 160, 110, 220} : Color{130, 190, 140, 210},
               !m_mpNameField.empty());
    if (click && start.hovered && !m_mpNameField.empty()) {
        if (mpTurnSeconds() == 0 && !m_mpConfirmSlow) {
            m_mpConfirmSlow = true;      // say it, then let them through
        } else {
            m_mpConfirmSlow = false;
            mpStartHosting();
        }
    }
}

// -------------------------------------------------------------------- lobby --

void Game::drawMpLobby(Vector2 mouse, bool click) {
    const int centerX = m_screenW / 2;
    const int panelW = 620;
    int y = 120;

    const bool hosting = m_netHost != nullptr;
    const Lobby* lobby = hosting ? &m_netHost->lobby() : nullptr;

    // What to give people, and whatever the transport had to say about it.
    if (hosting) {
        const std::string code = m_netHost->code();
        if (!code.empty()) {
            const std::string line = "Invite code:  " + code;
            DrawText(line.c_str(), centerX - MeasureText(line.c_str(), 24) / 2, y, 24,
                     Color{200, 220, 240, 255});
            y += 32;
            // What to actually give people. The tunnel address when there is
            // one, because that is the thing that works from outside -- a host
            // reading out "port 27015" to a friend on another continent is a
            // host whose game nobody joins.
            if (m_mpTunnel && m_mpTunnel->state() == Tunnel::State::Up) {
                const std::string addr = "Address:  " + m_mpTunnel->address();
                DrawText(addr.c_str(), centerX - MeasureText(addr.c_str(), 19) / 2, y, 19,
                         Color{170, 220, 180, 255});
                y += 26;
                const std::string via = std::string("via ") +
                    tunnelProviderName(m_mpTunnel->provider()) +
                    "  ·  local port " + std::to_string(m_netHost->listenPort());
                DrawText(via.c_str(), centerX - MeasureText(via.c_str(), 14) / 2, y, 14,
                         Color{130, 140, 155, 255});
                y += 24;
            } else if (m_mpTunnel && m_mpTunnel->state() == Tunnel::State::Starting) {
                const char* w = "Opening a tunnel...";
                DrawText(w, centerX - MeasureText(w, 15) / 2, y, 15,
                         Color{200, 190, 140, 255});
                y += 24;
            } else {
                const std::string port = "Listening on port " +
                                         std::to_string(m_netHost->listenPort()) +
                                         (m_mpBindAll ? " (open to your network)"
                                                      : " (this computer only)");
                DrawText(port.c_str(), centerX - MeasureText(port.c_str(), 15) / 2, y, 15,
                         Color{140, 150, 165, 255});
                y += 24;
            }
        } else {
            DrawText("Opening the game...", centerX - MeasureText("Opening the game...", 18) / 2,
                     y, 18, Color{170, 180, 200, 255});
            y += 30;
        }
        // Copy, and "can anyone actually get here". Both are things a host
        // otherwise has to guess at.
        {
            const int bw = 150, bh = 28;
            const MpButton copy = buttonAt((float)(centerX - bw - 6), (float)y,
                                           (float)bw, (float)bh, mouse);
            drawButton(copy, "Copy invite", 15, Color{40, 52, 68, 230},
                       Color{120, 150, 190, 210});
            if (click && copy.hovered) {
                SetClipboardText(mpInviteText().c_str());
                mpNote("Invite copied -- address and code together.");
            }

            const MpButton test = buttonAt((float)(centerX + 6), (float)y,
                                           (float)bw, (float)bh, mouse);
            drawButton(test, m_mpReach == MpReach::Testing ? "Checking..."
                                                           : "Can others reach me?",
                       15, Color{40, 46, 60, 230}, Color{120, 140, 170, 210},
                       m_mpReach != MpReach::Testing);
            if (click && test.hovered && m_mpReach != MpReach::Testing)
                mpBeginReachTest();
            y += bh + 6;

            if (!m_mpReachNote.empty()) {
                const Color c = m_mpReach == MpReach::Reachable ? Color{150, 210, 170, 255}
                              : m_mpReach == MpReach::Testing   ? Color{200, 200, 150, 255}
                                                                : Color{225, 175, 140, 255};
                y = drawWrapped(m_mpReachNote, centerX - panelW / 2, y, panelW, 14, c) + 4;
            }
        }

        const std::string note = m_netHost->listenNote();
        if (!note.empty()) {
            y = drawWrapped(note, centerX - panelW / 2, y, panelW, 15,
                            Color{235, 200, 140, 255}) + 6;
        }
    } else if (m_netSession) {
        const char* words = phaseWords(m_netSession->phase());
        std::string line = words;
        if (m_netSession->phase() == NetSession::Phase::Connecting &&
            m_netSession->addressCount() > 1) {
            line += "  (" + std::to_string(m_netSession->addressAttempt()) + " of " +
                    std::to_string(m_netSession->addressCount()) + ")";
        }
        DrawText(line.c_str(), centerX - MeasureText(line.c_str(), 20) / 2, y, 20,
                 Color{190, 200, 215, 255});
        y += 34;

        const NetWelcome& w = m_netSession->welcome();
        if (!w.sessionName.empty()) {
            DrawText(w.sessionName.c_str(),
                     centerX - MeasureText(w.sessionName.c_str(), 22) / 2, y, 22, RAYWHITE);
            y += 30;
            // Who is hosting: always declared, so "chose not to be named" and
            // "would not answer" look different here.
            const bool official =
                badgeIssuerIsOfficial(w.host.issuer, AccountClient::get().issuer());
            const std::string host = w.host.name.empty()
                ? std::string("Hosted by someone who chose not to be named")
                : "Hosted by " + w.host.name;
            DrawText(host.c_str(), centerX - MeasureText(host.c_str(), 15) / 2, y, 15,
                     w.host.name.empty() ? Color{150, 155, 170, 255}
                                         : peerNameColor(w.host.badges, official));
            y += 22;

            // A server using someone else's account service is a decision the
            // player should see, not a detail. Badges from it mean nothing.
            if (!official && !w.host.issuer.empty()) {
                const std::string warn =
                    "Accounts here come from " + w.host.issuer + ", not the usual service";
                DrawText(warn.c_str(), centerX - MeasureText(warn.c_str(), 14) / 2, y, 14,
                         Color{225, 185, 130, 255});
                y += 20;
            }
            y += 6;
        }
    }

    y += 10;

    // Two tabs: who is here, and which countries are played by whom.
    const int tabW = 140;
    const MpButton peopleTab = buttonAt((float)(centerX - tabW - 4), (float)y,
                                        (float)tabW, 34.0f, mouse);
    drawButton(peopleTab, "People", 17,
               m_mpPlayersTab ? Color{28, 30, 38, 210} : Color{44, 54, 70, 230},
               Color{100, 120, 150, 200});
    if (click && peopleTab.hovered) m_mpPlayersTab = false;

    const MpButton countriesTab = buttonAt((float)(centerX + 4), (float)y,
                                           (float)tabW, 34.0f, mouse);
    drawButton(countriesTab, "Players", 17,
               m_mpPlayersTab ? Color{44, 54, 70, 230} : Color{28, 30, 38, 210},
               Color{100, 120, 150, 200});
    if (click && countriesTab.hovered) m_mpPlayersTab = true;
    y += 46;

    const std::vector<NetPeer> roster = hosting ? m_netHost->lobby().roster()
                                  : m_netSession ? m_netSession->roster()
                                                 : std::vector<NetPeer>{};

    if (roster.empty()) {
        DrawText("Nobody else here yet.", centerX - MeasureText("Nobody else here yet.", 17) / 2,
                 y + 10, 17, Color{130, 135, 150, 255});
        y += 44;
    } else {
        for (const NetPeer& p : roster) {
            const Rectangle row{(float)(centerX - panelW / 2), (float)y, (float)panelW, 38.0f};
            DrawRectangleRounded(row, 0.12f, 8, Color{24, 26, 34, 190});
            DrawText(p.name.c_str(), (int)row.x + 14, (int)row.y + 10, 18,
                     peerNameColor(p.badges, p.officialIssuer));

            int rightX = (int)(row.x + row.width) - 14;
            if (!m_mpPlayersTab) {
                if (!p.connected) {
                    // A seat held from a previous session has never had a
                    // connection this time, so its peer id is still 0. Saying
                    // "disconnected" about somebody who simply has not opened
                    // the game yet would misdescribe them.
                    const bool heldOver = p.peerId == 0;

                    // Only the host may give a seat up, only in the lobby, and
                    // only for somebody who is not here. Doing it mid-game
                    // would hand away a country the AI is currently playing.
                    if (hosting && m_netHost && lobby &&
                        lobby->state() == NetSessionState::Lobby && !p.psid.empty()) {
                        const MpButton rel = buttonAt((float)(rightX - 84),
                                                      (float)(row.y + 6), 84.0f, 26.0f,
                                                      mouse);
                        drawButton(rel, "Release seat", 13, Color{46, 36, 36, 230},
                                   Color{150, 105, 105, 200});
                        if (click && rel.hovered) {
                            if (m_netHost->lobby().releaseSeat(p.psid)) {
                                m_netHost->broadcastLobby();
                                // Written through, or the seat returns on the
                                // next resume and the host has to do it again.
                                mpSaveSeats();
                                mpNote(std::string("Released ") +
                                       (p.name.empty() ? "that seat" : p.name) +
                                       "'s seat. Their country is free and somebody "
                                       "else can take it.");
                            }
                            // `roster` is a snapshot, so finishing the loop is
                            // safe -- but one release per click is the honest
                            // reading of one press.
                            break;
                        }
                        rightX -= 96;
                    }

                    const char* conn = heldOver ? "seat held" : "disconnected";
                    rightX -= MeasureText(conn, 14);
                    DrawText(conn, rightX, (int)row.y + 12, 14,
                             heldOver ? Color{190, 175, 130, 255}
                                      : Color{200, 140, 130, 255});
                    rightX -= 12;
                }
            } else {
                // Which country, and whether a person or the AI is playing it.
                // A disconnected player's country is played by the AI, which is
                // the thing everyone else in the game actually needs to know.
                const std::string who = p.spectator ? std::string("spectating")
                    : p.countryId == 0 ? std::string("no country yet")
                    : (p.connected ? "played by " + p.name : "AI is standing in");
                rightX -= MeasureText(who.c_str(), 14);
                DrawText(who.c_str(), rightX, (int)row.y + 12, 14,
                         p.connected ? Color{150, 200, 165, 255} : Color{200, 175, 130, 255});
                rightX -= 12;
            }
            drawBadgeTags(p.badges, p.officialIssuer, rightX, (int)row.y + 13);
            y += 44;
        }
    }

    // ---- pick a country -----------------------------------------------------
    //
    // The list comes from the host: a joining client has not loaded the map and
    // cannot know what this world contains. The host reads its own world.
    y += 8;
    std::vector<std::pair<uint16_t, std::string>> choices;
    if (hosting) {
        for (int cid : m_playableCountryIds) {
            const Country* c = m_countries.getCountry(cid);
            choices.emplace_back((uint16_t)cid, c ? c->name : std::to_string(cid));
        }
    } else if (m_netSession) {
        for (const auto& e : m_netSession->countries()) choices.emplace_back(e.id, e.name);
    }

    uint16_t myCountry = 0;
    const uint16_t myPeer = hosting ? m_netHost->lobby().hostPeerId()
                        : m_netSession ? m_netSession->welcome().peerId : 0;
    for (const NetPeer& p : roster) if (p.peerId == myPeer) myCountry = p.countryId;

    if (!choices.empty()) {
        // Which countries are already spoken for. A picker that lets you choose
        // a taken country only to be refused is a picker that looks broken.
        std::string mine = "not chosen yet";
        for (const auto& c : choices) if (c.first == myCountry) mine = c.second;
        const std::string label = "Your country:  " + mine;
        DrawText(label.c_str(), centerX - panelW / 2, y, 16,
                 myCountry ? Color{170, 210, 180, 255} : Color{200, 180, 140, 255});
        y += 24;

        // Which countries are already spoken for. A picker that lets you choose
        // a taken country only to be refused is a picker that looks broken.
        std::vector<uint16_t> taken;
        for (const NetPeer& p : roster)
            if (p.countryId != 0 && p.peerId != myPeer) taken.push_back(p.countryId);
        auto isTaken = [&](uint16_t id) {
            return std::find(taken.begin(), taken.end(), id) != taken.end();
        };
        auto claim = [&](uint16_t id) {
            if (isTaken(id)) { mpNote("Somebody already has that country.", true); return; }
            if (hosting) {
                if (m_netHost->lobby().claimCountry(myPeer, id) == LobbyDenial::None)
                    m_netHost->broadcastLobby();
            } else if (m_netSession) {
                m_netSession->claimCountry(id);
            }
            m_mpPickingCountry = false;
        };

        const MpButton pick = buttonAt((float)(centerX - panelW / 2), (float)y,
                                       200.0f, 32.0f, mouse);
        drawButton(pick, m_mpPickingCountry ? "Close the list"
                       : myCountry == 0 ? "Choose a country" : "Change country",
                   15, Color{40, 52, 68, 230}, Color{120, 150, 190, 210});
        if (click && pick.hovered) {
            m_mpPickingCountry = !m_mpPickingCountry;
            m_mpCountryScroll = 0;
        }

        // The host has the world open, so it can be pointed at. A joiner has
        // only the catalogue -- it never loaded the map -- so the list is the
        // honest option there rather than a map it cannot draw.
        if (hosting && !m_mpPickingCountry) {
            DrawText("or click the map, either side of this panel", (int)(centerX - panelW / 2) + 212,
                     y + 9, 14, Color{140, 150, 165, 255});
        }
        y += 40;

        if (m_mpPickingCountry) {
            const int rowH = 26, visible = 9;
            const float listW = 300.0f;
            const int total = (int)choices.size();
            m_mpCountryScroll = std::clamp(m_mpCountryScroll, 0,
                                           std::max(0, total - visible));
            const Rectangle box{(float)(centerX - panelW / 2), (float)y, listW,
                                (float)(rowH * visible + 8)};
            DrawRectangleRounded(box, 0.06f, 8, Color{18, 20, 26, 240});
            DrawRectangleRoundedLines(box, 0.06f, 8, Color{90, 100, 120, 200});

            if (CheckCollisionPointRec(mouse, box)) {
                const float wheel = GetMouseWheelMove();
                if (wheel != 0.0f) m_mpCountryScroll -= (int)wheel;
                m_mpCountryScroll = std::clamp(m_mpCountryScroll, 0,
                                               std::max(0, total - visible));
            }

            for (int i = 0; i < visible && m_mpCountryScroll + i < total; i++) {
                const auto& entry = choices[(size_t)(m_mpCountryScroll + i)];
                const Rectangle row{box.x + 4, box.y + 4 + i * (float)rowH,
                                    box.width - 8, (float)rowH};
                const bool hovered = CheckCollisionPointRec(mouse, row);
                const bool gone = isTaken(entry.first);
                const bool isMine = entry.first == myCountry;
                if (hovered && !gone)
                    DrawRectangleRounded(row, 0.2f, 6, Color{40, 52, 68, 200});
                DrawText(entry.second.c_str(), (int)row.x + 8, (int)row.y + 5, 15,
                         gone   ? Color{95, 100, 112, 255}
                       : isMine ? Color{150, 210, 170, 255}
                                : (hovered ? RAYWHITE : Color{195, 200, 210, 255}));
                if (gone) {
                    const char* t = "taken";
                    DrawText(t, (int)(row.x + row.width) - MeasureText(t, 13) - 8,
                             (int)row.y + 6, 13, Color{150, 120, 110, 255});
                }
                if (click && hovered && !gone) claim(entry.first);
            }

            if (total > visible) {
                const std::string more = std::to_string(m_mpCountryScroll + 1) + "-" +
                    std::to_string(std::min(total, m_mpCountryScroll + visible)) +
                    " of " + std::to_string(total) + "   (scroll)";
                DrawText(more.c_str(), (int)box.x, (int)(box.y + box.height) + 4, 13,
                         Color{130, 140, 155, 255});
            }
            y += (int)box.height + 24;
        } else if (hosting && click && m_playableCountryIds.size() && m_renderer &&
                   // NOT anywhere this screen draws. Every lobby widget lives in
                   // one centred column, and the previous test -- "below the
                   // panel" -- let the Start game press ALSO claim whatever
                   // country happened to sit under the button. Picking the
                   // Soviet Union and starting the game handed you West Africa.
                   !CheckCollisionPointRec(mouse,
                       Rectangle{(float)(centerX - panelW / 2 - 24), 90.0f,
                                 (float)(panelW + 48), (float)(m_screenH - 150)})) {
            // A click on the map itself. Same lookup the country-select screen
            // uses, so the two agree about what is playable and what is not.
            int px = 0, py = 0;
            m_renderer->screenToPixel(mouse.x, mouse.y, px, py);
            if (const Province* prov = m_provinces.getProvince(px, py)) {
                const int cid = prov->countryId;
                if (cid != UNC_CID && cid != BLC_CID &&
                    std::find(m_playableCountryIds.begin(), m_playableCountryIds.end(),
                              cid) != m_playableCountryIds.end()) {
                    claim((uint16_t)cid);
                    const Country* c = m_countries.getCountry(cid);
                    mpNote(std::string("You are playing as ") + (c ? c->name : "that country"));
                }
            }
        }
    }

    // ---- what the host can still change, with people already sitting here ----
    //
    // These were setup-only, which made every reconsideration cost a closed
    // lobby and a re-sent invite. Nothing here can be changed once the game is
    // running -- `live` is false then -- because moving the goalposts mid-turn
    // is a different thing entirely.
    if (hosting && m_netHost && lobby) {
        const int sx = centerX - panelW / 2;
        DrawText("Settings", sx, y, 15, Color{160, 170, 190, 255});
        y += 20;

        const bool inLobby = lobby->state() == NetSessionState::Lobby;

        // Turn length, as a box rather than a cycle: on a live campaign the
        // useful values are hours and days, which no sane list would enumerate.
        DrawText("Turn seconds (0 = no limit)", sx, y + 8, 14,
                 Color{175, 182, 196, 255});
        const Rectangle tf{(float)(sx + 230), (float)y, 90.0f, 30.0f};
        drawField(tf.x, tf.y, tf.width, tf.height, m_mpTurnField, "0", m_mpFocus == 4);
        if (click && CheckCollisionPointRec(mouse, tf)) m_mpFocus = 4;
        y += 38;

        auto toggle = [&](const char* label, bool on, int px, int py) {
            const MpButton b = buttonAt((float)px, (float)py, 24.0f, 24.0f, mouse);
            DrawRectangleRounded(b.rect, 0.2f, 6,
                                 on ? Color{80, 130, 90, 240} : Color{30, 32, 40, 230});
            DrawRectangleRoundedLines(b.rect, 0.2f, 6, Color{150, 160, 175, 200});
            if (on) DrawText("x", (int)b.rect.x + 8, (int)b.rect.y + 3, 17, WHITE);
            DrawText(label, (int)b.rect.x + 32, (int)b.rect.y + 4, 14,
                     Color{200, 205, 215, 255});
            return click && b.hovered;
        };

        // Public listing is deliberately NOT here. It is arranged with the
        // account service when hosting begins, and a toggle that quietly did
        // nothing until the next session would be worse than no toggle. It
        // stays on the host screen, where it is true.
        bool changed = false;
        if (toggle(m_mpLateJoin == 0 ? "Latecomers refused" : "Latecomers may watch",
                   m_mpLateJoin != 0, sx, y)) {
            m_mpLateJoin = m_mpLateJoin == 0 ? 1 : 0;
            changed = true;
        }
        if (toggle(m_mpAbsent == 0 ? "Absent: AI plays them"
                                   : "Absent: their country idles",
                   m_mpAbsent == 0, sx + 250, y)) {
            m_mpAbsent = m_mpAbsent == 0 ? 1 : 0;
            changed = true;
        }
        y += 34;

        // Seats. Lowering below the people already here would be a promise the
        // lobby cannot keep, so it stops at the current count.
        if (inLobby) {
            const int seated = (int)roster.size();
            DrawText(("Seats: " + std::to_string(m_mpMaxPlayers)).c_str(), sx, y + 8,
                     15, RAYWHITE);
            const MpButton fewer = buttonAt((float)(sx + 110), (float)y, 30.0f, 30.0f, mouse);
            drawButton(fewer, "-", 17, Color{34, 36, 44, 220}, Color{90, 95, 110, 190},
                       m_mpMaxPlayers > std::max(2, seated));
            if (click && fewer.hovered && m_mpMaxPlayers > std::max(2, seated)) {
                m_mpMaxPlayers--;
                changed = true;
            }
            const MpButton more = buttonAt((float)(sx + 146), (float)y, 30.0f, 30.0f, mouse);
            drawButton(more, "+", 17, Color{34, 36, 44, 220}, Color{90, 95, 110, 190},
                       m_mpMaxPlayers < 32);
            if (click && more.hovered && m_mpMaxPlayers < 32) {
                m_mpMaxPlayers++;
                changed = true;
            }
            y += 38;
        }

        if (changed) {
            // Push it into the lobby and tell everyone, so what players see is
            // what the host just chose rather than what they joined under.
            LobbySettings ls = lobby->settings();
            ls.maxPlayers = (uint8_t)m_mpMaxPlayers;
            ls.lateJoin = m_mpLateJoin == 0 ? NetLateJoin::Refuse : NetLateJoin::Spectate;
            ls.absent   = m_mpAbsent == 0 ? NetAbsent::Ai : NetAbsent::Idle;
            m_netHost->lobby().configure(ls);
            m_netHost->broadcastLobby();
        }
    }

    y += 12;
    const int btnW = 200, btnH = 48, gap = 16;

    if (hosting) {
        const MpButton start = buttonAt((float)(centerX - btnW - gap / 2), (float)y,
                                        (float)btnW, (float)btnH, mouse);
        const bool live = m_netHost->phase() == NetHost::Phase::Live;
        drawButton(start, "Start game", 19, Color{40, 60, 48, 230},
                   Color{130, 190, 140, 210}, live);
        if (click && start.hovered && live) {
            std::string why;
            if (!m_netHost->startGame(why)) {
                mpNote(why, true);
            } else {
                // Everyone gets the world before the host disappears into it.
                const std::vector<uint8_t> snapshot = mpBuildSnapshot();
                for (const NetPeer& p : m_netHost->lobby().roster()) {
                    if (p.peerId == m_netHost->lobby().hostPeerId()) continue;
                    m_netHost->sendSnapshot(p.peerId, (uint32_t)m_turnNumber, snapshot);
                }
                uint16_t mine = 0;
                for (const NetPeer& p : m_netHost->lobby().roster())
                    if (p.peerId == m_netHost->lobby().hostPeerId()) mine = p.countryId;
                mpEnterGame(mine);
            }
        }
    }

    const MpButton leave = buttonAt((float)(hosting ? centerX + gap / 2 : centerX - btnW / 2),
                                    (float)y, (float)btnW, (float)btnH, mouse);
    drawButton(leave, hosting ? "Close game" : "Leave", 19,
               Color{50, 34, 34, 230}, Color{170, 110, 110, 200});
    if (click && leave.hovered) {
        // RETURN, do not fall through. mpLeave() deletes the host, which
        // invalidates BOTH `m_netHost` and `lobby` -- and `hosting` was read
        // once at the top of this function, so it still says true. Everything
        // below this point would be reading a freed object through a stale
        // pointer, which is exactly what it did.
        mpLeave();
        return;
    }

    if (hosting && m_netHost && lobby && m_netHost->unauthenticatedCount() > 0) {
        const std::string s = std::to_string(m_netHost->unauthenticatedCount()) +
                              " connecting...";
        DrawText(s.c_str(), centerX - MeasureText(s.c_str(), 14) / 2, y + btnH + 10, 14,
                 Color{130, 135, 150, 255});
    }
}

// ---------------------------------------------------- the lobby -> game bridge --
//
// The world is loaded asynchronously and the loader is shared with
// singleplayer, so each of these sets up state, kicks off a load, and is
// resumed by mpOnWorldLoaded() when the loader reaches LOAD_FINALIZE.

bool Game::mpResolveMap(const std::string& id, std::string& pathOut, std::string& nameOut) {
    // A map travels as an ID, never a path: the host's directory layout means
    // nothing on anyone else's machine, and a path from the network is a path
    // someone else chose.
    if (m_mapEntries.empty()) loadMapEntries();
    for (const MapEntry& e : m_mapEntries) {
        if (e.id != id) continue;
        pathOut = e.directory + e.filename;
        nameOut = e.name.empty() ? e.id : e.name;
        return true;
    }
    return false;
}

void Game::mpPublishCountries() {
    if (!m_netHost) return;

    NetCountryList list;
    list.countries.reserve(m_playableCountryIds.size());
    for (int cid : m_playableCountryIds) {
        const Country* c = m_countries.getCountry(cid);
        NetCountryList::Entry e;
        e.id = (uint16_t)cid;
        e.name = c ? c->name : std::to_string(cid);
        list.countries.push_back(std::move(e));
    }
    m_netHost->setCountries(list);
}

void Game::mpOnWorldLoaded() {
    const MpLoad purpose = m_mpLoad;
    m_mpLoad = MpLoad::None;

    if (purpose == MpLoad::HostOpen) {
        // The world is open so the lobby can offer real countries. Back to the
        // lobby rather than into the game: nobody has joined yet.
        mpPublishCountries();

        // Whoever was playing before gets their country held for them. This is
        // not a second way to join: reserveSeat leaves a member that admit()
        // recognises by psid, so returning after a week and returning after a
        // dropped packet are the same path through the same code.
        int held = 0;
        SeatBook book;
        if (m_netHost && SeatBook::load(m_currentSavePath, book)) {
            for (const SeatRecord& seat : book.seats) {
                if (m_netHost->lobby().reserveSeat(seat.psid, seat.name, seat.countryId))
                    held++;
            }
            if (held) m_netHost->broadcastLobby();
        }

        m_currentScreen = SCREEN_MULTIPLAYER;
        m_mpPage = MpPage::Lobby;
        if (held) {
            mpNote(std::to_string(held) + " seat(s) are being held for players from "
                   "last time. They keep their country when they rejoin.");
        } else {
            mpNote("World loaded. Share the code and start when everyone is ready.");
        }
        return;
    }

    if (purpose == MpLoad::JoinApply) {
        // The map is loaded; now move it to where the host's world actually is.
        NetWorldSnapshot snap;
        if (NetWorldSnapshot::decode(m_mpPendingSnapshot.data(),
                                     m_mpPendingSnapshot.size(), snap)) {
            // The map is loaded at its starting state. Replaying the host's
            // turns moves it to now -- the same operation a save load performs,
            // through the same function, because a delta means the same thing
            // whether it came off disk or off a socket.
            int applied = 0;
            for (const NetTurnDelta& t : snap.turns) {
                TurnDelta delta;
                if (!SaveManager::unpackTurn(t.packed.data(), t.packed.size(), delta)) {
                    mpNote("That server sent a turn this build could not read.", true);
                    break;
                }
                applyTurnDelta(delta);
                applied++;
            }
            // Province owners have moved, so the pixel ownership and border
            // gradient are stale until rebuilt.
            if (applied) {
                synthesizeMissingRebels();
                rebuildOwnershipPixels();
            }
            if (!snap.stateJson.empty()) loadStateJson(snap.stateJson);
            m_turnNumber = (int)snap.turnNumber;
            reloadBorders();
        }
        m_mpPendingSnapshot.clear();
        mpEnterGame(m_mpMyCountry);
        return;
    }
}

std::vector<uint8_t> Game::mpBuildSnapshot() {
    NetWorldSnapshot snap;
    snap.mapName = m_mpMapId;
    snap.turnNumber = (uint32_t)m_turnNumber;
    snap.stateJson = saveStateJson();

    // Every turn played so far, so a joiner can be brought to now rather than
    // to the start. A game that has not begun has none, and the map's own
    // initial state is already right.
    if (!m_currentSavePath.empty()) {
        for (int t = 1; t <= m_turnNumber; t++) {
            TurnDelta delta = SaveManager::readTurn(m_currentSavePath, t);
            std::vector<uint8_t> packed = SaveManager::packTurn(delta);
            if (packed.empty()) continue;
            snap.turns.push_back(NetTurnDelta{(uint32_t)t, std::move(packed)});
        }
    }
    return snap.encode();
}

void Game::mpApplySnapshot(const std::vector<uint8_t>& payload) {
    NetWorldSnapshot snap;
    if (!NetWorldSnapshot::decode(payload.data(), payload.size(), snap)) {
        mpNote("That server sent a world this build could not read.", true);
        return;
    }

    std::string path, name;
    if (!mpResolveMap(snap.mapName, path, name)) {
        // Name the map. "Could not join" leaves someone with nothing to do;
        // "you need this map" is something they can act on.
        mpNote("You do not have the map this game uses: " + snap.mapName +
               ". Install it and join again.", true);
        return;
    }

    // Which country the server gave us. The server decides; the client only
    // reads the roster it was sent.
    m_mpMyCountry = 0;
    if (m_netSession) {
        const uint16_t me = m_netSession->welcome().peerId;
        for (const NetPeer& p : m_netSession->roster())
            if (p.peerId == me) m_mpMyCountry = p.countryId;
    }

    m_mpPendingSnapshot = payload;
    m_mpLoad = MpLoad::JoinApply;
    mpNote("Loading " + name + "...");
    // Async, for the same reason as the host: a joiner belongs in the game when
    // the world is ready, not the moment the file opens.
    const std::string worldName = m_netSession && !m_netSession->welcome().sessionName.empty()
        ? m_netSession->welcome().sessionName : name;
    startNewGameWithName(path, worldName);
}

void Game::mpEnterGame(uint16_t countryId) {
    if (countryId != 0 && m_countries.getCountry(countryId)) {
        m_playerCountryId = (int)countryId;
        for (auto& n : m_researchNodes) n.researched = hasResearched(n.id, m_playerCountryId);
    } else {
        // A spectator, or a seat with no country yet. The world is still shown.
        m_playerCountryId = 0;
    }

    if (m_renderer) {
        m_renderer->setBlockLeftPan(false);
        m_renderer->setShowCountryNames(false);
    }
    buildCountryProvinceList(m_playerCountryId);
    recordIncomeSnapshot();
    m_currentScreen = SCREEN_PLAYING;
}

// ------------------------------------------------------------- the turn loop --
//
// The host resolves turns; nobody else does. A client that ran processTurn()
// locally would compute its OWN answer to what happened, and the two machines
// would quietly diverge -- which is the failure mode this whole design exists
// to avoid. So a client submits orders and applies the delta the host produced.

bool Game::mpIsHost() const {
    return m_netHost && m_netHost->phase() == NetHost::Phase::Live;
}

bool Game::mpIsClient() const {
    return m_netSession &&
           (m_netSession->phase() == NetSession::Phase::InGame ||
            m_netSession->phase() == NetSession::Phase::Lobby);
}

// --------------------------------------------------------------- ownership --

std::vector<uint8_t> Game::mpSerializeOrders(int countryId) const {
    nlohmann::json j;

    // The lookup table rather than getProvinceById(): this is a const method,
    // and ownership is exactly what that table holds.
    auto ownsProvince = [&](int pid) {
        return pid > 0 && (size_t)pid < m_provinceCountryLookup.size() &&
               m_provinceCountryLookup[pid] == countryId;
    };
    auto ownsShip = [&](int idx) {
        return idx >= 0 && idx < (int)m_ships.size() && m_ships[idx].countryId == countryId;
    };

    for (auto& u : m_pendingUpgrades) if (ownsProvince(u.provinceId)) {
        j["pendingUpgrades"].push_back({{"provinceId", u.provinceId}, {"type", u.type},
                                        {"targetLevel", u.targetLevel},
                                        {"turnsRemaining", u.turnsRemaining}});
    }
    for (auto& s : m_pendingSpecializations) if (ownsProvince(s.provinceId)) {
        j["pendingSpecializations"].push_back({{"provinceId", s.provinceId},
                                               {"specialization", s.specialization},
                                               {"turnsRemaining", s.turnsRemaining}});
    }
    for (auto& r : m_pendingRecruitments) if (ownsProvince(r.provinceId)) {
        j["pendingRecruitments"].push_back({{"provinceId", r.provinceId}, {"count", r.count},
                                            {"turnsRemaining", r.turnsRemaining}});
    }
    for (auto& m : m_pendingMoveOrders) if (m.countryId == countryId) {
        j["pendingMoveOrders"].push_back({{"fromProvince", m.fromProvince},
                                          {"toProvince", m.toProvince}, {"pct", m.pct},
                                          {"countryId", m.countryId}});
    }
    for (auto& d : m_pendingDisbandOrders) if (ownsProvince(d.provinceId)) {
        j["pendingDisbandOrders"].push_back({{"provinceId", d.provinceId}, {"count", d.count}});
    }
    for (auto& sb : m_pendingShipBuilds) if (ownsProvince(sb.provinceId)) {
        j["pendingShipBuilds"].push_back({{"provinceId", sb.provinceId}, {"type", sb.type},
                                          {"turnsRemaining", sb.turnsRemaining}});
    }
    for (auto& ss : m_pendingScrapShips) if (ownsShip(ss.shipIndex)) {
        j["pendingScrapShips"].push_back({{"shipIndex", ss.shipIndex}});
    }
    for (auto& e : m_pendingEmbarkations) if (ownsProvince(e.provinceId)) {
        j["pendingEmbarkations"].push_back({{"provinceId", e.provinceId}, {"count", e.count},
                                            {"turnsRemaining", e.turnsRemaining}});
    }
    for (auto& a : m_pendingArtilleryOrders) if (ownsProvince(a.fromProvince)) {
        j["pendingArtilleryOrders"].push_back({{"fromProvince", a.fromProvince},
                                               {"targetProvince", a.targetProvince},
                                               {"ammoType", a.ammoType}});
    }
    for (auto& sm : m_pendingShipMoveOrders) if (ownsShip(sm.shipIndex)) {
        j["pendingShipMoveOrders"].push_back({{"shipIndex", sm.shipIndex},
                                              {"destLon", sm.destLon}, {"destLat", sm.destLat}});
    }
    for (auto& se : m_pendingShipEngageOrders) if (ownsShip(se.shipIndex)) {
        j["pendingShipEngageOrders"].push_back({{"shipIndex", se.shipIndex},
                                                {"targetIndex", se.targetIndex}});
    }
    for (auto& sb : m_pendingShipBombardOrders) if (ownsShip(sb.shipIndex)) {
        j["pendingShipBombardOrders"].push_back({{"shipIndex", sb.shipIndex},
                                                 {"targetProvince", sb.targetProvince},
                                                 {"ammoType", sb.ammoType}});
    }

    const std::string text = j.is_null() ? "{}" : j.dump();
    return std::vector<uint8_t>(text.begin(), text.end());
}

void Game::mpApplyOrders(int countryId, const std::vector<uint8_t>& payload) {
    if (countryId == 0 || payload.empty()) return;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(std::string(payload.begin(), payload.end()));
    } catch (...) {
        return;                 // discarded whole; the lobby records it as malformed
    }
    if (!j.is_object()) return;

    // THE authoritative check. A peer may submit anything; only orders over
    // provinces and ships that ITS country owns are kept. Nothing here trusts a
    // countryId sent by the client -- ownership is read from this world.
    auto ownsProvince = [&](int pid) {
        const Province* p = m_provinces.getProvinceById(pid);
        return p && p->countryId == countryId;
    };
    auto ownsShip = [&](int idx) {
        return idx >= 0 && idx < (int)m_ships.size() && m_ships[idx].countryId == countryId;
    };
    auto num = [](const nlohmann::json& e, const char* k, auto fallback) {
        return e.contains(k) && e[k].is_number() ? e[k].get<decltype(fallback)>() : fallback;
    };

    // Replace this country's existing orders rather than piling onto them: a
    // resubmission is a correction, not an addition.
    auto dropOwned = [&](auto& vec, auto pred) {
        vec.erase(std::remove_if(vec.begin(), vec.end(), pred), vec.end());
    };
    dropOwned(m_pendingUpgrades,       [&](const auto& o){ return ownsProvince(o.provinceId); });
    dropOwned(m_pendingSpecializations,[&](const auto& o){ return ownsProvince(o.provinceId); });
    dropOwned(m_pendingRecruitments,   [&](const auto& o){ return ownsProvince(o.provinceId); });
    dropOwned(m_pendingMoveOrders,     [&](const auto& o){ return o.countryId == countryId; });
    dropOwned(m_pendingDisbandOrders,  [&](const auto& o){ return ownsProvince(o.provinceId); });
    dropOwned(m_pendingShipBuilds,     [&](const auto& o){ return ownsProvince(o.provinceId); });
    dropOwned(m_pendingScrapShips,     [&](const auto& o){ return ownsShip(o.shipIndex); });
    dropOwned(m_pendingEmbarkations,   [&](const auto& o){ return ownsProvince(o.provinceId); });
    dropOwned(m_pendingArtilleryOrders,[&](const auto& o){ return ownsProvince(o.fromProvince); });
    dropOwned(m_pendingShipMoveOrders, [&](const auto& o){ return ownsShip(o.shipIndex); });
    dropOwned(m_pendingShipEngageOrders,[&](const auto& o){ return ownsShip(o.shipIndex); });
    dropOwned(m_pendingShipBombardOrders,[&](const auto& o){ return ownsShip(o.shipIndex); });

    auto each = [&](const char* key, auto fn) {
        if (!j.contains(key) || !j[key].is_array()) return;
        for (const auto& e : j[key]) if (e.is_object()) fn(e);
    };

    each("pendingUpgrades", [&](const nlohmann::json& e) {
        PendingUpgrade o;
        o.provinceId = num(e, "provinceId", 0);
        if (!ownsProvince(o.provinceId)) return;
        o.type = num(e, "type", 0);
        o.targetLevel = num(e, "targetLevel", 0);
        o.turnsRemaining = num(e, "turnsRemaining", 0);
        m_pendingUpgrades.push_back(o);
    });
    each("pendingSpecializations", [&](const nlohmann::json& e) {
        PendingSpecialization o;
        o.provinceId = num(e, "provinceId", 0);
        if (!ownsProvince(o.provinceId)) return;
        o.specialization = num(e, "specialization", 0);
        o.turnsRemaining = num(e, "turnsRemaining", 0);
        m_pendingSpecializations.push_back(o);
    });
    each("pendingRecruitments", [&](const nlohmann::json& e) {
        PendingRecruitment o;
        o.provinceId = num(e, "provinceId", 0);
        if (!ownsProvince(o.provinceId)) return;
        o.count = num(e, "count", 0);
        o.turnsRemaining = num(e, "turnsRemaining", 0);
        m_pendingRecruitments.push_back(o);
    });
    each("pendingMoveOrders", [&](const nlohmann::json& e) {
        PendingMoveOrder o;
        o.fromProvince = num(e, "fromProvince", 0);
        if (!ownsProvince(o.fromProvince)) return;
        o.toProvince = num(e, "toProvince", 0);
        o.pct = num(e, "pct", 50);
        // Re-attributed, never taken from the wire: a client does not get to
        // say which country issued its orders.
        o.countryId = countryId;
        m_pendingMoveOrders.push_back(o);
    });
    each("pendingDisbandOrders", [&](const nlohmann::json& e) {
        PendingDisbandOrder o;
        o.provinceId = num(e, "provinceId", 0);
        if (!ownsProvince(o.provinceId)) return;
        o.count = num(e, "count", 0);
        m_pendingDisbandOrders.push_back(o);
    });
    each("pendingShipBuilds", [&](const nlohmann::json& e) {
        PendingShipBuild o;
        o.provinceId = num(e, "provinceId", 0);
        if (!ownsProvince(o.provinceId)) return;
        o.type = num(e, "type", 0);
        o.turnsRemaining = num(e, "turnsRemaining", 0);
        m_pendingShipBuilds.push_back(o);
    });
    each("pendingScrapShips", [&](const nlohmann::json& e) {
        PendingScrapShip o;
        o.shipIndex = num(e, "shipIndex", -1);
        if (!ownsShip(o.shipIndex)) return;
        m_pendingScrapShips.push_back(o);
    });
    each("pendingEmbarkations", [&](const nlohmann::json& e) {
        PendingEmbark o;
        o.provinceId = num(e, "provinceId", 0);
        if (!ownsProvince(o.provinceId)) return;
        o.count = num(e, "count", 0);
        o.turnsRemaining = num(e, "turnsRemaining", 0);
        m_pendingEmbarkations.push_back(o);
    });
    each("pendingArtilleryOrders", [&](const nlohmann::json& e) {
        PendingArtilleryOrder o;
        o.fromProvince = num(e, "fromProvince", 0);
        if (!ownsProvince(o.fromProvince)) return;
        o.targetProvince = num(e, "targetProvince", 0);
        o.ammoType = num(e, "ammoType", 0);
        m_pendingArtilleryOrders.push_back(o);
    });
    each("pendingShipMoveOrders", [&](const nlohmann::json& e) {
        PendingShipMoveOrder o;
        o.shipIndex = num(e, "shipIndex", -1);
        if (!ownsShip(o.shipIndex)) return;
        o.destLon = num(e, "destLon", 0.0);
        o.destLat = num(e, "destLat", 0.0);
        m_pendingShipMoveOrders.push_back(o);
    });
    each("pendingShipEngageOrders", [&](const nlohmann::json& e) {
        PendingShipEngageOrder o;
        o.shipIndex = num(e, "shipIndex", -1);
        if (!ownsShip(o.shipIndex)) return;
        o.targetIndex = num(e, "targetIndex", -1);
        m_pendingShipEngageOrders.push_back(o);
    });
    each("pendingShipBombardOrders", [&](const nlohmann::json& e) {
        PendingShipBombardOrder o;
        o.shipIndex = num(e, "shipIndex", -1);
        if (!ownsShip(o.shipIndex)) return;
        o.targetProvince = num(e, "targetProvince", 0);
        o.ammoType = num(e, "ammoType", 0);
        m_pendingShipBombardOrders.push_back(o);
    });
}

// ------------------------------------------------------------ driving turns --

void Game::mpSubmitTurn() {
    // A client never resolves a turn. It says what it wants and waits for the
    // world the host actually produced.
    if (!m_netSession) return;
    m_netSession->submitOrders((uint32_t)m_turnNumber + 1,
                               mpSerializeOrders(m_playerCountryId));
    m_mpWaitingForTurn = true;
    mpNote("Orders sent. Waiting for the other players...");
}

long long Game::mpDeadlineLeft(uint16_t peerId, long long nowMs) const {
    if (mpTurnSeconds() <= 0) return -1;            // no timer at all
    const auto it = m_mpDeadlineMs.find(peerId);
    if (it == m_mpDeadlineMs.end()) return -1;
    return std::max(0LL, it->second - nowMs);
}

void Game::mpHostReady() {
    if (!mpIsHost() || !m_mpTurns) { mpHostTurnUpdate(); return; }
    if (m_netHost->lobby().state() != NetSessionState::Game) return;

    // Recorded against the host's own seat, with an EMPTY payload: the host's
    // orders were applied to this world as they were given, so there is
    // nothing to replay. What was missing was the lobby being told, which is
    // what everyone else's "waiting for..." is computed from.
    const uint16_t me = m_netHost->lobby().hostPeerId();
    const uint32_t turn = m_mpTurns->turnNumber();
    if (m_netHost->lobby().find(me))
        m_netHost->lobby().submitOrders(me, turn, {});
    m_netHost->broadcastLobby();

    mpHostTurnUpdate();
    if (m_netHost && m_mpTurns && m_mpTurns->running()) {
        const std::vector<uint16_t> waiting =
            m_netHost->lobby().missingSubmissions(m_mpTurns->turnNumber());
        if (!waiting.empty())
            mpNote("Your orders are in. Waiting for " +
                   std::to_string(waiting.size()) + " more player(s).");
    }
}

void Game::mpForceResolve() {
    if (!mpIsHost() || !m_mpTurns || !m_mpTurns->running()) return;
    // The host's own orders count as given -- pressing this IS the host
    // saying so -- and everyone still missing is handled by the substitution
    // rule, exactly as a timer running out would have handled them.
    const uint16_t me = m_netHost->lobby().hostPeerId();
    if (m_netHost->lobby().find(me))
        m_netHost->lobby().submitOrders(me, m_mpTurns->turnNumber(), {});
    mpNote("Resolving now without the remaining players.");
    mpResolveTurn();
}

void Game::mpHostTurnUpdate() {
    if (!mpIsHost()) return;
    if (m_netHost->lobby().state() != NetSessionState::Game) return;

    if (!m_mpTurns) {
        m_mpTurns = new TurnRunner();
        TurnRunner::Config c;
        c.turnSeconds = (uint32_t)mpTurnSeconds();
        m_mpTurns->configure(c);
    }

    const long long nowMs = (long long)(GetTime() * 1000.0);

    if (!m_mpTurns->running()) {
        const uint32_t next = (uint32_t)m_turnNumber + 1;
        m_mpTurns->beginTurn(next, nowMs);
        m_netHost->beginTurn(next, m_mpTurns->remainingMs(nowMs));
        // Everyone starts this turn with the full allowance.
        m_mpDeadlineMs.clear();
        const int secs = mpTurnSeconds();
        if (secs > 0)
            for (const NetPeer& p : m_netHost->lobby().roster())
                if (!p.spectator && p.countryId != 0)
                    m_mpDeadlineMs[p.peerId] = nowMs + (long long)secs * 1000;
        return;
    }

    // Somebody who arrived after the turn opened gets the whole allowance from
    // when they arrived, not what happens to be left of everyone else's.
    if (mpTurnSeconds() > 0) {
        for (const NetPeer& p : m_netHost->lobby().roster()) {
            if (p.spectator || p.countryId == 0) continue;
            if (m_mpDeadlineMs.find(p.peerId) == m_mpDeadlineMs.end())
                m_mpDeadlineMs[p.peerId] = nowMs + (long long)mpTurnSeconds() * 1000;
        }
    }

    // Resolve as soon as everyone has spoken, rather than sitting out the rest
    // of a countdown nobody is waiting on.
    const uint32_t turn = m_mpTurns->turnNumber();
    const std::vector<uint16_t> missing = m_netHost->lobby().missingSubmissions(turn);

    // Everyone is either in, or has run out of their own time. Waiting on
    // somebody whose clock expired ten minutes ago helps nobody -- and with
    // per-player clocks the last one to expire is what the turn waits for,
    // which is not the same as one shared countdown.
    bool allSettled = true;
    for (const uint16_t peer : missing) {
        const long long left = mpDeadlineLeft(peer, nowMs);
        if (left != 0) { allSettled = false; break; }
    }
    const bool timeUp = mpTurnSeconds() > 0 && m_mpTurns->due(nowMs);
    if (allSettled || timeUp) mpResolveTurn();
}

void Game::mpResolveTurn() {
    if (!mpIsHost() || !m_mpTurns) return;

    const uint32_t turn = m_mpTurns->turnNumber();

    // What each seat gets: its own orders, or the AI standing in. TurnRunner
    // decides, and says WHY, so the announcement is not guesswork.
    const std::vector<TurnResolution> resolutions =
        m_mpTurns->resolve(m_netHost->lobby(), turn);

    for (const TurnResolution& r : resolutions) {
        if (r.countryId == 0) continue;
        if (r.usePlayerOrders) {
            mpApplyOrders(r.countryId, r.orders);
        }
        // Anything substituted is ANNOUNCED, with the reason TurnRunner gave.
        // A country that silently behaves differently is worse than one that
        // explains itself -- and "the AI played you" is the single thing a
        // player most needs told.
        if (r.substitution != NetSubstitution::None) {
            m_netHost->announceSubstitution(r.countryId, r.substitution, r.announcement);
        }
    }

    // The host's own orders are already in this world; it never submitted them
    // to itself.
    m_mpTurns->stop();
    processTurn();

    // processTurn() appended the delta to the save; send exactly that, so what
    // players apply is what the host recorded rather than a second computation
    // of the same turn.
    if (!m_currentSavePath.empty()) {
        const TurnDelta delta = SaveManager::readTurn(m_currentSavePath, (int)turn);
        std::vector<uint8_t> packed = SaveManager::packTurn(delta);
        if (!packed.empty()) m_netHost->broadcastDelta(turn, packed);
    }
    m_netHost->lobby().clearSubmissions();
    m_mpDeadlineMs.clear();

    // Who holds what, written beside the save every turn. A campaign is
    // abandoned far more often than it is closed politely -- a crash, a closed
    // laptop -- so this cannot wait until shutdown to be durable.
    mpSaveSeats();
}

void Game::mpApplyDelta(uint32_t turnNumber, const std::vector<uint8_t>& payload) {
    TurnDelta delta;
    if (!SaveManager::unpackTurn(payload.data(), payload.size(), delta)) {
        mpNote("The server sent a turn this build could not read.", true);
        return;
    }
    applyTurnDelta(delta);
    synthesizeMissingRebels();
    rebuildOwnershipPixels();
    reloadBorders();

    m_turnNumber = (int)turnNumber;
    m_mpWaitingForTurn = false;

    // The orders just resolved; they are not pending any more.
    m_pendingMoveOrders.clear();
    m_pendingDisbandOrders.clear();
    m_pendingArtilleryOrders.clear();
    m_pendingShipMoveOrders.clear();
    m_pendingShipEngageOrders.clear();
    m_pendingShipBombardOrders.clear();
    m_pendingScrapShips.clear();

    recordIncomeSnapshot();
}

// ------------------------------------------------- inviting, and being found --

std::string Game::mpInviteText() const {
    if (!m_netHost) return {};

    // Both halves, because either alone is useless: the address says WHERE and
    // the code says WHICH GAME. A host pasting only one of them into a chat is
    // the most likely way this goes wrong, so the button hands over both.
    std::string address;
    if (m_mpTunnel && m_mpTunnel->state() == Tunnel::State::Up) {
        address = m_mpTunnel->address();
    } else if (m_mpBindAll) {
        address = "<your address>:" + std::to_string(m_netHost->listenPort());
    } else {
        address = "127.0.0.1:" + std::to_string(m_netHost->listenPort());
    }

    return "Join my OpenDoctrines game\n"
           "Address: " + address + "\n"
           "Code:    " + m_netHost->code() + "\n";
}

void Game::mpBeginReachTest() {
    if (!m_netHost) return;
    if (m_mpReach == MpReach::Testing) return;

    std::string address;
    if (m_mpTunnel && m_mpTunnel->state() == Tunnel::State::Up)
        address = m_mpTunnel->address();

    if (address.empty()) {
        // Nothing published, so nothing to test. Said plainly rather than
        // reporting a failure that would read as "the game is broken".
        m_mpReach = MpReach::Unreachable;
        m_mpReachNote = m_mpBindAll
            ? "Open to your own network only. People elsewhere cannot reach this "
              "unless you forward the port on your router."
            : "Not published at all -- this is bound to your own computer. Turn on "
              "the tunnel, or forward a port.";
        return;
    }

    if (m_mpReachProbe) { delete m_mpReachProbe; m_mpReachProbe = nullptr; }
    m_mpReachProbe = new WebSocket();

    // Dialled the same way a player would, over the public address. That is the
    // point: it exercises DNS, TLS, the tunnel and this listener, in that order,
    // rather than asserting they are fine.
    if (!m_mpReachProbe->connect(address + "/", false)) {
        m_mpReach = MpReach::Unreachable;
        m_mpReachNote = m_mpReachProbe->error().empty()
            ? "Could not reach the published address." : m_mpReachProbe->error();
        delete m_mpReachProbe;
        m_mpReachProbe = nullptr;
        return;
    }
    m_mpReach = MpReach::Testing;
    m_mpReachNote = "Checking from outside...";
    m_mpReachTimer = 0.0f;
}

void Game::mpUpdateReachTest() {
    if (m_mpReach != MpReach::Testing || !m_mpReachProbe) return;

    m_mpReachTimer += GetFrameTime();
    const WsState s = m_mpReachProbe->state();

    if (s == WsState::Open) {
        // The handshake completed, which means a stranger's connection would
        // have too. Dropped immediately: this was a question, not a player.
        m_mpReach = MpReach::Reachable;
        m_mpReachNote = "Reachable from outside. Anyone with the address and code "
                        "can join.";
    } else if (s == WsState::Closed) {
        m_mpReach = MpReach::Unreachable;
        m_mpReachNote = m_mpReachProbe->error().empty()
            ? "The published address did not answer. The tunnel may still be "
              "starting; try again in a moment."
            : m_mpReachProbe->error();
    } else if (m_mpReachTimer > 20.0f) {
        m_mpReach = MpReach::Unreachable;
        m_mpReachNote = "No answer from the published address within 20 seconds.";
    } else {
        return;   // still trying
    }

    m_mpReachProbe->close();
    delete m_mpReachProbe;
    m_mpReachProbe = nullptr;
}

// -------------------------------------------------- who the turn waits on ----
//
// A network turn that has not moved is either waiting on somebody or broken,
// and from the outside those look identical. This says which, by name, with
// the time left -- and gives the host the one control that was missing: go
// now, without the stragglers.

void Game::drawMpTurnPanel(int x, int bottomY) {
    const bool host = mpIsHost();
    if (!host && !mpIsClient()) return;

    const std::vector<NetPeer> roster = host ? m_netHost->lobby().roster()
                                             : m_netSession->roster();
    if (roster.empty()) return;

    // Rows for players only. A spectator is not somebody the turn waits on.
    std::vector<const NetPeer*> players;
    for (const NetPeer& p : roster)
        if (!p.spectator && p.countryId != 0) players.push_back(&p);
    if (players.empty()) return;

    const int rowH = 20, w = 320;
    const int headH = 46;
    const int hostRow = host ? 30 : 0;
    const int h = headH + (int)players.size() * rowH + 8 + hostRow;
    const int top = bottomY - h;

    DrawRectangleRounded({(float)x, (float)top, (float)w, (float)h}, 0.06f, 8,
                         Color{16, 18, 24, 232});
    DrawRectangleRoundedLines({(float)x, (float)top, (float)w, (float)h}, 0.06f, 8,
                              Color{70, 80, 100, 190});

    // The clock, in words rather than a raw count -- a long-form game's
    // remaining time is hours, and "6142" is not a number anyone can read.
    const int seconds = mpTurnSeconds();
    std::string clock;
    if (seconds == 0) {
        clock = "No timer -- waiting for everyone";
    } else if (host && m_mpTurns && m_mpTurns->running()) {
        const long long nowMs = (long long)(GetTime() * 1000.0);
        const int left = (int)(m_mpTurns->remainingMs(nowMs) / 1000);
        clock = left > 0 ? durationWords(left) + " left" : "time is up";
    } else {
        clock = durationWords(seconds) + " per turn";
    }
    DrawText("This turn", x + 10, top + 8, 15, Color{200, 210, 225, 255});
    DrawText(clock.c_str(), x + 10, top + 26, 13,
             seconds == 0 ? Color{195, 180, 135, 255} : Color{150, 190, 205, 255});

    int ry = top + headH;
    const uint32_t turn = host && m_mpTurns ? m_mpTurns->turnNumber()
                                            : (uint32_t)m_turnNumber + 1;
    for (const NetPeer* p : players) {
        const bool in = p->submitted;
        const Country* c = m_countries.getCountry((int)p->countryId);
        std::string label = p->name.empty() ? std::string("someone") : p->name;
        if (c) label += "  (" + c->name + ")";
        if ((int)label.size() > 30) label = label.substr(0, 29) + "...";
        DrawText(label.c_str(), x + 10, ry + 3, 13,
                 in ? Color{150, 200, 165, 255} : Color{200, 200, 210, 255});

        // Disconnected but submitted is DONE, not missing: they did their part
        // and closed the game, which with a long turn is ordinary.
        //
        // Everyone else shows THEIR OWN remaining time. A latecomer's clock
        // started when they arrived, so one shared countdown would have
        // reported a deadline that was never theirs.
        std::string mark;
        Color markColor;
        if (in) {
            mark = "ready";
            markColor = Color{130, 195, 150, 255};
        } else {
            const long long left = host ? mpDeadlineLeft(p->peerId,
                                              (long long)(GetTime() * 1000.0))
                                        : -1;
            if (left == 0) {
                mark = "out of time";
                markColor = Color{205, 155, 125, 255};
            } else if (left > 0) {
                mark = durationWords((int)(left / 1000)) + " left";
                markColor = left < 30000 ? Color{215, 175, 120, 255}
                                         : Color{165, 180, 200, 255};
            } else {
                mark = p->connected ? "thinking" : "away";
                markColor = p->connected ? Color{170, 175, 190, 255}
                                         : Color{195, 150, 130, 255};
            }
        }
        DrawText(mark.c_str(), x + w - MeasureText(mark.c_str(), 12) - 10, ry + 4, 12,
                 markColor);
        ry += rowH;
    }
    (void)turn;

    if (host) {
        const Vector2 m = getMouse();
        const Rectangle go{(float)(x + 10), (float)(ry + 4), (float)(w - 20), 24.0f};
        const bool hov = CheckCollisionPointRec(m, go);
        DrawRectangleRounded(go, 0.15f, 6, hov ? Color{78, 62, 36, 235}
                                               : Color{52, 44, 30, 225});
        DrawRectangleRoundedLines(go, 0.15f, 6, Color{170, 140, 90, 190});
        const char* t = "Resolve now, without the rest";
        DrawText(t, (int)go.x + ((int)go.width - MeasureText(t, 12)) / 2,
                 (int)go.y + 6, 12, Color{225, 205, 165, 255});
        if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !m_paused)
            mpForceResolve();
    }
}

// ------------------------------------------------------ the host's console ---
//
// Everything a host needs in order to answer "what is my server doing, and
// who is on it" without leaving the game -- and the two controls that only
// make sense mid-game: give up a seat nobody is coming back to, and stop
// waiting.
//
// Deliberately read-mostly. Settings that change how the table is played
// belong in the lobby, where everyone can see them change; this panel is for
// running the thing.

void Game::drawMpHostConsole(int x, int top) {
    if (!m_netHost) return;

    const int w = 380;
    const std::vector<NetPeer> roster = m_netHost->lobby().roster();
    const long long nowMs = (long long)(GetTime() * 1000.0);

    const int rowH = 22;
    const int h = 150 + (int)roster.size() * rowH + 40;
    DrawRectangleRounded({(float)x, (float)top, (float)w, (float)h}, 0.04f, 8,
                         Color{14, 16, 22, 238});
    DrawRectangleRoundedLines({(float)x, (float)top, (float)w, (float)h}, 0.04f, 8,
                              Color{75, 88, 110, 200});

    int y = top + 12;
    DrawText("Your server", x + 14, y, 19, Color{205, 218, 235, 255});
    y += 26;

    // ---- what this server is ------------------------------------------------
    auto line = [&](const std::string& label, const std::string& value, Color c) {
        DrawText(label.c_str(), x + 14, y, 13, Color{135, 145, 162, 255});
        DrawText(value.c_str(), x + 150, y, 13, c);
        y += 18;
    };

    line("Invite code", m_netHost->code().empty() ? "--" : m_netHost->code(),
         Color{200, 215, 235, 255});

    std::string where;
    Color whereColor{190, 200, 215, 255};
    if (m_mpTunnel && m_mpTunnel->state() == Tunnel::State::Up) {
        where = m_mpTunnel->address();
        whereColor = Color{150, 205, 170, 255};
    } else if (m_mpBindAll) {
        where = "your network, port " + std::to_string(m_netHost->listenPort());
    } else {
        where = "this computer only, port " + std::to_string(m_netHost->listenPort());
        whereColor = Color{200, 185, 140, 255};
    }
    if (where.size() > 34) where = where.substr(0, 33) + "...";
    line("Reachable at", where, whereColor);

    line("Turn", std::to_string(m_turnNumber), Color{190, 200, 215, 255});
    line("Turn length", mpTurnSeconds() == 0 ? "no timer"
                                             : durationWords(mpTurnSeconds()),
         mpTurnSeconds() == 0 ? Color{200, 185, 140, 255}
                              : Color{190, 200, 215, 255});

    int seated = 0, here = 0;
    for (const NetPeer& p : roster) {
        if (p.countryId != 0 && !p.spectator) seated++;
        if (p.connected) here++;
    }
    line("Players", std::to_string(here) + " here of " + std::to_string(seated) +
                    " seated  (max " + std::to_string(m_mpMaxPlayers) + ")",
         Color{190, 200, 215, 255});

    y += 6;
    DrawRectangle(x + 14, y, w - 28, 1, Color{60, 70, 88, 180});
    y += 8;

    // ---- who is on it -------------------------------------------------------
    const Vector2 mouse = getMouse();
    const bool pressed = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);

    for (const NetPeer& p : roster) {
        const Country* c = m_countries.getCountry((int)p.countryId);
        std::string name = p.name.empty() ? std::string("someone") : p.name;
        if (c) name += "  (" + c->name + ")";
        if (name.size() > 26) name = name.substr(0, 25) + "...";
        DrawText(name.c_str(), x + 14, y + 4, 13,
                 p.connected ? Color{205, 212, 225, 255} : Color{150, 156, 170, 255});

        // A seat nobody is coming back to can be given up from here too --
        // the lobby is not reachable without ending the game.
        if (!p.connected && !p.psid.empty()) {
            const Rectangle rel{(float)(x + w - 84), (float)(y + 1), 70.0f, 19.0f};
            const bool hov = CheckCollisionPointRec(mouse, rel);
            DrawRectangleRounded(rel, 0.2f, 6, hov ? Color{72, 40, 40, 235}
                                                   : Color{44, 34, 34, 215});
            DrawRectangleRoundedLines(rel, 0.2f, 6, Color{140, 105, 105, 190});
            DrawText("release", (int)rel.x + 12, (int)rel.y + 4, 11,
                     Color{215, 175, 175, 255});
            if (hov && pressed && m_netHost->lobby().releaseSeat(p.psid)) {
                m_netHost->broadcastLobby();
                mpSaveSeats();
            }
        } else {
            const long long left = mpDeadlineLeft(p.peerId, nowMs);
            std::string state = p.submitted ? "ready"
                              : left == 0   ? "out of time"
                              : left > 0    ? durationWords((int)(left / 1000)) + " left"
                                            : "thinking";
            DrawText(state.c_str(), x + w - MeasureText(state.c_str(), 12) - 14,
                     y + 5, 12,
                     p.submitted ? Color{135, 195, 155, 255}
                                 : Color{170, 178, 195, 255});
        }
        y += rowH;
    }

    // ---- stop waiting -------------------------------------------------------
    if (m_mpTurns && m_mpTurns->running()) {
        const Rectangle go{(float)(x + 14), (float)(y + 8), (float)(w - 28), 26.0f};
        const bool hov = CheckCollisionPointRec(mouse, go);
        DrawRectangleRounded(go, 0.15f, 6, hov ? Color{80, 64, 38, 240}
                                               : Color{54, 46, 32, 228});
        DrawRectangleRoundedLines(go, 0.15f, 6, Color{175, 145, 95, 195});
        const char* t = "Resolve this turn now";
        DrawText(t, (int)go.x + ((int)go.width - MeasureText(t, 13)) / 2,
                 (int)go.y + 7, 13, Color{228, 208, 168, 255});
        if (hov && pressed) mpForceResolve();
    }
}
