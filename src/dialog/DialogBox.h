#pragma once

// The visual-novel textbox: a parsed script on screen, one page at a time.
//
// WHAT IT OWNS
//   the page you are on, how much of it has been typed, and where every glyph
//   sits. Nothing else: it does not own the script file, the fonts, or the
//   screen it draws into. Those are handed in.
//
// HOW A PLAYER DRIVES IT (the classic contract, and the only one people expect)
//   click while it is typing  -> the rest of the page appears at once
//   click when it has finished -> the next page
//   click on the last page     -> it closes
//
// Layout is per GLYPH rather than per line, because half the effects here are
// per glyph: a wave needs each letter's own phase, a rainbow needs each
// letter's own hue, and a shake needs each letter's own jitter. Drawing a run
// with one DrawText call cannot do any of it.

#include "DialogScript.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "raylib.h"

namespace dlg {

/** One laid-out glyph: what it is, where it goes, and how it behaves. */
struct Placed {
    int   codepoint = 0;
    Style style;
    float x = 0, y = 0;      ///< relative to the text origin
    float advance = 0;
    int   size = 0;          ///< resolved point size (page size + style delta)
    int   line = 0;
    int   indexInPage = 0;   ///< typewriter order, spaces included
};

class Box {
public:
    /// Fonts are borrowed, not owned: the same hybrid pair the rest of the game
    /// uses -- raylib's default for ASCII, Unifont for everything else.
    void setFonts(Font ascii, Font unicode) { m_ascii = ascii; m_unicode = unicode; }

    /// How {key=action} is answered. The box does not know what a keybind is,
    /// and should not: the game owns the bindings and may change them while
    /// the script is open.
    /**
     * How a page's speaker is written on the plate above the box.
     *
     * The script keys on a formal name because that is a stable id -- but
     * "Signals Officer" on the plate tells a new player nothing about who is
     * suddenly talking to them. The game answers with the name, the role and
     * a short tag; the box has no cast list of its own.
     */
    struct Label { std::string name, role, tag; };
    void setLabelResolver(std::function<Label(const std::string&)> f) {
        m_labelResolver = std::move(f);
    }

    void setKeyResolver(std::function<std::string(const std::string&)> f) {
        m_keyResolver = std::move(f);
        m_dirty = true;
    }

    /** Load a script, or report why not. `dialogDir` is data/dialog. */
    bool open(const std::string& dialogDir, const std::string& name,
              const std::string& language);
    /** Show an already-parsed script (used by tests and by generated dialogue). */
    void openScript(Script script);

    /**
     * Jump straight to a page.
     *
     * For a scene that starts part way in, for a "skip to where I was" resume,
     * and for the screenshot tour, which has no way to click.
     */
    void jumpTo(int page);

    // Closing forgets the answer. A key is a one-shot instruction to the
    // caller -- "open that script", "go back to the world" -- and the caller
    // reads it by polling, every frame, for as long as it is set. Left behind,
    // the choice the player made on the way out is obeyed again on the way in:
    // picking "cover something specific" reloaded the tutorial world once a
    // frame, forever. The LABEL stays: {choice} echoes it back in the script
    // that was opened, which is across exactly this boundary.
    void close() { m_open = false; m_pickedKey.clear(); }
    bool isOpen() const { return m_open; }

    /// The rectangle the box occupies. Set before update/draw; layout is redone
    /// when it changes, and only then.
    void setBounds(Rectangle r);

    /**
     * Advance the typewriter and consume a click.
     *
     * `clicked` is passed in rather than read here so the caller decides what
     * counts -- a click anywhere, a key, a pad button -- and so the box does
     * not fight whatever else is on screen for the mouse.
     */
    /// `mouse` is where the pointer is, for hovering an option. Pass
    /// {-1,-1} when there is no pointer.
    void update(float dt, bool clicked, Vector2 mouse = {-1.0f, -1.0f});

    // ── Choices ──────────────────────────────────────────────────────────
    //
    // A page carrying options does not advance on a click. It stops, shows
    // them, and waits -- so the caller must ask whether it is waiting rather
    // than assuming a click always moves the script on.

