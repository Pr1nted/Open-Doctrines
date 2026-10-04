# 32-bit Arm Linux, hard-float (armhf): Raspberry Pi 2 and later on a 32-bit OS,
# and other ARMv7 boards.
#   cmake -S . -B build-armv7 -DCMAKE_TOOLCHAIN_FILE=tools/cmake/linux-armv7.cmake
# Needs: crossbuild-essential-armhf, the :armhf -dev packages, and qemu-user to
# run tests. armv7l is what a native 32-bit Arm `uname -m` says, so the odseal
# and WAMR selection see the same name here as on the device.
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(OD_TRIPLET   arm-linux-gnueabihf)
set(OD_MULTIARCH arm-linux-gnueabihf)
set(OD_QEMU      qemu-arm)
include(${CMAKE_CURRENT_LIST_DIR}/linux-cross-common.cmake)
