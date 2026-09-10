#include "GlobeView.h"

#include "raymath.h"
#include "rlgl.h"
#include "../util/Async.h"

#include <memory>
#include <mutex>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Enough to look round without a visible silhouette polygon at the limb, and
// small enough to upload once on a phone. 96x48 is 9,216 triangles; the limb is
// where a sphere gives itself away, and rings buy smoothness there.
constexpr int kRings  = 48;   ///< latitude divisions
constexpr int kSlices = 96;   ///< longitude divisions

constexpr float kMinDist = 1.35f;   ///< closer and the near plane clips the ground
constexpr float kMaxDist = 8.0f;
constexpr float kFovY    = 45.0f;

/// Clamp latitude just short of the poles. AT the pole the up-vector and the
/// view direction are parallel, the view matrix is degenerate, and the picture
/// flips over -- a bug that only appears when somebody drags all the way north.
constexpr float kLatLimit = 1.5533f;   // 89 degrees


// ── The atmosphere's limb ──
//
// Not a scattering integral: one rim term, additively blended, gated by the sun
// so the night side has no glow. That is the whole difference between a sphere
// with a picture on it and something that reads as a planet, and it costs one
// extra pass over a shell.
const char* kAirVertEs = R"(#version 100
attribute vec3 vertexPosition;
attribute vec3 vertexNormal;
uniform mat4 mvp;
uniform mat4 matModel;
varying vec3 vNormal;
varying vec3 vWorld;
void main() {
    vNormal = vertexNormal;
    vWorld  = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position = mvp * vec4(vertexPosition, 1.0);
})";

const char* kAirFragEs = R"(#version 100
precision mediump float;
varying vec3 vNormal;
varying vec3 vWorld;
uniform vec3 viewPos;
uniform vec3 sunDir;
uniform vec3 airColour;
uniform float airStrength;
uniform float airFalloff;
void main() {
    vec3 n = normalize(vNormal);
    vec3 v = normalize(viewPos - vWorld);
    float rim = pow(1.0 - max(dot(n, v), 0.0), airFalloff);
    float lit = smoothstep(-0.35, 0.30, dot(n, normalize(sunDir)));
    gl_FragColor = vec4(airColour * rim * lit * airStrength, rim * lit * airStrength);
})";

const char* kAirVert330 = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
uniform mat4 mvp;
uniform mat4 matModel;
out vec3 vNormal;
out vec3 vWorld;
void main() {
    vNormal = vertexNormal;
    vWorld  = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position = mvp * vec4(vertexPosition, 1.0);
})";

const char* kAirFrag330 = R"(#version 330
in vec3 vNormal;
in vec3 vWorld;
uniform vec3 viewPos;
uniform vec3 sunDir;
uniform vec3 airColour;
uniform float airStrength;
uniform float airFalloff;
out vec4 finalColor;
void main() {
    vec3 n = normalize(vNormal);
    vec3 v = normalize(viewPos - vWorld);
    float rim = pow(1.0 - max(dot(n, v), 0.0), airFalloff);
    float lit = smoothstep(-0.35, 0.30, dot(n, normalize(sunDir)));
    finalColor = vec4(airColour * rim * lit * airStrength, rim * lit * airStrength);
})";

// ── The sun's glare ──
//
// One shell, faded per pixel by how directly it faces the camera. Nested opaque
// shells were the first attempt and produced visible concentric rings: the
// falloff has to happen inside the fragment, not between draw calls.
const char* kGlowVertEs = R"(#version 100
attribute vec3 vertexPosition;
attribute vec3 vertexNormal;
uniform mat4 mvp;
uniform mat4 matModel;
varying vec3 vN;
varying vec3 vW;
void main() {
    vN = vertexNormal;
    vW = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position = mvp * vec4(vertexPosition, 1.0);
})";

const char* kGlowFragEs = R"(#version 100
precision mediump float;
varying vec3 vN;
varying vec3 vW;
uniform vec3 viewPos;
uniform vec3 glowColour;
uniform float glowFalloff;
void main() {
    vec3 n = normalize(vN);
    vec3 v = normalize(viewPos - vW);
    float f = pow(max(dot(n, v), 0.0), glowFalloff);
    gl_FragColor = vec4(glowColour * f, f);
})";

const char* kGlowVert330 = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
uniform mat4 mvp;
uniform mat4 matModel;
out vec3 vN;
out vec3 vW;
void main() {
    vN = vertexNormal;
    vW = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position = mvp * vec4(vertexPosition, 1.0);
})";

const char* kGlowFrag330 = R"(#version 330
in vec3 vN;
in vec3 vW;
uniform vec3 viewPos;
uniform vec3 glowColour;
uniform float glowFalloff;
out vec4 finalColor;
void main() {
    vec3 n = normalize(vN);
    vec3 v = normalize(viewPos - vW);
    float f = pow(max(dot(n, v), 0.0), glowFalloff);
    finalColor = vec4(glowColour * f, f);
})";


}  // namespace

namespace globe {

Vector3 unitFromPixel(float px, float py, int mapW, int mapH) {
    const float u = px / (float)(mapW > 0 ? mapW : 1);
    const float v = py / (float)(mapH > 0 ? mapH : 1);
    const float lat = PI * 0.5f - v * PI;
    const float lon = -PI + u * 2.0f * PI;
    const float cr = cosf(lat);
    // EAST IS -Z, and it has to be said once somewhere.
    //
    // raylib is right-handed with Y up. A camera on +X looking at the origin has
    // screen-right = cross(forward, up) = cross(-X, Y) = -Z. So placing east at
    // +Z draws the world MIRRORED -- a globe that looks entirely correct until
    // you notice Arabia is west of Gibraltar. Negated here, in eyeFromOrbit and
    // in the mesh, so all three agree.
    return { cr * cosf(lon), sinf(lat), -cr * sinf(lon) };
}

void pixelFromUnit(Vector3 p, int mapW, int mapH, int& px, int& py) {
    const float lat = asinf(std::clamp(p.y, -1.0f, 1.0f));
    const float lon = atan2f(-p.z, p.x);
    float u = (lon + PI) / (2.0f * PI);
    float v = (PI * 0.5f - lat) / PI;
    // Just inside the far edge: a point exactly at u=1 is longitude +PI, which
    // is the same meridian as u=0, and rounding it to mapW would index one
    // column past the end of the raster.
    u = std::clamp(u, 0.0f, 0.999999f);
    v = std::clamp(v, 0.0f, 0.999999f);
    px = (int)(u * (float)mapW);
    py = (int)(v * (float)mapH);
}

Vector3 eyeFromOrbit(float lat, float lon, float dist) {
    const float cr = cosf(lat);
    return { dist * cr * cosf(lon), dist * sinf(lat), -dist * cr * sinf(lon) };
}

bool onNearSide(Vector3 p, float lat, float lon, float dist) {
    if (dist <= 1.0f) return true;          // inside the sphere: everything faces you
    const Vector3 eye = eyeFromOrbit(lat, lon, dist);
    return Vector3DotProduct(p, eye) > 1.0f;
}

}  // namespace globe


// ── The lighting, written once for two GLSL profiles ──
//
// Desktop is GLSL 330; Android links GLESv2 and the web build runs WebGL, both
// of which are GLSL ES 1.00. That is not a detail to port later: ES 1.00 has no
// `in`/`out`, no `texture()`, and REQUIRES a precision qualifier in the
// fragment stage. A shader developed on desktop and adapted afterwards is a
// shader that fails on two of the four platforms, on a tag, where it cannot be
// rehearsed -- which is exactly how this release went. So both are here, side
// by side, and they compute the same thing.
namespace {

#if defined(GRAPHICS_API_OPENGL_ES2) || defined(PLATFORM_ANDROID) || defined(PLATFORM_WEB)
constexpr bool kEs = true;
#else
constexpr bool kEs = false;
#endif

const char* kVertexEs = R"(#version 100
attribute vec3 vertexPosition;
attribute vec2 vertexTexCoord;
attribute vec3 vertexNormal;
uniform mat4 mvp;
uniform mat4 matModel;
varying vec2 fragTexCoord;
varying vec3 fragNormal;
varying vec3 fragWorld;
void main() {
    fragTexCoord = vertexTexCoord;
    fragNormal   = vertexNormal;
    fragWorld    = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position  = mvp * vec4(vertexPosition, 1.0);
})";

