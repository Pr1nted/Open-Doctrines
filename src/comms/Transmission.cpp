#include "Transmission.h"

#include <algorithm>
#include <cmath>

#include "raymath.h"
#include "rlgl.h"

// THE NAME PLATE CAN SAY "Мія", so it cannot draw with raylib's built-in font.
//
// Everything in this window used the default font, which carries ASCII and
// nothing else. That was invisible while the only things it drew were
// "Pr1nted" and "tired developer" -- both Latin -- and became five tofu boxes
// the moment the speaker's name and role started going through the translation
// table. This is the same shadowing every other drawing file in the game uses;
// see i18n/Text.h.
#include "i18n/Text.h"

namespace comms {

// ── The filter ────────────────────────────────────────────────────────
//
// EVERY COLOUR ON SCREEN COMES OUT OF HERE. The signal buffer holds
// luminance and nothing else, and this maps it through a ramp built from
// the player's accent: near-black in the shadows, the accent through the
// midtones, and blown out towards white at the top. So art is authored once,
// in grey, and every accent the player can pick is free -- there is no
// palette to maintain per character, and no recolouring by hand, ever.
//
// The rest is what a bad link does to a picture: a phosphor grille, a
// scanline, bloom that bleeds the highlights, a slow roll bar, per-line tear
// when the quality drops, noise, vignette and flicker.

static const char* kFragmentDesktop = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;

uniform sampler2D texture0;
uniform vec2  uResolution;
uniform float uTime;
uniform vec3  uAccent;
uniform float uQuality;
uniform float uGain;
uniform float uStatic;

float hash(vec2 p) { return fract(sin(dot(p, vec2(41.3, 289.1))) * 43758.5453); }

void main() {
    vec2 uv = fragTexCoord;
    float bad = 1.0 - uQuality;

    // Per-line horizontal tear, plus the occasional whole band letting go.
    float line   = floor(uv.y * uResolution.y / 3.0);
    float jitter = (hash(vec2(line, floor(uTime * 24.0))) - 0.5) * 0.05 * bad * bad;
    float tear   = step(0.988 - bad * 0.08, hash(vec2(floor(uTime * 9.0), line * 0.11)));
    uv.x += jitter + tear * (hash(vec2(line, uTime)) - 0.5) * 0.22 * bad;

    float l = texture(texture0, uv).r;
    // Static: the picture dissolves into noise as the link is lost, and
    // resolves out of it as one is found. Done here rather than by fading the
    // whole window, because a portrait that simply fades reads as a UI
    // animation, and one that has to be tuned in reads as a transmission.
    l = mix(l, hash(uv * uResolution + uTime * 97.0), uStatic);

    // Bloom: the highlights bleed, which is most of why a CRT reads as a CRT.
    float b = 0.25 * (texture(texture0, uv + vec2( 0.005, 0.0)).r
                    + texture(texture0, uv + vec2(-0.005, 0.0)).r
                    + texture(texture0, uv + vec2(0.0,  0.007)).r
                    + texture(texture0, uv + vec2(0.0, -0.007)).r);
    // Only genuinely bright neighbours bleed. Taking max() against the raw
    // average instead floods every dark line that sits next to something
    // bright -- which erased the rim of the eye and the lid bar entirely.
    l = min(1.0, l + 0.55 * max(0.0, b - 0.62));

    vec3 shadow = uAccent * 0.05;
    vec3 mid    = uAccent;
    vec3 hot    = mix(uAccent, vec3(1.0), 0.85);
    vec3 col    = (l < 0.55) ? mix(shadow, mid, l / 0.55)
                             : mix(mid, hot, (l - 0.55) / 0.45);

    // Aperture grille: three columns of phosphor, in the accent's own hue.
    float phase = mod(uv.x * uResolution.x, 3.0);
    vec3 grille = vec3(phase < 1.0 ? 1.18 : 0.70,
                       (phase >= 1.0 && phase < 2.0) ? 1.18 : 0.70,
                       phase >= 2.0 ? 1.18 : 0.70);
    col *= mix(vec3(1.0), grille, 0.42);

    col *= 0.60 + 0.40 * abs(sin(uv.y * uResolution.y * 1.5708));

    float roll = fract(uv.y + uTime * 0.08);
    col *= 1.0 + 0.10 * smoothstep(0.0, 0.05, roll) * (1.0 - smoothstep(0.05, 0.13, roll));

    col += (hash(uv * uResolution + uTime * 60.0) - 0.5) * (0.04 + 0.20 * bad);

    vec2 d = uv - 0.5;
    col *= 1.0 - 0.95 * dot(d, d);
    col *= uGain * (0.95 + 0.05 * hash(vec2(floor(uTime * 30.0), 1.0)));

    finalColor = vec4(max(col, vec3(0.0)), 1.0) * fragColor;
}
)";

// Same filter for GLES2, which the browser and Android builds get.
static const char* kFragmentES = R"(#version 100
precision mediump float;
varying vec2 fragTexCoord;
varying vec4 fragColor;

uniform sampler2D texture0;
uniform vec2  uResolution;
uniform float uTime;
uniform vec3  uAccent;
uniform float uQuality;
uniform float uGain;
uniform float uStatic;

float hash(vec2 p) { return fract(sin(dot(p, vec2(41.3, 289.1))) * 43758.5453); }

