// ── The Scripts tab's block view ──
//
// Same script, drawn as nested blocks instead of lines. The text buffer and
// the block document are never edited at once: switching view regenerates the
// other side, so there is no merge to get wrong. That is only safe because
// text -> blocks -> text is byte-identical (see src/script/Blocks.cpp and the
// round-trip checks in tests/script_expr_test.cpp); without that guarantee,
// opening this view would quietly rewrite the author's file.
#include "MapEditor.h"

#include "Audio.h"
#include "TextInput.h"
#include "script/Blocks.h"

#include <algorithm>
#include <string>
#include <vector>

using odscript::Block;
using odscript::BlockList;

namespace {

// One colour per statement family, so shape is readable before the text is.
Color blockColor(Block::Kind k) {
    switch (k) {
        case Block::IF:                             return {104, 88, 168, 255};
        case Block::FOREACH: case Block::WHILE:
        case Block::FOR:     case Block::REPEAT:    return {58, 116, 148, 255};
        case Block::SET:                            return {52, 108, 76, 255};
        case Block::PRINT:                          return {132, 96, 48, 255};
        case Block::WAIT:                           return {140, 72, 96, 255};
        case Block::BREAK:  case Block::CONTINUE:   return {120, 60, 60, 255};
        case Block::INCLUDE: case Block::COLLECTION:return {70, 84, 110, 255};
        default:                                    return {74, 74, 86, 255};
    }
}

// The palette. Templates rather than a grammar: a new block arrives as text
// the parser already understands, which keeps one source of truth for syntax.
struct Template { const char* label; const char* head; const char* term; };
const Template kPalette[] = {
    {"set value",       "set var.name 0",                    nullptr},
    {"set expression",  "set var.name = 1 + 1",              nullptr},
    {"add to",          "set var.name += 1",                 nullptr},
    {"print",           "print \"message\"",                 nullptr},
    {"if",              "if var.name == 1",                  "endif"},
    {"unless",          "unless var.name == 1",              "endif"},
    {"for 1 to N",      "for var.i = 1 to 10",               "next"},
    {"repeat N",        "repeat 5",                          "next"},
    {"while",           "while var.name < 10",               "endwhile"},
    {"for each province","foreach province in country.USA",  "next"},
    {"break",           "break",                             nullptr},
    {"continue",        "continue",                          nullptr},
    {"wait until",      "waitUntil map.turn >= 10",          nullptr},
    {"include",         "include \"library\"",               nullptr},
};

bool pathLess(const std::vector<int>& a, const std::vector<int>& b) {
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i] != b[i]) return a[i] < b[i];
    return a.size() < b.size();
}

}  // namespace

// ── Document <-> text ──

void MapEditor::blocksFromText() {
    std::string joined;
    for (const auto& l : m_scriptEdLines) { joined += l; joined += '\n'; }
    m_blockDoc = odscript::parseScript(joined);
    m_blockSel.clear();
    m_blockDragPath.clear();
    m_blockScroll = 0;
}

