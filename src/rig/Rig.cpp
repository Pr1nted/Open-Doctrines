#include "Rig.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace rig {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg2Rad = kPi / 180.0f;

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string w;
    while (ss >> w) out.push_back(w);
    return out;
}

// Deterministic noise. NOT rand(): a character must move the same way on every
// machine, in a recorded timelapse and in a multiplayer session, and none of
// this may draw from the simulation's random stream. Same rule as the dialogue
// box's per-glyph jitter, for the same reasons.
//
// A HASH IS NOT NOISE, and this was a hash.
//
// `fract(sin(t) * 43758.5453)` is the standard shader hash: it is designed to
// turn nearby inputs into UNRELATED outputs, which is exactly what you want for
// a per-glyph seed and exactly what you must not have for a value that varies
// over time. Fed continuous time it returns white noise at frame rate -- the
// sway layer alone jumped up to 2.5 degrees between consecutive frames, and the
// character visibly shook, always.
//
// So: hash at INTEGER steps, and interpolate between them. That is what makes
// it noise rather than a random number generator with a clock attached. The
// smoothstep matters too -- a linear blend between hashed values is continuous
// but its derivative is not, which reads as a faint tick at every step.
float hashStep(int salt, int i) {
    const float a = std::sin(i * 12.9898f + salt * 78.2330f) * 43758.5453f;
    return (a - std::floor(a)) * 2.0f - 1.0f;      // [-1,1]
}

float noise1(int salt, float t) {
    const float f = std::floor(t);
    const float u = t - f;
    const float s = u * u * (3.0f - 2.0f * u);     // smoothstep
    const int i = (int)f;
    return hashStep(salt, i) * (1.0f - s) + hashStep(salt, i + 1) * s;
}

// HOW HARD IT IS BLOWING, RIGHT NOW.
//
// Wind does not hold a number. It freshens and drops over ten seconds or so,
// with faster turbulence riding on top of that, and a constant `speed` reads
// as a fan rather than as weather -- the coat reaches an angle and stays at
// it.
//
// TWO TIMESCALES, because one will not do: a single noise slow enough to be
// weather never gusts, and one fast enough to gust never settles into a lull.
//
// AND IT IS NOT SYMMETRIC ABOUT ITS MEAN, which is the part that matters most
// and is the easiest to leave out. Air surges; it does not stop. Real wind
// spends most of its time a little under its average and occasionally shoves
// hard through it, so the positive half of the noise is pushed up and the
// negative half is eased off. A gust that is exactly as deep as it is tall is
// a sine with extra steps.
//
// Clamped at zero: a lull is still air, never air blowing backwards.
float windStrength(const Wind& w, float t) {
    const float slow = noise1(101, t * 0.09f);     // freshening and dropping
    const float turb = noise1(103, t * 0.47f);     // turbulence on top of it
    float g = 0.65f * slow + 0.35f * turb;
    g = (g > 0.0f) ? g * (1.0f + g) : g * 0.55f;   // surge up, sag gently
    return std::max(0.0f, 1.0f + w.gust * g);
}

Ease easeFromName(const std::string& n) {
    if (n == "linear")   return Ease::Linear;
    if (n == "outquad")  return Ease::OutQuad;
    if (n == "outback")  return Ease::OutBack;
    // Anything unrecognised, and the unstated case, gets the one that suits a
    // body. See Pose::ease.
    return Ease::InOutQuad;
}

}  // namespace

float applyEase(Ease e, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (e) {
        case Ease::Linear:    return t;
        case Ease::OutQuad:   return 1.0f - (1.0f - t) * (1.0f - t);
        case Ease::InOutQuad: return t < 0.5f ? 2.0f * t * t
                                              : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
        case Ease::OutBack: {
            // A little overshoot. A limb that arrives dead-on reads as
            // mechanical; one that overshoots by a hair reads as weight.
            const float c = 1.70158f;
            const float u = t - 1.0f;
            return 1.0f + (c + 1.0f) * u * u * u + c * u * u;
        }
    }
    return t;
}

