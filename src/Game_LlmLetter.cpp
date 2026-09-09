// Does a letter typed in the interface actually reach the country's play?
//
// `OpenDoctrines --llm-letter [country]`
//
// WHY THIS EXISTS AS A MODE RATHER THAN AS A TEST
//
// The chain from a letter to a decision has six links, and every one of them
// was unit-tested and green while the whole was DEAD. The prompt asks for a
// disposition, small models make exactly one tool call per letter, that call
// went to the disposition, and intend/press/prefer_doctrine were never reached
// -- so llmIntentFor, llmSuppressesReflex, llmPressTarget and
// llmPreferredDoctrine returned nothing in every real game. Nothing failed. A
// counter read zero and no test was looking at it.
//
// Two other things hid it. The four accessors open with llmConfigured(), which
// is false in the server's eval path, so the bench can never see this feature
// at all. And --eval-ai is not reproducible run to run in either binary, so
// world aggregates cannot answer the question either.
//
// What is left is this: a real window, the player's real config, a real runner,
// a real letter sent through the same mailSendDraft() the Send button calls,
// and then a look at whether the four maps have anything in them.
//
// It needs a model, so it is not in run_all.sh. It prints PASS or FAIL on the
// one thing it can actually establish -- that a letter moved the country.
#include "Game.h"
#include "ai/AISystem.h"

#include <cstdio>
#include <string>

namespace {
// The letter. Deliberately one that DESERVES steering: measured, a small model
// steers on a note about the weather just as readily when asked directly, so a
// bland letter here would prove the gate is open rather than that the chain
// works. The bland case is the negative control and lives in
// tests/llm_tools_live_check.cpp, where it can be run many times cheaply.
const char* kLetter =
    "Your army is thin on the eastern frontier and everyone can see it. "
    "We are not going to help you if it comes to a war. What are you "
    "actually going to do about it?";
}  // namespace

void Game::beginLlmLetterWalk(const std::string& savePath,
                              const std::string& correspondent) {
    // Line-buffered: this mode is watched from a log while it runs, and a
    // 4 KB buffer means the last thing it printed before anything went wrong
    // is exactly the part that never reaches the file.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    m_llmLetter = true;
    m_llmLetterPhase = 0;
    m_llmLetterFrame = 0;
    m_llmLetterSave = savePath;
    m_llmLetterWho = correspondent;
    m_llmLetterTarget = 0;
    m_currentScreen = SCREEN_MENU;
    ClearWindowState(FLAG_VSYNC_HINT);
    SetTargetFPS(0);
    printf("[LETTER] endpoint %s, model %s\n",
           m_config.llmEndpoint.c_str(), m_config.llmModel.c_str());
    if (!llmConfigured())
        printf("[LETTER] llmConfigured() is FALSE -- nothing below can fire\n");
}

/// What the four hooks would read for `cid`, printed as the AI would see it.
void Game::reportLlmState(int cid, const char* when) {
    printf("[LETTER] --- %s ---\n", when);
    const auto goal = m_llmGoal.find(cid);
    printf("[LETTER]   goal      %s\n",
           goal == m_llmGoal.end() ? "(none)" : goal->second.c_str());

    int intents = 0;
    for (const auto& [key, want] : m_llmIntent) {
        if ((int)(key >> 20) != cid || want == 0.0f) continue;
        ++intents;
        printf("[LETTER]   intent    module %d action %d  %+.2f\n",
               (int)((key >> 8) & 0xFFF), (int)(key & 0xFF), want);
    }
    if (!intents) printf("[LETTER]   intent    (none)\n");

    const int pressed = llmPressTarget(cid);
    if (pressed > 0) {
        const Country* c = m_countries.getCountry(pressed);
        printf("[LETTER]   press     %s\n", c ? c->name.c_str() : "?");
    } else {
        printf("[LETTER]   press     (nobody)\n");
    }
    const std::string doc = llmPreferredDoctrine(cid);
    printf("[LETTER]   doctrine  %s\n", doc.empty() ? "(none)" : doc.c_str());

    // The reflex gates read through the cap, so ask them rather than the map:
    // two leans recorded and one suppression granted is the designed answer,
    // and printing the raw map would look like a bug.
    std::string off;
    for (const char* r : {"garrison", "fortify", "campaign", "austerity", "manpower",
                          "redeploy", "pacification", "withdraw", "peace", "siege"})
        if (llmSuppressesReflex(cid, r)) { off += off.empty() ? "" : ", "; off += r; }
    printf("[LETTER]   reflexes  %s\n", off.empty() ? "(all running)" : off.c_str());
}

