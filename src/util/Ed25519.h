#pragma once

// Ed25519 signature VERIFICATION, RFC 8032.
//
// WHY THE GAME NEEDS THIS
//
// Game connections no longer pass through the account service, so nobody else
// is left to check a join ticket. The host must do it, on its own machine, with
// only the account service's PUBLIC key. That is the whole point of a signature
// here: a host can be certain a ticket was issued by the account service
// without being able to issue one, and without the account service being
// involved in the connection at all.
//
// VERIFY ONLY, DELIBERATELY
//
// There is no signing function in this file and there should never be one. The
// game never holds the private key -- only the Worker does -- so the entire
// secret-handling surface is absent: no key storage, no random number
// generator, no constant-time discipline over secret data. Everything here
// operates on values an attacker already has (a public key, a signature and a
// message they sent us), which is what makes a compact implementation
// defensible instead of reckless.
//
// PROVENANCE
//
// The field arithmetic and group operations follow the TweetNaCl reference
// implementation by Bernstein, van Gastel, Janssen, Lange, Schwabe and
// Smetsers, which its authors placed in the public domain. It is the most
// reviewed compact Ed25519 in existence; deviating from it to be clever would
// be the mistake. What is added here is an explicit check that the scalar S is
// reduced (RFC 8032 section 5.1.7), which TweetNaCl omits.
//
// Correctness is pinned by the RFC 8032 section 7.1 test vectors in
// tests/net_crypto_test.cpp. As with the WebSocket handshake, the vectors come
// from outside this codebase, because an implementation that only agrees with
// itself proves nothing.

#include <cstddef>
#include <cstdint>

/**
 * True if `sig` is a valid signature by `pub` over `msg`.
 *
 * False for every kind of failure -- bad signature, malformed public key, a
 * non-reduced scalar -- and callers must not distinguish them. A verifier that
 * reports WHICH check failed is an oracle.
 */
bool ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t msgLen,
                   const uint8_t pub[32]);