float shortestArc(float a, float b) {
    float d = std::fmod(b - a, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

int Skeleton::indexOf(const std::string& name) const {
    for (size_t i = 0; i < bones.size(); ++i)
        if (bones[i].name == name) return (int)i;
    return -1;
}

// ─── Parsing ──────────────────────────────────────────────────────────────

Skeleton parse(const std::string& source) {
    Skeleton sk;
    Pose* cur = nullptr;
    PoseStep* curStep = nullptr;   // the keyframe of `cur` being filled in
    int lineNo = 0;

    auto warn = [&](const std::string& what) {
        sk.warnings.push_back("line " + std::to_string(lineNo) + ": " + what);
    };

    // key/value pairs after the first two words: "len 42 angle -90 breath 1"
    auto kv = [](const std::vector<std::string>& w, size_t from,
                 const char* key, float def) {
        for (size_t i = from; i + 1 < w.size(); ++i)
            if (w[i] == key) return (float)atof(w[i + 1].c_str());
        return def;
    };
    // Was the key there at all? `mouth 0` is a flat mouth and means something;
    // no `mouth` at all means the pose is not talking about the mouth, and it
    // should go back to rest. kv() cannot tell those two apart.
    auto kvOpt = [](const std::vector<std::string>& w, size_t from,
                    const char* key, float& out) {
        for (size_t i = from; i + 1 < w.size(); ++i)
            if (w[i] == key) { out = (float)atof(w[i + 1].c_str()); return true; }
        return false;
    };
    auto kvs = [](const std::vector<std::string>& w, size_t from,
                  const char* key, const std::string& def) {
        for (size_t i = from; i + 1 < w.size(); ++i)
            if (w[i] == key) return w[i + 1];
        return def;
    };

    // ONE reader for a pose's bone line. There are two places that parse one --
    // the ordinary case, and the "a known bone name wins over a keyword" case
    // -- and having two copies is how the mouth came to work in poses and not
    // in keyframes for as long as it took to notice.
    auto posedBone = [&](const std::vector<std::string>& w) {
        PoseBone pb;
        if (w.size() >= 2) {
            const bool numeric = (isdigit((unsigned char)w[1][0]) ||
                                  w[1][0] == '-' || w[1][0] == '+' || w[1][0] == '.');
            if (numeric) { pb.hasAngle = true; pb.angle = (float)atof(w[1].c_str()); }
            else if (w[1] != "mouth" && w[1] != "jaw") {
                pb.hasHand = true;
                pb.handClass = w[1];
                pb.facing = std::clamp(kv(w, 2, "facing", 0.0f), -1.0f, 1.0f);
                if (w.size() >= 4 && w[2] == "angle") {
                    pb.hasAngle = true; pb.angle = (float)atof(w[3].c_str());
                }
            }
        }
        // Heads: `head 8 mouth -0.7 jaw 0.3`, or `head mouth -0.7` on its own
        // when the pose has nothing to say about where the head is pointing.
        //
        // `jaw` AND NOT `open`, which is what it was called for about ten
        // minutes: `open` is already a hand class, so `handL open facing 1.0`
        // parsed its own class name as a mouth key, read "facing" as a number,
        // got zero, and quietly stopped being an open hand. The hand tests
        // caught it. A key in a flat namespace has to be unique across every
        // line the namespace serves, not just the ones it was written for.
        float v = 0.0f;
        if (kvOpt(w, 1, "mouth", v)) { pb.hasMouth = true; pb.mouth = std::clamp(v, -1.0f, 1.0f); }
        if (kvOpt(w, 1, "jaw", v))   { pb.hasOpen = true;  pb.open = std::clamp(v, 0.0f, 1.0f); }
        return pb;
    };

    std::istringstream in(source);
    std::string raw;
    while (std::getline(in, raw)) {
        lineNo++;
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line.compare(0, 2, "//") == 0) continue;

        const std::vector<std::string> w = words(line);
        const std::string& head = w[0];

        // INSIDE A POSE, A KNOWN BONE NAME WINS OVER A KEYWORD.
        //
        // "head" is both a declaration keyword and the obvious name for the
        // bone a head hangs on, and every character will have one. Written
        // inside a pose -- "head 8" -- it was parsed as the START OF A NEW
        // DECLARATION, which silently ended the pose block: the four bones
        // after it in the test rig became "don't understand 'armL.up'" and that
        // pose quietly lost both arms.
        //
        // A declaration always names a parent (only the root does not, and a
        // rig has one root); a pose entry never does. That is the test.
        if (cur && sk.indexOf(w[0]) >= 0 &&
            !(w.size() >= 2 && w[1] == "parent")) {
            PoseBone pb = posedBone(w);
            if (curStep) curStep->bones[w[0]] = pb;
            else          cur->bones[w[0]] = pb;
            continue;
        }

        if (head == "bone" || head == "hand" || head == "head") {
            if (w.size() < 2) { warn(head + " needs a name"); continue; }
            cur = nullptr;
            curStep = nullptr;
            Bone b;
            b.name = w[1];
            b.kind = (head == "hand") ? Bone::Kind::Hand
                   : (head == "head") ? Bone::Kind::Head : Bone::Kind::Bone;
            const std::string parent = kvs(w, 2, "parent", "");
            if (!parent.empty()) {
                b.parent = sk.indexOf(parent);
                // Parents must already exist. Solving walks the array once in
                // order, so a forward reference would read an unsolved parent
                // and put the limb somewhere arbitrary -- silently.
                if (b.parent < 0) warn("bone '" + b.name + "' has unknown parent '" + parent + "'");
            }
            b.length    = kv(w, 2, "len", 0.0f);
            b.restAngle = kv(w, 2, "angle", 0.0f);
            b.size      = kv(w, 2, "size", 0.0f);
            b.atlas     = kvs(w, 2, "atlas", "");
            {
                const std::string sd = kvs(w, 2, "side", "");
                const char tail = b.name.empty() ? 0 : b.name.back();
                b.chir = !sd.empty() ? ((sd == "left" || sd == "l") ? -1.0f : 1.0f)
                       : (tail == 'L' || tail == 'l') ? -1.0f : 1.0f;
            }
            b.collide   = std::max(0.0f, kv(w, 2, "collide", 0.0f));
            b.mouth     = std::clamp(kv(w, 2, "mouth", 0.0f), -1.0f, 1.0f);
            b.mouthOpen = std::clamp(kv(w, 2, "jaw", 0.0f), 0.0f, 1.0f);
            b.breath    = kv(w, 2, "breath", 0.0f);
            b.sway      = kv(w, 2, "sway", 0.0f);
            b.gaze      = kv(w, 2, "gaze", 0.0f);
            sk.bones.push_back(b);
            continue;
        }

        // ── look: how the character is DRAWN, as opposed to how it moves ──
        //
        // One line, the same key/value shape as everything else here. Colours
        // are #rrggbb because that is what an artist reads off a picture; the
        // parser is deliberately forgiving about the leading hash.
        if (head == "look") {
            cur = nullptr;
            Look& lk = sk.look;
            lk.drawn = true;
            auto col = [&](const char* key, Rgb def) {
                const std::string v = kvs(w, 1, key, "");
                if (v.empty()) return def;
                const std::string h = (v[0] == '#') ? v.substr(1) : v;
                if (h.size() != 6) { warn(std::string(key) + " wants #rrggbb, got '" + v + "'"); return def; }
                const long n = strtol(h.c_str(), nullptr, 16);
                return Rgb{(unsigned char)((n >> 16) & 0xff),
                           (unsigned char)((n >> 8) & 0xff),
                           (unsigned char)(n & 0xff)};
            };
            lk.coat  = col("coat",  lk.coat);
            lk.lapel = col("lapel", lk.lapel);
            lk.shirt = col("shirt", lk.shirt);
            lk.skin  = col("skin",  lk.skin);
            lk.hat   = col("hat",   lk.hat);
            lk.band  = col("band",  lk.band);
            lk.line  = col("line",  lk.line);
            lk.hasHat = kv(w, 1, "wearshat", lk.hasHat ? 1.0f : 0.0f) > 0.5f;
            lk.brim   = kv(w, 1, "brim", lk.brim);
            lk.ink    = kv(w, 1, "ink", lk.ink);
            continue;
        }

        // ── part: a drawn image pinned to a bone ──
        if (head == "part") {
            cur = nullptr;
            if (w.size() < 2) { warn("part needs a bone"); continue; }
            Part pt;
            pt.bone = w[1];
            if (sk.indexOf(pt.bone) < 0)
                warn("part is pinned to unknown bone '" + pt.bone + "'");
            pt.file = kvs(w, 2, "file", "");
            if (pt.file.empty()) { warn("part '" + pt.bone + "' has no file"); continue; }
            const std::string pv = kvs(w, 2, "pivot", "0.5,0.5");
            const size_t comma = pv.find(',');
            if (comma == std::string::npos) warn("pivot wants u,v -- got '" + pv + "'");
            else {
                pt.pivotU = (float)atof(pv.substr(0, comma).c_str());
                pt.pivotV = (float)atof(pv.substr(comma + 1).c_str());
            }
            pt.turn = kv(w, 2, "turn", 0.0f);
            pt.unit = std::max(0.01f, kv(w, 2, "unit", 4.0f));
            sk.parts.push_back(pt);
            continue;
        }

        if (head == "cloth") {
            cur = nullptr;
            curStep = nullptr;
            if (w.size() < 2) { warn("cloth needs a name"); continue; }
            cur = nullptr;
            Cloth c;
            c.name = w[1];
            const std::string parent = kvs(w, 2, "parent", "");
            c.parent = sk.indexOf(parent);
            if (c.parent < 0) warn("cloth '" + c.name + "' hangs off unknown bone '" + parent + "'");
            c.segments  = std::max(1, (int)kv(w, 2, "segs", 4));
            c.segLen    = kv(w, 2, "len", 10.0f);
            c.stiffness = std::clamp(kv(w, 2, "stiff", 0.7f), 0.0f, 1.0f);
            c.damping   = std::clamp(kv(w, 2, "damp", 0.92f), 0.0f, 1.0f);
            c.gravity   = kv(w, 2, "gravity", 420.0f);
            // 0 is a chain carried bodily along by its bone; 1 is one left
            // entirely behind. Past 2 the chain is thrown backwards further
            // than the bone travelled forwards and the relaxation pass hauls
            // it straight back the same frame, so it stops reading as more
            // whip and starts reading as a twitch. See Cloth::drag.
            c.drag      = std::clamp(kv(w, 2, "drag", 1.0f), 0.0f, 2.0f);
            c.windCatch = kv(w, 2, "wind", 1.0f);
            c.pair      = kvs(w, 2, "pair", "");
            c.pairWidth = std::max(0.0f, kv(w, 2, "width", 0.0f));
            sk.cloth.push_back(c);
            continue;
        }

        // A duration, an ease and an optional `hold`, in any order -- shared by
        // `pose` and `key` so the two lines cannot drift apart.
        auto timing = [&](const std::vector<std::string>& tk, size_t from,
                          float& seconds, Ease& ease, float& hold) {
            for (size_t i = from; i < tk.size(); ++i) {
                if (tk[i] == "then") { if (i + 1 < tk.size()) ++i; continue; }
                if (tk[i] == "hold") {
                    if (i + 1 < tk.size()) hold = std::max(0.0f, (float)atof(tk[++i].c_str()));
                    continue;
                }
                if (!tk[i].empty() && tk[i].back() == 's' &&
                    (isdigit((unsigned char)tk[i][0]) || tk[i][0] == '.')) {
                    seconds = std::max(0.0f, (float)atof(tk[i].c_str()));
                } else {
                    std::string e = tk[i];
                    std::transform(e.begin(), e.end(), e.begin(),
                                   [](unsigned char c) { return (char)tolower(c); });
                    ease = easeFromName(e);
                }
            }
        };

        if (head == "pose") {
            if (w.size() < 2) { warn("pose needs a name"); continue; }
            Pose p;
            p.name = w[1];
            p.next = kvs(w, 2, "then", "");
            timing(w, 2, p.seconds, p.ease, p.hold);
            sk.poses[p.name] = p;
            cur = &sk.poses[p.name];
            curStep = nullptr;
            continue;
        }

        // A further keyframe of the pose being defined. See PoseStep.
        if (head == "key") {
            if (!cur) { warn("'key' outside a pose"); continue; }
            PoseStep st;
            timing(w, 1, st.seconds, st.ease, st.hold);
            cur->more.push_back(st);
            curStep = &cur->more.back();
            continue;
        }

        // Inside a pose: "<bone> <angle>" or "<hand> <class> facing <f>"
        if (cur) {
            if (sk.indexOf(w[0]) < 0) {
                // The failure this catches is a limb that quietly stops being
                // posed because somebody renamed a bone.
                warn("pose '" + cur->name + "' names unknown bone '" + w[0] + "'");
                continue;
            }
            PoseBone pb = posedBone(w);
            if (curStep) curStep->bones[w[0]] = pb;
            else          cur->bones[w[0]] = pb;
            continue;
        }

        warn("don't understand '" + head + "'");
    }
    return sk;
}

// ─── Runtime ──────────────────────────────────────────────────────────────

void Rig::setSkeleton(Skeleton s) {
    m_skel = std::move(s);
    const size_t n = m_skel.bones.size();
    m_base.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) m_base[i] = m_skel.bones[i].restAngle;
    m_angle = m_base;
    m_from = m_to = m_base;
    m_handClass.assign(n, "");
    m_fromHandClass = m_toHandClass = m_handClass;
    m_facing.assign(n, 0.0f);
    m_fromFacing = m_toFacing = m_facing;
    // The mouth starts where the character rests, not at zero: a face whose
    // resting expression is a frown should be frowning on the first frame.
    m_mouth.assign(n, 0.0f);
    m_open.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        m_mouth[i] = m_skel.bones[i].mouth;
        m_open[i] = m_skel.bones[i].mouthOpen;
    }
    m_fromMouth = m_toMouth = m_mouth;
    m_fromOpen = m_toOpen = m_open;
    m_blend = 1.0f;
    m_solved.assign(n, Solved{});

    m_cloth.clear();
    m_clothPrev.clear();
    for (const Cloth& c : m_skel.cloth) {
        m_cloth.emplace_back(c.segments + 1);
        m_clothPrev.emplace_back(c.segments + 1);
    }
    solve();
    // Settle the cloth where it hangs rather than letting it fall into place on
    // the first frame the character is visible.
    for (size_t ci = 0; ci < m_cloth.size(); ++ci) {
        const Cloth& c = m_skel.cloth[ci];
        if (c.parent < 0 || c.parent >= (int)m_solved.size()) continue;
        Vec2 p = m_solved[c.parent].b;
        for (size_t k = 0; k < m_cloth[ci].size(); ++k) {
            m_cloth[ci][k] = p;
            m_clothPrev[ci][k] = p;
            p.y += c.segLen;
        }
    }
}

