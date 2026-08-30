// Look at the communication window while tuning it.
//
//   TransmissionPreview [options]
//     --shot PATH -n N      render N frames at 60 Hz, save the last, exit
//     --accent RRGGBB       the player's accent colour (default amber)
//     --quality 0..1        line quality
//     --profile NAME        advisor | officer | analyst | courier
//     --sweep PATH          one image showing the same frame in six accents
//     --tune PATH           a contact sheet of the link coming up and going down
//
// Keys, in the window:
//   1-4  profile      up/down  signal quality      space  force a blink
//   a    cycle accent  w  wander  click  look there  esc  quit
#include "comms/Transmission.h"
#include "comms/CastFile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "raylib.h"

struct Named { const char* name; comms::Profile p; };

static std::vector<Named> profiles() {
    comms::Profile advisor;
    advisor.hat = true; advisor.browDrop = 0.55f; advisor.seed = 11;

    // The drawn speaker: greyscale art, coloured entirely by the filter.
    comms::Profile drawn = advisor;
    drawn.image = "data/comms/advisor.png";
    drawn.imgEyeY = 0.338f; drawn.imgEyeGap = 0.223f;
    drawn.imgEyeW = 0.105f; drawn.imgEyeH = 0.094f;
    drawn.seed = 7;

    comms::Profile officer;
    officer.headW = 0.27f; officer.headH = 0.32f; officer.shoulderW = 0.92f;
    officer.neckW = 0.14f; officer.browDrop = 0.8f; officer.eyeGap = 0.105f;
    officer.eyeH = 0.028f; officer.seed = 23;

    comms::Profile analyst;
    analyst.headW = 0.32f; analyst.headH = 0.37f; analyst.shoulderW = 0.66f;
    analyst.eyeGap = 0.125f; analyst.eyeW = 0.058f; analyst.eyeH = 0.044f;
    analyst.browDrop = 0.15f; analyst.seed = 37;

    comms::Profile courier;
    courier.headW = 0.29f; courier.headH = 0.33f; courier.shoulderW = 0.72f;
    courier.hat = true; courier.hatBrim = 0.38f; courier.hatCrown = 0.13f;
    courier.eyeGap = 0.11f; courier.eyeH = 0.032f; courier.browDrop = 0.35f;
    courier.seed = 51;

    return {{"drawn", drawn}, {"advisor", advisor}, {"officer", officer},
            {"analyst", analyst}, {"courier", courier}};
}


// A speaker straight out of data/comms/cast.json, by the name the game knows
// it by. NEVER hand-copy a profile in here: this tool is what the window is
// tuned in, and a copy that has drifted renders a character that no longer
// exists -- which is exactly how an afternoon went missing.
static bool castProfile(const char* name, comms::Profile& out) {
    std::ifstream in("data/comms/cast.json");
    if (!in) { fprintf(stderr, "no data/comms/cast.json -- run from the repo root\n"); return false; }
    try {
        nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
        const nlohmann::json chars = j.value("characters", nlohmann::json::object());
        for (auto& [k, c] : chars.items()) {
            if (k != name) continue;
            comms::profileFromJson(c, "data/comms/", out);
            out.seed = comms::seedFromName(k);
            return true;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "cast.json: %s\n", e.what());
        return false;
    }
    fprintf(stderr, "no character named \"%s\" in data/comms/cast.json\n", name);
    return false;
}

static Color hexColour(const char* s) {
    unsigned v = (unsigned)strtoul(s, nullptr, 16);
    return Color{(unsigned char)(v >> 16), (unsigned char)((v >> 8) & 0xFF),
                 (unsigned char)(v & 0xFF), 255};
}

