cmake_minimum_required(VERSION 3.20)

# Set BEFORE project(), which is the only place it takes effect.
#
# Without this the binary's minimum OS is whatever SDK the build machine
# happens to have, and nothing warns you. A build here produced a binary
# stamped `minos 26.0` while Info.plist advertised LSMinimumSystemVersion 11.0
# -- so the app store page said Big Sur and the executable refused to launch on
# anything below macOS 26. CI would have shipped a different (equally wrong)
# floor again, set by the runner image rather than by any decision.
#
# 11.0 because that is what Info.plist has always claimed, and because it is
# the first release on Apple Silicon: below it there is no arm64 build to want.
# Raising it is a deliberate choice to drop machines, so it belongs here in one
# place next to the version, not in whatever Xcode a contributor installed.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

# The version lives in one file, ./VERSION, in the form MAJOR.MINOR.PATCH plus a
# state letter (a alpha, b beta, r release, s snapshot, optionally followed by a
# snapshot counter). tools/release.py is the only thing that edits it.
#
# CMake's project() takes only the numeric part; the full string, letter and all,
# is what the game displays and stamps into saves. Deriving both from one file is
# what stops the packaged version and the displayed version disagreeing, which is
# exactly what happened while CMakeLists said 1.0.2 and Game.cpp said 1.0.2a.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" OD_VERSION_RAW)
string(STRIP "${OD_VERSION_RAW}" OD_VERSION_FULL)
if(NOT OD_VERSION_FULL MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "VERSION file is malformed: '${OD_VERSION_FULL}'")
endif()
set(OD_VERSION_NUMERIC "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")

