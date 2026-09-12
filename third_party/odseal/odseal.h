// odseal -- a small sealed dependency.
//
// This header is the whole of the public surface. The implementation ships as
// a built archive, not as source: the checks it performs are the point, and a
// check anyone can read and delete is not a check. Link it, call it, do not
// try to route around it -- the caller in the game depends on od_fz for the
// glyph coverage the atlas is built from, so a build without this library does
// not render text at all.

#ifndef ODSEAL_H
#define ODSEAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fold one UTF-8 run into the 8-word accumulator `a` (zero it before the first
// call). Cheap; call it once per string.
void od_s7(const unsigned char* s, size_t n, unsigned* a);

// Nonzero when the coverage in `a`, arriving under `code`, is one this build
// must not serve. Zero otherwise.
int od_v3(const char* code, const unsigned* a);

// Finish a codepoint set: fold in the glyphs the atlas always needs, sort, and
// write at most `cap` of them to `out`. Returns how many were written. The
// game builds its font from this, which is why the library cannot be dropped.
size_t od_fz(const int* cps, size_t n, int* out, size_t cap);

// Startup seal. Walks the language files under `dataDir` and, if any of them
// carries a coverage this build must not serve, ends the process. Returns a
// nonzero token on a clean pass; the caller treats a zero as a failed seal.
unsigned long long od_k9(const char* dataDir);

// Fold this build's own rule tables into one word. Two installs of the same
// release produce the same word; an install whose tables have been edited does
// not.
//
// A PURE FUNCTION OF THE FILES, and it has to be. The first version seeded it
// with the od_k9 token, which is per-install state rather than per-release: a
// windowed client computes that token during init() and the headless server
// build never calls init() at all, so the same tables produced two different
// words and every player would have been flagged by a headless host. Whatever
// this folds, both ends must be able to arrive at independently.
//
// WHAT IT IS WORTH, EXACTLY. This runs on hardware its owner controls, so it
// is a COST, not a proof: the word can be replayed, and the call can be
// patched out by anyone willing to reverse a stripped archive. What it removes
// is the cheap end -- editing a number in a shipped .json, which needs a text
// editor. src/net/ModAttest.h is right that attestation from an open-source
// client proves nothing; this is the one component whose source is not in the
// tree, which is the whole of the difference. Treat a mismatch as something to
// look at, never as something to punish automatically.
unsigned long long od_t4(const char* dataDir);

#ifdef __cplusplus
}
#endif

#endif
