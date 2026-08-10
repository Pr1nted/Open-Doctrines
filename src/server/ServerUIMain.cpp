// The dedicated server, with a window.
//
// WHY THIS IS A SEPARATE BINARY
//
// OpenDoctrinesServer links no raylib at all -- that is what lets it run on a
// VPS with no GPU, and it is not a property to give up for a status window. So
// the window is a second target over the same server core: the same
// serverBegin/serverTick/serverEnd, the same config, the same console, real
// raylib underneath instead of src/server/ServerRaylib.cpp.
//
// WHAT IT IS FOR, HONESTLY
//
// Almost nothing, on a desktop. A server operator has a terminal and the
// terminal is better: it scrolls, it copies, it pipes. This exists for two
// cases where there is no terminal at all -- somebody double-clicking the
// server on their own machine to play with friends, and Android, which has no
// console for an app to read. It shows what a server prints and takes the same
// commands.
//
// IT DOES NOT OPEN THE GAME. No menu, no map, no renderer of the world. The
// window is a log, an address and a player list.

#include "ServerConfig.h"
#include "ServerConsole.h"

#include "../Game.h"

#include "raylib.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int WIN_W = 900, WIN_H = 560;
const Color BG      = {15, 15, 20, 255};
const Color PANEL   = {26, 26, 34, 255};
const Color ACCENT  = {255, 215, 0, 255};
const Color DIM     = {150, 150, 160, 255};

/** The single line a player has to be given, or why there is not one yet. */
std::string addressLine(const Game& game, const ServerConfig& cfg) {
    (void)game;
    if (cfg.tunnel == ServerTunnelMode::Off)
        return cfg.bindAll ? "no tunnel: give players this machine's address and port"
                           : "no tunnel and loopback only: nobody else can reach this";
    return "starting a tunnel...";
}

/**
 * A one-line text field the window can edit, for typing commands.
 *
 * Deliberately the only input this window has. Everything else is a command,
 * and a window that grew a button per command would drift from `help` -- which
 * is generated from the command table and is therefore always right.
 */
struct CommandBar {
    std::string text;
    bool focused = true;

    void update(ServerConsole& console) {
        if (!focused) return;
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= 32 && key < 127 && text.size() < 200) text.push_back((char)key);
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !text.empty()) text.pop_back();
        if (IsKeyPressed(KEY_ENTER) && !text.empty()) {
            console.raw("> " + text);
            console.dispatch(text);
            text.clear();
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string configPath = "server.json";
    std::string dataOverride;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) configPath = argv[++i];
        else if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) dataOverride = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0) {
            printf("OpenDoctrines dedicated server (windowed)\n"
                   "  --config <file>   config file (default server.json)\n"
                   "  --data <dir>      where maps, mods and saves live\n"
                   "For a server without a window, use OpenDoctrinesServer.\n");
            return 0;
        }
    }

    ServerConfig config;
    std::string why;
    if (!config.load(configPath, why)) {
        // Same rule as the console server: a config that exists and does not
        // parse is refused, never silently replaced with defaults.
        fprintf(stderr, "%s\n", why.c_str());
        return 2;
    }
    if (!dataOverride.empty()) config.dataDir = dataOverride;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(WIN_W, WIN_H, "OpenDoctrines dedicated server");
    SetTargetFPS(60);
    if (!IsWindowReady()) {
        fprintf(stderr, "could not open a window. Use OpenDoctrinesServer, which needs none.\n");
        return 3;
    }

    ServerConsole console;
    // No stdin reader. This binary's input is the command bar; a windowed app
    // launched from a file manager has no terminal attached, and a detached
    // thread blocking on a closed stdin is a thread that never ends.
    Game game;
    CommandBar bar;

    const int rc = game.serverBegin(config, console, configPath);
    bool running = (rc == 0) && game.serverRunning();
    if (rc != 0) console.error("the server could not start; see the log above.");

    while (!WindowShouldClose()) {
        bar.update(console);
        if (running) running = game.serverTick();

        BeginDrawing();
        ClearBackground(BG);
        const int w = GetScreenWidth(), h = GetScreenHeight();

        // ── header: the two things a player has to be given ──
        DrawRectangle(0, 0, w, 84, PANEL);
        DrawText(config.sessionName.c_str(), 16, 12, 20, ACCENT);
        const std::string state = !running ? "stopped"
                                 : game.serverRunning() ? "running" : "starting";
        DrawText(TextFormat("%s   ·   map %s   ·   port %d",
                            state.c_str(), config.map.c_str(), (int)config.port),
                 16, 40, 14, DIM);
        DrawText(addressLine(game, config).c_str(), 16, 60, 12, DIM);

        // ── the log, newest at the bottom, exactly what the console printed ──
        const int logY = 96, logH = h - logY - 52;
        DrawRectangle(8, logY, w - 16, logH, PANEL);
        const std::vector<std::string> lines = console.history();
        const int rows = std::max(1, (logH - 12) / 16);
        const int first = std::max(0, (int)lines.size() - rows);
        for (int i = first; i < (int)lines.size(); ++i) {
            const std::string& s = lines[i];
            Color c = WHITE;
            if (s.find("ERROR") != std::string::npos)      c = Color{255, 120, 120, 255};
            else if (s.find("WARN") != std::string::npos)  c = Color{240, 200, 120, 255};
            else if (!s.empty() && s[0] == '>')            c = ACCENT;
            DrawText(s.c_str(), 16, logY + 6 + (i - first) * 16, 12, c);
        }

        // ── the command bar: the same commands the console takes ──
        const int barY = h - 40;
        DrawRectangle(8, barY, w - 16, 32, PANEL);
        DrawRectangleLines(8, barY, w - 16, 32, Color{70, 70, 84, 255});
        DrawText("›", 18, barY + 8, 16, ACCENT);
        DrawText(bar.text.c_str(), 36, barY + 9, 14, WHITE);
        if (((int)(GetTime() * 2) % 2) == 0)
            DrawText("|", 36 + MeasureText(bar.text.c_str(), 14), barY + 9, 14, ACCENT);
        if (bar.text.empty())
            DrawText("type a command -- `help` lists them", 36, barY + 9, 14,
                     Color{90, 90, 100, 255});

        EndDrawing();
    }

    game.serverEnd();
    CloseWindow();
    return 0;
}