const char* kFragmentEs = R"(#version 100
precision mediump float;
varying vec2 fragTexCoord;
varying vec3 fragNormal;
varying vec3 fragWorld;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 sunDir;
uniform vec3 sunColour;
uniform float sunStrength;
uniform float nightFloor;
uniform float softness;
// WHAT IS BETWEEN THIS SURFACE AND THE SUN, whatever that happens to be.
//
// Drawing the planet, the occluder is the moon: a solar eclipse. Drawing the
// moon, the occluder is the planet at the origin: a lunar eclipse. They are the
// same question asked from two places, so they are one term rather than two --
// which also means a lunar eclipse cannot quietly not work while the solar one
// does. occR <= 0 means nothing is in the way.
uniform vec3 occPos;
uniform float occR;

// ── The cloud layer, as seen from the GROUND ──
//
// Without this the cloud shell is a decal: correct in every other way and still
// obviously stuck to the sphere, because nothing beneath it knows it is there.
// A shadow is what puts one surface above another. The ray from this point to
// the sun is intersected with the cloud shell, the cloud sampled where it
// crosses, and the ground darkened by what it finds -- so the shadow falls to
// one SIDE of the bank, further as the sun gets lower, exactly as a real one.
uniform sampler2D cloudTex;
uniform float cloudR;      // shell radius; <= 1 disables the term entirely
uniform float cloudRot;    // the shell's rotation, so shadows follow the drift
uniform float cloudAmt;
void main() {
    vec4 texel = texture2D(texture0, fragTexCoord) * colDiffuse;
    float d = dot(normalize(fragNormal), normalize(sunDir));
    float lit = smoothstep(-softness, softness, d);
    // ── The moon's shadow ──
    //
    // A point is eclipsed when the moon sits across its line to the sun. The
    // angle between that line and the moon's centre, measured against the angle
    // the moon subtends from here, gives coverage directly -- no ray march and
    // no shadow map. Partial cover shades softly, which is what a penumbra is.
    float shade = 1.0;
    if (occR > 0.0) {
        vec3 toOcc = occPos - fragWorld;
        float sep = acos(clamp(dot(normalize(toOcc), normalize(sunDir)), -1.0, 1.0));
        float ang = asin(clamp(occR / max(length(toOcc), 0.001), 0.0, 1.0));
        shade = mix(0.18, 1.0, smoothstep(0.0, ang * 1.6, sep));
    }
    float cloudShade = 0.0;
    if (cloudR > 1.0) {
        vec3 p = normalize(fragNormal);
        vec3 S = normalize(sunDir);
        float b = dot(p, S);
        float disc = b * b + (cloudR * cloudR - 1.0);
        if (disc > 0.0) {
            vec3 q = normalize(p + S * (-b + sqrt(disc)));
            float clon = atan(-q.z, q.x) - cloudRot;
            float clat = asin(clamp(q.y, -1.0, 1.0));
            vec2 cuv = vec2(fract((clon + 3.14159265) / 6.28318531),
                            (1.57079633 - clat) / 3.14159265);
            cloudShade = texture2D(cloudTex, cuv).a * cloudAmt;
        }
    }
    vec3 day   = texel.rgb * sunColour * sunStrength * shade * (1.0 - 0.38 * cloudShade);
    vec3 night = texel.rgb * nightFloor;
    gl_FragColor = vec4(mix(night, day, lit), texel.a);
})";

const char* kVertex330 = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
uniform mat4 mvp;
uniform mat4 matModel;
out vec2 fragTexCoord;
out vec3 fragNormal;
out vec3 fragWorld;
void main() {
    fragTexCoord = vertexTexCoord;
    fragNormal   = vertexNormal;
    fragWorld    = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position  = mvp * vec4(vertexPosition, 1.0);
})";

const char* kFragment330 = R"(#version 330
in vec2 fragTexCoord;
in vec3 fragNormal;
in vec3 fragWorld;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 sunDir;
uniform vec3 sunColour;
uniform float sunStrength;
uniform float nightFloor;
uniform float softness;
// WHAT IS BETWEEN THIS SURFACE AND THE SUN, whatever that happens to be.
//
// Drawing the planet, the occluder is the moon: a solar eclipse. Drawing the
// moon, the occluder is the planet at the origin: a lunar eclipse. They are the
// same question asked from two places, so they are one term rather than two --
// which also means a lunar eclipse cannot quietly not work while the solar one
// does. occR <= 0 means nothing is in the way.
uniform vec3 occPos;
uniform float occR;

// ── The cloud layer, as seen from the GROUND ──
//
// Without this the cloud shell is a decal: correct in every other way and still
// obviously stuck to the sphere, because nothing beneath it knows it is there.
// A shadow is what puts one surface above another. The ray from this point to
// the sun is intersected with the cloud shell, the cloud sampled where it
// crosses, and the ground darkened by what it finds -- so the shadow falls to
// one SIDE of the bank, further as the sun gets lower, exactly as a real one.
uniform sampler2D cloudTex;
uniform float cloudR;      // shell radius; <= 1 disables the term entirely
uniform float cloudRot;    // the shell's rotation, so shadows follow the drift
uniform float cloudAmt;
out vec4 finalColor;
void main() {
    vec4 texel = texture(texture0, fragTexCoord) * colDiffuse;
    float d = dot(normalize(fragNormal), normalize(sunDir));
    float lit = smoothstep(-softness, softness, d);
    // ── The moon's shadow ──
    //
    // A point is eclipsed when the moon sits across its line to the sun. The
    // angle between that line and the moon's centre, measured against the angle
    // the moon subtends from here, gives coverage directly -- no ray march and
    // no shadow map. Partial cover shades softly, which is what a penumbra is.
    float shade = 1.0;
    if (occR > 0.0) {
        vec3 toOcc = occPos - fragWorld;
        float sep = acos(clamp(dot(normalize(toOcc), normalize(sunDir)), -1.0, 1.0));
        float ang = asin(clamp(occR / max(length(toOcc), 0.001), 0.0, 1.0));
        shade = mix(0.18, 1.0, smoothstep(0.0, ang * 1.6, sep));
    }
    float cloudShade = 0.0;
    if (cloudR > 1.0) {
        vec3 p = normalize(fragNormal);
        vec3 S = normalize(sunDir);
        float b = dot(p, S);
        float disc = b * b + (cloudR * cloudR - 1.0);
        if (disc > 0.0) {
            vec3 q = normalize(p + S * (-b + sqrt(disc)));
            float clon = atan(-q.z, q.x) - cloudRot;
            float clat = asin(clamp(q.y, -1.0, 1.0));
            vec2 cuv = vec2(fract((clon + 3.14159265) / 6.28318531),
                            (1.57079633 - clat) / 3.14159265);
            cloudShade = texture(cloudTex, cuv).a * cloudAmt;
        }
    }
    vec3 day   = texel.rgb * sunColour * sunStrength * shade * (1.0 - 0.38 * cloudShade);
    vec3 night = texel.rgb * nightFloor;
    finalColor = vec4(mix(night, day, lit), texel.a);
})";

}  // namespace

