#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "raylib.h"

/**
 * Where a moment in the game sits, emotionally.
 *
 * Every track ships with a point in this space (its sidecar .json) and the game
 * reports the point it is currently at (Game::currentMood). Choosing music is
 * then just "which track is nearest". Three axes is a deliberate ceiling: it is
 * few enough that a composer can place a piece by ear without a manual, and
 * enough to keep a menu, a quiet expansion and a losing war apart.
 */
struct Mood {
    float tension = 0.0f;   // 0 nothing at stake .. 1 under real threat
    float energy  = 0.35f;  // 0 still .. 1 driving
    float valence = 0.0f;   // -1 bleak .. 0 neutral .. +1 triumphant
};

/** What the toast shows. Empty title means nothing is playing. */
struct TrackInfo {
    std::string title;
    std::string author;
};

/**
 * Sound effects and music.
 *
 * Every path through this class degrades to silence rather than to a failure: a
 * machine with no output device, a build where data/audio/ was never created,
 * and a call naming a sound that does not exist all leave the game running
 * exactly as it did before audio existed. That is deliberate, and it is what
 * lets this layer ship ahead of the assets -- the game is playable and silent
 * with an empty folder, and gains sound the moment files appear, no code
 * change involved.
 *
 * Layout it reads:
 *
 *   data/audio/sfx/       <name>.{wav,ogg,mp3,qoa}  -- playSfx("<name>")
 *   data/audio/sfx/<any>/  scanned recursively; the folder is not part of
 *                          the name, so ui/hover.ogg is playSfx("hover")
 *   data/audio/sfx/<name>_1..N   alternate takes of one sound. A trailing
 *                          _<digits> is stripped, and playSfx picks between
 *                          them so a repeated event never machine-guns one
 *                          recording.
 *   data/audio/music/     any.{ogg,mp3,xm,mod,...}  -- scanned recursively
 *   data/audio/music/     <same stem>.json          -- that track's mood
 *
 * data/audio/midi/ is deliberately NOT read. It holds the editable sources of
 * the compositions for anyone who wants to remix them, and the game has no
 * synthesiser to play a .mid with.
 *
 * Format support is whatever raylib was built with; see config.h in the raylib
 * source. As shipped that is WAV/OGG/MP3/QOA plus the XM and MOD tracker
 * formats, and NOT FLAC.
 */
class Audio {
public:
    static Audio& get();

    /**
     * Keeps the audio device closed for the whole run when set before init().
     *
     * `--train-ai` sets it. Self-play runs thousands of turns with nobody
     * listening, and opening a device there costs startup time on exactly the
     * headless machines that are least likely to have one.
     */
    static bool s_disabled;

    /** Opens the device, loads the sounds and indexes the music. */
    void init(const std::string& dataDir);
    void shutdown();

    /**
     * Pump once per frame, from the one place that runs on every screen.
     *
     * Feeds the music stream, runs crossfades, picks the next track when one is
     * due, and on the web build releases the first-gesture gate.
     */
    void update(float dt);

    /**
     * Refills the music buffer and nothing else. Cheap, reentrant, and safe to
     * call as often as you like.
     *
     * This exists for the loading screen. World loading runs one heavy phase
     * per frame -- decoding an 8192x4096 map is seconds of blocking work -- and
     * for that whole time the main loop is not calling update(), so the stream
     * runs dry and the music stutters. Calling this from inside the long
     * operations keeps it fed. It does NOT advance the playlist or run fades:
     * those need a real frame delta, and a loading step is not one.
     *
     * On the web it additionally hands the thread back to the browser, because
     * there the audio callback runs on this same thread and a refilled buffer
     * that nobody drains still loops. Rate-limited internally, so it is safe to
     * call from inside a loop.
     */
    void pump();

    /**
     * Feeds the music from a helper thread across one blocking operation.
     *
     * Measured, the worst world-loading phases block the main thread for six to
     * nine seconds each. No buffer size covers that without introducing a decode
     * hitch during normal play, and pump() cannot help from inside a single
     * opaque call, so the refill has to leave the main thread for the duration.
     *
     * Safe because raylib's UpdateMusicStream holds AUDIO.System.lock -- the
     * same mutex the device callback takes -- for its entire body, so the mixer
     * and this thread cannot tear each other. The contract this class must keep
     * is narrower: while the pump runs, the main thread must not START, STOP or
     * UNLOAD a track, because that swaps the Music the thread is reading.
     * update() therefore does nothing while it is active, and every caller
     * brackets one blocking call so the pump cannot outlive it.
     *
     * Nests by count, so bracketing an inner call inside an outer one is safe.
     *
     * WEB: a no-op, and it has to be -- the build is single-threaded and
     * compiled without exceptions, so constructing a std::thread aborts the
     * tab. Callers whose blocking work can reach pump() from inside are covered
     * by that instead. Callers whose work CANNOT want beginBlockingCall().
     */
    void beginBackgroundPump();
    void endBackgroundPump();

