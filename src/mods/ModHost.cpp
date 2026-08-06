#include <algorithm>
#include "ModHost.h"
#include "ModRuntime.h"

#include <cstdio>
#include <cstring>
#include <vector>

ModHostContext g_modHost;
ModUiBridge g_uiBridge;
ModGameAccess* g_modGame = nullptr;

namespace {

std::vector<ModLogLine> g_log;
constexpr size_t kMaxLogLines = 512;

// WAMR hands every native its execution environment as the first argument. The
// type is opaque here so this file does not depend on the backend headers;
// modCurrentInstance() does the translation.
using ExecEnv = void*;

ModInstance* self(ExecEnv e) { return modCurrentInstance(e); }

void pushLog(const std::string& id, int level, std::string text) {
    if (text.size() > 2048) text.resize(2048);
    if (g_log.size() >= kMaxLogLines) g_log.erase(g_log.begin());
    static const char* kNames[] = {"TRACE", "INFO", "WARN", "ERROR"};
    const char* lname = kNames[(level < 0 || level > 3) ? 1 : level];
    printf("[MOD %s] %s: %s\n", id.c_str(), lname, text.c_str());
    g_log.push_back({id, std::move(text), level});
}

// ------------------------------------------------------------------ Core --

void core_log(ExecEnv e, int32_t level, uint32_t msgPtr, uint32_t msgLen) {
    ModInstance* mi = self(e);
    if (!mi) return;
    std::string s;
    if (!mi->readString(msgPtr, msgLen, s)) {
        pushLog(mi->id(), 3, "gearbox_log called with an out-of-bounds string");
        return;
    }
    pushLog(mi->id(), level, std::move(s));
}

// Mirrors gearbox_env_t in sdk/gearbox.h. Layout is part of the ABI: fields are
// appended, never reordered, and the mod's own `size` field caps how much we
// write so an older mod stays safe against a newer host.
struct GearboxEnv {
    uint32_t size;
    uint32_t gearbox_major;
    uint32_t gearbox_minor;
    uint32_t host_version;
    uint8_t  platform;
    uint8_t  is_web;
    uint8_t  is_headless;
    uint8_t  net_role;
    uint32_t screen_w;
    uint32_t screen_h;
};
// 7 x uint32 worth: the four uint8 fields pack into one word. wasm32 uses the
// same 4-byte alignment, so the mod sees this exact layout.
static_assert(sizeof(GearboxEnv) == 28, "gearbox_env_t layout is ABI");

// Prefixed because raylib's web build defines PLATFORM_WEB as a macro, and an
// enumerator by that name will not compile there.
enum : uint8_t {
    kPlatformUnknown = 0, kPlatformWindows = 1, kPlatformMacos = 2,
    kPlatformLinux = 3, kPlatformWeb = 4
};

void core_env(ExecEnv e, uint32_t outPtr) {
    ModInstance* mi = self(e);
    if (!mi) return;

    // The mod writes its own sizeof() into `size` first. Read it, then write
    // back at most that many bytes: a mod built against an older, smaller
    // struct must not have its neighbouring memory overwritten by a newer host.
    uint32_t declared = 0;
    if (!mi->memRead(outPtr, sizeof(uint32_t), &declared)) return;
    if (declared == 0 || declared > sizeof(GearboxEnv))
        declared = sizeof(GearboxEnv);

    GearboxEnv env{};
    env.size = declared;
    env.gearbox_major = 1;
    env.gearbox_minor = 1;
    env.host_version = (1u << 16) | (0u << 8) | 6u;   // project version 1.0.6
#if defined(__EMSCRIPTEN__)
    env.platform = kPlatformWeb;
    env.is_web = 1;
#elif defined(_WIN32)
    env.platform = kPlatformWindows;
#elif defined(__APPLE__)
    env.platform = kPlatformMacos;
#elif defined(__linux__)
    env.platform = kPlatformLinux;
#else
    env.platform = kPlatformUnknown;
#endif
    env.is_headless = g_modHost.headless ? 1 : 0;
    // Was `reserved0`, always zero. Standalone is 0, so a mod built against
    // the older struct reads the same byte and gets the right answer for the
    // only game it could have been running: singleplayer.
    env.net_role = static_cast<uint8_t>(g_modHost.netRole);
    env.screen_w = g_modHost.headless ? 0 : g_modHost.screenW;
    env.screen_h = g_modHost.headless ? 0 : g_modHost.screenH;

    mi->memWrite(outPtr, declared, &env);
}

void core_abort(ExecEnv e, uint32_t msgPtr, uint32_t msgLen) {
    ModInstance* mi = self(e);
    if (!mi) return;
    std::string s;
    if (!mi->readString(msgPtr, msgLen, s)) s = "(unreadable abort message)";
    pushLog(mi->id(), 3, "aborted: " + s);
    mi->setAbort(s);
}

uint64_t core_fuel_budget(ExecEnv e) {
    ModInstance* mi = self(e);
    return mi ? mi->fuelRemaining() : UINT64_MAX;
}

// -------------------------------------------------------------------- UI --

// Panel handles are host-assigned and validated on every call. A mod passing
// another mod's handle is ignored: ownership is checked, not assumed.
ModPanel* ownedPanel(ModInstance* mi, uint32_t panel) {
    if (!mi || !mi->has(MODULE_UI)) return nullptr;
    ModPanel* p = ModUI::get().find(panel);
    if (!p || p->ownerId != mi->id()) return nullptr;
    if (p->cmds.size() >= ModUI::kMaxCmdsPerPanel) return nullptr;
    return p;
}

uint32_t ui_panel_register(ExecEnv e, uint32_t titlePtr, uint32_t titleLen,
                           uint32_t minW, uint32_t minH) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_UI)) return 0;
    // Headless is not an error -- the mod keeps running, it just has no panel.
    if (g_modHost.headless) return 0;

    std::string title;
    if (!mi->readString(titlePtr, titleLen, title)) title = mi->manifest().name;
    if (title.size() > 64) title.resize(64);
    return ModUI::get().registerPanel(mi->id(), title, minW, minH);
}

void ui_draw_rect(ExecEnv e, uint32_t panel, int32_t x, int32_t y,
                  int32_t w, int32_t h, uint32_t rgba) {
    ModPanel* p = ownedPanel(self(e), panel);
    if (!p) return;
    ModDrawCmd c;
    c.kind = ModDrawCmd::Rect;
    c.x = x; c.y = y; c.w = w; c.h = h; c.rgba = rgba;
    p->cmds.push_back(std::move(c));
}

void ui_draw_text(ExecEnv e, uint32_t panel, int32_t x, int32_t y,
                  uint32_t rgba, uint32_t textPtr, uint32_t textLen) {
    ModInstance* mi = self(e);
    ModPanel* p = ownedPanel(mi, panel);
    if (!p) return;
    std::string t;
    if (!mi->readString(textPtr, textLen, t)) return;
    if (t.size() > 512) t.resize(512);
    ModDrawCmd c;
    c.kind = ModDrawCmd::Text;
    c.x = x; c.y = y; c.rgba = rgba; c.text = std::move(t);
    p->cmds.push_back(std::move(c));
}

uint32_t ui_button(ExecEnv e, uint32_t panel, int32_t x, int32_t y,
                   int32_t w, int32_t h, uint32_t labelPtr, uint32_t labelLen) {
    ModInstance* mi = self(e);
    ModPanel* p = ownedPanel(mi, panel);
    if (!p) return 0;
    std::string label;
    if (!mi->readString(labelPtr, labelLen, label)) label.clear();
    if (label.size() > 64) label.resize(64);

    bool hovered = p->mouseInside && w > 0 && h > 0 &&
                   p->mouseX >= (float)x && p->mouseX < (float)(x + w) &&
                   p->mouseY >= (float)y && p->mouseY < (float)(y + h);

    ModDrawCmd c;
    c.kind = ModDrawCmd::Button;
    c.x = x; c.y = y; c.w = w; c.h = h;
    c.hovered = hovered;
    c.text = std::move(label);
    p->cmds.push_back(std::move(c));

    // One click activates one button: consume it so overlapping rects declared
    // by a careless mod cannot all fire from a single press.
    if (hovered && p->clickPending) {
        p->clickPending = false;
        return 1;
    }
    return 0;
}

// ------------------------------------------------------- GameState.Read --
//
// Every getter tolerates g_modGame being null (no world loaded) and returns a
// neutral value rather than trapping: a mod panel is reachable from the main
// menu, and making that a crash would be a trap for modders.

constexpr uint32_t kInvalid = 0xFFFFFFFFu;

uint32_t gs_turn_number(ExecEnv e) {
    (void)e;
    return g_modGame ? g_modGame->turnNumber() : 0;
}

uint32_t gs_country_count(ExecEnv e) {
    (void)e;
    return g_modGame ? g_modGame->countryCount() : 0;
}

uint32_t gs_country_at(ExecEnv e, uint32_t index) {
    (void)e;
    return g_modGame ? g_modGame->countryAt(index) : kInvalid;
}

// Two-call sizing: returns the full byte length and writes at most `cap`. A
// return greater than cap means truncation, not failure.
uint32_t gs_country_name(ExecEnv e, uint32_t cid, uint32_t bufPtr, uint32_t cap) {
    ModInstance* mi = self(e);
    if (!mi || !g_modGame || !g_modGame->countryExists(cid)) return 0;
    std::string n = g_modGame->countryName(cid);
    if (cap > 0 && bufPtr != 0) {
        uint32_t n2 = (uint32_t)n.size() < cap ? (uint32_t)n.size() : cap;
        mi->memWrite(bufPtr, n2, n.data());
    }
    return (uint32_t)n.size();
}

double gs_country_treasury(ExecEnv e, uint32_t cid) {
    (void)e;
    if (!g_modGame || !g_modGame->countryExists(cid)) return 0.0;
    return g_modGame->countryTreasury(cid);
}

uint32_t gs_country_province_count(ExecEnv e, uint32_t cid) {
    (void)e;
    if (!g_modGame || !g_modGame->countryExists(cid)) return 0;
    return g_modGame->countryProvinceCount(cid);
}

int64_t gs_province_population(ExecEnv e, uint32_t pid) {
    (void)e;
    return g_modGame ? (int64_t)g_modGame->provincePopulation(pid) : 0;
}

uint32_t gs_province_owner(ExecEnv e, uint32_t pid) {
    (void)e;
    return g_modGame ? g_modGame->provinceOwner(pid) : kInvalid;
}

// ---------------------------------------------------------------- Assets --
//
// Reads come straight out of the mod's own archive, which is never unpacked to
// disk. There is no path to another mod's data and no filesystem to escape to:
// the name is looked up in this package's entry list, not resolved as a path.

uint32_t assets_size(ExecEnv e, uint32_t namePtr, uint32_t nameLen) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_ASSETS) || !mi->package()) return 0;
    std::string name;
    if (!mi->readString(namePtr, nameLen, name)) return 0;
    std::vector<uint8_t> data;
    if (!mi->package()->readAsset(name, data)) return 0;
    return (uint32_t)data.size();
}

uint32_t assets_read(ExecEnv e, uint32_t namePtr, uint32_t nameLen,
                     uint32_t bufPtr, uint32_t cap) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_ASSETS) || !mi->package()) return 0;
    std::string name;
    if (!mi->readString(namePtr, nameLen, name)) return 0;
    std::vector<uint8_t> data;
    if (!mi->package()->readAsset(name, data)) return 0;
    if (cap > 0 && bufPtr != 0) {
        uint32_t n = (uint32_t)data.size() < cap ? (uint32_t)data.size() : cap;
        mi->memWrite(bufPtr, n, data.data());
    }
    return (uint32_t)data.size();
}

// ----------------------------------------------------------------- Audio --
//
// A mod plays its OWN sounds. The bytes come from its package, through the
// same reader `assets` uses, so a path outside the mod is not resolved -- it
// simply is not there. Volume is scaled by the player's own effects setting,
// which is what stops a mod being louder than they allowed.

ModAudioBridge g_audioBridge;

uint32_t audio_play(ExecEnv e, uint32_t pathPtr, uint32_t pathLen, float volume) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_AUDIO) || !mi->package() || !g_audioBridge.play)
        return 0;
    std::string path;
    if (!mi->readString(pathPtr, pathLen, path)) return 0;

    // Read through the package, so a path outside the mod is not resolved --
    // it simply is not there.
    std::vector<uint8_t> bytes;
    if (!mi->package()->readAsset(path, bytes) || bytes.empty()) return 0;

    // The extension picks the decoder, so it has to come from the name.
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return 0;

    return g_audioBridge.play(mi->id(), path.substr(dot), bytes,
                              volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume));
}