GlobeView::GlobeView(int mapW, int mapH)
    : m_mapW(mapW > 0 ? mapW : 1), m_mapH(mapH > 0 ? mapH : 1) {
    // ── The mesh, with the UVs the picking maths inverts ──
    //
    //   u = 0 at longitude -PI, rising east
    //   v = 0 at the NORTH pole, rising south
    //
    // which is the layout of an equirectangular map read top-left to
    // bottom-right, and therefore the layout of the political raster.
    const int verts = (kRings + 1) * (kSlices + 1);
    const int tris  = kRings * kSlices * 2;

    m_mesh = Mesh{};
    m_mesh.vertexCount   = verts;
    m_mesh.triangleCount = tris;
    m_mesh.vertices  = (float*)MemAlloc(sizeof(float) * 3 * (size_t)verts);
    m_mesh.normals   = (float*)MemAlloc(sizeof(float) * 3 * (size_t)verts);
    m_mesh.texcoords = (float*)MemAlloc(sizeof(float) * 2 * (size_t)verts);
    m_mesh.indices   = (unsigned short*)MemAlloc(sizeof(unsigned short) * 3 * (size_t)tris);

    int vi = 0, ti = 0;
    for (int r = 0; r <= kRings; ++r) {
        const float v   = (float)r / (float)kRings;          // 0 north -> 1 south
        const float lat = PI * 0.5f - v * PI;                // +PI/2 -> -PI/2
        const float cy  = sinf(lat), cr = cosf(lat);
        for (int s = 0; s <= kSlices; ++s) {
            const float u   = (float)s / (float)kSlices;     // 0 -> 1 eastward
            const float lon = -PI + u * 2.0f * PI;
            const float x = cr * cosf(lon);
            const float y = cy;
            const float z = -cr * sinf(lon);   // east is -Z: see the note in unitFromPixel
            m_mesh.vertices[vi * 3 + 0] = x;
            m_mesh.vertices[vi * 3 + 1] = y;
            m_mesh.vertices[vi * 3 + 2] = z;
            m_mesh.normals[vi * 3 + 0]  = x;   // unit sphere: position IS the normal
            m_mesh.normals[vi * 3 + 1]  = y;
            m_mesh.normals[vi * 3 + 2]  = z;
            m_mesh.texcoords[vi * 2 + 0] = u;
            m_mesh.texcoords[vi * 2 + 1] = v;
            ++vi;
        }
    }
    for (int r = 0; r < kRings; ++r) {
        for (int s = 0; s < kSlices; ++s) {
            const unsigned short a = (unsigned short)(r * (kSlices + 1) + s);
            const unsigned short b = (unsigned short)(a + kSlices + 1);
            // Wound so the OUTSIDE faces the camera -- and note this winding is
            // paired with the NEGATED z above. Putting east at -Z is a
            // reflection, and a reflection reverses triangle orientation, so
            // the two decisions are one decision: change either and the sphere
            // turns inside out. Getting it backwards is
            // not a blank screen -- the near face is culled, you see the inside
            // of the far wall, and the globe shows the ANTIPODE of wherever the
            // camera is. It looks like a working globe pointed at the wrong
            // place, which is why the preview harness measures what longitude
            // is actually on screen rather than trusting the picture.
            m_mesh.indices[ti++] = a;
            m_mesh.indices[ti++] = b;
            m_mesh.indices[ti++] = (unsigned short)(a + 1);
            m_mesh.indices[ti++] = (unsigned short)(a + 1);
            m_mesh.indices[ti++] = b;
            m_mesh.indices[ti++] = (unsigned short)(b + 1);
        }
    }

    UploadMesh(&m_mesh, false);
    m_material = LoadMaterialDefault();

    m_shader = LoadShaderFromMemory(kEs ? kVertexEs : kVertex330,
                                    kEs ? kFragmentEs : kFragment330);
    // A shader that failed to compile is not a reason to lose the globe: raylib
    // hands back id 0 and the map simply draws unlit, which is the look it has
    // always had on the flat view. Reported once rather than silently, because
    // "the globe is not lit" on one platform is otherwise invisible.
    m_haveShader = m_shader.id != 0 && m_shader.id != rlGetShaderIdDefault();
    if (m_haveShader) {
        m_uSunDir      = GetShaderLocation(m_shader, "sunDir");
        m_uSunColour   = GetShaderLocation(m_shader, "sunColour");
        m_uSunStrength = GetShaderLocation(m_shader, "sunStrength");
        m_uNightFloor  = GetShaderLocation(m_shader, "nightFloor");
        m_uSoftness    = GetShaderLocation(m_shader, "softness");
        m_uMoonPos     = GetShaderLocation(m_shader, "occPos");
        m_uMoonR       = GetShaderLocation(m_shader, "occR");
        m_uCloudTex    = GetShaderLocation(m_shader, "cloudTex");
        m_uCloudR      = GetShaderLocation(m_shader, "cloudR");
        m_uCloudRot    = GetShaderLocation(m_shader, "cloudRot");
        m_uCloudAmt    = GetShaderLocation(m_shader, "cloudAmt");
        m_material.shader = m_shader;
        m_glow = LoadShaderFromMemory(kEs ? kGlowVertEs : kGlowVert330,
                                      kEs ? kGlowFragEs : kGlowFrag330);
        m_haveGlow = m_glow.id != 0 && m_glow.id != rlGetShaderIdDefault();
        if (m_haveGlow) {
            m_gView    = GetShaderLocation(m_glow, "viewPos");
            m_gColour  = GetShaderLocation(m_glow, "glowColour");
            m_gFalloff = GetShaderLocation(m_glow, "glowFalloff");
        }

        m_air = LoadShaderFromMemory(kEs ? kAirVertEs : kAirVert330,
                                     kEs ? kAirFragEs : kAirFrag330);
        m_haveAir = m_air.id != 0 && m_air.id != rlGetShaderIdDefault();
        if (m_haveAir) {
            m_aViewPos   = GetShaderLocation(m_air, "viewPos");
            m_aSunDir    = GetShaderLocation(m_air, "sunDir");
            m_aColour    = GetShaderLocation(m_air, "airColour");
            m_aStrength  = GetShaderLocation(m_air, "airStrength");
            m_aFalloff   = GetShaderLocation(m_air, "airFalloff");
        }
    } else {
        TraceLog(LOG_WARNING, "GLOBE: lighting shader did not compile; drawing unlit");
    }
    m_ready = true;
    startSkyBake();
}

void GlobeView::setMonth(int month) {
    m_month = ((month % 12) + 12) % 12;

    // Solar declination for the middle of that month. The standard
    // approximation: delta = -23.44 deg * cos(2*pi*(N + 10)/365), where N is the
    // day of the year. -23.44 is the axial tilt, and the +10 is because the
    // solstice falls ten days before the year turns over.
    const float n = (float)m_month * 30.44f + 15.0f;
    const float decl = -23.44f * DEG2RAD * cosf(2.0f * PI * (n + 10.0f) / 365.0f);

    // Keep the sun's longitude wherever it was -- that is time of day, and it
    // is not this function's business. Only the tilt moves.
    const float lon = atan2f(-m_sun.dir.z, m_sun.dir.x);
    m_sun.dir = { cosf(decl) * cosf(lon), sinf(decl), -cosf(decl) * sinf(lon) };

    // The moon advances a twelfth of its way round. Its phase is then whatever
    // the geometry says, which is the point of not storing a phase at all.
    m_sky.moonLon = m_sky.moonLon + 30.0f * (float)m_month;
    while (m_sky.moonLon > 360.0f) m_sky.moonLon -= 360.0f;
}

bool GlobeView::moonOnScreen(int screenW, int screenH, Vector2& pos, float& radius) const {
    if (!m_ready || !m_sky.moon) return false;
    const Camera3D cam = camera(screenW, screenH);
    const Vector3 at = moonWorld();
    const Vector3 eye = cameraPosition();

    // Behind the camera? Compare against the view direction, not the distance:
    // the moon can be further from the planet than the camera and still be in
    // front of it, or nearer and behind it.
    const Vector3 fwd = Vector3Normalize(Vector3Scale(eye, -1.0f));
    const Vector3 toMoon = Vector3Subtract(at, eye);
    if (Vector3DotProduct(fwd, Vector3Normalize(toMoon)) <= 0.0f) return false;

    // Hidden by the planet? The planet is a unit sphere at the origin; the moon
    // is occluded when the segment eye->moon passes within radius 1 of it.
    const float len = Vector3Length(toMoon);
    const Vector3 dir = Vector3Scale(toMoon, 1.0f / len);
    const float t = -Vector3DotProduct(eye, dir);
    if (t > 0.0f && t < len) {
        const Vector3 closest = Vector3Add(eye, Vector3Scale(dir, t));
        if (Vector3Length(closest) < 1.0f) return false;
    }

    pos = GetWorldToScreenEx(at, cam, screenW, screenH);
    // Radius in pixels: project a point one moon-radius to the side.
    const Vector3 side = Vector3Normalize(Vector3CrossProduct(dir, {0.0f, 1.0f, 0.0f}));
    const Vector2 edge = GetWorldToScreenEx(
        Vector3Add(at, Vector3Scale(side, m_sky.moonSize)), cam, screenW, screenH);
    radius = Vector2Distance(pos, edge);
    return true;
}

