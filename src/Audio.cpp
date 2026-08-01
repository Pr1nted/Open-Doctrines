#include "Audio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

#include "json.hpp"

bool Audio::s_disabled = false;

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace {

#ifdef __EMSCRIPTEN__
// ---- streamed music (web only) ------------------------------------------
//
// The .ogg files are excluded from the preload -- 52 MB of a package the
// player waits on before the menu draws, for something no menu needs. They sit
// next to the page instead and are fetched the first time a track is chosen.
//
// ONE AT A TIME, deliberately. The only caller is the track picker, which
// wants exactly one track and then waits for it; fetching several in parallel
// would spend bandwidth on tracks the mood may never ask for.
//
// The state is file-static rather than a member because emscripten_async_wget
// takes plain C callbacks. There is one Audio.
std::string g_fetching;      // VFS path in flight; empty when idle

// Tracks whose bytes did not arrive. "Not retried" was the intent, but nothing
// wrote it down: the picker asks for a track every time it wants music, and a
// failure only cleared g_fetching, so the very next frame asked again. With the
// music directory missing that is one request per frame per track, forever --
// hundreds of identical failures in the console and a server being hammered for
// files it does not have. Remembering the failures is what makes "not retried"
// true.
std::unordered_set<std::string> g_unavailable;

void onMusicFetched(const char*) { g_fetching.clear(); }

void onMusicFailed(const char* file) {
    // Not fatal: a track whose bytes will not arrive is one track. Clearing the
    // slot lets the picker choose a different one, and it will, because
    // startTrack failing leaves m_needPick set.
    std::cerr << "  Music download failed: " << (file ? file : "?") << std::endl;
    if (!g_fetching.empty()) g_unavailable.insert(g_fetching);
    g_fetching.clear();
}
#endif

/** True while a track's audio is on its way. Always false off the web. */
bool musicFetchInFlight() {
#ifdef __EMSCRIPTEN__
    return !g_fetching.empty();
#else
    return false;
#endif
}

/**
 * Make sure `path` is readable, starting a download if it is not.
 *
 * Returns true when the file is there NOW. On web a first call for a missing
 * track returns false having started the fetch; the caller is expected to ask
 * again. Off the web this only ever answers the question it is asked.
 */
bool musicEnsureLocal(const std::string& path) {
    if (FileExists(path.c_str())) return true;
#ifdef __EMSCRIPTEN__
    if (g_unavailable.count(path)) return false;   // asked once, answered no
    if (!g_fetching.empty()) return false;         // one at a time
    // The copy next to the page mirrors the VFS layout exactly, so the URL is
    // the VFS path without its leading slash. See the POST_BUILD copy in
    // CMakeLists.txt -- if that directory was not deployed, this 404s and the
    // failure callback says which file.
    const std::string url = path.empty() || path[0] != '/' ? path : path.substr(1);
    g_fetching = path;
    emscripten_async_wget(url.c_str(), path.c_str(), onMusicFetched, onMusicFailed);
#endif
    return false;
}

// raylib restarts rather than layers a Sound that is already playing. Three
// aliases share the same sample data and cost only a struct each.
constexpr int SFX_ALIASES = 3;

// Extensions raylib is built with here. FLAC is deliberately absent: raylib's
// config.h leaves SUPPORT_FILEFORMAT_FLAC commented out, so a .flac dropped in
// would load as silence and look like a bug in this file.
const char* SFX_EXTS   = ".wav;.ogg;.mp3;.qoa";
const char* MUSIC_EXTS = ".ogg;.mp3;.qoa;.wav;.xm;.mod";

constexpr float CROSSFADE_SECONDS = 1.5f;

// Frames per stream subbuffer, against raylib's default of sampleRate/30 --
// about 33ms each, two of them, so ~67ms of runway.
//
// 67ms is not enough to ride out an ordinary heavy frame, let alone a spike.
// This is ~186ms each, ~370ms in total, which absorbs the frame hitches of
// normal play. It is deliberately NOT sized for world loading: covering a
// nine-second stall would mean decoding nine seconds of MP3 in one call, and
// that hitch would then land periodically during gameplay. Loading is the
// background pump's job instead.
//
// No latency cost that matters -- raudio applies volume when it mixes, not when
// it decodes, so crossfades stay responsive at any buffer size. Sound effects
// are unaffected: a Sound is buffered to its own length, not to this default.
constexpr unsigned int MUSIC_BUFFER_FRAMES = 8192;

// Relative pull of each mood axis when scoring a track. Tension leads because
// it is the axis a player feels immediately -- the wrong energy is a slightly
// odd choice, but peace music over a collapsing front is just wrong.
constexpr float W_TENSION = 1.4f;
constexpr float W_ENERGY  = 0.8f;
constexpr float W_VALENCE = 1.0f;

// Added to the track that just played, to break out of looping one piece when
// something else fits nearly as well.
//
// Deliberately comparable to a real mood difference rather than huge. An
// overwhelming penalty makes a two-track library alternate strictly, whatever
// the mood says -- the second track would win every other pick purely for not
// being the first. At this size a clearly better match still repeats, which is
// what keeps the main theme on the menu and the calm one over the map editor,
// and near-ties alternate, which is what stops either from wearing out.
constexpr float REPEAT_PENALTY = 0.15f;

float randUnit() { return (float)rand() / (float)RAND_MAX; }

// ── Map atmosphere ──────────────────────────────────────────────────────────
//
// On the map the music should sit further away than it does in the menus:
// quieter, in a room, and with the distance an echo implies. Three effects, one
// idea, one number.
//
// HEADROOM -- read before changing any constant below.
//
// All three run BEFORE the buffer volume is applied, and they add on top of the
// dry signal rather than replacing it. Measured against sustained full-scale
// input, dry + reverb + echo reaches 1.582x. What keeps that from clipping is
// that the dim is driven by the SAME intensity, so the worst case at intensity
// i is:
//
//     peak(i) = (1 + 0.582 i) * (1 - 0.58 i)
//
// which is highest at i = 0, where it is exactly 1.0, and falls to 0.664 at
// i = 1. It never exceeds 1.0 anywhere on the curve. That is only true because
// the wet levels and the dim move together -- deepening either wet level, or
// weakening MAP_DIM, without re-deriving this will clip when the player zooms
// out. There is a check for it in the turn notes; re-run it if you touch these.

// Level the music drops to at FULL atmosphere, i.e. fully zoomed out. About
// -7.5 dB: the whole world on screen should sound genuinely distant, not just a
// little quieter. Zoomed right in the music comes back to menu level, so this
// depth is only ever reached looking at everything at once.
constexpr float MAP_DIM = 0.42f;
// Seconds to slide between menu and map levels. Long enough not to read as a
// volume change, short enough to have finished by the time the map is drawn.
constexpr float SCENE_GLIDE_SECONDS = 0.8f;

// A Schroeder/Freeverb small room: four parallel combs into two allpasses.
//
// The comb lengths are the classic Freeverb primes at 44.1 kHz, kept mutually
// prime so their resonant peaks do not stack up into an audible ringing pitch.
// Feedback below 1 makes the network unconditionally stable.
//
// The processor runs on miniaudio's device thread and raylib gives it no user
// pointer, so the state has to live here at namespace scope. Only the audio
// thread touches it; the game thread communicates through one atomic.
constexpr int   RV_COMBS = 4;
constexpr int   RV_APS   = 2;
constexpr int   RV_COMB_LEN[RV_COMBS] = { 1557, 1617, 1491, 1422 };
constexpr int   RV_AP_LEN[RV_APS]     = { 225, 556 };
constexpr int   RV_SPREAD     = 23;     // right channel runs longer than left
constexpr float RV_FEEDBACK   = 0.70f;  // room size
constexpr float RV_DAMP       = 0.35f;  // high-frequency absorption
constexpr float RV_INPUT_GAIN = 0.025f;
// Wet level at full atmosphere. Measured against an impulse this network
// settles in 0.35 s, so even at this depth it reads as a room rather than as an
// effect.
constexpr float RV_WET        = 0.85f;
// Per-sample glide of the wet level, so toggling the setting cannot click.
constexpr float RV_GLIDE      = 0.0004f;

// A discrete echo, on top of the reverb and doing a different job: the reverb
// is a room, this is distance. Slightly different delay on each side so the
// repeats open outwards rather than sitting in the middle of the head.
constexpr int   EC_LEN_L    = 11907;   // ~270 ms at 44.1 kHz
constexpr int   EC_LEN_R    = 14553;   // ~330 ms
constexpr float EC_FEEDBACK = 0.33f;   // three or four audible repeats
constexpr float EC_MIX      = 0.20f;   // level at full atmosphere

struct EcDelay {
    std::vector<float> buf;
    int idx = 0;
    float process(float in) {
        const float out = buf[idx];
        buf[idx] = in + out * EC_FEEDBACK;
        if (++idx >= (int)buf.size()) idx = 0;
        return out;
    }
    void init(int n) { buf.assign(n, 0.0f); }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); idx = 0; }
};