int main(int argc, char** argv) {
    std::string shot, sweep, frameDir, signalOut, emotionsOut, tuneOut_, want = "drawn";
    float blinkPhase = -1.0f;
    Color accent{240, 176, 72, 255};
    float quality = 0.86f;
    int frames = 0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); }
            return argv[++i];
        };
        if (a == "--shot") shot = next();
        else if (a == "--sweep") sweep = next();
        else if (a == "--frames") frameDir = next();
        else if (a == "--signal") signalOut = next();
        else if (a == "--emotions") emotionsOut = next();
        else if (a == "--tune") tuneOut_ = next();
        else if (a == "--blink-phase") blinkPhase = (float)atof(next());
        else if (a == "-n") frames = atoi(next());
        else if (a == "--accent") accent = hexColour(next());
        else if (a == "--quality") quality = (float)atof(next());
        else if (a == "--profile") want = next();
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    const bool headless = !shot.empty() || !sweep.empty() || !frameDir.empty() ||
                          !signalOut.empty() || !emotionsOut.empty() || !tuneOut_.empty();

    const int W = 1280, H = 800;
    if (headless) SetConfigFlags(FLAG_WINDOW_HIDDEN);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(W, H, "OpenDoctrines transmission");
    SetTargetFPS(60);

    auto list = profiles();
    int which = 0;
    for (size_t i = 0; i < list.size(); i++)
        if (want == list[i].name) which = (int)i;

    comms::Transmission tx;
    if (!tx.open()) { fprintf(stderr, "cannot create the signal buffer\n"); return 1; }
    if (want == "mia") {
        comms::Profile m;
        if (!castProfile("Signals Officer", m)) return 1;
        list.push_back({"mia", m});
        which = (int)list.size() - 1;
    }
    tx.setProfile(list[which].p);
    tx.setSpeaker("ADVISOR");
    tx.setSignal({quality, 1.0f});
    tx.tuneIn();
    printf("filter: %s\n", tx.filtered() ? "compiled" : "FAILED, drawing unfiltered");

    const Color accents[] = {{240, 176, 72, 255}, {104, 232, 168, 255}, {92, 190, 255, 255},
                             {255, 108, 96, 255}, {198, 140, 255, 255}, {236, 236, 236, 255}};
    int accentIndex = 0;
    RenderTexture2D canvas = headless ? LoadRenderTexture(W, H) : RenderTexture2D{};
    const Rectangle screen{0, 0, (float)W, (float)H};
    const Rectangle box = comms::Transmission::suggestedBounds(screen);
    int frame = 0;

    while (!WindowShouldClose()) {
        const float dt = headless ? 1.0f / 60.0f : GetFrameTime();
        if (!headless) {
            for (int k = 0; k < (int)list.size(); k++)
                if (IsKeyPressed(KEY_ONE + k)) {
                    which = k;
                    // Through a dropout, the way the game changes speakers.
                    tx.changeSpeaker(list[k].p, list[k].name);
                }
            if (IsKeyPressed(KEY_SPACE)) tx.blinkSoon();
            if (IsKeyPressed(KEY_T)) tx.onAir() ? tx.tuneOut() : tx.tuneIn();
            if (IsKeyPressed(KEY_W)) tx.lookWander();
            if (IsKeyPressed(KEY_A)) accent = accents[++accentIndex % 6];
            if (IsKeyDown(KEY_UP)) quality = fminf(1.0f, quality + dt * 0.5f);
            if (IsKeyDown(KEY_DOWN)) quality = fmaxf(0.0f, quality - dt * 0.5f);
            tx.setSignal({quality, 1.0f});
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                const Vector2 m = GetMousePosition();
                tx.lookAt({(m.x - (box.x + box.width * 0.5f)) / (box.width * 0.5f),
                           (m.y - (box.y + box.height * 0.45f)) / (box.height * 0.45f)});
                tx.blinkSoon();
            }
        }
        if (headless) {
            tx.lookWander();          // exactly as the game calls it: every frame
        }
        tx.update(dt);
        if (blinkPhase >= 0.0f) tx.debugBlink(blinkPhase);
        if (headless && frame % 15 == 0)
            printf("%3d gaze %+.2f %+.2f\n", frame, tx.gaze().x, tx.gaze().y);

        if (headless) BeginTextureMode(canvas); else BeginDrawing();
        ClearBackground(Color{12, 14, 18, 255});
        DrawRectangleGradientV(0, 0, W, H, Color{22, 25, 32, 255}, Color{9, 10, 14, 255});
        tx.draw(box, accent);
        if (!headless) {
            DrawText(TextFormat("profile %s   quality %.2f   %s", list[which].name, quality,
                                tx.blinking() ? "blink" : ""),
                     (int)box.x, H - 60, 20, ColorAlpha(RAYWHITE, 0.75f));
            DrawText("1-4 profile   up/down signal   a accent   space blink   w wander   click look",
                     (int)box.x, H - 34, 17, ColorAlpha(RAYWHITE, 0.45f));
        }
        if (headless) EndTextureMode(); else EndDrawing();

        if (!frameDir.empty()) {
            Image f = LoadImageFromTexture(canvas.texture);
            ImageFlipVertical(&f);
            ImageFormat(&f, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
            ExportImage(f, TextFormat("%s/f%04i.png", frameDir.c_str(), frame));
            UnloadImage(f);
        }
        frame++;
        if (headless && frame >= (frames > 0 ? frames : 120)) break;
    }

    if (headless && !emotionsOut.empty()) {
        // The whole point of a parametric mouth: every expression between a
        // frown and a grin is a value, and speech is a second value on top.
        // Six cells, one drawing, no frames anywhere.
        comms::Profile mia;
        mia.image = "data/comms/officer.png";
        mia.imgEyeY = 0.410f; mia.imgEyeGap = 0.170f;
        mia.imgEyeW = 0.114f; mia.imgEyeH = 0.062f;
        mia.imgOutline = 0.0105f; mia.eyeOpenArc = 22.0f; mia.lidRest = 0.06f;
        mia.lashes = 3.2f; mia.pupilAspect = 3.0f; mia.pupilScale = 0.22f;
        mia.mouth = true; mia.mouthX = 0.503f; mia.mouthY = 0.561f;
        mia.mouthW = 0.125f; mia.mouthBase = 0.35f;
        tx.setProfile(mia);
        tx.setSpeaker("MIA");
        tx.setSignal({1.0f, 1.0f});

        const struct { float mood; int say; const char* label; } cells[] = {
            {-0.9f, 0, "grim"}, {-0.4f, 0, "unhappy"}, {0.0f, 0, "level"},
            {0.5f, 0, "pleased"}, {0.9f, 0, "delighted"}, {0.5f, 'a', "speaking"},
        };
        const int cols = 3, cw = (int)box.width + 40, ch = (int)box.height + 60;
        RenderTexture2D grid = LoadRenderTexture(cw * cols, ch * 2);
        std::vector<Image> cellImgs;
        for (const auto& c : cells) {
            tx.setEmotion(c.mood);
            tx.tuneIn();
            for (int i = 0; i < 140; i++) {          // settle the mood and the lock
                if (c.say && i > 100 && i % 6 == 0) tx.speak(c.say);
                tx.update(1.0f / 60.0f);
            }
            RenderTexture2D one = LoadRenderTexture((int)box.width, (int)box.height);
            BeginTextureMode(one);
            ClearBackground(Color{10, 11, 15, 255});
            tx.draw({0, 0, box.width, box.height}, Color{240, 176, 72, 255});
            EndTextureMode();
            Image im = LoadImageFromTexture(one.texture);
            ImageFlipVertical(&im);
            cellImgs.push_back(im);
            UnloadRenderTexture(one);
        }
        Image sheet = GenImageColor(cw * cols, ch * 2, Color{14, 15, 20, 255});
        for (size_t i = 0; i < cellImgs.size(); i++) {
            ImageDraw(&sheet, cellImgs[i],
                      {0, 0, (float)cellImgs[i].width, (float)cellImgs[i].height},
                      {(float)((i % cols) * cw + 20), (float)((i / cols) * ch + 20),
                       box.width, box.height}, WHITE);
            UnloadImage(cellImgs[i]);
        }
        ImageFormat(&sheet, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
        ExportImage(sheet, emotionsOut.c_str());
        UnloadImage(sheet);
        UnloadRenderTexture(grid);
    }

    if (!tuneOut_.empty()) {
        // THE LINK COMING UP AND GOING DOWN, one frame per column.
        //
        // Tuning is the one thing in this window that cannot be judged from a
        // still: it is entirely a matter of what happens over a fifth of a
        // second. Two rows -- arriving on top, leaving underneath -- sampled
        // where the shape actually changes rather than at even intervals,
        // because the interesting part of both is the first few frames.
        const int inAt[]  = {1, 3, 5, 7, 10, 14, 22, 45};
        const int outAt[] = {1, 22, 36, 40, 43, 46, 50, 58};
        const int cols = 8;
        const int cw = (int)box.width / 2 + 24, ch = (int)box.height / 2 + 64;

        auto capture = [&]() {
            RenderTexture2D one = LoadRenderTexture((int)box.width + 8, (int)box.height + 56);
            BeginTextureMode(one);
            ClearBackground(Color{10, 11, 15, 255});
            tx.draw({4, 48, box.width, box.height}, accent);
            EndTextureMode();
            Image im = LoadImageFromTexture(one.texture);
            ImageFlipVertical(&im);
            UnloadRenderTexture(one);
            return im;
        };

        std::vector<Image> cells;
        auto run = [&](const int* at, int n, int last) {
            int k = 0;
            for (int f = 1; f <= last; ++f) {
                tx.update(1.0f / 60.0f);
                if (k < n && f == at[k]) { cells.push_back(capture()); ++k; }
            }
        };

        tx.tuneOut();
        run(nullptr, 0, 200);                      // all the way dark first
        tx.tuneIn();
        run(inAt, cols, inAt[cols - 1]);
        run(nullptr, 0, 90);                       // settled, talking to nobody
        tx.tuneOut();
        run(outAt, cols, outAt[cols - 1]);

        Image sheet = GenImageColor(cw * cols, ch * 2, Color{14, 15, 20, 255});
        for (size_t i = 0; i < cells.size(); i++) {
            ImageDraw(&sheet, cells[i], {0, 0, (float)cells[i].width, (float)cells[i].height},
                      {(float)((i % cols) * cw), (float)((i / cols) * ch),
                       (float)cw, (float)ch}, WHITE);
            UnloadImage(cells[i]);
        }
        ImageFormat(&sheet, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
        ExportImage(sheet, tuneOut_.c_str());
        UnloadImage(sheet);
    }

    if (headless) {
        if (!signalOut.empty()) {
            Image sig = LoadImageFromTexture(tx.signalBuffer().texture);
            ImageFlipVertical(&sig);
            ExportImage(sig, signalOut.c_str());
            UnloadImage(sig);
        }
        if (!shot.empty()) {
            Image img = LoadImageFromTexture(canvas.texture);
            ImageFlipVertical(&img);
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
            ExportImage(img, shot.c_str());
            UnloadImage(img);
        }
        if (!sweep.empty()) {
            // The same instant in six accents, which is the whole point of
            // doing the colour in the filter: no art changes between these.
            const int cols = 3, rows = 2;
            const int cw = (int)(box.width + 90), ch = (int)(box.height + 130);
            RenderTexture2D grid = LoadRenderTexture(cw * cols, ch * rows);
            // One signal render, six accents: update() owns a render target of
            // its own and cannot run inside this one -- and the accent is a
            // draw-time argument anyway, which is the entire point.
            tx.update(1.0f / 60.0f);
            BeginTextureMode(grid);
            ClearBackground(Color{10, 11, 15, 255});
            for (int i = 0; i < 6; i++) {
                const Rectangle cell{(float)((i % cols) * cw) + 45,
                                     (float)((i / cols) * ch) + 78, box.width, box.height};
                tx.draw(cell, accents[i]);
            }
            EndTextureMode();
            Image img = LoadImageFromTexture(grid.texture);
            ImageFlipVertical(&img);
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
            ExportImage(img, sweep.c_str());
            UnloadImage(img);
        }
    }
    tx.close();
    CloseWindow();
    return 0;
}
