// A contact sheet of every hand the rig can draw.
//
//   RigPreview hands <out.png>              every class against every facing
//   RigPreview hand <class> <out.png>       ONE class, big enough to draw from
//   RigPreview poses <rig.odrig> <out.png>  the whole figure, one cell per pose
//
// WHY THIS EXISTS
//
// A hand is the one part of the rig that cannot be judged where it lives. In a
// screenshot of the whole figure it is thirty pixels across, held at whatever
// facing the current pose happens to want, and both hands are small enough that
// a left hand drawn on a right arm goes unnoticed for weeks. Every hand bug so
// far has been found by accident and at the wrong size.
//
// So: every class against every facing, both chiralities, big enough to see,
// in one picture that takes two seconds to make. What it is really testing is
// the projection -- that a fist folds away as the palm turns toward you instead
// of sweeping sideways, and that the thumb is on the inside of BOTH hands.
//
// The pose sheet exists for the other half of the problem: a hand can be
// correct on its own and still wrong on the end of an arm, because the pose
// chooses the wrist angle and the class, and those are what the drawing has to
// survive. Idle motion is switched off so two runs of this are comparable.
//
// AND IT IS A REFERENCE, NOT JUST A TEST. There is no hand art yet and the
// schematic is what the art will be drawn FROM, so the `hand` sheet renders one
// class large enough to measure against, with a grid in palm-widths: the
// proportions in the drawing are the ones the rig will animate, and an artist
// working from a 45-pixel game screenshot would be guessing at them.
//
// It writes a PNG and exits; it is not interactive and it is not shipped.

#include "raylib.h"
#include "rig/RigDraw.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

// Settle a pose: run the transition to completion at a fixed step, so the sheet
// shows the pose and not a frame part way into it.
void settle(rig::Rig& r, const std::string& want) {
    // Stop at the END OF THE GESTURE, not at rest. A pose with `then idle`
    // hands itself back the moment it finishes, so running to settled() would
    // photograph every gesture as the pose it falls back to.
    for (int i = 0; i < 300 && !r.settled() && r.activePose() == want; ++i)
        r.update(1.0f / 60.0f);
    for (int i = 0; i < 30 && r.activePose() == want; ++i)
        r.update(1.0f / 60.0f);                            // let the cloth hang
}

int poseSheet(const char* rigPath, const char* out) {
    rig::Rig probe;
    if (!probe.load(rigPath)) {
        printf("cannot load %s\n", rigPath);
        return 1;
    }
    std::vector<std::string> poses;
    for (const auto& p : probe.skeleton().poses) poses.push_back(p.first);
    if (poses.empty()) { printf("%s has no poses\n", rigPath); return 1; }

    const int CELL_W = 300, CELL_H = 400, HEAD = 34;
    const int cols = (int)poses.size();
    const int W = cols * CELL_W, H = HEAD + CELL_H;

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(W, H, "rig poses");
    RenderTexture2D rt = LoadRenderTexture(W, H);

    // One rig per cell, each settled into its own pose. Loading them all up
    // front rather than re-posing one is deliberate: a rig that has been
    // through four transitions is not in the same state as one that went
    // straight there, and this sheet is meant to show the pose itself.
    BeginTextureMode(rt);
    ClearBackground({16, 16, 20, 255});
    for (int i = 0; i < cols; ++i) {
        rig::Rig r;
        r.load(rigPath);
        r.setIdleEnabled(false);
        r.setPose(poses[i], true);
        settle(r, poses[i]);
        const int x = i * CELL_W;
        DrawRectangleLines(x, HEAD, CELL_W, CELL_H, {60, 60, 70, 255});
        DrawText(poses[i].c_str(), x + 10, 9, 18, {170, 170, 180, 255});
        rig::drawDebug(r, {(float)x + CELL_W * 0.5f, (float)HEAD + CELL_H * 0.62f},
                       1.9f, false);
    }
    EndTextureMode();

    Image img = LoadImageFromTexture(rt.texture);
    ImageFlipVertical(&img);
    ExportImage(img, out);
    UnloadImage(img);
    UnloadRenderTexture(rt);
    CloseWindow();
    printf("wrote %s (%dx%d, %d poses)\n", out, W, H, cols);
    return 0;
}