EcDelay g_ecL, g_ecR;

struct RvComb {
    std::vector<float> buf;
    int idx = 0;
    float store = 0.0f;
    float process(float in) {
        const float out = buf[idx];
        store = out * (1.0f - RV_DAMP) + store * RV_DAMP;
        buf[idx] = in + store * RV_FEEDBACK;
        if (++idx >= (int)buf.size()) idx = 0;
        return out;
    }
};

struct RvAllpass {
    std::vector<float> buf;
    int idx = 0;
    float process(float in) {
        const float bufout = buf[idx];
        buf[idx] = in + bufout * 0.5f;
        if (++idx >= (int)buf.size()) idx = 0;
        return -in + bufout;
    }
};

struct RvChannel {
    RvComb comb[RV_COMBS];
    RvAllpass ap[RV_APS];
    void init(int spread) {
        for (int i = 0; i < RV_COMBS; ++i) comb[i].buf.assign(RV_COMB_LEN[i] + spread, 0.0f);
        for (int i = 0; i < RV_APS; ++i)   ap[i].buf.assign(RV_AP_LEN[i] + spread, 0.0f);
    }
    void clear() {
        for (auto& c : comb) { std::fill(c.buf.begin(), c.buf.end(), 0.0f); c.idx = 0; c.store = 0.0f; }
        for (auto& a : ap)   { std::fill(a.buf.begin(), a.buf.end(), 0.0f); a.idx = 0; }
    }
    float process(float in) {
        const float x = in * RV_INPUT_GAIN;
        float acc = 0.0f;
        for (auto& c : comb) acc += c.process(x);
        for (auto& a : ap)   acc = a.process(acc);
        return acc;
    }
};

