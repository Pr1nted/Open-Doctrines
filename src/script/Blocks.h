#pragma once
// ── Script text as a tree of blocks, and back ──
//
// A Scratch-style editor needs the script as nested blocks; the engine, the
// map file and every existing script need it as text. This turns each into the
// other, and the round trip is the whole contract: a script opened in the block
// editor and saved without being touched must come back BYTE FOR BYTE, or the
// editor silently rewrites people's files.
//
// Two rules make that achievable here. Structure is explicit -- one statement
// per line, every opener closed by name (`endif`, `next`, `endwhile`) -- so
// nesting is never inferred from indentation, which the language treats as
// cosmetic. And anything this file does not recognise is carried through
// verbatim as a RAW block rather than dropped, so a script using something
// newer than the editor survives a visit to it.
#include <string>
#include <vector>

namespace odscript {

struct Block;
using BlockList = std::vector<Block>;

struct Block {
    enum Kind {
        RAW,        // anything not recognised — carried through untouched
        SET,        // set <ref> <value> | set <ref> = <expr>
        PRINT,
        INCLUDE,
        WAIT,       // waitUntil
        COLLECTION, // array … / list …
        BREAK,
        CONTINUE,
        IF,         // holds arms: the `if` body, then each elseif/else body
        FOREACH,
        WHILE,
        FOR,
        REPEAT,
    };

    Kind kind = RAW;
    std::string head;                 // the statement line, verbatim minus indent
    std::vector<std::string> lead;    // comments/blank lines that sit above it

    // Bodies. A plain block has one; an `if` has one per arm, and `armHeads`
    // holds the `elseif …`/`else` line that opens arms 1..n.
    std::vector<BlockList> bodies;
    std::vector<std::string> armHeads;

    std::string terminator;           // "endif" / "next" / "endwhile" as written
    std::vector<std::string> tail;    // comments directly above the terminator
};

struct Doc {
    BlockList blocks;
    std::vector<std::string> header;  // #OD/MapEngine/N and anything above the first statement
    std::vector<std::string> trailing;// comments after the last statement
    std::string error;                // non-empty if the structure did not close
};

// Text -> blocks. Never fails destructively: on a structural error the message
// lands in Doc::error and what parsed is still returned.
Doc parseScript(const std::string& text);

// Blocks -> text. `indent` spaces per nesting level.
std::string unparseScript(const Doc& doc, int indent = 4);

// `x++`, `++x`, `x--`, `--x` and a bare `x += 1` become the `set` form they
// stand for; anything else comes back unchanged. Shared by the engine (which
// runs the result) and the editor (which classifies and highlights it), so
// the shorthand cannot mean one thing to one and something else to the other.
std::string normaliseAssignment(const std::string& raw);

// The statement kind of a line, exposed for the editor's palette.
Block::Kind classify(const std::string& line);

// ── Addressing a block inside the tree ──
//
// A PATH is a run of indices: pairs of (block index, body index) for each
// level entered, then the index within the final body. Paths rather than
// pointers, because the editor reorders these vectors and a pointer into one
// does not survive that. These live here rather than in the editor so the
// move logic -- the part that can lose somebody's code -- is testable.
Block* blockAt(BlockList& root, const std::vector<int>& path);
bool removeBlockAt(BlockList& root, const std::vector<int>& path, Block& out);
bool insertBlockAt(BlockList& root, const std::vector<int>& path, Block&& b);

// True when `inner` addresses something at or inside `outer`.
bool pathContains(const std::vector<int>& outer, const std::vector<int>& inner);

}  // namespace odscript
