// Chat plays a country.
//
// ── HOW THE PIECES FIT ──
//
// stream/IrcParse.h  reads Twitch's wire format          (pure, tested)
// stream/ChatReader  keeps a socket open and pumps it
// stream/ChatVote.h  counts what chat typed              (pure, tested)
// here               decides WHAT chat may pick, and does it
//
// The two files with rules in them have no socket and no Game, which is why
// their tests are exhaustive. This file is the part that has to know about the
// game, and it is deliberately the smallest of the four.
//
// ── WHAT CHAT MAY PICK ──
//
// A list this file builds from what the country could legally do THIS TURN.
// Never free text, and never an action assembled from what somebody typed. So
// the worst a hostile chat can do is choose a legal order the streamer did not
// want, which is the game working rather than a vulnerability. Everything goes
// through queueDiplomaticAction, the same channel the diplomacy screen uses.

#include "Game.h"

#include "GameInternals.h"
#include "i18n/Text.h"
#include "stream/JoinLink.h"

#include <algorithm>
#include <set>

void Game::startChatPlays(const std::string& channel, int countryId) {
    m_chatCountry = countryId;
    if (!m_chatReader) m_chatReader = std::make_unique<chatread::Reader>();
    if (!m_chatReader->start(channel)) {
        m_chatNote = T("That is not a channel name.");
        return;
    }
    m_config.streamChatChannel = channel;
    m_chatNote.clear();
}

void Game::stopChatPlays() {
    if (m_chatReader) m_chatReader->stop();
    m_chatPoll.close();
    m_chatCountry = 0;
}

bool Game::chatPlaysActive() const {
    return m_chatReader && m_chatCountry > 0 &&
           m_chatReader->state() != chatread::State::Idle;
}

/**
 * What this country could be told to do, as a list chat can type.
 *
 * Kept SHORT. A viewer is reading a stream on a phone; a list of twelve is a
 * list nobody votes in, and the tally stops being readable at a glance. Five is
 * about the limit for a chat overlay.
 */
std::vector<chatvote::Option> Game::buildChatOptions(int cid) const {
    std::vector<chatvote::Option> out;
    const Country* self = m_countries.getCountry(cid);
    if (!self) return out;

    // Neighbours, found through the provinces this country holds: the countries
    // it actually touches are the ones it can plausibly act on, and they are
    // also the ones a viewer can see on the map.
    std::set<int> touching;
    for (int pid : provincesOf(cid)) {
        auto it = m_provinceNeighbors.find(pid);
        if (it == m_provinceNeighbors.end()) continue;
        for (int n : it->second) {
            const Province* p = m_provinces.getProvinceById(n);
            if (!p || p->countryId <= 0 || p->countryId == cid) continue;
            if (p->countryId >= REBEL_CID_MIN) continue;
            touching.insert(p->countryId);
        }
    }

    // Ordered by id so the same board produces the same list twice: a vote is
    // not a vote if "2" means something different by the time somebody types it.
    std::vector<int> neighbours(touching.begin(), touching.end());
    std::sort(neighbours.begin(), neighbours.end());

    int key = 1;
    for (int other : neighbours) {
        if (out.size() >= 4) break;
        const Country* c = m_countries.getCountry(other);
        if (!c) continue;
        const bool atWar = modAtWar(cid, other);
        chatvote::Option o;
        o.key = std::to_string(key++);
        if (atWar) {
            o.label = std::string(T("Offer peace to ")) + c->name;
            o.id = (CHAT_ACT_PEACE << 20) | other;
        } else {
            o.label = std::string(T("Declare war on ")) + c->name;
            o.id = (CHAT_ACT_WAR << 20) | other;
        }
        out.push_back(o);
    }

    // ALWAYS OFFERED. Without it a poll is a choice between things that all
    // change the game, and chat cannot express "hold". It is also what a
    // deadlocked vote should land on.
    chatvote::Option nothing;
    nothing.key = std::to_string(key);
    nothing.label = T("Do nothing");
    nothing.id = (CHAT_ACT_NONE << 20);
    out.push_back(nothing);
    return out;
}

