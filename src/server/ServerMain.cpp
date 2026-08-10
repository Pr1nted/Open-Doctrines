// The dedicated server's entry point.
//
//   OpenDoctrinesServer [--config <file>] [--data <dir>] [--port N] [--map ID]
//                       [--no-tunnel] [--write-config] [--help]
//
// Flags beat the config file, and the file beats the defaults. That order is
// what lets a server be configured once and still be started differently for
// one run without editing anything -- the case every operator hits the first
// time they want to test something on a spare port.
//
// WHAT THIS FILE DOES NOT DO
//
// It does not host. Hosting is Game::runDedicatedServer (src/Game_Server.cpp),
// which drives the game's own hosting path so the server and the client cannot
// disagree about the rules. This is argument parsing, the config file, signals
// and exit codes, and nothing else.

#include "ServerConfig.h"
#include "ServerConsole.h"

#include "../Game.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

extern "C" void odServerSeedRng(unsigned int seed);
extern "C" void odServerRequestStop(void);

namespace {

ServerConsole* g_console = nullptr;

/**
 * Ctrl-C and SIGTERM mean "stop cleanly", not "die".
 *
 * A server killed outright loses the turn it was holding and leaves players
 * looking at a session that will never answer. Both signals set the same flag
 * the `stop` command does, so the shutdown path is one path -- save the world,
 * close the tunnel, tell everyone -- rather than two that drift.
 *
 * The handler only sets flags. Nothing else is async-signal-safe.
 */
extern "C" void onSignal(int) {
    odServerRequestStop();
    if (g_console) g_console->requestStop();
}

void usage() {
    std::cout <<
        "OpenDoctrines dedicated server " OD_VERSION_STRING "\n"
        "\n"
        "  OpenDoctrinesServer [options]\n"
        "\n"
        "  --config <file>   config file to read (default: server.json beside the binary)\n"
        "  --data <dir>      where maps, mods and saves live\n"
        "  --port <n>        listen on this port, overriding the config\n"
        "  --map <id>        map to host: 1914, 1918, 1939, 1945, 1962, map, or a path\n"
        "  --load <save>     resume a .odsv instead of starting a new world\n"
        "  --name <text>     what the server calls itself\n"
        "  --no-tunnel       do not start a tunnel, whatever the config says\n"
        "  --check           load everything and exit without opening a session\n"
        "\n"
        "  --train-ai [maps] [turnsPerMap] [countries] [seed]\n"
        "                    self-play training, with no window. The reason this\n"
        "                    lives here: every AI mode of the game binary goes\n"
        "                    through init(), which opens an OpenGL window, so a\n"
        "                    headless box could not train. This one can.\n"
        "  --eval-ai [maps] [turnsPerMap] [seed] [difficulty]\n"
        "                    measure a model without learning from it\n"
        "  --worker <id> --workers <n>   one process of a parallel pool\n"
        "  --vs-random | --vs-model <p> | --scenarios   what to measure against\n"
        "  --write-config    write a commented default config file and exit\n"
        "  --help            this text\n"
        "\n"
        "Once running, type `help` at the console for the commands.\n";
}

}  // namespace

