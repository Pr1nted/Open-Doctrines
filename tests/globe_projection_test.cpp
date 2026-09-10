// Do the globe's texture mapping and its picking agree?
//
// They are inverses of each other. If they ever drift apart a province selects
// in one place and draws in another -- which presents as a camera bug, or as
// "clicking is off near the poles", and is really a disagreement about where
// u = 0 is. So this is a round-trip test: every pixel that goes out through the
// mesh's UV convention must come back through the picking maths as itself.
//
// Nothing here touches the GPU. The geometry is deliberately split out of
// GlobeView for exactly that reason -- the part that has to be exact is the
// part that can be tested without a window.

#include "renderer/GlobeView.h"
#include "raymath.h"

#include <cmath>
#include <cstdio>
#include <string>

static int checks = 0, fails = 0;
static void ok(bool c, const std::string& what) {
    ++checks;
    printf(c ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!c) ++fails;
}
static void section(const char* t) { printf("\n== %s ==\n", t); }

// The shipped political raster.
static constexpr int MW = 8192, MH = 4096;

int main() {
    printf("Globe projection\n");

    section("a pixel survives the round trip");
    {
        // Sampled across the whole sheet rather than near the middle: the
        // interesting failures are at the edges and the poles.
        int worst = 0;
        const char* worstWhere = "";
        for (int py = 1; py < MH; py += 149) {
            for (int px = 0; px < MW; px += 293) {
                const Vector3 p = globe::unitFromPixel((float)px + 0.5f, (float)py + 0.5f, MW, MH);
                int bx = 0, by = 0;
                globe::pixelFromUnit(p, MW, MH, bx, by);
                const int dx = std::abs(bx - px), dy = std::abs(by - py);
                if (dx + dy > worst) { worst = dx + dy; worstWhere = "somewhere"; }
            }
        }
        (void)worstWhere;
        // One texel of slack for float rounding on an 8192-wide sheet; anything
        // larger is a real disagreement, not precision.
        ok(worst <= 1, "every sampled pixel returns within a texel (worst " +
                       std::to_string(worst) + ")");
    }

    section("the corners of the sheet mean what the map says they mean");
    {
        // Left edge is the antimeridian, top is the north pole. If these are
        // wrong the whole map is rotated or flipped and everything else still
        // round-trips perfectly.
        const Vector3 topLeft = globe::unitFromPixel(0.0f, 0.0f, MW, MH);
        ok(topLeft.y > 0.999f, "the top row is the north pole");

        const Vector3 bottom = globe::unitFromPixel(0.0f, (float)MH, MW, MH);
        ok(bottom.y < -0.999f, "the bottom row is the south pole");

        const Vector3 mid = globe::unitFromPixel((float)MW * 0.5f, (float)MH * 0.5f, MW, MH);
        ok(std::fabs(mid.y) < 0.001f, "the middle row is the equator");
        ok(mid.x > 0.999f, "and the middle column is longitude zero");
    }

    section("no pixel is ever out of bounds");
    {
        bool inRange = true;
        for (float u = -0.5f; u <= 1.5f; u += 0.07f) {
            for (float v = -0.5f; v <= 1.5f; v += 0.11f) {
                const Vector3 p = globe::unitFromPixel(u * MW, v * MH, MW, MH);
                int px = 0, py = 0;
                globe::pixelFromUnit(p, MW, MH, px, py);
                if (px < 0 || px >= MW || py < 0 || py >= MH) inRange = false;
            }
        }
        // Out of range here would index past the end of the province raster,
        // which is a read out of bounds and not a wrong colour.
        ok(inRange, "even coordinates off the sheet clamp inside the raster");
    }

    section("the far side of the planet is not drawn");
    {
        const float dist = 3.0f;
        // Facing the camera at lat 0, lon 0: that point is the sub-camera point.
        const Vector3 front = globe::unitFromPixel((float)MW * 0.5f, (float)MH * 0.5f, MW, MH);
        ok(globe::onNearSide(front, 0.0f, 0.0f, dist), "the point under the camera is visible");

        // The opposite meridian is directly behind the planet.
        const Vector3 back = globe::unitFromPixel(0.0f, (float)MH * 0.5f, MW, MH);
        ok(!globe::onNearSide(back, 0.0f, 0.0f, dist),
           "the antipode is hidden, so a marker there must not draw");

        // The horizon itself. For distance d the terminator of visibility sits
        // at dot == 1/d, so just inside and just outside must differ.
        const float horizonLon = acosf(1.0f / dist);
        const float eps = 0.02f;
        const Vector3 inside{cosf(horizonLon - eps), 0.0f, sinf(horizonLon - eps)};
        const Vector3 outside{cosf(horizonLon + eps), 0.0f, sinf(horizonLon + eps)};
        ok(globe::onNearSide(inside, 0.0f, 0.0f, dist), "just inside the limb is visible");
        ok(!globe::onNearSide(outside, 0.0f, 0.0f, dist), "just beyond it is not");
    }

    section("EAST IS TO THE RIGHT");
    {
        // The round trip cannot catch a mirrored world: flipping the direction
        // of longitude in both the mesh and the picking leaves them perfect
        // inverses of each other, and every check above still passes. What it
        // produces is a globe where Arabia sits west of Gibraltar.
        //
        // So this is checked in SCREEN space, which is the only place the error
        // exists. GetWorldToScreenEx is matrix arithmetic and opens no window.
        Camera3D cam{};
        cam.position   = globe::eyeFromOrbit(0.0f, 0.0f, 3.0f);
        cam.target     = {0.0f, 0.0f, 0.0f};
        cam.up         = {0.0f, 1.0f, 0.0f};
        cam.fovy       = 45.0f;
        cam.projection = CAMERA_PERSPECTIVE;

        const float mid = (float)MW * 0.5f;
        const float row = (float)MH * 0.5f;
        const float east = mid + (float)MW * (20.0f / 360.0f);   // 20 degrees east
        const float west = mid - (float)MW * (20.0f / 360.0f);
        const Vector2 e = GetWorldToScreenEx(globe::unitFromPixel(east, row, MW, MH), cam, 1000, 1000);
        const Vector2 w = GetWorldToScreenEx(globe::unitFromPixel(west, row, MW, MH), cam, 1000, 1000);
        ok(e.x > w.x, "a point 20 east draws right of one 20 west");

        // And north is up, for the same reason: a vertical flip is equally
        // self-consistent and equally wrong.
        const Vector2 n = GetWorldToScreenEx(globe::unitFromPixel(mid, row - MH * 0.15f, MW, MH), cam, 1000, 1000);
        const Vector2 sth = GetWorldToScreenEx(globe::unitFromPixel(mid, row + MH * 0.15f, MW, MH), cam, 1000, 1000);
        ok(n.y < sth.y, "and a northern point draws above a southern one");
    }

    section("the horizon widens as you pull back");
    {
        // A point 80 degrees round from the camera is over the horizon close in
        // and visible from far out. This is the property that makes markers pop
        // in as you zoom out rather than at a fixed ring.
        const float lon = 80.0f * DEG2RAD;
        const Vector3 p{cosf(lon), 0.0f, sinf(lon)};
        ok(!globe::onNearSide(p, 0.0f, 0.0f, 1.5f), "hidden from close in");
        ok(globe::onNearSide(p, 0.0f, 0.0f, 8.0f), "and visible from far out");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
