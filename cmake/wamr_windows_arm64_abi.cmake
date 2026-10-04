# Give Windows on Arm64 the Arm64 calling convention in WAMR's native calls.
#
#   cmake -DWAMR_SRC=<wamr source dir> -P cmake/wamr_windows_arm64_abi.cmake
#
# WHAT IS BROKEN UPSTREAM (WAMR 2.4.5)
#
# wasm_runtime_invoke_native() packs a host call's arguments into a buffer that
# the invokeNative trampoline then loads into registers. For 64-bit targets it
# special-cases Windows:
#
#     #if defined(_WIN32) || defined(_WIN32_)
#     #define MAX_REG_FLOATS 4
#     #define MAX_REG_INTS 4
#     ...
#     #if defined(_WIN32) || defined(_WIN32_) || defined(BUILD_TARGET_RISCV64_LP64)
#     #define n_fps n_ints
#
# Both are the Windows x64 convention -- four argument registers, assigned by
# argument POSITION. But _WIN32 is defined on every Windows, and Windows on Arm64
# uses AAPCS64 for ordinary calls: eight integer and eight floating-point
# registers, counted independently. So an Arm64 build packed the buffer for x64
# while the trampoline (src/mods/invokeNative_arm64_msvc.asm, a translation of
# WAMR's own AAPCS64 one) unpacked it for Arm64. The first stack slot landed in
# x0, where the exec_env belongs, and the first host function to use its
# exec_env dereferenced an argument value: ModRuntimeTest died in
# wasm_exec_env_get_module_inst with x0 = 0x64 (an argument, 100), called from
# ui_panel_register. Host calls that ignore exec_env passed, which is why the
# capability tests did not see it.
#
# THE FIX is to make both conditions exclude BUILD_TARGET_AARCH64, so Windows on
# Arm64 takes the AAPCS64 layout every other Arm64 build already uses. x64 Windows
# is unchanged; every non-Windows build is unchanged (the conditions are false
# there before and after), which is why this applies everywhere rather than on
# one platform -- a source tree that differs per host is one nobody can reproduce.

if(NOT DEFINED WAMR_SRC)
    message(FATAL_ERROR "wamr_windows_arm64_abi.cmake: pass -DWAMR_SRC=<dir>")
endif()

set(_f "${WAMR_SRC}/core/iwasm/common/wasm_runtime_common.c")
if(NOT EXISTS "${_f}")
    message(FATAL_ERROR "wamr_windows_arm64_abi.cmake: no such file: ${_f}")
endif()

file(READ "${_f}" _src)

# The two edits, as (before, after) pairs. Each `before` must occur exactly once.
set(_a_before "#if defined(_WIN32) || defined(_WIN32_)\n#define MAX_REG_FLOATS 4\n#define MAX_REG_INTS 4\n")
set(_a_after  "#if (defined(_WIN32) || defined(_WIN32_)) && !defined(BUILD_TARGET_AARCH64)\n#define MAX_REG_FLOATS 4\n#define MAX_REG_INTS 4\n")
set(_b_before "#if defined(_WIN32) || defined(_WIN32_) || defined(BUILD_TARGET_RISCV64_LP64)\n    /* important difference in calling conventions */\n#define n_fps n_ints\n")
set(_b_after  "#if ((defined(_WIN32) || defined(_WIN32_)) && !defined(BUILD_TARGET_AARCH64)) || defined(BUILD_TARGET_RISCV64_LP64)\n    /* important difference in calling conventions */\n#define n_fps n_ints\n")

string(FIND "${_src}" "${_a_after}" _a_done)
string(FIND "${_src}" "${_b_after}" _b_done)
if(NOT _a_done EQUAL -1 AND NOT _b_done EQUAL -1)
    # The patch step re-runs whenever the tree is re-populated; doing nothing is
    # the correct answer the second time.
    message(STATUS "WAMR: Windows Arm64 calling convention already patched")
    return()
endif()

foreach(_p a b)
    string(FIND "${_src}" "${_${_p}_before}" _at)
    if(_at EQUAL -1)
        # Upstream moved and this script no longer knows what it is editing.
        # Fail the configure rather than build an Arm64 Windows mod runtime that
        # passes every host call its arguments in the wrong registers.
        message(FATAL_ERROR
            "wamr_windows_arm64_abi.cmake: the expected text (edit ${_p}) is not in\n"
            "  ${_f}\n"
            "WAMR has changed. Check whether wasm_runtime_invoke_native still applies\n"
            "the Windows x64 register rules to Arm64, and update or retire this patch.")
    endif()
    string(REPLACE "${_${_p}_before}" "${_${_p}_after}" _src "${_src}")
endforeach()

file(WRITE "${_f}" "${_src}")
message(STATUS "WAMR: Windows Arm64 calling convention patched in (AAPCS64, not x64)")
