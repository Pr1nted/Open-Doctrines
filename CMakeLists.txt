cmake_minimum_required(VERSION 3.20)

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
        set(OD_MODS_BACKEND "wamr")
    endif()
endif()
message(STATUS "Gearbox mod runtime backend: ${OD_MODS_BACKEND}")

# --- Game target ---
set(GAME_SOURCES
    main.cpp
    src/Game.cpp
    src/Game_Browsers.cpp
    src/Game_Claims.cpp
    src/Game_Economy.cpp
    src/Game_Loading.cpp
    src/Game_MapEditor.cpp
    src/Game_Menus.cpp
    src/Game_Mods.cpp
    src/Game_Policies.cpp
    src/Game_Render.cpp
    src/Game_Research.cpp
    src/Game_TurnLogic.cpp
    src/Game_AITrain.cpp
    src/Game_Update.cpp
    src/Game_UI.cpp
    src/MapEditor.cpp
    src/ProceduralGenerator.cpp
    src/ScriptEngine.cpp
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
    src/mods/ModRuntime.cpp
    src/mods/ModHost.cpp
    src/mods/ModManager.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)

# The NN inner loops (matvec + Adam) dominate AI training time; fast-math lets
# the compiler vectorize the sqrt/fma chains. Scoped to this file only so game
# logic keeps strict IEEE behavior.
set_source_files_properties(src/ai/NeuralNet.cpp PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math")

add_executable(OpenDoctrines ${GAME_SOURCES})
if(APPLE)
    set_target_properties(OpenDoctrines PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/dist/OpenDoctrines.app/Contents/Info.plist"
    )
    # Copy icon.icns into the app bundle's Resources
    set(ICON_SRC "${CMAKE_SOURCE_DIR}/dist/OpenDoctrines.app/Contents/Resources/icon.icns")
    add_custom_command(TARGET OpenDoctrines POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:OpenDoctrines>/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ICON_SRC}" "$<TARGET_BUNDLE_CONTENT_DIR:OpenDoctrines>/Resources/"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "${CMAKE_SOURCE_DIR}/data" "$<TARGET_BUNDLE_CONTENT_DIR:OpenDoctrines>/data"
    )
elseif(WIN32)
    set_target_properties(OpenDoctrines PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(OpenDoctrines PRIVATE "${CMAKE_SOURCE_DIR}/data/Icon/resources.rc")
endif()
target_include_directories(OpenDoctrines PRIVATE src tools/map_generator)
# Both system raylib (via find_package) and FetchContent create a target named raylib
target_link_libraries(OpenDoctrines raylib)

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
        "-sASYNCIFY"
        # The Gearbox ABI has i64 returns (fuel_budget, province_population).
        # Without BigInt they cannot cross the JS boundary that the browser mod
        # backend goes through.
        "-sWASM_BIGINT"
        "-sALLOW_TABLE_GROWTH"
        "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','stringToUTF8']"
        "-sTOTAL_MEMORY=2GB"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMAXIMUM_MEMORY=4GB"
        "--shell-file" "${CMAKE_SOURCE_DIR}/shell.html"
        "--preload-file" "${CMAKE_SOURCE_DIR}/data@/data"
    )
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

# --- .odmod reader tests ---
# Fixture-driven; builds hostile archives at runtime rather than committing
# them. Not built by default -- `cmake --build . --target ModArchiveTest`.
add_executable(ModArchiveTest EXCLUDE_FROM_ALL
    tests/mod_archive_test.cpp
    src/mods/ModPackage.cpp
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
    src/mods/ModRuntime.cpp
    src/mods/ModHost.cpp
    src/mods/ModManager.cpp
    tools/map_generator/miniz.c
    tools/map_generator/miniz_tdef.c
    tools/map_generator/miniz_tinfl.c
    tools/map_generator/miniz_zip.c
)
target_include_directories(ModManagerTest PRIVATE src tests tools/map_generator)
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
    src/mods/ModRuntime.cpp
    src/mods/ModHost.cpp
    src/mods/ModManager.cpp
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
elseif(WIN32)
    set(CPACK_GENERATOR "NSIS")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