bool Rig::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    Skeleton sk = parse(ss.str());
    // Part files are named relative to the .odrig, so a character folder can be
    // moved or copied whole and its art goes with it.
    const size_t slash = path.find_last_of("/\\");
    sk.dir = (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
    setSkeleton(std::move(sk));
    return !m_skel.bones.empty();
}

void Rig::commitTarget() {
    m_base = m_to;
    m_angle = m_base;
    m_handClass = m_toHandClass;
    m_facing = m_toFacing;
    m_mouth = m_toMouth;
    m_open = m_toOpen;
    solve();
}

void Rig::beginTarget(const std::unordered_map<std::string, PoseBone>& bones,
                      float seconds, Ease ease, bool unnamedToRest) {
    // FROM WHERE IT IS, not from where it was going. Interrupting a transition
    // half way must bend out of the current shape; starting again from the
    // previous pose is a visible snap backwards.
    //
    // From the POSE, not from what is drawn: m_base excludes breath and sway,
    // so a transition is the same every time rather than depending on which
    // part of the breathing cycle the page turned on.
    m_from = m_base;
    m_fromHandClass = m_handClass;
    m_fromFacing = m_facing;
    m_fromMouth = m_mouth;
    m_fromOpen = m_open;

    m_to = m_base;
    m_toHandClass = m_handClass;
    m_toFacing = m_facing;
    m_toMouth = m_mouth;
    m_toOpen = m_open;
    for (size_t i = 0; i < m_skel.bones.size(); ++i) {
        // A POSE that says nothing about a bone returns it to REST, or poses
        // silently compose and the same pose looks different depending on what
        // came before it. A KEYFRAME leaves it alone -- it is a step inside one
        // gesture, and a shrug's second beat must not drop the arms it never
        // mentions. See PoseStep.
        if (unnamedToRest) {
            m_to[i] = m_skel.bones[i].restAngle;
            m_toMouth[i] = m_skel.bones[i].mouth;
            m_toOpen[i] = m_skel.bones[i].mouthOpen;
        }
        auto pb = bones.find(m_skel.bones[i].name);
        if (pb == bones.end()) continue;
        if (pb->second.hasAngle) m_to[i] = pb->second.angle;
        if (pb->second.hasHand) {
            m_toHandClass[i] = pb->second.handClass;
            m_toFacing[i] = pb->second.facing;
        }
        if (pb->second.hasMouth) m_toMouth[i] = pb->second.mouth;
        if (pb->second.hasOpen)  m_toOpen[i] = pb->second.open;
    }

    m_blendDur = std::max(0.0001f, seconds);
    m_ease = ease;
    m_blend = (seconds <= 0.0f) ? 1.0f : 0.0f;
    if (m_blend >= 1.0f) commitTarget();
}