    /**
     * Brackets one blocking call that cannot yield from inside at all.
     *
     * The difference from beginBackgroundPump() is entirely about the web. A
     * map generation or a synchronous editor load is a single opaque call
     * measured in tens of seconds: there is no loop of ours to instrument
     * short of rewriting the generator, so there is nowhere to put a pump().
     * On desktop the helper thread covers it, and before this existed the web
     * had nothing -- the bracket compiled to nothing at all and the browser
     * looped the last fragment of audio for the whole operation.
     *
     * So on the web this SUSPENDS the audio device for the duration. The music
     * does not continue; it stops and picks up afterwards where it left off.
     * That is a deliberate trade and the better half of it: a stalled page
     * cannot produce music, and the only choice is between silence and a
     * repeating fragment.
     *
     * Prefer pump() wherever the work has a loop to hang it on. Reach for this
     * when it does not.
     *
     * Nests by count, like the pump it wraps.
     */
    void beginBlockingCall();
    void endBlockingCall();

    /**
     * Scoped form of the pair above, and the one to reach for by default.
     *
     * A hand-written begin/end pair with a `return` between them leaves the
     * device suspended for the rest of the session -- not a glitch, silence
     * until the page is reloaded -- and the functions that need bracketing are
     * exactly the ones that already have early returns. A scope cannot make
     * that mistake.
     *
     * Nests, because the underlying pair does: an outer guard around a region
     * makes inner ones free, which is how a run of consecutive texture uploads
     * costs one suspend instead of one each.
     *
     * ONLY WORTH IT FOR REGIONS MEASURED IN SECONDS. Suspending stops the
     * music and restarts it, and that transition is itself audible -- so
     * against a stall of a few audio periods it is the worse of the two
     * artefacts. Two sites here were guarded at 130 ms and 200 ms and have
     * been reverted to a plain pump() beforehand, which tops the buffer up and
     * accepts a few repeated blocks. Rule of thumb: under about half a second,
     * pump; over it, guard.
     */
    class BlockingCall {
    public:
        BlockingCall();
        ~BlockingCall();
        BlockingCall(const BlockingCall&) = delete;
        BlockingCall& operator=(const BlockingCall&) = delete;
    };

    // ---- sound effects -----------------------------------------------------

    /**
     * Plays data/audio/sfx/<name>.*, if it exists.
     *
     * An unknown name is silent and costs one hash lookup, so callers never
     * need to know which files the player actually has. pitchJitter (0..1)
     * varies playback speed randomly by that fraction, which is what stops a
     * rapidly repeated click from sounding mechanical.
     */
    void playSfx(const std::string& name, float pitchJitter = 0.0f);

    // ---- music -------------------------------------------------------------

    /**
     * States what is happening; the music follows. Call every frame.
     *
     * `context` is the coarse filter -- "menu", "game", "editor" -- and a track
     * only plays in a context its sidecar allows. `mood` is the fine one, and
     * it decides WHICH of the allowed tracks comes next.
     *
     * Mood deliberately does not interrupt. A war breaking out mid-phrase does
     * not yank the track away; it changes what is chosen when the current one
     * ends. Only a context change the playing track does not allow crossfades
     * immediately, because that one really is a different place.
     */
    void playForContext(const std::string& context, const Mood& mood);

    /**
     * Whether the map should sound like a different place from the menus.
     *
     * On, the music drops a few dB and picks up a small room reverb whenever
     * the context is anything but "menu" -- so the map sits behind the game and
     * the menus stay dry and close. Both slide rather than switch, and both are
     * one setting because they are one idea: distance.
     */
    void setMapAtmosphere(bool enabled);

    /**
     * How strongly the map atmosphere applies right now, 0..1.
     *
     * The game drives this from the camera: fully zoomed out is 1 and sounds
     * like a room seen from far away; zoomed right in is 0 and sounds like the
     * menus. Anything that is not a map reports 0. Multiplied by the setting,
     * so switching that off is still an absolute mute for the effect.
     */
    void setAtmosphereIntensity(float intensity);

    void stopMusic(float fadeSeconds = 1.0f);

    /** Ends the current track early and picks the next one for the context. */
    void skipTrack();

    /**
     * True once each time a new track starts, handing back what it is.
     *
     * A one-shot flag rather than a callback: the toast is the only consumer,
     * it lives on Game, and polling keeps Audio from holding a pointer back
     * into a class it otherwise knows nothing about.
     */
    bool takeTrackChange(TrackInfo& out);

    /** What is playing now. Empty title when nothing is. */
    TrackInfo nowPlaying() const;

    /** How many tracks were indexed. Zero is a normal, silent configuration. */
    int trackCount() const { return (int)m_tracks.size(); }

    // ---- volume ------------------------------------------------------------
    //
    // All 0..1. What reaches the device is master * category, so muting master
    // mutes everything without disturbing the two category settings under it.

