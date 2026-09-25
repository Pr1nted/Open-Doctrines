/* Open Doctrines, compiled ahead of time into a loadable binary.
 *
 * ── WHY THIS FILE EXISTS AT ALL ──
 *
 * `#include "ODGame"` compiles the game in memory every boot, and that works
 * because a JIT compile chains its symbol table onto the running system's:
 * I64, MAlloc, GrBlot and everything else are simply there.
 *
 * An AOT compile does not. CmpJoin (Compiler/CMain.HC) chains an AOT build to
 * `cmp.asm_hash` -- the ASSEMBLER symbols and nothing else -- so the compiler
 * starts with no C types whatsoever. That is why `Cmp` on a two-line function
 * fails with "Expecting type at I64": not a bug in the code being compiled,
 * but a build with no headers.
 *
 * So this is a project file, in the shape TempleOS's own two use
 * (/Compiler/Compiler.PRJ): declare the types, turn the system's externs into
 * IMPORTS so Load() resolves them against the running kernel at load time,
 * and only then pull in the game.
 *
 *     Cmp("ODBIN.HC",,"ODBIN.BIN");   // writes the binary
 *     Load("ODBIN.BIN");              // links it against this machine
 *     OD;                             // and it is just a function
 */
/* ── THE ORDER IS THE WHOLE TRICK ──
 *
 * Copied from /Compiler/Compiler.PRJ, which is this machine's own example of
 * a module compiled against a kernel it does not contain. Every line of the
 * preamble earns its place:
 *
 *   KernelA.HH        the types. Without it there is no I64 at all.
 *   CompilerA.HH      the IC_* intrinsic codes. KernelB.HH declares things
 *                     like `public _intern IC_BSF I64 Bsf(...)`, and without
 *                     these constants that line is "Invalid lval at IC_BSF".
 *   EXTERNS_TO_IMPORTS  everything declared while this is on becomes a NAME
 *                     to resolve at load time instead of code to emit. It is
 *                     what lets the binary call the running kernel.
 *   KEEP_PRIVATE      as the compiler's own project does, so private symbols
 *                     in those headers survive the pass.
 */
#exe {
  Option(OPTf_WARN_PAREN, ON);
  Option(OPTf_WARN_DUP_TYPES, ON);
  Option(OPTf_KEEP_PRIVATE, ON);
};
#include "::/Kernel/KernelA.HH"
#include "::/Compiler/CompilerA.HH"
#exe {Option(OPTf_EXTERNS_TO_IMPORTS, ON);};
#include "::/Kernel/KernelB.HH"
#include "::/Kernel/KernelC.HH"
#include "::/Adam/AExts.HC"
#exe {Option(OPTf_EXTERNS_TO_IMPORTS, OFF);};
#exe {Option(OPTf_KEEP_PRIVATE, OFF);};

#include "ODGame"