RvChannel g_rvL, g_rvR;
std::atomic<float> g_rvTarget{0.0f};   // written by the game thread only
float g_rvWet = 0.0f;                  // audio thread only
bool  g_rvSilent = true;               // delay lines known to be empty

/**
 * Stream processor: adds the wet tail on top of the dry signal.
 *
 * raylib hands this float32 interleaved stereo in the device mixing format, in
 * chunks of at most 512 frames, BEFORE the buffer's own volume is applied --
 * so the reverb is scaled by the crossfade along with everything else and
 * cannot survive a fade-out.
 */
void musicReverbProcessor(void* buffer, unsigned int frames) {
    float* s = static_cast<float*>(buffer);
    const float target = g_rvTarget.load(std::memory_order_relaxed);

    if (target <= 0.0f && g_rvWet <= 0.0001f) {
        // Fully bypassed. Empty every line once on the way out, otherwise the
        // tail frozen in them would burst back out when the atmosphere is next
        // switched on, seconds or minutes later.
        if (!g_rvSilent) {
            g_rvL.clear(); g_rvR.clear();
            g_ecL.clear(); g_ecR.clear();
            g_rvWet = 0.0f; g_rvSilent = true;
        }
        return;
    }
    g_rvSilent = false;

    // One glide drives both effects: they are two halves of the same "how far
    // away is this" and must not be able to drift apart, least of all because
    // the headroom budget is computed against their sum.
    for (unsigned int i = 0; i < frames; ++i) {
        g_rvWet += (target - g_rvWet) * RV_GLIDE;
        const float amount = g_rvWet / RV_WET;   // back to 0..1
        const float ec = EC_MIX * amount;
        const float l = s[i * 2], r = s[i * 2 + 1];
        s[i * 2]     = l + g_rvL.process(l) * g_rvWet + g_ecL.process(l) * ec;
        s[i * 2 + 1] = r + g_rvR.process(r) * g_rvWet + g_ecR.process(r) * ec;
    }
}

}  // namespace

Audio& Audio::get() {
    static Audio inst;
    return inst;
}

// ────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ────────────────────────────────────────────────────────────────────────────

void Audio::init(const std::string& dataDir) {
    if (s_disabled) return;
    m_dataDir = dataDir;

    // Before any stream is created: this only sets the default new streams take.
    SetAudioStreamBufferSizeDefault(MUSIC_BUFFER_FRAMES);

    InitAudioDevice();
    if (!IsAudioDeviceReady()) {
        // Not an error worth interrupting anyone over. A machine with no output
        // device is a machine that plays this game silently.
        std::cout << "  Audio device unavailable — running silent" << std::endl;
        return;
    }
    m_available = true;

    // Allocated once here, never from the audio thread.
    g_rvL.init(0);
    g_rvR.init(RV_SPREAD);
    g_ecL.init(EC_LEN_L);
    g_ecR.init(EC_LEN_R);

    loadSfxDir(m_dataDir + "audio/sfx");
    indexMusic(m_dataDir + "audio/music");
    std::cout << "  Audio ready (" << m_sfx_map.size() << " sound"
              << (m_sfx_map.size() == 1 ? "" : "s") << ", "
              << m_tracks.size() << " track"
              << (m_tracks.size() == 1 ? "" : "s") << ")" << std::endl;
}