void Game::openChatVote() {
    if (!chatPlaysActive()) return;
    m_chatPoll.open(buildChatOptions(m_chatCountry), GetTime(), m_config.streamChatSeconds);
    m_chatLastWinner.clear();
}

void Game::applyChatWinner() {
    const chatvote::Tally win = m_chatPoll.winner();
    m_chatPoll.close();
    if (win.id == 0) {
        // Nobody voted. Said out loud rather than silently doing nothing, so a
        // streamer can tell "chat is quiet" from "the reader is broken".
        m_chatLastWinner = T("Chat said nothing.");
        return;
    }
    m_chatLastWinner = win.label;

    const int act = win.id >> 20;
    const int other = win.id & 0xFFFFF;
    const Country* self = m_countries.getCountry(m_chatCountry);
    const Country* them = m_countries.getCountry(other);
    if (act == CHAT_ACT_NONE || !self || !them) return;

    // ── THE SAME CHANNEL THE DIPLOMACY SCREEN USES ──
    //
    // Not a special path with fewer checks in it. Everything that would refuse
    // this order from a player refuses it from chat, which is the property that
    // makes handing a country to strangers safe.
    PendingDiplomaticAction da;
    da.sourceIso = self->isoA3;
    da.targetIso = them->isoA3;
    da.action = (act == CHAT_ACT_WAR) ? "declare_war" : "request_ceasefire";
    da.turnsRemaining = 1;
    queueDiplomaticAction(da);
}

void Game::pumpChatPlays() {
    if (!m_chatReader) return;

    const double now = GetTime();
    m_chatReader->pump([this, now](const std::string& who, const std::string& text) {
        m_chatPoll.cast(who, text, now);
    });

    // A poll closes itself: the window is the point, and a streamer should not
    // have to press anything for chat's decision to land.
    if (m_chatPoll.closesAt() > 0.0 && !m_chatPoll.open_at(now)) applyChatWinner();
}

// ───────────────────────────────────────────────────────── what chat sees ──
//
// This is drawn for the AUDIENCE as much as the player: a viewer who cannot see
// their vote land does not vote twice, they stop voting. So it shows the token
// to type, the running count, and how long is left -- and it keeps showing the
// last result afterwards, because "what did chat just do to me" is the question
// the stream is actually about.

void Game::drawChatVotePanel(int x, int y, int w) {
    if (!chatPlaysActive()) return;
    const Color accent = hexToColor(m_config.accent());
    const double now = GetTime();
    int cy = y;

    const Country* c = m_countries.getCountry(m_chatCountry);
    const std::string who = std::string(T("Chat plays ")) +
                            (c ? c->name : std::string("?"));
    DrawText(who.c_str(), x, cy, 14, accent);
    cy += 19;

    // The reader's own state, because "quiet chat" and "broken socket" look
    // identical from the outside and only one of them is worth doing anything
    // about.
    const chatread::State st = m_chatReader->state();
    const char* status = st == chatread::State::Reading   ? "reading"
                       : st == chatread::State::Connecting ? "connecting..."
                       : st == chatread::State::Failed     ? "cannot read that channel"
                                                           : "off";
    char line[160];
    snprintf(line, sizeof(line), "#%s  ·  %s  ·  %lld seen",
             m_chatReader->channel().c_str(), status, m_chatReader->seen());
    DrawText(line, x, cy, 11, Color{120, 126, 148, 255});
    cy += 18;

    if (m_chatPoll.open_at(now)) {
        snprintf(line, sizeof(line), "%s  %.0fs",
                 T("Type the number to vote"), m_chatPoll.secondsLeft(now));
        DrawText(line, x, cy, 12, Color{200, 206, 226, 255});
        cy += 18;

        const int total = std::max(1, m_chatPoll.totalVotes());
        for (const chatvote::Tally& t : m_chatPoll.standings()) {
            // A bar, because a number alone is unreadable at stream bitrate.
            const float share = (float)t.votes / (float)total;
            DrawRectangle(x, cy + 2, (int)(w * share), 14, Color{accent.r, accent.g,
                                                                accent.b, 60});
            int fs = 12;
            const std::string text = t.key + "  " +
                odText::fitToWidth(t.label, w - 60, fs, 9);
            DrawText(text.c_str(), x + 4, cy + 2, fs,
                     t.votes > 0 ? WHITE : Color{150, 156, 178, 255});
            const std::string n = std::to_string(t.votes);
            DrawText(n.c_str(), x + w - MeasureText(n.c_str(), 12) - 4, cy + 2, 12,
                     Color{190, 196, 216, 255});
            cy += 18;
        }
    } else if (!m_chatLastWinner.empty()) {
        DrawText(T("Chat chose"), x, cy, 11, Color{120, 126, 148, 255});
        cy += 15;
        int fs = 13;
        DrawText(odText::fitToWidth(m_chatLastWinner, w, fs, 10).c_str(), x, cy, fs, accent);
    }
}