void audio_stop(ExecEnv e, uint32_t handle) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_AUDIO) || !g_audioBridge.stop) return;
    // The mod id goes with it: one mod must not be able to stop another's.
    g_audioBridge.stop(mi->id(), handle);
}

void audio_set_volume(ExecEnv e, uint32_t handle, float volume) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_AUDIO) || !g_audioBridge.setVolume) return;
    g_audioBridge.setVolume(mi->id(), handle,
                            volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume));
}

uint32_t audio_is_playing(ExecEnv e, uint32_t handle) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_AUDIO) || !g_audioBridge.isPlaying) return 0;
    return g_audioBridge.isPlaying(mi->id(), handle) ? 1 : 0;
}

// ------------------------------------------------------------------- Net --
//
// A channel between copies of ONE mod. Not access to game traffic: orders,
// deltas, tickets and chat never pass through here, because a client mod that
// could touch those could forge another player's turn -- which is precisely
// what the server being authoritative exists to prevent.
//
// The game installs the bridge below when a session exists. Until it does,
// every call answers "this is not a network game", which is a documented
// answer rather than a failure -- and is exactly right in singleplayer.



ModNetBridge g_netBridge;

uint32_t net_send(ExecEnv e, int32_t peer, uint32_t dataPtr, uint32_t dataLen) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NET) || !g_netBridge.send) return 0;
    if (dataLen > 8192) return 0;
    std::vector<uint8_t> buf(dataLen);
    if (dataLen && !mi->memRead(dataPtr, dataLen, buf.data())) return 0;
    // The mod id is stamped HERE, by the host. A mod cannot send as another.
    return g_netBridge.send(mi->id(), peer, buf) ? 1 : 0;
}

uint32_t net_recv(ExecEnv e, uint32_t outPtr, uint32_t outLen, uint32_t fromPtr) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NET) || !g_netBridge.recv) return 0;
    std::vector<uint8_t> msg;
    int32_t from = 0;
    if (!g_netBridge.recv(mi->id(), msg, from)) return 0;
    if (outLen && outPtr) {
        const uint32_t n = (uint32_t)msg.size() < outLen ? (uint32_t)msg.size() : outLen;
        mi->memWrite(outPtr, n, msg.data());
    }
    if (fromPtr) mi->memWrite(fromPtr, 4, &from);
    return (uint32_t)msg.size();
}

uint32_t net_peer_count(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NET) || !g_netBridge.peerCount) return 0;
    return g_netBridge.peerCount();
}

uint32_t net_self_peer(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NET) || !g_netBridge.selfPeer) return 0;
    return g_netBridge.selfPeer();
}

uint32_t net_is_host(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NET) || !g_netBridge.isHost) return 0;
    return g_netBridge.isHost() ? 1 : 0;
}

// -------------------------------------------------------------- WasiStub --
//
// Languages that cannot compile to wasm -- Python, Ruby, Lua, Java -- ship an
// interpreter inside the mod, and every prebuilt interpreter targets WASI. This
// is the narrowest surface that lets one boot.
//
// It is NOT a WASI implementation and must never grow into one. Two decisions
// keep it from becoming the hole the sandbox exists to prevent:
//
//   * random_get returns a DETERMINISTIC stream, not OS entropy. Real entropy
//     is a machine fingerprint, and it would also make self-play and save
//     replay non-reproducible. The stream is seeded from the mod id, so two
//     mods differ from each other but a given mod is identical run to run.
//   * clock_time_get returns a counter derived from the turn number, not the
//     wall clock. A mod learning the date and timezone is a fingerprint; a mod
//     needing "has time passed" gets a monotonic answer.
//
// Everything touching the filesystem is refused outright. fd_write goes to the
// log so a print() lands somewhere the user can see, and nothing else.

enum : uint32_t {                 // WASI errno subset
    WASI_ESUCCESS = 0, WASI_EBADF = 8, WASI_EINVAL = 28,
    WASI_ENOSYS = 52, WASI_ENOTCAPABLE = 76
};

// Deterministic per-instance stream. xorshift64*, seeded from the mod id.
uint64_t wasiSeedFor(ModInstance* mi) {
    uint64_t h = 1469598103934665603ull;          // FNV-1a
    for (char c : mi->id()) { h ^= (uint8_t)c; h *= 1099511628211ull; }
    return h ? h : 0x9E3779B97F4A7C15ull;
}

uint32_t wasi_fd_write(ExecEnv e, uint32_t fd, uint32_t iovs, uint32_t iovsLen,
                       uint32_t nwrittenPtr) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_WASISTUB)) return WASI_ENOTCAPABLE;
    if (fd != 1 && fd != 2) return WASI_EBADF;    // stdout/stderr only
    if (iovsLen > 64) iovsLen = 64;

    std::string out;
    uint32_t total = 0;
    for (uint32_t i = 0; i < iovsLen; i++) {
        uint32_t iov[2] = {0, 0};                 // { buf, len }
        if (!mi->memRead(iovs + i * 8, 8, iov)) break;
        if (iov[1] == 0) continue;
        if (iov[1] > 8192) iov[1] = 8192;
        std::string chunk;
        if (!mi->readString(iov[0], iov[1], chunk)) break;
        out += chunk;
        total += iov[1];
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (!out.empty()) pushLog(mi->id(), fd == 2 ? 2 : 1, std::move(out));
    mi->memWrite(nwrittenPtr, 4, &total);
    return WASI_ESUCCESS;
}

void wasi_proc_exit(ExecEnv e, uint32_t code) {
    ModInstance* mi = self(e);
    if (!mi) return;
    // A mod calling exit() is done. Trap so it unwinds rather than returning
    // into a runtime that believes the process is gone.
    mi->setAbort("the mod called exit(" + std::to_string(code) + ")");
}

uint32_t wasi_random_get(ExecEnv e, uint32_t buf, uint32_t len) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_WASISTUB)) return WASI_ENOTCAPABLE;
    if (len > 4096) return WASI_EINVAL;
    uint64_t x = wasiSeedFor(mi) + mi->wasiRandomCounter();
    std::vector<uint8_t> bytes(len);
    for (uint32_t i = 0; i < len; i++) {
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
        bytes[i] = (uint8_t)((x * 2685821657736338717ull) >> 56);
    }
    mi->bumpWasiRandom(len);
    if (len && !mi->memWrite(buf, len, bytes.data())) return WASI_EINVAL;
    return WASI_ESUCCESS;
}

uint32_t wasi_clock_time_get(ExecEnv e, uint32_t clockId, uint64_t precision,
                             uint32_t timePtr) {
    (void)clockId; (void)precision;
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_WASISTUB)) return WASI_ENOTCAPABLE;
    // Turn number as nanoseconds-since-an-arbitrary-epoch. Monotonic, coarse,
    // and carries no information about the machine or the real date.
    uint64_t ns = (uint64_t)(g_modGame ? g_modGame->turnNumber() : 0) * 1000000000ull;
    if (!mi->memWrite(timePtr, 8, &ns)) return WASI_EINVAL;
    return WASI_ESUCCESS;
}

// No arguments, no environment. Both report zero entries rather than failing,
// because a runtime that cannot read argv usually aborts during startup.
uint32_t wasi_zero_sizes(ExecEnv e, uint32_t countPtr, uint32_t bufSizePtr) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_WASISTUB)) return WASI_ENOTCAPABLE;
    uint32_t zero = 0;
    mi->memWrite(countPtr, 4, &zero);
    mi->memWrite(bufSizePtr, 4, &zero);
    return WASI_ESUCCESS;
}
uint32_t wasi_zero_get(ExecEnv e, uint32_t a, uint32_t b) {
    (void)a; (void)b;
    ModInstance* mi = self(e);
    return (mi && mi->has(MODULE_WASISTUB)) ? WASI_ESUCCESS : WASI_ENOTCAPABLE;
}

// The filesystem is refused, not stubbed. ENOTCAPABLE is what WASI itself
// returns for a capability the sandbox withheld, so runtimes handle it.
uint32_t wasi_denied2(ExecEnv e, uint32_t a, uint32_t b) {
    (void)e; (void)a; (void)b; return WASI_ENOTCAPABLE;
}
uint32_t wasi_denied1(ExecEnv e, uint32_t a) { (void)e; (void)a; return WASI_EBADF; }

// The preopen scan is a special case, and getting it wrong kills the mod before
// its first line runs.
//
// wasi-libc starts at fd 3 and calls fd_prestat_get repeatedly to discover
// preopened directories. It treats EBADF as "that is all of them" and stops --
// but ANY other error is fatal, and it responds by calling _Exit(EX_OSERR).
// EX_OSERR is 71, which is exactly the "the mod called exit(71)" that CPython
// and Ruby both died with while this returned ENOTCAPABLE. The interpreter was
// never reached; libc had already given up.
//
// EBADF is also the honest answer. ENOTCAPABLE means "you were denied a
// capability"; here there is genuinely no such descriptor to deny, because a mod
// is given no preopened directory at all. Saying so ends the scan cleanly and
// leaves the sandbox exactly as closed as before.
uint32_t wasi_prestat_none2(ExecEnv e, uint32_t a, uint32_t b) {
    (void)e; (void)a; (void)b; return WASI_EBADF;
}

// stdout and stderr genuinely exist -- fd_write routes them to the mod log --
// so describing them is not a concession, it is consistency. Refusing here
// while accepting writes is what made CPython fail with "can't initialize sys
// standard streams": it asks what fd 0/1/2 are before wrapping them.
//
// They are reported as character devices with no seek and no filesystem rights,
// which is exactly what they are. Every other descriptor is still EBADF,
// because there is genuinely no such thing.
uint32_t wasi_fd_fdstat_get(ExecEnv e, uint32_t fd, uint32_t out) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_WASISTUB)) return WASI_ENOTCAPABLE;
    if (fd > 2) return WASI_EBADF;

    // __wasi_fdstat_t: filetype u8 @0, fs_flags u16 @2,
    //                  rights_base u64 @8, rights_inheriting u64 @16.
    uint8_t buf[24] = {0};
    buf[0] = 2;                                  // CHARACTER_DEVICE
    const uint64_t kRightRead  = 1ull << 1;      // FD_READ
    const uint64_t kRightWrite = 1ull << 6;      // FD_WRITE
    // fd 0 is not readable either -- there is no stdin -- but claiming the read
    // right and then returning EBADF from fd_read is the shape libc expects for
    // an empty stream, and is what keeps CPython from refusing to start.
    uint64_t rights = (fd == 0) ? kRightRead : kRightWrite;
    memcpy(buf + 8, &rights, 8);                 // rights_base
    // rights_inheriting stays 0: nothing may be derived from these.

    if (!mi->memWrite(out, sizeof buf, buf)) return WASI_EINVAL;
    return WASI_ESUCCESS;
}

// Same three descriptors, same reasoning. CPython builds sys.stdout by handing
// the fd to io.FileIO, which calls fstat on it; refusing that is the second
// half of "can't initialize sys standard streams".
//
// Everything reported here is deliberately contentless: zero device, zero
// inode, zero size, zero timestamps. A real timestamp would be a machine
// fingerprint, which is the same reason wasi_clock_time_get reports the turn
// number rather than the wall clock.
uint32_t wasi_fd_filestat_get_std(ExecEnv e, uint32_t fd, uint32_t out) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_WASISTUB)) return WASI_ENOTCAPABLE;
    if (fd > 2) return WASI_EBADF;

    // __wasi_filestat_t: dev @0, ino @8, filetype u8 @16, nlink @24,
    //                    size @32, atim @40, mtim @48, ctim @56.
    uint8_t buf[64] = {0};
    buf[16] = 2;                                 // CHARACTER_DEVICE
    uint64_t nlink = 1;
    memcpy(buf + 24, &nlink, 8);

    if (!mi->memWrite(out, sizeof buf, buf)) return WASI_EINVAL;
    return WASI_ESUCCESS;
}
uint32_t wasi_prestat_none3(ExecEnv e, uint32_t a, uint32_t b, uint32_t c) {
    (void)e; (void)a; (void)b; (void)c; return WASI_EBADF;
}
uint32_t wasi_denied3(ExecEnv e, uint32_t a, uint32_t b, uint32_t c) {
    (void)e; (void)a; (void)b; (void)c; return WASI_ENOTCAPABLE;
}
uint32_t wasi_fd_seek(ExecEnv e, uint32_t fd, uint64_t off, uint32_t whence, uint32_t out) {
    (void)e; (void)fd; (void)off; (void)whence; (void)out; return WASI_EBADF;
}
uint32_t wasi_path_open(ExecEnv e, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                        uint32_t f, uint64_t g, uint64_t h, uint32_t i, uint32_t j) {
    (void)e;(void)a;(void)b;(void)c;(void)d;(void)f;(void)g;(void)h;(void)i;(void)j;
    return WASI_ENOTCAPABLE;      // there is no filesystem, by design
}