void Audio::shutdown() {
    if (!m_available) return;
    // Before anything is unloaded: an unjoined thread reading a freed Music is
    // the one way this design can crash.
    m_bgDepth = 1;
    endBackgroundPump();
    unloadTrack(m_cur);
    unloadTrack(m_prev);
    for (auto& [name, e] : m_sfx_map) {
        for (SfxTake& t : e.takes) {
            for (Sound& a : t.aliases) UnloadSoundAlias(a);
            UnloadSound(t.base);
        }
    }
    m_sfx_map.clear();
    m_tracks.clear();
    CloseAudioDevice();
    m_available = false;
}

// ────────────────────────────────────────────────────────────────────────────
// Loading and indexing
// ────────────────────────────────────────────────────────────────────────────

void Audio::loadSfxDir(const std::string& dir) {
    if (!DirectoryExists(dir.c_str())) return;

    // Recursive: the sets arrive grouped as ui/, artillery/, construction/, and
    // keeping that shape is worth more than one flat folder of a hundred files.
    // The subfolder is not part of the name -- only the stem is.
    FilePathList files = LoadDirectoryFilesEx(dir.c_str(), SFX_EXTS, true);
    for (unsigned int i = 0; i < files.count; ++i) {
        const char* path = files.paths[i];

        // GetFileNameWithoutExt returns a pointer into a static buffer that the
        // next call overwrites, so this has to become a std::string here.
        std::string name = GetFileNameWithoutExt(path);

        // "heavy_artillery_3" -> "heavy_artillery". A trailing _<digits> marks
        // an alternate take of the same sound rather than a sound of its own.
        const size_t us = name.find_last_of('_');
        if (us != std::string::npos && us + 1 < name.size() &&
            name.find_first_not_of("0123456789", us + 1) == std::string::npos) {
            name.erase(us);
        }

        Sound s = LoadSound(path);
        if (s.frameCount == 0) {
            std::cerr << "  Sound failed to load: " << path << std::endl;
            continue;
        }
        SfxTake take;
        take.base = s;
        take.aliases.reserve(SFX_ALIASES);
        for (int a = 0; a < SFX_ALIASES; ++a) take.aliases.push_back(LoadSoundAlias(s));
        m_sfx_map[name].takes.push_back(std::move(take));
    }
    UnloadDirectoryFiles(files);
}

void Audio::indexMusic(const std::string& dir) {
    if (!DirectoryExists(dir.c_str())) return;

    // Recursive: subfolders under music/ are organisation, not meaning. What a
    // track is FOR comes from its sidecar, so someone can file the same piece
    // under music/themes/ or music/ and get identical behaviour.
    //
    // On web the audio is NOT here to be found: it is excluded from the
    // preload and fetched on demand (see musicEnsureLocal). So the index is
    // built from the sidecars, which are preloaded, and each track's path is
    // derived from its sidecar's. The index is therefore complete before any
    // audio has been downloaded, which is what lets the mood system choose
    // between all 35 tracks on the first frame rather than between none.
#ifdef __EMSCRIPTEN__
    FilePathList files = LoadDirectoryFilesEx(dir.c_str(), ".json", true);
    for (unsigned int i = 0; i < files.count; ++i) {
        const std::string sidecar = files.paths[i];
        TrackMeta t;
        // "<stem>.json" -> "<stem>.ogg". Every shipped track is .ogg; a
        // sidecar with no audio beside it simply never loads, and says so
        // once, rather than being silently dropped from the index here.
        t.path  = sidecar.substr(0, sidecar.find_last_of('.')) + ".ogg";
        t.title = GetFileNameWithoutExt(sidecar.c_str());
        readSidecar(t.path, t);
        m_tracks.push_back(std::move(t));
    }
#else
    FilePathList files = LoadDirectoryFilesEx(dir.c_str(), MUSIC_EXTS, true);
    for (unsigned int i = 0; i < files.count; ++i) {
        TrackMeta t;
        t.path = files.paths[i];
        t.title = GetFileNameWithoutExt(files.paths[i]);
        readSidecar(t.path, t);
        m_tracks.push_back(std::move(t));
    }
#endif
    UnloadDirectoryFiles(files);

    // LoadDirectoryFilesEx does not promise an order, and an unordered index
    // would make the same folder score ties differently per platform.
    std::sort(m_tracks.begin(), m_tracks.end(),
              [](const TrackMeta& a, const TrackMeta& b) { return a.path < b.path; });
}

