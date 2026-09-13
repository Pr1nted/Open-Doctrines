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
#include "../ai/AISystem.h"   // mergeModelFiles; see --merge-ai below
#include "../llm/Runner.h"    // the advisor runner; see --llm-install
#include <filesystem>

#include <algorithm>   // std::clamp, for --vs-exploit
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstdlib>    // atoi, for the seat-bench flags
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
        "  --vs-random | --vs-model <p> | --vs-script | --scenarios   what to measure against\n"
        "  --merge-ai <out> <in...>  fold worker models into one\n"
        "  --reset-ai-head <model.bin> <module 0-4>  throw away one head\n"
        "  --probe-trade <map:ISO> [--seed N]  exercise the trade rules\n"
        "\n"
        "  --llm-status      whether an advisor runner is installed here, and where\n  --llm-serve       run the installed runner in the foreground\n"
        "  --llm-install     download and verify the pinned runner into <data>/llm\n"
        "  --llm-uninstall   remove it again, leaving nothing behind\n"
        "  --llm-pull <model>  fetch weights through a running Ollama, e.g. gemma3:4b\n"
        "                    A dedicated server needs the module too: advisors are\n"
        "                    the host's to provide, so a headless box hosting a game\n"
        "                    with \"advisors only\" has to be able to install one.\n"
        "\n"
        "  --write-config    write a commented default config file and exit\n"
        "  --help            this text\n"
        "\n"
        "Once running, type `help` at the console for the commands.\n";
}

}  // namespace

/**
 * The advisor runner, from a headless box.
 *
 * A dedicated server has no settings screen, so these three flags are how a
 * host installs, checks and removes it. The same code as the game's own module
 * -- same pinned version, same hash check, same directory -- because a server
 * that installed something the game would refuse would be the more dangerous of
 * the two.
 */
