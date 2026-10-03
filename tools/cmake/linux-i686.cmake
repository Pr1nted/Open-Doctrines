# 32-bit x86 Linux.   cmake -S . -B build-i686 -DCMAKE_TOOLCHAIN_FILE=tools/cmake/linux-i686.cmake
# Needs: crossbuild-essential-i386 (or g++-i686-linux-gnu), and the :i386 -dev
# packages for X11, GL and ALSA. See the release workflow for the exact list.
set(CMAKE_SYSTEM_PROCESSOR i686)
set(OD_TRIPLET   i686-linux-gnu)
set(OD_MULTIARCH i386-linux-gnu)
# An x86-64 host runs i686 natively; anything else needs QEMU.
if(NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(OD_QEMU qemu-i386)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/linux-cross-common.cmake)