void Audio::readSidecar(const std::string& musicPath, TrackMeta& out) const {
    // Same stem, .json extension: "Parade Uniform.mp3" -> "Parade Uniform.json".
    const std::string stem = musicPath.substr(0, musicPath.find_last_of('.'));
    const std::string path = stem + ".json";
    std::ifstream f(path);
    if (!f) return;   // no sidecar is fine: neutral mood, plays anywhere

    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("title"))  out.title  = j["title"].get<std::string>();
        if (j.contains("author")) out.author = j["author"].get<std::string>();
        if (j.contains("weight")) out.weight = std::max(0.01f, j["weight"].get<float>());
        if (j.contains("valenceMode"))
            out.valenceMagnitude = (j["valenceMode"].get<std::string>() == "magnitude");
        if (j.contains("contexts") && j["contexts"].is_array())
            for (const auto& c : j["contexts"]) out.contexts.push_back(c.get<std::string>());
        if (j.contains("mood") && j["mood"].is_object()) {
            const auto& m = j["mood"];
            if (m.contains("tension")) out.mood.tension = std::clamp(m["tension"].get<float>(), 0.0f, 1.0f);
            if (m.contains("energy"))  out.mood.energy  = std::clamp(m["energy"].get<float>(),  0.0f, 1.0f);
            if (m.contains("valence")) out.mood.valence = std::clamp(m["valence"].get<float>(), -1.0f, 1.0f);
        }
    } catch (const std::exception& e) {
        // A malformed sidecar must not cost us the track. It plays with neutral
        // mood, and the reason is on the console rather than silently guessed.
        std::cerr << "  Bad track sidecar " << path << ": " << e.what() << std::endl;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Sound effects
// ────────────────────────────────────────────────────────────────────────────

void Audio::playSfx(const std::string& name, float pitchJitter) {
    if (!m_available || !gestureReady()) return;
    auto it = m_sfx_map.find(name);
    if (it == m_sfx_map.end()) return;

    SfxEntry& e = it->second;
    if (e.takes.empty()) return;

    // A different take from the last one whenever there is a choice. Hearing
    // the identical waveform twice running is the thing that makes a sound
    // read as canned, and these arrive four takes deep for exactly that reason.
    int idx = 0;
    if (e.takes.size() > 1) {
        idx = std::min((int)(randUnit() * (float)e.takes.size()),
                       (int)e.takes.size() - 1);
        if (idx == e.last) idx = (idx + 1) % (int)e.takes.size();
    }
    e.last = idx;

    SfxTake& t = e.takes[(size_t)idx];
    Sound& s = t.aliases.empty() ? t.base : t.aliases[t.next];
    if (!t.aliases.empty()) t.next = (t.next + 1) % (int)t.aliases.size();

    SetSoundVolume(s, m_master * m_sfx);
    if (pitchJitter > 0.0f) {
        SetSoundPitch(s, 1.0f + (randUnit() * 2.0f - 1.0f) * pitchJitter);
    }
    PlaySound(s);
}

// ────────────────────────────────────────────────────────────────────────────
// Choosing music
// ────────────────────────────────────────────────────────────────────────────

bool Audio::allowsContext(const TrackMeta& t, const std::string& ctx) {
    if (t.contexts.empty()) return true;   // unlabelled tracks go anywhere
    return std::find(t.contexts.begin(), t.contexts.end(), ctx) != t.contexts.end();
}

int Audio::pickTrack(const std::string& ctx, const Mood& mood, int avoidIdx) const {
    int best = -1;
    float bestScore = 0.0f;

    for (int i = 0; i < (int)m_tracks.size(); ++i) {
        const TrackMeta& t = m_tracks[i];
        if (!allowsContext(t, ctx)) continue;

        // A magnitude track is asking "how decisive is this?", not "which way".
        // Comparing against |valence| is what lets one piece cover a war being
        // won and a war being lost, which no single signed point can.
        const float refV = t.valenceMagnitude ? std::fabs(mood.valence) : mood.valence;
        const float dt = t.mood.tension - mood.tension;
        const float de = t.mood.energy  - mood.energy;
        const float dv = t.mood.valence - refV;
        float score = W_TENSION * dt * dt + W_ENERGY * de * de + W_VALENCE * dv * dv;

        // Weight makes a track win ties more often without letting it override
        // a genuinely better match.
        score /= t.weight;

        // Enough jitter to keep two near-identical tracks from locking into a
        // fixed order, not enough to beat a clearly closer match.
        score += randUnit() * 0.05f;

        if (i == avoidIdx) score += REPEAT_PENALTY;

        if (best < 0 || score < bestScore) { best = i; bestScore = score; }
    }
    return best;
}

void Audio::playForContext(const std::string& context, const Mood& mood) {
    if (!m_available) return;

    m_mood = mood;   // consulted at the next boundary, not now

    // Called every frame, so the unchanged case returns before anything else.
    // It also means a context whose pick found nothing stays quiet instead of
    // re-scanning the index once a frame forever: the retry only comes from a
    // context change or a track ending, both of which are real events.
    if (context == m_context) return;
    m_context = context;

    // Only a context the playing track does not cover forces a switch.
    // Otherwise the track keeps going and the new context takes effect when it
    // ends -- crossfading on every hop between menus would be worse than both.
    const bool curOk = m_cur.loaded && m_curIdx >= 0 && m_curIdx < (int)m_tracks.size() &&
                       allowsContext(m_tracks[m_curIdx], m_context);
    if (!curOk) m_needPick = true;
}

void Audio::skipTrack() {
    if (!m_available) return;
    m_needPick = true;
}

void Audio::stopMusic(float fadeSeconds) {
    if (!m_available) return;
    m_needPick = false;
    if (!m_cur.loaded) return;

    unloadTrack(m_prev);
    m_prev = m_cur;
    m_prevIdx = m_curIdx;
    m_cur = Track{};
    m_curIdx = -1;
    m_fadeRate = (fadeSeconds > 0.01f) ? (1.0f / fadeSeconds) : 1000.0f;
}

bool Audio::startTrack(int trackIdx, bool crossfade) {
    if (trackIdx < 0 || trackIdx >= (int)m_tracks.size()) return false;
    const TrackMeta& meta = m_tracks[trackIdx];
    const int wasIdx = m_curIdx;

    // On web the first ask for a track only starts its download; the caller
    // retries once it lands. Nothing below can run on bytes that are not here
    // yet, and LoadMusicStream on a missing file would report it as a corrupt
    // track rather than an absent one.
    if (!musicEnsureLocal(meta.path)) return false;

    Track next;
    next.music = LoadMusicStream(meta.path.c_str());
    if (next.music.frameCount == 0) {
        std::cerr << "  Music failed to load: " << meta.path << std::endl;
        return false;
    }
    next.name = meta.title;
    next.loaded = true;

    // Playlist advance is ours, so raylib must not silently restart the track
    // at the end -- with looping on the end is never reached and a single track
    // would hold the slot forever regardless of what the mood did.
    next.music.looping = false;

    if (crossfade && m_cur.loaded) {
        unloadTrack(m_prev);
        m_prev = m_cur;
        m_prevIdx = m_curIdx;
    } else {
        unloadTrack(m_cur);
    }

    // Always from silence, even with nothing to cross from. Music arriving at
    // full level is a jolt whether or not something else was playing -- the
    // first track after startup used to snap straight in at 100%.
    next.gain = 0.0f;
    m_fadeRate = 1.0f / CROSSFADE_SECONDS;

    m_cur = next;
    m_curIdx = trackIdx;
    m_endQueued = false;
    // Every start, including a track looping back to itself. Suppressing
    // repeats was wrong: a context that holds one piece -- the main menu, which
    // the theme wins outright -- then never announced anything at all.
    (void)wasIdx;
    m_trackChanged = true;
    applyMusicVolume();
    PlayMusicStream(m_cur.music);
    // Attached for the life of the stream; when the map atmosphere is off the
    // processor sees a zero wet level and returns immediately.
    AttachAudioStreamProcessor(m_cur.music.stream, musicReverbProcessor);
    return true;
}

bool Audio::takeTrackChange(TrackInfo& out) {
    if (!m_trackChanged) return false;
    m_trackChanged = false;
    out = nowPlaying();
    return !out.title.empty();
}

TrackInfo Audio::nowPlaying() const {
    if (!m_cur.loaded || m_curIdx < 0 || m_curIdx >= (int)m_tracks.size()) return {};
    return { m_tracks[m_curIdx].title, m_tracks[m_curIdx].author };
}

void Audio::unloadTrack(Track& t) {
    if (!t.loaded) return;
    // Detached first: the processor list lives on the buffer that is about to
    // be freed, and the device thread walks it.
    DetachAudioStreamProcessor(t.music.stream, musicReverbProcessor);
    StopMusicStream(t.music);
    UnloadMusicStream(t.music);
    t = Track{};
}

void Audio::applyMusicVolume() {
    const float base = m_master * m_music * m_sceneGain;
    if (m_cur.loaded)  SetMusicVolume(m_cur.music,  base * m_cur.gain);
    if (m_prev.loaded) SetMusicVolume(m_prev.music, base * m_prev.gain);
}

void Audio::setMapAtmosphere(bool enabled) {
    m_mapAtmosphere = enabled;
}

void Audio::setAtmosphereIntensity(float intensity) {
    m_atmoIntensity = std::clamp(intensity, 0.0f, 1.0f);
}

// ────────────────────────────────────────────────────────────────────────────
// Per-frame
// ────────────────────────────────────────────────────────────────────────────

bool Audio::gestureReady() const {
#ifdef __EMSCRIPTEN__
    return m_gestureSeen;
#else
    return true;
#endif
}

void Audio::update(float dt) {
    if (!m_available) return;
    // The helper thread is reading m_cur/m_prev. Picking a track here would
    // swap them underneath it; see beginBackgroundPump.
    if (m_bgRunning.load(std::memory_order_acquire)) return;

#ifdef __EMSCRIPTEN__
    if (!m_gestureSeen) {
        // Deliberately NOT GetKeyPressed(): that pops raylib's key queue, which
        // the keybind screen and every text field read from. These checks do
        // not consume anything.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) ||
            IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ||
            IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
            IsKeyPressed(KEY_ESCAPE)) {
            m_gestureSeen = true;
        }
    }
#endif

    // Held rather than dropped while the browser has not been touched yet, so
    // the first click starts the music that was already due.
    // A fetch in flight IS the pick, still happening -- so do not pick again
    // over the top of it. Without this the picker would choose a second track
    // every frame while the first downloads, and musicEnsureLocal would refuse
    // each one for being busy.
    if (m_needPick && gestureReady() && !musicFetchInFlight()) {
        // Cleared before the attempt, not after: a context with no matching
        // track must not retry once per frame forever.
        m_needPick = false;
        const int idx = pickTrack(m_context, m_mood, m_curIdx);
        // On web a first attempt only starts the download and reports failure.
        // That is not "no track for this context", it is "not yet", so the ask
        // is put back. Off the web musicFetchInFlight() is always false and
        // this is the same "stay quiet" behaviour as before.
        if (idx >= 0 && !startTrack(idx, /*crossfade=*/true) && musicFetchInFlight())
            m_needPick = true;
    }

    if (m_fadeRate > 0.0f) {
        bool moving = false;
        if (m_cur.loaded && m_cur.gain < 1.0f) {
            m_cur.gain = std::min(1.0f, m_cur.gain + m_fadeRate * dt);
            moving = true;
        }
        if (m_prev.loaded) {
            m_prev.gain = std::max(0.0f, m_prev.gain - m_fadeRate * dt);
            if (m_prev.gain <= 0.0f) unloadTrack(m_prev);
            else moving = true;
        }
        applyMusicVolume();
        if (!moving) m_fadeRate = 0.0f;
    }

    // Menus are dry and close; the map recedes into a room, and how far it
    // recedes follows the camera. Both ends glide so crossing between them is
    // never heard as a step.
    {
        const float amount = m_mapAtmosphere ? m_atmoIntensity : 0.0f;
        g_rvTarget.store(RV_WET * amount, std::memory_order_relaxed);

        // Interpolating the dim alongside the wet is what keeps the headroom
        // invariant true at every intermediate point, not just at the ends.
        const float target = 1.0f - (1.0f - MAP_DIM) * amount;
        if (m_sceneGain != target) {
            const float step = dt / SCENE_GLIDE_SECONDS;
            m_sceneGain += std::clamp(target - m_sceneGain, -step, step);
            if (std::fabs(target - m_sceneGain) < 0.001f) m_sceneGain = target;
            applyMusicVolume();
        }
    }

    pump();
    if (!m_cur.loaded) return;

    // Ask for the successor a crossfade BEFORE the end, not at it. Asking at the
    // end meant the outgoing track was already silent by the time the fade
    // started, so there was no fade-out to hear -- just a stop and a fade-in.
    // Leading by the crossfade length makes the two genuinely overlap.
    if (!m_endQueued) {
        const float len = GetMusicTimeLength(m_cur.music);
        if (len > 0.0f) {
            // A short piece must not spend a quarter of itself fading out.
            const float lead = std::min(CROSSFADE_SECONDS, len * 0.25f);
            if (GetMusicTimePlayed(m_cur.music) >= len - lead) {
                m_endQueued = true;   // once per track, whatever the pick returns
                m_needPick = true;
            }
        }
    }
}