void GlobeView::setOccluder(Vector3 pos, float radius) const {
    if (!m_haveShader) return;
    SetShaderValue(m_shader, m_uMoonPos, &pos, SHADER_UNIFORM_VEC3);
    SetShaderValue(m_shader, m_uMoonR, &radius, SHADER_UNIFORM_FLOAT);
}

Vector3 GlobeView::moonWorld() const {
    // Computed, not remembered. It used to be a member written during drawSky,
    // and the planet's uniforms were uploaded BEFORE drawSky ran -- so the
    // eclipse read last frame's moon, and on the first frame read the origin,
    // which put the moon at the centre of the planet and produced no shadow at
    // all. A position that is a side effect of drawing is a position that is
    // wrong for anything that draws first.
    const float mlon = m_sky.moonLon * DEG2RAD, mlat = m_sky.moonLat * DEG2RAD;
    return { m_sky.moonDistance * cosf(mlat) * cosf(mlon),
             m_sky.moonDistance * sinf(mlat),
            -m_sky.moonDistance * cosf(mlat) * sinf(mlon) };
}

void GlobeView::setSky(const Sky& s) {
    m_sky = s;
    m_skyBuilt = false;      // counts and densities changed: regenerate
    m_bake.reset();          // and the bake in flight was for the old settings
    startSkyBake();
}

namespace {

/// The three sky images, as pixels. No GPU in here at all -- that is the point.
struct SkyImages { Image stars{}, cloud{}, moon{}, glow{}; };

/**
 * Paint the sky textures. PURE CPU, and safe to run off the render thread.
 *
 * Seventy-five million distance tests for the Worley fields alone, which is a
 * visible hitch if it happens on the frame the globe is opened. Nothing here
 * touches GL, raylib's Image functions being plain memory work, so it can be
 * handed to a worker and collected later.
 */
void bakeSky(const GlobeView::Sky& sky, SkyImages& out) {


    // ── Stars ──
    //
    // GENERATED, not an art asset. A star map is a few thousand bright pixels
    // on black, and shipping a 4 MB PNG of that would be four megabytes of
    // noise in every download for something a loop writes in a millisecond.
    // Seeded fixed, so the sky is the same every session -- a constellation
    // that moved between loads would be noticed.
    // ── Sized for the platform, and the web is not the desktop ──
    //
    // On desktop this bake runs on a real thread and nobody waits for it. The
    // web build has no pthreads, so odasync QUEUES it and runs it from the main
    // loop -- which means every one of these pixels is paid for on a frame the
    // browser is trying to render. Seventy-five million distance tests there is
    // not a hitch, it is a hung tab.
    //
    // Halved in each dimension is a quarter of the work, and at the size a
    // globe occupies on a phone screen it is not a difference anyone can see.
    //
    // All four sizes stay POWERS OF TWO. WebGL 1 refuses to wrap or mipmap a
    // non-power-of-two texture, and the cloud sheet has to wrap.
#if defined(PLATFORM_WEB) || defined(PLATFORM_ANDROID) || defined(GRAPHICS_API_OPENGL_ES2)
    const int SW = 1024, SH = 512;
#else
    const int SW = 2048, SH = 1024;
#endif
    Image stars = GenImageColor(SW, SH, BLACK);
    unsigned int seed = 0x5EED1234u;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) & 0xFFFF; };

    // A little smooth noise of its own: fbm below belongs to the cloud section
    // and is not in scope yet, and the band needs SMOOTH variation. Per-pixel
    // random is not mottling, it is white noise, and it reads as television
    // static rather than as unresolved stars.
    auto smooth2 = [](float x, float y) {
        auto h = [](int a, int b) {
            unsigned int n = (unsigned int)(a * 374761393 + b * 668265263);
            n = (n ^ (n >> 13)) * 1274126177u;
            return (float)((n ^ (n >> 16)) & 0xFFFF) / 65535.0f;
        };
        const int x0 = (int)floorf(x), y0 = (int)floorf(y);
        const float fx = x - x0, fy = y - y0;
        auto sm = [](float t) { return t * t * (3.0f - 2.0f * t); };
        const float a = h(x0, y0), b = h(x0 + 1, y0), c = h(x0, y0 + 1), d = h(x0 + 1, y0 + 1);
        const float top = a + (b - a) * sm(fx), bot = c + (d - c) * sm(fx);
        return top + (bot - top) * sm(fy);
    };

    // A faint band across the sky. Without it the field is uniform, and a
    // uniform scatter of points reads as static rather than as a galaxy seen
    // edge-on -- which is the single most recognisable thing about a night sky.
    for (int y = 0; y < SH; ++y) {
        for (int x = 0; x < SW; ++x) {
            const float u = (float)x / (float)SW;
            const float v = (float)y / (float)SH;
            // A great circle tilted off the equator, so it crosses the sheet
            // diagonally rather than running along a row.
            const float band = sinf(u * 2.0f * PI * 1.0f + 0.7f) * 0.16f + 0.5f;
            const float d = fabsf(v - band);
            float g = expf(-(d * d) / 0.0016f) * 0.055f;
            // Mottled, or it is a painted stripe.
            // Gently mottled. The first pass multiplied by a fresh random
            // number PER PIXEL, which is not mottling -- it is white noise, and
            // it read as television static rather than as unresolved stars.
            g *= 0.62f + 0.55f * smooth2((float)x / SW * 26.0f, (float)y / SH * 13.0f);
            if (g > 0.004f) {
                const unsigned char c = (unsigned char)std::clamp(g * 255.0f * sky.starBrightness, 0.0f, 255.0f);
                ImageDrawPixel(&stars, x, y, Color{c, c, (unsigned char)std::min(255, c + 6), 255});
            }
        }
    }

    for (int i = 0; i < sky.starCount; ++i) {
        const int x = (int)(rnd() % SW);
        // Uniform in sin(latitude), not in latitude: an equirectangular sheet
        // stretches enormously at the poles, and uniform rows would pile the
        // sky up above both of them.
        const float vv = ((float)(rnd() % 10000) / 10000.0f) * 2.0f - 1.0f;
        const int y = (int)(((asinf(vv) / PI) + 0.5f) * (float)SH) % SH;

        const float t = (float)(rnd() % 1000) / 1000.0f;
        const float mag = (t * t * t) * 0.85f + 0.15f;
        const float b = std::clamp(mag * 255.0f * sky.starBrightness, 0.0f, 255.0f);

        // COLOUR, because real stars have it: hot ones blue-white, cool ones
        // orange. A field of pure white points is the tell of a generated sky.
        const float hue = (float)(rnd() % 1000) / 1000.0f;
        float r = b, g = b, bl = b;
        if (hue < 0.28f)      { r = b * 0.80f; g = b * 0.88f; }            // blue-white
        else if (hue > 0.76f) { bl = b * 0.72f; g = b * 0.88f; }           // orange
        auto put = [&](int px, int py, float k) {
            px = ((px % SW) + SW) % SW;
            if (py < 0 || py >= SH) return;
            const Color o = GetImageColor(stars, px, py);
            ImageDrawPixel(&stars, px, py, Color{
                (unsigned char)std::min(255, (int)(o.r + r * k)),
                (unsigned char)std::min(255, (int)(o.g + g * k)),
                (unsigned char)std::min(255, (int)(o.b + bl * k)), 255});
        };
        put(x, y, 1.0f);
        // The brightest few get a little bleed, so magnitude reads as SIZE and
        // not only as brightness -- which is how the eye actually sorts stars.
        if (mag > 0.62f) {
            put(x + 1, y, 0.45f); put(x - 1, y, 0.45f);
            put(x, y + 1, 0.45f); put(x, y - 1, 0.45f);
        }
    }

    out.stars = stars;

    // ── Cloud ──
    //
    // Value noise, summed over a few octaves, as an ALPHA field on white. Also
    // generated: a cloud sheet is the sort of texture that is either procedural
    // or a large photograph, and a photograph would fight the map's flat
    // colour.