// ---- what a real interpreter pulls in ------------------------------------
//
// The 15 above were enough for tests/mods/wasitest.c. A language runtime wants
// considerably more: Ruby imports 37 of these and CPython 43, and a wasm module
// must resolve EVERY import at instantiation, whether or not it is ever called.
// A missing one is not a runtime error a mod can handle -- it refuses the load.
//
// So these exist to be refused, not to work. The rule from
// docs/gearbox-interpreter-sdks.md holds: anything that would let a mod observe
// or touch a filesystem is refused honestly, never stubbed with a plausible
// answer. A fake successful path_open is far worse than ENOTCAPABLE, because
// the interpreter then believes it has a file.
//
// Only two do anything at all, and neither reveals anything about the machine:
// clock_res_get reports a fixed coarse resolution, and sched_yield succeeds
// because there is nothing to yield to.

uint32_t wasi_clock_res_get(ExecEnv e, uint32_t clockId, uint32_t outPtr) {
    (void)clockId;
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_WASISTUB)) return WASI_ENOTCAPABLE;
    // Matches wasi_clock_time_get's granularity: it ticks once per turn, so
    // claiming anything finer would be a lie a runtime might act on.
    uint64_t ns = 1000000000ull;
    if (!mi->memWrite(outPtr, 8, &ns)) return WASI_EINVAL;
    return WASI_ESUCCESS;
}

uint32_t wasi_sched_yield(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_WASISTUB)) return WASI_ENOTCAPABLE;
    return WASI_ESUCCESS;         // single-threaded; a no-op, not a refusal
}

// Refusals, by arity. ENOTCAPABLE is what WASI itself returns for a capability
// the sandbox withheld, so runtimes are written to expect it; EBADF is used
// where the call names a descriptor, since the honest answer is that the mod
// never had one.
uint32_t wasi_denied4(ExecEnv e, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    (void)e;(void)a;(void)b;(void)c;(void)d; return WASI_ENOTCAPABLE;
}
uint32_t wasi_denied5(ExecEnv e, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                      uint32_t f) {
    (void)e;(void)a;(void)b;(void)c;(void)d;(void)f; return WASI_ENOTCAPABLE;
}
uint32_t wasi_denied6(ExecEnv e, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                      uint32_t f, uint32_t g) {
    (void)e;(void)a;(void)b;(void)c;(void)d;(void)f;(void)g; return WASI_ENOTCAPABLE;
}
uint32_t wasi_denied7(ExecEnv e, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                      uint32_t f, uint32_t g, uint32_t h) {
    (void)e;(void)a;(void)b;(void)c;(void)d;(void)f;(void)g;(void)h;
    return WASI_ENOTCAPABLE;
}

// fd-shaped refusals: EBADF rather than ENOTCAPABLE.
uint32_t wasi_fd_denied_i(ExecEnv e, uint32_t fd) { (void)e;(void)fd; return WASI_EBADF; }
uint32_t wasi_fd_denied_ii(ExecEnv e, uint32_t fd, uint32_t a) {
    (void)e;(void)fd;(void)a; return WASI_EBADF;
}
uint32_t wasi_fd_denied_iI(ExecEnv e, uint32_t fd, uint64_t a) {
    (void)e;(void)fd;(void)a; return WASI_EBADF;
}
uint32_t wasi_fd_denied_iII(ExecEnv e, uint32_t fd, uint64_t a, uint64_t b) {
    (void)e;(void)fd;(void)a;(void)b; return WASI_EBADF;
}
uint32_t wasi_fd_denied_iIIi(ExecEnv e, uint32_t fd, uint64_t a, uint64_t b,
                             uint32_t c) {
    (void)e;(void)fd;(void)a;(void)b;(void)c; return WASI_EBADF;
}
uint32_t wasi_fd_denied_iiiIi(ExecEnv e, uint32_t fd, uint32_t a, uint32_t b,
                              uint64_t c, uint32_t d) {
    (void)e;(void)fd;(void)a;(void)b;(void)c;(void)d; return WASI_EBADF;
}
uint32_t wasi_path_denied_iiiiIIi(ExecEnv e, uint32_t a, uint32_t b, uint32_t c,
                                  uint32_t d, uint64_t f, uint64_t g, uint32_t h) {
    (void)e;(void)a;(void)b;(void)c;(void)d;(void)f;(void)g;(void)h;
    return WASI_ENOTCAPABLE;
}

// ------------------------------------------------------------------ Map ----
//
// Read-only geometry. Like GameState.Read, every one tolerates g_modGame being
// null: a mod panel is reachable from the main menu, where there is no map.

uint32_t map_width(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_MAP) || !g_modGame) return 0;
    return g_modGame->mapWidth();
}

uint32_t map_height(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_MAP) || !g_modGame) return 0;
    return g_modGame->mapHeight();
}

uint32_t map_province_count(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_MAP) || !g_modGame) return 0;
    return g_modGame->provinceCount();
}

uint32_t map_province_at(ExecEnv e, uint32_t index) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_MAP) || !g_modGame) return kInvalid;
    return g_modGame->provinceAt(index);
}

uint32_t map_province_name(ExecEnv e, uint32_t pid, uint32_t buf, uint32_t cap) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_MAP) || !g_modGame) return 0;
    std::string n = g_modGame->provinceName(pid);
    uint32_t len = (uint32_t)n.size();
    if (cap > 0 && buf != 0) {
        uint32_t take = len < cap ? len : cap;
        if (take && !mi->memWrite(buf, take, n.data())) return 0;
    }
    return len;                            // full length, per two-call sizing
}

double map_province_center_x(ExecEnv e, uint32_t pid) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_MAP) || !g_modGame) return 0.0;
    return g_modGame->provinceCenterX(pid);
}

double map_province_center_y(ExecEnv e, uint32_t pid) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_MAP) || !g_modGame) return 0.0;
    return g_modGame->provinceCenterY(pid);
}

uint32_t map_province_is_land(ExecEnv e, uint32_t pid) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_MAP) || !g_modGame) return 0;
    return g_modGame->provinceIsLand(pid) ? 1 : 0;
}

uint32_t map_province_neighbor_count(ExecEnv e, uint32_t pid) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_MAP) || !g_modGame) return 0;
    return g_modGame->provinceNeighborCount(pid);
}

uint32_t map_province_neighbor_at(ExecEnv e, uint32_t pid, uint32_t index) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_MAP) || !g_modGame) return kInvalid;
    return g_modGame->provinceNeighborAt(pid, index);
}

// ------------------------------------------------------------- Neural -----
//
// OBSERVE ONLY. A mod can read the feature vector the AI sees and the running
// reward means; there is no import here that writes to the model, the training
// state or the reward history, and that is deliberate rather than unfinished.
// data/ai/model.bin represents hours of training, and a capability that could
// quietly retrain it is not one a user can meaningfully consent to.

uint32_t neural_feature_count(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NEURAL) || !g_modGame) return 0;
    return g_modGame->neuralFeatureCount();
}

uint32_t neural_features(ExecEnv e, uint32_t cid, uint32_t buf, uint32_t cap) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NEURAL) || !g_modGame) return 0;

    // Sized first, then copied into a staging vector, so a mod handing us a
    // bogus pointer cannot make the AI write into its memory directly.
    uint32_t need = g_modGame->neuralFeatures(cid, nullptr, 0);
    if (need == 0 || cap == 0 || buf == 0) return need;

    uint32_t take = need < cap ? need : cap;
    std::vector<float> tmp(take);
    g_modGame->neuralFeatures(cid, tmp.data(), take);
    if (!mi->memWrite(buf, take * (uint32_t)sizeof(float), tmp.data())) return 0;
    return need;                    // full length, per two-call sizing
}

uint32_t neural_reward_count(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NEURAL) || !g_modGame) return 0;
    return g_modGame->neuralRewardCount();
}

double neural_reward_mean(ExecEnv e, uint32_t index) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NEURAL) || !g_modGame) return 0.0;
    return g_modGame->neuralRewardMean(index);
}

// ------------------------------------------------------ GameState.Write ---
//
// The only capability that changes the world. Each call validates through the
// game's own code and returns 0 rather than trapping, so a mod that asks for
// something impossible degrades instead of dying -- and each is logged, so a
// player can see what a mod did to their game.

// Records the post-write value rather than the argument, so `set` and `add`
// are comparable: two mods that reach the same treasury by different routes
// are not in conflict, and two that reach different ones are.
void noteWrite(ModInstance* mi, const std::string& target, double value) {
    char buf[40];
    snprintf(buf, sizeof buf, "%.6g", value);
    ModConflicts::get().recordWrite(mi->id(), target, buf);
}

uint32_t gsw_set_country_treasury(ExecEnv e, uint32_t cid, double value) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_GAMESTATE_WRITE) || !g_modGame) return 0;
    if (!g_modGame->setCountryTreasury(cid, value)) return 0;
    noteWrite(mi, "country:" + std::to_string(cid) + ":treasury",
              g_modGame->countryTreasury(cid));
    return 1;
}

uint32_t gsw_add_country_treasury(ExecEnv e, uint32_t cid, double delta) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_GAMESTATE_WRITE) || !g_modGame) return 0;
    if (!g_modGame->addCountryTreasury(cid, delta)) return 0;
    noteWrite(mi, "country:" + std::to_string(cid) + ":treasury",
              g_modGame->countryTreasury(cid));
    return 1;
}

uint32_t gsw_set_province_population(ExecEnv e, uint32_t pid, uint64_t value) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_GAMESTATE_WRITE) || !g_modGame) return 0;
    // The ABI carries this as i64; a mod passing a negative is refused by the
    // game rather than wrapping into an enormous population.
    if (!g_modGame->setProvincePopulation(pid, (long long)value)) return 0;
    ModConflicts::get().recordWrite(mi->id(),
        "province:" + std::to_string(pid) + ":population",
        std::to_string((long long)value));
    return 1;
}

uint32_t gsw_set_province_owner(ExecEnv e, uint32_t pid, uint32_t cid) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_GAMESTATE_WRITE) || !g_modGame) return 0;
    bool ok = g_modGame->setProvinceOwner(pid, cid);
    // Territory changing hands is the single most consequential thing a mod can
    // do, so it is always logged rather than only on failure.
    pushLog(mi->id(), ok ? 1 : 2,
            ok ? "transferred a province via GameState.Write"
               : "province transfer refused");
    if (ok)
        ModConflicts::get().recordWrite(mi->id(),
            "province:" + std::to_string(pid) + ":owner", std::to_string(cid));
    return ok ? 1 : 0;
}

// ------------------------------------------------------------ Diplomacy ---
//
// Reads are flags. The one mutating call proposes rather than performs: it is
// routed through the game's own declareWar, which owns guarantee chains and
// every other consequence, and which may simply refuse.

uint32_t dip_at_war(ExecEnv e, uint32_t a, uint32_t b) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_DIPLOMACY) || !g_modGame) return 0;
    return g_modGame->atWar(a, b) ? 1 : 0;
}

uint32_t dip_allied(ExecEnv e, uint32_t a, uint32_t b) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_DIPLOMACY) || !g_modGame) return 0;
    return g_modGame->allied(a, b) ? 1 : 0;
}

uint32_t dip_non_aggression(ExecEnv e, uint32_t a, uint32_t b) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_DIPLOMACY) || !g_modGame) return 0;
    return g_modGame->nonAggression(a, b) ? 1 : 0;
}

uint32_t dip_guaranteed(ExecEnv e, uint32_t a, uint32_t b) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_DIPLOMACY) || !g_modGame) return 0;
    return g_modGame->guaranteed(a, b) ? 1 : 0;
}

