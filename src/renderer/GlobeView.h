#pragma once

// The map as a globe: the same world, wrapped onto a sphere you can turn.
//
// ── WHY THIS IS SMALL ──
//
// The political layer is an EQUIRECTANGULAR raster — the projection a UV sphere
// already wants. So there is no reprojection here and no second copy of the map:
// the texture that draws the flat view wraps this one unchanged.
//
// The same is true of picking. A province is identified by its pixel colour
// (Province::colorToId), so picking on a globe is ray→sphere, sphere→lat/lon,
// lat/lon→pixel, and then the lookup the flat map already does. There is no
// second pick path to keep in step with the first.
//
// ── THE MESH IS GENERATED HERE, ON PURPOSE ──
//
// raylib's GenMeshSphere would do, but its UV convention is its own business and
// could change under us. The picking maths below inverts the texture mapping
// exactly, so the two have to agree to the last texel or provinces select one
// place and draw in another — a bug that looks like a physics problem and is
// really a disagreement about where u=0 is. Generating the mesh here makes that
// agreement a property of one file.
//
// ── WHAT THE CAMERA DOES ──
//
// The sphere never moves. The camera orbits it, which means a point's world
// position is a pure function of its latitude and longitude, and picking needs
// no inverse rotation. Turning the globe is turning the camera.

#include "raylib.h"

#include <memory>

/**
 * The projection, as pure geometry.
 *
 * Split out of the class deliberately. The mesh's UVs and the picking maths are
 * inverses of each other, and if they ever disagree a province selects in one
 * place and draws in another -- which looks like a camera bug and is really a
 * disagreement about where u=0 is. These are the only functions that define
 * that convention, they touch no GPU state, and tests/globe_projection_test.cpp
 * holds them to it round-trip. The class below builds its mesh from these and
 * picks with these, so there is one convention and no second copy to drift.
 *
 * Convention, once: u runs east from longitude -PI, v runs south from the north
 * pole. That is an equirectangular map read top-left to bottom-right, which is
 * the layout of the political raster.
 */
namespace globe {

/// Map pixel -> position on the unit sphere.
Vector3 unitFromPixel(float px, float py, int mapW, int mapH);

/// Unit-sphere position -> map pixel. Clamped into range; never out of bounds.
void pixelFromUnit(Vector3 p, int mapW, int mapH, int& px, int& py);

/// Camera position for an orbit, in sphere radii from the centre.
Vector3 eyeFromOrbit(float lat, float lon, float dist);

/**
 * Is a surface point on the near side?
 *
 * For a unit sphere seen from distance d, the horizon is exactly where
 * dot(p, eye_direction) == 1/d. Exact, and cheaper than a depth test.
 */
bool onNearSide(Vector3 p, float lat, float lon, float dist);

}  // namespace globe

class GlobeView {
public:
    GlobeView(int mapW, int mapH);
    ~GlobeView();
    GlobeView(const GlobeView&) = delete;
    GlobeView& operator=(const GlobeView&) = delete;

    /// The composited map — political base and every overlay above it, already
    /// stacked. Not owned: the compositor keeps it.
    void setSurface(Texture2D tex) { m_surface = tex; }

    void draw(int screenW, int screenH);

    /**
     * Where the sun is, and how hard it is.
     *
     * `dir` points FROM the planet TOWARDS the sun and is normalised here, so a
     * caller can hand over an unnormalised season/time-of-day vector without
     * having to think about it.
     */
    struct Sun {
        Vector3 dir{1.0f, 0.15f, 0.35f};
        Color   colour{255, 247, 232, 255};  ///< very slightly warm, not orange
        float   strength = 1.0f;             ///< multiplier on the lit half
    };

    /**
     * How dark the unlit half goes, and how wide dawn is.
     *
     * `floorLevel` is NOT zero by default and should not be. A fully black night
     * side makes half a player's empire invisible -- borders, unit markers,
     * everything -- and the map is a working document before it is a picture.
     * This setting exists for legibility first and atmosphere second.
     */
    struct Night {
        float floorLevel = 0.34f;   ///< 0 = black, 1 = no night at all
        float softness   = 0.09f;   ///< width of the terminator band, in dot units
    };