bool Rig::setPose(const std::string& name, bool immediate) {
    if (!playPose(name, immediate)) return false;
    // The REQUEST, kept apart from what is playing: see pose().
    m_poseName = name;
    return true;
}

bool Rig::playPose(const std::string& name, bool immediate) {
    auto it = m_skel.poses.find(name);
    if (it == m_skel.poses.end()) return false;
    const Pose& p = it->second;

    m_activePose = name;
    m_next = p.next;
    m_more = p.more;
    m_stepIx = 0;
    m_holdLeft = p.hold;

    // A NEW PERSON DOES NOT EASE OUT OF THE LAST PERSON'S STANCE. When the
    // speaker changes, the body that appears is already standing the way it
    // stands; interpolating into it from a rest pose is the previous character
    // melting into this one.
    //
    // For a pose with keyframes, "already standing that way" is the END of the
    // gesture and not the start of it -- so an immediate set walks the whole
    // sequence at zero duration rather than freezing on the first beat, which
    // would leave a character permanently mid-shrug.
    if (immediate) {
        beginTarget(p.bones, 0.0f, p.ease, true);
        for (const PoseStep& st : m_more) beginTarget(st.bones, 0.0f, st.ease, false);
        m_stepIx = m_more.size();
        m_holdLeft = 0.0f;
        return true;
    }

    beginTarget(p.bones, p.seconds, p.ease, true);
    return true;
}

