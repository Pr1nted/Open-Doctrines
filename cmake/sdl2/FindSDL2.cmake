# Answers raylib's find_package(SDL2 REQUIRED) with the SDL2 this build made
# from source (CMakeLists.txt, OD_DESKTOP_SDL), so raylib links that SDL and
# never a system copy. On CMAKE_MODULE_PATH only when OD_DESKTOP_SDL is on.
if(TARGET SDL2::SDL2)
    set(SDL2_FOUND TRUE)
    set(SDL2_VERSION 2.30.9)
else()
    set(SDL2_FOUND FALSE)
    if(SDL2_FIND_REQUIRED)
        message(FATAL_ERROR "FindSDL2: no SDL2::SDL2 target -- OD_DESKTOP_SDL should have fetched one")
    endif()
endif()
