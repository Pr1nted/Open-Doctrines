// The communication window, in the game.
//
// Three small pieces, kept out of the render and update files because the one
// rule that matters here is a threading-of-frames rule and it is easier to see
// on its own:
//
//   toggleComms()  builds the window the first time it is asked for
//   updateComms()  advances it -- and REBUILDS ITS PICTURE, so it must run
//                  from Game::update(), outside any BeginDrawing block
//   drawComms()    puts it on screen, inside one
//
// raylib cannot nest render targets. Calling update() while the game's frame
// is open closes the frame instead, and everything drawn afterwards goes to
// the window rather than to wherever it was aimed -- silently.

#include "Game.h"
#include "util/LoadLog.h"
#include "Audio.h"
#include "GameInternals.h"
#include "ai/AISystem.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>

#include "json.hpp"
#include "comms/CastFile.h"

void Game::toggleComms() {
    if (!m_commsBuilt) {
        // Needs a GL context, so it cannot be built with the rest of Game.
        // Both slots are built together: the second is idle and costs a
        // render target, and building it lazily would mean compiling a
        // shader in the middle of a conversation.
        if (!m_comms.open() || !m_comms2.open()) {
            addNotification("Communication window unavailable", RED);
            return;
        }
        m_commsBuilt = true;

        comms::Profile advisor;
        advisor.image = m_dataDir + "comms/advisor.png";
        advisor.hat = true;
        advisor.browDrop = 0.55f;
        advisor.seed = 11;
        m_comms.setProfile(advisor);
        m_comms.setSpeaker("Advisor");
        m_comms.setSignal({0.88f, 1.0f});
        m_comms2.setSignal({0.80f, 1.0f});
    }
    m_commsOpen = !m_commsOpen;
    if (m_commsOpen) {
        m_comms.lookWander();
        m_comms.blinkSoon();
        m_comms.tuneIn();
    } else {
        m_comms.tuneOut();
        m_comms2.tuneOut();
        m_commsSlot[0].clear();
        m_commsSlot[1].clear();
    }
}

int Game::commsSlots() const {
    // A window that is on its way out still occupies its side of the screen.
    //
    // Counting only the slots meant the layout changed on the frame somebody
    // hung up -- their window vanished and the other one jumped across the
    // map to the middle, both in the same frame, which reads as a glitch
    // rather than as somebody leaving. The seat is theirs until the picture
    // has actually gone; see Transmission::power().
    int n = 0;
    for (int i = 0; i < 2; ++i)
        if (!m_commsSlot[i].empty() || commsAt(i).onAir()) n++;
    return n;
}

Rectangle Game::commsBounds(int slot) const {
    const Rectangle whole = commsBounds();
    // One on the link: the stage is theirs, exactly as before.
    if (commsSlots() < 2) return whole;
    // Two: OPPOSITE SIDES of the screen, facing each other across it.
    //
    // Side by side in the corner read as one panel with two pictures in it.
    // Apart, with the map between them, it reads as two people on a call --
    // and it leaves the middle of the map clear, which is where the tutorial
    // wants to be pointing.
    //
    // Slots keep their side rather than swapping to follow whoever is
    // talking: a portrait that jumps across the screen every time the other
    // person answers is unreadable in a back-and-forth.
    // The right side is the window's usual home and is clear. The LEFT is
    // not: the country header lives at 0,0 and the province panel hangs off
    // it, so a portrait dropped in that corner sits on the player's own
    // treasury. The left one starts below the header and is sized to finish
    // above the textbox.
    const float w = whole.width * 0.92f;
    // Room for the header AND for the window's own nameplate, which is drawn
    // ABOVE the frame rather than inside it -- measuring only the frame put
    // the name on top of the player's balance.
    const float headerH = 126.0f;
    const float floorY = dialogueBounds().y - 12.0f;
    const float leftH = std::max(180.0f, std::min(whole.height * 0.92f, floorY - headerH));
    const float rightH = whole.height * 0.92f;
    if (slot == 0)
        return {whole.x + whole.width - w, whole.y + (whole.height - rightH) * 0.5f, w, rightH};
    return {24.0f, headerH, w * (leftH / std::max(1.0f, rightH)), leftH};
}

Rectangle Game::commsBounds() const {
    // The stage the window may use: what is left of the screen once the HUD
    // has taken its share. The country panel owns the left edge, the view
    // buttons the right, and the turn bar the bottom -- a window placed on the
    // raw screen rectangle lands on top of all three.
    const Rectangle stage{380.0f, 40.0f,
                          std::max(320.0f, (float)m_screenW - 380.0f - 130.0f),
                          std::max(240.0f, (float)m_screenH - 40.0f - 110.0f)};
    return comms::Transmission::suggestedBounds(stage, /*rightSide=*/true);
}

void Game::updateComms(float dt) {
    if (!m_commsBuilt) return;
    // Kept updating while it drops out, or the static would freeze on screen.
    const bool anyOn = m_comms.onAir() || m_comms2.onAir();
    if (!m_commsOpen && !anyOn) {
        // Nothing on the link, and this function is about to stop running --
        // so forget where the windows were. Otherwise the next conversation
        // eases its first window in from wherever the last one left off,
        // which is a portrait sliding across the map for no reason.
        m_commsRectOn[0] = m_commsRectOn[1] = false;
        return;
    }
    if (!anyOn) m_commsOpen = false;

    // getMouse(), not GetMousePosition(): raylib reports the pointer in
    // PHYSICAL pixels and everything here is laid out in the logical space the
    // rest of the game hit-tests in. On a 2x display the raw number is half of
    // where the player is actually pointing.
    const Vector2 mouse = getMouse();
    const bool pair = commsSlots() > 1;

    // WHERE EACH WINDOW SITS, EASED.
    //
    // The rectangle a window wants depends on how many are up, so the moment
    // one of them finishes leaving the other is owed a different piece of the
    // screen. Cutting there is a jump; this walks it across in about a fifth
    // of a second. A window that is OFF is parked on its target, so the one
    // thing that never animates is arriving -- coming on is the tube opening,
    // and a portrait that also slid in from somewhere would be two entrances.
    for (int i = 0; i < 2; ++i) {
        const Rectangle want = commsBounds(i);
        const bool on = commsAt(i).onAir();
        if (!on || !m_commsRectOn[i]) {
            m_commsRect[i] = want;
        } else {
            const float k = 1.0f - std::exp(-dt * 11.0f);
            m_commsRect[i].x += (want.x - m_commsRect[i].x) * k;
            m_commsRect[i].y += (want.y - m_commsRect[i].y) * k;
            m_commsRect[i].width  += (want.width  - m_commsRect[i].width)  * k;
            m_commsRect[i].height += (want.height - m_commsRect[i].height) * k;
        }
        m_commsRectOn[i] = on;
    }
    for (int i = 0; i < 2; ++i) {
        comms::Transmission& t = commsAt(i);
        // A window with somebody in it is updated whether or not it is on air
        // YET. Skipping on !onAir() deadlocked the second slot: tuneIn() only
        // takes effect inside update(), so a window that was never updated
        // never came on air, and a window that was not on air was never
        // updated. Mia was handed her slot and simply never appeared.
        if (m_commsSlot[i].empty() && !t.onAir()) continue;
        // The listener sits on a worse signal. With two identical windows
        // side by side there is nothing to say which of them is talking, and
        // the eye picks the wrong one about half the time; a step down in
        // gain and reception reads instantly and needs no label.
        const bool active = (i == m_commsActive);
        t.setSignal(active || !pair ? comms::Signal{0.88f, 1.00f}
                                    : comms::Signal{0.55f, 0.72f});
        // The eyes follow the pointer while it is over THAT window, and look
        // away again when it leaves -- the whole of the interaction, and
        // enough to make the link feel attended to.
        const Rectangle box = commsBounds(i);
        if (CheckCollisionPointRec(mouse, box)) {
            t.lookAt({(mouse.x - (box.x + box.width * 0.5f)) / (box.width * 0.5f),
                      (mouse.y - (box.y + box.height * 0.45f)) / (box.height * 0.45f)});
        } else {
            t.lookWander();
        }
        t.update(dt);
    }
}