#if defined(PLATFORM_WEB) || defined(PLATFORM_ANDROID) || defined(GRAPHICS_API_OPENGL_ES2)
    const int CW = 1024, CH = 512;
#else
    const int CW = 2048, CH = 1024;
#endif
    Image cloud = GenImageColor(CW, CH, Color{255, 255, 255, 0});
    auto hash = [](int x, int y) {
        unsigned int h = (unsigned int)(x * 374761393 + y * 668265263);
        h = (h ^ (h >> 13)) * 1274126177u;
        return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
    };
    auto noise = [&](float x, float y, int period) {
        const int x0 = (int)floorf(x), y0 = (int)floorf(y);
        const float fx = x - x0, fy = y - y0;
        auto sm = [](float t) { return t * t * (3.0f - 2.0f * t); };
        const float a = hash(x0 % period, y0), b = hash((x0 + 1) % period, y0);
        const float c = hash(x0 % period, y0 + 1), d = hash((x0 + 1) % period, y0 + 1);
        return (a + (b - a) * sm(fx)) + ((c + (d - c) * sm(fx)) - (a + (b - a) * sm(fx))) * sm(fy);
    };
    // Six octaves with DOMAIN WARPING -- the field is sampled at coordinates
    // that are themselves displaced by a coarser field. Plain summed octaves
    // give evenly speckled cotton wool; warping is what produces the sheared,
    // stretched fronts that read as weather rather than as noise.
    // ── Worley (cellular) noise ──
    //
    // Value noise, however many octaves you sum, is smooth: it has no edges,
    // and cloud is nothing but edges. A cumulus field is a scatter of discrete
    // cells with clear air between them, and that is exactly what a distance-to-
    // nearest-feature-point field gives you. Summed noise decides WHERE the
    // weather is; this decides what it is made of.
    //
    // Tiles in x, because the sheet wraps around the planet and a seam down the
    // antimeridian would be the first thing anyone noticed.
    auto worley = [&](float x, float y, int period) {
        const float fx = x * (float)period;
        const float fy = y * (float)period * 0.5f;   // equirectangular: half the rows
        const int ix = (int)floorf(fx), iy = (int)floorf(fy);
        float best = 1e9f;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = ix + dx, cy = iy + dy;
                const int wx = ((cx % period) + period) % period;
                const float jx = hash(wx, cy);
                const float jy = hash(wx + 7919, cy + 104729);
                const float ddx = ((float)cx + jx) - fx;
                const float ddy = ((float)cy + jy) - fy;
                const float d = ddx * ddx + ddy * ddy;
                if (d < best) best = d;
            }
        }
        return sqrtf(best);
    };

    auto fbm = [&](float x, float y, int per, int oct) {
        float v = 0.0f, amp = 0.5f;
        for (int o = 0; o < oct; ++o) {
            v += noise(x * per, y * per * 0.5f, per) * amp;
            amp *= 0.5f; per *= 2;
        }
        return v;
    };
    // ── Storm centres ──
    //
    // The thing a satellite photograph has that summed noise never does is
    // ROTATION. Real weather is organised around lows: banks curl into hooks and
    // spirals, and the eye reads that instantly as weather rather than as
    // texture. Noise alone -- however many octaves, however warped -- gives torn
    // wool, because nothing in it turns.
    //
    // So the field is sampled through a rotation about a handful of centres,
    // strongest at the middle and falling to nothing at the edge. Signed, since
    // lows spin one way north of the equator and the other way south.
    struct Storm { float x, y, r, k; };
    Storm storms[18];
    for (auto& st : storms) {
        st.x = (float)(rnd() % 1000) / 1000.0f;
        // Away from the equator: cyclones need the Coriolis effect to organise,
        // and putting one on the line looks wrong to anybody who has seen a
        // weather map.
        const float band = 0.12f + 0.26f * (float)(rnd() % 1000) / 1000.0f;
        const bool north = (rnd() & 1) != 0;
        st.y = north ? 0.5f - band : 0.5f + band;
        st.r = 0.09f + 0.13f * (float)(rnd() % 1000) / 1000.0f;
        // Under a radian at the core. The first attempt used up to 4.6, which is
        // 260 degrees of rotation, and that does not spiral a cloud bank -- it
        // smears it into taffy. A cyclone's arms are a gentle shear applied to
        // structure that was already there.
        st.k = (north ? 1.0f : -1.0f) * (0.55f + 0.55f * (float)(rnd() % 1000) / 1000.0f);
    }

    for (int y = 0; y < CH; ++y) {
        const float fy = (float)y / (float)CH;
        const float lat = fy * PI - PI * 0.5f;
        const float polar = powf(cosf(lat), 0.45f);

        const float d = lat * RAD2DEG;
        const float itcz  = expf(-(d * d) / 150.0f) * 1.00f;
        const float storm = expf(-((fabsf(d) - 52.0f) * (fabsf(d) - 52.0f)) / 420.0f) * 0.95f;
        const float dry   = expf(-((fabsf(d) - 25.0f) * (fabsf(d) - 25.0f)) / 200.0f) * 0.60f;
        const float band  = std::clamp(0.42f + itcz + storm - dry, 0.0f, 1.6f);

        for (int x = 0; x < CW; ++x) {
            float fx = (float)x / (float)CW;
            float sx = fx, sy = fy;

            for (const Storm& st : storms) {
                float dx = sx - st.x;
                if (dx >  0.5f) dx -= 1.0f;       // the sheet wraps
                if (dx < -0.5f) dx += 1.0f;
                const float dy = (sy - st.y) * 2.0f;   // equirectangular: y is twice as tall
                const float rr = sqrtf(dx * dx + dy * dy);
                if (rr >= st.r) continue;
                const float f = 1.0f - rr / st.r;
                const float th = st.k * f * f;
                const float c = cosf(th), sn = sinf(th);
                sx = st.x + (dx * c - dy * sn);
                sy = st.y + (dx * sn + dy * c) * 0.5f;
            }

            const float w1x = fbm(sx + 0.31f, sy + 0.17f, 9, 3);
            const float w1y = fbm(sx + 0.77f, sy + 0.61f, 9, 3);
            const float ax = sx + (w1x - 0.5f) * 0.10f;
            const float ay = sy + (w1y - 0.5f) * 0.05f;
            const float w2x = fbm(ax + 0.11f, ay + 0.43f, 22, 3);
            const float w2y = fbm(ax + 0.53f, ay + 0.07f, 22, 3);
            const float bx = ax + (w2x - 0.5f) * 0.045f;
            const float by = ay + (w2y - 0.5f) * 0.022f;

            const float mass = fbm(bx, by, 34, 6);

            // Two scales of cell. The coarse one breaks a bank into separate
            // masses; the fine one is the stipple you see in a cumulus field.
            // Inverted, so a feature point is the MIDDLE of a cloud rather than
            // a hole in one.
            const float cellBig  = 1.0f - std::clamp(worley(bx, by, 40) * 1.45f, 0.0f, 1.0f);
            // TWO fine scales, multiplied. One Worley field alone has cells of
            // a single size sitting on a regular lattice, and the eye finds that
            // lattice immediately -- it reads as a mesh, not as weather. Two
            // periods that do not divide each other break it up.
            const float cf1 = 1.0f - std::clamp(
                worley(bx * 1.7f + 3.3f, by * 1.7f + 1.1f, 118) * 1.65f, 0.0f, 1.0f);
            const float cf2 = 1.0f - std::clamp(
                worley(bx * 2.6f + 6.9f, by * 2.6f + 4.7f, 173) * 1.55f, 0.0f, 1.0f);
            const float cellFine = std::clamp(cf1 * 0.65f + cf2 * 0.55f, 0.0f, 1.0f);

            // Filaments: the SECOND-nearest minus the nearest would be the
            // textbook way to draw cell walls, but a cheap stand-in is a very
            // stretched cell field, which reads as the drawn-out cirrus streaks
            // a jet stream leaves behind.
            const float streak = 1.0f - std::clamp(
                worley(bx * 0.35f + 8.7f, by * 3.4f + 5.2f, 26) * 1.9f, 0.0f, 1.0f);

            // TWO TIERS, because a photograph has two. A thin veil covers a lot
            // of ground at low opacity; bright cores cover much less at high
            // opacity. One threshold gives a uniform grey wash, which is the
            // haze this looked like before.
            const float m = mass - 0.50f + 0.13f * (band - 0.7f);
            float veil = std::clamp((m + 0.115f) * 3.2f, 0.0f, 1.0f) * 0.44f;
            float core = std::clamp((m + 0.020f) * 6.2f, 0.0f, 1.0f);
            // The cells CUT the core rather than tint it: multiplying by a
            // field that reaches zero is what puts clear sky between the
            // cloudlets, and clear sky between them is the whole look.
            core *= (0.26f + 0.88f * cellBig) * (0.42f + 0.80f * cellFine);
            veil *= 0.45f + 0.70f * streak;

            float a = std::clamp(veil + core, 0.0f, 1.0f);
            a = a * a * (3.0f - 2.0f * a);
            ImageDrawPixel(&cloud, x, y,
                           Color{255, 255, 255, (unsigned char)(a * polar * 255.0f)});
        }
    }

    out.cloud = cloud;

    // ── The moon ──
    //
    // A plain tinted ball reads as a placeholder, and it is the one object in
    // the sky a player looks straight at. Three things make it a moon: a mottled
    // base, dark MARIA -- the smooth basalt plains that give the near side its
    // face -- and craters with bright rims and darker floors. All procedural,
    // for the same reason the stars are.