int main(int argc, char** argv) {
    // Every raylib entry point in this binary is a no-op except the ones the
    // simulation reads data through, and nothing seeds the C RNG that combat
    // rolls come from. See ServerRaylib.cpp.
    odServerSeedRng((unsigned int)time(nullptr));

    // ── headless AI modes ──
    //
    // Handled before the config file, because they are not a server: they take
    // no session, open no port, and have nothing a server.json would say.
    {
        auto numAfter = [&](int i, int n) -> const char* {
            // Positional arguments only, and only while they look like numbers,
            // so `--train-ai --data x` does not read "--data" as a map count.
            const int at = i + 1 + n;
            if (at >= argc || argv[at][0] == '-') return nullptr;
            return argv[at];
        };
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            const bool train = (a == "--train-ai");
            if (!train && a != "--eval-ai") continue;

            Game::HeadlessAIOptions o;
            o.train = train;
            const char* a1 = numAfter(i, 0);
            const char* a2 = numAfter(i, 1);
            const char* a3 = numAfter(i, 2);
            const char* a4 = numAfter(i, 3);
            o.maps  = a1 ? atoi(a1) : 0;
            o.turns = a2 ? atoi(a2) : 0;
            if (train) {
                o.countries = a3 ? atoi(a3) : 0;
                o.seed = a4 ? (unsigned)strtoul(a4, nullptr, 10) : (unsigned)time(nullptr);
                if (o.turns < 1) o.turns = 3000;
            } else {
                o.seed = a3 ? (unsigned)strtoul(a3, nullptr, 10) : 4242u;
                o.difficulty = a4 ? atoi(a4) : 2;
            }
            for (int k = 1; k < argc; ++k) {
                const std::string f = argv[k];
                if (f == "--data" && k + 1 < argc)       o.dataDir = argv[k + 1];
                else if (f == "--worker" && k + 1 < argc)  o.workerId = atoi(argv[k + 1]);
                else if (f == "--workers" && k + 1 < argc) o.workerCount = atoi(argv[k + 1]);
                else if (f == "--vs-model" && k + 1 < argc) o.vsModel = argv[k + 1];
                else if (f == "--vs-random")               o.vsRandom = true;
                else if (f == "--scenarios")               o.scenarios = true;
            }
            Game game;
            return game.runHeadlessAI(o);
        }
    }


    std::string configPath;
    std::string dataOverride, mapOverride, loadOverride, nameOverride;
    int portOverride = -1;
    bool noTunnel = false, writeConfig = false, checkOnly = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << a << " needs " << what << "\n";
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h")      { usage(); return 0; }
        else if (a == "--config")            configPath = next("a file path");
        else if (a == "--data")              dataOverride = next("a directory");
        else if (a == "--map")               mapOverride = next("a map id or path");
        else if (a == "--load")              loadOverride = next("a save path");
        else if (a == "--name")              nameOverride = next("a name");
        else if (a == "--port")              portOverride = atoi(next("a port number").c_str());
        else if (a == "--no-tunnel")         noTunnel = true;
        else if (a == "--check")             checkOnly = true;
        else if (a == "--write-config")      writeConfig = true;
        else if (a == "--train-ai" || a == "--eval-ai" || a == "--worker" ||
                 a == "--workers" || a == "--vs-model" || a == "--vs-random" ||
                 a == "--scenarios") {
            // Consumed by the headless-AI block above, which returns before
            // reaching here. Listed so the loop does not reject them when they
            // trail a mode this parser never sees.
        }
        else {
            std::cerr << "unknown option '" << a << "'. Try --help.\n";
            return 2;
        }
    }

    if (configPath.empty()) configPath = "server.json";

    ServerConfig config;
    if (writeConfig) {
        std::string why;
        if (!config.save(configPath, why)) { std::cerr << why << "\n"; return 2; }
        std::cout << "wrote " << configPath << "\n";
        return 0;
    }

    std::string why;
    if (!config.load(configPath, why)) {
        // Refused rather than run on defaults: a typo in a setting somebody
        // thought they had changed is exactly what silent fallback hides, and
        // the first symptom would be a server behaving nothing like its file.
        std::cerr << why << "\n";
        return 2;
    }

    if (!dataOverride.empty()) config.dataDir = dataOverride;
    if (!mapOverride.empty())  config.map = mapOverride;
    if (!loadOverride.empty()) config.loadSave = loadOverride;
    if (!nameOverride.empty()) config.sessionName = nameOverride;
    if (portOverride >= 0 && portOverride <= 65535) config.port = (uint16_t)portOverride;
    if (noTunnel) config.tunnel = ServerTunnelMode::Off;
    if (checkOnly) config.checkOnly = true;

    ServerConsole console;
    g_console = &console;
    console.startReading();

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    // First run with no file: write one, so the settings are discoverable by
    // reading rather than by being told they exist.
    std::error_code ec;
    if (!fs::exists(configPath, ec)) {
        std::string saveWhy;
        if (config.save(configPath, saveWhy))
            console.info("wrote a default config to " + configPath);
    }

    Game game;
    const int code = game.runDedicatedServer(config, console, configPath);
    g_console = nullptr;
    return code;
}
