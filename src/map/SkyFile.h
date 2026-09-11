#pragma once

// The sky a map carries, on disk.
//
// Stored as sky.json inside the .odmap, beside policies.json and the rest --
// because a scenario is not always Earth, and the map format's own rule is that
// a map embeds its data and that copy wins. A nuclear-winter scenario wants a
// colder sun; something that is not Earth wants no moon at all.
//
// EVERY FIELD IS OPTIONAL. A map with no sky.json, or with half of one, gets
// the Earth-like defaults for whatever it did not say. That is deliberate: this
// went into a format with 118 files already in it, and a loader that refuses a
// map because it predates a feature is a loader that breaks every existing map.

#include "../renderer/GlobeView.h"

#include <string>

namespace skyfile {

/// Read `path` into `out` and `night`. False when there is no file -- not an
/// error. Fields the document omits keep whatever the outputs already held.
bool load(const std::string& path, GlobeView::Sky& out, GlobeView::Night& night);
inline bool load(const std::string& path, GlobeView::Sky& out) {
    GlobeView::Night ignored;
    return load(path, out, ignored);
}

/// Write `s` and `night` to `path`. False only if the file could not be written.
bool save(const std::string& path, const GlobeView::Sky& s, const GlobeView::Night& night);
inline bool save(const std::string& path, const GlobeView::Sky& s) {
    return save(path, s, GlobeView::Night{});
}

}  // namespace skyfile