    /**
     * The sky a map carries: stars, a moon and cloud.
     *
     * Authored per map rather than global, because a scenario is not always
     * Earth in 1939 -- and the map format already embeds its own data and lets
     * that copy win. Defaults here are the Earth-like ones, so a map that says
     * nothing looks right.
     */
    struct Sky {
        bool  stars = true;
        int   starCount = 1600;
        float starBrightness = 1.0f;

        bool  moon = true;
        Color moonColour{224, 221, 212, 255};
        // Distance matters more than it looks. At a few planet radii the moon
        // swings so wide that it leaves the frustum entirely -- it is BEHIND
        // the camera for most of an orbit, which reads as "the moon does not
        // work". Far away it converges toward the anti-camera direction and
        // stays in the sky, which is also what a real moon does: ours is about
        // sixty Earth radii out.
        float moonSize = 0.75f;       ///< radius, in planet radii; exaggerated
        float moonDistance = 18.0f;   ///< from the planet centre, same units
        float moonLon = 195.0f;       ///< degrees; where it sits in the sky
        float moonLat = 0.0f;

        bool  cloud = true;
        float cloudOpacity = 0.55f;
        float cloudDrift = 0.006f;   ///< radians per second, west to east
        /// Cloud sits INSIDE the atmosphere, not on top of the ground. This is
        /// the fraction of the atmosphere's height it floats at, so raising the
        /// atmosphere lifts the weather with it instead of leaving it stuck to
        /// the surface.
        float cloudHeight = 0.55f;

        // ── Atmosphere ──
        //
        // A shell above the ground whose limb catches the light. It is what
        // makes a textured sphere read as a PLANET rather than a painted ball,
        // and it is cheap: one rim term, no scattering integral.
        bool  atmosphere = true;
        Color airColour{118, 168, 232, 255};
        float airHeight = 0.045f;   ///< shell thickness, in planet radii
        float airStrength = 1.0f;
        float airFalloff = 2.6f;    ///< higher = thinner, harder rim

        // ── The sun as a body ──
        //
        // Distinct from the LIGHT. Sun{} says where the light comes from; this
        // says whether you can see the thing itself when you turn towards it.
        bool  sunDisc = true;
        float sunSize = 3.2f;        ///< radius in planet radii; it is far away
        float sunDistance = 140.0f;
    };
    void setSky(const Sky& s);
    /// Has the sky finished painting? False while the worker is still on it.
    bool skyReady() const { return m_skyBuilt; }

    /**
     * Which month it is, 0 = January.
     *
     * Moves the sun's DECLINATION, not its longitude: longitude is time of day
     * and turns hour to hour, declination is the season and is what a
     * scenario's date actually pins down. In June the northern hemisphere leans
     * into the light and the Arctic never goes dark; in December the reverse.
     *
     * The moon advances a twelfth of its orbit with it, so its PHASE follows --
     * phase is the angle between moon and sun seen from here, so it falls out
     * of the geometry rather than being a setting anyone has to keep in step.
     */
    /**
     * Where the moon lands on screen, and its radius in pixels.
     *
     * False when it is behind the camera or hidden by the planet. Added because
     * three attempts to test the lunar eclipse sampled the planet's disc while
     * believing they had found the moon -- the geometry is easy to reason about
     * wrongly, and the code already knows the answer. Useful beyond the test:
     * anything that wants to label or click the moon needs exactly this.
     */
    bool moonOnScreen(int screenW, int screenH, Vector2& pos, float& radius) const;

    void setMonth(int month);
    int month() const { return m_month; }
    const Sky& sky() const { return m_sky; }
    /// Advance the drifting parts. Safe to skip; nothing else depends on it.
    void update(float dt) { m_cloudPhase += m_sky.cloudDrift * dt; }