uint32_t dip_propose_war(ExecEnv e, uint32_t attacker, uint32_t defender) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_DIPLOMACY) || !g_modGame) return 0;
    bool ok = g_modGame->proposeWar(attacker, defender);
    if (ok) {
        // Declaring war on someone another mod is trying to keep at peace is a
        // conflict a player will feel, so it is tracked like any other write.
        ModConflicts::get().recordWrite(mi->id(),
            "diplomacy:" + std::to_string(attacker) + "-" + std::to_string(defender),
            "war");
    }
    // Logged either way. A mod starting a war is something a player should be
    // able to see in the mod log after the fact.
    pushLog(mi->id(), ok ? 1 : 2,
            ok ? "declared war via Diplomacy" : "war proposal refused");
    return ok ? 1 : 0;
}

// --------------------------------------------------------- Storage ----
//
// Namespaced by the calling mod's own id, which the mod cannot influence: it
// comes from the instance, not from an argument. There is deliberately no way
// to name another mod's store, and no way to enumerate one.

uint32_t storage_get(ExecEnv e, uint32_t keyPtr, uint32_t keyLen,
                     uint32_t buf, uint32_t cap) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_STORAGE)) return kInvalid;
    std::string key;
    if (!mi->readString(keyPtr, keyLen, key)) return kInvalid;

    std::string val;
    // Absent is kInvalid, not 0: a zero-length value is a real value, and
    // conflating them would make "have I stored this yet" unanswerable.
    if (!ModStorage::get().get(mi->id(), key, val)) return kInvalid;

    uint32_t n = (uint32_t)val.size();
    if (cap > 0 && buf != 0) {
        uint32_t take = n < cap ? n : cap;
        if (take && !mi->memWrite(buf, take, val.data())) return kInvalid;
    }
    return n;                              // full length, per two-call sizing
}

uint32_t storage_set(ExecEnv e, uint32_t keyPtr, uint32_t keyLen,
                     uint32_t valPtr, uint32_t valLen) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_STORAGE)) return 0;
    std::string key, val;
    if (!mi->readString(keyPtr, keyLen, key)) return 0;
    if (valLen > 0 && !mi->readString(valPtr, valLen, val)) return 0;

    std::string err;
    if (!ModStorage::get().set(mi->id(), key, val, err)) {
        pushLog(mi->id(), 2, "storage set refused: " + err);
        return 0;
    }
    return 1;
}

uint32_t storage_remove(ExecEnv e, uint32_t keyPtr, uint32_t keyLen) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_STORAGE)) return 0;
    std::string key;
    if (!mi->readString(keyPtr, keyLen, key)) return 0;
    return ModStorage::get().remove(mi->id(), key) ? 1 : 0;
}

// ---- ui, 1.1 additions: the reskin surface ----
//
// Rect + 14pt text is a debug overlay. To let a mod put its own interface on
// screen it needs lines, circles, its own art and a choice of type size, and it
// needs to know how big its panel actually is so it can lay out against it.
//
// The safety story does not change: these still only APPEND to the panel's
// command list, the host still owns the transform and the scissor, and a
// coordinate outside the panel is clipped rather than honoured.

void ui_draw_line(ExecEnv e, uint32_t panel, int32_t x1, int32_t y1,
                  int32_t x2, int32_t y2, double thick, uint32_t rgba) {
    ModPanel* p = ownedPanel(self(e), panel);
    if (!p || p->cmds.size() >= ModUI::kMaxCmdsPerPanel) return;
    ModDrawCmd c;
    c.kind = ModDrawCmd::Line;
    c.x = x1; c.y = y1; c.x2 = x2; c.y2 = y2;
    c.thickness = (float)std::clamp(thick, 0.25, 64.0);
    c.rgba = rgba;
    p->cmds.push_back(std::move(c));
}

void ui_draw_circle(ExecEnv e, uint32_t panel, int32_t cx, int32_t cy,
                    double r, uint32_t rgba) {
    ModPanel* p = ownedPanel(self(e), panel);
    if (!p || p->cmds.size() >= ModUI::kMaxCmdsPerPanel) return;
    ModDrawCmd c;
    c.kind = ModDrawCmd::Circle;
    c.x = cx; c.y = cy;
    c.radius = (float)std::clamp(r, 0.0, 4096.0);
    c.rgba = rgba;
    p->cmds.push_back(std::move(c));
}

// The image comes from the MOD'S OWN package and nowhere else -- the name is a
// path inside its .odmod, resolved by the same readAsset the assets module
// uses. A mod cannot name a file on disk, a game asset, or another mod's art.
void ui_draw_image(ExecEnv e, uint32_t panel, int32_t x, int32_t y,
                   int32_t w, int32_t h, uint32_t namePtr, uint32_t nameLen,
                   uint32_t tint) {
    ModInstance* mi = self(e);
    ModPanel* p = ownedPanel(mi, panel);
    if (!p || p->cmds.size() >= ModUI::kMaxCmdsPerPanel) return;
    std::string name;
    if (!mi->readString(namePtr, nameLen, name)) return;
    if (name.size() > 256) return;
    ModDrawCmd c;
    c.kind = ModDrawCmd::Image;
    c.x = x; c.y = y; c.w = w; c.h = h;
    c.rgba = tint ? tint : 0xFFFFFFFFu;
    c.text = std::move(name);
    p->cmds.push_back(std::move(c));
}

void ui_draw_text_sized(ExecEnv e, uint32_t panel, int32_t x, int32_t y,
                        int32_t size, uint32_t rgba, uint32_t textPtr,
                        uint32_t textLen) {
    ModInstance* mi = self(e);
    ModPanel* p = ownedPanel(mi, panel);
    if (!p || p->cmds.size() >= ModUI::kMaxCmdsPerPanel) return;
    std::string t;
    if (!mi->readString(textPtr, textLen, t)) return;
    if (t.size() > 512) t.resize(512);
    ModDrawCmd c;
    c.kind = ModDrawCmd::Text;
    c.x = x; c.y = y; c.rgba = rgba;
    c.fontSize = std::clamp(size, 6, 96);
    c.text = std::move(t);
    p->cmds.push_back(std::move(c));
}

// Laying text out needs its width BEFORE it is drawn -- centring, wrapping and
// right-alignment are all impossible without it. Measured by the host with the
// same font it will draw with, via the bridge, so the answer is truthful.
uint32_t ui_measure_text(ExecEnv e, uint32_t textPtr, uint32_t textLen, int32_t size) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_UI)) return 0;
    std::string t;
    if (!mi->readString(textPtr, textLen, t)) return 0;
    if (t.size() > 512) t.resize(512);
    if (!g_uiBridge.measureText) return (uint32_t)(t.size() * (size_t)std::max(1, size) / 2);
    return g_uiBridge.measureText(t, std::clamp(size, 6, 96));
}

uint32_t ui_panel_width(ExecEnv e, uint32_t panel) {
    ModPanel* p = ownedPanel(self(e), panel);
    return p ? (uint32_t)p->w : 0;
}
uint32_t ui_panel_height(ExecEnv e, uint32_t panel) {
    ModPanel* p = ownedPanel(self(e), panel);
    return p ? (uint32_t)p->h : 0;
}
void ui_panel_set_visible(ExecEnv e, uint32_t panel, uint32_t visible) {
    ModPanel* p = ownedPanel(self(e), panel);
    if (p) p->visible = visible != 0;
}

// Panel-relative, and zero when the pointer is elsewhere -- a mod still cannot
// watch the cursor outside its own box.
double ui_mouse_x(ExecEnv e, uint32_t panel) {
    ModPanel* p = ownedPanel(self(e), panel);
    return (p && p->mouseInside) ? p->mouseX : 0.0;
}
double ui_mouse_y(ExecEnv e, uint32_t panel) {
    ModPanel* p = ownedPanel(self(e), panel);
    return (p && p->mouseInside) ? p->mouseY : 0.0;
}
uint32_t ui_mouse_inside(ExecEnv e, uint32_t panel) {
    ModPanel* p = ownedPanel(self(e), panel);
    return (p && p->mouseInside) ? 1u : 0u;
}

// THE CHEAPEST REAL RESKIN LEVER IN THE GAME. The accent colour is read at 113
// sites -- every heading, highlight, selection and button in the interface --
// so one call here restyles the whole game rather than one panel. It is
// cosmetic and fully reversible, which is why it sits under UI rather than
// behind a write capability.
uint32_t ui_theme_accent(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_UI) || !g_uiBridge.themeAccent) return 0;
    return g_uiBridge.themeAccent();
}
uint32_t ui_set_theme_accent(ExecEnv e, uint32_t rgb) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_UI) || !g_uiBridge.setThemeAccent) return 0;
    g_uiBridge.setThemeAccent(rgb & 0x00FFFFFFu);
    return 1;
}

// ------------------------------------------------------- Gearbox 1.1 ------
//
// Everything below was added in ABI 1.1 for the reskin surface: the military,
// research, political and economic state a total conversion needs to read, the
// four order calls it needs to write, and the map queries a scenario generator
// needs. The v1.0 entries above are untouched -- see modAbiMinorFor().
//
// TWO RULES HOLD THROUGHOUT.
//
// Every read is bounds-checked in the Game accessor and returns a neutral value
// (0, or an empty string) for an id that does not exist, rather than trapping.
// A mod iterating a count that changed under it must not be able to crash the
// game.
//
// Every write goes through the same resolver the player's own click goes
// through. Nothing here reaches into a container directly. That is what makes
// the capability bit meaningful: granting Military.Write lets a mod issue
// orders, not fabricate outcomes.

// Two-call sizing, as map_province_name does it: call with cap 0 to learn the
// length, allocate, call again. Returns the FULL length either way.
uint32_t retStr(ModInstance* mi, const std::string& v, uint32_t buf, uint32_t cap) {
    uint32_t len = (uint32_t)v.size();
    if (cap > 0 && buf != 0) {
        uint32_t take = len < cap ? len : cap;
        if (take && !mi->memWrite(buf, take, v.data())) return 0;
    }
    return len;
}

#define MOD_GUARD(bit, fail) \
    ModInstance* mi = self(e); \
    if (!mi || !mi->has(bit) || !g_modGame) return fail; \
    (void)mi;

// ---- military.read ----

uint32_t mil_ship_count(ExecEnv e) {
    MOD_GUARD(MODULE_MILITARY_READ, 0) return g_modGame->shipCount();
}
uint32_t mil_ship_at(ExecEnv e, uint32_t i) {
    MOD_GUARD(MODULE_MILITARY_READ, 0xFFFFFFFFu) return g_modGame->shipAt(i);
}
uint32_t mil_ship_exists(ExecEnv e, uint32_t s) {
    MOD_GUARD(MODULE_MILITARY_READ, 0) return g_modGame->shipExists(s) ? 1u : 0u;
}
uint32_t mil_ship_owner(ExecEnv e, uint32_t s) {
    MOD_GUARD(MODULE_MILITARY_READ, 0xFFFFFFFFu) return g_modGame->shipOwner(s);
}
uint32_t mil_ship_type(ExecEnv e, uint32_t s, uint32_t buf, uint32_t cap) {
    MOD_GUARD(MODULE_MILITARY_READ, 0) return retStr(mi, g_modGame->shipType(s), buf, cap);
}
double mil_ship_lon(ExecEnv e, uint32_t s) {
    MOD_GUARD(MODULE_MILITARY_READ, 0.0) return g_modGame->shipLon(s);
}
double mil_ship_lat(ExecEnv e, uint32_t s) {
    MOD_GUARD(MODULE_MILITARY_READ, 0.0) return g_modGame->shipLat(s);
}
int32_t mil_ship_health(ExecEnv e, uint32_t s) {
    MOD_GUARD(MODULE_MILITARY_READ, 0) return g_modGame->shipHealth(s);
}
int32_t mil_ship_crew(ExecEnv e, uint32_t s) {
    MOD_GUARD(MODULE_MILITARY_READ, 0) return g_modGame->shipCrew(s);
}
double mil_ship_range(ExecEnv e, uint32_t s) {
    MOD_GUARD(MODULE_MILITARY_READ, 0.0) return g_modGame->shipRange(s);
}
uint32_t mil_army_stack_count(ExecEnv e, uint32_t p) {
    MOD_GUARD(MODULE_MILITARY_READ, 0) return g_modGame->armyStackCount(p);
}
uint32_t mil_army_stack_owner(ExecEnv e, uint32_t p, uint32_t i) {
    MOD_GUARD(MODULE_MILITARY_READ, 0xFFFFFFFFu) return g_modGame->armyStackOwner(p, i);
}
int64_t mil_army_stack_size(ExecEnv e, uint32_t p, uint32_t i) {
    MOD_GUARD(MODULE_MILITARY_READ, 0) return g_modGame->armyStackSize(p, i);
}
int64_t mil_country_army(ExecEnv e, uint32_t c) {
    MOD_GUARD(MODULE_MILITARY_READ, 0) return g_modGame->countryArmy(c);
}
int32_t mil_province_fortification(ExecEnv e, uint32_t p) {
    MOD_GUARD(MODULE_MILITARY_READ, 0) return g_modGame->provinceFortification(p);
}
int32_t mil_province_port_level(ExecEnv e, uint32_t p) {
    MOD_GUARD(MODULE_MILITARY_READ, 0) return g_modGame->provincePortLevel(p);
}