void MapEditor::blocksToText() {
    const std::string text = odscript::unparseScript(m_blockDoc);
    m_scriptEdLines.clear();
    std::string cur;
    for (char c : text) {
        if (c == '\n') { m_scriptEdLines.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) m_scriptEdLines.push_back(cur);
    if (m_scriptEdLines.empty()) m_scriptEdLines.push_back("");
    m_scriptEdCurLine = std::min(m_scriptEdCurLine, (int)m_scriptEdLines.size() - 1);
    m_scriptEdCurCol = 0;
}

// ── Path addressing ──
//
// A path is a run of indices: every pair but the last is (block index, body
// index), and the final entry is the block's index in that body.

Block* MapEditor::blockAt(const std::vector<int>& path) {
    return odscript::blockAt(m_blockDoc.blocks, path);
}

bool MapEditor::removeBlockAt(const std::vector<int>& path, Block& out) {
    return odscript::removeBlockAt(m_blockDoc.blocks, path, out);
}

bool MapEditor::insertBlockAt(const std::vector<int>& path, Block&& b) {
    return odscript::insertBlockAt(m_blockDoc.blocks, path, std::move(b));
}

void MapEditor::flattenBlocks(const BlockList& bl, std::vector<int>& path,
                              int depth, std::vector<BlockRow>& out) const {
    for (size_t i = 0; i < bl.size(); ++i) {
        const Block& b = bl[i];
        path.push_back((int)i);
        BlockRow row;
        row.path = path;
        row.depth = depth;
        row.b = &b;
        row.label = b.head;
        out.push_back(row);
        path.pop_back();

        for (size_t a = 0; a < b.bodies.size(); ++a) {
            path.push_back((int)i);
            path.push_back((int)a);
            if (b.bodies[a].empty()) {
                BlockRow slot;
                slot.path = path;
                slot.path.push_back(0);      // insert at index 0 of that body
                slot.depth = depth + 1;
                slot.label = "drop a block here";
                slot.isSlot = true;
                out.push_back(slot);
            } else {
                flattenBlocks(b.bodies[a], path, depth + 1, out);
            }
            path.pop_back();
            path.pop_back();
            if (a < b.armHeads.size()) {
                BlockRow arm;
                arm.depth = depth;
                arm.label = b.armHeads[a];
                arm.isArm = true;
                arm.b = &b;
                out.push_back(arm);
            }
        }
        if (!b.terminator.empty()) {
            BlockRow term;
            term.depth = depth;
            term.label = b.terminator;
            term.isTerm = true;
            term.b = &b;
            out.push_back(term);
        }
    }
}

// ── The view ──

void MapEditor::drawBlockEditor(int x, int y, int w, int h) {
    static const Color kAccent = {255, 215, 0, 255};   // matches the editor's gold
    const Vector2 mouse = GetMousePosition();
    const Rectangle area = {(float)x, (float)y, (float)w, (float)h};
    DrawRectangle(x, y, w, h, Color{18, 18, 25, 255});

    if (!m_blockDoc.error.empty()) {
        // A script that does not close its blocks cannot be shown as blocks
        // without guessing where they end, and guessing would move code.
        DrawText("This script cannot be shown as blocks yet:", x + 14, y + 14, 14, Color{235, 120, 120, 255});
        DrawText(m_blockDoc.error.c_str(), x + 14, y + 34, 13, Color{220, 180, 180, 255});
        DrawText("Fix it in the text view and switch back.", x + 14, y + 58, 13, Color{150, 150, 170, 255});
        return;
    }

    std::vector<BlockRow> rows;
    { std::vector<int> path; flattenBlocks(m_blockDoc.blocks, path, 0, rows); }

    const int rowH = 26, pad = 10;
    const int listH = h - 84;                    // room for the bottom bar
    const int visRows = std::max(1, listH / rowH);
    if (CheckCollisionPointRec(mouse, {(float)x, (float)y, (float)w, (float)listH}))
        m_blockScroll -= (int)GetMouseWheelMove() * 2;
    m_blockScroll = std::max(0, std::min(m_blockScroll, std::max(0, (int)rows.size() - visRows)));

    // ── Rows ──
    int hoverRow = -1;
    for (int i = m_blockScroll; i < (int)rows.size() && i < m_blockScroll + visRows; ++i) {
        const BlockRow& r = rows[i];
        const int ry = y + pad + (i - m_blockScroll) * rowH;
        const int rx = x + pad + r.depth * 18;
        // Sized to the text, the way a block in a block editor is: a row of
        // full-width bars reads as a list, not as structure.
        const int textW = MeasureText(r.label.c_str(), 13);
        const int rw = std::min(w - (rx - x) - pad * 2, std::max(140, textW + 26));
        const Rectangle rect = {(float)rx, (float)ry, (float)rw, (float)(rowH - 4)};
        const bool hov = CheckCollisionPointRec(mouse, rect);
        if (hov) hoverRow = i;

        if (r.isSlot) {
            DrawRectangleRoundedLines(rect, 0.3f, 4,
                                      hov && !m_blockDragPath.empty() ? kAccent : Color{80, 80, 100, 255});
            DrawText(r.label.c_str(), rx + 10, ry + 5, 12, Color{110, 110, 130, 255});
            continue;
        }
        if (r.isArm || r.isTerm) {
            // Arms and terminators belong to their opener; they are drawn so
            // the shape reads, but they are not separately draggable.
            DrawRectangleRounded(rect, 0.35f, 4, Color{40, 40, 52, 200});
            DrawText(r.label.c_str(), rx + 10, ry + 4, 13, Color{150, 150, 170, 255});
            continue;
        }

        const bool selected = (r.path == m_blockSel);
        const bool dragging = (!m_blockDragPath.empty() && r.path == m_blockDragPath);
        Color c = blockColor(r.b->kind);
        if (dragging) c = ColorAlpha(c, 0.45f);
        DrawRectangleRounded(rect, 0.25f, 4, c);
        if (selected) DrawRectangleRoundedLines(rect, 0.25f, 4, kAccent);
        else if (hov)  DrawRectangleRoundedLines(rect, 0.25f, 4, Color{255, 255, 255, 60});

        // Trimmed against the SAME measurement that sized the block. The first
        // version sized from MeasureText and then trimmed at a flat seven
        // pixels per character, so every block lost its last few characters --
        // `10000 + 500` came out as `10000 + 5`. And the ellipsis is "..."
        // because the font has no glyph for the single-character one; it drew
        // as a question mark, which reads as part of the code.
        std::string label = r.label;
        const int avail = rw - 20;
        if (MeasureText(label.c_str(), 13) > avail) {
            while (label.size() > 4 && MeasureText((label + "...").c_str(), 13) > avail)
                label.pop_back();
            label += "...";
        }
        DrawText(label.c_str(), rx + 10, ry + 4, 13, WHITE);

        // Comments that sit above a block, shown small so they are not lost.
        if (!r.b->lead.empty() && !dragging) {
            int n = 0;
            for (const auto& l : r.b->lead) if (!l.empty()) ++n;
            if (n > 0) DrawText(TextFormat("%d", n), rx + rw - 18, ry + 6, 11, Color{140, 140, 160, 200});
        }

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            // Light, and only when the selection actually moves: picking the
            // same block up twice should not click twice.
            if (m_blockSel != r.path) Audio::get().playSfx("click_light", 0.5f);
            m_blockSel = r.path;
            m_blockHeadEditing = false;
            m_blockDragArmed = true;
            m_blockDragStart = mouse;
        }
    }

    // ── Drag ──
    if (m_blockDragArmed && m_blockDragPath.empty() && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        // A few pixels of travel before it counts, so a click that wobbles
        // does not silently move code.
        if (std::abs(mouse.y - m_blockDragStart.y) > 5 || std::abs(mouse.x - m_blockDragStart.x) > 5) {
            m_blockDragPath = m_blockSel;
            Audio::get().playSfx("panel_open", 0.35f);   // lifted
        }
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) m_blockDragArmed = false;

    if (!m_blockDragPath.empty()) {
        // Where would it land? Before the hovered row, or at the end.
        m_blockDropPath.clear();
        if (hoverRow >= 0 && !rows[hoverRow].isArm && !rows[hoverRow].isTerm)
            m_blockDropPath = rows[hoverRow].path;
        else if (hoverRow < 0 && CheckCollisionPointRec(mouse, area) && !m_blockDoc.blocks.empty())
            m_blockDropPath = {(int)m_blockDoc.blocks.size()};

        if (!m_blockDropPath.empty() && !odscript::pathContains(m_blockDragPath, m_blockDropPath)) {
            // The insertion line, drawn where the block would go.
            for (int i = m_blockScroll; i < (int)rows.size() && i < m_blockScroll + visRows; ++i) {
                if (rows[i].path != m_blockDropPath) continue;
                const int ry = y + pad + (i - m_blockScroll) * rowH - 2;
                DrawRectangle(x + pad, ry, w - pad * 2, 2, kAccent);
                break;
            }
        }

        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (!m_blockDropPath.empty() && m_blockDropPath != m_blockDragPath &&
                !odscript::pathContains(m_blockDragPath, m_blockDropPath)) {
                Block moved;
                std::vector<int> dst = m_blockDropPath;
                if (removeBlockAt(m_blockDragPath, moved)) {
                    // Removing from the same list shifts everything after it,
                    // so a drop that pointed past the old position must come
                    // back by one or the block lands one slot too far down.
                    const bool sameParent =
                        dst.size() == m_blockDragPath.size() &&
                        std::equal(dst.begin(), dst.end() - 1, m_blockDragPath.begin());
                    if (sameParent && dst.back() > m_blockDragPath.back()) dst.back()--;
                    if (!insertBlockAt(dst, std::move(moved))) {
                        // Put it back rather than lose it.
                        insertBlockAt(m_blockDragPath, std::move(moved));
                    } else {
                        m_blockSel = dst;
                        Audio::get().playSfx("click_heavy", 0.55f);   // landed
                        trackChange();
                    }
                }
            }
            else if (!m_blockDragPath.empty() && m_blockDropPath.empty())
                Audio::get().playSfx("deny", 0.35f);   // let go over nothing
            m_blockDragPath.clear();
            m_blockDropPath.clear();
        }
    }

    // ── Bottom bar: edit / add / delete ──
    const int barY = y + h - 74;
    DrawRectangle(x, barY, w, 74, Color{26, 26, 34, 255});
    DrawLine(x, barY, x + w, barY, Color{60, 60, 76, 255});

    Block* sel = m_blockSel.empty() ? nullptr : blockAt(m_blockSel);
    if (sel) {
        const Rectangle field = {(float)(x + 12), (float)(barY + 10), (float)(w - 210), 26};
        const bool fhov = CheckCollisionPointRec(mouse, field);
        DrawRectangleRounded(field, 0.15f, 4, Color{35, 35, 48, 255});
        DrawRectangleRoundedLines(field, 0.15f, 4, m_blockHeadEditing ? kAccent : Color{70, 70, 90, 255});
        if (!m_blockHeadEditing && fhov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            m_blockHeadEditing = true;
            m_blockHeadBuf = sel->head;
        }
        const std::string shown = m_blockHeadEditing ? m_blockHeadBuf : sel->head;
        DrawText(shown.c_str(), (int)field.x + 8, (int)field.y + 5, 14, WHITE);
        if (m_blockHeadEditing) {
            DrawText("|", (int)field.x + 8 + MeasureText(shown.c_str(), 14), (int)field.y + 5, 14, kAccent);
            int key = GetCharPressed();
            while (key > 0) {
                if (key >= 32 && key < 127 && m_blockHeadBuf.size() < 200) {
                    m_blockHeadBuf.push_back((char)key);
                    Audio::get().playSfx("key_type", 0.12f);
                }
                key = GetCharPressed();
            }
            odTextEditKeys(m_blockHeadBuf, 200);
            if (IsKeyPressed(KEY_ENTER)) {
                // Re-classify on commit: editing `set x 1` into `if x == 1`
                // must become an if, terminator and all, or the document would
                // hold a statement that opens nothing and never closes.
                const Block::Kind was = sel->kind;
                sel->head = m_blockHeadBuf;
                sel->kind = odscript::classify(sel->head);
                const bool nowOpens = sel->kind == Block::IF || sel->kind == Block::FOREACH ||
                                      sel->kind == Block::WHILE || sel->kind == Block::FOR ||
                                      sel->kind == Block::REPEAT;
                const bool wasOpener = was == Block::IF || was == Block::FOREACH ||
                                       was == Block::WHILE || was == Block::FOR ||
                                       was == Block::REPEAT;
                if (nowOpens && !wasOpener) {
                    sel->bodies.assign(1, BlockList{});
                    sel->terminator = (sel->kind == Block::IF) ? "endif"
                                    : (sel->kind == Block::WHILE) ? "endwhile" : "next";
                } else if (!nowOpens && wasOpener) {
                    sel->bodies.clear();
                    sel->armHeads.clear();
                    sel->terminator.clear();
                }
                m_blockHeadEditing = false;
                Audio::get().playSfx("confirm", 0.5f);
                trackChange();
            }
            if (IsKeyPressed(KEY_ESCAPE)) {
                m_blockHeadEditing = false;
                Audio::get().playSfx("back", 0.4f);
            }
        }

        if (drawButton("Delete", {(float)(x + w - 186), (float)(barY + 10), 84, 26}, false, 13)) {
            Block gone;
            if (removeBlockAt(m_blockSel, gone)) {
                m_blockSel.clear();
                Audio::get().playSfx("deny", 0.45f);
                trackChange();
            }
        }
    } else {
        DrawText("Select a block to edit it, or add one below.",
                 x + 14, barY + 16, 13, Color{140, 140, 160, 255});
    }

    if (drawButton(m_blockPaletteOpen ? "Close palette" : "+ Add block",
                   {(float)(x + w - 96), (float)(barY + 10), 84, 26}, m_blockPaletteOpen, 13))
    {
        m_blockPaletteOpen = !m_blockPaletteOpen;
        Audio::get().playSfx(m_blockPaletteOpen ? "panel_open" : "panel_close", 0.45f);
    }

    DrawText(TextFormat("%d blocks", (int)rows.size()), x + 14, barY + 46, 12, Color{120, 120, 140, 255});

    // ── Palette ──
    if (m_blockPaletteOpen) {
        const int pw = 220, ph = std::min(h - 40, (int)(sizeof(kPalette) / sizeof(kPalette[0])) * 24 + 16);
        const int pxx = x + w - pw - 12, pyy = y + h - 84 - ph - 6;
        DrawRectangleRounded({(float)pxx, (float)pyy, (float)pw, (float)ph}, 0.04f, 6, Color{30, 30, 40, 245});
        DrawRectangleRoundedLines({(float)pxx, (float)pyy, (float)pw, (float)ph}, 0.04f, 6, kAccent);
        for (size_t i = 0; i < sizeof(kPalette) / sizeof(kPalette[0]); ++i) {
            const Rectangle rr = {(float)(pxx + 8), (float)(pyy + 8 + (int)i * 24), (float)(pw - 16), 22};
            const bool hov = CheckCollisionPointRec(mouse, rr);
            if (hov) DrawRectangleRounded(rr, 0.3f, 4, Color{255, 255, 255, 20});
            DrawText(kPalette[i].label, (int)rr.x + 8, (int)rr.y + 4, 13, hov ? kAccent : WHITE);
            if (hov && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                Block nb;
                nb.head = kPalette[i].head;
                nb.kind = odscript::classify(nb.head);
                if (kPalette[i].term) {
                    nb.terminator = kPalette[i].term;
                    nb.bodies.assign(1, BlockList{});
                }
                // After the selection when there is one, else at the end.
                // Into the selected container if it is one, else after it.
                // Adding an `if` while an empty `for` is selected should put it
                // in the loop -- that is what the selection means on screen.
                std::vector<int> dst;
                Block* selBlk = m_blockSel.empty() ? nullptr : blockAt(m_blockSel);
                if (selBlk && !selBlk->bodies.empty()) {
                    dst = m_blockSel;
                    dst.push_back(0);                                  // body 0
                    dst.push_back((int)selBlk->bodies[0].size());      // at its end
                } else if (!m_blockSel.empty()) {
                    dst = m_blockSel; dst.back()++;
                } else {
                    dst = {(int)m_blockDoc.blocks.size()};
                }
                if (insertBlockAt(dst, std::move(nb))) {
                    m_blockSel = dst;
                    Audio::get().playSfx("confirm", 0.5f);
                    trackChange();
                }
                m_blockPaletteOpen = false;
            }
        }
    }
}