#if defined(PLATFORM_WEB) || defined(PLATFORM_ANDROID) || defined(GRAPHICS_API_OPENGL_ES2)
    const int MWt = 512, MHt = 256;
#else
    const int MWt = 1024, MHt = 512;
#endif
    Image moon = GenImageColor(MWt, MHt, Color{176, 172, 164, 255});
    for (int y = 0; y < MHt; ++y) {
        for (int x = 0; x < MWt; ++x) {
            const float g = fbm((float)x / MWt, (float)y / MHt, 6, 5);
            const int v = 150 + (int)(g * 62.0f);
            const unsigned char c = (unsigned char)std::clamp(v, 0, 255);
            ImageDrawPixel(&moon, x, y, Color{c, (unsigned char)(c - 3), (unsigned char)(c - 9), 255});
        }
    }
    // Maria: a handful of large, soft, dark basins.
    for (int i = 0; i < 9; ++i) {
        const int cx = (int)(rnd() % MWt), cy = MHt / 4 + (int)(rnd() % (MHt / 2));
        const int r = 40 + (int)(rnd() % 90);
        for (int y = cy - r; y <= cy + r; ++y) {
            if (y < 0 || y >= MHt) continue;
            for (int x = cx - r; x <= cx + r; ++x) {
                const float d = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy)));
                if (d > r) continue;
                const float t = 1.0f - d / r;
                const int xx = ((x % MWt) + MWt) % MWt;
                Color p = GetImageColor(moon, xx, y);
                const float k = 1.0f - 0.34f * t * t;
                ImageDrawPixel(&moon, xx, y,
                               Color{(unsigned char)(p.r * k), (unsigned char)(p.g * k),
                                     (unsigned char)(p.b * k), 255});
            }
        }
    }
    // Craters: a bright rim and a slightly darker floor. Small ones outnumber
    // large ones heavily, which is what makes a cratered surface look right.
    for (int i = 0; i < 260; ++i) {
        const int cx = (int)(rnd() % MWt), cy = (int)(rnd() % MHt);
        const float t = (float)(rnd() % 1000) / 1000.0f;
        const int r = 2 + (int)(t * t * t * 34.0f);
        for (int y = cy - r - 1; y <= cy + r + 1; ++y) {
            if (y < 0 || y >= MHt) continue;
            for (int x = cx - r - 1; x <= cx + r + 1; ++x) {
                const float d = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy)));
                if (d > r + 1) continue;
                const int xx = ((x % MWt) + MWt) % MWt;
                Color p = GetImageColor(moon, xx, y);
                const float k = (d > r - 1.2f) ? 1.16f : (0.90f + 0.08f * (d / r));
                ImageDrawPixel(&moon, xx, y,
                               Color{(unsigned char)std::clamp((int)(p.r * k), 0, 255),
                                     (unsigned char)std::clamp((int)(p.g * k), 0, 255),
                                     (unsigned char)std::clamp((int)(p.b * k), 0, 255), 255});
            }
        }
    }
    out.moon = moon;

    // ── The sun's glare ──
    //
    // A bare disc reads as a sticker. What makes a star look like one is the
    // halo around it -- the eye expects light to spill. This is a radial
    // falloff drawn additively, which is a cheap stand-in for the bloom a real
    // camera would produce, and it needs no post-processing pass.
    const int GS = 256;
    Image glow = GenImageColor(GS, GS, Color{0, 0, 0, 0});
    for (int y = 0; y < GS; ++y) {
        for (int x = 0; x < GS; ++x) {
            const float dx = (float)x / GS * 2.0f - 1.0f;
            const float dy = (float)y / GS * 2.0f - 1.0f;
            const float r = sqrtf(dx * dx + dy * dy);
            if (r > 1.0f) continue;
            // Two terms: a tight core and a wide skirt. One falloff alone is
            // either a hard dot or a soft smudge; real glare is both at once.
            const float a = powf(std::max(0.0f, 1.0f - r), 6.0f) * 0.85f
                          + powf(std::max(0.0f, 1.0f - r), 1.6f) * 0.16f;
            ImageDrawPixel(&glow, x, y, Color{255, 250, 236,
                           (unsigned char)std::clamp(a * 255.0f, 0.0f, 255.0f)});
        }
    }
    out.glow = glow;

}


}  // namespace

/// What the worker writes and the render thread collects.
struct GlobeView::Bake {
    std::mutex lock;
    SkyImages img{};
    bool done = false;
};

void GlobeView::startSkyBake() {
    // Started when the sky is SET, not when it is first drawn. Lazily kicking
    // it off inside drawSky meant the work began on the frame the globe opened
    // -- exactly the frame it was moved off the main thread to protect. A map
    // is loaded long before anyone presses F7, so by then it is usually done.
    if (m_bake || m_skyBuilt) return;
    m_bake = std::make_shared<Bake>();
    // The worker holds a shared_ptr, so it cannot outlive its own storage even
    // if this GlobeView is destroyed while the bake is still running.
    auto bake = m_bake;
    const Sky sky = m_sky;
    odasync::run([bake, sky]() {
        SkyImages img;
        bakeSky(sky, img);
        std::lock_guard<std::mutex> g(bake->lock);
        bake->img = img;
        bake->done = true;
    });
}

