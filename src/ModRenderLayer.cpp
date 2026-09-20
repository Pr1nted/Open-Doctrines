#include "ModRenderLayer.h"

#include <algorithm>

namespace odrender {
namespace {

/// An entry for a province, or the end of the list.
template <typename T>
typename std::vector<T>::iterator findProvince(std::vector<T>& v, int pid) {
    return std::find_if(v.begin(), v.end(),
                        [pid](const T& e) { return e.provinceId == pid; });
}

}  // namespace

Layer::ModMarks* Layer::forMod(const std::string& modId, bool create) {
    auto it = std::lower_bound(m_byMod.begin(), m_byMod.end(), modId,
                               [](const auto& e, const std::string& id) {
                                   return e.first < id;
                               });
    if (it != m_byMod.end() && it->first == modId) return &it->second;
    if (!create) return nullptr;
    // Inserted in order, so tints() and labels() come out stable without a
    // sort on every frame.
    it = m_byMod.insert(it, {modId, ModMarks{}});
    return &it->second;
}

const Layer::ModMarks* Layer::forMod(const std::string& modId) const {
    return const_cast<Layer*>(this)->forMod(modId, false);
}

bool Layer::setTint(const std::string& modId, int provinceId, uint32_t rgba) {
    if (modId.empty()) return false;

    // Zero alpha means REMOVE. One call for set and clear, so a mod cannot
    // grow the list by "clearing" with transparent paint -- which is what a
    // naive implementation would do on every frame of a fading effect.
    const bool erasing = (rgba & 0xFFu) == 0;

    ModMarks* m = forMod(modId, !erasing);
    if (!m) return erasing;                 // nothing stored, nothing to erase

    auto at = findProvince(m->tints, provinceId);
    if (erasing) {
        if (at != m->tints.end()) m->tints.erase(at);
        return true;
    }
    if (at != m->tints.end()) { at->rgba = rgba; return true; }
    // THE CEILING IS A REFUSAL, NOT A SLOWER GAME. A mod that asks for more
    // than it can have is told so; the alternative is a player paying for the
    // mistake in frame time with nothing on screen to explain it.
    if (m->tints.size() >= kMaxTintsPerMod) return false;
    m->tints.push_back({provinceId, rgba});
    return true;
}

bool Layer::setLabel(const std::string& modId, int provinceId,
                     const std::string& text, uint32_t rgba) {
    if (modId.empty()) return false;
    const bool erasing = text.empty() || (rgba & 0xFFu) == 0;

    ModMarks* m = forMod(modId, !erasing);
    if (!m) return erasing;

    auto at = findProvince(m->labels, provinceId);
    if (erasing) {
        if (at != m->labels.end()) m->labels.erase(at);
        return true;
    }
    // Truncated rather than refused: a label one character too long is a
    // cosmetic mistake, and failing the call would make a mod author debug a
    // silent nothing instead of seeing a clipped word.
    std::string t = text.size() > kMaxLabelChars ? text.substr(0, kMaxLabelChars) : text;
    // A control character in a label is a drawing bug at best; strip rather
    // than refuse, same reasoning.
    t.erase(std::remove_if(t.begin(), t.end(),
                           [](unsigned char c) { return c < 0x20 || c == 0x7F; }),
            t.end());
    if (t.empty()) {
        if (at != m->labels.end()) m->labels.erase(at);
        return true;
    }

    if (at != m->labels.end()) { at->text = t; at->rgba = rgba; return true; }
    if (m->labels.size() >= kMaxLabelsPerMod) return false;
    m->labels.push_back({provinceId, t, rgba});
    return true;
}

void Layer::clearMod(const std::string& modId) {
    auto it = std::lower_bound(m_byMod.begin(), m_byMod.end(), modId,
                               [](const auto& e, const std::string& id) {
                                   return e.first < id;
                               });
    if (it != m_byMod.end() && it->first == modId) m_byMod.erase(it);
}

void Layer::clear() { m_byMod.clear(); }

size_t Layer::tintCount(const std::string& modId) const {
    const ModMarks* m = forMod(modId);
    return m ? m->tints.size() : 0;
}
size_t Layer::labelCount(const std::string& modId) const {
    const ModMarks* m = forMod(modId);
    return m ? m->labels.size() : 0;
}

std::vector<Tint> Layer::tints() const {
    std::vector<Tint> out;
    for (const auto& [id, m] : m_byMod)
        out.insert(out.end(), m.tints.begin(), m.tints.end());
    return out;
}

std::vector<Label> Layer::labels() const {
    std::vector<Label> out;
    for (const auto& [id, m] : m_byMod)
        out.insert(out.end(), m.labels.begin(), m.labels.end());
    return out;
}

bool Layer::empty() const {
    for (const auto& [id, m] : m_byMod)
        if (!m.tints.empty() || !m.labels.empty()) return false;
    return true;
}

}  // namespace odrender