// ---- military.write ----
//
// All four queue an order for the turn resolver rather than moving anything.
// A mod cannot teleport an army across the map or sink a ship out of range,
// because these land in the same queue the UI writes to and the same
// validation runs over them at end of turn.

uint32_t milw_order_army_move(ExecEnv e, uint32_t f, uint32_t t, uint32_t pct) {
    MOD_GUARD(MODULE_MILITARY_WRITE, 0) return g_modGame->orderArmyMove(f, t, pct) ? 1u : 0u;
}
uint32_t milw_order_ship_move(ExecEnv e, uint32_t s, double lon, double lat) {
    MOD_GUARD(MODULE_MILITARY_WRITE, 0) return g_modGame->orderShipMove(s, lon, lat) ? 1u : 0u;
}
uint32_t milw_order_ship_engage(ExecEnv e, uint32_t s, uint32_t t) {
    MOD_GUARD(MODULE_MILITARY_WRITE, 0) return g_modGame->orderShipEngage(s, t) ? 1u : 0u;
}
uint32_t milw_order_ship_bombard(ExecEnv e, uint32_t s, uint32_t p, uint32_t ammoPtr, uint32_t ammoLen) {
    MOD_GUARD(MODULE_MILITARY_WRITE, 0)
    std::string ammo;
    if (ammoLen > 0 && !mi->readString(ammoPtr, ammoLen, ammo)) return 0;
    return g_modGame->orderShipBombard(s, p, ammo) ? 1u : 0u;
}

// ---- research ----

uint32_t res_node_count(ExecEnv e) {
    MOD_GUARD(MODULE_RESEARCH_READ, 0) return g_modGame->researchNodeCount();
}
uint32_t res_node_id(ExecEnv e, uint32_t i, uint32_t buf, uint32_t cap) {
    MOD_GUARD(MODULE_RESEARCH_READ, 0) return retStr(mi, g_modGame->researchNodeId(i), buf, cap);
}
uint32_t res_node_name(ExecEnv e, uint32_t i, uint32_t buf, uint32_t cap) {
    MOD_GUARD(MODULE_RESEARCH_READ, 0) return retStr(mi, g_modGame->researchNodeName(i), buf, cap);
}
uint32_t res_node_category(ExecEnv e, uint32_t i, uint32_t buf, uint32_t cap) {
    MOD_GUARD(MODULE_RESEARCH_READ, 0) return retStr(mi, g_modGame->researchNodeCategory(i), buf, cap);
}
int32_t res_node_cost(ExecEnv e, uint32_t i) {
    MOD_GUARD(MODULE_RESEARCH_READ, 0) return g_modGame->researchNodeCost(i);
}
uint32_t res_country_has_researched(ExecEnv e, uint32_t c, uint32_t nPtr, uint32_t nLen) {
    MOD_GUARD(MODULE_RESEARCH_READ, 0)
    std::string n;
    if (!mi->readString(nPtr, nLen, n)) return 0;
    return g_modGame->countryHasResearched(c, n) ? 1u : 0u;
}
// A SHARE OF INCOME IN 0..1, not an absolute sum -- that is how the game stores
// research funding and how its own screen presents it.
double res_country_funding(ExecEnv e, uint32_t c) {
    MOD_GUARD(MODULE_RESEARCH_READ, 0.0) return g_modGame->countryResearchFunding(c);
}
uint32_t resw_set_country_funding(ExecEnv e, uint32_t c, double v) {
    MOD_GUARD(MODULE_RESEARCH_WRITE, 0) return g_modGame->setCountryResearchFunding(c, v) ? 1u : 0u;
}

// ---- politics ----

double pol_country_compass_econ(ExecEnv e, uint32_t c) {
    MOD_GUARD(MODULE_POLITICS_READ, 0.0) return g_modGame->countryCompassEcon(c);
}
double pol_country_compass_social(ExecEnv e, uint32_t c) {
    MOD_GUARD(MODULE_POLITICS_READ, 0.0) return g_modGame->countryCompassSocial(c);
}
double pol_province_unrest(ExecEnv e, uint32_t p) {
    MOD_GUARD(MODULE_POLITICS_READ, 0.0) return g_modGame->provinceUnrest(p);
}
uint32_t pol_policy_count(ExecEnv e) {
    MOD_GUARD(MODULE_POLITICS_READ, 0) return g_modGame->policyCount();
}
uint32_t pol_policy_id(ExecEnv e, uint32_t i, uint32_t buf, uint32_t cap) {
    MOD_GUARD(MODULE_POLITICS_READ, 0) return retStr(mi, g_modGame->policyId(i), buf, cap);
}
uint32_t pol_policy_name(ExecEnv e, uint32_t i, uint32_t buf, uint32_t cap) {
    MOD_GUARD(MODULE_POLITICS_READ, 0) return retStr(mi, g_modGame->policyName(i), buf, cap);
}
uint32_t pol_country_has_policy(ExecEnv e, uint32_t c, uint32_t pPtr, uint32_t pLen) {
    MOD_GUARD(MODULE_POLITICS_READ, 0)
    std::string id;
    if (!mi->readString(pPtr, pLen, id)) return 0;
    return g_modGame->countryHasPolicy(c, id) ? 1u : 0u;
}
uint32_t pol_province_minority_count(ExecEnv e, uint32_t p) {
    MOD_GUARD(MODULE_POLITICS_READ, 0) return g_modGame->provinceMinorityCount(p);
}
uint32_t pol_province_minority_name(ExecEnv e, uint32_t p, uint32_t i, uint32_t buf, uint32_t cap) {
    MOD_GUARD(MODULE_POLITICS_READ, 0) return retStr(mi, g_modGame->provinceMinorityName(p, i), buf, cap);
}
double pol_province_minority_share(ExecEnv e, uint32_t p, uint32_t i) {
    MOD_GUARD(MODULE_POLITICS_READ, 0.0) return g_modGame->provinceMinorityShare(p, i);
}
uint32_t polw_set_country_policy(ExecEnv e, uint32_t c, uint32_t pPtr, uint32_t pLen, uint32_t on) {
    MOD_GUARD(MODULE_POLITICS_WRITE, 0)
    std::string id;
    if (!mi->readString(pPtr, pLen, id)) return 0;
    return g_modGame->setCountryPolicy(c, id, on != 0) ? 1u : 0u;
}

// ---- economy ----

double eco_country_income_gross(ExecEnv e, uint32_t c) {
    MOD_GUARD(MODULE_ECONOMY_READ, 0.0) return g_modGame->countryIncomeGross(c);
}
double eco_country_income_net(ExecEnv e, uint32_t c) {
    MOD_GUARD(MODULE_ECONOMY_READ, 0.0) return g_modGame->countryIncomeNet(c);
}
double eco_country_army_upkeep(ExecEnv e, uint32_t c) {
    MOD_GUARD(MODULE_ECONOMY_READ, 0.0) return g_modGame->countryArmyUpkeep(c);
}
double eco_country_navy_upkeep(ExecEnv e, uint32_t c) {
    MOD_GUARD(MODULE_ECONOMY_READ, 0.0) return g_modGame->countryNavyUpkeep(c);
}
uint32_t eco_country_is_bankrupt(ExecEnv e, uint32_t c) {
    MOD_GUARD(MODULE_ECONOMY_READ, 0) return g_modGame->countryIsBankrupt(c) ? 1u : 0u;
}
int32_t eco_province_industry_level(ExecEnv e, uint32_t p) {
    MOD_GUARD(MODULE_ECONOMY_READ, 0) return g_modGame->provinceIndustryLevel(p);
}
uint32_t eco_province_industry_specialization(ExecEnv e, uint32_t p, uint32_t buf, uint32_t cap) {
    MOD_GUARD(MODULE_ECONOMY_READ, 0)
    return retStr(mi, g_modGame->provinceIndustrySpecialization(p), buf, cap);
}
double eco_province_resource(ExecEnv e, uint32_t p, uint32_t wPtr, uint32_t wLen) {
    MOD_GUARD(MODULE_ECONOMY_READ, 0.0)
    std::string w;
    if (!mi->readString(wPtr, wLen, w)) return 0.0;
    return g_modGame->provinceResource(p, w);
}
uint32_t ecow_set_province_industry_level(ExecEnv e, uint32_t p, int32_t l) {
    MOD_GUARD(MODULE_ECONOMY_WRITE, 0) return g_modGame->setProvinceIndustryLevel(p, l) ? 1u : 0u;
}

// ---- map, 1.1 additions ----

uint32_t map_province_is_coastal(ExecEnv e, uint32_t p) {
    MOD_GUARD(MODULE_MAP, 0) return g_modGame->provinceIsCoastal(p) ? 1u : 0u;
}
uint32_t map_sea_route_exists(ExecEnv e, double aLon, double aLat, double bLon, double bLat) {
    MOD_GUARD(MODULE_MAP, 0) return g_modGame->seaRouteExists(aLon, aLat, bLon, bLat) ? 1u : 0u;
}
uint32_t map_point_is_land(ExecEnv e, double lon, double lat) {
    MOD_GUARD(MODULE_MAP, 0) return g_modGame->pointIsLand(lon, lat) ? 1u : 0u;
}

#undef MOD_GUARD

// ---- mapeditor ----
//
// Two gates, not one. The capability says the player agreed to let this mod
// touch map projects; editorActive() says there IS a project to touch. Holding
// MapEditor while a game is running gets nothing, which is the point: the data
// behind these calls is an editor project, and a running game does not have one.

#define ED_GUARD(fail) \
    ModInstance* mi = self(e); \
    if (!mi || !mi->has(MODULE_MAPEDITOR) || !g_modGame) return fail; \
    if (!g_modGame->editorActive()) return fail; \
    (void)mi;

uint32_t ed_active(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_MAPEDITOR) || !g_modGame) return 0;
    return g_modGame->editorActive() ? 1u : 0u;
}
uint32_t ed_province_count(ExecEnv e) {
    ED_GUARD(0) return g_modGame->editorProvinceCount();
}
uint32_t ed_province_at(ExecEnv e, uint32_t i) {
    ED_GUARD(0xFFFFFFFFu) return g_modGame->editorProvinceAt(i);
}
int64_t ed_province_population(ExecEnv e, uint32_t p) {
    ED_GUARD(0) return g_modGame->editorProvincePopulation(p);
}
int32_t ed_province_industry_level(ExecEnv e, uint32_t p) {
    ED_GUARD(0) return g_modGame->editorProvinceIndustryLevel(p);
}
int32_t ed_province_fortification(ExecEnv e, uint32_t p) {
    ED_GUARD(0) return g_modGame->editorProvinceFortification(p);
}
int32_t ed_province_port_level(ExecEnv e, uint32_t p) {
    ED_GUARD(0) return g_modGame->editorProvincePortLevel(p);
}
double ed_province_resource(ExecEnv e, uint32_t p, uint32_t wPtr, uint32_t wLen) {
    ED_GUARD(0.0)
    std::string w;
    if (!mi->readString(wPtr, wLen, w)) return 0.0;
    return g_modGame->editorProvinceResource(p, w);
}
double ed_province_compass_econ(ExecEnv e, uint32_t p) {
    ED_GUARD(0.0) return g_modGame->editorProvinceCompassEcon(p);
}
double ed_province_compass_social(ExecEnv e, uint32_t p) {
    ED_GUARD(0.0) return g_modGame->editorProvinceCompassSocial(p);
}
uint32_t ed_set_province_population(ExecEnv e, uint32_t p, int64_t v) {
    ED_GUARD(0) return g_modGame->editorSetProvincePopulation(p, v) ? 1u : 0u;
}
uint32_t ed_set_province_industry_level(ExecEnv e, uint32_t p, int32_t v) {
    ED_GUARD(0) return g_modGame->editorSetProvinceIndustryLevel(p, v) ? 1u : 0u;
}
uint32_t ed_set_province_fortification(ExecEnv e, uint32_t p, int32_t v) {
    ED_GUARD(0) return g_modGame->editorSetProvinceFortification(p, v) ? 1u : 0u;
}
uint32_t ed_set_province_port_level(ExecEnv e, uint32_t p, int32_t v) {
    ED_GUARD(0) return g_modGame->editorSetProvincePortLevel(p, v) ? 1u : 0u;
}
uint32_t ed_set_province_resource(ExecEnv e, uint32_t p, uint32_t wPtr, uint32_t wLen, double v) {
    ED_GUARD(0)
    std::string w;
    if (!mi->readString(wPtr, wLen, w)) return 0;
    return g_modGame->editorSetProvinceResource(p, w, v) ? 1u : 0u;
}
uint32_t ed_set_province_compass(ExecEnv e, uint32_t p, double econ, double social) {
    ED_GUARD(0) return g_modGame->editorSetProvinceCompass(p, econ, social) ? 1u : 0u;
}
uint32_t ed_map_name(ExecEnv e, uint32_t buf, uint32_t cap) {
    ED_GUARD(0) return retStr(mi, g_modGame->editorMapName(), buf, cap);
}
uint32_t ed_set_map_name(ExecEnv e, uint32_t ptr, uint32_t len) {
    ED_GUARD(0)
    std::string n;
    if (!mi->readString(ptr, len, n)) return 0;
    return g_modGame->editorSetMapName(n) ? 1u : 0u;
}
uint32_t ed_set_author(ExecEnv e, uint32_t ptr, uint32_t len) {
    ED_GUARD(0)
    std::string a;
    if (!mi->readString(ptr, len, a)) return 0;
    return g_modGame->editorSetAuthor(a) ? 1u : 0u;
}
uint32_t ed_set_license(ExecEnv e, uint32_t ptr, uint32_t len) {
    ED_GUARD(0)
    std::string l;
    if (!mi->readString(ptr, len, l)) return 0;
    return g_modGame->editorSetLicense(l) ? 1u : 0u;
}

