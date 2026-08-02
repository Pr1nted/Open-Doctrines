#include <string>
#include "Keybinds.h"

struct Color;

/**
 * The account service this build was compiled against, or "" if none.
 *
 * Defined in Config.cpp, which is where OD_ACCOUNT_ISSUER is visible -- and a
 * function rather than the macro itself so every reader gets the same value
 * whether or not its translation unit was built with the definition.
 */
const std::string& bakedAccountIssuer();

struct Config {
    float flySpeed = 2.0f;
    float maxZoom = 5.0f;
    int screenW = 1600;
    int screenH = 900;
    bool fullscreen = false;
    bool showActualFlags = true;
    /** The account terms and privacy policy have been accepted. */
    bool accountAgreed = false;
    bool debugMode = false;
    bool showFps = false;
    bool showZoom = false;
    bool showConsole = false;
    int fpsTarget = 0; // -1=Unlimited, 0=VSync, 10-120=capped

    // Share of this machine the game is allowed to work at, 0.10..1.00.
    //
    // Two levers, because the two things that actually burn CPU here are not
    // the same thing: it caps the frame rate (the render loop, which otherwise
    // runs flat out at the monitor's refresh or faster), and it duty-cycles the
    // turn loop (the simulation + AI, which is single-threaded and pegs one
    // core solid during self-play training). 1.0 means "no limit" and is the
    // default — nothing is throttled unless the player asks for it.
    float resourceBudget = 1.0f;
    // AI difficulty: 0=Easy 1=Normal 2=Hard 3=Insane. One shared model —
    // difficulty only changes how deterministically countries follow it.
    int aiDifficulty = 1;
    bool aiDebug = false;   // log AI decisions + enable the in-game AI overlay

    // Audio, 0..1. What reaches the device is master * category, so pulling
    // master to zero silences the game without disturbing the two settings
    // under it. Music sits below effects by default because it plays
    // continuously and the effects do not.
    float masterVolume = 0.8f;
    float musicVolume  = 0.6f;
    float sfxVolume    = 0.8f;

    // Announce each new track in a corner toast. On by default: the music picks
    // itself now, and without this there is no way to tell what was chosen or
    // to find a piece again by name.
    bool nowPlayingToast = true;

    // Make the map sound like a different place from the menus: the music drops
    // a few dB and picks up a small room reverb once you are looking at the
    // world. On by default — it is what stops the menu music from following you
    // onto the map at full level, sitting in front of the game.
    bool mapAtmosphere = true;

    // Online reinforcement learning during a normal session (Experimental tab).
    // Off by default: it costs time on every AI decision and writes
    // data/ai/model.bin, so a single play session can overwrite progress that a
    // long self-play training run has accumulated in the shared model.
    bool aiLearning = false;

    // Ask each mod's declared updateUrl whether a newer version exists.
    //
    // OFF BY DEFAULT, and it stays that way unless the player turns it on. This
    // is the only outbound request the game ever makes, and it is to a URL a
    // MOD AUTHOR controls -- so switching it on tells every such author that
    // this player runs their mod, roughly when, and from which IP. That is a
    // real disclosure and not one to make on someone's behalf.
    //
    // Even when on, the game only ever LOOKS. It never downloads or installs a
    // mod: the button it enables opens the author's page in a browser.
    bool modUpdateChecks = false;

    // Whether the game asks its own release host whether a newer OpenDoctrines
    // exists. ON by default, unlike modUpdateChecks above, and the difference
    // is deliberate: a mod check tells a stranger which mods this player runs,
    // while this one asks the game's own host about the game. Players who want
    // no outbound traffic at all can switch it off in Settings > Advanced.
    bool gameUpdateChecks = true;

    // Where the account service lives, e.g.
    // "https://opendoctrines-net.example.workers.dev".
    //
    // Defaults to whatever this build was compiled against -- empty for a
    // source build, so it offers no sign-in rather than guessing at a host,
    // because whoever built it may be running their own service and a
    // hardcoded fallback would quietly send their players' logins to ours.
    //
    // INITIALISED HERE, not only in load(). load() returns early when there is
    // no config.json, so a build that set an issuer still came up with an empty
    // one on any copy that has no such file -- which is EVERY web build, where
    // config.json is user data and is deliberately not in the preload, and every
    // fresh desktop install before its first save. The Account screen then said
    // no service was configured and told the player to edit a file they do not
    // have. A value in config.json still wins, so a player can point at another.
    std::string accountIssuer = bakedAccountIssuer();

    /**
     * Proves WHICH server this machine is, when hosting.
     *
     * Issued once by the account service and then kept. It is what makes the
     * per-player pseudonyms on this server stable, so registering again would
     * make every returning player look like a stranger to it -- which is why
     * this is stored rather than fetched each time.
     *
     * Not a credential for the account: it names a server, and the account
     * session token is required alongside it to open a session.
     */
    std::string serverCredential;

    int keybinds[ACTION_COUNT];

    int accentColor = 0xFFD700; // default gold, hex 0xRRGGBB

    Config() {
        for (int i = 0; i < ACTION_COUNT; ++i)
            keybinds[i] = DEFAULT_KEYBINDS[i];
    }

    bool load(const std::string& path);
    bool save(const std::string& path);
};
