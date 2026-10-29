# DJGPP: 32-bit protected-mode DOS programs, cross-compiled from Linux.
#
#   cmake -S . -B build-dos -DCMAKE_TOOLCHAIN_FILE=cmake/dos/djgpp.toolchain.cmake
#
# DJGPP_PREFIX (environment or -D) is where tools/dos_toolchain.sh unpacked
# the compiler: <prefix>/bin/i586-pc-msdosdjgpp-gcc and friends.
#
# CMake has no DOS platform module, so the system is Generic and the build
# keys on DJGPP (set here) instead: OD_DOS defaults on, the odseal archive is
# dos-i586. Executables get the .exe DJGPP's stub makes them into.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR i586)
set(DJGPP TRUE)

if(NOT DJGPP_PREFIX)
    if(DEFINED ENV{DJGPP_PREFIX})
        set(DJGPP_PREFIX "$ENV{DJGPP_PREFIX}")
    else()
        set(DJGPP_PREFIX "$ENV{HOME}/djgpp")
    endif()
endif()
set(DJGPP_PREFIX "${DJGPP_PREFIX}" CACHE PATH "Where the DJGPP cross compiler is installed")
set(_dj "${DJGPP_PREFIX}/bin/i586-pc-msdosdjgpp")

set(CMAKE_C_COMPILER   "${_dj}-gcc")
set(CMAKE_CXX_COMPILER "${_dj}-g++")
set(CMAKE_AR           "${_dj}-ar"     CACHE FILEPATH "")
set(CMAKE_RANLIB       "${_dj}-ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP        "${_dj}-strip"  CACHE FILEPATH "")
# A Pentium, said outright. (The first DOSBox-X run died with SIGILL on an SSE
# instruction; that came from CMakeLists.txt's -msse2 for 32-bit x86, which
# now leaves DJGPP out.)
set(CMAKE_C_FLAGS_INIT   "-march=i586")
set(CMAKE_CXX_FLAGS_INIT "-march=i586")

set(CMAKE_EXECUTABLE_SUFFIX ".exe")
set(CMAKE_EXECUTABLE_SUFFIX_C ".exe")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".exe")

set(CMAKE_FIND_ROOT_PATH "${DJGPP_PREFIX}/i586-pc-msdosdjgpp")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