void main() {
    vec2 uv = fragTexCoord;
    float bad = 1.0 - uQuality;

    float line   = floor(uv.y * uResolution.y / 3.0);
    float jitter = (hash(vec2(line, floor(uTime * 24.0))) - 0.5) * 0.05 * bad * bad;
    float tear   = step(0.988 - bad * 0.08, hash(vec2(floor(uTime * 9.0), line * 0.11)));
    uv.x += jitter + tear * (hash(vec2(line, uTime)) - 0.5) * 0.22 * bad;

    float l = texture2D(texture0, uv).r;
    // Static: the picture dissolves into noise as the link is lost, and
    // resolves out of it as one is found. Done here rather than by fading the
    // whole window, because a portrait that simply fades reads as a UI
    // animation, and one that has to be tuned in reads as a transmission.
    l = mix(l, hash(uv * uResolution + uTime * 97.0), uStatic);
    float b = 0.25 * (texture2D(texture0, uv + vec2( 0.005, 0.0)).r
                    + texture2D(texture0, uv + vec2(-0.005, 0.0)).r
                    + texture2D(texture0, uv + vec2(0.0,  0.007)).r
                    + texture2D(texture0, uv + vec2(0.0, -0.007)).r);
    // Only genuinely bright neighbours bleed. Taking max() against the raw
    // average instead floods every dark line that sits next to something
    // bright -- which erased the rim of the eye and the lid bar entirely.
    l = min(1.0, l + 0.55 * max(0.0, b - 0.62));

    vec3 shadow = uAccent * 0.05;
    vec3 mid    = uAccent;
    vec3 hot    = mix(uAccent, vec3(1.0), 0.85);
    vec3 col    = (l < 0.55) ? mix(shadow, mid, l / 0.55)
                             : mix(mid, hot, (l - 0.55) / 0.45);

    float phase = mod(uv.x * uResolution.x, 3.0);
    vec3 grille = vec3(phase < 1.0 ? 1.18 : 0.70,
                       (phase >= 1.0 && phase < 2.0) ? 1.18 : 0.70,
                       phase >= 2.0 ? 1.18 : 0.70);
    col *= mix(vec3(1.0), grille, 0.42);

    col *= 0.60 + 0.40 * abs(sin(uv.y * uResolution.y * 1.5708));

    float roll = fract(uv.y + uTime * 0.08);
    col *= 1.0 + 0.10 * smoothstep(0.0, 0.05, roll) * (1.0 - smoothstep(0.05, 0.13, roll));

    col += (hash(uv * uResolution + uTime * 60.0) - 0.5) * (0.04 + 0.20 * bad);

    vec2 d = uv - 0.5;
    col *= 1.0 - 0.95 * dot(d, d);
    col *= uGain * (0.95 + 0.05 * hash(vec2(floor(uTime * 30.0), 1.0)));

    gl_FragColor = vec4(max(col, vec3(0.0)), 1.0) * fragColor;
}
)";

// ── lifecycle ─────────────────────────────────────────────────────────