project(OpenDoctrines VERSION ${OD_VERSION_NUMERIC})
add_compile_definitions(OD_VERSION_STRING="${OD_VERSION_FULL}")
message(STATUS "OpenDoctrines version ${OD_VERSION_FULL}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Raylib: try system first, fall back to FetchContent ---
find_package(raylib 5.5 QUIET)
if(NOT raylib_FOUND)
    message(STATUS "raylib not found system-wide, fetching via FetchContent...")
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
    set(SUPPORT_FILEFORMAT_JPG OFF CACHE BOOL "" FORCE)
    if(EMSCRIPTEN)
        set(PLATFORM "Web" CACHE STRING "raylib platform for Emscripten" FORCE)
    endif()
    FetchContent_Declare(raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.5
    )
    FetchContent_MakeAvailable(raylib)
    set(raylib_LIBRARY raylib)
endif()

# --- Gearbox mod runtime ---
#
# Optional by design. The game must build on a machine that cannot fetch WAMR
# (no network, unsupported target), so a build without it still compiles and
# links -- the mod menu then reports every mod as Failed with a diagnostic
# saying the runtime is unavailable, rather than the build breaking.
option(OD_ENABLE_MODS "Build the Gearbox WASM mod runtime" ON)

set(OD_MODS_BACKEND "none")
if(OD_ENABLE_MODS)
    if(EMSCRIPTEN)
        # The web build is itself WASM, so the browser already has a fast
        # sandbox. Using it is what makes hot-loading work on itch.io without
        # a page reload.
        set(OD_MODS_BACKEND "web")
    else()
        include(FetchContent)

        # Interpreter only, and deliberately no libc: WAMR's LIBC_BUILTIN and
        # LIBC_WASI options are what would give a mod file and clock imports.
        # Leaving them off is the sandbox -- a mod cannot call what was never
        # linked into its instance.
        set(WAMR_BUILD_INTERP           1)
        # The fast interpreter is roughly twice the speed of the classic one and
        # is what every shipped SDK is measured against. It has two structural
        # limits, both INT16_MAX, that the classic interpreter does not:
        #
        #   - a function's params + locals (wasm_loader.c, "fast interpreter
        #     offset overflow")
        #   - a function's operand stack depth (same error, different check)
        #
        # Ordinary mods are nowhere near either. A large C interpreter compiled
        # to wasm can be: CPython 3.12 trips the operand-stack limit and will
        # not load at all under the fast interpreter. Turning this OFF is the
        # only way to run one, at the cost of slowing every other mod, so it is
        # an explicit choice rather than a default.
        option(OD_MODS_FAST_INTERP
               "Use WAMR's fast interpreter (off = classic, slower but no INT16_MAX limits)" ON)
        if(OD_MODS_FAST_INTERP)
            set(WAMR_BUILD_FAST_INTERP  1)
        else()
            set(WAMR_BUILD_FAST_INTERP  0)
        endif()
        set(WAMR_BUILD_AOT              0)
        set(WAMR_BUILD_JIT              0)
        set(WAMR_BUILD_LIBC_BUILTIN     0)
        set(WAMR_BUILD_LIBC_WASI        0)
        set(WAMR_BUILD_LIB_PTHREAD      0)
        set(WAMR_BUILD_LIB_WASI_THREADS 0)
        set(WAMR_BUILD_MULTI_MODULE     0)
        set(WAMR_BUILD_SHARED_MEMORY    0)
        set(WAMR_BUILD_SIMD             0)
        # LLVM emits memory.copy/memory.fill by default, so a mod built with a
        # current clang needs this to link at all.
        set(WAMR_BUILD_BULK_MEMORY      1)
        # Gives us wasm_runtime_set_instruction_count_limit, which is how
        # limits.fuelPerTurn is enforced. Without it an infinite loop in a hook
        # would hang the turn.
        set(WAMR_BUILD_INSTRUCTION_METERING 1)

        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
            set(WAMR_BUILD_TARGET "AARCH64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
            set(WAMR_BUILD_TARGET "X86_64")
        elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(WAMR_BUILD_TARGET "X86_32")
        else()
            set(WAMR_BUILD_TARGET "X86_64")
        endif()

        FetchContent_Declare(wamr
            GIT_REPOSITORY https://github.com/bytecodealliance/wasm-micro-runtime.git
            GIT_TAG WAMR-2.4.5
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL          # keep WAMR's install() rules out of ours
        )
        FetchContent_MakeAvailable(wamr)

        # WAMR's wasm_export.h declares its whole API __declspec(dllimport) on
        # MSVC unless COMPILING_WASM_RUNTIME_API is defined. We link vmlib
        # STATICALLY, so without this every call resolved to an __imp_ symbol
        # that no DLL provides -- 22 unresolved externals at link time, on a
        # Windows build where every source file had compiled cleanly.
        #
        # Set as INTERFACE on vmlib rather than on each consumer: six targets
        # link it, and a define that has to be repeated is a define somebody
        # forgets on the seventh.
        if(MSVC AND TARGET vmlib)
            target_compile_definitions(vmlib INTERFACE COMPILING_WASM_RUNTIME_API)
        endif()
        set(OD_MODS_BACKEND "wamr")
    endif()
endif()
message(STATUS "Gearbox mod runtime backend: ${OD_MODS_BACKEND}")

# --- Multiplayer transport ---
#
# Optional for the same reason the mod runtime is: the game must still build on
# a machine that cannot fetch mbedTLS. Without it, src/net/WsUnavailable.cpp is
# linked instead and the multiplayer menu says this build cannot connect --
# rather than the build failing outright.
#
# The web target needs nothing fetched: the browser already has a WebSocket and
# already has the certificate store, and would not permit a raw socket anyway.
option(OD_ENABLE_NET "Build the multiplayer transport" ON)

set(OD_NET_BACKEND "none")
if(OD_ENABLE_NET)
    if(EMSCRIPTEN)
        set(OD_NET_BACKEND "browser")
    else()
        include(FetchContent)
        # TLS only. No programs, no tests, no self-test binaries: this is
        # linked into a game, and every extra target is build time nobody asked
        # for.
        set(ENABLE_PROGRAMS   OFF CACHE BOOL "" FORCE)
        set(ENABLE_TESTING    OFF CACHE BOOL "" FORCE)
        set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(mbedtls
            GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
            GIT_TAG v3.6.2
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(mbedtls)
        set(OD_NET_BACKEND "mbedtls")
    endif()
endif()
message(STATUS "Multiplayer transport backend: ${OD_NET_BACKEND}")

# --- Peer-to-peer fallback -----------------------------------------------
#
# For the player who cannot reach a host directly: WebRTC data channels over
# ICE, with the offer/answer exchange going through the account service as
# matchmaking (see NetSignal in src/net/NetProtocol.h). The media path is peer
# to peer, so this is not a relay.
#
# OFF by default, and deliberately so. libdatachannel pulls in usrsctp, libjuice
# and plog, which is a large addition to a build that currently fetches two
# things -- and the direct path works without it. A build with this off says
# plainly that the fallback is unavailable rather than failing to compile, which
# is the same bargain the mod runtime and the transport already make.
#
# USE_MBEDTLS matters: without it libdatachannel wants OpenSSL, and this project
# links mbedTLS precisely so that it builds on a machine with nothing installed.
option(OD_ENABLE_P2P "Build the WebRTC peer-to-peer fallback (fetches libdatachannel)" OFF)

set(OD_P2P_BACKEND "none")
if(OD_ENABLE_P2P AND OD_ENABLE_NET AND NOT EMSCRIPTEN)
    include(FetchContent)
    # Ahead of libdatachannel's own modules, so its find_package(MbedTLS) picks
    # up the mbedTLS we already build rather than looking for an installed one.
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    set(USE_MBEDTLS   ON  CACHE BOOL "" FORCE)   # reuse the TLS we already have
    set(NO_MEDIA      ON  CACHE BOOL "" FORCE)   # data channels only
    set(NO_WEBSOCKET  ON  CACHE BOOL "" FORCE)   # we have our own, in src/net
    set(NO_EXAMPLES   ON  CACHE BOOL "" FORCE)
    set(NO_TESTS      ON  CACHE BOOL "" FORCE)
    FetchContent_Declare(libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG v0.24.5
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
    )
    # libdatachannel exports install targets that reference mbedTLS, which is
    # not in any export set because we build it from source rather than
    # installing it. Nothing here is installed -- the game is packaged as an
    # app bundle -- so the rules are skipped for this subtree rather than
    # contorting mbedTLS into an export it has no other use for.
    # libdatachannel's mbedTLS backend references DTLS-SRTP unconditionally,
    # even with NO_MEDIA -- the block is behind `#elif USE_MBEDTLS` rather than
    # behind the media guard. mbedTLS ships that option off, so it is turned on
    # here. It is media-key negotiation we never use; enabling it is cheaper
    # than patching a dependency, and PUBLIC so libdatachannel compiles against
    # exactly the configuration mbedTLS was built with. A mismatch there is the
    # kind of thing that links fine and then misbehaves at runtime.
    if(TARGET mbedcrypto)
        target_compile_definitions(mbedcrypto PUBLIC MBEDTLS_SSL_DTLS_SRTP)
    endif()

    set(_od_skip_install "${CMAKE_SKIP_INSTALL_RULES}")
    set(CMAKE_SKIP_INSTALL_RULES ON)
    FetchContent_MakeAvailable(libdatachannel)
    set(CMAKE_SKIP_INSTALL_RULES "${_od_skip_install}")
    set(OD_P2P_BACKEND "libdatachannel")
endif()

# Both halves of the game need to know whether the fallback exists: the code
# that offers it, and PeerLink itself picking a backend.
if(OD_P2P_BACKEND STREQUAL "libdatachannel")
    set(OD_P2P_DEFS OD_ENABLE_P2P=1)
    set(OD_P2P_LIBS datachannel-static)
else()
    set(OD_P2P_DEFS "")
    set(OD_P2P_LIBS "")
endif()
message(STATUS "Peer-to-peer fallback: ${OD_P2P_BACKEND}")

# --- Game target ---
set(GAME_SOURCES
    main.cpp
    src/Game.cpp
    src/GameUpdates.cpp
    src/Game_Account.cpp
    src/Game_Browsers.cpp
    src/Game_Claims.cpp
    src/Game_Economy.cpp
    src/Game_Loading.cpp
    src/Game_MapEditor.cpp
    src/Game_Menus.cpp
    src/Game_Mods.cpp
    src/Game_Multiplayer.cpp
    src/Game_Policies.cpp
    src/Game_Render.cpp
    src/Game_Research.cpp
    src/Game_TurnLogic.cpp
    src/Game_AITrain.cpp
    src/Game_Screenshots.cpp
    src/Game_Update.cpp
    src/Game_UI.cpp
    src/MapEditor.cpp
    src/ProceduralGenerator.cpp
    src/ScriptEngine.cpp
    src/Audio.cpp
    src/Config.cpp
    src/SaveManager.cpp
    src/GifEncoder.cpp
    src/Game_History.cpp
    src/ai/NeuralNet.cpp
    src/ai/AISystem.cpp
    src/map/LandSeaMap.cpp
    src/map/ProvinceMap.cpp
    src/map/CountryMap.cpp
    src/renderer/MapRenderer.cpp
    src/renderer/FlagRenderer.cpp
    src/renderer/NanoSVGWrapper.cpp
    src/mods/ModPackage.cpp
    src/net/ModAttest.cpp
    src/net/NetProtocol.cpp
    src/net/AccountClient.cpp
    src/net/BadgeStyle.cpp
    src/net/Host.cpp
    src/net/HttpClient.cpp
    src/net/JoinTicket.cpp
    src/net/Lobby.cpp
    src/net/NetUrl.cpp
    src/net/PeerLinkNative.cpp
    src/net/PeerLinkWeb.cpp
    src/net/PeerLinkUnavailable.cpp
    src/net/ServerBook.cpp
    src/net/Session.cpp
    src/net/TlsSocket.cpp
    src/net/TurnRunner.cpp
    src/net/Tunnel.cpp
    src/net/TunnelInstall.cpp
    src/net/HostBook.cpp
    src/net/TurnSeal.cpp
    src/net/TurnStore.cpp
    src/util/Ed25519.cpp
    src/net/WorldSync.cpp
    src/net/WsCommon.cpp
    src/net/WsHandshake.cpp
    src/net/WsServer.cpp
    src/net/WsNative.cpp
    src/net/WsWeb.cpp
    src/net/WsUnavailable.cpp
    src/mods/ModRuntime.cpp
    src/mods/ModHost.cpp
    src/mods/ModManager.cpp
    src/mods/ModUpdates.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)

# The NN inner loops (matvec + Adam) dominate AI training time; fast-math lets
# the compiler vectorize the sqrt/fma chains. Scoped to this file only so game
# logic keeps strict IEEE behavior.
if(MSVC)
    # Same intent, MSVC's spelling. The GCC/Clang flags were being passed to cl
    # verbatim, which answered "command line warning D9002: ignoring unknown
    # option '-O3'" -- so on Windows this file was getting none of the
    # optimisation the comment above promises, silently.
    set_source_files_properties(src/ai/NeuralNet.cpp PROPERTIES COMPILE_OPTIONS "/O2;/fp:fast")
else()
    set_source_files_properties(src/ai/NeuralNet.cpp PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math")
endif()

add_executable(OpenDoctrines ${GAME_SOURCES})
if(APPLE)
    # The bundle's Info.plist and icon are SOURCES, and they live in the source
    # tree. They used to be read out of dist/, which is a build OUTPUT directory
    # and is gitignored -- so they existed on the machine that first made them
    # and in no fresh clone. Configuring macOS then failed with "Info.plist
    # template could not be found", naming a path that had simply never been
    # committed. Nobody noticed because nobody had built macOS from a clean
    # checkout; CI did, the first time it was asked to.
    #
    # The version is substituted rather than written twice. VERSION is the one
    # source of truth (odver.py and the updater both parse it), and a plist
    # carrying its own copy is a plist that drifts.
    configure_file("${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in"
                   "${CMAKE_BINARY_DIR}/Info.plist" @ONLY)
    set_target_properties(OpenDoctrines PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/Info.plist"
    )
    # Copy icon.icns into the app bundle's Resources
    set(ICON_SRC "${CMAKE_SOURCE_DIR}/packaging/macos/icon.icns")
    add_custom_command(TARGET OpenDoctrines POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:OpenDoctrines>/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ICON_SRC}" "$<TARGET_BUNDLE_CONTENT_DIR:OpenDoctrines>/Resources/"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/data" "$<TARGET_BUNDLE_CONTENT_DIR:OpenDoctrines>/data"
    )
elseif(WIN32)
    # WIN32_EXECUTABLE is what stops a console window appearing behind the game.
    # It also makes the linker look for WinMain(), which this program does not
    # have -- it has an ordinary main() -- so the Windows link failed with
    # "unresolved external symbol WinMain". /ENTRY:mainCRTStartup keeps the
    # windowed subsystem and points the entry point back at the CRT startup that
    # calls main(), which is the combination actually wanted.
    set_target_properties(OpenDoctrines PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(OpenDoctrines PRIVATE "/ENTRY:mainCRTStartup")
    endif()
    target_sources(OpenDoctrines PRIVATE "${CMAKE_SOURCE_DIR}/data/Icon/resources.rc")
endif()
target_include_directories(OpenDoctrines PRIVATE src tools/map_generator)
# Both system raylib (via find_package) and FetchContent create a target named raylib
target_link_libraries(OpenDoctrines raylib)

if(OD_NET_BACKEND STREQUAL "mbedtls")
    target_link_libraries(OpenDoctrines MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    target_compile_definitions(OpenDoctrines PRIVATE OD_ENABLE_NET=1)
elseif(OD_NET_BACKEND STREQUAL "browser")
    target_compile_definitions(OpenDoctrines PRIVATE OD_ENABLE_NET=1)
endif()

if(OD_P2P_DEFS)
    target_link_libraries(OpenDoctrines ${OD_P2P_LIBS})
    target_compile_definitions(OpenDoctrines PRIVATE ${OD_P2P_DEFS})
endif()

if(OD_MODS_BACKEND STREQUAL "wamr")
    target_link_libraries(OpenDoctrines vmlib)
    target_compile_definitions(OpenDoctrines PRIVATE OD_MODS_WAMR=1)
elseif(OD_MODS_BACKEND STREQUAL "web")
    target_compile_definitions(OpenDoctrines PRIVATE OD_MODS_WEB=1)
endif()

if(EMSCRIPTEN)
    set_target_properties(OpenDoctrines PROPERTIES SUFFIX ".html")
    target_compile_options(OpenDoctrines PRIVATE )
    target_link_options(OpenDoctrines PRIVATE
        "-sUSE_GLFW=3"
        # emscripten_websocket_*; the browser does the TLS.
        "-lwebsocket.js"
        "-sASYNCIFY"
        # The Gearbox ABI has i64 returns (fuel_budget, province_population).
        # Without BigInt they cannot cross the JS boundary that the browser mod
        # backend goes through.
        "-sWASM_BIGINT"
        "-sALLOW_TABLE_GROWTH"
        # The HEAP* views are NOT optional here, and the reason they look it is
        # that emscripten used to export them unconditionally. It stopped, so
        # this build linked cleanly and then died at startup with
        # "Aborted('HEAPF32' was not exported)" the moment raylib's GL layer
        # touched a vertex buffer -- after the audio had loaded, which made it
        # look like an audio problem.
        #
        # HEAPU8 and HEAPU32 are ours (the mod bridge). The rest are raylib's
        # and emscripten's own library JS. They are all views over one buffer,
        # so exporting the set costs nothing and removes a whole class of
        # "worked until it ran" failure.
        "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','stringToUTF8','HEAP8','HEAPU8','HEAP16','HEAPU16','HEAP32','HEAPU32','HEAP64','HEAPU64','HEAPF32','HEAPF64']"
        "-sTOTAL_MEMORY=2GB"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMAXIMUM_MEMORY=4GB"
        "--shell-file" "${CMAKE_SOURCE_DIR}/shell.html"
        "--preload-file" "${CMAKE_SOURCE_DIR}/data@/data"
        # The preload is downloaded in full before the menu draws, so anything
        # in data/ that the browser cannot use is time the player waits for
        # nothing. Two things qualify outright:
        #   tools/      cloudflared, a 37 MB arm64 Mach-O executable used to
        #               open a host tunnel. Tunnel.cpp is compiled out entirely
        #               under __EMSCRIPTEN__ -- a page cannot spawn a process,
        #               so the web build has no code path that could reach it.
        #   audio/midi/ editable sources for the music. The loader scans
        #               audio/music only; see data/audio/README.md.
        # Together they were 42 MB of a 145 MB package.
        #
        # Written as --exclude-file=PATTERN, one argument, NOT as the two
        # arguments em++ also accepts. CMake de-duplicates repeated link
        # options by exact string, so `"--exclude-file" "A" "--exclude-file"
        # "B"` had its second flag collapsed away and emitted as
        # `--exclude-file A B` -- leaving B as a bare argument that em++ then
        # looked for as an input file and failed the link on. Joining the flag
        # to its value makes the two strings different, so neither is dropped.
        "--exclude-file=*/tools/*"
        "--exclude-file=*/audio/midi/*"
        # The music itself: 52 MB of .ogg, a third of the package, and none of
        # it is needed before the main menu draws. Streamed instead -- Audio.cpp
        # fetches a track the first time it is chosen. The .json SIDECARS stay
        # in the preload deliberately: they are a few KB in total and they are
        # what the index is built from on web, so the mood system knows every
        # track exists before a byte of audio has been downloaded.
        "--exclude-file=*/audio/music/*.ogg"
    )

    # ...which means they have to be somewhere fetchable. Copied next to the
    # page under the SAME path they have in the virtual filesystem, so the URL
    # is the VFS path minus its leading slash and neither side has to translate.
    #
    # DEPLOYING: this directory ships with OpenDoctrines.{html,js,wasm,data}.
    # A deployment that uploads only the four files gets a silent game.
    add_custom_command(TARGET OpenDoctrines POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:OpenDoctrines>/data/audio/music"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/data/audio/music"
                "$<TARGET_FILE_DIR:OpenDoctrines>/data/audio/music"
        COMMENT "Copying streamable music next to the page"
        VERBATIM)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(OpenDoctrines Threads::Threads)
endif()

if(APPLE)
    # IOKit for the power assertion that keeps the Mac awake during headless
    # AI training (see Game_AITrain.cpp); CoreFoundation for CFStringRef.
    target_link_libraries(OpenDoctrines "-framework IOKit" "-framework CoreFoundation")
    # Accelerate provides a vectorised BLAS. NeuralNet::forward uses it for the
    # per-layer matrix-vector product, which profiled as the single largest
    # remaining cost in self-play. Optional and macOS-only: every other platform
    # (and the Emscripten build) compiles the portable scalar loop instead, so
    # this is purely an opt-in speedup, never a build requirement.
    if(NOT EMSCRIPTEN)
        target_link_libraries(OpenDoctrines "-framework Accelerate")
        # ACCELERATE_NEW_LAPACK selects the modern (non-deprecated) CBLAS headers.
        target_compile_definitions(OpenDoctrines PRIVATE
                                   OD_USE_ACCELERATE=1 ACCELERATE_NEW_LAPACK=1)
    endif()
endif()

# --- Map generator target ---
set(MAP_GEN_SOURCES
    tools/map_generator/main.cpp
    tools/map_generator/Generator.cpp
    tools/map_generator/ShapefileReader.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)

add_executable(MapGenerator ${MAP_GEN_SOURCES})
target_include_directories(MapGenerator PRIVATE tools/map_generator src)
target_compile_definitions(MapGenerator PRIVATE _CRT_SECURE_NO_WARNINGS)

if(APPLE)
    target_link_libraries(MapGenerator PRIVATE "-framework CoreFoundation")
endif()

# --- hashing and signature verification ---
# Every vector is from FIPS 180-4 or RFC 8032, never from our own output: this
# is the code that decides whether a join ticket is genuine, and it is the one
# place where being wrong is both silent and total.
add_executable(NetCryptoTest EXCLUDE_FROM_ALL
    tests/net_crypto_test.cpp
    src/util/Ed25519.cpp
)
target_include_directories(NetCryptoTest PRIVATE src tests)

# --- join tickets ---
# The check that used to be Cloudflare's. Almost every case is a refusal,
# because this is what a stranger passes through to be seated on a host's own
# machine. No sockets: the fixture is a real signed ticket, not a live call.
add_executable(NetTicketTest EXCLUDE_FROM_ALL
    tests/net_ticket_test.cpp
    src/net/JoinTicket.cpp
    src/net/HttpClient.cpp
    src/net/NetUrl.cpp
    src/util/Ed25519.cpp
)
target_include_directories(NetTicketTest PRIVATE src tests)

# --- seats remembered across sessions ---
# A campaign played over weeks lives or dies on returning players getting their
# own country back, so the pairing of psid to country is what this guards.
add_executable(NetHostBookTest EXCLUDE_FROM_ALL
    tests/net_hostbook_test.cpp
    src/net/HostBook.cpp
    src/net/Lobby.cpp
    src/net/HttpClient.cpp
    src/net/NetUrl.cpp
    src/net/NetProtocol.cpp
)
target_include_directories(NetHostBookTest PRIVATE src tests)

# --- tunnel address parsing ---
# Against the real shape of what these tools print, banners and all: the failure
# mode is handing players a documentation link instead of the game.
add_executable(NetTunnelTest EXCLUDE_FROM_ALL
    tests/net_tunnel_test.cpp
    src/net/Tunnel.cpp
)
target_include_directories(NetTunnelTest PRIVATE src tests)

# --- sealing turn submissions ---
# The store is a dumb bucket anyone can read and sometimes overwrite, so almost
# every case is something that must NOT open. Needs mbedTLS for AES-GCM.
add_executable(NetSealTest EXCLUDE_FROM_ALL
    tests/net_seal_test.cpp
    src/net/TurnSeal.cpp
)
target_include_directories(NetSealTest PRIVATE src tests)
target_compile_definitions(NetSealTest PRIVATE OD_ENABLE_NET=1)
if(OD_ENABLE_NET AND NOT EMSCRIPTEN)
    target_link_libraries(NetSealTest PRIVATE MbedTLS::mbedcrypto)
endif()

# --- connectivity, end to end ---
# A real NetHost and a real NetSession over loopback, against a stand-in
# account service (tests/mock_issuer.mjs). This is the only test of the SEAM
# between the two halves; run it with tests/connectivity_test.sh, which brings
# the stand-in up and down around it.
add_executable(NetConnectTest EXCLUDE_FROM_ALL
    tests/net_connect_test.cpp
    src/net/Host.cpp
    src/net/Session.cpp
    src/net/Lobby.cpp
    src/net/JoinTicket.cpp
    src/net/ModAttest.cpp
    src/net/NetProtocol.cpp
    src/net/NetUrl.cpp
    src/net/TurnRunner.cpp
    src/net/HttpClient.cpp
    src/net/TlsSocket.cpp
    src/net/WorldSync.cpp
    src/net/WsCommon.cpp
    src/net/WsHandshake.cpp
    src/net/WsNative.cpp
    src/net/WsServer.cpp
    src/util/Ed25519.cpp
)
target_include_directories(NetConnectTest PRIVATE src tests)
target_compile_definitions(NetConnectTest PRIVATE OD_ENABLE_NET=1)
if(OD_ENABLE_NET AND NOT EMSCRIPTEN)
    target_link_libraries(NetConnectTest PRIVATE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
endif()

# --- websocket server ---
# The handshake is pure string work over input that arrives on a listening port
# from strangers, so most of those cases are malformed rather than correct. The
# rest drives a real listener over loopback with a hand-built client, because
# framing bugs do not show up in string tests. No TLS: the server speaks plain
# ws:// by design (see WsServer.h), so this needs no mbedTLS.
add_executable(NetWsServerTest EXCLUDE_FROM_ALL
    tests/net_wsserver_test.cpp
    src/net/WsHandshake.cpp
    src/net/WsServer.cpp
)
target_include_directories(NetWsServerTest PRIVATE src tests)
target_compile_definitions(NetWsServerTest PRIVATE OD_ENABLE_NET=1)

# --- lobby rules ---
# Pure logic: no sockets, no renderer, no game headers. Almost every case is a
# rule a client must not be able to break by asking nicely.
add_executable(NetLobbyTest EXCLUDE_FROM_ALL
    tests/net_lobby_test.cpp
    src/net/Lobby.cpp
    src/net/NetProtocol.cpp
    src/net/TurnRunner.cpp
)
target_include_directories(NetLobbyTest PRIVATE src tests)

# --- account client ---
# The pure halves: the JSON reader that parses replies from the network, and
# the local nickname check. No sockets, so no OD_ENABLE_NET and no mbedTLS.
add_executable(NetAccountTest EXCLUDE_FROM_ALL
    tests/net_account_test.cpp
    src/net/AccountClient.cpp
    src/net/BadgeStyle.cpp
    src/net/HttpClient.cpp
    src/net/NetUrl.cpp
    src/net/ServerBook.cpp
    src/net/TurnStore.cpp
)
target_include_directories(NetAccountTest PRIVATE src tests)
find_package(Threads REQUIRED)
target_link_libraries(NetAccountTest Threads::Threads)

# --- multiplayer wire format ---
# Pure bytes in, bytes out: no raylib, no game headers, no sockets. This is the
# layer that parses input from strangers, so most of the test is about what it
# refuses rather than what it round-trips.
add_executable(NetProtocolTest EXCLUDE_FROM_ALL
    tests/net_protocol_test.cpp
    src/net/NetProtocol.cpp
    src/net/WorldSync.cpp
    src/net/NetUrl.cpp
    src/util/Ed25519.cpp
    src/net/WsCommon.cpp
    src/net/WsUnavailable.cpp
)
target_include_directories(NetProtocolTest PRIVATE src tests)

# --- mod sides and attestation ---
# Pure logic: no archives, no runtime, no network. What a mod's declared side
# means for its grants, and what the two ends of a join tell each other.
add_executable(NetAttestTest EXCLUDE_FROM_ALL
    tests/net_attest_test.cpp
    src/net/ModAttest.cpp
    src/mods/ModPackage.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)
target_include_directories(NetAttestTest PRIVATE src tests tools/map_generator)

# --- .odmod reader tests ---
# Fixture-driven; builds hostile archives at runtime rather than committing
# them. Not built by default -- `cmake --build . --target ModArchiveTest`.
add_executable(ModArchiveTest EXCLUDE_FROM_ALL
    tests/mod_archive_test.cpp
    src/mods/ModPackage.cpp
    src/net/ModAttest.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)
target_include_directories(ModArchiveTest PRIVATE src tests tools/map_generator)

# Drives a real compiled .wasm through the runtime: capability enforcement,
# fuel exhaustion, memory limits, UI panel ownership. Needs the fixture modules
# from tests/build_test_mods.sh; skips those cases loudly if they are absent.
add_executable(ModRuntimeTest EXCLUDE_FROM_ALL
    tests/mod_runtime_test.cpp
    src/mods/ModPackage.cpp
    src/net/ModAttest.cpp
    src/mods/ModRuntime.cpp
    src/mods/ModHost.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)
target_include_directories(ModRuntimeTest PRIVATE src tests tools/map_generator)
if(OD_MODS_BACKEND STREQUAL "wamr")
    target_link_libraries(ModRuntimeTest vmlib)
    target_compile_definitions(ModRuntimeTest PRIVATE OD_MODS_WAMR=1)
endif()

# Exercises the state machine, grants and persistence against a scratch mods
# directory, using the same import/scan path the mod menu uses.
add_executable(ModManagerTest EXCLUDE_FROM_ALL
    tests/mod_manager_test.cpp
    src/mods/ModPackage.cpp
    src/net/ModAttest.cpp
    src/mods/ModRuntime.cpp
    src/mods/ModHost.cpp
    src/mods/ModManager.cpp
    src/mods/ModUpdates.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)
target_include_directories(ModManagerTest PRIVATE src tests tools/map_generator)

# The in-game updater: the version comparison (which must agree with
# tools/odver.py), the hostile-input parsing of a release reply, and the rule
# that installing an update never deletes anything.
# GifEncoder is raylib-free by design (cstdint/cstdio/string/vector only), so
# the timelapse writer can be tested on its own. tests/gif_encoder_check.py then
# decodes what it wrote with Pillow -- an encoder cannot verify its own LZW.
add_executable(GifEncoderTest EXCLUDE_FROM_ALL
    tests/gif_encoder_test.cpp
    src/GifEncoder.cpp)
target_include_directories(GifEncoderTest PRIVATE src)

add_executable(GameUpdatesTest EXCLUDE_FROM_ALL
    tests/game_updates_test.cpp
    src/GameUpdates.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)
target_include_directories(GameUpdatesTest PRIVATE src tests tools/map_generator)
if(OD_MODS_BACKEND STREQUAL "wamr")
    target_link_libraries(ModManagerTest vmlib)
    target_compile_definitions(ModManagerTest PRIVATE OD_MODS_WAMR=1)
endif()

# Asserts the host capability table matches sdk/abi.json in both directions.
# The language bindings under sdk/ are transcriptions of that file and cannot be
# compiled here, so this is how they are kept honest.
add_executable(ModAbiTest EXCLUDE_FROM_ALL
    tests/mod_abi_test.cpp
    src/mods/ModPackage.cpp
    src/net/ModAttest.cpp
    src/mods/ModRuntime.cpp
    src/mods/ModHost.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)
target_include_directories(ModAbiTest PRIVATE src tests tools/map_generator)
if(OD_MODS_BACKEND STREQUAL "wamr")
    target_link_libraries(ModAbiTest vmlib)
    target_compile_definitions(ModAbiTest PRIVATE OD_MODS_WAMR=1)
endif()

# Drives every built example mod through mod_draw_panel against a synthetic
# world and compares the drawn output across languages. This is what catches a
# binding with its arguments in the wrong order -- every other test would pass.
add_executable(ModExamplesTest EXCLUDE_FROM_ALL
    tests/mod_examples_test.cpp
    src/mods/ModPackage.cpp
    src/net/ModAttest.cpp
    src/mods/ModRuntime.cpp
    src/mods/ModHost.cpp
    src/mods/ModManager.cpp
    src/mods/ModUpdates.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)
target_include_directories(ModExamplesTest PRIVATE src tests tools/map_generator)
if(OD_MODS_BACKEND STREQUAL "wamr")
    target_link_libraries(ModExamplesTest vmlib)
    target_compile_definitions(ModExamplesTest PRIVATE OD_MODS_WAMR=1)
endif()

# --- odmod-check: validate and dry-run a .odmod ---
# Ships with the SDK. Kept separate from the game on purpose: the game loads
# mods only from the mod menu, and a "load this file" flag there would defeat
# the point.
add_executable(OdmodCheck
    tools/odmod_check.cpp
    src/mods/ModPackage.cpp
    src/net/ModAttest.cpp
    src/mods/ModRuntime.cpp
    src/mods/ModHost.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)
set_target_properties(OdmodCheck PROPERTIES OUTPUT_NAME odmod-check)
target_include_directories(OdmodCheck PRIVATE src tools/map_generator)
if(OD_MODS_BACKEND STREQUAL "wamr")
    target_link_libraries(OdmodCheck vmlib)
    target_compile_definitions(OdmodCheck PRIVATE OD_MODS_WAMR=1)
endif()

# --- Install rules ---
if(APPLE)
    install(TARGETS OpenDoctrines BUNDLE DESTINATION .)
else()
    install(TARGETS OpenDoctrines RUNTIME DESTINATION bin)
endif()
install(DIRECTORY data/ DESTINATION share/OpenDoctrines/data
    FILES_MATCHING PATTERN "*"
    PATTERN ".DS_Store" EXCLUDE)
if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
    install(FILES LICENSE README.md DESTINATION share/OpenDoctrines/)
endif()

# CPack configuration
set(CPACK_PACKAGE_NAME "OpenDoctrines")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A Grand Strategy Game")
if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
endif()
if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "OpenDoctrines")
elseif(WIN32)
    set(CPACK_GENERATOR "NSIS")

    # PER-USER, not per-machine, and this is the decision the whole installer
    # rests on. The updater works by renaming the running binary and writing
    # beside it, which needs a directory the player can write. Program Files is
    # not: an update there would need elevation the game does not ask for, and
    # would fail with a permissions error the player cannot act on.
    #
    # $LOCALAPPDATA keeps the updater working with no UAC prompt at install OR
    # update time. It is what VS Code, Discord and Chrome do, for this reason.
    set(CPACK_NSIS_INSTALL_ROOT "$LOCALAPPDATA\\\\Programs")
    set(CPACK_NSIS_DEFINES "RequestExecutionLevel user")

    set(CPACK_PACKAGE_INSTALL_DIRECTORY "OpenDoctrines")
    set(CPACK_NSIS_PACKAGE_NAME "OpenDoctrines")
    set(CPACK_NSIS_DISPLAY_NAME "OpenDoctrines")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "OpenDoctrines.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\OpenDoctrines.lnk' '$INSTDIR\\\\OpenDoctrines.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$START_MENU\\\\OpenDoctrines.lnk'")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

# The marker that tells the updater this copy is not its own to replace.
#
# Installed by the INSTALLERS only. The plain zip and the tarball do not carry
# it, so those keep updating themselves as they always have -- which is the
# point: one build serves both, and what differs is how it reached the machine.
#
# See GameUpdates::managedInstall(). Without this an installed copy would try
# to rename its own binary underneath the installer that owns it, and on
# Windows that means the uninstaller's file list stops matching what is on disk.
if(NOT EMSCRIPTEN)
    file(WRITE "${CMAKE_BINARY_DIR}/MANAGED"
         "This copy of OpenDoctrines was put here by an installer, a store or a\n"
         "package manager, so the in-game updater will not replace it. Update it\n"
         "the same way you installed it.\n\n"
         "Delete this file only if you moved the game somewhere you manage\n"
         "yourself and want it to update itself again.\n")
    install(FILES "${CMAKE_BINARY_DIR}/MANAGED" DESTINATION .)
endif()

include(CPack)
