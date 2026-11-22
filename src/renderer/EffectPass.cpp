#include "EffectPass.h"

#include <cmath>
#include <cstdio>

namespace odeffect {

namespace {

// ── TWO DIALECTS, BECAUSE THE GAME SHIPS ON SIX GL BACKENDS ──
//
// The same arithmetic twice, as MapRenderer already does for its political and
// overlay passes. This is one of the costs the Shaders design refused to hand
// to mod authors: asking a modder for two dialects they cannot both test is how
// you get an effect that works for its author and nobody else.
const char* kFragEs = R"(#version 100
precision mediump float;
varying vec2 fragTexCoord;
varying vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec4 uArea;      // x, y, w, h in screen pixels
uniform vec4 uParam;     // hue turns, saturation, vignette, grain
uniform float uWobble;   // amplitude in texture-coordinate units, already clamped
uniform float uTime;
void main() {
    // ── THE DISPLACEMENT GOES FIRST ──
    //
    // Everything else here recolours the pixel under consideration; this one
    // chooses a DIFFERENT pixel to recolour. Two sines across the target, at
    // frequencies the host picked, so the image breathes rather than shears.
    //
    // The sample can land outside the quad, where the texture is clamped: at
    // these amplitudes that is a stripe of edge colour a pixel or two wide at
    // the border, which is why the amplitude is capped rather than trusted.
    vec2 uv = fragTexCoord;
    if (uWobble > 0.0) {
        vec2 p = (gl_FragCoord.xy - uArea.xy) / max(uArea.zw, vec2(1.0));
        uv += vec2(sin(p.y * 11.0 + uTime * 2.1),
                   cos(p.x *  9.0 + uTime * 1.7)) * uWobble;
    }
    vec4 src = texture2D(texture0, uv) * colDiffuse * fragColor;
    vec3 c = src.rgb;
    float hue = uParam.x;
    if (hue > 0.0) {
        // YIQ rotation: the cheap hue rotate, and it leaves luminance alone,
        // which is the half that matters for photosensitivity.
        float a = hue * 6.2831853;
        float cs = cos(a), sn = sin(a);
        float y  =  0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
        float i  =  0.596 * c.r - 0.274 * c.g - 0.322 * c.b;
        float q  =  0.211 * c.r - 0.523 * c.g + 0.312 * c.b;
        float i2 = i * cs - q * sn;
        float q2 = i * sn + q * cs;
        c = vec3(y + 0.956 * i2 + 0.621 * q2,
                 y - 0.272 * i2 - 0.647 * q2,
                 y - 1.106 * i2 + 1.703 * q2);
    }
    float lum = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    c = mix(vec3(lum), c, uParam.y);
    if (uParam.z > 0.0) {
        vec2 p = (gl_FragCoord.xy - uArea.xy) / max(uArea.zw, vec2(1.0));
        float d = length(p - vec2(0.5)) * 1.41421356;
        c *= mix(1.0, 1.0 - smoothstep(0.35, 1.0, d), uParam.z);
    }
    if (uParam.w > 0.0) {
        float n = fract(sin(dot(gl_FragCoord.xy + vec2(uTime * 37.0), vec2(12.9898, 78.233))) * 43758.5453);
        c += (n - 0.5) * 0.35 * uParam.w;
    }
    gl_FragColor = vec4(clamp(c, 0.0, 1.0), src.a);
}
)";

const char* kFrag330 = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec4 uArea;
uniform vec4 uParam;
uniform float uWobble;
uniform float uTime;
out vec4 finalColor;
void main() {
    // See the GLES dialect above for why this goes first and why the
    // amplitude is capped.
    vec2 uv = fragTexCoord;
    if (uWobble > 0.0) {
        vec2 p = (gl_FragCoord.xy - uArea.xy) / max(uArea.zw, vec2(1.0));
        uv += vec2(sin(p.y * 11.0 + uTime * 2.1),
                   cos(p.x *  9.0 + uTime * 1.7)) * uWobble;
    }
    vec4 src = texture(texture0, uv) * colDiffuse * fragColor;
    vec3 c = src.rgb;
    float hue = uParam.x;
    if (hue > 0.0) {
        float a = hue * 6.2831853;
        float cs = cos(a), sn = sin(a);
        float y  =  0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
        float i  =  0.596 * c.r - 0.274 * c.g - 0.322 * c.b;
        float q  =  0.211 * c.r - 0.523 * c.g + 0.312 * c.b;
        float i2 = i * cs - q * sn;
        float q2 = i * sn + q * cs;
        c = vec3(y + 0.956 * i2 + 0.621 * q2,
                 y - 0.272 * i2 - 0.647 * q2,
                 y - 1.106 * i2 + 1.703 * q2);
    }
    float lum = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    c = mix(vec3(lum), c, uParam.y);
    if (uParam.z > 0.0) {
        vec2 p = (gl_FragCoord.xy - uArea.xy) / max(uArea.zw, vec2(1.0));
        float d = length(p - vec2(0.5)) * 1.41421356;
        c *= mix(1.0, 1.0 - smoothstep(0.35, 1.0, d), uParam.z);
    }
    if (uParam.w > 0.0) {
        float n = fract(sin(dot(gl_FragCoord.xy + vec2(uTime * 37.0), vec2(12.9898, 78.233))) * 43758.5453);
        c += (n - 0.5) * 0.35 * uParam.w;
    }
    finalColor = vec4(clamp(c, 0.0, 1.0), src.a);
}
)";

