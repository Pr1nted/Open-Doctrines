#pragma once

// What Discord says this person is doing.
//
// ── TWO LINES, AND THEY ARE READ BY STRANGERS ──
//
// Rich presence appears under somebody's name in every server they are in, to
// people who are not playing and never will. So it says what the game is and
// roughly what they are doing, and NOTHING that would not survive being read by
// a stranger: no save paths (which carry a real name), no invite codes, no
// account id, no session code.
//
// That is not a setting -- it is the shape of the data. The only inputs this
// takes are a screen, a scenario name and a country name, so there is nothing
// else it COULD leak.
//
// ── WHY THE TEXT IS DECIDED HERE ──
//
// The socket half is platform code with a byte protocol in it. The half that
// gets read by people is these strings, and getting them wrong is not a crash,
// it is somebody's Discord saying the wrong thing for an hour. So the wording
// is pure and pinned in tests/presence_test.cpp.

#include <string>

namespace presence {

/** Where the player is. Anything not listed reads as the menu. */
enum class Where {
    Menu = 0,
    Playing,      ///< in a world; `scenario` and `country` may be set
    MapEditor,
    Multiplayer,  ///< in a lobby, not yet playing
};

/** The two lines Discord shows, in the order it shows them. */
struct Activity {
    std::string details;   ///< the bold line: what they are doing
    std::string state;     ///< the line under it: the particulars

    /**
     * The picture beside the two lines.
     *
     * This is an ASSET KEY, not a file or a URL: the image is uploaded once to
     * the Discord application and referenced by the name it was given there.
     * A client cannot send Discord a picture, which is the whole reason rich
     * presence needs an application at all -- and is why game detection alone
     * shows Discord's grey placeholder however good the game's own icon is.
     *
     * Empty means no image, which is what happens until somebody uploads one.
     */
    std::string largeImage;
    /// The tooltip on that picture. Wasted if largeImage is empty.
    std::string largeText;
};

/**
 * The asset key to send, or empty for none.
 *
 * ── EMPTY IS THE RIGHT DEFAULT, AND IT IS NOT LAZINESS ──
 *
 * An activity that names NO asset makes Discord fall back to the application's
 * own icon -- the one uploaded on the General Information page, which every
 * application has because Discord asks for it while you are creating one. An
 * activity that names an asset key which has not been uploaded gets nothing at
 * all: checked against a real application, whose /assets endpoint answered `[]`
 * while its icon was set.
 *
 * So sending nothing shows the logo, and sending "logo" shows a blank until
 * somebody has been to a second page and uploaded the same picture again. The
 * default is therefore empty, and this exists for whoever wants a DIFFERENT
 * picture from the app icon.
 */
Activity withAsset(Activity a, const std::string& key, const std::string& tooltip);

/**
 * Build the activity.
 *
 * `scenario` is a map or save name and `country` the country being played;
 * either may be empty, and the wording changes rather than leaving a dangling
 * "playing " with nothing after it.
 *
 * A scenario name comes from a file somebody may have named anything, so it is
 * trimmed and capped here -- Discord truncates at 128 bytes and a name longer
 * than the line is a name nobody can read anyway.
 */
Activity describe(Where where, const std::string& scenario, const std::string& country);

/// Discord's own cap on each field.
inline constexpr size_t kMaxField = 128;

}  // namespace presence