#undef ED_GUARD

// ---- net, 1.1 additions ----

uint32_t net_peer_at(ExecEnv e, uint32_t i) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NET) || !g_modGame) return 0xFFFFFFFFu;
    return g_modGame->netPeerAt(i);
}
uint32_t net_peer_name(ExecEnv e, uint32_t i, uint32_t buf, uint32_t cap) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NET) || !g_modGame) return 0;
    return retStr(mi, g_modGame->netPeerName(i), buf, cap);
}
uint32_t net_max_message_bytes(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NET) || !g_modGame) return 0;
    return g_modGame->netMaxMessageBytes();
}

// ---- neural, 1.1 additions ----

uint32_t neural_module_count(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NEURAL) || !g_modGame) return 0;
    return g_modGame->neuralModuleCount();
}
uint32_t neural_module_name(ExecEnv e, uint32_t m, uint32_t buf, uint32_t cap) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NEURAL) || !g_modGame) return 0;
    return retStr(mi, g_modGame->neuralModuleName(m), buf, cap);
}
uint32_t neural_action_count(ExecEnv e, uint32_t m) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NEURAL) || !g_modGame) return 0;
    return g_modGame->neuralActionCount(m);
}
uint32_t neural_action_name(ExecEnv e, uint32_t m, uint32_t a, uint32_t buf, uint32_t cap) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NEURAL) || !g_modGame) return 0;
    return retStr(mi, g_modGame->neuralActionName(m, a), buf, cap);
}
uint32_t neural_country_is_ai(ExecEnv e, uint32_t c) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NEURAL) || !g_modGame) return 0;
    return g_modGame->neuralCountryIsAI(c) ? 1u : 0u;
}
int64_t neural_update_count(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NEURAL) || !g_modGame) return 0;
    return g_modGame->neuralUpdateCount();
}
uint32_t neural_model_loaded(ExecEnv e) {
    ModInstance* mi = self(e);
    if (!mi || !mi->has(MODULE_NEURAL) || !g_modGame) return 0;
    return g_modGame->neuralModelLoaded() ? 1u : 0u;
}

// --------------------------------------------------------------- table ----

