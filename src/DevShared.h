#pragma once
#include <cstddef>

/**
 * The layout both sides of the live modding link agree on.
 *
 * COPIED, NOT SHARED. The editor has its own copy of this file, because the
 * two programs are built separately and neither should have to include the
 * other's tree to compile. The magic numbers are what make a mismatch a
 * refusal rather than a garbled frame: change anything here and bump
 * kFrameMagic, and an editor built against the old layout will politely show
 * nothing instead of interpreting pixels as key codes.
 *
 * ONE MAPPING, BOTH DIRECTIONS. The frame goes out, the pointer and keys come
 * back. A single file means a single path to agree on and a single thing to
 * clean up.
 */
namespace devshared {

/// Bumped whenever anything below changes shape.
constexpr char kFrameMagic[8] = {'O', 'D', 'F', 'R', 'A', 'M', 'E', '3'};
constexpr unsigned kInputMagic = 0x4F44494Eu;  // "ODIN"

constexpr int kViewWidth = 480;
constexpr int kViewHeight = 270;

constexpr unsigned kMaxKeys = 32;
constexpr unsigned kMaxChars = 16;

/**
 * Written by the game, read by the editor.
 *
 * `sequence` is a seqlock: odd while the pixels are being written, even when
 * they are whole. A reader that sees the same even value before and after its
 * copy read one frame and not a mixture of two. It is the only synchronisation
 * here, and deliberately so -- a lock shared between two processes lets either
 * one wedge the other by dying at the wrong moment.
 */
struct FrameHeader {
    char magic[8];
    unsigned width;   ///< the published picture's size
    unsigned height;
    unsigned sequence;
    /**
     * The game's own screen, which is NOT the published size.
     *
     * The editor has to send a pointer position in these coordinates, because
     * that is the space the game's own input lives in. Sending them in the
     * published size put every click in the top-left sixth of the screen --
     * the view looked right and nothing was where it was clicked.
     */
    unsigned screenWidth;
    unsigned screenHeight;
};

/**
 * Written by the editor, read by the game.
 *
 * `driving` is the whole protocol: it is 1 while the pointer is over the
 * editor's view and 0 the moment it leaves, and the game merges this input
 * with its own only while it is set. Without it, a key held when the pointer
 * left the view would stay held in the game forever.
 */
struct InputBlock {
    unsigned magic;
    unsigned sequence;  ///< bumped on every update, so a stale block is visible
    unsigned driving;
    int mouseX;
    int mouseY;
    unsigned buttons;  ///< bit per MOUSE_BUTTON_*
    float wheel;
    unsigned keyCount;
    int keys[kMaxKeys];  ///< raylib key codes currently down
    unsigned charCount;
    int chars[kMaxChars];  ///< unicode code points typed this frame
};

constexpr std::size_t kPixelBytes =
    static_cast<std::size_t>(kViewWidth) * static_cast<std::size_t>(kViewHeight) * 4u;
constexpr std::size_t kPixelOffset = sizeof(FrameHeader);
constexpr std::size_t kInputOffset = kPixelOffset + kPixelBytes;
constexpr std::size_t kTotalBytes = kInputOffset + sizeof(InputBlock);

}  // namespace devshared
