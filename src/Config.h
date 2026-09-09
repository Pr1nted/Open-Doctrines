#pragma once
#include <string>
#include <vector>
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
    /// How much bigger the whole interface is drawn. 1.0 is the layout the
    /// game was written against; odUi applies it in one place and getMouse()
    /// divides the pointer back down, so no screen knows it exists.
    /// Composed with the platform's own minimum on Android rather than
    /// replacing it -- a phone that needs 1.2x to be legible still gets it.
    float uiScale = 1.0f;
    /// Which colour-blind palette to paint meaning in.
    /// 0 off, 1 deuteranopia, 2 protanopia, 3 tritanopia -- odPalette::Mode.
    /// See src/Palette.h and tools/check_palette.py.
    int colourBlindMode = 0;
    int screenW = 1600;
    int screenH = 900;
    bool fullscreen = false;
    bool showActualFlags = true;

    /**
     * Playing in front of an audience.
     *
     * Hides the things a stream should not carry -- invite codes, tunnel
     * addresses, the account id, the real name inside a file path -- and forces
     * censored flags regardless of showActualFlags above. See src/StreamSafe.h
     * for what actually leaks and why this is a camera setting rather than a
     * security one.
     */
    bool streamSafe = false;

    /**
     * Chat plays a country: which channel to read, and how long a vote runs.
     *
     * The channel is a public name, not a credential -- the reader is anonymous
     * and can only read. See src/stream/IrcParse.h.
     */
    /**
     * The Discord application id for rich presence. Empty means no presence.
     *
     * Empty by DEFAULT, like the account service: presence is published under
     * somebody's name to everybody they share a server with, and a game that
     * starts announcing that on first run has decided something on the player's
     * behalf. Made at discord.com/developers/applications.
     */
    std::string discordAppId;

    std::string streamChatChannel;
    float       streamChatSeconds = 30.0f;
    /**
     * Which language the interface is in: "en", "uk", "ja", ... See
     * src/i18n/Locale.h. Stored as the code rather than as an index, so a
     * language added in the middle of the list does not silently move
     * everybody who had chosen the one after it.
     */
    std::string language = "en";
    /** The account terms and privacy policy have been accepted. */
    bool accountAgreed = false;
    /**
     * Skip the between-turns phase that shows what everybody did.
     *
     * Remembered between sessions because it is a preference about pacing, not
     * a per-game choice -- a player who does not want the beat never wants it.
     * Greater Diplomacy carries the same setting ("Skip Viewing AI Moves?").
     */
    bool skipViewingOrders = false;
    /**
     * Render at the display's real pixel density.
     *
     * Off, the framebuffer is the window's LOGICAL size and the system stretches
     * it to the panel -- so on a 2x display every pixel of the map, the
     * interface and the text is drawn once and shown as four. That is what
     * "blurry on a big display" is, and it is not a font problem.
     *
     * On, raylib asks for a framebuffer at the panel's own resolution. On macOS
     * that is coordinate-transparent: the ortho projection stays in logical
     * units and only the viewport grows (rcore.c SetupViewport has an __APPLE__
     * branch for exactly this), so nothing in the game has to be laid out
     * differently. Kept as a setting because the machine this was written on
     * reports a scale of 1.00 and cannot exercise the path.
     */
    bool highDpi = true;
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

    // A small credit in the corner of an exported timelapse GIF.
    //
    // On by default, because the export is the most shareable thing the game
    // produces and it used to travel with nothing on it saying what it was.
    // Off is this one line, and it is honoured everywhere the GIF is written,
    // including --export-timelapse.
    bool timelapseWatermark = true;

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

    // The Greater Diplomacy 5 translation layer (Experimental tab).
    //
    // Off by default. It converts a world into a second game's map format, and
    // the conversion is lossy in ways that depend on the map -- so it is opt-in
    // and every use of it warns before it writes anything. Only meaningful in a
    // build made with -DOD_ENABLE_GDTL=ON; without that the option is still
    // stored but the feature reports itself unavailable.
    bool gdtl = false;

    // Where Greater Diplomacy 5 lives, if the player has told us.
    //
    // Empty by default and NEVER filled in by guessing: the game does not look
    // for other software on the disk unless a player asks it to, from the
    // translate dialog, and what the search finds is shown before it is saved.
    std::string gd5Path;

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
     * Where in-game bug reports, suggestions and ratings are sent.
     *
     * Defaults to the account service, because a fork that has one has the
     * relay too -- they are the same Worker. Empty disables reporting outright
     * and the menu entry says so rather than failing on send: a fork with no
     * service of its own should not be sending its players' bug reports to
     * somebody else's channel.
     */
    std::string feedbackEndpoint = bakedAccountIssuer();

    /**
     * A random, meaningless id for this installation, minted on first use.
     *
     * It exists so the service can cap reports per copy of the game without
     * knowing who anybody is: not derived from hardware, not tied to an
     * account, and worth nothing to anyone who reads it. See Feedback.h.
     */
    std::string installId;

    /// Reports sent today, and the day they were counted for (YYYY-MM-DD).
    int         feedbackSentToday = 0;
    std::string feedbackCountedOn;

    /**
     * Rating prompt state.
     *
     * `ratingAsked` is set the first time the prompt is shown and never
     * cleared: a player who ignored it once has answered. `ratingGiven` is set
     * when they rate, and either one stops the prompt forever.
     */
    bool        ratingAsked = false;
    bool        ratingGiven = false;
    /// Minutes of play, accumulated across sessions. The prompt waits for some.
    int         minutesPlayed = 0;

    // ── Mail ──
    //
    // `mailPolicy` is the HOST's setting and travels with the lobby; the copy
    // here is what a single-player game uses and what a host's own client seeds
    // the lobby from. See mail::Policy.
    //   0 nobody   1 players only   2 advisors only   3 everyone
    int         mailPolicy = 3;
    /// This player's own door. See mail::Lock: 0 open, 1 advisors only, 2 shut.
    /// A host can narrow who may write; nothing a host permits overrides this.
    int         mailLock = 0;
    /// Words the host will not carry. Blunt on purpose -- see mail::blacklisted.
    std::vector<std::string> mailBlacklist;

    // ── The age prompt ──
    //
    // WHAT THIS IS: a local, self-declared question, asked once, stored here,
    // and never transmitted anywhere. WHAT IT IS NOT: a verified age check.
    // Self-declaration is not "highly effective age assurance" under the UK
    // Online Safety Act, so nothing in the game claims that it is -- the prompt
    // says what it does and the terms say the same. A host who needs a real
    // check needs a real provider, and that is a decision with legal advice
    // behind it rather than a checkbox here.
    //
    // Off by default: a prompt that appears for everybody, everywhere, in a
    // game that mostly has no chat in it at all, would be theatre.
    bool        agePromptOn = false;
    /// Set once the player has answered. 0 = unanswered, 1 = said adult,
    /// 2 = said under age, which locks mail to nobody.
    int         ageAnswer = 0;

    // ── The language-model module ──
    //
    // Off unless a player turns it on. `llmEndpoint` is a chat-completions base
    // URL: a runner on this machine, or a remote API. `llmApiKey` is only ever
    // sent to a NON-local endpoint the player typed themselves -- see
    // Game_Llm.cpp, which refuses to attach it otherwise.
    /**
     * Whether the lobby chat panel is shown to THIS player.
     *
     * Local and personal: it hides a window and never stops anybody else
     * talking. Whether chat exists in a game at all is the HOST's switch, and
     * lives in LobbySettings on the host.
     */
    bool        lobbyChatShown = true;

    bool        llmEnabled = false;
    /**
     * EMPTY UNTIL SET, and that is load-bearing rather than tidy.
     *
     * These shipped as "http://127.0.0.1:8080/v1" and "local-model", which are
     * not placeholders -- they are stored values, and every piece of code that
     * asks "has the player configured this yet?" asks whether they are empty.
     * So: the endpoint was never auto-filled after an install because it was
     * never empty; the "pull a model" step was skipped because a model name was
     * already present; and llmConfigured() returned true, so the game believed
     * it had a working advisor while pointing at a port nothing listens on --
     * 8080 is llama.cpp's, and the runner this game installs serves 11434.
     *
     * The field hints in the mail settings show what to type. A hint belongs in
     * the placeholder, where it cannot be mistaken for an answer.
     */
    std::string llmEndpoint;
    std::string llmModel;
    std::string llmApiKey;

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

    /**
     * A mod's accent colour, or -1 for none. NEVER WRITTEN TO DISK.
     *
     * The accent is read at over a hundred sites -- every heading, highlight and
     * selection in the game -- which makes it the cheapest full reskin lever
     * there is, and exactly why a mod must not be able to keep it. Writing
     * accentColor directly would have been enough: save() is called whenever the
     * player touches any setting, so the mod's colour would land in the config
     * file and outlive uninstalling the mod, with no way back but the reset
     * button. A separate field that save() ignores gives a mod the whole
     * interface while it runs and gives it back the moment it stops.
     *
     * Read through accent(). accentColor itself remains the player's own choice
     * and is what the settings screen writes.
     */
    int accentOverride = -1;
    int accent() const { return accentOverride >= 0 ? accentOverride : accentColor; }

    Config() {
        for (int i = 0; i < ACTION_COUNT; ++i)
            keybinds[i] = DEFAULT_KEYBINDS[i];
    }

    bool load(const std::string& path);
    bool save(const std::string& path);
};