bool Transmission::open(int w, int h) {
    close();
    m_target = LoadRenderTexture(w, h);
    if (m_target.id == 0) return false;
    SetTextureFilter(m_target.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(m_target.texture, TEXTURE_WRAP_CLAMP);

#if defined(PLATFORM_WEB) || defined(GRAPHICS_API_OPENGL_ES2)
    m_shader = LoadShaderFromMemory(nullptr, kFragmentES);
#else
    m_shader = LoadShaderFromMemory(nullptr, kFragmentDesktop);
#endif
    if (m_shader.id != 0) {
        m_locResolution = GetShaderLocation(m_shader, "uResolution");
        m_locTime = GetShaderLocation(m_shader, "uTime");
        m_locAccent = GetShaderLocation(m_shader, "uAccent");
        m_locQuality = GetShaderLocation(m_shader, "uQuality");
        m_locGain = GetShaderLocation(m_shader, "uGain");
        m_locStatic = GetShaderLocation(m_shader, "uStatic");
    } else {
        // Not fatal. Without the filter the window still shows the speaker,
        // tinted flat -- worse looking, never broken.
        TraceLog(LOG_WARNING, "COMMS: the transmission filter did not compile; drawing unfiltered");
    }
    seed(m_profile.seed);
    return true;
}

void Transmission::close() {
    if (m_signalTex.id != 0) UnloadTexture(m_signalTex);
    m_signalTex = Texture2D{};
    if (m_shader.id != 0) UnloadShader(m_shader);
    if (m_target.id != 0) UnloadRenderTexture(m_target);
    m_shader = Shader{};
    m_target = RenderTexture2D{};
}

void Transmission::setProfile(const Profile& p) {
    if (m_signalTex.id != 0) {
        UnloadTexture(m_signalTex);
        m_signalTex = Texture2D{};
    }
    m_profile = p;
    if (!p.image.empty()) {
        m_signalTex = LoadTexture(p.image.c_str());
        if (m_signalTex.id == 0)
            TraceLog(LOG_WARNING, "COMMS: no signal image at %s; drawing the silhouette instead",
                     p.image.c_str());
        else
            SetTextureFilter(m_signalTex, TEXTURE_FILTER_BILINEAR);

        // Read the skin from the art, beside the eye. The eye's white has to
        // be BRIGHTER than the face it sits in: fixed at 176 it was right for
        // a character whose skin is 113 and wrong for one whose skin is 233,
        // where it made the sclera darker than the cheek and the eye read as
        // a hole with a rim round it.
        Image im = LoadImage(p.image.c_str());
        if (im.data) {
            ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            const Color* px = (const Color*)im.data;
            const float ecx = im.width * (0.5f - p.imgEyeGap * 0.5f);
            const float ecy = im.height * p.imgEyeY;
            const float reach = im.width * p.imgEyeW * 1.5f;
            std::vector<float> lum;
            for (int a = 0; a < 16; a++) {
                const float th = a * 6.2831853f / 16.0f;
                const int sx = (int)(ecx + std::cos(th) * reach);
                const int sy = (int)(ecy + std::sin(th) * reach * 0.8f);
                if (sx < 0 || sy < 0 || sx >= im.width || sy >= im.height) continue;
                const Color c = px[sy * im.width + sx];
                if (c.a < 200) continue;
                const float l = (c.r + c.g + c.b) / 3.0f;
                if (l > 40.0f && l < 250.0f) lum.push_back(l);
            }
            if (!lum.empty()) {
                std::sort(lum.begin(), lum.end());
                m_skin = lum[lum.size() / 2];
            }
            UnloadImage(im);
        }
    }
    seed(p.seed);
}

void Transmission::tuneIn() {
    m_visible = true;
    m_powerTo = 1.0f;      // the tube opens; see powerRect
    m_staticTo = 0.0f;
}

void Transmission::tuneOut() {
    m_staticTo = 1.0f;
}

void Transmission::changeSpeaker(const Profile& p, const std::string& name,
                                 const std::string& role, const std::string& tag) {
    if (!m_visible || m_static > 0.85f) {
        // Nothing on the link yet: no dropout to hide the change behind.
        setProfile(p);
        setSpeaker(name, role, tag);
        tuneIn();
        return;
    }
    // Swap while nothing can be seen: the picture drops to static first, and
    // the new speaker is put in place at the bottom of it.
    m_hasSwap = true;
    m_swap = p;
    m_swapName = name;
    m_swapRole = role;
    m_swapTag = tag;
    m_staticTo = 1.0f;
}

void Transmission::seed(uint32_t s) {
    m_rng = s ? s : 0x9E3779B9u;
    m_blinkNext = 1.0f + (float)(m_rng % 1000) / 500.0f;
}

float Transmission::noise() {
    // xorshift32: deterministic, so a recording of a scene replays the same
    // blinks and the same dropouts on every machine.
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return (float)(m_rng >> 8) * (1.0f / 16777216.0f);
}

// ── direction ─────────────────────────────────────────────────────────

void Transmission::lookAt(Vector2 p) {
    // Screen point -> a direction in [-1, 1], relative to the window we were
    // last drawn in. Without a drawn frame yet, treat it as centre.
    m_gazeHeld = true;
    m_gazeTarget = {std::clamp(p.x, -1.0f, 1.0f), std::clamp(p.y, -1.0f, 1.0f)};
}

void Transmission::lookWander() {
    // Idempotent ON PURPOSE. The caller asks for this every frame the pointer
    // is not over the window, and resetting the hold each time made the eyes
    // choose a new target 60 times a second: they never travelled anywhere,
    // they just vibrated around the middle. Only the FIRST call after being
    // held anywhere counts.
    if (!m_gazeHeld) return;
    m_gazeHeld = false;
    m_gazeHold = 0.0f;
}

void Transmission::speak(int cp) {
    // Vowels open the mouth wide, consonants less, everything else closes it.
    // A mouth on a timer flaps; a mouth on the letters chatters, and the
    // difference is most of what makes typed speech look spoken.
    auto vowel = [](int c) {
        switch (c) {
            case 'a': case 'e': case 'i': case 'o': case 'u': case 'y':
            case 'A': case 'E': case 'I': case 'O': case 'U': case 'Y':
            case 0x430: case 0x435: case 0x438: case 0x456: case 0x457:
            case 0x43E: case 0x443: case 0x454: case 0x44E: case 0x44F:
                return true;
            default: return false;
        }
    };
    const bool letter = (cp > 0x2FF) || (cp >= 'a' && cp <= 'z') ||
                        (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9');
    if (!letter) { m_openTo = 0.0f; m_openHold = 0.0f; return; }
    m_openTo = vowel(cp) ? 0.70f + 0.30f * (float)((cp * 7919) % 11) / 10.0f
                         : 0.24f + 0.22f * (float)((cp * 7919) % 7) / 6.0f;
    m_openHold = vowel(cp) ? 0.055f : 0.030f;
}

void Transmission::blinkSoon() {
    const float remaining = m_blinkNext - m_clock;
    if (remaining > 0.35f) m_blinkNext = m_clock + 0.10f + noise() * 0.25f;
}

void Transmission::debugBlink(float phase) {
    // lidOpen() reads 1 at the ends of the window and 0 in the middle, so the
    // phase is mapped onto the shutting half of it.
    m_blinkT = std::clamp(phase, 0.0f, 1.0f) * m_blinkDur * 0.4f;
    m_blinkNext = m_clock + 1000.0f;      // and no real blink interrupts it
    // and update() must not advance it either. Without this the
    // preview's own settling frames ran the whole blink to its end
    // before the shot was taken, so every phase came out the same
    // and they did not even come out in order -- which read as a
    // broken lid rather than as a broken test.
    m_blinkHold = true;
}

float Transmission::lidOpen() const {
    if (m_blinkT >= m_blinkDur) return 1.0f;
    const float u = m_blinkT / m_blinkDur;
    // Shut fast, open slower: the asymmetry is what makes it read as a blink
    // rather than as a flicker.
    return u < 0.4f ? 1.0f - u / 0.4f : (u - 0.4f) / 0.6f;
}

void Transmission::update(float dt) {
    dt = std::clamp(dt, 0.0f, 0.1f);
    m_clock += dt;

    // The line's weather. Mostly a gentle wander a few percent either side of
    // where the game set it; now and then -- a tenth of the time -- a real
    // dip that takes a couple of seconds to clear, which is what makes the
    // good stretches read as good rather than as normal.
    if (m_clock >= m_driftNext) {
        const float u = noise();
        // The first numbers here were +/-5% with a rare dip, and the picture
        // simply did not change: the filter's own per-frame grain is larger
        // than that, so the wander was hidden inside it. The wander has to be
        // bigger than the noise it is modulating to read as anything at all.
        m_driftTo = (u < 0.22f) ? -(0.26f + noise() * 0.34f)     // a real drop
                                : (noise() - 0.55f) * 0.26f;     // ordinary wander
        m_driftNext = m_clock + 0.6f + noise() * 2.6f;
    }
    // Eased, never stepped: a jump between levels reads as a cut, not as
    // reception changing.
    m_drift += (m_driftTo - m_drift) * (1.0f - std::exp(-dt * 1.7f));
    m_attention += (m_attentionTarget - m_attention) * (1.0f - std::exp(-dt * 6.0f));
    // Mood eases; a face that snaps between expressions reads as a slideshow.
    m_emotion += (m_emotionTo - m_emotion) * (1.0f - std::exp(-dt * 3.2f));
    if (m_openTo > m_open) {
        m_open = m_openTo;                       // opens at once
    } else if (m_openHold > 0.0f) {
        m_openHold -= dt;
    } else {
        m_open += (m_openTo - m_open) * (1.0f - std::exp(-dt * 15.0f));
        if (m_open < 0.01f) m_open = 0.0f;
    }
    if (m_open >= m_openTo) m_openTo = std::min(m_openTo, 0.0f);

    // Blinks: exponential gaps around four seconds, with a floor, and one in
    // five or so doubled.
    if (!m_blinkHold) m_blinkT += dt;
    if (m_clock >= m_blinkNext) {
        m_blinkT = 0.0f;
        if (m_doubleBlink) {
            m_doubleBlink = false;
            m_blinkNext = m_clock + 0.24f + noise() * 0.10f;
        } else {
            const float u = std::max(1e-4f, noise());
            m_blinkNext = m_clock + 0.7f + 3.4f * -std::log(u);
            m_doubleBlink = noise() < 0.18f;
        }
    }

    // Gaze: hold, then jump. Small shifts mostly, the odd real glance.
    if (!m_gazeHeld) {
        m_gazeHold -= dt;
        if (m_gazeHold <= 0.0f) {
            // Nearly half of these are real glances rather than small
            // adjustments, and both are held long enough to be seen as a
            // decision to look somewhere.
            const bool wide = noise() < 0.45f;
            m_gazeTarget = {(noise() * 2 - 1) * (wide ? 0.95f : 0.40f),
                            (noise() * 2 - 1) * (wide ? 0.60f : 0.26f)};
            m_gazeHold = wide ? 0.7f + noise() * 1.3f : 1.4f + noise() * 2.4f;
        }
    }
    const float rate = 1.0f - std::exp(-dt * 26.0f);
    m_gaze.x += (m_gazeTarget.x - m_gaze.x) * rate;
    m_gaze.y += (m_gazeTarget.y - m_gaze.y) * rate;

    // The line itself: mostly steady, with dropouts that decay.
    m_glitch = std::max(0.0f, m_glitch - dt * 1.6f);
    m_glitchNext -= dt;
    if (m_glitchNext <= 0.0f) {
        m_glitch = 0.35f + noise() * 0.5f;
        m_glitchNext = 1.5f + noise() * 6.0f;
    }

    // Tuning. Faster to lose than to find, which is what a link does and what
    // makes the return feel like it was worked for.
    const float tuneRate = (m_staticTo > m_static) ? 7.0f : 3.4f;
    m_static += (m_staticTo - m_static) * (1.0f - std::exp(-dt * tuneRate));
    if (m_staticTo >= 1.0f && m_static > 0.985f) {
        if (m_hasSwap) {
            m_hasSwap = false;
            setProfile(m_swap);
            setSpeaker(m_swapName, m_swapRole, m_swapTag);
            m_staticTo = 0.0f;          // and lock straight back on
        } else {
            // Dropped for good -- but the picture goes out the way a picture
            // tube does, not by vanishing between two frames. m_visible stays
            // true until the collapse below has finished, so the window keeps
            // being drawn while there is anything left of it to draw.
            m_powerTo = 0.0f;
        }
    }

    // The tube. Quicker to close than to open, and both fast enough to read as
    // a switch rather than an animation somebody is waiting on.
    const float powerRate = (m_powerTo > m_power) ? 9.0f : 13.0f;
    m_power += (m_powerTo - m_power) * (1.0f - std::exp(-dt * powerRate));
    if (m_powerTo <= 0.0f && m_power < 0.004f) {
        m_power = 0.0f;
        m_visible = false;
    }
    if (m_static > 0.02f) {
        // Tearing tracks the static, so the moment of change is violent.
        m_glitch = std::max(m_glitch, m_static * 0.85f);
    }

    // The picture is rebuilt here, NOT in draw(): it needs its own render
    // target, and raylib cannot nest one inside another. Drawing the window
    // into a caller's own render texture would otherwise close the caller's
    // target and quietly send the rest of the frame to the screen.
    if (ready()) renderSignal();
}

// ── the picture ───────────────────────────────────────────────────────

void Transmission::renderSignal() {
    const float W = (float)m_target.texture.width;
    const float H = (float)m_target.texture.height;
    const Profile& p = m_profile;

    // Luminance only. The filter turns this into the player's colour, so
    // nothing here -- and nothing in any art added later -- carries a hue.
    const Color kVoid  = Color{6, 6, 6, 255};
    const Color kBody  = Color{150, 150, 150, 255};
    const Color kHead  = Color{176, 176, 176, 255};
    const Color kDark  = Color{58, 58, 58, 255};
    // Matched to how this character's eyes are actually drawn, because the
    // first attempt was uncanny and every cause was a departure from it:
    //   - the white was the brightest thing in frame (217 against the shirt's
    //     201), so the filter's bloom made the eyes glow out of dark sockets
    //   - the pupil was small and the lid curved, leaving a crescent of white
    //     UNDER it -- sclera below the iris, which reads as alarm on any face
    //   - there was no dark rim, so the white sat on the skin like a hole
    // His eyes are a ringed circle, a large pupil, and a STRAIGHT heavy bar.
    const Color kRing  = Color{12, 12, 12, 255};   ///< the eye's outline
    // Both derived from the speaker's own skin: the lid IS skin, and the
    // white of an eye is brighter than the face around it.
    const unsigned char lidV = (unsigned char)std::clamp(m_skin, 24.0f, 240.0f);
    const unsigned char eyeV = (unsigned char)std::clamp(m_skin + 62.0f, 90.0f, 252.0f);
    const Color kUpper = Color{lidV, lidV, lidV, 255};
    const Color kEye   = Color{eyeV, eyeV, eyeV, 255};
    const Color kIris  = Color{26, 26, 26, 255};

    BeginTextureMode(m_target);
    ClearBackground(kVoid);

    // A little backscatter, so the figure sits in a lit room rather than a
    // hole. Kept dim: the bloom in the filter will lift it.
    DrawEllipse((int)(W * 0.5f), (int)(H * 0.55f), W * 0.72f, H * 0.55f, Color{20, 20, 20, 255});
    DrawEllipse((int)(W * 0.5f), (int)(H * 0.74f), W * 0.52f, H * 0.34f, Color{30, 30, 30, 255});

    float headCx, headCy, headRx, headRy, eyeY, eyeRx, eyeHalf;
    float imgW = W;                    ///< width the speaker was drawn at
    float mouthCx = W * 0.5f, mouthCy = H * 0.56f, mouthHalf = W * 0.06f;

    if (m_signalTex.id != 0) {
        // A drawn speaker. Fitted, not stretched -- a face is the one thing a
        // player will notice the wrong aspect on.
        const float iw = (float)m_signalTex.width, ih = (float)m_signalTex.height;
        const float k = std::min(W / iw, H / ih);
        // The whole picture drifts a pixel or two, the way a held camera on a
        // long link does. Without it the eyes are the only motion in the
        // frame, and isolated eye movement on a dead face is most of what
        // made this unsettling.
        const float driftX = std::sin(m_clock * 0.37f) * 1.6f + std::sin(m_clock * 0.91f) * 0.8f;
        const float driftY = std::sin(m_clock * 0.29f + 1.3f) * 1.9f + std::sin(m_clock * 0.73f) * 0.7f;
        const Rectangle dst{(W - iw * k) * 0.5f + driftX, (H - ih * k) * 0.5f + driftY,
                            iw * k, ih * k};
        DrawTexturePro(m_signalTex, {0, 0, iw, ih}, dst, {0, 0}, 0.0f, WHITE);

        imgW = dst.width;
        mouthCx = dst.x + dst.width * p.mouthX;
        mouthCy = dst.y + dst.height * p.mouthY;
        mouthHalf = dst.width * p.mouthW * 0.5f;
        eyeY = dst.y + dst.height * p.imgEyeY;
        eyeHalf = dst.width * p.imgEyeGap * 0.5f;
        headCx = dst.x + dst.width * p.imgEyeX;
        headCy = eyeY;
        eyeRx = dst.width * p.imgEyeW * 0.5f;
        headRx = dst.width * 0.25f;
        headRy = dst.height * 0.2f;
        (void)headRx; (void)headRy;
    } else {
        headCx = W * 0.5f;
        headCy = H * p.headY;
        headRx = W * p.headW * 0.5f;
        headRy = H * p.headH * 0.5f;

        // Shoulders first, then neck, then head: back to front, no seam.
        const float shoulderTop = H * p.shoulderY;
        DrawEllipse((int)headCx, (int)(shoulderTop + H * 0.34f), W * p.shoulderW * 0.52f,
                    H * 0.34f, kBody);
        DrawRectangleRounded({headCx - W * p.neckW * 0.5f, headCy + headRy * 0.45f,
                              W * p.neckW, shoulderTop - headCy - headRy * 0.45f + 10},
                             0.45f, 6, kBody);
        DrawEllipse((int)headCx, (int)headCy, headRx, headRy, kHead);

        if (p.hat) {
            const float brimY = headCy - headRy * 0.56f;
            DrawEllipse((int)headCx, (int)brimY, W * p.hatBrim * 0.5f, H * 0.030f, kDark);
            DrawRectangleRounded({headCx - W * p.hatCrown * 0.5f, brimY - H * p.hatCrown * 0.72f,
                                  W * p.hatCrown, H * p.hatCrown * 0.78f}, 0.25f, 6, kDark);
        }
        if (p.browDrop > 0.01f) {
            DrawEllipse((int)headCx, (int)(headCy + H * p.eyeY - headRy * 0.34f),
                        headRx * 0.94f, headRy * 0.40f * p.browDrop, Color{126, 126, 126, 255});
        }
        eyeY = headCy + H * p.eyeY;
        eyeHalf = W * p.eyeGap * 0.5f;
        eyeRx = W * p.eyeW * 0.5f;
    }

    // ── the eyes, which are the only thing that moves ──
    const float eyeRyFull = (m_signalTex.id != 0)
                                ? (float)m_target.texture.height * p.imgEyeH * 0.5f
                                : H * p.eyeH * 0.5f;
    // Built the way this character's eyes are drawn:
    //
    //   - the white is the eye's whole CIRCLE and the lid HIDES part of it.
    //     Fitting an ellipse between the lid and the eye's floor instead
    //     squashes the white flatter as the eye closes, and a circle turning
    //     into a slot is not a blink -- so the lid is a clip, not a shape
    //   - the outline sits OUTSIDE the white, at an even weight
    //   - and it is a C, not a ring: the reference leaves the nose side of
    //     each eye open, which is most of what stops two circles on a face
    //     reading as goggles
    const float cover = p.lidRest + (1.0f - p.lidRest) * (1.0f - lidOpen());
    const int   texH  = m_target.texture.height;
    const float rim   = std::max(2.0f, imgW * p.imgOutline);
    for (int side = -1; side <= 1 && p.eyes; side += 2) {
        const float ex = headCx + side * eyeHalf;

        if (p.eyeArt) {
            // The drawn eye stays. Only the pupil moves, and only the lid
            // closes over it.
            const float pr = std::max(1.5f, imgW * p.pupilRadius);
            const float px = ex + m_gaze.x * eyeRx * 0.30f;
            const float py = eyeY + m_gaze.y * eyeRyFull * 0.22f;
            if (p.pupilAspect > 1.6f) {
                const float bw = pr * 2.0f, bh = pr * 2.0f * p.pupilAspect;
                DrawRectangleRounded({px - bw * 0.5f, py - bh * 0.5f, bw, bh}, 0.85f, 8, kRing);
            } else {
                DrawEllipse((int)px, (int)py, pr, pr * p.pupilAspect, kRing);
            }

            // The lid is the SAME cut every other speaker uses: scissor to
            // the band above the closing line and repaint the eye there in
            // the skin tone, so the cut is straight and the sides keep the
            // eye's own curve. Nothing here is squashed or slid down the
            // face.
            //
            // What is different for drawn eyes, and the whole reason this
            // took three tries: her rim and her lashes are IN THE ART, so a
            // repaint the size of the eye leaves them printed on top of the
            // closed lid. The repaint is a little wider than the eye, and
            // the band starts above it, so her drawn eye goes under the lid
            // whole -- and the lash is then drawn back on at the lid's edge,
            // where a lash actually sits.
            const float lidTop = eyeY - eyeRyFull * 1.22f - rim;
            const float barY   = (eyeY - eyeRyFull) + eyeRyFull * 2.0f * cover;
            const float lidH   = barY - lidTop;
            // Gated on the LID, not on the band's height. The band reaches
            // above the eye so her drawn rim goes under it, which means it
            // has height even when the lid is fully open -- and drawing it
            // then erased the top of her eye and her lashes while she was
            // wide awake. At rest her art is left entirely alone.
            if (cover > 0.01f && lidH > 0.5f) {
                const float cw = eyeRx * 1.16f, ch = eyeRyFull * 1.20f;
                rlDrawRenderBatchActive();
                rlEnableScissorTest();
                rlScissor((int)(ex - cw - rim), (int)(texH - (lidTop + lidH)),
                          (int)((cw + rim) * 2.0f), (int)lidH);
                DrawEllipse((int)ex, (int)eyeY, cw, ch, kUpper);
                rlDrawRenderBatchActive();
                rlDisableScissorTest();
            }

            // The lash, riding the lid's edge. A lash is not a line of even
            // weight -- it swells over the arc and comes to a point at each
            // corner -- so it is laid down as a brush stroke, the same way
            // the drawn ones are, along a shallow curve through the bar.
            // Only while the lid is moving: at rest her own drawn lashes are
            // there and a second one on top of them reads as a smudge.
            if (p.lashes > 0.01f && cover > 0.01f) {
                const float hw  = eyeRx * 1.06f;
                const float bow = eyeRyFull * 0.15f;
                const Vector2 la{ex - hw, barY - bow};
                const Vector2 lb{ex,      barY + bow};   // control: the midpoint lands on barY
                const Vector2 lc{ex + hw, barY - bow};
                const float lw = rim * p.lashes;
                const int kL = 26;
                for (int i = 0; i <= kL; i++) {
                    const float t = (float)i / kL, u = 1.0f - t;
                    // Heaviest toward the outer corner, tapering to a point
                    // at both ends. `side` is -1 for the eye on the left of
                    // the frame, so the outer corner is the one further out.
                    const float o = (side < 0) ? t : 1.0f - t;
                    const float taper = std::pow(std::sin(3.14159265f * std::pow(o, 0.82f)), 0.45f);
                    const float r = lw * 0.5f * taper;
                    if (r < 0.4f) continue;
                    DrawCircleV({u * u * la.x + 2 * u * t * lb.x + t * t * lc.x,
                                 u * u * la.y + 2 * u * t * lb.y + t * t * lc.y}, r, kRing);
                }
            }
            continue;
        }

        DrawEllipse((int)ex, (int)eyeY, eyeRx, eyeRyFull, kEye);

        // The pupil is a circle sitting in the eye. It does not shrink when
        // the lid comes down; it goes behind it, which is what a lid does.
        // Travel far enough to read as looking somewhere. The pupil is half
        // the eye wide, so a third of the radius is most of the room there is.
        const float pr = std::min(eyeRx, eyeRyFull) * p.pupilScale;
        const float sit = p.lidBottom ? -0.12f : 0.10f;   // away from the lid
        const float px = ex + m_gaze.x * eyeRx * 0.36f;
        const float py = eyeY + eyeRyFull * sit + m_gaze.y * eyeRyFull * 0.20f;
        if (p.pupilAspect > 1.6f) {
            // A slit: a rounded BAR. An ellipse of the same proportions still
            // reads as a dot that has been stretched, which is not the same
            // shape and not what the drawing has.
            const float bw = pr * 2.0f, bh = pr * 2.0f * p.pupilAspect;
            DrawRectangleRounded({px - bw * 0.5f, py - bh * 0.5f, bw, bh}, 0.85f, 8, kIris);
        } else {
            DrawEllipse((int)px, (int)py, pr, pr * p.pupilAspect, kIris);
        }

        const float topEdge = eyeY - eyeRyFull - rim;
        const float botEdge = eyeY + eyeRyFull + rim;

        // Which way the lid travels. Everything else about the eye is the
        // same; only the side it comes from changes.
        float barY, lidTop, lidH;
        if (p.lidBottom) {
            barY = (eyeY + eyeRyFull) - eyeRyFull * 2.0f * cover;
            lidTop = barY;
            lidH = botEdge - barY;
        } else {
            barY = (eyeY - eyeRyFull) + eyeRyFull * 2.0f * cover;
            lidTop = topEdge;
            lidH = barY - topEdge;
        }

        // The lid: scissor to everything past the bar, then repaint the eye
        // there in the lid tone. The cut is straight, the sides keep the
        // circle's curve, and nothing is ever squashed.
        if (lidH > 0.5f) {
            rlDrawRenderBatchActive();
            rlEnableScissorTest();
            rlScissor((int)(ex - eyeRx - rim), (int)(texH - (lidTop + lidH)),
                      (int)((eyeRx + rim) * 2.0f), (int)lidH);
            DrawEllipse((int)ex, (int)eyeY, eyeRx, eyeRyFull, kUpper);
            rlDrawRenderBatchActive();
            rlDisableScissorTest();
        }

        // The lid's edge. Two rules, both learned from it looking wrong:
        //
        // it is only there when the lid is. At lidRest 0 the bar sat exactly
        // on the top of a fully open eye, a straight line ruled across a
        // round one; and
        //
        // it is only as wide as the EYE is at that height. A bar of the eye's
        // full width, drawn near the top or the bottom, sticks out past the
        // curve on both sides like a pin through it.
        if (cover > 0.01f) {
            const float k = std::clamp((barY - eyeY) / std::max(1.0f, eyeRyFull), -1.0f, 1.0f);
            const float halfAt = eyeRx * std::sqrt(std::max(0.0f, 1.0f - k * k)) + rim;
            const float barH = std::max(2.0f, rim);
            DrawRectangle((int)(ex - halfAt), (int)(barY - barH * 0.5f),
                          (int)(halfAt * 2.0f), (int)barH, kRing);
        }

        // The lashes, over everything else on the eye.
        //
        // They ride the LID, not the eye. A lash grows from the edge of the
        // eyelid, so when the lid comes down the lash comes down with it --
        // pinned to the top of the eye instead, it stays put while the lid
        // slides out from under it, and the blink reads as the eye filling
        // in rather than closing.
        //
        // A tapered brush stroke, because a stroke of even width is a LINE
        // and the taper is the shape: it swells across the lid and comes to
        // a point at each corner.
        if (p.lashes > 0.01f) {
            const float k = std::clamp((barY - eyeY) / std::max(1.0f, eyeRyFull), -1.0f, 1.0f);
            // Floored: at full closure the chord across the lid is zero
            // wide, and a lash of zero length is not a shut eye, it is a
            // missing one. A closed eye still shows its lash line.
            const float hw = std::max(eyeRx * 0.42f,
                                      eyeRx * std::sqrt(std::max(0.0f, 1.0f - k * k)));
            const float lw = rim * p.lashes;
            const float bow = eyeRyFull * 0.16f;      // follows the lid's own curve
            const float lift = rim * p.lashLift;
            const Vector2 la{ex - hw, barY - bow - lift};
            const Vector2 lb{ex,      barY + bow - lift};   // control: mid lands on the bar
            const Vector2 lc{ex + hw, barY - bow - lift};
            const int kN = 28;
            for (int i = 0; i <= kN; i++) {
                const float t = (float)i / kN, u = 1.0f - t;
                // Heaviest toward the OUTER corner, which is the one further
                // from the centre of the face.
                const float o = (side < 0) ? t : 1.0f - t;
                const float taper = std::pow(std::sin(3.14159265f * std::pow(o, 0.82f)), 0.45f);
                const float r = lw * 0.5f * taper;
                if (r < 0.4f) continue;
                DrawCircleV({u * u * la.x + 2 * u * t * lb.x + t * t * lc.x,
                             u * u * la.y + 2 * u * t * lb.y + t * t * lc.y}, r, kRing);
            }

            // The flick, carrying on from the outer end of the lid and
            // kicking up away from the eye. A separate stroke, because the
            // lash tapers to nothing exactly where this must still have
            // weight. A straight taper: an earlier version curved as it went
            // and at this size that just scattered the circles into spikes.
            if (p.lashFlick > 0.01f) {
                // On the OUTER END OF THE LASH, so it travels with the lid
                // like the rest of the lash does. It once folded back into
                // the eye here -- but that was the chord across the lid
                // collapsing to a point at full closure, which the floor on
                // `hw` above now prevents. Anchoring the flick to the eye's
                // corner instead also cured the fold, and left the tip of
                // the lash sitting still while its body slid down, which is
                // not what a lash does.
                const float out = (side < 0) ? -1.0f : 1.0f;
                const Vector2 root = (side < 0) ? la : lc;
                const float len = eyeRx * p.lashFlick;
                const Vector2 dir{out * 0.80f, -0.60f};
                const int kF = 42;
                for (int i = 0; i <= kF; i++) {
                    const float t = (float)i / kF;
                    const float r2 = lw * 0.5f * std::pow(1.0f - t, 0.7f);
                    if (r2 < 0.35f) continue;
                    DrawCircleV({root.x + dir.x * len * t,
                                 root.y + dir.y * len * t}, r2, kRing);
                }
            }
        }

        // The lower eyelid: a short heavy stroke under the eye, running from
        // about the middle of the bottom toward the nose. Tapered to a point
        // at both ends like every other mark on this face, and drawn before
        // the outline so the two overlap the way a pen would.
        if (p.lowerLid > 0.01f) {
            const float lw = rim * p.lowerLid;
            // 90 degrees is the bottom of the eye; the inner corner is 0 for
            // the eye on the left of the frame and 180 for the one on the
            // right, so the sweep is mirrored with `side`.
            // Short, and sitting toward the nose: in the drawing this is a
            // flick of the pen at the inner corner, not a second outline.
            const float from = 95.0f;
            const float to   = (side < 0) ? 42.0f : 138.0f;
            const float f    = (side < 0) ? from : 180.0f - from;
            const int kN = 22;
            for (int i = 0; i <= kN; i++) {
                const float t = (float)i / kN;
                const float ang = (f + (to - f) * t) * 3.14159265f / 180.0f;
                const float taper = std::pow(std::sin(3.14159265f * t), 0.5f);
                const float r = lw * 0.5f * taper;
                if (r < 0.4f) continue;
                const float cxp = std::cos(ang), syp = std::sin(ang);
                Vector2 n{cxp / eyeRx, syp / eyeRyFull};
                const float nl = std::sqrt(n.x * n.x + n.y * n.y);
                if (nl > 1e-5f) { n.x /= nl; n.y /= nl; }
                // Riding the rim rather than cutting into the white: pushed
                // inside, a heavy lower lid eats the sclera and the eye reads
                // as half-full of ink.
                DrawCircleV({ex + cxp * eyeRx + n.x * r * 0.25f,
                             eyeY + syp * eyeRyFull + n.y * r * 0.25f}, r, kRing);
            }
        }

        // The outline last, over the lid and the bar, and broken on the side
        // that faces the nose: 0 degrees is the eye's outer edge for the eye
        // on the left of the frame, and its inner edge for the one on the
        // right, so the gap is mirrored by starting half a turn further round.
        const float gap = std::clamp(p.eyeOpenArc, 0.0f, 120.0f);
        const float from = (side < 0 ? gap : 180.0f + gap);
        rlPushMatrix();
        rlTranslatef(ex, eyeY, 0.0f);
        rlScalef(1.0f, eyeRyFull / std::max(1.0f, eyeRx), 1.0f);
        DrawRing({0, 0}, eyeRx, eyeRx + rim, from, from + (360.0f - 2.0f * gap), 48, kRing);
        rlPopMatrix();
    }

    // ── the mouth ────────────────────────────────────────────────────
    // One curve. Its bow is the mood, its opening is the glyph being typed,
    // and every shape between a frown and a grin is a value of those two --
    // which is why there is no set of drawn mouths anywhere in this project.
    if (p.mouth) {
        const float bow = std::clamp(p.mouthBase + m_emotion, -1.0f, 1.0f);
        // A smile hangs its middle BELOW the corners and lifts the ends; a
        // frown does the opposite. Getting this backwards gives a face that
        // grins when it is grim, which is exactly what it looked like.
        const float lift = bow * mouthHalf * 0.34f;
        const float lineW = std::max(4.0f, imgW * p.imgOutline * 1.5f);

        // The upper lip: a quadratic through left corner, middle, right.
        const int kSeg = 18;
        Vector2 upper[kSeg + 1];
        for (int i = 0; i <= kSeg; i++) {
            const float t = (float)i / kSeg, u = 1.0f - t;
            const Vector2 a{mouthCx - mouthHalf, mouthCy};
            const Vector2 b{mouthCx, mouthCy + lift * 2.0f};
            const Vector2 c{mouthCx + mouthHalf, mouthCy};
            upper[i] = {u * u * a.x + 2 * u * t * b.x + t * t * c.x,
                        u * u * a.y + 2 * u * t * b.y + t * t * c.y};
        }

        if (m_open < 0.06f) {
            for (int i = 0; i < kSeg; i++) DrawLineEx(upper[i], upper[i + 1], lineW, kRing);
            DrawCircleV(upper[0], lineW * 0.5f, kRing);
            DrawCircleV(upper[kSeg], lineW * 0.5f, kRing);
        } else {
            // Open: the same upper lip, with a lower one bowed away from it.
            const float drop = m_open * mouthHalf * 0.92f;
            Vector2 fan[2 * (kSeg + 1) + 1];
            int n = 0;
            fan[n++] = {mouthCx, mouthCy + drop * 0.4f};        // hub
            for (int i = 0; i <= kSeg; i++) fan[n++] = upper[i];
            for (int i = kSeg; i >= 0; i--) {
                const float t = (float)i / kSeg;
                const float bell = std::sin(t * 3.14159265f);
                fan[n++] = {upper[i].x, mouthCy + bell * drop};
            }
            DrawTriangleFan(fan, n, kRing);
            for (int i = 0; i < kSeg; i++) DrawLineEx(upper[i], upper[i + 1], lineW * 0.8f, kRing);
        }
    }

    EndTextureMode();
}

// ── the frame ─────────────────────────────────────────────────────────

void Transmission::drawFrame(Rectangle b, Color accent) const {
    const Color dim = ColorAlpha(accent, 0.55f);
    DrawRectangleLinesEx({b.x - 3, b.y - 3, b.width + 6, b.height + 6}, 2, dim);

    // Corner brackets: the cheapest way to say "instrument" rather than "photo".
    const float k = std::min(b.width, b.height) * 0.07f;
    for (int i = 0; i < 4; i++) {
        const float cx = (i & 1) ? b.x + b.width : b.x;
        const float cy = (i & 2) ? b.y + b.height : b.y;
        const float sx = (i & 1) ? -1.0f : 1.0f;
        const float sy = (i & 2) ? -1.0f : 1.0f;
        DrawLineEx({cx, cy}, {cx + sx * k, cy}, 3, accent);
        DrawLineEx({cx, cy}, {cx, cy + sy * k}, 3, accent);
    }

    // Speaker plate and a signal readout along the bottom edge.
    //
    // The NAME and nothing else. The window is a picture of a person, and the
    // person's name is all it needs to caption them -- the job title and the
    // short tag were reference information stapled to a portrait, and with
    // two portraits up they doubled. What they are is said once, on the
    // textbox's own plate, where the words are.
    if (!m_speaker.empty()) {
        const int fs = (int)std::clamp(b.height * 0.045f, 12.0f, 22.0f);
        const int tw = MeasureText(m_speaker.c_str(), fs);
        const Rectangle plate{b.x + 10, b.y - fs - 12.0f, (float)tw + 20, (float)fs + 10};
        DrawRectangleRec(plate, Color{10, 12, 16, 235});
        DrawRectangleLinesEx(plate, 1, dim);
        DrawText(m_speaker.c_str(), (int)plate.x + 10, (int)plate.y + 5, fs, accent);
    }
    const int bars = 5;
    const float bw = 5, gap = 3;
    // The bars read the SAME number the picture does, drift and all -- a
    // meter that says five while the image is crawling is a broken meter.
    const int lit = (int)std::round(std::clamp(m_signal.quality + m_drift, 0.0f, 1.0f) * bars);
    for (int i = 0; i < bars; i++) {
        const float h = 4.0f + i * 3.0f;
        const Rectangle r{b.x + b.width - (bars - i) * (bw + gap), b.y + b.height + 8 - h + 12, bw, h};
        DrawRectangleRec(r, i < lit ? accent : ColorAlpha(accent, 0.22f));
    }
}

namespace {
// Below this the picture is only a line: the beam has spread sideways but the
// frame has not opened yet. A quarter of the travel, which is enough to be
// seen at a tenth of a second and not so much that the window feels slow.
constexpr float POWER_LINE = 0.26f;

float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
}  // namespace

Rectangle Transmission::powerRect(Rectangle b) const {
    const float p = std::clamp(m_power, 0.0f, 1.0f);
    // Width first, then height. A tube that opened both at once would be a
    // box scaling up, which is a UI animation; this is a signal arriving.
    const float wx = smoothstep(p / POWER_LINE);
    const float hy = smoothstep((p - POWER_LINE) / (1.0f - POWER_LINE));
    const float w = b.width * (0.04f + 0.96f * wx);
    const float h = b.height * hy;
    return {b.x + (b.width - w) * 0.5f, b.y + (b.height - h) * 0.5f, w, h};
}

void Transmission::drawScanLine(Rectangle b, Color accent) const {
    // The line is nearly white with the accent bleeding out of it, because
    // that is what a phosphor line looks like: the colour is in the glow.
    // The caller's alpha rides over the lot, so the line can be faded across
    // the picture once there is a picture to fade it across.
    const float a = accent.a / 255.0f;
    const Color core{(unsigned char)(200 + accent.r / 5),
                     (unsigned char)(200 + accent.g / 5),
                     (unsigned char)(210 + accent.b / 5), 255};
    const float y = b.y + b.height * 0.5f;
    DrawRectangleRec({b.x, y - 8.0f, b.width, 16.0f}, ColorAlpha(accent, 0.16f * a));
    DrawRectangleRec({b.x, y - 3.0f, b.width, 6.0f}, ColorAlpha(accent, 0.42f * a));
    DrawRectangleRec({b.x, y - 1.0f, b.width, 2.0f}, ColorAlpha(core, a));
}

void Transmission::draw(Rectangle b, Color accent) {
    if (!ready() || !m_visible) return;

    // The speaker's own shift off the player's accent.
    Color tinted = accent;
    if (m_profile.hueShift != 0.0f || m_profile.tintGain != 1.0f) {
        Vector3 hsv = ColorToHSV(accent);
        hsv.x = std::fmod(hsv.x + m_profile.hueShift + 360.0f, 360.0f);
        hsv.z = std::clamp(hsv.z * m_profile.tintGain, 0.0f, 1.0f);
        tinted = ColorFromHSV(hsv.x, hsv.y, hsv.z);
    }

    // COMING ON, OR GOING OFF. The picture is drawn into whatever the tube has
    // opened so far, and while that is still a slit there is nothing to draw a
    // picture into -- only the line itself.
    const Rectangle full = b;
    b = powerRect(full);
    if (b.height < 3.0f) { drawScanLine(b, tinted); return; }

    // The window's own darkness first, so the filter's blacks sit on black
    // rather than on whatever the map happens to be showing.
    DrawRectangleRec(b, Color{4, 5, 7, 255});
    const Vector3 acc{tinted.r / 255.0f, tinted.g / 255.0f, tinted.b / 255.0f};
    const float quality = std::clamp(m_signal.quality + m_drift - m_glitch * 0.55f, 0.05f, 1.0f);
    const float gain = m_signal.gain * (0.72f + 0.28f * m_attention);
    const Vector2 res{b.width, b.height};

    const Rectangle src{0, 0, (float)m_target.texture.width,
                        -(float)m_target.texture.height};   // render textures are y-flipped

    if (m_shader.id != 0) {
        SetShaderValue(m_shader, m_locResolution, &res, SHADER_UNIFORM_VEC2);
        SetShaderValue(m_shader, m_locTime, &m_clock, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_locAccent, &acc, SHADER_UNIFORM_VEC3);
        SetShaderValue(m_shader, m_locQuality, &quality, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_locGain, &gain, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_locStatic, &m_static, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(m_shader);
        DrawTexturePro(m_target.texture, src, b, {0, 0}, 0.0f, WHITE);
        EndShaderMode();
    } else {
        DrawTexturePro(m_target.texture, src, b, {0, 0}, 0.0f, accent);
    }

    // The frame belongs to the link, not to the UI: it comes up with the
    // picture and goes with it -- and it arrives LAST, once the tube is most
    // of the way open. Brackets and a name plate drawn around a two-inch slit
    // are a caption on nothing.
    const float framed = smoothstep((m_power - 0.72f) / 0.28f);
    if (framed > 0.01f)
        drawFrame(b, ColorAlpha(tinted, (1.0f - m_static * 0.9f) * framed));

    // The line stays lit across the picture until the tube is fully open, and
    // comes back as it closes: it is the same beam, and seeing it ride over
    // the image for a moment is what ties the two halves together.
    const float lineFade = 1.0f - smoothstep((m_power - 0.45f) / 0.5f);
    if (lineFade > 0.01f)
        drawScanLine(b, ColorAlpha(tinted, lineFade));
}

Rectangle Transmission::suggestedBounds(Rectangle screen, bool rightSide) {
    // Deliberately large: room for a raised arm inside the frame, so a
    // gesture never has to leave the window.
    const float margin = std::max(24.0f, screen.width * 0.025f);
    const float h = screen.height * 0.44f;
    const float w = std::min(screen.width * 0.30f, h * 0.86f);
    const float x = rightSide ? screen.x + screen.width - margin - w : screen.x + margin;
    return {x, screen.y + margin * 1.4f, w, h};
}

}  // namespace comms