// A capability of 0 means "always available". Core uses it rather than
// MODULE_CORE so that the module genuinely cannot be revoked -- a hand-edited
// grants file cannot leave a mod unable to log its own failure.
const ModHostFn kHostFunctions[] = {
    {"gearbox:core", "log",          "(iii)", (void*)core_log,          0},
    {"gearbox:core", "env",          "(i)",   (void*)core_env,          0},
    {"gearbox:core", "abort",        "(ii)",  (void*)core_abort,        0},
    {"gearbox:core", "fuel_budget",  "()I",   (void*)core_fuel_budget,  0},

    {"gearbox:ui", "panel_register", "(iiii)i",   (void*)ui_panel_register, MODULE_UI},
    {"gearbox:ui", "draw_rect",      "(iiiiii)",  (void*)ui_draw_rect,      MODULE_UI},
    {"gearbox:ui", "draw_text",      "(iiiiii)",  (void*)ui_draw_text,      MODULE_UI},
    {"gearbox:ui", "button",         "(iiiiiii)i",(void*)ui_button,         MODULE_UI},

    {"gearbox:gamestate.read", "turn_number",           "()i",    (void*)gs_turn_number,           MODULE_GAMESTATE_READ},
    {"gearbox:gamestate.read", "country_count",         "()i",    (void*)gs_country_count,         MODULE_GAMESTATE_READ},
    {"gearbox:gamestate.read", "country_at",            "(i)i",   (void*)gs_country_at,            MODULE_GAMESTATE_READ},
    {"gearbox:gamestate.read", "country_name",          "(iii)i", (void*)gs_country_name,          MODULE_GAMESTATE_READ},
    {"gearbox:gamestate.read", "country_treasury",      "(i)F",   (void*)gs_country_treasury,      MODULE_GAMESTATE_READ},
    {"gearbox:gamestate.read", "country_province_count","(i)i",   (void*)gs_country_province_count,MODULE_GAMESTATE_READ},
    {"gearbox:gamestate.read", "province_population",   "(i)I",   (void*)gs_province_population,   MODULE_GAMESTATE_READ},
    {"gearbox:gamestate.read", "province_owner",        "(i)i",   (void*)gs_province_owner,        MODULE_GAMESTATE_READ},

    {"gearbox:neural", "feature_count", "()i",     (void*)neural_feature_count, MODULE_NEURAL},
    {"gearbox:neural", "features",      "(iii)i",  (void*)neural_features,      MODULE_NEURAL},
    {"gearbox:neural", "reward_count",  "()i",     (void*)neural_reward_count,  MODULE_NEURAL},
    {"gearbox:neural", "reward_mean",   "(i)F",    (void*)neural_reward_mean,   MODULE_NEURAL},

    {"gearbox:gamestate.write", "set_country_treasury", "(iF)i", (void*)gsw_set_country_treasury, MODULE_GAMESTATE_WRITE},
    {"gearbox:gamestate.write", "add_country_treasury", "(iF)i", (void*)gsw_add_country_treasury, MODULE_GAMESTATE_WRITE},
    {"gearbox:gamestate.write", "set_province_owner",   "(ii)i", (void*)gsw_set_province_owner,   MODULE_GAMESTATE_WRITE},
    {"gearbox:gamestate.write", "set_province_population","(iI)i",(void*)gsw_set_province_population, MODULE_GAMESTATE_WRITE},

    {"gearbox:diplomacy", "at_war",         "(ii)i", (void*)dip_at_war,         MODULE_DIPLOMACY},
    {"gearbox:diplomacy", "allied",         "(ii)i", (void*)dip_allied,         MODULE_DIPLOMACY},
    {"gearbox:diplomacy", "non_aggression", "(ii)i", (void*)dip_non_aggression, MODULE_DIPLOMACY},
    {"gearbox:diplomacy", "guaranteed",     "(ii)i", (void*)dip_guaranteed,     MODULE_DIPLOMACY},
    {"gearbox:diplomacy", "propose_war",    "(ii)i", (void*)dip_propose_war,    MODULE_DIPLOMACY},

    {"gearbox:map", "width",                  "()i",     (void*)map_width,                  MODULE_MAP},
    {"gearbox:map", "height",                 "()i",     (void*)map_height,                 MODULE_MAP},
    {"gearbox:map", "province_count",         "()i",     (void*)map_province_count,         MODULE_MAP},
    {"gearbox:map", "province_at",            "(i)i",    (void*)map_province_at,            MODULE_MAP},
    {"gearbox:map", "province_name",          "(iii)i",  (void*)map_province_name,          MODULE_MAP},
    {"gearbox:map", "province_center_x",      "(i)F",    (void*)map_province_center_x,      MODULE_MAP},
    {"gearbox:map", "province_center_y",      "(i)F",    (void*)map_province_center_y,      MODULE_MAP},
    {"gearbox:map", "province_is_land",       "(i)i",    (void*)map_province_is_land,       MODULE_MAP},
    {"gearbox:map", "province_neighbor_count","(i)i",    (void*)map_province_neighbor_count,MODULE_MAP},
    {"gearbox:map", "province_neighbor_at",   "(ii)i",   (void*)map_province_neighbor_at,   MODULE_MAP},

    {"gearbox:storage", "get",    "(iiii)i", (void*)storage_get,    MODULE_STORAGE},
    {"gearbox:storage", "set",    "(iiii)i", (void*)storage_set,    MODULE_STORAGE},
    {"gearbox:storage", "remove", "(ii)i",   (void*)storage_remove, MODULE_STORAGE},

    {"gearbox:audio", "play",       "(iif)i", (void*)audio_play,       MODULE_AUDIO},
    {"gearbox:audio", "stop",       "(i)",    (void*)audio_stop,       MODULE_AUDIO},
    {"gearbox:audio", "set_volume", "(if)",   (void*)audio_set_volume, MODULE_AUDIO},
    {"gearbox:audio", "is_playing", "(i)i",   (void*)audio_is_playing, MODULE_AUDIO},

    {"gearbox:net", "send",       "(iii)i", (void*)net_send,       MODULE_NET},
    {"gearbox:net", "recv",       "(iii)i", (void*)net_recv,       MODULE_NET},
    {"gearbox:net", "peer_count", "()i",    (void*)net_peer_count, MODULE_NET},
    {"gearbox:net", "self_peer",  "()i",    (void*)net_self_peer,  MODULE_NET},
    {"gearbox:net", "is_host",    "()i",    (void*)net_is_host,    MODULE_NET},

    {"gearbox:assets", "size", "(ii)i",   (void*)assets_size, MODULE_ASSETS},
    {"gearbox:assets", "read", "(iiii)i", (void*)assets_read, MODULE_ASSETS},

    // ── Gearbox 1.1 ──────────────────────────────────────────────────────────
    // Appended, never interleaved with the v1.0 rows above. modAbiMinorFor()
    // reads the module name to decide which minor a symbol arrived in, and a
    // v1.0 mod resolves only the rows that existed then.

    {"gearbox:ui", "draw_line",        "(iiiiiFi)",  (void*)ui_draw_line,        MODULE_UI},
    {"gearbox:ui", "draw_circle",      "(iiiFi)",    (void*)ui_draw_circle,      MODULE_UI},
    {"gearbox:ui", "draw_image",       "(iiiiiiii)", (void*)ui_draw_image,       MODULE_UI},
    {"gearbox:ui", "draw_text_sized",  "(iiiiiii)",  (void*)ui_draw_text_sized,  MODULE_UI},
    {"gearbox:ui", "measure_text",     "(iii)i",     (void*)ui_measure_text,     MODULE_UI},
    {"gearbox:ui", "panel_width",      "(i)i",       (void*)ui_panel_width,      MODULE_UI},
    {"gearbox:ui", "panel_height",     "(i)i",       (void*)ui_panel_height,     MODULE_UI},
    {"gearbox:ui", "panel_set_visible","(ii)",       (void*)ui_panel_set_visible,MODULE_UI},
    {"gearbox:ui", "mouse_x",          "(i)F",       (void*)ui_mouse_x,          MODULE_UI},
    {"gearbox:ui", "mouse_y",          "(i)F",       (void*)ui_mouse_y,          MODULE_UI},
    {"gearbox:ui", "mouse_inside",     "(i)i",       (void*)ui_mouse_inside,     MODULE_UI},
    {"gearbox:ui", "theme_accent",     "()i",        (void*)ui_theme_accent,     MODULE_UI},
    {"gearbox:ui", "set_theme_accent", "(i)i",       (void*)ui_set_theme_accent, MODULE_UI},

    {"gearbox:military.read", "ship_count",             "()i",     (void*)mil_ship_count,             MODULE_MILITARY_READ},
    {"gearbox:military.read", "ship_at",                "(i)i",    (void*)mil_ship_at,                MODULE_MILITARY_READ},
    {"gearbox:military.read", "ship_exists",            "(i)i",    (void*)mil_ship_exists,            MODULE_MILITARY_READ},
    {"gearbox:military.read", "ship_owner",             "(i)i",    (void*)mil_ship_owner,             MODULE_MILITARY_READ},
    {"gearbox:military.read", "ship_type",              "(iii)i",  (void*)mil_ship_type,              MODULE_MILITARY_READ},
    {"gearbox:military.read", "ship_lon",               "(i)F",    (void*)mil_ship_lon,               MODULE_MILITARY_READ},
    {"gearbox:military.read", "ship_lat",               "(i)F",    (void*)mil_ship_lat,               MODULE_MILITARY_READ},
    {"gearbox:military.read", "ship_health",            "(i)i",    (void*)mil_ship_health,            MODULE_MILITARY_READ},
    {"gearbox:military.read", "ship_crew",              "(i)i",    (void*)mil_ship_crew,              MODULE_MILITARY_READ},
    {"gearbox:military.read", "ship_range",             "(i)F",    (void*)mil_ship_range,             MODULE_MILITARY_READ},
    {"gearbox:military.read", "army_stack_count",       "(i)i",    (void*)mil_army_stack_count,       MODULE_MILITARY_READ},
    {"gearbox:military.read", "army_stack_owner",       "(ii)i",   (void*)mil_army_stack_owner,       MODULE_MILITARY_READ},
    {"gearbox:military.read", "army_stack_size",        "(ii)I",   (void*)mil_army_stack_size,        MODULE_MILITARY_READ},
    {"gearbox:military.read", "country_army",           "(i)I",    (void*)mil_country_army,           MODULE_MILITARY_READ},
    {"gearbox:military.read", "province_fortification",  "(i)i",   (void*)mil_province_fortification, MODULE_MILITARY_READ},
    {"gearbox:military.read", "province_port_level",     "(i)i",   (void*)mil_province_port_level,    MODULE_MILITARY_READ},

    {"gearbox:military.write", "order_army_move",    "(iii)i",  (void*)milw_order_army_move,    MODULE_MILITARY_WRITE},
    {"gearbox:military.write", "order_ship_move",    "(iFF)i",  (void*)milw_order_ship_move,    MODULE_MILITARY_WRITE},
    {"gearbox:military.write", "order_ship_engage",  "(ii)i",   (void*)milw_order_ship_engage,  MODULE_MILITARY_WRITE},
    {"gearbox:military.write", "order_ship_bombard", "(iiii)i", (void*)milw_order_ship_bombard, MODULE_MILITARY_WRITE},

    {"gearbox:research.read", "node_count",           "()i",     (void*)res_node_count,           MODULE_RESEARCH_READ},
    {"gearbox:research.read", "node_id",              "(iii)i",  (void*)res_node_id,              MODULE_RESEARCH_READ},
    {"gearbox:research.read", "node_name",            "(iii)i",  (void*)res_node_name,            MODULE_RESEARCH_READ},
    {"gearbox:research.read", "node_category",        "(iii)i",  (void*)res_node_category,        MODULE_RESEARCH_READ},
    {"gearbox:research.read", "node_cost",            "(i)i",    (void*)res_node_cost,            MODULE_RESEARCH_READ},
    {"gearbox:research.read", "country_has_researched","(iii)i", (void*)res_country_has_researched,MODULE_RESEARCH_READ},
    {"gearbox:research.read", "country_funding",      "(i)F",    (void*)res_country_funding,      MODULE_RESEARCH_READ},

    {"gearbox:research.write", "set_country_funding", "(iF)i",   (void*)resw_set_country_funding, MODULE_RESEARCH_WRITE},

    {"gearbox:politics.read", "country_compass_econ",   "(i)F",    (void*)pol_country_compass_econ,   MODULE_POLITICS_READ},
    {"gearbox:politics.read", "country_compass_social", "(i)F",    (void*)pol_country_compass_social, MODULE_POLITICS_READ},
    {"gearbox:politics.read", "province_unrest",        "(i)F",    (void*)pol_province_unrest,        MODULE_POLITICS_READ},
    {"gearbox:politics.read", "policy_count",           "()i",     (void*)pol_policy_count,           MODULE_POLITICS_READ},
    {"gearbox:politics.read", "policy_id",              "(iii)i",  (void*)pol_policy_id,              MODULE_POLITICS_READ},
    {"gearbox:politics.read", "policy_name",            "(iii)i",  (void*)pol_policy_name,            MODULE_POLITICS_READ},
    {"gearbox:politics.read", "country_has_policy",     "(iii)i",  (void*)pol_country_has_policy,     MODULE_POLITICS_READ},
    {"gearbox:politics.read", "province_minority_count","(i)i",    (void*)pol_province_minority_count, MODULE_POLITICS_READ},
    {"gearbox:politics.read", "province_minority_name", "(iiii)i", (void*)pol_province_minority_name, MODULE_POLITICS_READ},
    {"gearbox:politics.read", "province_minority_share","(ii)F",   (void*)pol_province_minority_share,MODULE_POLITICS_READ},

    {"gearbox:politics.write", "set_country_policy", "(iiii)i", (void*)polw_set_country_policy, MODULE_POLITICS_WRITE},

    {"gearbox:economy.read", "country_income_gross",  "(i)F",   (void*)eco_country_income_gross,  MODULE_ECONOMY_READ},
    {"gearbox:economy.read", "country_income_net",    "(i)F",   (void*)eco_country_income_net,    MODULE_ECONOMY_READ},
    {"gearbox:economy.read", "country_army_upkeep",   "(i)F",   (void*)eco_country_army_upkeep,   MODULE_ECONOMY_READ},
    {"gearbox:economy.read", "country_navy_upkeep",   "(i)F",   (void*)eco_country_navy_upkeep,   MODULE_ECONOMY_READ},
    {"gearbox:economy.read", "country_is_bankrupt",   "(i)i",   (void*)eco_country_is_bankrupt,   MODULE_ECONOMY_READ},
    {"gearbox:economy.read", "province_industry_level","(i)i",  (void*)eco_province_industry_level,MODULE_ECONOMY_READ},
    {"gearbox:economy.read", "province_industry_specialization","(iii)i",(void*)eco_province_industry_specialization,MODULE_ECONOMY_READ},
    {"gearbox:economy.read", "province_resource",     "(iii)F", (void*)eco_province_resource,     MODULE_ECONOMY_READ},

    {"gearbox:economy.write", "set_province_industry_level", "(ii)i", (void*)ecow_set_province_industry_level, MODULE_ECONOMY_WRITE},

    {"gearbox:map", "province_is_coastal", "(i)i",     (void*)map_province_is_coastal, MODULE_MAP},
    {"gearbox:map", "sea_route_exists",    "(FFFF)i",  (void*)map_sea_route_exists,    MODULE_MAP},
    {"gearbox:map", "point_is_land",       "(FF)i",    (void*)map_point_is_land,       MODULE_MAP},

    {"gearbox:mapeditor", "editor_active",                   "()i",      (void*)ed_active,                   MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_province_count",           "()i",      (void*)ed_province_count,           MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_province_at",              "(i)i",     (void*)ed_province_at,              MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_province_population",      "(i)I",     (void*)ed_province_population,      MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_province_industry_level",  "(i)i",     (void*)ed_province_industry_level,  MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_province_fortification",   "(i)i",     (void*)ed_province_fortification,   MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_province_port_level",      "(i)i",     (void*)ed_province_port_level,      MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_province_resource",        "(iii)F",   (void*)ed_province_resource,        MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_province_compass_econ",    "(i)F",     (void*)ed_province_compass_econ,    MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_province_compass_social",  "(i)F",     (void*)ed_province_compass_social,  MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_set_province_population",  "(iI)i",    (void*)ed_set_province_population,  MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_set_province_industry_level","(ii)i",  (void*)ed_set_province_industry_level, MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_set_province_fortification","(ii)i",   (void*)ed_set_province_fortification,MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_set_province_port_level",  "(ii)i",    (void*)ed_set_province_port_level,  MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_set_province_resource",    "(iiiF)i",  (void*)ed_set_province_resource,    MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_set_province_compass",     "(iFF)i",   (void*)ed_set_province_compass,     MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_map_name",                 "(ii)i",    (void*)ed_map_name,                 MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_set_map_name",             "(ii)i",    (void*)ed_set_map_name,             MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_set_author",               "(ii)i",    (void*)ed_set_author,               MODULE_MAPEDITOR},
    {"gearbox:mapeditor", "editor_set_license",              "(ii)i",    (void*)ed_set_license,              MODULE_MAPEDITOR},

    {"gearbox:net", "peer_at",           "(i)i",   (void*)net_peer_at,           MODULE_NET},
    {"gearbox:net", "peer_name",         "(iii)i", (void*)net_peer_name,         MODULE_NET},
    {"gearbox:net", "max_message_bytes", "()i",    (void*)net_max_message_bytes, MODULE_NET},

    {"gearbox:neural", "module_count",  "()i",      (void*)neural_module_count,  MODULE_NEURAL},
    {"gearbox:neural", "module_name",   "(iii)i",   (void*)neural_module_name,   MODULE_NEURAL},
    {"gearbox:neural", "action_count",  "(i)i",     (void*)neural_action_count,  MODULE_NEURAL},
    {"gearbox:neural", "action_name",   "(iiii)i",  (void*)neural_action_name,   MODULE_NEURAL},
    {"gearbox:neural", "country_is_ai", "(i)i",     (void*)neural_country_is_ai, MODULE_NEURAL},
    {"gearbox:neural", "update_count",  "()I",      (void*)neural_update_count,  MODULE_NEURAL},
    {"gearbox:neural", "model_loaded",  "()i",      (void*)neural_model_loaded,  MODULE_NEURAL},

    // Not a gearbox: namespace -- these must carry the names the interpreters
    // actually import. Gated on WasiStub like any other capability.
    {"wasi_snapshot_preview1", "fd_write",            "(iiii)i",     (void*)wasi_fd_write,       MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "proc_exit",           "(i)",         (void*)wasi_proc_exit,      MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "random_get",          "(ii)i",       (void*)wasi_random_get,     MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "clock_time_get",      "(iIi)i",      (void*)wasi_clock_time_get, MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "environ_sizes_get",   "(ii)i",       (void*)wasi_zero_sizes,     MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "environ_get",         "(ii)i",       (void*)wasi_zero_get,       MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "args_sizes_get",      "(ii)i",       (void*)wasi_zero_sizes,     MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "args_get",            "(ii)i",       (void*)wasi_zero_get,       MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_close",            "(i)i",        (void*)wasi_denied1,        MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_fdstat_get",       "(ii)i",       (void*)wasi_fd_fdstat_get,  MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_prestat_get",      "(ii)i",       (void*)wasi_prestat_none2,  MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_prestat_dir_name", "(iii)i",      (void*)wasi_prestat_none3,  MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_read",             "(iiii)i",     (void*)wasi_denied2,        MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_seek",             "(iIii)i",     (void*)wasi_fd_seek,        MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "path_open",           "(iiiiiIIii)i",(void*)wasi_path_open,      MODULE_WASISTUB},

    // Everything below exists so an interpreter can instantiate. See the note
    // above wasi_clock_res_get: a wasm module must resolve every import it
    // declares, so refusing at the call is the only way to refuse at all.
    {"wasi_snapshot_preview1", "clock_res_get",          "(ii)i",     (void*)wasi_clock_res_get,      MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "sched_yield",            "()i",       (void*)wasi_sched_yield,        MODULE_WASISTUB},

    {"wasi_snapshot_preview1", "fd_advise",              "(iIIi)i",   (void*)wasi_fd_denied_iIIi,     MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_allocate",            "(iII)i",    (void*)wasi_fd_denied_iII,      MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_datasync",            "(i)i",      (void*)wasi_fd_denied_i,        MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_sync",                "(i)i",      (void*)wasi_fd_denied_i,        MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_fdstat_set_flags",    "(ii)i",     (void*)wasi_fd_denied_ii,       MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_filestat_get",        "(ii)i",     (void*)wasi_fd_filestat_get_std,MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_tell",                "(ii)i",     (void*)wasi_fd_denied_ii,       MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_renumber",            "(ii)i",     (void*)wasi_fd_denied_ii,       MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_filestat_set_size",   "(iI)i",     (void*)wasi_fd_denied_iI,       MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_filestat_set_times",  "(iIIi)i",   (void*)wasi_fd_denied_iIIi,     MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_pread",               "(iiiIi)i",  (void*)wasi_fd_denied_iiiIi,    MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_pwrite",              "(iiiIi)i",  (void*)wasi_fd_denied_iiiIi,    MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "fd_readdir",             "(iiiIi)i",  (void*)wasi_fd_denied_iiiIi,    MODULE_WASISTUB},

    {"wasi_snapshot_preview1", "path_create_directory",  "(iii)i",    (void*)wasi_denied3,            MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "path_remove_directory",  "(iii)i",    (void*)wasi_denied3,            MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "path_unlink_file",       "(iii)i",    (void*)wasi_denied3,            MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "path_filestat_get",      "(iiiii)i",  (void*)wasi_denied5,            MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "path_symlink",           "(iiiii)i",  (void*)wasi_denied5,            MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "path_readlink",          "(iiiiii)i", (void*)wasi_denied6,            MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "path_rename",            "(iiiiii)i", (void*)wasi_denied6,            MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "path_link",              "(iiiiiii)i",(void*)wasi_denied7,            MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "path_filestat_set_times","(iiiiIIi)i",(void*)wasi_path_denied_iiiiIIi,MODULE_WASISTUB},

    {"wasi_snapshot_preview1", "poll_oneoff",            "(iiii)i",   (void*)wasi_denied4,            MODULE_WASISTUB},

    // No network. A mod that wants to talk to the internet is exactly what this
    // sandbox exists to prevent, so these refuse unconditionally.
    {"wasi_snapshot_preview1", "sock_accept",            "(iii)i",    (void*)wasi_denied3,            MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "sock_recv",              "(iiiiii)i", (void*)wasi_denied6,            MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "sock_send",              "(iiiii)i",  (void*)wasi_denied5,            MODULE_WASISTUB},
    {"wasi_snapshot_preview1", "sock_shutdown",          "(ii)i",     (void*)wasi_fd_denied_ii,       MODULE_WASISTUB},
};

}  // namespace