void Audio::pump() {
    if (!m_available) return;
    // The helper thread already owns the refill; doing it from here too would
    // just contend on the audio lock for no benefit.
    if (m_bgRunning.load(std::memory_order_acquire)) return;
    if (m_prev.loaded) UpdateMusicStream(m_prev.music);
    if (m_cur.loaded)  UpdateMusicStream(m_cur.music);

#ifdef __EMSCRIPTEN__
    // REFILLING THE BUFFER IS NOT ENOUGH IN A BROWSER.
    //
    // The web build has no threads, so miniaudio's callback -- the one that
    // DRAINS the buffer and hands samples to Web Audio -- runs on this thread,
    // the same one a loading phase blocks for seconds at a time. Feeding a
    // buffer nobody is emptying changes nothing: the browser replays whatever
    // it last received, and that is the fragment of a track that loops over the
    // whole loading screen.
    //
    // The callback needs the thread back, and ASYNCIFY is what can give it:
    // this unwinds to the browser, lets it run its event loop -- audio
    // included -- and resumes on the next line. It is safe here specifically
    // because Game::run() is an ordinary blocking loop rather than an
    // emscripten_set_main_loop callback, so this returns where it left off
    // instead of re-entering the frame.
    //
    // Desktop keeps the helper thread and never reaches this.
    //
    // Rate-limited so this is safe to call from inside a loop: unwinding and
    // rewinding the stack is not free, and one turn per frame's worth of time
    // is all the audio callback needs. Without the limit, adding this call to a
    // per-province loop would cost more than the loading it is protecting.
    double nowMs = emscripten_get_now();
    if (nowMs - m_lastPumpMs < 16.0) return;
    m_lastPumpMs = nowMs;
    emscripten_sleep(0);
#endif
}

