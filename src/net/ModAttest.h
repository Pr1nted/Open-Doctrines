#pragma once

// "Do we both have the same mods?"
//
// READ THIS BEFORE TRUSTING ANY OF IT
//
// This is an INTEGRITY check, not an anti-tamper one, and every string it
// produces for a user says so. A client is a program running on hardware its
// owner controls. It can report any mod list and any digest it likes, and no
// amount of hashing changes that -- least of all in a game whose source anyone
// can read and rebuild. A "mod verification" feature that implied otherwise
// would be worse than none, because people would rely on it.
//
// SO WHAT ACTUALLY STOPS CHEATING
//
// Authority, not attestation:
//
//   - Game state only ever flows server -> client. A client applies turn
//     deltas; it never computes a turn. Whatever a mod does to a client's copy
//     of the world is overwritten by the next delta.
//   - Orders are validated server-side and re-attributed to the authenticated
//     peer's country. A client cannot issue orders for anyone else.
//   - A client-side mod has GameState.Write and GameProcess masked off its
//     grants for the duration of a session (modSideGrantMask), so on the
//     client the capability is not merely useless, it is absent.
//   - The server never asks a client to compute game state. Even a "both"-side
//     mod runs authoritatively on the server and decoratively on the client.
//
// A client that lies about its mods therefore desyncs its own display and
// gains nothing. That is the guarantee, and it holds without trusting the
// client at all.
//
// WHAT THIS IS FOR
//
// The ordinary case: someone has version 1.2 and the server runs 1.3, someone's
// download was truncated, someone edited a mod and forgot to rebuild it. Those
// are the failures that actually happen, they are confusing when they surface
// as strange behaviour three turns in, and this catches them at the door with
// a message that names the mod.

#include <cstdint>
#include <string>
#include <vector>

#include "../mods/ModPackage.h"

// One installed mod, as described to the other end.
struct ModAttestEntry {
    std::string id;
    std::string version;
    std::string sha256;      // lowercase hex of the whole .odmod
    ModSide     side = ModSide::Both;

    // "id@version#sha256", the form used on the wire and in server config.
    std::string toString() const;
    static bool parse(const std::string& text, ModAttestEntry& out);
};

// What one end has, sorted by id so two ends always produce the same bytes.
struct ModAttestation {
    std::vector<ModAttestEntry> entries;

    // Entries whose side is Both, which is the only set that has to match.
    std::vector<ModAttestEntry> shared() const;

    // SHA-256 over the canonical "id@version#sha256" lines of shared(), so a
    // single comparison answers "are we the same" before anything has to walk
    // the list to explain why not.
    std::string digest() const;

    void sort();
};

// Why a client was refused, so the caller can decide how loudly to say it.
enum class ModAttestVerdict {
    Ok = 0,
    Missing,        // the client does not have a mod the server requires
    VersionDiffers,
    BytesDiffer,    // same id and version, different file
    Extra,          // the client has a shared-side mod the server does not
};

struct ModAttestProblem {
    ModAttestVerdict verdict = ModAttestVerdict::Ok;
    std::string modId;
    std::string detail;     // one sentence, addressed to the player
};

// How strict a server is about a client bringing shared-side mods it does not
// have itself.
enum class ModExtraPolicy {
    // Refuse. The default: a "both"-side mod the server lacks expects server
    // behaviour that is not there, and the symptom is a client quietly
    // disagreeing with the world.
    Refuse = 0,
    // Allow, and let the player live with it. For servers that would rather
    // admit everyone than be right.
    Allow = 1,
};

struct ModAttestResult {
    bool ok = true;
    std::vector<ModAttestProblem> problems;

    // A single sentence naming the first problem, for a REJECT message.
    std::string summary() const;
};

// Compare what a client offers against what a server requires. `required` is
// the server's own shared() set.
ModAttestResult modAttestCompare(const std::vector<ModAttestEntry>& required,
                                 const ModAttestation& offered,
                                 ModExtraPolicy extras = ModExtraPolicy::Refuse);

// Serialise for the HELLO/WELCOME messages: one entry per line.
std::string        modAttestEncode(const ModAttestation& a);
bool               modAttestDecode(const std::string& text, ModAttestation& out);