bool Game::tickLlmLetterWalk() {
    ++m_llmLetterFrame;

    switch (m_llmLetterPhase) {
    case 0: {   // ── load the world, exactly as the screenshot tour does ──
        printf("[LETTER] loading %s\n", m_llmLetterSave.c_str());
        startLoadedGame(m_llmLetterSave);
        while (m_loadingPhase != LOAD_NONE && m_loadingPhase != LOAD_DONE) {
            if (WindowShouldClose()) return false;
            updateLoading();
        }
        if (m_loadingFailed) {
            fprintf(stderr, "[LETTER] could not load %s\n", m_llmLetterSave.c_str());
            return false;
        }
        hideLoadingScreen();
        m_currentScreen = SCREEN_PLAYING;

        // Play the largest country, so the correspondent has somebody
        // substantial to answer and the mail screen is not empty.
        {
            std::vector<int> byCountry;
            for (int owner : m_provinceCountryLookup) {
                if (owner <= 0 || owner >= REBEL_CID_MIN) continue;
                if ((int)byCountry.size() <= owner) byCountry.resize(owner + 1, 0);
                byCountry[owner]++;
            }
            int best = 0, bestN = 0;
            for (int cid = 1; cid < (int)byCountry.size(); ++cid)
                if (byCountry[cid] > bestN) { bestN = byCountry[cid]; best = cid; }
            m_playerCountryId = best;
        }
        refreshLlmAvailability();

        // Whom to write to: the named country if one was given, otherwise the
        // first country the mail module will actually answer for.
        for (int cid : m_llmCountries) {
            if (cid == m_playerCountryId) continue;
            const Country* c = m_countries.getCountry(cid);
            if (!c) continue;
            if (!m_llmLetterWho.empty() && c->name != m_llmLetterWho) continue;
            m_llmLetterTarget = cid;
            break;
        }
        if (m_llmLetterTarget <= 0) {
            fprintf(stderr, "[LETTER] no bot country to write to "
                            "(%zu mail-capable countries)\n", m_llmCountries.size());
            return false;
        }
        {
            const Country* me = m_countries.getCountry(m_playerCountryId);
            const Country* them = m_countries.getCountry(m_llmLetterTarget);
            printf("[LETTER] playing %s, writing to %s\n",
                   me ? me->name.c_str() : "?", them ? them->name.c_str() : "?");
        }
        reportLlmState(m_llmLetterTarget, "before the letter");
        m_llmLetterPhase = 1;
        m_llmLetterFrame = 0;
        return true;
    }

    case 1: {   // ── type it and press Send, through the interface's own path ──
        openMail();
        m_mailThread = m_llmLetterTarget;
        m_mailGroupThread = 0;
        m_mailDraft = kLetter;
        // The same call the Send button and the Return key make. Going through
        // it rather than around it is the point of this mode: a driver that
        // wrote straight into the mailbox would pass while the button was
        // broken.
        if (!mailSendDraft()) {
            fprintf(stderr, "[LETTER] mailSendDraft() refused to send\n");
            return false;
        }
        printf("[LETTER] sent, and the draft box is %s\n",
               m_mailDraft.empty() ? "clear" : "NOT CLEAR (bug)");
        m_llmLetterPhase = 2;
        m_llmLetterFrame = 0;
        return true;
    }

    case 2: {   // ── end the turn: the post is delivered, the advisor is asked ──
        if (m_llmLetterFrame < 3) return true;   // let the send render first
        printf("[LETTER] turn %d -> processing\n", m_turnNumber);
        processTurn();
        printf("[LETTER] turn %d, advisor asked; waiting for the model\n", m_turnNumber);
        m_llmLetterPhase = 3;
        m_llmLetterFrame = 0;
        return true;
    }

    case 3: {   // ── wait for the worker, then end another turn to post it ──
        // The reply comes back on a worker thread and is collected by the NEXT
        // runAdvisors(), so this ends turns until one carries it -- with a
        // ceiling, because "no answer" has to end the run rather than hang it.
        //
        // ── THE CEILING IS IN SECONDS, NOT IN TURNS ──
        //
        // It was twenty turns, and twenty turns went by in less time than a
        // cold model takes to load: the run reported "no reply" and FAILED
        // while the request it was waiting for was still perfectly alive. A
        // budget measured in the units of the thing being waited on.
        if (m_llmLetterStart == 0.0) m_llmLetterStart = GetTime();
        const double waited = GetTime() - m_llmLetterStart;
        // ── EVERY 120th TICK, NOT EVERY 120th FRAME ──
        //
        // The tick is called from several places in the run loop, so this
        // counter advances more than once per frame and "% 120" was landing on
        // a different phase each time -- the run sat in this case for eight
        // minutes without ever reaching processTurn, printing nothing, because
        // the gate never opened. Counted down instead, which cannot be missed.
        if (--m_llmLetterCountdown > 0) return true;
        m_llmLetterCountdown = 120;
        const mail::Box* box = mailboxIfAny(m_playerCountryId);
        bool replied = false;
        if (box) {
            for (const mail::Thread* t : box->threads()) {
                if (!t || t->otherCountry != m_llmLetterTarget) continue;
                for (const mail::Message& m : t->messages)
                    if (m.fromCountry == m_llmLetterTarget) replied = true;
            }
        }
        if (replied) {
            printf("[LETTER] a reply arrived on turn %d\n", m_turnNumber);
            m_llmLetterPhase = 4;
            m_llmLetterFrame = 0;
            return true;
        }
        if ((int)waited != m_llmLetterSaidAt) {
            m_llmLetterSaidAt = (int)waited;
            if (m_llmLetterSaidAt % 15 == 0)
                printf("[LETTER] %ds waited, turn %d, %d request(s) in flight\n",
                       m_llmLetterSaidAt, m_turnNumber, llmInFlight());
        }
        if (waited > 240.0) {
            printf("[LETTER] no reply in 240s (%d in flight) -- giving up\n",
                   llmInFlight());
            m_llmLetterPhase = 4;
            m_llmLetterFrame = 0;
            return true;
        }
        processTurn();
        return true;
    }

    case 4: {   // ── what the letter left behind, and a picture of it ──
        if (m_llmLetterFrame < 30) {   // let the mail screen settle first
            openMail();
            m_mailThread = m_llmLetterTarget;
            return true;
        }
        reportLlmState(m_llmLetterTarget, "after the reply");

        const bool moved = m_llmGoal.count(m_llmLetterTarget) ||
                           llmPressTarget(m_llmLetterTarget) > 0 ||
                           !llmPreferredDoctrine(m_llmLetterTarget).empty() ||
                           [&] {
                               for (const auto& [key, want] : m_llmIntent)
                                   if ((int)(key >> 20) == m_llmLetterTarget && want != 0.0f)
                                       return true;
                               return false;
                           }();
        printf("[LETTER] %s\n", moved
               ? "PASS: a letter typed in the interface reached the country's play"
               : "FAIL: the letter changed nothing the AI reads");
        m_llmLetterPass = moved;
        m_llmLetterPhase = 5;
        m_llmLetterFrame = 0;
        return true;
    }

    default:
        // One frame in phase 5 so the screenshot below is of a drawn mail
        // screen rather than of whatever was on the buffer.
        if (m_llmLetterFrame < 2) return true;
        if (!m_llmLetterShot.empty()) {
            TakeScreenshot(m_llmLetterShot.c_str());
            printf("[LETTER] wrote %s\n", m_llmLetterShot.c_str());
        }
        return false;
    }
}
