// A window that draws the globe and nothing else.
//
// Not shipped and not a test: this exists so the sphere can be LOOKED at with a
// real map on it, without first wiring a second view into the game. It takes a
// political raster as a PNG, spins it to a given longitude, and writes a frame.
//
//   globe_preview <political.png> <out.png> [lon_deg] [lat_deg] [dist] [rt]
//
// Passing "rt" composites through a RenderTexture first, which is what the game
// does -- render targets are stored bottom-up, and that is exactly the sort of
// difference that only shows up on the real path.
//
// The projection maths it exercises is the same code the game uses; what it
// cannot tell you is anything about integration.

#include "raylib.h"
#include "renderer/GlobeView.h"
#include "util/Async.h"

#include <chrono>
#include <thread>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <cmath>
#include <algorithm>

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: globe_preview <political.png> <out.png> [lon] [lat] [dist]\n");
        return 2;
    }
    const float lon  = (argc > 3) ? (float)atof(argv[3]) : 20.0f;
    const float lat  = (argc > 4) ? (float)atof(argv[4]) : 25.0f;
    const float dist = (argc > 5) ? (float)atof(argv[5]) : 3.0f;

    const int W = 1400, H = 1000;
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(W, H, "globe preview");
    if (!IsWindowReady()) { printf("no window\n"); return 1; }

    Image img = LoadImage(argv[1]);
    if (img.width == 0) { printf("could not load %s\n", argv[1]); return 1; }
    Texture2D flat = LoadTextureFromImage(img);
    SetTextureFilter(flat, TEXTURE_FILTER_BILINEAR);

    const bool viaRT = (argc > 6 && std::string(argv[6]) == "rt");
    RenderTexture2D rt{};
    Texture2D surface = flat;
    if (viaRT) {
        rt = LoadRenderTexture(img.width, img.height);
        SetTextureFilter(rt.texture, TEXTURE_FILTER_BILINEAR);
        BeginTextureMode(rt);
        ClearBackground(BLANK);
        // The same negative source height MapRenderer::buildSurface uses.
        DrawTexturePro(flat, {0, 0, (float)flat.width, -(float)flat.height},
                       {0, 0, (float)img.width, (float)img.height}, {0, 0}, 0.0f, WHITE);
        EndTextureMode();
        surface = rt.texture;
    }

    GlobeView globe(img.width, img.height);
    globe.setSurface(surface);

    // The sun is placed ROUGHLY PERPENDICULAR to the view on purpose. Put it
    // near the camera and the whole visible face is daylight -- which looks
    // exactly like a shader that is doing nothing, and cost an hour of chasing
    // one that was working fine. The terminator has to cross the disc to be
    // judged at all.
    GlobeView::Sun sun;
    {
        // 90 degrees off the camera by default, so the terminator crosses the
        // disc and can be judged. OD_SUNOFF=0 puts the sun behind the camera
        // instead, which is what you want to inspect the sub-solar point --
        // an eclipse shadow lands there and nowhere else.
        float off = 90.0f;
        if (const char* e = getenv("OD_SUNOFF")) off = (float)atof(e);
        const float sunLon = (lon + off) * DEG2RAD;
        const float sunLat = 10.0f * DEG2RAD;
        sun.dir = { cosf(sunLat) * cosf(sunLon), sinf(sunLat),
                    -cosf(sunLat) * sinf(sunLon) };
    }
    if (const char* e = getenv("OD_SUN_STRENGTH")) sun.strength = (float)atof(e);
    globe.setSun(sun);

    GlobeView::Night night;
    if (const char* e = getenv("OD_NIGHT_FLOOR")) night.floorLevel = (float)atof(e);
    if (const char* e = getenv("OD_TERMINATOR"))  night.softness   = (float)atof(e);
    globe.setNight(night);
    if (const char* e = getenv("OD_UNLIT")) globe.setLit(atoi(e) == 0);

    GlobeView::Sky sky;
    if (const char* e = getenv("OD_STARS"))  sky.stars = atoi(e) != 0;
    if (const char* e = getenv("OD_MOON"))   sky.moon  = atoi(e) != 0;
    if (const char* e = getenv("OD_CLOUD"))  sky.cloudOpacity = (float)atof(e);
    if (const char* e = getenv("OD_MOONSZ")) sky.moonSize = (float)atof(e);
    sky.moonLon = lon + 180.0f;   // just past the limb from this camera
    sky.moonLat = 0.0f;
    globe.setSky(sky);
    globe.update(3.0f);   // a little drift, so cloud is not at phase zero
    if (const char* e = getenv("OD_MONTH")) globe.setMonth(atoi(e));
    // Park the moon between us and the sun to exercise the eclipse term.
    // ── Lunar eclipse ──
    //
    // The geometry here is SOLVED, not guessed, and the previous six attempts
    // are why. To see a moon inside the planet's umbra you need four things at
    // once, and most arrangements fail at least one:
    //
    //   1. the moon inside the shadow cone   (half-angle asin(1/moonDist))
    //   2. the moon clear of the planet's disc from the camera
    //   3. the moon inside the field of view
    //   4. the moon's SUN-FACING hemisphere toward the camera -- otherwise you
    //      are looking at its night side and it is dark whether eclipsed or not,
    //      which is a control that cannot fail and therefore proves nothing
    //
    // A sweep over camera and moon positions found this one with room on every
    // constraint: sun on the equator straight ahead, moon opposite at six radii
    // and 9.5 degrees up, camera at six radii and ten degrees up.
    if (const char* e = getenv("OD_LUNAR")) {
        const int mode = atoi(e);
        if (mode) {
            GlobeView::Sky lu = globe.sky();
            lu.moonDistance = 6.0f;
            lu.moonSize = 0.5f;
            lu.moonLon = 180.0f;
            lu.moonLat = (mode == 2) ? 20.0f : 9.5f;   // 2 = clear of the cone
            globe.setSky(lu);
            GlobeView::Sun ls = globe.sun();
            ls.dir = {1.0f, 0.0f, 0.0f};               // straight down +X
            globe.setSun(ls);
            globe.lookAt(0.0f, (float)img.height * (90.0f - 10.0f) / 180.0f);
            while (globe.distance() > 6.01f) globe.zoom(1.0f);
            while (globe.distance() < 5.99f) globe.zoom(-1.0f);
        }
    }
    if (const char* e = getenv("OD_ECLIPSE")) {
        if (atoi(e)) {
            GlobeView::Sky ec = globe.sky();
            // ON THE SUN'S LINE, both in longitude AND latitude. The sun sits
            // at a declination; a moon at latitude zero is degrees away from
            // it, which is many times the moon's angular radius, and there is
            // simply no eclipse to see. Getting this wrong reads as a broken
            // shadow term.
            const float sl = atan2f(-sun.dir.z, sun.dir.x) * RAD2DEG;
            const float sb = asinf(std::max(-1.0f, std::min(1.0f, sun.dir.y))) * RAD2DEG;
            ec.moonLon = sl; ec.moonLat = sb;
            ec.moonDistance = 9.0f; ec.moonSize = 0.42f;
            globe.setSky(ec);
        }
    }
    // orbit() takes mouse pixels; drive it in the same units the game will.
    globe.zoom(0.0f);
    globe.lookAt((float)img.width * (lon + 180.0f) / 360.0f,
                 (float)img.height * (90.0f - lat) / 180.0f);
    while (globe.distance() > dist + 0.01f) globe.zoom(1.0f);
    while (globe.distance() < dist - 0.01f) globe.zoom(-1.0f);

    // The sky is baked on a worker now, so a still frame has to WAIT for it --
    // a real session just draws a starless sky for a moment and carries on.
    // pump() is what runs the queued work on the web build; on desktop it is a
    // no-op and the sleep is what matters.
    for (int i = 0; i < 400 && !globe.skyReady(); ++i) {
        odasync::pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (int i = 0; i < 2; ++i) {
        BeginDrawing();
        ClearBackground(Color{9, 11, 15, 255});
        globe.draw(W, H);
        EndDrawing();
    }

    // Say where the moon actually is, so a test samples the moon rather than
    // whatever happens to be near the middle of the frame.
    {
        Vector2 mp{}; float mr = 0.0f;
        if (globe.moonOnScreen(W, H, mp, mr))
            printf("MOON %.0f %.0f %.1f\n", mp.x, mp.y, mr);
        else
        {
            const GlobeView::Sky k = globe.sky();
            printf("MOON none  (moon=%d lon=%.1f lat=%.1f dist=%.1f size=%.2f | cam lat=%.1f lon=%.1f d=%.2f)\n",
                   (int)k.moon, k.moonLon, k.moonLat, k.moonDistance, k.moonSize,
                   globe.latitude() * RAD2DEG, globe.longitude() * RAD2DEG, globe.distance());
        }
    }

    Image shot = LoadImageFromScreen();
    ExportImage(shot, argv[2]);
    printf("wrote %s  (lon %.0f lat %.0f dist %.2f)\n", argv[2], lon, lat, globe.distance());

    UnloadImage(shot);
    if (viaRT) UnloadRenderTexture(rt);
    UnloadTexture(flat);
    UnloadImage(img);
    CloseWindow();
    return 0;
}