    void setMasterVolume(float v);
    void setMusicVolume(float v);
    void setSfxVolume(float v);

    float masterVolume() const { return m_master; }
    float musicVolume()  const { return m_music; }
    float sfxVolume()    const { return m_sfx; }

    /** False when the device never opened; every call above is then a no-op. */
    bool available() const { return m_available; }

private:
    Audio() = default;
    ~Audio() = default;
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    /**
     * One loaded sound plus its aliases.
     *
     * raylib restarts a Sound that is already playing rather than layering it,
     * so a second click during the first would cut the first off. The aliases
     * share the same sample data (LoadSoundAlias copies no audio) and are
     * handed out round-robin, which is what lets repeats overlap for the cost
     * of a few structs.
     */
    /** One recorded take. The aliases let that take overlap itself. */
    struct SfxTake {
        Sound base{};
        std::vector<Sound> aliases;
        int next = 0;
    };

    /** Every take filed under one name: `click_light_1..4` -> `click_light`. */
    struct SfxEntry {
        std::vector<SfxTake> takes;
        int last = -1;   // never the same take twice running, given a choice
    };

    /** An indexed music file and whatever its sidecar said about it. */
    struct TrackMeta {
        std::string path;
        std::string title;    // falls back to the filename
        std::string author;
        std::vector<std::string> contexts;  // empty = plays anywhere
        Mood mood;
        // Score valence by magnitude instead of by sign, so a track can ask for
        // "decisively one way or the other" -- a piece that suits both a war
        // being won and a war being lost catastrophically, but not the
        // undecided middle. Sidecar: "valenceMode": "magnitude".
        bool valenceMagnitude = false;
        float weight = 1.0f;  // >1 makes a track win ties more often
    };

    struct Track {
        Music music{};
        std::string name;
        bool loaded = false;
        float gain = 0.0f;   // 0..1 crossfade position, multiplied into volume
    };

    void loadSfxDir(const std::string& dir);
    void indexMusic(const std::string& dir);
    void readSidecar(const std::string& musicPath, TrackMeta& out) const;

    /** Index of the best track for ctx/mood, or -1 when none qualifies. */
    int pickTrack(const std::string& ctx, const Mood& mood, int avoidIdx) const;
    /** True when the track allows this context (no list = allows all). */
    static bool allowsContext(const TrackMeta& t, const std::string& ctx);

    bool startTrack(int trackIdx, bool crossfade);
    void unloadTrack(Track& t);
    void applyMusicVolume();
    /** True when the browser will accept playback; always true off the web. */
    bool gestureReady() const;

    bool m_available = false;
    std::string m_dataDir;

    std::unordered_map<std::string, SfxEntry> m_sfx_map;
    float m_master = 1.0f;
    float m_music  = 0.6f;
    float m_sfx    = 0.8f;

    bool  m_mapAtmosphere = true;
    float m_atmoIntensity = 0.0f;   // 0..1, set per frame by the game
    // Glides between 1.0 in the menus and MAP_DIM on the map. Folded into the
    // music volume rather than into the player's setting, so the slider keeps
    // showing what they chose.
    float m_sceneGain = 1.0f;

    std::vector<TrackMeta> m_tracks;

    // Current and outgoing track. Only ever two are resident: the one playing
    // and, during a crossfade, the one fading out.
    Track m_cur;
    Track m_prev;
    int m_curIdx  = -1;
    int m_prevIdx = -1;   // what to avoid repeating on the next pick

    std::string m_context;      // context last reported by the game
    Mood m_mood;                // mood last reported, consulted at the next pick
    bool m_needPick = false;    // a track is due; update() does it
    bool m_endQueued = false;   // this track's successor has already been asked for

    // Gain per second while a crossfade is in flight: m_cur.gain rises to 1 as
    // m_prev.gain falls to 0. Zero means neither track is moving.
    float m_fadeRate = 0.0f;

    bool m_trackChanged = false;  // one-shot for takeTrackChange
    bool m_gestureSeen = false;

    void backgroundPumpLoop();
    std::thread m_bgThread;
    std::atomic<bool> m_bgRunning{false};
    int m_bgDepth = 0;            // main-thread only; begin/end nesting count
    double m_lastPumpMs = 0.0;    // web: last time pump() yielded to the browser

    // web: how often pump() is allowed to yield, in ms. Starts at the base and
    // backs off on its own when the browser stops handing the thread back
    // promptly -- see the cost measurement in pump().
    static constexpr double PUMP_INTERVAL_MS = 30.0;
    double m_pumpIntervalMs = PUMP_INTERVAL_MS;

#ifdef __EMSCRIPTEN__
    // web: beginBlockingCall nesting, and the suspend/resume it drives
    int m_blockDepth = 0;
    void suspendDevice();
    void resumeDevice();
#endif
};