    /// True when the page is fully typed and offering options.
    bool awaitingChoice() const;

    /**
     * Answer the current page's `until`.
     *
     * The box cannot evaluate one -- it does not know what a province is --
     * so the game works it out and says so every frame. Until this is true, a
     * page carrying a condition will not turn.
     */
    void setConditionMet(bool met) { m_conditionMet = met; }
    /// True when the page is finished typing and still waiting on its `until`.
    bool awaitingCondition() const;
    /// What the player picked most recently, as the script's `key`. Empty
    /// until something has been picked.
    const std::string& pickedKey() const { return m_pickedKey; }
    /// And what they read, for echoing back with {choice}.
    const std::string& pickedLabel() const { return m_pickedLabel; }
    /// Move the highlight, for keyboard and pad. Wraps.
    void moveSelection(int delta);
    /// Which option is highlighted. -1 when the page is not asking anything.
    int selectionIndex() const { return awaitingChoice() ? m_sel : -1; }
    /// Take the highlighted option. No-op unless awaitingChoice().
    void commitChoice();

    void draw(Color accent) const;

    /// Where the script asked to be anchored, for a caller that wants to place
    /// the box itself.
    Anchor anchor() const;

    /// The page being shown, for a caller that needs its speaker or its pose.
    const Page* currentPage() const {
        return (m_page >= 0 && m_page < (int)m_script.pages.size())
                   ? &m_script.pages[m_page] : nullptr;
    }
    const std::vector<std::string>& warnings() const { return m_script.warnings; }
    int pageIndex() const { return m_page; }
    int pageCount() const { return (int)m_script.pages.size(); }
    /// True once every glyph of the current page is on screen.
    bool pageComplete() const;

    /// How many glyphs of the page are on screen so far, and what the i-th
    /// one is -- so a speaker's mouth can follow the typewriter letter by
    /// letter rather than flapping on a timer. Counts in typewriter order.
    int revealedCount() const {
        return std::min((int)m_revealed, (int)m_glyphs.size());
    }
    int codepointAt(int i) const {
        return (i >= 0 && i < (int)m_glyphs.size()) ? m_glyphs[i].codepoint : 0;
    }

private:
    void layout();
    float glyphAdvanceFor(int codepoint, int size) const;
    Font  fontFor(int codepoint) const;
    /// Recompute whether the current page needs the Unicode atlas.
    void  refreshPageScript();
    enum class Prompt { More, End, Waiting };
    void  drawPrompt(Prompt kind, int size, float alpha, Color accent) const;

    Script m_script;
    int    m_page = 0;
    bool   m_open = false;

    Rectangle m_bounds{};
    bool      m_dirty = true;

    // Choices. `m_sel` is the highlight, which the mouse and the keyboard
    // both move; `m_choiceRects` is recomputed on every draw-sized layout.
    int         m_sel = 0;
    mutable std::vector<Rectangle> m_choiceRects;
    std::string m_pickedKey, m_pickedLabel;
    std::function<std::string(const std::string&)> m_keyResolver;
    std::function<Label(const std::string&)> m_labelResolver;
    bool m_conditionMet = false;
    /// Counts down once a page's `until` comes true; see update().
    float m_autoAdvance = 0.0f;
    /// Turn to the next page, or close if this was the last. Shared by the
    /// click, the choice and the condition being satisfied.
    void nextPage();

    std::vector<Placed> m_glyphs;
    int   m_lineCount = 0;
    float m_textHeight = 0;

    float m_revealed = 0.0f;   ///< glyphs shown so far, fractional
    float m_time = 0.0f;       ///< drives shake/wave/rainbow/obfuscation
    float m_pageAge = 0.0f;    ///< for the fade and rise entrances

    /// True when a string can be drawn by the ASCII font; see DialogBox.cpp.
    static bool isAsciiOnly(const std::string& s);

    /// True while the page on screen contains non-ASCII; see fontFor().
    bool m_pageNeedsAtlas = false;

    Font m_ascii{};
    Font m_unicode{};
};

}  // namespace dlg