// ─────────────────────────────────────────────────────────── the overlay ──
//
// Written to the game's own data directory whenever the turn changes, so a
// Text or Browser source in OBS can point at it once and never be touched
// again. See src/stream/OverlayFeed.h for why this is files and not a port.

void Game::writeOverlayFeed() {
    if (m_config.streamChatChannel.empty() && !m_config.streamSafe && !chatPlaysActive())
        return;   // nobody is streaming; do not litter the data directory

    overlay::Feed feed;
    feed.add("turn", std::to_string(m_turnNumber));
    if (!m_mapDate.empty()) feed.add("date", m_mapDate);
    if (const Country* me = m_countries.getCountry(m_playerCountryId))
        feed.add("country", me->name);

    if (chatPlaysActive()) {
        if (const Country* c = m_countries.getCountry(m_chatCountry))
            feed.add("chat.country", c->name);
        feed.add("chat.channel", m_chatReader->channel());
        feed.add("chat.votes", std::to_string(m_chatPoll.totalVotes()));
        if (!m_chatLastWinner.empty()) feed.add("chat.chose", m_chatLastWinner);
        int n = 1;
        for (const chatvote::Tally& t : m_chatPoll.standings()) {
            feed.add("vote." + std::to_string(n++), t.label + " (" +
                     std::to_string(t.votes) + ")");
            if (n > 5) break;
        }
    }

    // Failures are ignored on purpose: a full disk or a read-only directory
    // must not interrupt somebody's game, and an overlay that stops updating is
    // a thing they can see for themselves.
    const std::string base = m_dataDir + "/overlay";
    if (FILE* f = fopen((base + ".txt").c_str(), "wb")) {
        const std::string text = overlay::toText(feed);
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
    if (FILE* f = fopen((base + ".json").c_str(), "wb")) {
        const std::string json = overlay::toJson(feed);
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
    }
}

// ────────────────────────────────────────────────────── the viewer's link ──
//
// A viewer clicks a link in a chat message; the browser hands the URL to the
// game; the game goes to the join screen with the code already in it. The
// invite code never appears on the stream -- see net/src/live/viewerlink.ts for
// why the code cannot do this job itself.

bool Game::handleJoinUrl(const std::string& url) {
    const std::string code = joinlink::codeFrom(url);
    if (code.empty()) return false;

    // Straight to the join screen with the code filled in -- NOT joined
    // automatically. A link that drops somebody into a game the moment they
    // click it is a link that can be used to drag people into anything; the
    // last step stays theirs.
    m_pendingJoinCode = code;
    m_currentScreen = SCREEN_MULTIPLAYER;
    m_mpPage = MpPage::Join;
    m_mpCodeField = code;
    return true;
}