void GlobeView::buildSkyTextures() {
    // Collect only: startSkyBake put the work in flight.
    startSkyBake();
    if (!m_bake) return;

    SkyImages ready{};
    {
        std::lock_guard<std::mutex> g(m_bake->lock);
        if (!m_bake->done) return;          // still painting; try again next frame
        ready = m_bake->img;
        m_bake->img = SkyImages{};
    }
    m_bake.reset();

    // Upload on the render thread, which is the only thread allowed to.
    if (m_starTex.id > 0) UnloadTexture(m_starTex);
    if (m_cloudTex.id > 0) UnloadTexture(m_cloudTex);
    if (m_moonTex.id > 0) UnloadTexture(m_moonTex);
    m_starTex  = LoadTextureFromImage(ready.stars);
    m_cloudTex = LoadTextureFromImage(ready.cloud);
    m_moonTex  = LoadTextureFromImage(ready.moon);
    m_glowTex  = LoadTextureFromImage(ready.glow);
    SetTextureFilter(m_starTex, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(m_cloudTex, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(m_moonTex, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(m_glowTex, TEXTURE_FILTER_BILINEAR);
    UnloadImage(ready.stars);
    UnloadImage(ready.cloud);
    UnloadImage(ready.moon);
    UnloadImage(ready.glow);
    m_skyBuilt = true;
}

void GlobeView::drawSky(const Camera3D& cam) {
    // Asks for the bake and returns whether it has landed. Everything below
    // already checks its own texture for id 0, so a sky that is still being
    // painted simply draws nothing rather than drawing wrong.
    if (!m_skyBuilt) buildSkyTextures();

    // ── The star dome ──
    //
    // The SAME sphere mesh, scaled out and seen from inside. Reusing it means
    // one piece of geometry for the planet, the cloud and the sky, and the star
    // dome inherits the mesh's UVs so a map can supply its own star sheet in
    // exactly the layout the political raster uses.
    if (m_sky.stars && m_starTex.id > 0) {
        Material sky = LoadMaterialDefault();
        sky.maps[MATERIAL_MAP_DIFFUSE].texture = m_starTex;
        rlDisableBackfaceCulling();     // we are inside it
        rlDisableDepthMask();           // and it must never occlude the planet
        DrawMesh(m_mesh, sky, MatrixScale(-60.0f, 60.0f, 60.0f));
        rlEnableDepthMask();
        rlEnableBackfaceCulling();
        if (sky.maps) MemFree(sky.maps);
    }

    // ── The moon ──
    //
    // A small sphere rather than a billboard, so it takes the SAME light as the
    // planet and shows a phase for free. A billboard would need its own
    // crescent art and would have to be told where the sun is anyway.
    if (m_sky.moon) {
        const Vector3 at = moonWorld();
        Material moon = LoadMaterialDefault();
        if (m_haveShader) moon.shader = m_shader;
        if (m_moonTex.id > 0) moon.maps[MATERIAL_MAP_DIFFUSE].texture = m_moonTex;
        moon.maps[MATERIAL_MAP_DIFFUSE].color = m_sky.moonColour;
        const Matrix m = MatrixMultiply(MatrixScale(m_sky.moonSize, m_sky.moonSize, m_sky.moonSize),
                                        MatrixTranslate(at.x, at.y, at.z));
        // The planet, at the origin, radius one. This is the lunar eclipse, and
        // it is the same term the planet uses against the moon -- which is why
        // it works rather than being a second implementation nobody exercised.
        setOccluder({0.0f, 0.0f, 0.0f}, m_lit ? 1.0f : 0.0f);
        // No cloud term for the moon: it is a quarter of a million miles from
        // this planet's weather, and leaving the shell radius set would print
        // the map's cloud onto it.
        const float none = 0.0f;
        SetShaderValue(m_shader, m_uCloudR, &none, SHADER_UNIFORM_FLOAT);
        DrawMesh(m_mesh, moon, m);
        setOccluder(moonWorld(), (m_sky.moon && m_lit) ? m_sky.moonSize : 0.0f);

        // The cloud shell, so the ground can be shadowed by it. Radius 0 when
        // there is no cloud, which switches the whole term off in the shader.
        const float cr = (m_sky.cloud && m_cloudTex.id > 0 && m_lit)
                       ? 1.0f + m_sky.airHeight * std::clamp(m_sky.cloudHeight, 0.0f, 1.0f)
                       : 0.0f;
        const float camt = std::clamp(m_sky.cloudOpacity, 0.0f, 1.0f);
        SetShaderValue(m_shader, m_uCloudR, &cr, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_uCloudRot, &m_cloudPhase, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_uCloudAmt, &camt, SHADER_UNIFORM_FLOAT);
        if (m_cloudTex.id > 0) SetShaderValueTexture(m_shader, m_uCloudTex, m_cloudTex);
        if (moon.maps) MemFree(moon.maps);
    }

    // ── The sun, as a thing you can see ──
    //
    // Sun{} says where the light COMES FROM; this is whether the source itself
    // is in the sky when you turn towards it. Drawn unlit on purpose -- lighting
    // the sun with its own light would shade the half facing away from itself.
    if (m_sky.sunDisc) {
        const Vector3 d = Vector3Normalize(m_sun.dir);
        const Vector3 at = Vector3Scale(d, m_sky.sunDistance);
        Material disc = LoadMaterialDefault();     // default shader: emissive, flat
        disc.maps[MATERIAL_MAP_DIFFUSE].color = m_sun.colour;
        const Matrix m = MatrixMultiply(
            MatrixScale(m_sky.sunSize, m_sky.sunSize, m_sky.sunSize),
            MatrixTranslate(at.x, at.y, at.z));
        DrawMesh(m_mesh, disc, m);
        if (disc.maps) MemFree(disc.maps);

        // ── Glare ──
        //
        // Nested shells rather than a billboard. A camera-facing quad was the
        // obvious build and drew nothing here, and rather than keep debugging a
        // sprite this reuses the one mesh that is already proven in this file:
        // three additive spheres, each larger and fainter, which is a radial
        // falloff built out of geometry. It also cannot face the wrong way.
        if (m_haveGlow) {
            const Vector3 eye = cameraPosition();
            const Vector3 gc{m_sun.colour.r / 255.0f * 0.55f,
                             m_sun.colour.g / 255.0f * 0.55f,
                             m_sun.colour.b / 255.0f * 0.55f};
            const float fall = 2.4f;
            SetShaderValue(m_glow, m_gView, &eye, SHADER_UNIFORM_VEC3);
            SetShaderValue(m_glow, m_gColour, &gc, SHADER_UNIFORM_VEC3);
            SetShaderValue(m_glow, m_gFalloff, &fall, SHADER_UNIFORM_FLOAT);
            Material halo = LoadMaterialDefault();
            halo.shader = m_glow;
            const float rs = m_sky.sunSize * 5.5f;
            BeginBlendMode(BLEND_ADDITIVE);
            rlDisableDepthMask();
            DrawMesh(m_mesh, halo, MatrixMultiply(MatrixScale(rs, rs, rs),
                                                  MatrixTranslate(at.x, at.y, at.z)));
            rlEnableDepthMask();
            EndBlendMode();
            if (halo.maps) MemFree(halo.maps);
        }
    }
    (void)cam;
}

void GlobeView::setSun(const Sun& s)     { m_sun = s; }
void GlobeView::setNight(const Night& n) { m_night = n; }

GlobeView::~GlobeView() {
    if (!m_ready) return;
    // The material's default shader and its 1x1 white texture belong to raylib,
    // not to us: UnloadMaterial would take the shader every other material is
    // still using. Only the maps array is ours to release.
    if (m_starTex.id > 0) UnloadTexture(m_starTex);
    if (m_cloudTex.id > 0) UnloadTexture(m_cloudTex);
    if (m_moonTex.id > 0) UnloadTexture(m_moonTex);
    if (m_glowTex.id > 0) UnloadTexture(m_glowTex);
    if (m_haveGlow) UnloadShader(m_glow);
    if (m_haveAir) UnloadShader(m_air);
    if (m_haveShader) UnloadShader(m_shader);
    if (m_material.maps) MemFree(m_material.maps);
    UnloadMesh(m_mesh);
}

Vector3 GlobeView::cameraPosition() const {
    return globe::eyeFromOrbit(m_lat, m_lon, m_dist);
}

Camera3D GlobeView::camera(int screenW, int screenH) const {
    (void)screenW; (void)screenH;
    Camera3D c{};
    c.position   = cameraPosition();
    c.target     = { 0.0f, 0.0f, 0.0f };
    c.up         = { 0.0f, 1.0f, 0.0f };
    c.fovy       = kFovY;
    c.projection = CAMERA_PERSPECTIVE;
    return c;
}

Vector3 GlobeView::pixelToUnit(float px, float py) const {
    return globe::unitFromPixel(px, py, m_mapW, m_mapH);
}

void GlobeView::draw(int screenW, int screenH) {
    if (!m_ready || m_surface.id == 0) return;
    m_material.maps[MATERIAL_MAP_DIFFUSE].texture = m_surface;
    m_material.maps[MATERIAL_MAP_DIFFUSE].color   = WHITE;

    if (m_haveShader) {
        // Uniforms every frame rather than on change: five floats is nothing
        // next to a draw call, and a cached "dirty" flag here would be one more
        // thing to forget when a setting moves.
        const Vector3 d = Vector3Normalize(m_lit ? m_sun.dir : Vector3{0.0f, 0.0f, 0.0f});
        const float night = m_lit ? m_night.floorLevel : 1.0f;
        const float strength = m_lit ? m_sun.strength : 1.0f;
        const float soft = m_night.softness > 0.001f ? m_night.softness : 0.001f;
        const Vector3 col{m_sun.colour.r / 255.0f, m_sun.colour.g / 255.0f,
                          m_sun.colour.b / 255.0f};
        SetShaderValue(m_shader, m_uSunDir, &d, SHADER_UNIFORM_VEC3);
        SetShaderValue(m_shader, m_uSunColour, &col, SHADER_UNIFORM_VEC3);
        SetShaderValue(m_shader, m_uSunStrength, &strength, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_uNightFloor, &night, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_shader, m_uSoftness, &soft, SHADER_UNIFORM_FLOAT);
        // Only the moon is near enough and large enough to eclipse the ground.
        setOccluder(moonWorld(), (m_sky.moon && m_lit) ? m_sky.moonSize : 0.0f);
    }

    const Camera3D cam = camera(screenW, screenH);
    BeginMode3D(cam);
    drawSky(cam);
    DrawMesh(m_mesh, m_material, MatrixIdentity());

    // ── Cloud ──
    //
    // A shell a whisker larger than the planet, turning slowly. Lit by the same
    // shader, so cloud on the night side goes dark with the ground under it
    // rather than glowing.
    if (m_sky.cloud && m_cloudTex.id > 0 && m_sky.cloudOpacity > 0.001f) {
        // Cloud sits INSIDE the atmosphere, at a fraction of its height, rather
        // than pasted on the ground. It is the difference between weather in an
        // air column and a second coat of paint: the limb shows sky above the
        // cloud tops, which is what the eye reads as depth.
        const float r = 1.0f + m_sky.airHeight * std::clamp(m_sky.cloudHeight, 0.0f, 1.0f);
        Material cl = LoadMaterialDefault();
        if (m_haveShader) cl.shader = m_shader;
        cl.maps[MATERIAL_MAP_DIFFUSE].texture = m_cloudTex;
        // Thicker close in. From orbit you see the pattern of the weather and
        // want the ground through it; down near the surface you are looking
        // along a much longer path through the same air, and cloud should read
        // as something you are under rather than a decal over the map. One
        // multiplier, tied to camera distance, doing what the atmosphere would
        // do if this integrated one.
        const float t = std::clamp((kMaxDist - m_dist) / (kMaxDist - kMinDist), 0.0f, 1.0f);
        const float thick = std::clamp(m_sky.cloudOpacity * (0.72f + 0.85f * t * t),
                                       0.0f, 1.0f);
        cl.maps[MATERIAL_MAP_DIFFUSE].color = ColorAlpha(WHITE, thick);
        rlDisableDepthMask();     // transparent: depth-test, do not depth-write
        DrawMesh(m_mesh, cl, MatrixMultiply(MatrixScale(r, r, r),
                                            MatrixRotateY(m_cloudPhase)));
        rlEnableDepthMask();
        if (cl.maps) MemFree(cl.maps);
    }

    // ── Atmosphere, last, over everything it surrounds ──
    if (m_sky.atmosphere && m_haveAir && m_sky.airStrength > 0.001f) {
        const float r = 1.0f + m_sky.airHeight;
        const Vector3 eye = cameraPosition();
        const Vector3 sd = Vector3Normalize(m_lit ? m_sun.dir : Vector3{0.0f, 1.0f, 0.0f});
        const Vector3 col{m_sky.airColour.r / 255.0f, m_sky.airColour.g / 255.0f,
                          m_sky.airColour.b / 255.0f};
        SetShaderValue(m_air, m_aViewPos, &eye, SHADER_UNIFORM_VEC3);
        SetShaderValue(m_air, m_aSunDir, &sd, SHADER_UNIFORM_VEC3);
        SetShaderValue(m_air, m_aColour, &col, SHADER_UNIFORM_VEC3);
        SetShaderValue(m_air, m_aStrength, &m_sky.airStrength, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_air, m_aFalloff, &m_sky.airFalloff, SHADER_UNIFORM_FLOAT);
        Material air = LoadMaterialDefault();
        air.shader = m_air;
        rlDisableDepthMask();
        BeginBlendMode(BLEND_ADDITIVE);   // light added to the sky, not painted over it
        DrawMesh(m_mesh, air, MatrixScale(r, r, r));
        EndBlendMode();
        rlEnableDepthMask();
        if (air.maps) MemFree(air.maps);
    }
    EndMode3D();
}

void GlobeView::orbit(float dx, float dy) {
    // Scale with distance so a drag moves the same amount of ground whether you
    // are close in or far out. Without this the globe is unusably fast when
    // zoomed in and unusably slow when zoomed out.
    const float scale = 0.0045f * (m_dist / 3.0f);
    m_lon -= dx * scale;
    m_lat += dy * scale;
    m_lat = std::clamp(m_lat, -kLatLimit, kLatLimit);
    // Keep longitude bounded rather than letting it grow without limit across a
    // long session; float precision degrades visibly once it is large.
    if (m_lon >  PI) m_lon -= 2.0f * PI;
    if (m_lon < -PI) m_lon += 2.0f * PI;
}

void GlobeView::zoom(float amount) {
    // Multiplicative, so each notch covers the same PROPORTION of the remaining
    // distance -- linear steps crawl when far out and slam into the surface when
    // close in.
    m_dist *= powf(0.88f, amount);
    m_dist = std::clamp(m_dist, kMinDist, kMaxDist);
}

void GlobeView::lookAt(float px, float py) {
    const float u = px / (float)m_mapW;
    const float v = py / (float)m_mapH;
    m_lon = -PI + u * 2.0f * PI;
    m_lat = std::clamp(PI * 0.5f - v * PI, -kLatLimit, kLatLimit);
}

bool GlobeView::screenToPixel(float sx, float sy, int screenW, int screenH,
                              int& px, int& py) const {
    if (!m_ready) return false;
    const Camera3D cam = camera(screenW, screenH);
    // The Ex form, deliberately: the plain GetScreenToWorldRay reads
    // GetScreenWidth()/GetScreenHeight() internally, so it would silently
    // use the WINDOW rather than the viewport the map is drawn in, and
    // picking would drift the moment the map is not full-screen.
    const Ray ray = GetScreenToWorldRayEx({sx, sy}, cam, screenW, screenH);

    // Ray against the unit sphere at the origin. Solved here rather than through
    // GetRayCollisionSphere because we want the NEAR hit specifically, and
    // because this has to agree with pixelToUnit to the texel -- one file, one
    // convention.
    const Vector3 o = ray.position;
    const Vector3 d = ray.direction;               // normalised by raylib
    const float b = 2.0f * Vector3DotProduct(o, d);
    const float c = Vector3DotProduct(o, o) - 1.0f;
    const float disc = b * b - 4.0f * c;
    if (disc < 0.0f) return false;                 // missed the planet
    const float sq = sqrtf(disc);
    float t = (-b - sq) * 0.5f;
    if (t < 0.0f) t = (-b + sq) * 0.5f;            // inside the sphere
    if (t < 0.0f) return false;

    const Vector3 p = Vector3Add(o, Vector3Scale(d, t));
    globe::pixelFromUnit(p, m_mapW, m_mapH, px, py);
    return true;
}

bool GlobeView::pixelToScreen(float px, float py, int screenW, int screenH,
                              float& sx, float& sy) const {
    if (!m_ready) return false;
    const Vector3 p = pixelToUnit(px, py);
    const Vector3 eye = cameraPosition();

    // Is the point on the near side? The horizon is where the surface normal is
    // perpendicular to the line of sight. Compared against the EYE rather than
    // the view direction, so it stays correct at the edges of a wide field of
    // view, where those two disagree.
    //
    // For a unit sphere seen from distance d, a point is visible exactly when
    // dot(p, eye) > 1/d. Cheaper than a depth test and exact.
    (void)eye;
    if (!globe::onNearSide(p, m_lat, m_lon, m_dist)) return false;

    const Camera3D cam = camera(screenW, screenH);
    const Vector2 v = GetWorldToScreenEx(p, cam, screenW, screenH);
    sx = v.x;
    sy = v.y;
    return true;
}