void Audio::backgroundPumpLoop() {
    while (m_bgRunning.load(std::memory_order_acquire)) {
        if (m_prev.loaded) UpdateMusicStream(m_prev.music);
        if (m_cur.loaded)  UpdateMusicStream(m_cur.music);
        // Comfortably inside one subbuffer, so a refill is never late, and idle
        // enough that the thread costs nothing against a multi-second load.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void Audio::beginBackgroundPump() {
    if (!m_available) return;
#ifdef __EMSCRIPTEN__
    // No pump in a browser, and no way to have one. The web build is
    // single-threaded and compiled without exceptions, so constructing a
    // std::thread here does not fail -- it aborts the tab. This is the call
    // that killed the game every time a player opened a world: the loader
    // starts it to keep music fed across a long blocking load, so it fired on
    // the way into every single game.
    //
    // Nothing is lost by skipping it. The pump exists because a load blocks the
    // thread that would otherwise refill the music buffer; on web the loader is
    // stepped from the frame loop and asyncify yields to the browser, so the
    // buffer is refilled by the ordinary Audio::update() path anyway.
    return;
#else
    if (m_bgDepth++ > 0) return;              // already pumping
    if (!m_cur.loaded && !m_prev.loaded) return;   // nothing to feed
    m_bgRunning.store(true, std::memory_order_release);
    m_bgThread = std::thread(&Audio::backgroundPumpLoop, this);
#endif
}

void Audio::endBackgroundPump() {
    if (!m_available) return;
#ifdef __EMSCRIPTEN__
    return;   // begin was a no-op there, so m_bgDepth was never raised
#endif
    if (m_bgDepth == 0) return;
    if (--m_bgDepth > 0) return;
    m_bgRunning.store(false, std::memory_order_release);
    // Joined, not detached: the caller is about to be free to swap tracks, and
    // that must not happen while the thread still holds a pointer to this one.
    if (m_bgThread.joinable()) m_bgThread.join();
}

// ────────────────────────────────────────────────────────────────────────────
// Volume
// ────────────────────────────────────────────────────────────────────────────

void Audio::setMasterVolume(float v) {
    m_master = std::clamp(v, 0.0f, 1.0f);
    applyMusicVolume();
}

void Audio::setMusicVolume(float v) {
    m_music = std::clamp(v, 0.0f, 1.0f);
    applyMusicVolume();
}

void Audio::setSfxVolume(float v) {
    // Applied per PlaySound rather than here: sounds are one-shots, so a change
    // reaches the next one anyway and there is nothing sustained to correct.
    m_sfx = std::clamp(v, 0.0f, 1.0f);
}