// One class at every facing, big. The grid is in palm-lengths measured from the
// wrist, so a drawing can be checked against it rather than eyeballed.
int handSheet(const char* cls, const char* out) {
    const float FACINGS[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    const int NF = 5, CELL = 340, HEAD = 40, SIZE = 210;
    const int W = NF * CELL, H = HEAD + CELL + 30;

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(W, H, "hand reference");
    RenderTexture2D rt = LoadRenderTexture(W, H);

    BeginTextureMode(rt);
    ClearBackground({16, 16, 20, 255});
    DrawText(TextFormat("hand class '%s' -- grid is one palm length (%d px at this size)",
                        cls, (int)(SIZE * 0.52f)), 12, 12, 18, {170, 170, 180, 255});

    for (int f = 0; f < NF; ++f) {
        const int x = f * CELL;
        const float wx = x + CELL * 0.5f, wy = HEAD + CELL * 0.80f;
        DrawRectangleLines(x, HEAD, CELL, CELL, {60, 60, 70, 255});
        // Palm-length rules up from the wrist, and the palm's half width.
        const float P = SIZE * 0.52f, HW = SIZE * 0.21f;
        for (int g = 1; g <= 2; ++g)
            DrawLineEx({(float)x + 8, wy - P * g}, {(float)x + CELL - 8, wy - P * g},
                       1.0f, {70, 70, 86, 255});
        for (int sgn = -1; sgn <= 1; sgn += 2)
            DrawLineEx({wx + HW * sgn, (float)HEAD + 8}, {wx + HW * sgn, wy + 20},
                       1.0f, {70, 70, 86, 255});
        rig::drawHandCard({wx, wy}, -90.0f, (float)SIZE, cls, FACINGS[f], +1.0f,
                          {120, 210, 255, 255});
        DrawText(TextFormat("facing %+.1f", FACINGS[f]), x + CELL / 2 - 44,
                 HEAD + CELL + 6, 18, {170, 170, 180, 255});
    }
    EndTextureMode();

    Image img = LoadImageFromTexture(rt.texture);
    ImageFlipVertical(&img);
    ExportImage(img, out);
    UnloadImage(img);
    UnloadRenderTexture(rt);
    CloseWindow();
    printf("wrote %s (%dx%d)\n", out, W, H);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = (argc > 1) ? argv[1] : "hands";
    if (mode == "hand") {
        if (argc < 4) { printf("usage: RigPreview hand <class> <out.png>\n"); return 1; }
        return handSheet(argv[2], argv[3]);
    }
    if (mode == "poses") {
        if (argc < 4) { printf("usage: RigPreview poses <rig.odrig> <out.png>\n"); return 1; }
        return poseSheet(argv[2], argv[3]);
    }
    const char* out = (argc > 2) ? argv[2] : "rig_hands.png";

    const char* CLASSES[] = {"relaxed", "open", "fist", "point"};
    const float FACINGS[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
    const int NC = 4, NF = 5;

    const int CELL = 210, PAD = 60, HEAD = 46;
    const int W = PAD + NF * CELL, H = HEAD + PAD + NC * CELL;

    // Off-screen: the sheet is the product, the window is an implementation
    // detail of having a GL context at all.
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(W, H, "rig preview");
    RenderTexture2D rt = LoadRenderTexture(W, H);

    BeginTextureMode(rt);
    ClearBackground({16, 16, 20, 255});

    for (int f = 0; f < NF; ++f) {
        const int x = PAD + f * CELL + CELL / 2;
        DrawText(TextFormat("facing %+.1f", FACINGS[f]), x - 44, 14, 18, {170, 170, 180, 255});
    }

    for (int c = 0; c < NC; ++c) {
        const int y = HEAD + PAD + c * CELL;
        DrawText(CLASSES[c], 8, y + CELL / 2 - 8, 18, {170, 170, 180, 255});
        for (int f = 0; f < NF; ++f) {
            const int x = PAD + f * CELL;
            DrawRectangleLines(x, y, CELL, CELL, {60, 60, 70, 255});
            // Both hands of the same pair, wrists level, knuckles pointing up
            // the sheet -- so a thumb that has gone to the wrong side is a
            // difference between two things side by side rather than something
            // you have to hold in your head.
            //
            // -90 is straight up: screen space has +90 pointing DOWN.
            rig::drawHandCard({(float)x + CELL * 0.30f, (float)y + CELL * 0.74f},
                              -90.0f, 96.0f, CLASSES[c], FACINGS[f], +1.0f,
                              {120, 210, 255, 255});
            rig::drawHandCard({(float)x + CELL * 0.72f, (float)y + CELL * 0.74f},
                              -90.0f, 96.0f, CLASSES[c], FACINGS[f], -1.0f,
                              {255, 170, 120, 255});
            DrawText("R", x + (int)(CELL * 0.30f) - 5, y + CELL - 22, 14, {120, 210, 255, 200});
            DrawText("L", x + (int)(CELL * 0.72f) - 5, y + CELL - 22, 14, {255, 170, 120, 200});
        }
    }
    EndTextureMode();

    Image img = LoadImageFromTexture(rt.texture);
    ImageFlipVertical(&img);          // render textures come out upside down
    ExportImage(img, out);
    UnloadImage(img);
    UnloadRenderTexture(rt);
    CloseWindow();
    printf("wrote %s (%dx%d)\n", out, W, H);
    return 0;
}