#if defined(PLATFORM_WEB) || defined(PLATFORM_ANDROID) || defined(GRAPHICS_API_OPENGL_ES2)
constexpr bool kEs = true;
#else
constexpr bool kEs = false;
#endif

Shader g_shader{};
int g_locArea = -1, g_locParam = -1, g_locTime = -1, g_locWobble = -1;
bool g_tried = false, g_ok = false;

bool ensureShader() {
    if (g_tried) return g_ok;
    g_tried = true;
    g_shader = LoadShaderFromMemory(nullptr, kEs ? kFragEs : kFrag330);
    // A DRIVER THAT REFUSES IT LEAVES THE GAME DRAWING NORMALLY. Tried once,
    // not once per frame: a shader that failed to compile will fail again, and
    // asking the driver every frame is both slow and noisy in the log.
    if (g_shader.id == 0) {
        printf("[MODS] screen effects unavailable: the shader did not compile\n");
        return false;
    }
    g_locArea  = GetShaderLocation(g_shader, "uArea");
    g_locParam = GetShaderLocation(g_shader, "uParam");
    g_locTime  = GetShaderLocation(g_shader, "uTime");
    g_locWobble = GetShaderLocation(g_shader, "uWobble");
    // Said once, when a mod first asks. It is the only line that distinguishes
    // "a mod asked and the pass is running" from "a mod asked and the driver
    // refused" from "nothing ever asked", and all three look identical on a
    // map that happens to look normal.
    printf("[MODS] screen effects: pass compiled\n");
    fflush(stdout);
    g_ok = true;
    return true;
}

}  // namespace

bool beginPass(const Layer& layer, Target target, Rectangle area) {
    if (!layer.active(target)) return false;
    if (!ensureShader()) return false;

    // The held rotation and the rolling one, already added up: how HueCycle
    // works is the layer's business and is tested there.
    const float hue = (float)layer.huePermille(target) / 1000.0f;
    // 500 per mille is 1x, so the range is 0..2x with neutral in the middle.
    const float sat = (float)layer.rendered(target, Effect::Saturation) / 500.0f;
    const float vig = (float)layer.rendered(target, Effect::Vignette) / 1000.0f;
    const float grn = (float)layer.rendered(target, Effect::Grain) / 1000.0f;

    // Scaled into texture-coordinate units HERE, by the ceiling this target
    // carries: the shader is given a distance, not a dial it could misread.
    const float wob = (float)layer.rendered(target, Effect::Wobble) / 1000.0f
                      * maxWobbleFor(target);

    const float areaV[4] = {area.x, area.y, area.width, area.height};
    const float paramV[4] = {hue, sat, vig, grn};
    // The clock the grain moves on. Wrapped, so a session left running for days
    // does not lose precision in a float and freeze the noise.
    const float t = (float)std::fmod(GetTime(), 3600.0);
    SetShaderValue(g_shader, g_locArea,  areaV,  SHADER_UNIFORM_VEC4);
    SetShaderValue(g_shader, g_locParam, paramV, SHADER_UNIFORM_VEC4);
    SetShaderValue(g_shader, g_locTime,   &t,   SHADER_UNIFORM_FLOAT);
    SetShaderValue(g_shader, g_locWobble, &wob, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(g_shader);
    return true;
}

void endPass() { EndShaderMode(); }

void unloadPass() {
    if (g_ok && g_shader.id != 0) UnloadShader(g_shader);
    g_shader = Shader{};
    g_ok = false;
    g_tried = false;
}

}  // namespace odeffect
