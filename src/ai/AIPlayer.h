#pragma once
// ─── WHAT GAME ASKS OF AN AI ────────────────────────────────
//
// Groundwork item 2 of docs/ai/REWORK_PLAN.md.
//
// Game used to drive the AI as an AISystem, which is ParrotZero -- one engine,
// fourteen thousand lines, reached into by name. The rework wants a second
// engine (a search over sim::Order, 1.3.4b) beside it, chosen per country or
// per difficulty and A/B-tested against it. That needs the turn loop to talk to
// an INTERFACE, and this is it: the four calls the turn loop makes, and nothing
// else.
//
// Everything else Game calls on m_ai -- training stats, the debug overlay, the
// bench-agent doors, the league -- is ParrotZero's own and stays on AISystem.
// A second engine is not expected to have a learning curve.
//
// ORDERS GO THROUGH THE SINK. takeTurn is handed an OrderSink and every order a
// country issues is meant to go into it, rather than into Game::m_pending*. For
// ParrotZero that is true of every executor's order push; diplomacy still goes
// through Game::queueDiplomaticAction, which validates and answers, and a few
// executors still edit queued orders in place. AISystem.h lists those.
//
// The plan's eventual shape is `decide(const WorldView&, cid) -> vector<Order>`.
// It is not that yet because ParrotZero's executors read the queues back as
// they write them -- that is how they avoid issuing an order twice -- so its
// orders have to land as they are made. A sink that applies immediately is the
// form of that seam which is behaviour-neutral today; a collecting sink becomes
// possible once the executors stop reading Game directly.

#include <string>

namespace sim { class OrderSink; }
class AITraceSink;

class AIPlayer {
public:
    virtual ~AIPlayer() = default;

    /// Once per turn, before any country thinks.
    virtual void beginTurn() = 0;
    /// Decide this country's turn and issue its orders into `orders`.
    virtual void takeTurn(int cid, sim::OrderSink& orders) = 0;
    /// After the turn resolves: whatever the engine does with the result.
    virtual void endTurn() = 0;
    /**
     * Incoming diplomacy aimed at a country this AI plays: accept or reject.
     * `subjectIso` is the third party a request is ABOUT (the aggressor in a
     * call to arms); `statedReasonOut`, when given, receives what the country
     * SAYS about a refusal. See AISystem::decideDiplomacy.
     */
    virtual bool decideDiplomacy(int targetCid, const std::string& action,
                                 const std::string& sourceIso,
                                 const std::string& subjectIso,
                                 int* statedReasonOut) = 0;

    /**
     * Attach a trace sink (the visualiser's hook, groundwork item 6), or
     * nullptr to detach. Not owned. An engine with nothing to say may ignore
     * it; when none is attached, tracing must cost nothing.
     */
    virtual void setTraceSink(AITraceSink* sink) { (void)sink; }
};
