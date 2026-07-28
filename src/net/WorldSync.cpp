#include "WorldSync.h"

#include "NetProtocol.h"

std::vector<uint8_t> NetWorldSnapshot::encode() const {
    NetWriter w;
    w.str(mapName);
    w.u32(turnNumber);
    w.str(stateJson);
    w.u32(static_cast<uint32_t>(turns.size()));
    for (const NetTurnDelta& t : turns) {
        w.u32(t.turn);
        w.blob(t.packed);
    }
    return w.take();
}

bool NetWorldSnapshot::decode(const uint8_t* data, size_t size, NetWorldSnapshot& out) {
    NetReader r(data, size);

    NetWorldSnapshot s;
    // A map NAME, so it is bounded like one. A host cannot make a client hold
    // a megabyte of "filename".
    s.mapName = r.str(256);
    s.turnNumber = r.u32();
    s.stateJson = r.str(kNetMaxStateJson);

    const uint32_t count = r.u32();
    // Checked against the reader's own remaining bytes as well as the ceiling:
    // a count of a million with ten bytes left is a lie, and reserving on it
    // before reading is how a length field becomes an allocation.
    if (!r.ok() || count > kNetMaxTurns || count > r.remaining() / 4) return false;

    s.turns.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        NetTurnDelta t;
        t.turn = r.u32();
        t.packed = r.blob(kNetMaxDeltaBytes);
        if (!r.ok()) return false;
        s.turns.push_back(std::move(t));
    }

    // done() rather than ok(): trailing bytes mean the sender and this build
    // disagree about the shape, and a world is not something to load on a
    // guess.
    if (!r.done()) return false;

    // A snapshot with no map cannot be loaded, so it is not a snapshot.
    if (s.mapName.empty()) return false;

    out = std::move(s);
    return true;
}