void Rig::setPlacement(const Placement& p, float seconds) {
    m_placeFrom = m_place;
    m_placeTo = p;
    m_placeDur = std::max(0.0f, seconds);
    if (m_placeDur <= 0.0f) { m_place = p; m_placeBlend = 1.0f; }
    else m_placeBlend = 0.0f;
    // A flip is not a thing you can be half of, so it lands at the start of the
    // move rather than at the end: the body turns and then travels, which is
    // the order a person does it in.
    m_place.flip = p.flip;
}

void Rig::update(float dt) {
    m_time += dt;

    if (m_placeBlend < 1.0f) {
        m_placeBlend = std::min(1.0f, m_placeBlend + dt / std::max(0.0001f, m_placeDur));
        const float t = applyEase(Ease::InOutQuad, m_placeBlend);
        m_place.at.x = m_placeFrom.at.x + (m_placeTo.at.x - m_placeFrom.at.x) * t;
        m_place.at.y = m_placeFrom.at.y + (m_placeTo.at.y - m_placeFrom.at.y) * t;
        m_place.scale = m_placeFrom.scale + (m_placeTo.scale - m_placeFrom.scale) * t;
    }

    if (m_blend < 1.0f) {
        m_blend = std::min(1.0f, m_blend + dt / m_blendDur);
    } else if (m_holdLeft > 0.0f) {
        // A beat of stillness at the top of the gesture. A shrug with no hold
        // is a shoulder twitch: the shoulders have to be seen UP before they
        // are allowed to come down, and that is a wait rather than a slower
        // ease -- an ease would still be moving the whole time.
        m_holdLeft -= dt;
    } else if (m_stepIx < m_more.size()) {
        const PoseStep& st = m_more[m_stepIx++];
        m_holdLeft = st.hold;
        beginTarget(st.bones, st.seconds, st.ease, false);
    } else if (!m_next.empty()) {
        // The gesture is over; go back to being a person standing there. Taken
        // before the clear so a fallback that itself has a `then` cannot loop.
        const std::string n = m_next;
        m_next.clear();
        playPose(n, false);
    }
    const float t = applyEase(m_ease, m_blend);

    for (size_t i = 0; i < m_base.size(); ++i) {
        // Shortest arc, so a limb swinging from -170 to 170 goes the short way
        // round instead of sweeping through the whole body.
        m_base[i] = m_from[i] + shortestArc(m_from[i], m_to[i]) * t;
        m_angle[i] = m_base[i];
        m_facing[i] = m_fromFacing[i] + (m_toFacing[i] - m_fromFacing[i]) * t;
        // BLENDED, never switched. A mouth is one shape being pulled about,
        // unlike a hand, which is a different drawing at each pose class -- so
        // where the hand swaps at the midpoint, this eases the whole way.
        m_mouth[i] = m_fromMouth[i] + (m_toMouth[i] - m_fromMouth[i]) * t;
        m_open[i] = m_fromOpen[i] + (m_toOpen[i] - m_fromOpen[i]) * t;
        // THE DRAWING SWITCHES AT THE MIDDLE OF THE MOVE.
        //
        // See the hand note in Rig.h: the swap is masked by the wrist being in
        // motion, and the wrist is moving fastest around the midpoint. Swapping
        // at t=0 or t=1 would put the pop on a stationary hand, which is the
        // one place the eye is guaranteed to catch it.
        m_handClass[i] = (t < 0.5f) ? m_fromHandClass[i] : m_toHandClass[i];
    }

    if (m_idle) {
        // ── Gaze: a target held for a while, then a new one ──
        // Not a sine. Looking around is saccade-and-dwell; a smooth sweep reads
        // as a security camera.
        m_gazeHold -= dt;
        if (m_gazeHold <= 0.0f) {
            const float n1 = noise1(11, m_time * 0.31f);
            const float n2 = noise1(29, m_time * 0.27f);
            m_gazeTarget = {std::clamp(n1 * 1.2f, -1.0f, 1.0f),
                            std::clamp(n2 * 0.6f, -1.0f, 1.0f)};
            m_gazeHold = 1.4f + std::fabs(noise1(37, m_time)) * 2.6f;
        }
        const float k = std::min(1.0f, dt * 6.0f);
        m_gaze.x += (m_gazeTarget.x - m_gaze.x) * k;
        m_gaze.y += (m_gazeTarget.y - m_gaze.y) * k;

        // ── Blink: discrete, with a refractory period ──
        m_blinkNext -= dt;
        if (m_blinkNext <= 0.0f && m_blinkPhase <= 0.0f) {
            m_blinkPhase = 0.13f;
            // Occasionally two in quick succession, which is what people do.
            m_blinkNext = (noise1(53, m_time) > 0.72f)
                        ? 0.22f
                        : 2.2f + std::fabs(noise1(59, m_time)) * 3.4f;
        }
        if (m_blinkPhase > 0.0f) {
            m_blinkPhase -= dt;
            const float u = std::clamp(m_blinkPhase / 0.13f, 0.0f, 1.0f);
            m_blink = std::sin(u * kPi);      // shut and open again
        } else {
            m_blink = 0.0f;
        }

        // ── Breath and sway, additive on the pose ──
        const float breath = std::sin(m_time * 1.55f);            // ~0.25 Hz
        const float sway   = noise1(7, m_time * 0.11f);           // much slower
        for (size_t i = 0; i < m_angle.size(); ++i) {
            const Bone& b = m_skel.bones[i];
            if (b.breath != 0.0f) m_angle[i] += breath * 1.6f * b.breath;
            if (b.sway   != 0.0f) m_angle[i] += sway   * 2.2f * b.sway;
            if (b.gaze   != 0.0f) m_angle[i] += m_gaze.x * 7.0f * b.gaze;
        }
    }

    solve();
    stepCloth(dt);
}

