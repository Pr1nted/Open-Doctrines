cmake_minimum_required(VERSION 3.20)
project(OpenDoctrines VERSION 1.0.2)

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

if(EMSCRIPTEN)
    set_target_properties(OpenDoctrines PROPERTIES SUFFIX ".html")
    target_compile_options(OpenDoctrines PRIVATE )
    target_link_options(OpenDoctrines PRIVATE
        "-sUSE_GLFW=3"
        "-sASYNCIFY"
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
