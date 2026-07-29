# Give WAMR's fast interpreter its instruction limit back on compilers without
# computed goto -- which means MSVC, which means Windows.
#
#   cmake -DWAMR_SRC=<wamr source dir> -P cmake/wamr_fast_interp_metering.cmake
#
# WHAT IS BROKEN UPSTREAM (WAMR 2.4.5)
#
# limits.fuelPerTurn is enforced by wasm_runtime_set_instruction_count_limit(),
# which the interpreter honours by calling CHECK_INSTRUCTION_LIMIT() once per
# dispatched opcode. wasm_interp_fast.c only does that from inside
# FETCH_OPCODE_AND_DISPATCH(), and that macro exists only in the
# WASM_ENABLE_LABELS_AS_VALUES branch. The #else branch -- an ordinary switch --
# defines:
#
#     #define HANDLE_OP_END() continue
#
# with no check at all, so the limit is accepted and then never applied. The
# classic interpreter (wasm_interp_classic.c) gets this right in both branches;
# only the fast one is missing it.
#
# WHY THAT LANDS ON WINDOWS AND NOWHERE ELSE
#
# core/config.h keys the feature on the compiler, not the platform:
#
#     #ifdef __GNUC__
#     #define WASM_ENABLE_LABELS_AS_VALUES 1
#
# gcc, clang and emcc all define __GNUC__ and take the metered path. MSVC has no
# computed goto, does not define it, and takes the switch. So a Windows build
# accepts every fuel budget in every manifest and enforces none of them.
#
# The visible symptom is a mod with an infinite loop in a hook. Everywhere else
# the interpreter traps it and the turn continues; on Windows it spins forever,
# which is a hung game for a player and a 30-minute job timeout in CI.
#
# WHY A PATCH RATHER THAN THE CLASSIC INTERPRETER ON MSVC
#
# Setting OD_MODS_FAST_INTERP=OFF for MSVC would also restore metering, but it
# makes Windows a different runtime from every other platform: roughly half the
# speed, and -- because the classic interpreter has no INT16_MAX operand-stack
# limit -- it would load mods there that will not load anywhere else. A mod
# author on Windows would ship something nobody else can run. Three lines here
# keeps all four platforms on the same interpreter with the same limits.
#
# Upstream: the fix is HANDLE_OP_END()'s definition in the #else branch, and it
# is the same three lines wasm_interp_classic.c already has.

if(NOT DEFINED WAMR_SRC)
    message(FATAL_ERROR "wamr_fast_interp_metering.cmake: pass -DWAMR_SRC=<dir>")
endif()

set(_f "${WAMR_SRC}/core/iwasm/interpreter/wasm_interp_fast.c")
if(NOT EXISTS "${_f}")
    message(FATAL_ERROR "wamr_fast_interp_metering.cmake: no such file: ${_f}")
endif()

file(READ "${_f}" _src)

set(_unpatched "#define HANDLE_OP(opcode) case opcode:\n#define HANDLE_OP_END() continue\n")
set(_patched "#define HANDLE_OP(opcode) case opcode:\n#define HANDLE_OP_END()        \\\n    CHECK_INSTRUCTION_LIMIT(); \\\n    continue;\n")

string(FIND "${_src}" "${_unpatched}" _at)
if(NOT _at EQUAL -1)
    string(REPLACE "${_unpatched}" "${_patched}" _src "${_src}")
    file(WRITE "${_f}" "${_src}")
    message(STATUS "WAMR: fast interpreter instruction metering patched in (switch dispatch)")
    return()
endif()

# Already done -- the patch step re-runs whenever the tree is re-populated, and
# doing nothing is the correct answer the second time.
string(FIND "${_src}" "${_patched}" _at)
if(NOT _at EQUAL -1)
    message(STATUS "WAMR: fast interpreter instruction metering already patched")
    return()
endif()

# Neither form is present, so upstream moved and this script no longer knows
# what it is editing. Fail the configure rather than build a Windows binary that
# silently ignores every fuel budget -- a patch that quietly stops applying is
# exactly the bug this file exists to prevent, and it would come back as a
# hanging game rather than as an error.
message(FATAL_ERROR
    "wamr_fast_interp_metering.cmake: could not find HANDLE_OP_END's switch-dispatch\n"
    "  definition in ${_f}.\n"
    "  WAMR has changed. Check whether the fast interpreter now calls\n"
    "  CHECK_INSTRUCTION_LIMIT() on the non-computed-goto path; if it does, drop\n"
    "  this patch and the PATCH_COMMAND in CMakeLists.txt. If it does not, update\n"
    "  the match above. Do not skip it: without it, MSVC builds enforce no fuel\n"
    "  limit at all and a mod with an infinite loop hangs the game.")