void Rig::solve() {
    const size_t n = m_skel.bones.size();
    if (m_solved.size() != n) m_solved.assign(n, Solved{});
    for (size_t i = 0; i < n; ++i) {
        const Bone& b = m_skel.bones[i];
        Solved s;
        float parentAngle = 0.0f;
        Vec2  origin{0, 0};
        if (b.parent >= 0 && b.parent < (int)i) {
            parentAngle = m_solved[b.parent].worldAngle;
            origin = m_solved[b.parent].b;
        }
        s.worldAngle = parentAngle + m_angle[i];
        s.a = origin;
        const float r = s.worldAngle * kDeg2Rad;
        s.b = {origin.x + std::cos(r) * b.length, origin.y + std::sin(r) * b.length};
        s.handClass = m_handClass[i];
        s.facing = m_facing[i];
        s.mouth = m_mouth[i];
        s.mouthOpen = m_open[i];
        m_solved[i] = s;
    }
}

void Rig::stepCloth(float dt) {
    // Verlet, because it is three lines and never explodes: the position IS the
    // state, so a constraint that moves a point also removes the energy that
    // took it there. A spring solver here would need tuning per garment.
    if (dt <= 0.0f) return;
    dt = std::min(dt, 1.0f / 30.0f);   // a dropped frame must not fling a coat

    for (size_t ci = 0; ci < m_cloth.size(); ++ci) {
        const Cloth& c = m_skel.cloth[ci];
        if (c.parent < 0 || c.parent >= (int)m_solved.size()) continue;
        auto& pts = m_cloth[ci];
        auto& prev = m_clothPrev[ci];
        if (pts.empty()) continue;

        // The first point is pinned to the bone: the coat moves because the
        // shoulder moved, which is the entire point of hanging it off a bone.
        //
        // AND THE REST OF THE CHAIN IS CARRIED SOME OF THE WAY WITH IT.
        //
        // Pinning the head and leaving the rest where it was is one specific
        // fabric -- the one with enough mass to stay put while the body walks
        // out from under it. Everything lighter than that comes along, partly.
        // `drag` is how much it does NOT: 0 carries the whole chain by the
        // bone's full step, 1 carries none of it, and above 1 the chain is
        // carried BACKWARDS, so it trails further than standing still would
        // leave it and the hem cracks on a fast gesture.
        //
        // Both the point and its previous position move, which is the whole
        // trick: verlet keeps velocity as the gap between the two, so shifting
        // the pair changes where the cloth IS without touching how fast it is
        // going. A coat carried across the stage arrives hanging rather than
        // swinging, and the wind -- which is an acceleration further down --
        // adds on top of this instead of fighting it.
        const Vec2 anchor = m_solved[c.parent].b;
        const Vec2 carry{(anchor.x - pts[0].x) * (1.0f - c.drag),
                         (anchor.y - pts[0].y) * (1.0f - c.drag)};
        if (carry.x != 0.0f || carry.y != 0.0f) {
            for (size_t k = 1; k < pts.size(); ++k) {
                pts[k].x  += carry.x;  pts[k].y  += carry.y;
                prev[k].x += carry.x;  prev[k].y += carry.y;
            }
        }
        pts[0] = anchor;
        prev[0] = pts[0];

        // WIND IS CAUGHT BROADSIDE, NOT PUSHED ALONG.
        //
        // The whole difference between cloth in wind and cloth leaning over is
        // that a hem edge-on to the moving air catches nothing while the same
        // hem held across it catches all of it. Take only the component of the
        // wind perpendicular to each segment and the coat lifts, spills the
        // air as it turns into it, falls, and lifts again -- flapping, with no
        // oscillator anywhere in the code. Push the whole vector at it instead
        // and it simply hangs at an angle, which is a coat in a hurricane on a
        // stage with no air.
        //
        // The gust travels DOWN the chain: the phase is offset per segment, so
        // a lull reaches the hem after it has reached the shoulder, the way a
        // real one arrives as a wave rather than all at once.
        //
        // AND IT IS THE SCENE'S WIND, ON A RIG THAT MAY BE DRAWN MIRRORED.
        //
        // Everything in here is rig-local pixels, and `flip` mirrors the whole
        // body at draw time -- so a scene direction used as-is blows the right
        // way on a character facing one way and the wrong way on the same
        // character facing the other. Two of them talking in one wind had
        // their coats streaming apart. Un-mirror it ONCE, here, and every line
        // below stays in one space and never has to ask which way anybody is
        // standing.
        //
        // Only x, because the mirror is a negated x at draw time and nothing
        // else: gravity and the wind's vertical part are already right.
        const float mirror = m_place.flip ? -1.0f : 1.0f;
        const float wlen = std::sqrt(m_wind.dir.x * m_wind.dir.x +
                                     m_wind.dir.y * m_wind.dir.y);
        const bool  windy = m_wind.speed > 0.0f && wlen > 1e-5f && c.windCatch > 0.0f;
        const Vec2  wdir = windy
            ? Vec2{m_wind.dir.x / wlen * mirror, m_wind.dir.y / wlen}
            : Vec2{0.0f, 0.0f};

        for (size_t k = 1; k < pts.size(); ++k) {
            const Vec2 cur = pts[k];
            Vec2 vel{(cur.x - prev[k].x) * c.damping, (cur.y - prev[k].y) * c.damping};
            Vec2 acc{0.0f, c.gravity};
            if (windy) {
                // ONE WEATHER, ARRIVING AS A WAVE.
                //
                // Each segment feels the scene's wind as it was a moment ago,
                // so a gust reaches the hem after it has reached the shoulder.
                // That is a lag on a shared strength, not a noise per chain:
                // the old version salted the noise with the chain index, which
                // meant the two panels of one coat drew independent gusts and
                // disagreed with each other about the weather -- the exact
                // thing Wind exists to stop. Same air for everything standing
                // in it, reaching each part of it when it gets there.
                const float mag = m_wind.speed * c.windCatch *
                                  windStrength(m_wind,
                                               m_wind.time - (float)k * 0.035f);
                Vec2 w{wdir.x * mag, wdir.y * mag};
                const Vec2 seg{cur.x - pts[k - 1].x, cur.y - pts[k - 1].y};
                const float sl = std::sqrt(seg.x * seg.x + seg.y * seg.y);
                if (sl > 1e-5f) {
                    const Vec2 u{seg.x / sl, seg.y / sl};
                    const float along = w.x * u.x + w.y * u.y;
                    w = {w.x - u.x * along, w.y - u.y * along};
                }
                acc.x += w.x;
                acc.y += w.y;
            }
            pts[k] = {cur.x + vel.x + acc.x * dt * dt, cur.y + vel.y + acc.y * dt * dt};
            prev[k] = cur;
        }
    }

    // ── ONE SOLVE FOR EVERYTHING, not one per chain ──
    //
    // Length, garment width and the body all constrain the same points, so
    // they have to be relaxed TOGETHER. Solving each chain to convergence and
    // then pulling the panels together afterwards leaves the width correction
    // unopposed by the lengths: the next frame's length pass takes it back out
    // by sliding cloth up the chain, and over a second or two the coat crawls
    // up the body into a bib. Which is exactly what it did.
    //
    // Interleaved, each correction is answered by the others in the same
    // iteration and the whole garment settles into a shape all three agree on.
    int iterations = 1;
    for (const Cloth& c : m_skel.cloth)
        iterations = std::max(iterations, 1 + (int)(c.stiffness * 5.0f));

    for (int it = 0; it < iterations; ++it) {
        // Segment lengths. The parent end is immovable and the free end takes
        // the whole correction: that is what makes it hang rather than shrug.
        for (size_t ci = 0; ci < m_cloth.size(); ++ci) {
            const Cloth& c = m_skel.cloth[ci];
            auto& pts = m_cloth[ci];
            for (size_t k = 1; k < pts.size(); ++k) {
                const Vec2 d{pts[k].x - pts[k - 1].x, pts[k].y - pts[k - 1].y};
                const float len = std::sqrt(d.x * d.x + d.y * d.y);
                if (len < 1e-5f) continue;
                const float corr = (len - c.segLen) / len;
                pts[k].x -= d.x * corr;
                pts[k].y -= d.y * corr;
            }
        }

        // THE TWO PANELS ARE ONE GARMENT. Joined below the armhole, and joined
        // NARROWER than the shoulders are wide, so the cloth comes in under the
        // arms and swings out again over the hips -- the shape a coat has, made
        // by the simulation rather than faked in the drawing.
        //
        // A maximum, not a spring: the panels may come closer together freely,
        // they simply cannot get further apart than they are sewn. From k=1,
        // because point 0 is pinned to a shoulder and the shoulders are the one
        // place a coat IS as wide as the body.
        for (size_t ci = 0; ci < m_skel.cloth.size(); ++ci) {
            const Cloth& c = m_skel.cloth[ci];
            if (c.pair.empty() || c.pairWidth <= 0.0f) continue;
            int other = -1;
            for (size_t cj = 0; cj < m_skel.cloth.size(); ++cj)
                if (cj != ci && m_skel.cloth[cj].name == c.pair) other = (int)cj;
            if (other < 0 || (size_t)other < ci) continue;   // once per pair
            auto& A = m_cloth[ci];
            auto& B = m_cloth[other];
            // THE ARMHOLE HAS DEPTH. Clamping to the sewn width one segment
            // below the shoulder puts a fold across the chest and the coat
            // reads as a bib or a cape -- a garment is cut full at the chest
            // and comes in at the waist, over a hand's width of cloth, not
            // instantly. So the limit eases from however far apart the
            // shoulders actually are down to the sewn width over the first
            // few segments.
            const float sep0 = std::sqrt((B[0].x - A[0].x) * (B[0].x - A[0].x) +
                                         (B[0].y - A[0].y) * (B[0].y - A[0].y));
            const float armhole = 3.0f;
            for (size_t k = 1; k < A.size() && k < B.size(); ++k) {
                const float t = std::min(1.0f, (float)k / armhole);
                const float limit = sep0 + (c.pairWidth - sep0) * t;
                Vec2 d{B[k].x - A[k].x, B[k].y - A[k].y};
                const float len = std::sqrt(d.x * d.x + d.y * d.y);
                if (len <= limit || len < 1e-5f) continue;
                const float corr = (len - limit) / len * 0.5f;
                A[k].x += d.x * corr;  A[k].y += d.y * corr;
                B[k].x -= d.x * corr;  B[k].y -= d.y * corr;
            }
        }

        // ── AND THE BODY IS IN THE WAY ──
        //
        // The one thing that separates a coat from a curtain. Every bone with a
        // `collide` radius is a capsule the cloth cannot enter, so the panels
        // ride over the hips instead of through them, and a leg swinging
        // forward carries the cloth in front of it along with it.
        //
        // Only pts moves and not prev, deliberately: the gap between the two IS
        // the velocity, so a body that shoves cloth out of its way also throws
        // it, which is what being shoved does.
        for (size_t ci = 0; ci < m_cloth.size(); ++ci) {
            auto& pts = m_cloth[ci];
            for (size_t k = 1; k < pts.size(); ++k) {
                for (size_t bi = 0; bi < m_skel.bones.size() && bi < m_solved.size(); ++bi) {
                    const float rad = m_skel.bones[bi].collide;
                    if (rad <= 0.0f) continue;
                    const Vec2 a = m_solved[bi].a, b = m_solved[bi].b;
                    const Vec2 ab{b.x - a.x, b.y - a.y};
                    const float ll = ab.x * ab.x + ab.y * ab.y;
                    float t = 0.0f;
                    if (ll > 1e-6f) {
                        t = ((pts[k].x - a.x) * ab.x + (pts[k].y - a.y) * ab.y) / ll;
                        t = std::clamp(t, 0.0f, 1.0f);
                    }
                    const Vec2 on{a.x + ab.x * t, a.y + ab.y * t};
                    Vec2 d{pts[k].x - on.x, pts[k].y - on.y};
                    float dist = std::sqrt(d.x * d.x + d.y * d.y);
                    if (dist >= rad) continue;
                    // Dead centre: shove it out sideways rather than leaving it
                    // stuck on a division by zero.
                    if (dist < 1e-4f) { d = {1.0f, 0.0f}; dist = 1.0f; }
                    pts[k].x = on.x + d.x / dist * rad;
                    pts[k].y = on.y + d.y / dist * rad;
                }
            }
        }
    }
}


}  // namespace rig
