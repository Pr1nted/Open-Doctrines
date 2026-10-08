#pragma once

// Deflate for the network, kept in one place so the protocol layer has one
// dependency on a compressor and not one per message.
//
// Used for the world a late joiner is sent, which grows with every turn a
// campaign plays: the turns themselves are compact binary, but the state that
// rides along is JSON, and the whole of it shrinks several times over.

#include <cstddef>
#include <cstdint>
#include <vector>

/** Deflated `in`. Empty only when `in` is empty or compression failed. */
std::vector<uint8_t> netDeflate(const std::vector<uint8_t>& in);

/**
 * Inflate exactly `rawSize` bytes, refusing anything larger than `maxRaw`.
 *
 * The size comes off the wire, so it is checked against the ceiling BEFORE
 * anything is allocated, and the result must come out to exactly that size --
 * a stream that inflates short or long is not the one that was sent.
 */
bool netInflate(const uint8_t* data, size_t size, size_t rawSize, size_t maxRaw,
                std::vector<uint8_t>& out);
