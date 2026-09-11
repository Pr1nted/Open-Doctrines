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


    section("the unroll starts where the flat camera already was");
    {
        // The claim: at morph 0 the sheet camera shows EXACTLY what a 2D map
        // camera at (target, zoom) shows. Asserted rather than photographed
        // because the animation is driven by real time -- a screenshot taken one
        // frame after the switch is already a third of the way through it, and
        // an image comparison there measures the middle of the move while
        // looking like it measures the start. That mistake was made first.
        const int SW = 1600, SH = 900;
        const float tx = MW * 0.52f, ty = MH * 0.32f;
        const float zoom = 1.0986f;

        Vector3 eye{}, at{};
        globe::sheetAnchor(tx, ty, zoom, MW, MH, SW, SH, eye, at);

        Camera3D cam{};
        cam.position = eye;
        cam.target   = at;
        cam.up       = {0.0f, 1.0f, 0.0f};
        cam.fovy     = 45.0f;
        cam.projection = CAMERA_PERSPECTIVE;

        // The point under the middle of the 2D view is under the middle of this
        // one.
        const Vector2 c = GetWorldToScreenEx(globe::sheetFromPixel(tx, ty, MW, MH),
                                             cam, SW, SH);
        ok(fabsf(c.x - SW * 0.5f) < 1.0f, "the target is dead centre horizontally");
        ok(fabsf(c.y - SH * 0.5f) < 1.0f, "and dead centre vertically");

        // And the map pixels the 2D camera puts on each screen edge land on the
        // same edges here. This is the part that catches a wrong FIELD OF VIEW:
        // the centre is right for any distance, the edges only for one.
        const float halfPxW = SW * 0.5f / zoom;
        const float halfPxH = SH * 0.5f / zoom;
        const Vector2 l = GetWorldToScreenEx(globe::sheetFromPixel(tx - halfPxW, ty, MW, MH), cam, SW, SH);
        const Vector2 r = GetWorldToScreenEx(globe::sheetFromPixel(tx + halfPxW, ty, MW, MH), cam, SW, SH);
        const Vector2 t = GetWorldToScreenEx(globe::sheetFromPixel(tx, ty - halfPxH, MW, MH), cam, SW, SH);
        const Vector2 b = GetWorldToScreenEx(globe::sheetFromPixel(tx, ty + halfPxH, MW, MH), cam, SW, SH);
        ok(fabsf(l.x) < 2.0f,            "the left edge of the 2D view is the left edge here");
        ok(fabsf(r.x - (float)SW) < 2.0f, "and the right edge is the right edge");
        ok(fabsf(t.y) < 2.0f,             "the top edge is the top edge");
        ok(fabsf(b.y - (float)SH) < 2.0f, "and the bottom edge is the bottom edge");

        // Zooming the 2D view in must move the sheet camera closer, or the
        // anchor would be right in one place and wrong everywhere else.
        Vector3 eye2{}, at2{};
        globe::sheetAnchor(tx, ty, zoom * 2.0f, MW, MH, SW, SH, eye2, at2);
        ok(eye2.z < eye.z * 0.55f, "twice the zoom halves the distance");
    }


    section("a drag turns the globe the way the hand goes");
    {
        // The globe turned the wrong way on BOTH axes for as long as it existed,
        // and no test could have caught it because none of them asked the only
        // question that matters: after a drag, has the ground under the cursor
        // moved with the cursor? Signs are easy to argue about and this sphere
        // has a reversed one in it (east is -Z), so the property is asserted
        // rather than the arithmetic reviewed.
        const int SW = 1600, SH = 900;
        const float dist = 2.6f;
        const float scale = 0.0045f * (dist / 3.0f);   // as GlobeView::orbit uses

        auto screenOf = [&](Vector3 p, float lat, float lon) {
            Camera3D c{};
            c.position = globe::eyeFromOrbit(lat, lon, dist);
            c.target = {0.0f, 0.0f, 0.0f};
            c.up = {0.0f, 1.0f, 0.0f};
            c.fovy = 45.0f;
            c.projection = CAMERA_PERSPECTIVE;
            return GetWorldToScreenEx(p, c, SW, SH);
        };

        struct { float lat, lon; } spots[] = {
            {20.0f, 10.0f}, {-35.0f, 150.0f}, {0.0f, -80.0f}, {60.0f, 0.0f},
        };
        bool right = true, down = true;
        for (const auto& s2 : spots) {
            const float lat = s2.lat * DEG2RAD, lon = s2.lon * DEG2RAD;
            const Vector3 p = globe::unitFromPixel((lon + PI) / (2.0f * PI) * MW,
                                                   (PI * 0.5f - lat) / PI * MH, MW, MH);
            const Vector2 before = screenOf(p, lat, lon);

            // The input path hands the mouse delta straight to orbit(), which
            // does lon -= dx*scale and lat += dy*scale.
            const float drag = 20.0f;
            const Vector2 afterX = screenOf(p, lat, lon - drag * scale);
            const Vector2 afterY = screenOf(p, lat + drag * scale, lon);
            if (!(afterX.x - before.x > 2.0f)) right = false;
            if (!(afterY.y - before.y > 2.0f)) down = false;
        }
        ok(right, "dragging right carries the ground right");
        ok(down, "dragging down carries the ground down");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