void Game::drawComms() {
    if (!m_commsBuilt) return;
    // The listener first, so the speaker's window overlaps theirs rather than
    // the other way round -- whoever is talking should be the one in front.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < 2; ++i) {
            const bool active = (i == m_commsActive);
            if (active != (pass == 1)) continue;
            if (!commsAt(i).onAir()) continue;
            // An empty slot with a window still on air is one of two things:
            // somebody LEAVING, whose picture should be watched out; or a
            // window nobody has been put in, which used to paint the previous
            // conversation's face beside the current one. The difference is
            // whether the tube is closing, which is exactly what leaving()
            // answers.
            if (m_commsSlot[i].empty() && !commsAt(i).leaving()) continue;
            commsAt(i).draw(m_commsRect[i], hexToColor(m_config.accent()));
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// The tutorial
//
// A .oddlg script played as a conversation: the words in the textbox along the
// bottom, whoever is speaking in the communication window. The script names its
// speakers by @Name; each one is given a silhouette here, except the advisor,
// who has drawn art.
// ────────────────────────────────────────────────────────────────────────────

void Game::startTutorial() {
    // The opening conversation first, and it plays on the MENU. It is written
    // for a player who has no country yet -- Pr1nted asks who this is and
    // hands them to Mia -- so loading a world underneath it would be a world
    // nobody has mentioned, sitting there through the whole exchange.
    //
    // startTutorialWorld() is what actually loads, once he has handed over.
    // Everything a previous run could have left behind. A tutorial abandoned
    // half way is the normal case, not the exceptional one -- somebody tries
    // it, leaves, and comes back -- and any one of these still set is a
    // second tutorial that behaves like the tail of the first.
    m_dialogReturnTo.clear();
    m_tutorialPending = false;
    m_introRunning = false;
    m_commsSlot[0].clear();
    m_commsSlot[1].clear();
    m_commsActive = 0;
    if (m_dialogOpen) endDialogue();

    m_tutorialTurnUnlocked = false;
    m_tutorialMode = true;
    m_tutorialTrack.clear();
    m_introRunning = true;
    Audio::get().playSfx("confirm");
    beginDialogue("intro");
}

void Game::stopTutorial() {
    // Out, and all the way out: the dialogue, the pointer, the gate, the
    // hand-written opponents and the world itself. Half of it left running is
    // a main menu with a tutorial still holding the input.
    //
    // THE FLAGS GO FIRST. endDialogue acts on them -- m_introRunning means
    // "load the tutorial world next" -- so closing the box before clearing
    // them started the very thing this function exists to stop.
    m_dialogReturnTo.clear();
    m_introRunning = false;
    m_tutorialPending = false;
    endDialogue();
    m_tutorialMode = false;
    m_tutorialTrack.clear();
    m_tutorialTurnUnlocked = false;
    m_uiTargets.clear();
    AISystem::s_tutorialAI = false;
    m_tutorialStopRect = {0, 0, 0, 0};

    m_inSettings = false;
    m_paused = false;
    // Nothing was ever written for this world, so there is nothing to warn
    // about and nothing to keep.
    m_unsavedChanges = false;
    m_currentSavePath.clear();
    unloadGameData();
    initMenuBackground();
    m_currentScreen = SCREEN_MENU;
}

void Game::startTutorialWorld() {
    // Two islands, three small countries, stable province ids: the script
    // points at particular ground. See tools/make_tutorial_map.py.
    //
    // The save creation is switched OFF -- a practice world that turns up in
    // Load World a week later is one the player has to work out how to delete.
    m_tutorialPending = true;
    startNewGameWithName(m_dataDir + "STDmaps/tutorial.odmap", "Tutorial");
    // AFTER the call: startNewGameWithName clears the flag, because it is the
    // door every ordinary new world comes through too.
    m_tutorialMode = true;
    // Hand-written opponents for the whole tutorial: see AISystem::s_tutorialAI.
    AISystem::s_tutorialAI = true;
    m_loadingShouldCreateSave = false;
    m_currentSavePath.clear();
    m_quickStartPending = true;      // still skip country select
    m_forcedStartIso = "ASH";        // ...and it is Ashford, by name
}

void Game::loadCommsCast() {
    if (m_castLoaded) return;
    m_castLoaded = true;                       // once, success or not
    const std::string path = m_dataDir + "comms/cast.json";
    std::ifstream in(path);
    if (!in) return;                           // no cast file: everyone is a silhouette
    try {
        nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
        // Bound to a named value first. j.value() returns BY VALUE, and
        // iterating .items() straight off that temporary walks a destroyed
        // object -- it segfaults on the first character.
        const nlohmann::json chars = j.value("characters", nlohmann::json::object());
        for (auto& [name, c] : chars.items()) {
            comms::Profile p;
            comms::profileFromJson(c, m_dataDir + "comms/", p);
            p.seed = comms::seedFromName(name);
            m_cast[name] = p;
            m_castVoice[name] = c.value("voice", std::string("narrator"));
            m_castLabel[name] = {c.value("display", name),
                                 c.value("role", std::string()),
                                 c.value("short", std::string())};
        }
    } catch (const std::exception& e) {
        LoadLog() << "[comms] " << path << ": " << e.what() << std::endl;
    }
}

// ── A map's own cast ──
//
// comms/cast.json inside the .odmap, with its portraits beside it. Read from
// the archive already in memory, so nothing is written to disk, and layered
// OVER the base cast: a map may add characters, and may replace one of the
// game's own for its own story, but only while it is loaded.
void Game::loadMapCast() {
    loadCommsCast();                       // the base cast first, so a map layers over it
    for (const std::string& n : m_mapCastNames) { m_cast.erase(n); m_castVoice.erase(n); m_castLabel.erase(n); }
    m_mapCastNames.clear();

    auto it = m_odmJsonData.find("comms/cast.json");
    if (it == m_odmJsonData.end()) return;
    try {
        nlohmann::json j = nlohmann::json::parse(it->second, nullptr, true, true);
        const nlohmann::json chars = j.value("characters", nlohmann::json::object());
        for (auto& [name, c] : chars.items()) {
            comms::Profile p;
            comms::profileFromJson(c, "", p);     // no directory: the art is in the archive
            if (!p.image.empty()) {
                auto img = m_odmJsonData.find("comms/" + p.image);
                if (img != m_odmJsonData.end()) p.imageBytes = img->second;
                else LoadLog() << "[comms] map cast '" << name << "': no comms/" << p.image
                               << " in the archive" << std::endl;
                p.image.clear();                  // it is bytes now, not a path
            }
            p.seed = comms::seedFromName(name);
            m_cast[name] = p;
            m_castVoice[name] = c.value("voice", std::string("narrator"));
            m_castLabel[name] = {c.value("display", name),
                                 c.value("role", std::string()),
                                 c.value("short", std::string())};
            m_mapCastNames.push_back(name);
        }
        LoadLog() << "[comms] map cast: " << m_mapCastNames.size() << " character(s)" << std::endl;
    } catch (const std::exception& e) {
        LoadLog() << "[comms] map cast.json: " << e.what() << std::endl;
    }
}

// A dialogue the map carries. Same markup and the same window as the
// tutorial's, parsed straight out of the archive.
bool Game::beginMapDialogue(const std::string& name) {
    loadMapCast();
    // The active language first, then the map's default, so a map may ship
    // translations the way the game does.
    const std::string lang = od::i18n::language();
    const char* tries[] = {nullptr, nullptr, nullptr};
    const std::string a = "dialog/" + lang + "/" + name + ".oddlg";
    const std::string b = "dialog/en/" + name + ".oddlg";
    const std::string c = "dialog/" + name + ".oddlg";
    const std::string* order[] = {&a, &b, &c};
    (void)tries;
    for (const std::string* pth : order) {
        auto d = m_odmJsonData.find(*pth);
        if (d == m_odmJsonData.end()) continue;
        dlg::Script sc = dlg::parse(d->second);
        beginDialogue("");                       // wire the resolvers and reset state
        m_dialogScript = name;
        m_dialog.openScript(std::move(sc));
        m_dialogOpen = true;
        return true;
    }
    return false;
}

void Game::commsSpeaker(const std::string& speaker) {
    if (!m_commsBuilt) return;
    // Narration has no face. A page with no @speaker is the script talking,
    // not a person -- giving it a slot put a nameless silhouette on the link
    // and pushed one of the actual speakers off it.
    if (speaker.empty()) return;
    loadCommsCast();

    // Already on the link: they simply start talking. This is the whole point
    // of the two slots -- Pr1nted asking Mia a question and Mia answering is
    // ONE call with two people in it, not two calls.
    // Keyed on the NAME, not on onAir(). A window that was handed a speaker
    // this frame does not report itself on air until the next update, so
    // asking the picture whether somebody is there says "no" for one frame --
    // long enough for the next page to hand their slot to somebody else.
    for (int i = 0; i < 2; ++i) {
        if (m_commsSlot[i] == speaker) {
            m_commsActive = i;
            commsAt(i).blinkSoon();
            return;
        }
    }

    // A free slot if there is one, otherwise the person who is NOT talking
    // gives theirs up -- the speaker before this one stays, because they are
    // the one being replied to.
    int slot = -1;
    for (int i = 0; i < 2; ++i)
        if (m_commsSlot[i].empty()) { slot = i; break; }
    if (slot < 0) slot = 1 - m_commsActive;

    auto it = m_cast.find(speaker);
    comms::Profile p;
    if (it != m_cast.end()) {
        p = it->second;
    } else {
        // Not in the cast: a silhouette built from the name itself, so a
        // script can introduce somebody before anyone has drawn them and they
        // still look like a particular person rather than a default one.
        uint32_t h = 2166136261u;
        for (unsigned char c : speaker) { h ^= c; h *= 16777619u; }
        p.seed = h ? h : 1;
        p.headW  = 0.40f + (float)(h % 100) / 1000.0f;
        p.headH  = 0.36f + (float)((h >> 7) % 90) / 1000.0f;
        p.shoulderW = 0.82f + (float)((h >> 13) % 260) / 1000.0f;
        p.eyeGap = 0.170f + (float)((h >> 19) % 50) / 1000.0f;
        p.browDrop = (float)((h >> 23) % 90) / 100.0f;
        p.hat = ((h >> 5) & 1) != 0;
    }

    auto v = m_castVoice.find(speaker);
    m_speakerVoice = (v != m_castVoice.end()) ? v->second : std::string("narrator");

    // THE NAME AND THE ROLE ARE TRANSLATED; THE KEY IS NOT.
    //
    // A speaker is an id -- cast.json says so, and a .oddlg keys on it, so it
    // must read the same in every language. What the player sees is the
    // `display` and `role` beside it, and those are ordinary prose: "lazy
    // advisor" has no business staying English on a Ukrainian screen. Resolved
    // here rather than at load, because the language can change after the cast
    // file has been read and the label would otherwise be stuck in whatever
    // language was active at startup.
    auto lb = m_castLabel.find(speaker);
    const std::string shown = (lb != m_castLabel.end() && !lb->second.display.empty())
                            ? std::string(T(lb->second.display))
                            : (speaker.empty() ? std::string(T("Transmission")) : speaker);

    m_commsSlot[slot] = speaker;
    m_commsActive = slot;
    // Through a dropout, so somebody arriving on the link never simply
    // appears where the last person was standing.
    // NO tuneIn() here. changeSpeaker already calls it on a window that is
    // dark, and on one that is already lit it hides the swap behind a burst
    // of static -- which it does by driving the static UP. Tuning in drives
    // it down, so calling both cancelled the swap: the window stayed on the
    // face it already had, for the rest of the conversation.
    commsAt(slot).changeSpeaker(p, shown,
                                lb != m_castLabel.end() ? std::string(T(lb->second.role))
                                                        : std::string(),
                                lb != m_castLabel.end() ? lb->second.tag  : std::string());
}

/// Take whoever is speaking off the link, leaving anyone else on it.
void Game::commsHangUp() {
    if (!m_commsBuilt) return;
    commsAt(m_commsActive).tuneOut();
    m_commsSlot[m_commsActive].clear();
    // Whoever is left becomes the one being looked at.
    const int other = 1 - m_commsActive;
    if (!m_commsSlot[other].empty()) m_commsActive = other;
}

Rectangle Game::dialogueBounds() const {
    // Along the bottom, clear of the communication window above it and of the
    // turn bar below.
    const float margin = 28.0f;
    float h = std::max(160.0f, m_screenH * 0.24f);

    // A page offering choices needs room for them UNDER its own text. Five
    // options below a two-line question do not fit in the height a line of
    // dialogue needs, and the box is what has to give -- shrinking the type
    // or the padding to make them fit would make every page worse to read for
    // the sake of the few that ask something.
    if (const dlg::Page* page = m_dialog.currentPage())
        if (!page->choices.empty())
            h += page->choices.size() * std::max(12, page->style.size - 2) * 1.9f;

    return {margin, m_screenH - h - 96.0f, m_screenW - margin * 2.0f, h};
}

float Game::emotionFor(const dlg::Page& page) const {
    // A pose says it outright.
    static const struct { const char* pose; float mood; } kPoses[] = {
        {"happy", 0.80f}, {"smile", 0.60f}, {"hands_up", 0.35f}, {"point", 0.15f},
        {"shrug", 0.05f}, {"think", -0.15f}, {"grim", -0.55f}, {"sad", -0.70f},
        {"angry", -0.85f},
    };
    for (const auto& k : kPoses)
        if (page.pose == k.pose) return k.mood;

    // Otherwise the line itself does. Crude, but it costs nothing and it is
    // right far more often than a neutral face would be: a shout is not
    // delivered deadpan, and a sentence trailing off is not delivered brightly.
    std::string text;
    for (const dlg::Span& sp : page.spans) text += sp.text;
    if (text.find("...") != std::string::npos || text.find("\u2026") != std::string::npos)
        return -0.30f;
    if (text.find('!') != std::string::npos) return 0.45f;
    if (text.find('?') != std::string::npos) return 0.10f;
    return 0.0f;
}

void Game::beginDialogue(const std::string& script) {
    m_dialogScript = script;
    m_dialogReturnTo.clear();
    m_dialog.setFonts(m_defaultFont, m_gameFont);
    // {key=army_move} -> whatever that action is bound to right now. Read
    // through m_config every time rather than captured once, so rebinding a
    // key mid-tutorial changes what the next page says.
    m_dialog.setLabelResolver([this](const std::string& speaker) -> dlg::Box::Label {
        loadCommsCast();
        auto it = m_castLabel.find(speaker);
        if (it == m_castLabel.end()) return {speaker, "", ""};
        // Same as the plate above: translated on the way out, not on the way in.
        return {T(it->second.display), T(it->second.role), it->second.tag};
    });
    m_dialog.setKeyResolver([this](const std::string& id) -> std::string {
        for (int i = 0; i < ACTION_COUNT; ++i)
            if (id == ACTION_IDS[i]) return keyName(m_config.keybinds[i]);
        LoadLog() << "[dialogue] no such action \"" << id << "\" for {key=}" << std::endl;
        return "?";
    });
    // THE ACTIVE LANGUAGE, not "en".
    //
    // This was hardcoded, which meant data/dialog/<lang>/ was a mechanism that
    // existed, was documented in DialogBox::open, had a Ukrainian file sitting
    // in it -- and was never once reached. Every player saw English dialogue
    // whatever they had selected. Box::open already falls back to en/ for any
    // script a language has not translated, so passing the real language is
    // all that was ever needed.
    if (!m_dialog.open(m_dataDir + "dialog", script, od::i18n::language())) {
        addNotification("No dialogue script: " + script, RED);
        return;
    }
    for (const std::string& w : m_dialog.warnings())
        LoadLog() << "[dialogue] " << w << std::endl;

    m_dialogOpen = true;
    m_dialogPage = -1;
    m_lastRevealed = 0;
    m_commsShowing.clear();
    m_commsPose.clear();
    m_commsSlot[0].clear();
    m_commsSlot[1].clear();
    m_commsActive = 0;
    if (!m_commsBuilt) toggleComms();     // builds it
    m_commsOpen = true;
    m_comms.tuneIn();

    // Put the FIRST page's speaker up immediately. updateDialogue only reacts
    // when the page index CHANGES, and it does not change on the first frame
    // -- so without this the window opens on whoever was last on the link and
    // the opening line is delivered by the wrong face.
    if (const dlg::Page* first = m_dialog.currentPage()) {
        m_commsShowing = first->speaker;
        m_commsPose = first->pose;
        commsSpeaker(first->speaker);
        m_comms.setEmotion(emotionFor(*first));
    }
    m_dialogPage = m_dialog.pageIndex();

    // AND RUN THE FIRST PAGE'S ACT, for the same reason.
    //
    // updateDialogue only runs an act when the page index CHANGES, and it does
    // not change on the page a script opens on -- so an act written on page 0
    // was silently thrown away. outro.oddlg opens with act=to_menu, which is
    // the whole point of that page: Mia takes you back up to the menu and
    // talks about saves and multiplayer while pointing at them. Without this
    // the sign-off played over the map, pointing at buttons that were not on
    // the screen.
    //
    // Last, so the act sees the window already open and can hang up on it --
    // and after m_dialogReturnTo has been cleared, so act=then: sticks.
    if (const dlg::Page* first = m_dialog.currentPage()) {
        const std::string act = first->act;   // by value: an act may reload
        tutorialAct(act);
    }
}

void Game::offerUiTarget(const std::string& name, Rectangle r) {
    // Only while a tutorial is actually running. Every panel in the game
    // would otherwise pay for a map insert per element per frame to answer a
    // question nobody is asking.
    if (!m_dialogOpen) return;
    m_uiTargets[name] = {r, m_uiFrame};
}

bool Game::tutorialFocus(Rectangle& out, bool& round) const {
    if (!m_dialogOpen) return false;
    const dlg::Page* page = m_dialog.currentPage();
    if (!page || page->pointAt.empty()) return false;
    auto it = m_uiTargets.find(page->pointAt);
    // One frame of slack: the gate is asked during update, and the rectangle
    // it is asking about was offered during the PREVIOUS frame's draw.
    if (it == m_uiTargets.end() || it->second.frame + 1 < m_uiFrame) return false;
    out = it->second.rect;
    round = page->pointRound;
    return true;
}

bool Game::tutorialConditionMet(const std::string& cond) {
    const size_t colon = cond.find(':');
    const std::string what = cond.substr(0, colon);
    const std::string arg  = (colon == std::string::npos) ? "" : cond.substr(colon + 1);

    if (what == "open" || what == "closed") {
        // "closed" is not a luxury. A page that gets the player INTO a panel
        // and then carries on has left them in it: the panel covers the map,
        // and the next thing the tutorial points at is behind it. Every way
        // in needs a way out, and the script has to be able to wait for it.
        const bool want = (what == "open");
        if (arg == "economy")  return m_inEconomy  == want;
        if (arg == "research") return m_inResearch == want;
        if (arg == "politics") return m_inPolitics == want;
        if (arg == "claims")   return m_inClaims   == want;
        if (arg == "any")
            return (m_inEconomy || m_inResearch || m_inPolitics || m_inClaims) == want;
    } else if (what == "war") {
        // At war with that country, looked up both ways round: the table
        // records the relation once, under whichever side entered it, and a
        // war the OTHER country declared is still a war.
        const Country* me = m_countries.getCountry(m_playerCountryId);
        if (!me) return false;
        auto side = [&](const std::string& a, const std::string& b) {
            auto r = m_relations.find(a);
            if (r == m_relations.end()) return false;
            auto st = r->second.find(b);
            return st != r->second.end() && st->second.war;
        };
        return side(me->isoA3, arg) || side(arg, me->isoA3);
    } else if (what == "filed") {
        // A diplomatic order of that kind is QUEUED BY THE PLAYER.
        //
        // Deliberately not "the alliance exists": whether it exists is the
        // other country's decision, and in the tutorial that country is a
        // hand-written turtle which may simply say no. A page waiting on
        // somebody else's answer is a page that can hang forever. Every
        // condition a lesson waits on has to be one the player can satisfy
        // alone -- filing the request is that; having it accepted is not.
        const Country* me = m_countries.getCountry(m_playerCountryId);
        if (!me) return false;
        for (const auto& da : m_pendingDiplomaticActions)
            if (da.sourceIso == me->isoA3 && da.action == arg) return true;
        return false;
    } else if (what == "selected") {
        // Something is picked on the map, and whose it is.
        const int pid = m_renderer ? m_renderer->getSelectedProvinceId() : 0;
        if (pid <= 0) return false;
        auto it = m_provinces.getAllProvinces().find(pid);
        if (it == m_provinces.getAllProvinces().end()) return false;
        const int owner = it->second.countryId;
        if (arg == "any")     return true;
        if (arg == "own")     return owner == m_playerCountryId;
        if (arg == "foreign") return owner > 0 && owner != m_playerCountryId;
        if (arg == "atpeace") {
            // Foreign AND not at war. The diplomacy lesson needs this: a
            // country you are at war with offers one entry, so a page that
            // accepts any foreign province and then asks for a pact can be
            // satisfied in a way that makes the NEXT page impossible.
            if (owner <= 0 || owner == m_playerCountryId) return false;
            const Country* me = m_countries.getCountry(m_playerCountryId);
            const Country* them = m_countries.getCountry(owner);
            if (!me || !them) return false;
            auto side = [&](const std::string& a, const std::string& b) {
                auto r = m_relations.find(a);
                if (r == m_relations.end()) return false;
                auto st = r->second.find(b);
                return st != r->second.end() && st->second.war;
            };
            return !side(me->isoA3, them->isoA3) && !side(them->isoA3, me->isoA3);
        }
        return false;
    } else if (what == "recruiting") {
        // An order is in: the province panel's Recruit button has been used
        // this turn. Waiting for the MEN would mean waiting for a turn to
        // resolve, and the lesson wants to see the order given.
        return !m_pendingRecruitments.empty();
    } else if (what == "view") {
        // The bottom bar's eight views. A naval lesson that explains the ship
        // orders while the player is looking at the population map is a
        // lesson about a screen they cannot see.
        static const char* kViews[] = {"population", "industry", "defence", "relations",
                                       "army", "navy", "resources", "names"};
        for (int i = 0; i < 8; ++i)
            if (arg == kViews[i]) return m_activeViewTab == i + 1;
    } else if (what == "province") {
        // The player holds that particular ground.
        const int pid = atoi(arg.c_str());
        auto it = m_provinces.getAllProvinces().find(pid);
        return it != m_provinces.getAllProvinces().end() &&
               it->second.countryId == m_playerCountryId;
    } else if (what == "destroyed") {
        // Nobody is left flying that flag. Counted over provinces rather than
        // asked of the country, because a country with no ground still exists
        // as a record long after it has stopped being a country.
        for (const auto& [pid, pr] : m_provinces.getAllProvinces()) {
            (void)pid;
            const Country* c = m_countries.getCountry(pr.countryId);
            if (c && c->isoA3 == arg) return false;
        }
        return true;
    } else if (what == "rebels") {
        // Any live rebel country at all. The tutorial's rebellion is the only
        // one that can exist on this map, so "none left" means put down.
        if (arg == "none") {
            for (const auto& [pid, pr] : m_provinces.getAllProvinces()) {
                (void)pid;
                if (pr.countryId >= REBEL_CID_MIN && pr.countryId < SPC_CID) return false;
            }
            return true;
        }
    } else if (what == "turns") {
        return m_turnNumber - m_dialogPageTurn >= atoi(arg.c_str());
    }

    if (m_uiTargetsMissing.insert("cond:" + cond).second)
        LoadLog() << "[tutorial] unknown condition \"" << cond
                  << "\" -- treating it as already met" << std::endl;
    return true;
}

void Game::tutorialAct(const std::string& act) {
    if (act.empty()) return;
    if (act == "rebellion") {
        // Staged, not simulated. The lesson needs a rebellion at exactly this
        // point, in ground the player has just taken, and waiting for unrest
        // to produce one on its own means a tutorial that sometimes does not
        // happen.
        std::vector<int> faction;
        int parent = m_playerCountryId;
        for (const auto& [pid, pr] : m_provinces.getAllProvinces())
            if (pr.countryId == m_playerCountryId) faction.push_back(pid);
        if (faction.size() < 2) {
            LoadLog() << "[tutorial] no ground to rebel in" << std::endl;
            return;
        }
        // The two most recently taken, which on this map is the Kestrel end
        // of the island -- a revolt where the fighting just was.
        std::sort(faction.begin(), faction.end());
        std::vector<int> rebels(faction.end() - 2, faction.end());
        createRebelCountry(allocateRebelCid(), parent, rebels);

        // AND REDRAW THE MAP.
        //
        // createRebelCountry moves province ownership in the data, and the
        // picture of the map is a texture built from that data -- so without
        // this the breakaway simply does not appear: the provinces belong to
        // it, the panels say so, and the map still shows them as yours.
        //
        // The turn pipeline does this for its own rebellions further down; a
        // rebellion staged from a script arrives outside that path and has to
        // ask for it.
        rebuildOwnershipPixels();
        rebuildFlags();
        if (m_renderer) m_renderer->setCountryFlags(&m_countryFlags);
        addNotification("Rebellion!", RED);
        return;
    }
    if (act.rfind("then:", 0) == 0) {
        // "when this script ends, open that one". Both the beginner lesson
        // and the topic menu finish the same way -- back to the menu, saves
        // and multiplayer, Pr1nted signing off -- and that tail is written
        // once, in outro.oddlg, rather than pasted into the end of each.
        m_dialogReturnTo = act.substr(5);
        return;
    }
    if (act == "to_menu") {
        // Back to the menu WITHOUT ending the tutorial: saves and multiplayer
        // are menu things, so that is where they are explained, and Pr1nted
        // signs off from the same place he called from.
        m_tutorialPending = false;
        m_paused = false;
        m_inSettings = false;
        m_unsavedChanges = false;      // nothing was ever written for this world
        m_currentSavePath.clear();
        m_uiTargets.clear();
        unloadGameData();
        initMenuBackground();
        m_currentScreen = SCREEN_MENU;
        return;
    }
    if (act == "end_tutorial") {
        // Already on the menu by now, so this is only the flags and the link.
        m_tutorialMode = false;
        m_tutorialTrack.clear();
        m_tutorialPending = false;
        m_introRunning = false;
        m_dialogReturnTo.clear();
        AISystem::s_tutorialAI = false;
        m_tutorialStopRect = {0, 0, 0, 0};
        return;
    }
    if (act == "unlock_turn") {
        m_tutorialTurnUnlocked = true;
        return;
    }
    if (act == "tune_out") {
        // The caller hangs up -- and only the caller. Anyone else still on
        // the link stays there, which is what makes Pr1nted signing off and
        // leaving Mia to it read as him leaving rather than as the call
        // ending.
        commsHangUp();
        if (commsSlots() == 0) m_commsOpen = false;
        return;
    }
    LoadLog() << "[tutorial] unknown act \"" << act << "\"" << std::endl;
}

bool Game::tutorialGateActive() const {
    if (!m_dialogOpen) return false;
    const dlg::Page* page = m_dialog.currentPage();
    return page && page->gate;
}

bool Game::tutorialGateHoldsMap() const {
    if (!tutorialGateActive()) return false;
    const dlg::Page* page = m_dialog.currentPage();
    if (!page) return false;
    // A page about the province panel needs a province chosen first.
    return !(page->pointAt == "panel.province" || page->pointAt == "panel.stats");
}

bool Game::tutorialAllowsEndTurn() const {
    if (!m_tutorialMode) return true;
    // ONE RULE: has the lesson introduced the button yet.
    //
    // It used to be narrower -- gated pages allowed it only while pointing at
    // the button itself -- and that deadlocks. Declaring war does not happen
    // when you press Declare War; it queues a diplomatic action that resolves
    // when the turn does. So the page that waits for the war to exist was
    // also the page refusing to let the turn run, and there was no way out of
    // it at all.
    //
    // The same trap is waiting for anything else resolved at turn end:
    // recruitment, embarkation, an army moving. Blocking the turn per page is
    // not worth one more of those.
    return m_tutorialTurnUnlocked;
}

bool Game::tutorialBlocksInput(Vector2 mouse) const {
    if (!m_dialogOpen) return false;
    const dlg::Page* page = m_dialog.currentPage();
    if (!page || !page->gate) return false;
    // The textbox is always live: it is how the player gets past the page,
    // and a gate that also blocks "next" is a gate the player cannot open.
    if (CheckCollisionPointRec(mouse, dialogueBounds())) return false;
    // Nor is anything blocked while the page is asking for a province to be
    // picked -- the map IS the control it is pointing at.
    if (!tutorialGateHoldsMap()) return false;
    Rectangle r; bool round = false;
    if (!tutorialFocus(r, round)) return true;      // gated, nothing to allow
    if (!round) return !CheckCollisionPointRec(mouse, r);
    const Vector2 c{r.x + r.width * 0.5f, r.y + r.height * 0.5f};
    const float dx = (mouse.x - c.x) / (r.width * 0.5f);
    const float dy = (mouse.y - c.y) / (r.height * 0.5f);
    return (dx * dx + dy * dy) > 1.0f;
}

void Game::drawTutorialPointer() {
    if (!m_dialogOpen) return;
    const dlg::Page* page = m_dialog.currentPage();
    if (!page || page->pointAt.empty()) return;

    Rectangle r; bool round = false;
    // Everything that is not the answer goes dark.
    //
    // The ring says "here"; the dim says "and nowhere else", which is the
    // half that was missing. A gated page already refuses clicks elsewhere,
    // and a player who cannot see that refuses to believe it -- they click
    // the wrong thing, nothing happens, and the game looks broken rather than
    // deliberate. Drawn as four rectangles around the target instead of one
    // over the lot, so the thing being pointed at keeps its real colours.
    // Only where the gate actually holds. On a page that wants a province
    // picked the map is live, and darkening it would say the opposite of
    // what the words say -- "click one of ours" over a greyed-out map.
    if (page->gate && tutorialGateHoldsMap() && tutorialFocus(r, round)) {
        const float pad = 12.0f;
        const Rectangle keep{r.x - pad, r.y - pad, r.width + pad * 2, r.height + pad * 2};
        const Rectangle box = dialogueBounds();
        const Color veil{0, 0, 0, 130};
        // The textbox is never dimmed: it is how the page is got past.
        const float topEdge = std::min(keep.y, box.y);
        DrawRectangle(0, 0, m_screenW, (int)topEdge, veil);
        DrawRectangle(0, (int)(box.y + box.height), m_screenW,
                      m_screenH - (int)(box.y + box.height), veil);
        DrawRectangle(0, (int)topEdge, (int)keep.x, (int)(box.y + box.height - topEdge), veil);
        DrawRectangle((int)(keep.x + keep.width), (int)topEdge,
                      m_screenW - (int)(keep.x + keep.width),
                      (int)(box.y + box.height - topEdge), veil);
        DrawRectangle((int)keep.x, (int)(keep.y + keep.height), (int)keep.width,
                      (int)(box.y - (keep.y + keep.height)), veil);
    }

    if (!tutorialFocus(r, round)) {
        // A name nothing offered. Said once, loudly, because the symptom
        // otherwise is a tutorial that points at nothing and a player who
        // cannot tell whether that is the game or themselves.
        if (m_uiTargetsMissing.insert(page->pointAt).second) {
            // With the list of what IS on offer. A pointer name is a typo or
            // a rename nine times out of ten, and the fix is in the next line
            // of output rather than in a grep through the drawing code.
            LoadLog() << "[tutorial] nothing offers \"" << page->pointAt
                      << "\" -- the pointer has nowhere to go. On screen now:";
            if (m_uiTargets.empty()) LoadLog() << " (nothing)";
            for (const auto& [name, t] : m_uiTargets)
                if (t.frame + 1 >= m_uiFrame) LoadLog() << " " << name;
            LoadLog() << std::endl;
        }
        return;
    }

    const Color accent = hexToColor(m_config.accent());
    // Two clocks. The breath is what makes it read as alive; the march is
    // what makes it read as a selection rather than as a border somebody
    // drew. Both are slow: this sits on screen for a whole page of dialogue,
    // and anything quicker becomes something to look away from.
    const float breath = 0.5f + 0.5f * std::sin(m_pointerT * 2.1f);
    const float pad = 6.0f + 3.0f * breath;
    const Rectangle box{r.x - pad, r.y - pad, r.width + pad * 2, r.height + pad * 2};

    // A wash inside, so the eye lands on the thing and not on the ring.
    if (round) {
        const Vector2 c{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
        DrawEllipse((int)c.x, (int)c.y, box.width * 0.5f, box.height * 0.5f,
                    ColorAlpha(accent, 0.10f + 0.05f * breath));
        for (int i = 0; i < 2; ++i)
            DrawEllipseLines((int)c.x, (int)c.y, box.width * 0.5f - i, box.height * 0.5f - i,
                             ColorAlpha(accent, 0.75f + 0.25f * breath));
    } else {
        DrawRectangleRec(box, ColorAlpha(accent, 0.08f + 0.05f * breath));
        DrawRectangleLinesEx(box, 2.0f, ColorAlpha(accent, 0.75f + 0.25f * breath));

        // Corner brackets, drawn over the ring and marching inward with the
        // breath: the same language the transmission window's frame uses, so
        // "the game is pointing at this" looks like one idea in both places.
        const float k = std::min(box.width, box.height) * 0.28f;
        const float grow = 4.0f * (1.0f - breath);
        for (int i = 0; i < 4; ++i) {
            const float cx = (i & 1) ? box.x + box.width + grow : box.x - grow;
            const float cy = (i & 2) ? box.y + box.height + grow : box.y - grow;
            const float sx = (i & 1) ? -1.0f : 1.0f;
            const float sy = (i & 2) ? -1.0f : 1.0f;
            DrawLineEx({cx, cy}, {cx + sx * k, cy}, 3.0f, accent);
            DrawLineEx({cx, cy}, {cx, cy + sy * k}, 3.0f, accent);
        }
    }
}

void Game::endDialogue() {
    m_dialog.close();
    m_dialogOpen = false;

    // Take the link down FIRST, before anything that might open a new one.
    //
    // This used to run last, after the follow-ons below, which got it wrong
    // twice: the intro's branch returned early and left both speakers sitting
    // on screen, and the hand-over branch opened the next script and then
    // tuned out the window it had just filled.
    m_comms.tuneOut();
    m_comms2.tuneOut();
    m_commsSlot[0].clear();
    m_commsSlot[1].clear();
    m_commsActive = 0;

    // What happens next is decided by flags the CALLER may have cleared. That
    // is deliberate: stopTutorial clears them before closing, so leaving the
    // tutorial closes a dialogue and nothing else. Reading them the other way
    // round is what made "quit the tutorial" load the tutorial world on its
    // way out, and the next attempt to start one wedged.
    if (m_introRunning) {
        m_introRunning = false;
        startTutorialWorld();
        return;
    }
    if (!m_dialogReturnTo.empty()) {
        const std::string back = m_dialogReturnTo;
        m_dialogReturnTo.clear();
        beginDialogue(back);
    }
}

/**
 * Acts on the option the player just took, if any. True when it took over --
 * the conversation has been handed somewhere else and the caller must not go
 * on to touch a box that is no longer the same box.
 *
 * A key is a one-shot instruction, cleared when the box closes or opens (see
 * dlg::Box::close), so this is safe to call every frame: it does nothing until
 * there is an answer, and the answer is gone once it has been acted on.
 */
bool Game::consumePickedChoice() {
    const std::string key = m_dialog.pickedKey();
    if (key.empty()) return false;

    // The intro's fork, remembered before the intro is closed and replaced.
    if (m_introRunning) m_tutorialTrack = key;

    // "world:X" goes back into the tutorial map and opens X once it is up.
    // The specialised lessons point at the province panel and talk about
    // ports; from the menu there is nothing for them to point AT.
    if (key.rfind("world:", 0) == 0) {
        m_tutorialTrack = key.substr(6);
        endDialogue();
        startTutorialWorld();
        return true;
    }
    // "script:X" opens it and comes back here when it ends. This is the whole
    // of the branching in .oddlg, and deliberately: a topic menu is six options
    // that each run somewhere else and return, which is a script name and a
    // return address, not a jump table.
    if (key.rfind("script:", 0) == 0) {
        const std::string next = key.substr(7);
        const std::string back = m_dialogScript;
        if (next == "done") {
            m_dialogReturnTo.clear();
            endDialogue();
        } else {
            beginDialogue(next);
            m_dialogReturnTo = back;      // AFTER: beginDialogue clears it
        }
        return true;
    }
    return false;
}

void Game::updateDialogue(float dt) {
    // Armed by the menu, fired once the world it asked for is actually up.
    if (m_tutorialPending && m_currentScreen == SCREEN_PLAYING) {
        m_tutorialPending = false;

        // Look at the player's own country, not wherever the camera happened
        // to be. The tutorial world is two small islands in an otherwise
        // empty map, so the default view is open ocean -- a first impression
        // of a game with nothing in it.
        int home = 0;
        float bestPop = -1.0f;
        for (const auto& [pid, pr] : m_provinces.getAllProvinces()) {
            if (pr.countryId != m_playerCountryId) continue;
            auto pop = m_provincePopulations.find(pid);
            const float n = (pop != m_provincePopulations.end()) ? (float)pop->second : 0.0f;
            if (n > bestPop) { bestPop = n; home = pid; }
        }
        auto ctr = m_provinceCenters.find(home);
        if (home > 0 && ctr != m_provinceCenters.end() && m_renderer) {
            // Wide enough to hold both islands: the first thing the lesson
            // talks about is the one we cannot walk to.
            const float minZoom = std::max(m_screenW / (float)m_provinces.getWidth(),
                                           m_screenH / (float)m_provinces.getHeight());
            m_renderer->flyTo(ctr->second.x, ctr->second.y,
                              std::clamp(minZoom * 1.35f, minZoom, 3.0f), 9999.0f);
        }
        // Which lesson depends on what they said in the intro. Someone who
        // said they know the basics never meets the page that unlocks the
        // turn button, so it is unlocked for them here.
        if (m_tutorialTrack == "specifics") m_tutorialTurnUnlocked = true;
        beginDialogue(m_tutorialTrack == "specifics" ? "specifics" : "tutorial");
    }
    m_pointerT += dt;
    ++m_uiFrame;
    if (!m_dialogOpen) { m_uiTargets.clear(); return; }

    // ESCAPE MUST NOT DESTROY THE TUTORIAL.
    //
    // It used to close the dialogue outright, which in a tutorial leaves the
    // player standing in a practice world with no lesson, no save and no way
    // on -- stuck, with the only escape being to quit the game. Outside a
    // tutorial a dialogue is an interruption and closing it is right; inside
    // one it is the whole thing, so Escape is left alone and goes where it
    // normally goes: the pause menu, which is where "Stop the tutorial" is.
    if (IsKeyPressed(KEY_ESCAPE) && !m_tutorialMode) { endDialogue(); return; }

    m_dialog.setBounds(dialogueBounds());

    // Advance on a click INSIDE the box, or on space/enter anywhere. A click
    // anywhere is the classic contract, but the map is live underneath and
    // would take the same click to select a province -- so the box asks for
    // its own area and leaves the rest of the screen to the game.
    // THE POINTER, IN THE SAME SPACE AS THE BOX.
    //
    // This used to ask raylib directly. Every other screen in the game asks
    // getMouse(), which divides the physical position by the display's DPI
    // scale -- so on a 2x screen the dialogue was hit-testing against a point
    // at half the real coordinates. The box is big, so a click still landed
    // somewhere inside it and pages still turned, which is why it looked like
    // it worked; the choice rows are one row high near the bottom, and a click
    // on one of them landed hundreds of pixels above the list. Picking a topic
    // did nothing at all.
    const Vector2 mouse = getMouse();
    const bool inBox = CheckCollisionPointRec(mouse, dialogueBounds());

    // While the box is asking a question, space and enter TAKE the highlighted
    // option rather than turning the page -- and the arrows move it. Letting
    // the page-turn keys through would answer the question for the player
    // with whatever happened to be first.
    // One synthetic click, if --tutorial-walk asked for one. Taken here rather
    // than faked at the input layer because raylib has no way to inject a
    // press, and consumed on read so it is worth exactly one frame.
    const bool walkClick = m_dialogAdvance;
    m_dialogAdvance = false;

    if (m_dialog.awaitingChoice()) {
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_TAB))  m_dialog.moveSelection(1);
        if (IsKeyPressed(KEY_UP))                             m_dialog.moveSelection(-1);
        if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) m_dialog.commitChoice();
        m_dialog.update(dt, (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && inBox) || walkClick,
                        mouse);
    } else {
        const bool clicked =
            (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && inBox) ||
            IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER) || walkClick;
        m_dialog.update(dt, clicked, mouse);
    }

    // AN ANSWER IS ACTED ON IN THE FRAME IT IS GIVEN.
    //
    // This used to be read at the TOP of this function, on the frame after the
    // click -- which works only for a menu with a page after it. The topic menu
    // is one page: taking an option turns to a page that is not there, so the
    // box closes, and the bottom of this function ends the conversation the
    // same frame. By the next frame there was no dialogue left to read the
    // answer from, and picking a topic did nothing at all. The intro's fork
    // has three pages after it and so never showed the fault.
    if (consumePickedChoice()) return;

    // Answered every frame, before the box is asked to turn the page.
    if (const dlg::Page* page = m_dialog.currentPage())
        m_dialog.setConditionMet(page->until.empty() || tutorialConditionMet(page->until));

    if (m_dialog.pageIndex() != m_dialogPage) {
        m_dialogPage = m_dialog.pageIndex();
        m_dialogPageTurn = m_turnNumber;
        if (const dlg::Page* page = m_dialog.currentPage()) {
            // By value, and everything below re-reads the page: an act can
            // open a different script, which frees the vector this points
            // into. Nothing does that today; it costs a string to make sure
            // nothing ever can.
            const std::string act = page->act;
            const std::string speaker = page->speaker;
            const std::string pose = page->pose;
            const float mood = emotionFor(*page);
            tutorialAct(act);
            // A dropout means "somebody else is on the link now", so it fires
            // only when that is true. Turning the page is not an event in the
            // transmission; glitching on every page made the link look
            // broken rather than the speaker look changed.
            if (speaker != m_commsShowing || pose != m_commsPose) {
                m_commsShowing = speaker;
                m_commsPose = pose;
                commsSpeaker(speaker);
            }
            // The mood changes with every page, dropout or not: it is what
            // is being said, not who is saying it.
            m_comms.setEmotion(mood);
        }
        m_voiceLetters = 0;
    }

    // The voice: one blip every couple of letters while the typewriter runs.
    // Spaces and punctuation are silent, which is what gives it a rhythm
    // rather than a tone.
    m_voiceCooldown = std::max(0.0f, m_voiceCooldown - dt);
    const int revealed = m_dialog.revealedCount();
    for (int i = m_lastRevealed; i < revealed; i++) {
        const int cp = m_dialog.codepointAt(i);
        const bool letter = (cp > 0x2FF) || (cp >= 'a' && cp <= 'z') ||
                            (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9');
        // The mouth follows EVERY glyph; only the blip is rationed.
        m_comms.speak(cp);
        if (!letter) continue;
        if (++m_voiceLetters < 3 || m_voiceCooldown > 0.0f) continue;
        m_voiceLetters = 0;
        m_voiceCooldown = 0.075f;
        Audio::get().playSfx("voice_" + m_speakerVoice, 0.10f);
    }
    m_lastRevealed = revealed;
    if (m_dialog.pageComplete()) m_comms.hush();

    if (!m_dialog.isOpen()) endDialogue();
}

void Game::drawDialogue() {
    if (!m_dialogOpen) return;
    m_dialog.draw(hexToColor(m_config.accent()));

    // Inside the box, bottom right. Outside it the hint landed on the turn
    // bar, where it was both unreadable and in the way of the button.
    // What the footer promises has to be true. On a page waiting for the
    // player to DO something, "continue" is a lie the player will click
    // twenty times before deciding the game is broken.
    const char* hint = m_dialog.awaitingChoice()    ? T("pick one")
                     : m_dialog.awaitingCondition() ? T("waiting for you")
                     : m_dialog.pageComplete()      ? T("click / space  -  continue")
                                                    : T("click / space  -  show it all");
    const int fs = 15;
    const Rectangle b = dialogueBounds();
    DrawText(hint, (int)(b.x + b.width) - MeasureText(hint, fs) - 18,
             (int)(b.y + b.height) - fs - 12, fs,
             ColorAlpha(hexToColor(m_config.accent()), 0.45f));
}
