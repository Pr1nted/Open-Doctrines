# Shared by the linux-<arch>.cmake cross toolchains. Not used on its own.
#
# MULTIARCH, NOT A SYSROOT. Debian and Ubuntu install a foreign architecture's
# libraries side by side with the host's (`dpkg --add-architecture armhf`, then
# `apt install libx11-dev:armhf ...`): headers in /usr/include, libraries in
# /usr/lib/<triplet>. The cross gcc already searches its own triplet directory,
# so the only thing CMake needs told is which directory that is -- find_library
# then prefers /usr/lib/<triplet>, and pkg-config is pointed at that triplet's
# .pc files instead of the host's (which would hand back host library paths).
#
# Expects OD_TRIPLET (gcc's triplet) and OD_MULTIARCH (Debian's directory name,
# which differs for i386: gcc says i686-linux-gnu, Debian says i386-linux-gnu).

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_C_COMPILER   ${OD_TRIPLET}-gcc)
set(CMAKE_CXX_COMPILER ${OD_TRIPLET}-g++)
set(CMAKE_LIBRARY_ARCHITECTURE ${OD_MULTIARCH})

set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/${OD_MULTIARCH}/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

# Run what was built, under QEMU user mode, where the host cannot run it itself.
# ctest prefixes every test with this, which is how the cross-built tests run on
# an x86-64 CI runner (or an arm64 guest). OD_QEMU names the user-mode binary.
if(DEFINED OD_QEMU)
    find_program(OD_QEMU_BIN NAMES ${OD_QEMU}-static ${OD_QEMU})
    if(OD_QEMU_BIN)
        set(CMAKE_CROSSCOMPILING_EMULATOR "${OD_QEMU_BIN};-L;/usr/${OD_TRIPLET}")
    endif()
endif()
