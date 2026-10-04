# 64-bit RISC-V Linux (lp64d -- every riscv64 distribution's ABI).
#   cmake -S . -B build-riscv64 -DCMAKE_TOOLCHAIN_FILE=tools/cmake/linux-riscv64.cmake
# Needs: crossbuild-essential-riscv64 (Ubuntu; Debian 12 ships only the compiler),
# the :riscv64 -dev packages, and qemu-user to run tests.
set(CMAKE_SYSTEM_PROCESSOR riscv64)
set(OD_TRIPLET   riscv64-linux-gnu)
set(OD_MULTIARCH riscv64-linux-gnu)
set(OD_QEMU      qemu-riscv64)
include(${CMAKE_CURRENT_LIST_DIR}/linux-cross-common.cmake)