    void setSun(const Sun& s);
    void setNight(const Night& n);
    const Sun& sun() const { return m_sun; }
    const Night& night() const { return m_night; }
    /// Lighting off: the flat, unshaded look the map has always had.
    void setLit(bool on) { m_lit = on; }
    bool lit() const { return m_lit; }

    /// Turn the globe. `dx`/`dy` are mouse pixels; the conversion to radians
    /// scales with distance so a drag moves the same amount of SURFACE whether
    /// you are close in or far out.
    void orbit(float dx, float dy);
    /// Positive pulls in. Clamped so the camera can neither enter the sphere
    /// nor retreat until it is a dot.
    void zoom(float amount);
    /// Put a map pixel under the middle of the screen, without moving the camera
    /// distance -- used when switching from the flat view so the same ground
    /// stays in front of you.
    void lookAt(float px, float py);

    /// Screen → map pixel. False when the ray misses the planet entirely, which
    /// is what a click on empty space is.
    bool screenToPixel(float sx, float sy, int screenW, int screenH,
                       int& px, int& py) const;

    /**
     * Map pixel → screen.
     *
     * Returns FALSE when the point is on the far side of the globe. This is the
     * one thing the flat projection never had to say, and the reason it is
     * answered here rather than at each call site: a marker whose province has
     * rotated out of view must not draw, and every caller would otherwise have
     * to work that out for itself and get it subtly wrong.
     */
    bool pixelToScreen(float px, float py, int screenW, int screenH,
                       float& sx, float& sy) const;

    float latitude() const { return m_lat; }
    float longitude() const { return m_lon; }
    float distance() const { return m_dist; }

private:
    Camera3D camera(int screenW, int screenH) const;
    Vector3 cameraPosition() const;
    /// Unit-sphere position of a map pixel. The inverse of the mesh's UVs.
    Vector3 pixelToUnit(float px, float py) const;

    int m_mapW = 1, m_mapH = 1;
    Mesh m_mesh{};
    Material m_material{};
    bool m_ready = false;
    Texture2D m_surface{};
    Shader m_shader{};
    bool m_haveShader = false;
    int m_uSunDir = -1, m_uSunColour = -1, m_uSunStrength = -1;
    int m_uNightFloor = -1, m_uSoftness = -1;
    int m_uMoonPos = -1, m_uMoonR = -1;
    int m_uCloudTex = -1, m_uCloudR = -1, m_uCloudRot = -1, m_uCloudAmt = -1;
    Sun m_sun{};
    Night m_night{};
    bool m_lit = true;

    Sky m_sky{};
    Texture2D m_starTex{};      ///< generated, not loaded: no art asset for this
    Texture2D m_cloudTex{};
    Texture2D m_moonTex{};
    Texture2D m_glowTex{};
    Shader m_air{};
    Shader m_glow{};
    bool m_haveGlow = false;
    int m_gView = -1, m_gColour = -1, m_gFalloff = -1;
    bool m_haveAir = false;
    int m_aViewPos = -1, m_aSunDir = -1, m_aColour = -1, m_aStrength = -1, m_aFalloff = -1;
    bool m_skyBuilt = false;
    /// Storage the worker writes into. Held by shared_ptr so the bake outliving
    /// this object writes somewhere valid instead of into freed memory.
    struct Bake;
    std::shared_ptr<Bake> m_bake;
    float m_cloudPhase = 0.0f;
    int m_month = 5;                          ///< 0 = January
    Vector3 moonWorld() const;   ///< where the moon is; pure, no draw needed
    /// What stands between the next body drawn and the sun. Radius 0 = nothing.
    void setOccluder(Vector3 pos, float radius) const;

    void startSkyBake();
    void buildSkyTextures();
    void drawSky(const Camera3D& cam);

    // Camera, in spherical coordinates about the origin.
    float m_lat = 0.35f;      ///< radians, + is north
    float m_lon = 0.0f;       ///< radians
    float m_dist = 3.0f;      ///< in sphere radii, so 1.0 is the surface
};