void modSetNetBridge(const ModNetBridge& bridge) { g_netBridge = bridge; }
void modSetUiBridge(const ModUiBridge& bridge) { g_uiBridge = bridge; }
void modSetAudioBridge(const ModAudioBridge& bridge) { g_audioBridge = bridge; }

void modReleaseAudio(const std::string& modId) {
    if (g_audioBridge.stopAll) g_audioBridge.stopAll(modId);
}


const ModHostFn* modHostFunctions(size_t& count) {
    count = sizeof(kHostFunctions) / sizeof(kHostFunctions[0]);
    return kHostFunctions;
}

ModUI& ModUI::get() {
    static ModUI ui;
    return ui;
}

// ------------------------------------------------------------ Conflicts ----

ModConflicts& ModConflicts::get() {
    static ModConflicts c;
    return c;
}

void ModConflicts::beginTurn(int turn) {
    m_turn = turn;
    // Writes only conflict within a turn. Two mods that each adjust a treasury
    // on alternating turns are taking turns, not fighting, and the log would
    // fill with noise if that counted.
    m_thisTurn.clear();
}

void ModConflicts::recordWrite(const std::string& modId,
                               const std::string& target,
                               const std::string& value) {
    auto& latest = m_thisTurn[target];

    for (const auto& other : latest) {
        if (other.first == modId) continue;     // a mod may overwrite itself
        if (other.second == value) continue;    // agreement is not a conflict

        // Same pair, same target: count it rather than adding a duplicate, so a
        // mod writing every frame produces one finding and not thousands.
        bool merged = false;
        for (auto& c : m_clashes) {
            bool samePair = (c.modA == other.first && c.modB == modId) ||
                            (c.modA == modId && c.modB == other.first);
            if (samePair && c.target == target) {
                c.seen++;
                c.turn = m_turn;
                c.valueA = other.second;
                c.valueB = value;
                merged = true;
                break;
            }
        }
        if (!merged) {
            if (m_clashes.size() >= kMaxClashes) m_clashes.erase(m_clashes.begin());
            Clash c;
            c.modA = other.first; c.modB = modId;
            c.target = target;
            c.valueA = other.second; c.valueB = value;
            c.turn = m_turn;
            m_clashes.push_back(std::move(c));
            pushLog(modId, 2, "conflicts with " + other.first + " over " + target +
                              " (" + other.second + " vs " + value + ")");
        }
    }

    latest[modId] = value;
}

bool ModConflicts::anyFor(const std::string& modId) const {
    for (const auto& c : m_clashes)
        if (c.modA == modId || c.modB == modId) return true;
    return false;
}

void ModConflicts::forget(const std::string& modId) {
    for (auto it = m_clashes.begin(); it != m_clashes.end();)
        it = (it->modA == modId || it->modB == modId) ? m_clashes.erase(it) : it + 1;
    for (auto& e : m_thisTurn) e.second.erase(modId);
}

void ModConflicts::clear() {
    m_thisTurn.clear();
    m_clashes.clear();
}

// -------------------------------------------------------------- Storage ----

ModStorage& ModStorage::get() {
    static ModStorage s;
    return s;
}

namespace {

// A mod id is reverse-DNS and validated on load, but it becomes a filename
// here, so anything that is not plainly safe is replaced. Belt and braces: the
// manifest validator already rejects a hostile id, and this makes the file
// layout incapable of expressing one regardless.
std::string storageFileName(const std::string& modId) {
    std::string s;
    s.reserve(modId.size());
    for (char c : modId) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        s += ok ? c : '_';
    }
    if (s.empty()) s = "unnamed";
    if (s.size() > 128) s.resize(128);
    return s + ".kv";
}

// A tiny length-prefixed format rather than JSON: values are arbitrary bytes,
// including NULs and invalid UTF-8, and a text format would have to escape
// them. Four ASCII digits of version, then repeating
// <key_len> ' ' <val_len> '\n' <key><value>.
void encodeStore(const std::map<std::string, std::string>& kv, std::string& out) {
    out = "GBXKV1\n";
    for (const auto& e : kv) {
        out += std::to_string(e.first.size());
        out += ' ';
        out += std::to_string(e.second.size());
        out += '\n';
        out += e.first;
        out += e.second;
    }
}

bool decodeStore(const std::string& raw, std::map<std::string, std::string>& kv,
                 size_t& bytes) {
    kv.clear();
    bytes = 0;
    if (raw.rfind("GBXKV1\n", 0) != 0) return false;
    size_t i = 7;
    while (i < raw.size()) {
        size_t sp = raw.find(' ', i);
        if (sp == std::string::npos) return false;
        size_t nl = raw.find('\n', sp);
        if (nl == std::string::npos) return false;

        size_t klen = 0, vlen = 0;
        try {
            klen = (size_t)std::stoul(raw.substr(i, sp - i));
            vlen = (size_t)std::stoul(raw.substr(sp + 1, nl - sp - 1));
        } catch (...) { return false; }

        size_t at = nl + 1;
        if (klen > raw.size() - at) return false;
        if (vlen > raw.size() - at - klen) return false;

        std::string k = raw.substr(at, klen);
        std::string v = raw.substr(at + klen, vlen);
        bytes += k.size() + v.size();
        kv.emplace(std::move(k), std::move(v));
        i = at + klen + vlen;
    }
    return true;
}

}  // namespace

void ModStorage::setDir(const std::string& dir) {
    m_dir = dir;
    if (!m_dir.empty() && m_dir.back() != '/') m_dir += '/';
}

std::string ModStorage::pathFor(const std::string& modId) const {
    return m_dir + storageFileName(modId);
}

ModStorage::Store& ModStorage::storeFor(const std::string& modId) {
    Store& st = m_stores[modId];
    if (st.loaded) return st;
    st.loaded = true;
    if (m_dir.empty()) return st;          // no directory set: memory only

    FILE* f = fopen(pathFor(modId).c_str(), "rb");
    if (!f) return st;                     // no file yet is not an error
    std::string raw;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        raw.append(buf, n);
        if (raw.size() > kMaxTotalBytes * 4) break;   // refuse a silly file
    }
    fclose(f);

    if (!decodeStore(raw, st.kv, st.bytes)) {
        // A corrupt store is dropped rather than guessed at. Losing a mod's
        // settings is recoverable; handing it half-parsed values is not.
        st.kv.clear();
        st.bytes = 0;
        pushLog(modId, 2, "storage was unreadable and has been reset");
    }
    return st;
}

bool ModStorage::get(const std::string& modId, const std::string& key,
                     std::string& out) {
    Store& st = storeFor(modId);
    auto it = st.kv.find(key);
    if (it == st.kv.end()) return false;
    out = it->second;
    return true;
}

bool ModStorage::set(const std::string& modId, const std::string& key,
                     const std::string& value, std::string& err) {
    if (key.empty())                   { err = "empty key"; return false; }
    if (key.size() > kMaxKeyBytes)     { err = "key too long"; return false; }
    if (value.size() > kMaxValueBytes) { err = "value too large"; return false; }

    Store& st = storeFor(modId);
    auto it = st.kv.find(key);
    size_t was = (it == st.kv.end()) ? 0 : it->second.size() + key.size();
    size_t now = key.size() + value.size();

    if (it == st.kv.end() && st.kv.size() >= kMaxKeys) {
        err = "too many keys"; return false;
    }
    if (st.bytes - was + now > kMaxTotalBytes) {
        err = "storage quota exceeded"; return false;
    }

    st.bytes = st.bytes - was + now;
    st.kv[key] = value;
    st.dirty = true;
    return true;
}

bool ModStorage::remove(const std::string& modId, const std::string& key) {
    Store& st = storeFor(modId);
    auto it = st.kv.find(key);
    if (it == st.kv.end()) return false;
    st.bytes -= it->first.size() + it->second.size();
    st.kv.erase(it);
    st.dirty = true;
    return true;
}

void ModStorage::flush() {
    if (m_dir.empty()) return;
    for (auto& e : m_stores) {
        Store& st = e.second;
        if (!st.dirty) continue;
        std::string blob;
        encodeStore(st.kv, blob);
        // Written whole, then renamed, so an interrupted write cannot leave a
        // half-file that the next load would have to reject.
        const std::string finalPath = pathFor(e.first);
        const std::string tmpPath = finalPath + ".tmp";
        if (FILE* f = fopen(tmpPath.c_str(), "wb")) {
            bool ok = blob.empty() ||
                      fwrite(blob.data(), 1, blob.size(), f) == blob.size();
            fclose(f);
            if (ok && rename(tmpPath.c_str(), finalPath.c_str()) == 0) {
                st.dirty = false;
            } else {
                // Qualified: unqualified `remove` would find this class's own
                // member function, not the one that deletes a file.
                ::remove(tmpPath.c_str());
                pushLog(e.first, 3, "could not write storage");
            }
        } else {
            pushLog(e.first, 3, "could not open storage for writing");
        }
    }
}

void ModStorage::forget(const std::string& modId) {
    auto it = m_stores.find(modId);
    if (it == m_stores.end()) return;
    m_stores.erase(it);
}

void ModStorage::clear() { m_stores.clear(); }

uint32_t ModUI::registerPanel(const std::string& ownerId, const std::string& title,
                              uint32_t minW, uint32_t minH) {
    size_t owned = 0;
    for (const auto& p : m_panels) if (p.ownerId == ownerId) owned++;
    if (owned >= kMaxPanelsPerMod) return 0;

    ModPanel p;
    p.id = m_nextId++;
    p.ownerId = ownerId;
    p.title = title;
    p.minW = minW ? minW : 160;
    p.minH = minH ? minH : 80;
    m_panels.push_back(std::move(p));
    return m_panels.back().id;
}

ModPanel* ModUI::find(uint32_t id) {
    if (id == 0) return nullptr;
    for (auto& p : m_panels) if (p.id == id) return &p;
    return nullptr;
}

void ModUI::clearCommands() {
    for (auto& p : m_panels) p.cmds.clear();
}

void ModUI::removePanelsOf(const std::string& modId) {
    for (size_t i = m_panels.size(); i-- > 0;)
        if (m_panels[i].ownerId == modId) m_panels.erase(m_panels.begin() + (long)i);
}

void ModUI::clear() {
    m_panels.clear();
    m_nextId = 1;
}

const std::vector<ModLogLine>& modHostLog() { return g_log; }
void modHostLogClear() { g_log.clear(); }