int llmCommand(const std::string& what, const std::string& dataDir) {
    // A dedicated server has the same gap the game had: it could install a
    // runner and had no way to start one. This runs it in the FOREGROUND
    // rather than detaching, because a server is already supervised -- by
    // systemd, by a container, by the shell that launched it -- and a process
    // that daemonises itself out from under its supervisor is the thing those
    // supervisors are for.
    if (what == "serve") {
        if (!llm::installed(dataDir)) {
            fprintf(stderr, "no runner installed -- run --llm-install first\n");
            return 2;
        }
        const long long pid = llm::startServer(dataDir);
        if (pid == 0) { fprintf(stderr, "could not start it\n"); return 1; }
        printf("runner started, pid %lld, on %s\n", pid, llm::localEndpoint().c_str());
        printf("Ctrl-C to stop.\n");
        fflush(stdout);

        // THE RUNNER IS IN ITS OWN SESSION, so it does NOT die with us. That is
        // deliberate -- it keeps a terminal's Ctrl-C from killing the game's
        // runner out from under it -- but it means killing this wrapper leaves
        // ollama running and holding the port, which is exactly the orphan this
        // command exists to avoid. Verified by doing it: a plain SIGTERM to the
        // wrapper left the child alive.
        static long long s_child = 0;
        s_child = pid;
        auto bye = [](int) {
            if (s_child) llm::stopServer(s_child);
            s_child = 0;
            std::_Exit(0);
        };
        std::signal(SIGINT, bye);
        std::signal(SIGTERM, bye);
#ifdef SIGHUP
        // Not a POSIX-only nicety by choice: Windows has no SIGHUP at all,
        // and referring to it there is a compile error, not a no-op.
        std::signal(SIGHUP, bye);
#endif

        while (llm::serverAlive(pid)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
        printf("runner exited.\n");
        return 0;
    }
    if (what == "status") {
        printf("advisor runner\n");
        printf("  directory : %s\n", llm::installDir(dataDir).c_str());
        printf("  installed : %s\n", llm::installed(dataDir) ? "yes" : "no");
        if (!llm::installedPath(dataDir).empty()) {
            printf("  binary    : %s\n", llm::installedPath(dataDir).c_str());
        }
        if (llm::canInstall()) {
            printf("  available : yes -- run --llm-install\n");
            printf("  source    : %s\n", llm::describeDownload().c_str());
        } else {
            printf("  available : not through this game on this platform.\n");
            printf("              %s\n", llm::manualInstructions());
        }
        return 0;
    }

    if (what == "uninstall") {
        const bool gone = llm::uninstall(dataDir);
        printf(gone ? "removed %s\n" : "could NOT remove %s\n",
               llm::installDir(dataDir).c_str());
        return gone ? 0 : 1;
    }

    if (what == "install") {
        if (!llm::canInstall()) {
            fprintf(stderr, "This game does not install a runner on this platform.\n%s\n",
                    llm::manualInstructions());
            return 2;
        }
        if (llm::installed(dataDir)) {
            printf("already installed at %s\n", llm::installedPath(dataDir).c_str());
            return 0;
        }
        // Said out loud before anything is fetched, on a server too: a headless
        // box downloading an executable without saying so is worse, not better,
        // because nobody is watching it.
        printf("This will download and run third-party software:\n");
        printf("  %s\n", llm::describeDownload().c_str());
        printf("  into %s\n", llm::installDir(dataDir).c_str());
        printf("Proceed? [y/N]: ");
        fflush(stdout);
        char answer[8] = {0};
        if (!fgets(answer, sizeof answer, stdin) ||
            (answer[0] != 'y' && answer[0] != 'Y')) {
            printf("Nothing was downloaded.\n");
            return 1;
        }
        const llm::Install result = llm::fetch(dataDir, [](const char* step) {
            printf("  %s\n", step);
            fflush(stdout);
        });
        if (!result.ok()) {
            fprintf(stderr, "%s\n", result.error.c_str());
            return 3;
        }
        printf("Installed: %s\n", result.path.c_str());
        printf("Start it and pull a model, e.g.:\n");
        printf("  %s serve &\n", result.path.c_str());
        printf("  %s pull gemma3:4b\n", result.path.c_str());
        return 0;
    }
    return 2;
}

int main(int argc, char** argv) {
    // ── The advisor-runner flags, first ──
    //
    // Before any of the heavy paths below: installing or removing a runner
    // should not require a map to load, and --llm-status on a broken data
    // directory should still answer.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--llm-pull") {
            if (i + 1 >= argc) { fprintf(stderr, "--llm-pull needs a model name\n"); return 2; }
            std::string dataDir = "data";
            std::string api = "http://127.0.0.1:11434";
            for (int k = 1; k + 1 < argc; ++k) {
                if (std::string(argv[k]) == "--data") dataDir = argv[k + 1];
                if (std::string(argv[k]) == "--llm-endpoint") api = llm::apiRootOf(argv[k + 1]);
            }
            const std::string stream = dataDir + "/llm-pull.ndjson";
            printf("Pulling %s through %s ...\n", argv[i + 1], api.c_str());
            const bool ok = llm::pullModel(api, argv[i + 1], stream);
            const llm::PullProgress p = llm::pullProgress(stream);
            std::error_code ec;
            std::filesystem::remove(stream, ec);
            if (!ok || !p.error.empty()) {
                fprintf(stderr, "%s\n", p.error.empty()
                        ? "Ollama did not accept that. Is it running?" : p.error.c_str());
                return 3;
            }
            printf("Done.\n");
            return 0;
        }
        if (arg != "--llm-status" && arg != "--llm-install" &&
            arg != "--llm-uninstall" && arg != "--llm-serve") continue;
        std::string dataDir = "data";
        for (int k = 1; k + 1 < argc; ++k)
            if (std::string(argv[k]) == "--data") dataDir = argv[k + 1];
        return llmCommand(arg.substr(6), dataDir);
    }

    // Every raylib entry point in this binary is a no-op except the ones the
    // simulation reads data through, and nothing seeds the C RNG that combat
    // rolls come from. See ServerRaylib.cpp.
    odServerSeedRng((unsigned int)time(nullptr));

    // ── headless AI modes ──
    //
    // Handled before the config file, because they are not a server: they take
    // no session, open no port, and have nothing a server.json would say.
    // --merge-ai <out.bin> <in1.bin> [in2.bin ...]
    //
    // HERE AS WELL AS IN THE GAME BINARY, because the pool that produces the
    // inputs now runs on this one: train_parallel.py switched to the headless
    // binary so its workers stop dying when the display sleeps, and then its
    // final step -- folding the workers back into data/ai/model.bin -- called a
    // flag this parser rejected. Measured 2026-08-24: eleven hours and 96 maps
    // across three workers, stopped cleanly, "[POOL] merging 3 model(s)"
    // printed, and model.bin came back byte-identical to the one the run
    // started from. The training was all still there in the worker files; the
    // step that harvests it was the half that could not run.
    //
    // AISystem::mergeModelFiles is static and touches no Game, no window and no
    // world, which is why it can live in both.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--merge-ai") continue;
        std::vector<std::string> inputs;
        for (int k = i + 2; k < argc && std::string(argv[k]).rfind("--", 0) != 0; ++k)
            inputs.push_back(argv[k]);
        if (i + 1 >= argc || inputs.empty()) {
            fprintf(stderr, "--merge-ai needs an output path and at least one input\n");
            return 2;
        }
        return AISystem::mergeModelFiles(argv[i + 1], inputs) ? 0 : 1;
    }

    // --reset-ai-head <model.bin> <module>
    //
    // WIRED 2026-09-12 (journal 308). AISystem::resetModuleHead has existed,
    // documented as "Backs `--reset-ai-head`", with NO CALLER anywhere --
    // while AISystem.h, tools/ai_bench.py (twice) and docs/ai/LOOP.md all told
    // the reader to use the flag. LOOP.md's rule for a dead action is "below
    // ~1e-6 is out of reach of a bias and needs --reset-ai-head and a
    // retrain", and journals 305-307 found twelve actions below that in three
    // different models, so the one tool the protocol prescribes for the
    // situation turned out never to have been connected.
    //
    // Same placement and the same argument as --merge-ai above: static, takes
    // no session, opens no port, touches no Game and no world.
    //
    // The module is a MOD_* index -- 0 economy, 1 politics, 2 war, 3 navy, and
    // 4 (MOD_COUNT) for the diplomacy head. It rewrites the model IN PLACE, so
    // pass a copy unless you mean it.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--reset-ai-head") continue;
        if (i + 2 >= argc) {
            fprintf(stderr, "--reset-ai-head needs <model.bin> <module 0-4>\n");
            return 2;
        }
        const std::string path = argv[i + 1];
        const int mod = atoi(argv[i + 2]);
        if (mod < 0 || mod > AISystem::MOD_COUNT) {
            fprintf(stderr, "--reset-ai-head: module must be 0-%d\n", AISystem::MOD_COUNT);
            return 2;
        }
        return AISystem::resetModuleHead(path, mod) ? 0 : 1;
    }

    // ── A BENCH SEAT PLAYED BY HAND, HEADLESS ──
    //
    // `--bench-agent` existed only in main.cpp, which calls init() and therefore
    // opens a window and needs OpenGL. That put the one FAIR human-vs-AI
    // instrument this project has behind a renderer: the agent gets the same
    // action menu the policy gets and the same executor runs the choice, so a
    // difference in result is a difference in JUDGEMENT. Same argument as
    // --bench-seat, which moved here earlier for the same reason.
    //
    //   OpenDoctrinesServer --bench-agent 1939:NOR:hood /tmp/od.fifo [--until N] [--seed S]
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bench-agent") != 0) continue;
        if (i + 2 >= argc) {
            fprintf(stderr, "--bench-agent needs a seat and a command FIFO\n");
            return 2;
        }
        AISystem::s_readOnlyModel = true;      // a hand-played seat must never
                                               // write over the trained model
        const std::string seat = argv[i + 1];
        const std::string fifo = argv[i + 2];
        int until = 120;
        unsigned int seed = 20260801u;         // the seat bench's first seed
        std::string dataDir;
        for (int k = 1; k < argc - 1; ++k) {
            if (strcmp(argv[k], "--until") == 0) until = atoi(argv[k + 1]);
            else if (strcmp(argv[k], "--seed") == 0)
                seed = (unsigned int)strtoul(argv[k + 1], nullptr, 10);
            else if (strcmp(argv[k], "--data") == 0) dataDir = argv[k + 1];
        }
        Game game;
        if (!game.srvResolveDataDir(dataDir)) {
            fprintf(stderr, "no data directory -- pass --data <dir>\n");
            return 2;
        }
        return game.runBenchAgent(seat, fifo, seed, until) ? 0 : 1;
    }

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
            // Seat-bench state, applied to the Game once it exists. See below.
            std::string benchSeat;
            int rushNeighbours = 0;
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
                // THE SCRIPTED RUNG, which only main.cpp could switch on until
                // now. tools/ai_bench.py takes --vs-script, and the bench moved
                // to this binary -- so the flag was accepted by the bench, hit a
                // parser that ignores what it does not know, and the run
                // measured a DIFFERENT control than the one asked for, silently,
                // under the right heading. "Beats a coin flip" is not the
                // question when the goal is parity with a competent player.
                else if (f == "--vs-script")               AISystem::s_scriptedControl = true;
                else if (f == "--script-duel") {
                    AISystem::s_scriptedControl = true;
                    AISystem::s_scriptDuel = true;
                }
                // ── THE SEAT BENCH, in the binary that does not open a window ──
                //
                // These three were parsed only by main.cpp, so tools/od_bench.py
                // had to drive the GAME binary -- which means an OpenGL window
                // per seat, eighteen of them per rating, popping up in front of
                // whoever is using the machine, and a renderer's worth of
                // textures resident the whole time. Nothing in a bench run
                // draws anything. Parsed here they are exactly the same three
                // switches: a seat, who rushes, and which exploit they play.
                //
                // Identical semantics to main.cpp on purpose, including the
                // clamp on the variant, because the bench's whole claim is that
                // a score taken today compares with one taken last month. Two
                // parsers that drift make that false silently.
                else if (f == "--bench-seat" && k + 1 < argc) {
                    benchSeat = argv[k + 1];
                    AISystem::s_scriptedControl = true;
                }
                else if (f == "--rush-neighbours") {
                    rushNeighbours = 1;
                    if (k + 1 < argc && strcmp(argv[k + 1], "all") == 0) rushNeighbours = -1;
                    else if (k + 1 < argc && argv[k + 1][0] >= '0' && argv[k + 1][0] <= '9')
                        rushNeighbours = atoi(argv[k + 1]);
                }
                else if (f == "--vs-exploit" && k + 1 < argc) {
                    AISystem::s_scriptedControl = true;
                    AISystem::s_exploitVariant =
                        std::clamp(atoi(argv[k + 1]), 2,
                                   (int)AISystem::SCRIPT_VARIANT_COUNT - 1);
                }
            }
            Game game;
            // After construction, because these are members rather than
            // statics. setBenchSeat parses the "map:ISO" spec itself.
            if (!benchSeat.empty()) game.setBenchSeat(benchSeat);
            if (rushNeighbours != 0) game.setBenchRushNeighbours(rushNeighbours);
            return game.runHeadlessAI(o);
        }
    }


    // --probe-trade <map:ISO> [--seed N]
    //
    // WIRED 2026-09-12 (journal 311). Game::runTradeProbe has existed at
    // Game_AITrain.cpp:2782, declared in Game.h with a full doc comment, with
    // NO CALLER -- while docs/ai/LOOP.md, docs/ai/BACKLOG.md and
    // docs/design/community-roadmap-2026-09.md all referred to the flag, the
    // last of them asserting that it "exists".
    //
    // It is the only thing that exercises the trade rules: no eval proposes a
    // trade, so decideDiplomacy's trade branch is never reached by the bench.
    // Journal 256 records trade as "offered 7.5%, taken 0.00%" -- a subsystem
    // with a dead action, no eval coverage by construction, and its one test
    // unreachable.
    //
    // Needs a Game (runTradeProbe is a member, not static), so it sits here
    // rather than beside --merge-ai and --reset-ai-head above.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--probe-trade") continue;
        if (i + 1 >= argc) {
            fprintf(stderr, "--probe-trade needs a seat, e.g. 1914:FRA\n");
            return 2;
        }
        const std::string seatSpec = argv[i + 1];
        unsigned int seed = 20260801;
        for (int k = 1; k + 1 < argc; ++k)
            if (std::string(argv[k]) == "--seed") seed = (unsigned int)atoi(argv[k + 1]);
        // The no-save guard now lives at the TOP of Game::runTradeProbe, so a
        // future caller cannot reintroduce the hazard by forgetting it here --
        // which is what happened when this flag was first wired (journal 314).
        Game game;
        // Same setup runHeadlessAI does before it touches a map. Without it
        // the .odmap is found but its sibling assets are not -- the first
        // attempt died on "Failed to load land_sea.png", which is terrain
        // data rather than decoration.
        std::string probeData;
        for (int k = 1; k + 1 < argc; ++k)
            if (std::string(argv[k]) == "--data") probeData = argv[k + 1];
        if (!game.srvResolveDataDir(probeData)) {
            fprintf(stderr, "--probe-trade: no data directory -- pass --data <dir>\n");
            return 2;
        }
        return game.runTradeProbe(seatSpec, seed) ? 0 : 1;
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
                 a == "--scenarios" || a == "--resource-limit" ||
                 a == "--vs-script" || a == "--script-duel") {
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
